"""Replace only the Blue skin ETC2 payload; preserve serialized object layout."""
import argparse,hashlib,json
from pathlib import Path
import UnityPy
from PIL import Image
from UnityPy.enums import TextureFormat
from UnityPy.export.Texture2DConverter import image_to_texture2d

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('data',type=Path); p.add_argument('png',type=Path); p.add_argument('output',type=Path)
    a=p.parse_args(); a.output.mkdir(parents=True,exist_ok=False)
    parts=sorted(a.data.glob('sharedassets0.assets.split*'),key=lambda p:int(p.name.rsplit('split',1)[1]))
    assert len(parts)==16
    raw=b''.join(p.read_bytes() for p in parts); env=UnityPy.load(raw)
    obj=next(o for o in env.objects if o.path_id==16); tex=obj.read()
    assert tex.m_Name=='SpriteAtlasTexture-Blue-1024x1024-fmt47'
    assert tex.m_MipCount==1 and tex.m_TextureFormat==TextureFormat.ETC2_RGBA8
    im=Image.open(a.png).convert('RGBA'); assert im.size==(tex.m_Width,tex.m_Height)==(1024,1024)
    encoded,fmt=image_to_texture2d(im,TextureFormat.ETC2_RGBA8)
    old=bytes(tex.image_data); assert fmt==TextureFormat.ETC2_RGBA8 and len(old)==len(encoded)
    object_raw=obj.get_raw_data(); assert object_raw.count(old)==1
    start=obj.byte_start+object_raw.index(old)
    updated=raw[:start]+encoded+raw[start+len(old):]; assert len(raw)==len(updated)
    verified=UnityPy.load(updated)
    before={o.path_id:o.get_raw_data() for o in env.objects}
    after={o.path_id:o.get_raw_data() for o in verified.objects}
    assert before.keys()==after.keys()
    assert [k for k in before if before[k]!=after[k]]==[16]
    next(o for o in verified.objects if o.path_id==16).read().image.save(a.output/'Blue-ETC2-roundtrip.png')
    sha=lambda b:hashlib.sha256(b).hexdigest()
    changes=[]; offset=0
    for part in parts:
        old=part.read_bytes(); new=updated[offset:offset+len(old)]; offset+=len(old)
        if new!=old:
            (a.output/'previous').mkdir(exist_ok=True)
            (a.output/'previous'/part.name).write_bytes(old)
            (a.output/part.name).write_bytes(new)
            changes.append(dict(name=part.name,bytes=len(new),old_sha256=sha(old),sha256=sha(new)))
    (a.output/'manifest.json').write_text(json.dumps(dict(source=str(a.png.resolve()),source_sha256=sha(a.png.read_bytes()),texture_path_id=16,unchanged_objects=len(before)-1,changed_parts=changes),indent=2))
    print(json.dumps(changes,indent=2))
if __name__=='__main__': main()
