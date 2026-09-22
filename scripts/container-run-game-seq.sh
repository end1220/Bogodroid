#!/bin/bash
# Generic screenshot-sequence runner, container side (GlES_Dev).
#
# Like /regress-run.sh (loader copied *into* the game dir so unityloader.d/*.so
# plugins resolve), but it also drives the loader's own glReadPixels dump so a
# black screen can be told apart from "nothing was captured".
#
#   GAME_DIR=/game/Oddmar SECS=120 DUMP_AT=1000 SEQ_DIR=/game/Oddmar/seqDEEP \
#     bash /run-any-seq.sh
#   GAME_DIR=/game/Samurai2 LOADER=/game/Samurai2/unityloader.ref SECS=60 \
#     DUMP_AT=200 SEQ_DIR=/game/Samurai2/seqcheck bash /run-any-seq.sh
#
# Artifacts in $SEQ_DIR: %04d.png (root window), timeline.txt, xwininfo.txt,
# frame.<n>.gl.ppm (glReadPixels), and $GAME_DIR/log-seq.txt.
#
# DUMP_AT is the *swap index* the first dump is taken at; the same offsets x2
# and x3 are dumped too. Swap rate is roughly 25/s here (software GL), so
# DUMP_AT=1000 lands about 40 s in -- early indices (1..50) are still boot.
set -u
GAME_DIR="${GAME_DIR:?GAME_DIR required}"
LOADER="${LOADER:-$GAME_DIR/unityloader}"
SECS="${SECS:-60}"
CAP_MS="${CAP_MS:-250}"
DUMP_AT="${DUMP_AT:-${BD_DUMP_FRAME_AT:-600}}"
SEQ_DIR="${SEQ_DIR:-$GAME_DIR/seq}"
LOG="$GAME_DIR/log-seq.txt"
W="${W:-640}"
H="${H:-480}"

rm -rf "$SEQ_DIR"; mkdir -p "$SEQ_DIR"

# Keep whatever shipped with the port so the reference run survives, exactly as
# /regress-run.sh does.
if [ ! -f "$GAME_DIR/unityloader.ref" ]; then
    cp -f "$GAME_DIR/unityloader" "$GAME_DIR/unityloader.ref" 2>/dev/null || true
fi
cp -f "$LOADER" "$GAME_DIR/unityloader"
chmod +x "$GAME_DIR/unityloader"

export SDL_VIDEODRIVER=x11
export LIBGL_ALWAYS_SOFTWARE=1
export DISPLAY=:99
export SDL_AUDIODRIVER=dummy
export SDL_GAMECONTROLLERCONFIG_FILE="$GAME_DIR/gamecontrollerdb.txt"

export BD_DUMP_FRAME="$SEQ_DIR/frame"
export BD_DUMP_FRAME_AT="$DUMP_AT"

pkill -x Xvfb 2>/dev/null
sleep 0.3
Xvfb :99 -screen 0 "${W}x${H}x24" -ac > "$GAME_DIR/xvfb.log" 2>&1 &
XVFB=$!
for i in $(seq 1 200); do
    xdpyinfo -display :99 >/dev/null 2>&1 && break
    kill -0 "$XVFB" 2>/dev/null || break
    sleep 0.1
done
if ! xdpyinfo -display :99 >/dev/null 2>&1; then
    echo "Xvfb :99 not ready"; cat "$GAME_DIR/xvfb.log"; exit 1
fi
echo "Xvfb :99 ready (${W}x${H})"

cd "$GAME_DIR" || exit 1

START_NS=$(date +%s%N)
timeout -s INT "$SECS" ./unityloader unity.toml > "$LOG" 2>&1 &
PID=$!

: > "$SEQ_DIR/timeline.txt"
IDX=0
XWIN_DONE=0
while kill -0 "$PID" 2>/dev/null; do
    OFF=$(( ($(date +%s%N) - START_NS) / 1000000 ))
    if [ "$XWIN_DONE" -eq 0 ] && [ "$OFF" -ge 3000 ]; then
        xwininfo -root -tree > "$SEQ_DIR/xwininfo.txt" 2>&1
        XWIN_DONE=1
    fi
    SHOT=$(printf '%s/%04d.png' "$SEQ_DIR" "$IDX")
    import -window root "$SHOT" >/dev/null 2>&1
    BYTES=$(stat -c%s "$SHOT" 2>/dev/null || echo 0)
    printf '%04d %6d ms %8d B\n' "$IDX" "$OFF" "$BYTES" >> "$SEQ_DIR/timeline.txt"
    IDX=$((IDX + 1))
    sleep "$(awk -v ms="$CAP_MS" 'BEGIN{printf "%.3f", ms/1000}')"
done
wait "$PID"
EXIT=$?
EXIT_MS=$(( ($(date +%s%N) - START_NS) / 1000000 ))
echo "unityloader exit=$EXIT after ${EXIT_MS}ms"
echo "captured $IDX frames into $SEQ_DIR"

if [ "$XWIN_DONE" -eq 0 ]; then
    xwininfo -root -tree > "$SEQ_DIR/xwininfo.txt" 2>&1
fi
kill "$XVFB" 2>/dev/null

echo "--- gl frame dumps ---"
ls -la "$SEQ_DIR"/frame.*.gl.ppm 2>/dev/null || echo "  (none: BD_DUMP_FRAME_AT never reached)"
echo "--- log tail ---"
tail -4 "$LOG"
