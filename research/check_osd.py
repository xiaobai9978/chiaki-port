"""Deploy the OSD build, run a short stream, grab framebuffer + stats."""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import push_hwdec as p
import xfer

D = '/mnt/SDCARD/Apps/Chiaki'

p.deploy()
p.set_config({'hwdec': '1', 'autoconnect': '1', 'codec': '1',
              'resolution': '3', 'fps': '60', 'bitrate': '6000', 'loglevel': '7'})

cmd = (
    'cd %s && : > run.log && '
    'export LD_LIBRARY_PATH=$PWD/libs:/mnt/SDCARD/System/lib && '
    'CHIAKI_AUTOTEST=1 ./chiaki_brick >> run.log 2>&1 & '
    'PID=$!; sleep 12; '
    'dd if=/dev/fb0 of=/tmp/osd.raw bs=1024 count=3072 2>/dev/null; '
    'kill $PID 2>/dev/null; sleep 1; kill -9 $PID 2>/dev/null; echo CAP_DONE' % D
)
o, e, r = xfer.run('dev', cmd, timeout=60)
print(o.strip()[-200:])
print('get', xfer.get('dev', '/tmp/osd.raw', 'build/osd.raw'))
o, _, _ = xfer.run('dev', 'grep -a "stats:" %s/run.log | tail -4' % D)
print(o, end='')
