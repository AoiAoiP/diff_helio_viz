#include "engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace hviz {

ForwardEngine::ForwardEngine(VkCore &vk, const Config &cfg) : m_vk(vk), m_cfg(cfg) {
    m_gridPts = cfg.gridSize * cfg.gridSize;
    m_totalPixels = cfg.pixelWidth * cfg.pixelHeight;
    m_totalSpp = m_gridPts;
    m_tileCount = (m_totalSpp + 255u) / 256u;   // compile-time constant in forward.slang
    m_pixelArea = computePixelArea(cfg);
}

ForwardEngine::~ForwardEngine() {
    if (!m_initialized) {
        // Buffers may still exist if init() threw half-way; destroy defensively.
    }
    m_vk.destroyPipeline(m_pipeClear);
    m_vk.destroyPipeline(m_pipeDeform);
    m_vk.destroyPipeline(m_pipeForward);
    m_vk.destroyPipeline(m_pipeFinalize);
    m_vk.destroyPipeline(m_pipeS95Level);
    m_vk.destroyTexture(m_flux);
    m_vk.destroyTexture(m_dummyImage);
    for (Buffer *b : {&m_uboReceiver, &m_uboHeliostat, &m_uboSun, &m_uboHelioPos, &m_uboAimPoint,
                      &m_sunBatchFlat, &m_boltHeights, &m_yGrid, &m_nGrid, &m_yuGrid, &m_yvGrid,
                      &m_influencePhi, &m_influencePhiU, &m_influencePhiV, &m_gravityMerged,
                      &m_fluxPartial, &m_activePixelList, &m_rayValidityScratch, &m_s95State, &m_dummyBuf}) {
        m_vk.destroyBuffer(*b);
    }
    if (m_setLayout) vkDestroyDescriptorSetLayout(m_vk.device(), m_setLayout, nullptr);
}

void ForwardEngine::createDescriptorLayout() {
    using namespace binding;
    auto ssbo = [](uint32_t b) {
        return VkDescriptorSetLayoutBinding{b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                            VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    };
    auto ubo = [](uint32_t b) {
        return VkDescriptorSetLayoutBinding{b, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                            VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    };
    auto img = [](uint32_t b) {
        return VkDescriptorSetLayoutBinding{b, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                            VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    };

    // The union of everything the reused upstream shaders can reference. Extra
    // bindings cost nothing and let us swap shader variants (A/B profiling)
    // without rebuilding the descriptor set.
    const std::vector<VkDescriptorSetLayoutBinding> bindings = {
        ubo(kReceiver), ubo(kHeliostat), ubo(kSun), ubo(kHelioPos), ubo(kAimPoint),
        ssbo(kDummy5), ssbo(kYGrid), ssbo(kNGrid),
        img(kFluxImage),
        ssbo(kDummy9), ssbo(kFluxPartial), ssbo(kDummy11),
        img(kDummyImage12),
        ssbo(kDummy13), ssbo(kDummy14), ssbo(kDummy15), ssbo(kDummy16),
        ssbo(kBoltHeights), ssbo(kDummy18),
        ssbo(kInfluencePhi), ssbo(kInfluencePhiU), ssbo(kInfluencePhiV),
        ssbo(kDummy22), ssbo(kYuGrid), ssbo(kYvGrid),
        ssbo(kDummy25), ssbo(kDummy26), ssbo(kDummy27), ssbo(kDummy29),
        ssbo(kGravityMerged), ssbo(kDummy31),
        ssbo(kSunBatchFlat), ssbo(kS95State), ssbo(kDummy53), ssbo(kActivePixelList),
    };
    m_setLayout = m_vk.createSetLayout(bindings);
    m_set = m_vk.allocateSet(m_setLayout);
}

void ForwardEngine::createBuffers() {
    const VkDeviceSize f4 = sizeof(float);
    const VkDeviceSize gridBytes = m_gridPts * f4;

    // ---- Uniform buffers: exact upstream packing (see PLAN.md §7.1) ----
    m_uboReceiver = m_vk.createBuffer(10 * f4, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
    m_uboHeliostat = m_vk.createBuffer(11 * f4, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
    m_uboSun = m_vk.createBuffer(13 * f4, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
    m_uboHelioPos = m_vk.createBuffer(3 * f4, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
    m_uboAimPoint = m_vk.createBuffer(3 * f4, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
    m_sunBatchFlat = m_vk.createBuffer(6 * 8 * f4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);

    // ---- Plate model ----
    // Single sun at a time: the multi-sun batching dimension of the upstream
    // buffers (kSunBatchSize = 6) is not needed by the viewer.
    //
    // TRANSFER_SRC/DST are included because these buffers are uploaded through
    // the staging path and read back by readSurfaceY()/readSurfaceNormalsLocal()
    // (VUID-vkCmdCopyBuffer). Usage flags do not affect any computed value.
    const VkBufferUsageFlags kModelBufUsage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    m_yGrid = m_vk.createBuffer(gridBytes, kModelBufUsage, false);
    m_nGrid = m_vk.createBuffer(gridBytes * 4, kModelBufUsage, false);
    m_yuGrid = m_vk.createBuffer(gridBytes, kModelBufUsage, false);
    m_yvGrid = m_vk.createBuffer(gridBytes, kModelBufUsage, false);
    m_boltHeights = m_vk.createBuffer(m_cfg.numBolts * f4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);

    // ---- TPS proxy + gravity, loaded once ----
    const auto phi = loadInfluencePlane(m_cfg.proxyPath, "influence_phi.bin", m_cfg.numBolts, m_cfg.gridSize);
    const auto phiU = loadInfluencePlane(m_cfg.proxyPath, "influence_phi_u.bin", m_cfg.numBolts, m_cfg.gridSize);
    const auto phiV = loadInfluencePlane(m_cfg.proxyPath, "influence_phi_v.bin", m_cfg.numBolts, m_cfg.gridSize);
    const auto gravity = loadGravityMerged(m_cfg.proxyPath, m_cfg.gridSize);

    auto deviceBuf = [&](const std::vector<float> &data) {
        // TRANSFER_DST is required by the staging upload in VkCore::uploadBuffer
        // (the validation layer checks it; the driver happened not to care).
        Buffer b = m_vk.createBuffer(data.size() * f4,
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
        m_vk.uploadBuffer(b, data.data(), data.size() * f4);
        return b;
    };
    m_influencePhi = deviceBuf(phi);
    m_influencePhiU = deviceBuf(phiU);
    m_influencePhiV = deviceBuf(phiV);
    m_gravityMerged = deviceBuf(gravity);
    std::printf("[engine] proxy data: %u bolts x %u grid points, %zu gravity floats\n", m_cfg.numBolts,
                m_gridPts, gravity.size());

    // ---- Render targets / work buffers ----
    m_fluxPartial = m_vk.createBuffer(m_totalPixels * m_tileCount * f4, kModelBufUsage, false);
    m_activePixelList = m_vk.createBuffer(m_totalPixels * sizeof(uint32_t), kModelBufUsage, false);
    // Binding 29 is the upstream per-ray validity bitmap (only the backward pass
    // consumes it, but forward.slang sets the bit for every valid ray):
    //   index = (sp * totalPixels + pixelIdx) >> 5  ->  up to 250k uints here.
    // Binding it to the 4 KB dummy buffer — as the CLI-only path did — is an
    // out-of-bounds device write (~1 MB) that happens to survive on some
    // allocation layouts and takes the device down on others.
    const VkDeviceSize rayValidityBytes =
        ((static_cast<VkDeviceSize>(m_totalSpp) * m_totalPixels + 31u) / 32u) * sizeof(uint32_t);
    m_rayValidityScratch = m_vk.createBuffer(rayValidityBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
    m_s95State = m_vk.createBuffer(4 * f4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    // Host-visible so the viewer can read the upstream diagnostic counters
    // (diagBuf, binding 31) back without a staging copy — the A/B switch that
    // toggles the per-ray atomics is verified against these counters.
    m_dummyBuf = m_vk.createBuffer(4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);

    m_flux = m_vk.createTexture(m_cfg.pixelWidth, m_cfg.pixelHeight, VK_FORMAT_R32_SFLOAT,
                                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    m_dummyImage = m_vk.createTexture(4, 4, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT);

    // Zero the bolt heights; the caller uploads real values via setBolts().
    std::vector<float> zeros(m_cfg.numBolts, 0.0f);
    std::memcpy(m_boltHeights.mapped, zeros.data(), zeros.size() * f4);
}

void ForwardEngine::updateDescriptorSet() {
    using namespace binding;
    VkDescriptorBufferInfo uboInfos[] = {
        {m_uboReceiver.buffer, 0, m_uboReceiver.size},
        {m_uboHeliostat.buffer, 0, m_uboHeliostat.size},
        {m_uboSun.buffer, 0, m_uboSun.size},
        {m_uboHelioPos.buffer, 0, m_uboHelioPos.size},
        {m_uboAimPoint.buffer, 0, m_uboAimPoint.size},
    };

    auto dummyInfo = [&]() { return VkDescriptorBufferInfo{m_dummyBuf.buffer, 0, m_dummyBuf.size}; };
    VkDescriptorImageInfo fluxInfo{VK_NULL_HANDLE, m_flux.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo dummyImgInfo{VK_NULL_HANDLE, m_dummyImage.view, VK_IMAGE_LAYOUT_GENERAL};

    struct Entry {
        uint32_t binding;
        VkDescriptorType type;
        VkDescriptorBufferInfo *buf;
        VkDescriptorImageInfo *img;
    };
    VkDescriptorBufferInfo bDummy5 = dummyInfo(), bDummy9 = dummyInfo(), bDummy11 = dummyInfo();
    VkDescriptorBufferInfo bDummy13 = dummyInfo(), bDummy14 = dummyInfo(), bDummy15 = dummyInfo();
    VkDescriptorBufferInfo bDummy16 = dummyInfo(), bDummy18 = dummyInfo(), bDummy22 = dummyInfo();
    VkDescriptorBufferInfo bDummy25 = dummyInfo(), bDummy26 = dummyInfo(), bDummy27 = dummyInfo();
    VkDescriptorBufferInfo bDummy31 = dummyInfo(), bDummy53 = dummyInfo();
    // ...except binding 29, which forward.slang genuinely writes into.
    VkDescriptorBufferInfo bRayValidity{m_rayValidityScratch.buffer, 0, m_rayValidityScratch.size};
    VkDescriptorBufferInfo bYGrid{m_yGrid.buffer, 0, m_yGrid.size};
    VkDescriptorBufferInfo bNGrid{m_nGrid.buffer, 0, m_nGrid.size};
    VkDescriptorBufferInfo bYu{m_yuGrid.buffer, 0, m_yuGrid.size};
    VkDescriptorBufferInfo bYv{m_yvGrid.buffer, 0, m_yvGrid.size};
    VkDescriptorBufferInfo bPhi{m_influencePhi.buffer, 0, m_influencePhi.size};
    VkDescriptorBufferInfo bPhiU{m_influencePhiU.buffer, 0, m_influencePhiU.size};
    VkDescriptorBufferInfo bPhiV{m_influencePhiV.buffer, 0, m_influencePhiV.size};
    VkDescriptorBufferInfo bGrav{m_gravityMerged.buffer, 0, m_gravityMerged.size};
    VkDescriptorBufferInfo bPartial{m_fluxPartial.buffer, 0, m_fluxPartial.size};
    VkDescriptorBufferInfo bActive{m_activePixelList.buffer, 0, m_activePixelList.size};
    VkDescriptorBufferInfo bS95{m_s95State.buffer, 0, m_s95State.size};
    VkDescriptorBufferInfo bBolt{m_boltHeights.buffer, 0, m_boltHeights.size};
    VkDescriptorBufferInfo bBatch{m_sunBatchFlat.buffer, 0, m_sunBatchFlat.size};

    const std::vector<Entry> entries = {
        {kReceiver, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &uboInfos[0], nullptr},
        {kHeliostat, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &uboInfos[1], nullptr},
        {kSun, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &uboInfos[2], nullptr},
        {kHelioPos, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &uboInfos[3], nullptr},
        {kAimPoint, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &uboInfos[4], nullptr},
        {kDummy5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bDummy5, nullptr},
        {kYGrid, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bYGrid, nullptr},
        {kNGrid, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bNGrid, nullptr},
        {kFluxImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, nullptr, &fluxInfo},
        {kDummy9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bDummy9, nullptr},
        {kFluxPartial, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bPartial, nullptr},
        {kDummy11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bDummy11, nullptr},
        {kDummyImage12, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, nullptr, &dummyImgInfo},
        {kDummy13, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bDummy13, nullptr},
        {kDummy14, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bDummy14, nullptr},
        {kDummy15, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bDummy15, nullptr},
        {kDummy16, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bDummy16, nullptr},
        {kBoltHeights, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bBolt, nullptr},
        {kDummy18, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bDummy18, nullptr},
        {kInfluencePhi, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bPhi, nullptr},
        {kInfluencePhiU, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bPhiU, nullptr},
        {kInfluencePhiV, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bPhiV, nullptr},
        {kDummy22, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bDummy22, nullptr},
        {kYuGrid, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bYu, nullptr},
        {kYvGrid, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bYv, nullptr},
        {kDummy25, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bDummy25, nullptr},
        {kDummy26, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bDummy26, nullptr},
        {kDummy27, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bDummy27, nullptr},
        {kDummy29, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bRayValidity, nullptr},
        {kGravityMerged, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bGrav, nullptr},
        {kDummy31, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bDummy31, nullptr},
        {kSunBatchFlat, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bBatch, nullptr},
        {kS95State, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bS95, nullptr},
        {kDummy53, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bDummy53, nullptr},
        {kActivePixelList, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bActive, nullptr},
    };

    std::vector<VkWriteDescriptorSet> writes(entries.size());
    for (size_t i = 0; i < entries.size(); i++) {
        VkWriteDescriptorSet &w = writes[i];
        w = {};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = m_set;
        w.dstBinding = entries[i].binding;
        w.descriptorCount = 1;
        w.descriptorType = entries[i].type;
        w.pBufferInfo = entries[i].buf;
        w.pImageInfo = entries[i].img;
    }
    vkUpdateDescriptorSets(m_vk.device(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void ForwardEngine::createPipelines(const std::string &shaderDir) {
    auto path = [&](const char *name) { return shaderDir + "/" + name + ".spv"; };

    // NOTE: slangc names every SPIR-V entry point "main" by default, regardless
    // of the Slang function name (that is why the upstream code passes "main"
    // everywhere). Pass -fvk-use-entrypoint-name to slangc if you want the
    // original names instead.
    m_pipeClear = m_vk.createComputePipeline(VkCore::loadSpirv(path("clearFlux")), "main", 0, m_setLayout);
    m_pipeDeform = m_vk.createComputePipeline(VkCore::loadSpirv(path("computeBoltSurface")), "main",
                                              sizeof(uint32_t) * 4, m_setLayout);
    m_pipeFinalize = m_vk.createComputePipeline(VkCore::loadSpirv(path("finalizeFlux")), "main", 0, m_setLayout);
    m_pipeS95Level = m_vk.createComputePipeline(VkCore::loadSpirv(path("computeS95FindLevel")), "main", 0,
                                                m_setLayout);

    // P1-A2 compile-time sun-shape specialization (Buie / Pillbox / Gaussian).
    const char *spv = "renderForwardBuie";
    if (m_cfg.sunType == SunShapeType::PILLBOX) spv = "renderForwardPillbox";
    else if (m_cfg.sunType == SunShapeType::GAUSSIAN) spv = "renderForwardGaussian";
    m_pipeForward = m_vk.createComputePipeline(VkCore::loadSpirv(path(spv)), "main", 0, m_setLayout);
    std::printf("[engine] forward pipeline: %s.spv\n", spv);
}

// Active receiver pixels: only the half of the cylinder facing the heliostat can
// receive light. Same test as upstream pipeline.cpp buildActivePixelList().
void ForwardEngine::buildActivePixelList(const std::array<float, 3> &heliostatPos) {
    const uint32_t w = m_cfg.pixelWidth, h = m_cfg.pixelHeight;
    const float hdx = heliostatPos[0] - m_cfg.receiverPos[0];
    const float hdz = heliostatPos[2] - m_cfg.receiverPos[2];
    const float len = std::sqrt(hdx * hdx + hdz * hdz);
    const float cullX = len > 1e-6f ? hdx / len : 0.0f;
    const float cullZ = len > 1e-6f ? hdz / len : 0.0f;

    std::vector<uint32_t> list;
    list.reserve(m_totalPixels);
    for (uint32_t py = 0; py < h; py++) {
        for (uint32_t px = 0; px < w; px++) {
            const float ang = static_cast<float>(px) * (2.0f * 3.14159265f / static_cast<float>(w));
            const float nx = std::sin(ang), nz = -std::cos(ang);
            if (nx * cullX + nz * cullZ > -1e-4f) list.push_back(py * w + px);
        }
    }
    m_activePixelCount = std::max(1u, static_cast<uint32_t>(list.size()));
    list.resize(m_totalPixels, 0xFFFFFFFFu);  // sentinels: shader early-outs
    m_vk.uploadBuffer(m_activePixelList, list.data(), list.size() * sizeof(uint32_t));
    std::printf("[engine] sparse culling: %u / %u receiver pixels active (%.0f%%)\n", m_activePixelCount,
                m_totalPixels, 100.0f * static_cast<float>(m_activePixelCount) / static_cast<float>(m_totalPixels));
}

void ForwardEngine::init(const std::string &shaderDir, const HeliostatConfig &heliostat) {
    createDescriptorLayout();
    createBuffers();
    updateDescriptorSet();
    createPipelines(shaderDir);
    buildActivePixelList(heliostat.position);
    m_initialized = true;
}

void ForwardEngine::setBolts(const std::vector<float> &heights) {
    if (heights.size() != m_cfg.numBolts) {
        throw std::runtime_error("setBolts: expected " + std::to_string(m_cfg.numBolts) + " values, got " +
                                 std::to_string(heights.size()));
    }
    std::memcpy(m_boltHeights.mapped, heights.data(), heights.size() * sizeof(float));
}

void ForwardEngine::setSun(const std::array<float, 3> &sunDir, const HeliostatConfig &heliostat) {
    m_lastSunDir = sunDir;
    m_lastHeliostat = heliostat;
    const auto &hp = heliostat.position;

    // ---- Binding 0: ReceiverParams (40 B) ----
    float recv[10] = {};
    recv[0] = m_cfg.receiverPos[0];
    recv[1] = m_cfg.receiverPos[1];
    recv[2] = m_cfg.receiverPos[2];
    recv[3] = m_cfg.receiverRadius;
    recv[4] = m_cfg.receiverHeight;
    const uint32_t dims[2] = {m_cfg.pixelWidth, m_cfg.pixelHeight};
    std::memcpy(&recv[6], dims, sizeof(dims));
    recv[8] = m_cfg.receiverHeight / static_cast<float>(m_cfg.pixelHeight);
    recv[9] = 2.0f * 3.14159265f * m_cfg.receiverRadius / static_cast<float>(m_cfg.pixelWidth);
    std::memcpy(m_uboReceiver.mapped, recv, sizeof(recv));

    // ---- Binding 1: HeliostatParams (44 B) ----
    float helio[11] = {};
    helio[0] = m_cfg.plateWidth;
    helio[1] = m_cfg.plateLength;
    helio[2] = m_cfg.glassDepth;
    helio[3] = m_cfg.refractiveIndex;
    helio[4] = m_cfg.slopeError;
    helio[5] = m_cfg.plateWidth * m_cfg.plateLength;
    helio[6] = m_cfg.reflectivity;
    const float hdx = hp[0] - m_cfg.receiverPos[0];
    const float hdz = hp[2] - m_cfg.receiverPos[2];
    const float hlen = std::sqrt(hdx * hdx + hdz * hdz);
    helio[9] = hlen > 1e-6f ? hdx / hlen : 0.0f;
    helio[10] = hlen > 1e-6f ? hdz / hlen : 0.0f;
    std::memcpy(m_uboHeliostat.mapped, helio, sizeof(helio));

    // ---- Binding 2: SunParams (52 B) ----
    float sunp[13] = {};
    sunp[0] = sunDir[0];
    sunp[1] = sunDir[1];
    sunp[2] = sunDir[2];
    sunp[3] = m_cfg.dni;
    sunp[4] = m_cfg.buieThetaInner;
    sunp[5] = m_cfg.buieKappa;
    sunp[6] = m_cfg.buieGamma;
    sunp[7] = 0.0f;
    sunp[8] = m_cfg.sunShapeIntegral;
    sunp[9] = static_cast<float>(static_cast<uint32_t>(m_cfg.sunType));
    sunp[10] = 0.0f;  // iteration seed (per-iteration randomization is optimizer-only)
    float support = 0.0436f;
    if (m_cfg.sunType == SunShapeType::PILLBOX) support = m_cfg.thetaMax;
    else if (m_cfg.sunType == SunShapeType::GAUSSIAN) support = 5.0f * m_cfg.sigma;
    sunp[11] = m_cfg.rayCull ? std::cos(support + m_cfg.rayCullMarginMrad * 1e-3f) : -2.0f;
    sunp[12] = 0.0f;
    std::memcpy(m_uboSun.mapped, sunp, sizeof(sunp));

    // ---- Bindings 3 / 4, gravity bins and the macro normal ----
    std::memcpy(m_uboHelioPos.mapped, hp.data(), 3 * sizeof(float));
    updateAim();
}

// Everything that depends on the aim point: the aim UBO (binding 4), the gravity
// bin interpolation for computeBoltSurface (binding 51) and the cached macro
// normal the viewer draws the plate with. Called by setSun() and by the viewer's
// beam-target slider.
void ForwardEngine::updateAim() {
    const auto &hp = m_lastHeliostat.position;
    const auto &sunDir = m_lastSunDir;

    std::array<float, 3> ap = computeAimPoint(hp, m_cfg.receiverPos, m_cfg.receiverRadius);
    if (m_aimOffsetDeg != 0.0f) {
        // Rotate the aim point around the receiver axis, using the same angle
        // convention as the receiver pixel parameterisation in common.slang:
        // P = (cx + R sin(theta), y, cz - R cos(theta)), so a positive offset moves
        // the spot towards increasing pixel column index by deg/360*pixelWidth.
        const float r = m_cfg.receiverRadius;
        const float theta0 = std::atan2(ap[0] - m_cfg.receiverPos[0], -(ap[2] - m_cfg.receiverPos[2]));
        const float theta = theta0 + m_aimOffsetDeg * 3.14159265f / 180.0f;
        ap[0] = m_cfg.receiverPos[0] + r * std::sin(theta);
        ap[2] = m_cfg.receiverPos[2] - r * std::cos(theta);
    }
    std::memcpy(m_uboAimPoint.mapped, ap.data(), 3 * sizeof(float));
    m_aimPoint = ap;

    // ---- Gravity bin selection for computeBoltSurface (binding 51) ----
    uint32_t lo = 0, hi = 0;
    float t = 0.0f;
    packGravityParams(computeCosTheta(sunDir, hp, ap), lo, hi, t);
    float batch[8] = {};
    batch[0] = sunDir[0];
    batch[1] = sunDir[1];
    batch[2] = sunDir[2];
    std::memcpy(&batch[4], &lo, sizeof(uint32_t));
    std::memcpy(&batch[5], &hi, sizeof(uint32_t));
    batch[6] = t;
    std::memcpy(m_sunBatchFlat.mapped, batch, sizeof(batch));

    // Macro plate normal: bisector of sun direction and heliostat->aim direction.
    float n[3];
    {
        const float sl = std::sqrt(sunDir[0] * sunDir[0] + sunDir[1] * sunDir[1] + sunDir[2] * sunDir[2]);
        const float rx = ap[0] - hp[0], ry = ap[1] - hp[1], rz = ap[2] - hp[2];
        const float rl = std::sqrt(rx * rx + ry * ry + rz * rz);
        n[0] = sunDir[0] / sl + rx / rl;
        n[1] = sunDir[1] / sl + ry / rl;
        n[2] = sunDir[2] / sl + rz / rl;
        const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        m_macroNormal = {n[0] / nl, n[1] / nl, n[2] / nl};
    }
}

void ForwardEngine::setAimOffsetDeg(float degrees) {
    if (degrees == m_aimOffsetDeg) return;
    m_aimOffsetDeg = degrees;
    updateAim();
}

// Viewer bridge: rebuild the sparse active-pixel list for a different heliostat of
// the field. Values for the original heliostat are unchanged, so the CLI path and
// parity are unaffected.
void ForwardEngine::setActiveHeliostat(const HeliostatConfig &heliostat) {
    if (m_activeHelioValid && heliostat.position == m_activeHelioPos) return;
    m_activeHelioPos = heliostat.position;
    m_activeHelioValid = true;
    buildActivePixelList(heliostat.position);
}

void ForwardEngine::render(float *outGpuMs) {
    const uint32_t px = (m_cfg.pixelWidth + 15u) / 16u;
    const uint32_t py = (m_cfg.pixelHeight + 15u) / 16u;

    struct DeformPC {
        uint32_t numBolts;
        uint32_t disableGravity;
        uint32_t sunBatchCount;
        uint32_t gravityNormalCoupling;
    } pc{m_cfg.numBolts, m_cfg.disableGravity ? 1u : 0u, 1u, m_cfg.gravityNormalCoupling ? 1u : 0u};

    m_vk.submitOneShot(
        [&](VkCommandBuffer cmd) {
            // 1. clear flux + per-tile partial sums
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeClear.pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeClear.layout, 0, 1, &m_set, 0,
                                    nullptr);
            vkCmdDispatch(cmd, px, py, 1);
            VkCore::computeBarrier(cmd);

            // 2. plate deformation: TPS influence superposition + gravity sag
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeDeform.pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeDeform.layout, 0, 1, &m_set, 0,
                                    nullptr);
            vkCmdPushConstants(cmd, m_pipeDeform.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            vkCmdDispatch(cmd, 1, 1, 1);
            VkCore::computeBarrier(cmd);

            // 3. receiver -> plate -> sun ray tracing (the expensive pass)
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeForward.pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeForward.layout, 0, 1, &m_set, 0,
                                    nullptr);
            vkCmdDispatch(cmd, m_tileCount, m_activePixelCount, 1);
            VkCore::computeBarrier(cmd);

            // 4. tile partials -> flux texture
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeFinalize.pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeFinalize.layout, 0, 1, &m_set, 0,
                                    nullptr);
            vkCmdDispatch(cmd, px, py, 1);
            VkCore::computeBarrier(cmd);

            // 5. S95 threshold (single workgroup cooperative bisection, no readback)
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeS95Level.pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeS95Level.layout, 0, 1, &m_set, 0,
                                    nullptr);
            vkCmdDispatch(cmd, 1, 1, 1);
        },
        outGpuMs);
}

std::vector<float> ForwardEngine::readFlux() const {
    std::vector<float> flux(m_totalPixels);
    m_vk.downloadTexture(m_flux, flux.data());
    return flux;
}

std::array<float, 4> ForwardEngine::readS95State() const {
    std::array<float, 4> state{};
    m_vk.downloadBuffer(m_s95State, state.data(), state.size() * sizeof(float));
    return state;
}

FluxStats ForwardEngine::computeStats(const std::vector<float> &flux) const {
    FluxStats s;
    for (float f : flux) {
        s.sum += f;
        if (f > s.max) s.max = f;
    }
    const auto state = readS95State();
    s.s95LevelGpu = state[0];
    s.s95LevelCpu = computeS95LevelCPU(flux);
    s.s95AreaGpu = computeS95Area(flux, s.s95LevelGpu, m_pixelArea);
    s.s95AreaCpu = computeS95Area(flux, s.s95LevelCpu, m_pixelArea);
    // Energy figure reported by the GPU pass (s95State[2]) — useful as an
    // energy-retention guard when the viewer lets the user drag bolt sliders.
    s.energyRetention = s.sum > 1e-6f ? state[2] / s.sum : 0.0f;
    return s;
}

std::vector<float> ForwardEngine::readSurfaceY() const {
    std::vector<float> y(m_gridPts);
    m_vk.downloadBuffer(m_yGrid, y.data(), y.size() * sizeof(float));
    return y;
}

std::vector<float> ForwardEngine::readSurfaceNormalsLocal() const {
    std::vector<float> n(m_gridPts * 4);
    m_vk.downloadBuffer(m_nGrid, n.data(), n.size() * sizeof(float));
    return n;
}

// Viewer bridge: patch SunParams.cullCosCutoff (float 11 of the 52-byte UBO)
// without touching anything else. Same expression as setSun().
void ForwardEngine::setRayCull(bool enabled, float marginMrad) {
    float support = 0.0436f;
    if (m_cfg.sunType == SunShapeType::PILLBOX) support = m_cfg.thetaMax;
    else if (m_cfg.sunType == SunShapeType::GAUSSIAN) support = 5.0f * m_cfg.sigma;
    const float cullCos = enabled ? std::cos(support + marginMrad * 1e-3f) : -2.0f;
    std::memcpy(static_cast<float *>(m_uboSun.mapped) + 11, &cullCos, sizeof(float));
}

} // namespace hviz
