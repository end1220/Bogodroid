#!/bin/bash
# Compare the frames BD_DUMP_FRAME wrote and read out the video quad's UV range.
#
#   bash /work/scripts/fivehearts/dumpcheck.sh <dir> <dump-tag>
#
# Three frames are written at each dump point, all the same size:
#   <tag>.gl.ppm   what glReadPixels sees (Mesa/Mali back buffer)
#   <tag>.fb0.ppm / .fb1.ppm  the two halves of /dev/fb0 (mali-fbdev page-flips,
#                             so one of them is the frame actually on the panel)
# A .gl frame that differs from both .fb halves means the GL rendering is fine
# and the present path is the problem; identical frames mean the screen really
# shows what the guest drew.
#
# With the gradient probe on (BD_VIDEO_DEBUG_GRADIENT=1) the R channel of the
# video quad is u and G is v, so uvmap.sh's output on the same file is a direct
# readout of which part of the decoded frame made it to the screen.
set -u
DIR="${1:?usage: dumpcheck.sh <dir> <tag> [<uploaded-frame>]}"
TAG="${2:?usage: dumpcheck.sh <dir> <tag> [<uploaded-frame>]}"
UP="${3:-}"

cd "$DIR" || exit 1
for which in "$TAG.gl" "$TAG.fb0" "$TAG.fb1"; do
    [ -f "$which.ppm" ] || { echo "missing $which.ppm"; exit 1; }
    convert "$which.ppm" "$which.png"
done

echo "=== identical frames ==="
if cmp -s "$TAG.gl.ppm" "$TAG.fb0.ppm"; then echo "gl == fb0"; else echo "gl != fb0"; fi
if cmp -s "$TAG.gl.ppm" "$TAG.fb1.ppm"; then echo "gl == fb1"; else echo "gl != fb1"; fi
if cmp -s "$TAG.fb0.ppm" "$TAG.fb1.ppm"; then echo "fb0 == fb1"; else echo "fb0 != fb1"; fi

echo
bash /work/scripts/fivehearts/channels.sh "$DIR" "$TAG.gl.png" "$TAG.fb0.png" "$TAG.fb1.png"
if [ -n "$UP" ] && [ -f "$UP.ppm" ]; then
    convert "$UP.ppm" "$UP.png"
    echo
    # The uploaded frame is the loader's own RGBA: if the screen only shows a
    # corner of it, the difference between these two is the whole bug.
    bash /work/scripts/fivehearts/channels.sh "$DIR" "$UP.png"
fi

echo
echo "=== UV range of the video quad ($TAG.gl.png) ==="
bash /work/scripts/fivehearts/uvmap.sh "$DIR/$TAG.gl.png" 48 16
