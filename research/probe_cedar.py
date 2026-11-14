import xfer

cmds = [
    "find / -name 'libmpeg2*' 2>/dev/null | head -5",
    "strings /usr/trimui/bin/mplayer | grep -iE 'CedarC|libvdecoder|vdecoder|cedar_dev|H265|hevc' | head -15",
    "strings /usr/trimui/bin/mplayer | grep -oE 'VIDEO_CODEC_FORMAT_[A-Z0-9_]*' | sort -u | head",
]
for c in cmds:
    o, e, r = xfer.run('dev', c, timeout=60)
    print('=====', c)
    print(o[:1500], e[:200])
