#pragma once

// gpu_timer.h — per-pass GPU timing with vkCmdWriteTimestamp.
//
// One timestamp is written at the start of the frame and one at the end of each
// pass, so a pass duration is the difference of two consecutive timestamps. The
// pool is per frame-in-flight and read back *after* that slot's fence has been
// waited on — i.e. two frames late, with no stall in the frame loop.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

namespace viz {

enum Stage : uint32_t {
    kStageDeform = 0,   // computeBoltSurface (plate deformation)
    kStageFlux,         // clearFluxLite + fluxLite + finalizeFluxLite (+ S95)
    kStageScene,        // sky / ground / tower / receiver / plate / bolts / beams
    kStageBloomPre,     // bright pass + first downsample
    kStageBloomDown,    // 4x downsample chain
    kStageBloomUp,      // 4x tent upsample (additive)
    kStageComposite,    // ACES tonemap + LUT + sRGB encode
    kStageHud,          // bitmap-font overlay
    kStageCount
};

const char *stageName(uint32_t stage);

class GpuTimer {
public:
    void init(VkDevice device, float timestampPeriodNs, uint32_t framesInFlight, bool supported);
    void destroy();

    // Resets this frame's queries and writes the frame-start timestamp.
    void beginFrame(VkCommandBuffer cmd, uint32_t frame);
    // Writes the end timestamp of 'stage' (Stage enum value).
    void stamp(VkCommandBuffer cmd, uint32_t frame, uint32_t stage);
    // Reads the slot back; call after the slot's fence has been waited on.
    void collect(uint32_t frame);

    float stageMs(uint32_t stage) const { return stage < kStageCount ? m_ms[stage] : 0.0f; }
    float totalMs() const { return m_totalMs; }
    bool valid() const { return m_valid; }

    // Exponential moving average (for the title bar / HUD readout).
    float smoothedStageMs(uint32_t stage) const {
        return stage < kStageCount ? m_avg[stage] : 0.0f;
    }
    float smoothedTotalMs() const { return m_avgTotal; }

private:
    static constexpr uint32_t kTimestamps = kStageCount + 1;
    static constexpr uint32_t kMaxFrames = 4;

    VkDevice m_device = VK_NULL_HANDLE;
    VkQueryPool m_pool = VK_NULL_HANDLE;
    uint32_t m_frames = 2;
    float m_periodNs = 1.0f;
    bool m_supported = false;
    bool m_valid = false;
    uint64_t m_scratch[kTimestamps] = {};
    // How many timestamps of each frame slot were actually written. Reading a
    // query that was only reset (never written) makes vkGetQueryPoolResults
    // return VK_NOT_READY for the whole range, so the range is trimmed to what
    // the current frame really recorded.
    uint32_t m_written[kMaxFrames] = {};
    float m_ms[kStageCount] = {};
    float m_avg[kStageCount] = {};
    float m_totalMs = 0.0f;
    float m_avgTotal = 0.0f;
    uint32_t m_samples = 0;
};

} // namespace viz
