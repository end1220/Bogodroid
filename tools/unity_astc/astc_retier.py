#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2025-2026 jenny92-tech <jennyliu90223@gmail.com>
"""
Re-tier Unity bundle ASTC textures: big textures to a coarser block size,
fonts and small images left untouched.

Companion to shrink_bundle.py, taking the opposite approach: instead of
downscaling (which requires rewriting every Sprite's pixel-space rects and
breaks TMP glyph tables), this tool keeps every texture at its original
resolution and only changes the ASTC block size. Pixel dimensions never
change, so all Sprite/SpriteAtlas/TMP metadata stays valid with zero
rewriting. ASTC 4x4 -> 8x8 cuts both VRAM and (pre-LZMA) disk to 1/4.

Tier policy
-----------
A texture KEEPS its current format when any of these hold:
  - it is a font atlas: name ends in " Atlas" (TextMeshPro naming
    convention) or it is referenced by a legacy Font object's m_Texture
  - its long side is <= --keep-cap (default 256): tiny images save almost
    nothing at 8x8 and show block artifacts the most
  - its long side is >  --max-size (if set): the runtime [gpu]
    textureMaxDim cap will box-downsample it at upload; pre-encoding ASTC
    here would stack two quality losses for no memory win — the runtime
    cap is absolute (e.g. 384x384) and beats any ASTC compression ratio
    on large textures
  - its block size is already >= the target (never re-encode in place:
    ASTC->ASTC at the same size is pure quality loss)
  - its name is on the --skip list
Everything else is decoded and re-encoded at --block (default 8x8).

Non-ASTC formats are left alone by default. --include-raw also converts
big uncompressed RGBA32/RGB24 textures; --include-compressed also converts
ETC2/DXT/BCn/PVRTC sources (the target GPU must support ASTC — Mali-G31+
yes, GLES2-era Mali-400 no).

Division-of-labor strategy (recommended for memory-constrained handhelds)
-----------------------------------------------------------------------
ASTC and the runtime cap compress in different ways:
  - ASTC is RELATIVE: fixed N x compression regardless of input size
  - runtime cap is ABSOLUTE: fixed upper bound regardless of input size

So they fit complementary size ranges:
  - small textures (<= cap): runtime cap never touches them, ASTC is the
    only thing that can shrink them — let ASTC do its job
  - large textures (>  cap): the runtime cap will downsample them to cap x
    cap anyway, so ASTC-encoding them first only adds block artifacts on
    top of the eventual downsample loss

Aligning --max-size with wsm.toml [gpu] textureMaxDim turns the two tools
into a clean division of labor: ASTC owns small, runtime cap owns big,
fonts (R8/Alpha8) are silently exempt by both.

    # If wsm.toml has textureMaxDim = 384, this is the matching call:
    python3 tools/unity_astc/astc_retier.py data.unity3d \\
        --include-raw --keep-cap 0 --max-size 384 \\
        --block 6x6 --compact

(6x6 over the default 8x8: small textures like skill icons / sprite tiles
preserve 1-px edges better; the memory difference vs 8x8 is small enough
to spend on quality.)

What it does NOT do
-------------------
- Touch resolutions or any Sprite/TMP metadata (that is the point).
- Compact the bundle on disk unless --compact is given: re-tiered texture
  data is written inline and the original bytes in the bundle's internal
  .resS sections become unreferenced garbage, so without --compact the
  output is LARGER than the input. Runtime RAM is unaffected either way
  (dead ranges are never read). --compact rebuilds every internal .resS
  keeping only live ranges and re-points all stream references (textures,
  meshes, audio, video), matching what a clean Unity build would ship.

Usage
-----
    # Dry run - print the tier plan, write nothing
    python3 tools/unity_astc/astc_retier.py data.unity3d --dry-run

    # Default: fonts + <=256 px untouched, everything else ASTC 8x8,
    # dead .resS bytes dropped
    python3 tools/unity_astc/astc_retier.py data.unity3d --compact

    # Gentler: 6x6 target, keep up to 512 px untouched
    python3 tools/unity_astc/astc_retier.py data.unity3d --block 6x6 --keep-cap 512

    # Also crush big uncompressed RGBA32 overlays (e.g. touch UI)
    python3 tools/unity_astc/astc_retier.py data.unity3d --include-raw

    # Recommended for 1 GB-class handhelds: split labor with the runtime cap.
    # Set --max-size = wsm.toml [gpu] textureMaxDim. Small textures get ASTC
    # 6x6, big ones are left for the runtime to box-downsample.
    python3 tools/unity_astc/astc_retier.py data.unity3d \\
        --include-raw --keep-cap 0 --max-size 384 --block 6x6 --compact

    # Pixel-art game with mixed sprite sizes: keep small sprites at 6x6 to
    # protect 1-px edges, push large backgrounds/atlases to 8x8 to save
    # ~30% RAM on the part the eye doesn't notice block artifacts on.
    # 768 is the default --small-threshold.
    python3 tools/unity_astc/astc_retier.py data.unity3d \\
        --include-raw --keep-cap 0 --max-size 1280 \\
        --block 8x8 --block-small 6x6 --compact

    # QA: only process the first 10 candidates
    python3 tools/unity_astc/astc_retier.py data.unity3d --limit 10

Requirements
------------
    pip install UnityPy Pillow astc-encoder-py
"""

import argparse
import math
import sys
import time
from pathlib import Path

try:
    import UnityPy
    from UnityPy.enums import TextureFormat as TF
except ImportError as e:
    sys.exit(f"missing dependency: {e}\n"
             "install with:  pip install UnityPy Pillow astc-encoder-py")


COMPRESSION_NAMES = {0: "none", 1: "lzma", 2: "lz4", 3: "lz4hc"}

# bits in BundleFile.dataflags (UnityFS ArchiveFlags) — low 6 bits hold the
# compression id; the rest are modifier flags that affect on-disk layout
DATAFLAG_BITS = [
    (0x40,  "BlocksAndDirectoryInfoCombined"),
    (0x80,  "BlocksInfoAtTheEnd"),
    (0x100, "OldWebPluginCompatibility"),
    (0x200, "BlockInfoNeedPaddingAtStart"),
]


def decode_dataflags(flags):
    """(compression_name, [modifier_bit_names]) for a UnityFS dataflags int."""
    comp = flags & 0x3F
    name = COMPRESSION_NAMES.get(comp, f"unknown({comp})")
    bits = [n for mask, n in DATAFLAG_BITS if flags & mask]
    return name, bits


def print_bundle_info(env, log):
    """Dump everything that affects how we should repack: container kind,
    Unity engine version, raw flags + decoded modifier bits, and the file
    entries inside. Drives both human inspection and the --packer recommendation."""
    f = env.file
    log(f"  signature:        {f.signature}")
    log(f"  header version:   {f.version}")
    log(f"  engine version:   {f.version_engine}")
    log(f"  player version:   {f.version_player}")
    df = int(f.dataflags)
    name, bits = decode_dataflags(df)
    bit_str = (", " + ", ".join(bits)) if bits else ""
    log(f"  dataflags:        0x{df:x} ({df}) -> compression={name}{bit_str}")
    log(f"  files in archive:")
    for n, fl in f.files.items():
        try:
            sz = len(fl.bytes)
        except Exception:
            sz = 0
        flg = getattr(fl, "flags", None)
        log(f"    - {n}  ({sz / 1048576:.1f} MB, flags={flg})")
    # safety advice: --packer original preserves dataflags exactly; --packer lz4
    # is the one to avoid (UnityPy emits the 0x80 variant that pre-2020.3 players
    # SIGBUS on). If the source already uses padding bits, "original" is the
    # only way to round-trip them without writing them out by hand.
    log(f"  recommended --packer: original  (preserves source dataflags exactly)")


def parse_skip_file(path):
    """Read a skip-list file: one texture name per line, '#' starts a comment."""
    names = set()
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if line:
                names.add(line)
    return names


def parse_block(s):
    try:
        bx, by = (int(v) for v in s.lower().split("x"))
    except ValueError:
        raise argparse.ArgumentTypeError(f"invalid block size {s!r}, expected e.g. 8x8")
    if not hasattr(TF, f"ASTC_RGB_{bx}x{by}"):
        valid = sorted(t.name[len("ASTC_RGB_"):] for t in TF
                       if t.name.startswith("ASTC_RGB_"))
        raise argparse.ArgumentTypeError(f"unsupported block {s!r}, one of: {', '.join(valid)}")
    return bx, by


def astc_block_of(fmt):
    """(bx, by) if fmt is an LDR ASTC format, else None."""
    name = TF(fmt).name
    if name.startswith(("ASTC_RGB_", "ASTC_RGBA_")):
        bx, by = name.rsplit("_", 1)[1].split("x")
        return int(bx), int(by)
    return None


def astc_bytes(w, h, block, mips):
    bx, by = block
    total = 0
    for _ in range(max(1, mips)):
        total += math.ceil(w / bx) * math.ceil(h / by) * 16
        w, h = max(1, w // 2), max(1, h // 2)
    return total


def texture_key(obj):
    return (obj.assets_file.name, obj.path_id)


def collect_font_textures(env, log):
    """Keys of textures referenced by legacy Font objects' m_Texture."""
    keys = set()
    for obj in env.objects:
        if obj.type.name != "Font":
            continue
        try:
            d = obj.read()
        except Exception:
            continue
        tex = getattr(d, "m_Texture", None)
        pid = getattr(tex, "path_id", 0) or getattr(tex, "m_PathID", 0)
        if not pid:
            continue
        fid = getattr(tex, "file_id", 0) or getattr(tex, "m_FileID", 0)
        af = obj.assets_file
        if fid == 0:
            keys.add((af.name, pid))
        else:
            try:
                keys.add((af.externals[fid - 1].path, pid))
            except Exception:
                pass
        log(f"  font {getattr(d, 'm_Name', '?')!r} -> texture pid {pid}")
    return keys


def is_font_name(name):
    return name.endswith(" Atlas")


RAW_FORMATS = (TF.RGBA32, TF.RGB24, TF.BGRA32, TF.ARGB32)
# non-ASTC block-compressed color formats convertible to ASTC; the target
# GPU must support ASTC (Mali-G31+/Adreno yes, GLES2-era Mali-400 NO)
COMPRESSED_PREFIXES = ("ETC", "DXT", "BC", "PVRTC")


def plan(env, args, font_keys, log):
    """Classify every Texture2D.
    Returns (candidates, all parsed textures, kept_count, est MB pair).
    The parsed instances are reused by --compact: UnityPy's read() always
    re-parses the ORIGINAL bytes, so re-reading a modified object would
    silently expose (and on save, restore) its pre-edit state."""
    candidates = []
    all_texs = []
    n_keep = 0
    est_before = est_after = 0
    skip_logged = 0
    for obj in env.objects:
        if obj.type.name != "Texture2D":
            continue
        try:
            t = obj.read()
        except Exception:
            continue
        all_texs.append(t)
        name, w, h = t.m_Name, t.m_Width, t.m_Height
        mips = t.m_MipCount or 1
        block = astc_block_of(t.m_TextureFormat)

        # pick target block per-texture: --block-small kicks in for small
        # sprites when set, --block for everything else
        chosen_block = (args.block_small
                        if (args.block_small is not None
                            and max(w, h) <= args.small_threshold)
                        else args.block)

        if block is not None:
            if block[0] * block[1] >= chosen_block[0] * chosen_block[1]:
                continue
            src_bytes = astc_bytes(w, h, block, mips)
        elif t.m_TextureFormat in RAW_FORMATS:
            if not args.include_raw:
                continue
            src_bytes = w * h * (3 if t.m_TextureFormat == TF.RGB24 else 4)
        elif TF(t.m_TextureFormat).name.startswith(COMPRESSED_PREFIXES):
            if not args.include_compressed:
                continue
            src_bytes = t.m_CompleteImageSize or 0
        else:
            continue

        reason = None
        if name in args.skip_names:
            reason = "skip-list"
        elif is_font_name(name) or texture_key(obj) in font_keys:
            reason = "font"
        elif max(w, h) <= args.keep_cap:
            reason = "small"
        elif args.max_size > 0 and max(w, h) > args.max_size:
            # Long-side beyond runtime cap — let the runtime box-downsample
            # do the work. Pre-encoding ASTC here would just add a second
            # quality loss on top of the downsample.
            reason = "big-for-runtime-cap"
        if reason:
            n_keep += 1
            if reason != "small" or skip_logged < 8:
                log(f"  KEEP [{reason}] {name!r} {w}x{h}")
                skip_logged += reason == "small"
            continue

        dst_bytes = astc_bytes(w, h, chosen_block, mips)
        est_before += src_bytes
        est_after += dst_bytes
        candidates.append((obj, t, src_bytes, dst_bytes, chosen_block))
    return candidates, all_texs, n_keep, est_before, est_after


def retier(candidates, dry_run, log, jobs=1):
    """Decode on the main thread (shared .resS readers are not seek-safe),
    encode in a thread pool (astc_encoder releases the GIL; UnityPy keeps
    one ASTC context per thread), save on the main thread (shared
    SerializedFile state). Each candidate carries its own target block
    (set in plan() based on --block / --block-small), so different
    textures can land at different block sizes in the same run."""
    import concurrent.futures as cf

    n_ok = n_fail = 0
    n_done = 0
    t_start = time.time()
    total = len(candidates)

    def progress():
        elapsed = time.time() - t_start
        rate = n_done / elapsed if elapsed > 0 else 0
        eta = (total - n_done) / rate if rate > 0 else 0
        log(f"  [{n_done:>4}/{total}] ok={n_ok} fail={n_fail}  "
            f"rate={rate:.1f}/s  eta={eta:.0f}s", always=True)

    def encode(t, img, block):
        bx, by = block
        target = getattr(TF, f"ASTC_RGB_{bx}x{by}")
        t.set_image(img, target_format=target, mipmap_count=t.m_MipCount or 1)
        return t

    def finish(fut, t):
        nonlocal n_ok, n_fail, n_done
        n_done += 1
        try:
            fut.result().save()
            n_ok += 1
        except Exception as e:
            n_fail += 1
            if n_fail <= 5:
                log(f"  FAIL {t.m_Name!r}: {e}", always=True)
        if n_done % 20 == 0 or n_done == total:
            progress()

    if dry_run:
        for obj, t, src, dst, block in candidates:
            bx, by = block
            log(f"  would re-tier {t.m_Name!r} {t.m_Width}x{t.m_Height} "
                f"{TF(t.m_TextureFormat).name} -> {bx}x{by} "
                f"({src / 1048576:.2f} -> {dst / 1048576:.2f} MB)")
        return 0, 0

    with cf.ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
        in_flight = []
        for obj, t, src, dst, block in candidates:
            try:
                img = t.image
            except Exception as e:
                n_fail += 1
                n_done += 1
                if n_fail <= 5:
                    log(f"  FAIL {t.m_Name!r}: {e}", always=True)
                continue
            if img is None or img.size == (0, 0):
                n_fail += 1
                n_done += 1
                continue
            in_flight.append((pool.submit(encode, t, img, block), t))
            # bound decoded-image memory: drain oldest once the window fills
            while len(in_flight) >= max(2, jobs * 2):
                fut, ft = in_flight.pop(0)
                finish(fut, ft)
        for fut, ft in in_flight:
            finish(fut, ft)
    return n_ok, n_fail


# types whose objects can hold a StreamingInfo/StreamedResource pointing
# into the bundle's internal .resS files
STREAM_FIELDS = {
    "Texture2D": "m_StreamData", "Texture2DArray": "m_StreamData",
    "Texture3D": "m_StreamData", "Cubemap": "m_StreamData",
    "CubemapArray": "m_StreamData", "Mesh": "m_StreamData",
    "AudioClip": "m_Resource", "VideoClip": "m_ExternalResources",
}


def _stream_ref(d, field):
    """Normalized (path, offset, size, setter) for a stream field, or None."""
    info = getattr(d, field, None)
    if info is None:
        return None
    if field == "m_StreamData":
        path, offset, size = info.path, info.offset, info.size

        def setter(o):
            info.offset = o
    else:
        path, offset, size = info.m_Source, info.m_Offset, info.m_Size

        def setter(o):
            info.m_Offset = o
    if not path or size <= 0:
        return None
    return path, offset, size, setter


def compact_stream_files(env, texture_instances, log):
    """Rebuild internal .resS entries keeping only referenced ranges; re-point
    every live stream reference. Texture2D refs come from the caller's parsed
    instances (re-reading would expose pre-edit state); every other type is
    never modified by this tool, so a fresh read is safe.
    Returns (bytes_before, bytes_after)."""
    from UnityPy.streams import EndianBinaryReader

    bundle = env.file
    entries = {name: f for name, f in bundle.files.items()
               if name.endswith(".resS") and isinstance(f, EndianBinaryReader)}

    def entry_for(path):
        # EXACT basename match only. Loose stem-based fallbacks (the way
        # UnityPy's ResourceReader builds candidates) would map EXTERNAL
        # refs like "sharedassetsN.resource" onto the internal
        # "sharedassetsN.assets.resS" entry and corrupt their offsets.
        base = Path(path).name
        return base if base in entries else None

    # live references grouped by .resS entry
    refs = {}

    def add_ref(d, field):
        r = _stream_ref(d, field)
        if r is None:
            return
        path, offset, size, setter = r
        name = entry_for(path)
        if name is None:
            return   # external .resource or unknown — not ours to rebuild
        refs.setdefault(name, []).append((offset, size, d, setter))

    for t in texture_instances:
        add_ref(t, "m_StreamData")
    # entries historically owned by textures: discovered via fresh reads,
    # which parse the ORIGINAL bytes — exactly what we want here (pre-edit
    # paths of re-tiered textures), and never saved back
    known_tex_entries = set()
    for obj in env.objects:
        field = STREAM_FIELDS.get(obj.type.name)
        if field is None:
            continue
        try:
            d = obj.read(check_read=False)
        except Exception:
            continue
        if obj.type.name == "Texture2D":
            r = _stream_ref(d, field)
            if r:
                name = entry_for(r[0])
                if name:
                    known_tex_entries.add(name)
            continue
        add_ref(d, field)

    before = after = 0
    n_rebuilt = n_dropped = n_unknown = 0
    for name, f in entries.items():
        old_size = len(f.bytes)
        before += old_size
        live = refs.get(name)
        if not live and name not in known_tex_entries:
            # no live refs AND never texture-owned: an asset type this tool
            # doesn't track may use it — leave it alone
            after += old_size
            n_unknown += 1
            continue
        if not live:
            # remove the node entirely — a zero-length file entry is a
            # structure no real Unity build ships, and the player segfaults
            # (null deref) on it while mounting the archive (TrimUI, 2020.2)
            del bundle.files[name]
            n_dropped += 1
            continue
        # unique ranges, validated, in offset order
        ranges = sorted({(o, s) for o, s, _, _ in live})
        ok = all(o >= 0 and o + s <= old_size for o, s in ranges)
        if not ok:
            log(f"  COMPACT SKIP {name}: reference outside file bounds",
                always=True)
            after += old_size
            continue
        data = f.bytes
        out = bytearray()
        new_offset = {}
        for o, s in ranges:
            if len(out) % 16:
                out += b"\0" * (16 - len(out) % 16)
            new_offset[(o, s)] = len(out)
            out += data[o:o + s]
        for o, s, d, setter in live:
            if new_offset[(o, s)] != o:
                setter(new_offset[(o, s)])
                d.save()
        bundle.files[name] = nf = EndianBinaryReader(bytes(out))
        nf.flags = f.flags
        after += len(out)
        n_rebuilt += 1
    log(f"  compacted {n_rebuilt} .resS files, emptied {n_dropped}, "
        f"left {n_unknown} untracked: "
        f"{before / 1048576:.0f} -> {after / 1048576:.0f} MB stream data",
        always=True)
    return before, after


def main():
    ap = argparse.ArgumentParser(
        description="Re-tier Unity ASTC textures to a coarser block size, "
                    "keeping fonts and small images untouched.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Usage\n-----\n", 1)[1].split("Requirements")[0])
    ap.add_argument("bundle", help="path to data.unity3d (or any Unity bundle)")
    ap.add_argument("-o", "--out",
                    help="output path (default: ./output/<bundle name>)")
    ap.add_argument("--block", type=parse_block, default=(8, 8),
                    help="target ASTC block size for everything that isn't "
                         "matched by --block-small (default: 8x8)")
    ap.add_argument("--block-small", type=parse_block, default=None,
                    help="optional smaller block for textures with long side "
                         "<= --small-threshold (default: not set, i.e. every "
                         "texture uses --block). Set this to 6x6 for pixel-art "
                         "games to protect small sprites' 1-px edges while "
                         "still letting large textures land at the more "
                         "aggressive --block.")
    ap.add_argument("--small-threshold", type=int, default=768,
                    help="long-side threshold under which --block-small "
                         "applies (default: 768). Only meaningful when "
                         "--block-small is set.")
    ap.add_argument("--keep-cap", type=int, default=256,
                    help="leave textures whose long side is <= this untouched "
                         "(default: 256)")
    ap.add_argument("--max-size", type=int, default=0,
                    help="leave textures whose long side is > this untouched, "
                         "letting the runtime [gpu] textureMaxDim cap "
                         "box-downsample them at upload time (default: 0 = no "
                         "upper bound). Set to match wsm.toml textureMaxDim so "
                         "ASTC only compresses the size range the runtime "
                         "won't touch — pre-encoding ASTC for textures the "
                         "runtime is going to downsample anyway just stacks "
                         "two quality losses.")
    ap.add_argument("--include-raw", action="store_true",
                    help="also convert big uncompressed RGBA32/RGB24 textures")
    ap.add_argument("--include-compressed", action="store_true",
                    help="also convert non-ASTC compressed sources (ETC2/DXT/"
                         "BCn/PVRTC) to the target ASTC block; requires the "
                         "device GPU to support ASTC (not GLES2-era Mali-400)")
    ap.add_argument("--compact", action="store_true",
                    help="rebuild internal .resS sections dropping the dead "
                         "bytes left by re-tiered textures (output shrinks "
                         "instead of growing; recommended for full runs)")
    ap.add_argument("--skip", action="append", default=[],
                    help="texture name to leave untouched (repeatable)")
    ap.add_argument("--skip-file", help="file with one texture name per line")
    ap.add_argument("--limit", type=int, default=0,
                    help="process at most N candidates (0 = all; for QA)")
    ap.add_argument("--jobs", type=int, default=1,
                    help="parallel encode threads (encode scales ~linearly; "
                         "try cores-2)")
    ap.add_argument("--packer", default="lz4hc",
                    choices=("original", "lz4", "lzma", "lz4hc", "none"),
                    help="bundle block compression on save (default: lz4hc = "
                         "UnityFS flags 0x43, the device-proven format). NEVER "
                         "ship --packer lz4: UnityPy adds the 0x80 padding flag "
                         "and pre-2020.3 players SIGBUS on it")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the tier plan; don't encode or write")
    ap.add_argument("-q", "--quiet", action="store_true",
                    help="suppress per-texture/per-batch progress")
    args = ap.parse_args()

    bundle_in = Path(args.bundle).resolve()
    if not bundle_in.is_file():
        sys.exit(f"input not found: {bundle_in}")
    bundle_out = (Path(args.out).resolve() if args.out
                  else Path.cwd() / "output" / bundle_in.name)
    if bundle_out == bundle_in:
        sys.exit("output path equals input; pass -o to write elsewhere")

    args.skip_names = set(args.skip)
    if args.skip_file:
        args.skip_names |= parse_skip_file(args.skip_file)

    def log(msg, always=False):
        if always or not args.quiet or msg.startswith(("\n", "===", "  Pass", "  FAIL")):
            print(msg, flush=True)

    log(f"Loading {bundle_in}")
    t0 = time.time()
    env = UnityPy.load(str(bundle_in))
    log(f"  loaded in {time.time() - t0:.1f}s")

    log("\nBundle header:")
    print_bundle_info(env, log)

    log("\nPass 1: collecting font textures")
    font_keys = collect_font_textures(env, log)

    bx, by = args.block
    bound_str = f", max-size {args.max_size}" if args.max_size > 0 else ""
    if args.block_small is not None:
        sbx, sby = args.block_small
        block_str = f"ASTC {bx}x{by} (large) + {sbx}x{sby} (long-side ≤ {args.small_threshold})"
    else:
        block_str = f"ASTC {bx}x{by}"
    log(f"\nPass 2: planning (target {block_str}, keep-cap {args.keep_cap}{bound_str})")
    candidates, all_texs, n_keep, est_before, est_after = plan(env, args, font_keys, log)
    if args.limit:
        candidates = candidates[:args.limit]
    log(f"  {len(candidates)} to re-tier, {n_keep} kept  "
        f"(est. texture data {est_before / 1048576:.0f} -> "
        f"{est_after / 1048576:.0f} MB)")
    if not candidates:
        log("\nNothing to do.")
        return

    log(f"\nPass 3: {'plan' if args.dry_run else 'encoding'}"
        + (f" ({args.jobs} threads)" if args.jobs > 1 else ""))
    n_ok, n_fail = retier(candidates, args.dry_run, log, args.jobs)

    if args.dry_run:
        if args.compact:
            log("(--compact runs after encoding; nothing to show in dry-run)")
        log("\nDry run — no bundle written.")
        return

    if args.compact:
        log("\nPass 4: compacting internal .resS stream files", always=True)
        compact_stream_files(env, all_texs, log)

    log("\nSaving bundle ...")
    t0 = time.time()
    bundle_out.parent.mkdir(parents=True, exist_ok=True)
    with open(bundle_out, "wb") as f:
        f.write(env.file.save(packer=(67, 3) if args.packer == "lz4hc" else args.packer))
    log(f"  written in {time.time() - t0:.1f}s")

    in_mb = bundle_in.stat().st_size / 1024 / 1024
    out_mb = bundle_out.stat().st_size / 1024 / 1024
    log("\n=== Summary ===")
    log(f"  Target block:    ASTC {bx}x{by}")
    log(f"  Re-tiered:       {n_ok} ({n_fail} failed)")
    log(f"  Kept untouched:  {n_keep} (fonts/small/skip-list)")
    log(f"  Input bundle:    {in_mb:.1f} MB -> Output: {out_mb:.1f} MB ({out_mb - in_mb:+.1f} MB)")
    log(f"  Output: {bundle_out}")


if __name__ == "__main__":
    main()
