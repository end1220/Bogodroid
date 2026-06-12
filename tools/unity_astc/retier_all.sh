#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2025-2026 jenny92-tech <jennyliu90223@gmail.com>
#
# Batch-process every Unity bundle under a directory through astc_retier.py.
# Re-tiers big RGBA32/ASTC textures to a coarser ASTC block, compacts dead
# stream bytes, and repacks each bundle with the ORIGINAL UnityFS flags
# (compression + padding bits) so the player sees the same wrapper format it
# was shipped with.
#
# Usage:
#     retier_all.sh <dir> [--audio] [extra astc_retier.py args]
#     retier_all.sh <dir> --dry-run                  # plan only, no writes
#     retier_all.sh <dir> --block 6x6                # gentler quality for pixel art
#     retier_all.sh <dir> --audio                    # + audio_stream_patch.py step
#
# --audio chains audio_stream_patch.py (defaults) after the texture step.
# It only flips long AudioClips to Streaming — no re-encode, so it adds
# roughly one bundle load+repack of time. Clips whose data lives inside the
# bundle (archive:) are skipped loudly; --min-secs/--skip tuning needs a
# manual audio_stream_patch.py run.
#
# Defaults: --include-raw --compact --packer original
#   - block size is NOT forced; astc_retier.py's own default (8x8) wins unless
#     you pass --block. Override with --block 6x6 for pixel art / sharp edges.
# Output:   astc_retier.py's default — ./output/<original filename>; originals
#           untouched, deploy is one cp/rsync back, no renames.
# Summary:  aggregates per-bundle "est. texture data" into a Tex RAM total —
#           works in --dry-run too, so you can budget against device RAM
#           before encoding anything.
set -euo pipefail

if [[ $# -lt 1 ]]; then
    cat >&2 <<EOF
usage: $0 <dir> [--audio] [extra astc_retier.py args]

Processes every *.bundle and data.unity3d under <dir> (recursive) with:
    --include-raw --compact --packer original
(block size defaults to astc_retier.py's own default — 8x8 — unless overridden.)
--audio additionally runs audio_stream_patch.py on each result (no re-encode).
Outputs land in ./output/<original filename> (astc_retier.py's default).
Pass any extra astc_retier.py flags after <dir> to override or add.

Examples:
    $0 ~/Downloads/myapk/assets/aa/Android
    $0 ~/Downloads/myapk/assets/aa/Android --dry-run --audio
    $0 ~/Downloads/myapk/assets/aa/Android --block 6x6 --keep-cap 512
EOF
    exit 1
fi

DIR="$1"; shift
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RETIER="$SCRIPT_DIR/astc_retier.py"
AUDIO="$SCRIPT_DIR/audio_stream_patch.py"

if [[ ! -d "$DIR" ]]; then
    echo "error: not a directory: $DIR" >&2
    exit 1
fi
if [[ ! -f "$RETIER" ]]; then
    echo "error: astc_retier.py not found at $RETIER" >&2
    exit 1
fi
DIR="$(cd "$DIR" && pwd)"

# --audio is ours (chain audio_stream_patch.py after the texture step);
# everything else passes through to astc_retier.py. Note --dry-run so the
# audio step can mirror it.
audio=0
dry=0
extra=()
for a in "$@"; do
    case "$a" in
        --audio)   audio=1 ;;
        --dry-run) dry=1; extra+=("$a") ;;
        *)         extra+=("$a") ;;
    esac
done

# astc_retier.py writes to ./output/<name> by default; remember that so the
# size accounting below can find the files.
OUT_DIR="$PWD/output"

# Collect targets: every *.bundle plus data.unity3d. Prune the output dir in
# case it sits inside <dir>, and skip legacy .retiered files from old runs.
# NUL-delimited so spaces/CJK names work. while-read loop instead of
# `mapfile -d ''` — macOS ships bash 3.2 which lacks mapfile entirely.
targets=()
while IFS= read -r -d '' f; do
    targets+=("$f")
done < <(find "$DIR" \
    -path "$OUT_DIR" -prune -o \
    \( -name '*.bundle' -o -name 'data.unity3d' \) \
    ! -name '*.retiered' \
    -type f -print0)

if [[ ${#targets[@]} -eq 0 ]]; then
    echo "no *.bundle or data.unity3d under $DIR"
    exit 0
fi

echo "Found ${#targets[@]} bundle(s) under $DIR"
echo

total_in=0
total_out=0
n_ok=0
n_fail=0
n_skip=0
est_before_total=0
est_after_total=0
n_est=0
audio_freed_total=0
n_audio=0
est_re='est\. texture data ([0-9]+) -> ([0-9]+) MB'
audio_re='Est\. RAM freed when those clips are loaded: ([0-9.]+) MB'
tmp_log=$(mktemp)
trap 'rm -f "$tmp_log"' EXIT

for f in "${targets[@]}"; do
    out="$OUT_DIR/$(basename "$f")"
    in_sz=$(stat -f%z "$f" 2>/dev/null || stat -c%s "$f")
    echo "=================================================================="
    printf "[%d/%d] %s\n" $((n_ok + n_fail + n_skip + 1)) "${#targets[@]}" "$f"
    echo "       ($(printf '%.1f' "$(echo "$in_sz / 1048576" | bc -l)") MB in)"
    echo "=================================================================="

    if python3 "$RETIER" "$f" \
            --include-raw \
            --compact \
            --packer original \
            ${extra[@]+"${extra[@]}"} | tee "$tmp_log"; then
        # Aggregate the per-bundle texture-RAM estimate (the number that
        # matters on UMA devices; disk totals below only cover real runs).
        est_line=$(grep -E 'est\. texture data' "$tmp_log" | tail -1 || true)
        if [[ -n "$est_line" && "$est_line" =~ $est_re ]]; then
            est_before_total=$((est_before_total + BASH_REMATCH[1]))
            est_after_total=$((est_after_total + BASH_REMATCH[2]))
            n_est=$((n_est + 1))
        fi
        if [[ $audio -eq 1 ]]; then
            # Chain the audio step on the texture output when one exists,
            # otherwise on the original. Writes via a temp file so a
            # "Nothing to do" pass leaves the texture output untouched.
            a_in="$f"
            if [[ -f "$out" ]]; then a_in="$out"; fi
            if [[ $dry -eq 1 ]]; then
                if ! python3 "$AUDIO" "$a_in" --dry-run | tee "$tmp_log"; then
                    echo "  !! audio step FAILED on $f" >&2
                fi
            else
                if python3 "$AUDIO" "$a_in" -o "$out.tmp" --packer original \
                        | tee "$tmp_log"; then
                    if [[ -f "$out.tmp" ]]; then mv "$out.tmp" "$out"; fi
                else
                    echo "  !! audio step FAILED on $f (texture output kept)" >&2
                    rm -f "$out.tmp"
                fi
            fi
            a_line=$(grep -E 'Est\. RAM freed' "$tmp_log" | tail -1 || true)
            if [[ -n "$a_line" && "$a_line" =~ $audio_re ]]; then
                audio_freed_total=$(echo "$audio_freed_total + ${BASH_REMATCH[1]}" | bc)
                n_audio=$((n_audio + 1))
            fi
        fi
        if [[ -f "$out" ]]; then
            out_sz=$(stat -f%z "$out" 2>/dev/null || stat -c%s "$out")
            total_in=$((total_in + in_sz))
            total_out=$((total_out + out_sz))
            n_ok=$((n_ok + 1))
        else
            # dry-run or "nothing to do" — no output file
            n_skip=$((n_skip + 1))
        fi
    else
        echo "  !! FAILED on $f" >&2
        n_fail=$((n_fail + 1))
    fi
    echo
done

echo "=================================================================="
echo "Batch summary"
echo "=================================================================="
echo "  Re-tiered:  $n_ok"
echo "  Skipped:    $n_skip   (dry-run / nothing to do)"
echo "  Failed:     $n_fail"
if [[ $n_est -gt 0 ]]; then
    printf "  Tex RAM:    %d MB -> %d MB (%+d MB est., %d bundle(s))\n" \
        "$est_before_total" "$est_after_total" \
        "$((est_after_total - est_before_total))" "$n_est"
fi
if [[ $n_audio -gt 0 ]]; then
    printf "  Audio RAM:  -%s MB est. (clips flipped to Streaming, %d bundle(s))\n" \
        "$audio_freed_total" "$n_audio"
fi
if [[ $n_ok -gt 0 ]]; then
    in_mb=$(echo "$total_in / 1048576" | bc -l)
    out_mb=$(echo "$total_out / 1048576" | bc -l)
    delta=$(echo "$out_mb - $in_mb" | bc -l)
    printf "  Disk:       %.1f MB -> %.1f MB (%+.1f MB)\n" "$in_mb" "$out_mb" "$delta"
fi
echo
echo "Outputs are in $OUT_DIR/ with original filenames."
echo "After QA on device, copy them back over the originals in one shot:"
echo "    cp -f \"$OUT_DIR\"/* \"$DIR\"/"
echo "(bundles in nested subdirs: copy each back to its own subdir)"

exit $((n_fail > 0))
