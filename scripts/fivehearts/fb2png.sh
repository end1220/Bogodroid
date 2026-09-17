#!/bin/bash
# Convert a raw /dev/fb0 capture into PNGs and summarise them.
#
#   bash /work/scripts/fivehearts/fb2png.sh <fb.raw> [outdir]
#
# The Anbernic panel reports virtual_size=640,960 with 32bpp and stride 2560,
# i.e. two 640x480 buffers stacked in one mapping (page-flip double buffer). The
# visible one is whichever the driver last panned to, so both halves are written
# out and summarised; the buffer holding the game is the one with structure
# (sd in the tens) rather than a uniform sheet.
#
# Channel order on Mali fbdev is BGRA; if the output looks colour-swapped, that
# is the knob to change.
set -u
RAW="${1:?usage: fb2png.sh <fb.raw> [outdir]}"
OUT="${2:-/out}"
COLS="${COLS:-40}"
ROWS="${ROWS:-15}"
mkdir -p "$OUT"

for half in 0 1; do
    skip=$(( half * 480 ))
    png="$OUT/fb-half$half.png"
    convert -size 640x960 -depth 8 BGRA:"$RAW" -crop 640x480+0+0 +repage \
        -roll "+0-${skip}" -alpha off "$png"
    read -r mean sd min max < <(identify -format '%[fx:int(mean*255)] %[fx:int(standard_deviation*255)] %[fx:int(minima*255)] %[fx:int(maxima*255)]' "$png")
    centre=$(convert "$png" -gravity center -crop 50%x50%+0+0 +repage -format '%[fx:int(mean*255)]' info:)
    echo "=== half$half: mean=$mean sd=$sd min=$min max=$max centre=$centre ==="
    convert "$png" -colorspace gray -resize "${COLS}x${ROWS}!" -depth 8 gray:- \
        | od -An -v -tu1 \
        | awk -v cols="$COLS" '{ for (i = 1; i <= NF; i++) { v = $i + 0; if (v > 255) v = 255; printf "%x", int(v * 15 / 255); n++; if (n % cols == 0) printf "\n"; } }'
done
