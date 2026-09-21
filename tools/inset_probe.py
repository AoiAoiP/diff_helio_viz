#!/usr/bin/env python3
"""Check that the flux-map inset draws the spot in the middle of the panel.

Runs the viewer once per field mirror with --no-ui (only the inset is drawn),
then measures the luminance-weighted centroid of the spot inside the inset
rectangle and compares it with the rectangle centre.

    python tools/inset_probe.py
"""
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "Release", "heliostat_viz.exe")
OUT = os.path.join(ROOT, "out", "inset")


def read_bmp(path):
    with open(path, "rb") as f:
        data = f.read()
    off = struct.unpack_from("<I", data, 10)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    stride = ((w * bpp // 8) + 3) & ~3
    rows = []
    for y in range(h):
        base = off + y * stride
        row = []
        for x in range(w):
            o = base + x * bpp // 8
            b, g, r = data[o], data[o + 1], data[o + 2]
            row.append((r / 255.0, g / 255.0, b / 255.0))
        rows.append(row)
    rows.reverse()
    return w, h, rows


def inset_rect(w, h):
    """Must match SceneRenderer::fluxInsetRect."""
    mw = min(360.0, w * 0.28)
    mh = mw * 50.0 / 157.0
    return int(w - mw - 16.0), int(h - mh - 16.0), int(mw), int(mh)


def main():
    os.makedirs(OUT, exist_ok=True)
    # --raw measures the un-centred map (--no-inset-centre): that is the "before"
    # state, where a spot straddling the u = 0 seam is drawn cut in half.
    raw = "--raw" in sys.argv
    extra = ["--no-inset-centre"] if raw else []
    tag = "raw" if raw else "centred"
    names = ["North", "East", "South", "West"]
    print(f"{'mirror':8s} {'inset x,y':>12s} {'inset size':>11s} {'spot (px)':>16s} "
          f"{'offset px':>14s} {'peak offset':>13s} {'spread x':>9s} {'blobs':>6s}")
    worst = 0.0
    worst_peak = 0.0
    worst_spread = 0.0
    blobs_total = 0
    for m, name in enumerate(names):
        bmp = os.path.join(OUT, f"mirror{m}_{tag}.bmp")
        # No --no-ui: that flag also disables the inset itself. The panel lives in
        # the top-left and the caption sits *below* the rect, so the rect holds only
        # the inset (plus nothing else).
        subprocess.run([EXE, "--frames", "90", "--spp", "1024", "--t", "1", "--mirror", str(m),
                        "--fps", "0", *extra,
                        "--screenshot", bmp, "--shot-frame", "80"], check=True, cwd=ROOT)
        w, h, px = read_bmp(bmp)
        rx, ry, rw, rh = inset_rect(w, h)
        sx = sy = sw = 0.0
        peak = -1.0
        peak_x = peak_y = 0
        weights = {}
        for y in range(ry + 2, ry + rh - 2):          # skip the 1 px frame
            for x in range(rx + 2, rx + rw - 2):
                r, g, b = px[y][x]
                lum = 0.2126 * r + 0.7152 * g + 0.0722 * b
                weight = max(0.0, lum - 0.10) ** 3
                if weight <= 0.0:
                    continue
                sx += weight * x
                sy += weight * y
                sw += weight
                weights[(x, y)] = weight
                if weight > peak:
                    peak = weight
                    peak_x, peak_y = x, y
        cx, cy = sx / sw, sy / sw
        # Flux-weighted horizontal spread (texels) and the number of connected
        # bright blobs: a spot cut by the seam is *two* blobs at the two edges, and
        # its centroid still looks centred, so the blob count is what proves it.
        var = sum(wt * (x - cx) ** 2 for (x, _), wt in weights.items()) / sw
        spread = (var ** 0.5) * 157.0 / rw
        # The decisive metric is the horizontal spread: a spot split by the seam is
        # bimodal (half at each edge) and still has a *centred* centroid, but its
        # spread explodes. The blob count is reported for information only -- the
        # white S95 contour drawn over the map breaks the core into several
        # components, so it is not a reliable pass/fail signal.
        thresh = 0.50 * peak
        seen = set()
        blobs = 0
        for key in [k for k, wt in weights.items() if wt >= thresh]:
            if key in seen:
                continue
            blobs += 1
            stack = [key]
            seen.add(key)
            while stack:
                qx, qy = stack.pop()
                for n in ((qx + 1, qy), (qx - 1, qy), (qx, qy + 1), (qx, qy - 1)):
                    if n in weights and n not in seen and weights[n] >= thresh:
                        seen.add(n)
                        stack.append(n)
        ex = cx - (rx + rw * 0.5)
        ey = cy - (ry + rh * 0.5)
        pex = peak_x - (rx + rw * 0.5)
        worst = max(worst, abs(ex), abs(ey))
        worst_peak = max(worst_peak, abs(pex))
        worst_spread = max(worst_spread, spread)
        blobs_total = max(blobs_total, blobs)
        print(f"{name:8s} {rx:5d},{ry:4d} {rw:5d}x{rh:3d} {cx:8.1f},{cy:6.1f} "
              f"{ex:+7.1f},{ey:+6.1f} {pex:+12.1f} {spread:9.1f} {blobs:6d}")
    print(f"\nworst centroid |offset| = {worst:.1f} px, worst peak |offset| = {worst_peak:.1f} px, "
          f"worst horizontal spread = {worst_spread:.1f} texels of 157, blobs <= {blobs_total}")
    ok = worst <= 6.0 and worst_spread <= 25.0
    print("PASS: one centred spot in the panel for every mirror" if ok else
          "FAIL: the spot is off-centre and/or split across the seam")
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
