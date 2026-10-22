#!/bin/bash
# Run inside GlES_Dev against an isolated copy, never handheld save data.
set -eu
GAME_DIR=${GAME_DIR:-/game/SpaceSquids-cursor}
CASE=${CASE:-probe}
SECS=${SECS:-30}
GAME_CONFIG=${GAME_CONFIG:-unity.toml}
SCREEN_WIDTH=${SCREEN_WIDTH:-640}
SCREEN_HEIGHT=${SCREEN_HEIGHT:-480}
OUT="$GAME_DIR/results/$CASE"
for tool in Xvfb xdpyinfo import timeout; do
    command -v "$tool" >/dev/null || { echo "Missing tool: $tool" >&2; exit 1; }
done
mkdir -p "$OUT" "$GAME_DIR/cache" "$GAME_DIR/unityloader.d"
export DISPLAY=${DISPLAY:-:109} SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy
export LIBGL_ALWAYS_SOFTWARE=1
export SDL_GAMECONTROLLERCONFIG_FILE="$GAME_DIR/gamecontrollerdb.txt"
export BD_MEM_LOG_MS=0
PID=
XVFB=
cleanup() {
    if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
        kill -INT "$PID" 2>/dev/null || true
        wait "$PID" 2>/dev/null || true
    fi
    if [ -n "$XVFB" ]; then
        kill "$XVFB" 2>/dev/null || true
        wait "$XVFB" 2>/dev/null || true
    fi
}
trap cleanup EXIT
if xdpyinfo -display "$DISPLAY" >/dev/null 2>&1; then
    echo "Display $DISPLAY is already in use; choose a separate DISPLAY" >&2
    exit 1
fi
Xvfb "$DISPLAY" -screen 0 "${SCREEN_WIDTH}x${SCREEN_HEIGHT}x24" -ac >"$OUT/xvfb.log" 2>&1 &
XVFB=$!
for i in $(seq 1 100); do
    kill -0 "$XVFB" 2>/dev/null || { cat "$OUT/xvfb.log" >&2; exit 1; }
    xdpyinfo -display "$DISPLAY" >/dev/null 2>&1 && break
    sleep .1
done
xdpyinfo -display "$DISPLAY" >/dev/null 2>&1 || { echo "Xvfb did not become ready" >&2; exit 1; }
cd "$GAME_DIR"
# 124 means the requested duration elapsed; 137/139/etc. are failures.
timeout -k 5 -s INT "$SECS" ./unityloader "$GAME_CONFIG" >"$OUT/log.txt" 2>&1 &
PID=$!
START=$(date +%s%N)
IDX=0
: >"$OUT/timeline.txt"
while kill -0 "$PID" 2>/dev/null; do
    import -window root "$(printf '%s/%03d.png' "$OUT" "$IDX")"
    printf '%03d %d\n' "$IDX" "$(( ($(date +%s%N) - START) / 1000000 ))" >>"$OUT/timeline.txt"
    IDX=$((IDX+1))
    sleep 1
done
wait "$PID" && STATUS=0 || STATUS=$?
PID=
printf 'exit=%s shots=%s\n' "$STATUS" "$IDX"
grep -aE '\[BD-SQUIDS\]|controller input|SEGV|terminate|binding failed' "$OUT/log.txt" | tail -150 || true
case "$STATUS" in
    0|124) ;;
    *) exit "$STATUS" ;;
esac
if grep -aqE '\[BD-SEGV\]|Invalid Reference, Unexpected Type|terminate called|UI binding/layout failed' "$OUT/log.txt"; then
    echo "Regression failed; inspect $OUT/log.txt" >&2
    exit 1
fi
