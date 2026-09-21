#!/usr/bin/env python3
"""Check that the HUD panel actually paints its content, and how big it is.

Renders one frame with the UI and one without, then reports the panel's bounding box
and how many pixels inside it are *not* the flat panel background -- which is what
"the panel is empty" means. Also reports the panel's share of the frame.

    python tools/panel_probe.py [--width 1920] [--height 1080] [--sunpath 1]
"""
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "Release", "heliostat_viz.exe")
OUT = os.path.join(ROOT, "out", "hud")


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


def shoot(name, extra):
    bmp = os.path.join(OUT, name + ".bmp")
    os.makedirs(OUT, exist_ok=True)
    # --no-beams / --no-cardinals: those are *scene* content with a time-based pulse,
    # so without them two separate runs differ outside the HUD as well.
    subprocess.run([EXE, "--frames", "60", "--spp", "256", "--fps", "0", "--no-beams",
                    "--no-cardinals", *extra,
                    "--screenshot", bmp, "--shot-frame", "50"], check=True, cwd=ROOT)
    return bmp


def main():
    args = sys.argv[1:]
    w_win = int(args[args.index("--width") + 1]) if "--width" in args else 1280
    h_win = int(args[args.index("--height") + 1]) if "--height" in args else 720
    path = ["--sunpath", args[args.index("--sunpath") + 1]] if "--sunpath" in args else []
    if path:
        # Pin the hour angle: an animating day path makes the two renders differ in the
        # *scene* as well (sun position -> spot), which would swamp the HUD diff.
        path += ["--sun-hour", "0"]
    ui = shoot("panel_on", ["--width", str(w_win), "--height", str(h_win), *path])
    off = shoot("panel_off", ["--width", str(w_win), "--height", str(h_win), "--no-ui", *path])
    w, h, a = read_bmp(ui)
    _, _, b = read_bmp(off)

    # The panel lives in the top-left quadrant; HUD pixels are the ones that change
    # when the UI is switched off. Isolated differences (a handful of pixels from the
    # animated scene) must not stretch the box, so rows/columns need a minimum count.
    x1, y1 = int(w * 0.6), int(h * 0.75)
    mask = [[max(abs(a[y][x][c] - b[y][x][c]) for c in range(3)) > 8 for x in range(x1)]
            for y in range(y1)]
    rows = [sum(1 for v in r if v) for r in mask]
    cols = [sum(1 for y in range(y1) if mask[y][x]) for x in range(x1)]
    keep_y = [y for y in range(y1) if rows[y] >= 4]
    keep_x = [x for x in range(x1) if cols[x] >= 4]
    if not keep_y or not keep_x:
        print("FAIL: the HUD draws nothing at all in the top-left quadrant")
        return 1
    bx0, bx1 = keep_x[0], keep_x[-1]
    by0, by1 = keep_y[0], keep_y[-1]
    hud = sum(rows[y] for y in keep_y)
    pw, ph = bx1 - bx0 + 1, by1 - by0 + 1

    # Inside the panel box: background is a near-constant dark blue-grey. Count the
    # pixels that differ from the box's modal colour -- that is the panel *content*.
    from collections import Counter
    cnt = Counter()
    for y in range(by0, by1 + 1):
        for x in range(bx0, bx1 + 1):
            r, g, bl = a[y][x]
            cnt[(r // 8, g // 8, bl // 8)] += 1
    bg, bg_n = cnt.most_common(1)[0]
    content = 0
    for y in range(by0, by1 + 1):
        for x in range(bx0, bx1 + 1):
            r, g, bl = a[y][x]
            if abs(r - bg[0] * 8) > 24 or abs(g - bg[1] * 8) > 24 or abs(bl - bg[2] * 8) > 24:
                content += 1
    total = pw * ph
    print(f"window {w}x{h}: panel box x{bx0}..{bx1} y{by0}..{by1}  ({pw}x{ph} px)")
    print(f"  panel area {100.0 * total / (w * h):.1f}% of the frame; "
          f"content pixels {content} = {100.0 * content / total:.1f}% of the box")
    ok = content > 3000 and 100.0 * content / total > 5.0
    print("PASS: the panel has text/sliders in it" if ok else
          "FAIL: the panel box is (nearly) empty")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
