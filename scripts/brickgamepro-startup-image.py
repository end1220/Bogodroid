#!/usr/bin/env python3
"""Convert the APK's static Unity splash to BMP for the loader's SDL renderer."""
import argparse
from pathlib import Path
from PIL import Image

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("game_root", type=Path)
args = parser.parse_args()
source = args.game_root / "res/drawable/unity_static_splash.png"
target = source.with_suffix(".bmp")
with Image.open(source) as original:
    rgb = original.convert("RGB")
    rgb.save(target)
    with Image.open(target) as result:
        assert result.size == rgb.size and result.convert("RGB").tobytes() == rgb.tobytes()
print(f"{target}: {target.stat().st_size} bytes; RGB pixels verified")
