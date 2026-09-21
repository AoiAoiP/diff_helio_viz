#include "gpu_timer.h"

#include <cstdio>

namespace viz {

const char *stageName(uint32_t stage) {
    switch (stage) {
    case kStageDeform: return "deform";
    case kStageFlux: return "flux";
    case kStageScene: return "scene";
    case kStageBloomPre: return "bloom.pre";
    case kStageBloomDown: return "bloom.down";
    case kStageBloomUp: return "bloom.up";
    case kStageComposite: return "composite";
    case kStageHud: return "hud";
    default: return "?";
    }
}

void GpuTimer::init(VkDevice device, float timestampPeriodNs, uint32_t framesInFlight, bool supported) {
    m_device = device;
    m_frames = framesInFlight;
    m_periodNs = timestampPeriodNs > 0.0f ? timestampPeriodNs : 1.0f;
    m_supported = supported;

    VkQueryPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = m_frames * kTimestamps;
    if (vkCreateQueryPool(m_device, &info, nullptr, &m_pool) != VK_SUCCESS) {
        std::fprintf(stderr, "[timer] timestamp query pool unavailable; GPU ms disabled\n");
        m_pool = VK_NULL_HANDLE;
        m_supported = false;
    }
}

void GpuTimer::destroy() {
    if (m_pool) vkDestroyQueryPool(m_device, m_pool, nullptr);
    m_pool = VK_NULL_HANDLE;
}

void GpuTimer::beginFrame(VkCommandBuffer cmd, uint32_t frame) {
    if (!m_pool || !m_supported) return;
    const uint32_t base = frame * kTimestamps;
    vkCmdResetQueryPool(cmd, m_pool, base, kTimestamps);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_pool, base);
    m_written[frame % kMaxFrames] = 1;
}

void GpuTimer::stamp(VkCommandBuffer cmd, uint32_t frame, uint32_t stage) {
    if (!m_pool || !m_supported || stage >= kStageCount) return;
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_pool, frame * kTimestamps + stage + 1);
    uint32_t &w = m_written[frame % kMaxFrames];
    if (stage + 2 > w) w = stage + 2;
}

void GpuTimer::collect(uint32_t frame) {
    if (!m_pool || !m_supported) return;
    const uint32_t slot = frame % kMaxFrames;
    const uint32_t count = m_written[slot];
    if (count < 2) return;   // nothing measurable recorded for this slot yet
    const uint32_t base = frame * kTimestamps;
    const VkResult r = vkGetQueryPoolResults(m_device, m_pool, base, count, count * sizeof(uint64_t), m_scratch,
                                             sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    if (r != VK_SUCCESS) return;   // not ready yet: keep the previous numbers

    const double toMs = static_cast<double>(m_periodNs) * 1e-6;
    for (uint32_t i = 0; i + 1 < count; i++) {
        const uint64_t a = m_scratch[i], b = m_scratch[i + 1];
        m_ms[i] = b >= a ? static_cast<float>(static_cast<double>(b - a) * toMs) : 0.0f;
    }
    for (uint32_t i = count - 1; i < kStageCount; i++) m_ms[i] = 0.0f;   // stages not recorded
    m_totalMs = m_scratch[count - 1] >= m_scratch[0]
                    ? static_cast<float>(static_cast<double>(m_scratch[count - 1] - m_scratch[0]) * toMs)
                    : 0.0f;
    m_valid = true;

    const float a = 0.1f;
    if (m_samples == 0) {
        for (uint32_t i = 0; i < kStageCount; i++) m_avg[i] = m_ms[i];
        m_avgTotal = m_totalMs;
    } else {
        for (uint32_t i = 0; i < kStageCount; i++) m_avg[i] += (m_ms[i] - m_avg[i]) * a;
        m_avgTotal += (m_totalMs - m_avgTotal) * a;
    }
    if (m_samples < 1000) m_samples++;
}

} // namespace viz
