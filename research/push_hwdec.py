"""Push the hard-decoder build to the handheld and run a short autotest."""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import xfer
import hashlib

BIN = 'build/chiaki_brick_hw'
DIR = '/mnt/SDCARD/Apps/Chiaki'
REMOTE = DIR + '/chiaki_brick'


def deploy():
    local = open(BIN, 'rb').read()
    md5 = hashlib.md5(local).hexdigest()
    print('local size', len(local), 'md5', md5)
    o, e, r = xfer.run('dev',
        'killall chiaki_brick 2>/dev/null; '
        'cd %s && cp -f config.ini config.ini.bak_hwdec && '
        'rm -f chiaki_brick && echo READY' % DIR)
    print('prep:', o.strip())
    n = xfer.put('dev', BIN, REMOTE, chmod=0o755)
    print('pushed', n)
    o, e, r = xfer.run('dev', 'md5sum %s' % REMOTE)
    print('remote md5:', o.strip())
    assert md5 in o, 'MD5 MISMATCH'
    return md5


def set_config(overrides):
    o, _, _ = xfer.run('dev', 'cat %s/config.ini' % DIR)
    lines = []
    for ln in o.splitlines():
        k = ln.split('=', 1)[0].strip()
        if k in overrides:
            lines.append('%s=%s' % (k, overrides[k]))
            del overrides[k]
        elif ln.strip():
            lines.append(ln)
    for k, v in overrides.items():
        lines.append('%s=%s' % (k, v))
    path = 'build/config.hwdec.ini'
    with open(path, 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    xfer.put('dev', path, DIR + '/config.ini')
    print('--- config ---')
    print('\n'.join(lines[:16]))


def autotest(secs=25):
    cmd = (
        'cd %s && '
        ': > run.log && '
        'export LD_LIBRARY_PATH=$PWD/libs:/mnt/SDCARD/System/lib && '
        'CHIAKI_AUTOTEST=1 ./chiaki_brick >> run.log 2>&1 & '
        'PID=$!; sleep %d; kill $PID 2>/dev/null; sleep 1; '
        'kill -9 $PID 2>/dev/null; echo DONE' % (DIR, secs)
    )
    xfer.run('dev', cmd, timeout=secs + 45)
    o, _, _ = xfer.run('dev',
        "cd %s && echo '=== hwdec ==='; grep -aE 'cedar|hwdec|HARDWARE' run.log | head -20; "
        "echo '=== stats ==='; grep -a 'stats:' run.log | tail -6; "
        "echo '=== errors ==='; grep -aiE 'error|fail|invalid' run.log | grep -v CdcIonFree | tail -15; "
        "echo '=== tail ==='; tail -8 run.log" % DIR, timeout=60)
    print(o[-9000:], end='')


if __name__ == '__main__':
    deploy()
    set_config({'hwdec': '1', 'autoconnect': '1', 'loglevel': '31'})
    autotest(int(sys.argv[1]) if len(sys.argv) > 1 else 25)
