#!/usr/bin/env python3
"""
Strip source files that GDRE recovered but the runtime doesn't need.

GDRE `--recover` reverses imported resources back to their source format
(.ctex → .png, .oggvorbisstr → .ogg, …) so you can re-import them. Once
re-import has produced fresh .ctex / .oggvorbisstr under .godot/imported/,
the source PNG / OGG sitting next to the .import file is dead weight in
the repacked pck — godot loads from the path inside .import, not from the
source extension.

Original (Steam) pck never shipped those source files in the first place,
so dropping them gets us back to the same size profile.

Default kill list:
    *.png        (re-imported to *.ctex; runtime reads .ctex via .import)
    *.jpg
    *.jpeg
    *.webp

NOT removed (still in pck):
    *.import      .ctex location + format metadata, runtime needs it
    *.ctex        compiled GPU textures
    *.gd / *.cs   script source — godot 4 packs source by design
    *.tscn/.tres  scenes / resources

Usage:
    strip_sources.py <recovered_dir> [--dry-run]
        [--also <glob>]    add another suffix/glob to the kill list,
                            can be repeated
"""

import argparse
import sys
from pathlib import Path

DEFAULT_KILL_GLOBS = ("*.png", "*.jpg", "*.jpeg", "*.webp")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("project_dir", type=Path)
    p.add_argument("--also", action="append", default=[])
    p.add_argument("--dry-run", action="store_true")
    args = p.parse_args()

    if not (args.project_dir / "project.godot").exists():
        print(f"no project.godot at {args.project_dir}", file=sys.stderr)
        sys.exit(1)

    globs = list(DEFAULT_KILL_GLOBS) + list(args.also)
    print(f"globs: {globs}")

    total = 0
    bytes_freed = 0
    for glob in globs:
        for f in args.project_dir.rglob(glob):
            # Never touch .godot/imported/ — those are runtime artefacts.
            if ".godot/imported" in str(f):
                continue
            # Sanity: also leave .import files alone.
            if f.name.endswith(".import"):
                continue
            bytes_freed += f.stat().st_size
            total += 1
            if not args.dry_run:
                f.unlink()
    verb = "would delete" if args.dry_run else "deleted"
    print(f"{verb} {total} files, {bytes_freed/1024/1024:.1f} MB")


if __name__ == "__main__":
    main()
