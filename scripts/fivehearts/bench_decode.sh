#!/bin/sh
# Decode-cost microbenchmark for the H700, run on the handheld.
#
# "The video is stuttery" is decided by how many milliseconds of CPU one 720p
# H.264 frame costs, so measure that in isolation with mpv (null video/audio
# output: decode only, no display, no scaling). Compare the rows to see how much
# of the decode FFmpeg's speed flags can buy us before touching the loader.
#
#   sh /mnt/mmc/Roms/PORTS/FiveHearts/bench_decode.sh [file] [frames]
V="${1:-/mnt/mmc/Video/INTRO.mp4}"
FRAMES="${2:-300}"

now_ms() {
    awk '{printf "%d", $1 * 1000}' /proc/uptime
}

run() {
    label="$1"
    shift
    # shellcheck disable=SC2086 # the remaining args are the decoder options
    S=$(now_ms)
    mpv --no-config --vo=null --ao=null --frames="$FRAMES" --really-quiet "$@" "$V" \
        >/dev/null 2>&1
    rc=$?
    E=$(now_ms)
    total=$((E - S))
    echo "$label	rc=$rc	total_ms=$total	per_frame_ms=$((total / FRAMES))"
}

echo "file=$V frames=$FRAMES"
run baseline
run fast_skiploop	--vd-lavc-o=flags2=+fast:skip_loop_filter=all
run skiploop_only	--vd-lavc-o=skip_loop_filter=all
run fast_only		--vd-lavc-o=flags2=+fast
run threads1		--vd-lavc-o=threads=1
run threads2		--vd-lavc-o=threads=2
run fast_t2		--vd-lavc-o=flags2=+fast:skip_loop_filter=all:threads=2
