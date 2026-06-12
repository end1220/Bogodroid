#!/usr/bin/env python3
"""
Cap `amount = N` lines in every .tscn — godot's GpuParticles2D.Amount
property declared in the scene file. Big particle emitters (500-particle
sub-emitters in StS2's combat VFX) are the worst per-frame allocator and
GPU buffer hog on a 1 GB-RAM Mali handheld.

The cap mirrors the constant the community Android port of StS2 picked
for its `MobileShaderCompatibility.ApplyTimelineParticleCompatibility`
(96 for "background" particles, 16 for "foreground trail" particles).
We just hard-cap everything at one number; the per-emitter foreground
distinction can't be made from the .tscn alone.

Why this is safe: capping `amount` doesn't change rendering correctness,
just the visual density. Combat VFX with 500 → 96 particles still
"reads" the same animation, just with thinner trails / less smoke. The
.tscn lines we rewrite are static authored values, so the cap is final;
the cap_particle_amount run survives a godot --import pass.

Usage:
    cap_particle_amount.py <recovered_dir> [--cap 96] [--dry-run]
"""
import argparse
import re
from pathlib import Path


AMOUNT_LINE = re.compile(r"^amount = (\d+)\s*$", re.MULTILINE)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("project_dir", type=Path)
    p.add_argument("--cap", type=int, default=96,
                   help="upper bound on `amount = N` in .tscn (default 96)")
    p.add_argument("--dry-run", action="store_true")
    args = p.parse_args()

    if not (args.project_dir / "project.godot").exists():
        raise SystemExit(f"no project.godot at {args.project_dir}")

    tscns = list(args.project_dir.rglob("*.tscn"))
    bumped_files = 0
    bumped_lines = 0
    histogram = {}

    for f in tscns:
        try:
            text = f.read_text()
        except Exception:
            continue
        changed = False

        def cap(match):
            nonlocal bumped_lines, changed
            n = int(match.group(1))
            if n > args.cap:
                bumped_lines += 1
                histogram[n] = histogram.get(n, 0) + 1
                changed = True
                return f"amount = {args.cap}"
            return match.group(0)

        new = AMOUNT_LINE.sub(cap, text)
        if changed:
            bumped_files += 1
            if not args.dry_run:
                f.write_text(new)

    verb = "would cap" if args.dry_run else "capped"
    print(f"{verb} `amount = N > {args.cap}` to {args.cap} on {bumped_lines} lines across {bumped_files} .tscn files")
    if histogram:
        print("original values touched:")
        for n, count in sorted(histogram.items(), reverse=True):
            print(f"  {n:5d} → {args.cap}   ({count} occurrences)")


if __name__ == "__main__":
    main()
