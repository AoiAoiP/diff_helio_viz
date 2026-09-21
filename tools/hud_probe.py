#!/usr/bin/env python3
"""Where does the HUD actually draw? Diff a --no-ui frame against a normal one.

Region readouts (darkness thresholds, colour counts) are unreliable here because
the panel is translucent over a bright sky, so instead of guessing what a HUD
pixel looks like we take the *difference*: pixels that change when the HUD is
switched off are exactly the pixels the HUD owns.

    python tools/hud_probe.py            # renders both frames itself
    python tools/hud_probe.py a.bmp b.bmp   # with-UI, without-UI
"""
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "Release", "heliostat_viz.exe")


def read_bmp(path):
    with open(path, "rb") as f:
        d = f.read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    stride = ((w * bpp // 8) + 3) & ~3
    rows = []
    for y in range(h):
        base = off + y * stride
        row = []
        for x in range(w):
            o = base + x * bpp // 8
            b, g, r = d[o], d[o + 1], d[o + 2]
            row.append((r, g, b))
        rows.append(row)
    rows.reverse()
    return w, h, rows


def shoot(name, extra):
    bmp = os.path.join(ROOT, "out", "hud", name + ".bmp")
    os.makedirs(os.path.dirname(bmp), exist_ok=True)
    subprocess.run([EXE, "--frames", "80", "--spp", "1024", "--t", "1", "--fps", "0",
                    "--screenshot", bmp, "--shot-frame", "70", *extra], check=True, cwd=ROOT)
    return bmp


def main(argv):
    if len(argv) >= 3:
        a, b = argv[1], argv[2]
    else:
        a, b = shoot("with_ui", []), shoot("no_ui", ["--no-ui"])
    w, h, ui = read_bmp(a)
    w2, h2, ref = read_bmp(b)
    assert (w, h) == (w2, h2), "size mismatch"

    mask = [[max(abs(ui[y][x][c] - ref[y][x][c]) for c in range(3)) > 8 for x in range(w)]
            for y in range(h)]
    total = sum(sum(1 for v in row if v) for row in mask)
    bx0, by0, bx1, by1 = w, h, -1, -1
    for y in range(h):
        for x in range(w):
            if mask[y][x]:
                bx0 = min(bx0, x); by0 = min(by0, y); bx1 = max(bx1, x); by1 = max(by1, y)
    print(f"HUD pixels: {total} of {w * h} ({100.0 * total / (w * h):.1f}%), "
          f"bbox x {bx0}..{bx1}, y {by0}..{by1}")

    regions = {
        "panel          (top-left)": (0, 0, 490, 360),
        "help strip     (bottom-left)": (0, h - 90, 620, h),
        "compass        (top-right)": (1120, 0, w, 200),
        "flux inset     (bottom-right)": (880, h - 160, w, h),
        "centre         (must be empty)": (500, 200, 880, 520),
    }
    for name, (x0, y0, x1, y1) in regions.items():
        n = hit = 0
        rx0, ry0, rx1, ry1 = x1, y1, -1, -1
        for y in range(y0, y1):
            for x in range(x0, x1):
                n += 1
                if mask[y][x]:
                    hit += 1
                    rx0 = min(rx0, x); ry0 = min(ry0, y); rx1 = max(rx1, x); ry1 = max(ry1, y)
        bbox = f"bbox x {rx0}..{rx1} y {ry0}..{ry1}" if hit else "empty"
        print(f"  {name:32s} {hit:7d}/{n:<7d} = {100.0 * hit / n:5.1f}%   {bbox}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
