#!/usr/bin/env python3
"""What does bloom actually do? Same frame with bloom off / default / strong.

Crops the receiver spot region so the difference is visible, and prints how much light
bloom added (per-pixel maximum and the share of pixels that changed).

    python tools/bloom_effect.py    -> docs/figs/bloom_effect.png
"""
import os
import struct
import subprocess
import sys

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "Release", "heliostat_viz.exe")
OUT = os.path.join(ROOT, "out", "bloom")
FIG = os.path.join(ROOT, "docs", "figs", "bloom_effect.png")
# Receiver closeup: this is where the spot's glow matters most.
CROP = (int(1280 * 0.28), int(720 * 0.18), int(1280 * 0.72), int(720 * 0.82))


def shoot(name, extra):
    bmp = os.path.join(OUT, name + ".bmp")
    os.makedirs(OUT, exist_ok=True)
    subprocess.run([EXE, "--frames", "70", "--spp", "512", "--fps", "0", "--preset", "2",
                    "--no-ui", "--no-cardinals", "--no-beams", *extra,
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


def luma(c):
    return (0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]) / 255.0


def main():
    ref = read_bmp(shoot("eff_off", ["--bloom", "0"]))
    cases = [
        ("bloom OFF", ["--bloom", "0"]),
        ("bloom 0.55 / thr 2.5 (default)", ["--bloom", "0.55", "--bloom-thr", "2.5"]),
        ("bloom 1.50 / thr 2.5 (overdone)", ["--bloom", "1.5", "--bloom-thr", "2.5"]),
    ]
    panels = []
    for i, (cap, extra) in enumerate(cases):
        img = ref if i == 0 else read_bmp(shoot("eff_%d" % i, extra))
        w, h, _ = img
        added = 0.0
        mx = 0.0
        changed = 0
        for y in range(h):
            for x in range(w):
                d = luma(img[2][y][x]) - luma(ref[2][y][x])
                if d > 0.01:
                    changed += 1
                    added += d
                    mx = max(mx, d)
        mean = added / (w * h)
        pct = 100.0 * changed / (w * h)
        im = Image.open(os.path.join(OUT, ("eff_off.bmp" if i == 0 else "eff_%d.bmp" % i))).convert("RGB")
        panels.append((im.crop(CROP).resize((640, 420), Image.LANCZOS),
                       f"{cap}   |   brighter pixels: {changed} ({pct:.2f}% of frame), "
                       f"mean +{mean:.4f}, max +{mx:.3f}"))
        print(f"{cap:34s} changed {changed:7d} ({pct:5.2f}%)  mean +{mean:.4f}  max +{mx:.3f}")

    bar = 22
    fig = Image.new("RGB", (640, (420 + bar) * len(panels)), (14, 16, 22))
    d = ImageDraw.Draw(fig)
    for i, (im, cap) in enumerate(panels):
        y = i * (420 + bar)
        fig.paste(im, (0, y + bar))
        d.text((6, y + 6), cap, fill=(235, 238, 245))
    fig.save(FIG)
    print("wrote", os.path.relpath(FIG, ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
