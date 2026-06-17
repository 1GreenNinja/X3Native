// #28 monolith split — VulkanRenderDevice swapchain / framebuffer targets + HUD pipeline (out-of-line).
// Swapchain + offscreen + bloom + scene-copy + glass + post color targets and their
// (re)create/destroy + descriptor writers, plus the 2D HUD font-atlas + HUD pipeline.
// Boot-time setup only; no per-pixel lighting math. Bodies moved verbatim.
#include "VulkanRenderDevice_internal.h"
namespace x3::rhi {
bool VulkanRenderDevice::createSwapchain(uint32_t w, uint32_t h) {
        vkb::SwapchainBuilder scb{ m_dev };
        scb.set_desired_format(VkSurfaceFormatKHR{ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
           .set_desired_present_mode(m_vsync ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_MAILBOX_KHR)
           .set_desired_extent(w, h)
           // TRANSFER_SRC so captureFrame() can vkCmdCopyImageToBuffer the
           // presented color image to a host-visible readback buffer (--screenshot).
           .add_image_usage_flags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
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
        // SAMPLED so the SSAO pass can read view-space depth from this buffer (the
        // depth pre-pass writes it; SSAO reconstructs view position + normals).
        dici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                   | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;  // GI snapshots depth for temporal reproject
        VmaAllocationCreateInfo daci{};
        daci.usage = VMA_MEMORY_USAGE_AUTO;
        daci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        if (x3vmaCreateImage(&dici, &daci, &m_depthImg, &m_depthAlloc, nullptr) != VK_SUCCESS) {
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

void VulkanRenderDevice::destroySwapchain() {
        if (m_depthView) { vkDestroyImageView(m_dev.device, m_depthView, nullptr); m_depthView = VK_NULL_HANDLE; }
        if (m_depthImg)  { vmaDestroyImage(m_alloc, m_depthImg, m_depthAlloc); m_depthImg = VK_NULL_HANDLE; m_depthAlloc = nullptr; }
        for (auto v : m_swapViews) if (v) vkDestroyImageView(m_dev.device, v, nullptr);
        m_swapViews.clear();
        for (auto s : m_renderFinished) if (s) vkDestroySemaphore(m_dev.device, s, nullptr);
        m_renderFinished.clear();
        if (m_swapchain) { vkDestroySwapchainKHR(m_dev.device, m_swapchain, nullptr); m_swapchain = VK_NULL_HANDLE; }
    }

void VulkanRenderDevice::recreateSwapchain() {
        // ZERO-STUTTER: declared recreate boundary (resize/vsync). Extent-tracking
        // targets are reallocated here — exempt from the strict late-create assert,
        // flagged for the spike log. No PIPELINES are created (dynamic rendering;
        // all formats are extent-independent), only images/views/descriptors.
        m_creationBoundary = true;
        m_recreatedThisFrame = true;
        vkDeviceWaitIdle(m_dev.device);
        createSwapchain(m_width, m_height);
        // HDR scene + bloom mips track the frame extent — rebuild + rewrite their
        // descriptor sets after the swapchain (and m_extent) are updated.
        createBloomTargets();
        writePostDescriptors();
        // Glass set 4 references the scene-copy view (recreated above) — rewrite it.
        writeGlassDescriptors();
        // SSAO half-res targets track the extent; the depth view also changed, so
        // rewrite every SSAO descriptor that references depth/AO views.
        createSsaoTargets();
        // Reflections (if built): the target tracks the extent and the depth + TAA
        // history views changed — recreate + rewrite BEFORE writeSsaoDescriptors so
        // mesh set3 binding2 picks up the NEW refl view (not the destroyed one).
        if (m_reflBuilt) {
            if (!createReflTargets()) { destroyRefl(); m_refl.ssr = false; }
            else writeReflDescriptors();
        }
        writeSsaoDescriptors();
        // GI half-res targets + prev-depth track the extent; the depth/AO/scene
        // views changed, so rebuild + rewrite. History is invalid after a resize.
        createGiTargets();
        writeGiDescriptors();
        m_giHistoryValid = false;
        // Water samples the scene depth: the depth view changed -> rewire it.
        writeWaterDescriptors();
        // Particles sample the scene depth (soft fade): rewire on the new depth view.
        writeParticleDescriptors();
        // HZB pyramid tracks the extent + samples the (new) depth view.
        if (m_gpuCullReady) createHzbTargets();
        // RT AO (if built): half-res target tracks the extent + the depth view
        // changed, so recreate the target + rewrite all RT-AO descriptors.
        if (m_rtaoBuilt) { createRtaoTargets(); writeRtaoDescriptors(); }
        // Editor UI (if active): tell ImGui the new swapchain image count. With
        // dynamic rendering ImGui holds no per-image framebuffers and m_format is
        // stable, so no font/pipeline rebuild is needed.
        if (m_imguiInit)
            ImGui_ImplVulkan_SetMinImageCount(
                static_cast<uint32_t>(m_swapImages.empty() ? 2u : m_swapImages.size()));
        m_creationBoundary = false;
    }

bool VulkanRenderDevice::createOffscreenTarget(uint32_t w, uint32_t h) {
        if (w == 0 || h == 0) { logError("[rhi] offscreen: zero extent"); return false; }
        // Destroy any prior target first (recreate path); on first create these are
        // all null and destroy is a no-op.
        destroyOffscreenTarget();

        m_extent = { w, h };
        m_format = VK_FORMAT_B8G8R8A8_UNORM;   // match the windowed swapchain format

        // Offscreen color image: COLOR_ATTACHMENT (graph target) + TRANSFER_SRC
        // (vkCmdCopyImageToBuffer for --screenshot), single sample, optimal tiling.
        VkImageCreateInfo cici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        cici.imageType = VK_IMAGE_TYPE_2D;
        cici.format = m_format;
        cici.extent = { w, h, 1 };
        cici.mipLevels = 1; cici.arrayLayers = 1;
        cici.samples = VK_SAMPLE_COUNT_1_BIT;
        cici.tiling = VK_IMAGE_TILING_OPTIMAL;
        cici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        cici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo caci{};
        caci.usage = VMA_MEMORY_USAGE_AUTO;
        caci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        if (x3vmaCreateImage(&cici, &caci, &m_offscreenColorImg, &m_offscreenColorAlloc, nullptr) != VK_SUCCESS) {
            logError("[rhi] offscreen color image create failed"); return false;
        }
        VkImageViewCreateInfo cvci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        cvci.image = m_offscreenColorImg; cvci.viewType = VK_IMAGE_VIEW_TYPE_2D; cvci.format = m_format;
        cvci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(m_dev.device, &cvci, nullptr, &m_offscreenColorView) != VK_SUCCESS) {
            logError("[rhi] offscreen color view create failed"); return false;
        }

        // Depth buffer sized to the offscreen color (identical to createSwapchain).
        VkImageCreateInfo dici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        dici.imageType = VK_IMAGE_TYPE_2D;
        dici.format = m_depthFormat;
        dici.extent = { w, h, 1 };
        dici.mipLevels = 1; dici.arrayLayers = 1;
        dici.samples = VK_SAMPLE_COUNT_1_BIT;
        dici.tiling = VK_IMAGE_TILING_OPTIMAL;
        // SAMPLED so the SSAO pass can read view-space depth from this buffer (the
        // depth pre-pass writes it; SSAO reconstructs view position + normals).
        dici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                   | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;  // GI snapshots depth for temporal reproject
        VmaAllocationCreateInfo daci{};
        daci.usage = VMA_MEMORY_USAGE_AUTO;
        daci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        if (x3vmaCreateImage(&dici, &daci, &m_depthImg, &m_depthAlloc, nullptr) != VK_SUCCESS) {
            logError("[rhi] offscreen depth image create failed"); return false;
        }
        VkImageViewCreateInfo dvci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        dvci.image = m_depthImg; dvci.viewType = VK_IMAGE_VIEW_TYPE_2D; dvci.format = m_depthFormat;
        dvci.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(m_dev.device, &dvci, nullptr, &m_depthView) != VK_SUCCESS) {
            logError("[rhi] offscreen depth view create failed"); return false;
        }
        return true;
    }

void VulkanRenderDevice::destroyOffscreenTarget() {
        if (m_depthView) { vkDestroyImageView(m_dev.device, m_depthView, nullptr); m_depthView = VK_NULL_HANDLE; }
        if (m_depthImg)  { vmaDestroyImage(m_alloc, m_depthImg, m_depthAlloc); m_depthImg = VK_NULL_HANDLE; m_depthAlloc = nullptr; }
        if (m_offscreenColorView) { vkDestroyImageView(m_dev.device, m_offscreenColorView, nullptr); m_offscreenColorView = VK_NULL_HANDLE; }
        if (m_offscreenColorImg)  { vmaDestroyImage(m_alloc, m_offscreenColorImg, m_offscreenColorAlloc); m_offscreenColorImg = VK_NULL_HANDLE; m_offscreenColorAlloc = nullptr; }
    }

void VulkanRenderDevice::recreateOffscreenTarget() {
        // ZERO-STUTTER: declared recreate boundary (headless resize) — see
        // recreateSwapchain(). Images/views/descriptors only; no pipelines.
        m_creationBoundary = true;
        m_recreatedThisFrame = true;
        vkDeviceWaitIdle(m_dev.device);
        createOffscreenTarget(m_width, m_height);
        // The HDR scene + bloom mips are sized to the frame extent — rebuild them
        // (and rewrite the post descriptor sets) so they track the new size.
        createBloomTargets();
        writePostDescriptors();
        // Glass set 4 references the scene-copy view (recreated above) — rewrite it.
        writeGlassDescriptors();
        // SSAO half-res targets + depth-referencing descriptors track the extent.
        createSsaoTargets();
        // Reflections (if built): the target tracks the extent and the depth + TAA
        // history views were destroyed/recreated above (createOffscreenTarget +
        // createBloomTargets) — recreate + rewrite BEFORE writeSsaoDescriptors so
        // mesh set3 binding2 picks up the NEW refl view (not the destroyed one).
        // (Missing this was a live VUID-08114 source: the headless --smoketest
        // mid-run recreate left the refl compute set on destroyed views.)
        if (m_reflBuilt) {
            if (!createReflTargets()) { destroyRefl(); m_refl.ssr = false; }
            else writeReflDescriptors();
        }
        writeSsaoDescriptors();
        // GI half-res targets + prev-depth track the extent; rebuild + rewrite.
        createGiTargets();
        writeGiDescriptors();
        m_giHistoryValid = false;
        // Water samples the scene depth: the depth view changed -> rewire it.
        writeWaterDescriptors();
        // Particles sample the scene depth (soft fade): rewire on the new depth view.
        writeParticleDescriptors();
        // HZB pyramid tracks the extent + samples the (new) depth view.
        if (m_gpuCullReady) createHzbTargets();
        // RT AO (if built): the half-res target tracks the extent AND its compute +
        // apply sets reference the depth view destroyed/recreated above — recreate
        // + rewrite, exactly like the windowed recreateSwapchain() path. (Missing
        // this was a live VUID-08114 source: the headless --test-rt mid-run
        // recreate left the rtao sets on the destroyed depth view.)
        if (m_rtaoBuilt) { createRtaoTargets(); writeRtaoDescriptors(); }
        m_creationBoundary = false;
    }

bool VulkanRenderDevice::createBloomTargets() {
        destroyBloomTargets();
        const uint32_t W = m_extent.width, H = m_extent.height;
        if (W == 0 || H == 0) { logError("[rhi] bloom: zero extent"); return false; }

        // HDR scene target (full resolution). TRANSFER_SRC so the glass scene-copy
        // pass can vkCmdCopyImage it into the scene-color copy (glass refraction).
        if (!createColorTarget(kHdrFormat, W, H,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                               | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                               m_hdrImg, m_hdrAlloc, m_hdrView)) {
            logError("[rhi] HDR scene target create failed"); return false;
        }

        // ---- TAA targets (full-res, HDR format match) -----------------------
        // OUTPUT: the resolve renders into it, AE/bloom/composite sample it, and
        // the history-copy reads it (TRANSFER_SRC). HISTORY: persists across
        // frames; written only by the history-copy (TRANSFER_DST), sampled by the
        // next frame's resolve. (Re)created with the extent -> history is invalid.
        if (!createColorTarget(kHdrFormat, W, H,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                               | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                               m_taaOutImg, m_taaOutAlloc, m_taaOutView)) {
            logError("[rhi] TAA output target create failed"); return false;
        }
        if (!createColorTarget(kHdrFormat, W, H,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               m_taaHistImg, m_taaHistAlloc, m_taaHistView)) {
            logError("[rhi] TAA history target create failed"); return false;
        }
        m_taaHistState = ResourceState{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 };
        m_taaHistoryValid = false;

        // ---- VELOCITY target (#4: per-object screen-space motion vectors) ----
        // RG16F, full-res. Written by the velocity pre-pass (COLOR_ATTACHMENT),
        // sampled by the TAA resolve (SAMPLED). (Re)created with the extent. Failure
        // is NON-FATAL: the velocity pass simply won't be built and TAA falls back
        // to camera-only reprojection (byte-identical to the pre-velocity path).
        if (!createColorTarget(kVelocityFormat, W, H,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               m_velImg, m_velAlloc, m_velView)) {
            logError("[rhi] velocity target create failed — TAA stays camera-only");
            m_velImg = VK_NULL_HANDLE; m_velAlloc = nullptr; m_velView = VK_NULL_HANDLE;
        }
        m_velState = ResourceState{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 };

        // Bloom mips: mip0 = half res, each subsequent halves again (min 1px).
        uint32_t mw = W, mh = H;
        for (uint32_t i = 0; i < kBloomMips; ++i) {
            mw = std::max(1u, mw / 2);
            mh = std::max(1u, mh / 2);
            m_bloomMips[i].extent = { mw, mh };
            if (!createColorTarget(kHdrFormat, mw, mh,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                   m_bloomMips[i].img, m_bloomMips[i].alloc, m_bloomMips[i].view)) {
                logError("[rhi] bloom mip create failed"); return false;
            }
        }

        // ---- Scene-color COPY target (glass refraction/frost, spec §3.1) -----
        // A mip-chained HDR image: mip0 (full-res) receives a vkCmdCopyImage of the
        // opaque HDR scene; mips 1..N are progressively blurred (M4 frost). Usage:
        // TRANSFER_DST (copy into mip0) + SAMPLED (glass reads) + COLOR_ATTACHMENT
        // (frost blur passes render into mips 1..N). One image, per-mip + full-chain
        // views. Graceful: failure leaves it NULL (the glass pass falls back to
        // sampling without refraction).
        if (!createSceneCopyTarget(W, H)) {
            // Non-fatal: clean up partial state; glass will run without refraction.
            destroySceneCopyTarget();
            logError("[rhi] scene-color copy target create failed — glass refraction disabled");
        }
        return true;
    }

bool VulkanRenderDevice::createSceneCopyTarget(uint32_t W, uint32_t H) {
        // Full-res, single-mip copy: TRANSFER_DST (copy target), SAMPLED (refraction
        // read), TRANSFER_SRC (frost level 0 reads it as the blur source).
        if (!createColorTarget(kHdrFormat, W, H,
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                               | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                               m_sceneCopyImg, m_sceneCopyAlloc, m_sceneCopyView)) {
            return false;
        }
        // Frost levels: each half the previous (level0 = half-res). COLOR_ATTACHMENT
        // (blur render target) + SAMPLED (sampled by the next level + the glass shader).
        uint32_t mw = W, mh = H;
        for (uint32_t i = 0; i < kGlassFrostLevels; ++i) {
            mw = std::max(1u, mw / 2); mh = std::max(1u, mh / 2);
            m_glassFrostExt[i] = { mw, mh };
            if (!createColorTarget(kHdrFormat, mw, mh,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                   m_glassFrostImg[i], m_glassFrostAlloc[i], m_glassFrostView[i])) {
                return false;
            }
        }
        return true;
    }

void VulkanRenderDevice::destroySceneCopyTarget() {
        if (m_sceneCopyView) { vkDestroyImageView(m_dev.device, m_sceneCopyView, nullptr); m_sceneCopyView = VK_NULL_HANDLE; }
        if (m_sceneCopyImg) { vmaDestroyImage(m_alloc, m_sceneCopyImg, m_sceneCopyAlloc); m_sceneCopyImg = VK_NULL_HANDLE; m_sceneCopyAlloc = nullptr; }
        for (uint32_t i = 0; i < kGlassFrostLevels; ++i) {
            if (m_glassFrostView[i]) { vkDestroyImageView(m_dev.device, m_glassFrostView[i], nullptr); m_glassFrostView[i] = VK_NULL_HANDLE; }
            if (m_glassFrostImg[i])  { vmaDestroyImage(m_alloc, m_glassFrostImg[i], m_glassFrostAlloc[i]); m_glassFrostImg[i] = VK_NULL_HANDLE; m_glassFrostAlloc[i] = nullptr; }
            m_glassFrostExt[i] = {};
        }
    }

void VulkanRenderDevice::destroyBloomTargets() {
        for (uint32_t i = 0; i < kBloomMips; ++i) {
            BloomMip& m = m_bloomMips[i];
            if (m.view)  { vkDestroyImageView(m_dev.device, m.view, nullptr); m.view = VK_NULL_HANDLE; }
            if (m.img)   { vmaDestroyImage(m_alloc, m.img, m.alloc); m.img = VK_NULL_HANDLE; m.alloc = nullptr; }
            m.extent = {};
        }
        destroySceneCopyTarget();
        if (m_taaOutView)  { vkDestroyImageView(m_dev.device, m_taaOutView, nullptr); m_taaOutView = VK_NULL_HANDLE; }
        if (m_taaOutImg)   { vmaDestroyImage(m_alloc, m_taaOutImg, m_taaOutAlloc); m_taaOutImg = VK_NULL_HANDLE; m_taaOutAlloc = nullptr; }
        if (m_taaHistView) { vkDestroyImageView(m_dev.device, m_taaHistView, nullptr); m_taaHistView = VK_NULL_HANDLE; }
        if (m_taaHistImg)  { vmaDestroyImage(m_alloc, m_taaHistImg, m_taaHistAlloc); m_taaHistImg = VK_NULL_HANDLE; m_taaHistAlloc = nullptr; }
        m_taaHistState = ResourceState{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 };
        m_taaHistoryValid = false;
        if (m_velView) { vkDestroyImageView(m_dev.device, m_velView, nullptr); m_velView = VK_NULL_HANDLE; }
        if (m_velImg)  { vmaDestroyImage(m_alloc, m_velImg, m_velAlloc); m_velImg = VK_NULL_HANDLE; m_velAlloc = nullptr; }
        m_velState = ResourceState{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 };
        if (m_hdrView) { vkDestroyImageView(m_dev.device, m_hdrView, nullptr); m_hdrView = VK_NULL_HANDLE; }
        if (m_hdrImg)  { vmaDestroyImage(m_alloc, m_hdrImg, m_hdrAlloc); m_hdrImg = VK_NULL_HANDLE; m_hdrAlloc = nullptr; }
    }

bool VulkanRenderDevice::createGlassResources() {
        // Mip-aware LINEAR clamp sampler: the frost lookup (M4) samples an explicit
        // LOD; CLAMP_TO_EDGE so a refraction offset near the screen edge doesn't wrap.
        VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.minLod = 0.0f; sci.maxLod = 0.0f;   // single-mip scene copy + frost levels
        if (vkCreateSampler(m_dev.device, &sci, nullptr, &m_glassCopySampler) != VK_SUCCESS) {
            logError("[rhi] glass scene-copy sampler failed"); return false;
        }
        // Per-frame GlassControl UBOs (host-visible, persistently mapped).
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = sizeof(GlassControl); bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo ainfo{};
            if (x3vmaCreateBuffer(&bci, &aci, &m_glassCtrlBuf[i], &m_glassCtrlAlloc[i], &ainfo) != VK_SUCCESS) {
                logError("[rhi] glass control UBO alloc failed"); return false;
            }
            m_glassCtrlMapped[i] = ainfo.pMappedData;
        }
        // Descriptor pool: per-frame glass sets (2 image samplers [scene copy +
        // frost] + 1 UBO each) PLUS the frost downsample src sets (one single-sampler
        // set per frost level, reusing m_postSetLayout1).
        VkDescriptorPoolSize ps[2]{
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight * 2 + kGlassFrostLevels },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         kFramesInFlight } };
        VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pci.maxSets = kFramesInFlight + kGlassFrostLevels; pci.poolSizeCount = 2; pci.pPoolSizes = ps;
        if (x3CreateDescriptorPool(&pci, nullptr, &m_glassPool) != VK_SUCCESS) {
            logError("[rhi] glass descriptor pool failed"); return false;
        }
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = m_glassPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &m_glassSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &ai, &m_glassSet[i]) != VK_SUCCESS) {
                logError("[rhi] glass descriptor set alloc failed"); return false;
            }
        }
        // Frost (M4) downsample src sets: one single-sampler set per output level
        // (set[i] samples the SOURCE of level i). Reuses the bloom-down pipeline +
        // layout (m_bloomDownPipe / m_postSetLayout1). Only built when the post
        // single-sampler layout + scene copy exist; otherwise frost stays off.
        if (m_postSetLayout1 && m_bloomDownPipe && m_sceneCopyImg) {
            bool ok = true;
            for (uint32_t i = 0; i < kGlassFrostLevels && ok; ++i) {
                VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
                ai.descriptorPool = m_glassPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &m_postSetLayout1;
                ok = (vkAllocateDescriptorSets(m_dev.device, &ai, &m_glassFrostSrcSet[i]) == VK_SUCCESS);
            }
            // Reuse the existing bloom downsample pipeline for the frost blur (same
            // shader, layout, HDR format, dynamic viewport). NULL -> frost disabled.
            m_glassFrostPipe = ok ? m_bloomDownPipe : VK_NULL_HANDLE;
        }
        writeGlassDescriptors();
        return true;
    }

void VulkanRenderDevice::writeGlassDescriptors() {
        if (!m_glassPool || !m_glassCopySampler) return;
        VkImageView copyView = m_sceneCopyView ? m_sceneCopyView : m_hdrView;
        // Frostiest blur level for binding 2 (M4). Only bind the frost image when the
        // frost chain actually RUNS (m_glassFrostPipe set), so its layout is
        // transitioned to SHADER_READ_ONLY by the glass pass. Otherwise fall back to
        // the sharp copy (the shader's frostReady flag is also 0, so the lerp is a
        // no-op) — never bind an untransitioned image.
        VkImageView frostView = (m_glassFrostPipe && m_glassFrostView[kGlassFrostLevels - 1])
                                ? m_glassFrostView[kGlassFrostLevels - 1] : copyView;
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (!m_glassSet[i]) continue;
            VkDescriptorImageInfo di{ m_glassCopySampler, copyView,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo df{ m_glassCopySampler, frostView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorBufferInfo bi{ m_glassCtrlBuf[i], 0, sizeof(GlassControl) };
            VkWriteDescriptorSet w[3]{};
            w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[0].dstSet = m_glassSet[i]; w[0].dstBinding = 0; w[0].descriptorCount = 1;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &di;
            w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[1].dstSet = m_glassSet[i]; w[1].dstBinding = 1; w[1].descriptorCount = 1;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[1].pBufferInfo = &bi;
            w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[2].dstSet = m_glassSet[i]; w[2].dstBinding = 2; w[2].descriptorCount = 1;
            w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[2].pImageInfo = &df;
            vkUpdateDescriptorSets(m_dev.device, 3, w, 0, nullptr);
        }
        // Frost downsample SOURCE sets: set[0] samples the scene copy, set[i] samples
        // frost level i-1 (the previous, larger level). Written here so they track the
        // recreated views on resize.
        for (uint32_t i = 0; i < kGlassFrostLevels; ++i) {
            if (!m_glassFrostSrcSet[i]) continue;
            VkImageView src = (i == 0) ? copyView : m_glassFrostView[i - 1];
            if (!src) src = copyView;
            VkDescriptorImageInfo si{ m_glassCopySampler, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet = m_glassFrostSrcSet[i]; w.dstBinding = 0; w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &si;
            vkUpdateDescriptorSets(m_dev.device, 1, &w, 0, nullptr);
        }
    }

void VulkanRenderDevice::destroyGlassResources() {
        if (m_glassPool) { vkDestroyDescriptorPool(m_dev.device, m_glassPool, nullptr); m_glassPool = VK_NULL_HANDLE; }
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            m_glassSet[i] = VK_NULL_HANDLE;
            if (m_glassCtrlBuf[i]) { vmaDestroyBuffer(m_alloc, m_glassCtrlBuf[i], m_glassCtrlAlloc[i]); m_glassCtrlBuf[i] = VK_NULL_HANDLE; m_glassCtrlAlloc[i] = nullptr; m_glassCtrlMapped[i] = nullptr; }
        }
        // Frost src sets came from m_glassPool (freed above); the pipe is an ALIAS of
        // m_bloomDownPipe (owned by destroyPost) — clear, don't destroy.
        for (uint32_t i = 0; i < kGlassFrostLevels; ++i) m_glassFrostSrcSet[i] = VK_NULL_HANDLE;
        m_glassFrostPipe = VK_NULL_HANDLE;
        if (m_glassCopySampler) { vkDestroySampler(m_dev.device, m_glassCopySampler, nullptr); m_glassCopySampler = VK_NULL_HANDLE; }
    }

bool VulkanRenderDevice::createColorTarget(VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage,
                       VkImage& outImg, VmaAllocation& outAlloc, VkImageView& outView) {
        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D; ici.format = fmt;
        ici.extent = { w, h, 1 }; ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = usage; ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        if (x3vmaCreateImage(&ici, &aci, &outImg, &outAlloc, nullptr) != VK_SUCCESS) return false;
        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = outImg; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(m_dev.device, &vci, nullptr, &outView) != VK_SUCCESS) return false;
        return true;
    }

bool VulkanRenderDevice::createPost() {
        // CLAMP_TO_EDGE linear sampler so edge taps in the down/up filters do not
        // wrap (avoids bloom bleeding across screen edges).
        VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        if (vkCreateSampler(m_dev.device, &sci, nullptr, &m_postSampler) != VK_SUCCESS) {
            logError("[rhi] post sampler create failed"); return false;
        }

        // Descriptor set layout: 1 combined image sampler (down/up source).
        VkDescriptorSetLayoutBinding b0{};
        b0.binding = 0; b0.descriptorCount = 1;
        b0.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b0.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo s1{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        s1.bindingCount = 1; s1.pBindings = &b0;
        if (vkCreateDescriptorSetLayout(m_dev.device, &s1, nullptr, &m_postSetLayout1) != VK_SUCCESS) {
            logError("[rhi] post set layout (1) failed"); return false;
        }
        // Descriptor set layout: 2 combined image samplers (composite: HDR + bloom)
        // + the auto-exposure SSBO (binding 2, fragment-read).
        VkDescriptorSetLayoutBinding b2[3]{};
        b2[0].binding = 0; b2[0].descriptorCount = 1;
        b2[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b2[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b2[1].binding = 1; b2[1].descriptorCount = 1;
        b2[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b2[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b2[2].binding = 2; b2[2].descriptorCount = 1;
        b2[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b2[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo s2{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        s2.bindingCount = 3; s2.pBindings = b2;
        if (vkCreateDescriptorSetLayout(m_dev.device, &s2, nullptr, &m_postSetLayout2) != VK_SUCCESS) {
            logError("[rhi] post set layout (2) failed"); return false;
        }
        // Auto-exposure set layout: b0 = HDR scene sampler, b1 = exposure SSBO
        // (both compute-stage; the reduce/adapt runs in autoexposure.comp).
        VkDescriptorSetLayoutBinding ab[2]{};
        ab[0].binding = 0; ab[0].descriptorCount = 1;
        ab[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ab[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        ab[1].binding = 1; ab[1].descriptorCount = 1;
        ab[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        ab[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo sa{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        sa.bindingCount = 2; sa.pBindings = ab;
        if (vkCreateDescriptorSetLayout(m_dev.device, &sa, nullptr, &m_aeSetLayout) != VK_SUCCESS) {
            logError("[rhi] auto-exposure set layout failed"); return false;
        }
        // TAA resolve set layout: b0 = current HDR scene, b1 = history, b2 = depth
        // (all fragment samplers) + b3 = the per-frame TAA UBO (matrices/params)
        // + b4 = per-object velocity (RG16F MV, #4). When velocity is off / absent
        // b4 is written with the depth view as a harmless placeholder and the
        // shader ignores it (params1.z gate), so the binding is always valid.
        VkDescriptorSetLayoutBinding tb[5]{};
        for (uint32_t i = 0; i < 3; ++i) {
            tb[i].binding = i; tb[i].descriptorCount = 1;
            tb[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            tb[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        tb[3].binding = 3; tb[3].descriptorCount = 1;
        tb[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        tb[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        tb[4].binding = 4; tb[4].descriptorCount = 1;
        tb[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        tb[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo st{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        st.bindingCount = 5; st.pBindings = tb;
        if (vkCreateDescriptorSetLayout(m_dev.device, &st, nullptr, &m_taaSetLayout) != VK_SUCCESS) {
            logError("[rhi] TAA set layout failed"); return false;
        }
        // NEAREST clamp sampler for reading the depth image as data in the TAA
        // resolve (same role as the SSAO/water depth samplers; own instance so the
        // post stack stays self-contained).
        VkSamplerCreateInfo tds{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        tds.magFilter = VK_FILTER_NEAREST; tds.minFilter = VK_FILTER_NEAREST;
        tds.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        tds.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        tds.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        tds.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(m_dev.device, &tds, nullptr, &m_taaDepthSampler) != VK_SUCCESS) {
            logError("[rhi] TAA depth sampler failed"); return false;
        }

        // Descriptor pool: (HDR set + kBloomMips mip sets + TAA-out set)
        // single-sampler sets + 2 composite sets (2 samplers + 1 SSBO each: raw-HDR
        // + TAA variants) + 2 auto-exposure sets (1 sampler + 1 SSBO each) + the
        // per-frame TAA resolve sets (4 samplers + 1 UBO each: scene/hist/depth +
        // the #4 velocity sampler at b4). Sized exactly; no UPDATE_AFTER_BIND.
        const uint32_t single = 1 + kBloomMips + 1;     // HDR + each mip + TAA out
        VkDescriptorPoolSize ps[3]{
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
              single + 2*2 + 1*2 + 4*kFramesInFlight },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         4 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         kFramesInFlight },
        };
        VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pci.maxSets = single + 4 + kFramesInFlight; pci.poolSizeCount = 3; pci.pPoolSizes = ps;
        if (x3CreateDescriptorPool(&pci, nullptr, &m_postPool) != VK_SUCCESS) {
            logError("[rhi] post desc pool failed"); return false;
        }
        // Allocate the single-sampler sets (HDR + mips) and the composite set.
        auto alloc1 = [&](VkDescriptorSet& out) -> bool {
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = m_postPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &m_postSetLayout1;
            return vkAllocateDescriptorSets(m_dev.device, &ai, &out) == VK_SUCCESS;
        };
        if (!alloc1(m_setHdr)) { logError("[rhi] post set alloc (hdr) failed"); return false; }
        for (uint32_t i = 0; i < kBloomMips; ++i)
            if (!alloc1(m_setMip[i])) { logError("[rhi] post set alloc (mip) failed"); return false; }
        if (!alloc1(m_setTaaOut)) { logError("[rhi] post set alloc (taa-out) failed"); return false; }
        {
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = m_postPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &m_postSetLayout2;
            if (vkAllocateDescriptorSets(m_dev.device, &ai, &m_setComposite) != VK_SUCCESS) {
                logError("[rhi] post set alloc (composite) failed"); return false;
            }
            if (vkAllocateDescriptorSets(m_dev.device, &ai, &m_setCompositeTaa) != VK_SUCCESS) {
                logError("[rhi] post set alloc (composite-taa) failed"); return false;
            }
        }
        {
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = m_postPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &m_aeSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &ai, &m_aeSet) != VK_SUCCESS) {
                logError("[rhi] post set alloc (auto-exposure) failed"); return false;
            }
            if (vkAllocateDescriptorSets(m_dev.device, &ai, &m_aeSetTaa) != VK_SUCCESS) {
                logError("[rhi] post set alloc (auto-exposure-taa) failed"); return false;
            }
        }
        // Per-frame TAA resolve sets + their host-mapped UBOs (matrices change
        // every frame; one buffer per frame-in-flight so a write never races the
        // GPU's read of the previous frame's set).
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = m_postPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &m_taaSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &ai, &m_taaSet[i]) != VK_SUCCESS) {
                logError("[rhi] post set alloc (taa) failed"); return false;
            }
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = sizeof(TaaUBO); bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                        VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo ainfo{};
            if (x3vmaCreateBuffer(&bci, &aci, &m_taaUboBuf[i], &m_taaUboAlloc[i], &ainfo) != VK_SUCCESS) {
                logError("[rhi] TAA UBO alloc failed"); return false;
            }
            m_taaUboMapped[i] = ainfo.pMappedData;
        }

        // Auto-exposure SSBO: 16 bytes { adapted, avgLog, pad, pad }, persistent
        // across frames (the temporal adaptation state). Host-mapped so the initial
        // neutral value (exposure 1.0) is written without a staging submit; the GPU
        // then owns it (compute writes, composite reads). Tiny + once-per-frame —
        // host-visible memory is irrelevant to performance here.
        {
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = 16; bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                        VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info{};
            if (x3vmaCreateBuffer(&bci, &aci, &m_aeBuf, &m_aeAlloc, &info) != VK_SUCCESS) {
                logError("[rhi] auto-exposure buffer create failed"); return false;
            }
            float init[4] = { 1.0f, 0.0f, 0.0f, 0.0f };   // neutral exposure
            std::memcpy(info.pMappedData, init, sizeof(init));
        }

        // Auto-exposure compute pipeline (autoexposure.comp).
        {
            VkPushConstantRange pcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(AePush) };
            VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pl.setLayoutCount = 1; pl.pSetLayouts = &m_aeSetLayout;
            pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr;
            if (vkCreatePipelineLayout(m_dev.device, &pl, nullptr, &m_aeLayout) != VK_SUCCESS) {
                logError("[rhi] auto-exposure pipeline layout failed"); return false;
            }
            VkShaderModule cs = loadShaderModule("shaders\\autoexposure.comp.spv");
            if (!cs) { logError("[rhi] autoexposure.comp.spv load failed"); return false; }
            VkComputePipelineCreateInfo cpci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpci.stage.module = cs; cpci.stage.pName = "main";
            cpci.layout = m_aeLayout;
            VkResult pr = x3CreateComputePipelines(1, &cpci, nullptr, &m_aePipe);
            vkDestroyShaderModule(m_dev.device, cs, nullptr);
            if (pr != VK_SUCCESS) { logError("[rhi] auto-exposure pipeline create failed"); return false; }
        }

        // Pipeline layouts (push constants for tunables).
        VkPushConstantRange pcBloom{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(BloomPush) };
        VkPipelineLayoutCreateInfo bl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        bl.setLayoutCount = 1; bl.pSetLayouts = &m_postSetLayout1;
        bl.pushConstantRangeCount = 1; bl.pPushConstantRanges = &pcBloom;
        if (vkCreatePipelineLayout(m_dev.device, &bl, nullptr, &m_bloomLayout) != VK_SUCCESS) {
            logError("[rhi] bloom pipeline layout failed"); return false;
        }
        VkPushConstantRange pcComp{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(CompositePush) };
        VkPipelineLayoutCreateInfo cl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        cl.setLayoutCount = 1; cl.pSetLayouts = &m_postSetLayout2;
        cl.pushConstantRangeCount = 1; cl.pPushConstantRanges = &pcComp;
        if (vkCreatePipelineLayout(m_dev.device, &cl, nullptr, &m_compositeLayout) != VK_SUCCESS) {
            logError("[rhi] composite pipeline layout failed"); return false;
        }

        // Build the three full-screen-triangle pipelines.
        if (!createFullscreenPipeline("shaders\\fullscreen.vert.spv", "shaders\\bloom_down.frag.spv",
                                      m_bloomLayout, kHdrFormat, /*additiveBlend=*/false, m_bloomDownPipe))
            return false;
        // Upsample is ADDITIVELY blended onto the larger mip (ONE,ONE) so the
        // graph's load-op keeps the existing content and the driver combines.
        if (!createFullscreenPipeline("shaders\\fullscreen.vert.spv", "shaders\\bloom_up.frag.spv",
                                      m_bloomLayout, kHdrFormat, /*additiveBlend=*/true, m_bloomUpPipe))
            return false;
        // Composite writes the LDR final target (m_format).
        if (!createFullscreenPipeline("shaders\\fullscreen.vert.spv", "shaders\\composite.frag.spv",
                                      m_compositeLayout, m_format, /*additiveBlend=*/false, m_compositePipe))
            return false;

        // TAA resolve: full-screen pass writing the HDR-format TAA output. All
        // parameters ride in the per-frame UBO -> no push constants.
        {
            VkPipelineLayoutCreateInfo tl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            tl.setLayoutCount = 1; tl.pSetLayouts = &m_taaSetLayout;
            if (vkCreatePipelineLayout(m_dev.device, &tl, nullptr, &m_taaLayout) != VK_SUCCESS) {
                logError("[rhi] TAA pipeline layout failed"); return false;
            }
            if (!createFullscreenPipeline("shaders\\fullscreen.vert.spv", "shaders\\taa_resolve.frag.spv",
                                          m_taaLayout, kHdrFormat, /*additiveBlend=*/false, m_taaPipe))
                return false;
        }

        logInfo("[rhi] HDR post pipeline ready (R16G16B16A16_SFLOAT scene + " +
                std::to_string(kBloomMips) + "-mip bloom + TAA resolve + ACES composite)");
        return true;
    }

bool VulkanRenderDevice::createFullscreenPipeline(const char* vsPath, const char* fsPath,
                              VkPipelineLayout layout, VkFormat colorFmt,
                              bool additiveBlend, VkPipeline& outPipe) {
        VkShaderModule vs = loadShaderModule(vsPath);
        VkShaderModule fs = loadShaderModule(fsPath);
        if (!vs || !fs) return false;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vs; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = 1; vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo dss{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        dss.depthTestEnable = VK_FALSE; dss.depthWriteEnable = VK_FALSE;
        dss.depthCompareOp = VK_COMPARE_OP_ALWAYS;

        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
        if (additiveBlend) {
            cba.blendEnable = VK_TRUE;
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.colorBlendOp = VK_BLEND_OP_ADD;
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.alphaBlendOp = VK_BLEND_OP_ADD;
        } else {
            cba.blendEnable = VK_FALSE;
        }
        VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 1; cb.pAttachments = &cba;

        VkDynamicState dyn[2]{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;

        VkPipelineRenderingCreateInfo prci{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        prci.colorAttachmentCount = 1; prci.pColorAttachmentFormats = &colorFmt;
        prci.depthAttachmentFormat = VK_FORMAT_UNDEFINED;   // no depth in post passes

        VkGraphicsPipelineCreateInfo gpci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gpci.pNext = &prci;
        gpci.stageCount = 2; gpci.pStages = stages;
        gpci.pVertexInputState = &vin; gpci.pInputAssemblyState = &ia;
        gpci.pViewportState = &vp; gpci.pRasterizationState = &rs;
        gpci.pMultisampleState = &ms; gpci.pDepthStencilState = &dss;
        gpci.pColorBlendState = &cb; gpci.pDynamicState = &ds; gpci.layout = layout;
        VkResult pr = x3CreateGraphicsPipelines(1, &gpci, nullptr, &outPipe);

        vkDestroyShaderModule(m_dev.device, vs, nullptr);
        vkDestroyShaderModule(m_dev.device, fs, nullptr);
        if (pr != VK_SUCCESS) { logError("[rhi] post pipeline create failed"); return false; }
        return true;
    }

void VulkanRenderDevice::writePostDescriptors() {
        auto write1 = [&](VkDescriptorSet set, VkImageView view) {
            VkDescriptorImageInfo dii{ m_postSampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet = set; w.dstBinding = 0; w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &dii;
            vkUpdateDescriptorSets(m_dev.device, 1, &w, 0, nullptr);
        };
        write1(m_setHdr, m_hdrView);
        for (uint32_t i = 0; i < kBloomMips; ++i) write1(m_setMip[i], m_bloomMips[i].view);
        write1(m_setTaaOut, m_taaOutView);   // bloom bright-pass source when TAA is on

        // Composite set: binding 0 = HDR scene, binding 1 = bloom mip0,
        // binding 2 = auto-exposure SSBO.
        VkDescriptorImageInfo d0{ m_postSampler, m_hdrView,           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo d1{ m_postSampler, m_bloomMips[0].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorBufferInfo db{ m_aeBuf, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet cw[3]{};
        cw[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        cw[0].dstSet = m_setComposite; cw[0].dstBinding = 0; cw[0].descriptorCount = 1;
        cw[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; cw[0].pImageInfo = &d0;
        cw[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        cw[1].dstSet = m_setComposite; cw[1].dstBinding = 1; cw[1].descriptorCount = 1;
        cw[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; cw[1].pImageInfo = &d1;
        cw[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        cw[2].dstSet = m_setComposite; cw[2].dstBinding = 2; cw[2].descriptorCount = 1;
        cw[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; cw[2].pBufferInfo = &db;
        vkUpdateDescriptorSets(m_dev.device, 3, cw, 0, nullptr);

        // Auto-exposure set: b0 = HDR scene (sampled by the compute reduce; the
        // view changes on resize, hence rewritten here), b1 = the exposure SSBO.
        VkDescriptorImageInfo a0{ m_postSampler, m_hdrView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet aw[2]{};
        aw[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        aw[0].dstSet = m_aeSet; aw[0].dstBinding = 0; aw[0].descriptorCount = 1;
        aw[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; aw[0].pImageInfo = &a0;
        aw[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        aw[1].dstSet = m_aeSet; aw[1].dstBinding = 1; aw[1].descriptorCount = 1;
        aw[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; aw[1].pBufferInfo = &db;
        vkUpdateDescriptorSets(m_dev.device, 2, aw, 0, nullptr);

        // ---- TAA variants + per-frame resolve sets ---------------------------
        // Composite-TAA set: binding 0 = the TAA RESOLVE OUTPUT (instead of the
        // raw HDR scene), binding 1 = bloom mip0, binding 2 = AE SSBO.
        VkDescriptorImageInfo t0{ m_postSampler, m_taaOutView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet tw[3]{};
        tw[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        tw[0].dstSet = m_setCompositeTaa; tw[0].dstBinding = 0; tw[0].descriptorCount = 1;
        tw[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; tw[0].pImageInfo = &t0;
        tw[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        tw[1].dstSet = m_setCompositeTaa; tw[1].dstBinding = 1; tw[1].descriptorCount = 1;
        tw[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; tw[1].pImageInfo = &d1;
        tw[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        tw[2].dstSet = m_setCompositeTaa; tw[2].dstBinding = 2; tw[2].descriptorCount = 1;
        tw[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; tw[2].pBufferInfo = &db;
        vkUpdateDescriptorSets(m_dev.device, 3, tw, 0, nullptr);

        // AE-TAA set: meter the TAA output (b0) + the same exposure SSBO (b1).
        VkWriteDescriptorSet atw[2]{};
        atw[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        atw[0].dstSet = m_aeSetTaa; atw[0].dstBinding = 0; atw[0].descriptorCount = 1;
        atw[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; atw[0].pImageInfo = &t0;
        atw[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        atw[1].dstSet = m_aeSetTaa; atw[1].dstBinding = 1; atw[1].descriptorCount = 1;
        atw[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; atw[1].pBufferInfo = &db;
        vkUpdateDescriptorSets(m_dev.device, 2, atw, 0, nullptr);

        // Per-frame TAA resolve sets: b0 = current HDR scene, b1 = history,
        // b2 = scene depth (sampled as data in DEPTH_READ_ONLY), b3 = that
        // frame's TAA UBO. Views change on resize -> rewritten here every time.
        VkDescriptorImageInfo r0{ m_postSampler,     m_hdrView,     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo r1{ m_postSampler,     m_taaHistView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo r2{ m_taaDepthSampler, m_depthView,   VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL };
        // b4 velocity: real RG16F MV view when present, else the HDR view as a
        // harmless placeholder so the binding is always valid (shader ignores it
        // unless params1.z says velocity is valid). m_postSampler is LINEAR clamp.
        VkDescriptorImageInfo r4{ m_postSampler,
                                  m_velView ? m_velView : m_hdrView,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (!m_taaSet[i] || !m_taaUboBuf[i]) continue;
            VkDescriptorBufferInfo rb{ m_taaUboBuf[i], 0, VK_WHOLE_SIZE };
            VkWriteDescriptorSet rw[5]{};
            for (uint32_t b = 0; b < 3; ++b) {
                rw[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                rw[b].dstSet = m_taaSet[i]; rw[b].dstBinding = b; rw[b].descriptorCount = 1;
                rw[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            }
            rw[0].pImageInfo = &r0; rw[1].pImageInfo = &r1; rw[2].pImageInfo = &r2;
            rw[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            rw[3].dstSet = m_taaSet[i]; rw[3].dstBinding = 3; rw[3].descriptorCount = 1;
            rw[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; rw[3].pBufferInfo = &rb;
            rw[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            rw[4].dstSet = m_taaSet[i]; rw[4].dstBinding = 4; rw[4].descriptorCount = 1;
            rw[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; rw[4].pImageInfo = &r4;
            vkUpdateDescriptorSets(m_dev.device, 5, rw, 0, nullptr);
        }
    }

void VulkanRenderDevice::destroyPost() {
        destroyBloomTargets();
        if (m_taaPipe)        { vkDestroyPipeline(m_dev.device, m_taaPipe, nullptr); m_taaPipe = VK_NULL_HANDLE; }
        if (m_taaLayout)      { vkDestroyPipelineLayout(m_dev.device, m_taaLayout, nullptr); m_taaLayout = VK_NULL_HANDLE; }
        if (m_taaSetLayout)   { vkDestroyDescriptorSetLayout(m_dev.device, m_taaSetLayout, nullptr); m_taaSetLayout = VK_NULL_HANDLE; }
        if (m_taaDepthSampler){ vkDestroySampler(m_dev.device, m_taaDepthSampler, nullptr); m_taaDepthSampler = VK_NULL_HANDLE; }
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (m_taaUboBuf[i]) { vmaDestroyBuffer(m_alloc, m_taaUboBuf[i], m_taaUboAlloc[i]); m_taaUboBuf[i] = VK_NULL_HANDLE; m_taaUboAlloc[i] = nullptr; m_taaUboMapped[i] = nullptr; }
        }
        if (m_aePipe)         { vkDestroyPipeline(m_dev.device, m_aePipe, nullptr); m_aePipe = VK_NULL_HANDLE; }
        if (m_aeLayout)       { vkDestroyPipelineLayout(m_dev.device, m_aeLayout, nullptr); m_aeLayout = VK_NULL_HANDLE; }
        if (m_aeBuf)          { vmaDestroyBuffer(m_alloc, m_aeBuf, m_aeAlloc); m_aeBuf = VK_NULL_HANDLE; m_aeAlloc = nullptr; }
        if (m_aeSetLayout)    { vkDestroyDescriptorSetLayout(m_dev.device, m_aeSetLayout, nullptr); m_aeSetLayout = VK_NULL_HANDLE; }
        if (m_compositePipe)  { vkDestroyPipeline(m_dev.device, m_compositePipe, nullptr); m_compositePipe = VK_NULL_HANDLE; }
        if (m_bloomUpPipe)    { vkDestroyPipeline(m_dev.device, m_bloomUpPipe, nullptr); m_bloomUpPipe = VK_NULL_HANDLE; }
        if (m_bloomDownPipe)  { vkDestroyPipeline(m_dev.device, m_bloomDownPipe, nullptr); m_bloomDownPipe = VK_NULL_HANDLE; }
        if (m_compositeLayout){ vkDestroyPipelineLayout(m_dev.device, m_compositeLayout, nullptr); m_compositeLayout = VK_NULL_HANDLE; }
        if (m_bloomLayout)    { vkDestroyPipelineLayout(m_dev.device, m_bloomLayout, nullptr); m_bloomLayout = VK_NULL_HANDLE; }
        if (m_postPool)       { vkDestroyDescriptorPool(m_dev.device, m_postPool, nullptr); m_postPool = VK_NULL_HANDLE; }
        if (m_postSetLayout2) { vkDestroyDescriptorSetLayout(m_dev.device, m_postSetLayout2, nullptr); m_postSetLayout2 = VK_NULL_HANDLE; }
        if (m_postSetLayout1) { vkDestroyDescriptorSetLayout(m_dev.device, m_postSetLayout1, nullptr); m_postSetLayout1 = VK_NULL_HANDLE; }
        if (m_postSampler)    { vkDestroySampler(m_dev.device, m_postSampler, nullptr); m_postSampler = VK_NULL_HANDLE; }
    }

const VulkanRenderDevice::RoleFontDesc* VulkanRenderDevice::roleFontTable() {
        // Indexed by FontRole. Console==HudMono share index 0 (embedded mono).
        static const RoleFontDesc kRoleFontPaths[kFontRoleCount] = {
            /* 0 Console/HudMono */ { "Consolas.ttf",                                 false, "Consolas" }, // matches the BabylonJS x3-console (Consolas,Courier New,monospace); embedded Roboto Mono is the fallback
            /* 1 Title           */ { "Orbitron/static/Orbitron-Bold.ttf",           true,  "Orbitron-Bold" },
            /* 2 Menu            */ { "Space_Grotesk/static/SpaceGrotesk-Medium.ttf", true,  "SpaceGrotesk-Medium" },
            /* 3 Enemy           */ { "Tektur/static/Tektur_Condensed-Bold.ttf",      true,  "Tektur_Condensed-Bold" },
            /* 4 News            */ { "Space_Mono/SpaceMono-Bold.ttf",                false, "SpaceMono-Bold" },
        };
        return kRoleFontPaths;
    }

bool VulkanRenderDevice::buildFontAtlas() {
        // Universal last-resort bitmap atlas (used if a role's TTF AND the embedded
        // fallback both fail — extremely unlikely, but never ship blank text).
        m_bitmapFontReady = buildBitmapFontAtlas(m_bitmapFontTex);
        if (!m_bitmapFontReady)
            logError("[rhi] HUD font: 8x8 bitmap fallback atlas failed to build");

        const RoleFontDesc* table = roleFontTable();
        int roleOk = 0;
        for (int r = 0; r < kFontRoleCount; ++r) {
            // HudMono is an ALIAS of Console (same enum value 0); the loop visits each
            // distinct index once, so no special-casing is needed.
            FontAtlas& fa = m_fonts[r];
            fa = FontAtlas{};
            fa.proportional = table[r].proportional;

            // Load the role's TTF bytes from assets/fonts/, else the embedded Roboto
            // Mono. (Index 0 has no file => always embedded.)
            std::vector<unsigned char> fileBytes;
            const unsigned char* ttf = kRobotoMonoTTF;
            size_t ttfSize = kRobotoMonoTTFSize;
            std::string loaded = "Roboto Mono (embedded)";
            if (table[r].path) {
                std::string resolved;
                fileBytes = readFontFile(table[r].path, resolved);
                if (!fileBytes.empty()) {
                    ttf = fileBytes.data(); ttfSize = fileBytes.size();
                    loaded = std::string(table[r].label) + " (" + resolved + ")";
                } else {
                    loaded = std::string("Roboto Mono (embedded; ") + table[r].path +
                             " not found)";
                }
            }

            if (bakeTtfAtlas(r, ttf, ttfSize)) {
                fa.ready = true; ++roleOk;
                logInfo("[rhi] HUD font role " + roleName(r) + " -> " + loaded +
                        (fa.proportional ? " [proportional]" : " [monospace]"));
            } else {
                // The role's chosen TTF failed — fall back to the EMBEDDED font so the
                // role still has a proper atlas (mono), rather than the 8x8 bitmap.
                fa = FontAtlas{}; fa.proportional = false;
                if (ttf != kRobotoMonoTTF && bakeTtfAtlas(r, kRobotoMonoTTF, kRobotoMonoTTFSize)) {
                    fa.ready = true; ++roleOk;
                    logError("[rhi] HUD font role " + roleName(r) + " -> " + loaded +
                             " FAILED to bake; using Roboto Mono (embedded) [monospace]");
                } else {
                    logError("[rhi] HUD font role " + roleName(r) +
                             ": TTF bake failed entirely — using 8x8 bitmap fallback");
                }
            }
        }
        logInfo("[rhi] HUD fonts: " + std::to_string(roleOk) + "/" +
                std::to_string(kFontRoleCount) + " role atlases baked from TTF");
        // Success as long as SOMETHING can render text (a role atlas or the bitmap).
        return roleOk > 0 || m_bitmapFontReady;
    }

std::string VulkanRenderDevice::roleName(int r) {
        switch (r) {
            case 0: return "Console/HudMono";
            case 1: return "Title";
            case 2: return "Menu";
            case 3: return "Enemy";
            case 4: return "News";
            default: return std::to_string(r);
        }
    }

bool VulkanRenderDevice::bakeTtfAtlas(int role, const unsigned char* ttf, size_t ttfSize) {
        FontAtlas& fa = m_fonts[role];
        if (!ttf || ttfSize < 4) { logError("[rhi] TTF: empty font data"); return false; }

        stbtt_fontinfo info{};
        const int off = stbtt_GetFontOffsetForIndex(ttf, 0);
        if (off < 0) { logError("[rhi] TTF: GetFontOffsetForIndex failed"); return false; }
        if (!stbtt_InitFont(&info, ttf, off)) { logError("[rhi] TTF: InitFont failed"); return false; }

        // Global vertical metrics at the bake size (consistent baseline/cell height).
        const float scale = stbtt_ScaleForPixelHeight(&info, kTtfBakePx);
        int asc = 0, desc = 0, lineGap = 0;
        stbtt_GetFontVMetrics(&info, &asc, &desc, &lineGap);
        fa.ascent = asc * scale;   // baseline distance from the cell top

        // Reference cell advance (drives bake-px -> requested-px scale `s`). For mono
        // fonts every glyph shares this; for proportional fonts it's just the scale
        // reference ('M' advance), with real per-glyph advances stored per glyph.
        {
            int adv = 0, lsb = 0;
            stbtt_GetCodepointHMetrics(&info, 'M', &adv, &lsb);
            fa.cellAdvance = adv * scale;
            if (fa.cellAdvance <= 0.0f) fa.cellAdvance = kTtfBakePx; // sane fallback
        }

        // Pack the glyph range into an 8-bit coverage atlas.
        std::vector<unsigned char> coverage((size_t)kTtfAtlasW * kTtfAtlasH, 0);
        std::vector<stbtt_packedchar> packed(kTtfCharCount);
        stbtt_pack_context spc{};
        if (!stbtt_PackBegin(&spc, coverage.data(), kTtfAtlasW, kTtfAtlasH,
                             /*stride=*/0, /*padding=*/1, nullptr)) {
            logError("[rhi] TTF: PackBegin failed"); return false;
        }
        stbtt_PackSetOversampling(&spc, 2, 2);   // 2x2 supersample for crisp small text
        if (!stbtt_PackFontRange(&spc, ttf, 0, kTtfBakePx,
                                 kTtfFirstChar, kTtfCharCount, packed.data())) {
            logError("[rhi] TTF: PackFontRange failed (atlas too small?)");
            stbtt_PackEnd(&spc);
            return false;
        }
        stbtt_PackEnd(&spc);

        // Record per-glyph atlas rects + quad offsets + advance (bake-pixel units).
        for (int i = 0; i < kTtfCharCount; ++i) {
            const stbtt_packedchar& pc = packed[i];
            TtfGlyph& g = fa.glyphs[i];
            g.u0 = (float)pc.x0 / (float)kTtfAtlasW;
            g.v0 = (float)pc.y0 / (float)kTtfAtlasH;
            g.u1 = (float)pc.x1 / (float)kTtfAtlasW;
            g.v1 = (float)pc.y1 / (float)kTtfAtlasH;
            // pc.xoff/yoff are offsets from the pen (baseline) to the glyph's top-left.
            g.x0 = pc.xoff;  g.y0 = pc.yoff;
            g.x1 = pc.xoff2; g.y1 = pc.yoff2;
            g.advance = pc.xadvance;
        }

        // Expand coverage -> RGBA (white, alpha=coverage) and upload.
        std::vector<uint8_t> rgba((size_t)kTtfAtlasW * kTtfAtlasH * 4, 0);
        for (size_t p = 0; p < coverage.size(); ++p) {
            rgba[p*4+0] = 255; rgba[p*4+1] = 255; rgba[p*4+2] = 255;
            rgba[p*4+3] = coverage[p];
        }
        if (!createSampledTexture(rgba.data(), kTtfAtlasW, kTtfAtlasH, /*srgb=*/false, fa.tex)) {
            logError("[rhi] TTF: atlas texture upload failed"); return false;
        }

        // LINEAR + CLAMP sampler: smooth glyph edges (antialiased), no wrap bleed.
        if (fa.tex.sampler) vkDestroySampler(m_dev.device, fa.tex.sampler, nullptr);
        fa.tex.sampler = VK_NULL_HANDLE;
        VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(m_dev.device, &sci, nullptr, &fa.tex.sampler) != VK_SUCCESS) {
            logError("[rhi] TTF font sampler create failed"); return false;
        }
        return true;
    }

bool VulkanRenderDevice::buildBitmapFontAtlas(Texture& dst) {
        constexpr uint32_t kAtlasW = 128, kAtlasH = 64;
        std::vector<uint8_t> rgba((size_t)kAtlasW * kAtlasH * 4, 0);
        for (int ch = 0; ch < 128; ++ch) {
            int col = ch % 16, row = ch / 16;
            int baseX = col * 8, baseY = row * 8;
            for (int gy = 0; gy < 8; ++gy) {
                unsigned char bits = kFont8x8Basic[ch][gy];
                for (int gx = 0; gx < 8; ++gx) {
                    bool on = (bits >> gx) & 1u;  // bit0 = leftmost pixel
                    size_t px = ((size_t)(baseY + gy) * kAtlasW + (baseX + gx)) * 4;
                    uint8_t a = on ? 255 : 0;
                    rgba[px+0] = 255; rgba[px+1] = 255; rgba[px+2] = 255; rgba[px+3] = a;
                }
            }
        }
        // UNORM (data): the per-vertex color already carries the desired tint, so
        // no sRGB linearization of the mask is wanted. Nearest filtering keeps the
        // pixel font crisp.
        if (!createSampledTexture(rgba.data(), kAtlasW, kAtlasH, /*srgb=*/false, dst))
            return false;
        // Replace the linear sampler with a NEAREST, CLAMP one for crisp glyphs.
        if (dst.sampler) vkDestroySampler(m_dev.device, dst.sampler, nullptr);
        dst.sampler = VK_NULL_HANDLE;
        VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter = VK_FILTER_NEAREST; sci.minFilter = VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(m_dev.device, &sci, nullptr, &dst.sampler) != VK_SUCCESS) {
            logError("[rhi] font sampler create failed"); return false;
        }
        return true;
    }

bool VulkanRenderDevice::createHud() {
        // Descriptor set layout: just the HUD texture at binding 0 (frag).
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0; b.descriptorCount = 1;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        slci.bindingCount = 1; slci.pBindings = &b;
        if (vkCreateDescriptorSetLayout(m_dev.device, &slci, nullptr, &m_hudSetLayout) != VK_SUCCESS) {
            logError("[rhi] HUD set layout failed"); return false;
        }

        // Per-frame HUD descriptor pools + host-visible vertex rings.
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            auto& fr = m_frames[i];
            VkDescriptorPoolSize sz{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxHudDraws };
            VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.maxSets = kMaxHudDraws; pci.poolSizeCount = 1; pci.pPoolSizes = &sz;
            if (x3CreateDescriptorPool(&pci, nullptr, &fr.hudDescPool) != VK_SUCCESS) {
                logError("[rhi] HUD descriptor pool failed"); return false;
            }
            VkBufferCreateInfo vbci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            vbci.size = (VkDeviceSize)kMaxHudVerts * sizeof(HudVertex);
            vbci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
            VmaAllocationCreateInfo vaci{};
            vaci.usage = VMA_MEMORY_USAGE_AUTO;
            vaci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo vinfo{};
            if (x3vmaCreateBuffer(&vbci, &vaci, &fr.hudVbo, &fr.hudVboAlloc, &vinfo) != VK_SUCCESS) {
                logError("[rhi] HUD vertex ring create failed"); return false;
            }
            fr.hudVboMapped = vinfo.pMappedData;
        }

        // Font atlas (uploaded once).
        if (!buildFontAtlas()) { logError("[rhi] font atlas build failed"); return false; }

        // Shaders.
        VkShaderModule vs = loadShaderModule("shaders\\hud.vert.spv");
        VkShaderModule fs = loadShaderModule("shaders\\hud.frag.spv");
        if (!vs || !fs) return false;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vs; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

        VkVertexInputBindingDescription bind{ 0, sizeof(HudVertex), VK_VERTEX_INPUT_RATE_VERTEX };
        VkVertexInputAttributeDescription attrs[3]{
            { 0, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(HudVertex, pos)   },
            { 1, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(HudVertex, uv)    },
            { 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(HudVertex, color) },
        };
        VkPipelineVertexInputStateCreateInfo vin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &bind;
        vin.vertexAttributeDescriptionCount = 3; vin.pVertexAttributeDescriptions = attrs;

        VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = 1; vp.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // No depth test/write: the HUD draws on top of the 3D scene.
        VkPipelineDepthStencilStateCreateInfo dss{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        dss.depthTestEnable = VK_FALSE; dss.depthWriteEnable = VK_FALSE;
        dss.depthCompareOp = VK_COMPARE_OP_ALWAYS;

        // Straight (non-premultiplied) alpha blend.
        VkPipelineColorBlendAttachmentState cba{};
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 1; cb.pAttachments = &cba;

        VkDynamicState dyn[2]{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;

        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount = 1; plci.pSetLayouts = &m_hudSetLayout; // no push constants
        if (vkCreatePipelineLayout(m_dev.device, &plci, nullptr, &m_hudLayout) != VK_SUCCESS) {
            logError("[rhi] HUD pipeline layout failed"); return false;
        }

        // HDR pipeline: the HUD is now drawn in the COMPOSITE pass (after tonemap),
        // which writes the LDR final target (m_format) and has NO depth attachment.
        // So the HUD pipeline declares the LDR color format + UNDEFINED depth.
        VkPipelineRenderingCreateInfo prci{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        prci.colorAttachmentCount = 1; prci.pColorAttachmentFormats = &m_format;
        prci.depthAttachmentFormat = VK_FORMAT_UNDEFINED;  // composite pass has no depth

        VkGraphicsPipelineCreateInfo gpci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gpci.pNext = &prci;
        gpci.stageCount = 2; gpci.pStages = stages;
        gpci.pVertexInputState = &vin; gpci.pInputAssemblyState = &ia;
        gpci.pViewportState = &vp; gpci.pRasterizationState = &rs;
        gpci.pMultisampleState = &ms; gpci.pDepthStencilState = &dss;
        gpci.pColorBlendState = &cb; gpci.pDynamicState = &ds; gpci.layout = m_hudLayout;
        VkResult pr = x3CreateGraphicsPipelines(1, &gpci, nullptr, &m_hudPipeline);

        vkDestroyShaderModule(m_dev.device, vs, nullptr);
        vkDestroyShaderModule(m_dev.device, fs, nullptr);
        if (pr != VK_SUCCESS) { logError("[rhi] HUD pipeline create failed"); return false; }

        logInfo("[rhi] HUD 2D pipeline ready (NDC quads + TTF/bitmap glyph atlas, alpha-blended, no depth)");
        return true;
    }

void VulkanRenderDevice::destroyHud() {
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            auto& fr = m_frames[i];
            if (fr.hudVbo) { vmaDestroyBuffer(m_alloc, fr.hudVbo, fr.hudVboAlloc);
                             fr.hudVbo = VK_NULL_HANDLE; fr.hudVboAlloc = nullptr; fr.hudVboMapped = nullptr; }
            if (fr.hudDescPool) { vkDestroyDescriptorPool(m_dev.device, fr.hudDescPool, nullptr); fr.hudDescPool = VK_NULL_HANDLE; }
        }
        for (int r = 0; r < kFontRoleCount; ++r) {
            destroyTextureObj(m_fonts[r].tex);
            m_fonts[r].ready = false;
        }
        destroyTextureObj(m_bitmapFontTex);
        m_bitmapFontReady = false;
        if (m_hudPipeline)  vkDestroyPipeline(m_dev.device, m_hudPipeline, nullptr);
        if (m_hudLayout)    vkDestroyPipelineLayout(m_dev.device, m_hudLayout, nullptr);
        if (m_hudSetLayout) vkDestroyDescriptorSetLayout(m_dev.device, m_hudSetLayout, nullptr);
        m_hudPipeline = VK_NULL_HANDLE; m_hudLayout = VK_NULL_HANDLE; m_hudSetLayout = VK_NULL_HANDLE;
    }

} // namespace x3::rhi
