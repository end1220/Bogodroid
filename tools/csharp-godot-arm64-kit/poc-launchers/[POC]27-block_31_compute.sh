#!/bin/bash
# 模板:用 PRESET_NAME / PRESET_BLACKLIST 占位,gen 时替换。

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
PRESET="27-block_31_compute"
> "$GAMEDIR/${PRESET}.log" && exec > >(tee "$GAMEDIR/${PRESET}.log") 2>&1

echo "[POC] preset=$PRESET (shim + blacklist)"
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
export EGL_SHIM_BLACKLIST="glDispatchCompute:glDispatchComputeIndirect:glMemoryBarrier:glMemoryBarrierByRegion:glDrawArraysIndirect:glDrawElementsIndirect:glBindImageTexture:glTexStorage2DMultisample:glTexStorage3DMultisample:glFramebufferTextureMultiviewOVR:glFramebufferTextureMultisampleMultiviewOVR:glProgramUniform1iv:glProgramUniform1uiv:glProgramUniform2iv:glProgramUniform3iv:glProgramUniform4iv:glProgramUniform1fv:glProgramUniform2fv:glProgramUniform3fv:glProgramUniform4fv:glProgramUniformMatrix4fv:glProgramUniformMatrix3fv"
echo "[POC] EGL_SHIM_BLACKLIST=$EGL_SHIM_BLACKLIST"

$GPTOKEYB "godot" &
pm_platform_helper "godot"

echo "[POC] launching..."
./godot --verbose --display-driver sdl2 --rendering-driver opengl3 --resolution 320x180
echo "[POC] exit=$?"
pm_finish
