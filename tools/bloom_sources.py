#!/usr/bin/env python3
"""Show what bloomThr selects: the pixels that are allowed to glow.

Renders the same frame twice per threshold -- bloom off, then bloom at strength 1.5 --
and marks the pixels whose colour actually changed. Those pixels are the bright-pass
sources plus the halo they were smeared into, so the picture answers "what does the
threshold let through?" directly.

    python tools/bloom_sources.py            -> docs/figs/bloom_sources.png
"""
import os
import struct
import subprocess
import sys

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "Release", "heliostat_viz.exe")
OUT = os.path.join(ROOT, "out", "bloom")
FIG = os.path.join(ROOT, "docs", "figs", "bloom_sources.png")


def shoot(name, extra):
    bmp = os.path.join(OUT, name + ".bmp")
    os.makedirs(OUT, exist_ok=True)
    subprocess.run([EXE, "--frames", "70", "--spp", "512", "--fps", "0", "--preset", "0",
                    "--no-ui", "--no-cardinals", *extra,
                    "--screenshot", bmp, "--shot-frame", "60"], check=True, cwd=ROOT)
    return bmp


def read_bmp(path):
    with open(path, "rb") as f:
        d = f.read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    stride = ((w * bpp // 8) + 3) & ~3
    n = bpp // 8
    rows = []
    for y in range(h):
        base = off + y * stride
        rows.append([(d[base + x * n + 2], d[base + x * n + 1], d[base + x * n]) for x in range(w)])
    rows.reverse()
    return w, h, rows


def main():
    os.makedirs(OUT, exist_ok=True)
    ref = read_bmp(shoot("src_ref", ["--bloom", "0"]))
    panels = []
    for thr in ("1.0", "2.5", "6.0"):
        img = read_bmp(shoot("src_" + thr.replace(".", ""), ["--bloom", "1.5", "--bloom-thr", thr]))
        w, h, _ = ref
        lit = 0
        out = Image.new("RGB", (w, h))
        px = out.load()
        for y in range(h):
            for x in range(w):
                r, g, b = ref[2][y][x]
                d = max(abs(img[2][y][x][c] - ref[2][y][x][c]) for c in range(3))
                if d > 10:
                    lit += 1
                    # mark the bloom-lit pixel in green over a dimmed reference
                    px[x, y] = (40, 235, 90) if d > 40 else (20, 120, 50)
                else:
                    px[x, y] = (r // 3, g // 3, b // 3)
        pct = 100.0 * lit / (w * h)
        panels.append((out.resize((640, 360), Image.LANCZOS),
                       f"bloomThr = {thr}   strength 1.5   |   green = pixels that received bloom: "
                       f"{lit} ({pct:.2f}% of the frame)"))
        print(f"bloomThr {thr}: {lit} pixels lit up ({pct:.2f}%)")

    bar = 22
    fig = Image.new("RGB", (640, (360 + bar) * len(panels)), (14, 16, 22))
    d = ImageDraw.Draw(fig)
    for i, (im, cap) in enumerate(panels):
        y = i * (360 + bar)
        fig.paste(im, (0, y + bar))
        d.text((6, y + 6), cap, fill=(235, 238, 245))
    fig.save(FIG)
    print("wrote", os.path.relpath(FIG, ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
