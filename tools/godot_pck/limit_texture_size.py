#!/usr/bin/env python3
"""
Cap every texture's `process/size_limit` so godot --import downscales
sources whose largest dimension exceeds the limit.

Why: a Mali handheld with a 1280×720 panel never displays a texel finer
than the texture's native resolution divided by how much screen space the
sprite occupies. Shipping 4K atlases is pure GPU memory waste on this
form factor. Capping at the panel's long edge typically cuts VRAM 4–8×
with no visible loss on a 5–6" 720p screen.

godot's behaviour:
    process/size_limit=0     no cap (default)
    process/size_limit=N     scale source down so max(width, height) ≤ N
                             before compressing into the .ctex

We tune this in the .import file BEFORE re-import; the next
`reimport.sh` regenerates `.godot/imported/*.ctex` at the smaller size.

Usage:
    limit_texture_size.py <recovered_dir> --limit 1280 [--dry-run]
"""

import argparse
import fnmatch
import re
import sys
from pathlib import Path

SIZE_LINE = re.compile(r"^process/size_limit=\d+\s*$", re.MULTILINE)
TEXTURE_IMPORTER = re.compile(r'^importer="texture"\s*$', re.MULTILINE)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("project_dir", type=Path)
    p.add_argument("--limit", type=int, default=1280,
                   help="default size_limit for any .import not matched by --limit-glob")
    p.add_argument("--limit-glob", action="append", default=[],
                   metavar="GLOB:N",
                   help="per-pattern size_limit, e.g. '*card_atlas*:1280' or "
                        "'*ui_atlas*:720'; first match wins, falls back to --limit "
                        "(repeatable)")
    p.add_argument("--exclude-glob", action="append", default=[],
                   help="leave process/size_limit untouched on .import paths "
                        "matching glob (repeatable; e.g. '*logo_megacrit*')")
    p.add_argument("--dry-run", action="store_true")
    args = p.parse_args()

    # Parse --limit-glob entries into (pattern, value) pairs, ordered.
    per_pattern = []
    for spec in args.limit_glob:
        if ":" not in spec:
            sys.exit(f"--limit-glob must be GLOB:N, got: {spec}")
        pat, n = spec.rsplit(":", 1)
        per_pattern.append((pat, int(n)))

    if not (args.project_dir / "project.godot").exists():
        sys.exit(f"no project.godot at {args.project_dir}")

    imports = list(args.project_dir.rglob("*.import"))
    per_limit_count = {}      # value → count
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
        if not SIZE_LINE.search(text):
            skipped += 1
            continue
        rel = str(f.relative_to(args.project_dir))
        if any(fnmatch.fnmatch(rel, pat) for pat in args.exclude_glob):
            excluded += 1
            continue
        # Pick the first matching per-pattern limit, otherwise the default.
        limit = args.limit
        for pat, n in per_pattern:
            if fnmatch.fnmatch(rel, pat):
                limit = n
                break
        new = SIZE_LINE.sub(f"process/size_limit={limit}", text)
        if new == text:
            skipped += 1
            continue
        if not args.dry_run:
            f.write_text(new)
        per_limit_count[limit] = per_limit_count.get(limit, 0) + 1

    verb = "would set" if args.dry_run else "set"
    total = sum(per_limit_count.values())
    breakdown = ", ".join(f"{v}@{k}" for k, v in sorted(per_limit_count.items()))
    msg = f"{verb} process/size_limit on {total} of {len(imports)} .import files ({breakdown}; skipped {skipped})"
    if excluded:
        msg += f" (excluded {excluded} matching {args.exclude_glob})"
    print(msg)


if __name__ == "__main__":
    main()
