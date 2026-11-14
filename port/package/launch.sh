#!/bin/sh
# Chiaki Brick - PS5 Remote Play for TrimUI Brick Pro
progdir=$(dirname "$0")
cd "$progdir"

export LD_LIBRARY_PATH="$progdir/libs:/mnt/SDCARD/System/lib:$LD_LIBRARY_PATH"

# The TrimUI launcher quits MainUI before starting an app. If MainUI is still
# alive (e.g. the app was started over SSH) both would draw to /dev/fb0 and the
# screen would flicker between the UI and the stream - wait for it to go away.
i=0
while [ $i -lt 20 ]; do
    pgrep -x MainUI > /dev/null || break
    sleep 0.3
    i=$((i + 1))
done
if pgrep -x MainUI > /dev/null; then
    echo "[$(date)] WARNING: MainUI still running, screen may flicker. Launch from Apps menu." >> "$progdir/run.log"
fi

# max clocks: software H.264 decoding is CPU bound
for c in 0 1 2 3; do
    [ -e "/sys/devices/system/cpu/cpu$c/online" ] && echo 1 > "/sys/devices/system/cpu/cpu$c/online" 2>/dev/null
done
echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
echo 2000000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq 2>/dev/null

# SDL picks its own video driver (fbdev on this device).
# Force one with:  export SDL_VIDEODRIVER=fbdev

# run.log 以前只追加不轮转，串流日志密度高，长玩会写满 SD 卡并和解码抢 IO。
# 超过 2MB 只保留最近 1MB，与程序内部对 chiaki.log 的截断策略一致。
if [ -f "$progdir/run.log" ]; then
    sz=$(wc -c < "$progdir/run.log" 2>/dev/null)
    if [ -n "$sz" ] && [ "$sz" -gt 2097152 ]; then
        tail -c 1048576 "$progdir/run.log" > "$progdir/run.log.new" 2>/dev/null
        mv "$progdir/run.log.new" "$progdir/run.log"
    fi
fi

./chiaki_brick >> "$progdir/run.log" 2>&1

# back to the launcher
sync
