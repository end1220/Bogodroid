#!/bin/bash
# Visualise the UV field the guest submits for the video quad.
#
# The gradient probe makes the video texture R = u and G = v (0..255 over the
# full texture), so separating the frame's channels shows the UV mapping
# directly: R is "which column of the video", G is "which row of the video".
#
#   bash /work/scripts/fivehearts/uvmap.sh <frame.png> [cols] [rows]
#
# Printed as two ASCII maps (0 = 0.0, f = 1.0). A healthy full-screen video quad
# with UV 0..1 ramps 0 -> f across the whole width and top -> bottom; a quad that
# only samples a corner stops at 4/7 halfway across, which is exactly what
# "only the bottom-left of the video is visible" looks like.
#
# Both channels are averaged over each cell, so a cell that mixes video and
# static UI reads low - use the gradient's *shape* for the quad's extent.
set -u
IMAGE="${1:?usage: uvmap.sh <frame.png> [cols] [rows]}"
COLS="${2:-48}"
ROWS="${3:-18}"

map_channel() {
    local channel="$1" label="$2"
    echo "--- $label ($channel) ---"
    convert "$IMAGE" -channel "$channel" -separate +channel \
        -colorspace gray -resize "${COLS}x${ROWS}!" -depth 8 gray:- \
        | od -An -v -tu1 \
        | awk -v cols="$COLS" '{ for (i = 1; i <= NF; i++) { v = $i + 0; if (v > 255) v = 255; printf "%x", int(v * 15 / 255); n++; if (n % cols == 0) printf "\n"; } }'
}

map_channel R 'u = R/255 (0=left edge of video, f=right edge)'
map_channel G 'v = G/255 (0=bottom edge of video, f=top edge)'
