#!/usr/bin/env python3
"""Work out which part of an uploaded video frame ends up on the panel.

    python scripts/fivehearts/uvmatch.py <screen.ppm> <video.ppm> [--png]

Both a real frame (BD_VIDEO_DUMP_FRAME) and a screen dump (BD_DUMP_FRAME) can be
used: the video quad's u/v range is recovered by matching the column and row
luma profiles of the screen against affine sub-ranges of the video frame. That
answers "only the bottom-left corner is visible" with numbers (u in 0..0.36
instead of 0..1) rather than by eye, and works without the gradient probe.

--png also writes <screen>.png / <video>.png next to the inputs for eyeballing.
"""
import sys

import numpy as np
from PIL import Image


def read_ppm(path):
    img = Image.open(path)
    return np.asarray(img.convert("L"), dtype=np.float64), img.size


def normalise(values):
    values = values - values.mean()
    scale = values.std()
    return values / scale if scale > 1e-6 else values


def fit_axis(screen_profile, video_profile, steps=101):
    """Best (lo, hi) in 0..1 of the video profile that matches the screen."""
    positions = np.arange(len(video_profile), dtype=np.float64)
    target = normalise(screen_profile)
    best = None
    for lo in np.linspace(0.0, 1.0, steps):
        for hi in np.linspace(lo, 1.0, steps):
            if hi - lo < 1e-3:
                continue
            sampled = np.interp(
                np.linspace(lo, hi, len(target)) * positions[-1],
                positions, video_profile)
            error = np.mean(np.abs(normalise(sampled) - target))
            if best is None or error < best[0]:
                best = (error, lo, hi)
    return best


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    screen_path, video_path = argv[1], argv[2]
    screen, screen_size = read_ppm(screen_path)
    video, video_size = read_ppm(video_path)
    print("screen %dx%d  video %dx%d" % (screen_size + video_size))

    col = fit_axis(screen.mean(axis=0), video.mean(axis=0))
    row = fit_axis(screen.mean(axis=1), video.mean(axis=1))
    print("column (u) fit: error=%.4f  u in [%.3f, %.3f]" % col)
    print("row    (v) fit: error=%.4f  v in [%.3f, %.3f]" % row)

    reference = np.interp(np.linspace(0, 1, screen.shape[1]),
                          np.linspace(0, 1, video.shape[1]),
                          video.mean(axis=0))
    print("(u=0..1 reference error %.4f)"
          % np.mean(np.abs(normalise(reference) - normalise(screen.mean(axis=0)))))
    if "--png" in argv:
        for path in (screen_path, video_path):
            out = path.rsplit(".", 1)[0] + ".png"
            Image.open(path).save(out)
            print("wrote", out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
