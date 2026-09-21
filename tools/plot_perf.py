#!/usr/bin/env python3
"""tools/plot_perf.py — turn the viewer's performance CSV into the deliverables.

Reads docs/perf_viewer.csv (written by tools/perf_sweep.ps1) and produces:

  docs/figs/frametime_vs_spp.png   frame time (GPU per pass + total) vs spp
  docs/figs/ab_switches.png        A/B switch comparison (atomics is the story)
  docs/figs/pass_breakdown.png     stacked per-pass GPU time at 1024 spp

Every number in the plots comes from a measured run; nothing is modelled.
"""

import csv
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent


def load(path):
    rows = []
    with open(path, newline="", encoding="utf-8-sig") as f:
        for r in csv.DictReader(f):
            if not r.get("case"):
                continue
            for k in list(r.keys()):
                if k in ("case",):
                    continue
                try:
                    r[k] = float(r[k])
                except (TypeError, ValueError):
                    pass
            rows.append(r)
    return rows


def main():
    src = ROOT / "docs" / "perf_viewer.csv"
    if len(sys.argv) > 1:
        src = Path(sys.argv[1])
    rows = load(src)
    out = ROOT / "docs" / "figs"
    out.mkdir(parents=True, exist_ok=True)

    ladder = sorted([r for r in rows if str(r["case"]).startswith("spp")],
                    key=lambda r: r["spp"])

    # ---- 1. frame time vs spp -------------------------------------------------
    if ladder:
        spp = [r["spp"] for r in ladder]
        fig, ax = plt.subplots(figsize=(8, 4.5), dpi=140)
        ax.plot(spp, [r["gpu_flux_ms"] for r in ladder], "o-", label="flux (ray tracing)")
        ax.plot(spp, [r["gpu_total_ms"] for r in ladder], "s-", label="GPU total (all passes)")
        ax.plot(spp, [r["wall_ms"] for r in ladder], "^--", label="wall clock (IMMEDIATE present)")
        ax.axhline(16.67, color="grey", ls=":", lw=1, label="60 FPS budget (16.67 ms)")
        ax.set_xscale("log", base=2)
        ax.set_xticks(spp)
        ax.set_xticklabels([str(int(s)) for s in spp])
        ax.set_xlabel("samples per pixel (rays = spp x 3950 active pixels)")
        ax.set_ylabel("milliseconds per frame")
        ax.set_title("Heliostat Studio - frame time vs spp (RTX 4060 Laptop, 1280x720)")
        ax.grid(alpha=0.3)
        ax.legend()
        fig.tight_layout()
        fig.savefig(out / "frametime_vs_spp.png")
        print(f"wrote {out / 'frametime_vs_spp.png'}")

    # ---- 2. A/B switches -----------------------------------------------------
    ab = [r for r in rows if str(r["case"]).startswith("ab_")]
    if ab:
        fig, ax = plt.subplots(figsize=(8, 4.5), dpi=140)
        names, vals, colors = [], [], []
        for r in ab:
            names.append(str(r["case"]).replace("ab_", ""))
            vals.append(r["gpu_flux_ms"])
            colors.append("#d1495b" if r["gpu_flux_ms"] > 5 else "#3f7cac")
        bars = ax.bar(names, vals, color=colors)
        ax.set_yscale("log")
        ax.set_ylabel("flux (ray tracing) stage, ms - log scale")
        ax.set_title("A/B switches on the same 4.04 M-ray workload")
        ax.grid(alpha=0.3, axis="y")
        for b, v in zip(bars, vals):
            ax.text(b.get_x() + b.get_width() / 2, v * 1.1, f"{v:.3f}", ha="center", fontsize=8)
        ax.text(0.01, 0.95, "atomics: per-ray scattered InterlockedOr on rayValidity\n"
                            "(one per ray, 4,044,800 per frame - verified by reading diagBuf[5])",
                transform=ax.transAxes, fontsize=7, va="top")
        fig.tight_layout()
        fig.savefig(out / "ab_switches.png")
        print(f"wrote {out / 'ab_switches.png'}")

    # ---- 3. pass breakdown at full quality ----------------------------------
    full = [r for r in ladder if r["spp"] == 1024]
    if full:
        r = full[0]
        labels = ["deform", "flux", "scene", "bloom", "composite"]
        vals = [r["gpu_deform_ms"], r["gpu_flux_ms"], r["gpu_scene_ms"], r["gpu_bloom_ms"],
                r["gpu_post_ms"]]
        fig, ax = plt.subplots(figsize=(8, 3.2), dpi=140)
        left = 0.0
        for lab, v, c in zip(labels, vals, ["#8e6c88", "#d1495b", "#30638e", "#edae49", "#00798c"]):
            ax.barh([0], [v], left=left, label=f"{lab} {v:.3f} ms", color=c)
            left += v
        ax.set_yticks([])
        ax.set_xlabel("ms per frame (GPU timestamp)")
        ax.set_title(f"Per-pass GPU time, 1024 spp, total {r['gpu_total_ms']:.3f} ms")
        ax.legend(loc="upper center", bbox_to_anchor=(0.5, -0.35), ncol=5, fontsize=8)
        fig.tight_layout()
        fig.savefig(out / "pass_breakdown.png")
        print(f"wrote {out / 'pass_breakdown.png'}")


if __name__ == "__main__":
    main()
