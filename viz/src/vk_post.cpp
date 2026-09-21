#include "vk_post.h"

#include <cstdio>
#include <cstring>

using hviz::checkVk;

namespace viz {

namespace {
constexpr uint32_t kUboSliceBytes = 256;   // >= 2 float4, aligned for std140 offsets

// Must match struct BloomPC in viz/shaders/post.slang.
struct BloomPC {
    float srcTexelX, srcTexelY;
    float threshold;
    float intensity;
    float dstTexelX, dstTexelY;
    float clampPeak;
    float pad0;
};
} // namespace

void PostStack::init(hviz::VkCore &vk, VkFormat swapchainFormat, VkExtent2D extent,
                     const std::string &shaderDir) {
    m_vk = &vk;
    m_swapFormat = swapchainFormat;
    m_extent = extent;

    VkDevice dev = vk.device();
    const std::string dir = shaderDir + "/";

    // One descriptor set layout for every post pipeline (see vk_post.h).
    VkDescriptorSetLayoutBinding bindings[3]{};
    auto add = [&](uint32_t b, VkDescriptorType type, VkShaderStageFlags stages) {
        bindings[b].binding = b;
        bindings[b].descriptorType = type;
        bindings[b].descriptorCount = 1;
        bindings[b].stageFlags = stages;
    };
    add(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT);
    add(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
    add(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
    VkDescriptorSetLayoutCreateInfo lci{};
    lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.bindingCount = 3;
    lci.pBindings = bindings;
    checkVk(vkCreateDescriptorSetLayout(dev, &lci, nullptr, &m_setLayout), "vkCreateDescriptorSetLayout(post)");

    const VkDescriptorSetLayout sets[1] = {m_setLayout};
    m_pipeLayout = createPipelineLayout(dev, sets, sizeof(BloomPC), VK_SHADER_STAGE_FRAGMENT_BIT);

    VkShaderModule vs = loadShaderModule(dev, dir + "vsFullscreen.spv");
    VkShaderModule fsPre = loadShaderModule(dev, dir + "fsBloomPre.spv");
    VkShaderModule fsDown = loadShaderModule(dev, dir + "fsBloomDown.spv");
    VkShaderModule fsUp = loadShaderModule(dev, dir + "fsBloomUp.spv");
    VkShaderModule fsComposite = loadShaderModule(dev, dir + "fsComposite.spv");

    GraphicsPipelineDesc base{};
    base.layout = m_pipeLayout;
    base.vs = vs;
    base.colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    base.depthAttachment = false;   // the post chain runs without a depth buffer

    GraphicsPipelineDesc pre = base;
    pre.fs = fsPre;
    m_pipePre = createGraphicsPipeline(dev, pre);

    GraphicsPipelineDesc down = base;
    down.fs = fsDown;
    m_pipeDown = createGraphicsPipeline(dev, down);

    GraphicsPipelineDesc up = base;
    up.fs = fsUp;
    up.blend = true;
    up.additiveBlend = true;
    m_pipeUp = createGraphicsPipeline(dev, up);

    GraphicsPipelineDesc composite = base;
    composite.fs = fsComposite;
    composite.colorFormat = m_swapFormat;
    m_pipeComposite = createGraphicsPipeline(dev, composite);
    destroyShaderModules(dev, {vs, fsPre, fsDown, fsUp, fsComposite});

    m_sampler = createSampler(dev, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

    const float black[1] = {0.0f};
    m_bloomFallback = m_vk->createTexture(1, 1, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT);
    m_vk->uploadTexture(m_bloomFallback, black);

    m_ubo = vk.createBuffer(kUboSliceBytes * kFrames, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
    std::memset(m_ubo.mapped, 0, static_cast<size_t>(m_ubo.size));

    createTargets(extent);
    createDescriptors();
}

void PostStack::createTargets(VkExtent2D extent) {
    m_extent = extent;
    for (uint32_t f = 0; f < kFrames; f++) {
        uint32_t w = extent.width > 1 ? extent.width / 2 : 1;
        uint32_t h = extent.height > 1 ? extent.height / 2 : 1;
        for (uint32_t level = 0; level < kBloomLevels; level++) {
            m_bloom[f][level].extent = {w > 1 ? w : 1, h > 1 ? h : 1};
            m_bloom[f][level].tex =
                createImage2D(*m_vk, m_bloom[f][level].extent.width, m_bloom[f][level].extent.height,
                              VK_FORMAT_R16G16B16A16_SFLOAT,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
        }
    }
}

void PostStack::destroyTargets() {
    for (auto &frame : m_bloom)
        for (auto &b : frame) m_vk->destroyTexture(b.tex);
}

void PostStack::resize(VkExtent2D extent) {
    if (extent.width == m_extent.width && extent.height == m_extent.height) return;
    vkDeviceWaitIdle(m_vk->device());
    destroyTargets();
    createTargets(extent);
    writeDescriptors();
}

void PostStack::setSceneTexture(uint32_t frame, VkImageView view, VkImageLayout layout) {
    if (frame >= kFrames) return;
    m_sceneView[frame] = view;
    m_sceneLayout[frame] = layout;
    if (m_compositeSets[frame] != VK_NULL_HANDLE) writeDescriptorsForFrame(frame);
}

void PostStack::createDescriptors() {
    VkDevice dev = m_vk->device();
    VkDescriptorSetLayout bloomLayouts[kBloomSourceCount][kFrames];
    for (uint32_t s = 0; s < kBloomSourceCount; s++)
        for (uint32_t f = 0; f < kFrames; f++) bloomLayouts[s][f] = m_setLayout;

    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = m_vk->descriptorPool();
    ai.descriptorSetCount = kBloomSourceCount * kFrames;
    ai.pSetLayouts = &bloomLayouts[0][0];
    checkVk(vkAllocateDescriptorSets(dev, &ai, &m_bloomSets[0][0]), "vkAllocateDescriptorSets(bloom)");

    VkDescriptorSetLayout compLayouts[kFrames] = {m_setLayout, m_setLayout};
    ai.descriptorSetCount = kFrames;
    ai.pSetLayouts = compLayouts;
    checkVk(vkAllocateDescriptorSets(dev, &ai, m_compositeSets), "vkAllocateDescriptorSets(composite)");

    writeDescriptors();
}

// Bloom source index layout:
//   0                -> scene HDR (input of the bright pass)
//   1 .. levels      -> mip 0 .. levels-1 (inputs of the downsample chain)
//   levels+1 .. 2*levels-1 -> mip levels-1 .. 1 (inputs of the upsample chain)
namespace {
uint32_t downSourceIndex(uint32_t level) { return 1u + level; }
uint32_t upSourceIndex(uint32_t level) { return 1u + PostStack::kBloomLevels + (PostStack::kBloomLevels - 1u - level); }
} // namespace

void PostStack::writeDescriptors() {
    for (uint32_t f = 0; f < kFrames; f++) writeDescriptorsForFrame(f);
}

void PostStack::writeDescriptorsForFrame(uint32_t f) {
    VkDevice dev = m_vk->device();
    {
        for (uint32_t s = 0; s < kBloomSourceCount; s++) {
            VkImageView view = m_bloomFallback.view;
            VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
            if (s == 0) {
                if (m_sceneView[f] != VK_NULL_HANDLE) {
                    view = m_sceneView[f];
                    layout = m_sceneLayout[f];
                }
            } else if (s <= kBloomLevels) {
                view = m_bloom[f][s - 1].tex.view;
                layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            } else {
                const uint32_t level = 2u * kBloomLevels - s;   // see the index note above
                view = m_bloom[f][level].tex.view;
                layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
            writeCombinedImage(dev, m_bloomSets[s][f], 1, m_sampler, view, layout);
            writeCombinedImage(dev, m_bloomSets[s][f], 2, m_sampler, m_bloomFallback.view,
                               VK_IMAGE_LAYOUT_GENERAL);
            writeUniformBuffer(dev, m_bloomSets[s][f], 0, m_ubo.buffer, kUboSliceBytes);
        }

        // Composite: scene HDR + bloom level 0.
        const bool sceneReady = m_sceneView[f] != VK_NULL_HANDLE;
        writeUniformBuffer(dev, m_compositeSets[f], 0, m_ubo.buffer, kUboSliceBytes);
        writeCombinedImage(dev, m_compositeSets[f], 1, m_sampler,
                           sceneReady ? m_sceneView[f] : m_bloomFallback.view,
                           sceneReady ? m_sceneLayout[f] : VK_IMAGE_LAYOUT_GENERAL);
        writeCombinedImage(dev, m_compositeSets[f], 2, m_sampler, m_bloom[f][0].tex.view,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}

void PostStack::writeUbo(uint32_t frameIndex, VkExtent2D extent, const Params &params) {
    auto *f = reinterpret_cast<float *>(static_cast<uint8_t *>(m_ubo.mapped) +
                                        static_cast<size_t>(frameIndex) * kUboSliceBytes);
    f[0] = params.bloomStrength;
    f[1] = params.exposure;
    f[2] = params.bloomThreshold;
    f[3] = params.time;
    f[4] = static_cast<float>(extent.width);
    f[5] = static_cast<float>(extent.height);
    f[6] = extent.width > 0 ? 1.0f / static_cast<float>(extent.width) : 0.0f;
    f[7] = extent.height > 0 ? 1.0f / static_cast<float>(extent.height) : 0.0f;
}

void PostStack::renderBloom(VkCommandBuffer cmd, uint32_t frameIndex, const Params &params, GpuTimer *timer) {
    auto pass = [&](VkPipeline pipeline, VkDescriptorSet set, VkExtent2D dst, VkExtent2D src, float threshold,
                    float intensity) {
        const VkViewport vp{0.0f, 0.0f, static_cast<float>(dst.width), static_cast<float>(dst.height), 0.0f, 1.0f};
        const VkRect2D sc{{0, 0}, dst};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        BloomPC pc{};
        pc.srcTexelX = src.width ? 1.0f / static_cast<float>(src.width) : 0.0f;
        pc.srcTexelY = src.height ? 1.0f / static_cast<float>(src.height) : 0.0f;
        pc.threshold = threshold;
        pc.intensity = intensity;
        pc.dstTexelX = dst.width ? 1.0f / static_cast<float>(dst.width) : 0.0f;
        pc.dstTexelY = dst.height ? 1.0f / static_cast<float>(dst.height) : 0.0f;
        pc.clampPeak = params.bloomClamp;
        vkCmdPushConstants(cmd, m_pipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeLayout, 0, 1, &set, 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        drawFullscreen(cmd);
    };

    auto drawInto = [&](const BloomTarget &dst, VkDescriptorSet set, VkPipeline pipeline, VkExtent2D src,
                        bool load, float threshold, float intensity) {
        const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        VkRenderingAttachmentInfo color = colorAttachment(dst.tex.view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                          !load, clear);
        VkRenderingInfo ri{};
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea = {{0, 0}, dst.extent};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        vkCmdBeginRendering(cmd, &ri);
        pass(pipeline, set, dst.extent, src, threshold, intensity);
        vkCmdEndRendering(cmd);
    };

    auto toRead = [&](const hviz::Texture &t) {
        imageBarrier(cmd, t.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    };
    auto toWrite = [&](const hviz::Texture &t) {
        imageBarrier(cmd, t.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    };
    auto toBlend = [&](const hviz::Texture &t) {
        // Read -> write hazard: the previous pass sampled this level.
        imageBarrier(cmd, t.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT);
    };

    // ---- bright pass: scene HDR -> mip 0 ----
    {
        const BloomTarget &l0 = m_bloom[frameIndex][0];
        toWrite(l0.tex);
        drawInto(l0, m_bloomSets[0][frameIndex], m_pipePre, m_extent, false, params.bloomThreshold, 1.0f);
        toRead(l0.tex);
    }
    if (timer) timer->stamp(cmd, frameIndex, kStageBloomPre);

    // ---- downsample chain ----
    for (uint32_t level = 1; level < kBloomLevels; level++) {
        const BloomTarget &dst = m_bloom[frameIndex][level];
        const BloomTarget &src = m_bloom[frameIndex][level - 1];
        toWrite(dst.tex);
        drawInto(dst, m_bloomSets[downSourceIndex(level - 1)][frameIndex], m_pipeDown, src.extent, false, 0.0f,
                 1.0f);
        toRead(dst.tex);
    }
    if (timer) timer->stamp(cmd, frameIndex, kStageBloomDown);

    // ---- tent upsample, additively blended back into the larger level ----
    for (int level = static_cast<int>(kBloomLevels) - 1; level > 0; level--) {
        const BloomTarget &dst = m_bloom[frameIndex][level - 1];
        const BloomTarget &src = m_bloom[frameIndex][level];
        toBlend(dst.tex);
        drawInto(dst, m_bloomSets[upSourceIndex(static_cast<uint32_t>(level))][frameIndex], m_pipeUp,
                 src.extent, true, 0.0f, 1.0f);
        toRead(dst.tex);
    }
    if (timer) timer->stamp(cmd, frameIndex, kStageBloomUp);
}

void PostStack::renderComposite(VkCommandBuffer cmd, uint32_t frameIndex, VkImageView target, VkExtent2D extent,
                                const Params &params) {
    writeUbo(frameIndex, extent, params);

    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height),
                              0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    VkRenderingAttachmentInfo color = colorAttachment(target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true, clear);
    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = scissor;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeLayout, 0, 1,
                            &m_compositeSets[frameIndex], 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeComposite);
    drawFullscreen(cmd);
    vkCmdEndRendering(cmd);
}

void PostStack::destroy() {
    VkDevice dev = m_vk->device();
    destroyTargets();
    for (VkPipeline *p : {&m_pipePre, &m_pipeDown, &m_pipeUp, &m_pipeComposite}) {
        if (*p) vkDestroyPipeline(dev, *p, nullptr);
        *p = VK_NULL_HANDLE;
    }
    if (m_pipeLayout) vkDestroyPipelineLayout(dev, m_pipeLayout, nullptr);
    if (m_setLayout) vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);
    if (m_sampler) vkDestroySampler(dev, m_sampler, nullptr);
    m_vk->destroyTexture(m_bloomFallback);
    if (m_ubo.buffer) m_vk->destroyBuffer(m_ubo);
}

} // namespace viz
