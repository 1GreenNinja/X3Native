// Vulkan implementation of IRenderDevice — D1 (clean-room).
// Spec: specs/D1-render-device.spec.md
//
// IMPLEMENTED: instance + Win32 surface + device (vk-bootstrap), swapchain,
// per-frame command buffers + sync, dynamic-rendering clear-to-color, present,
// resize/out-of-date recreation. Validation-clean on RTX A2000 (Vulkan 1.3).
// S1: generic mesh/texture/draw API — staging-uploaded device-local vertex/index
// buffers, sampled textures (sRGB/UNORM), a per-material combined-image-sampler
// descriptor + per-draw factor UBO ring, and a textured-lit pipeline. Bindless /
// multidraw-indirect are deliberately deferred to subsystem D (see roadmap).
//
// Push-constant decision: the guaranteed push range is 128 bytes, exactly two
// mat4 (mvp + model). baseColorFactor (vec4) will not fit, so it rides in a
// per-frame UBO ring (set0/binding1) sub-allocated per draw; the texture is
// set0/binding0. Descriptors are written into a per-frame pool reset each frame.
//
// This file is the ONLY place Vulkan headers are included — IRenderDevice.h
// stays graphics-API-free.

#include "IRenderDevice.h"
#include "../core/x3_log.h"

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>

#include <vector>
#include <string>
#include <cmath>
#include <fstream>
#include <cstring>
#include <cstddef>
#include <algorithm>
#include <unordered_map>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

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

        // VMA allocator (needed by the swapchain's depth image + graphics buffers)
        VmaAllocatorCreateInfo aci{};
        aci.physicalDevice = m_dev.physical_device;
        aci.device = m_dev.device;
        aci.instance = m_inst.instance;
        aci.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        aci.vulkanApiVersion = VK_API_VERSION_1_3;
        if (vmaCreateAllocator(&aci, &m_alloc) != VK_SUCCESS) { logError("[rhi] VMA create failed"); return false; }

        if (!createSwapchain(m_width, m_height)) return false;
        if (!createPerFrame()) return false;
        if (!createGraphics()) return false;
        return true;
    }

    void shutdown() override {
        if (m_dev.device) vkDeviceWaitIdle(m_dev.device);
        destroyGraphics();
        destroyPerFrame();
        destroySwapchain();
        if (m_alloc)         { vmaDestroyAllocator(m_alloc); m_alloc = nullptr; }
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

    void setCamera(float x, float y, float z, float yaw, float pitch, float fovDeg) override {
        m_camPos = glm::vec3(x, y, z);
        m_camYaw = yaw; m_camPitch = pitch; m_camFov = fovDeg;
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

        // This frame's GPU work has retired (we waited on inFlight): it's safe to
        // recycle its descriptor sets and reuse the factor-UBO ring from the top.
        vkResetDescriptorPool(m_dev.device, fr.descPool, 0);
        fr.uboUsed = 0;

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(fr.cmd, &bi);

        // UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL
        imageBarrier(fr.cmd, m_swapImages[imageIndex],
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        // UNDEFINED -> DEPTH_ATTACHMENT_OPTIMAL (contents cleared, so UNDEFINED src is fine)
        depthBarrier(fr.cmd, m_depthImg,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

        VkClearValue clear{};
        clear.color = { { 0.04f, 0.05f, 0.08f, 1.0f } }; // dark slate backdrop

        VkRenderingAttachmentInfo color{};
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView = m_swapViews[imageIndex];
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue = clear;

        VkRenderingAttachmentInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth.imageView = m_depthView;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil = { 1.0f, 0 };

        VkRenderingInfo ri{};
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea = { {0,0}, m_extent };
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(fr.cmd, &ri);

        // Viewport + scissor for the whole frame; the app issues drawMesh() calls
        // between beginFrame/endFrame (each binds the mesh pipeline + descriptors).
        VkViewport vp{ 0.0f, 0.0f, (float)m_extent.width, (float)m_extent.height, 0.0f, 1.0f };
        VkRect2D scis{ {0,0}, m_extent };
        vkCmdSetViewport(fr.cmd, 0, 1, &vp);
        vkCmdSetScissor(fr.cmd, 0, 1, &scis);

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

    // ---- Mesh / texture resource API (S1) ----------------------------------
    MeshHandle createMesh(const MeshVertex* verts, uint32_t vcount,
                          const uint32_t* idx, uint32_t icount) override {
        if (!verts || vcount == 0 || !idx || icount == 0) return {};
        Mesh m{};
        const VkDeviceSize vbBytes = (VkDeviceSize)vcount * sizeof(MeshVertex);
        const VkDeviceSize ibBytes = (VkDeviceSize)icount * sizeof(uint32_t);
        if (!createDeviceLocalBuffer(verts, vbBytes,
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, m.vbo, m.vboAlloc)) return {};
        if (!createDeviceLocalBuffer(idx, ibBytes,
                VK_BUFFER_USAGE_INDEX_BUFFER_BIT, m.ibo, m.iboAlloc)) {
            vmaDestroyBuffer(m_alloc, m.vbo, m.vboAlloc); return {};
        }
        m.indexCount = icount;
        uint32_t id = m_nextMeshId++;
        m_meshes.emplace(id, m);
        return { id };
    }

    void destroyMesh(MeshHandle h) override {
        auto it = m_meshes.find(h.id);
        if (it == m_meshes.end()) return;
        if (m_dev.device) vkDeviceWaitIdle(m_dev.device);
        vmaDestroyBuffer(m_alloc, it->second.vbo, it->second.vboAlloc);
        vmaDestroyBuffer(m_alloc, it->second.ibo, it->second.iboAlloc);
        m_meshes.erase(it);
    }

    TextureHandle createTexture(const void* rgba8, uint32_t w, uint32_t h, bool srgb) override {
        if (!rgba8 || w == 0 || h == 0) return {};
        Texture t{};
        if (!createSampledTexture(rgba8, w, h, srgb, t)) return {};
        uint32_t id = m_nextTexId++;
        m_textures.emplace(id, t);
        return { id };
    }

    void destroyTexture(TextureHandle h) override {
        auto it = m_textures.find(h.id);
        if (it == m_textures.end()) return;
        if (m_dev.device) vkDeviceWaitIdle(m_dev.device);
        destroyTextureObj(it->second);
        m_textures.erase(it);
    }

    void drawMesh(const FrameContext& fc, MeshHandle mesh, TextureHandle baseColor,
                  const float baseColorFactor[4], const float model[16]) override {
        if (!fc.valid || !m_meshPipeline) return;
        auto mit = m_meshes.find(mesh.id);
        if (mit == m_meshes.end()) return;
        const Mesh& mh = mit->second;

        // Pick texture: requested base color, else built-in 1x1 white default.
        const Texture* tex = &m_whiteTex;
        if (baseColor.valid()) {
            auto tit = m_textures.find(baseColor.id);
            if (tit != m_textures.end()) tex = &tit->second;
        }

        auto& fr = m_frames[m_frameIdx];
        VkCommandBuffer cmd = fr.cmd;

        // Per-draw factor UBO (sub-allocated from this frame's ring).
        glm::vec4 factor = baseColorFactor
            ? glm::vec4(baseColorFactor[0], baseColorFactor[1], baseColorFactor[2], baseColorFactor[3])
            : glm::vec4(1.0f);
        if (fr.uboUsed >= kMaxDrawsPerFrame) return; // ring exhausted; skip safely
        uint32_t slot = fr.uboUsed++;
        VkDeviceSize uboOffset = (VkDeviceSize)slot * m_uboStride;
        std::memcpy(static_cast<char*>(fr.uboMapped) + uboOffset, &factor, sizeof(glm::vec4));

        // Allocate + write a descriptor set for this draw from the frame pool.
        VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dsai.descriptorPool = fr.descPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &m_meshSetLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(m_dev.device, &dsai, &set) != VK_SUCCESS) return;

        VkDescriptorImageInfo dii{};
        dii.sampler = tex->sampler;
        dii.imageView = tex->view;
        dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorBufferInfo dbi{};
        dbi.buffer = fr.uboBuf;
        dbi.offset = uboOffset;
        dbi.range = sizeof(glm::vec4);

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = set; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &dii;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = set; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].pBufferInfo = &dbi;
        vkUpdateDescriptorSets(m_dev.device, 2, writes, 0, nullptr);

        // mvp = proj * view * model (camera state + current aspect).
        float aspect = (float)m_extent.width / (float)std::max(1u, m_extent.height);
        glm::vec3 fwd(std::cos(m_camPitch) * std::cos(m_camYaw),
                      std::sin(m_camPitch),
                      std::cos(m_camPitch) * std::sin(m_camYaw));
        glm::mat4 mdl = glm::make_mat4(model);
        glm::mat4 view = glm::lookAt(m_camPos, m_camPos + fwd, glm::vec3(0, 1, 0));
        glm::mat4 proj = glm::perspective(glm::radians(m_camFov), aspect, 0.1f, 200.0f);
        proj[1][1] *= -1.0f;
        struct Push { glm::mat4 mvp; glm::mat4 model; } push{ proj * view * mdl, mdl };

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_meshPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_meshLayout,
                                0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, m_meshLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &mh.vbo, &off);
        vkCmdBindIndexBuffer(cmd, mh.ibo, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, mh.indexCount, 1, 0, 0, 0);
    }

private:
    struct Mesh {
        VkBuffer vbo = VK_NULL_HANDLE; VmaAllocation vboAlloc = nullptr;
        VkBuffer ibo = VK_NULL_HANDLE; VmaAllocation iboAlloc = nullptr;
        uint32_t indexCount = 0;
    };
    struct Texture {
        VkImage image = VK_NULL_HANDLE; VmaAllocation alloc = nullptr;
        VkImageView view = VK_NULL_HANDLE; VkSampler sampler = VK_NULL_HANDLE;
    };

    static constexpr uint32_t kMaxDrawsPerFrame = 256;

    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
        // Per-frame descriptor pool (reset every frame) + factor-UBO ring.
        VkDescriptorPool descPool = VK_NULL_HANDLE;
        VkBuffer      uboBuf = VK_NULL_HANDLE;
        VmaAllocation uboAlloc = nullptr;
        void*         uboMapped = nullptr;
        uint32_t      uboUsed = 0;
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

    void depthBarrier(VkCommandBuffer cmd, VkImage img,
                      VkImageLayout oldL, VkImageLayout newL,
                      VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                      VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
        VkImageMemoryBarrier2 b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = srcStage; b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
        b.oldLayout = oldL; b.newLayout = newL;
        b.image = img;
        b.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
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

        // Depth buffer sized to the swapchain
        VkImageCreateInfo dici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        dici.imageType = VK_IMAGE_TYPE_2D;
        dici.format = m_depthFormat;
        dici.extent = { m_extent.width, m_extent.height, 1 };
        dici.mipLevels = 1; dici.arrayLayers = 1;
        dici.samples = VK_SAMPLE_COUNT_1_BIT;
        dici.tiling = VK_IMAGE_TILING_OPTIMAL;
        dici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        VmaAllocationCreateInfo daci{};
        daci.usage = VMA_MEMORY_USAGE_AUTO;
        daci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        if (vmaCreateImage(m_alloc, &dici, &daci, &m_depthImg, &m_depthAlloc, nullptr) != VK_SUCCESS) {
            logError("[rhi] depth image create failed"); return false;
        }
        VkImageViewCreateInfo dvci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        dvci.image = m_depthImg; dvci.viewType = VK_IMAGE_VIEW_TYPE_2D; dvci.format = m_depthFormat;
        dvci.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(m_dev.device, &dvci, nullptr, &m_depthView) != VK_SUCCESS) {
            logError("[rhi] depth view create failed"); return false;
        }
        return true;
    }

    void destroySwapchain() {
        if (m_depthView) { vkDestroyImageView(m_dev.device, m_depthView, nullptr); m_depthView = VK_NULL_HANDLE; }
        if (m_depthImg)  { vmaDestroyImage(m_alloc, m_depthImg, m_depthAlloc); m_depthImg = VK_NULL_HANDLE; m_depthAlloc = nullptr; }
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

    // ---- Graphics: VMA + triangle pipeline (first geometry) ----
    static std::string exeDir() {
        char buf[1024]; DWORD n = GetModuleFileNameA(nullptr, buf, sizeof(buf));
        std::string p(buf, n);
        size_t slash = p.find_last_of("\\/");
        return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
    }

    VkShaderModule loadShaderModule(const std::string& relPath) {
        std::string full = exeDir() + "\\" + relPath;
        std::ifstream f(full, std::ios::binary | std::ios::ate);
        if (!f) { logError(std::string("[rhi] shader not found: ") + full); return VK_NULL_HANDLE; }
        size_t sz = (size_t)f.tellg(); f.seekg(0);
        std::vector<char> code(sz); f.read(code.data(), sz);
        VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = sz;
        ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule m = VK_NULL_HANDLE;
        if (vkCreateShaderModule(m_dev.device, &ci, nullptr, &m) != VK_SUCCESS)
            logError(std::string("[rhi] shader module create failed: ") + full);
        return m;
    }

    // One-time staging copy into a fresh DEVICE_LOCAL buffer (transient cmd + fence).
    bool createDeviceLocalBuffer(const void* data, VkDeviceSize bytes,
                                 VkBufferUsageFlags usage,
                                 VkBuffer& outBuf, VmaAllocation& outAlloc) {
        // Staging (host-visible, mapped).
        VkBufferCreateInfo sbci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        sbci.size = bytes; sbci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo svaci{};
        svaci.usage = VMA_MEMORY_USAGE_AUTO;
        svaci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer staging = VK_NULL_HANDLE; VmaAllocation stagingAlloc = nullptr; VmaAllocationInfo si{};
        if (vmaCreateBuffer(m_alloc, &sbci, &svaci, &staging, &stagingAlloc, &si) != VK_SUCCESS) return false;
        std::memcpy(si.pMappedData, data, (size_t)bytes);

        // Device-local destination.
        VkBufferCreateInfo dbci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        dbci.size = bytes; dbci.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo dvaci{};
        dvaci.usage = VMA_MEMORY_USAGE_AUTO;
        if (vmaCreateBuffer(m_alloc, &dbci, &dvaci, &outBuf, &outAlloc, nullptr) != VK_SUCCESS) {
            vmaDestroyBuffer(m_alloc, staging, stagingAlloc); return false;
        }

        bool ok = oneTimeSubmit([&](VkCommandBuffer cmd){
            VkBufferCopy region{ 0, 0, bytes };
            vkCmdCopyBuffer(cmd, staging, outBuf, 1, &region);
        });
        vmaDestroyBuffer(m_alloc, staging, stagingAlloc);
        if (!ok) { vmaDestroyBuffer(m_alloc, outBuf, outAlloc); outBuf = VK_NULL_HANDLE; outAlloc = nullptr; }
        return ok;
    }

    // Staging-upload an RGBA8 image into a single-mip sampled texture (+view+sampler).
    bool createSampledTexture(const void* rgba8, uint32_t w, uint32_t h, bool srgb, Texture& out) {
        const VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
        const VkFormat fmt = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;

        VkBufferCreateInfo sbci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        sbci.size = bytes; sbci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo svaci{};
        svaci.usage = VMA_MEMORY_USAGE_AUTO;
        svaci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer staging = VK_NULL_HANDLE; VmaAllocation stagingAlloc = nullptr; VmaAllocationInfo si{};
        if (vmaCreateBuffer(m_alloc, &sbci, &svaci, &staging, &stagingAlloc, &si) != VK_SUCCESS) return false;
        std::memcpy(si.pMappedData, rgba8, (size_t)bytes);

        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = fmt;
        ici.extent = { w, h, 1 };
        ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo ivaci{}; ivaci.usage = VMA_MEMORY_USAGE_AUTO;
        if (vmaCreateImage(m_alloc, &ici, &ivaci, &out.image, &out.alloc, nullptr) != VK_SUCCESS) {
            vmaDestroyBuffer(m_alloc, staging, stagingAlloc); return false;
        }

        bool ok = oneTimeSubmit([&](VkCommandBuffer cmd){
            // UNDEFINED -> TRANSFER_DST
            imageBarrier(cmd, out.image,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkBufferImageCopy region{};
            region.bufferOffset = 0;
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { w, h, 1 };
            vkCmdCopyBufferToImage(cmd, staging, out.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            // TRANSFER_DST -> SHADER_READ_ONLY
            imageBarrier(cmd, out.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        });
        vmaDestroyBuffer(m_alloc, staging, stagingAlloc);
        if (!ok) { vmaDestroyImage(m_alloc, out.image, out.alloc); out.image = VK_NULL_HANDLE; out.alloc = nullptr; return false; }

        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = out.image; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(m_dev.device, &vci, nullptr, &out.view) != VK_SUCCESS) {
            vmaDestroyImage(m_alloc, out.image, out.alloc); out = Texture{}; return false;
        }

        VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.maxLod = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(m_dev.device, &sci, nullptr, &out.sampler) != VK_SUCCESS) {
            vkDestroyImageView(m_dev.device, out.view, nullptr);
            vmaDestroyImage(m_alloc, out.image, out.alloc); out = Texture{}; return false;
        }
        return true;
    }

    void destroyTextureObj(Texture& t) {
        if (t.sampler) vkDestroySampler(m_dev.device, t.sampler, nullptr);
        if (t.view)    vkDestroyImageView(m_dev.device, t.view, nullptr);
        if (t.image)   vmaDestroyImage(m_alloc, t.image, t.alloc);
        t = Texture{};
    }

    // Record + submit a transient command buffer, wait on a one-shot fence.
    template <class Fn>
    bool oneTimeSubmit(Fn&& record) {
        VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = m_uploadPool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(m_dev.device, &ai, &cmd) != VK_SUCCESS) return false;
        VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        record(cmd);
        vkEndCommandBuffer(cmd);

        VkCommandBufferSubmitInfo cmdS{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
        cmdS.commandBuffer = cmd;
        VkSubmitInfo2 submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        submit.commandBufferInfoCount = 1; submit.pCommandBufferInfos = &cmdS;
        VkResult sr = vkQueueSubmit2(m_gfxQueue, 1, &submit, m_uploadFence);
        if (sr == VK_SUCCESS) {
            vkWaitForFences(m_dev.device, 1, &m_uploadFence, VK_TRUE, UINT64_MAX);
            vkResetFences(m_dev.device, 1, &m_uploadFence);
        }
        vkFreeCommandBuffers(m_dev.device, m_uploadPool, 1, &cmd);
        return sr == VK_SUCCESS;
    }

    bool createGraphics() {
        // ---- Upload primitives: transient command pool + fence for staging copies ----
        VkCommandPoolCreateInfo upci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        upci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        upci.queueFamilyIndex = m_gfxFamily;
        if (vkCreateCommandPool(m_dev.device, &upci, nullptr, &m_uploadPool) != VK_SUCCESS) {
            logError("[rhi] upload pool create failed"); return false;
        }
        VkFenceCreateInfo ufi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (vkCreateFence(m_dev.device, &ufi, nullptr, &m_uploadFence) != VK_SUCCESS) {
            logError("[rhi] upload fence create failed"); return false;
        }

        // ---- Per-material descriptor set layout: combined-image-sampler + factor UBO ----
        VkDescriptorSetLayoutBinding binds[2]{};
        binds[0].binding = 0; binds[0].descriptorCount = 1;
        binds[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binds[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        binds[1].binding = 1; binds[1].descriptorCount = 1;
        binds[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        binds[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        slci.bindingCount = 2; slci.pBindings = binds;
        if (vkCreateDescriptorSetLayout(m_dev.device, &slci, nullptr, &m_meshSetLayout) != VK_SUCCESS) {
            logError("[rhi] descriptor set layout failed"); return false;
        }

        // ---- Per-frame descriptor pools + factor-UBO rings ----
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_dev.physical_device, &props);
        VkDeviceSize minAlign = props.limits.minUniformBufferOffsetAlignment;
        m_uboStride = sizeof(glm::vec4);
        if (minAlign > 0) m_uboStride = (m_uboStride + minAlign - 1) & ~(minAlign - 1);

        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            auto& fr = m_frames[i];
            VkDescriptorPoolSize sizes[2]{};
            sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sizes[0].descriptorCount = kMaxDrawsPerFrame;
            sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;         sizes[1].descriptorCount = kMaxDrawsPerFrame;
            VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.maxSets = kMaxDrawsPerFrame; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
            if (vkCreateDescriptorPool(m_dev.device, &pci, nullptr, &fr.descPool) != VK_SUCCESS) {
                logError("[rhi] descriptor pool failed"); return false;
            }
            VkBufferCreateInfo ubci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            ubci.size = m_uboStride * kMaxDrawsPerFrame;
            ubci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo uvaci{};
            uvaci.usage = VMA_MEMORY_USAGE_AUTO;
            uvaci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo uinfo{};
            if (vmaCreateBuffer(m_alloc, &ubci, &uvaci, &fr.uboBuf, &fr.uboAlloc, &uinfo) != VK_SUCCESS) {
                logError("[rhi] factor UBO ring create failed"); return false;
            }
            fr.uboMapped = uinfo.pMappedData;
        }

        // ---- Built-in 1x1 white default texture (sRGB) ----
        const uint8_t white[4] = { 255, 255, 255, 255 };
        if (!createSampledTexture(white, 1, 1, true, m_whiteTex)) {
            logError("[rhi] default white texture create failed"); return false;
        }

        // ---- Mesh pipeline (pos/normal/uv + texture + factor + depth + lighting) ----
        VkShaderModule vs = loadShaderModule("shaders\\mesh.vert.spv");
        VkShaderModule fs = loadShaderModule("shaders\\mesh.frag.spv");
        if (!vs || !fs) return false;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vs; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

        VkVertexInputBindingDescription bind{ 0, sizeof(MeshVertex), VK_VERTEX_INPUT_RATE_VERTEX };
        VkVertexInputAttributeDescription attrs[3]{
            { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, pos)    },
            { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, normal) },
            { 2, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(MeshVertex, uv)     },
        };
        VkPipelineVertexInputStateCreateInfo vin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &bind;
        vin.vertexAttributeDescriptionCount = 3; vin.pVertexAttributeDescriptions = attrs;

        VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = 1; vp.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_BACK_BIT;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo dss{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        dss.depthTestEnable = VK_TRUE; dss.depthWriteEnable = VK_TRUE;
        dss.depthCompareOp = VK_COMPARE_OP_LESS;

        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 1; cb.pAttachments = &cba;

        VkDynamicState dyn[2]{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;

        // Push range = 2 mat4 = 128 bytes (vertex stage). baseColorFactor is in the UBO.
        VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT, 0, 2 * sizeof(glm::mat4) };
        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount = 1; plci.pSetLayouts = &m_meshSetLayout;
        plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
        if (vkCreatePipelineLayout(m_dev.device, &plci, nullptr, &m_meshLayout) != VK_SUCCESS) {
            logError("[rhi] pipeline layout failed"); return false;
        }

        VkPipelineRenderingCreateInfo prci{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        prci.colorAttachmentCount = 1; prci.pColorAttachmentFormats = &m_format;
        prci.depthAttachmentFormat = m_depthFormat;

        VkGraphicsPipelineCreateInfo gpci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gpci.pNext = &prci;
        gpci.stageCount = 2; gpci.pStages = stages;
        gpci.pVertexInputState = &vin; gpci.pInputAssemblyState = &ia;
        gpci.pViewportState = &vp; gpci.pRasterizationState = &rs;
        gpci.pMultisampleState = &ms; gpci.pDepthStencilState = &dss;
        gpci.pColorBlendState = &cb; gpci.pDynamicState = &ds; gpci.layout = m_meshLayout;
        VkResult pr = vkCreateGraphicsPipelines(m_dev.device, VK_NULL_HANDLE, 1, &gpci, nullptr, &m_meshPipeline);

        vkDestroyShaderModule(m_dev.device, vs, nullptr);
        vkDestroyShaderModule(m_dev.device, fs, nullptr);
        if (pr != VK_SUCCESS) { logError("[rhi] graphics pipeline create failed"); return false; }

        logInfo("[rhi] mesh pipeline ready (textured + factor + depth + lighting)");
        return true;
    }

    void destroyGraphics() {
        // Mesh + texture registries (created by the app via the public API).
        for (auto& kv : m_meshes) {
            vmaDestroyBuffer(m_alloc, kv.second.vbo, kv.second.vboAlloc);
            vmaDestroyBuffer(m_alloc, kv.second.ibo, kv.second.iboAlloc);
        }
        m_meshes.clear();
        for (auto& kv : m_textures) destroyTextureObj(kv.second);
        m_textures.clear();
        destroyTextureObj(m_whiteTex);

        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            auto& fr = m_frames[i];
            if (fr.uboBuf)   { vmaDestroyBuffer(m_alloc, fr.uboBuf, fr.uboAlloc); fr.uboBuf = VK_NULL_HANDLE; fr.uboAlloc = nullptr; fr.uboMapped = nullptr; }
            if (fr.descPool) { vkDestroyDescriptorPool(m_dev.device, fr.descPool, nullptr); fr.descPool = VK_NULL_HANDLE; }
        }

        if (m_meshPipeline)  vkDestroyPipeline(m_dev.device, m_meshPipeline, nullptr);
        if (m_meshLayout)    vkDestroyPipelineLayout(m_dev.device, m_meshLayout, nullptr);
        if (m_meshSetLayout) vkDestroyDescriptorSetLayout(m_dev.device, m_meshSetLayout, nullptr);
        if (m_uploadFence)   vkDestroyFence(m_dev.device, m_uploadFence, nullptr);
        if (m_uploadPool)    vkDestroyCommandPool(m_dev.device, m_uploadPool, nullptr);
        m_meshPipeline = VK_NULL_HANDLE; m_meshLayout = VK_NULL_HANDLE;
        m_meshSetLayout = VK_NULL_HANDLE; m_uploadFence = VK_NULL_HANDLE; m_uploadPool = VK_NULL_HANDLE;
    }

    // Core objects
    vkb::Instance m_inst{};
    vkb::Device   m_dev{};
    VkSurfaceKHR  m_surface = VK_NULL_HANDLE;
    VkQueue       m_gfxQueue = VK_NULL_HANDLE;
    uint32_t      m_gfxFamily = 0;
    bool          m_descriptorIndexing = false;

    // Graphics
    VmaAllocator  m_alloc = nullptr;
    VkPipeline       m_meshPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_meshLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_meshSetLayout = VK_NULL_HANDLE;

    // One-time staging upload (transient pool + fence)
    VkCommandPool m_uploadPool = VK_NULL_HANDLE;
    VkFence       m_uploadFence = VK_NULL_HANDLE;

    // Per-draw factor-UBO alignment stride (>= sizeof(vec4), aligned to device min)
    VkDeviceSize  m_uboStride = 0;

    // Resource registries (created via the public mesh/texture API)
    std::unordered_map<uint32_t, Mesh>    m_meshes;
    std::unordered_map<uint32_t, Texture> m_textures;
    uint32_t m_nextMeshId = 1;
    uint32_t m_nextTexId  = 1;
    Texture  m_whiteTex{};   // built-in 1x1 white default

    // Depth (sized to swapchain)
    VkFormat      m_depthFormat = VK_FORMAT_D32_SFLOAT;
    VkImage       m_depthImg = VK_NULL_HANDLE;
    VkImageView   m_depthView = VK_NULL_HANDLE;
    VmaAllocation m_depthAlloc = nullptr;

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

    // Camera (FPS); defaults frame the cube at origin
    glm::vec3 m_camPos{ 0.0f, 1.5f, 4.0f };
    float m_camYaw = -1.5708f;   // look toward -Z
    float m_camPitch = -0.30f;   // slightly down
    float m_camFov = 60.0f;
};

} // namespace

IRenderDevice* createRenderDevice() { return new VulkanRenderDevice(); }

} // namespace x3::rhi
