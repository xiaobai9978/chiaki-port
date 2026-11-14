import re
import sys

import xfer

RES_NAME = {1: '360p', 2: '540p', 3: '720p', 4: '1080p'}


def run_case(res, fps, br, vsync, noosd=0, secs=55):
    cfg = (
        'resolution=%d\nfps=%d\nbitrate=%d\ncodec=0\n'
        'autoconnect=1\nswap_ab=0\nswap_xy=0\nrumble=60\nvsync=%d\nloglevel=7\n'
        % (res, fps, br, vsync)
    )
    path = 'build/config.test.ini'
    with open(path, 'w') as f:
        f.write(cfg)

    xfer.put('dev', path, '/mnt/SDCARD/Apps/Chiaki/config.ini')

    cmd = (
        'cd /mnt/SDCARD/Apps/Chiaki\n'
        'killall MainUI 2>/dev/null; sleep 3\n'
        'export LD_LIBRARY_PATH=$PWD/libs:/mnt/SDCARD/System/lib\n'
        ': > run.log\n'
        '%s./chiaki_brick >> run.log 2>&1 &\n'
        'PID=$!\n'
        'sleep %d\n'
        'kill $PID 2>/dev/null; sleep 2; kill -9 $PID 2>/dev/null\n'
        'echo DONE\n' % ('CHIAKI_AUTOTEST=1 CHIAKI_NOOSD=1 ' if noosd else 'CHIAKI_AUTOTEST=1 ', secs)
    )
    xfer.run('dev', cmd, timeout=secs + 40)

    out, _, _ = xfer.run(
        'dev',
        "cd /mnt/SDCARD/Apps/Chiaki && grep -a 'stats:' run.log; "
        "echo ===FEC===; grep -ac 'FEC failed' run.log",
        timeout=40,
    )
    lines = out.splitlines()
    fps_values = []
    dec_values = []
    sin = []
    fec = 0
    for ln in lines:
        m = re.search(r'fps=(\d+).*?dec=(\d+)ms.*?rtt=(-?\d+)ms', ln)
        if m:
            fps_values.append(int(m.group(1)))
            dec_values.append(int(m.group(2)))
            sin.append(int(m.group(3)))
    m = re.search(r'===FEC===\s*(\d+)', out.replace('\n', ' '))
    if m:
        fec = int(m.group(1))

    # 丢掉第一段（连接/协商阶段的爬坡值）
    steady = fps_values[1:] if len(fps_values) > 2 else fps_values
    sdec = dec_values[1:] if len(dec_values) > 2 else dec_values
    srtt = sin[1:] if len(sin) > 2 else sin
    avg = sum(steady) / len(steady) if steady else 0
    lo = min(steady) if steady else 0
    hi = max(steady) if steady else 0
    davg = sum(sdec) / len(sdec) if sdec else 0
    ravg = sum(srtt) / len(srtt) if srtt else 0
    return {
        'label': '%s@%d / %dk / vsync=%d' % (RES_NAME.get(res, res), fps, br, vsync),
        'avg': avg, 'min': lo, 'max': hi, 'n': len(steady), 'fec': fec,
        'dec': davg, 'rtt': ravg,
    }


if __name__ == '__main__':
    import time
    cases = [
        (2, 60, 6000, 1, 0),
        (2, 60, 6000, 1, 1),
    ]
    results = []
    for i, c in enumerate(cases):
        if i > 0:
            time.sleep(75)
        r = run_case(*c)
        results.append(r)
        print('%-26s avg=%5.1f fps  min=%2d max=%2d  dec=%dms conv/rtt=%d/%dms  fec=%d'
              % (r['label'], r['avg'], r['min'], r['max'], r['dec'], r['dec'], r['rtt'], r['fec']))
    print()
    best = max(results, key=lambda x: x['avg'])
    print('BEST: %s  avg=%.1f fps' % (best['label'], best['avg']))
