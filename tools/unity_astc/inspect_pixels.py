#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2025-2026 jenny92-tech <jennyliu90223@gmail.com>
"""
Sample big textures out of a Unity bundle to PNG, and tell us the actual
pixel-cell size so we can pick the right ASTC block for re-tiering.

Why
---
Pixel-art assets are often DRAWN at a low native resolution and then either:
  - Shipped at that native resolution (cell size = 1 px, every pixel is hand-
    placed; ANY block compression smears 1-px edges)
  - Shipped already upscaled by an integer factor (cell size = N px, every
    NxN region is a solid color; ASTC blocks ALIGNED with that grid survive
    cleanly, misaligned ones hit block-boundary artifacts)
  - Shipped at a weird non-square cell (11x11, 13x13, etc) — possible when
    the artist worked at one resolution and the bundle was built at another

You can't tell which case you're in without LOOKING at the pixels. This tool
picks the N biggest candidates, decodes them, writes them out as PNG, and runs
a small block-variance test to find the fundamental cell size.

Cell-size test
--------------
For each candidate cell size N (2..max), reshape the image into NxN blocks
and compute the mean within-block variance. Near-zero variance => every NxN
region is a solid color => the texture really is upscaled NxN pixel art.
Largest N that still passes the threshold = the true cell size (since smaller
divisors of N also pass, e.g. a 6x6 grid is also 2x2 and 3x3 constant).

Usage
-----
    # Pick the 5 biggest non-font RGBA32 textures, save PNGs, report grids
    python3 tools/unity_astc/inspect_pixels.py data.unity3d -o /tmp/peek

    # Look at 12 of them, only big ones (>=512 long side)
    python3 tools/unity_astc/inspect_pixels.py data.unity3d -o /tmp/peek -n 12 --min 512

    # Only sample these texture names (substring match, repeatable)
    python3 tools/unity_astc/inspect_pixels.py data.unity3d -o /tmp/peek \\
        --name 时间轴 --name 多闻天王

Requirements
------------
    pip install UnityPy Pillow numpy
"""

import argparse
import sys
from pathlib import Path

try:
    import UnityPy
    from UnityPy.enums import TextureFormat as TF
    from PIL import Image
    import numpy as np
except ImportError as e:
    sys.exit(f"missing dependency: {e}\n"
             "install with:  pip install UnityPy Pillow numpy")


def detect_cell_size(img, max_test=16, threshold=1.5):
    """Find the largest N in [1, max_test] such that the image looks like an
    NxN-cell upscaled pixel grid (every NxN block internally constant).
    Returns (cell_size, score). Score is mean within-block stdev in 0..255
    units across all channels; lower = more grid-like. cell_size == 1 means
    no upscale detected.

    Threshold is in 0..255 units; ~1.5 catches near-constant blocks while
    tolerating PNG-style tiny variations from premultiplied alpha rounding.
    """
    a = np.array(img.convert("RGBA"), dtype=np.float32)
    h, w, c = a.shape
    best_n = 1
    best_score = float("inf")
    if best_score == best_score:  # always true; init scoreboard
        # also score N=1 (trivially 0)
        best_score = 0.0
    for n in range(2, max_test + 1):
        nh, nw = h // n, w // n
        if nh < 4 or nw < 4:
            # not enough blocks to make a reliable call
            continue
        crop = a[:nh * n, :nw * n]
        blocks = crop.reshape(nh, n, nw, n, c)
        # std across the n*n pixels inside each block, averaged over blocks
        # and channels — measures intra-block color variation
        score = blocks.std(axis=(1, 3)).mean()
        if score < threshold:
            # this n is "block-constant enough"; prefer the LARGEST such n
            best_n = n
            best_score = score
    return best_n, best_score


def count_unique_colors(img, cap=10000):
    """Heuristic: rough count of distinct RGBA values, capped. Cheap proxy for
    'is this anti-aliased art vs flat-color pixel art'. Pixel art with a
    palette tends to be in the dozens; AA'd / painted art in the thousands+.
    """
    a = np.array(img.convert("RGBA"))
    # pack RGBA into a single int per pixel for fast unique
    packed = a.view(dtype=np.uint32).reshape(-1)
    u = np.unique(packed[:200_000])  # sample if huge
    return min(len(u), cap), len(u) >= cap


# LDR ASTC square block sizes (UnityPy / astc_encoder supported); 4x4 is the
# floor — there is no "2x2" or "3x3" ASTC format.
ASTC_SIZES = (4, 5, 6, 8, 10, 12)


def suggested_astc_block(cell_size, unique_colors, long_side):
    """Map detected pixel-cell properties to an ASTC block recommendation.
    Returns (block_str, reason).

    Picking principle: choose the SMALLEST ASTC block whose size is an integer
    multiple of the detected cell. That keeps every ASTC block aligned with
    cell boundaries (so 1-px logical edges don't bleed) AND keeps each block
    representing a small number of cells (so ASTC's 2-endpoint model has an
    easy job). A larger aligned size is mentioned as a "more compression"
    fallback when present.
    """
    if cell_size == 1:
        if unique_colors < 256:
            return ("KEEP RGBA32",
                    f"1-px pixel art ({unique_colors} colors); ANY ASTC "
                    "block will smear hand-placed 1-px edges")
        return ("6x6",
                f"1-px content but rich colors ({unique_colors}+, likely "
                "anti-aliased or painted); 6x6 is a balanced tradeoff")

    aligned = [s for s in ASTC_SIZES if s % cell_size == 0]
    if aligned:
        best = aligned[0]   # smallest aligned size; safest quality
        k = best // cell_size
        # also mention a higher-compression option if there's one <= 8
        higher = [s for s in aligned if s > best and s <= 8]
        extra = ""
        if higher:
            h = higher[-1]
            hk = h // cell_size
            extra = f"  (or {h}x{h} = {hk} cells/block for more compression)"
        return (f"{best}x{best}",
                f"upscaled {cell_size}x{cell_size} grid; ASTC {best}x{best} "
                f"= {k} cell{'' if k == 1 else 's'} per block, edges align"
                + extra)

    # 7, 9, 11, 13, 14, 15 — cell size doesn't divide any ASTC block.
    # Nearest ASTC will have inevitable block-edge misalignment.
    nearest = min(ASTC_SIZES, key=lambda s: abs(s - cell_size))
    return (f"{nearest}x{nearest}",
            f"{cell_size}x{cell_size} cell does not divide any ASTC block; "
            f"{nearest}x{nearest} is the nearest — expect SOME block-edge "
            "artifacts (or consider KEEP RGBA32 if cells are critical)")


RAW_FORMATS = (TF.RGBA32, TF.RGB24, TF.BGRA32, TF.ARGB32)


def collect_font_keys(env):
    """Same logic as astc_retier.py — exclude legacy Font m_Texture refs."""
    keys = set()
    for obj in env.objects:
        if obj.type.name != "Font":
            continue
        try:
            d = obj.read()
        except Exception:
            continue
        tex = getattr(d, "m_Texture", None)
        pid = getattr(tex, "path_id", 0) or getattr(tex, "m_PathID", 0)
        if not pid:
            continue
        fid = getattr(tex, "file_id", 0) or getattr(tex, "m_FileID", 0)
        af = obj.assets_file
        if fid == 0:
            keys.add((af.name, pid))
        else:
            try:
                keys.add((af.externals[fid - 1].path, pid))
            except Exception:
                pass
    return keys


def main():
    ap = argparse.ArgumentParser(
        description="Extract sample textures and detect their pixel-cell size",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Usage\n-----\n", 1)[1].split("Requirements")[0])
    ap.add_argument("bundle", help="path to a Unity bundle (.unity3d / .bundle)")
    ap.add_argument("-o", "--out", required=True,
                    help="output directory for PNGs and a report")
    ap.add_argument("-n", "--count", type=int, default=5,
                    help="how many textures to sample (default: 5)")
    ap.add_argument("--min", type=int, default=256, dest="min_side",
                    help="ignore textures with long side < N (default: 256)")
    ap.add_argument("--name", action="append", default=[],
                    help="only include textures whose name contains this "
                         "substring (repeatable; OR across instances)")
    ap.add_argument("--all-formats", action="store_true",
                    help="don't restrict to RGBA32/RGB24; sample any "
                         "decodable Texture2D")
    ap.add_argument("--max-cell", type=int, default=16,
                    help="largest pixel-cell size to test (default: 16)")
    args = ap.parse_args()

    bundle_in = Path(args.bundle).resolve()
    if not bundle_in.is_file():
        sys.exit(f"input not found: {bundle_in}")
    out_dir = Path(args.out).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    print(f"Loading {bundle_in}")
    env = UnityPy.load(str(bundle_in))
    font_keys = collect_font_keys(env)

    # collect candidates: (long_side, name, t, obj)
    cands = []
    for obj in env.objects:
        if obj.type.name != "Texture2D":
            continue
        try:
            t = obj.read()
        except Exception:
            continue
        name = t.m_Name
        w, h = t.m_Width, t.m_Height
        if max(w, h) < args.min_side:
            continue
        if not args.all_formats and t.m_TextureFormat not in RAW_FORMATS:
            continue
        if (obj.assets_file.name, obj.path_id) in font_keys:
            continue
        if name.endswith(" Atlas"):
            continue
        if args.name and not any(s in name for s in args.name):
            continue
        cands.append((max(w, h), name, t))
    if not cands:
        sys.exit("no matching candidates — relax --min / --name / "
                 "or pass --all-formats")

    # biggest first; take N
    cands.sort(key=lambda c: c[0], reverse=True)
    cands = cands[: args.count]

    report_path = out_dir / "report.txt"
    report = []
    print(f"\nSampling {len(cands)} textures into {out_dir}/")
    print("=" * 78)
    for i, (_, name, t) in enumerate(cands, 1):
        w, h = t.m_Width, t.m_Height
        fmt = TF(t.m_TextureFormat).name
        try:
            img = t.image
        except Exception as e:
            print(f"[{i}] {name!r} {w}x{h} {fmt}: decode FAIL: {e}")
            continue
        if img is None:
            print(f"[{i}] {name!r} {w}x{h} {fmt}: empty image")
            continue

        # sanitize filename — slashes/colons/leading dots break some FSes
        safe = "".join(ch if ch.isalnum() or ch in "._-() " else "_"
                       for ch in name).strip() or f"tex_{i}"
        png_path = out_dir / f"{i:02d}_{safe}.png"
        img.save(png_path)

        cell, score = detect_cell_size(img, max_test=args.max_cell)
        n_colors, capped = count_unique_colors(img)
        block, reason = suggested_astc_block(cell, n_colors, max(w, h))

        line = (f"[{i}] {name!r}\n"
                f"    size:      {w}x{h}   format: {fmt}\n"
                f"    cell:      {cell}x{cell} px  (block-std {score:.2f}/255)"
                f"{'  <-- 1 means no upscale grid found' if cell == 1 else ''}\n"
                f"    colors:    {n_colors}{'+' if capped else ''} unique\n"
                f"    -> ASTC:   {block}\n"
                f"    why:       {reason}\n"
                f"    saved:     {png_path.name}")
        print(line)
        report.append(line)
        print("-" * 78)

    report_path.write_text("\n\n".join(report) + "\n", encoding="utf-8")
    print(f"\nReport: {report_path}")
    print("Open the PNGs in any image viewer to eyeball the actual cells.")


if __name__ == "__main__":
    main()
