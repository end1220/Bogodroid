#!/usr/bin/env bash
# Quick check on a recovered project's imported .ctex output to confirm the
# Bogodroid godot-editor patch landed:
#   1. No .etc2.ctex were emitted (all mobile textures should go to ASTC).
#   2. The .astc.ctex files carry format byte 0x25 (Image::FORMAT_ASTC_8x8 = 37),
#      not 0x23 (Image::FORMAT_ASTC_4x4 = 35). The format lives at offset 0x30
#      of godot's GST2 ctex header.
#
# Usage: verify_astc_8x8.sh <recovered_dir>
set -euo pipefail

DIR="${1:?usage: verify_astc_8x8.sh <recovered_dir>}"
IMPORTED="$DIR/.godot/imported"
[ -d "$IMPORTED" ] || { echo "no .godot/imported in $DIR" >&2; exit 1; }

echo "=== ctex variants in $IMPORTED ==="
find "$IMPORTED" -name "*.ctex" \
  | sed -E 's/.*\.([^.]+)\.ctex$/\1/' \
  | sort | uniq -c | sort -rn

echo
astc_total=$(find "$IMPORTED" -name "*.astc.ctex" 2>/dev/null | wc -l | tr -d ' ')
echo "=== sampling 5 .astc.ctex for format byte at 0x30 ==="
echo "expect: 25 00 00 00 (= 37 = ASTC_8x8)"
echo "  bad:  23 00 00 00 (= 35 = ASTC_4x4 — patch did not land)"
echo
find "$IMPORTED" -name "*.astc.ctex" 2>/dev/null | head -5 | while read -r f; do
  fmt=$(xxd -s 0x30 -l 4 -p "$f")
  size=$(wc -c < "$f" | tr -d ' ')
  case "$fmt" in
    25000000) tag="ASTC_8x8 ✓" ;;
    23000000) tag="ASTC_4x4 (patch did NOT land!) ✗" ;;
    *)        tag="unknown format ($fmt) ?" ;;
  esac
  printf "  %s  size=%9d B  fmt=%s  %s\n" "$(basename "$f")" "$size" "$fmt" "$tag"
done

echo
echo "=== summary ==="
all_8x8=$(find "$IMPORTED" -name "*.astc.ctex" -exec sh -c '
  fmt=$(xxd -s 0x30 -l 4 -p "$1")
  [ "$fmt" = "25000000" ] && echo OK || echo BAD
' _ {} \; 2>/dev/null | grep -c OK || true)
echo "  $all_8x8 of $astc_total .astc.ctex carry ASTC_8x8"
etc2_total=$(find "$IMPORTED" -name "*.etc2.ctex" 2>/dev/null | wc -l | tr -d ' ')
echo "  $etc2_total .etc2.ctex still present (expect 0 with the COMPRESS_ASTC override)"
