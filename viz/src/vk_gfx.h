#pragma once

// vk_gfx.h — small hand-written graphics helpers shared by every viewer pass:
// image creation, layout barriers, samplers, graphics pipelines.
//
// No render pass objects are used: every pass runs through dynamic rendering
// (VkRenderingInfo, core in Vulkan 1.3+). No vertex buffers are used either:
// geometry comes from gl_VertexIndex and storage buffers (vertex pulling).

#include "vk.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <span>
#include <vector>

namespace viz {

// ------------------------------------------------------------------ images --
hviz::Texture createImage2D(hviz::VkCore &vk, uint32_t width, uint32_t height, VkFormat format,
                            VkImageUsageFlags usage, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                            uint32_t mipLevels = 1);
VkImageView createImageView2D(VkDevice dev, VkImage image, VkFormat format, VkImageAspectFlags aspect,
                              uint32_t baseMip = 0, uint32_t mipCount = 1);

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                  VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage, VkAccessFlags srcAccess,
                  VkAccessFlags dstAccess, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT, uint32_t mipCount = 1);

VkSampler createSampler(VkDevice dev, VkFilter filter, VkSamplerMipmapMode mipMode, VkSamplerAddressMode address);

// Set up a dynamic-rendering colour attachment that is cleared on load and kept
// in COLOR_ATTACHMENT_OPTIMAL for the following pass.
struct RenderingAttachment {
    VkRenderingAttachmentInfo color{};
    VkRenderingAttachmentInfo depth{};
};

VkRenderingAttachmentInfo colorAttachment(VkImageView view, VkImageLayout layout, bool clear,
                                          const float rgba[4], bool store = true);
VkRenderingAttachmentInfo depthAttachment(VkImageView view, VkImageLayout layout, bool clear, float depth);

// ---------------------------------------------------------------- pipeline --
struct GraphicsPipelineDesc {
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE;
    VkShaderModule fs = VK_NULL_HANDLE;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkCullModeFlags cull = VK_CULL_MODE_NONE;
    bool depthTest = false;
    bool depthWrite = false;
    VkCompareOp depthCompare = VK_COMPARE_OP_GREATER;   // reversed-Z
    bool blend = false;
    bool additiveBlend = false;
    VkFormat colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    uint32_t colorAttachmentCount = 1;
    // True when the pipeline is used inside a dynamic-rendering pass that *has*
    // a depth attachment. The declared format must match the attachment's even
    // when depth testing is disabled (the sky pass writes the far value).
    bool depthAttachment = true;
    // Optional vertex input (the scene/post passes pull from storage buffers and
    // bind nothing at all; the HUD streams a vertex buffer).
    const VkVertexInputBindingDescription *vertexBindings = nullptr;
    uint32_t vertexBindingCount = 0;
    const VkVertexInputAttributeDescription *vertexAttributes = nullptr;
    uint32_t vertexAttributeCount = 0;
};

VkShaderModule loadShaderModule(VkDevice dev, const std::string &spvPath);

VkPipeline createGraphicsPipeline(VkDevice dev, const GraphicsPipelineDesc &desc);

// Pipeline layout with N descriptor set layouts and one push-constant range.
VkPipelineLayout createPipelineLayout(VkDevice dev, std::span<const VkDescriptorSetLayout> sets,
                                      uint32_t pushConstantSize, VkShaderStageFlags pushStages);

// Shader modules are only needed during pipeline creation.
void destroyShaderModules(VkDevice dev, std::initializer_list<VkShaderModule> modules);

// ------------------------------------------------------------- descriptors --
// One-shot descriptor write helper (init-time only; never called per frame).
void writeCombinedImage(VkDevice dev, VkDescriptorSet set, uint32_t binding, VkSampler sampler, VkImageView view,
                        VkImageLayout layout);
void writeStorageImage(VkDevice dev, VkDescriptorSet set, uint32_t binding, VkImageView view,
                       VkImageLayout layout);
void writeStorageBuffer(VkDevice dev, VkDescriptorSet set, uint32_t binding, VkBuffer buffer, VkDeviceSize size);
void writeUniformBuffer(VkDevice dev, VkDescriptorSet set, uint32_t binding, VkBuffer buffer, VkDeviceSize size);

// -------------------------------------------------------------- fullscreen --
// Draws a single triangle covering the viewport (no vertex buffer bound).
void drawFullscreen(VkCommandBuffer cmd);

} // namespace viz
