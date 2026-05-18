#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2025-2026 jenny92-tech <jennyliu90223@gmail.com>
"""
Shrink Unity asset-bundle textures to a configurable long-side cap.

Designed for Bogodroid Unity ports running on memory-constrained handhelds
(1 GB-class devices). Hollow Knight, for example, ships 4096×4096 sprite
atlases that consume ~335 MB of GPU memory on Mali UMA — too much for a 1 GB
device after kernel and audio daemons. This tool re-encodes oversized
textures in place at install time, with no runtime overhead.

What it does
------------
1. Loads data.unity3d (or any Unity bundle).
2. For every Texture2D whose long side exceeds --cap (default 1280):
     decode (via UnityPy) → Lanczos downsample → ASTC re-encode → write back
3. Rewrites every Sprite that references a shrunk atlas, scaling its
   pixel-space coordinates (m_Rect, m_Offset, textureRect, uvTransform, …)
   so visible sprite positions stay correct.
4. Saves the new bundle alongside the input as <input>.shrunk

What it does NOT do
-------------------
- Update TMP_FontAsset glyph rects. IL2CPP-stripped MonoBehaviour typetrees
  don't expose those fields without a TypeTreeGenerator pass; for ports
  using a system-font fallback (DroidSansFallback via /system/fonts/) the
  TMP atlas isn't on the render path anyway.
- Handle particle systems, custom shaders, or any code that hard-codes UVs
  outside the standard Sprite path. Failure mode: visibly broken textures
  in those features. Use --skip <name> (repeatable) to exclude problem
  atlases as they're discovered.
- Compact the bundle on disk. UnityPy appends new texture data without
  deleting the original .resS sections, so the output bundle is typically
  ~20% larger than input. RSS at runtime is what matters, not disk size.

Usage
-----
    # Default: cap at 1280, skip nothing
    python3 tools/shrink_bundle.py /path/to/data.unity3d

    # Tighter cap, suitable for a 540p-only device
    python3 tools/shrink_bundle.py data.unity3d --cap 960

    # Exclude atlases that proved broken in QA
    python3 tools/shrink_bundle.py data.unity3d \\
        --skip particle_smoke_atlas \\
        --skip shader_palette_lut

    # Skip list from a file (one name per line, '#' for comments)
    python3 tools/shrink_bundle.py data.unity3d --skip-file skip.txt

    # Dry run — list what would be changed, write nothing
    python3 tools/shrink_bundle.py data.unity3d --dry-run

Requirements
------------
    pip install UnityPy Pillow astc-encoder-py
"""

import argparse
import os
import struct
import sys
import time
from pathlib import Path

try:
    import UnityPy
    from PIL import Image
except ImportError as e:
    sys.exit(f"missing dependency: {e}\n"
             "install with:  pip install UnityPy Pillow astc-encoder-py")


# ─────────────────────────── helpers ───────────────────────────

def parse_skip_file(path):
    """Read a skip-list file: one atlas name per line, '#' starts a comment."""
    names = set()
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if line:
                names.add(line)
    return names


def resolve_pptr(env_obj, pptr):
    """Resolve a PPtr {m_FileID, m_PathID} to (assets_file_name, path_id)."""
    if not isinstance(pptr, dict):
        return None
    pid = pptr.get("m_PathID", 0)
    fid = pptr.get("m_FileID", 0)
    if pid == 0:
        return None
    af = env_obj.assets_file
    if fid == 0:
        return (af.name, pid)
    try:
        return (af.externals[fid - 1].path, pid)
    except Exception:
        return None


def scale_rect(r, sx, sy):
    """Scale a typetree {x,y,width,height} dict in place."""
    r["x"]      *= sx
    r["y"]      *= sy
    r["width"]  *= sx
    r["height"] *= sy


def scale_point(p, sx, sy):
    """Scale a typetree {x,y} dict in place."""
    p["x"] *= sx
    p["y"] *= sy


# ─────────────────────────── pipeline ───────────────────────────

def pass_shrink_textures(env, cap, skip_names, dry_run, log):
    """Shrink oversized Texture2Ds. Returns dict[(file, pid)] -> scale info."""
    candidates = []
    for obj in env.objects:
        if obj.type.name != "Texture2D":
            continue
        try:
            t = obj.read()
            if max(t.m_Width, t.m_Height) <= cap:
                continue
            if t.m_Name in skip_names:
                log(f"  SKIP {t.m_Name!r}")
                continue
            candidates.append((obj, t))
        except Exception:
            continue

    log(f"\nPass 1: shrinking {len(candidates)} textures > {cap} long-side")
    tex_scale = {}
    if dry_run:
        for obj, t in candidates:
            w, h = t.m_Width, t.m_Height
            log(f"  would shrink {t.m_Name!r} {w}x{h}")
        return tex_scale

    n_ok = n_fail = 0
    t_start = time.time()
    for i, (obj, t) in enumerate(candidates, 1):
        w, h = t.m_Width, t.m_Height
        long_side = max(w, h)
        scale = cap / long_side
        # round to multiple of 4 (ASTC 4x4 block alignment, prevents padding)
        nw = max(4, round(w * scale) // 4 * 4)
        nh = max(4, round(h * scale) // 4 * 4)
        try:
            img = t.image
            if img is None or img.size == (0, 0):
                n_fail += 1
                continue
            new_img = img.resize((nw, nh), Image.LANCZOS)
            t.image = new_img
            t.save()
            tex_scale[(obj.assets_file.name, obj.path_id)] = (w, h, nw, nh, nw / w, nh / h)
            n_ok += 1
        except Exception as e:
            n_fail += 1
            if n_fail <= 5:
                log(f"  FAIL {t.m_Name!r}: {e}")
        if i % 20 == 0 or i == len(candidates):
            elapsed = time.time() - t_start
            rate = i / elapsed if elapsed > 0 else 0
            eta = (len(candidates) - i) / rate if rate > 0 else 0
            log(f"  [{i:>4}/{len(candidates)}] ok={n_ok} fail={n_fail}  "
                f"rate={rate:.1f}/s  eta={eta:.0f}s")
    log(f"  Pass 1 done: {n_ok} shrunk, {n_fail} failed")
    return tex_scale


def pass_rewrite_sprites(env, tex_scale, dry_run, log):
    """Walk every Sprite, scale its pixel-space rects to match shrunk atlas."""
    log(f"\nPass 2: rewriting Sprite UV coords")
    n_total = n_touched = 0
    for obj in env.objects:
        if obj.type.name != "Sprite":
            continue
        n_total += 1
        try:
            tree = obj.read_typetree()
            rd = tree.get("m_RD", {})
            key = resolve_pptr(obj, rd.get("texture", {}))
            if key is None or key not in tex_scale:
                continue
            _, _, _, _, sx, sy = tex_scale[key]

            if "m_Rect"   in tree: scale_rect (tree["m_Rect"],   sx, sy)
            if "m_Offset" in tree: scale_point(tree["m_Offset"], sx, sy)
            if "m_Border" in tree:
                b = tree["m_Border"]
                for k, s in (("x", sx), ("y", sy), ("z", sx), ("w", sy)):
                    if k in b: b[k] *= s
            if "textureRect"       in rd: scale_rect (rd["textureRect"],       sx, sy)
            if "textureRectOffset" in rd: scale_point(rd["textureRectOffset"], sx, sy)
            if "uvTransform" in rd:
                uvt = rd["uvTransform"]
                uvt["x"] *= sx; uvt["y"] *= sy
                uvt["z"] *= sx; uvt["w"] *= sy

            if not dry_run:
                obj.save_typetree(tree)
            n_touched += 1
        except Exception as e:
            if n_touched < 3:
                log(f"  sprite err on pid={obj.path_id}: {e}")
    log(f"  Pass 2 done: {n_touched}/{n_total} sprites rewritten")
    return n_touched


# ─────────────────────────── main ───────────────────────────

def main():
    ap = argparse.ArgumentParser(
        description="Shrink Unity Texture2D atlases to a long-side cap.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Usage\n-----\n", 1)[1].split("Requirements")[0])
    ap.add_argument("bundle", help="path to data.unity3d (or any Unity bundle)")
    ap.add_argument("-o", "--out", help="output path (default: <bundle>.shrunk)")
    ap.add_argument("--cap", type=int, default=1280,
                    help="cap on Texture2D long side, in pixels (default: 1280)")
    ap.add_argument("--skip", action="append", default=[],
                    help="atlas name to skip (repeatable)")
    ap.add_argument("--skip-file",
                    help="file with one atlas name per line")
    ap.add_argument("--dry-run", action="store_true",
                    help="analyze only; don't shrink or write")
    ap.add_argument("-q", "--quiet", action="store_true",
                    help="suppress per-batch progress")
    args = ap.parse_args()

    bundle_in = Path(args.bundle).resolve()
    if not bundle_in.is_file():
        sys.exit(f"input not found: {bundle_in}")
    bundle_out = Path(args.out).resolve() if args.out else bundle_in.with_suffix(
        bundle_in.suffix + ".shrunk")

    skip_names = set(args.skip)
    if args.skip_file:
        skip_names |= parse_skip_file(args.skip_file)

    def log(msg):
        if not args.quiet or msg.startswith(("\n", "===", "  Pass", "  FAIL", "  SKIP")):
            print(msg, flush=True)

    log(f"Loading {bundle_in}")
    t0 = time.time()
    env = UnityPy.load(str(bundle_in))
    log(f"  loaded in {time.time()-t0:.1f}s")
    if skip_names:
        log(f"  skip list ({len(skip_names)}): {', '.join(sorted(skip_names))}")

    tex_scale = pass_shrink_textures(env, args.cap, skip_names, args.dry_run, log)
    if not tex_scale:
        log("\nNothing to do (no textures exceeded cap, or dry-run).")
        return

    pass_rewrite_sprites(env, tex_scale, args.dry_run, log)

    if args.dry_run:
        log("\nDry run — no bundle written.")
        return

    log(f"\nSaving bundle ...")
    t0 = time.time()
    with open(bundle_out, "wb") as f:
        f.write(env.file.save(packer="original"))
    log(f"  written in {time.time()-t0:.1f}s")

    in_mb  = bundle_in.stat().st_size  / 1024 / 1024
    out_mb = bundle_out.stat().st_size / 1024 / 1024
    log(f"\n=== Summary ===")
    log(f"  Cap:           {args.cap} px long-side")
    log(f"  Textures shrunk:  {len(tex_scale)}")
    log(f"  Input bundle:  {in_mb:.1f} MB → Output: {out_mb:.1f} MB ({out_mb-in_mb:+.1f} MB)")
    log(f"  Output: {bundle_out}")


if __name__ == "__main__":
    main()
