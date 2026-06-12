#!/usr/bin/env python3
"""
Patch a base pck with files from an overlay directory, using gdre_tools
--pck-patch. Replaces specific files in the original pck (textures,
extension configs, etc.) while preserving everything else — crucially the
binary spine/fmod/etc. resources we cannot recreate on Mac because the
godot editor here doesn't have those gdextensions installed.

This sidesteps the "full --recover → --import → --pck-create" round-trip,
which corrupts spine .spskel / .spatlas resources whenever the host
godot editor lacks the spine plugin.

Layout:
  --base    /path/to/original.pck         (untouched input)
  --output  /path/to/patched.pck          (gets the changes)
  --overlay /path/to/dir                  (each file mirrors a res:// path)

Every file under <overlay> becomes a --patch-file=<abs_src>=<res_dest>
arg to gdre. With thousands of files the shell command line would overflow
ARG_MAX, so we pass the args via subprocess as a list (no shell).

Usage:
  apply_overlay.py \\
      --base   /path/to/SlayTheSpire2.pck \\
      --output /path/to/SlayTheSpire2.astc.pck \\
      --overlay /path/to/overlay/
"""

import argparse
import os
import platform
import subprocess
import sys
from pathlib import Path


def gdre_bin() -> Path:
    base = Path(__file__).resolve().parent / "bin"
    if platform.system() == "Darwin":
        return base / "Godot RE Tools.app/Contents/MacOS/Godot RE Tools"
    if platform.system() == "Linux":
        return base / "gdre_tools"
    raise SystemExit(f"unsupported OS: {platform.system()}")


def collect_patches(overlay_root: Path, sandbox: Path):
    """Yield (abs_src, res_dest) for every file under the overlay tree.

    Files whose basename matches a name godot main() treats as a project
    config (project.godot, project.binary) are first copied into <sandbox>
    under an inert name — passing those literal strings as `--patch-file=`
    arg values trips godot's "path overrides" guard before the gdsdecomp
    module ever sees them."""
    overlay_root = overlay_root.resolve()
    sandbox.mkdir(parents=True, exist_ok=True)
    INERT = {"project.godot": "project_godot_overlay",
             "project.binary": "project_binary_overlay"}
    for src in overlay_root.rglob("*"):
        if not src.is_file():
            continue
        rel = src.relative_to(overlay_root)
        dest = f"res://{rel.as_posix()}"
        if src.name in INERT:
            relocated = sandbox / INERT[src.name]
            relocated.write_bytes(src.read_bytes())
            yield relocated, dest
        else:
            yield src, dest


CHUNK_BUDGET = 800_000  # bytes of --patch-file= per gdre call; macOS ARG_MAX is 1 MB


def _run_gdre(bin_path: Path, base: Path, output: Path, engine: str, chunk, excludes):
    cmd = [
        str(bin_path),
        "--headless",
        f"--pck-patch={base.resolve()}",
        f"--output={output.resolve()}",
        f"--pck-engine-version={engine}",
        "--pck-version=2",
    ]
    cmd.extend(f"--exclude={glob}" for glob in excludes)
    cmd.extend(f"--patch-file={src}={dest}" for src, dest in chunk)
    print(f"  → gdre with {len(chunk)} patches ({sum(len(c)+1 for c in cmd)//1024} KB cmdline)")
    r = subprocess.run(cmd, check=False)
    if r.returncode != 0:
        sys.exit(r.returncode)


DEFAULT_EXCLUDES = (
    # Desktop-only VRAM formats — Mali / Adreno can't decode them, so drop
    # the original pck's copies. The overlay supplies .astc.ctex / .etc2.ctex
    # in their place.
    "*.bptc.ctex",
    "*.s3tc.ctex",
)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--base", required=True, type=Path)
    p.add_argument("--output", required=True, type=Path)
    p.add_argument("--overlay", required=True, type=Path)
    p.add_argument("--engine-version", default="4.5.1")
    p.add_argument("--exclude", action="append", default=[],
                   help="glob to drop from original pck (default: bptc + s3tc ctex)")
    p.add_argument("--no-default-excludes", action="store_true",
                   help="don't drop desktop ctex from original pck")
    p.add_argument("--dry-run", action="store_true")
    args = p.parse_args()

    excludes = list(args.exclude)
    if not args.no_default_excludes:
        excludes.extend(DEFAULT_EXCLUDES)
    if excludes:
        print(f"excludes (drop from original pck): {excludes}")

    bin_path = gdre_bin()
    if not bin_path.exists():
        sys.exit(f"gdre missing at {bin_path}")

    sandbox = args.output.parent / ".overlay_sandbox"
    patches = list(collect_patches(args.overlay, sandbox))
    # godot main() scans every CLI value for "project.godot" / "project.binary"
    # substrings and aborts with "path overrides not supported" before the
    # gdsdecomp module can run. We can't rename the dest (gdre puts the file
    # at the dest you give it inside the pck), so we drop those patches from
    # the chunked run entirely — they'd never survive the godot guard. Apply
    # them separately afterwards via a different mechanism.
    skipped = [(s, d) for s, d in patches if "project.godot" in d or "project.binary" in d]
    patches = [(s, d) for s, d in patches if "project.godot" not in d and "project.binary" not in d]
    if skipped:
        print(f"  skipped {len(skipped)} reserved-name files (godot guard) — apply separately:")
        for s, d in skipped:
            print(f"    {d}  (from {s})")
    if not patches:
        sys.exit(f"no files under overlay {args.overlay}")
    print(f"overlay has {len(patches)} files (sandboxed reserved-name copies in {sandbox})")

    # macOS exec rejects argv beyond ARG_MAX (~256 KB - 1 MB). The full
    # surface is far above that, so chunk the patch set and chain gdre
    # invocations: each round patches the previous round's output.
    chunks = []
    cur, size = [], 0
    for src, dest in patches:
        entry = f"--patch-file={src}={dest}"
        if size + len(entry) > CHUNK_BUDGET and cur:
            chunks.append(cur)
            cur, size = [], 0
        cur.append((src, dest))
        size += len(entry) + 1
    if cur:
        chunks.append(cur)
    print(f"split into {len(chunks)} chunks")

    if args.dry_run:
        for i, c in enumerate(chunks):
            print(f"  chunk {i}: {len(c)} files")
        return

    tmp = args.output.with_suffix(".tmp.pck")
    current_base = args.base.resolve()
    final_output = args.output.resolve()
    for i, chunk in enumerate(chunks):
        this_output = final_output if i == len(chunks) - 1 else tmp
        print(f"chunk {i+1}/{len(chunks)}:")
        # Only apply excludes on the first round — once they're gone from the
        # base pck, re-passing them is wasted work.
        round_excludes = excludes if i == 0 else []
        _run_gdre(bin_path, current_base, this_output, args.engine_version,
                  chunk, round_excludes)
        # Next round patches THIS round's output.
        if this_output is tmp:
            current_base = tmp
    # Tidy up
    if tmp.exists():
        tmp.unlink()
    print(f"==> {final_output}")


if __name__ == "__main__":
    main()
