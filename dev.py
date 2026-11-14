"""Legacy SSH helper for the handheld; credentials come from xfer (secrets_local.py)."""
import xfer


def run(cmd, timeout=60):
    return xfer.run("dev", cmd, timeout)


def put(local, remote, chmod=None):
    return xfer.put("dev", local, remote, chmod)


def get(remote, local):
    return xfer.get("dev", remote, local)


if __name__ == "__main__":
    import sys
    o, e, r = run(" ".join(sys.argv[1:]))
    print(o, end="")
