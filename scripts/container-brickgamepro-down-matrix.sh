#!/usr/bin/env bash
# Container integration replay; writes /game/BrickGamePro/seq-down-*/log.txt.
# GUI flow matters: direct PlayTetris leaves the menu state stale, making a
# subsequent Start restart instead of pause.
set -eu
requested_case="${1:-}"
run_case() {
    local name="$1" game="$2" fix="$3" hold="$4" seconds="$5" replay="$6"
    if [ -n "$requested_case" ] && [ "$requested_case" != "$name" ]; then
        return
    fi
    local directory="/game/BrickGamePro/seq-down-$name"
    echo "CASE $name"
    env BRICKGAME_PROBE_START=1 BRICKGAME_PROBE_GAME="$game" \
        BRICKGAME_DOWN_FIX="$fix" BRICKGAME_DOWN_TRACE=1 \
        BD_PAD_REPLAY="$replay" BD_PAD_REPLAY_HOLD="$hold" \
        GAME_DIR=/game/BrickGamePro W=640 H=480 SECS="$seconds" \
        DUMP_AT=180 CAP_MS=3000 SEQ_DIR="$directory" \
        bash /workspace/Bogodroid/scripts/container-run-game-seq.sh
    cp /game/BrickGamePro/log-seq.txt "$directory/log.txt"
    echo "DONE $name"
}
run_case baseline Tetris 0 1000 19 '15000:dpdown'
run_case fixed Tetris 1 1000 19 '15000:dpdown'
# Game Over follows its animation coroutine, so keep each hold long enough
# to exercise release *after* FinishGame rather than just after collision.
run_case lifecycle Tetris 1 4000 72 '15000:dpdown,19500:dpdown,24000:dpdown,28500:dpdown,33000:dpdown,37500:dpdown,42000:dpdown,46500:dpdown,51000:dpdown,55500:dpdown,60000:dpdown,64500:dpdown'
run_case spawn-gate Tetris 1 10000 40 '15000:dpdown,27000:dpdown'
run_case five-final TetrisFive 1 6000 32 '15000:dpdown,23000:dpdown'
run_case taps-final Tetris 1 1 20 '15000:dpdown,15150:dpdown,15300:dpdown,15450:dpdown,15600:dpdown,15750:dpdown,15900:dpdown,16050:dpdown,16200:dpdown,16350:dpdown,16500:dpdown,16650:dpdown'
run_case tank-final Tank 1 1000 20 '15000:dpdown'
run_case gui-pause Tetris 1 1000 29 '15000:dpdown,15300:dpright,15500:a,18500:start,20000:dpdown,22500:start,24500:dpdown'
