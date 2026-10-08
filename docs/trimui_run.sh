#!/bin/sh
# Oddmar foreground runner for the stock TrimUI supervisor.
#
# Reached through /tmp/cmd_to_run.sh, which runtrimui.sh executes in the
# foreground only after MainUI has exited. Staying in the foreground is the
# point: if we backgrounded the loader, runtrimui.sh would relaunch MainUI on
# top of it and the panel would alternate between the two.
#
# The environment mirrors the shipped /mnt/SDCARD/Roms/PORTS/Oddmar.sh. That
# matters because runtrimui.sh exports LD_LIBRARY_PATH=/mnt/SDCARD/trimui/lib
# with a plain assignment (no append), so whatever we inherit has no FFmpeg
# 4.2 in it.
#
# Deliberately has NO background screenshot loop: reading /dev/fb0 in a loop
# competes with the page flips and shows up as a hitch every couple of seconds.
set -x

ODD="/mnt/SDCARD/Data/ports/Oddmar"
cd "$ODD" || exit 1

# /usr/lib ships FFmpeg 6 (libav*.so.60); this loader links FFmpeg 4.2
# (libav*.so.58). System/lib and the port's own ff58/ carry matching copies.
export LD_LIBRARY_PATH="/mnt/SDCARD/System/lib:$ODD/ff58${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# Unity 2018's AndroidVideoMedia rejects the hostless jar:file://!/assets/...
# form before it reaches the NDK extractor, so the loader answers that helper
# itself. Without it the cutscenes never decode.
export BD_BYPASS_VIDEO_TRANSLATE=1
export MALLOC_ARENA_MAX=2
export MALLOC_TRIM_THRESHOLD_=65536
export MALLOC_MMAP_THRESHOLD_=131072
export LP_NUM_THREADS=2
export SDL_GAMECONTROLLERCONFIG_FILE="$ODD/gamecontrollerdb.txt"
export XDG_DATA_HOME="$ODD/conf"
export XDG_CONFIG_HOME="$ODD/conf"

if pgrep -x pulseaudio >/dev/null 2>&1 || pgrep -x pipewire-pulse >/dev/null 2>&1; then
  export SDL_AUDIODRIVER=pulse
else
  export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-alsa}"
  if [ -z "${AUDIODEV:-}" ] && grep -qi audiocodec /proc/asound/cards 2>/dev/null; then
    export AUDIODEV=plughw:audiocodec
  fi
fi

# Frame dumps are a debugging aid, not part of the shipped configuration, and
# are therefore OFF by default. Set these before calling this script to enable
# them; the loader reads them with plain getenv(), so they still work in a
# BD_ENABLE_LOG=OFF build:
#   BD_DUMP_FRAME=<prefix>       GL drawable + both panel framebuffers (.ppm)
#   BD_DUMP_FRAME_AT=<swap>      default 600, i.e. ~10 s at 60 Hz (also x2, x3)
#   BD_VIDEO_DUMP_FRAME=<prefix> decoded video frames handed to submit_i420()
#   BD_VIDEO_DUMP_AT=<frame>     default 40
# A run with all four set leaves roughly 31 MB in the port directory, which is
# why nothing here turns them on.
rm -f "$ODD"/dump.*.ppm "$ODD"/early.*.ppm "$ODD"/vframe.*.ppm 2>/dev/null
: > "$ODD/log.txt"

chmod a+x "$ODD/unityloader"

# Foreground on purpose: see the header comment.
exec "$ODD/unityloader" unity.toml >>"$ODD/log.txt" 2>&1
