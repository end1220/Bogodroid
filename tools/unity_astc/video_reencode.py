#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2025-2026 jenny92-tech <jennyliu90223@gmail.com>
"""
Re-encode oversized VideoClips to the device's display height.

Step 4 of the pipeline in README.md. Cutscenes commonly ship as 1080p
H.264 inside sidecar .resource files; open handhelds top out around 720
lines (1280x720 / 960x720 / 720x720), so decoding above that wastes RAM
(YUV frame surfaces) and CPU. This tool extracts each video, re-encodes
it with ffmpeg capped at --max-height, and rebuilds the .resource files.

VideoClip Width/Height metadata is deliberately NOT touched: Unity
allocates from the actual stream, and both reference Hollow Knight packs
ship 1920x1080 metadata over re-encoded streams.

Layout safety
-------------
Videos can share a .resource file with FSB5 audio chunks at explicit
offsets. Before touching a file the tool collects EVERY reference into it
(AudioClip m_Resource, VideoClip m_ExternalResources, Texture2D
m_StreamData), verifies chunks are in-bounds and non-overlapping, and
preserves unreferenced gap bytes verbatim. Files that fail verification
are reported and left alone. After a video shrinks, every later chunk's
offset is re-pointed in the bundle (AudioClips included).

Output goes to --out-dir (default: <bundle_dir>/video_reencoded/),
containing the patched bundle plus only the rebuilt .resource files.
Copy them over the originals after QA.

Usage
-----
    python3 tools/unity_astc/video_reencode.py data.unity3d --dry-run
    python3 tools/unity_astc/video_reencode.py data.unity3d
    python3 tools/unity_astc/video_reencode.py data.unity3d --max-height 480 --crf 30

    # QA the .resource rebuild + offset patching without ffmpeg:
    python3 tools/unity_astc/video_reencode.py data.unity3d --passthrough

Requirements
------------
    pip install UnityPy
    ffmpeg on PATH (not needed for --dry-run / --passthrough)
"""

import argparse
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

try:
    import UnityPy
except ImportError as e:
    sys.exit(f"missing dependency: {e}\ninstall with:  pip install UnityPy")


def stream_dims(mp4_bytes):
    """Actual coded dims from the stsd sample entry; None if not found."""
    for fourcc in (b"avc1", b"hvc1", b"hev1", b"vp09", b"mp4v"):
        i = mp4_bytes.find(fourcc)
        while i != -1:
            try:
                w, h = struct.unpack_from(">HH", mp4_bytes, i + 28)
                if 16 <= w <= 8192 and 16 <= h <= 8192:
                    return w, h
            except struct.error:
                pass
            i = mp4_bytes.find(fourcc, i + 4)
    return None


def collect_refs(env):
    """Every (object, kind, offset, size) referencing an external .resource,
    grouped by source filename."""
    by_source = {}

    def add(obj, kind, src, offset, size):
        if not src or src.startswith("archive:") or size <= 0:
            return
        by_source.setdefault(Path(src).name, []).append(
            {"obj": obj, "kind": kind, "offset": offset, "size": size})

    for obj in env.objects:
        t = obj.type.name
        if t == "AudioClip":
            a = obj.read()
            if a.m_Resource:
                add(obj, "audio", a.m_Resource.m_Source,
                    a.m_Resource.m_Offset, a.m_Resource.m_Size)
        elif t == "VideoClip":
            v = obj.read()
            er = v.m_ExternalResources
            if er:
                add(obj, "video", er.m_Source, er.m_Offset, er.m_Size)
        elif t == "Texture2D":
            x = obj.read(check_read=False)
            sd = x.m_StreamData
            if sd and sd.path:
                add(obj, "texture", sd.path, sd.offset, sd.size)
    return by_source


def verify_layout(path, refs, log):
    """Chunks must be in-bounds and non-overlapping. Returns sorted refs or
    None on failure."""
    fsize = path.stat().st_size
    refs = sorted(refs, key=lambda r: r["offset"])
    prev_end = 0
    for r in refs:
        end = r["offset"] + r["size"]
        if end > fsize or r["offset"] < prev_end:
            log(f"  LAYOUT FAIL {path.name}: chunk at {r['offset']}+{r['size']} "
                f"(file {fsize}, prev chunk ends {prev_end}) — leaving file alone",
                always=True)
            return None
        prev_end = end
    return refs


def ffmpeg_reencode(src_bytes, max_height, crf, preset, log, name):
    with tempfile.TemporaryDirectory() as td:
        fin = Path(td) / "in.mp4"
        fout = Path(td) / "out.mp4"
        fin.write_bytes(src_bytes)
        cmd = ["ffmpeg", "-v", "error", "-y", "-i", str(fin),
               "-vf", f"scale=-2:'min({max_height},ih)'",
               "-c:v", "libx264", "-crf", str(crf), "-preset", preset,
               "-pix_fmt", "yuv420p", "-c:a", "copy",
               "-movflags", "+faststart", str(fout)]
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            log(f"  FAIL {name}: ffmpeg: {res.stderr.strip()[:300]}", always=True)
            return None
        return fout.read_bytes()


def rebuild_resource(path, refs, replacements, out_path):
    """Rewrite the file with replaced chunks, preserving leading/gap/trailing
    bytes. Updates each ref dict with new_offset/new_size in place."""
    data = path.read_bytes()
    out = bytearray()
    pos = 0
    for r in refs:
        out += data[pos:r["offset"]]          # gap (or leading bytes) verbatim
        new_bytes = replacements.get(id(r))
        chunk = new_bytes if new_bytes is not None else \
            data[r["offset"]:r["offset"] + r["size"]]
        r["new_offset"], r["new_size"] = len(out), len(chunk)
        out += chunk
        pos = r["offset"] + r["size"]
    out += data[pos:]                          # trailing bytes verbatim
    out_path.write_bytes(bytes(out))


def patch_refs(refs):
    for r in refs:
        if r["new_offset"] == r["offset"] and r["new_size"] == r["size"]:
            continue
        obj, kind = r["obj"], r["kind"]
        if kind == "audio":
            a = obj.read()
            a.m_Resource.m_Offset, a.m_Resource.m_Size = r["new_offset"], r["new_size"]
            a.save()
        elif kind == "video":
            v = obj.read()
            v.m_ExternalResources.m_Offset = r["new_offset"]
            v.m_ExternalResources.m_Size = r["new_size"]
            v.save()
        elif kind == "texture":
            x = obj.read(check_read=False)
            x.m_StreamData.offset, x.m_StreamData.size = r["new_offset"], r["new_size"]
            x.save()


def main():
    ap = argparse.ArgumentParser(
        description="Re-encode oversized Unity VideoClips for handheld displays.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Usage\n-----\n", 1)[1].split("Requirements")[0])
    ap.add_argument("bundle", help="path to data.unity3d (sidecar .resource "
                                   "files must sit next to it)")
    ap.add_argument("--out-dir",
                    help="output directory (default: <bundle_dir>/video_reencoded)")
    ap.add_argument("--max-height", type=int, default=720,
                    help="cap video height; streams already at or below are "
                         "skipped (default: 720)")
    ap.add_argument("--crf", type=int, default=28, help="x264 CRF (default: 28)")
    ap.add_argument("--preset", default="medium", help="x264 preset (default: medium)")
    ap.add_argument("--skip", action="append", default=[],
                    help="clip name to leave untouched (repeatable)")
    ap.add_argument("--passthrough", action="store_true",
                    help="QA mode: no ffmpeg, copy video bytes unchanged but "
                         "exercise the rebuild/patch path")
    ap.add_argument("--packer", default="lz4hc",
                    choices=("original", "lz4", "lzma", "lz4hc", "none"),
                    help="bundle block compression on save (default: lz4hc = UnityFS flags 0x43, the device-proven format). NEVER ship --packer lz4: UnityPy adds the 0x80 padding flag and pre-2020.3 players SIGBUS on it")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the plan; don't encode or write")
    ap.add_argument("-q", "--quiet", action="store_true",
                    help="suppress per-clip output")
    args = ap.parse_args()

    bundle_in = Path(args.bundle).resolve()
    if not bundle_in.is_file():
        sys.exit(f"input not found: {bundle_in}")
    data_dir = bundle_in.parent
    out_dir = Path(args.out_dir).resolve() if args.out_dir else \
        data_dir / "video_reencoded"

    if not (args.dry_run or args.passthrough) and shutil.which("ffmpeg") is None:
        sys.exit("ffmpeg not found on PATH (install it, e.g. `brew install ffmpeg`, "
                 "or use --dry-run / --passthrough)")

    def log(msg, always=False):
        if always or not args.quiet:
            print(msg, flush=True)

    log(f"Loading {bundle_in}", always=True)
    t0 = time.time()
    env = UnityPy.load(str(bundle_in))
    log(f"  loaded in {time.time() - t0:.1f}s", always=True)

    by_source = collect_refs(env)

    # decide per-video, grouped by .resource file
    n_skip_small = n_skip_list = n_planned = 0
    touched = {}   # source name -> (path, sorted refs, {id(ref): new bytes or None placeholder})
    for src_name, refs in by_source.items():
        videos = [r for r in refs if r["kind"] == "video"]
        if not videos:
            continue
        path = data_dir / src_name
        if not path.is_file():
            log(f"  MISSING {src_name} — referenced but not next to bundle",
                always=True)
            continue
        plan_here = []
        with open(path, "rb") as f:
            for r in videos:
                v = r["obj"].read(check_read=False)
                name = v.m_Name
                f.seek(r["offset"])
                chunk = f.read(r["size"])
                dims = stream_dims(chunk)
                w, h = dims if dims else (getattr(v, "Width", 0), getattr(v, "Height", 0))
                tag = "stream" if dims else "metadata(!)"
                if name in args.skip:
                    n_skip_list += 1
                    log(f"  SKIP [list] {name!r}")
                    continue
                if h <= args.max_height and not args.passthrough:
                    n_skip_small += 1
                    log(f"  SKIP [{tag} {w}x{h} <= cap] {name!r}")
                    continue
                log(f"  {'would re-encode' if args.dry_run else 'plan'} {name!r} "
                    f"{tag} {w}x{h}, {r['size'] / 1048576:.1f} MB, in {src_name}")
                plan_here.append((r, name, chunk))
                n_planned += 1
        if plan_here:
            sorted_refs = verify_layout(path, refs, log)
            if sorted_refs is None:
                n_planned -= len(plan_here)
                continue
            touched[src_name] = (path, sorted_refs, plan_here)

    log(f"\nPlan: {n_planned} videos in {len(touched)} .resource files "
        f"(skipped: {n_skip_small} at/below cap, {n_skip_list} listed)", always=True)
    if args.dry_run or not touched:
        log("Dry run — nothing written." if args.dry_run else "Nothing to do.",
            always=True)
        return

    out_dir.mkdir(parents=True, exist_ok=True)
    for src_name, (path, sorted_refs, plan_here) in touched.items():
        replacements = {}
        for r, name, chunk in plan_here:
            if args.passthrough:
                new_bytes = chunk
            else:
                t0 = time.time()
                new_bytes = ffmpeg_reencode(chunk, args.max_height, args.crf,
                                            args.preset, log, name)
                if new_bytes is None:
                    continue
                log(f"  {name!r}: {len(chunk) / 1048576:.1f} -> "
                    f"{len(new_bytes) / 1048576:.1f} MB in {time.time() - t0:.0f}s")
            replacements[id(r)] = new_bytes
        rebuild_resource(path, sorted_refs, replacements, out_dir / src_name)
        patch_refs(sorted_refs)
        log(f"  rebuilt {src_name}", always=True)

    log(f"\nSaving bundle ...", always=True)
    t0 = time.time()
    with open(out_dir / bundle_in.name, "wb") as f:
        f.write(env.file.save(packer=(67, 3) if args.packer == "lz4hc" else args.packer))
    log(f"  written in {time.time() - t0:.1f}s", always=True)
    log(f"\n=== Summary ===", always=True)
    log(f"  Re-encoded: {n_planned} videos, rebuilt {len(touched)} .resource files",
        always=True)
    log(f"  Output dir: {out_dir}  (copy contents over the originals after QA)",
        always=True)


if __name__ == "__main__":
    main()
