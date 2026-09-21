#include "vk_gfx.h"

#include <cstdio>
#include <stdexcept>

using hviz::checkVk;

namespace viz {

hviz::Texture createImage2D(hviz::VkCore &vk, uint32_t width, uint32_t height, VkFormat format,
                            VkImageUsageFlags usage, VkImageAspectFlags aspect, uint32_t mipLevels) {
    hviz::Texture t;
    t.width = width;
    t.height = height;
    t.format = format;

    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = mipLevels;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    checkVk(vkCreateImage(vk.device(), &info, nullptr, &t.image), "vkCreateImage(viewer)");

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(vk.device(), t.image, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    // Prefer device-local; every viewer target is GPU-resident.
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(vk.physicalDevice(), &mem);
    uint32_t typeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
        if ((req.memoryTypeBits & (1u << i)) &&
            (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            typeIndex = i;
            break;
        }
    }
    if (typeIndex == UINT32_MAX) throw std::runtime_error("no device-local memory type for image");
    alloc.memoryTypeIndex = typeIndex;
    checkVk(vkAllocateMemory(vk.device(), &alloc, nullptr, &t.memory), "vkAllocateMemory(viewer image)");
    vkBindImageMemory(vk.device(), t.image, t.memory, 0);
    t.view = createImageView2D(vk.device(), t.image, format, aspect, 0, mipLevels);
    return t;
}

VkImageView createImageView2D(VkDevice dev, VkImage image, VkFormat format, VkImageAspectFlags aspect,
                              uint32_t baseMip, uint32_t mipCount) {
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {aspect, baseMip, mipCount, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    checkVk(vkCreateImageView(dev, &vi, nullptr, &view), "vkCreateImageView");
    return view;
}

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                  VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage, VkAccessFlags srcAccess,
                  VkAccessFlags dstAccess, VkImageAspectFlags aspect, uint32_t mipCount) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, mipCount, 0, 1};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

VkSampler createSampler(VkDevice dev, VkFilter filter, VkSamplerMipmapMode mipMode,
                        VkSamplerAddressMode address) {
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = filter;
    info.minFilter = filter;
    info.mipmapMode = mipMode;
    info.addressModeU = address;
    info.addressModeV = address;
    info.addressModeW = address;
    info.maxLod = VK_LOD_CLAMP_NONE;
    VkSampler s = VK_NULL_HANDLE;
    checkVk(vkCreateSampler(dev, &info, nullptr, &s), "vkCreateSampler");
    return s;
}

VkRenderingAttachmentInfo colorAttachment(VkImageView view, VkImageLayout layout, bool clear,
                                          const float rgba[4], bool store) {
    VkRenderingAttachmentInfo a{};
    a.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    a.imageView = view;
    a.imageLayout = layout;
    a.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    a.storeOp = store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    if (clear && rgba)
        for (int i = 0; i < 4; i++) a.clearValue.color.float32[i] = rgba[i];
    return a;
}

VkRenderingAttachmentInfo depthAttachment(VkImageView view, VkImageLayout layout, bool clear, float depth) {
    VkRenderingAttachmentInfo a{};
    a.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    a.imageView = view;
    a.imageLayout = layout;
    a.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    a.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    if (clear) a.clearValue.depthStencil = {depth, 0};
    return a;
}

VkShaderModule loadShaderModule(VkDevice dev, const std::string &spvPath) {
    const auto words = hviz::VkCore::loadSpirv(spvPath);
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = words.size() * sizeof(uint32_t);
    info.pCode = words.data();
    VkShaderModule m = VK_NULL_HANDLE;
    checkVk(vkCreateShaderModule(dev, &info, nullptr, &m), "vkCreateShaderModule(viewer)");
    return m;
}

VkPipelineLayout createPipelineLayout(VkDevice dev, std::span<const VkDescriptorSetLayout> sets,
                                      uint32_t pushConstantSize, VkShaderStageFlags pushStages) {
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = static_cast<uint32_t>(sets.size());
    info.pSetLayouts = sets.empty() ? nullptr : sets.data();
    VkPushConstantRange range{};
    if (pushConstantSize > 0) {
        range.stageFlags = pushStages;
        range.size = pushConstantSize;
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &range;
    }
    VkPipelineLayout layout = VK_NULL_HANDLE;
    checkVk(vkCreatePipelineLayout(dev, &info, nullptr, &layout), "vkCreatePipelineLayout(viewer)");
    return layout;
}

VkPipeline createGraphicsPipeline(VkDevice dev, const GraphicsPipelineDesc &desc) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = desc.vs;
    stages[0].pName = "main";   // slangc always names the entry point "main"
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = desc.fs;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = desc.vertexBindingCount;
    vi.pVertexBindingDescriptions = desc.vertexBindings;
    vi.vertexAttributeDescriptionCount = desc.vertexAttributeCount;
    vi.pVertexAttributeDescriptions = desc.vertexAttributes;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = desc.topology;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = desc.cull;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = desc.depthTest ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = desc.depthWrite ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = desc.depthCompare;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (desc.blend) {
        blendAttachment.blendEnable = VK_TRUE;
        if (desc.additiveBlend) {
            blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
            blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        } else {
            blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
            blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        }
    }

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = desc.colorAttachmentCount;
    cb.pAttachments = &blendAttachment;

    const VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    VkFormat colorFormats[1] = {desc.colorFormat};
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = desc.colorAttachmentCount;
    rendering.pColorAttachmentFormats = colorFormats;
    rendering.depthAttachmentFormat = desc.depthAttachment ? desc.depthFormat : VK_FORMAT_UNDEFINED;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState = &ms;
    info.pDepthStencilState = &ds;
    info.pColorBlendState = &cb;
    info.pDynamicState = &dyn;
    info.layout = desc.layout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    checkVk(r, "vkCreateGraphicsPipelines");
    return pipeline;
}

void destroyShaderModules(VkDevice dev, std::initializer_list<VkShaderModule> modules) {
    for (auto m : modules)
        if (m) vkDestroyShaderModule(dev, m, nullptr);
}

void writeCombinedImage(VkDevice dev, VkDescriptorSet set, uint32_t binding, VkSampler sampler, VkImageView view,
                        VkImageLayout layout) {
    VkDescriptorImageInfo info{sampler, view, layout};
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &info;
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
}

void writeStorageImage(VkDevice dev, VkDescriptorSet set, uint32_t binding, VkImageView view,
                       VkImageLayout layout) {
    VkDescriptorImageInfo info{VK_NULL_HANDLE, view, layout};
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w.pImageInfo = &info;
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
}

void writeStorageBuffer(VkDevice dev, VkDescriptorSet set, uint32_t binding, VkBuffer buffer, VkDeviceSize size) {
    VkDescriptorBufferInfo info{buffer, 0, size};
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &info;
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
}

void writeUniformBuffer(VkDevice dev, VkDescriptorSet set, uint32_t binding, VkBuffer buffer, VkDeviceSize size) {
    VkDescriptorBufferInfo info{buffer, 0, size};
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w.pBufferInfo = &info;
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
}

void drawFullscreen(VkCommandBuffer cmd) {
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

} // namespace viz
