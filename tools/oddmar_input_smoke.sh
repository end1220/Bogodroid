#!/usr/bin/env bash
set -eu

loader=${1:-/workspace/Bogodroid/build-oddmar/unityloader}
config=${2:-/tmp/oddmar-input-v3.toml}
game=${3:-/game/Oddmar}
output=$(mktemp -d /tmp/oddmar-input-smoke.XXXXXX)
echo "logs=$output"
export DISPLAY=:98 SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1
Xvfb "$DISPLAY" -screen 0 640x480x24 -ac > "$output/xvfb.log" 2>&1 &
xvfb_pid=$!
trap 'kill "$xvfb_pid" 2>/dev/null || true' EXIT
for ((i=0; i<50; ++i)); do
    xdpyinfo -display "$DISPLAY" >/dev/null 2>&1 && break
    sleep 0.1
done
xdpyinfo -display "$DISPLAY" >/dev/null
cd "$game"

set +e
BD_PAD_REPLAY='8000:a,11000:b,14000:x,14600:x,15200:x,17000:y,20000:start,23000:guide' \
BD_PAD_REPLAY_HOLD=300 timeout -s INT 26 "$loader" "$config" > "$output/buttons.log" 2>&1
buttons_exit=$?
set -e
test "$buttons_exit" = 124
grep -q 'Oddmar SDL button bridge v4' "$output/buttons.log"
for index in 0 1 2 3; do
    printf -v mask '%x' "$((1 << index))"
    grep -q "Oddmar raw button index=$index native=.* SDL=1 mask=0x$mask$" "$output/buttons.log"
    if grep -E "Oddmar raw button index=[0-3] native=.* SDL=1 mask=0x$mask$" "$output/buttons.log" |
       grep -qv "index=$index "; then exit 1; fi
done
test "$(grep -c 'Oddmar raw button index=2 native=.* SDL=1 mask=0x4$' "$output/buttons.log")" -ge 3
if grep -Eq 'BD-SEGV|terminate called|detour:.*(arena full|unrelocatable)' "$output/buttons.log"; then exit 1; fi

BD_PAD_REPLAY='8000:start,8100:back' BD_PAD_REPLAY_HOLD=300 \
timeout -s INT 14 "$loader" "$config" > "$output/hotkey.log" 2>&1
grep -q 'Start+Select exit hotkey (replay)' "$output/hotkey.log"
echo "PASS: independent ABXY states, Start+Select exit=0; logs=$output"
grep -E 'Oddmar raw button|Oddmar SDL|BD-EXIT' "$output/buttons.log" "$output/hotkey.log"
