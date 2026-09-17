#!/bin/sh
# FiveHearts — start the game from the Ports frontend, headless (Dropbeak).
#
# The Ports frontend (dmenu.bin) owns the framebuffer, so a plain
# `dropbeak exec` of the launcher races it for the display and loses.
# The frontend's handoff contract is: write the command to run into
# /tmp/.next and poke dmenu.bin with SIGUSR1. It then tears its own UI
# down and execs that line. Always launch this way.
#
# Debug knobs are passed through with BD_ENV, which is a plain string of
# VAR=value pairs (the loader reads them with getenv, so anything the loader
# understands works - see docs/FIVEHEARTS.md §6):
#
#   BD_ENV="BD_VIDEO_DEBUG_GRADIENT=1 \
#           BD_DUMP_FRAME=/mnt/mmc/Roms/PORTS/FiveHearts/fbdump \
#           BD_DUMP_FRAME_AT=1200" sh launch.sh
#
# BD_VIDEO_DUMP / BD_VIDEO_DUMP_AT are kept as aliases for the two frame-dump
# variables because that is what earlier sessions used.
set -x
mount -o remount,exec /mnt/mmc 2>/dev/null || true
pkill -9 unityloader 2>/dev/null || true
sleep 1

env_pairs="${BD_ENV:-}"
if [ -n "${BD_VIDEO_DUMP:-}" ]; then
  env_pairs="$env_pairs BD_DUMP_FRAME=${BD_VIDEO_DUMP}"
fi
if [ -n "${BD_VIDEO_DUMP_AT:-}" ]; then
  env_pairs="$env_pairs BD_DUMP_FRAME_AT=${BD_VIDEO_DUMP_AT}"
fi

cmd='exec /mnt/mmc/Roms/PORTS/FiveHearts.sh'
if [ -n "$env_pairs" ]; then
  # shellcheck disable=SC2086 # intentional word splitting into VAR=value args
  cmd="exec env $env_pairs /mnt/mmc/Roms/PORTS/FiveHearts.sh"
fi
echo "launching: $cmd"
printf '%s\n' "$cmd" > /tmp/.next
killall -s SIGUSR1 dmenu.bin 2>/dev/null || true
sleep 2
ps -ef | grep -E 'dmenu|unityloader' | grep -v grep
