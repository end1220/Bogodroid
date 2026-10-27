"""Convert a raw Anbernic framebuffer dump (1280x1024 rgb565le) into PNG crops.

Usage: python fb2png.py <raw> <out-prefix>
Writes <out-prefix>-<x0>-<y0>.png for the 640x480 windows the panel may pan over.
"""
import sys
from PIL import Image

raw_path, prefix = sys.argv[1], sys.argv[2]
W, H = 1280, 1024          # fb0 virtual_size 1280,1024 @ 16bpp
PANEL_W, PANEL_H = 640, 480

with open(raw_path, "rb") as fh:
    data = fh.read(W * H * 2)

img = Image.frombytes("RGB", (W, H), data, "raw", "BGR;16")
img.save(f"{prefix}-full.png")

seen = {}
for x0 in (0, 640):
    for y0 in (0, 480):
        crop = img.crop((x0, y0, x0 + PANEL_W, y0 + PANEL_H))
        name = f"{prefix}-{x0}-{y0}.png"
        crop.save(name)
        # Cheap "is this the live frame?" heuristic: a live menu/game frame is
        # not a single flat colour.
        colors = crop.getcolors(maxcolors=1 << 20)
        seen[name] = (len(colors), len(set(c[1] for c in colors)))
        print(f"{name}: distinct_colors={seen[name][0]}")

print("full:", f"{prefix}-full.png", img.size)
