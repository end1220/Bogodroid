#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2025-2026 jenny92-tech <jennyliu90223@gmail.com>
"""
Re-encode Unity AudioClip FSB5 sidecars and rebuild .resource files.

This is the lossy counterpart to audio_stream_patch.py. It is intended for
1 GB handheld targets where Streaming reduces preload RAM but the shipped
audio sidecars are still too large. Each external AudioClip is:

  FSB5 -> Ogg Vorbis -> mono/downsampled WAV -> Ogg Vorbis -> FSB5

Then every affected .resource file is rebuilt, preserving unrelated chunks
and gap bytes, and every later offset is patched back into data.unity3d.

Requirements
------------
    pip install UnityPy fsb5
    brew install ffmpeg vorbis-tools libvorbis
    oggvorbis2fsb5 on PATH, or pass --fsb5-remuxer /path/to/oggvorbis2fsb5
"""

import argparse
import ctypes.util
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

try:
    import UnityPy
except ImportError as e:
    sys.exit(f"missing dependency: {e}\ninstall with:  pip install UnityPy")

try:
    from fsb5 import FSB5, MetadataChunkType
    from fsb5 import utils as fsb5_utils
except ImportError as e:
    sys.exit(f"missing dependency: {e}\ninstall with:  pip install fsb5")


LOAD_STREAMING = 2


def install_homebrew_vorbis_lookup():
    """fsb5 uses ctypes.util.find_library, which may miss /opt/homebrew."""
    orig_find = ctypes.util.find_library
    known = {
        "vorbis": "/opt/homebrew/lib/libvorbis.dylib",
        "vorbisenc": "/opt/homebrew/lib/libvorbisenc.dylib",
        "ogg": "/opt/homebrew/lib/libogg.dylib",
    }

    def find(name):
        found = orig_find(name)
        if found:
            return found
        path = known.get(name)
        if path and Path(path).exists():
            return path
        return None

    fsb5_utils.ctypes.util.find_library = find


def collect_refs(env):
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
    fsize = path.stat().st_size
    refs = sorted(refs, key=lambda r: r["offset"])
    prev_end = 0
    for r in refs:
        end = r["offset"] + r["size"]
        if end > fsize or r["offset"] < prev_end:
            log(f"  LAYOUT FAIL {path.name}: chunk at {r['offset']}+{r['size']} "
                f"(file {fsize}, prev chunk ends {prev_end})", always=True)
            return None
        prev_end = end
    return refs


def run(cmd, **kwargs):
    res = subprocess.run(cmd, capture_output=True, text=True, **kwargs)
    if res.returncode != 0:
        raise RuntimeError(
            f"{' '.join(map(str, cmd))}\n{res.stderr.strip() or res.stdout.strip()}")
    return res


def fsb_loop_args(sample, out_rate):
    loop = sample.metadata.get(MetadataChunkType.LOOP)
    if not loop:
        return []
    scale = out_rate / float(sample.frequency or out_rate)
    start = max(0, int(round(loop[0] * scale)))
    end = max(start, int(round(loop[1] * scale)))
    return [str(start), str(end)]


def reencode_fsb(chunk, name, args, tools, tmpdir):
    fsb = FSB5(chunk)
    if len(fsb.samples) != 1:
        raise RuntimeError(f"{name}: expected 1 FSB sample, got {len(fsb.samples)}")
    sample = fsb.samples[0]

    out_rate = min(sample.frequency, args.max_rate) if args.max_rate else sample.frequency
    out_channels = sample.channels if args.keep_stereo else 1

    base = tmpdir / f"clip_{os.getpid()}_{time.time_ns()}"
    in_ogg = base.with_suffix(".in.ogg")
    wav = base.with_suffix(".wav")
    out_ogg = base.with_suffix(".out.ogg")
    out_fsb = base.with_suffix(".fsb")

    in_ogg.write_bytes(fsb.rebuild_sample(sample))
    run([tools["ffmpeg"], "-hide_banner", "-y", "-v", "error",
         "-i", str(in_ogg), "-ac", str(out_channels), "-ar", str(out_rate),
         "-c:a", "pcm_s16le", str(wav)])
    run([tools["oggenc"], "-Q", "-q", str(args.quality),
         "-o", str(out_ogg), str(wav)])
    run([tools["remuxer"], str(out_ogg), str(out_fsb),
         *fsb_loop_args(sample, out_rate)])

    new = out_fsb.read_bytes()
    new_fsb = FSB5(new)
    new_sample = new_fsb.samples[0]
    info = {
        "old_rate": sample.frequency,
        "old_channels": sample.channels,
        "new_rate": new_sample.frequency,
        "new_channels": new_sample.channels,
        "old_size": len(chunk),
        "new_size": len(new),
    }
    return new, info


def rebuild_resource(path, refs, replacements, out_path):
    data = path.read_bytes()
    out = bytearray()
    pos = 0
    for r in refs:
        out += data[pos:r["offset"]]
        new_bytes = replacements.get(id(r))
        chunk = new_bytes if new_bytes is not None else \
            data[r["offset"]:r["offset"] + r["size"]]
        r["new_offset"], r["new_size"] = len(out), len(chunk)
        out += chunk
        pos = r["offset"] + r["size"]
    out += data[pos:]
    out_path.write_bytes(bytes(out))


def patch_refs(refs, infos):
    for r in refs:
        if "new_offset" not in r:
            continue
        obj, kind = r["obj"], r["kind"]
        if kind == "audio":
            a = obj.read()
            a.m_Resource.m_Offset, a.m_Resource.m_Size = r["new_offset"], r["new_size"]
            info = infos.get(id(r))
            if info:
                a.m_Channels = info["new_channels"]
                a.m_Frequency = info["new_rate"]
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


def find_tools(args):
    tools = {
        "ffmpeg": shutil.which(args.ffmpeg),
        "oggenc": shutil.which(args.oggenc),
        "remuxer": shutil.which(args.fsb5_remuxer)
        if args.fsb5_remuxer == Path(args.fsb5_remuxer).name
        else args.fsb5_remuxer,
    }
    missing = [k for k, v in tools.items() if not v or not Path(v).exists()]
    if missing:
        sys.exit("missing tool(s): " + ", ".join(missing))
    return tools


def main():
    ap = argparse.ArgumentParser(
        description="Re-encode Unity external AudioClip FSB5 resources.")
    ap.add_argument("bundle", help="path to data.unity3d")
    ap.add_argument("--out-dir",
                    help="output directory (default: <bundle_dir>/audio_reencoded)")
    ap.add_argument("--quality", type=float, default=2.0,
                    help="oggenc Vorbis quality, -1..10 (default: 2)")
    ap.add_argument("--max-rate", type=int, default=32000,
                    help="cap sample rate; 0 preserves original (default: 32000)")
    ap.add_argument("--keep-stereo", action="store_true",
                    help="preserve original channel count instead of forcing mono")
    ap.add_argument("--min-secs", type=float, default=0.0,
                    help="only re-encode clips at least this long (default: all)")
    ap.add_argument("--skip", action="append", default=[],
                    help="clip name to leave untouched (repeatable)")
    ap.add_argument("--limit", type=int, default=0,
                    help="re-encode at most N clips (0 = all; for QA)")
    ap.add_argument("--ffmpeg", default="ffmpeg")
    ap.add_argument("--oggenc", default="oggenc")
    ap.add_argument("--fsb5-remuxer", default="oggvorbis2fsb5")
    ap.add_argument("--packer", default="lz4hc",
                    choices=("original", "lz4", "lzma", "lz4hc", "none"),
                    help="bundle block compression on save (default: lz4hc)")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("-q", "--quiet", action="store_true")
    args = ap.parse_args()

    install_homebrew_vorbis_lookup()
    tools = find_tools(args)
    bundle_in = Path(args.bundle).resolve()
    if not bundle_in.is_file():
        sys.exit(f"input not found: {bundle_in}")
    data_dir = bundle_in.parent
    out_dir = Path(args.out_dir).resolve() if args.out_dir else \
        data_dir / "audio_reencoded"
    skip_names = set(args.skip)

    def log(msg, always=False):
        if always or not args.quiet:
            print(msg, flush=True)

    log(f"Loading {bundle_in}", always=True)
    t0 = time.time()
    env = UnityPy.load(str(bundle_in))
    log(f"  loaded in {time.time() - t0:.1f}s", always=True)

    by_source = collect_refs(env)
    plans = {}
    n_planned = n_skip_short = n_skip_list = n_skip_stream = 0
    for src_name, refs in by_source.items():
        audios = [r for r in refs if r["kind"] == "audio"]
        if not audios:
            continue
        path = data_dir / src_name
        if not path.is_file():
            log(f"  MISSING {src_name}", always=True)
            continue
        plan_here = []
        for r in audios:
            a = r["obj"].read()
            if a.m_Name in skip_names:
                n_skip_list += 1
                continue
            if a.m_Length < args.min_secs:
                n_skip_short += 1
                continue
            if a.m_LoadType == LOAD_STREAMING and args.min_secs > 0:
                n_skip_stream += 1
                continue
            plan_here.append(r)
            n_planned += 1
            if args.limit and n_planned >= args.limit:
                break
        if plan_here:
            sorted_refs = verify_layout(path, refs, log)
            if sorted_refs is not None:
                plans[src_name] = (path, sorted_refs, plan_here)
        if args.limit and n_planned >= args.limit:
            break

    log(f"\nPlan: {n_planned} AudioClips in {len(plans)} .resource files "
        f"(skipped: {n_skip_short} short, {n_skip_list} listed, "
        f"{n_skip_stream} already streaming)", always=True)
    if args.dry_run:
        return
    if not plans:
        log("Nothing to do.", always=True)
        return

    out_dir.mkdir(parents=True, exist_ok=True)
    infos = {}
    total_old = total_new = n_done = n_fail = 0
    with tempfile.TemporaryDirectory() as td:
        tmpdir = Path(td)
        for src_name, (path, sorted_refs, plan_here) in plans.items():
            data = path.read_bytes()
            replacements = {}
            for r in plan_here:
                a = r["obj"].read()
                chunk = data[r["offset"]:r["offset"] + r["size"]]
                try:
                    new, info = reencode_fsb(chunk, a.m_Name, args, tools, tmpdir)
                except Exception as e:
                    n_fail += 1
                    log(f"  FAIL {a.m_Name!r}: {e}", always=True)
                    continue
                replacements[id(r)] = new
                infos[id(r)] = info
                total_old += info["old_size"]
                total_new += info["new_size"]
                n_done += 1
                log(f"  {a.m_Name!r}: {info['old_size']/1024:.0f}K "
                    f"{info['old_channels']}ch/{info['old_rate']} -> "
                    f"{info['new_size']/1024:.0f}K "
                    f"{info['new_channels']}ch/{info['new_rate']}")
            rebuild_resource(path, sorted_refs, replacements, out_dir / src_name)
            patch_refs(sorted_refs, infos)
            log(f"  rebuilt {src_name}", always=True)

    log("\nSaving bundle ...", always=True)
    t0 = time.time()
    with open(out_dir / bundle_in.name, "wb") as f:
        packer = (67, 3) if args.packer == "lz4hc" else args.packer
        f.write(env.file.save(packer=packer))
    log(f"  written in {time.time() - t0:.1f}s", always=True)

    log("\n=== Summary ===", always=True)
    log(f"  Re-encoded: {n_done}; failed: {n_fail}", always=True)
    if total_old:
        log(f"  Audio refs: {total_old / 1048576:.1f} -> "
            f"{total_new / 1048576:.1f} MB", always=True)
    log(f"  Output dir: {out_dir}", always=True)


if __name__ == "__main__":
    main()
