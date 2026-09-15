#!/bin/bash
# Unity6 — Ports launcher (Unity 6000.6 GameActivity + unityloader).
#
# Deploy as /mnt/mmc/Roms/PORTS/Unity6.sh (the menu entry name comes from this
# file name), next to the port directory /mnt/mmc/Roms/PORTS/Unity6/ holding
# unityloader, unity.toml (= configs/unity6-device.toml) and gamedata/.
#
#   scripts/unity6/device-launcher.sh  ->  <PORTS>/Unity6.sh
#
# Two things this script exists to get right:
#   * The frontend (dmenu.bin) owns /dev/fb0. Launch from the Ports menu, or use
#     the menu's own contract (write the command to /tmp/.next and SIGINT
#     dmenu.bin). Launching unityloader directly makes SDL fail with
#     "mali-fbdev: Can't create EGL window surface" and exit 134 - that is a
#     display-ownership problem, not a Unity or loader bug.
#   * libgame.so must be present: Unity 6 ships its Java-side entry point there
#     (GameActivity), not in libmain.so, and without it initJni() never runs.
#
# gamedata/ is uploaded from the host; this script never unpacks APK content.
# Note that a Release loader (BD_ENABLE_LOG=OFF) forwards no LOG[Unity] lines
# into log.txt - the game script's own log/unity_player.log is the portable
# evidence that the scene actually loaded.

progdir="$(cd "$(dirname "$0")" && pwd)"
rundir="${progdir}/Unity6"
cd "${rundir}" || exit 1

program="${rundir}/unityloader"
config="${rundir}/unity.toml"
log_file="${rundir}/log.txt"

printf "\033c" > /dev/tty0 2>/dev/null || true
printf "\033c" > /dev/tty1 2>/dev/null || true

timestamp() {
  date -Iseconds 2>/dev/null || date "+%Y-%m-%dT%H:%M:%S"
}

mkdir -p "${rundir}/conf" "${rundir}/cache" "${rundir}/gamedata"

: > "${log_file}"

if [ ! -x "${program}" ]; then
  chmod a+x "${program}" 2>/dev/null || true
fi

if [ ! -f "${program}" ]; then
  echo "[$(timestamp)] ERROR: missing: ${program}" >> "${log_file}"
  printf "\033c" > /dev/tty1 2>/dev/null || true
  exit 1
fi

if [ ! -f "${config}" ]; then
  echo "[$(timestamp)] ERROR: missing config: ${config}" >> "${log_file}"
  printf "\033c" > /dev/tty1 2>/dev/null || true
  exit 1
fi

if [ ! -f "${rundir}/gamedata/lib/arm64-v8a/libunity.so" ]; then
  echo "[$(timestamp)] ERROR: missing libunity.so under gamedata/ (upload APK assets+lib first)" >> "${log_file}"
  printf "\033c" > /dev/tty1 2>/dev/null || true
  exit 1
fi

if [ ! -f "${rundir}/gamedata/lib/arm64-v8a/libgame.so" ]; then
  echo "[$(timestamp)] ERROR: missing libgame.so under gamedata/ (Unity 6 native activity lib)" >> "${log_file}"
  printf "\033c" > /dev/tty1 2>/dev/null || true
  exit 1
fi

if [ -f "${rundir}/gamecontrollerdb.txt" ]; then
  export SDL_GAMECONTROLLERCONFIG_FILE="${rundir}/gamecontrollerdb.txt"
fi

if pgrep -x pulseaudio >/dev/null 2>&1 || pgrep -x pipewire-pulse >/dev/null 2>&1; then
  export SDL_AUDIODRIVER=pulse
  echo "[$(timestamp)] SDL_AUDIODRIVER=pulse (system volume)" >> "${log_file}"
else
  export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-alsa}"
  if [ -z "${AUDIODEV:-}" ] && grep -qi audiocodec /proc/asound/cards 2>/dev/null; then
    export AUDIODEV=plughw:audiocodec
  fi
  echo "[$(timestamp)] SDL_AUDIODRIVER=${SDL_AUDIODRIVER} AUDIODEV=${AUDIODEV:-default}" >> "${log_file}"
fi

echo "[$(timestamp)] starting unityloader" >> "${log_file}"
echo "[$(timestamp)] program=${program}" >> "${log_file}"
echo "[$(timestamp)] config=${config}" >> "${log_file}"

"${program}" "${config}" >> "${log_file}" 2>&1
ec=$?
echo "[$(timestamp)] unityloader exited (${ec})" >> "${log_file}"

printf "\033c" > /dev/tty1 2>/dev/null || true
exit "${ec}"
