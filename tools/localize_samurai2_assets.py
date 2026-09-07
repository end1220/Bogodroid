#!/usr/bin/env python3
"""Safely localize Samurai II 2025 comic and UI Texture2D objects.

Design notes, kept inline with the implementation so the reasoning does not
drift into a separate document:

- Treat the 2025 APK data as the source of truth. The Unity 3 projects are
  reference image sources only; never copy their serialized Unity files into
  the 2025 build.
- Localize by equal-size Texture2D replacement. Preserve PathIDs, container
  sizes, and file-level metadata so scene and prefab references keep working.
- Join split Unity containers, patch only the intended object byte ranges, then
  split them back to the original chunk layout.
- For most UI textures, pull pixels from the old project's Unity import cache so
  the alpha/import behavior matches the source art that was already vetted in
  Unity.
- The current verified target set is 85 Texture2D objects: 64 comic pages,
  popandro interstitials, the Comic GUI texture, menu/UI atlases, and the
  tutorial phone/loading images.
- `menu_achiev` is the one special alpha case: the upper area keeps the APK
  alpha, while the lower area derives alpha from translated RGB content.
- `UnityPy.Environment.save()` is intentionally avoided because earlier tests
  showed that it could preserve the visible images while still breaking gameplay
  behavior by rewriting serialized metadata.
- The visible English UI is mostly baked into atlases rather than dynamic text.
  A TTF swap alone will not translate those labels.
- Unity splash suppression is a separate packaging concern. If needed, patch
  `globalgamemanagers` offline and validate the exact Unity version before
  trusting it.
- The generated `samurai2-localization.json` is an audit manifest only. It is
  useful for review and reproducibility, but the game does not read it.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import tempfile
from dataclasses import dataclass
from pathlib import Path

import UnityPy
import numpy as np
from PIL import Image
from psd_tools import PSDImage


CHUNK_SIZE = 1024 * 1024
# The asset mapping below mirrors the verified 2025 APK data layout.
# Every entry points at an existing serialized Texture2D object that has already
# been validated in the target build.
CHAPTER_ASSETS = {
    1: "sharedassets2.assets",
    2: "sharedassets5.assets",
    3: "sharedassets8.assets",
    4: "sharedassets11.assets",
    5: "sharedassets14.assets",
    6: "sharedassets17.assets",
    7: "sharedassets20.assets",
    8: "sharedassets23.assets",
}
POPANDRO_ASSETS = {
    1: "sharedassets2.assets",
    2: "sharedassets4.assets",
    3: "sharedassets7.assets",
    4: "sharedassets10.assets",
    5: "sharedassets13.assets",
    6: "sharedassets16.assets",
    7: "sharedassets19.assets",
    8: "sharedassets22.assets",
}


@dataclass(frozen=True)
class Target:
    # `source_kind` selects either a raw PSD/TGA reader or the old project's
    # Unity import cache. That keeps the replacement pixels faithful to the art
    # that was already authored for each texture family.
    name: str
    asset: str
    source: str
    source_kind: str = "raw"
    alpha_mode: str = "source"


TARGETS = [
    # Comic pages: exact name match, exact asset file match, equal-size swap.
    *[
        Target(
            name=f"c{chapter:02d}p{page:02d}",
            asset=asset,
            source=f"Comics_iPhone/c{chapter:02d}p{page:02d}.psd",
        )
        for chapter, asset in CHAPTER_ASSETS.items()
        for page in range(1, 9)
    ],
    # Interstitial comic panels that are also sourced from Unity-imported art.
    *[
        Target(
            name=f"popandro{chapter:02d}",
            asset=asset,
            source=f"Comics_iPhone/popandro{chapter:02d}.psd",
            source_kind="unity_cache",
        )
        for chapter, asset in POPANDRO_ASSETS.items()
    ],
    Target("guiHD", "a0f24782535324c00ba7ea53212b23c9", "Gui/guiHD.psd", "unity_cache"),
    Target("gui3G", "e2bf653a350e943d398ccfe5f0678b02", "Gui/gui3G.psd", "unity_cache"),
    Target("sum3g", "8ce326b7a235546d49292f3d1470a8f7", "Gui/sum3g.psd", "unity_cache"),
    Target("sum4g", "dafabfc813ede455aa6d6004eea773ea", "Gui/sum4g.psd", "unity_cache"),
    Target("MainMenu_Buttons", "sharedassets0.assets", "MainMenu/MainMenu_Buttons.tga", "unity_cache"),
    Target("mainmenuloading", "sharedassets0.assets", "MainMenu/mainmenuloading.tga", "unity_cache"),
    Target("easy_button", "sharedassets34.assets", "MainMenu/easy_button.psd", "unity_cache"),
    Target("menu_achiev", "sharedassets34.assets", "MainMenu/menu_achiev.tga", "unity_cache", "menu_achiev"),
    Target("ComixGui_iPhone", "sharedassets2.assets", "Comics_iPhone/ComixGui_iPhone.tga", "unity_cache"),
    Target("tutorloading", "sharedassets36.assets", "Tutorial/tutorloading.tga", "unity_cache"),
    Target("phone01", "sharedassets36.assets", "Tutorial/phone01.psd", "unity_cache"),
    Target("phone02", "sharedassets36.assets", "Tutorial/phone02.psd", "unity_cache"),
    Target("phone03", "sharedassets36.assets", "Tutorial/phone03.psd", "unity_cache"),
]
# Fast lookup by serialized asset name + Texture2D name.
TARGET_BY_KEY = {(target.asset, target.name): target for target in TARGETS}
TARGET_ASSETS = {target.asset for target in TARGETS}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def asset_pieces(directory: Path, name: str) -> list[Path]:
    plain = directory / name
    if plain.exists():
        return [plain]
    return sorted(
        directory.glob(name + ".split*"),
        key=lambda path: int(path.name.rsplit(".split", 1)[1]),
    )


def join_asset(directory: Path, name: str, work: Path) -> Path:
    # Serialized Unity files may be stored as plain files or .splitN fragments.
    # Reassemble them into a temporary single file before loading or patching.
    pieces = asset_pieces(directory, name)
    if not pieces:
        raise FileNotFoundError(f"missing serialized asset: {name}")
    joined = work / name
    with joined.open("wb") as output:
        for piece in pieces:
            with piece.open("rb") as source:
                shutil.copyfileobj(source, output)
    return joined


def split_asset(serialized: Path, output: Path, name: str, preserve_plain: bool = False) -> list[Path]:
    # Write the patched file back using the original container style. If the
    # source was a plain file and fits in one chunk, keep it plain; otherwise
    # regenerate the .splitN sequence and delete stale chunks first.
    for old in output.glob(name + ".split*"):
        old.unlink()
    plain = output / name
    if plain.exists():
        plain.unlink()
    data = serialized.read_bytes()
    if preserve_plain or len(data) <= CHUNK_SIZE:
        plain.write_bytes(data)
        return [plain]
    written = []
    for index, start in enumerate(range(0, len(data), CHUNK_SIZE)):
        path = output / f"{name}.split{index}"
        path.write_bytes(data[start:start + CHUNK_SIZE])
        written.append(path)
    return written


def load_raw(path: Path) -> Image.Image:
    if path.suffix.lower() == ".psd":
        return PSDImage.open(path).composite()
    return Image.open(path)


def unity_cache_path(project: Path, source: Path) -> Path:
    meta = Path(str(source) + ".meta")
    lines = meta.read_text(encoding="utf-8", errors="ignore").splitlines()
    guid = next(line.split(":", 1)[1].strip() for line in lines if line.startswith("guid:"))
    return project / "Library/cache" / guid[:2] / guid


def load_unity_cache(project: Path, source: Path, expected_name: str) -> Image.Image:
    # The old Unity project already contains the import settings that produced
    # the Chinese art. Reusing the cache is more accurate than re-decoding the
    # raw source file in isolation.
    cache = unity_cache_path(project, source)
    environment = UnityPy.load(str(cache))
    matches = []
    for obj in environment.objects:
        if obj.type.name == "Texture2D":
            texture = obj.read()
            if texture.m_Name == expected_name:
                matches.append(texture.image.copy())
    if len(matches) != 1:
        raise RuntimeError(f"{source}: expected one cached Texture2D named {expected_name}, found {len(matches)}")
    return matches[0]


def rebuild_menu_achiev_alpha(source: Image.Image, original: Image.Image) -> Image.Image:
    # The bottom portion is derived from translated RGB pixels; the top keeps the
    # original alpha so the atlas still blends the same way in-game.
    source = source.convert("RGBA")
    original = original.convert("RGBA")
    pixels = np.asarray(source).copy()
    original_pixels = np.asarray(original)
    cutoff = round(source.height * 0.43)
    pixels[:cutoff, :, 3] = original_pixels[:cutoff, :, 3]
    pixels[cutoff:, :, 3] = pixels[cutoff:, :, :3].max(axis=2)
    return Image.fromarray(pixels, "RGBA")


def replacement_image(target: Target, project: Path, target_texture: object) -> Image.Image:
    # Preserve the target canvas size and pixel mode. This keeps the serialized
    # Texture2D object structurally identical while still swapping the art.
    source_path = project / "Assets/Assets" / target.source
    if not source_path.exists():
        raise FileNotFoundError(source_path)
    if target.source_kind == "unity_cache":
        image = load_unity_cache(project, source_path, target.name)
    else:
        image = load_raw(source_path)
    target_image = target_texture.image
    if image.size != target_image.size:
        image = image.resize(target_image.size, Image.Resampling.LANCZOS)
    if target.alpha_mode == "menu_achiev":
        image = rebuild_menu_achiev_alpha(image, target_image)
    return image.convert(target_image.mode)


def validate_changed_ranges(original: bytes, patched: bytes, ranges: list[tuple[int, int]], asset: str) -> None:
    # Defensive check: only the intended serialized object ranges may change.
    if len(original) != len(patched):
        raise RuntimeError(f"{asset}: container length changed")
    ordered = sorted(ranges)
    range_index = 0
    for offset, (before, after) in enumerate(zip(original, patched)):
        if before == after:
            continue
        while range_index < len(ordered) and offset >= ordered[range_index][1]:
            range_index += 1
        if range_index >= len(ordered) or offset < ordered[range_index][0]:
            raise RuntimeError(f"{asset}: changed byte outside target objects at offset {offset}")


def main() -> int:
    # The tool is intentionally conservative: copy the extracted Data directory
    # to a fresh output tree, patch the copy, and refuse to overwrite anything
    # in place.
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True, help="extracted 2025 Data directory")
    parser.add_argument("--project", type=Path, required=True, help="translated Samurai2Tegra5 Unity project")
    parser.add_argument("--output", type=Path, required=True, help="new localized Data directory")
    args = parser.parse_args()
    source = args.input.resolve()
    project = args.project.resolve()
    output = args.output.resolve()
    if output.exists():
        raise SystemExit(f"refusing to overwrite existing output: {output}")
    shutil.copytree(source, output)

    manifest = []
    seen = set()
    with tempfile.TemporaryDirectory(prefix="samurai2-localization-") as temporary:
        work = Path(temporary)
        joined = {name: join_asset(source, name, work) for name in TARGET_ASSETS}
        originals = {name: path.read_bytes() for name, path in joined.items()}
        patched = {name: bytearray(data) for name, data in originals.items()}
        changed_ranges: dict[str, list[tuple[int, int]]] = {name: [] for name in TARGET_ASSETS}
        environment = UnityPy.load(str(source))
        for obj in environment.objects:
            if obj.type.name != "Texture2D":
                continue
            texture = obj.read()
            key = (obj.assets_file.name, getattr(texture, "m_Name", ""))
            target = TARGET_BY_KEY.get(key)
            if target is None:
                continue
            if key in seen:
                raise RuntimeError(f"duplicate target Texture2D: {key}")
            original_size = obj.byte_size
            translated = replacement_image(target, project, texture)
            texture.image = translated
            texture.save()
            replacement = bytes(obj.data)
            if len(replacement) != original_size:
                raise RuntimeError(
                    f"{target.asset} PathID {obj.path_id}: object size changed "
                    f"from {original_size} to {len(replacement)}"
                )
            start = obj.byte_start
            patched[target.asset][start:start + original_size] = replacement
            changed_ranges[target.asset].append((start, start + original_size))
            seen.add(key)
            source_path = project / "Assets/Assets" / target.source
            manifest.append({
                "name": target.name,
                "asset": target.asset,
                "path_id": obj.path_id,
                "width": texture.m_Width,
                "height": texture.m_Height,
                "source": target.source,
                "source_sha256": sha256(source_path),
                "source_kind": target.source_kind,
                "alpha_mode": target.alpha_mode,
            })
        missing = sorted(set(TARGET_BY_KEY) - seen)
        if missing:
            raise RuntimeError(f"missing {len(missing)} target Texture2D objects: {missing}")
        for asset in sorted(TARGET_ASSETS):
            validate_changed_ranges(originals[asset], bytes(patched[asset]), changed_ranges[asset], asset)
            joined[asset].write_bytes(patched[asset])
            split_asset(joined[asset], output, asset, preserve_plain=(source / asset).exists())

    # Reload every generated file and ensure all intended objects remain readable.
    generated = UnityPy.load(str(output))
    reloaded = set()
    for obj in generated.objects:
        if obj.type.name != "Texture2D":
            continue
        texture = obj.read()
        key = (obj.assets_file.name, getattr(texture, "m_Name", ""))
        if key in TARGET_BY_KEY:
            _ = texture.image
            reloaded.add(key)
    if reloaded != set(TARGET_BY_KEY):
        raise RuntimeError(f"reload validation missed {sorted(set(TARGET_BY_KEY) - reloaded)}")

    manifest_path = output / "samurai2-localization.json"
    manifest_path.write_text(
        json.dumps({
            "format": 3,
            "container_mode": "equal-size Texture2D object replacement",
            "chunk_size": CHUNK_SIZE,
            "objects": sorted(manifest, key=lambda item: (item["asset"], item["path_id"])),
        }, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(f"localized and validated {len(manifest)} Texture2D objects into {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
