#!/bin/bash

GAMEDIR="/mnt/mmc/Roms/PORTS/BrickGamePro"
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
require_file "$GAMEDIR/game/lib/arm64-v8a/libunity.so"
require_file "$GAMEDIR/game/lib/arm64-v8a/libil2cpp.so"
require_file "$GAMEDIR/game/assets/bin/Data/Managed/Metadata/global-metadata.dat"
require_file "$GAMEDIR/unityloader.d/brickgamepro.so"

chmod a+x "$PROGRAM" 2>/dev/null || true
export XDG_DATA_HOME="$GAMEDIR/conf"
export XDG_CONFIG_HOME="$GAMEDIR/conf"

export MALLOC_ARENA_MAX=2
export MALLOC_TRIM_THRESHOLD_=65536
export MALLOC_MMAP_THRESHOLD_=131072
export LP_NUM_THREADS=2

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

echo "[$(timestamp)] starting BrickGamePro" >> "$LOG"
echo "[$(timestamp)] program=$PROGRAM" >> "$LOG"
echo "[$(timestamp)] config=$CONFIG" >> "$LOG"

exec "$PROGRAM" "$CONFIG" >> "$LOG" 2>&1
