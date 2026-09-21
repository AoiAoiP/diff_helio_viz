// main.cpp — headless CLI for the forward engine.
//
//   heliostat_core --config configs/probe_single_sun.json --dump-flux out/flux.npy
//   heliostat_core --config configs/probe_single_sun.json --bench 20
//   heliostat_core --config configs/probe_single_sun.json --parity data/baseline/North_300m_sun0_flux.npy
//
// The output is the same physics the upstream optimizer sees, so numbers can be
// compared one-to-one (see data/baseline/BASELINE.md).

#include "config.h"
#include "data.h"
#include "engine.h"
#include "vk.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Args {
    std::string config = "configs/probe_single_sun.json";
    std::string bolts;
    std::string dumpFlux;
    std::string parity;
    std::string shaderDir = "shaders";
    int sunIndex = 0;
    int repeat = 1;
    bool bench = false;
    bool benchReadback = false;
    bool validate = false;
    bool printCells = false;
};

// The .spv files land in <build>/shaders and are copied next to the executable.
// Prefer the copy that sits beside the binary (deterministic), then fall back to
// a few common working directories, so the CLI "just runs" either way.
std::string resolveShaderDir(const std::string &requested, const std::string &exeDir) {
    const std::string candidates[] = {exeDir + "/shaders", requested, "shaders", "build/shaders", "../shaders",
                                      "../../shaders", "../../../shaders"};
    for (const auto &c : candidates) {
        if (fs::exists(fs::path(c) / "clearFlux.spv")) return c;
    }
    return requested;
}

void usage() {
    std::puts(
        "Heliostat Studio - forward engine (core package)\n"
        "\n"
        "Usage: heliostat_core [options]\n"
        "  --config <file>      scene/config JSON (default configs/probe_single_sun.json)\n"
        "  --bolts <file>       bolt heights: 1 col value | 2 col 'idx h' | 3 col 'idx h_pipe h_stroke'\n"
        "  --sun <n>            index into the sun-direction file (default 0)\n"
        "  --dump-flux <npy>    write the 157x50 float32 flux map\n"
        "  --parity <npy>       compare against an upstream flux dump (roll-invariant metrics)\n"
        "  --bench <n>          render n times, report GPU ms statistics\n"
        "  --shader-dir <dir>   where the .spv files live (default 'shaders')\n"
        "  --validate           enable the Vulkan validation layer\n"
        "  --print-cells        print every receiver pixel as text (debug)\n");
}

} // namespace

int main(int argc, char **argv) {
    Args args;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&](const char *what) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + what);
            return argv[++i];
        };
        try {
            if (a == "--config") args.config = next("--config");
            else if (a == "--bolts") args.bolts = next("--bolts");
            else if (a == "--dump-flux") args.dumpFlux = next("--dump-flux");
            else if (a == "--parity") args.parity = next("--parity");
            else if (a == "--shader-dir") args.shaderDir = next("--shader-dir");
            else if (a == "--sun") args.sunIndex = std::stoi(next("--sun"));
            else if (a == "--bench") {
                args.bench = true;
                args.repeat = std::stoi(next("--bench"));
            } else if (a == "--bench-readback") {
                args.bench = true;
                args.benchReadback = true;
                args.repeat = std::stoi(next("--bench-readback"));
            } else if (a == "--validate") args.validate = true;
            else if (a == "--print-cells") args.printCells = true;
            else if (a == "-h" || a == "--help") {
                usage();
                return 0;
            } else {
                std::fprintf(stderr, "unknown option: %s\n\n", a.c_str());
                usage();
                return 1;
            }
        } catch (const std::exception &e) {
            std::fprintf(stderr, "argument error: %s\n", e.what());
            return 1;
        }
    }

    try {
        hviz::Config cfg = hviz::loadConfig(args.config);

        // Assets are written relative to the project root in the config files.
        // Resolve them so the CLI runs from any working directory.
        const std::string exeDir = hviz::executableDir();
        cfg.sunFile = hviz::resolveAssetPath(cfg.sunFile, args.config, exeDir);
        cfg.ellipseFile = hviz::resolveAssetPath(cfg.ellipseFile, args.config, exeDir);
        cfg.proxyPath = hviz::resolveAssetPath(cfg.proxyPath, args.config, exeDir);
        if (!cfg.boltFile.empty()) cfg.boltFile = hviz::resolveAssetPath(cfg.boltFile, args.config, exeDir);
        if (!args.bolts.empty()) {
            args.bolts = hviz::resolveAssetPath(args.bolts, args.config, exeDir);
            cfg.boltFile = args.bolts;
        }

        std::printf("=== Heliostat Studio - forward engine ===\n");
        std::printf("config      : %s\n", args.config.c_str());
        std::printf("plate       : %.2f x %.2f m, grid %u x %u (%u spp)\n", cfg.plateWidth, cfg.plateLength,
                    cfg.gridSize, cfg.gridSize, cfg.gridSize * cfg.gridSize);
        std::printf("receiver    : R=%.1f m H=%.1f m, %u x %u px\n", cfg.receiverRadius, cfg.receiverHeight,
                    cfg.pixelWidth, cfg.pixelHeight);
        std::printf("bolts       : %u%s\n", cfg.numBolts,
                    cfg.boltFile.empty() ? " (zero-initialised)" : (" from " + cfg.boltFile).c_str());
        std::printf("gravity     : %s (normal coupling %s)\n", cfg.disableGravity ? "OFF" : "ON",
                    cfg.gravityNormalCoupling ? "ON" : "OFF");
        std::printf("ray pre-cull: %s\n", cfg.rayCull ? "ON (A1)" : "OFF");

        auto suns = hviz::loadSunDirections(cfg.sunFile);
        auto heliostats = hviz::loadHeliostats(cfg.ellipseFile);
        if (args.sunIndex < 0 || args.sunIndex >= static_cast<int>(suns.size())) {
            throw std::runtime_error("--sun index out of range (file has " + std::to_string(suns.size()) + ")");
        }
        const auto &hc = heliostats.front();
        const auto &sunDir = suns[static_cast<size_t>(args.sunIndex)];
        std::printf("heliostat   : %s @ (%.1f, %.1f, %.1f)  |pos| = %.1f m\n", hc.name.c_str(), hc.position[0],
                    hc.position[1], hc.position[2],
                    std::sqrt(hc.position[0] * hc.position[0] + hc.position[1] * hc.position[1] +
                              hc.position[2] * hc.position[2]));
        std::printf("sun[%d]      : (%.6f, %.6f, %.6f)  elevation %.2f deg\n", args.sunIndex, sunDir[0],
                    sunDir[1], sunDir[2], std::asin(std::min(1.0f, sunDir[1])) * 180.0 / 3.14159265358979);

        std::vector<float> bolts(cfg.numBolts, 0.0f);
        if (!cfg.boltFile.empty()) {
            bolts = hviz::loadBolts(cfg.boltFile, cfg.numBolts);
            const auto [lo, hi] = std::minmax_element(bolts.begin(), bolts.end());
            std::printf("bolt file   : %s  (min %.2f mm, max %.2f mm)\n", cfg.boltFile.c_str(), *lo * 1000.0f,
                        *hi * 1000.0f);
        }

        hviz::VkCore vk(args.validate);
        hviz::ForwardEngine engine(vk, cfg);
        const std::string shaderDir = resolveShaderDir(args.shaderDir, exeDir);
        std::printf("shaders     : %s\n", shaderDir.c_str());
        engine.init(shaderDir, hc);
        engine.setBolts(bolts);
        engine.setSun(sunDir, hc);

        // ---- Warm-up render (also allocates any driver-side state) ----
        float gpuMs = 0.0f;
        engine.render(&gpuMs);
        std::printf("\nrender      : %.3f ms GPU (first, includes warm-up)\n", gpuMs);

        if (args.bench && args.repeat > 1) {
            std::vector<float> samples;
            samples.reserve(static_cast<size_t>(args.repeat));
            const auto wallStart = std::chrono::steady_clock::now();
            for (int i = 0; i < args.repeat; i++) {
                float ms = 0.0f;
                engine.render(&ms);
                samples.push_back(ms);
                if (args.benchReadback) {
                    // Fair-comparison mode: the upstream per-sun loop always reads
                    // the flux texture back to the host (staging alloc + queue wait).
                    auto flux = engine.readFlux();
                    if (flux.empty()) return 1;
                }
            }
            const auto wallEnd = std::chrono::steady_clock::now();
            const double wallMsPerFrame =
                std::chrono::duration<double, std::milli>(wallEnd - wallStart).count() / args.repeat;
            std::sort(samples.begin(), samples.end());
            double sum = 0.0;
            for (float s : samples) sum += s;
            const float rays = static_cast<float>(engine.activePixelCount()) * static_cast<float>(engine.totalSpp());
            std::printf("bench       : %d renders%s, min %.3f / med %.3f / max %.3f ms, mean %.3f ms\n", args.repeat,
                        args.benchReadback ? " (with flux readback each frame)" : "", samples.front(),
                        samples[samples.size() / 2], samples.back(), sum / samples.size());
            std::printf("wall clock  : %.3f ms/frame end-to-end (GPU timestamp + submit + any readback)\n",
                        wallMsPerFrame);
            std::printf("throughput  : %.1f Mray/s at %.2f Mray/frame (%.0f%% of receiver pixels active)\n",
                        samples[samples.size() / 2] > 0.0f ? rays / (samples[samples.size() / 2] * 1e3f) : 0.0f,
                        rays / 1e6f,
                        100.0f * static_cast<float>(engine.activePixelCount()) /
                            static_cast<float>(engine.totalPixels()));
        }

        auto flux = engine.readFlux();
        const hviz::FluxStats stats = engine.computeStats(flux);
        std::printf("\nflux        : sum %.1f W, peak %.1f W/px\n", stats.sum, stats.max);
        std::printf("S95 level   : GPU %.6f  |  CPU reference %.6f  (rel. diff %.2e)\n", stats.s95LevelGpu,
                    stats.s95LevelCpu,
                    stats.s95LevelCpu > 0.0f ? std::abs(stats.s95LevelGpu - stats.s95LevelCpu) / stats.s95LevelCpu
                                             : 0.0f);
        std::printf("S95 area    : %.2f m^2 (pixel area %.4f m^2)\n", stats.s95AreaGpu, engine.pixelArea());

        if (!args.dumpFlux.empty()) {
            if (auto parent = fs::path(args.dumpFlux).parent_path(); !parent.empty()) fs::create_directories(parent);
            hviz::saveNpy2D(args.dumpFlux, flux, cfg.pixelWidth, cfg.pixelHeight);
            std::printf("wrote       : %s\n", args.dumpFlux.c_str());
        }

        if (!args.parity.empty()) {
            std::vector<float> ref;
            uint32_t w = 0, h = 0;
            if (!hviz::loadNpy2D(args.parity, ref, w, h)) {
                std::fprintf(stderr, "parity: cannot read %s\n", args.parity.c_str());
            } else if (w != cfg.pixelWidth || h != cfg.pixelHeight) {
                std::fprintf(stderr, "parity: size mismatch %ux%u vs %ux%u\n", w, h, cfg.pixelWidth,
                             cfg.pixelHeight);
            } else {
                float refSum = 0.0f, refMax = 0.0f;
                for (float f : ref) {
                    refSum += f;
                    if (f > refMax) refMax = f;
                }
                const float refLevel = hviz::computeS95LevelCPU(ref);
                const float refArea = hviz::computeS95Area(ref, refLevel, engine.pixelArea());
                std::printf("\n--- parity vs %s ---\n", args.parity.c_str());
                std::printf("  flux sum   ref %.1f   here %.1f   rel %.3e\n", refSum, stats.sum,
                            refSum > 0.0f ? std::abs(stats.sum - refSum) / refSum : 0.0f);
                std::printf("  flux peak  ref %.3f   here %.3f\n", refMax, stats.max);
                std::printf("  S95 level  ref %.6f   here %.6f\n", refLevel, stats.s95LevelCpu);
                std::printf("  S95 area   ref %.2f m^2   here %.2f m^2   delta %.2f%%\n", refArea,
                            stats.s95AreaCpu,
                            refArea > 0.0f ? 100.0f * (stats.s95AreaCpu - refArea) / refArea : 0.0f);
                std::printf("  note: upstream flux dumps are circularly rolled to centre the spot;\n"
                            "        sum / peak / S95 are roll-invariant, pixel positions are not.\n");
            }
        }

        if (args.printCells) {
            std::printf("\nflux map (log-scaled, 0-9):\n");
            for (uint32_t y = 0; y < cfg.pixelHeight; y++) {
                std::string row;
                for (uint32_t x = 0; x < cfg.pixelWidth; x++) {
                    const float v = flux[y * cfg.pixelWidth + x];
                    const float lv = v > 0.0f ? std::log10(1.0f + v) / std::log10(1.0f + std::max(stats.max, 1.0f))
                                              : 0.0f;
                    row += static_cast<char>('0' + std::min(9, static_cast<int>(lv * 9.999f)));
                }
                std::printf("%s\n", row.c_str());
            }
        }

        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "\nERROR: %s\n", e.what());
        return 1;
    }
}
