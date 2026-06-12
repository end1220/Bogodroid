#!/usr/bin/env bash
# One-shot StS2 build: take the original game pck and produce a Mali-ready
# pck via `gdre_tools --pck-patch`. The patch layers ASTC textures and
# game-specific overlays ON TOP of the unmodified original, which
# preserves anything we can't recreate on Mac (spine .spskel binaries,
# FMOD .bank archives, compiled .gdc, ...).
#
# Layout
#   tools/godot_pck/
#   ├── work/sts2/
#   │   ├── recovered/   intermediate (extract output)
#   │   ├── overlay/     intermediate (files gdre will patch in)
#   │   └── output.pck   final artifact
#   └── patches/sts2/    <— this directory
#       ├── build.sh     (you are here)
#       ├── apply.sh     overlay copy + project.godot surgery
#       └── overlay/     static patches that mirror res:// paths
#
# Usage:
#   patches/sts2/build.sh <input.pck>

set -euo pipefail

INPUT_PCK="${1:?usage: build.sh <input.pck>}"
[ -f "$INPUT_PCK" ] || { echo "input pck not found: $INPUT_PCK" >&2; exit 1; }
INPUT_PCK="$(cd "$(dirname "$INPUT_PCK")" && pwd)/$(basename "$INPUT_PCK")"

PATCH_DIR="$(cd "$(dirname "$0")" && pwd)"
TOOLS_DIR="$(cd "$PATCH_DIR/../.." && pwd)"
GAME_NAME="$(basename "$PATCH_DIR")"

WORK="$TOOLS_DIR/work/$GAME_NAME"
RECOVERED="$WORK/recovered"
OVERLAY="$WORK/overlay"
OUTPUT_PCK="$WORK/output.pck"
mkdir -p "$WORK"

echo "INPUT      = $INPUT_PCK"
echo "RECOVERED  = $RECOVERED"
echo "OVERLAY    = $OVERLAY"
echo "OUTPUT     = $OUTPUT_PCK"
echo

cd "$TOOLS_DIR"

echo "===== 1/8  extract pck → recovered project ====="
./extract.sh "$INPUT_PCK" "$RECOVERED"

echo
echo "===== 2/8  flip project.godot to mobile texture preset ====="
python3 patch_project.py "$RECOVERED" --target mobile

echo
# Logo / studio splash assets stay at their full quality settings —
# they're already authored small, ASTC compression on solid-color logo
# edges produces visible artefacts, and panel res capping wastes nothing
# we can spare elsewhere.
#   *logo_megacrit*       — MegaCrit studio splash (animations/ui/logo/)
#   *main_menu_logo*      — StS2 main menu title (animations/backgrounds/mainmenu/logo/)
#   *animations/ui/logo/* — catch-all for the studio splash dir
LOGO_GLOBS=(--exclude-glob '*logo_megacrit*'
            --exclude-glob '*main_menu_logo*'
            --exclude-glob '*animations/ui/logo/*'
            --exclude-glob '*animations/backgrounds/mainmenu/logo/*')

echo "===== 3a/8  force every texture to compress/mode=2 (VRAM compressed) ====="
python3 force_vram_compress.py "$RECOVERED" "${LOGO_GLOBS[@]}"

if [ -z "${BASELINE:-}" ]; then
    echo
    echo "===== 3b/8  cap process/size_limit per-pattern (panel-fit + small icons) ====="
    # Per-category caps, ordered by specificity (first match wins inside the tool).
    # Rationale: panel is 1280×720, so anything that must display full-screen needs
    # 1280 (loses crispness if smaller). Cards display at ~400×560 max → 540 source.
    # Icons (relic/power/potion/intent/status) are ~40-120 px on screen → 256 is
    # plenty. Defaulting to 360 catches everything else (loose sprites, vfx).
    #
    # Caveat: this shrinks source pixels only — AtlasTexture .tres regions
    # still reference the ORIGINAL coordinate space, so sub-sprites in
    # affected atlases (card_atlas, ui_atlas, etc.) appear misaligned.
    # Set BASELINE=1 to skip this step and measure pure mode=2-only impact.
    python3 limit_texture_size.py "$RECOVERED" \
        --limit-glob '*backgrounds*:720' \
        --limit-glob '*ui_atlas*:720' \
        --limit-glob '*card_atlas*:360' \
        --limit-glob '*character_select*:540' \
        --limit-glob '*spine*:540' \
        --limit-glob '*relic*:192' \
        --limit-glob '*power*:192' \
        --limit-glob '*potion*:192' \
        --limit-glob '*intent*:192' \
        --limit-glob '*era_atlas*:192' \
        --limit-glob '*epoch_atlas*:192' \
        --limit 256 \
        "${LOGO_GLOBS[@]}"

    echo
    echo "===== 3c/8  mipmap chain off (saves ~33% VRAM per texture) ====="
    # StS2 is 2D — sprites/atlas are drawn at fixed pixel size and never
    # minified, so the lower mip levels just sit in VRAM unused. On a 1 GB
    # Mali handheld with ~2700 textures resident that's ~35-40 MB pure waste.
    # Logos kept on (they actually do animate-scale during the splash).
    # An HD repack targeting beefier devices can flip --state on.
    python3 set_mipmaps.py "$RECOVERED" --state off "${LOGO_GLOBS[@]}"
else
    echo
    echo "===== 3b+3c/8  BASELINE — keep original texture sizes (mode=2 only) ====="
    echo "(skipping limit_texture_size + set_mipmaps; ASTC conversion only)"
fi

echo
echo "===== 4/8  reimport all textures (~25 min) ====="
./reimport.sh "$RECOVERED"

echo
echo "===== 5/8  strip non-ASTC ctex variants ====="
python3 strip_non_astc.py "$RECOVERED"

echo
echo "===== 6/8  apply StS2-specific overlay into recovered/ ====="
"$PATCH_DIR/apply.sh" "$RECOVERED"

echo
echo "===== 7/8  build merged overlay tree (files for gdre --pck-patch) ====="
rm -rf "$OVERLAY"
mkdir -p "$OVERLAY/.godot/imported"
# ASTC ctex from reimport
cp -r "$RECOVERED/.godot/imported"/. "$OVERLAY/.godot/imported/" 2>/dev/null || true
# Every .import outside .godot/ — the references inside reference our hashes.
find "$RECOVERED" -name "*.import" \
    -not -path "*/.godot/*" -not -path "*/.autoconverted/*" \
    | while read -r f; do
        rel="${f#$RECOVERED/}"
        mkdir -p "$OVERLAY/$(dirname "$rel")"
        cp "$f" "$OVERLAY/$rel"
    done
# project.godot + extension_list.cfg (both edited by apply.sh)
cp "$RECOVERED/project.godot" "$OVERLAY/project.godot"
mkdir -p "$OVERLAY/.godot"
cp "$RECOVERED/.godot/extension_list.cfg" "$OVERLAY/.godot/extension_list.cfg" 2>/dev/null || true
# Static patches from patches/<game>/overlay (gdextension swaps, autoload stubs, ...)
rsync -a "$PATCH_DIR/overlay/" "$OVERLAY/"

echo
echo "===== 8/8  gdre --pck-patch → $OUTPUT_PCK ====="
# Source pck is left untouched; spine .spskel / .spatlas / FMOD .bank /
# compiled .gdc / etc. are inherited from the original.
python3 apply_overlay.py \
    --base "$INPUT_PCK" \
    --output "$OUTPUT_PCK" \
    --overlay "$OVERLAY"

echo
echo "===== done ====="
ls -lh "$OUTPUT_PCK"
echo
echo "Next: scp this to the device."
echo "  scp $OUTPUT_PCK root@<device>:/path/to/ports/sts2/SlayTheSpire2.astc.pck"
