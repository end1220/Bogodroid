#!/bin/bash
# Preset 26: shim + full strace(无 -e filter)
# 比 20-shim 多:strace 捕获 ALL syscalls(mmap/mprotect/futex/...),
# 给我们交棒后 → SIGSEGV 之间 godot 在 libmali 里实际干了啥的最完整证据。

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
PRESET="26-shim_full_strace"
> "$GAMEDIR/${PRESET}.log" && exec > >(tee "$GAMEDIR/${PRESET}.log") 2>&1

echo "[POC] preset=$PRESET (shim + FULL strace)"
export MALLOC_ARENA_MAX=2
export SDL_VIDEODRIVER=dummy
export SDL_AUDIODRIVER=dummy
export LD_LIBRARY_PATH="/usr/lib:/usr/lib64:/usr/trimui/lib:/mnt/SDCARD/System/lib"
export POC_DIAG=1
export POC_DIAG_FILE="$GAMEDIR/${PRESET}.diag"
> "$POC_DIAG_FILE"

export LD_PRELOAD="$GAMEDIR/shim_egl.so"
export EGL_SHIM_LOG_FILE="$GAMEDIR/${PRESET}.shim"
> "$EGL_SHIM_LOG_FILE"

$GPTOKEYB "godot" &
pm_platform_helper "godot"

echo "[POC] launching with shim + FULL strace..."
# 注意:不带 -e,所有 syscall 都抓。文件会很大(几十 MB)。
strace -f -o "$GAMEDIR/${PRESET}.strace" \
  ./godot --verbose --display-driver sdl2 --rendering-driver opengl3 --resolution 320x180
echo "[POC] exit=$?"

# tail strace 末尾 200 行直接进 .strace_tail,方便快查
tail -200 "$GAMEDIR/${PRESET}.strace" > "$GAMEDIR/${PRESET}.strace_tail"

pm_finish
