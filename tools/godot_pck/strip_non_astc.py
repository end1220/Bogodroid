#!/usr/bin/env python3
"""
After `--import` has filled `.godot/imported/` with .bptc.ctex /
.s3tc.ctex / .etc2.ctex / .astc.ctex variants, strip everything Mali
GPU can't decode (.bptc / .s3tc) and rewrite each .import to reference
only the mobile-friendly paths.

Why post-process instead of patching before import? godot's importer
rewrites the .import metadata when it runs and always emits every
VRAM-format the project setting allows; trying to suppress it from the
project.godot side is fragile across godot versions. Letting it write
the noisy dual-format output and then trimming after is reliable.

What we keep:
    .astc.ctex   ← preferred mobile format, hardware-decoded on Mali-G57+
    .etc2.ctex   ← universal mobile fallback, all GLES 3.0+ GPUs

What we drop:
    .bptc.ctex   ← desktop dGPU only
    .s3tc.ctex   ← desktop iGPU/dGPU, predates ETC2

Per-import rewrites:
    path.astc / path.etc2 kept; path.bptc / path.s3tc dropped
    metadata.imported_formats reduced to ["etc2_astc"]
    dest_files reduced to entries containing .astc.ctex or .etc2.ctex

Usage:
    strip_non_astc.py <recovered_dir> [--dry-run]
"""
import argparse
import re
import sys
from pathlib import Path

# Both ASTC and ETC2 are Mali-native; godot picks one based on what was
# generated. Anything else is dead weight in the final pck.
KEEP_KEYS = ("path.astc", "path.etc2")
DROP_KEYS = ("path.bptc", "path.s3tc")

LINE_PATTERN = re.compile(r'^(path\.\w+)=("[^"]+")$', re.MULTILINE)
FORMATS_PATTERN = re.compile(r'"imported_formats"\s*:\s*\[[^\]]*\]')
DEST_FILES_PATTERN = re.compile(r'^dest_files=\[(.*?)\]$', re.MULTILINE | re.DOTALL)


def rewrite_one(path: Path) -> bool:
    text = path.read_text()
    original = text

    # Strip path.bptc / path.s3tc; keep path.astc / path.etc2 if present.
    def _path_filter(m):
        return m.group(0) if m.group(1) in KEEP_KEYS else ""
    text = LINE_PATTERN.sub(_path_filter, text)
    # Collapse any blank lines we introduced.
    text = re.sub(r"\n{3,}", "\n\n", text)

    # imported_formats → mobile only.
    text = FORMATS_PATTERN.sub('"imported_formats": ["etc2_astc"]', text)

    # dest_files → only mobile-format entries.
    def _dest_filter(m):
        items = [s.strip() for s in m.group(1).split(",")]
        kept = [s for s in items if ".astc.ctex" in s or ".etc2.ctex" in s]
        return "dest_files=[" + ", ".join(kept) + "]"
    text = DEST_FILES_PATTERN.sub(_dest_filter, text)

    if text == original:
        return False
    path.write_text(text)
    return True


def main():
    p = argparse.ArgumentParser()
    p.add_argument("project_dir", type=Path)
    p.add_argument("--dry-run", action="store_true")
    args = p.parse_args()

    if not (args.project_dir / "project.godot").exists():
        print(f"no project.godot at {args.project_dir}", file=sys.stderr)
        sys.exit(1)

    imported_dir = args.project_dir / ".godot" / "imported"

    # 1) delete desktop-only .ctex (.bptc, .s3tc). Keep .astc + .etc2 — both
    # are mobile-native and one of the two will be the only format some
    # textures actually got generated as.
    non_mobile = []
    for suffix in (".bptc.ctex", ".s3tc.ctex"):
        non_mobile.extend(imported_dir.rglob(f"*{suffix}"))
    print(f"desktop-only .ctex to delete: {len(non_mobile)}")
    if not args.dry_run:
        for f in non_mobile:
            f.unlink()

    # 1b) delete dead .ctex — every file in .godot/imported/ not referenced
    # by any .import. After flipping compress/mode 0→2 the reimport step
    # emits new ASTC/ETC2 ctex with new hashes; the original lossless
    # ".ctex" (no format suffix) stays on disk and is then pure dead weight
    # in the final pck. Source of truth: every `path*="res://.godot/..."`
    # line across all .import files.
    ref_re = re.compile(r'path[^=]*=\"(res://\.godot/imported/[^\"]+)\"')
    referenced = set()
    for f in args.project_dir.rglob("*.import"):
        try:
            for m in ref_re.finditer(f.read_text()):
                referenced.add(m.group(1).replace("res://", ""))
        except Exception:
            pass
    on_disk = list(imported_dir.rglob("*.ctex"))
    dead = [p for p in on_disk
            if str(p.relative_to(args.project_dir)) not in referenced]
    print(f"dead .ctex (orphaned by reimport) to delete: {len(dead)}")
    if not args.dry_run:
        for p in dead:
            p.unlink()

    # 2) rewrite .import files
    imports = list(args.project_dir.rglob("*.import"))
    print(f"scanning {len(imports)} .import files…")
    changed = 0
    for f in imports:
        try:
            if args.dry_run:
                t = f.read_text()
                if any(k in t for k in DROP_KEYS) or "s3tc_bptc" in t:
                    changed += 1
            else:
                if rewrite_one(f):
                    changed += 1
        except Exception as e:
            print(f"  ! {f}: {e}", file=sys.stderr)
    verb = "would rewrite" if args.dry_run else "rewrote"
    print(f"{verb} {changed} .import files")

    # 3) summary of what's left in .godot/imported/
    remaining = sorted({
        p.suffix.lstrip(".") + "." + p.stem.split(".")[-1]
        for p in imported_dir.rglob("*.ctex")
    })
    print("remaining .ctex types:", remaining)


if __name__ == "__main__":
    main()
