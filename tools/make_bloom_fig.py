#!/usr/bin/env python3
"""Build docs/figs/bloom_threshold.png - why a high bloom strength produced blobs.

Runs the viewer three times on the same frame (bloom off / threshold 1.0 / 2.5),
measures how many pixels receive extra light, and stacks the crops with captions.

Usage: python tools/make_bloom_fig.py
"""
import os
import struct
import subprocess
import sys

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "Release", "heliostat_viz.exe")
OUTDIR = os.path.join(ROOT, "out", "bloom")
FIG = os.path.join(ROOT, "docs", "figs", "bloom_threshold.png")


def luma(path):
    img = Image.open(path).convert("RGB")
    w, h = img.size
    px = img.load()
    out = [[0.0] * w for _ in range(h)]
    for y in range(h):
        row = out[y]
        for x in range(w):
            r, g, b = px[x, y]
            row[x] = (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255.0
    return w, h, out


def shoot(name, extra):
    bmp = os.path.join(OUTDIR, name + ".bmp")
    args = [EXE, "--screenshot", bmp, "--shot-frame", "120", "--frames", "200",
            "--preset", "0", "--no-ui"] + extra
    subprocess.run(args, check=True, cwd=ROOT)
    return bmp


def main():
    os.makedirs(OUTDIR, exist_ok=True)
    cases = [
        ("bloom_off", ["--bloom", "0"], "bloom OFF (reference)"),
        ("bloom_thr10", ["--bloom", "1.5", "--bloom-thr", "1.0"],
         "strength 1.5, threshold 1.0 (old hard-coded default)"),
        ("bloom_thr25", ["--bloom", "1.5", "--bloom-thr", "2.5"],
         "strength 1.5, threshold 2.5 (new default)"),
    ]
    shots = [(n, shoot(n, e), cap) for n, e, cap in cases]
    _, _, ref = luma(shots[0][1])

    panels = []
    for name, bmp, cap in shots:
        w, h, img = luma(bmp)
        added = sum(1 for y in range(h) for x in range(w) if img[y][x] - ref[y][x] > 0.05)
        strong = sum(1 for y in range(h) for x in range(w) if img[y][x] - ref[y][x] > 0.15)
        pct = 100.0 * added / (w * h)
        im = Image.open(bmp).convert("RGB").resize((640, 360), Image.LANCZOS)
        panels.append((im, f"{cap}   |   pixels lit up by bloom: {added} ({pct:.1f}% of frame), "
                            f"of which strong: {strong}"))
        print(f"{name:14s} added={added:7d} ({pct:5.2f}%)  strong={strong:6d}")

    bar = 22
    W, H = 640, (360 + bar) * len(panels)
    fig = Image.new("RGB", (W, H), (14, 16, 22))
    d = ImageDraw.Draw(fig)
    for i, (im, cap) in enumerate(panels):
        y = i * (360 + bar)
        fig.paste(im, (0, y + bar))
        d.text((6, y + 6), cap, fill=(235, 238, 245))
    fig.save(FIG)
    print("wrote", os.path.relpath(FIG, ROOT))
    print("note: the sun disc and the receiver core are the only emitters above 2.5;")
    print("      at threshold 1.0 the whole sky gradient passes the bright pass instead.")


if __name__ == "__main__":
    sys.exit(main())
