"""Push/pull files between this Windows box and PVE CT 210 via the PVE termproxy channel."""
import base64
import gzip
import hashlib
import os
import subprocess
import sys

PVE_DIR = r"C:\Users\jhgroup\WorkBuddy\2026-10-16-08-43-15"
PY = sys.executable
API = os.path.join(PVE_DIR, "pve_api.py")


def run(cmd, timeout=180):
    p = subprocess.run([PY, API, cmd, str(timeout)], cwd=PVE_DIR,
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    return p.stdout + p.stderr


def push(local, remote, chunk=20000):
    with open(local, "rb") as f:
        data = f.read()
    md5 = hashlib.md5(data).hexdigest()
    payload = base64.b64encode(gzip.compress(data, 6)).decode()
    tmp = "/tmp/_p%s.b64" % md5[:12]
    rdir = os.path.dirname(remote)

    run("pct exec 210 -- bash -c 'mkdir -p %s; rm -f %s %s.gz'" % (rdir, tmp, remote), 60)
    total = (len(payload) + chunk - 1) // chunk
    for i in range(total):
        part = payload[i * chunk:(i + 1) * chunk]
        run("pct exec 210 -- bash -c 'printf %%s \"%s\" >> %s'" % (part, tmp), 120)

    out = run("pct exec 210 -- bash -c 'base64 -d %s > %s.gz; gzip -df %s.gz; md5sum %s'"
              % (tmp, remote, remote, remote), 120)
    ok = md5 in out
    print("%s %s" % ("OK  " if ok else "FAIL", remote), flush=True)
    if not ok:
        print(out[-600:])
    return ok


def pull(remote, local, chunk=120000):
    info = run("pct exec 210 -- bash -c 'base64 -w0 %s | wc -c'" % remote, 120)
    size = 0
    for line in info.splitlines():
        line = line.strip()
        if line.isdigit():
            size = int(line)
    if size == 0:
        print("empty or missing:", remote)
        return False
    print("remote size(b64) =", size)
    chunks = []
    off = 0
    while off < size:
        end = min(off + chunk, size)
        cmd = ("pct exec 210 -- bash -c 'echo ---B---; base64 -w0 %s | cut -c%d-%d; echo ---E---'"
               % (remote, off + 1, end))
        out = run(cmd, 180)
        try:
            b = out.index("---B---") + len("---B---")
            e = out.index("---E---", b)
        except ValueError:
            print("marker fail at offset", off)
            print(out[:500])
            return False
        blob = "".join(out[b:e].split())
        chunks.append(blob)
        off = end
        print("  got %d/%d" % (end, size), flush=True)
    data = base64.b64decode("".join(chunks))
    with open(local, "wb") as f:
        f.write(data)
    print("saved", local, len(data), "bytes")
    return True


if __name__ == "__main__":
    mode = sys.argv[1]
    if mode == "push":
        ok = push(sys.argv[2], sys.argv[3])
        sys.exit(0 if ok else 1)
    else:
        pull(sys.argv[2], sys.argv[3])
