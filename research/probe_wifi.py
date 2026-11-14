import xfer

cmds = [
    "iw dev wlan0 get power_save 2>&1",
    "iwconfig wlan0 2>&1 | head -8",
    "cat /sys/module/xradio_core/parameters/* 2>/dev/null | head -5; ls /sys/module/xradio_core/parameters/ 2>/dev/null | head -10",
]
for c in cmds:
    o, e, r = xfer.run('dev', c, timeout=25)
    print('=====', c)
    print(o[:1200], e[:200])
