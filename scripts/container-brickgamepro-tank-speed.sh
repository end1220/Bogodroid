#!/usr/bin/env bash
# Sequential: these replays share Xvfb and log-seq.txt.
set -eu
cd /workspace/Bogodroid
for fix in 0 1; do
  output="/game/BrickGamePro/seq-tank-speed-$fix"
  env BRICKGAME_PROBE_START=1 BRICKGAME_PROBE_GAME=Tank \
    BRICKGAME_TANK_SPEED_FIX="$fix" BRICKGAME_DOWN_TRACE=1 \
    BD_PAD_REPLAY='15000:dpright,18000:dpleft,21000:dpup,24000:dpdown' \
    BD_PAD_REPLAY_HOLD=1600 GAME_DIR=/game/BrickGamePro W=640 H=480 \
    SECS=28 DUMP_AT=400 CAP_MS=1000 SEQ_DIR="$output" \
    bash scripts/container-run-game-seq.sh
  cp /game/BrickGamePro/log-seq.txt "$output/log.txt"
done

# In the long baseline the player can be hit before Up, so also test Up first.
output=/game/BrickGamePro/seq-tank-up-baseline
env BRICKGAME_PROBE_START=1 BRICKGAME_PROBE_GAME=Tank \
  BRICKGAME_TANK_SPEED_FIX=0 BRICKGAME_DOWN_TRACE=1 \
  BD_PAD_REPLAY='15000:dpup' BD_PAD_REPLAY_HOLD=1600 \
  GAME_DIR=/game/BrickGamePro W=640 H=480 SECS=19 DUMP_AT=200 \
  CAP_MS=1000 SEQ_DIR="$output" bash scripts/container-run-game-seq.sh
cp /game/BrickGamePro/log-seq.txt "$output/log.txt"
