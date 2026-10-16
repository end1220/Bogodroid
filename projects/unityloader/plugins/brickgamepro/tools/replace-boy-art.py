#!/usr/bin/env python3
"""Prepare artwork and patch only Boy atlas ETC2 blocks.

Requires Pillow, numpy, UnityPy and its ETC encoder. Never overwrites inputs.
The output directory must be new; changed resource splits are backed up there.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path

import numpy as np
import UnityPy
from PIL import Image, ImageDraw
from UnityPy.enums import TextureFormat
from UnityPy.export.Texture2DConverter import image_to_texture2d


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inputs', type=Path)
    parser.add_argument('data', type=Path, help='staged game/assets/bin/Data')
    parser.add_argument('output', type=Path)
    parser.add_argument('--white-point', type=int, default=235,
                        help='remove near-white baked checkerboard; lower tones become black alpha')
    parser.add_argument('--preserve-alpha', action='store_true',
                        help='use already transparent source pixels verbatim, without unmatting')
    parser.add_argument('--boys-only', action='store_true', help='replace only Boy1/Boy2; ignore the separate Game Over icon')
    parser.add_argument('--layout-manifest', type=Path,
                        help='reuse a previous shared-canvas Boy transform after manual edits')
    args = parser.parse_args()
    if args.output.exists():
        parser.error('output exists; choose a new directory to preserve previous work')
    if not 1 <= args.white_point <= 255:
        parser.error('white-point must be 1..255')
    processed = {}
    provenance = {}
    layout = json.loads(args.layout_manifest.read_text(encoding='utf-8')) if args.layout_manifest else None
    if layout and 'shared_transform' not in layout:
        parser.error('legacy per-sprite layouts break frame alignment; omit --layout-manifest to regenerate')
    for name in ('Boy1', 'Boy2') if args.boys_only else ('Boy1', 'Boy2', 'gameover-icon'):
        path = args.inputs / (name + '.png')
        source = Image.open(path).convert('RGBA')
        if args.preserve_alpha:
            assert source.getextrema()[3][0] == 0, f'{name}: expected existing transparency'
            processed[name] = source
        else:
            pixels = np.asarray(source).astype(np.float32)
            # Unmatte black artwork from its near-white backdrop. Gray edge
            # coverage becomes black alpha without white RGB scaling halos.
            luminance = pixels[..., :3] @ np.array([0.2126, 0.7152, 0.0722])
            alpha = np.rint(np.clip(1 - luminance / args.white_point, 0, 1) * pixels[..., 3]).astype('uint8')
            out = np.zeros((*alpha.shape, 4), dtype='uint8')
            out[..., 3] = alpha
            processed[name] = Image.fromarray(out, 'RGBA')
            assert alpha.max() > 200 and (alpha == 0).any() and ((alpha > 0) & (alpha < 255)).any()
        provenance[name] = dict(source=str(path.resolve()), source_sha256=sha(path.read_bytes()),
                                dimensions=list(source.size), alpha_bbox=processed[name].getbbox())

    parts = sorted(args.data.glob('sharedassets0.assets.split*'),
                   key=lambda p: int(p.name.rsplit('split', 1)[1]))
    assert len(parts) == 16, 'unexpected resource split count'
    original = b''.join(p.read_bytes() for p in parts)
    env = UnityPy.load(original)
    texture_obj = next(o for o in env.objects if o.path_id == 12)
    texture = texture_obj.read()
    assert texture.m_Name == 'SpriteAtlasTexture-UI-2048x1024-fmt47'
    assert texture.m_TextureFormat == TextureFormat.ETC2_RGBA8 and texture.m_MipCount == 1
    assert texture.m_Width == 2048 and texture.m_Height == 1024
    atlas = texture.image.convert('RGBA')
    original_pixels = np.asarray(atlas).copy()
    packed = bytes(texture.image_data)
    assert len(packed) == atlas.width * atlas.height
    packed_offset = texture_obj.get_raw_data().find(packed)
    assert packed_offset >= 0 and texture_obj.get_raw_data().count(packed) == 1
    sprites = {o.read().m_Name: o for o in env.objects if o.type.name == 'Sprite'
               and o.path_id in (140, 115)}
    assert set(sprites) == {'Boy1', 'Boy2'}
    # Both poses share source-canvas coordinates. Resample their entire canvases
    # with one scale and origin, then cut each sprite's packed region out of the
    # common logical 256x256 canvas. Independent alpha crops would shift/scale
    # the body as the arms change pose.
    geometry = {}
    logical_sizes = set()
    for name, obj in sprites.items():
        sprite = obj.read()
        rect = sprite.m_RD.textureRect
        offset = sprite.m_RD.textureRectOffset
        assert sprite.m_RD.texture.m_PathID == 12
        logical_size = (round(sprite.m_Rect.width), round(sprite.m_Rect.height))
        logical_sizes.add(logical_size)
        assert sprite.m_Pivot.x == .5 and sprite.m_Pivot.y == .5
        assert sprite.m_RD.settingsRaw == 67, 'expected unrotated original sprites'
        box = (round(rect.x), atlas.height-round(rect.y+rect.height),
               round(rect.x+rect.width), atlas.height-round(rect.y))
        exact_origin = (rect.x-offset.x,
                        atlas.height-rect.y+offset.y-logical_size[1])
        origin = tuple(round(v) for v in exact_origin)
        assert all(abs(a-b) < .001 for a, b in zip(exact_origin, origin)), 'noninteger packing origin'
        logical_box = (box[0]-origin[0], box[1]-origin[1],
                       box[2]-origin[0], box[3]-origin[1])
        geometry[name] = (box, logical_box)
    assert len(logical_sizes) == 1
    logical_size = logical_sizes.pop()
    assert processed['Boy1'].size == processed['Boy2'].size
    source_size = processed['Boy1'].size
    assert source_size[0] == source_size[1], 'expected square aligned input canvases'
    safe = (max(b[0]+2 for _, b in geometry.values()),
            max(b[1]+2 for _, b in geometry.values()),
            min(b[2]-2 for _, b in geometry.values()),
            min(b[3]-2 for _, b in geometry.values()))
    available = (safe[2]-safe[0], safe[3]-safe[1])
    assert min(available) > 0
    if layout:
        shared = layout['shared_transform']
        assert shared['input_canvas_size'] == list(source_size)
        scaled_size = tuple(shared['scaled_canvas_size'])
        placement = tuple(shared['placement_in_logical_canvas'])
    else:
        bounds = [processed[n].getbbox() for n in ('Boy1', 'Boy2')]
        union = (min(b[0] for b in bounds), min(b[1] for b in bounds),
                 max(b[2] for b in bounds), max(b[3] for b in bounds))
        scale = min(1, available[0]/(union[2]-union[0]), available[1]/(union[3]-union[1]))
        edge = math.floor(source_size[0]*scale)
        while edge > 0:
            scaled_size = (edge, edge)
            bounds = [processed[n].resize(scaled_size, Image.Resampling.LANCZOS).getbbox()
                      for n in ('Boy1', 'Boy2')]
            union = (min(b[0] for b in bounds), min(b[1] for b in bounds),
                     max(b[2] for b in bounds), max(b[3] for b in bounds))
            if union[2]-union[0] <= available[0] and union[3]-union[1] <= available[1]:
                break
            edge -= 1
        assert edge > 0
        placement = ((safe[0]+safe[2]-(union[2]-union[0]))//2-union[0],
                     (safe[1]+safe[3]-(union[3]-union[1]))//2-union[1])
    shared = dict(input_canvas_size=list(source_size), scaled_canvas_size=list(scaled_size),
                  logical_canvas_size=list(logical_size), placement_in_logical_canvas=list(placement),
                  safe_logical_box=list(safe))
    regions = []
    sprite_info = {}
    logical_canvases = {}
    for name in ('Boy1', 'Boy2'):
        obj = sprites[name]
        sprite = obj.read()
        rect = sprite.m_RD.textureRect
        assert sprite.m_RD.texture.m_PathID == 12
        box, logical_box = geometry[name]
        canvas = Image.new('RGBA', logical_size)
        canvas.paste(processed[name].resize(scaled_size, Image.Resampling.LANCZOS), placement)
        bounds = canvas.getbbox()
        assert bounds and bounds[0] >= safe[0] and bounds[1] >= safe[1]
        assert bounds[2] <= safe[2] and bounds[3] <= safe[3], 'art outside shared layout'
        tile = canvas.crop(logical_box)
        # Reconstruct through each sprite's original trim offset. This catches
        # loss of pixels or any difference between packed and logical placement.
        reconstructed = Image.new('RGBA', logical_size)
        reconstructed.paste(tile, logical_box[:2])
        assert reconstructed.tobytes() == canvas.tobytes()
        logical_canvases[name] = canvas
        atlas.paste(tile, box[:2])
        aligned = (box[0]//4*4, box[1]//4*4, (box[2]+3)//4*4, (box[3]+3)//4*4)
        regions.append(aligned)
        sprite_info[name] = dict(path_id=obj.path_id, texture_rect=[rect.x, rect.y, rect.width, rect.height],
                                 png_box=box, etc2_block_box=aligned, logical_box=list(logical_box),
                                 sprite_sha256=sha(obj.get_raw_data()), mesh_and_uv_unchanged=True)

    args.output.mkdir(parents=True)
    for name, canvas in logical_canvases.items():
        canvas.save(args.output / (name + '-对齐画布.png'))
    for name, result in processed.items():
        destination = args.output / (name + '.png')
        if args.preserve_alpha:
            destination.write_bytes((args.inputs / (name + '.png')).read_bytes())
            assert Image.open(destination).convert('RGBA').tobytes() == result.tobytes()
        else:
            result.save(destination)
    atlas.save(args.output / 'UI-Boy替换图集.png')
    texture.image.save(args.output / 'UI-替换前图集.png')
    payload = bytearray(packed)
    changed_blocks = set()
    blocks_per_row = atlas.width // 4
    for x0, y0, x1, y1 in regions:
        region = atlas.crop((x0, y0, x1, y1))
        encoded, fmt = image_to_texture2d(region, TextureFormat.ETC2_RGBA8)
        assert fmt == TextureFormat.ETC2_RGBA8 and len(encoded) == region.width * region.height
        row_bytes = region.width // 4 * 16
        # ETC2 rows are bottom-up in Unity; image_to_texture2d performs that flip.
        for by in range(region.height // 4):
            dest = ((atlas.height-y1)//4+by)*blocks_per_row+x0//4
            payload[dest*16:dest*16+row_bytes] = encoded[by*row_bytes:(by+1)*row_bytes]
            changed_blocks.update(range(dest, dest + region.width//4))
    assert all(payload[i:i+16] == packed[i:i+16] for i in range(0, len(packed), 16)
               if i//16 not in changed_blocks)
    start = texture_obj.byte_start + packed_offset
    updated = original[:start] + payload + original[start+len(payload):]
    assert len(updated) == len(original)
    verify = UnityPy.load(updated)
    before_objects = {o.path_id: sha(o.get_raw_data()) for o in env.objects}
    after_objects = {o.path_id: sha(o.get_raw_data()) for o in verify.objects}
    assert set(before_objects) == set(after_objects)
    changed_objects = [key for key in before_objects if before_objects[key] != after_objects[key]]
    assert changed_objects == [12], changed_objects
    roundtrip = next(o for o in verify.objects if o.path_id == 12).read().image.convert('RGBA')
    roundtrip.save(args.output / 'UI-游戏内ETC2图集.png')
    mask = np.ones((atlas.height, atlas.width), dtype=bool)
    for x0, y0, x1, y1 in regions:
        mask[y0:y1, x0:x1] = False
    assert np.array_equal(np.asarray(roundtrip)[mask], original_pixels[mask])

    backup = args.output / '资源备份'
    patched = args.output / '替换资源'
    backup.mkdir()
    patched.mkdir()
    changed_parts = []
    offset = 0
    for part in parts:
        old = part.read_bytes()
        new = updated[offset:offset+len(old)]
        if new != old:
            (backup / part.name).write_bytes(old)
            (patched / part.name).write_bytes(new)
            changed_parts.append(dict(name=part.name, bytes=len(new), old_sha256=sha(old), sha256=sha(new)))
        offset += len(old)
    assert offset == len(updated) and changed_parts
    preview = Image.new('RGB', (720, 340 if args.boys_only else 590), '#bfc5bc')
    draw = ImageDraw.Draw(preview)
    for i, name in enumerate(('Boy1', 'Boy2')):
        im = processed[name].copy()
        im.thumbnail((300, 300), Image.Resampling.LANCZOS)
        preview.paste(im, (30+i*350, 35), im)
        draw.text((30+i*350, 12), name, fill='black')
    if not args.boys_only:
        im = processed['gameover-icon'].copy()
        im.thumbnail((500, 230), Image.Resampling.LANCZOS)
        preview.paste(im, ((720-im.width)//2, 350), im)
    preview.save(args.output / '透明处理预览.png')
    manifest = dict(white_point=None if args.preserve_alpha else args.white_point,
                    alpha_formula='unchanged' if args.preserve_alpha else f'source_alpha * clamp(1 - luminance/{args.white_point}, 0, 1)',
                    rgb='unchanged' if args.preserve_alpha else 'black (0,0,0)',
                    layout_manifest=str(args.layout_manifest.resolve()) if layout else None,
                    resize='shared full-canvas LANCZOS and placement, 2px packed-rect inset',
                    shared_transform=shared,
                    inputs=provenance, sprites=sprite_info, resource_before_sha256=sha(original),
                    resource_sha256=sha(updated), unchanged_objects=len(before_objects)-1,
                    changed_object_ids=changed_objects, untouched_atlas_blocks_identical=True,
                    changed_parts=changed_parts)
    (args.output / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    # Activate only after all byte-level and decoded-pixel checks have passed.
    for part in changed_parts:
        path = args.data / part['name']
        assert sha(path.read_bytes()) == part['old_sha256'], 'staging changed during preparation'
        path.write_bytes((patched / part['name']).read_bytes())
    print(json.dumps(manifest, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
