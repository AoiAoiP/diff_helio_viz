#!/usr/bin/env python3
"""Measure how much of the frame glows, to explain "why do bright blobs appear".

Reads one or more screenshots produced by heliostat_viz --screenshot and reports,
for each: the mean sky luminance (top band), the number of pixels above a couple
of luminance levels, and the size of the largest connected bright blob.

Usage:  python tools/bloom_probe.py shot_a.bmp shot_b.bmp ...
"""
import struct
import sys


def read_bmp(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"BM":
        raise ValueError(f"{path}: not a BMP")
    pix_off = struct.unpack_from("<I", data, 10)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    if bpp not in (24, 32):
        raise ValueError(f"{path}: unsupported bpp {bpp}")
    stride = ((w * bpp // 8) + 3) & ~3
    rows = []
    for y in range(h):
        base = pix_off + y * stride
        row = []
        for x in range(w):
            o = base + x * bpp // 8
            b, g, r = data[o], data[o + 1], data[o + 2]
            row.append((0.0722 * b + 0.7152 * g + 0.2126 * r) / 255.0)
        rows.append(row)
    rows.reverse()  # BMP rows are bottom-up -> top-left origin
    return w, h, rows


def stats(w, h, rows, band_frac=0.30):
    band = rows[: max(1, int(h * band_frac))]
    sky_mean = sum(sum(r) for r in band) / (len(band) * w)
    counts = {t: 0 for t in (0.50, 0.75, 0.90)}
    total = 0.0
    for row in rows:
        for v in row:
            total += v
            for t in counts:
                if v >= t:
                    counts[t] += 1
    # largest connected bright blob (simple 2-pass flood fill on a subsampled grid)
    seen = [[False] * w for _ in range(h)]
    best = 0
    step = 2
    for y in range(0, h, step):
        for x in range(0, w, step):
            if seen[y][x] or rows[y][x] < 0.50:
                continue
            stack = [(x, y)]
            seen[y][x] = True
            n = 0
            while stack:
                cx, cy = stack.pop()
                n += 1
                for dx, dy in ((step, 0), (-step, 0), (0, step), (0, -step)):
                    nx, ny = cx + dx, cy + dy
                    if 0 <= nx < w and 0 <= ny < h and not seen[ny][nx] and rows[ny][nx] >= 0.50:
                        seen[ny][nx] = True
                        stack.append((nx, ny))
            best = max(best, n)
    return sky_mean, counts, total / (w * h), best


def main(paths):
    if paths[0] == "--diff":
        ref_w, ref_h, ref = read_bmp(paths[1])
        print(f"reference: {paths[1]}  (bloom strength 0 = the same frame with bloom off)")
        print(f"{'file':30s} {'add>0.05':>9s} {'add>0.15':>9s} {'max add':>8s} {'mean add':>9s}")
        for p in paths[2:]:
            import os
            w, h, rows = read_bmp(p)
            if (w, h) != (ref_w, ref_h):
                raise ValueError("size mismatch")
            c5 = c15 = 0
            mx = 0.0
            tot = 0.0
            for y in range(h):
                for x in range(w):
                    d = rows[y][x] - ref[y][x]
                    if d > 0.05:
                        c5 += 1
                    if d > 0.15:
                        c15 += 1
                    if d > mx:
                        mx = d
                    tot += d
            print(f"{os.path.basename(p):30s} {c5:9d} {c15:9d} {mx:8.3f} {tot / (w * h):9.4f}")
        return
    print(f"{'file':34s} {'sky mean':>9s} {'>0.50':>9s} {'>0.75':>9s} {'>0.90':>9s} "
          f"{'frame mean':>10s} {'blob px':>9s}")
    for p in paths:
        w, h, rows = read_bmp(p)
        sky, counts, mean, blob = stats(w, h, rows)
        import os
        print(f"{os.path.basename(p):34s} {sky:9.4f} {counts[0.50]:9d} {counts[0.75]:9d} "
              f"{counts[0.90]:9d} {mean:10.4f} {blob:9d}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        raise SystemExit(2)
    main(sys.argv[1:])
