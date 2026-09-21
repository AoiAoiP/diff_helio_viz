#pragma once

// vk.h — minimal Vulkan compute wrapper.
//
// Scope: instance / physical device / compute queue / buffers / textures /
// compute pipelines / one-shot submission with optional GPU timing.
//
// Deliberately NOT here (the visualization milestone adds these, see PLAN.md):
//   * VkSurfaceKHR + swapchain + present
//   * graphics pipelines (vertex/fragment) and render passes
//   * multi-frame-in-flight synchronisation
//
// The per-call staging-buffer allocation used by upload/download is fine for a
// CLI tool that runs once per asset, but it is exactly what a real-time loop
// must avoid: see "Host-side sync" in PLAN.md §8.2.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace hviz {

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void *mapped = nullptr;  // non-null for host-visible buffers
};

struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_R32_SFLOAT;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct Pipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
};

class VkCore {
public:
    // 'win32Hwnd' (optional, Win32 only): create a VK_KHR_win32_surface for that
    // window and pick a queue family that can do compute + graphics + present.
    // The headless CLI path passes nullptr and behaves exactly as before (no
    // surface extensions are requested at all).
    explicit VkCore(bool enableValidation, void *win32Hwnd = nullptr);
    ~VkCore();

    VkCore(const VkCore &) = delete;
    VkCore &operator=(const VkCore &) = delete;

    VkInstance instance() const { return m_instance; }
    VkPhysicalDevice physicalDevice() const { return m_physical; }
    VkDevice device() const { return m_device; }
    VkQueue queue() const { return m_queue; }
    uint32_t queueFamily() const { return m_queueFamily; }
    VkDescriptorPool descriptorPool() const { return m_descPool; }
    const std::string &deviceName() const { return m_deviceName; }
    float timestampPeriodNs() const { return m_timestampPeriodNs; }
    // ---- Present path (non-null only when a window handle was supplied) ----
    VkSurfaceKHR surface() const { return m_surface; }
    bool presentCapable() const { return m_surface != VK_NULL_HANDLE; }
    bool timestampsSupported() const { return m_timestampsSupported; }

    // ---- Buffers ----
    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible);
    void uploadBuffer(const Buffer &dst, const void *data, VkDeviceSize size);
    void downloadBuffer(const Buffer &src, void *data, VkDeviceSize size);
    void destroyBuffer(Buffer &b);

    // ---- Textures (2D, R32F by default) ----
    Texture createTexture(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage);
    void uploadTexture(const Texture &tex, const void *data);
    void downloadTexture(const Texture &tex, void *out);
    void destroyTexture(Texture &t);

    // ---- Pipelines / descriptors ----
    VkDescriptorSetLayout createSetLayout(std::span<const VkDescriptorSetLayoutBinding> bindings);
    VkDescriptorSet allocateSet(VkDescriptorSetLayout layout);
    Pipeline createComputePipeline(std::span<const uint32_t> spirv, const char *entryPoint,
                                   uint32_t pushConstantSize, VkDescriptorSetLayout setLayout);
    // Additive overload: build on an externally created pipeline layout. The
    // viewer needs a push-constant range on top of the engine's descriptor set
    // layout (its flux kernels take spp / tileCount at runtime).
    Pipeline createComputePipelineOnLayout(std::span<const uint32_t> spirv, const char *entryPoint,
                                           VkPipelineLayout layout);
    void destroyPipeline(Pipeline &p);

    // ---- Submission ----
    // Records 'record' into a fresh command buffer, submits once and waits.
    // If outGpuMs is non-null the elapsed GPU time of the recording is measured
    // with two timestamps (same mechanism the real-time HUD will use).
    void submitOneShot(const std::function<void(VkCommandBuffer)> &record, float *outGpuMs = nullptr);

    // Compute -> compute barrier. Coarse but correct; a production renderer
    // should narrow the stage/access masks to what each dispatch actually needs.
    static void computeBarrier(VkCommandBuffer cmd);

    void waitIdle();

    // SPIR-V loader (returns uint32 words).
    static std::vector<uint32_t> loadSpirv(const std::string &path);

private:
    void createInstance(bool enableValidation);
    void createSurface();
    bool deviceUsableForPresent(VkPhysicalDevice dev) const;
    void pickPhysicalDevice();
    void createDevice();
    void createPools();
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;

    void *m_hwnd = nullptr;  // Win32 HWND, null on the headless CLI path

    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    uint32_t m_queueFamily = 0;
    VkCommandPool m_cmdPool = VK_NULL_HANDLE;
    VkDescriptorPool m_descPool = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    std::string m_deviceName;
    float m_timestampPeriodNs = 1.0f;
    bool m_timestampsSupported = false;
    uint32_t m_validTimestampBits = 0;
};

void checkVk(VkResult result, const char *what);

} // namespace hviz
