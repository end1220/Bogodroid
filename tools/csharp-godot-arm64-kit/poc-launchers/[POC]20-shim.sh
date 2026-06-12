#!/bin/bash
# Preset 20: LD_PRELOAD shim_egl.so 抓 godot 启动时每次 eglGetProcAddress 查询。
# 目的:找出 Mali NULL deref 的那个 GL 函数名,作 godot RasterizerGLES3 修补的靶。

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
PRESET="20-shim"
> "$GAMEDIR/${PRESET}.log" && exec > >(tee "$GAMEDIR/${PRESET}.log") 2>&1

echo "[POC] preset=$PRESET (LD_PRELOAD shim_egl.so)"
export MALLOC_ARENA_MAX=2
export SDL_VIDEODRIVER=dummy
export SDL_AUDIODRIVER=dummy
export LD_LIBRARY_PATH="/usr/lib:/usr/lib64:/usr/trimui/lib:/mnt/SDCARD/System/lib"
export POC_DIAG=1
export POC_DIAG_FILE="$GAMEDIR/${PRESET}.diag"
> "$POC_DIAG_FILE"

# 关键:LD_PRELOAD shim 抓 eglGetProcAddress + eglQueryString
export LD_PRELOAD="$GAMEDIR/shim_egl.so"
export EGL_SHIM_LOG_FILE="$GAMEDIR/${PRESET}.shim"
> "$EGL_SHIM_LOG_FILE"

$GPTOKEYB "godot" &
pm_platform_helper "godot"

echo "[POC] launching with shim..."
./godot --verbose --display-driver sdl2 --rendering-driver opengl3 --resolution 320x180
echo "[POC] exit=$?"
pm_finish
