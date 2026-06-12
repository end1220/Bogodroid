#!/bin/bash
# PORTMASTER: sts2, [中]杀戮尖塔2-sdl2.sh
# StS2 自家 godot 4.5 mono + self-KMS+GBM+EGL,完全绕开 weston/crusty 老路。

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
if [ -d "/opt/system/Tools/PortMaster/" ]; then controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then controlfolder="$XDG_DATA_HOME/PortMaster"
else controlfolder="/roms/ports/PortMaster"
fi
source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR="/$directory/ports/sts2"
CONFDIR="$GAMEDIR/conf"
mkdir -p "$CONFDIR"
cd "$GAMEDIR"
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1
echo "[STS2-SDL2] CFW=$CFW_NAME ${DISPLAY_WIDTH}x${DISPLAY_HEIGHT} GAMEDIR=$GAMEDIR"

# malloc 调优(1GB 设备)
export MALLOC_ARENA_MAX=2
export MALLOC_TRIM_THRESHOLD_=131072
export MALLOC_MMAP_THRESHOLD_=131072

# 1.5GB swap on SD card (Mali 不支持 BPTC/S3TC,纹理 CPU 解压成原始 RGBA → 内存暴涨)
SWAP_FILE="/mnt/SDCARD/.sts2_swap"
if ! swapon -s 2>/dev/null | grep -q "$SWAP_FILE"; then
  [ ! -f "$SWAP_FILE" ] && dd if=/dev/zero of="$SWAP_FILE" bs=1M count=1536 status=none && mkswap "$SWAP_FILE" >/dev/null
  $ESUDO swapon "$SWAP_FILE" 2>&1
  $ESUDO sysctl -w vm.swappiness=80 >/dev/null 2>&1
fi
echo "[STS2-SDL2] swap: $(swapon -s | tail -1)"

# SDL2 用 dummy backend(事件/手柄);显示走我们 KMSGBM+EGL,SDL2 不碰
export SDL_VIDEODRIVER=dummy
export SDL_AUDIODRIVER=alsa

# .NET 9 self-contained globalization fallback(libicu 缺/版本不匹配时)
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1

# 清掉 control.txt / mod_TrimUI.txt 可能注入的 weston/crusty 路径污染
export LD_LIBRARY_PATH="/usr/lib:/usr/lib64:/usr/trimui/lib:/mnt/SDCARD/System/lib"
echo "[STS2-SDL2] LD_LIBRARY_PATH=$LD_LIBRARY_PATH"

# gptokeyb + 平台 helper
$GPTOKEYB "godot.mono" &
pm_platform_helper "godot.mono"

echo "[STS2-SDL2] launching..."
XDG_CONFIG_HOME="$CONFDIR" XDG_DATA_HOME="$CONFDIR" \
  ./godot.mono --verbose --display-driver sdl2 --rendering-driver opengl3 \
  --resolution 1280x720 --main-pack "$GAMEDIR/SlayTheSpire2.astc.pck"
echo "[STS2-SDL2] exit code: $?"

pm_finish
