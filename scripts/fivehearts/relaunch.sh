#!/bin/sh
# Hand the launcher line to the Ports frontend, then wait until the loader is
# really up. The frontend (dmenu.bin) owns the framebuffer, so a plain exec of
# the game races it and loses; and because it is restarted by the system
# watchdog, a poke can arrive before it is ready to serve /tmp/.next. Polling
# (and re-poking when the frontend is not there) keeps a long debug session from
# silently running against a game that never started.
#
#   sh relaunch.sh 'BD_VIDEO_TRACE_UI=2'
set -u

env_pairs="${1:-}"
cmd='exec /mnt/mmc/Roms/PORTS/FiveHearts.sh'
if [ -n "$env_pairs" ]; then
  cmd="exec env $env_pairs /mnt/mmc/Roms/PORTS/FiveHearts.sh"
fi

poke() {
  printf '%s\n' "$cmd" > /tmp/.next
  killall -s SIGUSR1 dmenu.bin 2>/dev/null || true
}

echo "handoff: $cmd"
poke

i=0
while [ "$i" -lt 12 ]; do
  sleep 5
  i=$((i + 1))
  loader=$(ps aux | grep -c '[u]nityloader')
  frontend=$(ps aux | grep -c '[d]menu.bin')
  echo "t=$((i * 5))s loader=$loader dmenu=$frontend"
  if [ "$loader" -gt 0 ]; then
    exit 0
  fi
  if [ "$frontend" -eq 0 ]; then
    sleep 5
    poke
    echo "frontend was down, re-poked"
  fi
done

echo "loader never started"
tail -3 /mnt/mmc/Roms/PORTS/FiveHearts/log.txt 2>/dev/null
exit 1
