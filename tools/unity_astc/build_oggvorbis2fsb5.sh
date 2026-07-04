#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2025-2026 jenny92-tech <jennyliu90223@gmail.com>
#
# Fetch and build uyjulian/oggvorbis2fsb5 for audio_reencode_fsb.py.
# The third-party source and built binary are local tool artifacts and are
# ignored by git:
#   tools/unity_astc/.cache/oggvorbis2fsb5/
#   tools/unity_astc/bin/oggvorbis2fsb5
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CACHE_DIR="$SCRIPT_DIR/.cache/oggvorbis2fsb5"
BIN_DIR="$SCRIPT_DIR/bin"
OUT="$BIN_DIR/oggvorbis2fsb5"
REPO="${OGGVORBIS2FSB5_REPO:-https://github.com/uyjulian/oggvorbis2fsb5.git}"
REV="${OGGVORBIS2FSB5_REV:-}"

need() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "error: missing required command: $1" >&2
        exit 1
    fi
}

need git
need clang

mkdir -p "$BIN_DIR" "$(dirname "$CACHE_DIR")"

if [[ ! -d "$CACHE_DIR/.git" ]]; then
    rm -rf "$CACHE_DIR"
    git clone "$REPO" "$CACHE_DIR"
fi

if [[ -n "$REV" ]]; then
    git -C "$CACHE_DIR" fetch --depth 1 origin "$REV" >/dev/null 2>&1 || true
    git -C "$CACHE_DIR" checkout "$REV" >/dev/null
fi
git -C "$CACHE_DIR" submodule update --init --recursive

SRC=(
    "$CACHE_DIR/oggvorbis2fsb5.c"
    "$CACHE_DIR/external/ogg/src/bitwise.c"
    "$CACHE_DIR/external/ogg/src/framing.c"
    "$CACHE_DIR/external/vorbis/lib/bitrate.c"
    "$CACHE_DIR/external/vorbis/lib/block.c"
    "$CACHE_DIR/external/vorbis/lib/codebook.c"
    "$CACHE_DIR/external/vorbis/lib/envelope.c"
    "$CACHE_DIR/external/vorbis/lib/floor0.c"
    "$CACHE_DIR/external/vorbis/lib/floor1.c"
    "$CACHE_DIR/external/vorbis/lib/info.c"
    "$CACHE_DIR/external/vorbis/lib/lpc.c"
    "$CACHE_DIR/external/vorbis/lib/lsp.c"
    "$CACHE_DIR/external/vorbis/lib/mapping0.c"
    "$CACHE_DIR/external/vorbis/lib/mdct.c"
    "$CACHE_DIR/external/vorbis/lib/psy.c"
    "$CACHE_DIR/external/vorbis/lib/registry.c"
    "$CACHE_DIR/external/vorbis/lib/res0.c"
    "$CACHE_DIR/external/vorbis/lib/sharedbook.c"
    "$CACHE_DIR/external/vorbis/lib/smallft.c"
    "$CACHE_DIR/external/vorbis/lib/synthesis.c"
    "$CACHE_DIR/external/vorbis/lib/vorbisfile.c"
    "$CACHE_DIR/external/vorbis/lib/window.c"
)

clang -O3 -std=c11 -include stdint.h \
    -I"$CACHE_DIR/external/ogg/include" \
    -I"$CACHE_DIR/external/vorbis/include" \
    -I"$CACHE_DIR/external/vorbis/lib" \
    -o "$OUT" "${SRC[@]}" -lm

echo "built $OUT"
