#!/usr/bin/env bash
# Container-only SDL -> title plugin -> original IL2CPP combo regression.
# LOADER may select a diagnostic loader; deployed binaries stay LOG=OFF.
set -eu
requested="${1:-}"
run_case() {
    local name="$1" game="$2" hold="$3" seconds="$4" replay="$5"
    [ -z "$requested" ] || [ "$requested" = "$name" ] || return 0
    local directory="/game/BrickGamePro/seq-rotate-combo-$name"
    env BRICKGAME_PROBE_START=1 BRICKGAME_PROBE_GAME="$game" \
        BRICKGAME_DOWN_TRACE=1 BD_PAD_REPLAY_HOLD="$hold" \
        BD_PAD_REPLAY="$replay" GAME_DIR=/game/BrickGamePro \
        W=640 H=480 SECS="$seconds" DUMP_AT=180 CAP_MS=200 \
        SEQ_DIR="$directory" \
        bash /workspace/Bogodroid/scripts/container-run-game-seq.sh \
        > "/tmp/brickgame-rotate-$name.txt" 2>&1
    cp /game/BrickGamePro/log-seq.txt "$directory/log.txt"
    cp "/tmp/brickgame-rotate-$name.txt" "$directory/runner.txt"
    echo "DONE $name"
    grep 'unityloader exit=' "$directory/runner.txt"
}
combo='11000:dpdown,11150:a,15000:dpleft,15300:a,19000:dpright,19300:a,23000:a'
run_case tetris Tetris 1000 26 "$combo"
run_case five TetrisFive 1000 26 "$combo"
# Pause press is swallowed; after resume check same-frame and reversed order.
run_case pause Tetris 600 30 '15000:start,16500:a,18000:start,20000:a,22000:dpleft,22000:a,25000:a,25000:dpright,28000:y'
run_case quick Tetris 1 22 '15000:a,15300:a,15600:a,15900:a,16200:a,16500:y'
