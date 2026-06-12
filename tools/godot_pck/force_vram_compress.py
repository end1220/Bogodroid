#!/usr/bin/env python3
"""
Rewrite every texture .import file in a recovered godot project to use
VRAM compression (compress/mode=2 = "VRAM Compressed").

Godot lets each .import pick a compression mode independently:
    0 = Lossless  ← decoded to uncompressed RGBA8 at load time
    1 = Lossy
    2 = VRAM Compressed   ← stays compressed in VRAM (ETC2/ASTC/BPTC/S3TC)
    3 = VRAM Uncompressed
    4 = Basis Universal

On a Mali handheld the difference is dramatic:
    mode=0  2048² PNG-source texture → 16 MB RGBA8 in VRAM
    mode=2  2048² ASTC 8×8            → ~1 MB in VRAM (GPU decodes)

StS2 ships ~2200 textures at mode=0 (UI icons, card portraits, etc.) which
each take their full uncompressed footprint in Mali GPU memory. That's
fine on a desktop GPU with gigabytes of VRAM but blows past Mali's
typical ~512 MB budget on 1 GB handhelds and triggers the GPU's own OOM.

Strategy: flip every "compress/mode=0" to 2 so the next `reimport.sh`
regenerates them as ETC2/ASTC. Visual cost is minor (~5% quality on edges,
indistinguishable on a 1280×720 panel); memory cost goes from 16 MB per
2048² texture to 1 MB.

Usage:
    force_vram_compress.py <recovered_dir> [--dry-run]
"""

import argparse
import fnmatch
import re
import sys
from pathlib import Path

MODE_LINE = re.compile(r"^compress/mode=0\s*$", re.MULTILINE)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("project_dir", type=Path)
    p.add_argument("--exclude-glob", action="append", default=[],
                   help="leave compress/mode untouched on .import paths matching glob "
                        "(repeatable; e.g. '*logo_megacrit*')")
    p.add_argument("--dry-run", action="store_true")
    args = p.parse_args()

    if not (args.project_dir / "project.godot").exists():
        sys.exit(f"no project.godot at {args.project_dir}")

    imports = list(args.project_dir.rglob("*.import"))
    changed = 0
    excluded = 0
    for f in imports:
        try:
            text = f.read_text()
        except Exception:
            continue
        if not MODE_LINE.search(text):
            continue
        rel = str(f.relative_to(args.project_dir))
        if any(fnmatch.fnmatch(rel, pat) for pat in args.exclude_glob):
            excluded += 1
            continue
        new = MODE_LINE.sub("compress/mode=2", text)
        if args.dry_run:
            changed += 1
        else:
            f.write_text(new)
            changed += 1

    verb = "would flip" if args.dry_run else "flipped"
    msg = f"{verb} compress/mode 0→2 on {changed} of {len(imports)} .import files"
    if excluded:
        msg += f" (excluded {excluded} matching {args.exclude_glob})"
    print(msg)


if __name__ == "__main__":
    main()
