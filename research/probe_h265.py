import xfer

cmds = [
    "ar t /opt/arm64/build/ffmpeg6/lib/libavcodec.a 2>/dev/null | grep -E 'hevc|h264' | head -20",
    "ls -l /opt/arm64/build/ffmpeg-src/config.h 2>&1",
    "grep -n 'CONFIG_HEVC_DECODER\\|CONFIG_H264_DECODER' /opt/arm64/build/ffmpeg-src/config.h 2>&1 | head",
]
for c in cmds:
    o, e, r = xfer.run('ct', c, timeout=40)
    print('$', c)
    print(o[:1800], e[:300])
