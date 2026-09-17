#!/bin/bash
# Coarse ASCII map of a frame: downscale to COLSxROWS and print each cell's
# luminance as a hex digit (0 = black, f = white). Enough to tell "uniform
# black", "black with a UI strip at the top", "bright rectangle in the middle
# where the video should be" apart without opening a PNG.
#
#   bash /work/scripts/fivehearts/look.sh <frames-dir> [frame.png]
#
# With no file it prints a map for every frame in the directory.
#
# The pixels are read as raw bytes (`gray:-` piped through od) rather than from
# ImageMagick's text output: the text form varies with the image's channel count
# and colourspace, which made a first version of this script report an all-white
# map for a mostly-black frame.
set -u
DIR="${1:-/f}"
COLS="${COLS:-48}"
ROWS="${ROWS:-18}"

map_one() {
    local image="$1"
    echo "--- $(basename "$image") ---"
    convert "$image" -colorspace gray -resize "${COLS}x${ROWS}!" -depth 8 gray:- \
        | od -An -v -tu1 \
        | awk -v cols="$COLS" '{ for (i = 1; i <= NF; i++) { v = $i + 0; if (v > 255) v = 255; printf "%x", int(v * 15 / 255); n++; if (n % cols == 0) printf "\n"; } }'
}

if [ -n "${2:-}" ]; then
    map_one "$DIR/$2"
else
    for image in "$DIR"/*.png; do
        [ -e "$image" ] || continue
        map_one "$image"
    done
fi
