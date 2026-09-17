#!/bin/bash
# Frame timeline: runs the loader and grabs the root window once per second
# WHILE it is alive, so the run shows what the engine actually presents
# (black -> splash -> intro video -> lobby) instead of one post-mortem frame of a
# window that has already been torn down. A PNG of a few hundred bytes is a
# uniform screen; tens of kB of neighbours means real frames.
#
#   SECS=25 bash /work/scripts/fivehearts/frames.sh
#
# Frames land in /game/frames/fNN.png; the "still black" frames are kept on
# purpose, the transition is the evidence.
set -u
: "${SECS:=25}"
: "${STEP:=1}"
OUT=/game/frames
LOG=/game/log.txt

rm -rf "$OUT"; mkdir -p "$OUT"

export SDL_VIDEODRIVER=x11
export LIBGL_ALWAYS_SOFTWARE=1
export DISPLAY=:99
export SDL_AUDIODRIVER=dummy
[ -n "${JTRACE:-}" ] && export BD_JNI_TRACE="$JTRACE"

Xvfb :99 -screen 0 640x480x24 -ac > /game/xvfb.log 2>&1 &
XVFB=$!
for i in $(seq 1 200); do
    [ -e /tmp/.X11-unix/X99 ] || { kill -0 "$XVFB" 2>/dev/null || break; sleep 0.1; continue; }
    xdpyinfo -display :99 >/dev/null 2>&1 && break
    sleep 0.1
done
echo "Xvfb :99 accepting connections after ${i}00ms"

cd /game || exit 1
cp /boot/unityloader /tmp/bd-unityloader
cp /boot/fivehearts.toml /tmp/bd-fivehearts.toml
chmod +x /tmp/bd-unityloader

timeout -s INT "$SECS" /tmp/bd-unityloader /tmp/bd-fivehearts.toml > "$LOG" 2>&1 &
PID=$!

n=0
guard=0
while kill -0 "$PID" 2>/dev/null; do
    import -window root "$OUT/f$(printf '%02d' $n).png" >/dev/null 2>&1
    n=$((n+1))
    sleep "$STEP"
    guard=$((guard+1))
    [ "$guard" -gt $((SECS + 10)) ] && break
done
wait "$PID"; echo "unityloader exit=$?"

echo "frames: $n"
ls -l "$OUT" | awk '{print $5, $9}'
echo
echo "=== Unity's own log lines (only present in LOG builds) ==="
grep -E 'LOG\[Unity\]:' "$LOG" | tail -12 || true
echo
echo "=== video bridge ==="
grep -E 'BD-VIDEO' "$LOG" | head -30 || true
echo
echo "=== crash markers (want 0) ==="
grep -cE 'LOG\[CRASH\]|signal 11' "$LOG" || true
kill $XVFB 2>/dev/null
