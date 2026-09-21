#!/usr/bin/env python3
"""tools/make_gif.py — assemble the recorded BMP frames into the README GIF.

Frames come from `heliostat_viz --demo 27 --record out/gif --record-stride N`.
Requires ImageMagick (`magick`) on PATH for the palette pass; the file-size guard
re-runs with fewer frames / smaller size until the GIF fits the target budget.

    python tools/make_gif.py --frames out/gif --out docs/figs/demo.gif --mb 12
"""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def run(cmd):
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        print(proc.stdout)
        print(proc.stderr)
        raise SystemExit(f"command failed: {' '.join(str(c) for c in cmd)}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", default="out/gif")
    ap.add_argument("--out", default="docs/figs/demo.gif")
    ap.add_argument("--fps", type=int, default=16)
    ap.add_argument("--width", type=int, default=640)
    ap.add_argument("--mb", type=float, default=12.0)
    args = ap.parse_args()

    frame_dir = Path(args.frames)
    out = Path(args.out)
    frames = sorted(frame_dir.glob("frame_*.bmp"))
    if not frames:
        raise SystemExit(f"no frames in {frame_dir}")
    out.parent.mkdir(parents=True, exist_ok=True)
    magick = shutil.which("magick")
    if not magick:
        raise SystemExit("ImageMagick 'magick' not found on PATH")

    step = 1
    fps = args.fps
    width = args.width
    for attempt in range(4):
        picked = frames[::step]
        print(f"attempt {attempt + 1}: {len(picked)} frames, {fps} fps, {width}px wide")
        run([
            magick, "-delay", str(int(100 / fps)),
            *[str(p) for p in picked],
            "-resize", f"{width}x",
            "-layers", "Optimize",
            "-loop", "0",
            str(out),
        ])
        size_mb = out.stat().st_size / (1024 * 1024)
        print(f"  -> {out} {size_mb:.2f} MB")
        if size_mb <= args.mb:
            print(f"OK: {out} ({size_mb:.2f} MB, {len(picked)} frames)")
            return 0
        # Too big: keep the same duration by halving the frame rate and size.
        step *= 2
        fps = max(8, fps // 2)
        width = max(360, int(width * 0.8))
    print("WARNING: still over the size budget")
    return 1


if __name__ == "__main__":
    sys.exit(main())
