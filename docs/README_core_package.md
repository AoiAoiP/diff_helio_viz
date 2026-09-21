# Heliostat Studio — core package

A **self-contained, dependency-light extract** of the heliostat surface-optimizer
research code, containing only what a real-time visualization front-end needs:
the differentiable-rendering physics (Slang shaders), the TPS + gravity plate
model, and a lean Vulkan compute driver.

The upstream repository is a research pipeline: headless, configuration-heavy,
and full of experiment scaffolding. This package keeps the **verified core** and
drops everything else.

```
heliostat_viz/
├─ PROMPT.md            ← start here when you open a new session: the working prompt
├─ PLAN.md              ← the project plan (architecture, milestones, acceptance)
├─ NOTES_provenance.md  ← exactly what was taken from upstream, and what was left behind
├─ CMakeLists.txt
├─ src/                 headless forward engine (verified against upstream)
├─ shaders/             the physics core (Slang)
│  └─ reference/        upstream inverse-rendering (bwd_diff) shaders, kept for reference
├─ data/                scene, sun directions, bolt presets, verification baseline
├─ data_proxy/          TPS influence functions + 20-bin FEA gravity (1.1 MB)
├─ configs/             ready-to-run scene configs
└─ viz/                 (empty) the real-time viewer you are going to write
```

## Build

Requirements: **Vulkan SDK 1.4+** (headers, loader and the bundled `slangc`),
Visual Studio 2022. Nothing else — no fmt, no glm, no GLFW, no ImGui, no
network access at build time (Slang is used as a compiler only, so there is not
even a `slang.dll` runtime dependency).

```powershell
# NOTE: prefer a native cmake. A msys2/msys cmake on PATH can fail in restricted shells.
& "C:\Program Files\CMake\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64
& "C:\Program Files\CMake\bin\cmake.exe" --build build --config Release
```

## Run

Assets and SPIR-V are resolved automatically (the config file's directory and the
executable's directory are searched), so **any** working directory works — pick one:

```powershell
cd build\Release
# single sun direction, parity check against the upstream reference flux map
heliostat_core.exe --config ..\..\configs\probe_single_sun.json --sun 1 `
                   --parity ..\..\data\baseline\North_300m_sun0_flux.npy

# performance: GPU timestamp + wall clock + Mray/s
heliostat_core.exe --config ..\..\configs\probe_single_sun.json --sun 1 --bench 200
heliostat_core.exe --config ..\..\configs\probe_single_sun.json --sun 1 --bench-readback 200

# optimised vs initial bolt pattern (36 sun directions)
heliostat_core.exe --config ..\..\configs\forward_36sun_optimized.json --sun 5 --print-cells

heliostat_core.exe --help
```

## What is verified

Run on RTX 4060 Laptop (driver 596.21), 4.04 M rays per render, zero bolts,
gravity on, 300 m North heliostat:

| Metric | Upstream research repo | This package |
|---|---|---|
| flux sum | 480460.3 W | 480460.2 W (rel. 2e-7) |
| S95 level | 75.207809 | 75.207809 (identical) |
| S95 area | 226.67 m² | 226.67 m² |
| GPU vs CPU S95 bisection | — | bit-identical |
| Per-render wall clock | ≈ 83 ms | **1.09 ms** (same work, incl. flux readback) |
| Pure GPU time | — | **0.50 ms** (≈ 8.1 Gray/s) |

Details, caveats and how to reproduce: `data/baseline/BASELINE.md`.

The honest framing of the speed-up: upstream spent its time in **host-side
synchronisation** (6–7 staging allocations + `vkQueueWaitIdle` per render) and in
a **per-ray global atomic** in a diagnostic counter (4 M same-address
`InterlockedAdd` per dispatch, measured 5.2× on its own). Same physics, same
dispatch chain, no host round-trips.

## What is missing (your job)

Everything visual. `viz/` is empty on purpose — see `PROMPT.md` and `PLAN.md`:

1. Win32 window + `VK_KHR_win32_surface` + swapchain (2 frames in flight).
2. A graphics pass: sky, tower, receiver cylinder (flux as an emissive heat map),
   the 12.84 × 9.45 m plate mesh pulled straight from the compute-written
   `yGrid` / `nGrid` buffers.
3. HDR + bloom + ACES, a bolt-stroke UI, sun sliders, and a live performance panel.
4. A shader-variant A/B switch that reproduces the 5.2× atomic finding on screen.

## Reusing the physics

The engine and the viewer share the same shader entry points: `clearFlux`,
`computeBoltSurface`, `renderForward*`, `finalizeFlux`, `computeS95FindLevel`.
`src/engine.cpp` shows the exact dispatch chain, push constants and descriptor
binding numbers (identical to upstream, so upstream SPIR-V drops in unchanged).
