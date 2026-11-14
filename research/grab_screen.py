"""Grab the live framebuffer and long-test stats from the handheld."""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import xfer

D = '/mnt/SDCARD/Apps/Chiaki'
cmd = (
    'dd if=/dev/fb0 of=/tmp/screen2.raw bs=1024 count=3072 2>/dev/null; '
    'ls -la /tmp/screen2.raw; '
    'echo ===MEM===; cat /tmp/mem1.txt /tmp/mem2.txt 2>/dev/null; '
    'echo ===STATS===; grep -a "stats:" %s/run.log | tail -4; '
    'echo ===PROC===; ps aux | grep "[c]hiaki_brick" | head -3' % D
)
o, e, r = xfer.run('dev', cmd, timeout=60)
print(o, end='')
if e:
    print('ERR', e[-500:])
n = xfer.get('dev', '/tmp/screen2.raw', 'build/screen2.raw')
print('downloaded', n)
