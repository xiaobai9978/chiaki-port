import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, 'research')
import xfer, push_hwdec as p
D = '/mnt/SDCARD/Apps/Chiaki'
p.set_config({'hwdec': '1', 'autoconnect': '1', 'codec': '0',
              'resolution': '2', 'fps': '60', 'bitrate': '4000', 'loglevel': '7'})
cmd = (
    'cd %s && : > run.log && '
    'export LD_LIBRARY_PATH=$PWD/libs:/mnt/SDCARD/System/lib && '
    'CHIAKI_AUTOTEST=1 ./chiaki_brick >> run.log 2>&1 & '
    'PID=$!; sleep 14; '
    'dd if=/dev/fb0 of=/tmp/h264.raw bs=1024 count=3072 2>/dev/null; '
    'kill $PID 2>/dev/null; sleep 1; kill -9 $PID 2>/dev/null; echo CAP_DONE' % D
)
o, e, r = xfer.run('dev', cmd, timeout=70)
print(o.strip()[-60:])
xfer.get('dev', '/tmp/h264.raw', 'build/h264.raw')
from PIL import Image
raw = open('build/h264.raw','rb').read()
img = Image.frombytes('RGBA',(1024,768),raw[:1024*768*4],'raw','BGRA')
img.crop((0,0,1024,600)).save('build/h264_full.png')
print('saved')
