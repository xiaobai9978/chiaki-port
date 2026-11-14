"""UDP+TCP relay that bridges the handheld (office LAN) to the PS5 (reached over the home VPN).

The handheld talks to this machine as if it were the console; every byte is forwarded
to the PS5 through the VPN, and replies come back the same way.
"""
import argparse
import select
import selectors
import socket
import threading
import time

PS5 = ("192.168.50.116", 9295)
PORTS = [9295, 9296, 9297]


def log(msg):
    print(time.strftime("[%H:%M:%S] ") + msg, flush=True)


# ---------------------------------------------------------------- TCP
def tcp_pipe(a, b, tag=""):
    n_down = n_up = 0
    try:
        while True:
            r, _, x = select.select([a, b], [], [a, b], 60)
            if x:
                log("tcp  %s exception on socket" % tag)
                break
            if not r:
                log("tcp  %s idle timeout" % tag)
                break
            for s in r:
                try:
                    data = s.recv(65536)
                except BlockingIOError:
                    continue
                if not data:
                    log("tcp  %s eof (down=%d up=%d)" % (tag, n_down, n_up))
                    return
                other = b if s is a else a
                other.sendall(data)
                if s is a:
                    n_down += len(data)
                else:
                    n_up += len(data)
    except Exception as e:
        log("tcp  %s pipe error %s (down=%d up=%d)" % (tag, e, n_down, n_up))
    finally:
        try:
            a.close()
        except Exception:
            pass
        try:
            b.close()
        except Exception:
            pass


def tcp_server(listen_port, upstream):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        srv.bind(("0.0.0.0", listen_port))
    except OSError as e:
        log("tcp  bind %d failed: %s -> 中继可能已在运行，直接玩即可" % (listen_port, e))
        return
    srv.listen(16)
    log("tcp  listening on 0.0.0.0:%d -> %s:%d" % (listen_port, upstream[0], upstream[1]))
    while True:
        client, addr = srv.accept()
        tag = "%s:%d" % addr
        log("tcp  %s connected" % tag)
        try:
            up = socket.create_connection(upstream, timeout=10)
            up.settimeout(None)
        except Exception as e:
            log("tcp  upstream connect failed: %s" % e)
            client.close()
            continue
        threading.Thread(target=tcp_pipe, args=(client, up, tag), daemon=True).start()


# ---------------------------------------------------------------- UDP
class UdpRelay:
    """One downstream socket per client endpoint, replies routed back."""

    def __init__(self, listen_port, upstream):
        self.upstream = upstream
        self.sel = selectors.DefaultSelector()
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            self.sock.bind(("0.0.0.0", listen_port))
        except OSError as e:
            log("udp  bind %d failed: %s -> 中继可能已在运行" % (listen_port, e))
            return
        self.sock.setblocking(False)
        self.up_of_client = {}
        self.client_of_up = {}
        self.sel.register(self.sock, selectors.EVENT_READ, ("listen", None))
        log("udp  listening on 0.0.0.0:%d -> %s:%d" % (listen_port, upstream[0], upstream[1]))
        threading.Thread(target=self.loop, daemon=True).start()

    def loop(self):
        while True:
            try:
                events = self.sel.select(timeout=1.0)
            except OSError:
                continue
            for key, _ in events:
                kind, _unused = key.data
                if kind == "listen":
                    try:
                        data, addr = self.sock.recvfrom(65536)
                    except OSError:
                        continue
                    up = self.up_of_client.get(addr)
                    if up is None:
                        up = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                        up.bind(("0.0.0.0", 0))
                        up.setblocking(False)
                        self.up_of_client[addr] = up
                        self.client_of_up[up] = addr
                        self.sel.register(up, selectors.EVENT_READ, ("up", None))
                        log("udp  new client %s:%d" % addr)
                    try:
                        up.sendto(data, self.upstream)
                    except OSError as e:
                        log("udp  send failed: %s" % e)
                else:
                    up = key.fileobj
                    try:
                        data = up.recv(65536)
                    except OSError:
                        continue
                    client = self.client_of_up.get(up)
                    if client:
                        try:
                            self.sock.sendto(data, client)
                        except OSError:
                            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ps5", default="192.168.50.116")
    ap.add_argument("--port", type=int, default=9295)
    ap.add_argument("--ports", default="9295,9296,9297")
    args = ap.parse_args()
    upstream = (args.ps5, args.port)
    log("mapping ports %s -> %s (same port)" % (args.ports, args.ps5))

    for p in [int(x) for x in args.ports.split(",") if x.strip()]:
        # same port on both sides: ctrl/session on 9295, stream on 9296/9297
        target = (args.ps5, p)
        threading.Thread(target=tcp_server, args=(p, target), daemon=True).start()
        UdpRelay(p, target)

    log("relay ready (ctrl-tcp + stream-udp) -> %s:%d" % upstream)
    while True:
        time.sleep(3600)


if __name__ == "__main__":
    main()
