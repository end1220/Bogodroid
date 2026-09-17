#!/usr/bin/env python3
"""Split a raw /dev/fb0 capture into its two 640x480 halves and summarise them.

    python scripts/fivehearts/fbcap.py <fb.raw> [out.png-prefix]

The Anbernic panel reports virtual_size=640,960 with 32bpp and stride 2560: two
640x480 frames stacked in one mapping, and which one is on the panel depends on
the last page flip. Both are written out with their brightness statistics; the
half with structure (sd in the tens) is the one showing the game, a uniform
sheet is the idle buffer. Channel order on Mali fbdev is BGRA.
"""
import sys

import numpy as np
from PIL import Image

HALF = (640, 480)


def half_at(data, index):
    frame_bytes = HALF[0] * HALF[1] * 4  # BGRA, 4 bytes per pixel
    chunk = data[index * frame_bytes:(index + 1) * frame_bytes]
    if chunk.size < frame_bytes:
        return None
    rgba = chunk.reshape(HALF[1], HALF[0], 4)[:, :, [2, 1, 0]]
    return Image.fromarray(rgba)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    path = argv[1]
    prefix = argv[2] if len(argv) > 2 else path.rsplit(".", 1)[0]
    raw = np.fromfile(path, dtype=np.uint8)
    print("%s: %d bytes (%.2f panel buffers)"
          % (path, raw.size, raw.size / (HALF[0] * HALF[1] * 4)))
    for index in (0, 1):
        image = half_at(raw, index)
        if image is None:
            print("half%d: not present" % index)
            continue
        grey = np.asarray(image.convert("L"), dtype=np.float64)
        print("half%d: mean=%.1f sd=%.1f min=%.0f max=%.0f -> %s.png"
              % (index, grey.mean(), grey.std(), grey.min(), grey.max(),
                 "%s.%d" % (prefix, index)))
        image.save("%s.%d.png" % (prefix, index))
        rows = []
        for row in range(12):
            line = ""
            for col in range(16):
                block = grey[row * 40:(row + 1) * 40, col * 40:(col + 1) * 40]
                line += " .:-=+*#%@"[min(9, int(block.mean() / 25.6))]
            rows.append(line)
        print("\n".join(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
