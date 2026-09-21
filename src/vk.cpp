// Win32 window-system integration (VkWin32SurfaceCreateInfoKHR,
// vkCreateWin32SurfaceKHR, vkGetPhysicalDeviceWin32PresentationSupportKHR).
// The macro has to be set before <vulkan/vulkan.h> is pulled in, and this is the
// only translation unit that touches those entry points.
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR 1
#endif
#include <windows.h>

#include "vk.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace hviz {

void checkVk(VkResult result, const char *what) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string("Vulkan error in ") + what + " (VkResult " +
                                 std::to_string(static_cast<int>(result)) + ")");
    }
}

// ---------------------------------------------------------------- instance --

static bool layerAvailable(const char *name) {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const auto &l : layers)
        if (std::strcmp(l.layerName, name) == 0) return true;
    return false;
}

static bool instanceExtensionAvailable(const char *name) {
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, exts.data());
    for (const auto &e : exts)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

static bool deviceExtensionAvailable(VkPhysicalDevice dev, const char *name) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, exts.data());
    for (const auto &e : exts)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                   VkDebugUtilsMessageTypeFlagsEXT types,
                                                   const VkDebugUtilsMessengerCallbackDataEXT *data,
                                                   void * /*user*/) {
    (void)types;
    const char *tag = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)       ? "ERROR"
                      : (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)   ? "warn"
                                                                                       : "info";
    std::fprintf(stderr, "[validation:%s] %s\n", tag, data->pMessage);
    return VK_FALSE;
}

VkCore::VkCore(bool enableValidation, void *win32Hwnd) : m_hwnd(win32Hwnd) {
    createInstance(enableValidation);
    if (m_hwnd) createSurface();
    pickPhysicalDevice();
    createDevice();
    createPools();
}

VkCore::~VkCore() {
    if (m_device) {
        vkDeviceWaitIdle(m_device);
        vkDestroyDescriptorPool(m_device, m_descPool, nullptr);
        vkDestroyCommandPool(m_device, m_cmdPool, nullptr);
        vkDestroyDevice(m_device, nullptr);
    }
    if (m_surface) vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    if (m_debugMessenger) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy) destroy(m_instance, m_debugMessenger, nullptr);
    }
    if (m_instance) vkDestroyInstance(m_instance, nullptr);
}

void VkCore::createInstance(bool enableValidation) {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = m_hwnd ? "Heliostat Studio (viewer)" : "Heliostat Studio (core)";
    // The headless path keeps 1.3 (unchanged); the viewer asks for 1.4 so that
    // dynamic rendering / timestamp queries are core, not extensions.
    app.apiVersion = m_hwnd ? VK_API_VERSION_1_4 : VK_API_VERSION_1_3;

    std::vector<const char *> layers;
    if (enableValidation) {
        const char *kValidation = "VK_LAYER_KHRONOS_validation";
        if (layerAvailable(kValidation)) {
            layers.push_back(kValidation);
            std::puts("[vk] validation layer enabled");
        } else {
            std::puts("[vk] validation layer requested but not installed - continuing without it");
        }
    }

    std::vector<const char *> exts;
    bool wantDebugUtils = false;
    if (m_hwnd) {
        // Present path: window system integration.
        for (const char *e : {"VK_KHR_surface", "VK_KHR_win32_surface"}) {
            if (!instanceExtensionAvailable(e))
                throw std::runtime_error(std::string("Vulkan instance extension missing: ") + e);
            exts.push_back(e);
        }
        if ((enableValidation || true) && instanceExtensionAvailable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            wantDebugUtils = true;
        }
    }

    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    info.enabledLayerCount = static_cast<uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.data();
    info.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    info.ppEnabledExtensionNames = exts.data();
    checkVk(vkCreateInstance(&info, nullptr, &m_instance), "vkCreateInstance");

    if (wantDebugUtils) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
        if (create) {
            VkDebugUtilsMessengerCreateInfoEXT dci{};
            dci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
            dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            dci.pfnUserCallback = debugCallback;
            if (create(m_instance, &dci, nullptr, &m_debugMessenger) != VK_SUCCESS) m_debugMessenger = VK_NULL_HANDLE;
        }
    }
}

void VkCore::createSurface() {
    VkWin32SurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    info.hinstance = GetModuleHandleW(nullptr);
    info.hwnd = static_cast<HWND>(m_hwnd);
    checkVk(vkCreateWin32SurfaceKHR(m_instance, &info, nullptr, &m_surface), "vkCreateWin32SurfaceKHR");
}

// Present-capable devices must additionally expose a queue family that does
// compute + graphics + present (the viewer runs the flux compute chain and the
// scene pass on one queue, then presents from it).
bool VkCore::deviceUsableForPresent(VkPhysicalDevice dev) const {
    if (m_surface == VK_NULL_HANDLE) return true;
    if (!deviceExtensionAvailable(dev, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) return false;

    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &familyCount, families.data());
    for (uint32_t i = 0; i < familyCount; i++) {
        if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        if (!(families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) continue;
        if (vkGetPhysicalDeviceWin32PresentationSupportKHR(dev, i)) return true;
    }
    return false;
}

void VkCore::pickPhysicalDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    if (count == 0) throw std::runtime_error("No Vulkan-capable GPU found");
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_instance, &count, devices.data());

    VkPhysicalDevice integrated = VK_NULL_HANDLE;
    for (auto dev : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(dev, &props);
        if (!deviceUsableForPresent(dev)) {
            if (m_surface) std::printf("[vk] skipping %s (no compute+graphics+present family)\n", props.deviceName);
            continue;
        }
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            m_physical = dev;
            break;
        }
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU && integrated == VK_NULL_HANDLE)
            integrated = dev;
    }
    if (m_physical == VK_NULL_HANDLE) m_physical = integrated != VK_NULL_HANDLE ? integrated : devices[0];

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(m_physical, &props);
    m_deviceName = props.deviceName;
    m_timestampPeriodNs = props.limits.timestampPeriod;
    m_timestampsSupported = props.limits.timestampComputeAndGraphics != 0;
    m_validTimestampBits = props.limits.timestampComputeAndGraphics ? 64u : 0u;
    std::printf("[vk] selected GPU: %s (API %u.%u.%u, timestampPeriod %.2f ns)\n", props.deviceName,
                VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
                VK_VERSION_PATCH(props.apiVersion), props.limits.timestampPeriod);
}

void VkCore::createDevice() {
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &familyCount, families.data());

    bool found = false;
    for (uint32_t i = 0; i < familyCount; i++) {
        if (!(families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) continue;
        if (m_surface) {
            if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            if (!vkGetPhysicalDeviceWin32PresentationSupportKHR(m_physical, i)) continue;
        }
        m_queueFamily = i;
        found = true;
        break;
    }
    if (!found) {
        throw std::runtime_error(m_surface ? "No compute+graphics+present queue family on the selected GPU"
                                           : "No compute queue family on the selected GPU");
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qinfo{};
    qinfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qinfo.queueFamilyIndex = m_queueFamily;
    qinfo.queueCount = 1;
    qinfo.pQueuePriorities = &priority;

    std::vector<const char *> devExts;
    if (m_surface) devExts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    // Vulkan features are opt-in even when they are core for the requested API
    // version. The viewer needs dynamic rendering (no VkRenderPass objects at
    // all) and the DrawParameters capability that Slang emits for graphics
    // stages. The headless CLI path enables nothing extra.
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceVulkan11Features features11{};
    features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    features11.shaderDrawParameters = VK_TRUE;
    features11.pNext = &features13;

    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.pNext = m_surface ? &features11 : nullptr;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &qinfo;
    info.enabledExtensionCount = static_cast<uint32_t>(devExts.size());
    info.ppEnabledExtensionNames = devExts.data();
    checkVk(vkCreateDevice(m_physical, &info, nullptr, &m_device), "vkCreateDevice");
    vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);
}


void VkCore::createPools() {
    VkCommandPoolCreateInfo cp{};
    cp.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cp.queueFamilyIndex = m_queueFamily;
    cp.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    checkVk(vkCreateCommandPool(m_device, &cp, nullptr, &m_cmdPool), "vkCreateCommandPool");

    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 16},
        // The viewer's graphics passes sample the compute-written flux texture,
        // the HDR target and the bloom chain through combined image samplers
        // (unused by the headless CLI, which allocates a single set).
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64},
    };
    VkDescriptorPoolCreateInfo dp{};
    dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp.maxSets = 32;
    dp.poolSizeCount = 4;
    dp.pPoolSizes = sizes;
    checkVk(vkCreateDescriptorPool(m_device, &dp, nullptr, &m_descPool), "vkCreateDescriptorPool");
}

uint32_t VkCore::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(m_physical, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
        if ((typeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & props) == props) return i;
    }
    throw std::runtime_error("No suitable memory type");
}

// ----------------------------------------------------------------- buffers --

Buffer VkCore::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    checkVk(vkCreateBuffer(m_device, &info, nullptr, &b.buffer), "vkCreateBuffer");

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(m_device, b.buffer, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = hostVisible
                                ? findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                                : findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    checkVk(vkAllocateMemory(m_device, &alloc, nullptr, &b.memory), "vkAllocateMemory(buffer)");
    vkBindBufferMemory(m_device, b.buffer, b.memory, 0);
    if (hostVisible) checkVk(vkMapMemory(m_device, b.memory, 0, size, 0, &b.mapped), "vkMapMemory");
    return b;
}

void VkCore::uploadBuffer(const Buffer &dst, const void *data, VkDeviceSize size) {
    if (dst.mapped) {
        std::memcpy(dst.mapped, data, static_cast<size_t>(size));
        return;
    }
    // Staging path: allocate -> copy -> submit -> wait -> free.
    Buffer staging = createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::memcpy(staging.mapped, data, static_cast<size_t>(size));
    submitOneShot([&](VkCommandBuffer cmd) {
        VkBufferCopy region{0, 0, size};
        vkCmdCopyBuffer(cmd, staging.buffer, dst.buffer, 1, &region);
    });
    destroyBuffer(staging);
}

void VkCore::downloadBuffer(const Buffer &src, void *data, VkDeviceSize size) {
    if (src.mapped) {
        std::memcpy(data, src.mapped, static_cast<size_t>(size));
        return;
    }
    Buffer staging = createBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    submitOneShot([&](VkCommandBuffer cmd) {
        VkBufferCopy region{0, 0, size};
        vkCmdCopyBuffer(cmd, src.buffer, staging.buffer, 1, &region);
    });
    std::memcpy(data, staging.mapped, static_cast<size_t>(size));
    destroyBuffer(staging);
}

void VkCore::destroyBuffer(Buffer &b) {
    if (b.mapped) vkUnmapMemory(m_device, b.memory);
    if (b.buffer) vkDestroyBuffer(m_device, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(m_device, b.memory, nullptr);
    b = {};
}

// ---------------------------------------------------------------- textures --

Texture VkCore::createTexture(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage) {
    Texture t;
    t.width = width;
    t.height = height;
    t.format = format;

    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    checkVk(vkCreateImage(m_device, &info, nullptr, &t.image), "vkCreateImage");

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(m_device, t.image, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    checkVk(vkAllocateMemory(m_device, &alloc, nullptr, &t.memory), "vkAllocateMemory(image)");
    vkBindImageMemory(m_device, t.image, t.memory, 0);

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = t.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    checkVk(vkCreateImageView(m_device, &view, nullptr, &t.view), "vkCreateImageView");

    // Compute writes need GENERAL layout.
    submitOneShot([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = t.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
    });
    return t;
}

void VkCore::uploadTexture(const Texture &tex, const void *data) {
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(tex.width) * tex.height * sizeof(float);
    Buffer staging = createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::memcpy(staging.mapped, data, static_cast<size_t>(bytes));
    submitOneShot([&](VkCommandBuffer cmd) {
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {tex.width, tex.height, 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, tex.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    });
    destroyBuffer(staging);
}

void VkCore::downloadTexture(const Texture &tex, void *out) {
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(tex.width) * tex.height * sizeof(float);
    Buffer staging = createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    submitOneShot([&](VkCommandBuffer cmd) {
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {tex.width, tex.height, 1};
        vkCmdCopyImageToBuffer(cmd, tex.image, VK_IMAGE_LAYOUT_GENERAL, staging.buffer, 1, &region);
    });
    std::memcpy(out, staging.mapped, static_cast<size_t>(bytes));
    destroyBuffer(staging);
}

void VkCore::destroyTexture(Texture &t) {
    if (t.view) vkDestroyImageView(m_device, t.view, nullptr);
    if (t.image) vkDestroyImage(m_device, t.image, nullptr);
    if (t.memory) vkFreeMemory(m_device, t.memory, nullptr);
    t = {};
}

// ------------------------------------------------------- pipelines / sets --

VkDescriptorSetLayout VkCore::createSetLayout(std::span<const VkDescriptorSetLayoutBinding> bindings) {
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    checkVk(vkCreateDescriptorSetLayout(m_device, &info, nullptr, &layout), "vkCreateDescriptorSetLayout");
    return layout;
}

VkDescriptorSet VkCore::allocateSet(VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorPool = m_descPool;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    checkVk(vkAllocateDescriptorSets(m_device, &info, &set), "vkAllocateDescriptorSets");
    return set;
}

Pipeline VkCore::createComputePipeline(std::span<const uint32_t> spirv, const char *entryPoint,
                                       uint32_t pushConstantSize, VkDescriptorSetLayout setLayout) {
    Pipeline p;
    VkPipelineLayoutCreateInfo linfo{};
    linfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    linfo.setLayoutCount = 1;
    linfo.pSetLayouts = &setLayout;
    VkPushConstantRange pcRange{};
    if (pushConstantSize > 0) {
        pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcRange.size = pushConstantSize;
        linfo.pushConstantRangeCount = 1;
        linfo.pPushConstantRanges = &pcRange;
    }
    checkVk(vkCreatePipelineLayout(m_device, &linfo, nullptr, &p.layout), "vkCreatePipelineLayout");

    VkShaderModuleCreateInfo sinfo{};
    sinfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sinfo.codeSize = spirv.size() * sizeof(uint32_t);
    sinfo.pCode = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    checkVk(vkCreateShaderModule(m_device, &sinfo, nullptr, &module), "vkCreateShaderModule");

    VkComputePipelineCreateInfo pinfo{};
    pinfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pinfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pinfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pinfo.stage.module = module;
    pinfo.stage.pName = entryPoint;
    pinfo.layout = p.layout;
    const VkResult r = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pinfo, nullptr, &p.pipeline);
    vkDestroyShaderModule(m_device, module, nullptr);
    checkVk(r, "vkCreateComputePipelines");
    return p;
}

void VkCore::destroyPipeline(Pipeline &p) {
    if (p.pipeline) vkDestroyPipeline(m_device, p.pipeline, nullptr);
    if (p.layout) vkDestroyPipelineLayout(m_device, p.layout, nullptr);
    p = {};
}

Pipeline VkCore::createComputePipelineOnLayout(std::span<const uint32_t> spirv, const char *entryPoint,
                                               VkPipelineLayout layout) {
    VkShaderModuleCreateInfo sinfo{};
    sinfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sinfo.codeSize = spirv.size() * sizeof(uint32_t);
    sinfo.pCode = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    checkVk(vkCreateShaderModule(m_device, &sinfo, nullptr, &module), "vkCreateShaderModule");

    VkComputePipelineCreateInfo pinfo{};
    pinfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pinfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pinfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pinfo.stage.module = module;
    pinfo.stage.pName = entryPoint;
    pinfo.layout = layout;
    Pipeline p;
    p.layout = layout;   // owned by the caller
    const VkResult r = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pinfo, nullptr, &p.pipeline);
    vkDestroyShaderModule(m_device, module, nullptr);
    checkVk(r, "vkCreateComputePipelines");
    return p;
}

// -------------------------------------------------------------- submission --

void VkCore::computeBarrier(VkCommandBuffer cmd) {
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &barrier, 0, nullptr, 0, nullptr);
}

void VkCore::submitOneShot(const std::function<void(VkCommandBuffer)> &record, float *outGpuMs) {
    VkCommandBufferAllocateInfo ainfo{};
    ainfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ainfo.commandPool = m_cmdPool;
    ainfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ainfo.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    checkVk(vkAllocateCommandBuffers(m_device, &ainfo, &cmd), "vkAllocateCommandBuffers");

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkVk(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");

    const bool timed = outGpuMs != nullptr && m_timestampsSupported;
    VkQueryPool queryPool = VK_NULL_HANDLE;
    if (timed) {
        VkQueryPoolCreateInfo qinfo{};
        qinfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qinfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qinfo.queryCount = 2;
        checkVk(vkCreateQueryPool(m_device, &qinfo, nullptr, &queryPool), "vkCreateQueryPool");
        vkCmdResetQueryPool(cmd, queryPool, 0, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, 0);
    }

    record(cmd);

    if (timed) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 1);
    checkVk(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

    VkFenceCreateInfo finfo{};
    finfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    checkVk(vkCreateFence(m_device, &finfo, nullptr, &fence), "vkCreateFence");

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    checkVk(vkQueueSubmit(m_queue, 1, &submit, fence), "vkQueueSubmit");
    checkVk(vkWaitForFences(m_device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");

    if (timed) {
        uint64_t stamps[2] = {0, 0};
        if (vkGetQueryPoolResults(m_device, queryPool, 0, 2, sizeof(stamps), stamps, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            *outGpuMs = static_cast<float>(static_cast<double>(stamps[1] - stamps[0]) * m_timestampPeriodNs * 1e-6);
        }
        vkDestroyQueryPool(m_device, queryPool, nullptr);
    }

    vkDestroyFence(m_device, fence, nullptr);
    vkFreeCommandBuffers(m_device, m_cmdPool, 1, &cmd);
}

void VkCore::waitIdle() { vkQueueWaitIdle(m_queue); }

std::vector<uint32_t> VkCore::loadSpirv(const std::string &path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("Cannot open SPIR-V: " + path +
                                     "\n  (build the project first: shaders are compiled into build/shaders/)");
    const size_t bytes = static_cast<size_t>(f.tellg());
    if (bytes == 0 || bytes % 4 != 0) throw std::runtime_error("Invalid SPIR-V size: " + path);
    f.seekg(0);
    std::vector<uint32_t> words(bytes / 4);
    f.read(reinterpret_cast<char *>(words.data()), static_cast<std::streamsize>(bytes));
    return words;
}

} // namespace hviz
