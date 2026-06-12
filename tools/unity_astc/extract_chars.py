#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2025-2026 jenny92-tech <jennyliu90223@gmail.com>
"""
Sweep every Unity asset container under a directory and collect the unique
CJK codepoints that appear in any string-bearing field.

Why
---
Before prebaking a static font atlas to bypass the runtime [gpu] textureMaxDim
downsample, we need the exact set of CJK characters the game actually uses.
The set's size decides whether a 384x384 / 512x512 / 768x768 atlas is enough.

What it scans
-------------
- data.unity3d                  (main resource container)
- *.bundle                      (Addressables groups)
- *.assets                      (loose SerializedFile)

What it considers a "character"
-------------------------------
Unicode codepoints in the practical CJK set:
  U+3000..U+303F   CJK Symbols & Punctuation (e.g. ， 。 「 」)
  U+3400..U+4DBF   CJK Extension A
  U+4E00..U+9FFF   CJK Unified Ideographs (basic)
  U+F900..U+FAFF   CJK Compatibility Ideographs

Detection strategy
------------------
IL2CPP-stripped MonoBehaviour typetrees don't expose field names, so we
can't enumerate "string fields" by schema. Instead we scan raw object
data for valid UTF-8 sequences in the CJK byte ranges and collect every
codepoint that decodes cleanly. This catches strings regardless of how
Unity wraps them (length-prefixed, null-terminated, embedded in
TextAsset.m_Script, etc.).

Usage
-----
    # Sweep all containers under a directory tree
    python3 tools/unity_astc/extract_chars.py /path/to/port_root

    # Multiple roots / specific files
    python3 tools/unity_astc/extract_chars.py data.unity3d bundles/

    # Write the character set to a file (one char per line) for downstream
    # use (e.g. font baker codepoint list)
    python3 tools/unity_astc/extract_chars.py /path -o chars.txt

Requirements
------------
    pip install UnityPy
"""

import argparse
import os
import re
import sys
from pathlib import Path

try:
    import UnityPy
except ImportError as e:
    sys.exit(f"missing UnityPy: {e}\ninstall with:  pip install UnityPy")


# Valid UTF-8 byte ranges that cover our CJK target.
# 3-byte UTF-8 form:  1110xxxx 10xxxxxx 10xxxxxx, codepoint = (b1&0x0F)<<12 | (b2&0x3F)<<6 | (b3&0x3F)
# - U+3000..U+303F  -> bytes E3 80 80 .. E3 80 BF
# - U+3400..U+4DBF  -> bytes E3 90 80 .. E4 B6 BF
# - U+4E00..U+9FFF  -> bytes E4 B8 80 .. E9 BF BF
# - U+F900..U+FAFF  -> bytes EF A4 80 .. EF AB BF
# We do not include 4-byte forms (CJK Ext B+) — vanishingly rare in game text
# and the font likely won't have them anyway.
UTF8_RUN_RE = re.compile(rb'(?:[\xE3-\xE9\xEF][\x80-\xBF][\x80-\xBF])+')

CJK_RANGES = (
    (0x3000, 0x303F),
    (0x3400, 0x4DBF),
    (0x4E00, 0x9FFF),
    (0xF900, 0xFAFF),
)


def is_cjk(cp: int) -> bool:
    for lo, hi in CJK_RANGES:
        if lo <= cp <= hi:
            return True
    return False


def harvest_cjk_from_bytes(raw: bytes, sink: set):
    """Find every valid 3-byte UTF-8 sequence decoding to a CJK codepoint
    in raw, add to sink."""
    for m in UTF8_RUN_RE.finditer(raw):
        try:
            text = m.group(0).decode("utf-8", "strict")
        except UnicodeDecodeError:
            continue
        for ch in text:
            cp = ord(ch)
            if is_cjk(cp):
                sink.add(ch)


def iter_targets(roots):
    """Yield (kind, path) for every Unity container under roots."""
    suffixes = (".unity3d", ".bundle", ".assets")
    for root in roots:
        rp = Path(root)
        if rp.is_file():
            yield rp.suffix or "?", rp
            continue
        if not rp.is_dir():
            print(f"  warn: not a file or dir: {rp}", file=sys.stderr)
            continue
        for sub in sorted(rp.rglob("*")):
            if sub.is_file() and sub.suffix.lower() in suffixes:
                yield sub.suffix.lower(), sub


def scan_container(path: Path, sink: set) -> int:
    """Scan one container. Return number of objects walked."""
    env = UnityPy.load(str(path))
    n = 0
    for obj in env.objects:
        n += 1
        raw = obj.get_raw_data()
        if not raw:
            continue
        harvest_cjk_from_bytes(raw, sink)
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("roots", nargs="+",
                    help="one or more directories or .unity3d / .bundle / .assets files")
    ap.add_argument("-o", "--output",
                    help="write unique CJK chars to this file (one per line)")
    ap.add_argument("-q", "--quiet", action="store_true",
                    help="suppress per-container line, only print totals")
    args = ap.parse_args()

    all_chars: set = set()
    per_file: list = []  # (label, count_in_this_file)

    targets = list(iter_targets(args.roots))
    if not targets:
        sys.exit("no Unity containers found under given roots")

    print(f"Scanning {len(targets)} container(s)...\n", flush=True)

    for kind, p in targets:
        local: set = set()
        try:
            n_objs = scan_container(p, local)
        except Exception as e:
            print(f"  {p.name:60s} ERROR: {e}")
            continue
        all_chars |= local
        per_file.append((p.name, len(local), n_objs))
        if not args.quiet:
            print(f"  {p.name:60s}  objs={n_objs:6d}  unique CJK={len(local):5d}")

    print()
    print("=" * 78)
    print(f"  TOTAL unique CJK codepoints: {len(all_chars)}")
    print("=" * 78)

    # Breakdown by range
    range_counts = {f"U+{lo:04X}-{hi:04X}": 0 for lo, hi in CJK_RANGES}
    for ch in all_chars:
        cp = ord(ch)
        for lo, hi in CJK_RANGES:
            if lo <= cp <= hi:
                range_counts[f"U+{lo:04X}-{hi:04X}"] += 1
                break
    for label, cnt in range_counts.items():
        print(f"    {label}  : {cnt}")

    # Atlas size estimate (16pt pixel font, ~20x20 per glyph, ~65% packing)
    n = len(all_chars)
    print()
    print("Atlas size estimate (16pt pixel font, ~20x20 px/glyph, 65% packing):")
    for atlas in (256, 384, 512, 640, 768, 1024, 1280, 2048):
        capacity = int((atlas * atlas * 0.65) / (20 * 20))
        fit = "FITS" if capacity >= n else "TOO SMALL"
        mem_mb_rgba8 = atlas * atlas * 4 / (1024 * 1024)
        print(f"  {atlas:5d} x {atlas:<5d}  cap~{capacity:5d} glyphs   "
              f"RGBA8 ram={mem_mb_rgba8:5.2f} MB   {fit}")

    if args.output:
        Path(args.output).write_text("".join(sorted(all_chars)), encoding="utf-8")
        print(f"\nWrote {len(all_chars)} chars to {args.output}")


if __name__ == "__main__":
    main()
