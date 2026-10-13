#!/bin/bash
# Template for TrimUI Unity ports using Bogodroid unityloader.
# Replace PORT_ROOT, GAME_DIR_NAME, GAME_DATA_DIR, and plugin/video checks per game.
# If PortMaster setup is needed, keep the port's control.txt/helper wrapper.

PORT_ROOT="/mnt/SDCARD/Data/ports"
GAME_DIR_NAME="ExampleGame"
GAME_DATA_DIR="gamedata"
GAMEDIR="$PORT_ROOT/$GAME_DIR_NAME"
PROGRAM="$GAMEDIR/unityloader"
CONFIG="$GAMEDIR/unity.toml"
LOG="$GAMEDIR/log.txt"

cd "$GAMEDIR" || exit 1

mkdir -p "$GAMEDIR/conf/shared_prefs" "$GAMEDIR/cache" "$GAMEDIR/unityloader.d"
: > "$LOG"

timestamp() {
  date -Iseconds 2>/dev/null || date "+%Y-%m-%dT%H:%M:%S"
}

require_file() {
  if [ ! -f "$1" ]; then
    echo "[$(timestamp)] ERROR: missing $1" >> "$LOG"
    exit 1
  fi
}

require_file "$PROGRAM"
require_file "$CONFIG"
require_file "$GAMEDIR/$GAME_DATA_DIR/lib/arm64-v8a/libunity.so"

# For plugin-based patches, add one require_file per required plugin:
# require_file "$GAMEDIR/unityloader.d/example_plugin.so"

chmod a+x "$PROGRAM" 2>/dev/null || true

export XDG_DATA_HOME="$GAMEDIR/conf"
export XDG_CONFIG_HOME="$GAMEDIR/conf"

# Optional generic RSS/soft-renderer tuning. Not title-specific.
export MALLOC_ARENA_MAX=2
export MALLOC_TRIM_THRESHOLD_=65536
export MALLOC_MMAP_THRESHOLD_=131072
export LP_NUM_THREADS=2

# Only video ports that use Android media need FFmpeg in LD_LIBRARY_PATH.
# If FFmpeg is bundled inside the port:
# export LD_LIBRARY_PATH="$GAMEDIR/ff58${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# If the firmware has shared FFmpeg 4.2 copies:
# export LD_LIBRARY_PATH="/mnt/SDCARD/System/lib:$GAMEDIR/ff58${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
#
# Oddmar-style video path translation is title-specific:
# export BD_BYPASS_VIDEO_TRANSLATE=1

if [ -f "$GAMEDIR/gamecontrollerdb.txt" ]; then
  export SDL_GAMECONTROLLERCONFIG_FILE="$GAMEDIR/gamecontrollerdb.txt"
fi

if pgrep -x pulseaudio >/dev/null 2>&1 || pgrep -x pipewire-pulse >/dev/null 2>&1; then
  export SDL_AUDIODRIVER=pulse
else
  export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-alsa}"
  if [ -z "${AUDIODEV:-}" ] && grep -qi audiocodec /proc/asound/cards 2>/dev/null; then
    export AUDIODEV=plughw:audiocodec
  fi
fi

echo "[$(timestamp)] starting $GAME_DIR_NAME" >> "$LOG"
echo "[$(timestamp)] program=$PROGRAM" >> "$LOG"
echo "[$(timestamp)] config=$CONFIG" >> "$LOG"

"$PROGRAM" "$CONFIG" >> "$LOG" 2>&1
ec=$?

echo "[$(timestamp)] unityloader exited ($ec)" >> "$LOG"
printf "\033c" > /dev/tty1 2>/dev/null || true
exit "$ec"
