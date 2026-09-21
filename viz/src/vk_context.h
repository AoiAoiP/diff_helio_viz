#pragma once

// vk_context.h — surface, swapchain and the frame synchronisation ring.
//
// Design (PLAN.md §7):
//   * one vkQueueSubmit per frame, 2 frames in flight;
//   * vkAcquireNextImageKHR + one binary semaphore per frame slot, one
//     render-finished semaphore per swapchain image, per-image fence;
//   * timestamp queries are read back for frame slot N *after* its fence has
//     been waited on, i.e. two frames late — the frame loop never stalls;
//   * zero per-frame allocation: everything is created in init()/recreate().
//
// The only place a device wait appears is recreateSwapchain(), which runs on a
// resize event (never in the steady-state frame path).

#include "gpu_timer.h"

#include "vk.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

struct HWND__;
using HWND = HWND__ *;

namespace viz {

class VkContext {
public:
    static constexpr uint32_t kFramesInFlight = 2;

    void init(hviz::VkCore &vk, HWND hwnd, uint32_t width, uint32_t height, bool vsync);
    void shutdown();

    // Returns false when the swapchain must be recreated (out of date, or a
    // resize was requested); the caller then calls recreate() and skips the
    // frame. Never blocks on GPU work in the steady state.
    bool beginFrame();
    void endFrame();

    // Resize / out-of-date handling. Waits for the device to go idle, which is
    // safe here because it is not part of the per-frame submit path. Returns
    // false when the rebuild was deferred (the surface has no valid size yet,
    // e.g. while minimised) — the caller should idle and retry later.
    bool recreate(uint32_t width, uint32_t height);
    bool needsRecreate() const { return m_needsRecreate; }
    void requestRecreate() { m_needsRecreate = true; }

    VkCommandBuffer cmd() const { return m_cmd[m_frame]; }
    uint32_t frameIndex() const { return m_frame; }
    uint32_t imageIndex() const { return m_imageIndex; }
    VkExtent2D extent() const { return m_extent; }
    VkFormat format() const { return m_format; }
    VkImage image(uint32_t i) const { return m_images[i]; }
    VkImageView imageView(uint32_t i) const { return m_views[i]; }
    uint32_t imageCount() const { return static_cast<uint32_t>(m_images.size()); }

    GpuTimer &timer() { return m_timer; }
    const GpuTimer &timer() const { return m_timer; }

    VkPresentModeKHR presentMode() const { return m_presentMode; }
    // Cycles FIFO -> MAILBOX -> IMMEDIATE (uncapped measurement), skipping modes
    // the surface does not support.
    void toggleVsync();
    void setPresentMode(VkPresentModeKHR mode);
    bool vsync() const { return m_presentMode == VK_PRESENT_MODE_FIFO_KHR; }
    const char *presentModeName() const;

    double refreshRateHz() const { return m_refreshHz; }

    // Evidence for the "no stall in the frame loop" claim: how often the loop
    // actually had to wait on GPU work, and for how long in total.
    void printCounters() const;

private:
    void createSwapchain(uint32_t width, uint32_t height, VkSwapchainKHR old);
    void createSyncObjects();
    void destroySwapchain();
    VkSurfaceFormatKHR chooseFormat() const;
    VkPresentModeKHR choosePresentMode() const;

    hviz::VkCore *m_vk = nullptr;
    HWND m_hwnd = nullptr;

    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_format = VK_FORMAT_B8G8R8A8_SRGB;
    VkColorSpaceKHR m_colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D m_extent{};
    VkPresentModeKHR m_presentMode = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkImage> m_images;
    std::vector<VkImageView> m_views;

    VkCommandPool m_cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer m_cmd[kFramesInFlight] = {};
    VkFence m_fence[kFramesInFlight] = {};
    VkSemaphore m_acquireSem[kFramesInFlight] = {};
    std::vector<VkSemaphore> m_renderSem;
    std::vector<VkFence> m_imagesInFlight;

    uint32_t m_frame = 0;
    uint32_t m_imageIndex = 0;
    bool m_needsRecreate = false;
    bool m_needsVsyncToggle = false;
    double m_refreshHz = 60.0;
    uint64_t m_slotWaits = 0;      // waits on a 2-frame-old slot fence
    uint64_t m_imageWaits = 0;     // extra waits because the image was still busy
    uint64_t m_recreates = 0;
    double m_blockedMs = 0.0;      // total time inside vkWaitForFences
    double m_imageWaitMs = 0.0;    // ... of which the image-reuse fence
    double m_acquireMs = 0.0;      // total time inside vkAcquireNextImageKHR
    GpuTimer m_timer;
};

} // namespace viz
