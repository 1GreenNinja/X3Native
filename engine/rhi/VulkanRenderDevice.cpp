// Vulkan implementation of IRenderDevice — D1 (clean-room).
// Spec: specs/D1-render-device.spec.md
//
// IMPLEMENTED: instance + Win32 surface + device (vk-bootstrap), swapchain,
// per-frame command buffers + sync, dynamic-rendering clear-to-color, present,
// resize/out-of-date recreation. Validation-clean on RTX A2000 (Vulkan 1.3).
// S1: generic mesh/texture/draw API — staging-uploaded device-local vertex/index
// buffers, sampled textures (sRGB/UNORM), and a textured-lit pipeline.
//
// Subsystem D — GPU-DRIVEN RENDER CORE (this file): the per-draw descriptor
// allocate/bind + per-draw push-constants that made the renderer CPU-bound are
// GONE. The fast path is now:
//   * BINDLESS textures: one large COMBINED_IMAGE_SAMPLER array (set 0, binding
//     0; runtimeDescriptorArray + partiallyBound + updateAfterBind). Each
//     createTexture() grabs a stable bindless index; the 1x1 white default is
//     index 0. The shader samples textures[texIndex] (non-uniform).
//   * PER-OBJECT data in a per-frame persistent-mapped SSBO ring (set 1, binding
//     0): { mat4 model, vec4 baseColorFactor, uint texIndex }. The camera
//     viewProj is one frame UBO (set 1, binding 1). drawMesh() binds NOTHING and
//     pushes NOTHING — it just appends a CPU record.
//   * MULTIDRAW-INDIRECT: endFrame() groups the frame's draws by mesh, fills the
//     SSBO (instances of a mesh contiguous) + a VkDrawIndexedIndirectCommand
//     buffer, and issues ONE vkCmdDrawIndexedIndirect per distinct mesh. The
//     vertex shader reads its row via gl_InstanceIndex (firstInstance + instance).
//     100k single-mesh cubes => ONE draw call instead of 100k.
//   * Culling: NOT implemented here (neither CPU nor compute). The bottleneck the
//     baseline exposed was per-draw CPU submission, which bindless+multidraw kills
//     outright; the worst-case bench points the camera so the whole field is
//     on-screen anyway, where culling would not help. Compute frustum culling is
//     the documented stretch and is left for a follow-up (see RETURN notes).
//
// The HUD 2D overlay keeps its own pipeline + per-draw descriptor path unchanged
// (few draws/frame; not a bottleneck).
//
// This file is the ONLY place Vulkan headers are included — IRenderDevice.h
// stays graphics-API-free.

#include "IRenderDevice.h"
#include "RenderGraph.h"
#include "../core/x3_log.h"
#include "font8x8_basic.h"
#include "font_robotomono.h"   // embedded Roboto Mono TTF (Apache-2.0) — modern HUD font

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>

// PNG writer for captureFrame() (--screenshot). This is the ONLY translation unit
// that defines STB_IMAGE_WRITE_IMPLEMENTATION (ModelLoader.cpp owns the matching
// STB_IMAGE_IMPLEMENTATION for the reader — they are separate stb headers).
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

// stb_truetype: bake a crisp glyph atlas from a real TTF at device init (modern
// HUD/menu font). This is the ONLY translation unit that defines the impl macro.
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#include <vector>
#include <string>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <system_error>
#include <cstring>
#include <cstddef>
#include <cstdio>
#include <cassert>
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
        m_headless = desc.headless;

        // ---- Instance ----
        vkb::InstanceBuilder ib;
        ib.set_app_name("X3Native")
          .set_engine_name("X3Native")
          .require_api_version(1, 3, 0)
          .request_validation_layers(desc.validation)
          .use_default_debug_messenger();
        // Fix 6 (b): standing best-practices validation guard. Wired but OFF BY
        // DEFAULT — best-practices emits a flood of benign perf/style warnings that
        // would break the smoketest's "0 VUID" gate. Define X3_VK_BEST_PRACTICES
        // (e.g. /D X3_VK_BEST_PRACTICES) to opt in for a deliberate lifetime/usage
        // audit; only active when validation is also enabled.
#ifdef X3_VK_BEST_PRACTICES
        if (desc.validation) {
            ib.add_validation_feature_enable(VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT);
            logInfo("[rhi] best-practices validation ENABLED (X3_VK_BEST_PRACTICES) — "
                    "expect benign perf warnings; not for the 0-VUID gate");
        }
#endif
        auto inst_ret = ib.build();
        if (!inst_ret) { logError(std::string("[rhi] instance: ") + inst_ret.error().message()); return false; }
        m_inst = inst_ret.value();

        // ---- Win32 surface ----
        // HEADLESS: skip surface creation entirely. No VK_KHR_surface / swapchain
        // extension is needed for the offscreen path — we never present. The
        // physical-device selector below uses defer_surface_initialization() so it
        // does NOT require a presentable queue against a surface.
        if (!m_headless) {
            if (!desc.nativeWindowHandle) { logError("[rhi] init: null HWND"); return false; }
            VkWin32SurfaceCreateInfoKHR sci{};
            sci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
            sci.hinstance = ::GetModuleHandle(nullptr);
            sci.hwnd = static_cast<HWND>(desc.nativeWindowHandle);
            if (vkCreateWin32SurfaceKHR(m_inst.instance, &sci, nullptr, &m_surface) != VK_SUCCESS) {
                logError("[rhi] vkCreateWin32SurfaceKHR failed"); return false;
            }
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
        // ---- Bindless (Subsystem D): the descriptor-indexing sub-features that
        // make a single large, partially-bound, update-after-bind texture array
        // legal + a non-uniform shader index into it. All supported on Pascal
        // (1080 Ti) and newer; required for the GPU-driven render core. ----
        f12.runtimeDescriptorArray                          = VK_TRUE;
        f12.descriptorBindingPartiallyBound                 = VK_TRUE;
        f12.descriptorBindingSampledImageUpdateAfterBind    = VK_TRUE;
        f12.descriptorBindingVariableDescriptorCount        = VK_TRUE;
        f12.shaderSampledImageArrayNonUniformIndexing       = VK_TRUE;

        // Core (1.0) features the GPU-driven path needs:
        //   drawIndirectFirstInstance — our per-mesh indirect command sets
        //     firstInstance = the SSBO base row so gl_InstanceIndex addresses the
        //     object directly; non-zero firstInstance in an indirect draw requires
        //     this feature. (multiDrawIndirect not needed: drawCount == 1 per call.)
        VkPhysicalDeviceFeatures f10{};
        f10.drawIndirectFirstInstance = VK_TRUE;

        vkb::PhysicalDeviceSelector sel{ m_inst };
        sel.set_minimum_version(1,3)
           .set_required_features(f10)
           .set_required_features_13(f13).set_required_features_12(f12);
        if (m_headless) {
            // No surface: select a device without checking present support. The
            // graphics queue we pick below is all the offscreen path needs.
            sel.defer_surface_initialization();
        } else {
            sel.set_surface(m_surface);
        }
        auto phys_ret = sel.select();
        if (!phys_ret) { logError(std::string("[rhi] phys: ") + phys_ret.error().message()); return false; }
        vkb::PhysicalDevice phys = phys_ret.value();
        m_descriptorIndexing = true;

        // ---- Hardware ray tracing (RT Phase 0) — OPTIONAL + non-breaking. We do NOT
        // require these in the selector (that would refuse a non-RT GPU); instead we
        // enable them on the ALREADY-selected device only if present. If absent,
        // m_rtSupported stays false and device creation proceeds exactly as before
        // (SSAO/CSM raster fallback). We use RAY QUERY (inline RT in compute/fragment)
        // — no RT pipeline / shader binding table — so the AS feeds the existing
        // lighting pass. bufferDeviceAddress (needed for AS builds) is already on. ----
        {
            const std::vector<const char*> rtExts = {
                "VK_KHR_acceleration_structure",
                "VK_KHR_ray_query",
                "VK_KHR_deferred_host_operations",
            };
            if (phys.enable_extensions_if_present(rtExts)) {
                VkPhysicalDeviceAccelerationStructureFeaturesKHR asf{};
                asf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
                asf.accelerationStructure = VK_TRUE;
                VkPhysicalDeviceRayQueryFeaturesKHR rqf{};
                rqf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
                rqf.rayQuery = VK_TRUE;
                const bool a = phys.enable_extension_features_if_present(asf);
                const bool b = phys.enable_extension_features_if_present(rqf);
                m_rtSupported = a && b;
            }
            logInfo(m_rtSupported
                ? "[rhi] RT: ray-query SUPPORTED (VK_KHR_ray_query + acceleration_structure) — RT shadows/reflections/GI available"
                : "[rhi] RT: not available on this device — SSAO/CSM raster fallback");
        }

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
                " (Vulkan 1.3, dynamic-rendering + sync2 + descriptor-indexing)" +
                (m_headless ? " [HEADLESS: offscreen target, no surface/swapchain]" : ""));

        // VMA allocator (needed by the swapchain's depth image + graphics buffers)
        VmaAllocatorCreateInfo aci{};
        aci.physicalDevice = m_dev.physical_device;
        aci.device = m_dev.device;
        aci.instance = m_inst.instance;
        aci.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        aci.vulkanApiVersion = VK_API_VERSION_1_3;
        if (vmaCreateAllocator(&aci, &m_alloc) != VK_SUCCESS) { logError("[rhi] VMA create failed"); return false; }

        if (m_headless) { if (!createOffscreenTarget(m_width, m_height)) return false; }
        else            { if (!createSwapchain(m_width, m_height)) return false; }
        if (!createPerFrame()) return false;
        if (!createShadowImage()) return false;   // before createGraphics (mesh layout needs set 2)
        if (!createGraphics()) return false;      // builds the shadow depth pipeline at the end
        if (!createHud()) return false;
        if (!createSky()) return false;           // analytic sky (open-world track, task A)
        // HDR pipeline + bloom: build the post pipelines (extent-independent) then
        // the HDR scene + bloom-mip targets at the current extent + their sets.
        if (!createPost()) return false;
        if (!createBloomTargets()) return false;
        writePostDescriptors();
        // SSAO (depth pre-pass + half-res hemisphere AO + depth-aware blur). Built
        // after the mesh layout (it adds set 3 to the mesh pipeline) + the extent
        // is known. Enabled by default with tasteful tunables.
        buildSsaoKernelAndNoise();
        if (!createSsao()) return false;
        if (!createSsaoTargets()) return false;
        writeSsaoDescriptors();
        // GI (real-time dynamic global illumination / screen-space indirect diffuse).
        // Built after SSAO (it reuses the SSAO depth pre-pass + the blurred AO + the
        // lit HDR scene as its radiance source). On by default with tasteful values.
        buildGiKernelAndNoise();
        if (!createGi()) return false;
        if (!createGiTargets()) return false;
        writeGiDescriptors();
        // Water (undersea-world foundation): a grid plane + Gerstner-wave pipeline
        // that samples the scene depth (above) for the depth-based color. Built
        // after the depth buffer exists; OFF by default (gated by setWaterParams).
        if (!createWater()) return false;
        // Particles + impact decals (combat juice): the billboard + decal pipelines,
        // the shared unit quad, and the per-frame instance rings / UBOs. Built after
        // the depth buffer exists (soft particles sample it). The whole pass is gated
        // per-frame on whether anything was submitted, so it costs nothing when idle.
        if (!createParticles()) return false;
        // GPU-compute persistent debris world (K-T2): the FIRST compute pipeline in
        // the renderer. A host-visible pool SSBO integrated by a compute dispatch +
        // drawn via the same instanced pattern the particles use. Additive + opt-in:
        // nothing happens unless the host spawns + steps + draws debris.
        if (!createDebris()) return false;
        // GPU compute skinning (GPU SKINNING OF MODELS): a compute LBS pre-pass that
        // skins registered meshes into their per-frame skinned-output vbo BEFORE the
        // depth/shadow/color passes (which draw it unchanged). Additive + opt-in:
        // nothing runs unless a mesh is registered + a palette uploaded each frame.
        if (!createSkinning()) return false;
        return true;
    }

    void shutdown() override {
        if (m_dev.device) vkDeviceWaitIdle(m_dev.device);
        destroySkinning();
        destroyDebris();
        destroyParticles();
        destroyWater();
        destroyGi();
        destroySsao();
        destroyPost();
        destroySky();
        destroyHud();
        destroyGraphics();
        destroyPerFrame();
        if (m_headless) destroyOffscreenTarget();
        else            destroySwapchain();
        // Fix 6 (a): standing GPU-allocation leak guard. By this point EVERYTHING
        // the renderer allocated through VMA should be freed; ask VMA how many live
        // allocations remain. Log the count always; in Debug ASSERT it is zero so a
        // leaked image/buffer (one that survived teardown) is caught immediately.
        if (m_alloc) {
            VmaTotalStatistics vmaStats{};
            vmaCalculateStatistics(m_alloc, &vmaStats);
            const uint32_t liveAllocs = vmaStats.total.statistics.allocationCount;
            char msg[160];
            std::snprintf(msg, sizeof(msg),
                "[rhi] VMA shutdown leak check: live allocationCount=%u (expect 0)",
                liveAllocs);
            if (liveAllocs == 0) logInfo(msg);
            else                 logError(msg);
            assert(liveAllocs == 0 &&
                   "VMA leak: GPU allocations survived renderer teardown");
            vmaDestroyAllocator(m_alloc); m_alloc = nullptr;
        }
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

    void setVsync(bool enabled) override {
        // Headless has no swapchain: just record the desired state. Windowed:
        // flag a recreate so createSwapchain() picks the new present mode. No-op if
        // the value is unchanged (avoids a pointless device-idle stall).
        if (enabled == m_vsync) return;
        m_vsync = enabled;
        if (!m_headless) m_needsRecreate = true;
    }

    void setCamera(float x, float y, float z, float yaw, float pitch, float fovDeg) override {
        m_camPos = glm::vec3(x, y, z);
        m_camYaw = yaw; m_camPitch = pitch; m_camFov = fovDeg;
    }

    void setPointLights(const PointLight* lights, uint32_t count) override {
        // Copy a clamped snapshot (we never retain the caller's pointer). The
        // cached set is re-uploaded into each frame's UBO by prepareFrameData, so
        // static lights only need one call. count==0 clears them.
        const uint32_t n = std::min<uint32_t>(count, kMaxPointLights);
        m_pointLights.assign(lights, lights + n);
    }

    void setSkyParams(const SkyParams& sp) override {
        // Cache a snapshot; prepareFrameData() writes it into the per-frame sky UBO
        // and ensureMainPass() draws the full-screen sky when enabled. Disabled by
        // default, so indoor levels + every existing flag are unchanged.
        m_sky = sp;
    }

    // Project a world point -> HUD pixel coords (top-left origin) using the cached render
    // viewProj. false if behind the camera / well off-screen. For monster health bars etc.
    bool rayTracingSupported() const override { return m_rtSupported; }

    bool worldToScreen(float wx, float wy, float wz, float& sx, float& sy) const override {
        const glm::vec4 clip = m_lastViewProj * glm::vec4(wx, wy, wz, 1.0f);
        if (clip.w <= 1e-4f) return false;
        const float nx = clip.x / clip.w, ny = clip.y / clip.w;
        if (nx < -1.3f || nx > 1.3f || ny < -1.3f || ny > 1.3f) return false;
        sx = (nx * 0.5f + 0.5f) * (float)m_extent.width;
        sy = (ny * 0.5f + 0.5f) * (float)m_extent.height;
        return true;
    }

    void setSsaoParams(const SsaoParams& sp) override {
        // Cache a snapshot; prepareFrameData() bakes radius/bias/intensity/power
        // into the per-frame SSAO UBO + the mesh.frag control block, and
        // buildAndExecuteGraph gates the SSAO chain on `enabled`. Enabled by
        // default with tasteful values (no app wiring required for it to work).
        m_ssao = sp;
    }

    void setWaterParams(const WaterParams& wp) override {
        // Cache a snapshot; prepareFrameData() writes it into the per-frame water
        // UBO and buildAndExecuteGraph adds the water pass when enabled. Disabled
        // by default, so indoor levels + every existing flag are unchanged.
        m_water = wp;
    }

    void setGiParams(const GiParams& gp) override {
        // Cache a snapshot; prepareFrameData() bakes the tunables into the per-frame
        // GI UBO + temporal UBO, and buildAndExecuteGraph gates the GI chain on
        // `enabled`. Enabled by default with tasteful values (no app wiring required
        // for it to work, exactly like SSAO).
        m_gi = gp;
    }

    // ---- Particles + decals (combat juice) ---------------------------------
    // Append this frame's particle instances into the additive / alpha CPU staging
    // buffers (cleared each beginFrame). The buffers are FIXED-capacity (reserved
    // once at init to kMaxParticles); appends past the cap are dropped — NO per-
    // frame heap alloc. prepareFrameData() uploads them into the per-frame instance
    // ring and buildAndExecuteGraph adds the particle pass when any are present.
    void submitParticles(const ParticleInstance* instances, uint32_t count,
                         ParticleBlend mode) override {
        if (!instances || count == 0) return;
        std::vector<ParticleInstance>& dst =
            (mode == ParticleBlend::Alpha) ? m_partAlpha : m_partAdd;
        for (uint32_t i = 0; i < count; ++i) {
            if (dst.size() >= kMaxParticles) break;   // bounded; drop the overflow
            dst.push_back(instances[i]);
        }
    }
    void submitDecals(const DecalInstance* decals, uint32_t count) override {
        if (!decals || count == 0) return;
        for (uint32_t i = 0; i < count; ++i) {
            if (m_decals.size() >= kMaxDecals) break;
            m_decals.push_back(decals[i]);
        }
    }

    FrameContext beginFrame() override {
        FrameContext fc{};
        if (m_needsRecreate) {
            if (m_headless) recreateOffscreenTarget(); else recreateSwapchain();
            m_needsRecreate = false;
        }
        // Windowed needs a live swapchain; headless needs a live offscreen image.
        if (!m_headless && m_swapchain == VK_NULL_HANDLE) return fc;
        if (m_headless && m_offscreenColorImg == VK_NULL_HANDLE) return fc;

        auto& fr = m_frames[m_frameIdx];
        vkWaitForFences(m_dev.device, 1, &fr.inFlight, VK_TRUE, UINT64_MAX);

        // Retire any deferred buffer frees whose referencing frames have now
        // completed (fix 2: promoting a mesh to dynamic queues its old vbo here).
        drainPendingFrees();

        // The fence above retired this ring slot's PREVIOUS submission, so its
        // timestamps (written kFramesInFlight frames ago) are now guaranteed
        // available — read them back without a stall, then recycle the pool.
        if (m_tsSupported && fr.tsPending) {
            uint64_t ticks[2] = { 0, 0 };
            VkResult qr = vkGetQueryPoolResults(m_dev.device, fr.tsPool, 0, 2,
                sizeof(ticks), ticks, sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT);
            if (qr == VK_SUCCESS) {
                uint64_t t0 = ticks[0] & m_tsValidMask;
                uint64_t t1 = ticks[1] & m_tsValidMask;
                if (t1 >= t0)
                    m_lastGpuMs = (float)((t1 - t0) * (double)m_tsPeriodNs * 1e-6);
            }
            fr.tsPending = false;
        }

        // HEADLESS: there is no swapchain to acquire from — the single offscreen
        // color image is the only "backbuffer" and is always available once its
        // ring slot's prior submission has retired (the fence wait above). So we
        // skip vkAcquireNextImageKHR entirely and the imageAvailable semaphore is
        // not signalled/waited (endFrame() submits with no wait semaphore headless).
        uint32_t imageIndex = 0;
        if (!m_headless) {
            VkResult acq = vkAcquireNextImageKHR(m_dev.device, m_swapchain, UINT64_MAX,
                                                 fr.imageAvailable, VK_NULL_HANDLE, &imageIndex);
            if (acq == VK_ERROR_OUT_OF_DATE_KHR) { m_needsRecreate = true; return fc; }
            if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) { logError("[rhi] acquire failed"); return fc; }
        }

        vkResetFences(m_dev.device, 1, &fr.inFlight);
        vkResetCommandPool(m_dev.device, fr.pool, 0);

        // Start accumulating this frame's counters fresh.
        m_building = RenderStats{};

        // This frame's GPU work has retired (we waited on inFlight): it's safe to
        // overwrite its per-frame object SSBO / camera UBO / indirect rings and to
        // recycle the HUD descriptor pool + HUD vertex ring.
        m_drawRecords.clear();
        // Particle/decal per-frame staging (capacity persists -> no heap churn).
        m_partAdd.clear();
        m_partAlpha.clear();
        m_decals.clear();
        m_partAddCount = m_partAlphaCount = m_decalCount = 0;
        // Debris compute/draw are re-armed per frame by gpuDebrisStep/gpuDebrisDraw.
        m_debrisStepPending = false;
        m_debrisDrawPending = false;
        // GPU skinning is re-armed per frame by setSkinnedPalette().
        m_skinPending.clear();
        m_skinStepPending = false;
        m_framePrepared = false;
        m_frameCmdCount = 0;
        if (fr.hudDescPool) vkResetDescriptorPool(m_dev.device, fr.hudDescPool, 0);
        fr.hudVertsUsed = 0;

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(fr.cmd, &bi);

        // Recycle this frame's timestamp queries before re-writing them. Resetting
        // on the command buffer (vs host) keeps the pool's availability state
        // VUID-correct: every query is reset, then written exactly once below.
        if (m_tsSupported && fr.tsPool)
            vkCmdResetQueryPool(fr.cmd, fr.tsPool, 0, 2);

        // Timestamp the start of the frame at TOP_OF_PIPE; paired with the
        // BOTTOM_OF_PIPE stamp at endFrame this brackets the whole frame (shadow
        // depth pass + main scene + HUD). (sync2 vkCmdWriteTimestamp2.)
        if (m_tsSupported && fr.tsPool)
            vkCmdWriteTimestamp2(fr.cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, fr.tsPool, 0);

        // Per-frame render-graph build (perf-stack B): all pass barriers + layout
        // transitions + begin/endRendering are now DERIVED + emitted by the graph
        // in endFrame() from each pass's declared resource reads/writes. beginFrame
        // no longer hand-codes the swapchain/depth UNDEFINED transitions — the
        // graph's color pass declares them. Reset the transient graph build here.
        m_graph.beginFrame();
        m_hudRecords.clear();
        m_curImageIndex = imageIndex;

        fc.frameIndex = m_frameIdx;
        fc.cmd = reinterpret_cast<uint64_t>(fr.cmd);
        fc.backbuffer = imageIndex;
        fc.valid = true;
        return fc;
    }

    // Compute the sun's ortho viewProj for this frame. Single map (no CSM): an
    // ortho box of half-extent kShadowOrtho centered on the camera position, with
    // the light positioned kShadowDepthHalf back along the sun direction. The box
    // follows the camera so the visible ~60 m level is always covered. Matches the
    // sun L in mesh.frag: normalize(0.4, 1.0, 0.3).
    glm::mat4 computeLightViewProj() const {
        const glm::vec3 sunDir = glm::normalize(glm::vec3(0.4f, 1.0f, 0.3f));
        const glm::vec3 center = m_camPos;
        const glm::vec3 eye = center + sunDir * kShadowDepthHalf;
        // Up vector not parallel to sunDir.
        const glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
        glm::vec3 upPick = (std::abs(glm::dot(sunDir, up)) > 0.99f) ? glm::vec3(0,0,1) : up;
        glm::mat4 view = glm::lookAt(eye, center, upPick);
        // Ortho with Vulkan's [0,1] Z (GLM_FORCE_DEPTH_ZERO_TO_ONE), reverse-Y clip.
        glm::mat4 proj = glm::ortho(-kShadowOrtho, kShadowOrtho,
                                    -kShadowOrtho, kShadowOrtho,
                                    0.0f, 2.0f * kShadowDepthHalf);
        proj[1][1] *= -1.0f;
        return proj * view;
    }

    // Record the BODY of the main color pass into `cmd` (the graph has already
    // begun dynamic rendering + emitted the swapchain/depth/shadow barriers). This
    // draws: the analytic sky (if enabled), the deferred mesh multidraw, then the
    // deferred HUD overlay — in exactly the order the hand-coded path used.
    void recordMainPassBody(VkCommandBuffer cmd) {
        auto& fr = m_frames[m_frameIdx];

        VkViewport vp{ 0.0f, 0.0f, (float)m_extent.width, (float)m_extent.height, 0.0f, 1.0f };
        VkRect2D scis{ {0,0}, m_extent };
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &scis);

        // Analytic sky FIRST (open-world track, task A): a full-screen triangle at
        // far depth with depth-test LESS_OR_EQUAL + depth-write OFF. Drawn before
        // any mesh so opaque geometry (depth-test LESS, depth-write ON) overwrites
        // it wherever it's nearer — the sky composites correctly behind the world.
        // Gated by setSkyParams(enabled): default OFF, so indoor levels are
        // unchanged (the dark-slate clear still shows where no geometry exists).
        if (m_sky.enabled && m_skyPipeline && fr.skyMapped) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skyPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skyLayout,
                                    0, 1, &fr.skySet, 0, nullptr);
            vkCmdDraw(cmd, 3, 1, 0, 0); // vertexless full-screen triangle
        }

        // Mesh multidraw (Subsystem D) into the linear HDR scene target. The HUD is
        // NO LONGER drawn here — in the HDR pipeline it is drawn AFTER tonemap, in
        // the composite pass, so it composites on the final LDR image (not bloomed
        // and not double-tonemapped).
        recordMeshDraws(cmd);
    }

    void endFrame(const FrameContext& fc) override {
        if (!fc.valid) return;
        auto& fr = m_frames[m_frameIdx];
        uint32_t imageIndex = fc.backbuffer;

        // ===================================================================
        // Build + execute the render graph (perf-stack B). The whole per-frame
        // GPU command sequence — shadow depth pass, main color pass (sky + mesh
        // multidraw + HUD), and the present/capture finalize — is expressed as
        // graph nodes that declare their resource reads/writes. The graph derives
        // and emits every sync2 barrier + layout transition from those
        // declarations and drives vkCmdBeginRendering/EndRendering. NO barriers or
        // begin/end-rendering are hand-coded in this per-frame path anymore.
        // ===================================================================
        prepareFrameData();  // fill camera/light/sky UBO + SSBO + indirect (data only)

        const bool wantCapture = (m_captureArmed && m_captureBuf &&
                                  m_captureW == m_extent.width && m_captureH == m_extent.height);
        buildAndExecuteGraph(fr.cmd, imageIndex, wantCapture);
        const bool capturedThisFrame = wantCapture;
        m_captureArmed = false; // consume the arm regardless

        vkEndCommandBuffer(fr.cmd);

        // HEADLESS: no acquire semaphore to wait on and no present, so submit with
        // NO wait/signal semaphores. The inFlight fence alone serializes the ring
        // slot (beginFrame waits it before reusing the slot's offscreen image +
        // command buffer). WINDOWED keeps the acquire-wait + renderFinished-signal
        // that the present below consumes.
        VkSemaphoreSubmitInfo waitS{};
        waitS.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        waitS.semaphore = fr.imageAvailable;
        waitS.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSemaphoreSubmitInfo signalS{};
        signalS.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signalS.semaphore = m_headless ? VK_NULL_HANDLE : m_renderFinished[imageIndex];
        signalS.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;

        VkCommandBufferSubmitInfo cmdS{};
        cmdS.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmdS.commandBuffer = fr.cmd;

        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.waitSemaphoreInfoCount   = m_headless ? 0u : 1u;
        submit.pWaitSemaphoreInfos      = m_headless ? nullptr : &waitS;
        submit.commandBufferInfoCount   = 1;   submit.pCommandBufferInfos = &cmdS;
        submit.signalSemaphoreInfoCount = m_headless ? 0u : 1u;
        submit.pSignalSemaphoreInfos    = m_headless ? nullptr : &signalS;
        vkQueueSubmit2(m_gfxQueue, 1, &submit, fr.inFlight);

        // Fix 1: if this frame recorded the capture copy, remember which fence to
        // wait on (this frame's inFlight) so captureFrame() can finalize the PNG.
        if (capturedThisFrame) {
            m_captureReady = true;
            m_captureFence = fr.inFlight;
            m_captureFrameSlot = m_frameIdx;
        }

        // HEADLESS: "present" is a no-op — there is no swapchain. The offscreen
        // image's final state was set by the graph's finalize pass and the readback
        // (if any) already copied it. WINDOWED presents the acquired image.
        if (!m_headless) {
            VkPresentInfoKHR present{};
            present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            present.waitSemaphoreCount = 1;
            present.pWaitSemaphores = &m_renderFinished[imageIndex];
            present.swapchainCount = 1;
            present.pSwapchains = &m_swapchain;
            present.pImageIndices = &imageIndex;
            VkResult pr = vkQueuePresentKHR(m_gfxQueue, &present);
            if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) m_needsRecreate = true;
        }

        m_frameIdx = (m_frameIdx + 1) % kFramesInFlight;
        ++m_totalFrames;

        // Snapshot this frame's counters + the latest GPU time for stats().
        m_building.gpuFrameMs = m_lastGpuMs;
        m_building.frameCount = m_totalFrames;
        m_lastStats = m_building;
    }

    RenderStats stats() const override { return m_lastStats; }

    // ---- Offscreen capture (--screenshot) ----------------------------------
    // Step 1: arm a capture for the NEXT frame. endFrame() will record the color-
    // image -> host readback copy inside that frame's live command buffer, reading
    // the freshly-rendered, properly-acquired swapchain image (no non-acquired
    // image is ever touched). Allocates the host-visible readback buffer up front.
    void armCapture(const char* path) override {
        (void)path; // path is consumed by captureFrame() at finalize time
        const bool ready = m_headless ? (m_offscreenColorImg != VK_NULL_HANDLE)
                                      : (m_swapchain && !m_swapImages.empty());
        if (!ready) {
            logError("[rhi] armCapture: not ready (no render target)"); return;
        }
        const uint32_t W = m_extent.width, H = m_extent.height;
        if (W == 0 || H == 0) { logError("[rhi] armCapture: zero extent"); return; }

        // (Re)allocate the readback buffer if the extent changed or it's the first
        // arm. Tightly packed: vkCmdCopyImageToBuffer(bufferRowLength=0) packs rows
        // to image width, so pitch == W*4.
        if (m_captureBuf && (m_captureW != W || m_captureH != H)) {
            vmaDestroyBuffer(m_alloc, m_captureBuf, m_captureAlloc);
            m_captureBuf = VK_NULL_HANDLE; m_captureAlloc = nullptr; m_captureMapped = nullptr;
        }
        if (!m_captureBuf) {
            const VkDeviceSize bytes = (VkDeviceSize)W * H * 4;
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = bytes; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VmaAllocationCreateInfo vaci{};
            vaci.usage = VMA_MEMORY_USAGE_AUTO;
            vaci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo rinfo{};
            if (vmaCreateBuffer(m_alloc, &bci, &vaci, &m_captureBuf, &m_captureAlloc, &rinfo) != VK_SUCCESS) {
                logError("[rhi] armCapture: readback buffer alloc failed");
                m_captureBuf = VK_NULL_HANDLE; return;
            }
            m_captureMapped = rinfo.pMappedData;
        }
        m_captureW = W; m_captureH = H;
        m_captureArmed = true;
        m_captureReady = false;
    }

    // (The in-frame capture copy is now expressed as graph passes — see
    // buildAndExecuteGraph()'s "capture-copy"/"present" nodes. The graph derives
    // the COLOR_ATTACHMENT->TRANSFER_SRC->PRESENT_SRC transitions automatically.)

    // Step 3: finalize an armed capture. Wait on the captured frame's inFlight
    // fence (its copy has retired), map the readback buffer, swizzle (BGRA->RGBA)
    // and write the PNG. If no capture was armed/recorded, fall back to the legacy
    // self-contained "last-presented image" copy (idles the device first). Returns
    // true on success.
    bool captureFrame(const char* path) override {
        if (!path) return false;
        if (m_captureReady) {
            // Wait for the frame that recorded the copy to retire (its fence).
            if (m_captureFence)
                vkWaitForFences(m_dev.device, 1, &m_captureFence, VK_TRUE, UINT64_MAX);
            const bool ok = writeCapturePng(path, m_captureMapped, m_captureW, m_captureH);
            m_captureReady = false;
            return ok;
        }
        // Legacy fallback path (no arm): copy the last presented image. This still
        // works for callers that don't use armCapture (kept for API safety).
        return legacyCaptureLastPresented(path);
    }

    // Swizzle the mapped readback bytes to tightly-packed RGBA8 and write the PNG.
    bool writeCapturePng(const char* path, const void* mapped, uint32_t W, uint32_t H) {
        if (!mapped || W == 0 || H == 0) {
            logError("[rhi] captureFrame: no captured data"); return false;
        }
        // The default swapchain format is B8G8R8A8_UNORM (raw bytes are exactly what
        // is shown) — swap B<->R. Handle R/B-already-RGBA formats too.
        const bool bgra = (m_format == VK_FORMAT_B8G8R8A8_UNORM ||
                           m_format == VK_FORMAT_B8G8R8A8_SRGB);
        const uint8_t* src = static_cast<const uint8_t*>(mapped);
        std::vector<uint8_t> rgba((size_t)W * H * 4);
        for (size_t p = 0; p < (size_t)W * H; ++p) {
            const uint8_t* s = src + p * 4;
            uint8_t* d = rgba.data() + p * 4;
            if (bgra) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; }
            else      { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
            d[3] = 255;  // force opaque (swapchain alpha is undefined for display)
        }
        const int rc = stbi_write_png(path, (int)W, (int)H, 4, rgba.data(), (int)(W * 4));
        if (rc == 0) { logError(std::string("[rhi] captureFrame: PNG write failed: ") + path); return false; }
        logInfo(std::string("[rhi] captureFrame: wrote ") + path + " (" +
                std::to_string(W) + "x" + std::to_string(H) + ")");
        return true;
    }

    // Legacy: copy the LAST presented image via a self-contained one-time-submit.
    // Idles the device, transitions PRESENT_SRC -> TRANSFER_SRC -> PRESENT_SRC.
    // Retained as a fallback for callers that don't arm a capture in-frame.
    bool legacyCaptureLastPresented(const char* path) {
        if (!m_swapchain || m_swapImages.empty()) {
            logError("[rhi] captureFrame: not ready (no swapchain)"); return false;
        }
        const uint32_t imageIndex = m_curImageIndex;
        if (imageIndex >= m_swapImages.size()) {
            logError("[rhi] captureFrame: bad image index"); return false;
        }
        const uint32_t W = m_extent.width, H = m_extent.height;
        if (W == 0 || H == 0) { logError("[rhi] captureFrame: zero extent"); return false; }
        vkDeviceWaitIdle(m_dev.device);

        const VkDeviceSize bytes = (VkDeviceSize)W * H * 4;
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = bytes; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo vaci{};
        vaci.usage = VMA_MEMORY_USAGE_AUTO;
        vaci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer readback = VK_NULL_HANDLE; VmaAllocation readbackAlloc = nullptr; VmaAllocationInfo rinfo{};
        if (vmaCreateBuffer(m_alloc, &bci, &vaci, &readback, &readbackAlloc, &rinfo) != VK_SUCCESS) {
            logError("[rhi] captureFrame: readback buffer alloc failed"); return false;
        }
        VkImage img = m_swapImages[imageIndex];
        bool ok = oneTimeSubmit([&](VkCommandBuffer cmd){
            imageBarrier(cmd, img,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            VkBufferImageCopy region{};
            region.bufferRowLength = 0; region.bufferImageHeight = 0;
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { W, H, 1 };
            vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   readback, 1, &region);
            imageBarrier(cmd, img,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
        });
        if (!ok) {
            vmaDestroyBuffer(m_alloc, readback, readbackAlloc);
            logError("[rhi] captureFrame: copy submit failed"); return false;
        }
        const bool wrote = writeCapturePng(path, rinfo.pMappedData, W, H);
        vmaDestroyBuffer(m_alloc, readback, readbackAlloc);
        return wrote;
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
        m.vertexCount = vcount;
        uint32_t id = m_nextMeshId++;
        m_meshes.emplace(id, m);
        return { id };
    }

    void destroyMesh(MeshHandle h) override {
        auto it = m_meshes.find(h.id);
        if (it == m_meshes.end()) return;
        // Fix 2: NO vkDeviceWaitIdle. The mesh's buffers may still be referenced by
        // command buffers from up to kFramesInFlight-1 earlier frames, so DEFER the
        // free to drainPendingFrees() (retired after kFramesInFlight frames begin).
        // During terrain-streaming eviction this turns dozens of full-GPU stalls per
        // boundary-cross into a few cheap queue pushes. The mesh is erased from the
        // registry immediately, so no future frame can issue a draw referencing it.
        Mesh& m = it->second;
        if (m.dynamic) {
            for (uint32_t i = 0; i < kFramesInFlight; ++i)
                deferDestroyBuffer(m.dynVbo[i], m.dynVboAlloc[i]);
        } else if (m.vbo) {
            deferDestroyBuffer(m.vbo, m.vboAlloc);
        }
        deferDestroyBuffer(m.ibo, m.iboAlloc);
        // GPU-skinning resources keyed to this mesh (if any). The descriptor sets
        // reference the dynVbo we just deferred, so wait idle before freeing them
        // (eviction is off the hot path; this matches unregisterSkinnedMesh).
        auto skIt = m_skinnedMeshes.find(h.id);
        if (skIt != m_skinnedMeshes.end()) {
            if (m_dev.device) vkDeviceWaitIdle(m_dev.device);
            destroySkinnedMeshResources(skIt->second);
            m_skinnedMeshes.erase(skIt);
            for (auto p = m_skinPending.begin(); p != m_skinPending.end(); )
                p = (*p == h.id) ? m_skinPending.erase(p) : p + 1;
        }
        m_meshes.erase(it);
        // Drop this mesh's draw-group key so m_groups doesn't accumulate dead
        // entries over a long terrain-streaming session (fix 4).
        m_groups.erase(h.id);
    }

    // ---- Dynamic mesh re-upload (CPU skinning, J1; fix 2 frames-in-flight) ---
    // Overwrite an existing mesh's vertices in place for the frame CURRENTLY being
    // recorded (m_frameIdx). On the first call the mesh is promoted to dynamic:
    // kFramesInFlight HOST_VISIBLE, persistently-mapped vertex buffers are
    // allocated and seeded from the static buffer's source so every frame slot has
    // valid contents even before its first per-slot write. Each call then writes
    // ONLY the buffer for m_frameIdx — which the GPU finished reading kFramesInFlight
    // frames ago (guaranteed by the inFlight fence waited in beginFrame), so there
    // is NO device wait and NO write-while-read hazard. The draw path binds the
    // matching per-frame buffer (Mesh::drawVbo(m_frameIdx)). Called per skinned
    // character per frame; scales to many NPCs without a GPU stall.
    void updateMesh(MeshHandle h, const MeshVertex* verts, uint32_t vcount) override {
        if (!verts || vcount == 0) return;
        auto it = m_meshes.find(h.id);
        if (it == m_meshes.end()) return;
        Mesh& m = it->second;
        if (vcount != m.vertexCount) return;  // count must match the original mesh
        const VkDeviceSize bytes = (VkDeviceSize)vcount * sizeof(MeshVertex);

        if (!m.dynamic) {
            // Promote: allocate kFramesInFlight HOST_VISIBLE mapped vbos and seed
            // each with the incoming vertices (so any frame slot the draw path may
            // bind before its own first write still holds a valid pose). The
            // original DEVICE_LOCAL vbo is freed last; it is no longer referenced by
            // any draw once `dynamic` is set (the draw path reads drawVbo()). It can
            // still be in-flight from earlier frames, so we DON'T free it until all
            // its referencing frames have retired — defer it to deferDestroyBuffer.
            bool allocFailed = false;
            for (uint32_t i = 0; i < kFramesInFlight; ++i) {
                VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                bci.size = bytes; bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
                VmaAllocationCreateInfo vaci{};
                vaci.usage = VMA_MEMORY_USAGE_AUTO;
                vaci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                             VMA_ALLOCATION_CREATE_MAPPED_BIT;
                VmaAllocationInfo ai{};
                VkBuffer nb = VK_NULL_HANDLE; VmaAllocation na = nullptr;
                if (vmaCreateBuffer(m_alloc, &bci, &vaci, &nb, &na, &ai) != VK_SUCCESS) {
                    logError("[rhi] updateMesh: dynamic vbo alloc failed"); allocFailed = true; break;
                }
                void* mapped = ai.pMappedData;
                if (!mapped && vmaMapMemory(m_alloc, na, &mapped) != VK_SUCCESS) {
                    vmaDestroyBuffer(m_alloc, nb, na);
                    logError("[rhi] updateMesh: dynamic vbo map failed"); allocFailed = true; break;
                }
                m.dynVbo[i] = nb; m.dynVboAlloc[i] = na; m.dynMapped[i] = mapped;
                std::memcpy(mapped, verts, (size_t)bytes);          // seed all slots
                vmaFlushAllocation(m_alloc, na, 0, bytes);
            }
            if (allocFailed) {
                // Roll back any partial allocation; leave the mesh static + intact.
                for (uint32_t i = 0; i < kFramesInFlight; ++i) {
                    if (m.dynVbo[i]) {
                        if (m.dynMapped[i]) vmaUnmapMemory(m_alloc, m.dynVboAlloc[i]);
                        vmaDestroyBuffer(m_alloc, m.dynVbo[i], m.dynVboAlloc[i]);
                        m.dynVbo[i] = VK_NULL_HANDLE; m.dynVboAlloc[i] = nullptr; m.dynMapped[i] = nullptr;
                    }
                }
                return;
            }
            // Defer-free the old static vbo (may still be read by in-flight frames).
            deferDestroyBuffer(m.vbo, m.vboAlloc);
            m.vbo = VK_NULL_HANDLE; m.vboAlloc = nullptr;
            m.dynamic = true;
            // We already seeded the current frame's slot above; done.
            return;
        }

        // Steady state: write ONLY the current frame's buffer. The inFlight fence
        // waited in beginFrame guarantees the GPU finished reading this slot's
        // buffer (last bound kFramesInFlight frames ago), so this overwrite cannot
        // race a GPU read — no device wait, no WAR/RAW hazard.
        const uint32_t fi = m_frameIdx;
        std::memcpy(m.dynMapped[fi], verts, (size_t)bytes);
        // HOST_VISIBLE allocations from VMA_MEMORY_USAGE_AUTO may not be coherent;
        // flush so the GPU sees the write (no-op if host-coherent).
        vmaFlushAllocation(m_alloc, m.dynVboAlloc[fi], 0, bytes);
    }

    TextureHandle createTexture(const void* rgba8, uint32_t w, uint32_t h, bool srgb) override {
        if (!rgba8 || w == 0 || h == 0) return {};
        Texture t{};
        if (!createSampledTexture(rgba8, w, h, srgb, t)) return {};
        // Grab a stable bindless slot and write it into the bindless array. If the
        // array is full the texture still exists but falls back to white (index 0).
        if (!registerBindless(t)) {
            x3::logError("[rhi] bindless texture array full; new texture uses white");
            t.bindlessIndex = 0;
        }
        uint32_t id = m_nextTexId++;
        m_textures.emplace(id, t);
        return { id };
    }

    void destroyTexture(TextureHandle h) override {
        auto it = m_textures.find(h.id);
        if (it == m_textures.end()) return;
        // Fix 2: NO vkDeviceWaitIdle. The bindless-slot write-back to the default
        // white texture MUST happen immediately (a host-side vkUpdateDescriptorSets
        // is safe even while frames are in flight — it only rewrites the descriptor
        // the NEXT frame reads; the in-flight frame already captured its handles).
        // The ACTUAL image/view/sampler destruction is deferred to drainPendingFrees,
        // because earlier in-flight frames may still sample the old view. This avoids
        // a full-GPU stall per texture eviction during terrain streaming.
        Texture& t = it->second;
        const uint32_t slot = t.bindlessIndex;
        if (slot != 0 && m_whiteTex.view) writeBindlessSlot(slot, m_whiteTex);
        deferDestroyImage(t.image, t.alloc, t.view, t.sampler);
        m_textures.erase(it);
    }

    // ---- Terrain material splat (open-world ground) -----------------------
    // Resolve the four detail textures' bindless indices, cache them, and hand
    // back a synthetic MARKER handle. drawMeshEmissive() recognises this exact
    // handle id and flags the per-object SSBO row as terrain, packing the four
    // indices into the previously-reserved pad fields. No GPU resource is created
    // here (the marker is purely a CPU sentinel), so there is nothing to destroy.
    TextureHandle registerTerrainMaterial(TextureHandle grass, TextureHandle rock,
                                          TextureHandle snow,  TextureHandle sand) override {
        auto idxOf = [this](TextureHandle h) -> uint32_t {
            if (!h.valid()) return 0;
            auto it = m_textures.find(h.id);
            return (it != m_textures.end()) ? it->second.bindlessIndex : 0u;
        };
        if (!grass.valid() || !rock.valid() || !snow.valid() || !sand.valid())
            return {};                              // invalid set -> flat fallback
        m_terrainTexIdx[0] = idxOf(grass);
        m_terrainTexIdx[1] = idxOf(rock);
        m_terrainTexIdx[2] = idxOf(snow);
        m_terrainTexIdx[3] = idxOf(sand);
        // Allocate a marker id that can never collide with a real texture id
        // (createTexture hands out m_nextTexId++ starting at 1). A high reserved
        // value keeps the two id spaces disjoint without touching m_nextTexId.
        m_terrainMarkerId = 0xFFFF0001u;
        return TextureHandle{ m_terrainMarkerId };
    }

    void drawMesh(const FrameContext& fc, MeshHandle mesh, TextureHandle baseColor,
                  const float baseColorFactor[4], const float model[16]) override {
        // The 5-arg form is the no-emissive case (emissive = {0,0,0,0}).
        const float noEmissive[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        drawMeshEmissive(fc, mesh, baseColor, baseColorFactor, noEmissive, model);
    }

    void drawMeshEmissive(const FrameContext& fc, MeshHandle mesh, TextureHandle baseColor,
                          const float baseColorFactor[4], const float emissive[4],
                          const float model[16]) override {
        if (!fc.valid || !m_meshPipeline) return;
        // GPU-driven path: drawMesh records NO commands and binds NO descriptors.
        // It appends a CPU record; endFrame() groups by mesh + emits multidraw-
        // indirect. This is the CPU win (no per-draw vkAllocate/vkUpdate/vkCmd*).
        ++m_building.objectsSubmitted;
        auto mit = m_meshes.find(mesh.id);
        if (mit == m_meshes.end()) return;          // unknown mesh -> skip
        if (m_drawRecords.size() >= kMaxDrawsPerFrame) return; // ring full; skip safely

        // Resolve the bindless texture index (0 == built-in white default).
        // A draw that uses the terrain MARKER handle is flagged as terrain: the
        // fragment shader splats grass/rock/snow/sand by height+slope instead of
        // sampling textures[texIndex], and the four detail indices ride along in
        // the pad fields. All other draws set terrainFlag = 0 and are unchanged.
        uint32_t texIndex = 0;
        uint32_t terrainFlag = 0;
        if (m_terrainMarkerId != 0 && baseColor.id == m_terrainMarkerId) {
            terrainFlag = 1;
            texIndex    = m_terrainTexIdx[0];   // grass index (sane default sample)
        } else if (baseColor.valid()) {
            auto tit = m_textures.find(baseColor.id);
            if (tit != m_textures.end()) texIndex = tit->second.bindlessIndex;
        }

        DrawRecord r;
        r.meshId      = mesh.id;
        r.texIndex    = texIndex;
        r.terrainFlag = terrainFlag;
        // Pack the four detail indices into two uints: pad1 = grass<<16 | rock,
        // pad2 = snow<<16 | sand (each well under 65535 — kMaxTextures = 4096).
        r.terrainPack1 = (m_terrainTexIdx[0] << 16) | (m_terrainTexIdx[1] & 0xFFFFu);
        r.terrainPack2 = (m_terrainTexIdx[2] << 16) | (m_terrainTexIdx[3] & 0xFFFFu);
        std::memcpy(r.model, model, sizeof(r.model));
        if (baseColorFactor) std::memcpy(r.factor, baseColorFactor, sizeof(r.factor));
        else { r.factor[0] = r.factor[1] = r.factor[2] = r.factor[3] = 1.0f; }
        if (emissive) std::memcpy(r.emissive, emissive, sizeof(r.emissive));
        else { r.emissive[0] = r.emissive[1] = r.emissive[2] = r.emissive[3] = 0.0f; }
        m_drawRecords.push_back(r);
    }

    // ---- Screen-space 2D HUD overlay (S7) ----------------------------------
    void drawHudQuad(const FrameContext& fc, float xPx, float yPx,
                     float wPx, float hPx, const float rgba[4]) override {
        if (!fc.valid || !m_hudPipeline) return;
        const float c[4] = { rgba ? rgba[0] : 1.0f, rgba ? rgba[1] : 1.0f,
                             rgba ? rgba[2] : 1.0f, rgba ? rgba[3] : 1.0f };
        // Whole-quad UV (0,0)-(1,1) samples the 1x1 white texel everywhere.
        HudVertex verts[6];
        emitQuad(verts, xPx, yPx, wPx, hPx, 0.0f, 0.0f, 1.0f, 1.0f, c);
        flushHud(verts, 6, /*texFont=*/-1);
    }

    // Back-compat: render with the DEFAULT mono role (Console/HudMono — embedded
    // Roboto Mono). All existing non-UI callers route here unchanged.
    void drawHudText(const FrameContext& fc, const char* text, float xPx,
                     float yPx, float pxPerGlyph, const float rgba[4]) override {
        drawHudTextF(fc, x3::rhi::FontRole::Console, text, xPx, yPx, pxPerGlyph, rgba);
    }

    // Role-aware HUD text. Picks the role's baked atlas; PROPORTIONAL roles advance
    // by each glyph's real width, monospace roles advance by a fixed cell. Falls
    // back to the 8x8 bitmap path only if no TTF baked at all (NEVER blank text).
    void drawHudTextF(const FrameContext& fc, x3::rhi::FontRole role, const char* text,
                      float xPx, float yPx, float px, const float rgba[4]) override {
        if (!fc.valid || !m_hudPipeline || !text || px <= 0.0f) return;
        const float c[4] = { rgba ? rgba[0] : 1.0f, rgba ? rgba[1] : 1.0f,
                             rgba ? rgba[2] : 1.0f, rgba ? rgba[3] : 1.0f };
        const int r = (int)role;
        if (r >= 0 && r < kFontRoleCount && m_fonts[r].ready) {
            drawHudTextAtlas(r, text, xPx, yPx, px, c);
            return;
        }
        // ---- Legacy 8x8 bitmap fallback (only if NO TTF baked for this role) ---
        // Atlas layout: 16 cols x 8 rows of 8x8 glyphs in a 128x64 texture.
        constexpr float kCols = 16.0f, kRows = 8.0f;
        constexpr float kCellU = 1.0f / kCols, kCellV = 1.0f / kRows;
        constexpr float kInsetU = (7.0f / 8.0f) * kCellU; // 7 of 8 columns used
        constexpr float kInsetV = (7.0f / 8.0f) * kCellV;

        m_hudScratch.clear();
        float penX = xPx, penY = yPx;
        for (const char* p = text; *p; ++p) {
            unsigned char ch = static_cast<unsigned char>(*p);
            if (ch == '\n') { penX = xPx; penY += px; continue; }
            if (ch >= 128) ch = '?';
            if (ch > 32) { // skip space + control chars (blank glyphs)
                int col = ch % 16, row = ch / 16;
                float u0 = col * kCellU, v0 = row * kCellV;
                HudVertex q[6];
                emitQuad(q, penX, penY, px, px,
                         u0, v0, u0 + kInsetU, v0 + kInsetV, c);
                for (auto& v : q) m_hudScratch.push_back(v);
            }
            penX += px;
        }
        if (!m_hudScratch.empty())
            flushHud(m_hudScratch.data(), (uint32_t)m_hudScratch.size(), /*texFont=*/r);
    }

    // Render `text` from role `role`'s baked TTF atlas. For PROPORTIONAL roles the
    // pen advances by each glyph's real advance; for MONOSPACE roles every glyph
    // advances by a fixed cell and the (already-fixed-pitch) shape is placed
    // directly. `px` is the cap pixel size; the role index is also the texFont to
    // bind. (Takes the index, not a FontAtlas&, so the signature needs no early type
    // visibility — FontAtlas is declared with the other members further below.)
    void drawHudTextAtlas(int role, const char* text,
                          float xPx, float yPx, float px, const float c[4]) {
        const FontAtlas& fa = m_fonts[role];
        const int texFont = role;
        // bake-pixel units -> requested pixels (the cell advance maps to px).
        const float s = px / std::max(1.0f, fa.cellAdvance);
        const float baseline = fa.ascent * s;   // baseline below the cell top (yPx)
        const float lineH = px * 1.2f;           // newline step

        m_hudScratch.clear();
        float penX = xPx, penY = yPx;
        for (const char* p = text; *p; ++p) {
            unsigned char ch = static_cast<unsigned char>(*p);
            if (ch == '\n') { penX = xPx; penY += lineH; continue; }
            if (ch < kTtfFirstChar || ch >= kTtfFirstChar + kTtfCharCount) ch = '?';
            const TtfGlyph& g = fa.glyphs[ch - kTtfFirstChar];
            const float advance = g.advance * s;
            if (ch > 32) { // space + control glyphs have no quad
                // Proportional: place the glyph at the pen using its real bearings.
                // Monospace: center the (fixed-pitch) shape inside the cell so it
                // reads exactly as the cell layout expects.
                const float cellOff = fa.proportional ? 0.0f
                                                      : (px - advance) * 0.5f;
                const float gx0 = penX + cellOff + g.x0 * s;
                const float gy0 = penY + baseline + g.y0 * s;
                const float gw  = (g.x1 - g.x0) * s;
                const float gh  = (g.y1 - g.y0) * s;
                HudVertex q[6];
                emitQuad(q, gx0, gy0, gw, gh, g.u0, g.v0, g.u1, g.v1, c);
                for (auto& v : q) m_hudScratch.push_back(v);
            }
            // PROPORTIONAL advances by the glyph's real width; MONOSPACE by the cell.
            penX += fa.proportional ? advance : px;
        }
        if (!m_hudScratch.empty())
            flushHud(m_hudScratch.data(), (uint32_t)m_hudScratch.size(), /*texFont=*/texFont);
    }

    // The TRUE rendered width of `text` for `role` at glyph size `px`. Pure metrics
    // (no GPU work, safe before a frame). Proportional roles sum per-glyph advances;
    // mono roles (and the bitmap fallback) return N*px so legacy layout stays exact.
    float textAdvance(x3::rhi::FontRole role, const char* text, float px) const override {
        if (!text || px <= 0.0f) return 0.0f;
        const int r = (int)role;
        if (r >= 0 && r < kFontRoleCount && m_fonts[r].ready) {
            const FontAtlas& fa = m_fonts[r];
            const float s = px / std::max(1.0f, fa.cellAdvance);
            if (!fa.proportional) {
                // Monospace: N printable+space cells of width px (newlines reset).
                float maxLine = 0.0f, line = 0.0f;
                for (const char* p = text; *p; ++p) {
                    if (*p == '\n') { maxLine = std::max(maxLine, line); line = 0.0f; }
                    else            { line += px; }
                }
                return std::max(maxLine, line);
            }
            // Proportional: sum each glyph's real advance (longest line for newlines).
            float maxLine = 0.0f, line = 0.0f;
            for (const char* p = text; *p; ++p) {
                unsigned char ch = static_cast<unsigned char>(*p);
                if (ch == '\n') { maxLine = std::max(maxLine, line); line = 0.0f; continue; }
                if (ch < kTtfFirstChar || ch >= kTtfFirstChar + kTtfCharCount) ch = '?';
                line += fa.glyphs[ch - kTtfFirstChar].advance * s;
            }
            return std::max(maxLine, line);
        }
        // No TTF for this role: N*px (matches the 8x8 cell + the old static math).
        float maxLine = 0.0f, line = 0.0f;
        for (const char* p = text; *p; ++p) {
            if (*p == '\n') { maxLine = std::max(maxLine, line); line = 0.0f; }
            else            { line += px; }
        }
        return std::max(maxLine, line);
    }

    void hudSize(uint32_t& outW, uint32_t& outH) const override {
        outW = m_extent.width; outH = m_extent.height;
    }

private:
    struct Mesh {
        VkBuffer vbo = VK_NULL_HANDLE; VmaAllocation vboAlloc = nullptr;
        VkBuffer ibo = VK_NULL_HANDLE; VmaAllocation iboAlloc = nullptr;
        uint32_t indexCount = 0;
        uint32_t vertexCount = 0;       // vertices the vbo was sized for
        // CPU-skinning support (J1, scaled for fix 2): a mesh that has been updated
        // at least once becomes DYNAMIC and holds kFramesInFlight HOST_VISIBLE,
        // persistently-mapped vertex buffers — one per frame-in-flight — instead of
        // one shared buffer. updateMesh() writes the buffer for the frame being
        // recorded; the draw path (recordShadowPass/flushMeshDraws) binds the same
        // per-frame buffer. Because each frame's GPU work has retired (its inFlight
        // fence was waited in beginFrame) before that slot is reused, we never
        // overwrite a buffer the GPU may still be reading — NO vkDeviceWaitIdle and
        // no write-while-read hazard. Static meshes leave dynamic=false and use
        // only vbo (the device-local buffer); dynVbo[]/dynMapped[] stay null.
        bool     dynamic = false;
        VkBuffer      dynVbo[kFramesInFlight]    = {};   // per-frame HOST_VISIBLE vbos
        VmaAllocation dynVboAlloc[kFramesInFlight] = {};
        void*         dynMapped[kFramesInFlight] = {};   // persistent maps

        // The vertex buffer the draw path must bind for the frame currently being
        // recorded: the matching per-frame dynamic buffer when dynamic, else the
        // single static device-local vbo.
        VkBuffer drawVbo(uint32_t frameIdx) const {
            return dynamic ? dynVbo[frameIdx] : vbo;
        }
    };
    struct Texture {
        VkImage image = VK_NULL_HANDLE; VmaAllocation alloc = nullptr;
        VkImageView view = VK_NULL_HANDLE; VkSampler sampler = VK_NULL_HANDLE;
        uint32_t bindlessIndex = 0;   // slot in the bindless texture array
    };

    // ---- GPU compute skinning TYPES (GPU SKINNING OF MODELS) --------------
    // Declared here (with Mesh/Texture) so method signatures that take a SkinnedMesh&
    // are in complete-class context. The std430 row layouts match shaders/skin.comp
    // EXACTLY. The runtime member variables (maps, pool, pipeline) live in the
    // members section further down with the other subsystem state.
    struct SkinSrcVertex { glm::vec4 posPad; glm::vec4 nrmPad; glm::vec4 uvPad; }; // 48B
    struct SkinSrcInfluence { glm::uvec4 idx; glm::vec4 wt; };                     // 32B
    static_assert(sizeof(SkinSrcVertex) == 48, "SkinSrcVertex must be 3x vec4");
    static_assert(sizeof(SkinSrcInfluence) == 32, "SkinSrcInfluence must be uvec4+vec4");
    static constexpr uint32_t kMaxSkinJoints = 256;  // palette cap per instance
    struct SkinnedMesh {
        uint32_t      vertexCount = 0;
        VkBuffer      srcVbo   = VK_NULL_HANDLE; VmaAllocation srcAlloc = nullptr; // bind-pose verts (immutable)
        VkBuffer      infBuf   = VK_NULL_HANDLE; VmaAllocation infAlloc = nullptr; // influences (immutable)
        VkBuffer      palBuf[kFramesInFlight]   = {};  // per-frame palette SSBO (host-visible)
        VmaAllocation palAlloc[kFramesInFlight] = {};
        void*         palMapped[kFramesInFlight] = {};
        VkDescriptorSet set[kFramesInFlight] = {};     // per-frame compute set
        uint32_t      jointCount = 0;                  // palette joints set THIS frame
        uint32_t      lastSkinnedFrame = ~0u;          // m_frameIdx the output was last written
    };
    struct SkinPush { uint32_t vertexCount; uint32_t jointCount; };

    // 2D HUD vertex: position already in clip space (NDC), uv, rgba color.
    struct HudVertex { float pos[2]; float uv[2]; float color[4]; };

    // Forward point-light cap. A bounded fragment-shader loop over this many
    // lights is fine for a corridor (NOT clustered/tiled — that's a later perf
    // item). 64 omni fills cover Level 1's ceiling fixtures with headroom.
    static constexpr uint32_t kMaxPointLights = 64;

    // One GPU point light row (matches the GLSL std140 PointLight in mesh.frag).
    // std140 packs each as two vec4s: (pos.xyz, range) + (color.rgb, _pad) = 32 B.
    struct GpuPointLight {
        glm::vec4 posRange;     // xyz = world pos, w = range (meters)
        glm::vec4 colorPad;     // rgb = linear color * intensity, a = unused
    };
    static_assert(sizeof(GpuPointLight) == 32, "GpuPointLight must be two vec4 (std140)");

    // Per-frame UBO (matches shaders/mesh.{vert,frag} + shadow.vert). The camera
    // viewProj + the sun's lightViewProj (perf-stack E), plus the forward point-
    // light set: an ambient/count header vec4 then a fixed array. std140: the two
    // mat4s pack back-to-back (128 B); ambientCount is a vec4 (16 B, 16-aligned);
    // the GpuPointLight[] array of 32-B (vec4-aligned) elements follows tightly.
    struct FrameUBO {
        glm::mat4 viewProj;          // offset 0
        glm::mat4 lightViewProj;     // offset 64
        glm::vec4 ambientCount;      // offset 128: rgb = ambient color, w = light count (as float)
        GpuPointLight lights[kMaxPointLights]; // offset 144
    };
    static_assert(sizeof(FrameUBO) == 144 + kMaxPointLights * 32,
                  "FrameUBO must match the std140 layout in mesh.frag");

    // ---- Analytic sky UBO (open-world track, task A) -----------------------
    // Matches the SkyUBO block in shaders/sky.frag (std140). One mat4 (inverse
    // viewProj for per-pixel ray reconstruction) + four vec4s of sun/haze params.
    // Its own per-frame buffer + descriptor set (set 0 of the sky pipeline) — it
    // does NOT touch the shared mesh FrameUBO, so the locked mesh layout is safe.
    struct SkyUBO {
        glm::mat4 invViewProj;   // offset 0
        glm::vec4 camPos;        // offset 64: xyz = camera world pos
        glm::vec4 sunDir;        // offset 80: xyz = normalized toward-sun direction
        glm::vec4 sunColor;      // offset 96: rgb = sun color, a = intensity
        glm::vec4 params;        // offset 112: x = haze, y = exposure, z/w reserved
    };
    static_assert(sizeof(SkyUBO) == 128, "SkyUBO must match the std140 layout in sky.frag");

    // Per-object GPU row (matches shaders/mesh.vert ObjectData; std430). The 3
    // uint pads keep the struct 16-byte aligned after texIndex so std430's mat4
    // base alignment is satisfied for the next row. `emissive` (HDR pipeline) is
    // an extra vec4 (rgb = linear emissive color, a = strength) added on top of
    // the lit result in linear HDR so fixtures glow + feed the bloom chain.
    struct ObjectData {
        glm::mat4 model;
        glm::vec4 baseColorFactor;
        glm::vec4 emissive;          // rgb = linear color, a = strength
        uint32_t  texIndex;
        uint32_t  _pad0, _pad1, _pad2;
    };
    static_assert(sizeof(ObjectData) == 112, "ObjectData must match std430 layout");

    // CPU-side per-draw record accumulated by drawMesh(), consumed by endFrame().
    struct DrawRecord {
        uint32_t meshId;
        uint32_t texIndex;
        float    model[16];
        float    factor[4];
        float    emissive[4];   // rgb = linear emissive color, a = strength
        uint32_t terrainFlag;   // 1 = procedural terrain splat (mesh.frag branch)
        uint32_t terrainPack1;  // grass<<16 | rock  (bindless detail indices)
        uint32_t terrainPack2;  // snow<<16  | sand
    };

    // Per-frame mesh-draw capacity: sizes the per-object SSBO ring (one
    // ObjectData row per drawMesh) + the indirect-command buffer. 128k supports
    // the stress harness (spawn up to 100k cubes); records beyond this are
    // skipped safely. Cost: ~12 MB of SSBO ring per frame (96 B * cap).
    static constexpr uint32_t kMaxDrawsPerFrame = 131072;
    // Bindless texture array capacity (slot 0 == built-in white default). The
    // array is PARTIALLY_BOUND so only the slots actually created are written;
    // unused slots cost nothing. 4096 is ample for this game.
    static constexpr uint32_t kMaxTextures = 4096;
    // Per-frame HUD draw + vertex capacity. HUD draws are few (one per text/quad
    // flush); kept small + independent of the mesh cap so the HUD descriptor pool
    // stays tiny. 6 verts/quad; a full-screen console is well under this.
    static constexpr uint32_t kMaxHudDraws = 256;
    static constexpr uint32_t kMaxHudVerts = 24576;

    // ---- Directional shadow mapping (perf-stack E) ------------------------
    // Square depth map resolution. 2048 is a good quality/cost balance for a
    // single ~80 m cascade over a ~60 m level (4x the area of 1024 for one extra
    // mip of crispness; still cheap on the A2000/1080 Ti).
    static constexpr uint32_t kShadowDim = 2048;
    // The sun's ortho half-extent (meters) centered on the camera, and the depth
    // range along the sun direction. Sized to cover the level's working set; the
    // box follows the camera so the visible area is always shadowed.
    static constexpr float kShadowOrtho     = 45.0f;   // half-width/height
    static constexpr float kShadowDepthHalf = 80.0f;   // +/- along the sun dir

    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
        // GPU-driven per-frame rings (persistent-mapped, written each frame):
        //   objBuf      : per-object SSBO  (set 1, binding 0) — kMaxDrawsPerFrame rows
        //   camBuf      : camera viewProj UBO (set 1, binding 1)
        //   indirectBuf : VkDrawIndexedIndirectCommand array (one per distinct mesh)
        //   objSet      : the set-1 descriptor (points at objBuf+camBuf; written once)
        VkBuffer      objBuf = VK_NULL_HANDLE;      VmaAllocation objAlloc = nullptr;  void* objMapped = nullptr;
        VkBuffer      camBuf = VK_NULL_HANDLE;      VmaAllocation camAlloc = nullptr;  void* camMapped = nullptr;
        VkBuffer      indirectBuf = VK_NULL_HANDLE; VmaAllocation indirectAlloc = nullptr; void* indirectMapped = nullptr;
        VkDescriptorSet objSet = VK_NULL_HANDLE;
        // Per-frame analytic-sky UBO (set 0 of the sky pipeline) + its descriptor.
        VkBuffer      skyBuf = VK_NULL_HANDLE;      VmaAllocation skyAlloc = nullptr;  void* skyMapped = nullptr;
        VkDescriptorSet skySet = VK_NULL_HANDLE;
        // Per-frame HUD descriptor pool (image-sampler only) + vertex ring.
        VkDescriptorPool hudDescPool = VK_NULL_HANDLE;
        VkBuffer      hudVbo = VK_NULL_HANDLE;
        VmaAllocation hudVboAlloc = nullptr;
        void*         hudVboMapped = nullptr;
        uint32_t      hudVertsUsed = 0;  // write cursor into the vertex ring
        // Per-frame GPU timestamp query pool (2 stamps: pass begin + pass end).
        // Reset + written each beginFrame/endFrame; results are read back when the
        // SAME ring slot comes around again (kFramesInFlight frames later, so the
        // fence has already guaranteed the timestamps are available — no stall).
        VkQueryPool   tsPool = VK_NULL_HANDLE;
        bool          tsPending = false; // a frame's timestamps await readback
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

    // ---- HUD helpers --------------------------------------------------------
    // Fill 6 vertices (two triangles) for a pixel-space rect, converting to NDC
    // using the current framebuffer extent (origin top-left -> NDC y flips).
    void emitQuad(HudVertex out[6], float xPx, float yPx, float wPx, float hPx,
                  float u0, float v0, float u1, float v1, const float c[4]) {
        const float fw = (float)std::max(1u, m_extent.width);
        const float fh = (float)std::max(1u, m_extent.height);
        auto ndcX = [&](float px){ return (px / fw) * 2.0f - 1.0f; };
        auto ndcY = [&](float py){ return (py / fh) * 2.0f - 1.0f; }; // top-left origin
        const float x0 = ndcX(xPx),        y0 = ndcY(yPx);
        const float x1 = ndcX(xPx + wPx),  y1 = ndcY(yPx + hPx);
        const HudVertex tl{ { x0, y0 }, { u0, v0 }, { c[0], c[1], c[2], c[3] } };
        const HudVertex tr{ { x1, y0 }, { u1, v0 }, { c[0], c[1], c[2], c[3] } };
        const HudVertex bl{ { x0, y1 }, { u0, v1 }, { c[0], c[1], c[2], c[3] } };
        const HudVertex br{ { x1, y1 }, { u1, v1 }, { c[0], c[1], c[2], c[3] } };
        out[0] = tl; out[1] = bl; out[2] = br;   // CCW: tl->bl->br
        out[3] = tl; out[4] = br; out[5] = tr;   //      tl->br->tr
    }

    // ---- GPU-driven per-frame data prep (Subsystem D + shadows E) ----------
    // Build the camera+light UBO, group this frame's drawMesh() records by mesh,
    // write the per-object SSBO (instances of a mesh contiguous), and fill one
    // VkDrawIndexedIndirectCommand per mesh. Pure data; records NO commands. Runs
    // once per frame (m_framePrepared) because BOTH the shadow depth pass and the
    // main color pass consume the same SSBO + indirect buffer. Caches the produced
    // command count in m_frameCmdCount.
    void prepareFrameData() {
        if (m_framePrepared) return;
        m_framePrepared = true;
        m_frameCmdCount = 0;

        auto& fr = m_frames[m_frameIdx];
        if (!fr.objMapped || !fr.indirectMapped || !fr.camMapped) return;

        // Camera viewProj (right-handed, reverse-Y for Vulkan clip) + the sun's
        // ortho lightViewProj, written together into the per-frame camera UBO.
        const float aspect = (float)m_extent.width / (float)std::max(1u, m_extent.height);
        const glm::vec3 fwd(std::cos(m_camPitch) * std::cos(m_camYaw),
                            std::sin(m_camPitch),
                            std::cos(m_camPitch) * std::sin(m_camYaw));
        glm::mat4 view = glm::lookAt(m_camPos, m_camPos + fwd, glm::vec3(0, 1, 0));
        glm::mat4 proj = glm::perspective(glm::radians(m_camFov), aspect, 0.1f, 200.0f);
        proj[1][1] *= -1.0f;
        FrameUBO ubo{};
        ubo.viewProj = proj * view;
        m_lastViewProj = ubo.viewProj;  // cached for the debris instanced draw UBO

        // ---- SSAO per-frame UBO + mesh.frag control. The SSAO pass reconstructs
        // VIEW-space position from depth via invProj and projects samples via proj
        // (the SAME camera projection, reverse-Y, used by the meshes). Fill the
        // baked kernel/noise + the tunables each frame so cvar edits take effect
        // immediately. The depth/AO image views are wired by writeSsaoDescriptors. ----
        {
            SsaoUBO su{};
            su.proj    = proj;
            su.invProj = glm::inverse(proj);
            su.params0 = glm::vec4(m_ssao.radius, m_ssao.bias, m_ssao.intensity, m_ssao.power);
            su.params1 = glm::vec4((float)m_extent.width, (float)m_extent.height,
                                   (float)m_extent.width / 4.0f, (float)m_extent.height / 4.0f);
            for (int i = 0; i < kSsaoKernel; ++i) su.kernel[i] = m_ssaoKernelCPU[i];
            for (int i = 0; i < 16; ++i)          su.noise[i]  = m_ssaoNoiseCPU[i];
            if (m_ssaoUboMapped[m_frameIdx])
                std::memcpy(m_ssaoUboMapped[m_frameIdx], &su, sizeof(SsaoUBO));

            SsaoControl sc{};
            const float invW = (m_extent.width  > 0) ? 1.0f / (float)m_extent.width  : 0.0f;
            const float invH = (m_extent.height > 0) ? 1.0f / (float)m_extent.height : 0.0f;
            sc.ctrl = glm::vec4(m_ssao.enabled ? 1.0f : 0.0f, m_ssao.strength, invW, invH);
            if (m_ssaoCtrlMapped[m_frameIdx])
                std::memcpy(m_ssaoCtrlMapped[m_frameIdx], &sc, sizeof(SsaoControl));
        }

        // ---- GI per-frame UBOs (gather + temporal). The gather reconstructs view
        // pos/normal from depth via invProj + projects samples via proj (the SAME
        // camera projection as the meshes). The temporal pass camera-reprojects last
        // frame's GI: it needs the CURRENT inverse viewProj (clip->world) + the
        // PREVIOUS viewProj (world->prev clip). Choose this frame's ping-pong write
        // buffer (read the OTHER as history) and wire the per-frame descriptor sets.
        if (m_gi.enabled) {
            GiUBO gu{};
            gu.proj    = proj;
            gu.invProj = glm::inverse(proj);
            gu.params0 = glm::vec4(m_gi.radius, m_gi.intensity, m_gi.maxRadiance, m_gi.falloffPower);
            const int nSamp = std::max(1, std::min(m_gi.numSamples, kGiKernel));
            gu.params1 = glm::vec4((float)m_extent.width, (float)m_extent.height,
                                   (float)nSamp, 0.05f /*cosine bias*/);
            for (int i = 0; i < kGiKernel; ++i) gu.kernel[i] = m_giKernelCPU[i];
            for (int i = 0; i < 16; ++i)        gu.noise[i]  = m_giNoiseCPU[i];
            if (m_giUboMapped[m_frameIdx]) std::memcpy(m_giUboMapped[m_frameIdx], &gu, sizeof(GiUBO));

            // Ping-pong: write into m_giAccumWrite, read the other as history.
            const uint32_t writeIdx = m_giAccumWrite;
            const uint32_t histIdx  = writeIdx ^ 1u;

            GiTemporalUBO tu{};
            tu.invViewProjCur = glm::inverse(ubo.viewProj);
            tu.viewProjPrev   = m_giPrevViewProj;
            // History invalid on the first frame after init/resize -> z = 0 forces
            // the temporal pass to fall back to the raw gather (no stale reproject).
            tu.params0 = glm::vec4(m_gi.temporalAlpha, 0.25f /*reject tol scale (m)*/,
                                   m_giHistoryValid ? 1.0f : 0.0f, 0.0f);
            if (m_giTempUboMapped[m_frameIdx]) std::memcpy(m_giTempUboMapped[m_frameIdx], &tu, sizeof(GiTemporalUBO));

            // Denoise + apply push constants (half-res texel + tunables).
            const float gw = 1.0f / (float)std::max(1u, m_giExtent.width);
            const float gh = 1.0f / (float)std::max(1u, m_giExtent.height);
            m_giBlurPush.giTexel[0] = gw; m_giBlurPush.giTexel[1] = gh;
            m_giBlurPush.depthSigma = 0.0015f; m_giBlurPush.stepScale = 2.0f;
            m_giApplyPush.giTexel[0] = gw; m_giApplyPush.giTexel[1] = gh;
            m_giApplyPush.strength = m_gi.strength;
            // AO modulation only when the SSAO chain actually produced AO this frame;
            // otherwise force 0 (and bind a harmless valid image) so we never sample
            // an unwritten/wrong-layout AO target.
            const bool aoAvail = m_ssao.enabled;
            m_giApplyPush.aoAmount = aoAvail ? m_gi.aoModulate : 0.0f;
            VkImageView aoView = aoAvail ? m_ssaoBlurView : m_giDenoiseView;

            // Wire the per-frame ping-pong descriptors for the temporal/blur/apply
            // sets (allocation-free vkUpdateDescriptorSets).
            writeGiFrameDescriptors(writeIdx, histIdx, aoView);

            // Stash this frame's viewProj for next frame's reprojection.
            m_giPrevViewProj = ubo.viewProj;
        }

        m_lightViewProj = computeLightViewProj();
        ubo.lightViewProj = m_lightViewProj;
        // Forward point lights: a constant hemispheric-ish ambient lift in the rgb,
        // the active light count in w, then the cached light rows. Static lights are
        // set once via setPointLights(); we re-upload the cached copy each frame.
        const uint32_t lc = std::min<uint32_t>((uint32_t)m_pointLights.size(), kMaxPointLights);
        ubo.ambientCount = glm::vec4(m_ambient, (float)lc);
        for (uint32_t i = 0; i < lc; ++i) {
            const PointLight& s = m_pointLights[i];
            ubo.lights[i].posRange = glm::vec4(s.pos[0], s.pos[1], s.pos[2], s.range);
            ubo.lights[i].colorPad = glm::vec4(s.color[0], s.color[1], s.color[2], 0.0f);
        }
        std::memcpy(fr.camMapped, &ubo, sizeof(FrameUBO));

        // Analytic sky UBO (open-world track, task A): the camera's INVERSE viewProj
        // (for per-pixel world-ray reconstruction) + the sun/haze params. Uses the
        // SAME camera matrix the meshes use, so the sky and the lit world are
        // perfectly registered. Cheap; written every frame whether or not the sky is
        // enabled (the draw itself is gated by m_sky.enabled in ensureMainPass).
        if (fr.skyMapped) {
            SkyUBO sky{};
            sky.invViewProj = glm::inverse(ubo.viewProj);
            sky.camPos   = glm::vec4(m_camPos, 1.0f);
            glm::vec3 sd = glm::normalize(glm::vec3(m_sky.sunDir[0], m_sky.sunDir[1], m_sky.sunDir[2]));
            sky.sunDir   = glm::vec4(sd, 0.0f);
            sky.sunColor = glm::vec4(m_sky.sunColor[0], m_sky.sunColor[1], m_sky.sunColor[2], m_sky.sunIntensity);
            sky.params   = glm::vec4(m_sky.haze, m_sky.exposure, 0.0f, 0.0f);
            std::memcpy(fr.skyMapped, &sky, sizeof(SkyUBO));
        }

        // Water UBO (undersea-world foundation): the SAME camera viewProj the meshes
        // use (so the water is registered with the world), the sun, the tunables,
        // and the depth-reconstruction screen size. Written every frame whether or
        // not water is enabled (the pass itself is gated by m_water.enabled).
        if (m_waterUboMapped[m_frameIdx]) {
            WaterUBO w{};
            w.viewProj = ubo.viewProj;
            w.camPos   = glm::vec4(m_camPos, 1.0f);
            glm::vec3 wsd = glm::normalize(glm::vec3(m_water.sunDir[0], m_water.sunDir[1], m_water.sunDir[2]));
            w.sunDir   = glm::vec4(wsd, 0.0f);
            w.deepColor    = glm::vec4(m_water.deepColor[0], m_water.deepColor[1], m_water.deepColor[2], 1.0f);
            w.shallowColor = glm::vec4(m_water.shallowColor[0], m_water.shallowColor[1], m_water.shallowColor[2], 1.0f);
            w.p0 = glm::vec4(m_water.seaLevel, m_water.time, m_water.amplitude, m_water.steepness);
            w.p1 = glm::vec4(m_water.waveLength, m_water.speed, m_water.specular, m_water.fresnel);
            const float invW = (m_extent.width  > 0) ? 1.0f / (float)m_extent.width  : 0.0f;
            const float invH = (m_extent.height > 0) ? 1.0f / (float)m_extent.height : 0.0f;
            w.p2 = glm::vec4(kWaterPatchHalf, invW, invH, 0.0f);
            std::memcpy(m_waterUboMapped[m_frameIdx], &w, sizeof(WaterUBO));
        }

        // ---- Particles + impact decals (combat juice) ----------------------
        // Stream this frame's submitted instances into the per-frame rings and fill
        // the UBOs (camera viewProj + the screen-aligned billboard basis + depth-
        // reconstruction near/far). Done BEFORE the early-out below so a mesh-less FX
        // capture still uploads. Counts are clamped to the rings' capacity.
        {
            // Camera basis (device convention; matches fwd above). right is the XZ
            // perpendicular; up = right x forward (orthonormal, screen-aligned).
            const float cy = std::cos(m_camYaw),   sy = std::sin(m_camYaw);
            const glm::vec3 camRight(-sy, 0.0f, cy);
            const glm::vec3 camUp = glm::normalize(glm::cross(camRight, fwd));
            const float invW = (m_extent.width  > 0) ? 1.0f / (float)m_extent.width  : 0.0f;
            const float invH = (m_extent.height > 0) ? 1.0f / (float)m_extent.height : 0.0f;

            m_partAddCount   = (uint32_t)std::min<size_t>(m_partAdd.size(),   kMaxParticles);
            m_partAlphaCount = (uint32_t)std::min<size_t>(m_partAlpha.size(), kMaxParticles);
            m_decalCount     = (uint32_t)std::min<size_t>(m_decals.size(),    kMaxDecals);

            if (m_partUboMapped[m_frameIdx]) {
                ParticleUBO pu{};
                pu.viewProj = ubo.viewProj;
                pu.camRight = glm::vec4(camRight, 0.0f);
                pu.camUp    = glm::vec4(camUp, 0.0f);
                pu.camPos   = glm::vec4(m_camPos, 1.0f);
                pu.params   = glm::vec4(invW, invH, 0.1f, 200.0f);  // near/far match the proj
                std::memcpy(m_partUboMapped[m_frameIdx], &pu, sizeof(ParticleUBO));
            }
            if (m_decalUboMapped[m_frameIdx]) {
                DecalUBO du{};
                du.viewProj = ubo.viewProj;
                std::memcpy(m_decalUboMapped[m_frameIdx], &du, sizeof(DecalUBO));
            }
            if (m_partAddCount && m_partInstAddMapped[m_frameIdx]) {
                ParticleGpu* d = (ParticleGpu*)m_partInstAddMapped[m_frameIdx];
                for (uint32_t i = 0; i < m_partAddCount; ++i) {
                    const ParticleInstance& s = m_partAdd[i];
                    d[i].posSize = glm::vec4(s.pos[0], s.pos[1], s.pos[2], s.size);
                    d[i].color   = glm::vec4(s.color[0], s.color[1], s.color[2], s.color[3]);
                }
            }
            if (m_partAlphaCount && m_partInstAlphaMapped[m_frameIdx]) {
                ParticleGpu* d = (ParticleGpu*)m_partInstAlphaMapped[m_frameIdx];
                for (uint32_t i = 0; i < m_partAlphaCount; ++i) {
                    const ParticleInstance& s = m_partAlpha[i];
                    d[i].posSize = glm::vec4(s.pos[0], s.pos[1], s.pos[2], s.size);
                    d[i].color   = glm::vec4(s.color[0], s.color[1], s.color[2], s.color[3]);
                }
            }
            if (m_decalCount && m_decalInstMapped[m_frameIdx]) {
                DecalGpu* d = (DecalGpu*)m_decalInstMapped[m_frameIdx];
                for (uint32_t i = 0; i < m_decalCount; ++i) {
                    const DecalInstance& s = m_decals[i];
                    d[i].centerSize  = glm::vec4(s.center[0], s.center[1], s.center[2], s.halfSize);
                    d[i].normalAngle = glm::vec4(s.normal[0], s.normal[1], s.normal[2], s.angle);
                    d[i].color       = glm::vec4(s.color[0], s.color[1], s.color[2], s.color[3]);
                }
            }
        }

        if (m_drawRecords.empty() || !m_meshPipeline) return;

        // Group records by mesh (preserve first-seen order for determinism). Reuse
        // a scratch map across frames to avoid per-frame allocation churn.
        // Fix 4: also PRUNE dead keys so m_groups doesn't grow unbounded over a long
        // terrain-streaming session (each evicted mesh would otherwise leave an empty
        // vector behind forever). destroyMesh() erases the key directly; this is the
        // belt-and-suspenders sweep that drops any key whose mesh is no longer live
        // (and reuses the vector storage of surviving keys via clear()).
        m_groupOrder.clear();
        for (auto it = m_groups.begin(); it != m_groups.end(); ) {
            if (m_meshes.find(it->first) == m_meshes.end()) {
                it = m_groups.erase(it);          // mesh gone -> drop the dead key
            } else {
                it->second.clear();               // live mesh -> reuse the vector
                ++it;
            }
        }
        for (uint32_t i = 0; i < (uint32_t)m_drawRecords.size(); ++i) {
            uint32_t mid = m_drawRecords[i].meshId;
            auto it = m_groups.find(mid);
            if (it == m_groups.end()) { m_groups.emplace(mid, std::vector<uint32_t>{}); m_groupOrder.push_back(mid); it = m_groups.find(mid); }
            else if (it->second.empty()) m_groupOrder.push_back(mid);
            it->second.push_back(i);
        }

        // Write the SSBO grouped (instances of each mesh contiguous) and build one
        // indirect command per group; firstInstance = the group's SSBO base row, so
        // gl_InstanceIndex in the shader indexes the object row directly.
        ObjectData* objs = static_cast<ObjectData*>(fr.objMapped);
        VkDrawIndexedIndirectCommand* cmds =
            static_cast<VkDrawIndexedIndirectCommand*>(fr.indirectMapped);
        uint32_t row = 0, cmdCount = 0;
        for (uint32_t mid : m_groupOrder) {
            auto mit = m_meshes.find(mid);
            if (mit == m_meshes.end()) continue;
            const std::vector<uint32_t>& list = m_groups[mid];
            if (list.empty()) continue;
            if (cmdCount >= kMaxDrawMeshes) break;
            const uint32_t baseRow = row;
            for (uint32_t ri : list) {
                const DrawRecord& dr = m_drawRecords[ri];
                ObjectData& o = objs[row++];
                std::memcpy(&o.model, dr.model, sizeof(o.model));
                o.baseColorFactor = glm::vec4(dr.factor[0], dr.factor[1], dr.factor[2], dr.factor[3]);
                o.emissive = glm::vec4(dr.emissive[0], dr.emissive[1], dr.emissive[2], dr.emissive[3]);
                o.texIndex = dr.texIndex;
                // Reuse the previously-reserved pads: pad0 = terrain flag, pad1/pad2
                // = the four packed detail-texture indices (see mesh.{vert,frag}).
                o._pad0 = dr.terrainFlag;
                o._pad1 = dr.terrainPack1;
                o._pad2 = dr.terrainPack2;
            }
            VkDrawIndexedIndirectCommand& c = cmds[cmdCount];
            c.indexCount    = mit->second.indexCount;
            c.instanceCount = (uint32_t)list.size();
            c.firstIndex    = 0;
            c.vertexOffset  = 0;
            c.firstInstance = baseRow;
            m_drawMeshOrder[cmdCount] = mid;
            ++cmdCount;
            m_building.drawCalls += 1;
            m_building.objectsDrawn += (uint32_t)list.size();
            m_building.triangles += (mit->second.indexCount / 3) * (uint32_t)list.size();
        }
        m_frameCmdCount = cmdCount;
    }

    // ---- Directional shadow depth pass (perf-stack E) ----------------------
    // Render the scene depth from the sun's POV into the shadow map using the SAME
    // SSBO + indirect commands as the color pass (prepared by prepareFrameData),
    // with the depth-only pipeline (set 0 = object SSBO + camera UBO; shadow.vert
    // reads model rows + lightViewProj). Always runs (even with zero occluders) so
    // the map is cleared + left in DEPTH_READ_ONLY_OPTIMAL for the main pass to
    // sample. Recorded BEFORE the main color pass begins (no nested rendering).
    // Record the BODY of the shadow depth pass into `cmd` (the graph has begun
    // dynamic rendering on the shadow map + emitted its UNDEFINED->DEPTH_ATTACHMENT
    // barrier). Same SSBO + indirect draws as the color pass, from the sun's POV.
    void recordShadowPassBody(VkCommandBuffer cmd) {
        if (!m_shadowPipeline) return;
        auto& fr = m_frames[m_frameIdx];

        VkViewport vp{ 0.0f, 0.0f, (float)kShadowDim, (float)kShadowDim, 0.0f, 1.0f };
        VkRect2D scis{ {0,0}, { kShadowDim, kShadowDim } };
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &scis);

        if (m_frameCmdCount > 0) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline);
            // set 0 = object SSBO + camera UBO (shadow.vert reads lightViewProj).
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowLayout,
                                    0, 1, &fr.objSet, 0, nullptr);
            for (uint32_t i = 0; i < m_frameCmdCount; ++i) {
                const Mesh& mh = m_meshes[m_drawMeshOrder[i]];
                VkDeviceSize off = 0;
                VkBuffer vb = mh.drawVbo(m_frameIdx); // fix 2: per-frame dynamic vbo
                vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
                vkCmdBindIndexBuffer(cmd, mh.ibo, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexedIndirect(cmd, fr.indirectBuf,
                    (VkDeviceSize)i * sizeof(VkDrawIndexedIndirectCommand), 1,
                    sizeof(VkDrawIndexedIndirectCommand));
            }
        }
    }

    // ---- SSAO depth pre-pass body ------------------------------------------
    // Record the camera depth-only pre-pass into the (already-open) depth pass:
    // bind the depth pre-pass pipeline (depth.vert, set0 = objSet) and replay the
    // SAME indirect draws as the main pass, so the depth buffer holds the exact
    // camera depth before lighting. Runs only when SSAO is enabled.
    void recordDepthPrePassBody(VkCommandBuffer cmd) {
        if (!m_depthPrePipeline || m_frameCmdCount == 0) return;
        auto& fr = m_frames[m_frameIdx];
        VkViewport vp{ 0.0f, 0.0f, (float)m_extent.width, (float)m_extent.height, 0.0f, 1.0f };
        VkRect2D scis{ {0,0}, m_extent };
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &scis);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_depthPrePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowLayout,
                                0, 1, &fr.objSet, 0, nullptr);
        for (uint32_t i = 0; i < m_frameCmdCount; ++i) {
            const Mesh& mh = m_meshes[m_drawMeshOrder[i]];
            VkDeviceSize off = 0;
            VkBuffer vb = mh.drawVbo(m_frameIdx);
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
            vkCmdBindIndexBuffer(cmd, mh.ibo, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexedIndirect(cmd, fr.indirectBuf,
                (VkDeviceSize)i * sizeof(VkDrawIndexedIndirectCommand), 1,
                sizeof(VkDrawIndexedIndirectCommand));
        }
    }

    // ---- GPU-driven mesh multidraw (Subsystem D) ---------------------------
    // Record the mesh multidraw into the (already-open) main color pass: bind the
    // mesh pipeline + descriptor sets and issue ONE vkCmdDrawIndexedIndirect per
    // distinct mesh. Called from recordMainPassBody (inside the graph's color pass).
    void recordMeshDraws(VkCommandBuffer cmd) {
        if (m_frameCmdCount == 0 || !m_meshPipeline) return;
        auto& fr = m_frames[m_frameIdx];
        // Pre-pass on (SSAO OR GI) -> the EQUAL/no-write pipeline (the depth pre-pass
        // already wrote depth). Neither on -> the original LESS/write pipeline (main
        // pass owns depth, no pre-pass ran). The mesh.frag AO sample is independently
        // gated by the SSAO control UBO, so the EQUAL pipeline is safe when GI is on
        // but SSAO is off (no AO is read in that case).
        const bool prePassOn = m_ssao.enabled || m_gi.enabled;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          prePassOn ? m_meshPipeline : m_meshPipelineNoSsao);
        // set 0 = bindless textures, set 1 = object SSBO + camera UBO, set 2 = shadow
        // map, set 3 = the SSAO AO texture + control UBO (this frame's set).
        VkDescriptorSet sets[4] = { m_bindlessSet, fr.objSet, m_shadowSet, m_meshAoSet[m_frameIdx] };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_meshLayout,
                                0, 4, sets, 0, nullptr);
        for (uint32_t i = 0; i < m_frameCmdCount; ++i) {
            const Mesh& mh = m_meshes[m_drawMeshOrder[i]];
            VkDeviceSize off = 0;
            VkBuffer vb = mh.drawVbo(m_frameIdx); // fix 2: per-frame dynamic vbo
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
            vkCmdBindIndexBuffer(cmd, mh.ibo, 0, VK_INDEX_TYPE_UINT32);
            // ONE indirect draw per mesh: instanceCount instances, the GPU reads
            // each instance's transform/texture from the SSBO via gl_InstanceIndex.
            vkCmdDrawIndexedIndirect(cmd, fr.indirectBuf,
                (VkDeviceSize)i * sizeof(VkDrawIndexedIndirectCommand), 1,
                sizeof(VkDrawIndexedIndirectCommand));
        }
    }

    // A deferred HUD draw: a span of this frame's HUD vertex ring + which texture
    // to bind. `texFont` selects the atlas: -1 => the 1x1 white texel (filled
    // quads); >=0 => that FontRole's glyph atlas (or the 8x8 bitmap fallback if the
    // role didn't bake). drawHudQuad/drawHudText(F) append these; they are replayed
    // (in order, after the meshes) by recordHudDraws inside the graph's color pass —
    // keeping HUD-on-top ordering identical to the hand-coded path.
    struct HudRecord { uint32_t first; uint32_t count; int texFont; };

    // Append `count` vertices to this frame's HUD ring and queue a deferred HUD
    // draw record (replayed inside the graph's color pass). No command recording
    // here — the color pass is not open yet (commands are recorded in endFrame).
    // `texFont`: -1 = white texel; otherwise a FontRole index whose atlas to bind.
    void flushHud(const HudVertex* verts, uint32_t count, int texFont) {
        auto& fr = m_frames[m_frameIdx];
        if (!fr.hudVboMapped || fr.hudVertsUsed + count > kMaxHudVerts) return; // ring full
        uint32_t first = fr.hudVertsUsed;
        std::memcpy(static_cast<HudVertex*>(fr.hudVboMapped) + first,
                    verts, (size_t)count * sizeof(HudVertex));
        fr.hudVertsUsed += count;
        m_hudRecords.push_back(HudRecord{ first, count, texFont });
    }

    // Resolve a HudRecord's texFont to the Texture to bind: white texel for -1, the
    // role's baked atlas if ready, else the 8x8 bitmap fallback, else white.
    const Texture* hudRecordTexture(int texFont) const {
        if (texFont < 0) return &m_whiteTex;
        if (texFont < kFontRoleCount && m_fonts[texFont].ready) return &m_fonts[texFont].tex;
        if (m_bitmapFontReady && m_bitmapFontTex.view) return &m_bitmapFontTex;
        return &m_whiteTex;
    }

    // Replay the frame's deferred HUD draws into the (already-open) color pass.
    // Allocates the per-draw descriptor from the frame's HUD pool here, exactly as
    // the old in-line flushHud did (same descriptor lifetime: recycled next reuse).
    void recordHudDraws(VkCommandBuffer cmd) {
        auto& fr = m_frames[m_frameIdx];
        if (m_hudRecords.empty() || !m_hudPipeline) return;
        for (const HudRecord& hr : m_hudRecords) {
            const Texture* tex = hudRecordTexture(hr.texFont);

            VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dsai.descriptorPool = fr.hudDescPool;
            dsai.descriptorSetCount = 1;
            dsai.pSetLayouts = &m_hudSetLayout;
            VkDescriptorSet set = VK_NULL_HANDLE;
            if (vkAllocateDescriptorSets(m_dev.device, &dsai, &set) != VK_SUCCESS) return;

            VkDescriptorImageInfo dii{};
            dii.sampler = tex->sampler;
            dii.imageView = tex->view;
            dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet = set; w.dstBinding = 0; w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &dii;
            vkUpdateDescriptorSets(m_dev.device, 1, &w, 0, nullptr);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_hudPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_hudLayout,
                                    0, 1, &set, 0, nullptr);
            VkDeviceSize off = (VkDeviceSize)hr.first * sizeof(HudVertex);
            vkCmdBindVertexBuffers(cmd, 0, 1, &fr.hudVbo, &off);
            vkCmdDraw(cmd, hr.count, 1, 0, 0);
        }
    }

    // ===================================================================
    // Build the per-frame render graph + execute it (perf-stack B). This is the
    // single place the frame's GPU command sequence is assembled. Each pass
    // declares the resources it reads/writes (layout + stage + access); the graph
    // derives + emits all sync2 barriers + layout transitions and drives
    // begin/endRendering. Replaces every previously hand-coded barrier.
    //
    //   Pass 1  shadow-depth : WRITE shadow map (DEPTH_ATTACHMENT). The graph
    //                          transitions it from its persistent prior-frame
    //                          state (DEPTH_READ_ONLY, or UNDEFINED on first use).
    //   Pass 2  main-color   : WRITE swapchain color (COLOR_ATTACHMENT) + depth
    //                          (DEPTH_ATTACHMENT) + READ shadow map
    //                          (DEPTH_READ_ONLY). The graph emits the
    //                          shadow DEPTH_ATTACHMENT->DEPTH_READ_ONLY transition
    //                          (the #1 VUID-sensitive barrier) automatically, plus
    //                          the swapchain/depth UNDEFINED->attachment transitions.
    //   Pass 3  present OR capture : transitions the swapchain color to
    //                          PRESENT_SRC, or (if a capture is armed) to
    //                          TRANSFER_SRC, copies to the readback buffer, then to
    //                          PRESENT_SRC — all derived from declared uses.
    // ===================================================================
    // Add the bloom downsample + upsample passes to the graph for this frame.
    // `rgHdr` is the linear HDR scene target (already written by the main pass);
    // `rgMip[]` are the kBloomMips bloom targets. Each pass is a full-screen
    // triangle; the graph derives the COLOR_ATTACHMENT<->SHADER_READ_ONLY
    // transitions between mips. No per-frame heap alloc: the push payloads live in
    // member arrays captured by value into the record lambdas.
    void addBloomPasses(RgResource rgHdr, RgResource* rgMip) {
        // ---- Downsample chain ----
        for (uint32_t i = 0; i < kBloomMips; ++i) {
            const bool firstPass = (i == 0);
            // Source: HDR scene (mip0) or the previous, larger mip.
            RgResource srcRes = firstPass ? rgHdr : rgMip[i - 1];
            VkDescriptorSet srcSet = firstPass ? m_setHdr : m_setMip[i - 1];
            // Source resolution (1/texel) for the filter taps.
            VkExtent2D srcExt = firstPass ? m_extent : m_bloomMips[i - 1].extent;
            const VkExtent2D dstExt = m_bloomMips[i].extent;

            BloomPush& pc = m_bloomDownPush[i];
            pc.srcTexel[0] = 1.0f / (float)std::max(1u, srcExt.width);
            pc.srcTexel[1] = 1.0f / (float)std::max(1u, srcExt.height);
            pc.threshold = kBloomThreshold; pc.knee = kBloomKnee;
            pc.intensity = 1.0f; pc.firstPass = firstPass ? 1 : 0;

            m_bloomAttach[i] = VkRenderingAttachmentInfo{};
            m_bloomAttach[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            m_bloomAttach[i].imageView = m_bloomMips[i].view;
            m_bloomAttach[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            m_bloomAttach[i].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; // fully written
            m_bloomAttach[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            m_bloomRenderInfo[i] = VkRenderingInfo{};
            m_bloomRenderInfo[i].sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            m_bloomRenderInfo[i].renderArea = { {0,0}, dstExt };
            m_bloomRenderInfo[i].layerCount = 1;
            m_bloomRenderInfo[i].colorAttachmentCount = 1;
            m_bloomRenderInfo[i].pColorAttachments = &m_bloomAttach[i];

            RenderPassDesc dp{};
            dp.name = "bloom-down";
            dp.addUse(ResourceUse{
                rgMip[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/true });
            dp.addUse(ResourceUse{
                srcRes, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/false });
            dp.usesDynamicRendering = true;
            dp.renderInfo = m_bloomRenderInfo[i];
            // Stash this mip's params in stable per-pass storage; ctx points at it.
            m_bloomDownCtx[i] = BloomPassCtx{ this, srcSet, dstExt, i };
            dp.recordCtx = &m_bloomDownCtx[i];
            dp.record = [](void* ctx, VkCommandBuffer c){
                auto* pc = static_cast<BloomPassCtx*>(ctx);
                auto* self = pc->self;
                self->postViewport(c, pc->dstExt);
                vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, self->m_bloomDownPipe);
                vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, self->m_bloomLayout,
                                        0, 1, &pc->srcSet, 0, nullptr);
                vkCmdPushConstants(c, self->m_bloomLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(BloomPush), &self->m_bloomDownPush[pc->mip]);
                vkCmdDraw(c, 3, 1, 0, 0);
            };
            m_graph.addPass(std::move(dp));
        }

        // ---- Upsample chain (smallest -> largest), additively blended ----
        for (int i = (int)kBloomMips - 2; i >= 0; --i) {
            const uint32_t src = (uint32_t)i + 1;      // smaller mip we sample
            const uint32_t dst = (uint32_t)i;          // larger mip we add into
            const VkExtent2D srcExt = m_bloomMips[src].extent;
            const VkExtent2D dstExt = m_bloomMips[dst].extent;

            BloomPush& pc = m_bloomUpPush[dst];
            pc.srcTexel[0] = 1.0f / (float)std::max(1u, srcExt.width);
            pc.srcTexel[1] = 1.0f / (float)std::max(1u, srcExt.height);
            pc.threshold = 0.0f; pc.knee = 0.0f;
            pc.intensity = kBloomUpScale; pc.firstPass = 0;

            // Reuse this mip's attachment struct but LOAD (keep) its content so the
            // additive blend accumulates onto the downsampled value already there.
            m_bloomUpAttach[dst] = VkRenderingAttachmentInfo{};
            m_bloomUpAttach[dst].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            m_bloomUpAttach[dst].imageView = m_bloomMips[dst].view;
            m_bloomUpAttach[dst].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            m_bloomUpAttach[dst].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;  // keep + accumulate
            m_bloomUpAttach[dst].storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            m_bloomUpRenderInfo[dst] = VkRenderingInfo{};
            m_bloomUpRenderInfo[dst].sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            m_bloomUpRenderInfo[dst].renderArea = { {0,0}, dstExt };
            m_bloomUpRenderInfo[dst].layerCount = 1;
            m_bloomUpRenderInfo[dst].colorAttachmentCount = 1;
            m_bloomUpRenderInfo[dst].pColorAttachments = &m_bloomUpAttach[dst];

            VkDescriptorSet srcSet = m_setMip[src];
            RenderPassDesc up{};
            up.name = "bloom-up";
            up.addUse(ResourceUse{
                rgMip[dst], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/true });
            up.addUse(ResourceUse{
                rgMip[src], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/false });
            up.usesDynamicRendering = true;
            up.renderInfo = m_bloomUpRenderInfo[dst];
            // Stash this mip's params in stable per-pass storage; ctx points at it.
            m_bloomUpCtx[dst] = BloomPassCtx{ this, srcSet, dstExt, (uint32_t)dst };
            up.recordCtx = &m_bloomUpCtx[dst];
            up.record = [](void* ctx, VkCommandBuffer c){
                auto* pc = static_cast<BloomPassCtx*>(ctx);
                auto* self = pc->self;
                self->postViewport(c, pc->dstExt);
                vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, self->m_bloomUpPipe);
                vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, self->m_bloomLayout,
                                        0, 1, &pc->srcSet, 0, nullptr);
                vkCmdPushConstants(c, self->m_bloomLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(BloomPush), &self->m_bloomUpPush[pc->mip]);
                vkCmdDraw(c, 3, 1, 0, 0);
            };
            m_graph.addPass(std::move(up));
        }
    }

    // Set dynamic viewport+scissor to a post target's extent.
    void postViewport(VkCommandBuffer c, VkExtent2D ext) {
        VkViewport vp{ 0.0f, 0.0f, (float)ext.width, (float)ext.height, 0.0f, 1.0f };
        VkRect2D scis{ {0,0}, ext };
        vkCmdSetViewport(c, 0, 1, &vp);
        vkCmdSetScissor(c, 0, 1, &scis);
    }

    void buildAndExecuteGraph(VkCommandBuffer cmd, uint32_t imageIndex, bool wantCapture) {
        // The frame's COLOR target: the acquired swapchain image (windowed) or the
        // single persistent offscreen color image (headless). Both are imported
        // UNDEFINED at entry — the main pass CLEARs them, so prior contents are
        // intentionally discarded; there is no cross-frame color dependency.
        VkImage  colorTargetImg  = m_headless ? m_offscreenColorImg  : m_swapImages[imageIndex];
        VkImageView colorTargetView = m_headless ? m_offscreenColorView : m_swapViews[imageIndex];

        // Import this frame's images with their correct ENTRY state. The color
        // target is freshly acquired/reused -> UNDEFINED. The depth buffer is
        // cleared each frame -> UNDEFINED is valid. The shadow map persists its
        // prior-frame state across frames (DEPTH_READ_ONLY after the last main pass
        // sampled it), except on the very first use where it is UNDEFINED.
        RgResource rgColor = m_graph.importImage("frame.color", colorTargetImg,
            ResourceState{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 });
        RgResource rgDepth = m_graph.importImage("scene.depth", m_depthImg,
            ResourceState{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 });
        RgResource rgShadow = m_graph.importImage("shadow.map", m_shadowImg, m_shadowState);

        // HDR pipeline resources: the linear HDR scene target (main pass writes it,
        // bloom + composite read it) and the bloom mip chain. All imported UNDEFINED
        // each frame (fully overwritten by their producing pass -> no cross-frame
        // color dependency). The graph derives every transition between them.
        RgResource rgHdr = m_graph.importImage("scene.hdr", m_hdrImg,
            ResourceState{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 });
        RgResource rgMip[kBloomMips];
        for (uint32_t i = 0; i < kBloomMips; ++i)
            rgMip[i] = m_graph.importImage("bloom.mip", m_bloomMips[i].img,
                ResourceState{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 });

        // SSAO images (half-res). Imported UNDEFINED each frame (fully overwritten
        // by their producing pass). Only used when SSAO is enabled this frame.
        const bool ssaoOn = m_ssao.enabled;
        // GI (screen-space indirect diffuse). Adds a half-res gather/temporal/denoise
        // chain after the main color pass + a full-res additive apply, before bloom.
        const bool giOn = m_gi.enabled;
        // The camera depth PRE-PASS runs when SSAO OR GI need a complete depth buffer
        // before the post chain; the main pass then tests EQUAL (no depth write).
        const bool prePassOn = ssaoOn || giOn;
        // Water adds a pass after the main mesh pass that samples + depth-tests the
        // scene depth this frame (gated; OFF == no water pass + zero cost).
        const bool waterOn = m_water.enabled;
        // Particles/decals: add the HDR transparent pass only when something was
        // submitted this frame (zero GPU cost when idle). It samples the scene depth
        // (soft particles) + depth-tests against it, like water.
        const bool particlesOn = (m_partAddCount + m_partAlphaCount + m_decalCount) > 0;
        // GPU-compute debris (K-T2): a compute pass integrates the pool this frame
        // (when stepped), and the live pool is drawn (when requested) into the HDR
        // target with read-only scene depth — exactly like the particle pass.
        const bool debrisStep = m_debrisStepPending && m_debrisComputePipeline;
        const bool debrisDraw = m_debrisDrawPending && m_debrisDrawPipeline;
        // The scene depth must be STORED (not transient) when water/GI/particles/debris read it.
        const bool storeDepth = waterOn || giOn || particlesOn || debrisDraw;
        RgResource rgSsaoRaw  = m_graph.importImage("ssao.raw",  m_ssaoRawImg,
            ResourceState{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 });
        RgResource rgSsaoBlur = m_graph.importImage("ssao.blur", m_ssaoBlurImg,
            ResourceState{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 });
        // GI half-res buffers + the prev-depth copy. Imported each frame; the accum
        // buffers persist across frames (history), but the graph only tracks layout
        // within a frame so importing UNDEFINED is correct (each is fully written by
        // its producing pass before being read; the cross-frame data lives in the
        // image memory, not the graph's per-frame layout state).
        RgResource rgGiRaw = {}, rgGiAccumW = {}, rgGiAccumH = {}, rgGiDenoise = {}, rgGiPrevDepth = {};
        if (giOn) {
            const uint32_t writeIdx = m_giAccumWrite, histIdx = writeIdx ^ 1u;
            rgGiRaw = m_graph.importImage("gi.raw", m_giRawImg,
                ResourceState{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 });
            rgGiAccumW = m_graph.importImage("gi.accumW", m_giAccumImg[writeIdx],
                ResourceState{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 });
            rgGiAccumH = m_graph.importImage("gi.accumH", m_giAccumImg[histIdx],
                ResourceState{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 });
            rgGiDenoise = m_graph.importImage("gi.denoise", m_giDenoiseImg,
                ResourceState{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 });
            rgGiPrevDepth = m_graph.importImage("gi.prevDepth", m_giPrevDepthImg,
                ResourceState{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 });
        }

        // ---- Pass 0a: GPU compute skinning pre-pass (GPU SKINNING OF MODELS) --
        // Skin every registered+palette-set mesh into its per-frame skinned-output
        // vbo with one vkCmdDispatch per instance, BEFORE the shadow/depth/color
        // passes (which then draw the skinned geometry through their unchanged vertex
        // shaders — so the mesh is skinned ONCE for all three passes). An SSBO write
        // -> vertex-read barrier inside the record body orders the dispatch before the
        // draws. Synchronous on the graphics queue (correct on Pascal). Gated: zero
        // cost when no skinned palette was set this frame.
        const bool skinStep = m_skinStepPending && m_skinPipeline && !m_skinPending.empty();
        if (skinStep) {
            RenderPassDesc sc{};
            sc.name = "skin-compute";
            sc.queue = RgQueue::Compute;
            sc.usesDynamicRendering = false;
            sc.recordCtx = this;
            sc.record = [](void* ctx, VkCommandBuffer c){
                static_cast<VulkanRenderDevice*>(ctx)->recordSkinComputeBody(c); };
            m_graph.addPass(std::move(sc));
        }

        // ---- Pass 0: GPU-compute debris integrate (K-T2) --------------------
        // The FIRST compute pass in the renderer. Integrates the persistent debris
        // pool SSBO (gravity, ground/AABB collision, damping, sleep, lifetime free)
        // with one vkCmdDispatch over the pool capacity. No graph-tracked IMAGE uses
        // (the pool is an SSBO; the compute->draw and compute->host barrier is emitted
        // inside the record body). Synchronous on the graphics queue — correct on
        // Pascal where async-compute overlap is weak. Gated: zero cost when not stepped.
        if (debrisStep) {
            RenderPassDesc dc{};
            dc.name = "debris-compute";
            dc.queue = RgQueue::Compute;
            dc.usesDynamicRendering = false;
            dc.recordCtx = this;
            dc.record = [](void* ctx, VkCommandBuffer c){
                static_cast<VulkanRenderDevice*>(ctx)->recordDebrisComputeBody(c); };
            m_graph.addPass(std::move(dc));
        }

        // ---- Pass 1: shadow depth pass --------------------------------------
        {
            m_shadowDepthAttach = VkRenderingAttachmentInfo{};
            m_shadowDepthAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            m_shadowDepthAttach.imageView = m_shadowView;
            m_shadowDepthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            m_shadowDepthAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            m_shadowDepthAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;  // sampled in main pass
            m_shadowDepthAttach.clearValue.depthStencil = { 1.0f, 0 };

            RenderPassDesc shadowPass{};
            shadowPass.name = "shadow-depth";
            shadowPass.addUse(ResourceUse{
                rgShadow, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_ASPECT_DEPTH_BIT, /*isWrite=*/true });
            shadowPass.usesDynamicRendering = true;
            m_shadowRenderInfo = VkRenderingInfo{};
            m_shadowRenderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            m_shadowRenderInfo.renderArea = { {0,0}, { kShadowDim, kShadowDim } };
            m_shadowRenderInfo.layerCount = 1;
            m_shadowRenderInfo.colorAttachmentCount = 0;
            m_shadowRenderInfo.pDepthAttachment = &m_shadowDepthAttach;
            shadowPass.renderInfo = m_shadowRenderInfo;
            shadowPass.recordCtx = this;
            shadowPass.record = [](void* ctx, VkCommandBuffer c){
                static_cast<VulkanRenderDevice*>(ctx)->recordShadowPassBody(c); };
            m_graph.addPass(std::move(shadowPass));
        }

        // ---- Depth pre-pass + SSAO chain ------------------------------------
        // The depth pre-pass writes the camera depth buffer so SSAO/GI have a
        // complete depth image BEFORE lighting; the main color pass then runs
        // depth-test EQUAL with depth-write off (same geometry, same depth). It runs
        // whenever SSAO OR GI is enabled. When neither is on, none of these run and
        // the main pass clears+writes depth itself (LESS). The SSAO + blur passes
        // are nested under ssaoOn (GI needs the depth pre-pass but not the AO passes).
        if (prePassOn) {
            // Pass: depth pre-pass (camera POV) -> m_depthImg (DEPTH_ATTACHMENT).
            {
                m_depthPreAttach = VkRenderingAttachmentInfo{};
                m_depthPreAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                m_depthPreAttach.imageView = m_depthView;
                m_depthPreAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                m_depthPreAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                m_depthPreAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;  // sampled by SSAO + reused by main pass
                m_depthPreAttach.clearValue.depthStencil = { 1.0f, 0 };

                RenderPassDesc dpre{};
                dpre.name = "depth-prepass";
                dpre.addUse(ResourceUse{
                    rgDepth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT, /*isWrite=*/true });
                dpre.usesDynamicRendering = true;
                m_depthPreRenderInfo = VkRenderingInfo{};
                m_depthPreRenderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                m_depthPreRenderInfo.renderArea = { {0,0}, m_extent };
                m_depthPreRenderInfo.layerCount = 1;
                m_depthPreRenderInfo.colorAttachmentCount = 0;
                m_depthPreRenderInfo.pDepthAttachment = &m_depthPreAttach;
                dpre.renderInfo = m_depthPreRenderInfo;
                dpre.recordCtx = this;
                dpre.record = [](void* ctx, VkCommandBuffer c){
                    static_cast<VulkanRenderDevice*>(ctx)->recordDepthPrePassBody(c); };
                m_graph.addPass(std::move(dpre));
            }
          if (ssaoOn) {
            // Pass: SSAO (read depth as DEPTH_READ_ONLY, write raw AO) -> half-res.
            {
                m_ssaoAttach = VkRenderingAttachmentInfo{};
                m_ssaoAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                m_ssaoAttach.imageView = m_ssaoRawView;
                m_ssaoAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                m_ssaoAttach.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                m_ssaoAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

                RenderPassDesc sp{};
                sp.name = "ssao";
                sp.addUse(ResourceUse{
                    rgSsaoRaw, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/true });
                sp.addUse(ResourceUse{
                    rgDepth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT, /*isWrite=*/false });
                sp.usesDynamicRendering = true;
                m_ssaoRenderInfo = VkRenderingInfo{};
                m_ssaoRenderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                m_ssaoRenderInfo.renderArea = { {0,0}, m_ssaoExtent };
                m_ssaoRenderInfo.layerCount = 1;
                m_ssaoRenderInfo.colorAttachmentCount = 1;
                m_ssaoRenderInfo.pColorAttachments = &m_ssaoAttach;
                sp.renderInfo = m_ssaoRenderInfo;
                sp.recordCtx = this;
                sp.record = [](void* ctx, VkCommandBuffer c){
                    auto* self = static_cast<VulkanRenderDevice*>(ctx);
                    self->postViewport(c, self->m_ssaoExtent);
                    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, self->m_ssaoPipe);
                    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, self->m_ssaoLayout,
                                            0, 1, &self->m_ssaoSet[self->m_frameIdx], 0, nullptr);
                    vkCmdDraw(c, 3, 1, 0, 0);
                };
                m_graph.addPass(std::move(sp));
            }
            // Pass: SSAO blur (read raw AO + depth, write blurred AO) -> half-res.
            {
                m_ssaoBlurAttach = VkRenderingAttachmentInfo{};
                m_ssaoBlurAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                m_ssaoBlurAttach.imageView = m_ssaoBlurView;
                m_ssaoBlurAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                m_ssaoBlurAttach.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                m_ssaoBlurAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

                m_ssaoBlurPush.aoTexel[0] = 1.0f / (float)std::max(1u, m_ssaoExtent.width);
                m_ssaoBlurPush.aoTexel[1] = 1.0f / (float)std::max(1u, m_ssaoExtent.height);
                m_ssaoBlurPush.depthSigma = 0.0008f;  // clip-z depth-similarity falloff
                m_ssaoBlurPush.pad0 = 0.0f;

                RenderPassDesc bp{};
                bp.name = "ssao-blur";
                bp.addUse(ResourceUse{
                    rgSsaoBlur, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/true });
                bp.addUse(ResourceUse{
                    rgSsaoRaw, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/false });
                bp.addUse(ResourceUse{
                    rgDepth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT, /*isWrite=*/false });
                bp.usesDynamicRendering = true;
                m_ssaoBlurRenderInfo = VkRenderingInfo{};
                m_ssaoBlurRenderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                m_ssaoBlurRenderInfo.renderArea = { {0,0}, m_ssaoExtent };
                m_ssaoBlurRenderInfo.layerCount = 1;
                m_ssaoBlurRenderInfo.colorAttachmentCount = 1;
                m_ssaoBlurRenderInfo.pColorAttachments = &m_ssaoBlurAttach;
                bp.renderInfo = m_ssaoBlurRenderInfo;
                bp.recordCtx = this;
                bp.record = [](void* ctx, VkCommandBuffer c){
                    auto* self = static_cast<VulkanRenderDevice*>(ctx);
                    self->postViewport(c, self->m_ssaoExtent);
                    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, self->m_ssaoBlurPipe);
                    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, self->m_ssaoBlurLayout,
                                            0, 1, &self->m_ssaoBlurSet, 0, nullptr);
                    vkCmdPushConstants(c, self->m_ssaoBlurLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                                       0, sizeof(SsaoBlurPush), &self->m_ssaoBlurPush);
                    vkCmdDraw(c, 3, 1, 0, 0);
                };
                m_graph.addPass(std::move(bp));
            }
          } // if (ssaoOn) — SSAO + blur passes
        } // if (prePassOn) — depth pre-pass (+ optional SSAO)

        // ---- Pass 2: main color pass (sky + meshes) -> LINEAR HDR target ----
        // Renders the lit scene into the R16G16B16A16_SFLOAT HDR target in linear
        // light (no tonemap). The HUD is drawn later, in the composite pass, on the
        // tonemapped LDR image. The clear color is the SAME dark slate as before but
        // in LINEAR HDR (the composite's ACES curve maps it back to the prior look).
        {
            m_hdrColorAttach = VkRenderingAttachmentInfo{};
            m_hdrColorAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            m_hdrColorAttach.imageView = m_hdrView;
            m_hdrColorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            m_hdrColorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            m_hdrColorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            m_hdrColorAttach.clearValue.color = { { 0.04f, 0.05f, 0.08f, 1.0f } }; // dark slate (linear)

            // Depth: with the pre-pass on (SSAO or GI), depth is already populated,
            // so LOAD it (preserve) + the EQUAL pipeline writes nothing. Otherwise
            // this pass owns depth: CLEAR + the LESS/write pipeline.
            m_mainDepthAttach = VkRenderingAttachmentInfo{};
            m_mainDepthAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            m_mainDepthAttach.imageView = m_depthView;
            m_mainDepthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            m_mainDepthAttach.loadOp = prePassOn ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
            // Water OR GI (when enabled) sample + read this depth in a following pass,
            // so STORE it; otherwise the depth is transient.
            m_mainDepthAttach.storeOp = storeDepth ? VK_ATTACHMENT_STORE_OP_STORE
                                                   : VK_ATTACHMENT_STORE_OP_DONT_CARE;
            m_mainDepthAttach.clearValue.depthStencil = { 1.0f, 0 };

            RenderPassDesc colorPass{};
            colorPass.name = "main-color";
            colorPass.addUse(ResourceUse{
                rgHdr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/true });
            // Depth use: with the pre-pass on it wrote depth (we LOAD + test EQUAL,
            // no write) so declare it READ; otherwise this pass writes depth. Either
            // way it must end as DEPTH_ATTACHMENT for this pass; the graph derives the
            // transition from the pre-pass / SSAO pass's prior state.
            colorPass.addUse(ResourceUse{
                rgDepth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                prePassOn ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                          : VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_ASPECT_DEPTH_BIT, /*isWrite=*/!prePassOn });
            // READ the shadow map in DEPTH_READ_ONLY — the graph derives the
            // DEPTH_ATTACHMENT->DEPTH_READ_ONLY transition from pass 1's write state.
            colorPass.addUse(ResourceUse{
                rgShadow, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_IMAGE_ASPECT_DEPTH_BIT, /*isWrite=*/false });
            // When SSAO is on, mesh.frag samples the blurred AO texture — declare it
            // so the graph transitions it COLOR_ATTACHMENT -> SHADER_READ_ONLY.
            if (ssaoOn) {
                colorPass.addUse(ResourceUse{
                    rgSsaoBlur, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/false });
            }
            colorPass.usesDynamicRendering = true;
            m_mainRenderInfo = VkRenderingInfo{};
            m_mainRenderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            m_mainRenderInfo.renderArea = { {0,0}, m_extent };
            m_mainRenderInfo.layerCount = 1;
            m_mainRenderInfo.colorAttachmentCount = 1;
            m_mainRenderInfo.pColorAttachments = &m_hdrColorAttach;
            m_mainRenderInfo.pDepthAttachment = &m_mainDepthAttach;
            colorPass.renderInfo = m_mainRenderInfo;
            colorPass.recordCtx = this;
            colorPass.record = [](void* ctx, VkCommandBuffer c){
                static_cast<VulkanRenderDevice*>(ctx)->recordMainPassBody(c); };
            m_graph.addPass(std::move(colorPass));
        }

        // ---- Water pass (undersea-world foundation) -------------------------
        // Drawn AFTER the opaque mesh pass into the SAME linear HDR target (LOAD,
        // so the lit scene + sky stay), depth-testing LESS_OR_EQUAL against the
        // stored scene depth (so terrain in front of the sea occludes it) WITHOUT
        // writing depth, and SAMPLING that same depth for the depth-based color.
        // The depth is used in DEPTH_READ_ONLY layout for BOTH the read-only depth
        // attachment and the texture sample — one declared use covers both (the
        // graph derives the DEPTH_ATTACHMENT -> DEPTH_READ_ONLY transition). Only
        // added when water is enabled (zero cost otherwise).
        if (waterOn) {
            m_waterColorAttach = VkRenderingAttachmentInfo{};
            m_waterColorAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            m_waterColorAttach.imageView = m_hdrView;
            m_waterColorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            m_waterColorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;   // keep the lit scene + sky
            m_waterColorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            m_waterDepthAttach = VkRenderingAttachmentInfo{};
            m_waterDepthAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            m_waterDepthAttach.imageView = m_depthView;
            m_waterDepthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL; // read-only depth
            m_waterDepthAttach.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            m_waterDepthAttach.storeOp = VK_ATTACHMENT_STORE_OP_NONE;

            RenderPassDesc waterPass{};
            waterPass.name = "water";
            waterPass.addUse(ResourceUse{
                rgHdr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/true });
            // Depth: read-only attachment AND sampled texture, same DEPTH_READ_ONLY
            // layout. Combined fragment-test + fragment-shader stages, read access.
            waterPass.addUse(ResourceUse{
                rgDepth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT
                    | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_IMAGE_ASPECT_DEPTH_BIT, /*isWrite=*/false });
            waterPass.usesDynamicRendering = true;
            m_waterRenderInfo = VkRenderingInfo{};
            m_waterRenderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            m_waterRenderInfo.renderArea = { {0,0}, m_extent };
            m_waterRenderInfo.layerCount = 1;
            m_waterRenderInfo.colorAttachmentCount = 1;
            m_waterRenderInfo.pColorAttachments = &m_waterColorAttach;
            m_waterRenderInfo.pDepthAttachment = &m_waterDepthAttach;
            waterPass.renderInfo = m_waterRenderInfo;
            waterPass.recordCtx = this;
            waterPass.record = [](void* ctx, VkCommandBuffer c){
                static_cast<VulkanRenderDevice*>(ctx)->recordWaterPassBody(c); };
            m_graph.addPass(std::move(waterPass));
        }

        // ================================================================
        // GI CHAIN (screen-space indirect diffuse) — after the lit scene exists,
        // before bloom. gather (half-res) -> temporal -> denoise -> apply (additive
        // into the HDR target) -> prev-depth copy (for next frame's reprojection).
        // All half-res except the full-res additive apply. Gated by giOn (zero cost
        // when disabled). The graph derives every layout transition.
        // ----------------------------------------------------------------
        if (giOn) {
            // ---- GI gather: read depth + lit HDR scene -> half-res raw radiance.
            {
                m_giGatherAttach = VkRenderingAttachmentInfo{};
                m_giGatherAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                m_giGatherAttach.imageView = m_giRawView;
                m_giGatherAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                m_giGatherAttach.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                m_giGatherAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

                RenderPassDesc gp{};
                gp.name = "gi-gather";
                gp.addUse(ResourceUse{
                    rgGiRaw, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/true });
                gp.addUse(ResourceUse{
                    rgDepth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT, /*isWrite=*/false });
                gp.addUse(ResourceUse{
                    rgHdr, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/false });
                gp.usesDynamicRendering = true;
                m_giGatherRenderInfo = VkRenderingInfo{};
                m_giGatherRenderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                m_giGatherRenderInfo.renderArea = { {0,0}, m_giExtent };
                m_giGatherRenderInfo.layerCount = 1;
                m_giGatherRenderInfo.colorAttachmentCount = 1;
                m_giGatherRenderInfo.pColorAttachments = &m_giGatherAttach;
                gp.renderInfo = m_giGatherRenderInfo;
                gp.recordCtx = this;
                gp.record = [](void* ctx, VkCommandBuffer c){
                    auto* self = static_cast<VulkanRenderDevice*>(ctx);
                    self->postViewport(c, self->m_giExtent);
                    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, self->m_giGatherPipe);
                    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, self->m_giGatherLayout,
                                            0, 1, &self->m_giGatherSet[self->m_frameIdx], 0, nullptr);
                    vkCmdDraw(c, 3, 1, 0, 0);
                };
                m_graph.addPass(std::move(gp));
            }
            // ---- GI temporal: blend raw with reprojected history -> accum[write].
            {
                m_giTemporalAttach = VkRenderingAttachmentInfo{};
                m_giTemporalAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                m_giTemporalAttach.imageView = m_giAccumView[m_giAccumWrite];
                m_giTemporalAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                m_giTemporalAttach.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                m_giTemporalAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

                RenderPassDesc tp{};
                tp.name = "gi-temporal";
                tp.addUse(ResourceUse{
                    rgGiAccumW, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/true });
                tp.addUse(ResourceUse{
                    rgGiRaw, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/false });
                // History accum buffer (read-only this frame). On the first frame the
                // shader ignores it (valid=0); the import-UNDEFINED is harmless then.
                tp.addUse(ResourceUse{
                    rgGiAccumH, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/false });
                tp.addUse(ResourceUse{
                    rgDepth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT, /*isWrite=*/false });
                tp.addUse(ResourceUse{
                    rgGiPrevDepth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT, /*isWrite=*/false });
                tp.usesDynamicRendering = true;
                m_giTemporalRenderInfo = VkRenderingInfo{};
                m_giTemporalRenderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                m_giTemporalRenderInfo.renderArea = { {0,0}, m_giExtent };
                m_giTemporalRenderInfo.layerCount = 1;
                m_giTemporalRenderInfo.colorAttachmentCount = 1;
                m_giTemporalRenderInfo.pColorAttachments = &m_giTemporalAttach;
                tp.renderInfo = m_giTemporalRenderInfo;
                tp.recordCtx = this;
                tp.record = [](void* ctx, VkCommandBuffer c){
                    auto* self = static_cast<VulkanRenderDevice*>(ctx);
                    self->postViewport(c, self->m_giExtent);
                    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, self->m_giTemporalPipe);
                    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, self->m_giTemporalLayout,
                                            0, 1, &self->m_giTemporalSet[self->m_frameIdx], 0, nullptr);
                    vkCmdDraw(c, 3, 1, 0, 0);
                };
                m_graph.addPass(std::move(tp));
            }
            // ---- GI denoise: depth-aware bilateral -> denoise buffer.
            {
                m_giBlurAttach = VkRenderingAttachmentInfo{};
                m_giBlurAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                m_giBlurAttach.imageView = m_giDenoiseView;
                m_giBlurAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                m_giBlurAttach.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                m_giBlurAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

                RenderPassDesc dp{};
                dp.name = "gi-denoise";
                dp.addUse(ResourceUse{
                    rgGiDenoise, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/true });
                dp.addUse(ResourceUse{
                    rgGiAccumW, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/false });
                dp.addUse(ResourceUse{
                    rgDepth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT, /*isWrite=*/false });
                dp.usesDynamicRendering = true;
                m_giBlurRenderInfo = VkRenderingInfo{};
                m_giBlurRenderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                m_giBlurRenderInfo.renderArea = { {0,0}, m_giExtent };
                m_giBlurRenderInfo.layerCount = 1;
                m_giBlurRenderInfo.colorAttachmentCount = 1;
                m_giBlurRenderInfo.pColorAttachments = &m_giBlurAttach;
                dp.renderInfo = m_giBlurRenderInfo;
                dp.recordCtx = this;
                dp.record = [](void* ctx, VkCommandBuffer c){
                    auto* self = static_cast<VulkanRenderDevice*>(ctx);
                    self->postViewport(c, self->m_giExtent);
                    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, self->m_giBlurPipe);
                    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, self->m_giBlurLayout,
                                            0, 1, &self->m_giBlurSet[self->m_frameIdx], 0, nullptr);
                    vkCmdPushConstants(c, self->m_giBlurLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                                       0, sizeof(GiBlurPush), &self->m_giBlurPush);
                    vkCmdDraw(c, 3, 1, 0, 0);
                };
                m_graph.addPass(std::move(dp));
            }
            // ---- GI apply: full-res depth-aware up-sample + ADDITIVE into the HDR
            //      scene (modulated by SSAO AO). Writes rgHdr (load existing scene).
            {
                m_giApplyAttach = VkRenderingAttachmentInfo{};
                m_giApplyAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                m_giApplyAttach.imageView = m_hdrView;
                m_giApplyAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                m_giApplyAttach.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;     // keep the lit scene
                m_giApplyAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

                RenderPassDesc ap{};
                ap.name = "gi-apply";
                ap.addUse(ResourceUse{
                    rgHdr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/true });
                ap.addUse(ResourceUse{
                    rgGiDenoise, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/false });
                ap.addUse(ResourceUse{
                    rgDepth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT, /*isWrite=*/false });
                // The AO read (binding 2) is the blurred SSAO when SSAO ran this frame
                // (already in SHADER_READ_ONLY from the main pass); when SSAO is off the
                // apply set binds the GI denoise image instead (already declared above)
                // and forces aoAmount=0, so no extra/incorrect resource use is needed.
                if (ssaoOn) {
                    ap.addUse(ResourceUse{
                        rgSsaoBlur, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/false });
                }
                ap.usesDynamicRendering = true;
                m_giApplyRenderInfo = VkRenderingInfo{};
                m_giApplyRenderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                m_giApplyRenderInfo.renderArea = { {0,0}, m_extent };
                m_giApplyRenderInfo.layerCount = 1;
                m_giApplyRenderInfo.colorAttachmentCount = 1;
                m_giApplyRenderInfo.pColorAttachments = &m_giApplyAttach;
                ap.renderInfo = m_giApplyRenderInfo;
                ap.recordCtx = this;
                ap.record = [](void* ctx, VkCommandBuffer c){
                    auto* self = static_cast<VulkanRenderDevice*>(ctx);
                    VkViewport vp{ 0.0f, 0.0f, (float)self->m_extent.width, (float)self->m_extent.height, 0.0f, 1.0f };
                    VkRect2D scis{ {0,0}, self->m_extent };
                    vkCmdSetViewport(c, 0, 1, &vp);
                    vkCmdSetScissor(c, 0, 1, &scis);
                    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, self->m_giApplyPipe);
                    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, self->m_giApplyLayout,
                                            0, 1, &self->m_giApplySet[self->m_frameIdx], 0, nullptr);
                    vkCmdPushConstants(c, self->m_giApplyLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                                       0, sizeof(GiApplyPush), &self->m_giApplyPush);
                    vkCmdDraw(c, 3, 1, 0, 0);
                };
                m_graph.addPass(std::move(ap));
            }
            // ---- Prev-depth copy: snapshot THIS frame's depth into the persistent
            //      prev-depth image for NEXT frame's temporal reprojection. Runs last
            //      in the GI chain (after temporal consumed last frame's prev-depth).
            {
                RenderPassDesc cp{};
                cp.name = "gi-prevdepth-copy";
                cp.addUse(ResourceUse{
                    rgDepth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT, /*isWrite=*/false });
                cp.addUse(ResourceUse{
                    rgGiPrevDepth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT, /*isWrite=*/true });
                cp.recordCtx = this;
                cp.record = [](void* ctx, VkCommandBuffer c){
                    auto* self = static_cast<VulkanRenderDevice*>(ctx);
                    VkImageCopy region{};
                    region.srcSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
                    region.dstSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
                    region.extent = { self->m_extent.width, self->m_extent.height, 1 };
                    vkCmdCopyImage(c, self->m_depthImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   self->m_giPrevDepthImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                };
                m_graph.addPass(std::move(cp));
            }
        }

        // ---- GPU-compute debris draw pass (K-T2) ----------------------------
        // ONE instanced unit-cube draw over the whole pool capacity into the SAME
        // linear HDR target (LOAD), with read-only scene depth (depth-TEST, no write)
        // — exactly the resource pattern the particle pass uses, so the graph derives
        // the DEPTH_ATTACHMENT->DEPTH_READ_ONLY transition. Dead pool slots collapse
        // to nothing in the vertex shader (no compaction). Gated: zero cost when idle.
        if (debrisDraw) {
            m_debrisColorAttach = VkRenderingAttachmentInfo{};
            m_debrisColorAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            m_debrisColorAttach.imageView = m_hdrView;
            m_debrisColorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            m_debrisColorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            m_debrisColorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            m_debrisDepthAttach = VkRenderingAttachmentInfo{};
            m_debrisDepthAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            m_debrisDepthAttach.imageView = m_depthView;
            m_debrisDepthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
            m_debrisDepthAttach.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            m_debrisDepthAttach.storeOp = VK_ATTACHMENT_STORE_OP_NONE;

            RenderPassDesc dp{};
            dp.name = "debris-draw";
            dp.addUse(ResourceUse{
                rgHdr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/true });
            dp.addUse(ResourceUse{
                rgDepth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                VK_IMAGE_ASPECT_DEPTH_BIT, /*isWrite=*/false });
            dp.usesDynamicRendering = true;
            m_debrisRenderInfo = VkRenderingInfo{};
            m_debrisRenderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            m_debrisRenderInfo.renderArea = { {0,0}, m_extent };
            m_debrisRenderInfo.layerCount = 1;
            m_debrisRenderInfo.colorAttachmentCount = 1;
            m_debrisRenderInfo.pColorAttachments = &m_debrisColorAttach;
            m_debrisRenderInfo.pDepthAttachment = &m_debrisDepthAttach;
            dp.renderInfo = m_debrisRenderInfo;
            dp.recordCtx = this;
            dp.record = [](void* ctx, VkCommandBuffer c){
                static_cast<VulkanRenderDevice*>(ctx)->recordDebrisDrawBody(c); };
            m_graph.addPass(std::move(dp));
        }

        // ---- Particle + decal pass (combat juice) ---------------------------
        // Drawn AFTER opaque + water + the full GI chain into the SAME linear HDR
        // target (LOAD, so the lit scene stays), BEFORE bloom (so bright additive
        // sparks/muzzle feed the bloom chain). Depth-tests LESS_OR_EQUAL against the
        // stored scene depth WITHOUT writing it, and SAMPLES that same depth for the
        // soft-particle fade — both in DEPTH_READ_ONLY, one declared use covers both
        // (the graph derives DEPTH_ATTACHMENT -> DEPTH_READ_ONLY). Only added when
        // something was submitted this frame (gated by particlesOn -> zero idle cost).
        if (particlesOn) {
            m_partColorAttach = VkRenderingAttachmentInfo{};
            m_partColorAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            m_partColorAttach.imageView = m_hdrView;
            m_partColorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            m_partColorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;   // keep the lit scene
            m_partColorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            m_partDepthAttach = VkRenderingAttachmentInfo{};
            m_partDepthAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            m_partDepthAttach.imageView = m_depthView;
            m_partDepthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL; // read-only depth
            m_partDepthAttach.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            m_partDepthAttach.storeOp = VK_ATTACHMENT_STORE_OP_NONE;

            RenderPassDesc partPass{};
            partPass.name = "particles";
            partPass.addUse(ResourceUse{
                rgHdr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/true });
            partPass.addUse(ResourceUse{
                rgDepth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT
                    | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_IMAGE_ASPECT_DEPTH_BIT, /*isWrite=*/false });
            partPass.usesDynamicRendering = true;
            m_partRenderInfo = VkRenderingInfo{};
            m_partRenderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            m_partRenderInfo.renderArea = { {0,0}, m_extent };
            m_partRenderInfo.layerCount = 1;
            m_partRenderInfo.colorAttachmentCount = 1;
            m_partRenderInfo.pColorAttachments = &m_partColorAttach;
            m_partRenderInfo.pDepthAttachment = &m_partDepthAttach;
            partPass.renderInfo = m_partRenderInfo;
            partPass.recordCtx = this;
            partPass.record = [](void* ctx, VkCommandBuffer c){
                static_cast<VulkanRenderDevice*>(ctx)->recordParticlePassBody(c); };
            m_graph.addPass(std::move(partPass));
        }

        // ================================================================
        // BLOOM CHAIN + HDR COMPOSITE (HDR pipeline).
        // ----------------------------------------------------------------
        // Downsample: pass 0 bright-passes the HDR scene into mip0 (Karis-average
        // 13-tap), passes 1..N-1 progressively downsample mip[i-1] -> mip[i].
        // Upsample: from the smallest mip back up, each step tent-filters mip[i+1]
        // and ADDITIVELY blends it onto mip[i] (pipeline ONE,ONE blend). Result:
        // mip0 holds the full accumulated bloom. The graph derives every
        // COLOR_ATTACHMENT <-> SHADER_READ_ONLY transition between the mips.
        addBloomPasses(rgHdr, rgMip);

        // Composite: HDR scene + bloom mip0 -> ACES tonemap -> LDR final target.
        // The HUD is recorded here (after tonemap) so it composites on the LDR
        // image. The pass writes the swapchain/offscreen color (rgColor).
        {
            m_compositeAttach = VkRenderingAttachmentInfo{};
            m_compositeAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            m_compositeAttach.imageView = colorTargetView;
            m_compositeAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            m_compositeAttach.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; // fully overwritten
            m_compositeAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            RenderPassDesc comp{};
            comp.name = "composite";
            // WRITE the final color target.
            comp.addUse(ResourceUse{
                rgColor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/true });
            // READ the HDR scene + bloom mip0 (both sampled in the fragment stage).
            comp.addUse(ResourceUse{
                rgHdr, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/false });
            comp.addUse(ResourceUse{
                rgMip[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/false });
            comp.usesDynamicRendering = true;
            m_compositeRenderInfo = VkRenderingInfo{};
            m_compositeRenderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            m_compositeRenderInfo.renderArea = { {0,0}, m_extent };
            m_compositeRenderInfo.layerCount = 1;
            m_compositeRenderInfo.colorAttachmentCount = 1;
            m_compositeRenderInfo.pColorAttachments = &m_compositeAttach;
            comp.renderInfo = m_compositeRenderInfo;
            comp.recordCtx = this;
            comp.record = [](void* ctx, VkCommandBuffer c){
                auto* self = static_cast<VulkanRenderDevice*>(ctx);
                VkViewport vp{ 0.0f, 0.0f, (float)self->m_extent.width, (float)self->m_extent.height, 0.0f, 1.0f };
                VkRect2D scis{ {0,0}, self->m_extent };
                vkCmdSetViewport(c, 0, 1, &vp);
                vkCmdSetScissor(c, 0, 1, &scis);
                vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, self->m_compositePipe);
                vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, self->m_compositeLayout,
                                        0, 1, &self->m_setComposite, 0, nullptr);
                CompositePush cp{};
                cp.bloomIntensity = kBloomIntensity;
                cp.exposure = 1.0f;
                vkCmdPushConstants(c, self->m_compositeLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(cp), &cp);
                vkCmdDraw(c, 3, 1, 0, 0);
                // HUD on top of the tonemapped LDR image (drawn in the composite pass).
                self->recordHudDraws(c);
                // GPU-frame END timestamp after the WHOLE pipeline (incl. bloom +
                // composite) so --bench measures the full added cost.
                auto& f = self->m_frames[self->m_frameIdx];
                if (self->m_tsSupported && f.tsPool) {
                    vkCmdWriteTimestamp2(c, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, f.tsPool, 1);
                    f.tsPending = true;
                }
            };
            m_graph.addPass(std::move(comp));
        }

        // ---- Pass 3: present finalize (or in-frame capture copy) ------------
        // The finalize layout differs by mode: WINDOWED leaves the color image in
        // PRESENT_SRC_KHR for vkQueuePresentKHR; HEADLESS never presents, so there
        // is no PRESENT_SRC transition (that layout requires the swapchain ext).
        const VkImageLayout finalLayout = m_headless
            ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL  // headless: nothing presents
            : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        if (wantCapture) {
            // The capture copy reads the color image as TRANSFER_SRC then leaves it
            // in the finalize layout. Two uses on the same resource within one pass
            // would be ambiguous, so express the capture as TWO tiny passes: one
            // that transitions to TRANSFER_SRC + does the copy, one that transitions
            // to the finalize layout. The graph derives both transitions.
            RenderPassDesc copyPass{};
            copyPass.name = "capture-copy";
            copyPass.addUse(ResourceUse{
                rgColor, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/false });
            // Stable per-pass storage for the color image handle (ctx points at the
            // device; the handle lives in a member that outlives execute()).
            m_captureColorImg = colorTargetImg;
            copyPass.recordCtx = this;
            copyPass.record = [](void* ctx, VkCommandBuffer c){
                auto* self = static_cast<VulkanRenderDevice*>(ctx);
                VkBufferImageCopy region{};
                region.bufferOffset = 0;
                region.bufferRowLength = 0;     // tightly packed to image width
                region.bufferImageHeight = 0;
                region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                region.imageOffset = { 0, 0, 0 };
                region.imageExtent = { self->m_captureW, self->m_captureH, 1 };
                vkCmdCopyImageToBuffer(c, self->m_captureColorImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       self->m_captureBuf, 1, &region);
            };
            m_graph.addPass(std::move(copyPass));

            // HEADLESS non-present: the copy already left the image TRANSFER_SRC and
            // nothing reads it afterward, so the extra COLOR_ATTACHMENT transition is
            // unnecessary work — skip the finalize pass entirely. WINDOWED still
            // needs the TRANSFER_SRC -> PRESENT_SRC transition for the present.
            if (!m_headless) {
                RenderPassDesc presentPass{};
                presentPass.name = "present";
                presentPass.addUse(ResourceUse{
                    rgColor, finalLayout,
                    VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0,
                    VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/false });
                presentPass.record = nullptr; // pure layout transition, no commands
                m_graph.addPass(std::move(presentPass));
            }
        } else if (!m_headless) {
            // WINDOWED no-capture: transition COLOR_ATTACHMENT -> PRESENT_SRC.
            // HEADLESS no-capture: nothing reads the image, so leave it in
            // COLOR_ATTACHMENT_OPTIMAL (no finalize pass; the image is re-imported
            // UNDEFINED + cleared next frame anyway).
            RenderPassDesc presentPass{};
            presentPass.name = "present";
            presentPass.addUse(ResourceUse{
                rgColor, finalLayout,
                VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0,
                VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/false });
            presentPass.record = nullptr;
            m_graph.addPass(std::move(presentPass));
        }

        m_graph.execute(cmd);

        // Persist the shadow map's post-frame state so next frame imports it with
        // the right entry layout (the main pass left it DEPTH_READ_ONLY). After the
        // first use we never see UNDEFINED again — the WAR barrier protecting last
        // frame's sampling reads is derived from this stored read state.
        m_shadowState = m_graph.stateOf(rgShadow);

        // GI ping-pong + history: this frame wrote accum[m_giAccumWrite] (now the
        // freshest accumulated GI) + snapshotted depth into prev-depth. Next frame
        // reads accum[m_giAccumWrite] as history and writes the OTHER buffer, so flip
        // the index. History becomes valid after the first GI frame (reprojection
        // safe once a previous frame + its depth + viewProj exist).
        if (giOn) {
            m_giAccumWrite ^= 1u;
            m_giHistoryValid = true;
        }
    }

    // ---- ROLE -> FONT FILE MAP (the ONE place to reassign a role's font) -------
    // To change which typeface a role uses: edit the path string here and restart
    // (no rebuild of the engine logic needed — the file ships in assets/). The
    // *static* (non-VariableFont) weights are used. `proportional=false` forces a
    // monospace cell (News/Console are genuinely mono; the proportional roles get
    // real per-glyph advances). The embedded Roboto Mono is the guaranteed fallback
    // for ANY role whose file is missing/unreadable, so text is NEVER blank.
    //
    // To EMBED a role's font for a single-binary ship: run
    //   tools/embed_font.ps1 -InFont assets/fonts/<dir>/<file>.ttf -Symbol k<Name>TTF
    // and point that role's load at the generated blob (see kRobotoMonoTTF usage).
    // Read a font file by trying a few roots so the load works regardless of the
    // process CWD (run-from-repo-root, run-from-build-dir, assets-next-to-exe). The
    // relative path (e.g. "Orbitron/static/Orbitron-Bold.ttf") is joined onto each
    // candidate prefix; the first readable hit wins. Returns the resolved path in
    // `outResolved`. Mirrors app/asset_root.h's candidate order but stays self-
    // contained in the engine layer (no app dependency). Empty vector => not found.
    std::vector<unsigned char> readFontFile(const char* relPath, std::string& outResolved) {
        std::vector<unsigned char> bytes;
        if (!relPath) return bytes;
        std::filesystem::path exeDir(".");
#ifdef _WIN32
        {
            char buf[1024];
            DWORD n = GetModuleFileNameA(nullptr, buf, (DWORD)sizeof(buf));
            if (n > 0 && n < sizeof(buf))
                exeDir = std::filesystem::path(std::string(buf, n)).parent_path();
        }
#endif
        const std::filesystem::path rel(relPath);
        const std::filesystem::path candidates[] = {
            std::filesystem::path("assets/fonts") / rel,                       // CWD = repo root
            exeDir / ".." / ".." / ".." / "assets" / "fonts" / rel,            // build/bin/<Config>
            exeDir / "assets" / "fonts" / rel,                                 // assets next to exe
            std::filesystem::path("../assets/fonts") / rel,                    // CWD = a subdir
        };
        for (const auto& c : candidates) {
            std::ifstream f(c, std::ios::binary | std::ios::ate);
            if (!f) continue;
            const std::streamsize sz = f.tellg();
            if (sz <= 4) continue;
            f.seekg(0);
            bytes.resize((size_t)sz);
            if (f.read(reinterpret_cast<char*>(bytes.data()), sz)) {
                std::error_code ec; auto norm = std::filesystem::weakly_canonical(c, ec);
                outResolved = (ec ? c : norm).string();
                return bytes;
            }
            bytes.clear();
        }
        return bytes;
    }

    struct RoleFontDesc { const char* path; bool proportional; const char* label; };
    static const RoleFontDesc* roleFontTable() {
        // Indexed by FontRole. Console==HudMono share index 0 (embedded mono).
        static const RoleFontDesc kRoleFontPaths[kFontRoleCount] = {
            /* 0 Console/HudMono */ { nullptr /* embedded Roboto Mono */,           false, "Roboto Mono (embedded)" },
            /* 1 Title           */ { "Orbitron/static/Orbitron-Bold.ttf",           true,  "Orbitron-Bold" },
            /* 2 Menu            */ { "Space_Grotesk/static/SpaceGrotesk-Medium.ttf", true,  "SpaceGrotesk-Medium" },
            /* 3 Enemy           */ { "Tektur/static/Tektur_Condensed-Bold.ttf",      true,  "Tektur_Condensed-Bold" },
            /* 4 News            */ { "Space_Mono/SpaceMono-Bold.ttf",                false, "SpaceMono-Bold" },
        };
        return kRoleFontPaths;
    }

    // Font atlas builder. Bakes ONE crisp glyph atlas PER FontRole from a real TTF
    // (assets/fonts/, embedded Roboto Mono fallback) via stb_truetype, and bakes the
    // legacy 8x8 bitmap font as the universal last-resort so text is NEVER blank.
    // Logs which typeface actually loaded for each role.
    bool buildFontAtlas() {
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

    static std::string roleName(int r) {
        switch (r) {
            case 0: return "Console/HudMono";
            case 1: return "Title";
            case 2: return "Menu";
            case 3: return "Enemy";
            case 4: return "News";
            default: return std::to_string(r);
        }
    }

    // Bake ASCII 32..126 from `ttf` into an antialiased R8 coverage atlas via
    // stb_truetype's packer (2x2 oversample for crisp small text), then expand to
    // RGBA (white, alpha=coverage) so the existing LINEAR-sampler HUD path renders
    // smooth, alpha-blended, per-vertex-tinted glyphs. Records per-glyph atlas UVs +
    // offsets + advance (bake-pixel units) into m_fonts[role]. Does NOT set ready.
    // (Takes the role index, not a FontAtlas&, so the signature needs no early type
    // visibility — FontAtlas is declared with the other members further below.)
    bool bakeTtfAtlas(int role, const unsigned char* ttf, size_t ttfSize) {
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

    // Build a 128x64 RGBA atlas (16 cols x 8 rows of 8x8 glyphs) from the embedded
    // public-domain font8x8_basic bits (white texel, alpha = pixel-on) into `dst`.
    // Universal last-resort fallback (NEVER ship blank text).
    bool buildBitmapFontAtlas(Texture& dst) {
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

    // Create the 2D HUD pipeline: NDC quads, no depth, alpha blend, one combined-
    // image-sampler set, per-frame HUD descriptor pools + vertex rings.
    bool createHud() {
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
            if (vkCreateDescriptorPool(m_dev.device, &pci, nullptr, &fr.hudDescPool) != VK_SUCCESS) {
                logError("[rhi] HUD descriptor pool failed"); return false;
            }
            VkBufferCreateInfo vbci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            vbci.size = (VkDeviceSize)kMaxHudVerts * sizeof(HudVertex);
            vbci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
            VmaAllocationCreateInfo vaci{};
            vaci.usage = VMA_MEMORY_USAGE_AUTO;
            vaci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo vinfo{};
            if (vmaCreateBuffer(m_alloc, &vbci, &vaci, &fr.hudVbo, &fr.hudVboAlloc, &vinfo) != VK_SUCCESS) {
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
        VkResult pr = vkCreateGraphicsPipelines(m_dev.device, VK_NULL_HANDLE, 1, &gpci, nullptr, &m_hudPipeline);

        vkDestroyShaderModule(m_dev.device, vs, nullptr);
        vkDestroyShaderModule(m_dev.device, fs, nullptr);
        if (pr != VK_SUCCESS) { logError("[rhi] HUD pipeline create failed"); return false; }

        logInfo("[rhi] HUD 2D pipeline ready (NDC quads + TTF/bitmap glyph atlas, alpha-blended, no depth)");
        return true;
    }

    // ---- Analytic sky (open-world track, task A) ---------------------------
    // Create the per-frame sky UBO buffers + descriptor sets and the full-screen
    // sky pipeline. The pipeline has NO vertex input (the vertex shader generates a
    // covering triangle from gl_VertexIndex), draws at far depth with depth test
    // LESS_OR_EQUAL + depth write OFF so opaque geometry occludes it and the sky
    // writes nothing to depth, and does not blend (opaque background fill).
    bool createSky() {
        // Set-0 layout: a single UBO (the SkyUBO) read by the fragment stage.
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0; b.descriptorCount = 1;
        b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        slci.bindingCount = 1; slci.pBindings = &b;
        if (vkCreateDescriptorSetLayout(m_dev.device, &slci, nullptr, &m_skySetLayout) != VK_SUCCESS) {
            logError("[rhi] sky set layout failed"); return false;
        }

        // One UBO descriptor per frame-in-flight.
        VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight };
        VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pci.maxSets = kFramesInFlight; pci.poolSizeCount = 1; pci.pPoolSizes = &ps;
        if (vkCreateDescriptorPool(m_dev.device, &pci, nullptr, &m_skyPool) != VK_SUCCESS) {
            logError("[rhi] sky desc pool failed"); return false;
        }

        // Per-frame UBO buffer (persistently mapped) + its descriptor set.
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            auto& fr = m_frames[i];
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = sizeof(SkyUBO);
            bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info{};
            if (vmaCreateBuffer(m_alloc, &bci, &aci, &fr.skyBuf, &fr.skyAlloc, &info) != VK_SUCCESS) {
                logError("[rhi] sky UBO create failed"); return false;
            }
            fr.skyMapped = info.pMappedData;

            VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dsai.descriptorPool = m_skyPool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &m_skySetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &dsai, &fr.skySet) != VK_SUCCESS) {
                logError("[rhi] sky set alloc failed"); return false;
            }
            VkDescriptorBufferInfo dbi{ fr.skyBuf, 0, sizeof(SkyUBO) };
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet = fr.skySet; w.dstBinding = 0; w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w.pBufferInfo = &dbi;
            vkUpdateDescriptorSets(m_dev.device, 1, &w, 0, nullptr);
        }

        // Shaders.
        VkShaderModule vs = loadShaderModule("shaders\\sky.vert.spv");
        VkShaderModule fs = loadShaderModule("shaders\\sky.frag.spv");
        if (!vs || !fs) return false;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vs; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

        // No vertex input: the vertex shader builds the triangle from gl_VertexIndex.
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

        // Far-depth fill: the sky's gl_Position.z == w (depth 1.0). LESS_OR_EQUAL
        // passes only where the cleared depth (1.0) still stands (no nearer
        // geometry). depthWrite OFF leaves the depth buffer untouched for later use.
        VkPipelineDepthStencilStateCreateInfo dss{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        dss.depthTestEnable = VK_TRUE; dss.depthWriteEnable = VK_FALSE;
        dss.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

        VkPipelineColorBlendAttachmentState cba{};
        cba.blendEnable = VK_FALSE; // opaque background fill
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 1; cb.pAttachments = &cba;

        VkDynamicState dyn[2]{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;

        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount = 1; plci.pSetLayouts = &m_skySetLayout; // no push constants
        if (vkCreatePipelineLayout(m_dev.device, &plci, nullptr, &m_skyLayout) != VK_SUCCESS) {
            logError("[rhi] sky pipeline layout failed"); return false;
        }

        // HDR pipeline: the sky also renders into the linear HDR scene target.
        const VkFormat hdrFmt = kHdrFormat;
        VkPipelineRenderingCreateInfo prci{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        prci.colorAttachmentCount = 1; prci.pColorAttachmentFormats = &hdrFmt;
        prci.depthAttachmentFormat = m_depthFormat;  // pass has a depth attachment

        VkGraphicsPipelineCreateInfo gpci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gpci.pNext = &prci;
        gpci.stageCount = 2; gpci.pStages = stages;
        gpci.pVertexInputState = &vin; gpci.pInputAssemblyState = &ia;
        gpci.pViewportState = &vp; gpci.pRasterizationState = &rs;
        gpci.pMultisampleState = &ms; gpci.pDepthStencilState = &dss;
        gpci.pColorBlendState = &cb; gpci.pDynamicState = &ds; gpci.layout = m_skyLayout;
        VkResult pr = vkCreateGraphicsPipelines(m_dev.device, VK_NULL_HANDLE, 1, &gpci, nullptr, &m_skyPipeline);

        vkDestroyShaderModule(m_dev.device, vs, nullptr);
        vkDestroyShaderModule(m_dev.device, fs, nullptr);
        if (pr != VK_SUCCESS) { logError("[rhi] sky pipeline create failed"); return false; }

        logInfo("[rhi] analytic sky pipeline ready (full-screen tri, far-depth, depth-test no-write)");
        return true;
    }

    void destroySky() {
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            auto& fr = m_frames[i];
            if (fr.skyBuf) { vmaDestroyBuffer(m_alloc, fr.skyBuf, fr.skyAlloc);
                             fr.skyBuf = VK_NULL_HANDLE; fr.skyAlloc = nullptr; fr.skyMapped = nullptr; }
        }
        if (m_skyPipeline)  { vkDestroyPipeline(m_dev.device, m_skyPipeline, nullptr); m_skyPipeline = VK_NULL_HANDLE; }
        if (m_skyLayout)    { vkDestroyPipelineLayout(m_dev.device, m_skyLayout, nullptr); m_skyLayout = VK_NULL_HANDLE; }
        if (m_skyPool)      { vkDestroyDescriptorPool(m_dev.device, m_skyPool, nullptr); m_skyPool = VK_NULL_HANDLE; }
        if (m_skySetLayout) { vkDestroyDescriptorSetLayout(m_dev.device, m_skySetLayout, nullptr); m_skySetLayout = VK_NULL_HANDLE; }
    }

    // ---- Water (undersea-world foundation) ---------------------------------
    // Build the unit-patch grid mesh (vec2 grid coord per vertex, [-1,1]), the
    // per-frame water UBO buffers + descriptor sets (set0: UBO + scene-depth
    // sampler), and the water graphics pipeline (water.vert/.frag; depth-test
    // LESS_OR_EQUAL, depth-write OFF — the water reads the depth the opaque pass
    // produced and never writes it, so post passes are unharmed). Created once;
    // the depth descriptor binding is (re)written by writeWaterDescriptors() at
    // init + on resize (the depth view changes).
    bool createWater() {
        // --- Unit-patch grid mesh (device-local; built once via staging). ---
        const uint32_t dim = kWaterGridDim;            // verts per edge
        std::vector<glm::vec2> verts; verts.reserve((size_t)dim * dim);
        for (uint32_t z = 0; z < dim; ++z) {
            for (uint32_t x = 0; x < dim; ++x) {
                float fx = (float)x / (float)(dim - 1) * 2.0f - 1.0f; // [-1,1]
                float fz = (float)z / (float)(dim - 1) * 2.0f - 1.0f;
                verts.emplace_back(fx, fz);
            }
        }
        std::vector<uint32_t> idx; idx.reserve((size_t)(dim - 1) * (dim - 1) * 6);
        for (uint32_t z = 0; z < dim - 1; ++z) {
            for (uint32_t x = 0; x < dim - 1; ++x) {
                uint32_t i0 = z * dim + x;
                uint32_t i1 = z * dim + (x + 1);
                uint32_t i2 = (z + 1) * dim + x;
                uint32_t i3 = (z + 1) * dim + (x + 1);
                // CCW from above (+Y); winding matches the device's front face.
                idx.push_back(i0); idx.push_back(i2); idx.push_back(i1);
                idx.push_back(i1); idx.push_back(i2); idx.push_back(i3);
            }
        }
        m_waterIndexCount = (uint32_t)idx.size();
        if (!createDeviceLocalBuffer(verts.data(), verts.size() * sizeof(glm::vec2),
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, m_waterVbo, m_waterVboAlloc)) {
            logError("[rhi] water vbo create failed"); return false;
        }
        if (!createDeviceLocalBuffer(idx.data(), idx.size() * sizeof(uint32_t),
                                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT, m_waterIbo, m_waterIboAlloc)) {
            logError("[rhi] water ibo create failed"); return false;
        }

        // --- Scene-depth sampler (LINEAR, clamp): samples the depth buffer as data
        // for the depth-based water color. ---
        VkSamplerCreateInfo dsci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        dsci.magFilter = VK_FILTER_LINEAR; dsci.minFilter = VK_FILTER_LINEAR;
        dsci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        dsci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        dsci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        dsci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(m_dev.device, &dsci, nullptr, &m_waterDepthSampler) != VK_SUCCESS) {
            logError("[rhi] water depth sampler failed"); return false;
        }

        // --- Set-0 layout: WaterUBO (b0, VS+FS) + scene-depth sampler (b1, FS). ---
        VkDescriptorSetLayoutBinding b[2]{};
        b[0].binding = 0; b[0].descriptorCount = 1;
        b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        b[1].binding = 1; b[1].descriptorCount = 1;
        b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        slci.bindingCount = 2; slci.pBindings = b;
        if (vkCreateDescriptorSetLayout(m_dev.device, &slci, nullptr, &m_waterSetLayout) != VK_SUCCESS) {
            logError("[rhi] water set layout failed"); return false;
        }

        // --- Descriptor pool: UBO + sampler per frame-in-flight. ---
        VkDescriptorPoolSize ps[2]{
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         kFramesInFlight },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight } };
        VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pci.maxSets = kFramesInFlight; pci.poolSizeCount = 2; pci.pPoolSizes = ps;
        if (vkCreateDescriptorPool(m_dev.device, &pci, nullptr, &m_waterPool) != VK_SUCCESS) {
            logError("[rhi] water desc pool failed"); return false;
        }

        // --- Per-frame UBO + descriptor set (depth binding written later). ---
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = sizeof(WaterUBO);
            bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info{};
            if (vmaCreateBuffer(m_alloc, &bci, &aci, &m_waterUboBuf[i], &m_waterUboAlloc[i], &info) != VK_SUCCESS) {
                logError("[rhi] water UBO create failed"); return false;
            }
            m_waterUboMapped[i] = info.pMappedData;

            VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dsai.descriptorPool = m_waterPool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &m_waterSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &dsai, &m_waterSet[i]) != VK_SUCCESS) {
                logError("[rhi] water set alloc failed"); return false;
            }
            VkDescriptorBufferInfo dbi{ m_waterUboBuf[i], 0, sizeof(WaterUBO) };
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet = m_waterSet[i]; w.dstBinding = 0; w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w.pBufferInfo = &dbi;
            vkUpdateDescriptorSets(m_dev.device, 1, &w, 0, nullptr);
        }
        writeWaterDescriptors();   // wire the scene-depth binding (depth view)

        // --- Pipeline: vec2 grid vertex; depth-test LESS_OR_EQUAL, no write. ---
        VkShaderModule vs = loadShaderModule("shaders\\water.vert.spv");
        VkShaderModule fs = loadShaderModule("shaders\\water.frag.spv");
        if (!vs || !fs) return false;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vs; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

        VkVertexInputBindingDescription vib{ 0, sizeof(glm::vec2), VK_VERTEX_INPUT_RATE_VERTEX };
        VkVertexInputAttributeDescription via{ 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 };
        VkPipelineVertexInputStateCreateInfo vin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &vib;
        vin.vertexAttributeDescriptionCount = 1; vin.pVertexAttributeDescriptions = &via;

        VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = 1; vp.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; // sea seen from both sides
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // Test against the opaque depth (so terrain in front of the water occludes
        // it) but DON'T write — the depth buffer is also sampled for the depth color
        // and post passes expect it unchanged. LESS_OR_EQUAL is robust at the seam.
        VkPipelineDepthStencilStateCreateInfo dss{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        dss.depthTestEnable = VK_TRUE; dss.depthWriteEnable = VK_FALSE;
        dss.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

        VkPipelineColorBlendAttachmentState cba{};
        cba.blendEnable = VK_FALSE; // opaque water (depth-color carries the look)
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 1; cb.pAttachments = &cba;

        VkDynamicState dyn[2]{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;

        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount = 1; plci.pSetLayouts = &m_waterSetLayout;
        if (vkCreatePipelineLayout(m_dev.device, &plci, nullptr, &m_waterLayout) != VK_SUCCESS) {
            logError("[rhi] water pipeline layout failed"); return false;
        }

        const VkFormat hdrFmt = kHdrFormat;
        VkPipelineRenderingCreateInfo prci{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        prci.colorAttachmentCount = 1; prci.pColorAttachmentFormats = &hdrFmt;
        prci.depthAttachmentFormat = m_depthFormat;

        VkGraphicsPipelineCreateInfo gpci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gpci.pNext = &prci;
        gpci.stageCount = 2; gpci.pStages = stages;
        gpci.pVertexInputState = &vin; gpci.pInputAssemblyState = &ia;
        gpci.pViewportState = &vp; gpci.pRasterizationState = &rs;
        gpci.pMultisampleState = &ms; gpci.pDepthStencilState = &dss;
        gpci.pColorBlendState = &cb; gpci.pDynamicState = &ds; gpci.layout = m_waterLayout;
        VkResult pr = vkCreateGraphicsPipelines(m_dev.device, VK_NULL_HANDLE, 1, &gpci, nullptr, &m_waterPipeline);

        vkDestroyShaderModule(m_dev.device, vs, nullptr);
        vkDestroyShaderModule(m_dev.device, fs, nullptr);
        if (pr != VK_SUCCESS) { logError("[rhi] water pipeline create failed"); return false; }

        logInfo("[rhi] water pipeline ready (Gerstner grid, sky-reflection + depth-refraction + sun glint)");
        return true;
    }

    // (Re)write the scene-depth binding of each frame's water set. Called at init
    // + on resize (the depth image view changes). The depth is sampled in the
    // DEPTH_READ_ONLY layout — the SAME layout it is bound as a read-only depth
    // attachment in the water pass, so simultaneous test + sample is valid.
    void writeWaterDescriptors() {
        if (!m_depthView) return;
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (!m_waterSet[i]) continue;
            VkDescriptorImageInfo di{ m_waterDepthSampler, m_depthView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet = m_waterSet[i]; w.dstBinding = 1; w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &di;
            vkUpdateDescriptorSets(m_dev.device, 1, &w, 0, nullptr);
        }
    }

    void destroyWater() {
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (m_waterUboBuf[i]) { vmaDestroyBuffer(m_alloc, m_waterUboBuf[i], m_waterUboAlloc[i]);
                                    m_waterUboBuf[i] = VK_NULL_HANDLE; m_waterUboAlloc[i] = nullptr; m_waterUboMapped[i] = nullptr; }
        }
        if (m_waterVbo) { vmaDestroyBuffer(m_alloc, m_waterVbo, m_waterVboAlloc); m_waterVbo = VK_NULL_HANDLE; m_waterVboAlloc = nullptr; }
        if (m_waterIbo) { vmaDestroyBuffer(m_alloc, m_waterIbo, m_waterIboAlloc); m_waterIbo = VK_NULL_HANDLE; m_waterIboAlloc = nullptr; }
        if (m_waterPipeline)  { vkDestroyPipeline(m_dev.device, m_waterPipeline, nullptr); m_waterPipeline = VK_NULL_HANDLE; }
        if (m_waterLayout)    { vkDestroyPipelineLayout(m_dev.device, m_waterLayout, nullptr); m_waterLayout = VK_NULL_HANDLE; }
        if (m_waterPool)      { vkDestroyDescriptorPool(m_dev.device, m_waterPool, nullptr); m_waterPool = VK_NULL_HANDLE; }
        if (m_waterSetLayout) { vkDestroyDescriptorSetLayout(m_dev.device, m_waterSetLayout, nullptr); m_waterSetLayout = VK_NULL_HANDLE; }
        if (m_waterDepthSampler) { vkDestroySampler(m_dev.device, m_waterDepthSampler, nullptr); m_waterDepthSampler = VK_NULL_HANDLE; }
    }

    // Record the water surface into the (already-open) water pass: bind the water
    // pipeline + this frame's set (UBO + scene depth) and draw the grid mesh. The
    // VS displaces the grid with Gerstner waves; the FS does sky-reflection +
    // depth-refraction + sun glint into the linear HDR target.
    void recordWaterPassBody(VkCommandBuffer cmd) {
        if (!m_waterPipeline || !m_waterVbo || m_waterIndexCount == 0) return;
        postViewport(cmd, m_extent);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_waterPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_waterLayout,
                                0, 1, &m_waterSet[m_frameIdx], 0, nullptr);
        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &m_waterVbo, &off);
        vkCmdBindIndexBuffer(cmd, m_waterIbo, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, m_waterIndexCount, 1, 0, 0, 0);
    }

    // ---- Particles + impact decals (combat juice) --------------------------
    // Build: the shared unit quad (4 corners, triangle strip), the per-frame
    // instance rings (additive/alpha particles + decals) + UBOs + descriptor sets,
    // and the three graphics pipelines (additive particles, alpha particles, alpha
    // decals). All depth-TEST LESS_OR_EQUAL, depth-write OFF (the billboards/decals
    // read the opaque depth the main pass produced and never overwrite it). The
    // particle FS samples the scene depth for the soft-particle fade (set0,b1).
    bool createParticles() {
        // Reserve the CPU staging vectors ONCE so per-frame submitParticles/Decals
        // appends never reallocate (the bounded "no per-frame heap alloc" promise).
        m_partAdd.reserve(kMaxParticles);
        m_partAlpha.reserve(kMaxParticles);
        m_decals.reserve(kMaxDecals);

        // --- Shared unit quad: 4 corners in [-0.5,0.5], drawn as a triangle strip. ---
        const glm::vec2 quad[4] = {
            { -0.5f, -0.5f }, {  0.5f, -0.5f }, { -0.5f,  0.5f }, {  0.5f,  0.5f } };
        if (!createDeviceLocalBuffer(quad, sizeof(quad),
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, m_partQuadVbo, m_partQuadAlloc)) {
            logError("[rhi] particle quad vbo create failed"); return false;
        }

        // --- Scene-depth sampler (LINEAR clamp) for the soft-particle fade. ---
        VkSamplerCreateInfo dsci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        dsci.magFilter = VK_FILTER_LINEAR; dsci.minFilter = VK_FILTER_LINEAR;
        dsci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        dsci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        dsci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        dsci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(m_dev.device, &dsci, nullptr, &m_partDepthSampler) != VK_SUCCESS) {
            logError("[rhi] particle depth sampler failed"); return false;
        }

        // --- Set-0 layouts. Particle: UBO(b0,VS+FS) + scene-depth(b1,FS). Decal: UBO(b0). ---
        {
            VkDescriptorSetLayoutBinding b[2]{};
            b[0].binding = 0; b[0].descriptorCount = 1;
            b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            b[1].binding = 1; b[1].descriptorCount = 1;
            b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            slci.bindingCount = 2; slci.pBindings = b;
            if (vkCreateDescriptorSetLayout(m_dev.device, &slci, nullptr, &m_partSetLayout) != VK_SUCCESS) {
                logError("[rhi] particle set layout failed"); return false;
            }
            VkDescriptorSetLayoutBinding db{};
            db.binding = 0; db.descriptorCount = 1;
            db.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            db.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            VkDescriptorSetLayoutCreateInfo dslci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            dslci.bindingCount = 1; dslci.pBindings = &db;
            if (vkCreateDescriptorSetLayout(m_dev.device, &dslci, nullptr, &m_decalSetLayout) != VK_SUCCESS) {
                logError("[rhi] decal set layout failed"); return false;
            }
        }

        // --- Descriptor pool: per frame-in-flight, 2 UBO sets + 1 sampler. ---
        VkDescriptorPoolSize ps[2]{
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         kFramesInFlight * 2 },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight } };
        VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pci.maxSets = kFramesInFlight * 2; pci.poolSizeCount = 2; pci.pPoolSizes = ps;
        if (vkCreateDescriptorPool(m_dev.device, &pci, nullptr, &m_partPool) != VK_SUCCESS) {
            logError("[rhi] particle desc pool failed"); return false;
        }

        // --- Per-frame instance rings + UBOs + descriptor sets. ---
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            auto makeMapped = [&](VkDeviceSize bytes, VkBufferUsageFlags usage,
                                  VkBuffer& buf, VmaAllocation& alloc, void*& mapped) -> bool {
                VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                bci.size = bytes; bci.usage = usage;
                VmaAllocationCreateInfo aci{};
                aci.usage = VMA_MEMORY_USAGE_AUTO;
                aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
                VmaAllocationInfo info{};
                if (vmaCreateBuffer(m_alloc, &bci, &aci, &buf, &alloc, &info) != VK_SUCCESS) return false;
                mapped = info.pMappedData;
                return true;
            };
            if (!makeMapped(sizeof(ParticleGpu) * kMaxParticles, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                            m_partInstAddBuf[i], m_partInstAddAlloc[i], m_partInstAddMapped[i])) {
                logError("[rhi] particle add ring create failed"); return false; }
            if (!makeMapped(sizeof(ParticleGpu) * kMaxParticles, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                            m_partInstAlphaBuf[i], m_partInstAlphaAlloc[i], m_partInstAlphaMapped[i])) {
                logError("[rhi] particle alpha ring create failed"); return false; }
            if (!makeMapped(sizeof(DecalGpu) * kMaxDecals, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                            m_decalInstBuf[i], m_decalInstAlloc[i], m_decalInstMapped[i])) {
                logError("[rhi] decal ring create failed"); return false; }
            if (!makeMapped(sizeof(ParticleUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                            m_partUboBuf[i], m_partUboAlloc[i], m_partUboMapped[i])) {
                logError("[rhi] particle UBO create failed"); return false; }
            if (!makeMapped(sizeof(DecalUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                            m_decalUboBuf[i], m_decalUboAlloc[i], m_decalUboMapped[i])) {
                logError("[rhi] decal UBO create failed"); return false; }

            VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dsai.descriptorPool = m_partPool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &m_partSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &dsai, &m_partSet[i]) != VK_SUCCESS) {
                logError("[rhi] particle set alloc failed"); return false; }
            VkDescriptorSetAllocateInfo dsai2{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dsai2.descriptorPool = m_partPool; dsai2.descriptorSetCount = 1; dsai2.pSetLayouts = &m_decalSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &dsai2, &m_decalSet[i]) != VK_SUCCESS) {
                logError("[rhi] decal set alloc failed"); return false; }

            // Bind the UBO (b0) of each set now; the depth (b1) is wired below/on resize.
            VkDescriptorBufferInfo pbi{ m_partUboBuf[i], 0, sizeof(ParticleUBO) };
            VkDescriptorBufferInfo dbi{ m_decalUboBuf[i], 0, sizeof(DecalUBO) };
            VkWriteDescriptorSet w[2]{};
            w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[0].dstSet = m_partSet[i]; w[0].dstBinding = 0; w[0].descriptorCount = 1;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &pbi;
            w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[1].dstSet = m_decalSet[i]; w[1].dstBinding = 0; w[1].descriptorCount = 1;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[1].pBufferInfo = &dbi;
            vkUpdateDescriptorSets(m_dev.device, 2, w, 0, nullptr);
        }
        writeParticleDescriptors();   // wire the scene-depth binding (depth view)

        // --- Pipelines. Shared: triangle strip, depth-test LESS_OR_EQUAL no write,
        // vertex input = quad corner (binding 0, per-vertex) + instance (binding 1). ---
        const VkFormat hdrFmt = kHdrFormat;
        VkPipelineRenderingCreateInfo prci{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        prci.colorAttachmentCount = 1; prci.pColorAttachmentFormats = &hdrFmt;
        prci.depthAttachmentFormat = m_depthFormat;

        VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = 1; vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo dss{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        dss.depthTestEnable = VK_TRUE; dss.depthWriteEnable = VK_FALSE;
        dss.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkDynamicState dyn[2]{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;

        // Pipeline layouts.
        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount = 1; plci.pSetLayouts = &m_partSetLayout;
        if (vkCreatePipelineLayout(m_dev.device, &plci, nullptr, &m_partLayout) != VK_SUCCESS) {
            logError("[rhi] particle pipeline layout failed"); return false; }
        VkPipelineLayoutCreateInfo dplci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        dplci.setLayoutCount = 1; dplci.pSetLayouts = &m_decalSetLayout;
        if (vkCreatePipelineLayout(m_dev.device, &dplci, nullptr, &m_decalLayout) != VK_SUCCESS) {
            logError("[rhi] decal pipeline layout failed"); return false; }

        // ---- Particle pipelines (additive + alpha): quad corner + ParticleGpu. ----
        {
            VkShaderModule vs = loadShaderModule("shaders\\particle.vert.spv");
            VkShaderModule fs = loadShaderModule("shaders\\particle.frag.spv");
            if (!vs || !fs) return false;
            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vs; stages[0].pName = "main";
            stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

            VkVertexInputBindingDescription vibs[2]{
                { 0, sizeof(glm::vec2),   VK_VERTEX_INPUT_RATE_VERTEX   },   // quad corner
                { 1, sizeof(ParticleGpu), VK_VERTEX_INPUT_RATE_INSTANCE } }; // instance
            VkVertexInputAttributeDescription vias[3]{
                { 0, 0, VK_FORMAT_R32G32_SFLOAT,       0 },                              // inCorner
                { 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ParticleGpu, posSize) },// inPosSize
                { 2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ParticleGpu, color)   }};// inColor
            VkPipelineVertexInputStateCreateInfo vin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            vin.vertexBindingDescriptionCount = 2; vin.pVertexBindingDescriptions = vibs;
            vin.vertexAttributeDescriptionCount = 3; vin.pVertexAttributeDescriptions = vias;

            // Additive blend (sparks/fire/muzzle): src*srcA + dst (glow, feeds bloom).
            VkPipelineColorBlendAttachmentState addBlend{};
            addBlend.blendEnable = VK_TRUE;
            addBlend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            addBlend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            addBlend.colorBlendOp = VK_BLEND_OP_ADD;
            addBlend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            addBlend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            addBlend.alphaBlendOp = VK_BLEND_OP_ADD;
            addBlend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
            // Alpha blend (smoke/dust/blood): standard over.
            VkPipelineColorBlendAttachmentState aBlend = addBlend;
            aBlend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;

            auto buildPart = [&](const VkPipelineColorBlendAttachmentState& cba, VkPipeline& out) -> bool {
                VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
                cb.attachmentCount = 1; cb.pAttachments = &cba;
                VkGraphicsPipelineCreateInfo gpci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
                gpci.pNext = &prci; gpci.stageCount = 2; gpci.pStages = stages;
                gpci.pVertexInputState = &vin; gpci.pInputAssemblyState = &ia;
                gpci.pViewportState = &vp; gpci.pRasterizationState = &rs;
                gpci.pMultisampleState = &ms; gpci.pDepthStencilState = &dss;
                gpci.pColorBlendState = &cb; gpci.pDynamicState = &ds; gpci.layout = m_partLayout;
                return vkCreateGraphicsPipelines(m_dev.device, VK_NULL_HANDLE, 1, &gpci, nullptr, &out) == VK_SUCCESS;
            };
            bool ok = buildPart(addBlend, m_partAddPipeline) && buildPart(aBlend, m_partAlphaPipeline);
            vkDestroyShaderModule(m_dev.device, vs, nullptr);
            vkDestroyShaderModule(m_dev.device, fs, nullptr);
            if (!ok) { logError("[rhi] particle pipeline create failed"); return false; }
        }

        // ---- Decal pipeline (alpha): quad corner + DecalGpu. ----
        {
            VkShaderModule vs = loadShaderModule("shaders\\decal.vert.spv");
            VkShaderModule fs = loadShaderModule("shaders\\decal.frag.spv");
            if (!vs || !fs) return false;
            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vs; stages[0].pName = "main";
            stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

            VkVertexInputBindingDescription vibs[2]{
                { 0, sizeof(glm::vec2), VK_VERTEX_INPUT_RATE_VERTEX   },
                { 1, sizeof(DecalGpu),  VK_VERTEX_INPUT_RATE_INSTANCE } };
            VkVertexInputAttributeDescription vias[4]{
                { 0, 0, VK_FORMAT_R32G32_SFLOAT,       0 },
                { 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(DecalGpu, centerSize)  },
                { 2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(DecalGpu, normalAngle) },
                { 3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(DecalGpu, color)       }};
            VkPipelineVertexInputStateCreateInfo vin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            vin.vertexBindingDescriptionCount = 2; vin.pVertexBindingDescriptions = vibs;
            vin.vertexAttributeDescriptionCount = 4; vin.pVertexAttributeDescriptions = vias;

            VkPipelineColorBlendAttachmentState cba{};
            cba.blendEnable = VK_TRUE;
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cba.colorBlendOp = VK_BLEND_OP_ADD;
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.alphaBlendOp = VK_BLEND_OP_ADD;
            cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
            cb.attachmentCount = 1; cb.pAttachments = &cba;

            VkGraphicsPipelineCreateInfo gpci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
            gpci.pNext = &prci; gpci.stageCount = 2; gpci.pStages = stages;
            gpci.pVertexInputState = &vin; gpci.pInputAssemblyState = &ia;
            gpci.pViewportState = &vp; gpci.pRasterizationState = &rs;
            gpci.pMultisampleState = &ms; gpci.pDepthStencilState = &dss;
            gpci.pColorBlendState = &cb; gpci.pDynamicState = &ds; gpci.layout = m_decalLayout;
            VkResult pr = vkCreateGraphicsPipelines(m_dev.device, VK_NULL_HANDLE, 1, &gpci, nullptr, &m_decalPipeline);
            vkDestroyShaderModule(m_dev.device, vs, nullptr);
            vkDestroyShaderModule(m_dev.device, fs, nullptr);
            if (pr != VK_SUCCESS) { logError("[rhi] decal pipeline create failed"); return false; }
        }

        logInfo("[rhi] particle + decal pipelines ready (instanced billboards, additive/alpha, soft vs scene depth)");
        return true;
    }

    // (Re)write the scene-depth binding (b1) of each frame's particle set. Called at
    // init + on resize (the depth image view changes). Sampled in DEPTH_READ_ONLY —
    // the SAME layout it is bound as a read-only depth attachment in the pass.
    void writeParticleDescriptors() {
        if (!m_depthView) return;
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (!m_partSet[i]) continue;
            VkDescriptorImageInfo di{ m_partDepthSampler, m_depthView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet = m_partSet[i]; w.dstBinding = 1; w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &di;
            vkUpdateDescriptorSets(m_dev.device, 1, &w, 0, nullptr);
        }
    }

    void destroyParticles() {
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            auto kill = [&](VkBuffer& b, VmaAllocation& a, void*& m) {
                if (b) { vmaDestroyBuffer(m_alloc, b, a); b = VK_NULL_HANDLE; a = nullptr; m = nullptr; } };
            kill(m_partInstAddBuf[i],   m_partInstAddAlloc[i],   m_partInstAddMapped[i]);
            kill(m_partInstAlphaBuf[i], m_partInstAlphaAlloc[i], m_partInstAlphaMapped[i]);
            kill(m_decalInstBuf[i],     m_decalInstAlloc[i],     m_decalInstMapped[i]);
            kill(m_partUboBuf[i],       m_partUboAlloc[i],       m_partUboMapped[i]);
            kill(m_decalUboBuf[i],      m_decalUboAlloc[i],      m_decalUboMapped[i]);
        }
        if (m_partQuadVbo) { vmaDestroyBuffer(m_alloc, m_partQuadVbo, m_partQuadAlloc); m_partQuadVbo = VK_NULL_HANDLE; m_partQuadAlloc = nullptr; }
        if (m_partAddPipeline)   { vkDestroyPipeline(m_dev.device, m_partAddPipeline, nullptr);   m_partAddPipeline = VK_NULL_HANDLE; }
        if (m_partAlphaPipeline) { vkDestroyPipeline(m_dev.device, m_partAlphaPipeline, nullptr); m_partAlphaPipeline = VK_NULL_HANDLE; }
        if (m_decalPipeline)     { vkDestroyPipeline(m_dev.device, m_decalPipeline, nullptr);     m_decalPipeline = VK_NULL_HANDLE; }
        if (m_partLayout)    { vkDestroyPipelineLayout(m_dev.device, m_partLayout, nullptr);  m_partLayout = VK_NULL_HANDLE; }
        if (m_decalLayout)   { vkDestroyPipelineLayout(m_dev.device, m_decalLayout, nullptr); m_decalLayout = VK_NULL_HANDLE; }
        if (m_partPool)      { vkDestroyDescriptorPool(m_dev.device, m_partPool, nullptr); m_partPool = VK_NULL_HANDLE; }
        if (m_partSetLayout) { vkDestroyDescriptorSetLayout(m_dev.device, m_partSetLayout, nullptr);  m_partSetLayout = VK_NULL_HANDLE; }
        if (m_decalSetLayout){ vkDestroyDescriptorSetLayout(m_dev.device, m_decalSetLayout, nullptr); m_decalSetLayout = VK_NULL_HANDLE; }
        if (m_partDepthSampler) { vkDestroySampler(m_dev.device, m_partDepthSampler, nullptr); m_partDepthSampler = VK_NULL_HANDLE; }
    }

    // =======================================================================
    // GPU-compute persistent debris world (Subsystem K, tier T2).
    // -----------------------------------------------------------------------
    // CLEAN-ROOM, original work. Built ONLY from the K spec (§5/§7), the engine's
    // own renderer patterns (the SSBO + persistent-mapped buffer + instanced-draw
    // path used by particles/multidraw above), and the Vulkan 1.3 spec (compute
    // pipelines, SSBOs, vkCmdDispatch). NO id Tech / RBDOOM source consulted.
    //
    // A fixed-size pool of debris fragments lives in ONE host-visible DEVICE_LOCAL
    // SSBO (VMA HOST_ACCESS_RANDOM -> BAR/host-visible device memory; reads + writes
    // both work, no staging needed for spawn/readback). A COMPUTE shader integrates
    // the whole pool each step (gravity, ground + AABB collision, damping, sleep,
    // lifetime free). The pool draws via ONE instanced unit-cube draw (instanceCount
    // == capacity); each instance reads its row from the same SSBO and dead slots
    // collapse to nothing in the vertex shader (no per-frame compaction).
    //
    // This is the SYNCHRONOUS path (graphics queue): correct on Pascal (1080 Ti)
    // where async-compute overlap is weak. The compute dispatch and the draw run in
    // the same frame's command buffer, ordered by an SSBO memory barrier.

    // Mirror of shaders/debris.comp's Fragment (std430; four 16-byte vec4 rows).
    struct GpuDebrisFragment {
        glm::vec4 posLife;    // xyz position, w remaining lifetime (s)
        glm::vec4 velScale;   // xyz velocity, w half-extent
        glm::vec4 spinState;  // xyz angular velocity, w packed state+sleepCtr
        glm::vec4 rot;        // orientation quaternion (x,y,z,w)
    };
    static_assert(sizeof(GpuDebrisFragment) == 64, "GpuDebrisFragment must be 4x vec4");

    // Mirror of shaders/debris.comp's Params UBO (std140).
    struct GpuDebrisParamsUBO {
        glm::vec4 gravityDt;     // xyz gravity, w dt
        glm::vec4 groundDamp;    // x groundY, y restitution, z friction, w linDamp
        glm::vec4 sleepCap;      // x sleepLin, y sleepAng, z sleepFrames, w capacity
        glm::vec4 aabbCount;     // x aabb count
        glm::vec4 aabbMin[4];
        glm::vec4 aabbMax[4];
    };

    // Mirror of shaders/debris.{vert} DebrisDrawUBO (std140).
    struct GpuDebrisDrawUBO { glm::mat4 viewProj; glm::vec4 color; };

    static constexpr uint32_t kDebrisCapacity = 65536;  // pool size (spec §11 target: 50k+)
    static constexpr float    kDebrisDeadState = 0.0f;
    static constexpr float    kDebrisActiveState = 1.0f;

    bool createDebris() {
        m_debrisParams = IRenderDevice::GpuDebrisParams{};   // device defaults
        m_debrisAlive = 0;
        m_debrisSpawnCursor = 0;

        // --- Pool SSBO (host-visible, mapped; storage + the compute reads/writes it). ---
        {
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = (VkDeviceSize)sizeof(GpuDebrisFragment) * kDebrisCapacity;
            bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info{};
            if (vmaCreateBuffer(m_alloc, &bci, &aci, &m_debrisPoolBuf, &m_debrisPoolAlloc, &info) != VK_SUCCESS) {
                logError("[rhi] debris pool SSBO create failed"); return false; }
            m_debrisPoolMapped = info.pMappedData;
            // Zero the pool -> every slot DEAD (spinState.w == 0).
            std::memset(m_debrisPoolMapped, 0, (size_t)bci.size);
        }
        // --- Counters SSBO (host-visible; counters[0] = alive count). ---
        {
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = sizeof(uint32_t) * 4;
            bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info{};
            if (vmaCreateBuffer(m_alloc, &bci, &aci, &m_debrisCountBuf, &m_debrisCountAlloc, &info) != VK_SUCCESS) {
                logError("[rhi] debris counters SSBO create failed"); return false; }
            m_debrisCountMapped = info.pMappedData;
            std::memset(m_debrisCountMapped, 0, (size_t)bci.size);
        }
        // --- Per-frame params UBO (compute) + draw UBO (graphics), host-visible. ---
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            auto makeMapped = [&](VkDeviceSize bytes, VkBufferUsageFlags usage,
                                  VkBuffer& buf, VmaAllocation& alloc, void*& mapped) -> bool {
                VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                bci.size = bytes; bci.usage = usage;
                VmaAllocationCreateInfo aci{};
                aci.usage = VMA_MEMORY_USAGE_AUTO;
                aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
                VmaAllocationInfo info{};
                if (vmaCreateBuffer(m_alloc, &bci, &aci, &buf, &alloc, &info) != VK_SUCCESS) return false;
                mapped = info.pMappedData; return true;
            };
            if (!makeMapped(sizeof(GpuDebrisParamsUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                            m_debrisParamsBuf[i], m_debrisParamsAlloc[i], m_debrisParamsMapped[i])) {
                logError("[rhi] debris params UBO create failed"); return false; }
            if (!makeMapped(sizeof(GpuDebrisDrawUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                            m_debrisDrawUboBuf[i], m_debrisDrawUboAlloc[i], m_debrisDrawUboMapped[i])) {
                logError("[rhi] debris draw UBO create failed"); return false; }
        }

        // --- Shared unit cube (24 verts, per-face normals) for the instanced draw. ---
        {
            struct DV { glm::vec3 pos; glm::vec3 nrm; };
            std::vector<DV> verts; std::vector<uint32_t> idx;
            auto face = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, glm::vec3 n) {
                uint32_t base = (uint32_t)verts.size();
                verts.push_back({a,n}); verts.push_back({b,n}); verts.push_back({c,n}); verts.push_back({d,n});
                idx.insert(idx.end(), { base, base+1, base+2, base, base+2, base+3 });
            };
            const float h = 0.5f;
            face({-h,-h, h},{ h,-h, h},{ h, h, h},{-h, h, h},{ 0, 0, 1});
            face({ h,-h,-h},{-h,-h,-h},{-h, h,-h},{ h, h,-h},{ 0, 0,-1});
            face({ h,-h, h},{ h,-h,-h},{ h, h,-h},{ h, h, h},{ 1, 0, 0});
            face({-h,-h,-h},{-h,-h, h},{-h, h, h},{-h, h,-h},{-1, 0, 0});
            face({-h, h, h},{ h, h, h},{ h, h,-h},{-h, h,-h},{ 0, 1, 0});
            face({-h,-h,-h},{ h,-h,-h},{ h,-h, h},{-h,-h, h},{ 0,-1, 0});
            m_debrisCubeIndexCount = (uint32_t)idx.size();
            if (!createDeviceLocalBuffer(verts.data(), verts.size() * sizeof(DV),
                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, m_debrisCubeVbo, m_debrisCubeAlloc)) {
                logError("[rhi] debris cube vbo create failed"); return false; }
            if (!createDeviceLocalBuffer(idx.data(), idx.size() * sizeof(uint32_t),
                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT, m_debrisCubeIbo, m_debrisCubeIboAlloc)) {
                logError("[rhi] debris cube ibo create failed"); return false; }
        }

        // --- Descriptor pool: per-frame compute sets (2 SSBO + 1 UBO each) + per-frame
        //     draw sets (1 SSBO + 1 UBO each). Per-frame so a set updated this frame is
        //     never one a still-pending command buffer references (avoids VUID 03047).
        VkDescriptorPoolSize ps[2]{
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFramesInFlight * 3 },   // compute: pool+counters; draw: pool
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight * 2 } }; // compute params + draw UBO
        VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pci.maxSets = kFramesInFlight * 2; pci.poolSizeCount = 2; pci.pPoolSizes = ps;
        if (vkCreateDescriptorPool(m_dev.device, &pci, nullptr, &m_debrisPool) != VK_SUCCESS) {
            logError("[rhi] debris desc pool failed"); return false; }

        // --- Compute set layout: b0 pool SSBO, b1 counters SSBO, b2 params UBO. ---
        {
            VkDescriptorSetLayoutBinding b[3]{};
            b[0].binding = 0; b[0].descriptorCount = 1;
            b[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[1].binding = 1; b[1].descriptorCount = 1;
            b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[2].binding = 2; b[2].descriptorCount = 1;
            b[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; b[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            slci.bindingCount = 3; slci.pBindings = b;
            if (vkCreateDescriptorSetLayout(m_dev.device, &slci, nullptr, &m_debrisComputeSetLayout) != VK_SUCCESS) {
                logError("[rhi] debris compute set layout failed"); return false; }
        }
        // --- Draw set layout: b0 draw UBO (VS), b1 pool SSBO (VS readonly). ---
        {
            VkDescriptorSetLayoutBinding b[2]{};
            b[0].binding = 0; b[0].descriptorCount = 1;
            b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            b[1].binding = 1; b[1].descriptorCount = 1;
            b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            slci.bindingCount = 2; slci.pBindings = b;
            if (vkCreateDescriptorSetLayout(m_dev.device, &slci, nullptr, &m_debrisDrawSetLayout) != VK_SUCCESS) {
                logError("[rhi] debris draw set layout failed"); return false; }
        }

        // --- Compute pipeline. ---
        {
            VkShaderModule cs = loadShaderModule("shaders\\debris.comp.spv");
            if (!cs) return false;
            VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            plci.setLayoutCount = 1; plci.pSetLayouts = &m_debrisComputeSetLayout;
            if (vkCreatePipelineLayout(m_dev.device, &plci, nullptr, &m_debrisComputeLayout) != VK_SUCCESS) {
                vkDestroyShaderModule(m_dev.device, cs, nullptr);
                logError("[rhi] debris compute pipeline layout failed"); return false; }
            VkComputePipelineCreateInfo cpci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpci.stage.module = cs; cpci.stage.pName = "main";
            cpci.layout = m_debrisComputeLayout;
            VkResult cr = vkCreateComputePipelines(m_dev.device, VK_NULL_HANDLE, 1, &cpci, nullptr, &m_debrisComputePipeline);
            vkDestroyShaderModule(m_dev.device, cs, nullptr);
            if (cr != VK_SUCCESS) { logError("[rhi] debris compute pipeline create failed"); return false; }
        }

        // --- Per-frame compute descriptor sets. The pool/counters SSBOs are shared;
        //     each set binds ITS OWN frame's params UBO at creation, so no per-step
        //     vkUpdateDescriptorSets is needed (a pending set is never re-written). ---
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dsai.descriptorPool = m_debrisPool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &m_debrisComputeSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &dsai, &m_debrisComputeSet[i]) != VK_SUCCESS) {
                logError("[rhi] debris compute set alloc failed"); return false; }
            VkDescriptorBufferInfo pool{ m_debrisPoolBuf, 0, VK_WHOLE_SIZE };
            VkDescriptorBufferInfo cnt { m_debrisCountBuf, 0, VK_WHOLE_SIZE };
            VkDescriptorBufferInfo prm { m_debrisParamsBuf[i], 0, sizeof(GpuDebrisParamsUBO) };
            VkWriteDescriptorSet w[3]{};
            w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = m_debrisComputeSet[i];
            w[0].dstBinding = 0; w[0].descriptorCount = 1; w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[0].pBufferInfo = &pool;
            w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = m_debrisComputeSet[i];
            w[1].dstBinding = 1; w[1].descriptorCount = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &cnt;
            w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[2].dstSet = m_debrisComputeSet[i];
            w[2].dstBinding = 2; w[2].descriptorCount = 1; w[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[2].pBufferInfo = &prm;
            vkUpdateDescriptorSets(m_dev.device, 3, w, 0, nullptr);
        }

        // --- Draw pipeline (instanced cube, into the HDR scene target). ---
        {
            VkShaderModule vs = loadShaderModule("shaders\\debris.vert.spv");
            VkShaderModule fs = loadShaderModule("shaders\\debris.frag.spv");
            if (!vs || !fs) { if(vs) vkDestroyShaderModule(m_dev.device,vs,nullptr); if(fs) vkDestroyShaderModule(m_dev.device,fs,nullptr); return false; }
            VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            plci.setLayoutCount = 1; plci.pSetLayouts = &m_debrisDrawSetLayout;
            if (vkCreatePipelineLayout(m_dev.device, &plci, nullptr, &m_debrisDrawLayout) != VK_SUCCESS) {
                vkDestroyShaderModule(m_dev.device, vs, nullptr); vkDestroyShaderModule(m_dev.device, fs, nullptr);
                logError("[rhi] debris draw pipeline layout failed"); return false; }

            // Per-frame draw sets (UBO is per-frame; the pool SSBO is shared).
            for (uint32_t i = 0; i < kFramesInFlight; ++i) {
                VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
                dsai.descriptorPool = m_debrisPool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &m_debrisDrawSetLayout;
                if (vkAllocateDescriptorSets(m_dev.device, &dsai, &m_debrisDrawSet[i]) != VK_SUCCESS) {
                    vkDestroyShaderModule(m_dev.device, vs, nullptr); vkDestroyShaderModule(m_dev.device, fs, nullptr);
                    logError("[rhi] debris draw set alloc failed"); return false; }
                VkDescriptorBufferInfo ubi{ m_debrisDrawUboBuf[i], 0, sizeof(GpuDebrisDrawUBO) };
                VkDescriptorBufferInfo pool{ m_debrisPoolBuf, 0, VK_WHOLE_SIZE };
                VkWriteDescriptorSet w[2]{};
                w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = m_debrisDrawSet[i];
                w[0].dstBinding = 0; w[0].descriptorCount = 1; w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &ubi;
                w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = m_debrisDrawSet[i];
                w[1].dstBinding = 1; w[1].descriptorCount = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &pool;
                vkUpdateDescriptorSets(m_dev.device, 2, w, 0, nullptr);
            }

            const VkFormat hdrFmt = kHdrFormat;
            VkPipelineRenderingCreateInfo prci{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
            prci.colorAttachmentCount = 1; prci.pColorAttachmentFormats = &hdrFmt;
            prci.depthAttachmentFormat = m_depthFormat;
            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs; stages[0].pName = "main";
            stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";
            VkVertexInputBindingDescription vib{ 0, sizeof(glm::vec3) * 2, VK_VERTEX_INPUT_RATE_VERTEX };
            VkVertexInputAttributeDescription via[2]{
                { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
                { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(glm::vec3) } };
            VkPipelineVertexInputStateCreateInfo vin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &vib;
            vin.vertexAttributeDescriptionCount = 2; vin.pVertexAttributeDescriptions = via;
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
            dss.depthTestEnable = VK_TRUE; dss.depthWriteEnable = VK_FALSE;  // read-only scene depth in the part pass
            dss.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
            VkPipelineColorBlendAttachmentState cba{};
            cba.blendEnable = VK_FALSE;
            cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
            cb.attachmentCount = 1; cb.pAttachments = &cba;
            VkDynamicState dyn[2]{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
            VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
            ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
            VkGraphicsPipelineCreateInfo gpci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
            gpci.pNext = &prci; gpci.stageCount = 2; gpci.pStages = stages;
            gpci.pVertexInputState = &vin; gpci.pInputAssemblyState = &ia;
            gpci.pViewportState = &vp; gpci.pRasterizationState = &rs;
            gpci.pMultisampleState = &ms; gpci.pDepthStencilState = &dss;
            gpci.pColorBlendState = &cb; gpci.pDynamicState = &ds; gpci.layout = m_debrisDrawLayout;
            VkResult pr = vkCreateGraphicsPipelines(m_dev.device, VK_NULL_HANDLE, 1, &gpci, nullptr, &m_debrisDrawPipeline);
            vkDestroyShaderModule(m_dev.device, vs, nullptr);
            vkDestroyShaderModule(m_dev.device, fs, nullptr);
            if (pr != VK_SUCCESS) { logError("[rhi] debris draw pipeline create failed"); return false; }
        }

        logInfo("[rhi] GPU-compute debris world ready (compute integrate + instanced cube draw, capacity "
                + std::to_string(kDebrisCapacity) + ")");
        return true;
    }

    void destroyDebris() {
        auto killBuf = [&](VkBuffer& b, VmaAllocation& a, void*& m) {
            if (b) { vmaDestroyBuffer(m_alloc, b, a); b = VK_NULL_HANDLE; a = nullptr; m = nullptr; } };
        auto killBuf2 = [&](VkBuffer& b, VmaAllocation& a) {
            if (b) { vmaDestroyBuffer(m_alloc, b, a); b = VK_NULL_HANDLE; a = nullptr; } };
        killBuf(m_debrisPoolBuf,  m_debrisPoolAlloc,  m_debrisPoolMapped);
        killBuf(m_debrisCountBuf, m_debrisCountAlloc, m_debrisCountMapped);
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            killBuf(m_debrisParamsBuf[i],  m_debrisParamsAlloc[i],  m_debrisParamsMapped[i]);
            killBuf(m_debrisDrawUboBuf[i], m_debrisDrawUboAlloc[i], m_debrisDrawUboMapped[i]);
        }
        killBuf2(m_debrisCubeVbo, m_debrisCubeAlloc);
        killBuf2(m_debrisCubeIbo, m_debrisCubeIboAlloc);
        if (m_debrisComputePipeline) { vkDestroyPipeline(m_dev.device, m_debrisComputePipeline, nullptr); m_debrisComputePipeline = VK_NULL_HANDLE; }
        if (m_debrisDrawPipeline)    { vkDestroyPipeline(m_dev.device, m_debrisDrawPipeline, nullptr);    m_debrisDrawPipeline = VK_NULL_HANDLE; }
        if (m_debrisComputeLayout)   { vkDestroyPipelineLayout(m_dev.device, m_debrisComputeLayout, nullptr); m_debrisComputeLayout = VK_NULL_HANDLE; }
        if (m_debrisDrawLayout)      { vkDestroyPipelineLayout(m_dev.device, m_debrisDrawLayout, nullptr);    m_debrisDrawLayout = VK_NULL_HANDLE; }
        if (m_debrisPool)            { vkDestroyDescriptorPool(m_dev.device, m_debrisPool, nullptr); m_debrisPool = VK_NULL_HANDLE; }
        if (m_debrisComputeSetLayout){ vkDestroyDescriptorSetLayout(m_dev.device, m_debrisComputeSetLayout, nullptr); m_debrisComputeSetLayout = VK_NULL_HANDLE; }
        if (m_debrisDrawSetLayout)   { vkDestroyDescriptorSetLayout(m_dev.device, m_debrisDrawSetLayout, nullptr);    m_debrisDrawSetLayout = VK_NULL_HANDLE; }
    }

    void gpuDebrisConfig(const IRenderDevice::GpuDebrisParams& p) override { m_debrisParams = p; }

    uint32_t gpuDebrisAliveCount() const override {
        if (m_debrisCountMapped) return ((const uint32_t*)m_debrisCountMapped)[0];
        return m_debrisAlive;
    }
    uint32_t gpuDebrisCapacity() const override { return kDebrisCapacity; }

    uint32_t gpuDebrisSpawnBurst(const float pos[3], uint32_t count, float speed,
                                 float lifetime, float halfExtent, uint32_t seed) override {
        if (!m_debrisPoolMapped || count == 0) return 0;
        count = std::min(count, kDebrisCapacity);
        // Deterministic PRNG (xorshift) seeded by `seed` so the same call reproduces.
        uint32_t rng = seed ? seed : 0x9E3779B9u;
        auto next = [&]() -> float {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            return (rng & 0xFFFFFFu) / (float)0xFFFFFF;   // [0,1)
        };
        GpuDebrisFragment* pool = (GpuDebrisFragment*)m_debrisPoolMapped;
        uint32_t* counters = (uint32_t*)m_debrisCountMapped;
        uint32_t spawned = 0;
        for (uint32_t k = 0; k < count; ++k) {
            // Write into free (DEAD) slots starting at the recycle cursor; if the pool
            // is full, overwrite the oldest (ring) — never blocks, never allocs.
            uint32_t slot = m_debrisSpawnCursor;
            m_debrisSpawnCursor = (m_debrisSpawnCursor + 1) % kDebrisCapacity;
            bool wasDead = (floorf(pool[slot].spinState.w + 0.5f) == kDebrisDeadState);
            // Outward direction on a hemisphere (upward bias) + speed jitter.
            float az = next() * 6.2831853f;
            float el = next() * 1.2566370f + 0.1f;          // ~6..78 deg above horizon
            float sp = speed * (0.5f + 0.5f * next());
            glm::vec3 dir(std::cos(az) * std::cos(el), std::sin(el), std::sin(az) * std::cos(el));
            glm::vec3 v = dir * sp;
            float he = halfExtent * (0.6f + 0.6f * next());
            // Random unit quaternion (uniform) for the initial orientation.
            float u1 = next(), u2 = next(), u3 = next();
            float s1 = std::sqrt(1.0f - u1), s2 = std::sqrt(u1);
            glm::vec4 q(s1 * std::sin(6.2831853f * u2), s1 * std::cos(6.2831853f * u2),
                        s2 * std::sin(6.2831853f * u3), s2 * std::cos(6.2831853f * u3));
            glm::vec3 spin((next() - 0.5f) * 12.0f, (next() - 0.5f) * 12.0f, (next() - 0.5f) * 12.0f);
            pool[slot].posLife   = glm::vec4(pos[0], pos[1], pos[2], lifetime * (0.7f + 0.6f * next()));
            pool[slot].velScale  = glm::vec4(v, he);
            pool[slot].spinState = glm::vec4(spin, kDebrisActiveState); // ACTIVE, sleepCtr 0
            pool[slot].rot       = q;
            if (wasDead) { counters[0] += 1; ++m_debrisAlive; }
            ++spawned;
        }
        // Make the host writes visible to the GPU before the next compute dispatch
        // (no-op when the allocation is HOST_COHERENT; correct when it is not).
        vmaFlushAllocation(m_alloc, m_debrisPoolAlloc, 0, VK_WHOLE_SIZE);
        vmaFlushAllocation(m_alloc, m_debrisCountAlloc, 0, VK_WHOLE_SIZE);
        return spawned;
    }

    void gpuDebrisStep(float dt) override {
        if (!m_debrisComputePipeline) return;
        // Write this frame's params UBO + (re)bind it to the compute set.
        GpuDebrisParamsUBO u{};
        const auto& p = m_debrisParams;
        u.gravityDt  = glm::vec4(p.gravity[0], p.gravity[1], p.gravity[2], dt);
        u.groundDamp = glm::vec4(p.groundY, p.restitution, p.friction, p.linearDamping);
        u.sleepCap   = glm::vec4(p.sleepLinSpeed, p.sleepAngSpeed, (float)p.sleepFrames, (float)kDebrisCapacity);
        u.aabbCount  = glm::vec4((float)std::min<uint32_t>(p.aabbCount, 4u), 0, 0, 0);
        for (uint32_t a = 0; a < 4; ++a) {
            u.aabbMin[a] = glm::vec4(p.aabbMin[a][0], p.aabbMin[a][1], p.aabbMin[a][2], 0);
            u.aabbMax[a] = glm::vec4(p.aabbMax[a][0], p.aabbMax[a][1], p.aabbMax[a][2], 0);
        }
        // Write THIS frame's params UBO (the per-frame compute set already points at
        // it — no descriptor update needed, so no pending-set hazard). The compute
        // dispatch is recorded by the graph's debris-compute pass this frame.
        if (m_debrisParamsMapped[m_frameIdx])
            std::memcpy(m_debrisParamsMapped[m_frameIdx], &u, sizeof(u));
        m_debrisStepPending = true;  // buildAndExecuteGraph adds the compute pass this frame
    }

    void gpuDebrisDraw(const FrameContext& fc, const float tint[4]) override {
        if (!fc.valid || !m_debrisDrawPipeline) return;
        GpuDebrisDrawUBO u{};
        u.viewProj = m_lastViewProj;
        u.color = tint ? glm::vec4(tint[0], tint[1], tint[2], tint[3]) : glm::vec4(0.7f, 0.55f, 0.4f, 1.0f);
        if (m_debrisDrawUboMapped[m_frameIdx])
            std::memcpy(m_debrisDrawUboMapped[m_frameIdx], &u, sizeof(u));
        m_debrisDrawPending = true;  // buildAndExecuteGraph records the instanced cube draw
    }

    IRenderDevice::GpuDebrisStats gpuDebrisReadback(float boundsLimit) const override {
        IRenderDevice::GpuDebrisStats s{};
        s.capacity = kDebrisCapacity;
        if (!m_debrisPoolMapped) return s;
        // Diagnostic / test path (NOT the hot path): make sure every in-flight compute
        // dispatch has retired so the host-visible mapped pool reflects the final GPU
        // state, then invalidate the allocation before reading (no-op when coherent).
        if (m_dev.device) vkDeviceWaitIdle(m_dev.device);
        vmaInvalidateAllocation(m_alloc, m_debrisPoolAlloc, 0, VK_WHOLE_SIZE);
        vmaInvalidateAllocation(m_alloc, m_debrisCountAlloc, 0, VK_WHOLE_SIZE);
        // The pool SSBO is host-visible mapped; summarize live slots.
        const GpuDebrisFragment* pool = (const GpuDebrisFragment*)m_debrisPoolMapped;
        bool any = false;
        float minY = 0, maxY = 0, maxSpeed = 0;
        for (uint32_t i = 0; i < kDebrisCapacity; ++i) {
            float state = floorf(pool[i].spinState.w + 0.5f);
            if (state == kDebrisDeadState) continue;
            ++s.alive;
            if (state == 2.0f) ++s.settled;   // SLEEP
            const glm::vec4& pl = pool[i].posLife;
            const glm::vec4& vs = pool[i].velScale;
            // NaN/Inf check on all motion components.
            float comps[7] = { pl.x, pl.y, pl.z, vs.x, vs.y, vs.z, pl.w };
            bool bad = false;
            for (float c : comps) if (std::isnan(c) || std::isinf(c)) bad = true;
            if (bad) { ++s.nanCount; continue; }
            if (std::abs(pl.x) > boundsLimit || std::abs(pl.y) > boundsLimit || std::abs(pl.z) > boundsLimit)
                ++s.outOfBounds;
            float sp = std::sqrt(vs.x*vs.x + vs.y*vs.y + vs.z*vs.z);
            if (!any) { minY = maxY = pl.y; maxSpeed = sp; any = true; }
            else { minY = std::min(minY, pl.y); maxY = std::max(maxY, pl.y); maxSpeed = std::max(maxSpeed, sp); }
        }
        s.minY = minY; s.maxY = maxY; s.maxSpeed = maxSpeed;
        return s;
    }

    // Record the debris compute dispatch (synchronous, graphics queue). Called by the
    // graph's debris-compute pass (before the draw). An SSBO write->read barrier after
    // the dispatch lets the instanced draw + the next-frame readback see the result.
    void recordDebrisComputeBody(VkCommandBuffer cmd) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_debrisComputePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_debrisComputeLayout,
                                0, 1, &m_debrisComputeSet[m_frameIdx], 0, nullptr);
        uint32_t groups = (kDebrisCapacity + 63u) / 64u;
        vkCmdDispatch(cmd, groups, 1, 1);
        // Barrier: compute SSBO write -> (vertex SSBO read for the draw) + (host read
        // for the readback) + (next frame's compute read/write of the persistent pool).
        // Covers the in-frame draw, the post-fence readback, AND the cross-frame pool
        // read-modify-write hazard (the pool persists; consecutive compute dispatches
        // on this single graphics queue must order against each other).
        VkMemoryBarrier2 mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        mb.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        mb.dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_HOST_BIT
                         | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_HOST_READ_BIT
                         | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        VkDependencyInfo di{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &di);
    }

    // Record the debris instanced cube draw into the (already-open) particle/transparent
    // pass. ONE instanced draw over the whole pool; dead slots collapse in the VS.
    void recordDebrisDrawBody(VkCommandBuffer cmd) {
        if (!m_debrisDrawPending || !m_debrisDrawPipeline) return;
        postViewport(cmd, m_extent);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_debrisDrawPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_debrisDrawLayout,
                                0, 1, &m_debrisDrawSet[m_frameIdx], 0, nullptr);
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &m_debrisCubeVbo, &zero);
        vkCmdBindIndexBuffer(cmd, m_debrisCubeIbo, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, m_debrisCubeIndexCount, kDebrisCapacity, 0, 0, 0);
    }

    // ======================================================================
    // GPU compute skinning of models (GPU SKINNING OF MODELS).
    //
    // Clean-room, original work. Built ONLY from the engine's own RHI/loader
    // interfaces, the Vulkan 1.3 spec (compute pipelines, SSBOs, barriers), and
    // public glTF 2.0 linear-blend-skinning math. No id Tech / RBDOOM source.
    //
    // Create the compute pipeline + descriptor pool/layout once at init. Each
    // registered skinned mesh allocates its own buffers + per-frame descriptor sets.
    // ======================================================================
    bool createSkinning() {
        // Descriptor pool: per skinned mesh we allocate kFramesInFlight sets, each
        // with 4 storage buffers (src verts, influences, palette, dst output). Size
        // for a generous number of simultaneously-registered skinned instances.
        const uint32_t kMaxSkinnedMeshes = 256;
        const uint32_t maxSets = kMaxSkinnedMeshes * kFramesInFlight;
        VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, maxSets * 4 };
        VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        // FREE_DESCRIPTOR_SET so unregisterSkinnedMesh can return sets to the pool
        // (a long session may register/free many characters).
        pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pci.maxSets = maxSets; pci.poolSizeCount = 1; pci.pPoolSizes = &ps;
        if (vkCreateDescriptorPool(m_dev.device, &pci, nullptr, &m_skinPool) != VK_SUCCESS) {
            logError("[rhi] skin desc pool failed"); return false; }

        // Set layout: b0 src verts (RO), b1 influences (RO), b2 palette (RO), b3 dst (RW).
        VkDescriptorSetLayoutBinding b[4]{};
        for (uint32_t i = 0; i < 4; ++i) {
            b[i].binding = i; b[i].descriptorCount = 1;
            b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        slci.bindingCount = 4; slci.pBindings = b;
        if (vkCreateDescriptorSetLayout(m_dev.device, &slci, nullptr, &m_skinSetLayout) != VK_SUCCESS) {
            logError("[rhi] skin set layout failed"); return false; }

        // Pipeline layout: 1 set + a push constant (vertexCount, jointCount).
        VkPushConstantRange pcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SkinPush) };
        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount = 1; plci.pSetLayouts = &m_skinSetLayout;
        plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
        if (vkCreatePipelineLayout(m_dev.device, &plci, nullptr, &m_skinPipelineLayout) != VK_SUCCESS) {
            logError("[rhi] skin pipeline layout failed"); return false; }

        VkShaderModule cs = loadShaderModule("shaders\\skin.comp.spv");
        if (!cs) return false;
        VkComputePipelineCreateInfo cpci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpci.stage.module = cs; cpci.stage.pName = "main";
        cpci.layout = m_skinPipelineLayout;
        VkResult cr = vkCreateComputePipelines(m_dev.device, VK_NULL_HANDLE, 1, &cpci, nullptr, &m_skinPipeline);
        vkDestroyShaderModule(m_dev.device, cs, nullptr);
        if (cr != VK_SUCCESS) { logError("[rhi] skin compute pipeline create failed"); return false; }

        logInfo("[rhi] GPU compute skinning ready (compute LBS pre-pass into per-frame skinned vbo)");
        return true;
    }

    void destroySkinning() {
        // Free all registered skinned meshes' resources first.
        for (auto& kv : m_skinnedMeshes) destroySkinnedMeshResources(kv.second);
        m_skinnedMeshes.clear();
        m_skinPending.clear();
        if (m_skinPipeline)       { vkDestroyPipeline(m_dev.device, m_skinPipeline, nullptr); m_skinPipeline = VK_NULL_HANDLE; }
        if (m_skinPipelineLayout) { vkDestroyPipelineLayout(m_dev.device, m_skinPipelineLayout, nullptr); m_skinPipelineLayout = VK_NULL_HANDLE; }
        if (m_skinSetLayout)      { vkDestroyDescriptorSetLayout(m_dev.device, m_skinSetLayout, nullptr); m_skinSetLayout = VK_NULL_HANDLE; }
        if (m_skinPool)           { vkDestroyDescriptorPool(m_dev.device, m_skinPool, nullptr); m_skinPool = VK_NULL_HANDLE; }
    }

    // Free one skinned mesh's GPU resources (immediate; callers ensure the GPU is
    // idle for this buffer set — registration-time rollback, unregister waits idle,
    // and shutdown already waited idle).
    void destroySkinnedMeshResources(SkinnedMesh& sm) {
        if (sm.srcVbo) { vmaDestroyBuffer(m_alloc, sm.srcVbo, sm.srcAlloc); sm.srcVbo = VK_NULL_HANDLE; sm.srcAlloc = nullptr; }
        if (sm.infBuf) { vmaDestroyBuffer(m_alloc, sm.infBuf, sm.infAlloc); sm.infBuf = VK_NULL_HANDLE; sm.infAlloc = nullptr; }
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (sm.palBuf[i]) { vmaDestroyBuffer(m_alloc, sm.palBuf[i], sm.palAlloc[i]); sm.palBuf[i] = VK_NULL_HANDLE; sm.palAlloc[i] = nullptr; sm.palMapped[i] = nullptr; }
        }
        if (m_skinPool && sm.set[0]) {
            vkFreeDescriptorSets(m_dev.device, m_skinPool, kFramesInFlight, sm.set);
            for (uint32_t i = 0; i < kFramesInFlight; ++i) sm.set[i] = VK_NULL_HANDLE;
        }
    }

    // Promote a mesh to a compute-skinned dynamic mesh: allocate kFramesInFlight
    // skinned-output vertex buffers with STORAGE usage (so the compute can write them
    // and the draw can read them as vertex buffers) and seed them from the bind pose.
    // Mirrors updateMesh's dynamic promotion but the buffers are GPU-written, so they
    // are DEVICE_LOCAL + STORAGE | VERTEX (no host mapping needed).
    bool promoteMeshForSkinning(Mesh& m, const MeshVertex* bindVerts, uint32_t vcount) {
        const VkDeviceSize bytes = (VkDeviceSize)vcount * sizeof(MeshVertex);
        VkBuffer made[kFramesInFlight] = {}; VmaAllocation madeA[kFramesInFlight] = {};
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            // Seed from the bind pose via a staging copy so a draw before the first
            // dispatch (or a pass that reads a not-yet-dispatched slot) shows valid
            // geometry rather than garbage.
            if (!createDeviceLocalBuffer(bindVerts, bytes,
                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                    | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,   // readbackSkinnedMesh copies from it (test path)
                    made[i], madeA[i])) {
                for (uint32_t k = 0; k < i; ++k) vmaDestroyBuffer(m_alloc, made[k], madeA[k]);
                logError("[rhi] skin: output vbo alloc failed"); return false;
            }
        }
        // If the mesh was already dynamic (CPU-skinning had run), defer-free those.
        if (m.dynamic) {
            for (uint32_t i = 0; i < kFramesInFlight; ++i)
                deferDestroyBuffer(m.dynVbo[i], m.dynVboAlloc[i]);
        } else if (m.vbo) {
            deferDestroyBuffer(m.vbo, m.vboAlloc);
            m.vbo = VK_NULL_HANDLE; m.vboAlloc = nullptr;
        }
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            m.dynVbo[i] = made[i]; m.dynVboAlloc[i] = madeA[i]; m.dynMapped[i] = nullptr;
        }
        m.dynamic = true;
        return true;
    }

    bool registerSkinnedMesh(MeshHandle mesh, const MeshVertex* bindVerts, uint32_t vcount,
                             const uint16_t* jointIdx4, const float* jointWt4) override {
        if (!m_skinPipeline) return false;          // no compute skinning support
        if (!bindVerts || !jointIdx4 || !jointWt4 || vcount == 0) return false;
        auto mit = m_meshes.find(mesh.id);
        if (mit == m_meshes.end()) return false;
        Mesh& m = mit->second;
        if (vcount != m.vertexCount) { logError("[rhi] registerSkinnedMesh: vcount mismatch"); return false; }
        // Re-register: free the prior skinning resources (after the GPU drains them).
        auto existing = m_skinnedMeshes.find(mesh.id);
        if (existing != m_skinnedMeshes.end()) {
            vkDeviceWaitIdle(m_dev.device);
            destroySkinnedMeshResources(existing->second);
            m_skinnedMeshes.erase(existing);
        }

        SkinnedMesh sm{};
        sm.vertexCount = vcount;

        // --- Immutable bind-pose source verts (SkinSrcVertex rows). ---
        {
            std::vector<SkinSrcVertex> src(vcount);
            for (uint32_t v = 0; v < vcount; ++v) {
                const MeshVertex& iv = bindVerts[v];
                src[v].posPad = glm::vec4(iv.pos[0], iv.pos[1], iv.pos[2], 0.0f);
                src[v].nrmPad = glm::vec4(iv.normal[0], iv.normal[1], iv.normal[2], 0.0f);
                src[v].uvPad  = glm::vec4(iv.uv[0], iv.uv[1], 0.0f, 0.0f);
            }
            if (!createDeviceLocalBuffer(src.data(), (VkDeviceSize)vcount * sizeof(SkinSrcVertex),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, sm.srcVbo, sm.srcAlloc)) {
                logError("[rhi] skin: src vbo alloc failed"); return false; }
        }
        // --- Immutable influences (SkinSrcInfluence rows). ---
        {
            std::vector<SkinSrcInfluence> inf(vcount);
            for (uint32_t v = 0; v < vcount; ++v) {
                inf[v].idx = glm::uvec4(jointIdx4[v*4+0], jointIdx4[v*4+1],
                                        jointIdx4[v*4+2], jointIdx4[v*4+3]);
                inf[v].wt  = glm::vec4(jointWt4[v*4+0], jointWt4[v*4+1],
                                       jointWt4[v*4+2], jointWt4[v*4+3]);
            }
            if (!createDeviceLocalBuffer(inf.data(), (VkDeviceSize)vcount * sizeof(SkinSrcInfluence),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, sm.infBuf, sm.infAlloc)) {
                logError("[rhi] skin: influence buffer alloc failed");
                vmaDestroyBuffer(m_alloc, sm.srcVbo, sm.srcAlloc); return false; }
        }
        // --- Per-frame palette SSBO (host-visible mapped; seeded to identity). ---
        const VkDeviceSize palBytes = (VkDeviceSize)kMaxSkinJoints * 16 * sizeof(float);
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = palBytes; bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info{};
            if (vmaCreateBuffer(m_alloc, &bci, &aci, &sm.palBuf[i], &sm.palAlloc[i], &info) != VK_SUCCESS) {
                logError("[rhi] skin: palette SSBO alloc failed");
                destroySkinnedMeshResources(sm); return false; }
            sm.palMapped[i] = info.pMappedData;
            // Seed identity so a pre-palette dispatch reproduces the bind pose.
            float* pal = (float*)sm.palMapped[i];
            for (uint32_t j = 0; j < kMaxSkinJoints; ++j) {
                float* mm = pal + j * 16;
                for (int e = 0; e < 16; ++e) mm[e] = (e % 5 == 0) ? 1.0f : 0.0f;
            }
            vmaFlushAllocation(m_alloc, sm.palAlloc[i], 0, palBytes);
        }

        // --- Promote the drawable mesh's vbo to a compute-written skinned output. ---
        if (!promoteMeshForSkinning(m, bindVerts, vcount)) {
            destroySkinnedMeshResources(sm); return false; }

        // --- Per-frame descriptor sets (b0 src, b1 inf, b2 palette[i], b3 dst[i]). ---
        VkDescriptorSetLayout layouts[kFramesInFlight];
        for (uint32_t i = 0; i < kFramesInFlight; ++i) layouts[i] = m_skinSetLayout;
        VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dsai.descriptorPool = m_skinPool; dsai.descriptorSetCount = kFramesInFlight; dsai.pSetLayouts = layouts;
        if (vkAllocateDescriptorSets(m_dev.device, &dsai, sm.set) != VK_SUCCESS) {
            logError("[rhi] skin: descriptor set alloc failed");
            destroySkinnedMeshResources(sm); return false; }
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkDescriptorBufferInfo srcI{ sm.srcVbo, 0, VK_WHOLE_SIZE };
            VkDescriptorBufferInfo infI{ sm.infBuf, 0, VK_WHOLE_SIZE };
            VkDescriptorBufferInfo palI{ sm.palBuf[i], 0, VK_WHOLE_SIZE };
            VkDescriptorBufferInfo dstI{ m.dynVbo[i], 0, VK_WHOLE_SIZE };
            VkWriteDescriptorSet w[4]{};
            const VkDescriptorBufferInfo* infos[4] = { &srcI, &infI, &palI, &dstI };
            for (uint32_t k = 0; k < 4; ++k) {
                w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[k].dstSet = sm.set[i];
                w[k].dstBinding = k; w[k].descriptorCount = 1;
                w[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[k].pBufferInfo = infos[k];
            }
            vkUpdateDescriptorSets(m_dev.device, 4, w, 0, nullptr);
        }

        m_skinnedMeshes.emplace(mesh.id, sm);
        return true;
    }

    void unregisterSkinnedMesh(MeshHandle mesh) override {
        auto it = m_skinnedMeshes.find(mesh.id);
        if (it == m_skinnedMeshes.end()) return;
        if (m_dev.device) vkDeviceWaitIdle(m_dev.device);   // ensure no in-flight use
        destroySkinnedMeshResources(it->second);
        m_skinnedMeshes.erase(it);
        // Drop any pending dispatch for it this frame.
        for (auto p = m_skinPending.begin(); p != m_skinPending.end(); ) {
            if (*p == mesh.id) p = m_skinPending.erase(p); else ++p;
        }
    }

    void setSkinnedPalette(MeshHandle mesh, const float* palette, uint32_t jointCount) override {
        if (!palette || jointCount == 0) return;
        auto it = m_skinnedMeshes.find(mesh.id);
        if (it == m_skinnedMeshes.end()) return;
        SkinnedMesh& sm = it->second;
        const uint32_t jc = std::min(jointCount, kMaxSkinJoints);
        const uint32_t fi = m_frameIdx;
        if (!sm.palMapped[fi]) return;
        std::memcpy(sm.palMapped[fi], palette, (size_t)jc * 16 * sizeof(float));
        vmaFlushAllocation(m_alloc, sm.palAlloc[fi], 0, (VkDeviceSize)jc * 16 * sizeof(float));
        sm.jointCount = jc;
        // Queue the dispatch for this frame (dedup: only once per frame per mesh).
        bool queued = false;
        for (uint32_t id : m_skinPending) if (id == mesh.id) { queued = true; break; }
        if (!queued) m_skinPending.push_back(mesh.id);
        m_skinStepPending = true;
    }

    bool readbackSkinnedMesh(MeshHandle mesh, MeshVertex* out, uint32_t vcount) override {
        auto it = m_skinnedMeshes.find(mesh.id);
        if (it == m_skinnedMeshes.end() || !out) return false;
        SkinnedMesh& sm = it->second;
        if (vcount != sm.vertexCount) return false;
        auto mit = m_meshes.find(mesh.id);
        if (mit == m_meshes.end()) return false;
        // The most-recently-skinned slot (set by the last dispatch). If skinning has
        // never run for this mesh, fall back to the current frame's slot (bind pose).
        uint32_t slot = (sm.lastSkinnedFrame < kFramesInFlight) ? sm.lastSkinnedFrame : m_frameIdx;
        VkBuffer srcBuf = mit->second.dynVbo[slot];
        if (!srcBuf) return false;
        const VkDeviceSize bytes = (VkDeviceSize)vcount * sizeof(MeshVertex);
        // Wait for all in-flight GPU work (the dispatch that wrote this slot) to retire.
        if (m_dev.device) vkDeviceWaitIdle(m_dev.device);
        // Copy the DEVICE_LOCAL skinned output into a host-visible readback buffer.
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = bytes; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer rb = VK_NULL_HANDLE; VmaAllocation rbA = nullptr; VmaAllocationInfo rbI{};
        if (vmaCreateBuffer(m_alloc, &bci, &aci, &rb, &rbA, &rbI) != VK_SUCCESS) {
            logError("[rhi] readbackSkinnedMesh: readback buffer alloc failed"); return false; }
        bool ok = oneTimeSubmit([&](VkCommandBuffer cmd){
            VkBufferCopy region{ 0, 0, bytes };
            vkCmdCopyBuffer(cmd, srcBuf, rb, 1, &region);
        });
        if (ok) {
            vmaInvalidateAllocation(m_alloc, rbA, 0, bytes);
            std::memcpy(out, rbI.pMappedData, (size_t)bytes);
        }
        vmaDestroyBuffer(m_alloc, rb, rbA);
        return ok;
    }

    bool supportsGpuSkinning() const override { return m_skinPipeline != VK_NULL_HANDLE; }

    // Record the skinning compute pass: ONE dispatch per pending skinned instance,
    // each writing its mesh's per-frame skinned-output vbo. A single SSBO write ->
    // vertex-read barrier after all dispatches lets the depth/shadow/color passes
    // read the skinned vertices. Recorded as the FIRST graphics-queue pass each frame
    // (before shadow/depth/color), so all three passes draw the skinned geometry.
    void recordSkinComputeBody(VkCommandBuffer cmd) {
        if (m_skinPending.empty() || !m_skinPipeline) return;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_skinPipeline);
        for (uint32_t id : m_skinPending) {
            auto it = m_skinnedMeshes.find(id);
            if (it == m_skinnedMeshes.end()) continue;
            SkinnedMesh& sm = it->second;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_skinPipelineLayout,
                                    0, 1, &sm.set[m_frameIdx], 0, nullptr);
            SkinPush pc{ sm.vertexCount, sm.jointCount };
            vkCmdPushConstants(cmd, m_skinPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(pc), &pc);
            uint32_t groups = (sm.vertexCount + 63u) / 64u;
            vkCmdDispatch(cmd, groups, 1, 1);
            sm.lastSkinnedFrame = m_frameIdx;     // this slot now holds the skinned output
        }
        // Barrier: compute SSBO write -> vertex-attribute read (the draw passes bind
        // the output as a vertex buffer) + host read (the test readback) + index/
        // vertex stages of the upcoming shadow/depth/color passes.
        VkMemoryBarrier2 mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        mb.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        mb.dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT
                         | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
        mb.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT
                         | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo di{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &di);
    }

    // Record the particle + decal draws into the (already-open) particle pass.
    // Order: DECALS first (alpha, on surfaces) then ALPHA particles (smoke/dust)
    // then ADDITIVE particles (sparks/muzzle — glow last so they sit brightest).
    void recordParticlePassBody(VkCommandBuffer cmd) {
        postViewport(cmd, m_extent);
        VkDeviceSize zero = 0;

        if (m_decalCount > 0 && m_decalPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_decalPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_decalLayout,
                                    0, 1, &m_decalSet[m_frameIdx], 0, nullptr);
            vkCmdBindVertexBuffers(cmd, 0, 1, &m_partQuadVbo, &zero);
            vkCmdBindVertexBuffers(cmd, 1, 1, &m_decalInstBuf[m_frameIdx], &zero);
            vkCmdDraw(cmd, 4, m_decalCount, 0, 0);   // 4 verts (strip) x N instances
        }
        if (m_partAlphaCount > 0 && m_partAlphaPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_partAlphaPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_partLayout,
                                    0, 1, &m_partSet[m_frameIdx], 0, nullptr);
            vkCmdBindVertexBuffers(cmd, 0, 1, &m_partQuadVbo, &zero);
            vkCmdBindVertexBuffers(cmd, 1, 1, &m_partInstAlphaBuf[m_frameIdx], &zero);
            vkCmdDraw(cmd, 4, m_partAlphaCount, 0, 0);
        }
        if (m_partAddCount > 0 && m_partAddPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_partAddPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_partLayout,
                                    0, 1, &m_partSet[m_frameIdx], 0, nullptr);
            vkCmdBindVertexBuffers(cmd, 0, 1, &m_partQuadVbo, &zero);
            vkCmdBindVertexBuffers(cmd, 1, 1, &m_partInstAddBuf[m_frameIdx], &zero);
            vkCmdDraw(cmd, 4, m_partAddCount, 0, 0);
        }
    }

    void destroyHud() {
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

    bool createSwapchain(uint32_t w, uint32_t h) {
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
        // HDR scene + bloom mips track the frame extent — rebuild + rewrite their
        // descriptor sets after the swapchain (and m_extent) are updated.
        createBloomTargets();
        writePostDescriptors();
        // SSAO half-res targets track the extent; the depth view also changed, so
        // rewrite every SSAO descriptor that references depth/AO views.
        createSsaoTargets();
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
    }

    // ---- HEADLESS offscreen render target (no window, no swapchain) ---------
    // Creates the offscreen COLOR image the render graph targets in place of an
    // acquired swapchain image, plus the matching depth image. The color image
    // uses the SAME default format the windowed swapchain used (B8G8R8A8_UNORM)
    // and COLOR_ATTACHMENT | TRANSFER_SRC usage, so the existing capture readback
    // + BGRA->RGBA swizzle produce byte-identical PNGs. This mirrors the depth
    // image creation in createSwapchain() so both modes size their depth the same.
    bool createOffscreenTarget(uint32_t w, uint32_t h) {
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
        if (vmaCreateImage(m_alloc, &cici, &caci, &m_offscreenColorImg, &m_offscreenColorAlloc, nullptr) != VK_SUCCESS) {
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
        if (vmaCreateImage(m_alloc, &dici, &daci, &m_depthImg, &m_depthAlloc, nullptr) != VK_SUCCESS) {
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

    void destroyOffscreenTarget() {
        if (m_depthView) { vkDestroyImageView(m_dev.device, m_depthView, nullptr); m_depthView = VK_NULL_HANDLE; }
        if (m_depthImg)  { vmaDestroyImage(m_alloc, m_depthImg, m_depthAlloc); m_depthImg = VK_NULL_HANDLE; m_depthAlloc = nullptr; }
        if (m_offscreenColorView) { vkDestroyImageView(m_dev.device, m_offscreenColorView, nullptr); m_offscreenColorView = VK_NULL_HANDLE; }
        if (m_offscreenColorImg)  { vmaDestroyImage(m_alloc, m_offscreenColorImg, m_offscreenColorAlloc); m_offscreenColorImg = VK_NULL_HANDLE; m_offscreenColorAlloc = nullptr; }
    }

    // Headless analogue of recreateSwapchain(): idle the device, destroy + recreate
    // the offscreen color + depth images at the new size, and re-point the graph
    // (the graph re-imports the offscreen image by handle each frame in
    // buildAndExecuteGraph, so simply recreating the images here is enough — the
    // next beginFrame/endFrame targets the new images). This exercises the same
    // resize/recreate code path the windowed swapchain recreate does, validated.
    void recreateOffscreenTarget() {
        vkDeviceWaitIdle(m_dev.device);
        createOffscreenTarget(m_width, m_height);
        // The HDR scene + bloom mips are sized to the frame extent — rebuild them
        // (and rewrite the post descriptor sets) so they track the new size.
        createBloomTargets();
        writePostDescriptors();
        // SSAO half-res targets + depth-referencing descriptors track the extent.
        createSsaoTargets();
        writeSsaoDescriptors();
        // GI half-res targets + prev-depth track the extent; rebuild + rewrite.
        createGiTargets();
        writeGiDescriptors();
        m_giHistoryValid = false;
        // Water samples the scene depth: the depth view changed -> rewire it.
        writeWaterDescriptors();
        // Particles sample the scene depth (soft fade): rewire on the new depth view.
        writeParticleDescriptors();
    }

    // ---- HDR scene + bloom render targets ----------------------------------
    // Create (or recreate) the linear HDR scene target + the bloom mip chain at
    // the current frame extent. Called after the swapchain/offscreen target exists
    // (so m_extent is valid) and on every resize. Images: COLOR_ATTACHMENT (render
    // target) | SAMPLED (read by the next post pass) | TRANSFER_DST is not needed.
    bool createBloomTargets() {
        destroyBloomTargets();
        const uint32_t W = m_extent.width, H = m_extent.height;
        if (W == 0 || H == 0) { logError("[rhi] bloom: zero extent"); return false; }

        // HDR scene target (full resolution).
        if (!createColorTarget(kHdrFormat, W, H,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               m_hdrImg, m_hdrAlloc, m_hdrView)) {
            logError("[rhi] HDR scene target create failed"); return false;
        }

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
        return true;
    }

    void destroyBloomTargets() {
        for (uint32_t i = 0; i < kBloomMips; ++i) {
            BloomMip& m = m_bloomMips[i];
            if (m.view)  { vkDestroyImageView(m_dev.device, m.view, nullptr); m.view = VK_NULL_HANDLE; }
            if (m.img)   { vmaDestroyImage(m_alloc, m.img, m.alloc); m.img = VK_NULL_HANDLE; m.alloc = nullptr; }
            m.extent = {};
        }
        if (m_hdrView) { vkDestroyImageView(m_dev.device, m_hdrView, nullptr); m_hdrView = VK_NULL_HANDLE; }
        if (m_hdrImg)  { vmaDestroyImage(m_alloc, m_hdrImg, m_hdrAlloc); m_hdrImg = VK_NULL_HANDLE; m_hdrAlloc = nullptr; }
    }

    // Helper: create a single-mip 2D color image + view (used for HDR + bloom mips).
    bool createColorTarget(VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage,
                           VkImage& outImg, VmaAllocation& outAlloc, VkImageView& outView) {
        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D; ici.format = fmt;
        ici.extent = { w, h, 1 }; ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = usage; ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        if (vmaCreateImage(m_alloc, &ici, &aci, &outImg, &outAlloc, nullptr) != VK_SUCCESS) return false;
        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = outImg; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(m_dev.device, &vci, nullptr, &outView) != VK_SUCCESS) return false;
        return true;
    }

    // ---- Post-process pipelines (bloom down/up + composite) ----------------
    // Build the sampler, descriptor-set layouts (1 sampler for down/up, 2 for the
    // composite), descriptor pool + sets, and the three full-screen-triangle
    // pipelines. The pipelines are extent-independent (dynamic viewport/scissor),
    // so they are created ONCE; only the target IMAGES + descriptor set writes are
    // recreated on resize (via createBloomTargets + writePostDescriptors).
    bool createPost() {
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
        // Descriptor set layout: 2 combined image samplers (composite: HDR + bloom).
        VkDescriptorSetLayoutBinding b2[2]{};
        b2[0].binding = 0; b2[0].descriptorCount = 1;
        b2[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b2[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b2[1].binding = 1; b2[1].descriptorCount = 1;
        b2[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b2[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo s2{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        s2.bindingCount = 2; s2.pBindings = b2;
        if (vkCreateDescriptorSetLayout(m_dev.device, &s2, nullptr, &m_postSetLayout2) != VK_SUCCESS) {
            logError("[rhi] post set layout (2) failed"); return false;
        }

        // Descriptor pool: (HDR set + kBloomMips mip sets) single-sampler sets +
        // 1 composite set (2 samplers). Sized exactly; no UPDATE_AFTER_BIND needed.
        const uint32_t single = 1 + kBloomMips;     // HDR + each mip
        VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, single + 2 };
        VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pci.maxSets = single + 1; pci.poolSizeCount = 1; pci.pPoolSizes = &ps;
        if (vkCreateDescriptorPool(m_dev.device, &pci, nullptr, &m_postPool) != VK_SUCCESS) {
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
        {
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = m_postPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &m_postSetLayout2;
            if (vkAllocateDescriptorSets(m_dev.device, &ai, &m_setComposite) != VK_SUCCESS) {
                logError("[rhi] post set alloc (composite) failed"); return false;
            }
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

        logInfo("[rhi] HDR post pipeline ready (R16G16B16A16_SFLOAT scene + " +
                std::to_string(kBloomMips) + "-mip bloom + ACES composite)");
        return true;
    }

    // Build a full-screen-triangle post pipeline (no vertex input, no depth, single
    // color attachment of `colorFmt`). `additiveBlend` selects ONE,ONE additive
    // blending (bloom upsample accumulation) vs. opaque write.
    bool createFullscreenPipeline(const char* vsPath, const char* fsPath,
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
        VkResult pr = vkCreateGraphicsPipelines(m_dev.device, VK_NULL_HANDLE, 1, &gpci, nullptr, &outPipe);

        vkDestroyShaderModule(m_dev.device, vs, nullptr);
        vkDestroyShaderModule(m_dev.device, fs, nullptr);
        if (pr != VK_SUCCESS) { logError("[rhi] post pipeline create failed"); return false; }
        return true;
    }

    // (Re)write the post descriptor sets to point at the current HDR + bloom mip
    // image views. Called after createBloomTargets() at init + every resize. The
    // images are in SHADER_READ_ONLY when sampled (the graph transitions them), so
    // the descriptor imageLayout is SHADER_READ_ONLY_OPTIMAL.
    void writePostDescriptors() {
        auto write1 = [&](VkDescriptorSet set, VkImageView view) {
            VkDescriptorImageInfo dii{ m_postSampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet = set; w.dstBinding = 0; w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &dii;
            vkUpdateDescriptorSets(m_dev.device, 1, &w, 0, nullptr);
        };
        write1(m_setHdr, m_hdrView);
        for (uint32_t i = 0; i < kBloomMips; ++i) write1(m_setMip[i], m_bloomMips[i].view);

        // Composite set: binding 0 = HDR scene, binding 1 = bloom mip0.
        VkDescriptorImageInfo d0{ m_postSampler, m_hdrView,           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo d1{ m_postSampler, m_bloomMips[0].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet cw[2]{};
        cw[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        cw[0].dstSet = m_setComposite; cw[0].dstBinding = 0; cw[0].descriptorCount = 1;
        cw[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; cw[0].pImageInfo = &d0;
        cw[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        cw[1].dstSet = m_setComposite; cw[1].dstBinding = 1; cw[1].descriptorCount = 1;
        cw[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; cw[1].pImageInfo = &d1;
        vkUpdateDescriptorSets(m_dev.device, 2, cw, 0, nullptr);
    }

    void destroyPost() {
        destroyBloomTargets();
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

    // =====================================================================
    // SSAO setup. Builds: a NEAREST depth sampler (sample the depth image as
    // data), a CLAMP linear sampler (up-sample the AO), the depth pre-pass
    // pipeline (depth.vert, reuses m_shadowLayout = objSet), the SSAO + blur
    // full-screen pipelines, descriptor layouts/pool/sets, and the per-frame
    // SSAO + control UBOs. Extent-dependent images are made in createSsaoTargets.
    // =====================================================================
    bool createSsao() {
        // NEAREST sampler for reading the depth image as plain data (no compare).
        VkSamplerCreateInfo dsci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        dsci.magFilter = VK_FILTER_NEAREST; dsci.minFilter = VK_FILTER_NEAREST;
        dsci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        dsci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        dsci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        dsci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        dsci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        if (vkCreateSampler(m_dev.device, &dsci, nullptr, &m_depthSampler) != VK_SUCCESS) {
            logError("[rhi] ssao depth sampler failed"); return false;
        }
        // CLAMP linear sampler for up-sampling the half-res AO into mesh.frag.
        VkSamplerCreateInfo lsci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        lsci.magFilter = VK_FILTER_LINEAR; lsci.minFilter = VK_FILTER_LINEAR;
        lsci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        lsci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        lsci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        lsci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        lsci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        if (vkCreateSampler(m_dev.device, &lsci, nullptr, &m_ssaoLinearSampler) != VK_SUCCESS) {
            logError("[rhi] ssao linear sampler failed"); return false;
        }

        // ---- Descriptor set layout (ssao.frag): binding0 = depth sampler,
        //      binding1 = SsaoUBO. ----
        {
            VkDescriptorSetLayoutBinding b[2]{};
            b[0].binding = 0; b[0].descriptorCount = 1;
            b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            b[1].binding = 1; b[1].descriptorCount = 1;
            b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = 2; ci.pBindings = b;
            if (vkCreateDescriptorSetLayout(m_dev.device, &ci, nullptr, &m_ssaoSetLayout) != VK_SUCCESS) {
                logError("[rhi] ssao set layout failed"); return false;
            }
        }
        // ---- Blur set layout: binding0 = raw AO, binding1 = depth. ----
        {
            VkDescriptorSetLayoutBinding b[2]{};
            b[0].binding = 0; b[0].descriptorCount = 1;
            b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            b[1].binding = 1; b[1].descriptorCount = 1;
            b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = 2; ci.pBindings = b;
            if (vkCreateDescriptorSetLayout(m_dev.device, &ci, nullptr, &m_ssaoBlurSetLayout) != VK_SUCCESS) {
                logError("[rhi] ssao blur set layout failed"); return false;
            }
        }
        // (mesh.frag set 3 layout m_meshAoSetLayout was created in createGraphics so
        //  the mesh pipeline layout could include it; we only ALLOCATE its sets here.)

        // ---- Descriptor pool: per-frame ssao sets + per-frame mesh-ao sets + 1
        //      blur set. Samplers + uniform buffers sized exactly. ----
        {
            const uint32_t nFrames = kFramesInFlight;
            VkDescriptorPoolSize sizes[2]{};
            sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            sizes[0].descriptorCount = nFrames /*ssao depth*/ + nFrames /*mesh ao*/ + 2 /*blur*/;
            sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            sizes[1].descriptorCount = nFrames /*ssao ubo*/ + nFrames /*ctrl ubo*/;
            VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.maxSets = nFrames + nFrames + 1; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
            if (vkCreateDescriptorPool(m_dev.device, &pci, nullptr, &m_ssaoPool) != VK_SUCCESS) {
                logError("[rhi] ssao desc pool failed"); return false;
            }
        }

        // ---- Per-frame SSAO + control UBOs + their descriptor sets. ----
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkBufferCreateInfo ub{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            ub.size = sizeof(SsaoUBO); ub.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info{};
            if (vmaCreateBuffer(m_alloc, &ub, &aci, &m_ssaoUboBuf[i], &m_ssaoUboAlloc[i], &info) != VK_SUCCESS) {
                logError("[rhi] ssao ubo create failed"); return false;
            }
            m_ssaoUboMapped[i] = info.pMappedData;

            VkBufferCreateInfo cb{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            cb.size = sizeof(SsaoControl); cb.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationInfo cinfo{};
            if (vmaCreateBuffer(m_alloc, &cb, &aci, &m_ssaoCtrlBuf[i], &m_ssaoCtrlAlloc[i], &cinfo) != VK_SUCCESS) {
                logError("[rhi] ssao ctrl ubo create failed"); return false;
            }
            m_ssaoCtrlMapped[i] = cinfo.pMappedData;

            VkDescriptorSetAllocateInfo a0{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            a0.descriptorPool = m_ssaoPool; a0.descriptorSetCount = 1; a0.pSetLayouts = &m_ssaoSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &a0, &m_ssaoSet[i]) != VK_SUCCESS) {
                logError("[rhi] ssao set alloc failed"); return false;
            }
            VkDescriptorSetAllocateInfo a1{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            a1.descriptorPool = m_ssaoPool; a1.descriptorSetCount = 1; a1.pSetLayouts = &m_meshAoSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &a1, &m_meshAoSet[i]) != VK_SUCCESS) {
                logError("[rhi] mesh ao set alloc failed"); return false;
            }
        }
        {
            VkDescriptorSetAllocateInfo ab{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ab.descriptorPool = m_ssaoPool; ab.descriptorSetCount = 1; ab.pSetLayouts = &m_ssaoBlurSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &ab, &m_ssaoBlurSet) != VK_SUCCESS) {
                logError("[rhi] ssao blur set alloc failed"); return false;
            }
        }

        // ---- Depth pre-pass pipeline (depth.vert; depth-only, camera viewProj,
        //      set0 = objSet via m_shadowLayout, writes m_depthImg). ----
        if (!createDepthPrePipeline()) return false;

        // ---- SSAO pipeline (ssao.frag -> R8). ----
        {
            VkPushConstantRange pcr{}; // none for ssao; params come from the UBO
            (void)pcr;
            VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pl.setLayoutCount = 1; pl.pSetLayouts = &m_ssaoSetLayout;
            if (vkCreatePipelineLayout(m_dev.device, &pl, nullptr, &m_ssaoLayout) != VK_SUCCESS) {
                logError("[rhi] ssao pipeline layout failed"); return false;
            }
            if (!createFullscreenPipeline("shaders\\fullscreen.vert.spv", "shaders\\ssao.frag.spv",
                                          m_ssaoLayout, kSsaoFormat, /*additiveBlend=*/false, m_ssaoPipe))
                return false;
        }
        // ---- SSAO blur pipeline (ssao_blur.frag -> R8, push constant). ----
        {
            VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SsaoBlurPush) };
            VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pl.setLayoutCount = 1; pl.pSetLayouts = &m_ssaoBlurSetLayout;
            pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr;
            if (vkCreatePipelineLayout(m_dev.device, &pl, nullptr, &m_ssaoBlurLayout) != VK_SUCCESS) {
                logError("[rhi] ssao blur pipeline layout failed"); return false;
            }
            if (!createFullscreenPipeline("shaders\\fullscreen.vert.spv", "shaders\\ssao_blur.frag.spv",
                                          m_ssaoBlurLayout, kSsaoFormat, /*additiveBlend=*/false, m_ssaoBlurPipe))
                return false;
        }

        logInfo("[rhi] SSAO ready (half-res 32-tap hemisphere + depth-aware blur, depth-reconstruction)");
        return true;
    }

    // Depth-only CAMERA pre-pass pipeline (depth.vert): writes the main depth
    // buffer from the camera's POV before lighting so SSAO has a full depth image.
    // Reuses m_shadowLayout (set0 = objSet); renders to m_depthFormat, no color.
    bool createDepthPrePipeline() {
        VkShaderModule vs = loadShaderModule("shaders\\depth.vert.spv");
        if (!vs) return false;
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_VERTEX_BIT; stage.module = vs; stage.pName = "main";

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
        // Same back-face cull + winding as the main mesh pass so the depth values
        // match EXACTLY (the color pass then runs depth-test EQUAL).
        VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_BACK_BIT;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo dss{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        dss.depthTestEnable = VK_TRUE; dss.depthWriteEnable = VK_TRUE;
        dss.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 0; cb.pAttachments = nullptr;
        VkDynamicState dyn[2]{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfo prci{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        prci.colorAttachmentCount = 0;
        prci.depthAttachmentFormat = m_depthFormat;
        VkGraphicsPipelineCreateInfo gpci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gpci.pNext = &prci;
        gpci.stageCount = 1; gpci.pStages = &stage;
        gpci.pVertexInputState = &vin; gpci.pInputAssemblyState = &ia;
        gpci.pViewportState = &vp; gpci.pRasterizationState = &rs;
        gpci.pMultisampleState = &ms; gpci.pDepthStencilState = &dss;
        gpci.pColorBlendState = &cb; gpci.pDynamicState = &ds; gpci.layout = m_shadowLayout;
        VkResult pr = vkCreateGraphicsPipelines(m_dev.device, VK_NULL_HANDLE, 1, &gpci, nullptr, &m_depthPrePipeline);
        vkDestroyShaderModule(m_dev.device, vs, nullptr);
        if (pr != VK_SUCCESS) { logError("[rhi] depth pre-pass pipeline create failed"); return false; }
        return true;
    }

    // Create (or recreate) the half-res SSAO raw + blurred R8 targets at the
    // current frame extent. Called after createBloomTargets() at init + on resize.
    bool createSsaoTargets() {
        destroySsaoTargets();
        m_ssaoExtent = { std::max(1u, m_extent.width / 2), std::max(1u, m_extent.height / 2) };
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (!createColorTarget(kSsaoFormat, m_ssaoExtent.width, m_ssaoExtent.height, usage,
                               m_ssaoRawImg, m_ssaoRawAlloc, m_ssaoRawView)) {
            logError("[rhi] ssao raw target create failed"); return false;
        }
        if (!createColorTarget(kSsaoFormat, m_ssaoExtent.width, m_ssaoExtent.height, usage,
                               m_ssaoBlurImg, m_ssaoBlurAlloc, m_ssaoBlurView)) {
            logError("[rhi] ssao blur target create failed"); return false;
        }
        return true;
    }

    void destroySsaoTargets() {
        if (m_ssaoBlurView) { vkDestroyImageView(m_dev.device, m_ssaoBlurView, nullptr); m_ssaoBlurView = VK_NULL_HANDLE; }
        if (m_ssaoBlurImg)  { vmaDestroyImage(m_alloc, m_ssaoBlurImg, m_ssaoBlurAlloc); m_ssaoBlurImg = VK_NULL_HANDLE; m_ssaoBlurAlloc = nullptr; }
        if (m_ssaoRawView)  { vkDestroyImageView(m_dev.device, m_ssaoRawView, nullptr); m_ssaoRawView = VK_NULL_HANDLE; }
        if (m_ssaoRawImg)   { vmaDestroyImage(m_alloc, m_ssaoRawImg, m_ssaoRawAlloc); m_ssaoRawImg = VK_NULL_HANDLE; m_ssaoRawAlloc = nullptr; }
    }

    // (Re)write the SSAO descriptor sets that reference the depth + AO image views
    // (these change on resize). Called after createSsaoTargets() at init + resize.
    void writeSsaoDescriptors() {
        // ssao.frag set: binding0 = depth (NEAREST), binding1 = per-frame SsaoUBO.
        // The depth image is sampled while in DEPTH_READ_ONLY_OPTIMAL (the graph's
        // SSAO/blur passes transition it there), so the descriptor layout MUST be
        // DEPTH_READ_ONLY_OPTIMAL to satisfy VUID-vkCmdDraw-imageLayout-00344.
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkDescriptorImageInfo di{ m_depthSampler, m_depthView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL };
            VkDescriptorBufferInfo bi{ m_ssaoUboBuf[i], 0, sizeof(SsaoUBO) };
            VkWriteDescriptorSet w[2]{};
            w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[0].dstSet = m_ssaoSet[i]; w[0].dstBinding = 0; w[0].descriptorCount = 1;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &di;
            w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[1].dstSet = m_ssaoSet[i]; w[1].dstBinding = 1; w[1].descriptorCount = 1;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[1].pBufferInfo = &bi;
            vkUpdateDescriptorSets(m_dev.device, 2, w, 0, nullptr);
        }
        // blur set: binding0 = raw AO (linear, SHADER_READ_ONLY), binding1 = depth
        // (NEAREST, DEPTH_READ_ONLY — same layout-match requirement as above).
        {
            VkDescriptorImageInfo da{ m_ssaoLinearSampler, m_ssaoRawView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo dd{ m_depthSampler, m_depthView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet w[2]{};
            w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[0].dstSet = m_ssaoBlurSet; w[0].dstBinding = 0; w[0].descriptorCount = 1;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &da;
            w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[1].dstSet = m_ssaoBlurSet; w[1].dstBinding = 1; w[1].descriptorCount = 1;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo = &dd;
            vkUpdateDescriptorSets(m_dev.device, 2, w, 0, nullptr);
        }
        // mesh.frag set3: binding0 = blurred AO (linear), binding1 = per-frame ctrl.
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkDescriptorImageInfo da{ m_ssaoLinearSampler, m_ssaoBlurView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorBufferInfo bi{ m_ssaoCtrlBuf[i], 0, sizeof(SsaoControl) };
            VkWriteDescriptorSet w[2]{};
            w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[0].dstSet = m_meshAoSet[i]; w[0].dstBinding = 0; w[0].descriptorCount = 1;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &da;
            w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[1].dstSet = m_meshAoSet[i]; w[1].dstBinding = 1; w[1].descriptorCount = 1;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[1].pBufferInfo = &bi;
            vkUpdateDescriptorSets(m_dev.device, 2, w, 0, nullptr);
        }
    }

    void destroySsao() {
        destroySsaoTargets();
        if (m_ssaoBlurPipe)   { vkDestroyPipeline(m_dev.device, m_ssaoBlurPipe, nullptr); m_ssaoBlurPipe = VK_NULL_HANDLE; }
        if (m_ssaoPipe)       { vkDestroyPipeline(m_dev.device, m_ssaoPipe, nullptr); m_ssaoPipe = VK_NULL_HANDLE; }
        if (m_depthPrePipeline){ vkDestroyPipeline(m_dev.device, m_depthPrePipeline, nullptr); m_depthPrePipeline = VK_NULL_HANDLE; }
        if (m_ssaoBlurLayout) { vkDestroyPipelineLayout(m_dev.device, m_ssaoBlurLayout, nullptr); m_ssaoBlurLayout = VK_NULL_HANDLE; }
        if (m_ssaoLayout)     { vkDestroyPipelineLayout(m_dev.device, m_ssaoLayout, nullptr); m_ssaoLayout = VK_NULL_HANDLE; }
        if (m_ssaoPool)       { vkDestroyDescriptorPool(m_dev.device, m_ssaoPool, nullptr); m_ssaoPool = VK_NULL_HANDLE; }
        if (m_meshAoSetLayout){ vkDestroyDescriptorSetLayout(m_dev.device, m_meshAoSetLayout, nullptr); m_meshAoSetLayout = VK_NULL_HANDLE; }
        if (m_ssaoBlurSetLayout){ vkDestroyDescriptorSetLayout(m_dev.device, m_ssaoBlurSetLayout, nullptr); m_ssaoBlurSetLayout = VK_NULL_HANDLE; }
        if (m_ssaoSetLayout)  { vkDestroyDescriptorSetLayout(m_dev.device, m_ssaoSetLayout, nullptr); m_ssaoSetLayout = VK_NULL_HANDLE; }
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (m_ssaoUboBuf[i])  { vmaDestroyBuffer(m_alloc, m_ssaoUboBuf[i], m_ssaoUboAlloc[i]); m_ssaoUboBuf[i] = VK_NULL_HANDLE; }
            if (m_ssaoCtrlBuf[i]) { vmaDestroyBuffer(m_alloc, m_ssaoCtrlBuf[i], m_ssaoCtrlAlloc[i]); m_ssaoCtrlBuf[i] = VK_NULL_HANDLE; }
        }
        if (m_ssaoLinearSampler){ vkDestroySampler(m_dev.device, m_ssaoLinearSampler, nullptr); m_ssaoLinearSampler = VK_NULL_HANDLE; }
        if (m_depthSampler)   { vkDestroySampler(m_dev.device, m_depthSampler, nullptr); m_depthSampler = VK_NULL_HANDLE; }
    }

    // Build the baked hemisphere kernel + 4x4 rotation-noise tables ONCE (CPU,
    // deterministic) and stash them; prepareFrameData copies them into each
    // frame's SSAO UBO. Kernel: cosine-ish hemisphere samples, biased toward the
    // origin so near-surface occlusion dominates (standard SSAO weighting).
    void buildSsaoKernelAndNoise() {
        if (m_ssaoKernelBuilt) return;
        m_ssaoKernelBuilt = true;
        // Deterministic LCG so the look is identical across runs (and clean-room).
        uint32_t s = 0x13572468u;
        auto rnd = [&]() { s = s * 1664525u + 1013904223u; return (float)(s >> 8) / (float)(1u << 24); };
        for (int i = 0; i < kSsaoKernel; ++i) {
            glm::vec3 v(rnd() * 2.0f - 1.0f, rnd() * 2.0f - 1.0f, rnd()); // hemisphere (+z)
            v = glm::normalize(v) * rnd();
            float t = (float)i / (float)kSsaoKernel;
            float scale = 0.1f + 0.9f * t * t;            // accelerate toward 1 (cluster near origin)
            v *= scale;
            m_ssaoKernelCPU[i] = glm::vec4(v, 0.0f);
        }
        for (int i = 0; i < 16; ++i) {
            // Rotation vectors in the XY plane (z=0), uniform in [-1,1].
            m_ssaoNoiseCPU[i] = glm::vec4(rnd() * 2.0f - 1.0f, rnd() * 2.0f - 1.0f, 0.0f, 0.0f);
        }
    }

    // ======================================================================
    // GI — creation / targets / descriptors / teardown.
    // ======================================================================
    // Baked cosine-weighted hemisphere kernel (Malley's method: project a uniform
    // disk to the hemisphere) so the gather is importance-sampled toward the normal
    // — exactly the cosine weighting an irradiance integral wants. Deterministic
    // LCG (clean-room, identical across runs). Plus a 4x4 rotation-noise table.
    void buildGiKernelAndNoise() {
        if (m_giKernelBuilt) return;
        m_giKernelBuilt = true;
        uint32_t s = 0x9E3779B9u;
        auto rnd = [&]() { s = s * 1664525u + 1013904223u; return (float)(s >> 8) / (float)(1u << 24); };
        for (int i = 0; i < kGiKernel; ++i) {
            // Cosine-weighted hemisphere about +z (Malley): r = sqrt(u1), phi = 2pi u2.
            float u1 = rnd(), u2 = rnd();
            float r = std::sqrt(u1);
            float phi = 6.2831853f * u2;
            glm::vec3 v(r * std::cos(phi), r * std::sin(phi), std::sqrt(std::max(0.0f, 1.0f - u1)));
            // Vary the radial reach so taps spread across the hemisphere shell, with
            // a mild bias toward mid-range (broad soft bounce, not just contact).
            float t = (float)i / (float)kGiKernel;
            float scale = 0.25f + 0.75f * t;
            m_giKernelCPU[i] = glm::vec4(v * scale, 0.0f);
        }
        for (int i = 0; i < 16; ++i)
            m_giNoiseCPU[i] = glm::vec4(rnd() * 2.0f - 1.0f, rnd() * 2.0f - 1.0f, 0.0f, 0.0f);
    }

    // Build the GI samplers, descriptor-set layouts, pool + sets, per-frame UBOs,
    // and the four full-screen GI pipelines. Extent-independent (dynamic viewport);
    // the half-res IMAGES + per-frame descriptor writes are (re)done in
    // createGiTargets()/writeGiDescriptors(). Reuses m_depthSampler (NEAREST) for
    // depth and m_ssaoLinearSampler (CLAMP linear) for colour/AO — both created by
    // createSsao(), which runs first.
    bool createGi() {
        // ---- Descriptor set layouts ----
        auto makeLayout = [&](uint32_t nImg, bool hasUbo, VkDescriptorSetLayout& out) -> bool {
            VkDescriptorSetLayoutBinding b[8]{};
            uint32_t n = 0;
            for (uint32_t i = 0; i < nImg; ++i) {
                b[n].binding = n; b[n].descriptorCount = 1;
                b[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                b[n].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; ++n;
            }
            if (hasUbo) {
                b[n].binding = n; b[n].descriptorCount = 1;
                b[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                b[n].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; ++n;
            }
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = n; ci.pBindings = b;
            return vkCreateDescriptorSetLayout(m_dev.device, &ci, nullptr, &out) == VK_SUCCESS;
        };
        // gather: depth + scene + GiUBO ; temporal: cur+hist+depth+prevDepth + UBO ;
        // blur: gi + depth (push const) ; apply: gi + depth + ao (push const).
        if (!makeLayout(2, true,  m_giGatherSetLayout))   { logError("[rhi] gi gather layout failed"); return false; }
        if (!makeLayout(4, true,  m_giTemporalSetLayout)) { logError("[rhi] gi temporal layout failed"); return false; }
        if (!makeLayout(2, false, m_giBlurSetLayout))     { logError("[rhi] gi blur layout failed"); return false; }
        if (!makeLayout(3, false, m_giApplySetLayout))    { logError("[rhi] gi apply layout failed"); return false; }

        // ---- Descriptor pool ----
        {
            const uint32_t nF = kFramesInFlight;
            VkDescriptorPoolSize sizes[2]{};
            sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            sizes[0].descriptorCount = nF * 2 /*gather*/ + nF * 4 /*temporal*/ + nF * 2 /*blur*/ + nF * 3 /*apply*/;
            sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            sizes[1].descriptorCount = nF /*gather ubo*/ + nF /*temporal ubo*/;
            VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.maxSets = nF * 4; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
            if (vkCreateDescriptorPool(m_dev.device, &pci, nullptr, &m_giPool) != VK_SUCCESS) {
                logError("[rhi] gi desc pool failed"); return false;
            }
        }
        // ---- Per-frame UBOs + gather/temporal sets ----
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VkBufferCreateInfo ub{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            ub.size = sizeof(GiUBO); ub.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationInfo info{};
            if (vmaCreateBuffer(m_alloc, &ub, &aci, &m_giUboBuf[i], &m_giUboAlloc[i], &info) != VK_SUCCESS) {
                logError("[rhi] gi ubo create failed"); return false;
            }
            m_giUboMapped[i] = info.pMappedData;
            VkBufferCreateInfo tb{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            tb.size = sizeof(GiTemporalUBO); tb.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationInfo tinfo{};
            if (vmaCreateBuffer(m_alloc, &tb, &aci, &m_giTempUboBuf[i], &m_giTempUboAlloc[i], &tinfo) != VK_SUCCESS) {
                logError("[rhi] gi temporal ubo create failed"); return false;
            }
            m_giTempUboMapped[i] = tinfo.pMappedData;

            VkDescriptorSetAllocateInfo ag{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ag.descriptorPool = m_giPool; ag.descriptorSetCount = 1; ag.pSetLayouts = &m_giGatherSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &ag, &m_giGatherSet[i]) != VK_SUCCESS) {
                logError("[rhi] gi gather set alloc failed"); return false;
            }
            VkDescriptorSetAllocateInfo at{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            at.descriptorPool = m_giPool; at.descriptorSetCount = 1; at.pSetLayouts = &m_giTemporalSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &at, &m_giTemporalSet[i]) != VK_SUCCESS) {
                logError("[rhi] gi temporal set alloc failed"); return false;
            }
            VkDescriptorSetAllocateInfo ab{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ab.descriptorPool = m_giPool; ab.descriptorSetCount = 1; ab.pSetLayouts = &m_giBlurSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &ab, &m_giBlurSet[i]) != VK_SUCCESS) {
                logError("[rhi] gi blur set alloc failed"); return false;
            }
            VkDescriptorSetAllocateInfo aa{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            aa.descriptorPool = m_giPool; aa.descriptorSetCount = 1; aa.pSetLayouts = &m_giApplySetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &aa, &m_giApplySet[i]) != VK_SUCCESS) {
                logError("[rhi] gi apply set alloc failed"); return false;
            }
        }
        // ---- Pipeline layouts ----
        auto makePipeLayout = [&](VkDescriptorSetLayout setL, uint32_t pcSize, VkPipelineLayout& out) -> bool {
            VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, pcSize };
            VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pl.setLayoutCount = 1; pl.pSetLayouts = &setL;
            if (pcSize > 0) { pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr; }
            return vkCreatePipelineLayout(m_dev.device, &pl, nullptr, &out) == VK_SUCCESS;
        };
        if (!makePipeLayout(m_giGatherSetLayout,   0, m_giGatherLayout))   { logError("[rhi] gi gather pl failed"); return false; }
        if (!makePipeLayout(m_giTemporalSetLayout, 0, m_giTemporalLayout)) { logError("[rhi] gi temporal pl failed"); return false; }
        if (!makePipeLayout(m_giBlurSetLayout,  sizeof(GiBlurPush),  m_giBlurLayout))  { logError("[rhi] gi blur pl failed"); return false; }
        if (!makePipeLayout(m_giApplySetLayout, sizeof(GiApplyPush), m_giApplyLayout)) { logError("[rhi] gi apply pl failed"); return false; }

        // ---- Pipelines (full-screen triangle). Apply uses ADDITIVE blend into the
        //      HDR target; the rest write their own half-res buffers (no blend). ----
        if (!createFullscreenPipeline("shaders\\fullscreen.vert.spv", "shaders\\ssgi_gather.frag.spv",
                                      m_giGatherLayout, kGiFormat, /*additiveBlend=*/false, m_giGatherPipe)) return false;
        if (!createFullscreenPipeline("shaders\\fullscreen.vert.spv", "shaders\\ssgi_temporal.frag.spv",
                                      m_giTemporalLayout, kGiFormat, /*additiveBlend=*/false, m_giTemporalPipe)) return false;
        if (!createFullscreenPipeline("shaders\\fullscreen.vert.spv", "shaders\\ssgi_blur.frag.spv",
                                      m_giBlurLayout, kGiFormat, /*additiveBlend=*/false, m_giBlurPipe)) return false;
        if (!createFullscreenPipeline("shaders\\fullscreen.vert.spv", "shaders\\ssgi_apply.frag.spv",
                                      m_giApplyLayout, kHdrFormat, /*additiveBlend=*/true, m_giApplyPipe)) return false;
        return true;
    }

    // Create (or recreate) the half-res GI targets + the full-res prev-depth copy
    // at the current frame extent. Called after createGi() at init + on resize.
    bool createGiTargets() {
        destroyGiTargets();
        m_giExtent = { std::max(1u, m_extent.width / 2), std::max(1u, m_extent.height / 2) };
        const VkImageUsageFlags use = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (!createColorTarget(kGiFormat, m_giExtent.width, m_giExtent.height, use,
                               m_giRawImg, m_giRawAlloc, m_giRawView)) { logError("[rhi] gi raw target failed"); return false; }
        for (int i = 0; i < 2; ++i)
            if (!createColorTarget(kGiFormat, m_giExtent.width, m_giExtent.height, use,
                                   m_giAccumImg[i], m_giAccumAlloc[i], m_giAccumView[i])) { logError("[rhi] gi accum target failed"); return false; }
        if (!createColorTarget(kGiFormat, m_giExtent.width, m_giExtent.height, use,
                               m_giDenoiseImg, m_giDenoiseAlloc, m_giDenoiseView)) { logError("[rhi] gi denoise target failed"); return false; }
        // Prev-depth: a full-res copy of the depth buffer (TRANSFER_DST + SAMPLED).
        // Same format as the main depth so vkCmdCopyImage is a straight blit.
        VkImageCreateInfo dici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        dici.imageType = VK_IMAGE_TYPE_2D; dici.format = m_depthFormat;
        dici.extent = { m_extent.width, m_extent.height, 1 }; dici.mipLevels = 1; dici.arrayLayers = 1;
        dici.samples = VK_SAMPLE_COUNT_1_BIT; dici.tiling = VK_IMAGE_TILING_OPTIMAL;
        dici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        dici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo daci{}; daci.usage = VMA_MEMORY_USAGE_AUTO;
        daci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        if (vmaCreateImage(m_alloc, &dici, &daci, &m_giPrevDepthImg, &m_giPrevDepthAlloc, nullptr) != VK_SUCCESS) {
            logError("[rhi] gi prev-depth image failed"); return false;
        }
        VkImageViewCreateInfo dvci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        dvci.image = m_giPrevDepthImg; dvci.viewType = VK_IMAGE_VIEW_TYPE_2D; dvci.format = m_depthFormat;
        dvci.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(m_dev.device, &dvci, nullptr, &m_giPrevDepthView) != VK_SUCCESS) {
            logError("[rhi] gi prev-depth view failed"); return false;
        }
        m_giHistoryValid = false;   // no usable history until the second frame
        m_giAccumWrite = 0;
        return true;
    }

    void destroyGiTargets() {
        auto killImg = [&](VkImage& im, VmaAllocation& al, VkImageView& v) {
            if (v)  { vkDestroyImageView(m_dev.device, v, nullptr); v = VK_NULL_HANDLE; }
            if (im) { vmaDestroyImage(m_alloc, im, al); im = VK_NULL_HANDLE; al = nullptr; }
        };
        killImg(m_giPrevDepthImg, m_giPrevDepthAlloc, m_giPrevDepthView);
        killImg(m_giDenoiseImg, m_giDenoiseAlloc, m_giDenoiseView);
        killImg(m_giAccumImg[0], m_giAccumAlloc[0], m_giAccumView[0]);
        killImg(m_giAccumImg[1], m_giAccumAlloc[1], m_giAccumView[1]);
        killImg(m_giRawImg, m_giRawAlloc, m_giRawView);
    }

    // (Re)write the GI descriptor sets to point at the current image views. The
    // gather set (depth + scene + UBO) is stable per resize; the temporal/blur/
    // apply sets reference ping-pong accum views and are rewritten each frame in
    // prepareFrameData (cheap). Here we set the per-resize-stable bindings + the
    // gather/temporal UBOs. Sampler reuse: m_depthSampler (NEAREST), m_ssaoLinearSampler (LINEAR).
    void writeGiDescriptors() {
        // Gather: 0=depth(NEAREST), 1=scene(LINEAR), 2=GiUBO. Stable per resize.
        // Depth is sampled in DEPTH_READ_ONLY_OPTIMAL (the layout the graph leaves it
        // in for the GI passes); colour images use SHADER_READ_ONLY_OPTIMAL.
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkDescriptorImageInfo d{ m_depthSampler, m_depthView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo s{ m_ssaoLinearSampler, m_hdrView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorBufferInfo u{ m_giUboBuf[i], 0, sizeof(GiUBO) };
            VkWriteDescriptorSet w[3]{};
            w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = m_giGatherSet[i];
            w[0].dstBinding = 0; w[0].descriptorCount = 1;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &d;
            w[1] = w[0]; w[1].dstBinding = 1; w[1].pImageInfo = &s;
            w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[2].dstSet = m_giGatherSet[i];
            w[2].dstBinding = 2; w[2].descriptorCount = 1;
            w[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[2].pBufferInfo = &u;
            vkUpdateDescriptorSets(m_dev.device, 3, w, 0, nullptr);
        }
        // The temporal/blur/apply sets are written per-frame (ping-pong) by
        // writeGiFrameDescriptors(); nothing stable to do here for them.
    }

    // Per-frame: point the temporal/blur/apply descriptor sets at the right
    // ping-pong accum buffers for this frame. `writeIdx` = accum we write this
    // frame, `histIdx` = accum we read as history. Cheap vkUpdateDescriptorSets;
    // no allocation. Called from prepareFrameData after m_giAccumWrite is chosen.
    void writeGiFrameDescriptors(uint32_t writeIdx, uint32_t histIdx, VkImageView aoView) {
        const VkImageLayout RO  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;  // colour
        const VkImageLayout DRO = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;   // depth
        // Temporal set (this frame's m_frameIdx): 0=cur(raw), 1=hist(accum[hist]),
        // 2=depth, 3=prevDepth, 4=GiTemporalUBO.
        {
            VkDescriptorImageInfo cur { m_ssaoLinearSampler, m_giRawView,          RO  };
            VkDescriptorImageInfo hist{ m_ssaoLinearSampler, m_giAccumView[histIdx], RO };
            VkDescriptorImageInfo dep { m_depthSampler,      m_depthView,          DRO };
            VkDescriptorImageInfo pdep{ m_depthSampler,      m_giPrevDepthView,    DRO };
            VkDescriptorBufferInfo ub { m_giTempUboBuf[m_frameIdx], 0, sizeof(GiTemporalUBO) };
            VkWriteDescriptorSet w[5]{};
            for (int k = 0; k < 4; ++k) {
                w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[k].dstSet = m_giTemporalSet[m_frameIdx];
                w[k].dstBinding = (uint32_t)k; w[k].descriptorCount = 1;
                w[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            }
            w[0].pImageInfo = &cur; w[1].pImageInfo = &hist; w[2].pImageInfo = &dep; w[3].pImageInfo = &pdep;
            w[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[4].dstSet = m_giTemporalSet[m_frameIdx];
            w[4].dstBinding = 4; w[4].descriptorCount = 1;
            w[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[4].pBufferInfo = &ub;
            vkUpdateDescriptorSets(m_dev.device, 5, w, 0, nullptr);
        }
        // Blur set: 0 = accum[writeIdx] (the temporal output), 1 = depth.
        {
            VkDescriptorImageInfo gi { m_ssaoLinearSampler, m_giAccumView[writeIdx], RO  };
            VkDescriptorImageInfo dep{ m_depthSampler,      m_depthView,            DRO };
            VkWriteDescriptorSet w[2]{};
            w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = m_giBlurSet[m_frameIdx];
            w[0].dstBinding = 0; w[0].descriptorCount = 1;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &gi;
            w[1] = w[0]; w[1].dstBinding = 1; w[1].pImageInfo = &dep;
            vkUpdateDescriptorSets(m_dev.device, 2, w, 0, nullptr);
        }
        // Apply set: 0 = denoised GI, 1 = depth, 2 = AO (the blurred SSAO when on,
        // otherwise a harmless valid image — apply forces aoAmount=0 in that case).
        {
            VkDescriptorImageInfo gi { m_ssaoLinearSampler, m_giDenoiseView, RO  };
            VkDescriptorImageInfo dep{ m_depthSampler,      m_depthView,     DRO };
            VkDescriptorImageInfo ao { m_ssaoLinearSampler, aoView,          RO  };
            VkWriteDescriptorSet w[3]{};
            w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = m_giApplySet[m_frameIdx];
            w[0].dstBinding = 0; w[0].descriptorCount = 1;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &gi;
            w[1] = w[0]; w[1].dstBinding = 1; w[1].pImageInfo = &dep;
            w[2] = w[0]; w[2].dstBinding = 2; w[2].pImageInfo = &ao;
            vkUpdateDescriptorSets(m_dev.device, 3, w, 0, nullptr);
        }
    }

    void destroyGi() {
        destroyGiTargets();
        auto killPipe = [&](VkPipeline& p){ if (p) { vkDestroyPipeline(m_dev.device, p, nullptr); p = VK_NULL_HANDLE; } };
        killPipe(m_giApplyPipe); killPipe(m_giBlurPipe); killPipe(m_giTemporalPipe); killPipe(m_giGatherPipe);
        auto killPL = [&](VkPipelineLayout& l){ if (l) { vkDestroyPipelineLayout(m_dev.device, l, nullptr); l = VK_NULL_HANDLE; } };
        killPL(m_giApplyLayout); killPL(m_giBlurLayout); killPL(m_giTemporalLayout); killPL(m_giGatherLayout);
        if (m_giPool) { vkDestroyDescriptorPool(m_dev.device, m_giPool, nullptr); m_giPool = VK_NULL_HANDLE; }
        auto killSL = [&](VkDescriptorSetLayout& l){ if (l) { vkDestroyDescriptorSetLayout(m_dev.device, l, nullptr); l = VK_NULL_HANDLE; } };
        killSL(m_giApplySetLayout); killSL(m_giBlurSetLayout); killSL(m_giTemporalSetLayout); killSL(m_giGatherSetLayout);
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (m_giUboBuf[i])     { vmaDestroyBuffer(m_alloc, m_giUboBuf[i], m_giUboAlloc[i]); m_giUboBuf[i] = VK_NULL_HANDLE; }
            if (m_giTempUboBuf[i]) { vmaDestroyBuffer(m_alloc, m_giTempUboBuf[i], m_giTempUboAlloc[i]); m_giTempUboBuf[i] = VK_NULL_HANDLE; }
        }
    }

    bool createPerFrame() {
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = m_gfxFamily;
        VkSemaphoreCreateInfo si{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkFenceCreateInfo fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        // ---- GPU timestamp support (perf instrumentation) ----
        // timestampPeriod (ns/tick) is a device limit; a graphics queue family with
        // timestampValidBits>0 can write timestamps. If unsupported we skip the
        // query pools entirely and gpuFrameMs stays 0 (CPU stats still work).
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_dev.physical_device, &props);
        m_tsPeriodNs = props.limits.timestampPeriod;
        uint32_t qfCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(m_dev.physical_device, &qfCount, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(qfCount);
        vkGetPhysicalDeviceQueueFamilyProperties(m_dev.physical_device, &qfCount, qfs.data());
        uint32_t validBits = (m_gfxFamily < qfCount) ? qfs[m_gfxFamily].timestampValidBits : 0;
        m_tsSupported = (m_tsPeriodNs > 0.0f) && (validBits > 0);
        m_tsValidMask = (validBits >= 64) ? ~0ull : ((1ull << validBits) - 1ull);

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

            // 2 timestamps per frame (pass begin + pass end).
            if (m_tsSupported) {
                VkQueryPoolCreateInfo qci{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
                qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
                qci.queryCount = 2;
                if (vkCreateQueryPool(m_dev.device, &qci, nullptr, &fr.tsPool) != VK_SUCCESS) {
                    logError("[rhi] timestamp query pool create failed; GPU timing disabled");
                    m_tsSupported = false; // fall back: CPU stats only
                }
            }
        }
        if (m_tsSupported)
            logInfo("[rhi] GPU timestamp queries enabled (timestampPeriod=" +
                    std::to_string(m_tsPeriodNs) + " ns/tick)");
        return true;
    }

    void destroyPerFrame() {
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            auto& fr = m_frames[i];
            if (fr.tsPool) vkDestroyQueryPool(m_dev.device, fr.tsPool, nullptr);
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

        // ====================================================================
        // BINDLESS set (set 0): one large COMBINED_IMAGE_SAMPLER array. Flags make
        // it partially-bound (only created slots written) + update-after-bind (we
        // write slots lazily at createTexture, even after the set is bound). The
        // pool carries UPDATE_AFTER_BIND_BIT to match.
        // ====================================================================
        {
            VkDescriptorSetLayoutBinding b{};
            b.binding = 0;
            b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b.descriptorCount = kMaxTextures;
            b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            VkDescriptorBindingFlags bf =
                VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
            VkDescriptorSetLayoutBindingFlagsCreateInfo bfci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
            bfci.bindingCount = 1; bfci.pBindingFlags = &bf;

            VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            slci.pNext = &bfci;
            slci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
            slci.bindingCount = 1; slci.pBindings = &b;
            if (vkCreateDescriptorSetLayout(m_dev.device, &slci, nullptr, &m_bindlessLayout) != VK_SUCCESS) {
                logError("[rhi] bindless set layout failed"); return false;
            }

            VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures };
            VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
            pci.maxSets = 1; pci.poolSizeCount = 1; pci.pPoolSizes = &ps;
            if (vkCreateDescriptorPool(m_dev.device, &pci, nullptr, &m_bindlessPool) != VK_SUCCESS) {
                logError("[rhi] bindless pool failed"); return false;
            }
            VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dsai.descriptorPool = m_bindlessPool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &m_bindlessLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &dsai, &m_bindlessSet) != VK_SUCCESS) {
                logError("[rhi] bindless set alloc failed"); return false;
            }
        }

        // ====================================================================
        // Object/camera set (set 1): SSBO (binding 0) + camera UBO (binding 1),
        // one set per frame pointing at that frame's persistent-mapped rings.
        // ====================================================================
        {
            VkDescriptorSetLayoutBinding binds[2]{};
            binds[0].binding = 0; binds[0].descriptorCount = 1;
            binds[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            binds[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            binds[1].binding = 1; binds[1].descriptorCount = 1;
            binds[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            // VERTEX: camera viewProj (mesh.vert) + lightViewProj (shadow.vert).
            // FRAGMENT: lightViewProj for the per-pixel shadow projection (mesh.frag).
            binds[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            slci.bindingCount = 2; slci.pBindings = binds;
            if (vkCreateDescriptorSetLayout(m_dev.device, &slci, nullptr, &m_objSetLayout) != VK_SUCCESS) {
                logError("[rhi] object set layout failed"); return false;
            }

            VkDescriptorPoolSize sizes[2]{};
            sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; sizes[0].descriptorCount = kFramesInFlight;
            sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; sizes[1].descriptorCount = kFramesInFlight;
            VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.maxSets = kFramesInFlight; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
            if (vkCreateDescriptorPool(m_dev.device, &pci, nullptr, &m_objPool) != VK_SUCCESS) {
                logError("[rhi] object pool failed"); return false;
            }

            for (uint32_t i = 0; i < kFramesInFlight; ++i) {
                auto& fr = m_frames[i];
                // Object SSBO ring.
                VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                bci.size = (VkDeviceSize)kMaxDrawsPerFrame * sizeof(ObjectData);
                bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                VmaAllocationCreateInfo aci{};
                aci.usage = VMA_MEMORY_USAGE_AUTO;
                aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
                VmaAllocationInfo info{};
                if (vmaCreateBuffer(m_alloc, &bci, &aci, &fr.objBuf, &fr.objAlloc, &info) != VK_SUCCESS) {
                    logError("[rhi] object SSBO create failed"); return false;
                }
                fr.objMapped = info.pMappedData;

                // Frame UBO (camera viewProj + sun lightViewProj + point lights).
                VkBufferCreateInfo cbci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                cbci.size = sizeof(FrameUBO);
                cbci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
                VmaAllocationInfo cinfo{};
                if (vmaCreateBuffer(m_alloc, &cbci, &aci, &fr.camBuf, &fr.camAlloc, &cinfo) != VK_SUCCESS) {
                    logError("[rhi] camera UBO create failed"); return false;
                }
                fr.camMapped = cinfo.pMappedData;

                // Indirect-command buffer (one VkDrawIndexedIndirectCommand per
                // distinct mesh; capped at kMaxTextures meshes which is plenty).
                VkBufferCreateInfo ibci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                ibci.size = (VkDeviceSize)kMaxDrawMeshes * sizeof(VkDrawIndexedIndirectCommand);
                ibci.usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
                VmaAllocationInfo iinfo{};
                if (vmaCreateBuffer(m_alloc, &ibci, &aci, &fr.indirectBuf, &fr.indirectAlloc, &iinfo) != VK_SUCCESS) {
                    logError("[rhi] indirect buffer create failed"); return false;
                }
                fr.indirectMapped = iinfo.pMappedData;

                // Allocate + write the set-1 descriptor (points at this frame's
                // SSBO + camera UBO; written once, buffers are persistent-mapped).
                VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
                dsai.descriptorPool = m_objPool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &m_objSetLayout;
                if (vkAllocateDescriptorSets(m_dev.device, &dsai, &fr.objSet) != VK_SUCCESS) {
                    logError("[rhi] object set alloc failed"); return false;
                }
                VkDescriptorBufferInfo sbi{ fr.objBuf, 0, VK_WHOLE_SIZE };
                VkDescriptorBufferInfo cbi{ fr.camBuf, 0, sizeof(FrameUBO) };
                VkWriteDescriptorSet writes[2]{};
                writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[0].dstSet = fr.objSet; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
                writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[0].pBufferInfo = &sbi;
                writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[1].dstSet = fr.objSet; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
                writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; writes[1].pBufferInfo = &cbi;
                vkUpdateDescriptorSets(m_dev.device, 2, writes, 0, nullptr);
            }
        }

        // ---- Built-in 1x1 white default texture (sRGB) -> bindless slot 0 ----
        const uint8_t white[4] = { 255, 255, 255, 255 };
        if (!createSampledTexture(white, 1, 1, true, m_whiteTex)) {
            logError("[rhi] default white texture create failed"); return false;
        }
        if (!registerBindless(m_whiteTex) || m_whiteTex.bindlessIndex != 0) {
            logError("[rhi] white default must occupy bindless slot 0"); return false;
        }

        // ---- mesh.frag SSAO set (set 3): AO sampler + SsaoControl UBO. Created
        // here (only needs the device) so the mesh pipeline layout can include it;
        // the rest of the SSAO objects are built later in createSsao(). ----
        {
            VkDescriptorSetLayoutBinding b[2]{};
            b[0].binding = 0; b[0].descriptorCount = 1;
            b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            b[1].binding = 1; b[1].descriptorCount = 1;
            b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = 2; ci.pBindings = b;
            if (vkCreateDescriptorSetLayout(m_dev.device, &ci, nullptr, &m_meshAoSetLayout) != VK_SUCCESS) {
                logError("[rhi] mesh ao set layout failed"); return false;
            }
        }

        // ---- Mesh pipeline (bindless texture + per-object SSBO + indirect) ----
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

        // Depth: the SSAO depth pre-pass already wrote the EXACT camera depth, so
        // the main pass tests EQUAL with depth-write OFF (no double depth write, no
        // z-fight — same geometry, same transform). This also lets SSAO read a
        // complete depth buffer before lighting.
        VkPipelineDepthStencilStateCreateInfo dss{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        dss.depthTestEnable = VK_TRUE; dss.depthWriteEnable = VK_FALSE;
        dss.depthCompareOp = VK_COMPARE_OP_EQUAL;

        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 1; cb.pAttachments = &cba;

        VkDynamicState dyn[2]{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;

        // GPU-driven layout: set 0 = bindless textures, set 1 = object SSBO +
        // camera UBO, set 2 = the shadow map (perf-stack E), set 3 = the SSAO AO
        // texture + control UBO. NO push constants (per-object data is in the SSBO).
        VkDescriptorSetLayout setLayouts[4] = { m_bindlessLayout, m_objSetLayout, m_shadowSetLayout, m_meshAoSetLayout };
        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount = 4; plci.pSetLayouts = setLayouts;
        if (vkCreatePipelineLayout(m_dev.device, &plci, nullptr, &m_meshLayout) != VK_SUCCESS) {
            logError("[rhi] pipeline layout failed"); return false;
        }

        // HDR pipeline: the mesh pass now renders into the R16G16B16A16_SFLOAT
        // linear HDR scene target (NOT the LDR swapchain). Tonemap moved to the
        // composite pass. The depth attachment format is unchanged.
        const VkFormat hdrFmt = kHdrFormat;
        VkPipelineRenderingCreateInfo prci{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        prci.colorAttachmentCount = 1; prci.pColorAttachmentFormats = &hdrFmt;
        prci.depthAttachmentFormat = m_depthFormat;

        VkGraphicsPipelineCreateInfo gpci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gpci.pNext = &prci;
        gpci.stageCount = 2; gpci.pStages = stages;
        gpci.pVertexInputState = &vin; gpci.pInputAssemblyState = &ia;
        gpci.pViewportState = &vp; gpci.pRasterizationState = &rs;
        gpci.pMultisampleState = &ms; gpci.pDepthStencilState = &dss;
        gpci.pColorBlendState = &cb; gpci.pDynamicState = &ds; gpci.layout = m_meshLayout;
        // SSAO path pipeline: depth-test EQUAL, depth-write OFF (the depth pre-pass
        // already wrote depth). Created with `dss` set above.
        VkResult pr = vkCreateGraphicsPipelines(m_dev.device, VK_NULL_HANDLE, 1, &gpci, nullptr, &m_meshPipeline);
        if (pr != VK_SUCCESS) {
            vkDestroyShaderModule(m_dev.device, vs, nullptr);
            vkDestroyShaderModule(m_dev.device, fs, nullptr);
            logError("[rhi] graphics pipeline create failed"); return false;
        }

        // No-SSAO path pipeline: the ORIGINAL behavior (no depth pre-pass) — depth
        // test LESS + depth write ON, so the main pass clears + writes depth itself.
        // Selected at draw time when m_ssao.enabled is false (true zero SSAO cost).
        VkPipelineDepthStencilStateCreateInfo dssNo = dss;
        dssNo.depthWriteEnable = VK_TRUE;
        dssNo.depthCompareOp   = VK_COMPARE_OP_LESS;
        gpci.pDepthStencilState = &dssNo;
        pr = vkCreateGraphicsPipelines(m_dev.device, VK_NULL_HANDLE, 1, &gpci, nullptr, &m_meshPipelineNoSsao);

        vkDestroyShaderModule(m_dev.device, vs, nullptr);
        vkDestroyShaderModule(m_dev.device, fs, nullptr);
        if (pr != VK_SUCCESS) { logError("[rhi] no-ssao graphics pipeline create failed"); return false; }

        logInfo("[rhi] GPU-driven mesh pipeline ready (bindless textures + per-object SSBO + multidraw-indirect)");

        // Now that m_objSetLayout exists, build the depth-only shadow pipeline.
        if (!createShadowPipeline()) return false;
        return true;
    }

    // ---- Directional shadow mapping (perf-stack E) -------------------------
    // Create the shadow depth texture (+ view + compare sampler) and the set-2
    // descriptor (sampler2DShadow) the mesh fragment shader reads. Resolution is
    // fixed + swapchain-independent, so this is created once at init.
    bool createShadowImage() {
        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = m_shadowFormat;
        ici.extent = { kShadowDim, kShadowDim, 1 };
        ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        if (vmaCreateImage(m_alloc, &ici, &aci, &m_shadowImg, &m_shadowAlloc, nullptr) != VK_SUCCESS) {
            logError("[rhi] shadow image create failed"); return false;
        }
        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = m_shadowImg; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = m_shadowFormat;
        vci.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(m_dev.device, &vci, nullptr, &m_shadowView) != VK_SUCCESS) {
            logError("[rhi] shadow view create failed"); return false;
        }

        // Compare-enabled sampler: hardware PCF. LESS_OR_EQUAL means texture()
        // returns 1 where refDepth <= storedDepth (lit). CLAMP_TO_EDGE + a white
        // border avoids spurious shadowing at the map edges.
        VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; // outside == lit
        sci.compareEnable = VK_TRUE;
        sci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        sci.maxLod = 0.0f;
        if (vkCreateSampler(m_dev.device, &sci, nullptr, &m_shadowSampler) != VK_SUCCESS) {
            logError("[rhi] shadow sampler create failed"); return false;
        }

        // Set-2 layout: a single combined-image-sampler (sampler2DShadow) in frag.
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0; b.descriptorCount = 1;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        slci.bindingCount = 1; slci.pBindings = &b;
        if (vkCreateDescriptorSetLayout(m_dev.device, &slci, nullptr, &m_shadowSetLayout) != VK_SUCCESS) {
            logError("[rhi] shadow set layout failed"); return false;
        }
        VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
        VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pci.maxSets = 1; pci.poolSizeCount = 1; pci.pPoolSizes = &ps;
        if (vkCreateDescriptorPool(m_dev.device, &pci, nullptr, &m_shadowDescPool) != VK_SUCCESS) {
            logError("[rhi] shadow desc pool failed"); return false;
        }
        VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dsai.descriptorPool = m_shadowDescPool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &m_shadowSetLayout;
        if (vkAllocateDescriptorSets(m_dev.device, &dsai, &m_shadowSet) != VK_SUCCESS) {
            logError("[rhi] shadow set alloc failed"); return false;
        }
        // The shadow map's sampled layout is DEPTH_READ_ONLY_OPTIMAL (it's never a
        // color/general image); write the descriptor once with that layout. The
        // per-frame barrier leaves the image in exactly this layout before the
        // main pass samples it.
        VkDescriptorImageInfo dii{};
        dii.sampler = m_shadowSampler;
        dii.imageView = m_shadowView;
        dii.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = m_shadowSet; w.dstBinding = 0; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &dii;
        vkUpdateDescriptorSets(m_dev.device, 1, &w, 0, nullptr);
        return true;
    }

    // Depth-only pipeline for the shadow pass: shadow.vert (lightViewProj*model),
    // no fragment shader, no color attachment, depth write/test LESS. set 0 =
    // the object SSBO + camera UBO (shadow.vert reads model rows + lightViewProj).
    // A small rasterizer depth bias supplements the shader's slope-scaled bias.
    bool createShadowPipeline() {
        VkShaderModule vs = loadShaderModule("shaders\\shadow.vert.spv");
        if (!vs) return false;

        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_VERTEX_BIT; stage.module = vs; stage.pName = "main";

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

        // Front-face cull renders back faces into the shadow map: the depth values
        // come from surfaces facing AWAY from the light, which pushes self-shadow
        // acne behind the lit geometry (a standard, robust acne mitigation). A
        // constant + slope depth bias supplements it.
        VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_FRONT_BIT;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        rs.depthBiasEnable = VK_TRUE;
        rs.depthBiasConstantFactor = 1.25f;
        rs.depthBiasSlopeFactor = 1.75f;

        VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo dss{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        dss.depthTestEnable = VK_TRUE; dss.depthWriteEnable = VK_TRUE;
        dss.depthCompareOp = VK_COMPARE_OP_LESS;

        // No color attachment in the shadow pass.
        VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 0; cb.pAttachments = nullptr;

        VkDynamicState dyn[2]{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;

        // set 0 = the object SSBO + camera UBO (shadow.vert reads both).
        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount = 1; plci.pSetLayouts = &m_objSetLayout;
        if (vkCreatePipelineLayout(m_dev.device, &plci, nullptr, &m_shadowLayout) != VK_SUCCESS) {
            logError("[rhi] shadow pipeline layout failed"); vkDestroyShaderModule(m_dev.device, vs, nullptr); return false;
        }

        // Dynamic rendering: depth-only (no color formats).
        VkPipelineRenderingCreateInfo prci{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        prci.colorAttachmentCount = 0;
        prci.depthAttachmentFormat = m_shadowFormat;

        VkGraphicsPipelineCreateInfo gpci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gpci.pNext = &prci;
        gpci.stageCount = 1; gpci.pStages = &stage;
        gpci.pVertexInputState = &vin; gpci.pInputAssemblyState = &ia;
        gpci.pViewportState = &vp; gpci.pRasterizationState = &rs;
        gpci.pMultisampleState = &ms; gpci.pDepthStencilState = &dss;
        gpci.pColorBlendState = &cb; gpci.pDynamicState = &ds; gpci.layout = m_shadowLayout;
        VkResult pr = vkCreateGraphicsPipelines(m_dev.device, VK_NULL_HANDLE, 1, &gpci, nullptr, &m_shadowPipeline);
        vkDestroyShaderModule(m_dev.device, vs, nullptr);
        if (pr != VK_SUCCESS) { logError("[rhi] shadow pipeline create failed"); return false; }

        logInfo("[rhi] directional shadow pipeline ready (2048^2 depth, depth-only, PCF compare sampler)");
        return true;
    }

    // Write one bindless array slot to point at `tex` (combined image+sampler).
    // update-after-bind means this is legal even while the set is bound: it only
    // rewrites the descriptor the NEXT frame reads (createTexture happens at load /
    // between frames; destroyTexture repoints the slot to white immediately, then
    // DEFERS the old image/view destruction until the in-flight frames retire).
    void writeBindlessSlot(uint32_t slot, const Texture& tex) {
        VkDescriptorImageInfo dii{};
        dii.sampler = tex.sampler;
        dii.imageView = tex.view;
        dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = m_bindlessSet; w.dstBinding = 0; w.dstArrayElement = slot;
        w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &dii;
        vkUpdateDescriptorSets(m_dev.device, 1, &w, 0, nullptr);
    }

    // Assign `tex` the next free bindless slot + write the descriptor. Returns
    // false (and leaves bindlessIndex 0) if the array is full.
    bool registerBindless(Texture& tex) {
        if (m_nextBindless >= kMaxTextures) return false;
        tex.bindlessIndex = m_nextBindless++;
        writeBindlessSlot(tex.bindlessIndex, tex);
        return true;
    }

    void destroyGraphics() {
        // Mesh + texture registries (created by the app via the public API).
        for (auto& kv : m_meshes) {
            Mesh& m = kv.second;
            if (m.dynamic) {
                for (uint32_t i = 0; i < kFramesInFlight; ++i)
                    if (m.dynVbo[i]) vmaDestroyBuffer(m_alloc, m.dynVbo[i], m.dynVboAlloc[i]);
            } else if (m.vbo) {
                vmaDestroyBuffer(m_alloc, m.vbo, m.vboAlloc);
            }
            vmaDestroyBuffer(m_alloc, m.ibo, m.iboAlloc);
        }
        m_meshes.clear();
        // Drain any still-pending deferred frees (buffers AND images/views/samplers
        // queued by destroyMesh/destroyTexture). shutdown() waited idle first, so
        // every referencing frame has retired and it is safe to free immediately.
        flushPendingFrees();
        // Free the persistent capture readback buffer (fix 1).
        if (m_captureBuf) {
            vmaDestroyBuffer(m_alloc, m_captureBuf, m_captureAlloc);
            m_captureBuf = VK_NULL_HANDLE; m_captureAlloc = nullptr; m_captureMapped = nullptr;
        }
        for (auto& kv : m_textures) destroyTextureObj(kv.second);
        m_textures.clear();
        destroyTextureObj(m_whiteTex);

        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            auto& fr = m_frames[i];
            if (fr.objBuf)      { vmaDestroyBuffer(m_alloc, fr.objBuf, fr.objAlloc); fr.objBuf = VK_NULL_HANDLE; fr.objAlloc = nullptr; fr.objMapped = nullptr; }
            if (fr.camBuf)      { vmaDestroyBuffer(m_alloc, fr.camBuf, fr.camAlloc); fr.camBuf = VK_NULL_HANDLE; fr.camAlloc = nullptr; fr.camMapped = nullptr; }
            if (fr.indirectBuf) { vmaDestroyBuffer(m_alloc, fr.indirectBuf, fr.indirectAlloc); fr.indirectBuf = VK_NULL_HANDLE; fr.indirectAlloc = nullptr; fr.indirectMapped = nullptr; }
        }

        if (m_objPool)        { vkDestroyDescriptorPool(m_dev.device, m_objPool, nullptr); m_objPool = VK_NULL_HANDLE; }
        if (m_objSetLayout)   { vkDestroyDescriptorSetLayout(m_dev.device, m_objSetLayout, nullptr); m_objSetLayout = VK_NULL_HANDLE; }
        if (m_bindlessPool)   { vkDestroyDescriptorPool(m_dev.device, m_bindlessPool, nullptr); m_bindlessPool = VK_NULL_HANDLE; }
        if (m_bindlessLayout) { vkDestroyDescriptorSetLayout(m_dev.device, m_bindlessLayout, nullptr); m_bindlessLayout = VK_NULL_HANDLE; }

        // Shadow mapping resources (perf-stack E).
        if (m_shadowPipeline)  { vkDestroyPipeline(m_dev.device, m_shadowPipeline, nullptr); m_shadowPipeline = VK_NULL_HANDLE; }
        if (m_shadowLayout)    { vkDestroyPipelineLayout(m_dev.device, m_shadowLayout, nullptr); m_shadowLayout = VK_NULL_HANDLE; }
        if (m_shadowDescPool)  { vkDestroyDescriptorPool(m_dev.device, m_shadowDescPool, nullptr); m_shadowDescPool = VK_NULL_HANDLE; }
        if (m_shadowSetLayout) { vkDestroyDescriptorSetLayout(m_dev.device, m_shadowSetLayout, nullptr); m_shadowSetLayout = VK_NULL_HANDLE; }
        if (m_shadowSampler)   { vkDestroySampler(m_dev.device, m_shadowSampler, nullptr); m_shadowSampler = VK_NULL_HANDLE; }
        if (m_shadowView)      { vkDestroyImageView(m_dev.device, m_shadowView, nullptr); m_shadowView = VK_NULL_HANDLE; }
        if (m_shadowImg)       { vmaDestroyImage(m_alloc, m_shadowImg, m_shadowAlloc); m_shadowImg = VK_NULL_HANDLE; m_shadowAlloc = nullptr; }

        if (m_meshPipeline)  vkDestroyPipeline(m_dev.device, m_meshPipeline, nullptr);
        if (m_meshPipelineNoSsao) vkDestroyPipeline(m_dev.device, m_meshPipelineNoSsao, nullptr);
        if (m_meshLayout)    vkDestroyPipelineLayout(m_dev.device, m_meshLayout, nullptr);
        if (m_uploadFence)   vkDestroyFence(m_dev.device, m_uploadFence, nullptr);
        if (m_uploadPool)    vkDestroyCommandPool(m_dev.device, m_uploadPool, nullptr);
        m_meshPipeline = VK_NULL_HANDLE; m_meshPipelineNoSsao = VK_NULL_HANDLE; m_meshLayout = VK_NULL_HANDLE;
        m_uploadFence = VK_NULL_HANDLE; m_uploadPool = VK_NULL_HANDLE;
    }

    // Core objects
    vkb::Instance m_inst{};
    vkb::Device   m_dev{};
    VkSurfaceKHR  m_surface = VK_NULL_HANDLE;
    VkQueue       m_gfxQueue = VK_NULL_HANDLE;
    uint32_t      m_gfxFamily = 0;
    bool          m_descriptorIndexing = false;
    bool          m_rtSupported = false;   // VK_KHR_ray_query + acceleration_structure enabled (RT P0)

    // Graphics
    VmaAllocator  m_alloc = nullptr;
    VkPipeline       m_meshPipeline = VK_NULL_HANDLE;        // SSAO path: depth EQUAL, no write
    VkPipeline       m_meshPipelineNoSsao = VK_NULL_HANDLE;  // no-SSAO path: depth LESS, write
    VkPipelineLayout m_meshLayout = VK_NULL_HANDLE;

    // GPU-driven descriptor objects:
    //   set 0 = bindless texture array (one shared set, update-after-bind)
    //   set 1 = per-frame object SSBO + camera UBO (allocated in createGraphics)
    VkDescriptorSetLayout m_bindlessLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_bindlessPool   = VK_NULL_HANDLE;
    VkDescriptorSet       m_bindlessSet    = VK_NULL_HANDLE;
    uint32_t              m_nextBindless   = 0;   // next free bindless slot
    // Terrain material splat: the marker handle id returned by
    // registerTerrainMaterial() + the four resolved detail bindless indices
    // (grass, rock, snow, sand). 0 marker id == no terrain material registered.
    uint32_t              m_terrainMarkerId = 0;
    uint32_t              m_terrainTexIdx[4] = { 0, 0, 0, 0 };
    VkDescriptorSetLayout m_objSetLayout   = VK_NULL_HANDLE;
    VkDescriptorPool      m_objPool        = VK_NULL_HANDLE;

    // Max distinct meshes per frame (sizes the indirect-command buffer).
    static constexpr uint32_t kMaxDrawMeshes = 4096;

    // Per-frame draw accumulation (GPU-driven). m_drawRecords is filled by
    // drawMesh(); prepareFrameData() groups it (m_groups/m_groupOrder reused to
    // avoid per-frame allocs) into the SSBO + indirect buffer, and the graph's
    // shadow/color passes replay the multidraw from it.
    std::vector<DrawRecord> m_drawRecords;
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_groups;
    std::vector<uint32_t>   m_groupOrder;
    uint32_t                m_drawMeshOrder[kMaxDrawMeshes] = {};
    // Per-frame data preparation (camera/light UBO + SSBO + indirect) is shared by
    // the shadow depth pass AND the main color pass, so it runs once (guarded) and
    // caches the number of indirect commands produced.
    bool                    m_framePrepared = false;
    uint32_t                m_frameCmdCount = 0;

    // ---- Analytic sky pipeline (open-world track, task A) ------------------
    // A vertexless full-screen triangle drawn at the START of the main color pass
    // (after the clear, before meshes) at far depth with depthTest=LESS_OR_EQUAL +
    // depthWrite=OFF, so it fills only un-covered pixels and geometry occludes it.
    // set 0 = the per-frame sky UBO (invViewProj + sun/haze params). Disabled by
    // default; only drawn when setSkyParams(enabled=true).
    VkPipeline            m_skyPipeline  = VK_NULL_HANDLE;
    VkPipelineLayout      m_skyLayout    = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_skySetLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_skyPool      = VK_NULL_HANDLE;
    SkyParams             m_sky{};   // cached params (enabled=false by default)

    // 2D HUD overlay pipeline (NDC quads, no depth, alpha-blended)
    VkPipeline       m_hudPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_hudLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_hudSetLayout = VK_NULL_HANDLE;
    std::vector<HudVertex> m_hudScratch;        // CPU scratch for text batching

    // ---- Role-based glyph atlases (modern HUD/UI fonts) --------------------
    // One baked atlas per FontRole (see IRenderDevice.h). Each is baked once at
    // init from a TTF in assets/fonts/ (kRoleFontPaths) via stb_truetype, with the
    // embedded Roboto Mono as the guaranteed fallback so text is NEVER blank.
    // Proportional roles advance the pen by each glyph's real width; monospace
    // roles advance by a fixed cell (cellAdvance) so legacy N*px layout stays exact.
    // Per-glyph atlas rects/offsets/advance are stored in bake-pixel units so they
    // scale to any requested px at draw time.
    static constexpr int   kTtfFirstChar = 32;   // ASCII space
    static constexpr int   kTtfCharCount = 95;   // 32..126 inclusive
    static constexpr int   kTtfAtlasW    = 1024;  // fits 95 glyphs @ 48px w/ 2x2 oversampling
    static constexpr int   kTtfAtlasH    = 1024;
    static constexpr float kTtfBakePx    = 48.0f; // bake size (oversampled for crispness)
    struct TtfGlyph {
        float u0, v0, u1, v1;   // atlas UVs
        float x0, y0, x1, y1;   // quad offsets in bake-pixel units (relative to pen, baseline at y=0)
        float advance;          // horizontal advance in bake-pixel units
    };
    struct FontAtlas {
        Texture  tex{};                       // R8-coverage-as-RGBA glyph atlas
        TtfGlyph glyphs[kTtfCharCount]{};
        float    cellAdvance  = kTtfBakePx;   // monospace cell width (bake px); proportional uses per-glyph advance
        float    ascent       = kTtfBakePx;   // baseline offset from cell top (bake px)
        bool     proportional = false;        // true => advance per glyph; false => fixed cell
        bool     ready        = false;        // a TTF atlas baked OK for this role
    };
    static constexpr int kFontRoleCount = (int)x3::rhi::FontRole::Count;  // 5
    FontAtlas m_fonts[kFontRoleCount]{};
    bool      m_bitmapFontReady = false;       // legacy 8x8 fallback baked (no TTF at all)
    Texture   m_bitmapFontTex{};               // 8x8 atlas used when a role has no TTF

    // One-time staging upload (transient pool + fence)
    VkCommandPool m_uploadPool = VK_NULL_HANDLE;
    VkFence       m_uploadFence = VK_NULL_HANDLE;

    // ---- Offscreen capture (fix 1: in-frame, acquired-image copy) ----------
    // armCapture() arms a request; endFrame() then records the color-image -> host
    // readback copy INSIDE the live frame (operating on the freshly-rendered,
    // properly-acquired swapchain image, before the PRESENT_SRC transition). The
    // copy completes when that frame's inFlight fence signals. captureFrame()
    // (called after that endFrame) waits on the captured frame's fence, maps the
    // readback buffer, swizzles + writes the PNG. No non-acquired image is read.
    bool          m_captureArmed = false;     // a capture is pending for this frame
    bool          m_captureReady = false;     // the in-frame copy was recorded
    VkBuffer      m_captureBuf   = VK_NULL_HANDLE;
    VmaAllocation m_captureAlloc = nullptr;
    void*         m_captureMapped = nullptr;
    uint32_t      m_captureW = 0, m_captureH = 0;
    uint32_t      m_captureFrameSlot = 0;     // m_frameIdx the copy was recorded into
    VkFence       m_captureFence = VK_NULL_HANDLE; // inFlight fence of that frame
    // Stable storage for the capture-copy pass's source image (the function-pointer
    // record path needs the handle in a member, not a lambda capture).
    VkImage       m_captureColorImg = VK_NULL_HANDLE;

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

    // ======================================================================
    // HDR pipeline + bloom (task: HDR scene target + bloom + emissive).
    // ----------------------------------------------------------------------
    // The lit scene (sky + meshes) renders into an offscreen R16G16B16A16_SFLOAT
    // target in LINEAR HDR (no tonemap). A bloom chain (bright-pass -> N-mip
    // progressive Karis downsample -> tent upsample, all full-screen-triangle
    // fragment passes in the render graph) extracts + blurs the bright HDR
    // radiance. A final composite pass reads the HDR scene + bloom mip0, combines
    // them, applies the shared ACES tonemap ONCE, and writes the LDR
    // swapchain/offscreen image (which the HUD then draws on top of). All images
    // are sized to the frame extent + recreated on resize; the descriptor sets are
    // written once at create time so the per-frame path does NO heap allocation.
    static constexpr uint32_t kBloomMips      = 5;     // 5 progressively-smaller mips
    static constexpr VkFormat kHdrFormat      = VK_FORMAT_R16G16B16A16_SFLOAT;
    // Bloom tunables (subtle/filmic defaults; could be promoted to cvars).
    static constexpr float kBloomThreshold    = 1.10f; // bright-pass luminance threshold
    static constexpr float kBloomKnee         = 0.55f; // soft-knee width
    static constexpr float kBloomUpScale      = 0.85f; // per-mip upsample contribution
    static constexpr float kBloomIntensity    = 0.06f; // final additive bloom strength

    // Linear HDR scene target (sky + meshes render here; sampled by bloom mip0
    // bright-pass + the composite). COLOR_ATTACHMENT | SAMPLED.
    VkImage       m_hdrImg   = VK_NULL_HANDLE;
    VmaAllocation m_hdrAlloc = nullptr;
    VkImageView   m_hdrView  = VK_NULL_HANDLE;

    // One bloom mip (its own image so each can be both a render target AND a
    // sampled source for the next/prev pass). mip[0] is half the frame extent;
    // each subsequent mip halves again.
    struct BloomMip {
        VkImage       img   = VK_NULL_HANDLE;
        VmaAllocation alloc = nullptr;
        VkImageView   view  = VK_NULL_HANDLE;
        VkExtent2D    extent{};
    };
    BloomMip      m_bloomMips[kBloomMips];
    VkSampler     m_postSampler = VK_NULL_HANDLE;   // CLAMP linear sampler (post passes)

    // Post (HDR/bloom/composite) pipelines + layouts.
    VkDescriptorSetLayout m_postSetLayout1 = VK_NULL_HANDLE; // 1 sampler (down/up)
    VkDescriptorSetLayout m_postSetLayout2 = VK_NULL_HANDLE; // 2 samplers (composite)
    VkPipelineLayout      m_bloomLayout    = VK_NULL_HANDLE; // set0=1 sampler + push
    VkPipelineLayout      m_compositeLayout = VK_NULL_HANDLE;// set0=2 samplers + push
    VkPipeline            m_bloomDownPipe  = VK_NULL_HANDLE;
    VkPipeline            m_bloomUpPipe    = VK_NULL_HANDLE;
    VkPipeline            m_compositePipe  = VK_NULL_HANDLE;

    // Descriptor pool + pre-written sets for the post passes. One "1-sampler" set
    // per distinct sampled source (HDR scene + each bloom mip) used by the down/up
    // passes, plus one "2-sampler" composite set. Written once at create time.
    VkDescriptorPool      m_postPool        = VK_NULL_HANDLE;
    VkDescriptorSet       m_setHdr          = VK_NULL_HANDLE;             // samples HDR scene
    VkDescriptorSet       m_setMip[kBloomMips] = {};                      // samples mip i
    VkDescriptorSet       m_setComposite    = VK_NULL_HANDLE;            // HDR scene + bloom mip0

    // Stable storage for the post passes' VkRenderingInfo + attachments (must
    // outlive execute(); the graph holds pointers into these). One per bloom mip
    // for down + up, plus the composite color attach.
    VkRenderingAttachmentInfo m_bloomAttach[kBloomMips]{};       // downsample targets
    VkRenderingInfo           m_bloomRenderInfo[kBloomMips]{};
    VkRenderingAttachmentInfo m_bloomUpAttach[kBloomMips]{};     // upsample targets (LOAD)
    VkRenderingInfo           m_bloomUpRenderInfo[kBloomMips]{};
    VkRenderingAttachmentInfo m_hdrColorAttach{};   // main pass -> HDR target
    VkRenderingAttachmentInfo m_compositeAttach{};
    VkRenderingInfo           m_compositeRenderInfo{};
    // Push-constant payloads for the post passes (stable storage referenced by the
    // record lambdas; no per-frame heap alloc).
    struct BloomPush { float srcTexel[2]; float threshold, knee, intensity; int firstPass; float pad0, pad1; };
    struct CompositePush { float bloomIntensity, exposure, pad0, pad1; };
    BloomPush m_bloomDownPush[kBloomMips]{};
    BloomPush m_bloomUpPush[kBloomMips]{};
    // Stable per-pass record context for the bloom down/up passes. The graph's
    // record path is a raw function pointer + void* ctx (no std::function, no
    // per-frame heap alloc); each pass's per-mip parameters (which descriptor set
    // to bind, the dst extent, the mip index) live here, NOT in a lambda capture.
    struct BloomPassCtx {
        VulkanRenderDevice* self = nullptr;
        VkDescriptorSet     srcSet = VK_NULL_HANDLE;
        VkExtent2D          dstExt{};
        uint32_t            mip = 0;
    };
    BloomPassCtx m_bloomDownCtx[kBloomMips]{};
    BloomPassCtx m_bloomUpCtx[kBloomMips]{};

    // ======================================================================
    // SSAO — screen-space ambient occlusion (idTech-8 grounding/contact AO).
    // ----------------------------------------------------------------------
    // A half-res hemisphere-kernel pass reconstructs each pixel's VIEW-SPACE
    // position + normal FROM THE DEPTH BUFFER (no G-buffer), accumulates occlusion
    // against a noise-rotated 32-sample kernel, and writes a single-channel R8 AO
    // image; a depth-aware bilateral blur removes the 4x4 noise tiling. The result
    // modulates ONLY mesh.frag's ambient term. Depth comes from a NEW depth
    // pre-pass (depth.vert) so the full depth buffer exists before lighting; the
    // main pass then runs depth-test EQUAL with depth-write off. Half-res + a
    // 4x4-tap blur keep the cost well under ~1 ms at 1080p on a 1080 Ti.
    static constexpr int      kSsaoKernel = 32;          // hemisphere samples
    static constexpr VkFormat kSsaoFormat = VK_FORMAT_R8_UNORM;  // single-channel AO
    // SSAO UBO (std140; matches the Ssao block in ssao.frag): proj + invProj, two
    // param vec4s, then the baked kernel[32] + noise[16] tables.
    struct SsaoUBO {
        glm::mat4 proj;
        glm::mat4 invProj;
        glm::vec4 params0;             // x=radius, y=bias, z=intensity, w=power
        glm::vec4 params1;             // x=screenW, y=screenH, z=noiseScale.x, w=noiseScale.y
        glm::vec4 kernel[kSsaoKernel];
        glm::vec4 noise[16];
    };
    static_assert(sizeof(SsaoUBO) == 128 + 32 + (kSsaoKernel + 16) * 16,
                  "SsaoUBO must match the std140 layout in ssao.frag");
    // Tiny control block fed to mesh.frag (set3/binding1): x=enabled, y=strength,
    // z=1/screenW, w=1/screenH.
    struct SsaoControl { glm::vec4 ctrl; };
    // Half-res AO targets: raw (ssao.frag output) + blurred (ssao_blur output,
    // sampled by mesh.frag). Both R8, recreated with the frame extent.
    VkImage       m_ssaoRawImg  = VK_NULL_HANDLE; VmaAllocation m_ssaoRawAlloc  = nullptr; VkImageView m_ssaoRawView  = VK_NULL_HANDLE;
    VkImage       m_ssaoBlurImg = VK_NULL_HANDLE; VmaAllocation m_ssaoBlurAlloc = nullptr; VkImageView m_ssaoBlurView = VK_NULL_HANDLE;
    VkExtent2D    m_ssaoExtent{};                       // half the frame extent
    // Depth pre-pass pipeline (depth.vert; set0 = objSet, reuses m_shadowLayout).
    VkPipeline    m_depthPrePipeline = VK_NULL_HANDLE;
    // SSAO + blur pipelines (full-screen-triangle fragment passes).
    VkSampler             m_depthSampler  = VK_NULL_HANDLE;  // NEAREST, sample depth as data
    VkDescriptorSetLayout m_ssaoSetLayout = VK_NULL_HANDLE;  // depth + SsaoUBO (ssao.frag)
    VkDescriptorSetLayout m_ssaoBlurSetLayout = VK_NULL_HANDLE; // aoRaw + depth (blur)
    VkPipelineLayout      m_ssaoLayout    = VK_NULL_HANDLE;
    VkPipelineLayout      m_ssaoBlurLayout = VK_NULL_HANDLE;
    VkPipeline            m_ssaoPipe      = VK_NULL_HANDLE;
    VkPipeline            m_ssaoBlurPipe  = VK_NULL_HANDLE;
    VkDescriptorPool      m_ssaoPool      = VK_NULL_HANDLE;
    // Per-frame SSAO UBO + sets (the SsaoUBO is filled in prepareFrameData; the
    // depth-sampling sets are rewritten on resize when the depth/AO views change).
    VkBuffer      m_ssaoUboBuf[kFramesInFlight] = {}; VmaAllocation m_ssaoUboAlloc[kFramesInFlight] = {}; void* m_ssaoUboMapped[kFramesInFlight] = {};
    VkDescriptorSet m_ssaoSet[kFramesInFlight]   = {};  // ssao.frag: depth + UBO
    VkDescriptorSet m_ssaoBlurSet                = VK_NULL_HANDLE; // blur: aoRaw + depth
    // mesh.frag set 3: AO texture + SsaoControl UBO (per-frame control buffer).
    // (The mesh-AO descriptor sets are allocated from m_ssaoPool — see createSsao();
    //  there is no separate pool, so no m_meshAoPool member exists.)
    VkDescriptorSetLayout m_meshAoSetLayout = VK_NULL_HANDLE;
    VkBuffer      m_ssaoCtrlBuf[kFramesInFlight] = {}; VmaAllocation m_ssaoCtrlAlloc[kFramesInFlight] = {}; void* m_ssaoCtrlMapped[kFramesInFlight] = {};
    VkDescriptorSet m_meshAoSet[kFramesInFlight] = {};  // mesh.frag set3: AO + ctrl
    VkSampler     m_ssaoLinearSampler = VK_NULL_HANDLE; // CLAMP linear (sample AO)
    // Stable storage for the SSAO/blur/depth-prepass VkRenderingInfo + attachments.
    VkRenderingAttachmentInfo m_depthPreAttach{};
    VkRenderingInfo           m_depthPreRenderInfo{};
    VkRenderingAttachmentInfo m_ssaoAttach{};
    VkRenderingInfo           m_ssaoRenderInfo{};
    VkRenderingAttachmentInfo m_ssaoBlurAttach{};
    VkRenderingInfo           m_ssaoBlurRenderInfo{};
    struct SsaoBlurPush { float aoTexel[2]; float depthSigma, pad0; };
    SsaoBlurPush m_ssaoBlurPush{};
    SsaoParams   m_ssao{};   // cached tunables (setSsaoParams)
    // Baked (deterministic) hemisphere kernel + 4x4 rotation noise (CPU-side),
    // copied into each frame's SSAO UBO by prepareFrameData.
    bool      m_ssaoKernelBuilt = false;
    glm::vec4 m_ssaoKernelCPU[kSsaoKernel]{};
    glm::vec4 m_ssaoNoiseCPU[16]{};

    // ======================================================================
    // GI — real-time dynamic global illumination (screen-space indirect diffuse).
    // CLEAN-ROOM, original. Built from public, non-engine references only: the
    // Vulkan 1.3 spec; Real-Time Rendering 4th ed.; the screen-space directional
    // occlusion / SSGI formulation (Ritschel/Grosch/Seidel, I3D 2009) where screen
    // occluders carry surface radiance for indirect colour bleeding; temporal-
    // reprojection / EMA accumulation + a-trous/bilateral denoise write-ups. No
    // game-engine source consulted.
    // ----------------------------------------------------------------------
    // Chain (all added to the render graph after the main color pass, before the
    // bloom chain): GATHER (half-res, ssgi_gather.frag — reconstruct view pos/normal
    // from the SSAO depth pre-pass, march a cosine hemisphere, sample the lit HDR
    // scene as incoming radiance) -> TEMPORAL (ssgi_temporal.frag — camera-reproject
    // last frame's GI + EMA blend, reject disocclusion) -> DENOISE (ssgi_blur.frag —
    // depth-aware bilateral) -> APPLY (full-res, ssgi_apply.frag — depth-aware
    // up-sample + ADDITIVE into the linear HDR target, modulated by the SSAO AO).
    // Half-res + a few taps + temporal target well under ~2 ms at 720p on a 1080 Ti.
    static constexpr int      kGiKernel = 24;                       // hemisphere taps
    static constexpr VkFormat kGiFormat = VK_FORMAT_R16G16B16A16_SFLOAT; // HDR indirect radiance
    // GI gather UBO (std140; matches the Gi block in ssgi_gather.frag).
    struct GiUBO {
        glm::mat4 proj;
        glm::mat4 invProj;
        glm::vec4 params0;             // x=radius, y=intensity, z=maxRadiance, w=falloffPower
        glm::vec4 params1;             // x=screenW, y=screenH, z=numSamples, w=bias
        glm::vec4 kernel[kGiKernel];
        glm::vec4 noise[16];
    };
    static_assert(sizeof(GiUBO) == 128 + 32 + (kGiKernel + 16) * 16,
                  "GiUBO must match the std140 layout in ssgi_gather.frag");
    // GI temporal UBO (matches GiTemporal in ssgi_temporal.frag).
    struct GiTemporalUBO {
        glm::mat4 invViewProjCur;
        glm::mat4 viewProjPrev;
        glm::vec4 params0;             // x=alpha, y=depthRejectScale, z=valid, w=unused
    };
    // Push constants for the denoise blur + apply passes.
    struct GiBlurPush  { float giTexel[2]; float depthSigma, stepScale; };
    struct GiApplyPush { float giTexel[2]; float strength, aoAmount; };
    GiBlurPush  m_giBlurPush{};
    GiApplyPush m_giApplyPush{};
    // Half-res GI targets: raw gather, two ping-pong accumulation buffers (one is
    // the live history across frames), and a denoised buffer. All RGBA16F.
    VkImage m_giRawImg = VK_NULL_HANDLE; VmaAllocation m_giRawAlloc = nullptr; VkImageView m_giRawView = VK_NULL_HANDLE;
    VkImage m_giAccumImg[2] = {}; VmaAllocation m_giAccumAlloc[2] = {}; VkImageView m_giAccumView[2] = {};
    VkImage m_giDenoiseImg = VK_NULL_HANDLE; VmaAllocation m_giDenoiseAlloc = nullptr; VkImageView m_giDenoiseView = VK_NULL_HANDLE;
    // Full-res previous-frame depth copy (for temporal disocclusion reject). Kept
    // across frames; the main depth is overwritten each frame so we snapshot it.
    VkImage m_giPrevDepthImg = VK_NULL_HANDLE; VmaAllocation m_giPrevDepthAlloc = nullptr; VkImageView m_giPrevDepthView = VK_NULL_HANDLE;
    VkExtent2D m_giExtent{};                  // half the frame extent
    uint32_t   m_giAccumWrite = 0;            // which accum buffer this frame writes (ping-pong)
    bool       m_giHistoryValid = false;      // false on first frame / after resize
    glm::mat4  m_giPrevViewProj{1.0f};        // last frame's camera viewProj (for reproject)
    // Pipelines + layouts.
    VkDescriptorSetLayout m_giGatherSetLayout   = VK_NULL_HANDLE; // depth + scene + GiUBO
    VkDescriptorSetLayout m_giTemporalSetLayout = VK_NULL_HANDLE; // cur + hist + depth + prevDepth + UBO
    VkDescriptorSetLayout m_giBlurSetLayout     = VK_NULL_HANDLE; // gi + depth
    VkDescriptorSetLayout m_giApplySetLayout    = VK_NULL_HANDLE; // gi + depth + ao
    VkPipelineLayout m_giGatherLayout   = VK_NULL_HANDLE;
    VkPipelineLayout m_giTemporalLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_giBlurLayout     = VK_NULL_HANDLE;
    VkPipelineLayout m_giApplyLayout    = VK_NULL_HANDLE;
    VkPipeline m_giGatherPipe   = VK_NULL_HANDLE;
    VkPipeline m_giTemporalPipe = VK_NULL_HANDLE;
    VkPipeline m_giBlurPipe     = VK_NULL_HANDLE;
    VkPipeline m_giApplyPipe    = VK_NULL_HANDLE;
    VkDescriptorPool m_giPool = VK_NULL_HANDLE;
    // Per-frame UBOs + their gather/temporal descriptor sets. The temporal +
    // blur/apply sets reference ping-pong views, so they are rewritten each frame
    // (cheap vkUpdateDescriptorSets, no allocation) to point at the right buffers.
    VkBuffer m_giUboBuf[kFramesInFlight] = {}; VmaAllocation m_giUboAlloc[kFramesInFlight] = {}; void* m_giUboMapped[kFramesInFlight] = {};
    VkBuffer m_giTempUboBuf[kFramesInFlight] = {}; VmaAllocation m_giTempUboAlloc[kFramesInFlight] = {}; void* m_giTempUboMapped[kFramesInFlight] = {};
    VkDescriptorSet m_giGatherSet[kFramesInFlight] = {};
    VkDescriptorSet m_giTemporalSet[kFramesInFlight] = {};
    VkDescriptorSet m_giBlurSet[kFramesInFlight]  = {};   // per-frame: rewritten each frame (ping-pong)
    VkDescriptorSet m_giApplySet[kFramesInFlight] = {};   // per-frame: rewritten each frame
    // Stable storage for the GI VkRenderingInfo + attachments (one per pass).
    VkRenderingAttachmentInfo m_giGatherAttach{};   VkRenderingInfo m_giGatherRenderInfo{};
    VkRenderingAttachmentInfo m_giTemporalAttach{}; VkRenderingInfo m_giTemporalRenderInfo{};
    VkRenderingAttachmentInfo m_giBlurAttach{};     VkRenderingInfo m_giBlurRenderInfo{};
    VkRenderingAttachmentInfo m_giApplyAttach{};    VkRenderingInfo m_giApplyRenderInfo{};
    GiParams  m_gi{};                  // cached tunables (setGiParams)
    bool      m_giKernelBuilt = false;
    glm::vec4 m_giKernelCPU[kGiKernel]{};
    glm::vec4 m_giNoiseCPU[16]{};

    // ======================================================================
    // WATER — animated ocean surface (undersea-world foundation).
    // CLEAN-ROOM, original work. Wave model = the standard public Gerstner /
    // sum-of-sines formulation (Tessendorf "Simulating Ocean Water", the GPU Gems
    // Gerstner-wave chapter, public ocean-rendering articles); shading from public
    // water-rendering references. No GPL / id Tech / RBDOOM source consulted.
    // ----------------------------------------------------------------------
    // A large tessellated grid plane centered + snapped under the camera at
    // `seaLevel`, displaced by a sum of Gerstner waves (water.vert) with analytic
    // normals, drawn in the MAIN pass AFTER opaque meshes into the linear HDR
    // target. water.frag Fresnel-blends an analytic-sky reflection with a
    // depth-based refraction color (reads the scene depth buffer — the SAME depth
    // the SSAO pre-pass / main pass produced) and adds a sharp sun glint (HDR ->
    // bloom). Gated by setWaterParams(enabled): default OFF, zero cost when off.
    // The grid is a unit-patch mesh uploaded once; its own per-frame UBO + set.
    static constexpr uint32_t kWaterGridDim = 192;   // verts per edge (191^2 quads)
    static constexpr float    kWaterPatchHalf = 240.0f; // world half-extent (m)
    struct WaterUBO {
        glm::mat4 viewProj;       // 0
        glm::vec4 camPos;         // 64
        glm::vec4 sunDir;         // 80
        glm::vec4 deepColor;      // 96
        glm::vec4 shallowColor;   // 112
        glm::vec4 p0;             // 128: x=seaLevel,y=time,z=amplitude,w=steepness
        glm::vec4 p1;             // 144: x=baseWavelength,y=speed,z=specular,w=fresnelBase
        glm::vec4 p2;             // 160: x=patchHalfExtent,y=1/W,z=1/H,w=reserved
    };
    static_assert(sizeof(WaterUBO) == 176, "WaterUBO must match the std140 layout in water.{vert,frag}");
    WaterParams m_water{};   // cached tunables (setWaterParams)
    // Unit-patch grid mesh (vec2 grid coord per vertex), built once at init.
    VkBuffer      m_waterVbo = VK_NULL_HANDLE; VmaAllocation m_waterVboAlloc = nullptr;
    VkBuffer      m_waterIbo = VK_NULL_HANDLE; VmaAllocation m_waterIboAlloc = nullptr;
    uint32_t      m_waterIndexCount = 0;
    VkDescriptorSetLayout m_waterSetLayout = VK_NULL_HANDLE; // UBO + scene depth
    VkPipelineLayout      m_waterLayout    = VK_NULL_HANDLE;
    VkPipeline            m_waterPipeline  = VK_NULL_HANDLE;
    VkDescriptorPool      m_waterPool      = VK_NULL_HANDLE;
    // Per-frame UBO + descriptor set (set0: WaterUBO + scene-depth sampler). The
    // depth-binding write is refreshed each frame (depth view persists, but rewire
    // on resize); the UBO is mapped + filled in prepareFrameData.
    VkBuffer      m_waterUboBuf[kFramesInFlight] = {}; VmaAllocation m_waterUboAlloc[kFramesInFlight] = {}; void* m_waterUboMapped[kFramesInFlight] = {};
    VkDescriptorSet m_waterSet[kFramesInFlight] = {};
    VkSampler     m_waterDepthSampler = VK_NULL_HANDLE; // LINEAR clamp, samples scene depth
    // Stable storage for the water pass's VkRenderingInfo + attachments.
    VkRenderingAttachmentInfo m_waterColorAttach{};
    VkRenderingAttachmentInfo m_waterDepthAttach{};
    VkRenderingInfo           m_waterRenderInfo{};

    // ---- GPU-instanced billboard particles + impact decals (combat juice) ---
    // A bounded, per-frame stream of camera-facing billboards drawn into the linear
    // HDR target AFTER opaque + water + GI, BEFORE bloom (so bright additive sparks
    // feed the bloom chain), depth-TESTED against the scene (no depth-write), with
    // a SOFT-PARTICLE fade vs. the scene depth (the SSAO/water depth buffer). Impact
    // decals (oriented quads on the hit surface) are drawn in the SAME pass, alpha-
    // blended, depth-tested, no write. CPU owns the sim (app/fx.*) + submits each
    // frame; the device streams the instances into a per-frame ring + draws ONE
    // instanced quad per sub-batch. The whole pass is added only when something was
    // submitted this frame (zero GPU cost when idle).
    static constexpr uint32_t kMaxParticles = 16384; // per-frame instance cap (each mode)
    static constexpr uint32_t kMaxDecals    = 256;    // per-frame decal instance cap
    // ParticleUBO (set0,b0): camera viewProj + screen-aligned billboard basis +
    // depth-reconstruction params. std140; matches particle.{vert,frag}.
    struct ParticleUBO {
        glm::mat4 viewProj;   // 0
        glm::vec4 camRight;   // 64
        glm::vec4 camUp;      // 80
        glm::vec4 camPos;     // 96
        glm::vec4 params;     // 112: x=1/W, y=1/H, z=near, w=far
    };
    static_assert(sizeof(ParticleUBO) == 128, "ParticleUBO must match the std140 layout");
    // DecalUBO (set0,b0): camera viewProj. std140; matches decal.{vert,frag}.
    struct DecalUBO {
        glm::mat4 viewProj;   // 0
        glm::vec4 params;     // 64: reserved
    };
    static_assert(sizeof(DecalUBO) == 80, "DecalUBO must match the std140 layout");
    // Per-instance GPU layout for the billboard pipeline: pos.xyz+halfSize, rgba.
    struct ParticleGpu { glm::vec4 posSize; glm::vec4 color; };
    // Per-instance GPU layout for the decal pipeline: center+halfSize, normal+angle, rgba.
    struct DecalGpu { glm::vec4 centerSize; glm::vec4 normalAngle; glm::vec4 color; };

    // CPU staging (fixed capacity; cleared each beginFrame -> no per-frame heap churn).
    std::vector<ParticleInstance> m_partAdd;    // additive batch (sparks/fire/muzzle)
    std::vector<ParticleInstance> m_partAlpha;  // alpha batch (smoke/dust/blood)
    std::vector<DecalInstance>    m_decals;     // impact decals this frame
    uint32_t m_partAddCount = 0, m_partAlphaCount = 0, m_decalCount = 0; // uploaded counts

    // Shared unit quad (4 corners in [-0.5,0.5], triangle strip) for both pipelines.
    VkBuffer      m_partQuadVbo = VK_NULL_HANDLE; VmaAllocation m_partQuadAlloc = nullptr;
    // Per-frame instance rings (persistent-mapped): additive + alpha particles, decals.
    VkBuffer m_partInstAddBuf[kFramesInFlight] = {};   VmaAllocation m_partInstAddAlloc[kFramesInFlight] = {};   void* m_partInstAddMapped[kFramesInFlight] = {};
    VkBuffer m_partInstAlphaBuf[kFramesInFlight] = {}; VmaAllocation m_partInstAlphaAlloc[kFramesInFlight] = {}; void* m_partInstAlphaMapped[kFramesInFlight] = {};
    VkBuffer m_decalInstBuf[kFramesInFlight] = {};     VmaAllocation m_decalInstAlloc[kFramesInFlight] = {};     void* m_decalInstMapped[kFramesInFlight] = {};
    // Per-frame UBOs (particle + decal) + descriptor sets.
    VkBuffer m_partUboBuf[kFramesInFlight] = {};  VmaAllocation m_partUboAlloc[kFramesInFlight] = {};  void* m_partUboMapped[kFramesInFlight] = {};
    VkBuffer m_decalUboBuf[kFramesInFlight] = {}; VmaAllocation m_decalUboAlloc[kFramesInFlight] = {}; void* m_decalUboMapped[kFramesInFlight] = {};
    VkDescriptorSet m_partSet[kFramesInFlight] = {};   // UBO + scene-depth sampler
    VkDescriptorSet m_decalSet[kFramesInFlight] = {};  // UBO only
    VkSampler             m_partDepthSampler = VK_NULL_HANDLE;  // LINEAR clamp, scene depth
    VkDescriptorSetLayout m_partSetLayout    = VK_NULL_HANDLE;  // b0 UBO + b1 depth
    VkDescriptorSetLayout m_decalSetLayout   = VK_NULL_HANDLE;  // b0 UBO
    VkDescriptorPool      m_partPool         = VK_NULL_HANDLE;
    VkPipelineLayout      m_partLayout       = VK_NULL_HANDLE;
    VkPipelineLayout      m_decalLayout      = VK_NULL_HANDLE;
    VkPipeline            m_partAddPipeline  = VK_NULL_HANDLE;  // additive blend
    VkPipeline            m_partAlphaPipeline= VK_NULL_HANDLE;  // alpha blend
    VkPipeline            m_decalPipeline    = VK_NULL_HANDLE;  // alpha blend
    // Stable storage for the particle pass's VkRenderingInfo + attachments.
    VkRenderingAttachmentInfo m_partColorAttach{};
    VkRenderingAttachmentInfo m_partDepthAttach{};
    VkRenderingInfo           m_partRenderInfo{};

    // ---- GPU-compute persistent debris world (Subsystem K, tier T2) -------
    // A host-visible DEVICE_LOCAL pool SSBO of GpuDebrisFragment rows integrated by
    // a compute shader each step + drawn via ONE instanced unit-cube draw. The pool
    // + counters persist across frames (debris is persistent); the params/draw UBOs
    // are per-frame-in-flight. Spawn writes free slots on the CPU (mapped); readback
    // summarizes the mapped pool. Pending flags gate this frame's compute/draw pass.
    IRenderDevice::GpuDebrisParams m_debrisParams{};
    VkBuffer      m_debrisPoolBuf   = VK_NULL_HANDLE; VmaAllocation m_debrisPoolAlloc  = nullptr; void* m_debrisPoolMapped  = nullptr;
    VkBuffer      m_debrisCountBuf  = VK_NULL_HANDLE; VmaAllocation m_debrisCountAlloc = nullptr; void* m_debrisCountMapped = nullptr;
    VkBuffer      m_debrisParamsBuf[kFramesInFlight]  = {}; VmaAllocation m_debrisParamsAlloc[kFramesInFlight]  = {}; void* m_debrisParamsMapped[kFramesInFlight]  = {};
    VkBuffer      m_debrisDrawUboBuf[kFramesInFlight] = {}; VmaAllocation m_debrisDrawUboAlloc[kFramesInFlight] = {}; void* m_debrisDrawUboMapped[kFramesInFlight] = {};
    VkBuffer      m_debrisCubeVbo = VK_NULL_HANDLE; VmaAllocation m_debrisCubeAlloc    = nullptr;
    VkBuffer      m_debrisCubeIbo = VK_NULL_HANDLE; VmaAllocation m_debrisCubeIboAlloc = nullptr;
    uint32_t      m_debrisCubeIndexCount = 0;
    VkDescriptorPool      m_debrisPool             = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_debrisComputeSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_debrisDrawSetLayout    = VK_NULL_HANDLE;
    VkDescriptorSet       m_debrisComputeSet[kFramesInFlight] = {};  // per-frame (params UBO is per-frame)
    VkDescriptorSet       m_debrisDrawSet[kFramesInFlight] = {};
    VkPipelineLayout      m_debrisComputeLayout    = VK_NULL_HANDLE;
    VkPipelineLayout      m_debrisDrawLayout       = VK_NULL_HANDLE;
    VkPipeline            m_debrisComputePipeline  = VK_NULL_HANDLE;
    VkPipeline            m_debrisDrawPipeline     = VK_NULL_HANDLE;
    uint32_t              m_debrisSpawnCursor      = 0;   // recycle ring cursor
    uint32_t              m_debrisAlive            = 0;   // CPU mirror (counters[0] is authoritative)
    bool                  m_debrisStepPending      = false; // dispatch the compute pass this frame
    bool                  m_debrisDrawPending      = false; // record the instanced draw this frame
    glm::mat4             m_lastViewProj{ 1.0f };           // cached viewProj for the debris draw UBO
    VkRenderingAttachmentInfo m_debrisColorAttach{};
    VkRenderingAttachmentInfo m_debrisDepthAttach{};
    VkRenderingInfo           m_debrisRenderInfo{};

    // ---- GPU compute skinning of models (GPU SKINNING OF MODELS) ----------
    // A compute pre-pass that linear-blend-skins registered meshes into their
    // per-frame dynamic vbo BEFORE the depth/shadow/color passes (which then draw
    // that buffer with their unchanged vertex shaders). Each registered skinned mesh
    // owns: an IMMUTABLE bind-pose+attrs SSBO + an IMMUTABLE influences SSBO
    // (uploaded once), a per-frame-in-flight palette SSBO (host-visible; the CPU
    // Skinner uploads the joint-matrix palette into it each frame), and a per-frame
    // compute descriptor set. The skinned OUTPUT is the Mesh's dynVbo[frameIdx]
    // (promoted to dynamic + given STORAGE usage so the compute can write it AND the
    // draw can read it as a vertex buffer). One vkCmdDispatch per pending instance.
    // (The SkinSrcVertex / SkinSrcInfluence / SkinnedMesh / SkinPush TYPES are
    // declared up near the Mesh/Texture structs so the method signatures that take
    // them are visible — see the private struct block after Texture.)
    std::unordered_map<uint32_t, SkinnedMesh> m_skinnedMeshes;       // keyed by mesh id
    std::vector<uint32_t>  m_skinPending;                            // mesh ids to dispatch this frame
    VkDescriptorPool       m_skinPool          = VK_NULL_HANDLE;
    VkDescriptorSetLayout  m_skinSetLayout     = VK_NULL_HANDLE;
    VkPipelineLayout       m_skinPipelineLayout= VK_NULL_HANDLE;
    VkPipeline             m_skinPipeline      = VK_NULL_HANDLE;
    bool                   m_skinStepPending   = false;              // any skinning to dispatch this frame

    // ---- Directional shadow mapping (perf-stack E) ------------------------
    // A single fixed-resolution depth texture rendered from the sun's POV each
    // frame (depth-only pipeline, same SSBO + indirect draws as the color pass),
    // then sampled by mesh.frag via a compare sampler (sampler2DShadow). One map
    // (no CSM); the ortho box follows the camera. Created once (resolution is
    // swapchain-independent); the per-frame work is the depth pass + a barrier.
    VkFormat              m_shadowFormat   = VK_FORMAT_D32_SFLOAT;
    VkImage               m_shadowImg      = VK_NULL_HANDLE;
    VmaAllocation         m_shadowAlloc    = nullptr;
    VkImageView           m_shadowView     = VK_NULL_HANDLE;
    VkSampler             m_shadowSampler  = VK_NULL_HANDLE;   // compare-enabled
    VkPipeline            m_shadowPipeline = VK_NULL_HANDLE;   // depth-only
    VkPipelineLayout      m_shadowLayout   = VK_NULL_HANDLE;   // set0 = objSet
    VkDescriptorSetLayout m_shadowSetLayout = VK_NULL_HANDLE;  // set2: sampler2DShadow
    VkDescriptorPool      m_shadowDescPool = VK_NULL_HANDLE;
    VkDescriptorSet       m_shadowSet      = VK_NULL_HANDLE;   // points at the map
    glm::mat4             m_lightViewProj{ 1.0f };  // computed each frame
    uint32_t              m_curImageIndex  = 0;

    // ---- Render graph (perf-stack B) --------------------------------------
    // ONE persistent graph; rebuilt cheaply each frame in endFrame() from the
    // passes' declared resource reads/writes (capacity persists -> no per-frame
    // heap churn). It derives + emits every sync2 barrier + layout transition and
    // drives begin/endRendering. The shadow map's state PERSISTS across frames
    // (DEPTH_READ_ONLY after the main pass samples it; UNDEFINED on first use) and
    // is fed back in via importImage so the cross-frame WAR barrier is derived.
    RenderGraph           m_graph;
    ResourceState         m_shadowState{ VK_IMAGE_LAYOUT_UNDEFINED,
                                         VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 };
    // Deferred HUD draws (appended by drawHudQuad/drawHudText, replayed inside the
    // graph's color pass). Capacity persists across frames.
    std::vector<HudRecord> m_hudRecords;
    // Stable storage for the VkRenderingInfo + attachment structs the graph passes
    // reference (must outlive execute(); the graph holds pointers into these).
    VkRenderingInfo           m_shadowRenderInfo{};
    VkRenderingAttachmentInfo m_shadowDepthAttach{};
    VkRenderingInfo           m_mainRenderInfo{};
    VkRenderingAttachmentInfo m_mainDepthAttach{};

    // Swapchain (windowed mode only; all null/empty in headless mode)
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkExtent2D     m_extent{};
    VkFormat       m_format = VK_FORMAT_B8G8R8A8_UNORM;
    std::vector<VkImage>     m_swapImages;
    std::vector<VkImageView> m_swapViews;
    std::vector<VkSemaphore> m_renderFinished; // per swapchain image

    // ---- Headless offscreen render target (headless mode only) -------------
    // The single color image the render graph targets in place of an acquired
    // swapchain image. Same format/extent the swapchain used (B8G8R8A8_UNORM,
    // COLOR_ATTACHMENT | TRANSFER_SRC) so the capture readback is byte-identical.
    // m_extent / m_format / m_depthImg / m_depthView above are shared with the
    // windowed path (only ONE of the two target sets is ever live).
    bool          m_headless = false;
    VkImage       m_offscreenColorImg   = VK_NULL_HANDLE;
    VmaAllocation m_offscreenColorAlloc = nullptr;
    VkImageView   m_offscreenColorView  = VK_NULL_HANDLE;

    // Per-frame-in-flight
    Frame    m_frames[kFramesInFlight];
    uint32_t m_frameIdx = 0;
    uint64_t m_totalFrames = 0;

    // ---- Deferred buffer destruction (fix 2) ------------------------------
    // When updateMesh() promotes a static mesh to dynamic, its old device-local
    // vbo may still be referenced by command buffers from up to kFramesInFlight-1
    // earlier frames. Rather than vkDeviceWaitIdle, we queue the buffer for
    // destruction tagged with the frame index it became unreferenced, and free it
    // once kFramesInFlight more frames have started (all referencing frames retired).
    struct PendingFree { VkBuffer buf; VmaAllocation alloc; uint64_t retireAtFrame; };
    std::vector<PendingFree> m_pendingFrees;
    void deferDestroyBuffer(VkBuffer buf, VmaAllocation alloc) {
        if (!buf) return;
        // Safe to free once kFramesInFlight frames have begun past the current one
        // (every in-flight cmd buffer that could reference it will have retired).
        m_pendingFrees.push_back({ buf, alloc, m_totalFrames + kFramesInFlight });
    }

    // ---- Deferred image/view/sampler destruction (fix 2) ------------------
    // destroyTexture() / destroyMesh() used to vkDeviceWaitIdle before freeing —
    // a full GPU stall PER destroy. During terrain-streaming eviction that is
    // dozens of stalls per boundary-cross. Instead we queue the GPU objects with
    // the frame index they became unreferenced and free them once kFramesInFlight
    // frames have begun (every in-flight cmd buffer that could reference them has
    // retired — same retirement rule as deferDestroyBuffer). The bindless-slot
    // write-back to white (host-side vkUpdateDescriptorSets) is done IMMEDIATELY
    // by the caller; only the actual destroy is deferred. Any of the handles may
    // be VK_NULL_HANDLE (e.g. a mesh defers only buffers; a texture defers
    // image+view+sampler) — the drain skips null handles.
    struct PendingImageFree {
        VkImage       image; VmaAllocation alloc;
        VkImageView   view;  VkSampler     sampler;
        uint64_t      retireAtFrame;
    };
    std::vector<PendingImageFree> m_pendingImageFrees;
    void deferDestroyImage(VkImage image, VmaAllocation alloc,
                           VkImageView view, VkSampler sampler) {
        if (!image && !view && !sampler) return;
        m_pendingImageFrees.push_back({ image, alloc, view, sampler,
                                        m_totalFrames + kFramesInFlight });
    }
    void drainPendingFrees() {
        for (size_t i = 0; i < m_pendingFrees.size();) {
            if (m_totalFrames >= m_pendingFrees[i].retireAtFrame) {
                vmaDestroyBuffer(m_alloc, m_pendingFrees[i].buf, m_pendingFrees[i].alloc);
                m_pendingFrees[i] = m_pendingFrees.back();
                m_pendingFrees.pop_back();
            } else { ++i; }
        }
        for (size_t i = 0; i < m_pendingImageFrees.size();) {
            PendingImageFree& p = m_pendingImageFrees[i];
            if (m_totalFrames >= p.retireAtFrame) {
                if (p.sampler) vkDestroySampler(m_dev.device, p.sampler, nullptr);
                if (p.view)    vkDestroyImageView(m_dev.device, p.view, nullptr);
                if (p.image)   vmaDestroyImage(m_alloc, p.image, p.alloc);
                p = m_pendingImageFrees.back();
                m_pendingImageFrees.pop_back();
            } else { ++i; }
        }
    }
    // Force-free EVERYTHING still queued (used at shutdown after a final
    // vkDeviceWaitIdle, when all frames are guaranteed retired).
    void flushPendingFrees() {
        for (auto& pf : m_pendingFrees) vmaDestroyBuffer(m_alloc, pf.buf, pf.alloc);
        m_pendingFrees.clear();
        for (auto& p : m_pendingImageFrees) {
            if (p.sampler) vkDestroySampler(m_dev.device, p.sampler, nullptr);
            if (p.view)    vkDestroyImageView(m_dev.device, p.view, nullptr);
            if (p.image)   vmaDestroyImage(m_alloc, p.image, p.alloc);
        }
        m_pendingImageFrees.clear();
    }

    // ---- Perf instrumentation ---------------------------------------------
    // GPU timestamps: the device's valid bit count + ns-per-tick (timestampPeriod).
    // Both come from VkPhysicalDeviceLimits; if timestamps are unsupported on the
    // graphics queue the pool is never created and gpuFrameMs stays 0.
    bool     m_tsSupported = false;
    float    m_tsPeriodNs  = 0.0f;       // nanoseconds per timestamp tick
    uint64_t m_tsValidMask = 0;          // mask of meaningful timestamp bits
    float    m_lastGpuMs   = 0.0f;       // most recent GPU pass time (ms)
    // Counters being accumulated for the in-flight frame (reset each beginFrame),
    // and the snapshot of the last completed frame (returned by stats()).
    RenderStats m_building{};            // accumulates during the current frame
    RenderStats m_lastStats{};           // snapshot taken at endFrame()

    bool m_vsync = true;
    bool m_needsRecreate = false;
    uint32_t m_width = 0, m_height = 0;

    // Camera (FPS); defaults frame the cube at origin
    glm::vec3 m_camPos{ 0.0f, 1.5f, 4.0f };
    float m_camYaw = -1.5708f;   // look toward -Z
    float m_camPitch = -0.30f;   // slightly down
    float m_camFov = 60.0f;

    // ---- Forward point lights (interior fill) -----------------------------
    // CPU-side cache set by setPointLights(); re-uploaded into each frame's UBO.
    // m_ambient is a small constant lift so shadowed/back-faces aren't pure black
    // (a touch cool/blue to read as a sci-fi interior without washing out).
    std::vector<PointLight> m_pointLights;
    // Hemispheric ambient FLOOR (mesh.frag ambientCount.rgb). The original 0.10/0.11/
    // 0.14 was tuned far too dark for a SUNLESS indoor scene (B1 has no directional
    // sun, so unlit areas fell to near-black — "incredibly dark"). Lifted ~2.6x to a
    // readable cool-sci-fi floor. PERF: render-cost-neutral (a shader constant; no
    // extra draws/work). See docs/PERF_LOG.md.
    // 2nd lift (still "couldn't see"): 0.26 -> 0.42. Sunless B1 needs a real ambient
    // floor; point lights only pool under fixtures, leaving floor/walls black between.
    glm::vec3               m_ambient{ 0.42f, 0.44f, 0.50f };
};

} // namespace

IRenderDevice* createRenderDevice() { return new VulkanRenderDevice(); }

} // namespace x3::rhi
