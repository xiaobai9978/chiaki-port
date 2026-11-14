"""Fast file transfer + command execution for the build container and the handheld.

连接凭据不在本仓库里。在本文件旁边建一个 secrets_local.py（已加入 .gitignore）：

    HOSTS = {
        "ct":  ("<编译容器 IP>", "root", "<密码>"),
        "dev": ("<掌机 IP>",    "root", "<密码>"),
    }

也可以用环境变量 CHIAKI_CT_HOST/_USER/_PASSWORD、CHIAKI_DEV_HOST/_USER/_PASSWORD（优先生效）。
"""
import os
import sys
import paramiko

try:
    from secrets_local import HOSTS
except ImportError:
    HOSTS = {}

for _name, _pfx in (("ct", "CHIAKI_CT"), ("dev", "CHIAKI_DEV")):
    _ip = os.environ.get(_pfx + "_HOST")
    if _ip:
        HOSTS[_name] = (_ip, os.environ.get(_pfx + "_USER", "root"),
                        os.environ.get(_pfx + "_PASSWORD", ""))

_clients = {}


def client(name):
    if name not in HOSTS:
        sys.exit('no credentials for "%s": create secrets_local.py '
                 '(gitignored) or set CHIAKI_* env vars' % name)
    if name not in _clients:
        ip, user, pwd = HOSTS[name]
        c = paramiko.SSHClient()
        c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        c.connect(ip, port=22, username=user, password=pwd, timeout=20,
                  allow_agent=False, look_for_keys=False)
        _clients[name] = c
    return _clients[name]


def run(name, cmd, timeout=300):
    stdin, stdout, stderr = client(name).exec_command(cmd, timeout=timeout)
    out = stdout.read().decode("utf-8", "replace")
    err = stderr.read().decode("utf-8", "replace")
    rc = stdout.channel.recv_exit_status()
    return out, err, rc


def get(name, remote, local):
    os.makedirs(os.path.dirname(os.path.abspath(local)), exist_ok=True)
    sftp = client(name).open_sftp()
    sftp.get(remote, local)
    sftp.close()
    return os.path.getsize(local)


def put(name, local, remote, chmod=None):
    sftp = client(name).open_sftp()
    try:
        sftp.mkdir(os.path.dirname(remote))
    except IOError:
        pass
    sftp.put(local, remote)
    if chmod:
        sftp.chmod(remote, chmod)
    size = sftp.stat(remote).st_size
    sftp.close()
    return size


if __name__ == "__main__":
    what = sys.argv[1]
    if what == "sh":
        o, e, r = run(sys.argv[2], sys.argv[3])
        print(o, end="")
        if e:
            print(e, end="", file=sys.stderr)
        sys.exit(r)
