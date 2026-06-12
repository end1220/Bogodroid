#!/usr/bin/env python3
"""
Remove the [node name="WorldEnvironment" type="WorldEnvironment" ...]
block from scenes/game.tscn. Stock godot template_release built with
disable_3d=yes has no WorldEnvironment native binding, so the node
instantiates as a half-broken placeholder, and StS2's renderer then
darkens the whole frame trying to apply the missing ambient + glow.

The community Android port of StS2 added Activate/Deactivate methods on
NGame and ships release builds with the node detached. We mirror that —
strip the node out of the scene file before the pck is repacked, so the
runtime never tries to instantiate it.

Usage:
    strip_worldenv.py <recovered_dir> [--dry-run]
"""
import argparse
import re
import sys
from pathlib import Path


def main():
    p = argparse.ArgumentParser()
    p.add_argument("project_dir", type=Path)
    p.add_argument("--dry-run", action="store_true")
    args = p.parse_args()

    scene = args.project_dir / "scenes" / "game.tscn"
    if not scene.exists():
        print(f"no scenes/game.tscn at {scene}", file=sys.stderr)
        sys.exit(1)

    text = scene.read_text()

    # The block we want to remove looks like:
    #
    #     [node name="WorldEnvironment" type="WorldEnvironment" parent="."]
    #     unique_name_in_owner = true
    #     environment = SubResource("Environment_smd41")
    #
    # ending at the next [node ...] header or end-of-file. Match the
    # whole block including the trailing blank line that godot writes.
    pattern = re.compile(
        r'\n\[node name="WorldEnvironment" type="WorldEnvironment"[^\]]*\][^\[]*',
        re.DOTALL,
    )
    new_text, count = pattern.subn("\n", text)

    if count == 0:
        print(f"{scene}: no WorldEnvironment node found (already stripped?)")
        return

    if args.dry_run:
        print(f"--- {scene} (would strip {count} WorldEnvironment node)")
        return

    scene.write_text(new_text)
    print(f"{scene}: stripped {count} WorldEnvironment node ({len(text) - len(new_text)} chars removed)")


if __name__ == "__main__":
    main()
