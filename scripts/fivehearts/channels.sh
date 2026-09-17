#!/bin/bash
# Per-channel min/max/mean of a frame.
#
# Needed on top of analyze.sh because of how the gradient probe works: it fills
# the video texture with R ramping along x and G ramping along y. If the video
# quad samples the WHOLE texture, the screen shows R from 0..255 and G from
# 0..255. If the quad samples a SUB-RECT - the "only the bottom-left corner of
# the video is visible" symptom - the ramps are compressed into whatever range
# the UVs cover, so max R tells us the u range and max G the v range:
#
#   max R ~ 255, max G ~ 255  -> UVs 0..1, geometry is fine
#   max R ~ 127               -> u only reaches 0.5  (half the frame's width)
#   max G ~ 170               -> v only reaches 0.667 (two thirds of the height)
#
#   bash /work/scripts/fivehearts/channels.sh <frames-dir> [file.png ...]
set -u
DIR="${1:-/f}"
shift || true

stats_one() {
    local image="$1"
    echo "--- $(basename "$image") ---"
    local ch
    for ch in R G B; do
        printf '%s: ' "$ch"
        convert "$image" -channel "$ch" -separate +channel \
            -format 'min=%[fx:minima*255] max=%[fx:maxima*255] mean=%[fx:mean*255]' info:
        echo
    done
}

if [ "$#" -gt 0 ]; then
    for image in "$@"; do
        stats_one "$DIR/$image"
    done
else
    for image in "$DIR"/*.png; do
        [ -e "$image" ] || continue
        stats_one "$image"
    done
fi
