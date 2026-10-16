#!/usr/bin/env python3
"""Prepare BrickGame's silent H.264 startup MP4 (requires FFmpeg on PATH)."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("game_root", type=Path)
parser.add_argument("--source", type=Path,
                    default=Path(__file__).resolve().parent.parent / "assets/splashvideo.mp4")
parser.add_argument("--ffmpeg", default="ffmpeg")
parser.add_argument("--crf", type=int, default=24)
parser.add_argument("--preset", default="veryfast")
parser.add_argument("--low-cpu", action="store_true",
                    help="two-pass baseline H.264, no B frames, one reference, two slices")
parser.add_argument("--bitrate", default="400k", help="average bitrate for --low-cpu")
parser.add_argument("--maxrate", default="440k", help="VBV maximum for --low-cpu")
parser.add_argument("--bufsize", default="800k", help="VBV buffer for --low-cpu")
parser.add_argument("--copy-video", action="store_true",
                    help="copy an already compatible H.264/yuv420p video without re-encoding")
args = parser.parse_args()
target = args.game_root / "res/drawable/splashvideo.mp4"
target.parent.mkdir(parents=True, exist_ok=True)
if args.source.resolve() == target.resolve():
    parser.error("source and output must be different files")
if args.copy_video and args.low_cpu:
    parser.error("--copy-video and --low-cpu cannot be combined")
if args.low_cpu:
    # Keep the supplied timestamps and speed. Slice threading matches the
    # startup decoder's two slice workers; baseline avoids CABAC/B-frame cost.
    with tempfile.TemporaryDirectory(prefix="brickgame-splash-") as work:
        command = [args.ffmpeg, "-hide_banner", "-y", "-i", str(args.source),
                   "-map", "0:v:0", "-an", "-c:v", "libx264", "-preset", args.preset,
                   "-profile:v", "baseline", "-bf", "0", "-refs", "1",
                   "-x264-params", "slices=2", "-b:v", args.bitrate,
                   "-maxrate", args.maxrate, "-bufsize", args.bufsize,
                   "-pix_fmt", "yuv420p", "-fps_mode", "passthrough",
                   "-enc_time_base", "demux", "-passlogfile", str(Path(work) / "pass")]
        subprocess.run([*command, "-pass", "1", "-f", "null", "-"], check=True)
        subprocess.run([*command, "-pass", "2", "-movflags", "+faststart", str(target)],
                       check=True)
    print(f"{target}: {target.stat().st_size} bytes")
    raise SystemExit(0)
video_args = (["-c:v", "copy"] if args.copy_video else
              ["-c:v", "libx264", "-preset", args.preset, "-crf", str(args.crf), "-pix_fmt", "yuv420p"])
subprocess.run([args.ffmpeg, "-hide_banner", "-y", "-i", str(args.source),
                "-map", "0:v:0", "-an", *video_args, "-movflags", "+faststart",
                str(target)], check=True)
print(f"{target}: {target.stat().st_size} bytes")
