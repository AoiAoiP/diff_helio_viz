# Baseline reference — North 300 m, single sun direction

This file records the upstream numbers that `heliostat_core` is verified against.
Do not edit the numbers; if you re-verify and get different ones, something
changed (shader edit, proxy data regeneration, driver behaviour).

## Scene

| Item | Value |
|---|---|
| Heliostat | `North` @ (0, 0, −300) m — `data/ellipse_north.txt` |
| Sun direction | `(0.050684, 0.738197, 0.672679)` — line 2 of `data/sundir/sundir_3val.txt` (elevation 47.58°) |
| Bolt heights | all zero |
| Gravity | ON, normal coupling ON (3-plane bins, 20 angles) |
| Ray pre-cull (A1) | ON, margin 8 mrad |
| Plate / grid | 12.84 × 9.45 m, 32 × 32 samples = **1024 spp** |
| Receiver | cylinder R = 10 m, H = 20 m, 157 × 50 px |
| Active pixels | 3950 / 7850 (50 %) |
| Rays per render | 3950 × 1024 = **4.04 M** |

## Upstream reference (produced by `bezier_opt --dump-flux`)

Command (from the research repository root):

```powershell
./build/src/Release/bezier_opt.exe --dump-flux demo_scratch/probe.json
```

Console output:

```
Saved flux: demo_scratch/out/North_300m_sun0_flux.npy (7850 pixels, sum=480460.3)
EVAL: avg S95 = 226.6749 m² over 1 / 1 valid sun directions
```

The dump is stored here as `data/baseline/North_300m_sun0_flux.npy`.

**Caveat**: the upstream dump path circularly rolls each flux map in x so that the
spot centroid sits at the map centre (`main.cpp`, "Center flux" block). Sum, peak
and S95 are invariant under that roll; **pixel positions are not**. This package
does not roll, so compare the invariant metrics (or roll before a per-pixel diff).

## Verification result of this package

```
heliostat_core --config configs/probe_single_sun.json --sun 1 \
               --parity data/baseline/North_300m_sun0_flux.npy
```

| Metric | Upstream | This package | Delta |
|---|---|---|---|
| flux sum (W) | 480460.3 | 480460.2 | **1.95e-07 relative** |
| flux peak (W/px) | 664.911 | 664.911 | identical |
| S95 level | 75.207809 | 75.207809 | identical |
| S95 area (m²) | 226.67 | 226.67 | **0.00 %** |
| GPU S95 vs CPU bisection | — | — | **0.00e+00 relative (bit-identical)** |

Conclusion: the distilled engine reproduces the upstream forward physics. The
remaining 2e-7 flux-sum difference is float accumulation order in the tile
reduction.

## Performance reference (same 4.04 M-ray workload)

Measured on RTX 4060 Laptop, driver 596.21, Release build:

| Path | GPU time | Wall clock per render |
|---|---|---|
| Upstream `bezier_opt --dump-flux` (per sun, incl. flux readback) | — | **≈ 83 ms** (marginal, from 1/2/4/8/16-sun runs) |
| Upstream, diagnostic atomic removed (shader A/B) | — | ≈ 83 ms (4 renders 7707 → 1470 ms, i.e. **5.2×**) |
| `heliostat_core --bench` (render only) | 0.50 ms | 0.64 ms |
| `heliostat_core --bench-readback` (equivalent work per iteration) | 0.50 ms | **1.09 ms** |

Throughput: **≈ 8.1 Gray/s** (4.04 M rays / 0.50 ms).

Interpretation — be precise when presenting this:

* The upstream 83 ms was **host-bound**: every forward render performed 6–7
  staging-buffer allocations plus `vkQueueWaitIdle` (5 UBO uploads, the
  diagnostic-counter clear, and the flux readback), and the shader additionally
  did one global atomic per ray (4 M same-address `InterlockedAdd`).
* This package keeps the same physics and the same dispatch chain, but uploads
  UBOs through persistent mappings and submits the whole chain once. The GPU-side
  work drops to ~0.5 ms and the per-frame end-to-end cost to ~1.1 ms.
* So the honest headline is **~76× end-to-end on the same workload, of which
  5.2× comes from removing the per-ray global atomic and the rest from removing
  per-call host/GPU synchronisation** — not "the ray tracer got 76× faster".

## Re-verifying

```powershell
cmake --build build --config Release
cd build/Release   # or stay in the project root: the CLI finds build/shaders
heliostat_core.exe --config ../../configs/probe_single_sun.json --sun 1 `
                   --parity ../../data/baseline/North_300m_sun0_flux.npy
```
