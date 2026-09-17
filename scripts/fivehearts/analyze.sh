#!/bin/bash
# Summarise a frame timeline: for every grabbed root-window PNG print mean
# brightness, standard deviation and the centre crop's mean. A uniform screen
# (splash, black) shows sd ~ 0; a rendered scene has sd in the tens; and a video
# that is present but black shows a dark centre while the UI around it is lit.
#
#   bash /work/scripts/fivehearts/analyze.sh [frames-dir]
#
# Run it inside the container (imagemagick lives there):
#   docker run --rm --platform linux/arm64 \
#     -v "<stage>/FiveHearts/frames:/f" \
#     -v "<repo>/scripts/fivehearts:/work/scripts/fivehearts" \
#     bogo-arm64-test:20.04 bash /work/scripts/fivehearts/analyze.sh /f
set -u
DIR="${1:-/f}"
printf '%-10s %8s %8s %8s %8s %8s\n' frame mean sd min max centre
for image in "$DIR"/*.png; do
    [ -e "$image" ] || continue
    name=$(basename "$image")
    read -r mean sd min max < <(identify -format '%[fx:int(mean*255)] %[fx:int(standard_deviation*255)] %[fx:int(minima*255)] %[fx:int(maxima*255)]' "$image")
    centre=$(convert "$image" -gravity center -crop 50%x50%+0+0 +repage -format '%[fx:int(mean*255)]' info:)
    printf '%-10s %8s %8s %8s %8s %8s\n' "$name" "$mean" "$sd" "$min" "$max" "$centre"
done
