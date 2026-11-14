"""Final regression matrix for the whole night's changes.

Runs each config through a real PS5 stream, parses the stats lines, sleeps
between cases so the console releases the previous session, then restores
the user's original config.
"""
import os
import re
import sys
import time
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import push_hwdec as p
import xfer

D = '/mnt/SDCARD/Apps/Chiaki'
BASE = {'hwdec': '1', 'autoconnect': '1', 'loglevel': '7'}

CASES = [
    ('H.265 720p60 硬解', dict(BASE, codec='1', resolution='3', fps='60', bitrate='6000'), 20),
    ('H.264 540p60 硬解', dict(BASE, codec='0', resolution='2', fps='60', bitrate='4000'), 20),
    ('H.264 540p60 软解', dict(BASE, hwdec='0', codec='0', resolution='2', fps='60', bitrate='4000'), 20),
    ('H.265 1080p30 硬解', dict(BASE, codec='1', resolution='4', fps='30', bitrate='10000'), 20),
    ('H.265 720p60 长测', dict(BASE, codec='1', resolution='3', fps='60', bitrate='6000'), 150),
]

RESTORE = {'hwdec': '1', 'autoconnect': '0', 'codec': '0', 'resolution': '2',
           'fps': '60', 'bitrate': '4000', 'rumble': '25', 'vsync': '1', 'loglevel': '7'}


def run_case(label, cfg, secs):
    p.set_config(cfg)
    cmd = (
        'cd %s && : > run.log && '
        'export LD_LIBRARY_PATH=$PWD/libs:/mnt/SDCARD/System/lib && '
        'CHIAKI_AUTOTEST=1 ./chiaki_brick >> run.log 2>&1 & '
        'PID=$!; sleep %d; kill $PID 2>/dev/null; sleep 1; '
        'kill -9 $PID 2>/dev/null; echo DONE' % (D, secs)
    )
    xfer.run('dev', cmd, timeout=secs + 50)
    o, _, _ = xfer.run('dev', "grep -a 'stats:' %s/run.log" % D, timeout=40)
    vals = []
    for ln in o.splitlines():
        m = re.search(r'fps=(\d+).*?dec=(\d+)ms.*?rtt=(-?\d+)ms.*?lost=(-?\d+).*?rx=(\d+)k', ln)
        if m:
            vals.append(tuple(int(x) for x in m.groups()))
    o2, _, _ = xfer.run('dev',
        "grep -a 'hwdec: frames' %s/run.log | tail -1; "
        "grep -aicE 'segmentation|assert|abort' %s/run.log" % (D, D), timeout=40)
    if not vals:
        print('%-24s NO_STATS  %s' % (label, o2.strip().replace('\n', ' | ')))
        return None
    steady = vals[1:] if len(vals) > 2 else vals
    avg = lambda i: sum(v[i] for v in steady) / len(steady)
    print('%-24s fps=%4.1f  dec=%4.1fms  rtt=%4.1fms  lost=%3.1f  rx=%5.0fk  n=%d  %s'
          % (label, avg(0), avg(1), avg(2), avg(3), avg(4), len(steady),
             o2.strip().replace('\n', ' | ')))
    return True


if __name__ == '__main__':
    p.deploy()
    print('=== regression matrix ===')
    for i, (label, cfg, secs) in enumerate(CASES):
        run_case(label, cfg, secs)
        if i < len(CASES) - 1:
            time.sleep(75)      # let the PS5 release the session
    p.set_config(RESTORE)
    print('=== config restored ===')
    o, _, _ = xfer.run('dev', 'grep -E "^(resolution|fps|bitrate|codec|hwdec|autoconnect)=" %s/config.ini' % D)
    print(o, end='')
