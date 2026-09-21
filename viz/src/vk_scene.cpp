#include "vk_scene.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

using hviz::checkVk;

namespace viz {

namespace {
// 20 float4 base + viewport + uiParams + 4x4 heliostat records + stats + fluxCenter
// = 40 float4 (160 floats = 640 bytes, which still fits the 768-byte slice below).
constexpr uint32_t kUboFloats = 40 * 4;
// Actuator marker radius in metres (the octahedron in vsScene mode 4).
constexpr float kBoltMarkerRadiusM = 0.22f;
// The slice stride must be a multiple of minUniformBufferOffsetAlignment (64 on
// most devices, 256 on some) because each frame slot binds its own slice.
constexpr uint32_t kUboSliceBytes = 768;

// Must match struct ScenePC in viz/shaders/scene.slang.
struct ScenePC {
    uint32_t mode;
    uint32_t res;
    uint32_t extra;
    float scale;
};
} // namespace

void SceneRenderer::init(hviz::VkCore &vk, hviz::ForwardEngine &engine, VkExtent2D extent,
                         const std::string &shaderDir, VkFormat swapchainFormat) {
    m_vk = &vk;
    m_engine = &engine;
    m_swapFormat = swapchainFormat;
    m_gridSize = engine.config().gridSize;

    createTargets(extent);
    createPipelines(shaderDir);
    createDescriptors();
}

void SceneRenderer::createTargets(VkExtent2D extent) {
    m_extent = extent;
    for (auto &hdr : m_hdr) {
        hdr = createImage2D(*m_vk, extent.width, extent.height, VK_FORMAT_R16G16B16A16_SFLOAT,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    }
    m_depth = createImage2D(*m_vk, extent.width, extent.height, VK_FORMAT_D32_SFLOAT,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
}

void SceneRenderer::destroyTargets() {
    for (auto &hdr : m_hdr) m_vk->destroyTexture(hdr);
    m_vk->destroyTexture(m_depth);
}

void SceneRenderer::resize(VkExtent2D extent) {
    if (extent.width == m_extent.width && extent.height == m_extent.height) return;
    vkDeviceWaitIdle(m_vk->device());
    destroyTargets();
    createTargets(extent);
    // The descriptor *sets* are reused; only the HDR view they point at changed.
    if (m_sets[0] != VK_NULL_HANDLE) writeDescriptors();
}
uint32_t SceneRenderer::plateTriangles() const {
    const uint32_t cells = m_gridSize > 1 ? m_gridSize - 1 : 1;
    return cells * cells * 2;
}

uint32_t SceneRenderer::receiverTriangles() const {
    const hviz::Config &cfg = m_engine->config();
    return cfg.pixelWidth * cfg.pixelHeight * 2;
}

void SceneRenderer::createPipelines(const std::string &shaderDir) {
    VkDevice dev = m_vk->device();
    const std::string dir = shaderDir + "/";

    VkDescriptorSetLayoutBinding bindings[5]{};
    auto add = [&](uint32_t b, VkDescriptorType type, VkShaderStageFlags stages) {
        bindings[b].binding = b;
        bindings[b].descriptorType = type;
        bindings[b].descriptorCount = 1;
        bindings[b].stageFlags = stages;
    };
    const VkShaderStageFlags vf = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    add(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, vf);
    add(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
    add(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vf);
    add(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vf);
    add(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vf);

    VkDescriptorSetLayoutCreateInfo lci{};
    lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.bindingCount = 5;
    lci.pBindings = bindings;
    checkVk(vkCreateDescriptorSetLayout(dev, &lci, nullptr, &m_setLayout), "vkCreateDescriptorSetLayout(scene)");

    const VkDescriptorSetLayout setLayouts[1] = {m_setLayout};
    // The object mode + its parameters travel in push constants, so every object
    // is a plain vkCmdDraw with no vertex buffer bound.
    m_pipeLayout = createPipelineLayout(dev, setLayouts, sizeof(ScenePC), vf);

    VkShaderModule vsSky = loadShaderModule(dev, dir + "vsSky.spv");
    VkShaderModule fsSky = loadShaderModule(dev, dir + "fsSky.spv");
    VkShaderModule vsScene = loadShaderModule(dev, dir + "vsScene.spv");
    VkShaderModule fsScene = loadShaderModule(dev, dir + "fsScene.spv");

    GraphicsPipelineDesc sky{};
    sky.layout = m_pipeLayout;
    sky.vs = vsSky;
    sky.fs = fsSky;
    sky.depthTest = false;         // writes the far value (0) unconditionally
    sky.depthWrite = true;
    sky.depthAttachment = true;    // the scene pass always renders with depth
    sky.colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    sky.depthFormat = VK_FORMAT_D32_SFLOAT;
    m_pipeSky = createGraphicsPipeline(dev, sky);

    GraphicsPipelineDesc opaque{};
    opaque.layout = m_pipeLayout;
    opaque.vs = vsScene;
    opaque.fs = fsScene;
    opaque.depthTest = true;
    opaque.depthWrite = true;
    opaque.depthCompare = VK_COMPARE_OP_GREATER;   // reversed-Z
    opaque.depthAttachment = true;
    opaque.colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    opaque.depthFormat = VK_FORMAT_D32_SFLOAT;
    m_pipeScene = createGraphicsPipeline(dev, opaque);

    // Beams: additive, depth-tested but not depth-writing, so they layer over the
    // scene without occluding each other.
    GraphicsPipelineDesc beam = opaque;
    beam.blend = true;
    beam.additiveBlend = true;
    beam.depthWrite = false;
    m_pipeBeam = createGraphicsPipeline(dev, beam);

    // Flux-map inset: screen space, no depth attachment, drawn onto the swapchain
    // image after the composite (so bloom/ACES/vignette do not touch it).
    VkShaderModule vsMap = loadShaderModule(dev, dir + "vsFluxMap.spv");
    VkShaderModule fsMap = loadShaderModule(dev, dir + "fsFluxMap.spv");
    GraphicsPipelineDesc map{};
    map.layout = m_pipeLayout;
    map.vs = vsMap;
    map.fs = fsMap;
    map.colorFormat = m_swapFormat;
    map.depthAttachment = false;      // drawn without any depth buffer
    map.blend = true;                 // mild alpha so it reads as an overlay
    m_pipeFluxMap = createGraphicsPipeline(dev, map);
    destroyShaderModules(dev, {vsMap, fsMap});

    destroyShaderModules(dev, {vsSky, fsSky, vsScene, fsScene});

    // R32F linear filtering is optional on some devices; ask before using it.
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(m_vk->physicalDevice(), VK_FORMAT_R32_SFLOAT, &props);
    const bool linearOk = (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    m_fluxSampler = createSampler(dev, linearOk ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                  VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
}

void SceneRenderer::createDescriptors() {
    VkDevice dev = m_vk->device();
    const uint32_t uboBytes = kUboSliceBytes;
    // 2 frames in flight -> 2 UBO slices in one persistently mapped buffer.
    m_ubo = m_vk->createBuffer(uboBytes * 2, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
    std::memset(m_ubo.mapped, 0, static_cast<size_t>(m_ubo.size));

    const VkDescriptorSetLayout layouts[2] = {m_setLayout, m_setLayout};
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = m_vk->descriptorPool();
    ai.descriptorSetCount = 2;
    ai.pSetLayouts = layouts;
    checkVk(vkAllocateDescriptorSets(dev, &ai, m_sets), "vkAllocateDescriptorSets(scene)");

    writeDescriptors();
}

void SceneRenderer::writeDescriptors() {
    VkDevice dev = m_vk->device();
    const uint32_t uboBytes = kUboSliceBytes;
    const hviz::Texture &flux = m_engine->fluxTexture();
    for (uint32_t i = 0; i < 2; i++) {
        const VkDescriptorBufferInfo uboInfo{m_ubo.buffer, static_cast<VkDeviceSize>(i) * uboBytes, uboBytes};
        const VkDescriptorImageInfo fluxInfo{m_fluxSampler, flux.view, VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorBufferInfo yInfo{m_engine->yGridBuffer().buffer, 0, m_engine->yGridBuffer().size};
        const VkDescriptorBufferInfo nInfo{m_engine->nGridBuffer().buffer, 0, m_engine->nGridBuffer().size};
        const VkDescriptorBufferInfo bInfo{m_engine->boltHeightBuffer().buffer, 0,
                                           m_engine->boltHeightBuffer().size};

        VkWriteDescriptorSet w[5]{};
        auto set = [&](int k, uint32_t binding, VkDescriptorType type) {
            w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[k].dstSet = m_sets[i];
            w[k].dstBinding = binding;
            w[k].descriptorCount = 1;
            w[k].descriptorType = type;
        };
        set(0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        w[0].pBufferInfo = &uboInfo;
        set(1, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        w[1].pImageInfo = &fluxInfo;
        set(2, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        w[2].pBufferInfo = &yInfo;
        set(3, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        w[3].pBufferInfo = &nInfo;
        set(4, 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        w[4].pBufferInfo = &bInfo;
        vkUpdateDescriptorSets(dev, 5, w, 0, nullptr);
    }
}

void SceneRenderer::writeUbo(uint32_t frameIndex, const Camera &camera, const FrameInputs &in) {
    auto *f = reinterpret_cast<float *>(static_cast<uint8_t *>(m_ubo.mapped) +
                                        static_cast<size_t>(frameIndex) * kUboSliceBytes);
    std::memset(f, 0, kUboSliceBytes);

    const float aspect = static_cast<float>(m_extent.width) / static_cast<float>(m_extent.height > 0 ? m_extent.height : 1);
    const Mat4 vp = camera.viewProjMatrix(aspect);
    std::memcpy(f + 0, vp.m, sizeof(float) * 16);

    const Vec3 eye = camera.eye();
    f[16] = eye.x; f[17] = eye.y; f[18] = eye.z; f[19] = 0.0f;
    const Vec3 fwd = camera.forward(), rgt = camera.right(), up = camera.up();
    const float tanHalf = std::tan(camera.fovY() * 0.5f);
    f[20] = fwd.x; f[21] = fwd.y; f[22] = fwd.z; f[23] = tanHalf;
    f[24] = rgt.x; f[25] = rgt.y; f[26] = rgt.z; f[27] = aspect;
    f[28] = up.x;  f[29] = up.y;  f[30] = up.z;  f[31] = 0.0f;

    const Vec3 sun = in.sunDir.normalized();
    f[32] = sun.x; f[33] = sun.y; f[34] = sun.z; f[35] = 0.0f;

    // Macro basis: the same construction as computeHeliostatNormal()+computeBasis()
    // in shaders/common.slang, so the drawn plate matches the traced one.
    const auto &mn = m_engine->macroNormal();
    const Vec3 macroN{mn[0], mn[1], mn[2]};
    Vec3 mu, mv;
    if (std::fabs(macroN.x) < 1e-6f && std::fabs(macroN.z) < 1e-6f) {
        mu = Vec3{1, 0, 0};
        mv = Vec3{0, 0, 1};
    } else {
        mu = Vec3{0, 1, 0}.cross(macroN).normalized();
        mv = mu.cross(macroN);
    }
    f[36] = macroN.x; f[37] = macroN.y; f[38] = macroN.z; f[39] = 0.0f;
    f[40] = mu.x; f[41] = mu.y; f[42] = mu.z; f[43] = 0.0f;
    f[44] = mv.x; f[45] = mv.y; f[46] = mv.z; f[47] = 0.0f;

    f[48] = in.plateWidth; f[49] = in.plateLength; f[50] = in.gridSize; f[51] = in.deformScale;
    f[52] = in.receiverRadius; f[53] = in.receiverHeight;
    f[54] = in.receiverPos.x; f[55] = in.receiverPos.y;   // cx, cy
    f[56] = in.receiverPos.x; f[57] = in.receiverPos.y; f[58] = in.receiverPos.z;
    f[59] = static_cast<float>(m_engine->config().pixelWidth);
    f[60] = in.helioPos.x; f[61] = in.helioPos.y; f[62] = in.helioPos.z; f[63] = 0.0f;
    f[64] = in.fluxScale; f[65] = in.fluxLogFloor; f[66] = in.fluxLogCeil; f[67] = in.exposure;
    f[68] = in.debugView; f[69] = in.showBeams; f[70] = in.heatMode; f[71] = in.time;
    f[72] = 10.0f; f[73] = 900.0f; f[74] = in.beamIntensity;
    // ground.w is the receiver pixel *height* count (50), which the receiver
    // geometry needs to build one quad per flux texel. Getting this wrong draws
    // the cylinder with the wrong row count and samples the wrong part of the
    // flux texture -- which is exactly how the heat map went missing once.
    f[75] = static_cast<float>(m_engine->config().pixelHeight);
    const auto aim = m_engine->aimPoint();
    f[76] = aim[0]; f[77] = aim[1]; f[78] = aim[2]; f[79] = 0.0f;

    // ---- viewport / UI ----
    f[80] = static_cast<float>(m_extent.width);
    f[81] = static_cast<float>(m_extent.height);
    f[82] = m_extent.width ? 1.0f / static_cast<float>(m_extent.width) : 0.0f;
    f[83] = m_extent.height ? 1.0f / static_cast<float>(m_extent.height) : 0.0f;

    // Flux-map inset: bottom-right, keeping the 157x50 aspect.
    if (in.showFluxMap) {
        const VkRect2D r = fluxInsetRect(m_extent);
        f[84] = static_cast<float>(r.offset.x);
        f[85] = static_cast<float>(r.offset.y);
        f[86] = static_cast<float>(r.extent.width);
        f[87] = static_cast<float>(r.extent.height);
    } else {
        f[84] = 0.0f; f[85] = 0.0f; f[86] = -1.0f; f[87] = -1.0f;
    }

    // ---- heliostat field records: pos|selected, macroN, macroU, macroV ----
    for (uint32_t i = 0; i < 4; i++) {
        float *r = f + 88 + i * 16;
        std::memset(r, 0, sizeof(float) * 16);
        if (i < in.field.size()) {
            const auto &m = in.field[i];
            r[0] = m.pos.x; r[1] = m.pos.y; r[2] = m.pos.z; r[3] = m.selected ? 1.0f : 0.0f;
            r[4] = m.macroN.x; r[5] = m.macroN.y; r[6] = m.macroN.z; r[7] = 0.0f;
            r[8] = m.macroU.x; r[9] = m.macroU.y; r[10] = m.macroU.z; r[11] = 0.0f;
            r[12] = m.macroV.x; r[13] = m.macroV.y; r[14] = m.macroV.z; r[15] = 0.0f;
        }
    }
    f[152] = in.s95Level;   // stats.x: S95 contour on the inset
    f[153] = in.fluxPeak;
    // fluxCenter: where the inset rolls the cylinder to (see fsFluxMap).
    f[156] = in.fluxCenterU;
    f[157] = in.fluxCenterV;
}

void SceneRenderer::render(VkCommandBuffer cmd, uint32_t frameIndex, const Camera &camera,
                           const FrameInputs &in) {
    writeUbo(frameIndex, camera, in);
    m_fluxMapEnabled = in.showFluxMap;

    const VkExtent2D ext = m_extent;
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(ext.width), static_cast<float>(ext.height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, ext};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Discard-and-reuse transitions to the attachment layouts: both targets are
    // fully overwritten every frame (colour is cleared, depth is cleared), so
    // UNDEFINED as the old layout is both legal and the cheapest option.
    hviz::Texture &hdr = m_hdr[frameIndex & 1u];
    imageBarrier(cmd, hdr.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    imageBarrier(cmd, m_depth.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, 0,
                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);

    const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    VkRenderingAttachmentInfo color = colorAttachment(hdr.view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                     true, clear);
    VkRenderingAttachmentInfo depth = depthAttachment(m_depth.view, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                                      true, 0.0f);
    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = scissor;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &ri);
    {
        const VkDescriptorSet set = m_sets[frameIndex];
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeLayout, 0, 1, &set, 0, nullptr);

        // 1. sky (fullscreen triangle, writes the far depth value)
        ScenePC pcs{};
        pcs.mode = 99u;   // unused by vsSky
        vkCmdPushConstants(cmd, m_pipeLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(ScenePC), &pcs);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeSky);
        vkCmdDraw(cmd, 3, 1, 0, 0);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeScene);
        // 2. ground + cardinal markers, then every mirror of the field: foundation,
        //    tower and the receiver cylinder (one quad per flux texel)
        drawObject(cmd, 0u, 6u, 0u, 0u, 0.0f);
        if (in.showCardinals) {
            for (uint32_t axis = 0; axis < 4u; axis++) drawObject(cmd, 7u, 6u, 0u, axis, 3.0f);
        }
        const uint32_t mirrorCount = static_cast<uint32_t>(in.field.size());
        for (uint32_t i = 0; i < mirrorCount && i < 4u; i++) drawObject(cmd, 6u, 6u, 0u, i, 0.0f);
        drawObject(cmd, 1u, 32u * 6u, 32u, 0u, 0.0f);
        const hviz::Config &cfg = m_engine->config();
        drawObject(cmd, 2u, cfg.pixelWidth * cfg.pixelHeight * 6u, cfg.pixelWidth, cfg.pixelHeight, 0.0f);
        // 3. plates: vertex-pulled from yGrid/nGrid for the selected mirror, flat
        //    for the others (pc.extra carries the heliostat index)
        const uint32_t gs = m_gridSize;
        for (uint32_t i = 0; i < mirrorCount && i < 4u; i++) {
            drawObject(cmd, 3u, (gs - 1u) * (gs - 1u) * 6u, gs, i, 0.0f);
        }
        // 4. actuator markers: only on the traced (selected) mirror, and drawn in
        //    that mirror's own frame (pc.extra = its field index). With '8' the field
        //    is empty, so there is no frame to draw them in at all.
        if (mirrorCount > 0u) {
            uint32_t selectedMirror = 0u;
            for (uint32_t i = 0; i < mirrorCount && i < 4u; i++) {
                if (in.field[i].selected) selectedMirror = i;
            }
            drawObject(cmd, 4u, 35u * 36u, gs, selectedMirror, in.boltRadius);
        }

        // 5. beams (additive)
        if (in.showBeams > 0.5f) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeBeam);
            drawObject(cmd, 5u, 24u, 0u, 0u, m_beamWidth * in.beamIntensity);
        }
    }
    vkCmdEndRendering(cmd);
}

// Screen-space inset drawn *after* the post stack: it must not be tonemapped,
// bloomed or vignetted like scene content (it is a measurement display).
VkRect2D SceneRenderer::fluxInsetRect(VkExtent2D extent) {
    // ~28% of the width, capped, keeping the 157:50 receiver aspect, 16 px margin.
    const float mapW = std::min(360.0f, static_cast<float>(extent.width) * 0.28f);
    const float mapH = mapW * 50.0f / 157.0f;
    VkRect2D r{};
    r.offset.x = static_cast<int32_t>(static_cast<float>(extent.width) - mapW - 16.0f);
    r.offset.y = static_cast<int32_t>(static_cast<float>(extent.height) - mapH - 16.0f);
    r.extent.width = static_cast<uint32_t>(mapW);
    r.extent.height = static_cast<uint32_t>(mapH);
    return r;
}

void SceneRenderer::renderOverlays(VkCommandBuffer cmd, uint32_t frameIndex, VkImageView target,
                                   VkExtent2D extent) {
    if (!m_fluxMapEnabled) return;
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height),
                              0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    VkRenderingAttachmentInfo color =
        colorAttachment(target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false, nullptr);
    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = scissor;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeLayout, 0, 1, &m_sets[frameIndex], 0,
                            nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeFluxMap);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

void SceneRenderer::drawObject(VkCommandBuffer cmd, uint32_t mode, uint32_t vertexCount, uint32_t res,
                               uint32_t extra, float scale) {
    ScenePC pc{};
    pc.mode = mode;
    pc.res = res;
    pc.extra = extra;
    pc.scale = scale;
    vkCmdPushConstants(cmd, m_pipeLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(ScenePC), &pc);
    vkCmdDraw(cmd, vertexCount, 1, 0, 0);
}

void SceneRenderer::destroy() {
    VkDevice dev = m_vk->device();
    destroyTargets();
    if (m_fluxSampler) vkDestroySampler(dev, m_fluxSampler, nullptr);
    for (VkPipeline *p : {&m_pipeSky, &m_pipeScene, &m_pipeBeam, &m_pipeFluxMap}) {
        if (*p) vkDestroyPipeline(dev, *p, nullptr);
        *p = VK_NULL_HANDLE;
    }
    if (m_pipeLayout) vkDestroyPipelineLayout(dev, m_pipeLayout, nullptr);
    if (m_setLayout) vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);
    if (m_ubo.buffer) m_vk->destroyBuffer(m_ubo);
}

} // namespace viz
