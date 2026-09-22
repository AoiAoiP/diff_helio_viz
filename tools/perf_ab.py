#!/usr/bin/env python3
"""tools/perf_ab.py - re-measure the viewer's A/B switches the way the README quotes them.

The two heavy cases (--atomics, --reference-chain) hold the GPU at 100% for tens of
seconds, and on a laptop that is enough to move the clock: the same real-time kernel
has been measured at 0.357 / 0.478 / 0.496 ms and the reference chain at 62.31 /
62.49 / 63.72 ms, in the same session, with no code change in between. A single
sequential pass over the four cases therefore reports whatever clock state the
previous case left behind - that is exactly how an older sweep ended up quoting
78.0 ms for the reference chain.

So: interleave the cases over several rounds, pause in front of the heavy ones, and
report the minimum of each case (clocks only ever drop below the nominal boost, so
the minimum is the closest thing to "what the code costs"). The ratios are then
paired minima against minima, and the spread is printed next to them so nobody
quotes a single number without its band.

    python tools/perf_ab.py                 # 3 rounds, 1024 spp
    python tools/perf_ab.py --rounds 5 --frames 400

Everything is read back from the viewer's own end-of-run report (`--log`), so no
number here is typed by hand.
"""
import argparse
import csv
import os
import re
import statistics
import subprocess
import sys
import time

TAIL = re.compile(
    r"deform (?P<deform>[\d.]+) \| flux (?P<flux>[\d.]+) \| scene (?P<scene>[\d.]+) \| "
    r"bloom\.pre (?P<pre>[\d.]+) \| bloom\.down (?P<down>[\d.]+) \| bloom\.up (?P<up>[\d.]+) \| "
    r"composite (?P<comp>[\d.]+) \| total (?P<total>[\d.]+) ms")
WALL = re.compile(r"(\d+) frames, wall avg ([\d.]+) ms \(([\d.]+) FPS\)")
FLUX = re.compile(r"sum ([\d.]+) W, peak ([\d.]+) W/px, S95 ([\d.]+) m2")

# name, extra args, frames, pause before the run (s)
CASES = [
    ("lite", [], 900, 0),
    ("nocull", ["--no-cull"], 900, 0),
    ("atomics", ["--atomics"], 200, 8),
    ("reference", ["--reference-chain"], 200, 8),
]
BASE_ARGS = ["--present", "immediate", "--fps", "0", "--t", "1"]


def run_once(exe, out_dir, spp, name, extra, frames, pause, rnd):
    if pause:
        time.sleep(pause)
    log = os.path.join(out_dir, "logs", f"ab_{name}_r{rnd}.log")
    os.makedirs(os.path.dirname(log), exist_ok=True)
    cmd = [exe, *BASE_ARGS, "--spp", str(spp), "--frames", str(frames), *extra, "--log", log]
    subprocess.run(cmd, capture_output=True)
    text = open(log, encoding="utf-8", errors="replace").read()
    m, w, f = TAIL.search(text), WALL.search(text), FLUX.search(text)
    if not (m and w):
        return None
    row = {k: float(v) for k, v in m.groupdict().items()}
    row["wall"] = float(w.group(2))
    row["fps"] = float(w.group(3))
    if f:
        row["peak"] = float(f.group(2))
        row["s95"] = float(f.group(3))
    return row


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=os.path.join("build", "Release", "heliostat_viz.exe"))
    ap.add_argument("--out", default=os.path.join("out", "perf_ab.csv"))
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--frames", type=int, default=0, help="override the per-case frame count")
    ap.add_argument("--spp", type=int, default=1024)
    args = ap.parse_args()

    if not os.path.exists(args.exe):
        raise SystemExit(f"{args.exe} not found - build first (see README)")
    out_dir = os.path.dirname(os.path.abspath(args.out))

    samples = {name: [] for name, _, _, _ in CASES}
    for rnd in range(1, args.rounds + 1):
        for name, extra, frames, pause in CASES:
            row = run_once(args.exe, out_dir, args.spp, name, extra,
                           args.frames or frames, pause, rnd)
            if row:
                samples[name].append(row)
                print(f"  round {rnd}  {name:10s} flux {row['flux']:8.3f}  total "
                      f"{row['total']:8.3f}  wall {row['wall']:8.3f}  {row['fps']:7.1f} FPS  "
                      f"S95 {row.get('s95', 0):7.2f}")

    if not samples["lite"]:
        raise SystemExit("no usable runs - is the viewer starting?")

    base = min(x["flux"] for x in samples["lite"])
    rows = []
    print(f"\n{'case':10s} {'flux min':>9s} {'flux med':>9s} {'flux max':>9s} {'wall med':>9s} "
          f"{'total med':>10s} {'fps med':>8s} {'ratio':>7s}")
    for name, _, _, _ in CASES:
        s = samples[name]
        if not s:
            continue
        flux = [x["flux"] for x in s]
        wall = statistics.median(x["wall"] for x in s)
        total = statistics.median(x["total"] for x in s)
        fps = statistics.median(x["fps"] for x in s)
        rows.append(dict(case=name, flux_min=min(flux), flux_med=statistics.median(flux),
                         flux_max=max(flux), wall_med=wall, total_med=total, fps_med=fps,
                         ratio=min(flux) / base, rounds=len(s)))
        print(f"{name:10s} {min(flux):9.3f} {statistics.median(flux):9.3f} {max(flux):9.3f} "
              f"{wall:9.3f} {total:10.3f} {fps:8.1f} {min(flux) / base:6.2f}x")

    os.makedirs(out_dir, exist_ok=True)
    with open(args.out, "w", newline="", encoding="utf-8") as fh:
        fh.write(f"# {args.spp} spp, {args.rounds} interleaved rounds, "
                 f"--t 1 --present immediate --fps 0, ratios paired on flux_min\n")
        wr = csv.DictWriter(fh, fieldnames=["case", "flux_min", "flux_med", "flux_max",
                                            "wall_med", "total_med", "fps_med", "ratio", "rounds"])
        wr.writeheader()
        wr.writerows(rows)
    print(f"\nwrote {args.out}   (flux 列取最小值，wall/total 取中位；比率 = flux_min / lite 的 flux_min)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
