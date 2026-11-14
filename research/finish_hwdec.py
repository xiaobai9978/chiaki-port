"""Stop the long autotest, collect final stats, restore the user's config."""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import xfer

D = '/mnt/SDCARD/Apps/Chiaki'

# Final stats before killing
o, _, _ = xfer.run('dev',
    'echo ===FINAL_STATS===; grep -a "stats:" %s/run.log | tail -3; '
    'echo ===HWDEC_FRAMES===; grep -a "hwdec: frames" %s/run.log | tail -3; '
    'echo ===QUIT===; grep -aiE "QUIT|reason=" %s/run.log | tail -3; '
    'echo ===RSS===; ps -o pid,rss,vsz -p $(pgrep -x chiaki_brick | head -1) 2>/dev/null'
    % (D, D, D), timeout=40)
print(o, end='')

# Kill only our binary and the stuck helper shell; never touch MainUI
xfer.run('dev', 'killall chiaki_brick 2>/dev/null; '
                'kill 19908 19909 2>/dev/null; sleep 1; '
                'killall -9 chiaki_brick 2>/dev/null; echo STOPPED', timeout=30)

# Restore the user's original streaming parameters, but keep hwdec enabled
cfg = {
    'host': '', 'ps5': '1', 'regkey': '', 'morning': '', 'regkey_host': '',
    'resolution': '2', 'fps': '60', 'bitrate': '4000', 'codec': '0',
    'hwdec': '1', 'autoconnect': '0',
    'swap_ab': '0', 'swap_xy': '0', 'rumble': '25', 'vsync': '1', 'loglevel': '7',
}
o, _, _ = xfer.run('dev', 'cat %s/config.ini' % D)
keys = list(cfg.keys())
lines = []
for ln in o.splitlines():
    if not ln.strip():
        continue
    k = ln.split('=', 1)[0].strip()
    if k in cfg:
        lines.append('%s=%s' % (k, cfg[k]))
        cfg.pop(k, None)
    elif not k.startswith('key_'):
        lines.append(ln)
for ln in o.splitlines():
    if ln.strip().startswith('key_'):
        lines.append(ln)
path = 'build/config.restored.ini'
with open(path, 'w', newline='\n') as f:
    f.write('\n'.join(lines) + '\n')
xfer.put('dev', path, D + '/config.ini')
print('--- restored config ---')
o, _, _ = xfer.run('dev', 'head -16 %s/config.ini; echo ===PROC===; '
                          'ps aux | grep "[c]hiaki_brick" | head -3' % D)
print(o, end='')
