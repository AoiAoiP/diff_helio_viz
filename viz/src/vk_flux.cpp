#include "vk_flux.h"

#include "vk_gfx.h"

#include <cstdio>
#include <stdexcept>

using hviz::checkVk;

namespace viz {

namespace {

struct FluxPC {
    uint32_t spp;
    uint32_t tileCount;
    uint32_t totalPixels;
    uint32_t cols;
    uint32_t rows;
    uint32_t diagAtomics;
};

// compute -> compute barrier (same coarse-but-correct pattern the verified
// engine uses in VkCore::computeBarrier).
void computeBarrier(VkCommandBuffer cmd) { hviz::VkCore::computeBarrier(cmd); }

} // namespace

uint32_t FluxPipeline::tileCountFor(uint32_t spp) const { return (spp + 255u) / 256u; }

void FluxPipeline::init(hviz::VkCore &vk, hviz::ForwardEngine &engine, const std::string &shaderDir) {
    m_vk = &vk;
    m_engine = &engine;
    m_gridPoints = engine.gridPoints();
    m_maxSpp = m_gridPoints;   // 32x32 grid = 1024 rays per pixel at full quality
    m_activePixels = engine.activePixelCount();

    if (tileCountFor(m_maxSpp) > engine.tileCapacity()) {
        throw std::runtime_error("flux_lite: spp exceeds the reused fluxPartial capacity");
    }

    VkDevice dev = vk.device();
    const std::string dir = shaderDir + "/";

    // The lite kernels need push constants; the descriptor *set layout* stays the
    // engine's, so the verified upstream binding table is reused byte-for-byte.
    const VkDescriptorSetLayout sets[1] = {engine.setLayout()};
    m_layout = createPipelineLayout(dev, sets, sizeof(FluxPC), VK_SHADER_STAGE_COMPUTE_BIT);

    auto make = [&](const char *spv) {
        return vk.createComputePipelineOnLayout(hviz::VkCore::loadSpirv(dir + spv + ".spv"), "main", m_layout)
            .pipeline;
    };
    m_clear = make("clearFluxLite");
    m_flux = make("fluxLite");
    m_finalize = make("finalizeFluxLite");

    std::printf("[flux] lite chain ready: spp 16..%u (%u tiles), %u active pixels (%.2f M rays at %u spp)\n",
                m_maxSpp, engine.tileCapacity(), m_activePixels, raysPerFrame(m_maxSpp) / 1e6, m_maxSpp);
}

void FluxPipeline::record(VkCommandBuffer cmd, const Settings &s, GpuTimer *timer, uint32_t frameIndex) {
    const uint32_t spp = s.spp < 16u ? 16u : (s.spp > m_maxSpp ? m_maxSpp : s.spp);
    const uint32_t tiles = tileCountFor(spp);
    const uint32_t tilesX = (m_engine->config().pixelWidth + 15u) / 16u;
    const uint32_t tilesY = (m_engine->config().pixelHeight + 15u) / 16u;

    FluxPC pc{};
    pc.spp = spp;
    pc.tileCount = tiles;
    pc.totalPixels = m_engine->totalPixels();
    // Balanced 2D sub-grid: cols*rows == spp, both divide the 32x32 grid, and
    // (32,32) at full quality reproduces forward.slang's sample ordering exactly.
    {
        uint32_t k = 0;
        while ((1u << (k + 1)) <= spp) k++;          // spp = 2^k
        const uint32_t half = (k + 1) / 2;
        uint32_t cols = 1u << half;
        uint32_t rows = spp / cols;
        const uint32_t gs = m_gridPoints >= 4 ? m_engine->config().gridSize : 1;
        while (cols > gs) { cols >>= 1; rows <<= 1; }
        while (rows > gs) { rows >>= 1; cols <<= 1; }
        if (cols * rows != spp) { cols = gs; rows = gs; }   // non power of two: full grid
        pc.cols = cols;
        pc.rows = rows;
    }
    pc.diagAtomics = s.diagAtomics ? 1u : 0u;

    const VkDescriptorSet set = m_engine->descriptorSet();
    const uint32_t px = tilesX, py = tilesY;

    // ---- 1. clear flux + tile partials (reference path uses the fixed-tile one) ----
    if (s.useReference) {
        const hviz::Pipeline &p = m_engine->clearPipeline();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &set, 0, nullptr);
        vkCmdDispatch(cmd, px, py, 1);
    } else {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_clear);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, px, py, 1);
    }
    computeBarrier(cmd);

    // ---- 2. plate deformation (reused, verified upstream pipeline) ----
    if (s.deform) {
        struct DeformPC {
            uint32_t numBolts, disableGravity, sunBatchCount, gravityNormalCoupling;
        } dpc{m_engine->config().numBolts, s.gravityEnabled ? 0u : 1u, 1u, s.gravityNormalCoupling ? 1u : 0u};
        const hviz::Pipeline &p = m_engine->deformPipeline();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(dpc), &dpc);
        vkCmdDispatch(cmd, 1, 1, 1);
        computeBarrier(cmd);
    }
    if (timer) timer->stamp(cmd, frameIndex, kStageDeform);   // end of the deformation stage

    if (s.useReference) {
        // ---- parity mode: the verified forward.slang trace + finalize ----
        // clearFlux / finalizeFlux have kTileCount = 4 compiled in (grid 32x32) and
        // renderForward* traces all 1024 grid points; those pipelines take no push
        // constants. Everything else (UBOs, descriptor set, active pixel list) is
        // identical to the real-time path, so this is a true A/B.
        const uint32_t referenceTiles = tileCountFor(m_maxSpp);
        const hviz::Pipeline *chain[2] = {&m_engine->forwardPipeline(), &m_engine->finalizePipeline()};
        for (int i = 0; i < 2; i++) {
            const hviz::Pipeline &p = *chain[i];
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &set, 0, nullptr);
            vkCmdDispatch(cmd, i == 0 ? referenceTiles : px, i == 0 ? m_activePixels : py, 1);
            computeBarrier(cmd);
        }
        if (s.runS95) {
            const hviz::Pipeline &p = m_engine->s95Pipeline();
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &set, 0, nullptr);
            vkCmdDispatch(cmd, 1, 1, 1);
            computeBarrier(cmd);
        }
        if (timer) timer->stamp(cmd, frameIndex, kStageFlux);
        return;
    }

    // ---- 3. ray tracing: receiver pixel -> plate -> sun ----
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_flux);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, tiles, m_activePixels, 1);
    computeBarrier(cmd);

    // ---- 4. tile partials -> flux texture ----
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_finalize);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, px, py, 1);
    computeBarrier(cmd);

    // ---- 5. S95 (reused, verified upstream pipeline; no readback here) ----
    if (s.runS95) {
        const hviz::Pipeline &p = m_engine->s95Pipeline();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &set, 0, nullptr);
        vkCmdDispatch(cmd, 1, 1, 1);
        computeBarrier(cmd);
    }
    if (timer) timer->stamp(cmd, frameIndex, kStageFlux);   // end of the ray-tracing stage
}

void FluxPipeline::destroy() {
    VkDevice dev = m_vk->device();
    for (VkPipeline *p : {&m_clear, &m_flux, &m_finalize}) {
        if (*p) vkDestroyPipeline(dev, *p, nullptr);
        *p = VK_NULL_HANDLE;
    }
    if (m_layout) vkDestroyPipelineLayout(dev, m_layout, nullptr);
    m_layout = VK_NULL_HANDLE;
}

} // namespace viz
