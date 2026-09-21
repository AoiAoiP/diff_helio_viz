#pragma once

// vk_post.h — the post stack: a 5-level bloom chain (bright pass + 4 downsample
// + 4 tent upsample passes), then composite (ACES, exposure, vignette) into the
// sRGB swapchain image.
//
// Every size-dependent target is per frame in flight: the scene pass of frame
// N+1 must never overwrite an HDR target that frame N's bloom/composite is still
// reading (a WAR hazard that a layout transition alone does not order).

#include "gpu_timer.h"
#include "vk.h"
#include "vk_gfx.h"

#include <string>

namespace viz {

class PostStack {
public:
    static constexpr uint32_t kBloomLevels = 5;
    static constexpr uint32_t kFrames = 2;

    struct Params {
        float bloomStrength = 0.55f;
        float exposure = 1.0f;
        float bloomThreshold = 1.0f;
        float time = 0.0f;
        // Firefly guard for the bright pass. The stylised sky sun disc reaches 16x
        // and a mirror specular hit ~8x; without a clamp those single regions turn
        // into large blooms as soon as the strength is raised.
        float bloomClamp = 6.0f;
    };

    // 'sceneView' is the scene HDR target of frame 0; frame 1 is registered
    // through setFrameSceneTexture(). Both are re-pointed on resize.
    void init(hviz::VkCore &vk, VkFormat swapchainFormat, VkExtent2D extent, const std::string &shaderDir);
    void destroy();

    void setSceneTexture(uint32_t frame, VkImageView view, VkImageLayout layout);
    void resize(VkExtent2D extent);

    // Records bright pass -> downsample chain -> upsample chain, stamping the
    // three bloom stages into the pass timer.
    void renderBloom(VkCommandBuffer cmd, uint32_t frameIndex, const Params &params, GpuTimer *timer);
    // Records the composite into 'target' (the swapchain image).
    void renderComposite(VkCommandBuffer cmd, uint32_t frameIndex, VkImageView target, VkExtent2D extent,
                         const Params &params);

    VkExtent2D bloomExtent(uint32_t level) const { return m_bloom[0][level].extent; }

private:
    struct BloomTarget {
        hviz::Texture tex;
        VkExtent2D extent{};
    };

    void createTargets(VkExtent2D extent);
    void destroyTargets();
    void createDescriptors();
    void writeDescriptors();
    // Writes only one frame's sets, so re-registering a single frame's scene view
    // never touches a view that another frame's resize already destroyed.
    void writeDescriptorsForFrame(uint32_t frame);
    void writeUbo(uint32_t frameIndex, VkExtent2D extent, const Params &params);

    hviz::VkCore *m_vk = nullptr;
    VkFormat m_swapFormat = VK_FORMAT_B8G8R8A8_SRGB;
    VkExtent2D m_extent{};

    // One layout for every post pipeline: 0 = UBO, 1 = source image, 2 = second
    // image (only the composite uses it for the bloom result). Sharing the layout
    // keeps the descriptor churn at zero and matches the shader's global
    // declarations, which live in one file.
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    static constexpr uint32_t kBloomLevels5 = kBloomLevels;
    // Bloom source sets: index 0 = scene HDR, 1..4 = mip levels 0..3 used as
    // downsample sources, 5..8 = mip levels 4..1 used as upsample sources.
    static constexpr uint32_t kBloomSourceCount = 1 + 2 * kBloomLevels;
    static constexpr uint32_t kFrames2 = kFrames;
    VkDescriptorSet m_bloomSets[kBloomSourceCount][kFrames] = {};
    VkDescriptorSet m_compositeSets[kFrames] = {};
    VkPipelineLayout m_pipeLayout = VK_NULL_HANDLE;
    VkPipeline m_pipePre = VK_NULL_HANDLE;
    VkPipeline m_pipeDown = VK_NULL_HANDLE;
    VkPipeline m_pipeUp = VK_NULL_HANDLE;
    VkPipeline m_pipeComposite = VK_NULL_HANDLE;

    VkImageView m_sceneView[kFrames] = {};
    VkImageLayout m_sceneLayout[kFrames] = {};
    VkSampler m_sampler = VK_NULL_HANDLE;
    hviz::Texture m_bloomFallback;   // 1x1 black, keeps descriptor writes valid
    hviz::Texture m_uboFallback;     // 16 B, keeps the unused UBO binding valid
    hviz::Buffer m_ubo;
    BloomTarget m_bloom[kFrames][kBloomLevels];
};

} // namespace viz
