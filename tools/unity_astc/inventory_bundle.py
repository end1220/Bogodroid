#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Inventory Unity AssetBundle contents by type + estimated resident memory.

Read-only. Prefer this over guessing from disk size (UnityPy / Addressables
bundles often inflate on disk after rewrite).

Texture VRAM uses format-aware estimate (ASTC/ETC/RGBA). Mesh uses
approx vertex+index bytes. Audio uses m_Size / stream info when present.
Other types are counted only (metadata weight is usually small vs textures).

Usage
-----
    python tools/unity_astc/inventory_bundle.py path/to/a.bundle
    python tools/unity_astc/inventory_bundle.py path/to/dir --glob "*.bundle"
    python tools/unity_astc/inventory_bundle.py a.bundle --top 30
"""

from __future__ import annotations

import argparse
import math
import sys
from collections import defaultdict
from pathlib import Path

try:
    import UnityPy
    from UnityPy.enums import TextureFormat as TF
except ImportError as e:
    sys.exit(f"missing dependency: {e}\ninstall: pip install UnityPy")

MB = 1048576.0


def astc_block(fmt) -> tuple[int, int] | None:
    try:
        name = TF(fmt).name
    except Exception:
        return None
    if name.startswith(("ASTC_RGB_", "ASTC_RGBA_")):
        bx, by = name.rsplit("_", 1)[1].split("x")
        return int(bx), int(by)
    return None


def tex_vram_bytes(w: int, h: int, fmt, mips: int) -> int:
    """Resident GPU bytes (UMA-relevant), not disk blob size."""
    mips = max(1, int(mips or 1))
    block = astc_block(fmt)
    if block:
        bx, by = block
        total = 0
        cw, ch = w, h
        for _ in range(mips):
            total += math.ceil(cw / bx) * math.ceil(ch / by) * 16
            cw, ch = max(1, cw // 2), max(1, ch // 2)
        return total
    try:
        name = TF(fmt).name
    except Exception:
        name = str(fmt)
    # rough bpp / compressed block heuristics
    if name in ("RGBA32", "ARGB32", "BGRA32"):
        bpp = 4
    elif name in ("RGB24",):
        bpp = 3
    elif name in ("RGB565", "RGBA4444", "ARGB4444"):
        bpp = 2
    elif name.startswith(("DXT1", "BC1", "ETC_RGB4", "ETC2_RGB")):
        # 4x4 block, 8 bytes
        total = 0
        cw, ch = w, h
        for _ in range(mips):
            total += math.ceil(cw / 4) * math.ceil(ch / 4) * 8
            cw, ch = max(1, cw // 2), max(1, ch // 2)
        return total
    elif name.startswith(("DXT5", "BC3", "BC7", "ETC2_RGBA", "EAC")):
        total = 0
        cw, ch = w, h
        for _ in range(mips):
            total += math.ceil(cw / 4) * math.ceil(ch / 4) * 16
            cw, ch = max(1, cw // 2), max(1, ch // 2)
        return total
    else:
        bpp = 4  # unknown -> RGBA32 upper bound
    total = 0
    cw, ch = w, h
    for _ in range(mips):
        total += cw * ch * bpp
        cw, ch = max(1, cw // 2), max(1, ch // 2)
    return total


def mesh_bytes(tree: dict) -> int:
    """Very rough: vertex float count * 4 + index bytes."""
    n = 0
    vb = tree.get("m_VertexData") or {}
    if isinstance(vb, dict):
        vd = vb.get("m_DataSize") or vb.get("m_VertexCount")
        if isinstance(vd, int) and "m_DataSize" in vb:
            n += int(vb["m_DataSize"])
        elif isinstance(vb.get("m_VertexCount"), int):
            # assume ~32 bytes/vert if layout unknown
            n += int(vb["m_VertexCount"]) * 32
    ib = tree.get("m_IndexBuffer")
    if isinstance(ib, (bytes, bytearray, list)):
        n += len(ib)
    return n


def audio_bytes(obj) -> int:
    try:
        a = obj.read()
        size = getattr(a, "m_Size", None) or getattr(a, "m_Resource", None)
        if isinstance(size, int):
            return size
        # StreamingInfo
        res = getattr(a, "m_Resource", None)
        if res is not None and hasattr(res, "m_Size"):
            return int(res.m_Size)
        tree = obj.read_typetree()
        si = tree.get("m_Resource") or tree.get("m_AudioData")
        if isinstance(si, dict) and "m_Size" in si:
            return int(si["m_Size"])
        if isinstance(si, (bytes, bytearray, list)):
            return len(si)
    except Exception:
        pass
    return 0


TYPE_BUCKET = {
    "Texture2D": "texture",
    "Texture3D": "texture",
    "Cubemap": "texture",
    "RenderTexture": "texture",
    "Sprite": "sprite_meta",
    "SpriteAtlas": "sprite_meta",
    "Mesh": "mesh",
    "MeshFilter": "mesh_ref",
    "MeshRenderer": "mesh_ref",
    "SkinnedMeshRenderer": "mesh_ref",
    "AudioClip": "audio",
    "AudioSource": "audio_ref",
    "ParticleSystem": "particle",
    "ParticleSystemRenderer": "particle",
    "Animator": "anim",
    "AnimatorController": "anim",
    "AnimationClip": "anim",
    "RuntimeAnimatorController": "anim",
    "Font": "font",
    "TextAsset": "text",
    "MonoBehaviour": "mono",
    "GameObject": "scene_obj",
    "Transform": "scene_obj",
    "RectTransform": "scene_obj",
    "Material": "material",
    "Shader": "shader",
    "ShaderVariantCollection": "shader",
    "ComputeShader": "shader",
    "AssetBundle": "bundle_meta",
    "AssetBundleManifest": "bundle_meta",
}


def bucket_of(type_name: str) -> str:
    return TYPE_BUCKET.get(type_name, "other")


def scan_file(path: Path, top_n: int):
    env = UnityPy.load(str(path))
    by_type = defaultdict(lambda: {"count": 0, "bytes": 0})
    by_bucket = defaultdict(lambda: {"count": 0, "bytes": 0})
    tex_rows = []

    for obj in env.objects:
        tname = obj.type.name
        b = bucket_of(tname)
        est = 0
        name = ""
        try:
            if tname == "Texture2D":
                tex = obj.read()
                w, h = int(tex.m_Width), int(tex.m_Height)
                mips = int(getattr(tex, "m_MipCount", 1) or 1)
                fmt = tex.m_TextureFormat
                est = tex_vram_bytes(w, h, fmt, mips)
                try:
                    fmt_name = TF(fmt).name
                except Exception:
                    fmt_name = str(fmt)
                name = getattr(tex, "m_Name", "") or ""
                tex_rows.append((est, name, w, h, fmt_name, mips))
            elif tname == "Mesh":
                tree = obj.read_typetree()
                est = mesh_bytes(tree)
                name = tree.get("m_Name", "") or ""
            elif tname == "AudioClip":
                est = audio_bytes(obj)
                try:
                    name = obj.read().m_Name
                except Exception:
                    name = ""
            else:
                # count only; optional tiny typetree name
                try:
                    tree = obj.read_typetree()
                    name = tree.get("m_Name", "") or ""
                except Exception:
                    pass
        except Exception:
            pass

        by_type[tname]["count"] += 1
        by_type[tname]["bytes"] += est
        by_bucket[b]["count"] += 1
        by_bucket[b]["bytes"] += est

    return {
        "path": path,
        "disk": path.stat().st_size,
        "by_type": dict(by_type),
        "by_bucket": dict(by_bucket),
        "tex_rows": sorted(tex_rows, reverse=True)[:top_n],
        "tex_total": sum(r[0] for r in tex_rows),
    }


def print_report(rep: dict, top_n: int):
    path = rep["path"]
    disk = rep["disk"]
    print(f"\n=== {path.name} ===")
    print(f"disk={disk / MB:.1f} MB  path={path}")

    buckets = sorted(rep["by_bucket"].items(), key=lambda x: -x[1]["bytes"])
    tex_b = rep["by_bucket"].get("texture", {"bytes": 0})["bytes"]
    est_total = sum(v["bytes"] for v in rep["by_bucket"].values())
    print("\nBy category (estimated resident bytes; textures = GPU VRAM):")
    print(f"{'bucket':<14} {'count':>8} {'est_MB':>10} {'share':>8}")
    for name, v in buckets:
        share = (100.0 * v["bytes"] / est_total) if est_total else 0
        print(f"{name:<14} {v['count']:>8} {v['bytes'] / MB:>10.1f} {share:>7.1f}%")
    print(f"{'TOTAL_est':<14} {'':>8} {est_total / MB:>10.1f}")
    print(f"(texture alone {tex_b / MB:.1f} MB est VRAM; disk {disk / MB:.1f} MB)")

    print("\nTop Unity types by count:")
    types = sorted(rep["by_type"].items(), key=lambda x: (-x[1]["count"], x[0]))
    for name, v in types[:25]:
        extra = f"  est={v['bytes'] / MB:.1f}MB" if v["bytes"] else ""
        print(f"  {v['count']:>6}  {name}{extra}")

    if rep["tex_rows"]:
        print(f"\nTop {min(top_n, len(rep['tex_rows']))} textures by est VRAM:")
        print(f"  {'MB':>7}  {'WxH':>13}  {'mips':>4}  {'format':<18}  name")
        for est, name, w, h, fmt, mips in rep["tex_rows"]:
            print(f"  {est / MB:7.2f}  {w:>5}x{h:<5}  {mips:>4}  {fmt:<18}  {name}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("paths", nargs="+", type=Path)
    ap.add_argument("--glob", default="", help="if path is dir, glob filter")
    ap.add_argument("--top", type=int, default=20)
    args = ap.parse_args()

    files: list[Path] = []
    for p in args.paths:
        if p.is_dir():
            pat = args.glob or "*.bundle"
            files.extend(sorted(p.rglob(pat)))
        elif p.is_file():
            files.append(p)
        else:
            print(f"skip missing: {p}", file=sys.stderr)

    if not files:
        sys.exit("no files")

    grand = defaultdict(lambda: {"count": 0, "bytes": 0})
    for f in files:
        print(f"scanning {f} ...", flush=True)
        try:
            rep = scan_file(f, args.top)
        except Exception as e:
            print(f"FAIL {f}: {e}", file=sys.stderr)
            continue
        print_report(rep, args.top)
        for k, v in rep["by_bucket"].items():
            grand[k]["count"] += v["count"]
            grand[k]["bytes"] += v["bytes"]

    if len(files) > 1:
        total = sum(v["bytes"] for v in grand.values())
        print("\n======== AGGREGATE ========")
        for name, v in sorted(grand.items(), key=lambda x: -x[1]["bytes"]):
            share = (100.0 * v["bytes"] / total) if total else 0
            print(f"{name:<14} n={v['count']:<8} {v['bytes'] / MB:8.1f} MB  {share:5.1f}%")
        print(f"{'TOTAL_est':<14} {'' :<8} {total / MB:8.1f} MB")


if __name__ == "__main__":
    main()
