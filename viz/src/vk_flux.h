#pragma once

// vk_flux.h — records the per-frame compute chain into the viewer's own command
// buffer (the engine's ForwardEngine::render() does its own submit + wait, which
// is fine for the CLI but must never happen inside a frame loop):
//
//   clearFluxLite -> computeBoltSurface -> fluxLite -> finalizeFluxLite -> S95
//
// It reuses the engine's descriptor set layout (so the verified upstream binding
// table, UBO packing and push constants are all untouched) and the engine's own
// pipelines for the two stages it does not override (deformation, S95).

#include "engine.h"
#include "gpu_timer.h"
#include "vk.h"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace viz {

class FluxPipeline {
public:
    struct Settings {
        uint32_t spp = 64;                  // A/B switch 2: samples per pixel
        bool diagAtomics = false;           // A/B switch 3: per-ray global atomic
        bool rayCull = true;                // A/B switch 1: A1 angular pre-cull
        bool deform = true;                 // run computeBoltSurface
        bool gravityEnabled = true;
        bool gravityNormalCoupling = true;
        bool runS95 = true;                 // cooperative S95 bisection
        // Parity mode: run the verified forward.slang chain (fixed 1024 spp)
        // instead of the real-time kernel, into the same flux texture.
        bool useReference = false;
    };

    void init(hviz::VkCore &vk, hviz::ForwardEngine &engine, const std::string &shaderDir);
    void destroy();

    // Records the whole chain into 'cmd'. When 'timer' is given, timestamp
    // stamps are written at the pass boundaries (deform / flux).
    void record(VkCommandBuffer cmd, const Settings &settings, GpuTimer *timer = nullptr,
                uint32_t frameIndex = 0);

    uint32_t tileCountFor(uint32_t spp) const;
    uint32_t activePixels() const { return m_activePixels; }
    uint32_t maxSpp() const { return m_maxSpp; }
    uint32_t gridPoints() const { return m_gridPoints; }
    double raysPerFrame(uint32_t spp) const {
        return static_cast<double>(m_activePixels) * static_cast<double>(spp);
    }

private:
    hviz::VkCore *m_vk = nullptr;
    hviz::ForwardEngine *m_engine = nullptr;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_clear = VK_NULL_HANDLE;
    VkPipeline m_flux = VK_NULL_HANDLE;
    VkPipeline m_finalize = VK_NULL_HANDLE;
    uint32_t m_maxSpp = 0;
    uint32_t m_gridPoints = 0;
    uint32_t m_activePixels = 0;
};

} // namespace viz
