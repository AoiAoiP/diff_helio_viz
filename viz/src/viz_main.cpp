// viz_main.cpp - Heliostat Studio viewer: window, frame loop, UI state machine.
//
// Frame loop contract (PLAN.md section 7):
//   * exactly one vkQueueSubmit per frame, 2 frames in flight;
//   * no vkQueueWaitIdle / vkDeviceWaitIdle anywhere in the steady-state path;
//   * no allocation per frame (buffers, descriptors, query pools and pipelines
//     are all created up front; per-frame data goes into persistently mapped
//     descriptor-set-per-frame UBO slices);
//   * every readback is delayed: GPU timestamps are collected two frames later,
//     the S95 state every N frames.

#include "camera.h"
#include "config.h"
#include "data.h"
#include "engine.h"
#include "gpu_timer.h"
#include "hud.h"
#include "platform_win32.h"
#include "vk.h"
#include "vk_context.h"
#include "vk_flux.h"
#include "vk_gfx.h"
#include "vk_post.h"
#include "vk_scene.h"

#include <windows.h>
#include <shellapi.h>   // CommandLineToArgvW

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <io.h>
#include <share.h>   // _fsopen / _SH_DENYWR
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// ----------------------------------------------------------------- options --
struct Options {
    std::string config = "configs/probe_single_sun.json";
    std::string bolts;
    std::string shaderDir = "shaders";
    std::string perfCsv;               // append one row per second
    std::string logFile;               // tee stdout/stderr (GUI app: no console)
    std::string screenshot;            // render N frames, save a BMP, exit
    std::string dumpFlux;              // write the final flux map as NPY
    bool referenceChain = false;       // parity mode: run forward.slang instead of flux_lite
    std::string recordDir;             // dump every Nth frame as a BMP (GIF source)
    int sunIndex = -1;                 // -1 = live sliders; >=0 = direction from the sun file
    int width = 1280, height = 720;
    int frames = 0;                    // 0 = run until ESC/close
    int screenshotFrame = 120;
    int recordStride = 1;
    bool validate = false;
    bool vsync = false;                // DoD: >= 60 FPS with vsync off
    float fpsCap = 60.0f;              // software frame-rate cap, 0 = uncapped
    bool atomics = false;              // A/B switch 3 (per-ray global atomic)
    float bloomThreshold = 2.5f;       // bright-pass knee: what is allowed to glow
    float bloomStrength = -1.0f;       // <0 = keep the interactive default
    bool noCull = false;               // A/B switch 1 off (no A1 pre-cull)
    std::string presentMode;           // fifo | mailbox | immediate
    bool noUI = false;
    float sppOverride = 0.0f;
    float tOverride = -1.0f;           // convergence slider
    float azimuthOverride = 1e9f;
    float elevationOverride = 1e9f;
    int preset = -1;
    float demoSeconds = 0.0f;          // >0: run the scripted demo sequence for that long
    bool demoExit = false;             // terminate when the demo sequence ends (opt-in)
    float hudScale = 0.0f;             // 0 = auto (from the window size), else pinned
    float aimOffsetDeg = 0.0f;         // beam target: aim point offset on the receiver
    int sunPath = 0;                   // 1..3 = Delingha day path (solstice/equinox)
    float sunHourDeg = 1e9f;            // pin the hour angle (stills, tests)
    int mirror = 0;                    // 0..3 = N/E/S/W field mirror to trace
    bool noFluxMap = false;            // hide the bottom-right flux inset
    bool noCardinals = false;          // hide the NSWE ground markers (testability)
    bool noBolts = false;              // hide the actuator markers
    float boltScale = 0.22f;           // actuator marker radius (m)
    bool noBeams = false;              // hide the sun/reflected beams
    bool noInsetCentre = false;        // keep the raw map (demo: shows the cut spot)
    bool uiLayout = false;             // print the HUD widget geometry (drives the UI test)
    bool stateLog = false;             // print a state line every 500 ms (drives the UI test)
    int debugView = 0;                 // F7 debug view (0 shaded, 1 normals, 2 slope, 3 height)
    int deformIdx = 0;                 // F6 deformation exaggeration index
};

void usage() {
    std::puts(
        "Heliostat Studio - real-time viewer\n"
        "\n"
        "Usage: heliostat_viz [options]\n"
        "  --config <file>       scene/config JSON (default configs/probe_single_sun.json)\n"
        "  --sun <n>             sun-direction index into the sun file\n"
        "  --bolts <file>        bolt heights (default: the optimized preset)\n"
        "  --shader-dir <dir>    where the .spv files live\n"
        "  --width/--height <n>  window size (default 1280x720)\n"
        "  --vsync               FIFO present (default is MAILBOX, kept in step by --fps)\n"
        "  --fps <n>             software frame-rate cap: 60 (default), 120, or 0 = uncapped\n"
        "  --uncapped            same as --fps 0 (run as fast as the GPU allows)\n"
        "  --validate            enable VK_LAYER_KHRONOS_validation\n"
        "  --frames <n>          run n frames then exit (scripting)\n"
        "  --screenshot <bmp>    save a screenshot at --shot-frame and exit\n"
        "  --shot-frame <n>      frame to capture (default 120)\n"
        "  --record <dir>        dump frames as BMP for GIF assembly\n"
        "  --record-stride <n>   capture every n-th frame (default 1)\n"
        "  --perf-csv <file>     append per-second performance rows\n"
        "  --spp <n>             samples per pixel (16..1024)\n"
        "  --t <0..1>            convergence slider position\n"
        "  --az/--el <deg>       sun azimuth / elevation\n"
        "  --preset <0..3>       camera preset\n"
        "  --no-ui               hide the overlay panel\n"
        "  --demo <seconds>      run the scripted demo sequence (15 s: sun sweep, spot closeup,\n"
        "                        convergence slider, atomics A/B, debug views). It drives the camera\n"
        "                        itself, then returns control to you (--demo-exit to terminate).\n"
        "  --demo-exit           terminate the process when the demo sequence ends\n"
        "  --aim <deg>           beam target: aim point offset around the receiver (-60..60)\n"
        "  --hud-scale <f>       UI scale (e.g. 1.7 when recording for a GIF)\n"
        "  --bloom-thr <f>       bloom bright-pass threshold (default 2.5)\n"
        "  --bloom <f>           bloom strength (default 0.55)\n"
        "  --sunpath <0..3>      Delingha day path: 0 off, 1 summer solstice, 2 equinox, 3 winter\n"
        "  --sun-hour <deg>      pin the day-path hour angle (implies --sunpath if > 0)\n"
        "  --hud-scale <f>       pin the UI scale (default: auto from the window size; 'U' cycles)\n"
        "  --no-bolts            hide the actuator markers\n"
        "  --mirror <0..3>       field mirror to ray trace: 0 N, 1 E, 2 S, 3 W\n"
        "  --no-flux-map         start with the flux-map inset hidden\n"
        "  --no-cardinals        hide the NSWE ground markers\n"
        "  --bolt-scale <m>      actuator marker radius (default 0.22)\n"
        "  --no-beams            hide the sun + reflected beams (stills, diagnostics)\n"
        "  --no-inset-centre     draw the raw map in the inset (spot can straddle the seam)\n"
        "\n"
        "Keys: LMB orbit, RMB/MMB pan, wheel zoom, WASD/QE fly, Shift = fast\n"
        "      F1-F4 camera presets, F5 bolt preset, F6 deformation scale,\n"
        "      F7 debug view, F8 beams, F9 exposure, F10 vsync,\n"
        "      1 cull A/B, 2 spp, 3 atomics A/B, Space screenshot, ESC quit\n"
        "      4/5/6/7 select + fly to mirror N/E/S/W, 8 field off, 9/0 Delingha day path,\n"
        "      F flux-map inset, C compass + ground markers, H panel, M heat range,\n"
        "      K beam centre, [ ] convergence t, F11 frame-rate cap 60/120/off\n");
}

std::vector<std::string> commandLineArgs() {
    std::vector<std::string> out;
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return out;
    for (int i = 0; i < argc; i++) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, s.data(), n, nullptr, nullptr);
        out.push_back(std::move(s));
    }
    LocalFree(argv);
    return out;
}

bool parseArgs(const std::vector<std::string> &argv, Options &o, bool &help) {
    help = false;
    for (size_t i = 1; i < argv.size(); i++) {
        const std::string &a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argv.size()) throw std::runtime_error("missing value for " + a);
            return argv[++i];
        };
        try {
            if (a == "--config") o.config = next();
            else if (a == "--bolts") o.bolts = next();
            else if (a == "--shader-dir") o.shaderDir = next();
            else if (a == "--perf-csv") o.perfCsv = next();
            else if (a == "--log") o.logFile = next();
            else if (a == "--screenshot") o.screenshot = next();
            else if (a == "--dump-flux") o.dumpFlux = next();
            else if (a == "--reference-chain") o.referenceChain = true;
            else if (a == "--shot-frame") o.screenshotFrame = std::stoi(next());
            else if (a == "--record") o.recordDir = next();
            else if (a == "--record-stride") o.recordStride = std::max(1, std::stoi(next()));
            else if (a == "--sun") o.sunIndex = std::stoi(next());
            else if (a == "--width") o.width = std::stoi(next());
            else if (a == "--height") o.height = std::stoi(next());
            else if (a == "--frames") o.frames = std::stoi(next());
            else if (a == "--spp") o.sppOverride = std::stof(next());
            else if (a == "--t") o.tOverride = std::stof(next());
            else if (a == "--az") o.azimuthOverride = std::stof(next());
            else if (a == "--el") o.elevationOverride = std::stof(next());
            else if (a == "--preset") o.preset = std::stoi(next());
            else if (a == "--demo") o.demoSeconds = std::stof(next());
            else if (a == "--demo-exit") o.demoExit = true;
            else if (a == "--aim") o.aimOffsetDeg = std::stof(next());
            else if (a == "--sunpath") o.sunPath = std::stoi(next());
            else if (a == "--sun-hour") o.sunHourDeg = std::stof(next());
            else if (a == "--mirror") o.mirror = std::stoi(next());
            else if (a == "--no-flux-map") o.noFluxMap = true;
            else if (a == "--no-cardinals") o.noCardinals = true;
            else if (a == "--no-bolts") o.noBolts = true;
            else if (a == "--bolt-scale") o.boltScale = std::stof(next());
            else if (a == "--no-beams") o.noBeams = true;
            else if (a == "--no-inset-centre") o.noInsetCentre = true;
            else if (a == "--ui-layout") o.uiLayout = true;
            else if (a == "--state-log") o.stateLog = true;
            else if (a == "--hud-scale") o.hudScale = std::stof(next());
            else if (a == "--debug-view") o.debugView = std::stoi(next());
            else if (a == "--deform") o.deformIdx = std::stoi(next());
            else if (a == "--vsync") o.vsync = true;
            else if (a == "--fps") o.fpsCap = std::stof(next());
            else if (a == "--uncapped") o.fpsCap = 0.0f;
            else if (a == "--atomics") o.atomics = true;
            else if (a == "--bloom-thr") o.bloomThreshold = std::stof(next());
            else if (a == "--bloom") o.bloomStrength = std::stof(next());
            else if (a == "--no-cull") o.noCull = true;
            else if (a == "--present") o.presentMode = next();
            else if (a == "--validate") o.validate = true;
            else if (a == "--no-ui") o.noUI = true;
            else if (a == "-h" || a == "--help") { help = true; return true; }
            else {
                std::fprintf(stderr, "unknown option: %s\n", a.c_str());
                return false;
            }
        } catch (const std::exception &e) {
            std::fprintf(stderr, "argument error: %s\n", e.what());
            return false;
        }
    }
    return true;
}

std::string resolveShaderDir(const std::string &requested, const std::string &exeDir) {
    const std::string candidates[] = {exeDir + "/shaders", requested, "shaders", "build/shaders", "../shaders",
                                      "../../shaders", "../../../shaders"};
    for (const auto &c : candidates) {
        if (fs::exists(fs::path(c) / "fluxLite.spv")) return c;
    }
    return requested;
}

// The scene config is written relative to the project root, while the executable
// lives in build/Release. loadConfig() silently falls back to Config defaults for
// a missing file -which leaves the CSR-derived Buie constants (kappa/gamma) at
// zero and silently changes the sun shape -so the path is resolved here and the
// caller is told when the file could not be found.
std::string resolveConfigPath(const std::string &requested, const std::string &exeDir) {
    const std::string candidates[] = {requested, exeDir + "/" + requested, exeDir + "/../../" + requested,
                                      "../../" + requested, "../../../" + requested};
    for (const auto &c : candidates) {
        if (fs::exists(c)) return c;
    }
    return {};
}

// ------------------------------------------------------------- BMP capture --
bool saveBmp(const std::string &path, const std::vector<uint8_t> &bgra, uint32_t w, uint32_t h) {    const uint32_t rowBytes = w * 3;
    const uint32_t padding = (4 - (rowBytes % 4)) % 4;
    const uint32_t imageBytes = (rowBytes + padding) * h;
    const uint32_t fileBytes = 54 + imageBytes;

    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    uint8_t header[54] = {};
    header[0] = 'B'; header[1] = 'M';
    std::memcpy(header + 2, &fileBytes, 4);
    const uint32_t dataOffset = 54;
    std::memcpy(header + 10, &dataOffset, 4);
    const uint32_t dibSize = 40;
    std::memcpy(header + 14, &dibSize, 4);
    const int32_t sw = static_cast<int32_t>(w), sh = static_cast<int32_t>(h);
    std::memcpy(header + 18, &sw, 4);
    std::memcpy(header + 22, &sh, 4);
    const uint16_t planes = 1, bpp = 24;
    std::memcpy(header + 26, &planes, 2);
    std::memcpy(header + 28, &bpp, 2);
    std::memcpy(header + 34, &imageBytes, 4);
    std::fwrite(header, 1, 54, f);

    std::vector<uint8_t> row(rowBytes + padding, 0);
    for (int32_t y = static_cast<int32_t>(h) - 1; y >= 0; y--) {   // BMP is bottom-up
        const uint8_t *src = bgra.data() + static_cast<size_t>(y) * w * 4;
        for (uint32_t x = 0; x < w; x++) {
            row[x * 3 + 0] = src[x * 4 + 0];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 2];
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    return true;
}

// ASCII preview of a frame: a low-cost, text-only way to eyeball the
// composition (and the only way to check a render in a terminal/CI run).
// Prints a luminance ramp plus, for each cell, which channel dominates, so the
// sky/ground/spot layout is readable without an image viewer.
void saveAsciiPreview(const std::string &path, const std::vector<uint8_t> &bgra, uint32_t w, uint32_t h,
                      uint32_t cols = 108, uint32_t rows = 34) {
    FILE *f = std::fopen(path.c_str(), "w");
    if (!f) return;
    const char *ramp = " .:-=+*#%@";
    const uint32_t stepX = std::max(1u, w / cols);
    const uint32_t stepY = std::max(1u, h / rows);
    std::fprintf(f, "# %ux%u frame, luminance ramp \" .:-=+*#%%@\", cell %ux%u\n", w, h, stepX, stepY);
    for (uint32_t y = 0; y < h; y += stepY) {
        std::string line;
        for (uint32_t x = 0; x < w; x += stepX) {
            uint64_t r = 0, g = 0, b = 0, n = 0;
            for (uint32_t yy = y; yy < std::min(h, y + stepY); yy++) {
                for (uint32_t xx = x; xx < std::min(w, x + stepX); xx++) {
                    const uint8_t *p = bgra.data() + (static_cast<size_t>(yy) * w + xx) * 4;
                    b += p[0];
                    g += p[1];
                    r += p[2];
                    n++;
                }
            }
            if (!n) n = 1;
            const double rr = static_cast<double>(r) / n, gg = static_cast<double>(g) / n,
                         bb = static_cast<double>(b) / n;
            const double lum = (0.2126 * rr + 0.7152 * gg + 0.0722 * bb) / 255.0;
            int idx = static_cast<int>(lum * 9.999);
            line += ramp[idx < 0 ? 0 : (idx > 9 ? 9 : idx)];
        }
        std::fprintf(f, "%s\n", line.c_str());
    }
    std::fclose(f);
}

// --------------------------------------------------------------- UI state --
enum class BoltMode { Converge, Lsq, Zero };

const char *boltModeName(BoltMode m) {
    switch (m) {
    case BoltMode::Lsq: return "LSQ";
    case BoltMode::Zero: return "ZERO";
    default: return "SLIDER";
    }
}

struct UiState {
    float sunAzimuthDeg = 4.31f;
    float sunElevationDeg = 47.58f;
    float convergence = 1.0f;          // t: 0 = flat plate, 1 = optimized strokes
    BoltMode boltMode = BoltMode::Converge;
    int deformScaleIndex = 0;          // F6: 1x / 50x / 200x
    int debugView = 0;                 // F7 cycles 0..3
    int beamMode = 1;                  // F8
    float exposure = 1.0f;
    float bloomStrength = 0.55f;
    float bloomThreshold = 2.5f;       // only genuinely HDR emitters bloom
    float bloomClamp = 6.0f;           // firefly guard for the bright pass
    float aimOffsetDeg = 0.0f;         // beam target: moves the aim point along the receiver
    float fluxCeil = 700.0f;           // adaptive heat-map ceiling (tracked from the readback)
    bool heatAutoRange = true;
    bool panelVisible = true;          // 'H' toggles the overlay
    bool showFluxMap = true;           // 'F' toggles the bottom-right flux inset
    bool showCardinals = true;         // 'C' toggles the N/E/S/W field markers
    int heliostat = 0;                 // 4/5/6/7 select N/E/S/W (only that one is traced)
    int sunPath = 0;                   // 8/9/0: off / equator / mid-lat / high-lat
    float sunPathHourDeg = -70.0f;     // hour angle along the day path
    bool sunPathAnimating = true;
    uint32_t spp = 64;
    bool rayCull = true;               // A/B switch 1
    bool diagAtomics = false;          // A/B switch 3
    bool vsync = false;
    bool paused = false;
    int preset = -1;
    float time = 0.0f;
    // Live flux statistics (delayed readback, see FluxReadback).
    float fluxSum = 0.0f, fluxPeak = 0.0f, s95Area = 0.0f, s95Level = 0.0f, s95AreaCpu = 0.0f;
    float energyRetention = 0.0f;
    // Flux-weighted centroid of the map (0..1): the flux inset rolls the cylinder
    // to it, so the spot is centred in the panel for every mirror.
    float fluxCenterU = 0.5f, fluxCenterV = 0.5f;
    // Software frame-rate cap (F11): 60 / 120 / 0 = uncapped. Displayed in the HUD.
    float fpsCap = 60.0f;
    const char *fpsCapName = "60";
    void setFpsCap(float fps) {
        fpsCap = fps;
        fpsCapName = fps < 1.0f ? "off" : (fps < 61.0f ? "60" : "120");
    }
    // Action feedback: every key/toggle writes a one-line message that the panel shows
    // for a few seconds, so a switch that only changes a subtle visual (heat range,
    // beams, cull, atomics) is still obviously acknowledged.
    std::string toastText;
    float toastUntil = -1.0f;
    bool toastActive(float now) const { return now < toastUntil && !toastText.empty(); }
    // UI scale: 0 = auto (derived from the window size), otherwise the --hud-scale value.
    float hudScale = 0.0f;
    // Field ray tracing (key 8): when false the compute chain is not recorded at all.
    bool fieldTraced = true;
    const char *demoCaption = nullptr;   // shown as a banner while --demo runs
};

// The scene tells three stories, so the presets and A/B switches are the UI.
struct BoltPresets {
    std::vector<float> zero;
    std::vector<float> lsq;
    std::vector<float> optimized;
    std::vector<float> blended;

    void blend(float t) {
        blended.resize(optimized.size());
        for (size_t i = 0; i < optimized.size(); i++) {
            blended[i] = optimized[i] * t + zero[i] * (1.0f - t);
        }
    }
    float maxStrokeMm(float t) const {
        float m = 0.0f;
        for (size_t i = 0; i < optimized.size(); i++) {
            m = std::max(m, std::fabs(optimized[i] * t + zero[i] * (1.0f - t)));
        }
        return m * 1000.0f;
    }
};

const char *debugViewName(int v) {
    switch (v) {
    case 1: return "normals";
    case 2: return "slope error";
    case 3: return "height";
    default: return "shaded";
    }
}
// Short float for the HUD toasts, e.g. 1.5 -> "1.5", 2 -> "2".
std::string trimFloat(float v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.1f", v);
    std::string s(b);
    if (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

// ------------------------------------------------------------ frame limiter --
// A *software* frame-rate cap.
//
// The present modes alone cannot express "60 FPS": MAILBOX follows the monitor's
// refresh (115 FPS on this 59 Hz panel, because the page flip is decoupled) and
// IMMEDIATE runs flat out. A production viewer needs a deterministic cadence, so
// the loop paces itself against a monotonic clock and sleeps the remainder of the
// frame period on a high-resolution waitable timer (kernel32; no extra library and
// no global timeBeginPeriod). The wait is coarse + spin so the cadence is exact:
// sleeping alone would overshoot by a timer tick and the FPS would read low.
struct FrameLimiter {
    float cap = 60.0f;        // frames per second, 0 = uncapped
    double nextSec = 0.0;
    HANDLE timer = nullptr;

    void init() {
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);   // older Windows: coarse
    }
    void destroy() {
        if (timer) CloseHandle(timer);
        timer = nullptr;
    }
    void setCap(float fps) {
        cap = fps;
        nextSec = 0.0;   // re-phase the cadence
    }
    // Blocks until the next frame slot is due. Called at the top of the frame so
    // the reported wall time is the capped frame time (the GPU timestamps are
    // unaffected: they measure device time only).
    void wait(const viz::FrameClock &clock) {
        if (cap <= 0.0f) {
            nextSec = 0.0;
            return;
        }
        const double period = 1.0 / static_cast<double>(cap);
        double now = clock.elapsedSec();
        if (nextSec <= 0.0) nextSec = now + period;
        const double remain = nextSec - now;
        if (remain > 0.0005) {
            if (timer && remain > 0.002) {
                LARGE_INTEGER due{};
                due.QuadPart = -static_cast<LONGLONG>(remain * 1e7);   // 100 ns units, relative
                if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
                    WaitForSingleObject(timer, 50);
            }
            while (clock.elapsedSec() < nextSec) YieldProcessor();
        }
        now = clock.elapsedSec();
        nextSec += period;
        if (nextSec < now) nextSec = now + period;   // fell behind: resync, never spiral
    }
};


// A fixed timeline so the recorded GIF (and the 3-minute walkthrough) is exactly
// reproducible: same camera, same sun trajectory, same slider moves, every run.
struct DemoPhase {
    float duration;
    const char *caption;
};
constexpr DemoPhase kDemoPhases[] = {
    {3.5f, "1. real-time flux - the sun sweeps, the spot follows (0.36 ms of ray tracing per frame)"},
    {3.0f, "2. receiver closeup - 157x50 heat map, log-mapped + bloom"},
    {3.5f, "3. convergence slider t: flat plate -> optimised - S95 226.7 -> 46.3 m2"},
    {3.0f, "4. A/B switch: per-ray global atomics OFF (0.36 ms) / ON (48.8 ms)"},
    {2.0f, "5. debug views: normals, slope error, plate height"},
    {5.0f, "6. four-mirror field - only the selected mirror (N/E/S/W) is ray traced: S95 46 / 55 / 176 / 53 m2"},
    {3.0f, "7. flux-map inset (F) - the receiver map with its S95 contour, live"},
    {4.0f, "8. Delingha day path, summer solstice - same plate, sun from sunrise to sunset"},
};
constexpr int kDemoPhaseCount = static_cast<int>(sizeof(kDemoPhases) / sizeof(kDemoPhases[0]));
constexpr float kDemoTotal = 27.0f;

// ------------------------------------------------------- flux readback ring --
// The flux texture (157x50 R32F, 31 KB) is copied into a host-visible staging
// buffer every 'stride' frames and read two frames later, once the fence of that
// frame slot has been signalled. Nothing in the frame loop ever waits.
class FluxReadback {
public:
    void init(hviz::VkCore &vk, hviz::ForwardEngine &engine, uint32_t framesInFlight, uint32_t stride) {
        m_vk = &vk;
        m_engine = &engine;
        m_stride = stride;
        m_pixels = engine.totalPixels();
        for (uint32_t i = 0; i < framesInFlight; i++) {
            m_stage[i] = vk.createBuffer(m_pixels * sizeof(float), VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
            m_pending[i] = false;
        }
        m_flux.resize(m_pixels);
    }
    void destroy() {
        for (auto &b : m_stage)
            if (b.buffer) m_vk->destroyBuffer(b);
    }

    // Records the copy for this frame (called between the compute chain and the
    // graphics passes).
    void record(VkCommandBuffer cmd, uint32_t slot, uint64_t frameNo) {
        if (m_stride == 0 || frameNo % m_stride != 0) return;
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {m_engine->config().pixelWidth, m_engine->config().pixelHeight, 1};
        vkCmdCopyImageToBuffer(cmd, m_engine->fluxTexture().image, VK_IMAGE_LAYOUT_GENERAL, m_stage[slot].buffer,
                               1, &region);
        m_pending[slot] = true;
    }

    // Called at the top of a frame whose slot fence has just been waited on.
    bool collect(uint32_t slot) {
        if (!m_pending[slot]) return false;
        m_pending[slot] = false;
        std::memcpy(m_flux.data(), m_stage[slot].mapped, m_pixels * sizeof(float));
        return true;
    }

    const std::vector<float> &flux() const { return m_flux; }
    // Sum / peak / S95 for the most recently collected map. Reads the S95 level
    // straight out of the engine's persistently mapped state buffer (the same
    // trick the HUD uses) -no readback call, no stall.
    hviz::FluxStats stats() const {
        hviz::FluxStats s;
        for (float f : m_flux) {
            s.sum += f;
            if (f > s.max) s.max = f;
        }
        const auto *state = static_cast<const float *>(m_engine->s95StateBuffer().mapped);
        s.s95LevelGpu = state ? state[0] : 0.0f;
        s.s95LevelCpu = hviz::computeS95LevelCPU(m_flux);
        s.s95AreaGpu = hviz::computeS95Area(m_flux, s.s95LevelGpu, m_engine->pixelArea());
        s.s95AreaCpu = hviz::computeS95Area(m_flux, s.s95LevelCpu, m_engine->pixelArea());
        s.energyRetention = s.sum > 1e-6f ? (state ? state[2] / s.sum : 0.0f) : 0.0f;
        return s;
    }

    // Flux-weighted centroid of the map, in 0..1 texture coordinates.
    //
    // u is the azimuth around the receiver cylinder, so it is *periodic*: a plain
    // weighted mean would land in the middle of nowhere for a spot that straddles
    // the u = 0 seam (which is exactly the North mirror's case). Averaging the unit
    // vector (cos 2*pi*u, sin 2*pi*u) instead gives the correct circular mean, and
    // v (height, not periodic) is a plain weighted mean.
    void centroid(float *outU, float *outV) const {
        const uint32_t w = m_engine->config().pixelWidth;
        const uint32_t h = m_engine->config().pixelHeight;
        const float twoPi = 6.283185307179586f;
        double sc = 0.0, ss = 0.0, sv = 0.0, sw = 0.0;
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
                const double f = m_flux[static_cast<size_t>(y) * w + x];
                if (f <= 0.0) continue;
                const double a = twoPi * (static_cast<double>(x) + 0.5) / w;
                sc += f * std::cos(a);
                ss += f * std::sin(a);
                sv += f * ((static_cast<double>(y) + 0.5) / h);
                sw += f;
            }
        }
        if (sw <= 0.0) {
            *outU = 0.5f;
            *outV = 0.5f;
            return;
        }
        double u = std::atan2(ss, sc) / twoPi;
        if (u < 0.0) u += 1.0;
        *outU = static_cast<float>(u);
        *outV = static_cast<float>(sv / sw);
    }

private:
    hviz::VkCore *m_vk = nullptr;
    hviz::ForwardEngine *m_engine = nullptr;
    hviz::Buffer m_stage[viz::VkContext::kFramesInFlight];
    bool m_pending[viz::VkContext::kFramesInFlight] = {};
    std::vector<float> m_flux;
    uint32_t m_pixels = 0;
    uint32_t m_stride = 15;
};

// ---------------------------------------------------------------- the field --
// Four heliostats at 300 m, one per cardinal direction. Only the *selected* mirror
// is ray traced (the engine holds one heliostat's parameters, which is exactly the
// behaviour we want); the other three are drawn as flat plates aimed at the
// receiver. The bolt preset is the North-optimised one for every mirror, so the
// E/S/W spots are "the same mirror re-aimed", not a re-optimised surface.
enum class Cardinal { North = 0, East = 1, South = 2, West = 3 };

const char *cardinalName(Cardinal c) {
    switch (c) {
    case Cardinal::North: return "North";
    case Cardinal::East: return "East";
    case Cardinal::South: return "South";
    default: return "West";
    }
}

// World axes of this scene: the heliostat looks towards +Z (the northern-hemisphere
// case: the field is north of the tower and faces south), +X is east.
viz::Vec3 cardinalPosition(Cardinal c, float distance) {
    switch (c) {
    case Cardinal::North: return viz::Vec3{0.0f, 0.0f, -distance};
    case Cardinal::East: return viz::Vec3{distance, 0.0f, 0.0f};
    case Cardinal::South: return viz::Vec3{0.0f, 0.0f, distance};
    default: return viz::Vec3{-distance, 0.0f, 0.0f};
    }
}

// ------------------------------------------------------------ sun path demo --
// The field is at Delingha, Qinghai, China (the 50 MW molten-salt tower plant):
// 37.37 N, 97.37 E, solar time = Beijing time - 1h31m. The demo drives the sun
// along a real day path for that site, so the trajectory is not decorative: the
// hour angle goes from sunrise to sunset, the declination is the true solar
// declination of the date, and the resulting direction feeds the same physics the
// sliders do (one vkQueueSubmit per frame, no special case in the tracer).
//
//   declination  summer solstice +23.44 deg, equinox 0, winter solstice -23.44 deg
//   sunrise/sunset  cos(H0) = -tan(lat) * tan(dec)
//   noon elevation  90 - |lat - dec|          (76.07 deg at the summer solstice)
//   day length      2 * H0 / 15 hours
//
// Formulas in the horizontal frame, with the hour angle H (0 = solar noon,
// positive in the afternoon):
//   east  = -cos(dec) sin(H)
//   north =  sin(dec) cos(lat) - cos(dec) sin(lat) cos(H)
//   up    =  sin(dec) sin(lat) + cos(dec) cos(lat) cos(H)
struct SunPathMode {
    const char *label;        // shown in the HUD
    const char *date;         // shown in the HUD
    float declinationDeg;
};

// Delingha, Qinghai (the site of the 50 MW tower plant). Longitude only affects
// the wall-clock display: solar time = Beijing time - (120 - lon)/15 h.
constexpr float kFieldLatitudeDeg = 37.37f;
constexpr float kFieldLongitudeDeg = 97.37f;
constexpr float kBeijingMeridianDeg = 120.0f;   // UTC+8 standard meridian

constexpr SunPathMode kSunPaths[3] = {
    {"summer solstice", "Jun 21", 23.44f},
    {"equinox", "Mar 21 / Sep 23", 0.0f},
    {"winter solstice", "Dec 21", -23.44f},
};

viz::Vec3 sunDirFromPath(float latitudeDeg, float hourAngleDeg, float declinationDeg) {
    const float lat = latitudeDeg * 3.14159265358979f / 180.0f;
    const float h = hourAngleDeg * 3.14159265358979f / 180.0f;
    const float dec = declinationDeg * 3.14159265358979f / 180.0f;
    const float east = -std::cos(dec) * std::sin(h);
    const float north = std::sin(dec) * std::cos(lat) - std::cos(dec) * std::sin(lat) * std::cos(h);
    const float up = std::sin(dec) * std::sin(lat) + std::cos(dec) * std::cos(lat) * std::cos(h);
    return viz::Vec3{east, up, -north};   // world: +X east, +Y up, +Z south
}

// Half the day length, in degrees of hour angle (0 at the summer solstice +90,
// i.e. 12 h, at the equinox). Undefined for a polar day/night, which cannot happen
// at 37 N.
float sunPathMaxHourDeg(float latitudeDeg, float declinationDeg) {
    const float lat = latitudeDeg * 3.14159265358979f / 180.0f;
    const float dec = declinationDeg * 3.14159265358979f / 180.0f;
    const float c = -std::tan(lat) * std::tan(dec);
    return std::acos(std::min(1.0f, std::max(-1.0f, c))) * 180.0f / 3.14159265358979f;
}

// Hour angle -> decimal hours of *true solar time* (12.00 = solar noon).
float solarTimeHours(float hourAngleDeg) { return 12.0f + hourAngleDeg / 15.0f; }
// True solar time -> Beijing time (UTC+8): the site is 22.63 deg west of 120 E.
float beijingTimeHours(float solarHours) {
    float t = solarHours + (kBeijingMeridianDeg - kFieldLongitudeDeg) / 15.0f;
    while (t < 0.0f) t += 24.0f;
    while (t >= 24.0f) t -= 24.0f;
    return t;
}

float elevationOf(const viz::Vec3 &d) {
    return std::asin(std::min(1.0f, std::max(-1.0f, d.y))) * 180.0f / 3.14159265358979f;
}
float azimuthOf(const viz::Vec3 &d) { return std::atan2(d.x, d.z) * 180.0f / 3.14159265358979f; }

// Aim point for an arbitrary field position (the engine only knows the selected
// one; the other mirrors' plates are oriented with the same construction so the
// field reads correctly).
//
// `aimOffsetDeg` must match the beam-target slider, because
// ForwardEngine::updateAim() moves the aim point along the receiver circumference
// and recomputes the macro normal from it: an aim point that ignores the offset
// leaves every drawn plate tilted by up to ~1.2 deg relative to the plate the
// physics actually traces (worst on the South mirror, whose normal is nearly
// vertical). The rotation below is the same convention as updateAim(): the receiver
// pixel parameterisation is P = (cx + R sin(theta), y, cz - R cos(theta)).
std::array<float, 3> aimPointFor(const std::array<float, 3> &heliostatPos, const hviz::Config &cfg,
                                 float aimOffsetDeg) {
    std::array<float, 3> ap = hviz::computeAimPoint(heliostatPos, cfg.receiverPos, cfg.receiverRadius);
    if (aimOffsetDeg != 0.0f) {
        const float r = cfg.receiverRadius;
        const float theta0 = std::atan2(ap[0] - cfg.receiverPos[0], -(ap[2] - cfg.receiverPos[2]));
        const float theta = theta0 + aimOffsetDeg * 3.14159265f / 180.0f;
        ap[0] = cfg.receiverPos[0] + r * std::sin(theta);
        ap[2] = cfg.receiverPos[2] - r * std::cos(theta);
    }
    return ap;
}

// Bolt-cloud vs plate alignment, measured rather than assumed.
//
// The bolt markers are drawn in the *engine's* macro frame (scene.macroN/U/V, set
// from ForwardEngine::macroNormal()) while the plates are drawn in the *viewer's*
// per-mirror frame (macroBasisFor, stored in the field records). Two frames, two
// code paths: if they disagree the bolt cloud is rotated relative to the plate it
// is supposed to sit on, and the corner bolts (kBoltUV is a full [0,1]^2 pattern)
// leave the mirror. This function reproduces the shader's bolt placement and
// reports how far the plate's corners move when expressed in the wrong frame.
struct BoltFit {
    int outside = 0;         // plate corners (of 4) whose bolt marker falls off
    float worstM = 0.0f;     // worst lateral overshoot, in metres
    float frameDeg = 0.0f;   // angle between the two plate frames (about the normal)
    float normalDeg = 0.0f;  // angle between the two macro normals
    float viewerNY = 0.0f;   // y component of the viewer's plate normal (must be > 0)
    float engineNY = 0.0f;   // y component of the engine's macro normal
};

BoltFit measureBoltFit(const hviz::ForwardEngine &engine, const viz::Vec3 &platePos, const viz::Vec3 &vN,
                       const viz::Vec3 &vU, const viz::Vec3 &vV, float plateW, float plateL) {
    BoltFit fit;
    const auto &mn = engine.macroNormal();
    const viz::Vec3 eN{mn[0], mn[1], mn[2]};
    viz::Vec3 eU, eV;
    if (std::fabs(eN.x) < 1e-6f && std::fabs(eN.z) < 1e-6f) {
        eU = viz::Vec3{1, 0, 0};
        eV = viz::Vec3{0, 0, 1};
    } else {
        eU = viz::Vec3{0, 1, 0}.cross(eN).normalized();
        eV = eU.cross(eN);
    }
    const float dn = std::max(-1.0f, std::min(1.0f, eN.dot(vN)));
    fit.normalDeg = std::acos(dn) * 180.0f / 3.14159265358979f;
    const float du = std::max(-1.0f, std::min(1.0f, eU.dot(vU)));
    fit.frameDeg = std::acos(du) * 180.0f / 3.14159265358979f;
    fit.viewerNY = vN.y;
    fit.engineNY = eN.y;

    // The four corners of the actuator pattern (kBoltUV entries 0, 6, 28, 34 sit at
    // exactly (0,0),(1,0),(0,1),(1,1)) are the ones that leave the plate first.
    const float halfW = plateW * 0.5f, halfL = plateL * 0.5f;
    for (int c = 0; c < 4; c++) {
        const float sx = (c & 1) ? 1.0f : -1.0f;
        const float sz = (c & 2) ? 1.0f : -1.0f;
        // Placed by the shader: pos + hx*eU + y*eN + hz*eV (y is the surface height,
        // ~cm, so it cannot move a bolt off the plate).
        const viz::Vec3 world = platePos + eU * (sx * halfW) + eV * (sz * halfL);
        const viz::Vec3 d = world - platePos;
        const float lx = d.dot(vU), lz = d.dot(vV);
        const float over = std::max(std::fabs(lx) - halfW, std::fabs(lz) - halfL);
        if (over > 0.25f) fit.outside++;   // 0.25 m -the marker radius
        fit.worstM = std::max(fit.worstM, over);
    }
    return fit;
}

// Macro basis exactly as shaders/common.slang computes it (bisector of sun and
// heliostat->aim), needed to draw a flat plate for a mirror the engine is not
// tracing.
void macroBasisFor(const std::array<float, 3> &sunDir, const std::array<float, 3> &heliostatPos,
                   const std::array<float, 3> &aim, viz::Vec3 &n, viz::Vec3 &u, viz::Vec3 &v) {
    const viz::Vec3 s = viz::Vec3{sunDir[0], sunDir[1], sunDir[2]}.normalized();
    const viz::Vec3 r = (viz::Vec3{aim[0], aim[1], aim[2]} - viz::Vec3{heliostatPos[0], heliostatPos[1],
                                                                       heliostatPos[2]})
                            .normalized();
    n = (s + r).normalized();
    if (std::fabs(n.x) < 1e-6f && std::fabs(n.z) < 1e-6f) {
        u = viz::Vec3{1, 0, 0};
        v = viz::Vec3{0, 0, 1};
    } else {
        u = viz::Vec3{0, 1, 0}.cross(n).normalized();
        v = u.cross(n);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    // Attach to the parent console when launched from a shell so the diagnostics
    // are visible; stay silent (no console window) when double-clicked.
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE *f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }

    Options opts;
    bool help = false;
    if (!parseArgs(commandLineArgs(), opts, help)) return 2;
    if (help) {
        usage();
        return 0;
    }
    // --log: GUI-subsystem binaries get no usable stdout when launched from a
    // script, so the diagnostics can be redirected to a file instead. Opened with
    // _SH_DENYWR (not the CRT default) so a running viewer's log can be tailed by
    // another process -the acceptance scripts read it live.
    FILE *logOut = nullptr;
    if (!opts.logFile.empty()) {
        if (auto parent = fs::path(opts.logFile).parent_path(); !parent.empty()) fs::create_directories(parent);
        logOut = _fsopen(opts.logFile.c_str(), "w", _SH_DENYWR);
        if (logOut) {
            _dup2(_fileno(logOut), _fileno(stdout));
            _dup2(_fileno(logOut), _fileno(stderr));
        }
    }

    try {
        // ------------------------------------------------------------ window --
        viz::Window window;
        if (!window.create(L"Heliostat Studio", opts.width, opts.height)) return 1;
        viz::Input input;
        viz::Camera camera;
        viz::FrameClock clock;
        clock.reset();
        FrameLimiter limiter;
        limiter.init();
        limiter.setCap(opts.fpsCap);

        // ------------------------------------------------------------ Vulkan --
        hviz::VkCore vk(opts.validate, window.hwnd());
        viz::VkContext ctx;
        ctx.init(vk, window.hwnd(), static_cast<uint32_t>(opts.width), static_cast<uint32_t>(opts.height),
                 opts.vsync);
        if (!opts.presentMode.empty()) {
            if (opts.presentMode == "fifo") ctx.setPresentMode(VK_PRESENT_MODE_FIFO_KHR);
            else if (opts.presentMode == "mailbox") ctx.setPresentMode(VK_PRESENT_MODE_MAILBOX_KHR);
            else if (opts.presentMode == "immediate") ctx.setPresentMode(VK_PRESENT_MODE_IMMEDIATE_KHR);
            else throw std::runtime_error("--present must be fifo|mailbox|immediate");
        }

        // ------------------------------------------------------------ assets --
        const std::string exeDir = hviz::executableDir();
        const std::string configPath = resolveConfigPath(opts.config, exeDir);
        if (configPath.empty()) {
            std::fprintf(stderr,
                         "[viz] WARNING: config '%s' not found - falling back to built-in defaults.\n"
                         "      The Buie sun-shape constants (kappa/gamma) are derived from the config's "
                         "CSR, so the flux will not match BASELINE.md without it.\n",
                         opts.config.c_str());
        }
        hviz::Config cfg = hviz::loadConfig(configPath.empty() ? opts.config : configPath);
        cfg.sunFile = hviz::resolveAssetPath(cfg.sunFile, opts.config, exeDir);
        cfg.ellipseFile = hviz::resolveAssetPath(cfg.ellipseFile, opts.config, exeDir);
        cfg.proxyPath = hviz::resolveAssetPath(cfg.proxyPath, opts.config, exeDir);

        auto suns = hviz::loadSunDirections(cfg.sunFile);
        auto heliostats = hviz::loadHeliostats(cfg.ellipseFile);
        if (heliostats.empty()) throw std::runtime_error("no heliostat in " + cfg.ellipseFile);
        const hviz::HeliostatConfig &hc = heliostats.front();
        // --sun <n>: use the file's direction verbatim (parity runs); otherwise the
        // direction comes from the live azimuth/elevation sliders.
        const bool useFileSun = opts.sunIndex >= 0 && opts.sunIndex < static_cast<int>(suns.size());
        if (opts.sunIndex >= 0 && !useFileSun) {
            std::fprintf(stderr, "[viz] WARNING: --sun %d out of range (%zu directions in %s); using sliders\n",
                         opts.sunIndex, suns.size(), cfg.sunFile.c_str());
        }

        const std::string shaderDir = resolveShaderDir(opts.shaderDir, exeDir);
        hviz::ForwardEngine engine(vk, cfg);
        engine.init(shaderDir, hc);

        // Bolt presets: zero / LSQ ellipse fit / end-to-end optimized. The slider
        // interpolates zero -> optimized, which is the 4.8x spot-contraction
        // story measured in docs/perf_log.md.
        // Bolt presets: zero / LSQ ellipse fit / end-to-end optimized. The slider
        // interpolates zero -> optimized, which is the 4.8x spot-contraction story
        // measured in docs/perf_log.md. The files live under data/bolts (NOT next to
        // the proxy data), so they are resolved against the project root like every
        // other asset.
        BoltPresets bolts;
        bolts.zero.assign(cfg.numBolts, 0.0f);
        bolts.optimized = bolts.zero;
        bolts.lsq = bolts.zero;
        auto loadPreset = [&](const char *name, std::vector<float> &out) {
            const std::string p = hviz::resolveAssetPath(std::string("data/bolts/") + name, opts.config, exeDir);
            if (fs::exists(p)) {
                out = hviz::loadBolts(p, cfg.numBolts);
                const auto mm = std::minmax_element(out.begin(), out.end());
                std::printf("[viz] bolt preset %-26s %s  (stroke %.1f .. %.1f mm)\n", name, p.c_str(),
                            *mm.first * 1000.0f, *mm.second * 1000.0f);
            } else {
                std::fprintf(stderr, "[viz] WARNING: bolt preset %s not found (%s) - preset disabled\n",
                             name, p.c_str());
            }
        };
        loadPreset("North_300m_optimized.txt", bolts.optimized);
        loadPreset("North_300m_lsq_init.txt", bolts.lsq);
        if (!opts.bolts.empty())
            bolts.optimized = hviz::loadBolts(hviz::resolveAssetPath(opts.bolts, opts.config, exeDir), cfg.numBolts);
        bolts.blend(1.0f);
        engine.setBolts(bolts.optimized);

        // ------------------------------------------------------------ renderers --
        viz::SceneRenderer scene;
        scene.init(vk, engine, ctx.extent(), shaderDir, ctx.format());
        viz::PostStack post;
        post.init(vk, ctx.format(), ctx.extent(), shaderDir);
        for (uint32_t f = 0; f < viz::VkContext::kFramesInFlight; f++) {
            post.setSceneTexture(f, scene.hdrTexture(f).view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        viz::FluxPipeline flux;
        flux.init(vk, engine, shaderDir);
        viz::Hud hud;
        if (!opts.noUI) hud.init(vk, ctx.format(), ctx.extent(), shaderDir);
        // UI scale: the default is *auto* (derived from the window size, so a maximised
        // window on a big screen gets a 2x bitmap font instead of a postage stamp);
        // --hud-scale pins it explicitly, and 'U' cycles it at runtime.
        hud.setScale(opts.hudScale > 0.0f ? opts.hudScale : 1.0f);
        hud.setDumpLayout(opts.uiLayout);
        FluxReadback fluxRead;
        fluxRead.init(vk, engine, viz::VkContext::kFramesInFlight, 15);   // every 15 frames

        UiState ui;
        ui.spp = opts.sppOverride > 0.0f ? static_cast<uint32_t>(opts.sppOverride) : 64;
        ui.spp = std::min(ui.spp, flux.maxSpp());
        ui.vsync = opts.vsync;
        ui.setFpsCap(opts.fpsCap);
        ui.convergence = opts.tOverride >= 0.0f ? opts.tOverride : 1.0f;
        ui.bloomThreshold = opts.bloomThreshold;
        if (opts.bloomStrength >= 0.0f) ui.bloomStrength = opts.bloomStrength;
        ui.diagAtomics = opts.atomics;
        ui.rayCull = !opts.noCull;
        ui.debugView = opts.debugView;
        ui.aimOffsetDeg = opts.aimOffsetDeg;
        ui.sunPath = std::max(0, std::min(3, opts.sunPath));
        ui.heliostat = std::max(0, std::min(3, opts.mirror));
        if (opts.noFluxMap) ui.showFluxMap = false;
        if (opts.noCardinals) ui.showCardinals = false;
        if (opts.noBolts) opts.boltScale = 0.0f;
        if (opts.noBeams) ui.beamMode = 0;
        if (opts.sunHourDeg < 1e8f) { ui.sunPathHourDeg = opts.sunHourDeg; ui.sunPathAnimating = false; }
        ui.deformScaleIndex = std::max(0, std::min(2, opts.deformIdx));
        hviz::FluxStats lastStats;

        std::printf("[viz] plate %.2f x %.2f m, grid %u, %u bolts; receiver R=%.1f H=%.1f\n", cfg.plateWidth,
                    cfg.plateLength, cfg.gridSize, cfg.numBolts, cfg.receiverRadius, cfg.receiverHeight);
        std::printf("[viz] physics: config %s | dni %.0f | csr %.4f | buie kappa %.4f gamma %.4f | "
                    "shapeIntegral %.6e | slope %.4f rad | reflectivity %.2f\n",
                    configPath.empty() ? "(defaults!)" : configPath.c_str(), cfg.dni, cfg.csr, cfg.buieKappa,
                    cfg.buieGamma, cfg.sunShapeIntegral, cfg.slopeError, cfg.reflectivity);
        std::printf("[viz] sun file %s (%zu directions), heliostat %s @ (%.0f, %.0f, %.0f)\n",
                    cfg.sunFile.c_str(), suns.size(), hc.name.c_str(), hc.position[0], hc.position[1],
                    hc.position[2]);
        std::printf("[viz] shaders %s\n", shaderDir.c_str());
        std::printf("[viz] present mode %s, %u swapchain images, display %.0f Hz, timestamp queries %s\n",
                    ctx.presentModeName(), ctx.imageCount(), ctx.refreshRateHz(),
                    ctx.timer().valid() ? "ready" : (vk.timestampsSupported() ? "pending" : "UNSUPPORTED"));

        // Writes the BMP plus a text preview (the preview is what a terminal-only
        // run can actually check).
        auto writeShot = [&](const std::string &path, const std::vector<uint8_t> &pixels, uint32_t w,
                             uint32_t h) {
            if (saveBmp(path, pixels, w, h)) {
                std::string preview = path;
                if (auto dot = preview.rfind('.'); dot != std::string::npos) preview.erase(dot);
                preview += ".txt";
                saveAsciiPreview(preview, pixels, w, h);
                std::printf("[shot] %s (%ux%u) + %s\n", path.c_str(), w, h, preview.c_str());
            }
        };

        // Screenshot capture, delayed by design: the frame's copy is recorded into
        // the buffer of its frame slot and read back two frames later, once that
        // slot's fence has been waited on. No stall is ever introduced.
        hviz::Buffer capture[viz::VkContext::kFramesInFlight];
        std::string pendingShot[viz::VkContext::kFramesInFlight];
        const uint64_t captureBytes =
            static_cast<uint64_t>(ctx.extent().width) * ctx.extent().height * 4;
        auto createCaptureBuffers = [&](VkExtent2D ext) {
            for (auto &b : capture) {
                if (b.buffer) vk.destroyBuffer(b);
                b = vk.createBuffer(static_cast<VkDeviceSize>(ext.width) * ext.height * 4,
                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
            }
        };
        if (!opts.screenshot.empty() || !opts.recordDir.empty()) createCaptureBuffers(ctx.extent());

        bool running = true;
        int frameNo = 0;
        int shots = 0;
        double titleTimer = 0.0;
        // Every key/toggle announces itself in the panel for ~3.5 s: several switches
        // (heat range, beams, cull, atomics) only change something subtle, and a HUD
        // that silently ignores a key is indistinguishable from a broken key.
        auto toast = [&](const std::string &msg) {
            ui.toastText = msg;
            ui.toastUntil = static_cast<float>(ui.time) + 3.5f;
            std::printf("[viz] %s\n", msg.c_str());
            std::fflush(stdout);
        };
        auto selectToast = [&](int idx) {
            static const char *names[4] = {"North", "East", "South", "West"};
            toast(std::string("4/5/6/7 -> tracing mirror ") + names[idx] + " (camera flew to it)");
        };
        bool hudScaleLocked = opts.hudScale > 0.0f;   // --hud-scale wins over the auto scale
        auto applyAutoScale = [&](VkExtent2D e) {
            if (hudScaleLocked) return;
            // Ladder chosen so the panel stays a *status column* rather than filling the
            // screen: 1x at 720p/1080p, 1.5x from ~1800x1000, 2x from ~2400x1350,
            // 3x from ~3600x2000. 'U' cycles it and --hud-scale pins any value.
            const float fit = std::min(static_cast<float>(e.width) / 1280.0f,
                                       static_cast<float>(e.height) / 720.0f);
            float s = 1.0f;
            if (fit >= 2.8f) s = 3.0f;
            else if (fit >= 1.9f) s = 2.0f;
            else if (fit >= 1.4f) s = 1.5f;
            ui.hudScale = s;
            hud.setScale(ui.hudScale);
        };
        applyAutoScale(ctx.extent());
        ui.hudScale = opts.hudScale > 0.0f ? opts.hudScale : ui.hudScale;
        std::printf("[viz] UI scale %.2f (%s) - U cycles it, --hud-scale pins it\n", ui.hudScale,
                    opts.hudScale > 0.0f ? "pinned by --hud-scale" : "auto from the window size");
        double fpsAccum = 0.0;
        int fpsFrames = 0;
        double wallMsAvg = 0.0;
        float gpuMsAvg = 0.0;
        double perfTimer = 0.0;
        int perfFrames = 0;
        FILE *perfFile = nullptr;
        if (!opts.perfCsv.empty()) {
            const bool exists = fs::exists(opts.perfCsv);
            if (auto parent = fs::path(opts.perfCsv).parent_path(); !parent.empty())
                fs::create_directories(parent);
            perfFile = std::fopen(opts.perfCsv.c_str(), "a");
            if (perfFile && !exists) {
                std::fprintf(perfFile,
                             "frame,gpu_ms_deform,gpu_ms_flux,gpu_ms_scene,gpu_ms_post,gpu_ms_total,"
                             "wall_ms,fps,spp,cull,atomics,rays_m,s95_area_m2\n");
            }
        }

        // Sun direction: from the sliders (az/el) unless a --sun index is asked for.
        auto computeSunDir = [&](float azDeg, float elDeg) {
            const float az = azDeg * 3.14159265358979f / 180.0f;
            const float el = elDeg * 3.14159265358979f / 180.0f;
            return std::array<float, 3>{std::cos(el) * std::sin(az), std::sin(el), std::cos(el) * std::cos(az)};
        };

        std::printf("\n--- controls: LMB orbit / drag the panel sliders | RMB/MMB pan | wheel zoom | "
                    "WASD/QE fly | F1-F4 camera | F5 bolts | F6 deform | F7 debug | F8 beams | "
                    "F9 exposure | F10 vsync | 1 cull | 2 spp | 3 atomics | H panel | M heat range | "
                    "K recentre beam | Space shot | ESC quit ---\n");
        if (opts.demoSeconds > 0.0f) {
            std::printf("[viz] DEMO MODE: a fixed %.1f s scripted sequence (%d phases: sun sweep, receiver "
                        "spot, convergence slider, atomics A/B, debug views).\n"
                        "      It drives the camera and the sliders itself; when the sequence ends it %s.\n"
                        "      Run without --demo for free interaction from the first frame.\n",
                        kDemoTotal, kDemoPhaseCount,
                        opts.demoExit ? "terminates the process (--demo-exit)"
                                      : "returns control to you (use --demo-exit to terminate)");
        }
        std::printf("\n");

        while (running) {
            if (!window.pump(input)) break;
            limiter.wait(clock);            // --fps cap (default 60)
            const double wallMs = clock.tickMs();
            const float dt = static_cast<float>(wallMs * 1e-3);
            ui.time += dt;

            // ------------------------------------------------------------ input --
            if (input.keyPressed(VK_ESCAPE)) break;
            if (input.keyPressed(VK_SPACE)) shots++;   // screenshot request
            if (input.keyPressed('P')) {
                ui.paused = !ui.paused;
                toast(ui.paused ? "P -> paused (physics frozen)" : "P -> running");
            }
            if (input.keyPressed(VK_F1)) { ui.preset = 0; toast("F1 -> camera: field overview"); }
            if (input.keyPressed(VK_F2)) { ui.preset = 1; toast("F2 -> camera: fly to the selected mirror"); }
            if (input.keyPressed(VK_F3)) { ui.preset = 2; toast("F3 -> camera: receiver spot closeup"); }
            if (input.keyPressed(VK_F4)) { ui.preset = 3; toast("F4 -> camera: beam side view (mirror to tower)"); }
            // F2 = "fly to the mirror that is currently selected" (preset 1 is the plate
            // closeup, framed on the selected mirror's reflecting face).
            if (input.keyPressed(VK_F5)) {
                ui.boltMode = ui.boltMode == BoltMode::Converge
                                  ? BoltMode::Lsq
                                  : (ui.boltMode == BoltMode::Lsq ? BoltMode::Zero : BoltMode::Converge);
                toast(std::string("F5 -> bolt preset: ") + boltModeName(ui.boltMode));
            }
            if (input.keyPressed(VK_F6)) {
                ui.deformScaleIndex = (ui.deformScaleIndex + 1) % 3;
                const char *x = ui.deformScaleIndex == 0 ? "1x" : (ui.deformScaleIndex == 1 ? "50x" : "200x");
                toast(std::string("F6 -> deformation exaggeration: ") + x + " (display only)");
            }
            if (input.keyPressed(VK_F7)) {
                ui.debugView = (ui.debugView + 1) % 4;
                toast(std::string("F7 -> plate view: ") + debugViewName(ui.debugView));
            }
            if (input.keyPressed(VK_F8)) {
                ui.beamMode = (ui.beamMode + 1) % 3;
                const char *b = ui.beamMode == 0 ? "off" : (ui.beamMode == 1 ? "normal" : "strong (2x)");
                toast(std::string("F8 -> light beams: ") + b);
            }
            if (input.keyPressed(VK_F9)) ui.exposure = std::min(4.0f, ui.exposure * 1.25f);
            if (input.keyDown(VK_CONTROL) && input.keyPressed('9')) ui.exposure = std::max(0.25f, ui.exposure * 0.8f);
            if (input.keyPressed(VK_F10)) ctx.toggleVsync();
            // F11 cycles the software frame-rate cap: 60 -> 120 -> uncapped.
            if (input.keyPressed(VK_F11)) {
                const float next = ui.fpsCap < 1.0f ? 60.0f : (ui.fpsCap < 61.0f ? 120.0f : 0.0f);
                ui.setFpsCap(next);
                limiter.setCap(next);
                toast(std::string("F11 -> frame cap: ") + ui.fpsCapName + " fps");
            }
            // U cycles the UI scale (it scales the bitmap font too, so the glyphs stay
            // crisp: they are drawn in whole-pixel blocks).
            if (input.keyPressed('U')) {
                const float ladder[] = {0.0f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f};   // 0 = auto
                int idx = 0;
                for (int i = 0; i < 6; i++) {
                    if (std::fabs(ladder[i] - ui.hudScale) < 0.01f) idx = i;
                }
                ui.hudScale = ladder[(idx + 1) % 6];
                hudScaleLocked = ui.hudScale > 0.0f;
                // The scale has to be pushed into the HUD here: applyAutoScale() returns
                // early while the scale is pinned, so setting ui.hudScale alone did
                // nothing on screen.
                if (hudScaleLocked) {
                    hud.setScale(ui.hudScale);
                } else {
                    applyAutoScale(ctx.extent());
                }
                toast(ui.hudScale > 0.0f ? ("U -> UI scale " + trimFloat(ui.hudScale))
                                         : std::string("U -> UI scale AUTO"));
            }
            if (input.keyPressed('1')) {
                ui.rayCull = !ui.rayCull;
                toast(ui.rayCull ? "1 -> A/B: A1 per-ray pre-cull ON" : "1 -> A/B: pre-cull OFF");
            }
            if (input.keyPressed('2')) {
                const uint32_t ladder[] = {16, 32, 64, 128, 256, 512, 1024};
                size_t idx = 0;
                while (idx < 7 && ladder[idx] != ui.spp) idx++;
                ui.spp = ladder[(idx + 1) % 7];
                toast("2 -> spp " + std::to_string(ui.spp));
            }
            if (input.keyPressed('3')) {
                ui.diagAtomics = !ui.diagAtomics;
                toast(ui.diagAtomics ? "3 -> A/B: per-ray global atomics ON (watch the flux pass)"
                                     : "3 -> A/B: per-ray atomics OFF");
            }
            if (input.keyPressed('H')) ui.panelVisible = !ui.panelVisible;
            // M: heat-map range. Auto tracks the read-back peak (x1.25, slow fall-off);
            // fixed pins the ceiling where it is now.
            if (input.keyPressed('M')) {
                ui.heatAutoRange = !ui.heatAutoRange;
                char mbuf[96];
                std::snprintf(mbuf, sizeof(mbuf), "M -> heat range: %s (ceiling %.0f W/px)",
                              ui.heatAutoRange ? "AUTO (tracks the peak)" : "FIXED", ui.fluxCeil);
                toast(mbuf);
            }
            if (input.keyPressed('K')) {
                ui.aimOffsetDeg = 0.0f;   // recentre the beam target
                toast("K -> beam target recentred to +0.0 deg");
            }
            if (input.keyPressed('F')) {
                ui.showFluxMap = !ui.showFluxMap;      // flux inset
                toast(ui.showFluxMap ? "F -> flux-map inset ON" : "F -> flux-map inset OFF");
            }
            if (input.keyPressed('C')) {
                ui.showCardinals = !ui.showCardinals;  // N/E/S/W markers
                toast(ui.showCardinals ? "C -> compass + ground markers ON"
                                       : "C -> compass + ground markers OFF");
            }
            // 4/5/6/7 select the field mirror that gets ray traced (N/E/S/W) *and*
            // fly the camera to it, so one key both switches the physics target and
            // shows you that mirror. F2 (below) does the same for whichever mirror is
            // already selected, so the two paths stay consistent.
            if (input.keyPressed('4')) { ui.heliostat = 0; ui.preset = 1; selectToast(0); }
            if (input.keyPressed('5')) { ui.heliostat = 1; ui.preset = 1; selectToast(1); }
            if (input.keyPressed('6')) { ui.heliostat = 2; ui.preset = 1; selectToast(2); }
            if (input.keyPressed('7')) { ui.heliostat = 3; ui.preset = 1; selectToast(3); }
            // 8 toggles the Delingha day path off *and* (with the field switch below)
            // stops the field's ray tracing: the compute chain is simply not recorded,
            // so the flux pass disappears from the per-pass table and the receiver goes
            // cold. The kernel itself is untouched.
            if (input.keyPressed('8')) {
                ui.fieldTraced = !ui.fieldTraced;
                toast(ui.fieldTraced ? "8 -> field ray tracing ON (compute chain recorded)"
                                     : "8 -> field ray tracing OFF (chain skipped, receiver cold)");
            }
            // 9/0 cycle the Delingha day paths (off -> summer solstice -> equinox ->
            // winter solstice); 9 also restarts the sweep at sunrise.
            if (input.keyPressed('9')) {
                ui.sunPath = (ui.sunPath % 3) + 1;
                ui.sunPathHourDeg = -std::max(5.0f, sunPathMaxHourDeg(kFieldLatitudeDeg,
                                                                     kSunPaths[ui.sunPath - 1].declinationDeg) - 4.0f);
                toast(std::string("9 -> Delingha day path: ") + kSunPaths[ui.sunPath - 1].label);
            }
            if (input.keyPressed('0')) {
                ui.sunPath = 0;
                toast("0 -> day path OFF (sliders control the sun again)");
            }
            if (input.keyPressed('L')) {
                ui.sunPathAnimating = !ui.sunPathAnimating;
                toast(ui.sunPathAnimating ? "L -> day path animating" : "L -> day path held");
            }
            if (!opts.noUI) {
                // Panel first: the overlay owns the mouse while the cursor is over
                // it, so dragging a slider never also orbits the camera.
                float sliderValue = 0.0f;
                const int active = hud.handleMouse(input.mouseX, input.mouseY, input.mouseClicked(0),
                                                   input.mouseHeld(0), &sliderValue);
                switch (active) {
                case 1: ui.sunAzimuthDeg = sliderValue * 120.0f - 60.0f; break;
                case 2: ui.sunElevationDeg = 5.0f + sliderValue * 80.0f; break;
                case 3:
                    ui.convergence = sliderValue;
                    ui.boltMode = BoltMode::Converge;
                    break;
                case 4: ui.exposure = 0.25f + sliderValue * 2.75f; break;
                case 5: ui.bloomStrength = sliderValue * 2.0f; break;
                case 6: ui.aimOffsetDeg = sliderValue * 120.0f - 60.0f; break;   // beam target
                case 7: ui.bloomThreshold = 0.5f + sliderValue * 7.5f; break;
                default: break;
                }
                viz::Input cameraInput = input;
                if (active >= 0 || hud.wantsMouse()) {
                    cameraInput.mouseDown[0] = false;
                    cameraInput.mousePressed[0] = false;
                }
                camera.update(cameraInput, dt);
            }

            if (opts.preset >= 0) ui.preset = opts.preset;

            // ---------------------------------------------------- demo script --
            if (opts.demoSeconds > 0.0f) {
                const float t = static_cast<float>(ui.time);
                float acc = 0.0f;
                int phase = 0;
                for (int i = 0; i < kDemoPhaseCount; i++) {
                    if (t < acc + kDemoPhases[i].duration) {
                        phase = i;
                        break;
                    }
                    acc += kDemoPhases[i].duration;
                    phase = i;
                }
                const float local = (t - acc) / std::max(kDemoPhases[phase].duration, 0.01f);
                ui.demoCaption = kDemoPhases[phase].caption;
                switch (phase) {
                case 0:   // sun sweep across the field, overview camera
                    ui.preset = 0;
                    ui.sunAzimuthDeg = -45.0f + local * 90.0f;
                    ui.sunElevationDeg = 42.0f + 8.0f * std::sin(local * 3.14159f);
                    ui.boltMode = BoltMode::Converge;
                    ui.convergence = 1.0f;
                    ui.diagAtomics = false;
                    ui.debugView = 0;
                    ui.spp = 1024;
                    break;
                case 1:   // receiver closeup, spot + bloom
                    ui.preset = 2;
                    ui.bloomStrength = 0.55f + 0.35f * std::sin(local * 3.14159f);
                    ui.spp = 1024;
                    ui.diagAtomics = false;
                    break;
                case 2:   // convergence slider sweep
                    ui.preset = 1;
                    ui.convergence = 0.5f - 0.5f * std::cos(local * 3.14159f);   // smooth 0 -> 1
                    ui.boltMode = BoltMode::Converge;
                    ui.spp = 1024;
                    ui.diagAtomics = false;
                    break;
                case 3:
                    // A/B: the per-ray atomics, live. 64 spp keeps the recording
                    // smooth while the effect on the flux stage is still ~32x
                    // (0.09 ms -> 2.8 ms); the 1024 spp number (0.36 -> 48.8 ms)
                    // is in docs/perf_log.md.
                    ui.preset = 0;
                    ui.spp = 64;
                    ui.diagAtomics = (static_cast<int>(local * 4.0f) % 2) == 1;
                    break;
                default:  // debug views
                    if (phase == 5) {
                        // ---- four-mirror field: cycle N -> E -> S -> W -----------
                        ui.preset = 0;
                        ui.heliostat = std::min(3, static_cast<int>(local * 4.0f));
                        ui.showFluxMap = true;
                        ui.showCardinals = true;
                        ui.sunPath = 0;
                        ui.spp = 1024;
                        ui.diagAtomics = false;
                        ui.debugView = 0;
                        ui.convergence = 1.0f;
                        ui.boltMode = BoltMode::Converge;
                        ui.sunAzimuthDeg = 4.31f;
                        ui.sunElevationDeg = 47.6f;
                    } else if (phase == 6) {
                        // ---- flux-map inset on/off -------------------------------
                        ui.preset = 2;
                        ui.heliostat = 0;
                        ui.spp = 1024;
                        ui.showFluxMap = (static_cast<int>(local * 4.0f) % 2) == 0;
                        ui.showCardinals = true;
                        ui.debugView = 0;
                        ui.exposure = 1.0f;
                        ui.bloomStrength = 0.55f;
                    } else if (phase == 7) {
                        // ---- Delingha day path, animated (summer solstice first) ---
                        ui.preset = 0;
                        ui.heliostat = 0;
                        ui.showFluxMap = true;
                        ui.showCardinals = true;
                        ui.spp = 512;
                        ui.debugView = 0;
                        const int leg = std::min(2, static_cast<int>(local * 3.0f));
                        ui.sunPath = leg + 1;
                        ui.sunPathAnimating = false;
                        // each latitude sweeps -30 -> +30 deg of hour angle
                        ui.sunPathHourDeg = -30.0f + (local * 3.0f - leg) * 60.0f;
                    } else {
                        ui.preset = 1;
                        ui.sunPath = 0;
                        ui.heliostat = 0;
                        ui.debugView = static_cast<int>(local * 4.0f) % 4;
                        ui.spp = 512;
                        ui.diagAtomics = false;
                    }
                    break;
                }
                if (opts.demoSeconds > 0.0f && opts.demoExit && t >= kDemoTotal) {
                    std::printf("[viz] demo sequence finished (%d phases, %.1f s) - exiting because "
                                "--demo-exit was given.\n",
                                kDemoPhaseCount, kDemoTotal);
                    running = false;
                }
                if (opts.demoSeconds > 0.0f && !opts.demoExit && t >= kDemoTotal + 2.0f &&
                    ui.demoCaption == kDemoPhases[kDemoPhaseCount - 1].caption) {
                    ui.demoCaption = nullptr;   // banner cleared, interaction continues
                }
            }
            // Sun: keyboard nudges (the HUD sliders are added in D4).
            const float sunStep = (input.keyDown(VK_SHIFT) ? 4.0f : 0.6f);
            if (input.keyDown(VK_LEFT)) ui.sunAzimuthDeg -= sunStep;
            if (input.keyDown(VK_RIGHT)) ui.sunAzimuthDeg += sunStep;
            if (input.keyDown(VK_UP)) ui.sunElevationDeg = std::min(85.0f, ui.sunElevationDeg + sunStep);
            if (input.keyDown(VK_DOWN)) ui.sunElevationDeg = std::max(5.0f, ui.sunElevationDeg - sunStep);
            if (opts.azimuthOverride < 1e8f) ui.sunAzimuthDeg = opts.azimuthOverride;
            if (opts.elevationOverride < 1e8f) ui.sunElevationDeg = opts.elevationOverride;
            // Convergence slider (the mouse-driven version lands with the HUD).
            // Convergence fine-tune. NOTE: '[' and ']' are *not* VK codes - on a Windows
            // keyboard they arrive as VK_OEM_4 / VK_OEM_6, which is why these two keys
            // used to do nothing at all. A tap steps t by 0.02 (so a single press is
            // visible) and holding sweeps it at 0.7/s.
            {
                const bool dn = input.keyDown(VK_OEM_4) || input.keyDown(VK_OEM_6);
                float delta = 0.0f;
                if (input.keyPressed(VK_OEM_4)) delta -= 0.02f;
                if (input.keyPressed(VK_OEM_6)) delta += 0.02f;
                if (input.keyDown(VK_OEM_4)) delta -= dt * 0.7f;
                if (input.keyDown(VK_OEM_6)) delta += dt * 0.7f;
                if (delta != 0.0f) {
                    ui.convergence = std::max(0.0f, std::min(1.0f, ui.convergence + delta));
                    ui.boltMode = BoltMode::Converge;
                    char cb[110];
                    std::snprintf(cb, sizeof(cb), "%s -> convergence t = %.3f  (S95 %.2f m2)",
                                  delta < 0.0f ? "[" : "]", ui.convergence, ui.s95Area);
                    toast(cb);
                }
                (void)dn;
            }

            if (ui.boltMode == BoltMode::Converge) {
                bolts.blend(ui.convergence);
                engine.setBolts(bolts.blended);
            } else if (ui.boltMode == BoltMode::Lsq) {
                engine.setBolts(bolts.lsq);
            } else {
                engine.setBolts(bolts.zero);
            }

            // ---- the selected field mirror (only this one is ray traced) ----
            const Cardinal selected = static_cast<Cardinal>(std::max(0, std::min(3, ui.heliostat)));
            hviz::HeliostatConfig activeHc = hc;
            activeHc.name = cardinalName(selected);
            {
                const viz::Vec3 p = cardinalPosition(selected, std::sqrt(hc.position[0] * hc.position[0] +
                                                                          hc.position[2] * hc.position[2]));
                activeHc.position = {p.x, 0.0f, p.z};
            }

            // ---- sun: sliders, or the Delingha day-path demo ----
            std::array<float, 3> sunDir{};
            if (ui.sunPath > 0) {
                const SunPathMode &path = kSunPaths[ui.sunPath - 1];
                const float maxH = sunPathMaxHourDeg(kFieldLatitudeDeg, path.declinationDeg);
                // Sunrise -> sunset, minus a small margin so the sun never sits exactly
                // on the horizon (a day path starts and ends at elevation 0, where the
                // mirrors are edge-on and the frame would just go black).
                const float sweep = std::max(5.0f, maxH - 4.0f);
                if (ui.sunPathAnimating) {
                    // One full day in ~30 s, looping.
                    ui.sunPathHourDeg += dt * (2.0f * sweep / 30.0f);
                    if (ui.sunPathHourDeg > sweep) ui.sunPathHourDeg = -sweep;
                }
                const float step = (input.keyDown(VK_SHIFT) ? 6.0f : 1.5f);
                if (input.keyDown(VK_LEFT)) ui.sunPathHourDeg = std::max(-sweep, ui.sunPathHourDeg - step);
                if (input.keyDown(VK_RIGHT)) ui.sunPathHourDeg = std::min(sweep, ui.sunPathHourDeg + step);
                const viz::Vec3 d =
                    sunDirFromPath(kFieldLatitudeDeg, ui.sunPathHourDeg, path.declinationDeg);
                sunDir = {d.x, d.y, d.z};
                ui.sunAzimuthDeg = azimuthOf(d);
                ui.sunElevationDeg = elevationOf(d);
            } else {
                const std::array<float, 3> sliderSun = computeSunDir(ui.sunAzimuthDeg, ui.sunElevationDeg);
                sunDir = useFileSun ? suns[static_cast<size_t>(opts.sunIndex)] : sliderSun;
            }

            // ---- camera presets ----
            // Applied *after* the mirror selection and after the sun is known, so the
            // closeups can be framed on the selected mirror's own reflecting face:
            // F2/F3 used to frame the config's built-in heliostat (North) from behind,
            // which is why the actuator markers were never visible in the closeup.
            if (ui.preset >= 0) {
                const viz::Vec3 plate{activeHc.position[0], activeHc.position[1], activeHc.position[2]};
                const std::array<float, 3> hp3 = {plate.x, plate.y, plate.z};
                viz::Vec3 n, uu, vv;
                macroBasisFor(sunDir, hp3, aimPointFor(hp3, cfg, ui.aimOffsetDeg), n, uu, vv);
                camera.applyPreset(ui.preset, plate,
                                   viz::Vec3{cfg.receiverPos[0], cfg.receiverPos[1], cfg.receiverPos[2]}, n);
                ui.preset = -1;
            }

            // Rebuild the sparse active-receiver-pixel list when the traced mirror
            // changes (its facing half of the cylinder is a different one).
            engine.setActiveHeliostat(activeHc);
            engine.setSun(sunDir, activeHc);
            engine.setAimOffsetDeg(ui.aimOffsetDeg);                 // beam-target slider
            engine.setRayCull(ui.rayCull, cfg.rayCullMarginMrad);   // A/B switch 1

            // ------------------------------------------------------- resize --
            const uint32_t winW = window.width(), winH = window.height();
            if (window.consumeResize() || ctx.needsRecreate()) {
                if (!ctx.recreate(winW, winH)) {
                    // Deferred (minimised / no valid surface size): idle briefly
                    // instead of spinning on the rebuild attempt.
                    Sleep(10);
                    continue;
                }
                const VkExtent2D ext = ctx.extent();
                // Order matters: the scene re-registers its new HDR views first, so
                // the post stack never writes a descriptor pointing at a view that
                // scene.resize() just destroyed.
                scene.resize(ext);
                for (uint32_t f = 0; f < viz::VkContext::kFramesInFlight; f++) {
                    post.setSceneTexture(f, scene.hdrTexture(f).view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                }
                post.resize(ext);
                hud.resize(ext);
                applyAutoScale(ext);   // the auto UI scale follows the new window size
                if (capture[0].buffer) createCaptureBuffers(ext);
                std::printf("[viz] resize -> %ux%u\n", ext.width, ext.height);
                continue;
            }
            if (window.minimized()) {
                // Nothing to present while minimized; keep the CPU quiet.
                Sleep(16);
                continue;
            }

            if (!ctx.beginFrame()) continue;
            VkCommandBuffer cmd = ctx.cmd();
            const uint32_t frameIdx = ctx.frameIndex();
            viz::GpuTimer &timer = ctx.timer();

            // This slot's capture from 2 frames ago is now safe to read: its fence
            // has just been waited on inside beginFrame().
            if (!pendingShot[frameIdx].empty()) {
                const uint32_t w = ctx.extent().width, h = ctx.extent().height;
                std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
                std::memcpy(pixels.data(), capture[frameIdx].mapped, pixels.size());
                writeShot(pendingShot[frameIdx], pixels, w, h);
                pendingShot[frameIdx].clear();
                if (!opts.screenshot.empty() && frameNo >= opts.screenshotFrame) running = false;
            }

            // ------------------------------------------------- compute chain --
            // '8' switches the field's ray tracing off: the whole chain (deform ->
            // flux -> finalize -> S95) is simply not recorded into this frame, so the
            // tracing really stops -- the flux pass drops out of the per-pass table,
            // the receiver goes cold and the S95 numbers read 0. Nothing about the
            // kernel or its dispatch semantics changes; it is a per-frame decision by
            // the caller (see the FrameLoop contract in viz_main.cpp's header comment).
            viz::FluxPipeline::Settings fs2;
            fs2.spp = ui.spp;
            fs2.diagAtomics = ui.diagAtomics;
            fs2.rayCull = ui.rayCull;
            fs2.useReference = opts.referenceChain;
            if (ui.fieldTraced) {
                flux.record(cmd, fs2, &timer, frameIdx);
            } else {
                // Keep every stage ticked (the timer trims to the last written query, and
                // an unwritten stage would invalidate the whole frame's timings); both
                // stamps land back to back, so the deform/flux rows read ~0.000 ms.
                timer.stamp(cmd, frameIdx, viz::kStageDeform);
                timer.stamp(cmd, frameIdx, viz::kStageFlux);
                ui.s95Area = 0.0f;
                ui.fluxSum = 0.0f;
                ui.fluxPeak = 0.0f;
                ui.energyRetention = 0.0f;
            }

            // compute -> fragment: the flux texture is now a heat map.
            viz::imageBarrier(cmd, engine.fluxTexture().image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                              VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

            // Delayed statistics: copy the flux map into the slot's staging buffer
            // (read back two frames later) and refresh the HUD numbers.
            fluxRead.record(cmd, frameIdx, static_cast<uint64_t>(frameNo));
            if (fluxRead.collect(frameIdx)) {
                const hviz::FluxStats st = fluxRead.stats();
                ui.fluxSum = st.sum;
                ui.fluxPeak = st.max;
                ui.s95Area = st.s95AreaGpu;
                ui.s95Level = st.s95LevelGpu;
                ui.s95AreaCpu = st.s95AreaCpu;
                ui.energyRetention = st.energyRetention;
                fluxRead.centroid(&ui.fluxCenterU, &ui.fluxCenterV);
                lastStats = st;
                // Heat-map range follows the data: the flat plate peaks near 665 W/px
                // and the optimised one near 4800 W/px, so a fixed ceiling either
                // clips the focused spot or hides the diffuse one. Tracking the peak
                // (with a slow fall-off) keeps the spot's structure visible in both.
                if (ui.heatAutoRange && st.max > 1.0f) {
                    const float want = std::min(20000.0f, std::max(50.0f, st.max * 1.25f));
                    ui.fluxCeil += (want - ui.fluxCeil) * (want > ui.fluxCeil ? 0.25f : 0.05f);
                }
            }

            // ------------------------------------------------------ scene pass --
            viz::SceneRenderer::FrameInputs sin;
            sin.sunDir = viz::Vec3{sunDir[0], sunDir[1], sunDir[2]};
            sin.helioPos = viz::Vec3{activeHc.position[0], activeHc.position[1], activeHc.position[2]};
            sin.receiverPos = viz::Vec3{cfg.receiverPos[0], cfg.receiverPos[1], cfg.receiverPos[2]};
            sin.receiverRadius = cfg.receiverRadius;
            sin.receiverHeight = cfg.receiverHeight;
            sin.plateWidth = cfg.plateWidth;
            sin.plateLength = cfg.plateLength;
            sin.gridSize = static_cast<float>(cfg.gridSize);
            sin.deformScale = ui.deformScaleIndex == 0 ? 1.0f : (ui.deformScaleIndex == 1 ? 50.0f : 200.0f);
            sin.debugView = static_cast<float>(ui.debugView);
            sin.showBeams = ui.beamMode == 0 ? 0.0f : 1.0f;
            sin.beamIntensity = ui.beamMode == 2 ? 2.0f : 1.0f;
            sin.exposure = ui.exposure;
            sin.time = static_cast<float>(ui.time);
            sin.fluxLogFloor = 1.0f;
            sin.fluxLogCeil = ui.fluxCeil;
            sin.showCardinals = ui.showCardinals;
            sin.showFluxMap = ui.showFluxMap && !opts.noUI;
            // '8': with the tracing off the field is drawn hidden and the receiver shows
            // its cold body instead of a heat map (there is no fresh flux to show).
            if (!ui.fieldTraced) sin.field.clear();
            sin.fluxScale = ui.fieldTraced ? 1.0f : 0.0f;
            sin.s95Level = ui.s95Level;
            sin.fluxPeak = ui.fluxPeak;
            sin.boltRadius = opts.boltScale;
            sin.fluxCenterU = opts.noInsetCentre ? 0.5f : ui.fluxCenterU;
            sin.fluxCenterV = opts.noInsetCentre ? 0.5f : ui.fluxCenterV;
            // The whole field: the selected mirror gets the computed surface, the
            // others are drawn flat but aimed at the same receiver.
            for (int c = 0; c < 4; c++) {
                const Cardinal card = static_cast<Cardinal>(c);
                const viz::Vec3 p = cardinalPosition(card, std::sqrt(hc.position[0] * hc.position[0] +
                                                                     hc.position[2] * hc.position[2]));
                viz::SceneRenderer::FrameInputs::Mirror m;
                m.pos = p;
                const std::array<float, 3> hp3 = {p.x, 0.0f, p.z};
                const std::array<float, 3> aim = aimPointFor(hp3, cfg, ui.aimOffsetDeg);
                macroBasisFor(sunDir, hp3, aim, m.macroN, m.macroU, m.macroV);
                m.selected = (c == ui.heliostat);
                sin.field.push_back(m);
            }
            sin.aimPoint = engine.aimPoint();
            scene.render(cmd, frameIdx, camera, sin);
            timer.stamp(cmd, frameIdx, viz::kStageScene);

            // scene HDR -> bloom source + composite input
            viz::imageBarrier(cmd, scene.hdrTexture(frameIdx).image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                              VK_ACCESS_SHADER_READ_BIT);

            // ------------------------------------------------------------ bloom --
            viz::PostStack::Params pp;
            pp.bloomStrength = ui.bloomStrength;
            pp.bloomThreshold = ui.bloomThreshold;
            pp.bloomClamp = ui.bloomClamp;
            pp.exposure = ui.exposure;
            pp.time = static_cast<float>(ui.time);
            post.renderBloom(cmd, frameIdx, pp, &timer);

            // -------------------------------------------------------- composite --
            viz::imageBarrier(cmd, ctx.image(ctx.imageIndex()), VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            post.renderComposite(cmd, frameIdx, ctx.imageView(ctx.imageIndex()), ctx.extent(), pp);
            timer.stamp(cmd, frameIdx, viz::kStageComposite);

            // ----------------------------------------------------------- HUD --
            // Overlays that must bypass the post stack (bloom/ACES/vignette): the
            // flux-map inset first, then the panel on top of everything.
            if (sin.showFluxMap) {
                viz::imageBarrier(cmd, ctx.image(ctx.imageIndex()), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                  VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
                scene.renderOverlays(cmd, frameIdx, ctx.imageView(ctx.imageIndex()), ctx.extent());
            }
            if (!opts.noUI && ui.panelVisible) {
                // Composite wrote the image as a colour attachment; the overlay
                // loads it back, so order the write -> read dependency explicitly.
                viz::imageBarrier(cmd, ctx.image(ctx.imageIndex()), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                  VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
                viz::HudStats hs;
                hs.fps = wallMsAvg > 0.0 ? 1000.0 / wallMsAvg : 0.0;
                hs.wallMs = wallMsAvg;
                hs.timer = &timer;
                hs.raysPerFrame = static_cast<float>(flux.raysPerFrame(ui.spp));
                hs.spp = ui.spp;
                hs.rayCull = ui.rayCull;
                hs.diagAtomics = ui.diagAtomics;
                hs.gravity = fs2.gravityEnabled;
                hs.s95Area = ui.s95Area;
                hs.fluxSum = ui.fluxSum;
                hs.fluxPeak = ui.fluxPeak;
                hs.fluxCeil = ui.fluxCeil;
                hs.heatAutoRange = ui.heatAutoRange;
                hs.aimOffsetDeg = ui.aimOffsetDeg;
                hs.convergence = ui.convergence;
                hs.deformScale = sin.deformScale;
                hs.exposure = ui.exposure;
                hs.bloomStrength = ui.bloomStrength;
                hs.bloomThreshold = ui.bloomThreshold;
                hs.debugView = ui.debugView;
                hs.debugViewName = debugViewName(ui.debugView);
                hs.beamName = ui.beamMode == 0 ? "off" : (ui.beamMode == 1 ? "normal" : "strong 2x");
                hs.fieldTraced = ui.fieldTraced;
                hs.uiScale = ui.hudScale;
                hs.toast = ui.toastActive(static_cast<float>(ui.time)) ? ui.toastText.c_str() : nullptr;
                hs.boltModeName = boltModeName(ui.boltMode);
                hs.cameraName = camera.presetName();
                hs.presentMode = ctx.presentModeName();
                hs.deviceName = vk.deviceName().c_str();
                hs.triangles = scene.plateTriangles() + scene.receiverTriangles() + scene.boltTriangles() + 76;
                hs.vsync = ctx.vsync();
                hs.sunAzimuth = ui.sunAzimuthDeg;
                hs.sunElevation = ui.sunElevationDeg;
                hs.paused = ui.paused;
                hs.caption = ui.demoCaption;
                {
                    const viz::Vec3 fwd = camera.forward();
                    hs.cameraAzimuthDeg = azimuthOf(fwd);
                }
                hs.heliostatName = cardinalName(static_cast<Cardinal>(std::max(0, std::min(3, ui.heliostat))));
                hs.heliostatTraced = true;
                hs.sunPathIndex = ui.sunPath;
                hs.sunPathLatitude = kFieldLatitudeDeg;
                hs.sunPathLongitude = kFieldLongitudeDeg;
                hs.sunPathHourDeg = ui.sunPathHourDeg;
                hs.sunPathAnimating = ui.sunPathAnimating;
                if (ui.sunPath > 0) {
                    const SunPathMode &sp = kSunPaths[ui.sunPath - 1];
                    hs.sunPathLabel = sp.label;
                    hs.sunPathDate = sp.date;
                    hs.sunPathDeclination = sp.declinationDeg;
                    hs.sunPathNoonElevation = 90.0f - std::fabs(kFieldLatitudeDeg - sp.declinationDeg);
                    hs.sunPathDayLengthHours =
                        2.0f * sunPathMaxHourDeg(kFieldLatitudeDeg, sp.declinationDeg) / 15.0f;
                    hs.sunPathSolarHours = solarTimeHours(ui.sunPathHourDeg);
                    hs.sunPathBeijingHours = beijingTimeHours(hs.sunPathSolarHours);
                }
                hs.showFluxMap = sin.showFluxMap;
                hs.showCardinals = ui.showCardinals;
                hs.fpsCapName = ui.fpsCapName;
                if (sin.showFluxMap) {
                    const VkRect2D r = viz::SceneRenderer::fluxInsetRect(ctx.extent());
                    hs.insetX = static_cast<float>(r.offset.x);
                    hs.insetY = static_cast<float>(r.offset.y);
                    hs.insetW = static_cast<float>(r.extent.width);
                    hs.insetH = static_cast<float>(r.extent.height);
                }
                hud.build(hs, dt);
                // Report the scale the HUD actually settled on (it may shrink itself so
                // that the panel always fits the window).
                ui.hudScale = hud.scale();
                hud.render(cmd, ctx.imageView(ctx.imageIndex()), ctx.extent(), nullptr);
            }
            timer.stamp(cmd, frameIdx, viz::kStageHud);

            // ------------------------------------------------------- capture --
            const bool wantShot = shots > 0 || (!opts.screenshot.empty() && frameNo >= opts.screenshotFrame) ||
                                  (!opts.recordDir.empty() && frameNo % opts.recordStride == 0);
            if (wantShot && capture[frameIdx].buffer) {
                char name[512];
                if (!opts.recordDir.empty()) {
                    fs::create_directories(opts.recordDir);
                    std::snprintf(name, sizeof(name), "%s/frame_%05d.bmp", opts.recordDir.c_str(), frameNo);
                } else if (shots > 0) {
                    std::snprintf(name, sizeof(name), "shot_%03d.bmp", shots);
                    shots--;
                } else {
                    std::snprintf(name, sizeof(name), "%s", opts.screenshot.c_str());
                }
                if (auto parent = fs::path(name).parent_path(); !parent.empty()) fs::create_directories(parent);
                pendingShot[frameIdx] = name;

                viz::imageBarrier(cmd, ctx.image(ctx.imageIndex()), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {ctx.extent().width, ctx.extent().height, 1};
                vkCmdCopyImageToBuffer(cmd, ctx.image(ctx.imageIndex()), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       capture[frameIdx].buffer, 1, &region);
                viz::imageBarrier(cmd, ctx.image(ctx.imageIndex()), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0);
            } else {
                viz::imageBarrier(cmd, ctx.image(ctx.imageIndex()), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                  VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0);
            }

            ctx.endFrame();
            frameNo++;

            // ------------------------------------------------- statistics --
            fpsAccum += wallMs;
            fpsFrames++;
            wallMsAvg += (wallMs - wallMsAvg) * 0.1;
            if (timer.valid()) gpuMsAvg += (timer.totalMs() - gpuMsAvg) * 0.1f;
            perfTimer += wallMs;
            perfFrames++;
            titleTimer += wallMs;

            if (opts.stateLog && titleTimer >= 250.0) {
                // Bolt/plate frame consistency (see measureBoltFit): 'boltsOut' must be
                // 0 and 'frame' 0.0 deg, otherwise the markers do not sit on the plate.
                BoltFit fit{};
                if (ui.heliostat >= 0 && static_cast<size_t>(ui.heliostat) < sin.field.size()) {
                    const auto &m = sin.field[static_cast<size_t>(ui.heliostat)];
                    fit = measureBoltFit(engine, m.pos, m.macroN, m.macroU, m.macroV, cfg.plateWidth,
                                         cfg.plateLength);
                }
                std::printf("[state] spp %u cull %d atomics %d t %.3f az %.2f el %.2f aim %+.2f "
                            "exposure %.2f bloom %.2f panel %d s95 %.2f peak %.1f fps %.1f "
                            "cap %s cam %.2f mirror %s centre %.3f %.3f "
                            "boltsOut %d worst %.2fm frame %.2fdeg nrm %.2fdeg nY %+.3f/%+.3f sunY %+.3f "
                            "path %d dec %+.2f H %+.1f lst %.3f bjt %.3f daylen %.3f noonel %.3f "
                            "field %d beams %d view %d heat %d uiscale %.2f\n",
                            ui.spp, ui.rayCull ? 1 : 0, ui.diagAtomics ? 1 : 0, ui.convergence,
                            ui.sunAzimuthDeg, ui.sunElevationDeg, ui.aimOffsetDeg, ui.exposure,
                            ui.bloomStrength, ui.panelVisible ? 1 : 0, ui.s95Area, ui.fluxPeak,
                            (fpsAccum > 0.0 ? 1000.0 * fpsFrames / fpsAccum : 0.0), ui.fpsCapName,
                            camera.yawDeg(),
                            cardinalName(static_cast<Cardinal>(std::max(0, std::min(3, ui.heliostat)))),
                            ui.fluxCenterU, ui.fluxCenterV,
                            fit.outside, fit.worstM, fit.frameDeg, fit.normalDeg,
                            fit.viewerNY, fit.engineNY, sunDir[1],
                            ui.sunPath,
                            ui.sunPath > 0 ? kSunPaths[ui.sunPath - 1].declinationDeg : 0.0f,
                            ui.sunPathHourDeg, solarTimeHours(ui.sunPathHourDeg),
                            beijingTimeHours(solarTimeHours(ui.sunPathHourDeg)),
                            ui.sunPath > 0 ? 2.0f * sunPathMaxHourDeg(
                                                 kFieldLatitudeDeg, kSunPaths[ui.sunPath - 1].declinationDeg) /
                                                 15.0f
                                           : 0.0f,
                            ui.sunPath > 0
                                ? 90.0f - std::fabs(kFieldLatitudeDeg - kSunPaths[ui.sunPath - 1].declinationDeg)
                                : 0.0f,
                            ui.fieldTraced ? 1 : 0, ui.beamMode, ui.debugView,
                            ui.heatAutoRange ? 1 : 0, ui.hudScale);
                std::fflush(stdout);
            }
            if (titleTimer >= 250.0) {
                const double fps = fpsFrames > 0 ? 1000.0 / (fpsAccum / fpsFrames) : 0.0;
                char title[512];
                std::snprintf(title, sizeof(title),
                              "Heliostat Studio | %.1f FPS | wall %.2f ms | GPU %.2f ms (deform %.2f flux %.2f "
                              "scene %.2f post %.2f) | spp %u | cull %s | atomics %s | S95 %.1f m2 | t %.2f | %s",
                              fps, wallMsAvg, gpuMsAvg, timer.smoothedStageMs(viz::kStageDeform),
                              timer.smoothedStageMs(viz::kStageFlux), timer.smoothedStageMs(viz::kStageScene),
                              timer.smoothedStageMs(viz::kStageComposite), ui.spp, ui.rayCull ? "ON" : "OFF",
                              ui.diagAtomics ? "ON" : "OFF", ui.s95Area, ui.convergence,
                              ctx.presentModeName());
                window.setTitleUtf8(title);
                titleTimer = 0.0;
                fpsAccum = 0.0;
                fpsFrames = 0;
            }

            if (perfFile && perfTimer >= 1000.0) {
                const double fps = perfFrames > 0 ? 1000.0 * perfFrames / perfTimer : 0.0;
                std::fprintf(perfFile, "%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.2f,%u,%d,%d,%.2f,%.3f\n", frameNo,
                             timer.smoothedStageMs(viz::kStageDeform), timer.smoothedStageMs(viz::kStageFlux),
                             timer.smoothedStageMs(viz::kStageScene),
                             timer.smoothedStageMs(viz::kStageBloomPre) +
                                 timer.smoothedStageMs(viz::kStageBloomDown) +
                                 timer.smoothedStageMs(viz::kStageBloomUp) +
                                 timer.smoothedStageMs(viz::kStageComposite),
                             timer.smoothedTotalMs(), wallMsAvg, fps, ui.spp, ui.rayCull ? 1 : 0,
                             ui.diagAtomics ? 1 : 0, flux.raysPerFrame(ui.spp) / 1e6, ui.s95Area);
                std::fflush(perfFile);
                perfTimer = 0.0;
                perfFrames = 0;
            }

            if (opts.frames > 0 && frameNo >= opts.frames) running = false;
        }

        // ------------------------------------------------------------- report --
        // Flush any capture that is still in flight (shutdown only).
        vk.waitIdle();
        for (uint32_t i = 0; i < viz::VkContext::kFramesInFlight; i++) {
            if (pendingShot[i].empty()) continue;
            const uint32_t w = ctx.extent().width, h = ctx.extent().height;
            std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
            std::memcpy(pixels.data(), capture[i].mapped, pixels.size());
            writeShot(pendingShot[i], pixels, w, h);
            pendingShot[i].clear();
        }

        std::printf("\n[viz] %d frames, wall avg %.3f ms (%.1f FPS)\n", frameNo, wallMsAvg,
                    wallMsAvg > 0.0 ? 1000.0 / wallMsAvg : 0.0);
        std::printf("[viz] flux (viewer, delayed readback): sum %.1f W, peak %.1f W/px, "
                    "S95 %.2f m2 (level %.6f), S95 %.2f m2 with the CPU level, energy retention %.4f\n",
                    lastStats.sum, lastStats.max, lastStats.s95AreaGpu, lastStats.s95LevelGpu,
                    lastStats.s95AreaCpu, lastStats.energyRetention);
        // Verification of the per-ray-atomics A/B switch: the upstream diagnostic
        // counters are host-visible, so the switch is proven, not just timed.
        if (const auto *diag = static_cast<const uint32_t *>(engine.diagnosticBuffer().mapped)) {
            std::printf("[viz] diagBuf[5] (rays traced by the kernel): %u  [%s]\n", diag[5],
                        ui.diagAtomics ? "per-ray atomics were ON: this counter was written by the kernel"
                                       : "atomics OFF: counter untouched (value left over from a previous run)");
            std::printf("[viz] diagBuf[0] (TIR fallbacks) = %u, diagBuf[1..4] = %u %u %u %u\n", diag[0], diag[1],
                        diag[2], diag[3], diag[4]);
        }
        std::printf("[viz] GPU per pass (EMA, timestamp %s): deform %.3f | flux %.3f | scene %.3f | "
                    "bloom.pre %.3f | bloom.down %.3f | bloom.up %.3f | composite %.3f | total %.3f ms\n",
                    ctx.timer().valid() ? "valid" : "INVALID",
                    ctx.timer().smoothedStageMs(viz::kStageDeform),
                    ctx.timer().smoothedStageMs(viz::kStageFlux),
                    ctx.timer().smoothedStageMs(viz::kStageScene),
                    ctx.timer().smoothedStageMs(viz::kStageBloomPre),
                    ctx.timer().smoothedStageMs(viz::kStageBloomDown),
                    ctx.timer().smoothedStageMs(viz::kStageBloomUp),
                    ctx.timer().smoothedStageMs(viz::kStageComposite),
                    ctx.timer().smoothedTotalMs());
        std::printf("[viz] wall includes the present queue and the OS compositor; GPU ms is pure device time\n");
        ctx.printCounters();

        if (perfFile) std::fclose(perfFile);

        // Parity dump: blocking readback at shutdown (never inside the frame loop).
        if (!opts.dumpFlux.empty()) {
            if (auto parent = fs::path(opts.dumpFlux).parent_path(); !parent.empty())
                fs::create_directories(parent);
            const std::vector<float> map = engine.readFlux();
            hviz::saveNpy2D(opts.dumpFlux, map, cfg.pixelWidth, cfg.pixelHeight);
            double sum = 0.0;
            float peak = 0.0f;
            for (float f : map) {
                sum += f;
                peak = std::max(peak, f);
            }
            std::printf("[viz] dumped flux: %s (%zu values, sum %.1f W, peak %.3f W/px)\n", opts.dumpFlux.c_str(),
                        map.size(), sum, peak);
        }
        for (auto &b : capture)
            if (b.buffer) vk.destroyBuffer(b);
        fluxRead.destroy();
        limiter.destroy();
        hud.destroy();
        flux.destroy();
        post.destroy();
        scene.destroy();
        ctx.shutdown();
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "\nERROR: %s\n", e.what());
        MessageBoxA(nullptr, e.what(), "Heliostat Studio - fatal error", MB_OK | MB_ICONERROR);
        return 1;
    }
}
