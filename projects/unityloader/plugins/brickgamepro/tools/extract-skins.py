#!/usr/bin/env python3
"""Export original BrickGamePro skin atlases from its APK for manual editing.

Requires UnityPy and Pillow. Reads the APK without modifying game resources.
"""
import argparse
import hashlib
import json
import re
import zipfile
from pathlib import Path

import UnityPy
from PIL import Image, ImageDraw
from UnityPy.enums import TextureFormat


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("apk", type=Path)
    parser.add_argument("output", type=Path, help="new export directory")
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output already exists; choose a new directory to preserve manual edits")

    asset = "assets/bin/Data/sharedassets0.assets"
    with zipfile.ZipFile(args.apk) as archive:
        if asset in archive.namelist():
            parts = [asset]
        else:
            parts = sorted(
                (name for name in archive.namelist()
                 if re.fullmatch(re.escape(asset) + r"\.split\d+", name)),
                key=lambda name: int(name.rsplit("split", 1)[1]))
        if not parts:
            parser.error("APK does not contain BrickGamePro's sharedassets0.assets")
        raw = b"".join(archive.read(name) for name in parts)
    env = UnityPy.load(raw)
    textures, sprites = {}, {}
    for obj in env.objects:
        if obj.type.name == "Texture2D":
            textures[obj.path_id] = obj
        elif obj.type.name == "Sprite":
            sprite = obj.read()
            if sprite.m_RD.texture.m_FileID == 0:
                sprites.setdefault(sprite.m_RD.texture.m_PathID, []).append((obj, sprite))
    skins = {}
    for path_id, obj in textures.items():
        related = sprites.get(path_id, [])
        if not any(sprite.m_Name.startswith("Casing_") for _, sprite in related):
            continue
        texture = obj.read()
        match = re.fullmatch(r"SpriteAtlasTexture-(.+)-\d+x\d+-fmt\d+", texture.m_Name)
        if not match:
            raise ValueError(f"Unrecognized skin atlas: {texture.m_Name}")
        skins[match.group(1)] = (obj, texture, related)
    if not skins:
        parser.error("no skin atlases referenced by Casing sprites found")

    args.output.mkdir(parents=True)
    overview = Image.new("RGB", (4 * 276, ((len(skins) + 3) // 4) * 300), "#333333")
    draw = ImageDraw.Draw(overview)
    manifest = {
        "apk": str(args.apk.resolve()),
        "apk_sha256": sha256(args.apk.read_bytes()),
        "source_parts": parts,
        "serialized_asset_sha256": sha256(raw),
        "unitypy_version": UnityPy.__version__,
        "png_origin": "top-left (UnityPy image orientation; no extra flips)",
        "skins": {},
    }
    for index, (name, (obj, texture, related)) in enumerate(sorted(skins.items())):
        image = texture.image.convert("RGBA")
        for folder in ("可编辑图集", "原图备份"):
            target = args.output / folder / f"{name}.png"
            target.parent.mkdir(parents=True, exist_ok=True)
            image.save(target)
            with Image.open(target) as saved:
                if saved.mode != "RGBA" or saved.size != image.size or saved.tobytes() != image.tobytes():
                    raise ValueError(f"PNG pixel verification failed: {target}")
        entry = {
            "asset": "sharedassets0.assets", "path_id": obj.path_id,
            "texture": texture.m_Name, "width": image.width, "height": image.height,
            "format": TextureFormat(texture.m_TextureFormat).name,
            "mip_count": texture.m_MipCount,
            "object_sha256": sha256(obj.get_raw_data()),
            "pixel_sha256": sha256(image.tobytes()), "sprites": [],
        }
        preview_dir = args.output / "部件预览" / name
        preview_dir.mkdir(parents=True)
        for sprite_obj, sprite in sorted(related, key=lambda pair: pair[1].m_Name):
            sprite.image.save(preview_dir / f"{sprite.m_Name}.png")
            rect = sprite.m_RD.textureRect
            entry["sprites"].append({
                "name": sprite.m_Name, "path_id": sprite_obj.path_id,
                "rect_unity_bottom_left": [rect.x, rect.y, rect.width, rect.height],
                "packing_settings_raw": sprite.m_RD.settingsRaw,
            })
        manifest["skins"][name] = entry
        thumb = image.copy()
        thumb.thumbnail((256, 256))
        x, y = (index % 4) * 276 + 10, (index // 4) * 300 + 28
        overview.paste(thumb, (x, y), thumb)
        draw.text((x, y - 20), f"{name}  {image.width}x{image.height}", fill="white")
        print(f"{name}: {image.width}x{image.height}, path_id={obj.path_id}, {len(related)} sprites; pixels verified")
    overview.save(args.output / "皮肤总览.png")
    (args.output / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (args.output / "修改说明.txt").write_text(
        "BrickGamePro 原版皮肤纹理\n\n"
        "可编辑图集：手工修改这里的 PNG。每张 1024×1024，包含机身、屏幕底图和按钮。\n"
        "原图备份：从原版 APK 解码得到的相同图片，用于恢复或比较。\n"
        "部件预览：按原 Sprite 导出的独立部件，方便辨认；完整替换请使用可编辑图集。\n"
        "皮肤总览.png：11 套皮肤的缩略图。Blue.png 对应蓝色机身。\n\n"
        "修改时保留 1024×1024 画布、透明通道以及每个部件的位置和大小。\n"
        "可以改颜色、图案或内容；不要裁切画布、重新排版图集或上下翻转。\n"
        "保存为 RGBA PNG。预览中透明区域可能显示为黑色，请勿将透明区填黑。\n"
        "原游戏使用 ETC2_RGBA8 压缩纹理，导出 PNG 保留解码后的原始分辨率和像素。\n"
        "修改后需要重新写入 Unity 资源；将 PNG 放入游戏目录不会自动替换皮肤。\n"
        "manifest.json 记录 APK 哈希、资源名、path_id、尺寸和 Sprite 坐标，供后续回写核对。\n",
        encoding="utf-8")
    print(f"Exported {len(skins)} skins to {args.output.resolve()}")


if __name__ == "__main__":
    main()
