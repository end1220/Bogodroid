#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2025-2026 jenny92-tech <jennyliu90223@gmail.com>
"""
Patch long AudioClips to Streaming load type — RAM savings at zero
quality cost.

Step 2 of the pipeline in README.md. Unity's default CompressedInMemory
keeps each clip's full compressed bytes resident in RAM while loaded;
games that preload aggressively can hold tens of MB of audio per scene.
Flipping music/ambience to Streaming leaves the bytes on disk and reads
them through a ~200 KB buffer per playing clip instead. The audio data
itself is untouched, so there is no quality loss and nothing to re-encode.

Selection
---------
A clip is patched when ALL of these hold:
  - current load type is DecompressOnLoad or CompressedInMemory
  - length >= --min-secs (default 10): music and ambience qualify, short
    SFX keep their in-memory latency
  - its data lives in an external .resource file (streaming from inside
    an LZMA bundle would be pathological; such clips are skipped loudly)
  - name not on the --skip list

The patch sets m_LoadType = Streaming and clears m_PreloadAudioData.

Risks / QA
----------
- Layered synced music (e.g. Hollow Knight's Main/Action area loops) may
  drift if buffers underrun on slow SD cards. Listen to one layered area;
  --skip the layer clips if they desync.
- Many simultaneous streams = more file handles + SD I/O. --min-secs high
  enough keeps the count to music/ambience only.

Usage
-----
    python3 tools/unity_astc/audio_stream_patch.py data.unity3d --dry-run
    python3 tools/unity_astc/audio_stream_patch.py data.unity3d
    python3 tools/unity_astc/audio_stream_patch.py data.unity3d --min-secs 20
    python3 tools/unity_astc/audio_stream_patch.py data.unity3d \\
        --skip "S23-11 INSIDE LOOP" --skip "S23-11 OUTSIDE LOOP"

Requirements
------------
    pip install UnityPy
"""

import argparse
import sys
import time
from pathlib import Path

try:
    import UnityPy
except ImportError as e:
    sys.exit(f"missing dependency: {e}\ninstall with:  pip install UnityPy")

LOAD_DECOMPRESS = 0
LOAD_COMPRESSED_IN_MEMORY = 1
LOAD_STREAMING = 2
LOAD_NAMES = {0: "DecompressOnLoad", 1: "CompressedInMemory", 2: "Streaming"}


def parse_skip_file(path):
    """Read a skip-list file: one clip name per line, '#' starts a comment."""
    names = set()
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if line:
                names.add(line)
    return names


def ram_estimate(a):
    """Resident bytes for a loaded clip under its current load type."""
    if a.m_LoadType == LOAD_DECOMPRESS:
        return int(a.m_Length * a.m_Frequency * a.m_Channels * 4)
    if a.m_LoadType == LOAD_COMPRESSED_IN_MEMORY:
        return a.m_Resource.m_Size if a.m_Resource else 0
    return 0


def main():
    ap = argparse.ArgumentParser(
        description="Patch long AudioClips to Streaming load type.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Usage\n-----\n", 1)[1].split("Requirements")[0])
    ap.add_argument("bundle", help="path to data.unity3d (or any Unity bundle)")
    ap.add_argument("-o", "--out", help="output path (default: <bundle>.streamed)")
    ap.add_argument("--min-secs", type=float, default=10.0,
                    help="only patch clips at least this long (default: 10)")
    ap.add_argument("--skip", action="append", default=[],
                    help="clip name to leave untouched (repeatable)")
    ap.add_argument("--skip-file", help="file with one clip name per line")
    ap.add_argument("--limit", type=int, default=0,
                    help="patch at most N clips (0 = all; for QA)")
    ap.add_argument("--packer", default="lz4hc",
                    choices=("original", "lz4", "lzma", "lz4hc", "none"),
                    help="bundle block compression on save (default: lz4hc = UnityFS flags 0x43, the device-proven format). NEVER ship --packer lz4: UnityPy adds the 0x80 padding flag and pre-2020.3 players SIGBUS on it")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the patch plan; don't write")
    ap.add_argument("-q", "--quiet", action="store_true",
                    help="suppress per-clip output")
    args = ap.parse_args()

    bundle_in = Path(args.bundle).resolve()
    if not bundle_in.is_file():
        sys.exit(f"input not found: {bundle_in}")
    bundle_out = Path(args.out).resolve() if args.out else bundle_in.with_suffix(
        bundle_in.suffix + ".streamed")

    skip_names = set(args.skip)
    if args.skip_file:
        skip_names |= parse_skip_file(args.skip_file)

    def log(msg, always=False):
        if always or not args.quiet:
            print(msg, flush=True)

    log(f"Loading {bundle_in}", always=True)
    t0 = time.time()
    env = UnityPy.load(str(bundle_in))
    log(f"  loaded in {time.time() - t0:.1f}s", always=True)

    n_patched = n_short = n_skip = n_inbundle = 0
    ram_freed = 0
    for obj in env.objects:
        if obj.type.name != "AudioClip":
            continue
        a = obj.read()
        if a.m_LoadType == LOAD_STREAMING:
            continue
        if a.m_Name in skip_names:
            n_skip += 1
            log(f"  SKIP [list] {a.m_Name!r}")
            continue
        if a.m_Length < args.min_secs:
            n_short += 1
            continue
        src = a.m_Resource.m_Source if a.m_Resource else ""
        if not src or src.startswith("archive:"):
            n_inbundle += 1
            log(f"  SKIP [in-bundle data] {a.m_Name!r} (source {src!r})", always=True)
            continue

        freed = ram_estimate(a)
        log(f"  {'would patch' if args.dry_run else 'patch'} {a.m_Name!r} "
            f"{a.m_Length:6.0f}s {a.m_Channels}ch "
            f"[{LOAD_NAMES.get(a.m_LoadType, a.m_LoadType)}] "
            f"frees {freed / 1048576:.2f} MB")
        if not args.dry_run:
            a.m_LoadType = LOAD_STREAMING
            a.m_PreloadAudioData = False
            a.save()
        ram_freed += freed
        n_patched += 1
        if args.limit and n_patched >= args.limit:
            log(f"  (limit {args.limit} reached)", always=True)
            break

    log(f"\n=== Summary ===", always=True)
    log(f"  Patched to Streaming: {n_patched}  "
        f"(skipped: {n_short} short, {n_skip} listed, {n_inbundle} in-bundle)",
        always=True)
    log(f"  Est. RAM freed when those clips are loaded: {ram_freed / 1048576:.1f} MB",
        always=True)

    if args.dry_run:
        log("\nDry run — no bundle written.", always=True)
        return
    if not n_patched:
        log("\nNothing to do.", always=True)
        return

    log(f"\nSaving bundle ...", always=True)
    t0 = time.time()
    bundle_out.parent.mkdir(parents=True, exist_ok=True)
    with open(bundle_out, "wb") as f:
        f.write(env.file.save(packer=(67, 3) if args.packer == "lz4hc" else args.packer))
    log(f"  written in {time.time() - t0:.1f}s", always=True)
    log(f"  Output: {bundle_out}", always=True)


if __name__ == "__main__":
    main()
