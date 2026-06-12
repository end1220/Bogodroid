#!/bin/bash
# 模板:用 `11-skip_egl` / `export POC_SKIP_EGL=1` 占位,gen 时替换。

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
if [ -d "/opt/system/Tools/PortMaster/" ]; then controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then controlfolder="$XDG_DATA_HOME/PortMaster"
else controlfolder="/roms/ports/PortMaster"
fi
source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR="/$directory/ports/poc_sdl2"
cd "$GAMEDIR"
PRESET="11-skip_egl"
> "$GAMEDIR/${PRESET}.log" && exec > >(tee "$GAMEDIR/${PRESET}.log") 2>&1

echo "[POC] preset=$PRESET"
export MALLOC_ARENA_MAX=2
export SDL_VIDEODRIVER=dummy
export SDL_AUDIODRIVER=dummy
export LD_LIBRARY_PATH="/usr/lib:/usr/lib64:/usr/trimui/lib:/mnt/SDCARD/System/lib"
export POC_DIAG=1
export POC_DIAG_FILE="$GAMEDIR/${PRESET}.diag"
> "$POC_DIAG_FILE"

# === PRESET ENV ===
export POC_SKIP_EGL=1
# ==================

echo "[POC] POC_* env:"
env | grep "^POC_" | sort

$GPTOKEYB "godot" &
pm_platform_helper "godot"

echo "[POC] launching..."
which strace 2>/dev/null && \
  strace -f -e 'trace=openat,ioctl,write,exit_group,signal' -o "$GAMEDIR/${PRESET}.strace" \
    ./godot --verbose --display-driver sdl2 --rendering-driver opengl3 --resolution 320x180 \
  || ./godot --verbose --display-driver sdl2 --rendering-driver opengl3 --resolution 320x180
echo "[POC] exit=$?"

pm_finish
