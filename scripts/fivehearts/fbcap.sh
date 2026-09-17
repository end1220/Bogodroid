#!/bin/sh
# Capture the Anbernic panel while a game is running, from the handheld side.
#
#   sh fbcap.sh [count] [interval_s] [dir]
#
# The panel is a fbdev double buffer: 640x960x32 BGRA in one mapping, i.e. two
# 640x480 frames stacked. Both halves are written out by this script's caller
# (scripts/fivehearts/fbcap.py picks the one with structure), because which half
# the driver last panned to is the only way to know which one is on screen.
#
# Run it over dropbeak *after* the game has started:
#   dropbeak exec "sh /mnt/mmc/Roms/PORTS/FiveHearts/fbcap.sh 8 3"
COUNT="${1:-6}"
INTERVAL="${2:-4}"
DIR="${3:-/mnt/mmc/Roms/PORTS/FiveHearts/cap}"
mkdir -p "$DIR"
i=0
while [ "$i" -lt "$COUNT" ]; do
    name=$(printf '%s/fb.%02d.raw' "$DIR" "$i")
    dd if=/dev/fb0 of="$name" bs=2457600 count=1 2>/dev/null
    echo "$(date +%H:%M:%S) $name $(stat -c%s "$name" 2>/dev/null)"
    i=$((i + 1))
    sleep "$INTERVAL"
done
