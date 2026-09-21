#include "vk_context.h"

#include "platform_win32.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <stdexcept>

using hviz::checkVk;

namespace viz {

namespace {

VkSurfaceCapabilitiesKHR queryCaps(VkPhysicalDevice phys, VkSurfaceKHR surface) {
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);
    return caps;
}

} // namespace

void VkContext::init(hviz::VkCore &vk, HWND hwnd, uint32_t width, uint32_t height, bool vsync) {
    m_vk = &vk;
    m_hwnd = hwnd;
    if (!vk.surface()) throw std::runtime_error("VkContext::init: VkCore has no surface (pass the HWND)");

    // A rough refresh-rate guess for the HUD; vkGetPhysicalDeviceSurfaceCapabilitiesKHR
    // reports min/max but not the current rate, so we use the monitor's mode.
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) {
        m_refreshHz = static_cast<double>(dm.dmDisplayFrequency);
    }

    VkPresentModeKHR forced = vsync ? VK_PRESENT_MODE_FIFO_KHR : choosePresentMode();
    m_presentMode = forced;
    createSwapchain(width, height, VK_NULL_HANDLE);
    createSyncObjects();
    m_timer.init(vk.device(), vk.timestampPeriodNs(), kFramesInFlight, vk.timestampsSupported());

    std::printf("[vk] swapchain: %ux%u %s, %u images, present mode %s, %s\n", m_extent.width, m_extent.height,
                m_format == VK_FORMAT_B8G8R8A8_SRGB ? "B8G8R8A8_SRGB" : "non-sRGB", imageCount(),
                presentModeName(), vk.deviceName().c_str());
}

VkPresentModeKHR VkContext::choosePresentMode() const {
    const auto &phys = m_vk->physicalDevice();
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, m_vk->surface(), &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, m_vk->surface(), &count, modes.data());
    // Prefer MAILBOX (uncapped, no tearing), then IMMEDIATE (uncapped, tearing).
    for (VkPresentModeKHR want : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
        for (auto m : modes)
            if (m == want) return want;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkSurfaceFormatKHR VkContext::chooseFormat() const {
    const auto &phys = m_vk->physicalDevice();
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, m_vk->surface(), &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, m_vk->surface(), &count, formats.data());
    if (formats.empty()) throw std::runtime_error("surface reports no formats");

    const VkSurfaceFormatKHR *fallback = &formats[0];
    for (const auto &f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) return f;
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            fallback = &f;
    }
    return *fallback;
}

void VkContext::createSwapchain(uint32_t width, uint32_t height, VkSwapchainKHR old) {
    const auto &phys = m_vk->physicalDevice();
    const VkSurfaceCapabilitiesKHR caps = queryCaps(phys, m_vk->surface());
    const VkSurfaceFormatKHR fmt = chooseFormat();
    m_format = fmt.format;
    m_colorSpace = fmt.colorSpace;

    // A degenerated capability set (all zeros) means the surface currently has no
    // valid size — typical while the window is minimised. Clamping against
    // min=max=0 would produce an out-of-bounds extent, so fall back to the window
    // size and let the caller retry after the restore.
    const bool capsDegenerate = caps.currentExtent.width == 0 && caps.currentExtent.height == 0 &&
                                caps.maxImageExtent.width == 0 && caps.maxImageExtent.height == 0;
    if (caps.currentExtent.width != 0xFFFFFFFFu && !capsDegenerate) {
        m_extent = caps.currentExtent;
    } else if (capsDegenerate) {
        m_extent.width = std::max(1u, width);
        m_extent.height = std::max(1u, height);
    } else {
        m_extent.width = std::max(caps.minImageExtent.width, std::min(caps.maxImageExtent.width, width));
        m_extent.height = std::max(caps.minImageExtent.height, std::min(caps.maxImageExtent.height, height));
    }
    if (m_extent.width == 0 || m_extent.height == 0) {
        m_extent.width = std::max(1u, width);
        m_extent.height = std::max(1u, height);
    }

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

    // FIFO must always be supported; MAILBOX/IMMEDIATE are checked already.
    if (m_presentMode != VK_PRESENT_MODE_FIFO_KHR) {
        uint32_t modeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(phys, m_vk->surface(), &modeCount, nullptr);
        std::vector<VkPresentModeKHR> modes(modeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(phys, m_vk->surface(), &modeCount, modes.data());
        bool ok = false;
        for (auto m : modes)
            if (m == m_presentMode) ok = true;
        if (!ok) m_presentMode = VK_PRESENT_MODE_FIFO_KHR;
    }

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = m_vk->surface();
    info.minImageCount = imageCount;
    info.imageFormat = m_format;
    info.imageColorSpace = m_colorSpace;
    info.imageExtent = m_extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = m_presentMode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = old;

    VkSwapchainKHR sc = VK_NULL_HANDLE;
    checkVk(vkCreateSwapchainKHR(m_vk->device(), &info, nullptr, &sc), "vkCreateSwapchainKHR");
    if (old != VK_NULL_HANDLE) vkDestroySwapchainKHR(m_vk->device(), old, nullptr);
    m_swapchain = sc;

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(m_vk->device(), m_swapchain, &n, nullptr);
    m_images.resize(n);
    vkGetSwapchainImagesKHR(m_vk->device(), m_swapchain, &n, m_images.data());

    m_views.resize(n, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < n; i++) {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = m_images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = m_format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        checkVk(vkCreateImageView(m_vk->device(), &vi, nullptr, &m_views[i]), "vkCreateImageView(swapchain)");
    }
}

void VkContext::createSyncObjects() {
    VkDevice dev = m_vk->device();

    VkCommandPoolCreateInfo cp{};
    cp.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cp.queueFamilyIndex = m_vk->queueFamily();
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    checkVk(vkCreateCommandPool(dev, &cp, nullptr, &m_cmdPool), "vkCreateCommandPool(viewer)");

    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = m_cmdPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = kFramesInFlight;
    checkVk(vkAllocateCommandBuffers(dev, &ai, m_cmd), "vkAllocateCommandBuffers");

    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkSemaphoreCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (uint32_t i = 0; i < kFramesInFlight; i++) {
        checkVk(vkCreateFence(dev, &fi, nullptr, &m_fence[i]), "vkCreateFence");
        checkVk(vkCreateSemaphore(dev, &si, nullptr, &m_acquireSem[i]), "vkCreateSemaphore");
    }

    m_renderSem.assign(m_images.size(), VK_NULL_HANDLE);
    for (auto &s : m_renderSem) checkVk(vkCreateSemaphore(dev, &si, nullptr, &s), "vkCreateSemaphore(present)");
    m_imagesInFlight.assign(m_images.size(), VK_NULL_HANDLE);
}

bool VkContext::beginFrame() {
    if (m_needsVsyncToggle) {
        m_needsVsyncToggle = false;
        m_needsRecreate = true;   // recreate with the new present mode
    }
    if (m_needsRecreate) return false;

    VkDevice dev = m_vk->device();
    const uint32_t f = m_frame;

    // This slot's work is 2 frames old; waiting here is the only sync point and
    // it does not stall the GPU pipeline. The wait time is accumulated so the
    // claim can be checked at the end of a run.
    const auto waitStart = std::chrono::steady_clock::now();
    checkVk(vkWaitForFences(dev, 1, &m_fence[f], VK_TRUE, UINT64_MAX), "vkWaitForFences(slot)");
    m_slotWaits++;
    m_blockedMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart).count();
    m_timer.collect(f);

    const auto acquireStart = std::chrono::steady_clock::now();
    VkResult r = vkAcquireNextImageKHR(dev, m_swapchain, UINT64_MAX, m_acquireSem[f], VK_NULL_HANDLE,
                                       &m_imageIndex);
    m_acquireMs +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - acquireStart).count();
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        m_needsRecreate = true;
        return false;
    }
    if (r == VK_SUBOPTIMAL_KHR) m_needsRecreate = true;   // finish this frame, recreate next
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
        checkVk(r, "vkAcquireNextImageKHR");

    if (m_imagesInFlight[m_imageIndex] != VK_NULL_HANDLE) {
        const auto t0 = std::chrono::steady_clock::now();
        checkVk(vkWaitForFences(dev, 1, &m_imagesInFlight[m_imageIndex], VK_TRUE, UINT64_MAX),
                "vkWaitForFences(image)");
        const double dt = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        m_imageWaits++;
        m_imageWaitMs += dt;
        m_blockedMs += dt;
        m_timer.collect(f);   // slot numbers may have refreshed
    }
    m_imagesInFlight[m_imageIndex] = m_fence[f];
    checkVk(vkResetFences(dev, 1, &m_fence[f]), "vkResetFences");

    VkCommandBuffer cmd = m_cmd[f];
    checkVk(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkVk(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");

    m_timer.beginFrame(cmd, f);
    return true;
}

void VkContext::endFrame() {
    VkDevice dev = m_vk->device();
    const uint32_t f = m_frame;
    VkCommandBuffer cmd = m_cmd[f];
    checkVk(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &m_acquireSem[f];
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &m_renderSem[m_imageIndex];
    checkVk(vkQueueSubmit(m_vk->queue(), 1, &submit, m_fence[f]), "vkQueueSubmit(frame)");

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &m_renderSem[m_imageIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &m_swapchain;
    present.pImageIndices = &m_imageIndex;
    const VkResult r = vkQueuePresentKHR(m_vk->queue(), &present);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) m_needsRecreate = true;
    else if (r != VK_SUCCESS) checkVk(r, "vkQueuePresentKHR");

    m_frame = (m_frame + 1) % kFramesInFlight;
}

void VkContext::destroySwapchain() {
    VkDevice dev = m_vk->device();
    for (auto v : m_views)
        if (v) vkDestroyImageView(dev, v, nullptr);
    m_views.clear();
    m_images.clear();
    for (auto s : m_renderSem)
        if (s) vkDestroySemaphore(dev, s, nullptr);
    m_renderSem.clear();
    m_imagesInFlight.clear();
    if (m_swapchain) vkDestroySwapchainKHR(dev, m_swapchain, nullptr);
    m_swapchain = VK_NULL_HANDLE;
}

bool VkContext::recreate(uint32_t width, uint32_t height) {
    m_needsRecreate = false;
    if (width == 0 || height == 0) {   // minimized: keep the old chain
        m_needsRecreate = true;
        return false;
    }
    // Minimised/zero-sized surfaces report a degenerate capability set; rebuilding
    // then would create an invalid swapchain. Defer until the window has a size.
    const VkSurfaceCapabilitiesKHR caps = queryCaps(m_vk->physicalDevice(), m_vk->surface());
    if (caps.currentExtent.width == 0 && caps.currentExtent.height == 0 &&
        caps.maxImageExtent.width == 0 && caps.maxImageExtent.height == 0) {
        m_needsRecreate = true;
        return false;
    }
    m_recreates++;
    VkDevice dev = m_vk->device();
    // Not on the per-frame path: resize / present-mode change only.
    vkDeviceWaitIdle(dev);
    const VkSwapchainKHR old = m_swapchain;
    m_swapchain = VK_NULL_HANDLE;
    // Views and per-image semaphores are destroyed before the new chain is
    // created; the old swapchain itself is passed as oldSwapchain and released
    // by createSwapchain() once the replacement exists.
    for (auto v : m_views) vkDestroyImageView(dev, v, nullptr);
    m_views.clear();
    for (auto s : m_renderSem) vkDestroySemaphore(dev, s, nullptr);
    m_renderSem.clear();
    m_images.clear();

    createSwapchain(width, height, old);
    VkSemaphoreCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    m_renderSem.assign(m_images.size(), VK_NULL_HANDLE);
    for (auto &s : m_renderSem) checkVk(vkCreateSemaphore(dev, &si, nullptr, &s), "vkCreateSemaphore(present)");
    m_imagesInFlight.assign(m_images.size(), VK_NULL_HANDLE);
    std::printf("[vk] swapchain recreated: %ux%u (%u images, %s)\n", m_extent.width, m_extent.height,
                imageCount(), presentModeName());
    return true;
}

void VkContext::toggleVsync() {
    // FIFO (vsync) -> MAILBOX (uncapped, no tearing) -> IMMEDIATE (uncapped, no
    // queueing at all: the honest way to measure the wall-clock ceiling).
    const auto &phys = m_vk->physicalDevice();
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, m_vk->surface(), &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, m_vk->surface(), &count, modes.data());
    auto supported = [&](VkPresentModeKHR m) {
        if (m == VK_PRESENT_MODE_FIFO_KHR) return true;   // always supported
        for (auto x : modes)
            if (x == m) return true;
        return false;
    };
    const VkPresentModeKHR order[] = {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR,
                                      VK_PRESENT_MODE_FIFO_KHR};
    size_t cur = 0;
    for (size_t i = 0; i < 3; i++)
        if (order[i] == m_presentMode) cur = i;
    for (size_t step = 1; step <= 3; step++) {
        const VkPresentModeKHR next = order[(cur + step) % 3];
        if (!supported(next)) continue;
        m_presentMode = next;
        break;
    }
    m_needsVsyncToggle = true;
}

void VkContext::setPresentMode(VkPresentModeKHR mode) {
    if (mode == m_presentMode) return;
    m_presentMode = mode;
    m_needsVsyncToggle = true;
}

const char *VkContext::presentModeName() const {
    switch (m_presentMode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE (uncapped, tearing)";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX (uncapped)";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
    default: return "FIFO (vsync)";
    }
}

void VkContext::shutdown() {
    if (!m_vk) return;
    VkDevice dev = m_vk->device();
    vkDeviceWaitIdle(dev);
    m_timer.destroy();
    destroySwapchain();
    for (uint32_t i = 0; i < kFramesInFlight; i++) {
        if (m_fence[i]) vkDestroyFence(dev, m_fence[i], nullptr);
        if (m_acquireSem[i]) vkDestroySemaphore(dev, m_acquireSem[i], nullptr);
    }
    if (m_cmdPool) vkDestroyCommandPool(dev, m_cmdPool, nullptr);
    m_cmdPool = VK_NULL_HANDLE;
    m_vk = nullptr;
}

void VkContext::printCounters() const {
    std::printf("[viz] frame-loop stalls (per run): slot-fence %llu waits (%.3f ms), image-fence %llu waits "
                "(%.3f ms), vkAcquireNextImageKHR %.3f ms | swapchain recreations %llu\n",
                static_cast<unsigned long long>(m_slotWaits), m_blockedMs - m_imageWaitMs,
                static_cast<unsigned long long>(m_imageWaits), m_imageWaitMs, m_acquireMs,
                static_cast<unsigned long long>(m_recreates));
}

} // namespace viz
