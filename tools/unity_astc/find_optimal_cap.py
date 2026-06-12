#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2025-2026 jenny92-tech <jennyliu90223@gmail.com>
"""
For a chosen long-side cap, score how "point-sample-clean" the resulting
downsampled textures will be across an entire Unity bundle. Use it to pick a
cap that aligns with the most textures' pixel grids, instead of guessing.

The math
--------
A texture's effective downsample ratio at cap T is R = orig_long / T.
For PIXEL ART with detected cell size C (every C×C source pixels = one
hand-placed logical pixel), the only point-sample-clean R values are:

    R = C × k   for k = 1, 2, 3, ...   →   each k×k group of cells → 1 output pixel
    R = 1 / k   for k = 1, 2, 3, ...   →   each cell → k×k output pixels (upscale)

Any other R requires sub-pixel averaging → visible blur.

For each candidate cap we compute the mean "alignment error" over all
qualifying textures: distance from R to the nearest valid R-of-{C,2C,3C,...}
divided by C. 0 means every texture downsamples cleanly. Lower is better.

The score is also weighted by texture pixel count so big textures (which
dominate visual cost AND VRAM) count more than 64x64 icons.

Usage
-----
    # Default: scan caps 256..960 in steps of 16, ignore textures <= 256 px
    python3 tools/unity_astc/find_optimal_cap.py <bundle>

    # Custom range / step / minimum size to consider
    python3 tools/unity_astc/find_optimal_cap.py <bundle> \\
        --min 300 --max 600 --step 8 --include-min-side 400
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


# reuse the cell-size detector from inspect_pixels.py without importing it
# (this script is meant to be standalone runnable in isolation)
def detect_cell_size(img, max_test=16, threshold=1.5):
    """Largest N s.t. every NxN block is internally near-constant. 1 = no
    upscale grid detected. See inspect_pixels.py for the full explanation."""
    a = np.array(img.convert("RGBA"), dtype=np.float32)
    h, w, c = a.shape
    best_n = 1
    for n in range(2, max_test + 1):
        nh, nw = h // n, w // n
        if nh < 4 or nw < 4:
            continue
        crop = a[:nh * n, :nw * n]
        blocks = crop.reshape(nh, n, nw, n, c)
        score = blocks.std(axis=(1, 3)).mean()
        if score < threshold:
            best_n = n
    return best_n


def alignment_error(long_side, cell, cap):
    """Fractional misalignment for downsampling long_side → cap with cell-grid
    of size `cell`. 0 = clean integer-multiple-of-cell ratio. Up to 0.5.

    For cap >= long_side we don't downsample, score 0 (trivially clean).
    """
    if cap >= long_side:
        return 0.0
    R = long_side / cap
    # closest k*cell (k >= 1)
    k_float = R / cell
    k = max(1, round(k_float))
    R_ideal = k * cell
    # normalized error: distance from ideal R, scaled by cell so cells with
    # different sizes contribute on equal footing
    err = abs(R - R_ideal) / cell
    return err


RAW_FORMATS = (TF.RGBA32, TF.RGB24, TF.BGRA32, TF.ARGB32)


def collect_textures(env, min_side, log):
    """Scan bundle: for each big RGBA32/RGB24 Texture2D, decode and detect
    cell size. Returns list of (name, w, h, cell, pixel_count)."""
    out = []
    n_total = 0
    for obj in env.objects:
        if obj.type.name != "Texture2D":
            continue
        n_total += 1
        try:
            t = obj.read()
        except Exception:
            continue
        if t.m_TextureFormat not in RAW_FORMATS:
            continue
        if max(t.m_Width, t.m_Height) < min_side:
            continue
        if t.m_Name.endswith(" Atlas"):
            continue
        try:
            img = t.image
        except Exception:
            continue
        if img is None or img.size == (0, 0):
            continue
        cell = detect_cell_size(img)
        out.append((t.m_Name, t.m_Width, t.m_Height, cell, t.m_Width * t.m_Height))
    log(f"  scanned {n_total} Texture2D, kept {len(out)} candidates "
        f"(RGBA32/RGB24 with long side >= {min_side})")
    return out


def score_cap(textures, cap):
    """Pixel-count-weighted mean alignment error at the given cap."""
    tot_w = tot_err = 0
    for _, w, h, cell, px in textures:
        err = alignment_error(max(w, h), cell, cap)
        tot_err += err * px
        tot_w += px
    return tot_err / tot_w if tot_w else 0.0


def main():
    ap = argparse.ArgumentParser(
        description=__doc__.split("Usage\n", 1)[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Usage\n-----\n", 1)[1])
    ap.add_argument("bundle", help="path to a Unity bundle")
    ap.add_argument("--min", type=int, default=256, dest="cap_min",
                    help="smallest cap to test (default: 256)")
    ap.add_argument("--max", type=int, default=960, dest="cap_max",
                    help="largest cap to test (default: 960)")
    ap.add_argument("--step", type=int, default=16, dest="cap_step",
                    help="cap step (default: 16)")
    ap.add_argument("--include-min-side", type=int, default=384,
                    help="only consider textures whose long side >= this "
                         "(default: 384; smaller textures wouldn't be capped "
                         "by reasonable values anyway)")
    args = ap.parse_args()

    bundle_in = Path(args.bundle).resolve()
    if not bundle_in.is_file():
        sys.exit(f"input not found: {bundle_in}")

    def log(msg): print(msg, flush=True)

    log(f"Loading {bundle_in}")
    env = UnityPy.load(str(bundle_in))

    log("\nDetecting cell sizes (may take a minute on big bundles) ...")
    textures = collect_textures(env, args.include_min_side, log)
    if not textures:
        sys.exit("no candidates — lower --include-min-side")

    # quick cell-size histogram for context
    cell_hist = {}
    for _, _, _, c, _ in textures:
        cell_hist[c] = cell_hist.get(c, 0) + 1
    log("\n  cell-size distribution among candidates:")
    for c in sorted(cell_hist):
        log(f"    cell={c}: {cell_hist[c]} textures")

    log("\n--- alignment score (lower = sharper after downsample) ---")
    results = []
    for cap in range(args.cap_min, args.cap_max + 1, args.cap_step):
        s = score_cap(textures, cap)
        results.append((cap, s))

    # find local minima
    best_cap, best_score = min(results, key=lambda r: r[1])

    # render an ASCII bar chart of the score curve, normalized
    max_score = max(s for _, s in results) or 1.0
    BAR = 50
    log(f"\n  cap        score    {' ' * 4}<-- lower is sharper")
    for cap, s in results:
        bar = "#" * int(round(s / max_score * BAR))
        marker = "  <== BEST" if cap == best_cap else ""
        log(f"  {cap:4d}   {s:8.4f}    {bar}{marker}")

    log(f"\nBest cap by mean alignment error: {best_cap}")
    log(f"Score: {best_score:.4f}  (0 = every texture downsamples to an "
        "integer-cell multiple)")

    # top 5 lowest-error caps
    log("\nTop 5 caps:")
    for cap, s in sorted(results, key=lambda r: r[1])[:5]:
        log(f"  cap={cap}  score={s:.4f}")

    # per-texture sanity check at the best cap
    log(f"\nPer-texture alignment at cap={best_cap} (worst 8):")
    rows = sorted(((name, w, h, cell,
                    alignment_error(max(w, h), cell, best_cap))
                   for name, w, h, cell, _ in textures),
                  key=lambda r: r[4], reverse=True)
    for name, w, h, cell, err in rows[:8]:
        R = max(w, h) / best_cap
        log(f"  err={err:.3f}  {name!r}  {w}x{h}  cell={cell}  ratio={R:.3f}")


if __name__ == "__main__":
    main()
