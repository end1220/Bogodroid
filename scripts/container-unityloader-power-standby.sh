#!/usr/bin/env bash
# Simulate the handheld power key and kernel resume without suspending the host.
set -eu
cd /workspace/Bogodroid
state_path=/tmp/bd-power-state-test
output=/game/BrickGamePro/seq-power-standby
rm -f "$state_path"
: > "$state_path"
env BD_POWER_REPLAY=15000 BD_POWER_REPLAY_HOLD=120 \
  BD_POWER_STATE_PATH="$state_path" BD_POWER_TEST_HOLD_MS=1500 \
  GAME_DIR=/game/BrickGamePro W=640 H=480 SECS=24 CAP_MS=250 \
  DUMP_AT=400 SEQ_DIR="$output" \
  bash scripts/container-run-game-seq.sh
cp /game/BrickGamePro/log-seq.txt "$output/log.txt"
python3 - "$output/log.txt" "$state_path" <<'PY'
from pathlib import Path
import re, sys
log = Path(sys.argv[1]).read_text(errors="replace")
state = Path(sys.argv[2]).read_bytes()
paused = re.findall(r"Unity nativeRender paused at frame=(\d+)", log)
resumed = re.findall(r"Unity nativeRender resumed at frame=(\d+) after (\d+) ms", log)
assert paused and resumed, "standby pause/resume logs were not produced"
assert paused[-1] == resumed[-1][0], f"Unity rendered across standby: {paused[-1]} -> {resumed[-1][0]}"
assert state == b"mem\n", f"unexpected mocked sysfs command: {state!r}"
assert "test standby active" in log and "test wake" in log
assert int(resumed[-1][1]) >= 1400, f"simulated standby too short: {resumed[-1][1]} ms"
print(f"PASS: nativeRender frame {paused[-1]} remained unchanged across {resumed[-1][1]} ms standby")
PY
