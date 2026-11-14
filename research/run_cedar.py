"""Compile the cedar H.265 probe in the CT chroot, push to the handheld, run, fetch log."""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import xfer

SRC = 'research/test_cedar2.c'
STREAM = 'research/h265_720p60.h265'
REMOTE_BIN = '/tmp/tc2'
REMOTE_STREAM = '/tmp/h265.h265'
REMOTE_LOG = '/tmp/tc2.log'


def build():
    xfer.put('ct', SRC, '/opt/arm64/build/test_cedar2.c')
    cmd = ("chroot /opt/arm64 /bin/bash -c 'cd /build && "
           "gcc -O2 -Wall -o tc2 test_cedar2.c -ldl -lm 2>&1; "
           "echo BUILD_EXIT=$?; ls -la tc2'")
    out, err, rc = xfer.run('ct', cmd, timeout=180)
    print('--- compile ---')
    print(out, end='')
    if err:
        print(err, end='', file=sys.stderr)
    if 'BUILD_EXIT=0' not in out:
        raise SystemExit('BUILD FAILED')
    return xfer.get('ct', '/opt/arm64/build/tc2', 'build/tc2')


def deploy(size):
    print('binary bytes', size)
    n = xfer.put('dev', 'build/tc2', REMOTE_BIN, chmod=0o755)
    print('pushed bin', n)
    n = xfer.put('dev', STREAM, REMOTE_STREAM)
    print('pushed stream', n)


def run():
    cmd = ('cd /tmp && ./tc2 %s > %s 2>&1; echo RUN_EXIT=$?; '
           'echo ====LOG====; cat %s' % (REMOTE_STREAM, REMOTE_LOG, REMOTE_LOG))
    out, err, rc = xfer.run('dev', cmd, timeout=180)
    print('--- run ---')
    print(out[-12000:], end='')
    if err:
        print(err[-2000:], end='', file=sys.stderr)


if __name__ == '__main__':
    deploy(build())
    run()
