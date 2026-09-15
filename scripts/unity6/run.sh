#!/bin/bash
# Runs the aarch64 unityloader against the extracted Unity 6 build in /game,
# inside the bogo-arm64-test container (see Dockerfile.test). The host mounts
# this directory at /work/scripts and gamefiles/unity6 at /game.
#
#   SECS=40 JTRACE=1 bash /work/scripts/run.sh
#
#   SECS    how long to stay up before SIGINT (default 60)
#   JTRACE  BD_JNI_TRACE=1: full libjnivm trace (tens of thousands of lines)
#   OUT     screenshot path inside the container (default /game/shot.png)
#
# A run is judged by the log, not by the exit code: timeout -s INT reports 124
# for a loader that was still healthy when the clock ran out.
set -u
: "${SECS:=60}"
: "${OUT:=/game/shot.png}"
LOG=/game/log.txt

# Software GL: no GPU in the container, and Unity's own EGL entry points are
# stubbed (BD_FAKE_EGL) so SDL creates the real context.
export SDL_VIDEODRIVER=x11
export LIBGL_ALWAYS_SOFTWARE=1
export DISPLAY=:99
export SDL_AUDIODRIVER=dummy
[ -n "${JTRACE:-}" ] && export BD_JNI_TRACE="$JTRACE"

# Start Xvfb and WAIT for it to accept clients. A bare `sleep 2` is a race under
# qemu: when Xvfb is not ready, SDL_Init(SDL_INIT_VIDEO) fails with "x11 not
# available" and unityloader takes fatal_error -> SIGABRT, which looks exactly
# like a Unity-side crash (tombstone, exit 134) and wastes a whole debug cycle.
# Polling the socket alone is still not enough - the socket can exist before the
# server accepts connections - so ask xdpyinfo.
Xvfb :99 -screen 0 640x480x24 -ac > /game/xvfb.log 2>&1 &
XVFB=$!
for i in $(seq 1 200); do
    [ -e /tmp/.X11-unix/X99 ] || { kill -0 "$XVFB" 2>/dev/null || break; sleep 0.1; continue; }
    xdpyinfo -display :99 >/dev/null 2>&1 && break
    sleep 0.1
done
if ! kill -0 "$XVFB" 2>/dev/null; then
    echo "Xvfb :99 died before accepting connections:"
    cat /game/xvfb.log
    exit 1
fi
echo "Xvfb :99 accepting connections after ${i}00ms"

cd /game || exit 1
# Both the binary and its toml have to live on the container's own overlay:
# ld.so mmaps the ELF straight off the bind mount and a short read comes back
# as
#   Inconsistency detected by ld.so: rtld.c: 1494: dl_main: Assertion
#   `GL(dl_rtld_map).l_libname' failed!
# and toml++'s parse_file() sees the mounted toml as empty (fatal_error
# "Could not change directory to "). game_files stays "./" so the process still
# needs /game as its cwd - only the two files are copied out.
cp ./unityloader /tmp/bd-unityloader
cp ./unity6.toml /tmp/bd-unity6.toml
chmod +x /tmp/bd-unityloader

timeout -s INT "$SECS" /tmp/bd-unityloader /tmp/bd-unity6.toml > "$LOG" 2>&1 &
PID=$!

# Screenshot WHILE it runs. The engine's first frames are black by design (no
# swap yet) and the splash that follows is a uniform colour too, so grab as late
# as possible: taking it after the process exits would only capture an empty
# root window and read as a false black screen.
SHOT_AT=$(( SECS > 3 ? SECS - 2 : SECS ))
[ "$SHOT_AT" -lt 1 ] && SHOT_AT=1
sleep "$SHOT_AT"
DISPLAY=:99 import -window root "$OUT" >/dev/null 2>&1

wait "$PID"
echo "unityloader exit=$?"
echo "log lines: $(wc -l < "$LOG")"
if [ -f "$OUT" ]; then
    echo "shot (t=${SHOT_AT}s): $(stat -c%s "$OUT") bytes - hundreds = uniform screen, tens of kB = scene content"
fi

kill $XVFB 2>/dev/null
