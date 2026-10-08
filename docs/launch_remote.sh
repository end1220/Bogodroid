#!/bin/sh
# Trimui remote launch: drop MainUI, start Oddmar with video dumps.
set -x
ODD="/mnt/SDCARD/Data/ports/Oddmar"
cd "$ODD" || exit 1

killall -9 unityloader 2>/dev/null || true
sleep 1

# MainUI owns the panel; Trimui has no Anbernic /tmp/.next handoff.
killall -9 MainUI 2>/dev/null || true
sleep 1

chmod a+x "$ODD/unityloader" 2>/dev/null || true
rm -f "$ODD"/dump.*.gl.ppm "$ODD"/dump.*.fb.ppm "$ODD"/vframe.*.ppm "$ODD"/splash.png 2>/dev/null || true
: > "$ODD/log.txt"

# Trimui /usr/lib is FFmpeg 6 (so.60). Ports/MainUI normally prepends
# /mnt/SDCARD/System/lib which has FFmpeg 4.2 (so.58) matching this binary.
export LD_LIBRARY_PATH="/mnt/SDCARD/System/lib:$ODD/ff58${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

export BD_BYPASS_VIDEO_TRANSLATE=1
export BD_DUMP_FRAME="$ODD/dump"
# splash ~4s then logo; dump at ~8s / 16s / 24s if 60 Hz
export BD_DUMP_FRAME_AT=90
export BD_VIDEO_DUMP_FRAME="$ODD/vframe"
export BD_VIDEO_DUMP_AT=40
export MALLOC_ARENA_MAX=2
export MALLOC_TRIM_THRESHOLD_=65536
export MALLOC_MMAP_THRESHOLD_=131072
export LP_NUM_THREADS=2

# background: dropbeak exec times out if we wait on the game
nohup "$ODD/unityloader" "$ODD/unity.toml" >>"$ODD/log.txt" 2>&1 &
echo "PID:$!"
sleep 1
ps | grep unityloader | grep -v grep || true
