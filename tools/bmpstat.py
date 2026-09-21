#!/usr/bin/env python3
"""BMP statistics for verifying a render without an image viewer.

Usage:
  python tools/bmpstat.py out/frame.bmp            # global + 6x6 region table
  python tools/bmpstat.py out/frame.bmp --rows 8   # band luminance profile
  python tools/bmpstat.py a.bmp b.bmp --diff       # mean abs difference

The tool exists because the renderer must be checkable from a terminal: it
reports region mean RGB, luminance, saturation and the fraction of bright
pixels, which is enough to tell a sky gradient from a flat colour, a blown-out
spot from an empty receiver, and to spot regressions.
"""
import struct
import sys
import math


def load_bmp(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[0:2] != b"BM":
        raise ValueError(f"{path}: not a BMP")
    offset = struct.unpack_from("<I", data, 10)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    if bpp != 24:
        raise ValueError(f"{path}: expected 24bpp, got {bpp}")
    flip = h > 0
    h = abs(h)
    row_bytes = w * 3
    padding = (4 - row_bytes % 4) % 4
    stride = row_bytes + padding
    px = [[None] * w for _ in range(h)]
    for y in range(h):
        src = offset + y * stride
        row = data[src:src + row_bytes]
        ty = (h - 1 - y) if flip else y
        for x in range(w):
            b, g, r = row[x * 3], row[x * 3 + 1], row[x * 3 + 2]
            px[ty][x] = (r, g, b)
    return w, h, px


def lum(c):
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]


def region_stats(px, x0, y0, x1, y1):
    r = g = b = 0.0
    n = 0
    bright = 0
    maxl = 0.0
    for y in range(y0, y1):
        for x in range(x0, x1):
            c = px[y][x]
            r += c[0]
            g += c[1]
            b += c[2]
            l = lum(c)
            maxl = max(maxl, l)
            if l > 200:
                bright += 1
            n += 1
    if n == 0:
        return (0, 0, 0, 0, 0, 0)
    return (r / n, g / n, b / n, maxl, bright / n, n)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = [a for a in sys.argv[1:] if a.startswith("--")]
    if not args:
        print(__doc__)
        return 1
    if "--diff" in flags:
        if len(args) < 2:
            print("--diff needs two files")
            return 1
        w1, h1, a = load_bmp(args[0])
        w2, h2, b = load_bmp(args[1])
        if (w1, h1) != (w2, h2):
            print(f"size mismatch: {w1}x{h1} vs {w2}x{h2}")
            return 1
        tot = 0.0
        worst = 0
        for y in range(h1):
            for x in range(w1):
                d = max(abs(a[y][x][i] - b[y][x][i]) for i in range(3))
                tot += d
                worst = max(worst, d)
        print(f"{args[0]} vs {args[1]}: mean abs diff {tot / (w1 * h1):.3f}, max {worst}")
        return 0

    rows = 8
    if "--rows" in flags:
        rows = int(args[1])
        args = args[:1] + args[2:]
    path = args[0]
    w, h, px = load_bmp(path)
    tot = region_stats(px, 0, 0, w, h)
    print(f"{path}: {w}x{h}")
    print(f"  overall: mean RGB ({tot[0]:.1f}, {tot[1]:.1f}, {tot[2]:.1f})  lum {lum(tot[:3]):.1f}  "
          f"max lum {tot[3]:.0f}  bright frac {tot[4] * 100:.2f}%")

    print("  band profile (top -> bottom): row-band  meanRGB            lum   bright%")
    for i in range(rows):
        y0 = h * i // rows
        y1 = h * (i + 1) // rows
        s = region_stats(px, 0, y0, w, y1)
        print(f"    {i:2d} y[{y0:4d}:{y1:4d}]  ({s[0]:6.1f},{s[1]:6.1f},{s[2]:6.1f})  {lum(s[:3]):6.1f}  "
              f"{s[4] * 100:6.2f}")

    cols = 6
    print("  region grid (mean luminance):")
    for j in range(4):
        y0 = h * j // 4
        y1 = h * (j + 1) // 4
        line = []
        for i in range(cols):
            x0 = w * i // cols
            x1 = w * (i + 1) // cols
            s = region_stats(px, x0, y0, x1, y1)
            line.append(f"{lum(s[:3]):6.1f}")
        print("    " + " ".join(line))
    return 0


if __name__ == "__main__":
    sys.exit(main())
