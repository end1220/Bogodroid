#!/usr/bin/env bash
# Re-import every resource in a recovered godot project using the local godot
# 4.5 mono editor in headless mode. This regenerates .godot/imported/*.ctex
# according to the (patched) [rendering] textures/vram_compression/* settings.
#
# Usage: reimport.sh <recovered_dir>

set -euo pipefail

PROJECT_DIR="${1:?usage: reimport.sh <recovered_dir>}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
case "$(uname -s)" in
  Darwin) GODOT="/Users/smallraw/Development/Jenny92Work/godot-sdl2/bin/godot.macos.editor.arm64" ;;
  Linux)  GODOT="$SCRIPT_DIR/bin/godot_mono_linux" ;;
  *)      echo "Unsupported OS"; exit 1 ;;
esac

if [ ! -x "$GODOT" ]; then
  echo "godot editor missing at $GODOT — see bin/README.md"
  exit 1
fi

cd "$PROJECT_DIR"
# Wipe the cached .ctex so godot fully regenerates with new settings.
rm -rf .godot/imported

echo "==> First-pass import (godot 4 needs two passes to settle uids)"
"$GODOT" --headless --import 2>&1 | tail -5
echo "==> Second-pass import"
"$GODOT" --headless --import 2>&1 | tail -5

echo "==> ctex by format:"
find .godot/imported -name "*.ctex" 2>/dev/null \
  | sed -E 's/.*\.([^.]+)\.ctex$/\1/' | sort | uniq -c | sort -rn
