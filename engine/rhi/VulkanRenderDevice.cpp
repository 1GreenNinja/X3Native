// Vulkan implementation of IRenderDevice — D1 (clean-room).
// Spec: specs/D1-render-device.spec.md
//
// IMPLEMENTED: instance + Win32 surface + device (vk-bootstrap), swapchain,
// per-frame command buffers + sync, dynamic-rendering clear-to-color, present,
// resize/out-of-date recreation. Validation-clean on RTX A2000 (Vulkan 1.3).
// TODO (D2+): VMA allocator, real draw passes, descriptor sets, pipelines.
//
// This file is the ONLY place Vulkan headers are included — IRenderDevice.h
// stays graphics-API-free.

#include "IRenderDevice.h"
#include "../core/x3_log.h"

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>
#include <VkBootstrap.h>

#include <vector>
#include <string>
#include <cmath>

namespace x3::rhi {

namespace {

constexpr uint32_t kFramesInFlight = 2;

class VulkanRenderDevice final : public IRenderDevice {
public:
    bool init(const DeviceDesc& desc) override {
        m_vsync = desc.vsync;
        m_width = desc.width; m_height = desc.height;

        // ---- Instance ----
        vkb::InstanceBuilder ib;
        auto inst_ret = ib.set_app_name("X3Native")
                          .set_engine_name("X3Native")
                          .require_api_version(1, 3, 0)
                          .request_validation_layers(desc.validation)
                          .use_default_debug_messenger()
                          .build();
        if (!inst_ret) { logError(std::string("[rhi] instance: ") + inst_ret.error().message()); return false; }
        m_inst = inst_ret.value();

        // ---- Win32 surface ----
        if (!desc.nativeWindowHandle) { logError("[rhi] init: null HWND"); return false; }
        VkWin32SurfaceCreateInfoKHR sci{};
        sci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        sci.hinstance = ::GetModuleHandle(nullptr);
        sci.hwnd = static_cast<HWND>(desc.nativeWindowHandle);
        if (vkCreateWin32SurfaceKHR(m_inst.instance, &sci, nullptr, &m_surface) != VK_SUCCESS) {
            logError("[rhi] vkCreateWin32SurfaceKHR failed"); return false;
        }

        // ---- Physical + logical device (Vulkan 1.3 features) ----
        VkPhysicalDeviceVulkan13Features f13{};
        f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        f13.dynamicRendering = VK_TRUE;
        f13.synchronization2 = VK_TRUE;
        VkPhysicalDeviceVulkan12Features f12{};
        f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        f12.descriptorIndexing = VK_TRUE;
        f12.timelineSemaphore  = VK_TRUE;
        f12.bufferDeviceAddress = VK_TRUE;

        vkb::PhysicalDeviceSelector sel{ m_inst };
        auto phys_ret = sel.set_surface(m_surface).set_minimum_version(1,3)
                           .set_required_features_13(f13).set_required_features_12(f12).select();
        if (!phys_ret) { logError(std::string("[rhi] phys: ") + phys_ret.error().message()); return false; }
        vkb::PhysicalDevice phys = phys_ret.value();
        m_descriptorIndexing = true;

        vkb::DeviceBuilder db{ phys };
        auto dev_ret = db.build();
        if (!dev_ret) { logError(std::string("[rhi] device: ") + dev_ret.error().message()); return false; }
        m_dev = dev_ret.value();

        auto q   = m_dev.get_queue(vkb::QueueType::graphics);
        auto qfi = m_dev.get_queue_index(vkb::QueueType::graphics);
        if (!q || !qfi) { logError("[rhi] no graphics queue"); return false; }
        m_gfxQueue = q.value();
        m_gfxFamily = qfi.value();

        logInfo(std::string("[rhi] device ready: ") + phys.name +
                " (Vulkan 1.3, dynamic-rendering + sync2 + descriptor-indexing)");

        if (!createSwapchain(m_width, m_height)) return false;
        if (!createPerFrame()) return false;
        return true;
    }

    void shutdown() override {
        if (m_dev.device) vkDeviceWaitIdle(m_dev.device);
        destroyPerFrame();
        destroySwapchain();
        if (m_dev.device)    vkb::destroy_device(m_dev);
        if (m_surface)       vkDestroySurfaceKHR(m_inst.instance, m_surface, nullptr);
        if (m_inst.instance) vkb::destroy_instance(m_inst);
        m_surface = VK_NULL_HANDLE;
    }

    void onResize(uint32_t w, uint32_t h) override {
        if (w == 0 || h == 0) return;
        m_width = w; m_height = h;
        m_needsRecreate = true;
    }

    FrameContext beginFrame() override {
        FrameContext fc{};
        if (m_needsRecreate) { recreateSwapchain(); m_needsRecreate = false; }
        if (m_swapchain == VK_NULL_HANDLE) return fc;

        auto& fr = m_frames[m_frameIdx];
        vkWaitForFences(m_dev.device, 1, &fr.inFlight, VK_TRUE, UINT64_MAX);

        uint32_t imageIndex = 0;
        VkResult acq = vkAcquireNextImageKHR(m_dev.device, m_swapchain, UINT64_MAX,
                                             fr.imageAvailable, VK_NULL_HANDLE, &imageIndex);
        if (acq == VK_ERROR_OUT_OF_DATE_KHR) { m_needsRecreate = true; return fc; }
        if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) { logError("[rhi] acquire failed"); return fc; }

        vkResetFences(m_dev.device, 1, &fr.inFlight);
        vkResetCommandPool(m_dev.device, fr.pool, 0);

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(fr.cmd, &bi);

        // UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL
        imageBarrier(fr.cmd, m_swapImages[imageIndex],
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        // Animated clear color so the window is visibly alive.
        float t = static_cast<float>(m_totalFrames) * 0.02f;
        VkClearValue clear{};
        clear.color = { { 0.05f + 0.05f * std::sin(t),
                          0.10f + 0.10f * std::sin(t * 0.7f + 1.0f),
                          0.18f + 0.10f * std::sin(t * 0.5f + 2.0f), 1.0f } };

        VkRenderingAttachmentInfo color{};
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView = m_swapViews[imageIndex];
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue = clear;

        VkRenderingInfo ri{};
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea = { {0,0}, m_extent };
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        vkCmdBeginRendering(fr.cmd, &ri);

        // (TODO D2+: bind pipeline + draw here)

        fc.frameIndex = m_frameIdx;
        fc.cmd = reinterpret_cast<uint64_t>(fr.cmd);
        fc.backbuffer = imageIndex;
        fc.valid = true;
        return fc;
    }

    void endFrame(const FrameContext& fc) override {
        if (!fc.valid) return;
        auto& fr = m_frames[m_frameIdx];
        uint32_t imageIndex = fc.backbuffer;

        vkCmdEndRendering(fr.cmd);

        // COLOR_ATTACHMENT_OPTIMAL -> PRESENT_SRC
        imageBarrier(fr.cmd, m_swapImages[imageIndex],
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);

        vkEndCommandBuffer(fr.cmd);

        VkSemaphoreSubmitInfo waitS{};
        waitS.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        waitS.semaphore = fr.imageAvailable;
        waitS.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSemaphoreSubmitInfo signalS{};
        signalS.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signalS.semaphore = m_renderFinished[imageIndex];
        signalS.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;

        VkCommandBufferSubmitInfo cmdS{};
        cmdS.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmdS.commandBuffer = fr.cmd;

        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.waitSemaphoreInfoCount = 1;   submit.pWaitSemaphoreInfos = &waitS;
        submit.commandBufferInfoCount = 1;   submit.pCommandBufferInfos = &cmdS;
        submit.signalSemaphoreInfoCount = 1; submit.pSignalSemaphoreInfos = &signalS;
        vkQueueSubmit2(m_gfxQueue, 1, &submit, fr.inFlight);

        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &m_renderFinished[imageIndex];
        present.swapchainCount = 1;
        present.pSwapchains = &m_swapchain;
        present.pImageIndices = &imageIndex;
        VkResult pr = vkQueuePresentKHR(m_gfxQueue, &present);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) m_needsRecreate = true;

        m_frameIdx = (m_frameIdx + 1) % kFramesInFlight;
        ++m_totalFrames;
    }

    bool supportsDescriptorIndexing() const override { return m_descriptorIndexing; }
    bool supportsMeshShaders() const override { return false; }

private:
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
    };

    void imageBarrier(VkCommandBuffer cmd, VkImage img,
                      VkImageLayout oldL, VkImageLayout newL,
                      VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                      VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
        VkImageMemoryBarrier2 b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = srcStage; b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
        b.oldLayout = oldL; b.newLayout = newL;
        b.image = img;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    bool createSwapchain(uint32_t w, uint32_t h) {
        vkb::SwapchainBuilder scb{ m_dev };
        scb.set_desired_format(VkSurfaceFormatKHR{ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
           .set_desired_present_mode(m_vsync ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_MAILBOX_KHR)
           .set_desired_extent(w, h)
           .add_image_usage_flags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        if (m_swapchain != VK_NULL_HANDLE) scb.set_old_swapchain(m_swapchain);
        auto ret = scb.build();
        if (!ret) { logError(std::string("[rhi] swapchain: ") + ret.error().message()); return false; }
        vkb::Swapchain sc = ret.value();

        // destroy old views/swapchain after building the new one
        destroySwapchain();

        m_swapchain = sc.swapchain;
        m_extent = sc.extent;
        m_format = sc.image_format;
        m_swapImages = sc.get_images().value();
        m_swapViews  = sc.get_image_views().value();

        // (re)create per-image renderFinished semaphores
        m_renderFinished.resize(m_swapImages.size());
        VkSemaphoreCreateInfo si{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        for (auto& s : m_renderFinished) vkCreateSemaphore(m_dev.device, &si, nullptr, &s);
        return true;
    }

    void destroySwapchain() {
        for (auto v : m_swapViews) if (v) vkDestroyImageView(m_dev.device, v, nullptr);
        m_swapViews.clear();
        for (auto s : m_renderFinished) if (s) vkDestroySemaphore(m_dev.device, s, nullptr);
        m_renderFinished.clear();
        if (m_swapchain) { vkDestroySwapchainKHR(m_dev.device, m_swapchain, nullptr); m_swapchain = VK_NULL_HANDLE; }
    }

    void recreateSwapchain() {
        vkDeviceWaitIdle(m_dev.device);
        createSwapchain(m_width, m_height);
    }

    bool createPerFrame() {
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = m_gfxFamily;
        VkSemaphoreCreateInfo si{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkFenceCreateInfo fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            auto& fr = m_frames[i];
            if (vkCreateCommandPool(m_dev.device, &pci, nullptr, &fr.pool) != VK_SUCCESS) return false;
            VkCommandBufferAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            ai.commandPool = fr.pool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(m_dev.device, &ai, &fr.cmd) != VK_SUCCESS) return false;
            vkCreateSemaphore(m_dev.device, &si, nullptr, &fr.imageAvailable);
            vkCreateFence(m_dev.device, &fi, nullptr, &fr.inFlight);
        }
        return true;
    }

    void destroyPerFrame() {
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            auto& fr = m_frames[i];
            if (fr.inFlight) vkDestroyFence(m_dev.device, fr.inFlight, nullptr);
            if (fr.imageAvailable) vkDestroySemaphore(m_dev.device, fr.imageAvailable, nullptr);
            if (fr.pool) vkDestroyCommandPool(m_dev.device, fr.pool, nullptr);
            fr = Frame{};
        }
    }

    // Core objects
    vkb::Instance m_inst{};
    vkb::Device   m_dev{};
    VkSurfaceKHR  m_surface = VK_NULL_HANDLE;
    VkQueue       m_gfxQueue = VK_NULL_HANDLE;
    uint32_t      m_gfxFamily = 0;
    bool          m_descriptorIndexing = false;

    // Swapchain
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkExtent2D     m_extent{};
    VkFormat       m_format = VK_FORMAT_B8G8R8A8_UNORM;
    std::vector<VkImage>     m_swapImages;
    std::vector<VkImageView> m_swapViews;
    std::vector<VkSemaphore> m_renderFinished; // per swapchain image

    // Per-frame-in-flight
    Frame    m_frames[kFramesInFlight];
    uint32_t m_frameIdx = 0;
    uint64_t m_totalFrames = 0;

    bool m_vsync = true;
    bool m_needsRecreate = false;
    uint32_t m_width = 0, m_height = 0;
};

} // namespace

IRenderDevice* createRenderDevice() { return new VulkanRenderDevice(); }

} // namespace x3::rhi
