#!/usr/bin/env bash
# Natural Game Over, physical-button visibility, and real Yes/No exit flow.
set -eu
requested_case="${1:-}"
run_case() {
    local name="$1" hold="$2" seconds="$3" replay="$4"
    if [ -n "$requested_case" ] && [ "$requested_case" != "$name" ]; then
        return
    fi
    local directory="/game/BrickGamePro/seq-ui-$name"
    local probe=1
    [ "$name" != exit ] || probe=0
    echo "CASE $name"
    env BRICKGAME_PROBE_START="$probe" BRICKGAME_PROBE_GAME=Tetris \
        BRICKGAME_DOWN_TRACE=1 BRICKGAME_UI_TRACE=1 \
        BD_PAD_REPLAY="$replay" BD_PAD_REPLAY_HOLD="$hold" \
        GAME_DIR=/game/BrickGamePro W=640 H=480 SECS="$seconds" \
        DUMP_AT=180 CAP_MS=200 SEQ_DIR="$directory" \
        bash /workspace/Bogodroid/scripts/container-run-game-seq.sh \
        > "/tmp/brickgame-ui-$name.txt" 2>&1
    cp "/tmp/brickgame-ui-$name.txt" "$directory/runner.txt"
    cat "$directory/runner.txt"
    cp /game/BrickGamePro/log-seq.txt "$directory/log.txt"
    echo "DONE $name"
}
run_case finish 2000 56 '15000:dpdown,17500:dpdown,20000:dpdown,22500:dpdown,25000:dpdown,27500:dpdown,30000:dpdown,32500:dpdown,35000:dpdown,37500:dpdown,40000:dpdown,42500:dpdown,46000:a,49000:start,52000:dpdown'
run_case buttons 600 41 '15000:start,18000:start,21000:x,24000:back,27000:guide,31000:start,34000:a,37000:dpright'
run_case exit 600 42 '15000:start,19000:back,22000:guide,26000:b,30000:guide,34000:a'
