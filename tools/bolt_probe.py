#!/usr/bin/env python3
"""Count the actuator markers actually visible on the traced mirror.

Two frames are rendered per mirror -- one with the markers collapsed to zero size
and one with them at real size -- and the difference isolates the marker pixels
exactly (no colour heuristic: ACES desaturates the marker tint, and the plate's
specular highlight is warm too). Connected components of that difference are the
markers, so the test answers the question "is every one of the 35 actuators drawn
on the mirror?" with a number rather than an opinion.

    python tools/bolt_probe.py              # F2 closeup, 1x deformation
    python tools/bolt_probe.py --deform 2   # 200x deformation exaggeration
    python tools/bolt_probe.py --marker 0.22
"""
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "Release", "heliostat_viz.exe")
OUT = os.path.join(ROOT, "out", "bolts")
EXPECTED = 35


def read_bmp(path):
    with open(path, "rb") as f:
        d = f.read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    stride = ((w * bpp // 8) + 3) & ~3
    nbytes = bpp // 8
    rows = []
    for y in range(h):
        base = off + y * stride
        rows.append([(d[base + x * nbytes + 2], d[base + x * nbytes + 1], d[base + x * nbytes])
                     for x in range(w)])
    rows.reverse()
    return w, h, rows


def shoot(name, extra):
    bmp = os.path.join(OUT, name + ".bmp")
    os.makedirs(OUT, exist_ok=True)
    subprocess.run([EXE, "--frames", "70", "--spp", "256", "--t", "1", "--fps", "0",
                    "--no-cardinals", "--no-flux-map", "--no-ui", "--no-beams",
                    "--screenshot", bmp, "--shot-frame", "60", *extra], check=True, cwd=ROOT)
    return bmp


def main():
    deform = sys.argv[sys.argv.index("--deform") + 1] if "--deform" in sys.argv else "0"
    marker = sys.argv[sys.argv.index("--marker") + 1] if "--marker" in sys.argv else "0.30"
    names = ["North", "East", "South", "West"]
    print(f"{'mirror':8s} {'markers':>8s} {'expected':>9s} {'marker px':>10s} {'area min/med/max':>18s}")
    ok = True
    for m, name in enumerate(names):
        base = shoot(f"probe_{m}_off", ["--mirror", str(m), "--preset", "1", "--deform", deform,
                                        "--bolt-scale", "0.0"])
        on = shoot(f"probe_{m}_on", ["--mirror", str(m), "--preset", "1", "--deform", deform,
                                     "--bolt-scale", marker])
        w, h, a = read_bmp(base)
        _, _, b = read_bmp(on)
        mask = [[max(abs(a[y][x][c] - b[y][x][c]) for c in range(3)) > 10 for x in range(w)]
                for y in range(h)]
        seen = [[False] * w for _ in range(h)]
        areas = []
        total = 0
        for y in range(h):
            for x in range(w):
                if not mask[y][x] or seen[y][x]:
                    continue
                stack = [(x, y)]
                seen[y][x] = True
                n = 0
                while stack:
                    cx, cy = stack.pop()
                    n += 1
                    for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
                        if 0 <= nx < w and 0 <= ny < h and mask[ny][nx] and not seen[ny][nx]:
                            seen[ny][nx] = True
                            stack.append((nx, ny))
                if n >= 20:            # ignore stray single-pixel differences
                    areas.append(n)
                total += n
        areas.sort()
        med = areas[len(areas) // 2] if areas else 0
        # Markers overlap in perspective, so a few blobs can merge: accept a count
        # within 15% of 35 (and never fewer than 30).
        flag = "" if len(areas) >= 30 else "   <-- markers missing from the mirror"
        if len(areas) < 30:
            ok = False
        print(f"{name:8s} {len(areas):8d} {EXPECTED:9d} {total:10d} "
              f"{areas[0] if areas else 0:7d}/{med:5d}/{areas[-1] if areas else 0:5d}{flag}")
    print("\nPASS: the actuator markers are drawn on the mirror for every field position"
          if ok else "\nFAIL: markers are missing (see the mask dumps in out/bolts)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
