#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2025-2026 jenny92-tech <jennyliu90223@gmail.com>
"""
Validate Unity AudioClip references to external .resource sidecars.

This catches the broken-package class where data.unity3d contains AudioClip
offset/size metadata from one resource pack, but the deployed .resource files
come from another. FMOD then seeks into the wrong bytes and reports
"Unsupported file or audio format".

The tool does not re-encode audio. It is a guardrail for packaging and
retier/audio-streaming pipelines.

Usage
-----
    python3 tools/unity_astc/audio_resource_validate.py data.unity3d
    python3 tools/unity_astc/audio_resource_validate.py output/data.unity3d \\
        --resource-dir gamedata/assets/bin/Data
"""

import argparse
import sys
import time
from dataclasses import dataclass
from pathlib import Path

try:
    import UnityPy
except ImportError as e:
    sys.exit(f"missing dependency: {e}\ninstall with:  pip install UnityPy")


DEFAULT_AUDIO_HEADERS = {
    b"FSB5": "FSB5",
    b"OggS": "Ogg",
    b"RIFF": "RIFF/WAV",
    b"fLaC": "FLAC",
}


@dataclass(frozen=True)
class AudioRef:
    clip_name: str
    source: str
    offset: int
    size: int


def _source_path(resource_dir, source):
    return Path(resource_dir) / Path(source).name


def validate_ref(resource_dir, ref, headers=DEFAULT_AUDIO_HEADERS):
    problems = []
    source = ref.source or ""
    if not source or source.startswith("archive:"):
        return problems

    path = _source_path(resource_dir, source)
    label = f"{ref.clip_name!r} -> {Path(source).name}:{ref.offset}+{ref.size}"
    if not path.is_file():
        return [f"{label}: missing resource file {path}"]
    if ref.offset < 0 or ref.size <= 0:
        return [f"{label}: invalid offset/size"]

    fsize = path.stat().st_size
    end = ref.offset + ref.size
    if end > fsize:
        return [f"{label}: out of bounds (file size {fsize})"]

    max_header = max(len(h) for h in headers)
    with open(path, "rb") as f:
        f.seek(ref.offset)
        head = f.read(max_header)

    if not any(head.startswith(h) for h in headers):
        shown = head[:16].hex()
        problems.append(f"{label}: invalid audio header at offset "
                        f"(got {shown or 'empty'})")
    return problems


def collect_audio_refs(bundle):
    env = UnityPy.load(str(bundle))
    refs = []
    for obj in env.objects:
        if obj.type.name != "AudioClip":
            continue
        try:
            clip = obj.read()
        except Exception as e:
            refs.append(AudioRef(f"<unreadable:{e}>", "", 0, 0))
            continue
        res = getattr(clip, "m_Resource", None)
        if not res:
            continue
        source = getattr(res, "m_Source", "") or ""
        if not source or source.startswith("archive:"):
            continue
        refs.append(AudioRef(
            getattr(clip, "m_Name", "<unnamed>"),
            source,
            int(getattr(res, "m_Offset", 0)),
            int(getattr(res, "m_Size", 0)),
        ))
    return refs


def main():
    ap = argparse.ArgumentParser(
        description="Validate external AudioClip .resource references.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Usage\n-----\n", 1)[1])
    ap.add_argument("bundle", help="path to data.unity3d or another Unity bundle")
    ap.add_argument("--resource-dir",
                    help="directory containing sidecar .resource files "
                         "(default: bundle directory)")
    ap.add_argument("--max-errors", type=int, default=25,
                    help="stop printing after N errors (default: 25; 0 = all)")
    ap.add_argument("-q", "--quiet", action="store_true",
                    help="only print the summary")
    args = ap.parse_args()

    bundle = Path(args.bundle).resolve()
    if not bundle.is_file():
        sys.exit(f"input not found: {bundle}")
    resource_dir = Path(args.resource_dir).resolve() if args.resource_dir else bundle.parent
    if not resource_dir.is_dir():
        sys.exit(f"resource dir not found: {resource_dir}")

    print(f"Loading {bundle}", flush=True)
    t0 = time.time()
    refs = collect_audio_refs(bundle)
    print(f"  loaded in {time.time() - t0:.1f}s; "
          f"{len(refs)} external AudioClip ref(s)", flush=True)

    problems = []
    printed = 0
    for ref in refs:
        ref_problems = validate_ref(resource_dir, ref)
        if ref_problems:
            problems.extend(ref_problems)
            if not args.quiet:
                for problem in ref_problems:
                    if args.max_errors and printed >= args.max_errors:
                        continue
                    print(f"  FAIL {problem}", flush=True)
                    printed += 1

    if problems:
        if args.max_errors and len(problems) > printed:
            print(f"  ... stopped after {args.max_errors} error(s)", flush=True)
        print(f"\nAudio resource validation FAILED: {len(problems)} problem(s)",
              flush=True)
        sys.exit(1)

    print("\nAudio resource validation OK", flush=True)


if __name__ == "__main__":
    main()
