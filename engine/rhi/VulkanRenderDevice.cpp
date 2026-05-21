// Vulkan implementation of IRenderDevice — D1 (clean-room).
// Spec: specs/D1-render-device.spec.md
//
// SKELETON STATUS (Option-A blind scaffold from the A2000 laptop):
//   DONE  : instance + Win32 surface + physical device + logical device via
//           vk-bootstrap. This proves the toolchain end-to-end (SDK found,
//           vk-bootstrap links, validation layers load, a real GPU selected
//           at Vulkan 1.3).
//   TODO  : swapchain bring-up, per-frame acquire/clear/present (dynamic
//           rendering), VMA allocator, the 5 acceptance tests. ← 13700K work.
//
// This file is the ONLY place Vulkan headers are included — IRenderDevice.h
// stays graphics-API-free.

#include "IRenderDevice.h"
#include "../core/x3_log.h"

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>
#include <VkBootstrap.h>

#include <string>

namespace x3::rhi {

namespace {

class VulkanRenderDevice final : public IRenderDevice {
public:
    bool init(const DeviceDesc& desc) override {
        // ---- Instance ----
        vkb::InstanceBuilder ib;
        auto inst_ret = ib.set_app_name("X3Native")
                          .set_engine_name("X3Native")
                          .require_api_version(1, 3, 0)
                          .request_validation_layers(desc.validation)
                          .use_default_debug_messenger()
                          .build();
        if (!inst_ret) {
            logError(std::string("[rhi] instance build failed: ") + inst_ret.error().message());
            return false;
        }
        m_inst = inst_ret.value();

        // ---- Win32 surface ----
        if (!desc.nativeWindowHandle) {
            logError("[rhi] init: nativeWindowHandle is null (need an HWND)");
            return false;
        }
        VkWin32SurfaceCreateInfoKHR sci{};
        sci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        sci.hinstance = ::GetModuleHandle(nullptr);
        sci.hwnd = static_cast<HWND>(desc.nativeWindowHandle);
        if (vkCreateWin32SurfaceKHR(m_inst.instance, &sci, nullptr, &m_surface) != VK_SUCCESS) {
            logError("[rhi] vkCreateWin32SurfaceKHR failed");
            return false;
        }

        // ---- Physical device (Vulkan 1.3, with the features we want enabled) ----
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
        auto phys_ret = sel.set_surface(m_surface)
                           .set_minimum_version(1, 3)
                           .set_required_features_13(f13)
                           .set_required_features_12(f12)
                           .select();
        if (!phys_ret) {
            logError(std::string("[rhi] physical device select failed: ") + phys_ret.error().message());
            return false;
        }
        vkb::PhysicalDevice phys = phys_ret.value();
        m_descriptorIndexing = true; // required above

        // ---- Logical device ----
        vkb::DeviceBuilder db{ phys };
        auto dev_ret = db.build();
        if (!dev_ret) {
            logError(std::string("[rhi] device build failed: ") + dev_ret.error().message());
            return false;
        }
        m_dev = dev_ret.value();

        logInfo(std::string("[rhi] device ready: ") + phys.name +
                " (Vulkan 1.3, dynamic-rendering + sync2 + descriptor-indexing)");

        // TODO(13700K): create swapchain (vkb::SwapchainBuilder), per-frame
        // command pools/buffers, sync (timeline semaphores), VMA allocator.
        (void)desc.width; (void)desc.height; (void)desc.vsync;
        return true;
    }

    void shutdown() override {
        // TODO(13700K): destroy swapchain + per-frame resources + VMA first.
        if (m_dev.device)      vkb::destroy_device(m_dev);
        if (m_surface)         vkDestroySurfaceKHR(m_inst.instance, m_surface, nullptr);
        if (m_inst.instance)   vkb::destroy_instance(m_inst);
        m_surface = VK_NULL_HANDLE;
    }

    void onResize(uint32_t w, uint32_t h) override {
        // TODO(13700K): wait-idle, recreate swapchain.
        (void)w; (void)h;
    }

    FrameContext beginFrame() override {
        // TODO(13700K): acquire image, begin command buffer, begin dynamic rendering.
        return FrameContext{ /*frameIndex*/0, /*cmd*/0, /*backbuffer*/0, /*valid*/false };
    }

    void endFrame(const FrameContext&) override {
        // TODO(13700K): end rendering, submit (timeline sem), present.
    }

    bool supportsDescriptorIndexing() const override { return m_descriptorIndexing; }
    bool supportsMeshShaders() const override { return false; } // TODO: query VK_EXT_mesh_shader

private:
    vkb::Instance m_inst{};
    vkb::Device   m_dev{};
    VkSurfaceKHR  m_surface = VK_NULL_HANDLE;
    bool          m_descriptorIndexing = false;
};

} // namespace

IRenderDevice* createRenderDevice() {
    // Clean impl. (When the GPL scaffold is wired, a factory switch on
    // X3_USE_GPL_SCAFFOLD would return the v0 impl instead — see GPL_DEBT.md D1.)
    return new VulkanRenderDevice();
}

} // namespace x3::rhi
