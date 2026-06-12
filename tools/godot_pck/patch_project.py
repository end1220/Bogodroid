#!/usr/bin/env python3
"""
Patch a recovered project's project.godot to import textures for a specific
target GPU family, dropping the variants the device can't use.

godot's default `[rendering] textures/vram_compression/` settings:
    import_s3tc_bptc=true   # generates .bptc.ctex (desktop dGPU / iGPU)
    import_etc2_astc=false  # generates .astc.ctex (mobile Mali/Adreno)

For a Mali handheld port:
    import_s3tc_bptc=false  # skip BPTC — wasted disk + import time
    import_etc2_astc=true   # ASTC is what the GPU actually decodes

Bogodroid-godot patches (require the patched editor binary, no-op on stock):
    astc_block_size=4       # 4 = ASTC_FORMAT_4x4 (8 bpp), 8 = ASTC_FORMAT_8x8 (2 bpp)
    force_astc_for_mobile=  # if true, the mobile branch emits ASTC instead of ETC2
    astc_preset=            # "" / "medium" (default) / "fastest" / "fast" / "thorough" / "exhaustive"

Usage:
    patch_project.py <recovered_dir>
        [--target mobile|both]            default mobile = ASTC only
        [--astc-block-size {4,8}]         default 4 (matches stock godot)
        [--astc-preset PRESET]            default "" → medium (stock)
        [--force-astc-for-mobile]         force ASTC over ETC2 on the mobile branch
        [--dry-run]
"""
import argparse
import re
import sys
from pathlib import Path


def patch_project_godot(path: Path, target: str, astc_block_size: int,
                        astc_preset: str, force_astc: bool, dry_run: bool):
    text = path.read_text()
    original = text

    # All settings to set under [rendering] textures/vram_compression/<key>=value.
    wanted = {
        "mobile": {"import_s3tc_bptc": "false", "import_etc2_astc": "true"},
        "both":   {"import_s3tc_bptc": "true",  "import_etc2_astc": "true"},
    }[target]
    # Only emit Bogodroid keys when they differ from godot's defaults, so we
    # don't pollute project.godot when running with the stock editor.
    if astc_block_size and astc_block_size != 4:
        wanted["astc_block_size"] = str(astc_block_size)
    if astc_preset:
        wanted["astc_preset"] = f'"{astc_preset}"'
    if force_astc:
        wanted["force_astc_for_mobile"] = "true"

    for key, val in wanted.items():
        full_key = f"textures/vram_compression/{key}"
        pattern = re.compile(rf"^{re.escape(full_key)}=.*$", re.MULTILINE)
        replacement = f"{full_key}={val}"
        if pattern.search(text):
            text = pattern.sub(replacement, text)
        else:
            # Insert under [rendering] section, or create one if missing.
            if re.search(r"^\[rendering\]\s*$", text, re.MULTILINE):
                text = re.sub(
                    r"(^\[rendering\]\s*\n)",
                    rf"\1{replacement}\n",
                    text,
                    count=1,
                    flags=re.MULTILINE,
                )
            else:
                text += f"\n[rendering]\n{replacement}\n"

    if text == original:
        print(f"{path}: already in target state, no change")
        return False

    if dry_run:
        print(f"--- {path} (would change)")
        for k, v in wanted.items():
            print(f"  textures/vram_compression/{k} = {v}")
        return True

    path.write_text(text)
    print(f"{path}: patched")
    for k, v in wanted.items():
        print(f"  textures/vram_compression/{k} = {v}")
    return True


def main():
    p = argparse.ArgumentParser()
    p.add_argument("project_dir", type=Path)
    p.add_argument("--target", choices=["mobile", "both"], default="mobile")
    p.add_argument("--astc-block-size", type=int, choices=[4, 8], default=4,
                   help="ASTC block size (4=8bpp default, 8=2bpp). "
                        "Requires the Bogodroid-patched godot editor for any value other than 4.")
    p.add_argument("--astc-preset", default="",
                   choices=["", "fastest", "fast", "medium", "thorough", "exhaustive"],
                   help="ASTC encoder quality preset (empty=stock medium).")
    p.add_argument("--force-astc-for-mobile", action="store_true",
                   help="Force the mobile branch to emit ASTC instead of ETC2 for non-HQ textures.")
    p.add_argument("--dry-run", action="store_true")
    args = p.parse_args()

    pg = args.project_dir / "project.godot"
    if not pg.exists():
        print(f"no project.godot at {pg}", file=sys.stderr)
        sys.exit(1)

    patch_project_godot(pg, args.target, args.astc_block_size, args.astc_preset,
                        args.force_astc_for_mobile, args.dry_run)


if __name__ == "__main__":
    main()
