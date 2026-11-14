"""Deploy batch-A build + rotated launch.sh, run a stream, report log volume."""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import push_hwdec as p
import xfer

D = '/mnt/SDCARD/Apps/Chiaki'

p.deploy()
# launch.sh is a /bin/sh script; make sure it is LF-only and executable
local = open('port/package/launch.sh', 'rb').read().replace(b'\r\n', b'\n')
open('build/launch_lf.sh', 'wb').write(local)
print('launch.sh', xfer.put('dev', 'build/launch_lf.sh', D + '/launch.sh', chmod=0o755))
p.set_config({'hwdec': '1', 'autoconnect': '1', 'codec': '1',
              'resolution': '3', 'fps': '60', 'bitrate': '6000', 'loglevel': '7'})
p.autotest(20)

o, _, _ = xfer.run('dev',
    'echo ===LOG_VOLUME===; wc -l %s/run.log; '
    'echo ===HAPTIC_LINES===; grep -ac "HAPTICS" %s/run.log; '
    'echo ===HWDEC_LINES===; grep -ac "hwdec: decoded" %s/run.log; '
    'echo ===ERRORS===; grep -aicE "segmentation|assert|abort" %s/run.log'
    % (D, D, D, D))
print(o, end='')
