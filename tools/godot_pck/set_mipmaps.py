#!/usr/bin/env python3
"""
Flip `mipmaps/generate` in every texture .import so the next `reimport.sh`
either skips the mip chain (default — saves ~33 % VRAM per texture) or
brings it back (e.g. for a future HD repack where minification artefacts
matter).

For 2D card games like StS2: sprites draw at fixed pixel size and never
minify, so the lower mip levels never get sampled — `--state off` is the
right call and reclaims ~35-40 MB physical VRAM on a 1 GB Mali handheld
with ~2700 resident textures.

For a hypothetical HD patch that targets a desktop / future 2-4 GB
handheld: `--state on` keeps mipmaps on so scaled or rotated draws stay
crisp.

Usage:
    set_mipmaps.py <recovered_dir>
        --state {on,off}                   default: off
        [--exclude-glob '*pattern*']       repeatable; e.g. keep mipmaps on
                                            logo so the splash scales sharply
        [--dry-run]
"""
import argparse
import fnmatch
import re
import sys
from pathlib import Path

MIP_LINE = re.compile(r"^mipmaps/generate=(true|false)\s*$", re.MULTILINE)
TEXTURE_IMPORTER = re.compile(r'^importer="texture"\s*$', re.MULTILINE)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("project_dir", type=Path)
    p.add_argument("--state", choices=["on", "off"], default="off",
                   help="on = enable mipmaps (HD), off = disable (handheld)")
    p.add_argument("--exclude-glob", action="append", default=[])
    p.add_argument("--dry-run", action="store_true")
    args = p.parse_args()

    if not (args.project_dir / "project.godot").exists():
        sys.exit(f"no project.godot at {args.project_dir}")

    target = "true" if args.state == "on" else "false"
    imports = list(args.project_dir.rglob("*.import"))
    changed = 0
    skipped = 0
    excluded = 0
    for f in imports:
        try:
            text = f.read_text()
        except Exception:
            continue
        if not TEXTURE_IMPORTER.search(text):
            skipped += 1
            continue
        m = MIP_LINE.search(text)
        if not m or m.group(1) == target:
            skipped += 1
            continue
        rel = str(f.relative_to(args.project_dir))
        if any(fnmatch.fnmatch(rel, pat) for pat in args.exclude_glob):
            excluded += 1
            continue
        new = MIP_LINE.sub(f"mipmaps/generate={target}", text)
        if not args.dry_run:
            f.write_text(new)
        changed += 1

    verb = "would set" if args.dry_run else "set"
    msg = f"{verb} mipmaps/generate={target} on {changed} of {len(imports)} .import files (skipped {skipped})"
    if excluded:
        msg += f" (excluded {excluded} matching {args.exclude_glob})"
    print(msg)


if __name__ == "__main__":
    main()
