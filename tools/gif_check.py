#!/usr/bin/env python3
"""tools/gif_check.py - check a re-recorded demo GIF against the previous one.

There is no image viewer on the build machine, so "did the new recording come out
right?" is answered numerically: both GIFs render the same fixed 27 s script
(`kDemoPhases` in viz/src/viz_main.cpp), so every phase must show the same
signature - mean luminance, bright-pixel share and HUD-panel contrast.

Frames are coalesced first, otherwise `magick file.gif[i]` returns the raw
optimised sub-frame instead of the composited picture and the numbers are junk.
Every sampled frame of the old recording is then matched to the *closest* frame of
the new one inside the same phase, because the two recordings sample the 27 s
timeline at different rates (298 vs 330 frames) and a phase can switch between
discrete views on a timer. Continuous phases match their own timestamp; discrete
ones match the same state; a phase that failed to record matches nothing and shows
up as a large distance.

    # the previous GIF is whatever is published, fetch it as raw bytes
    gh api -H "Accept: application/vnd.github.raw" \
        repos/AoiAoiP/diff_helio_viz/contents/docs/figs/demo.gif > out/old_demo.gif

    python tools/gif_check.py --old out/old_demo.gif --new docs/figs/demo.gif

Exit status is 0 when every phase matches, 1 otherwise.
"""

import argparse
import glob
import shutil
import statistics
import subprocess
import sys
from pathlib import Path

# Must match kDemoPhases in viz/src/viz_main.cpp.
PHASES = [
    (3.5, "1 sun sweep"),
    (3.0, "2 receiver closeup"),
    (3.5, "3 convergence t"),
    (3.0, "4 atomics A/B"),
    (2.0, "5 debug views"),
    (5.0, "6 four-mirror field"),
    (3.0, "7 flux inset"),
    (4.0, "8 Delingha day path"),
]
SAMPLES = 12          # per phase, spread over the phase with an 8% margin
WIDTH, HEIGHT = 320, 180
MAX_MEAN_DELTA = 6.0  # 0-255 scale, on sorted samples
MAX_PSD_DELTA = 8.0


def run(cmd):
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode != 0:
        raise SystemExit(f"{' '.join(str(c) for c in cmd)} failed:\n"
                         f"{r.stderr.decode('utf-8', 'replace')}")
    return r.stdout


def coalesce(gif, out_dir, tag):
    out_dir.mkdir(parents=True, exist_ok=True)
    run(["magick", str(gif), "-coalesce", str(out_dir / f"{tag}_%04d.png")])
    frames = sorted(glob.glob(str(out_dir / f"{tag}_*.png")))
    if not frames:
        raise SystemExit(f"{gif}: no frames after coalesce")
    return frames


def gray(path):
    px = run(["magick", path, "-resize", f"{WIDTH}x{HEIGHT}!", "-colorspace", "Gray",
              "-depth", "8", "gray:-"])
    if len(px) != WIDTH * HEIGHT:
        raise SystemExit(f"{path}: unexpected frame size ({len(px)} bytes)")
    return px


def signature(px):
    panel = [px[y * WIDTH + x] for y in range(0, int(HEIGHT * 0.8))
             for x in range(0, int(WIDTH * 0.52))]
    return statistics.fmean(px), statistics.pstdev(panel)


def phase_deltas(frames_a, frames_b, t0, t1):
    """Match each sampled frame of A to the closest frame of B inside the phase.

    Nearest-neighbour matching instead of same-timestamp pairing: a phase whose
    content is continuous (camera moves, sun sweeps) matches its own timestamp,
    while a phase that switches between a few discrete states (debug views)
    matches the same state even though the two recordings land on different sides
    of the switch. A phase that did not record at all has no close match anywhere
    in B, so its distance stays large.
    """
    lo, hi = t0 + 0.08 * (t1 - t0), t1 - 0.08 * (t1 - t0)

    def sample(frames, count):
        sigs = []
        for i in range(count):
            t = lo + (hi - lo) * i / (count - 1)
            sigs.append(signature(gray(frames[min(len(frames) - 1, int(t / 27.0 * len(frames)))])))
        return sigs

    ref, cand = sample(frames_a, SAMPLES), sample(frames_b, SAMPLES * 2)
    dmean, dpsd = [], []
    for ma, pa in ref:
        mb, pb = min(cand, key=lambda s: abs(s[0] - ma))
        dmean.append(abs(ma - mb))
        dpsd.append(abs(pa - pb))
    return statistics.median(dmean), statistics.median(dpsd)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--old", required=True, help="previously published GIF")
    ap.add_argument("--new", default="docs/figs/demo.gif", help="freshly recorded GIF")
    ap.add_argument("--tmp", default="out/gif_check_tmp")
    ap.add_argument("--keep", action="store_true", help="keep the coalesced frames")
    args = ap.parse_args()

    if not shutil.which("magick"):
        raise SystemExit("ImageMagick 'magick' not found on PATH")
    tmp = Path(args.tmp)
    if tmp.exists():
        shutil.rmtree(tmp, ignore_errors=True)
    old = coalesce(Path(args.old), tmp, "old")
    new = coalesce(Path(args.new), tmp, "new")
    print(f"{args.old}: {len(old)} frames   {args.new}: {len(new)} frames")
    print(f"{'phase':22s} {'median dmean':>13s} {'median dpsd':>12s}   verdict")

    t = 0.0
    failed = 0
    for duration, name in PHASES:
        dmean, dpsd = phase_deltas(old, new, t, t + duration)
        ok = dmean <= MAX_MEAN_DELTA and dpsd <= MAX_PSD_DELTA
        failed += 0 if ok else 1
        print(f"{name:22s} {dmean:13.2f} {dpsd:12.2f}   {'ok' if ok else 'MISMATCH'}")
        t += duration

    if not args.keep:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"\n{'PASS' if failed == 0 else f'FAIL: {failed} phase(s) differ'}"
          f"  (tolerance: mean {MAX_MEAN_DELTA}, panel sd {MAX_PSD_DELTA})")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
