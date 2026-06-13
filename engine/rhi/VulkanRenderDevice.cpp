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
#include "VulkanRT.h"          // hardware ray-tracing AS manager (ray-query path)
#include "../core/x3_log.h"
#include "../core/x3_boot.h"   // [boot] timeline marks (device-init sub-phases)
#include "font8x8_basic.h"
#include "font_robotomono.h"   // embedded Roboto Mono TTF (Apache-2.0) — modern HUD font

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>

// Dear ImGui (docking) — EDITOR-ONLY. These headers + the GLFW/Vulkan backends are
// referenced ONLY by the editor-UI methods (initEditorUI/beginEditorUI/endEditorUI/
// shutdownEditorUI) and the editor-UI graph pass; nothing in the game/headless path
// touches them. ImGui types stay inside THIS .cpp — IRenderDevice.h never sees them.
// GLFW is forward-declared by imgui_impl_glfw.h (no GLFW include needed here; the
// host passes the GLFWwindow* as void* across the IRenderDevice boundary).
#include <imgui.h>
#include <imgui_impl_glfw.h>     // vcpkg installs the backends flat in include/ (no backends/ prefix)
#include <imgui_impl_vulkan.h>

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
#include <chrono>
#include <fstream>
#include <filesystem>
#include <system_error>
#include <cstring>
#include <cstddef>
#include <cstdio>
#include <cassert>
#include <algorithm>
#include <limits>
#include <unordered_map>
#include <chrono>
#include <mutex>   // m_uploadMu: parallel boot-time model preload (docs/BOOT_TIME.md)

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "FrustumCull.h"   // CPU per-object frustum cull (r_frustumcull, D15 baseline)
#include "GpuCull.h"       // D15 GPU-driven culling (r_cullpath: Tier 0/1/2 + HZB)

namespace x3::rhi {

namespace {

// Compute a model-space bounding sphere from a vertex list for the CPU frustum cull:
// AABB center, then the radius is the max vertex distance from that center (a tight
// conservative sphere). Empty/null -> zero radius (treated as unbounded -> never culled).
inline void computeLocalSphere(const MeshVertex* verts, uint32_t vcount,
                               glm::vec3& outCenter, float& outRadius) {
    outCenter = glm::vec3(0.0f);
    outRadius = 0.0f;
    if (!verts || vcount == 0) return;
    glm::vec3 lo(verts[0].pos[0], verts[0].pos[1], verts[0].pos[2]);
    glm::vec3 hi = lo;
    for (uint32_t i = 1; i < vcount; ++i) {
        const glm::vec3 p(verts[i].pos[0], verts[i].pos[1], verts[i].pos[2]);
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    outCenter = 0.5f * (lo + hi);
    float r2 = 0.0f;
    for (uint32_t i = 0; i < vcount; ++i) {
        const glm::vec3 p(verts[i].pos[0], verts[i].pos[1], verts[i].pos[2]);
        r2 = std::max(r2, glm::dot(p - outCenter, p - outCenter));
    }
    outRadius = std::sqrt(r2);
}

// Transform a model-space bounding sphere to world space by an instance model matrix:
// center -> (model * center); radius -> radius * (max scale axis), so non-uniform
// scale stays conservative (the largest axis bounds the whole sphere). Returns the
// CullSphere (xyz = world center, w = world radius) the GPU-equivalent test consumes.
inline CullSphere worldSphere(const glm::mat4& model, const glm::vec3& cLocal, float rLocal) {
    const glm::vec3 cWorld = glm::vec3(model * glm::vec4(cLocal, 1.0f));
    // Column lengths of the upper-left 3x3 = per-axis scale (incl. shear contribution).
    const float sx = glm::length(glm::vec3(model[0]));
    const float sy = glm::length(glm::vec3(model[1]));
    const float sz = glm::length(glm::vec3(model[2]));
    const float maxScale = std::max(sx, std::max(sy, sz));
    return CullSphere(cWorld, rLocal * maxScale);
}

constexpr uint32_t kFramesInFlight = 2;
// Glass frost (M4): number of progressively-downsampled blur levels of the scene
// copy. Each is a separate single-mip image (the render-graph tracks one layout per
// image). The glass shader samples the deepest level for the frosted look.
constexpr uint32_t kGlassFrostLevels = 3;

class VulkanRenderDevice final : public IRenderDevice {
public:
    bool init(const DeviceDesc& desc) override {
        m_vsync = desc.vsync;
        m_headless = desc.headless;
        // SSAA (headless only): render at outW*ssaa x outH*ssaa, box-downscale on capture.
        m_ssaa  = (m_headless && desc.ssaa > 1) ? desc.ssaa : 1;
        m_outW  = desc.width; m_outH = desc.height;
        m_width = desc.width * m_ssaa; m_height = desc.height * m_ssaa;

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
        x3::boot::mark("rhi: vk instance");

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
        f10.samplerAnisotropy = VK_TRUE;   // anisotropic filtering so grazing surfaces aren't blurry

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
            // VK_KHR_ray_tracing_position_fetch (OPTIONAL on top of ray query):
            // lets a ray-query shader read the committed triangle's vertex
            // positions — DDGI's hit-normal source (no SBT / vertex pulls).
            // Absent (older drivers): DDGI stays off; RT AO/reflections unaffected.
            if (m_rtSupported) {
                const std::vector<const char*> pfExts = { "VK_KHR_ray_tracing_position_fetch" };
                if (phys.enable_extensions_if_present(pfExts)) {
                    VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR pff{};
                    pff.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR;
                    pff.rayTracingPositionFetch = VK_TRUE;
                    m_rtPosFetch = phys.enable_extension_features_if_present(pff);
                }
            }
            logInfo(m_rtSupported
                ? (m_rtPosFetch
                    ? "[rhi] RT: ray-query SUPPORTED (+ position fetch) — RT AO/reflections/DDGI available"
                    : "[rhi] RT: ray-query SUPPORTED (no position fetch) — RT AO/reflections available; DDGI off")
                : "[rhi] RT: not available on this device — SSAO/CSM raster fallback");
        }

        x3::boot::mark("rhi: phys-device select");
        vkb::DeviceBuilder db{ phys };
        auto dev_ret = db.build();
        if (!dev_ret) { logError(std::string("[rhi] device: ") + dev_ret.error().message()); return false; }
        m_dev = dev_ret.value();

        auto q   = m_dev.get_queue(vkb::QueueType::graphics);
        auto qfi = m_dev.get_queue_index(vkb::QueueType::graphics);
        if (!q || !qfi) { logError("[rhi] no graphics queue"); return false; }
        m_gfxQueue = q.value();
        m_gfxFamily = qfi.value();

        // D15 Tier 1: a DEDICATED compute queue (compute && !graphics family),
        // if the device has one (Turing+/RDNA+ do; 5090 has several). Optional —
        // absence just pins the GPU cull to the graphics queue (Tier 0).
        {
            auto cq  = m_dev.get_queue(vkb::QueueType::compute);
            auto cqi = m_dev.get_queue_index(vkb::QueueType::compute);
            if (cq && cqi && cqi.value() != m_gfxFamily) {
                m_computeQueue  = cq.value();
                m_computeFamily = cqi.value();
                logInfo("[cull] dedicated compute queue available (family " +
                        std::to_string(m_computeFamily) + ") — Tier 1 eligible");
            }
        }

        logInfo(std::string("[rhi] device ready: ") + phys.name +
                " (Vulkan 1.3, dynamic-rendering + sync2 + descriptor-indexing)" +
                (m_headless ? " [HEADLESS: offscreen target, no surface/swapchain]" : ""));
        x3::boot::mark("rhi: instance+device");

        // VMA allocator (needed by the swapchain's depth image + graphics buffers)
        VmaAllocatorCreateInfo aci{};
        aci.physicalDevice = m_dev.physical_device;
        aci.device = m_dev.device;
        aci.instance = m_inst.instance;
        aci.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        aci.vulkanApiVersion = VK_API_VERSION_1_3;
        if (vmaCreateAllocator(&aci, &m_alloc) != VK_SUCCESS) { logError("[rhi] VMA create failed"); return false; }

        // ZERO-STUTTER: the persistent VkPipelineCache must exist BEFORE the first
        // pipeline is created (createGraphics below) so every compile feeds/hits it.
        createPipelineCache();

        if (m_headless) { if (!createOffscreenTarget(m_width, m_height)) return false; }
        else            { if (!createSwapchain(m_width, m_height)) return false; }
        if (!createPerFrame()) return false;
        if (!createShadowImage()) return false;   // before createGraphics (mesh layout needs set 2)
        x3::boot::mark("rhi: swapchain+frames+shadow");
        if (!createGraphics()) return false;      // builds the shadow depth pipeline at the end
        // D15 GPU cull (r_cullpath). NON-FATAL: a failure logs + leaves the CPU
        // path (r_cullpath 0) as the only option — rendering is unaffected.
        m_gpuCullReady = createGpuCull();
        if (!m_gpuCullReady) logWarn("[cull] GPU cull unavailable — r_cullpath >= 1 falls back to CPU");
        else if (!createHzbTargets()) logWarn("[cull] HZB unavailable — r_hzb stays frustum-only");
        x3::boot::mark("rhi: core pipelines (createGraphics)");
        // BOOT-TIME hook: the upload path (pool + bindless + VMA) is live — let the
        // host kick its async asset warmup overlapped with the rest of this init.
        if (desc.onUploadReady) desc.onUploadReady(desc.onUploadReadyUser);
        if (!createHud()) return false;
        x3::boot::mark("rhi: hud (font atlas + pipelines)");
        if (!createSky()) return false;           // analytic sky (open-world track, task A)
        // Image-based lighting (IBL): build the env/irradiance/prefilter cubes + BRDF
        // LUT objects (set 4 of the mesh pipeline). Default ON for ALL scenes; the
        // actual bake from the sky runs lazily on the first frame (m_iblDirty). If
        // this fails, IBL stays inactive and mesh.frag falls back to the flat ambient.
        if (!createIbl()) { logError("[rhi] IBL init failed; falling back to flat ambient"); m_iblReady = false; }
        else              { m_iblDirty = true; }
        x3::boot::mark("rhi: sky+ibl objects");
        // HDR pipeline + bloom: build the post pipelines (extent-independent) then
        // the HDR scene + bloom-mip targets at the current extent + their sets.
        if (!createPost()) return false;
        if (!createBloomTargets()) return false;
        writePostDescriptors();
        // Glass set-4 resources (scene-copy sampler + per-frame control UBO + sets).
        // After createBloomTargets so the scene-copy view exists. Non-fatal: a
        // failure leaves the glass sets null and the glass pass falls back gracefully.
        if (!createGlassResources()) {
            destroyGlassResources();
            logError("[rhi] glass resources init failed — glass refraction/shimmer disabled (opaque + M1 alpha unaffected)");
        }
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
        x3::boot::mark("rhi: post/ssao/gi/fx pipelines");

        // ===================================================================
        // ZERO-STUTTER boot precompile (docs/ZERO_STUTTER.md step 1).
        // The RT chains (RT-AO, SSR/RT reflections, DDGI) used to be built
        // LAZILY on the first frame that enabled them — a mid-frame
        // vkCreate*Pipelines + vkDeviceWaitIdle, i.e. exactly the UE-style PSO
        // hitch this engine forbids. Build them NOW, at boot, on any device
        // that could ever run them. The ensure*Ready() lazy paths remain as a
        // graceful fallback but find m_*Built == true and create nothing.
        //   * RT-AO + reflections: any ray-query device (reflections' SSR-only
        //     compute pipeline is also used on non-RT devices — build it too).
        //   * DDGI: additionally needs position-fetch + the IBL env cube.
        // ===================================================================
        {
            const uint32_t psoBefore = m_psoTotal;
            const double   msBefore  = m_psoCreateMs;
            if (m_rtSupported) ensureRtCore();   // AS module (no PSOs; never mid-frame again)
            if (m_rtSupported && !m_rtaoBuilt) {
                if (createRtao() && createRtaoTargets()) {
                    writeRtaoDescriptors();
                    m_rtaoBuilt = true;
                } else {
                    logError("[rhi] boot precompile: RT-AO chain failed (lazy fallback remains)");
                }
            }
            if (!m_reflBuilt) {                  // SSR works on EVERY device (no RT needed)
                if (createRefl() && createReflTargets()) {
                    writeReflDescriptors();
                    writeSsaoDescriptors();      // point mesh set3 binding2 at the refl buffer
                    m_reflBuilt = true;
                } else {
                    logError("[rhi] boot precompile: reflections chain failed (lazy fallback remains)");
                    destroyRefl();
                }
            }
            if (m_rtSupported && m_rtPosFetch && m_iblEnvCubeView != VK_NULL_HANDLE && !m_ddgiBuilt) {
                if (createDdgi() && createDdgiTargets()) {
                    writeDdgiDescriptors();
                    writeSsaoDescriptors();      // point mesh set3 bindings 3/4 at the atlases
                    m_ddgiBuilt = true;
                } else {
                    logError("[rhi] boot precompile: DDGI chain failed (lazy fallback remains)");
                    destroyDdgi();
                }
            }
            char pbuf[224];
            std::snprintf(pbuf, sizeof(pbuf),
                "[rhi] boot precompile: %u pipelines total (%u RT-chain) in %.1f ms (%s cache) — "
                "rtao=%d refl=%d ddgi=%d; no pipeline may be created after frame 1",
                m_psoTotal, m_psoTotal - psoBefore, m_psoCreateMs,
                m_cacheLoadedBytes ? "warm" : "cold",
                (int)m_rtaoBuilt, (int)m_reflBuilt, (int)m_ddgiBuilt);
            logInfo(pbuf);
            (void)msBefore;
        }
        x3::boot::mark("rhi: RT precompile");
        return true;
    }

    void shutdown() override {
        if (m_dev.device) flushUploadBatch();   // land + free any still-batched uploads
        if (m_dev.device) vkDeviceWaitIdle(m_dev.device);
        // ZERO-STUTTER: persist the pipeline cache FIRST (device still alive) so
        // the next boot compiles near-zero pipelines from scratch.
        savePipelineCache();
        if (m_pipelineCache) { vkDestroyPipelineCache(m_dev.device, m_pipelineCache, nullptr); m_pipelineCache = VK_NULL_HANDLE; }
        shutdownEditorUI();   // editor-only ImGui teardown (no-op if --editor was absent)
        destroyRefl();        // SSR/RT reflections pipelines/targets (no-op if never built)
        destroyDdgi();        // DDGI probe-grid pipelines/atlases (no-op if never built)
        destroyAudioRays();   // RT-acoustics ray batch pipeline/buffers (no-op if never built)
        destroyRt();          // RT AO pipelines/targets + AS module (no-op if never built)
        destroySkinning();
        destroyDebris();
        destroyParticles();
        destroyWater();
        destroyGi();
        destroySsao();
        destroyGlassResources();
        destroyPost();
        destroyIbl();
        destroySky();
        destroyHud();
        destroyGpuCull();
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

    // =====================================================================
    // Editor UI (Dear ImGui, docking) — EDITOR-ONLY (Level Architect P0).
    // None of these allocate or run unless the host calls initEditorUI() (only
    // app/main.cpp does, and only with --editor). The game/headless paths never
    // call them, so the shipping game is byte-for-byte unchanged. resolved imgui
    // == vcpkg 1.92.8 (>= 1.90): fonts AUTO-UPLOAD on the first
    // ImGui_ImplVulkan_RenderDrawData and PipelineRenderingCreateInfo is an
    // InitInfo struct field — so NO explicit ImGui_ImplVulkan_CreateFontsTexture()
    // call is needed (the pre-1.90 branch).
    // =====================================================================
    void initEditorUI(void* glfwWindow) override {
        // A window handle is REQUIRED (the ImGui GLFW backend binds to it for input).
        // A second call is a no-op. In the normal game/headless paths nothing calls
        // this (only app/main.cpp, and only with --editor) so the shipping game is
        // untouched. Headless WITH a window is the --screenshot-editor proof path:
        // ImGui renders into the offscreen color image (colorTargetView in the graph
        // pass resolves to the offscreen view in headless), so the proof verifies the
        // integration rasterizes without a display.
        if (m_imguiInit || !glfwWindow) return;

        // ZERO-STUTTER: declared boundary — editor-UI init is an explicit dev-tool
        // moment (--editor / --screenshot-editor), never mid-gameplay. ImGui creates
        // its own internal pipeline (outside our wrappers) + this descriptor pool.
        m_creationBoundary = true;
        struct BoundaryReset {
            bool* f; ~BoundaryReset() { *f = false; }
        } boundaryReset{ &m_creationBoundary };

        // (1) DEDICATED descriptor pool for ImGui's font + per-texture image samplers.
        // FREE_DESCRIPTOR_SET so ImGui can free/realloc sets. imgui 1.92.8 SPLITS its
        // texture binding into separate SAMPLER + SAMPLED_IMAGE descriptors (no longer
        // a single COMBINED_IMAGE_SAMPLER), so the pool MUST declare all three types or
        // validation warns (AllocateDescriptorSets-WrongType) and a strict driver could
        // return OUT_OF_POOL_MEMORY. 1000 of each is ImGui's documented generous cap.
        // This pool is NEVER shared with the bindless/HUD pools (sharing would corrupt
        // them).
        {
            VkDescriptorPoolSize ps[] = {
                { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
                { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          1000 },
                { VK_DESCRIPTOR_TYPE_SAMPLER,                1000 },
            };
            VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
            pci.maxSets = 1000;
            pci.poolSizeCount = static_cast<uint32_t>(std::size(ps));
            pci.pPoolSizes = ps;
            if (x3CreateDescriptorPool(&pci, nullptr, &m_imguiPool) != VK_SUCCESS) {
                logError("[rhi] editor UI: ImGui descriptor pool create failed");
                return;
            }
        }

        // (2) ImGui context + DOCKING (NOT multi-viewport yet — ViewportsEnable would
        // spawn secondary swapchains the device does not manage; deferred to a later
        // 2nd-monitor piece).
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        ImGui::StyleColorsDark();

        // (3) GLFW backend, install_callbacks=true => ImGui CHAINS the game's existing
        // GLFW key/char/scroll/mouse callbacks (so gameplay input still fires; the host
        // gates viewport input on editorWantsInput()/WantCaptureMouse/Keyboard).
        ImGui_ImplGlfw_InitForVulkan(static_cast<GLFWwindow*>(glfwWindow), true);

        // (4) Vulkan backend, sharing the device's OWN handles + dynamic rendering with
        // the SAME color format as the composite/HUD pass (m_format) so ImGui composites
        // correctly onto the LDR composited image. No render pass object (dynamic
        // rendering); depth UNDEFINED (the editor-UI pass has no depth attachment).
        const uint32_t imageCount = static_cast<uint32_t>(
            m_swapImages.empty() ? 2u : m_swapImages.size());

        // imgui 1.92.8 (2025/09/26+) moved RenderPass/MSAASamples/PipelineRenderingCreateInfo
        // into the PipelineInfoMain sub-struct (the old top-level fields are removed). Use
        // PipelineInfoMain.PipelineRenderingCreateInfo with the SAME color format as the
        // composite/HUD pass (m_format) + depth UNDEFINED (the editor-UI pass has no depth).
        VkPipelineRenderingCreateInfoKHR prci{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
        prci.colorAttachmentCount = 1;
        prci.pColorAttachmentFormats = &m_format;
        prci.depthAttachmentFormat = VK_FORMAT_UNDEFINED;

        ImGui_ImplVulkan_InitInfo init{};
        init.ApiVersion      = VK_API_VERSION_1_3;
        init.Instance        = m_inst.instance;
        init.PhysicalDevice  = m_dev.physical_device;
        init.Device          = m_dev.device;
        init.QueueFamily     = m_gfxFamily;
        init.Queue           = m_gfxQueue;
        init.DescriptorPool  = m_imguiPool;
        init.MinImageCount   = imageCount;
        init.ImageCount      = imageCount;
        init.UseDynamicRendering = true;
        init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        init.PipelineInfoMain.PipelineRenderingCreateInfo = prci;
        if (!ImGui_ImplVulkan_Init(&init)) {
            logError("[rhi] editor UI: ImGui_ImplVulkan_Init failed");
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
            vkDestroyDescriptorPool(m_dev.device, m_imguiPool, nullptr);
            m_imguiPool = VK_NULL_HANDLE;
            return;
        }
        // imgui 1.92.8 (>= 1.90): the font atlas auto-uploads on the first
        // RenderDrawData, so no ImGui_ImplVulkan_CreateFontsTexture() call here.

        m_imguiInit = true;
        logInfo("[rhi] editor UI: Dear ImGui (docking) initialized (imgui 1.92.8, "
                "dynamic-rendering, dedicated descriptor pool)");
    }

    void beginEditorUI() override {
        if (!m_imguiInit) return;
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        // Phase 1: the device only opens the docking ROOT here. The EditorHost (app/
        // editor) submits all panels between beginEditorUI() and endEditorUI(). (P0's
        // device-side dockspace + demo window have moved out to the host.) Pass-through
        // central node so the live scene shows through the dockspace background.
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(),
                                     ImGuiDockNodeFlags_PassthruCentralNode);
    }

    void endEditorUI() override {
        if (!m_imguiInit) return;
        // CPU draw-data build OUTSIDE the command buffer; the editor-UI graph pass in
        // buildAndExecuteGraph() records ImGui_ImplVulkan_RenderDrawData against this.
        // Must run BEFORE endFrame() so the draw data exists when the graph records.
        ImGui::Render();
        m_editorDrawData = ImGui::GetDrawData();
    }

    void shutdownEditorUI() override {
        if (!m_imguiInit) return;
        if (m_dev.device) vkDeviceWaitIdle(m_dev.device);
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        if (m_imguiPool) {
            vkDestroyDescriptorPool(m_dev.device, m_imguiPool, nullptr);
            m_imguiPool = VK_NULL_HANDLE;
        }
        m_imguiInit = false;
        m_editorDrawData = nullptr;
    }

    void editorWantsInput(bool& mouse, bool& kbd) const override {
        if (!m_imguiInit) { mouse = false; kbd = false; return; }
        const ImGuiIO& io = ImGui::GetIO();
        mouse = io.WantCaptureMouse;
        kbd   = io.WantCaptureKeyboard;
    }

    bool editorUIActive() const override { return m_imguiInit; }

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

    void setAmbient(float r, float g, float b) override { m_ambient = glm::vec3(r, g, b); }

    // CPU per-object frustum cull toggle (r_frustumcull). Default ON. When OFF the
    // draw path is byte-identical to before this feature (objectsDrawn == list.size()).
    void setFrustumCullEnabled(bool enabled) override { m_frustumCull = enabled; }

    // D15 GPU cull host requests (resolved per frame in prepareFrameData).
    void setCullPath(int path) override { m_cullPathReq = path; }
    void setHzbEnabled(bool enabled) override { m_hzbEnabled = enabled; }
    void setGpuCullEquivalenceCheck(bool enabled) override {
        if (enabled && !m_cullEquivCheck) { m_cullEquivFrames = 0; m_cullEquivMismatches = 0; }
        m_cullEquivCheck = enabled;
    }
    // vis-unify: host-injected per-frame PVS numbers (room/portal skips + flood ms).
    void setVisHostStats(uint32_t roomsCulled, float pvsMs) override {
        m_visRoomsCulled = roomsCulled; m_visPvsMs = pvsMs;
    }

    void setBloom(float intensity) override { m_bloomIntensity = intensity; }

    // Whole-scene brightness: pre-tonemap exposure multiplier in the composite pass.
    // With auto-exposure ON this is the compensation BIAS on the adapted value.
    void setExposure(float e) override { m_exposure = (e > 0.0f) ? e : 1.0f; }

    // Metal ambient-specular floor strength (mesh.frag IBL path; rides ssao ctrl ibl.w).
    void setMetalAmbient(float s) override { m_metalAmbient = (s >= 0.0f) ? s : 1.0f; }

    // HDR post-stack settings (r_tonemap / r_bloom* / r_autoexposure / r_ae*),
    // synced per frame by the app. Toggling AE on re-arms the adaptation snap so
    // the first adapted frame lands on target instantly (no multi-second crawl).
    void setPostFX(const PostFXParams& p) override {
        if (p.autoExposure && !m_post.autoExposure) m_aeSnap = true;
        // TAA toggled ON: the history image holds stale (or never-written) data —
        // invalidate so the first TAA frame is a clean passthrough, not a blend
        // against garbage. Toggling OFF needs nothing (the passes simply stop).
        if (p.taa && !m_post.taa) m_taaHistoryValid = false;
        m_post = p;
    }

    void setShadowBounds(float cx, float cy, float cz, float halfExtent) override {
        m_shadowOverride  = true;
        m_shadowCenter    = glm::vec3(cx, cy, cz);
        m_shadowOrtho     = halfExtent;
        m_shadowDepthHalf = halfExtent * 1.6f;   // deep enough for tall geometry + sun setback
    }

    // Interior reflection probe: when ON, the IBL environment cube is baked from the
    // SCENE geometry (around the camera) instead of the analytic sky, so glossy metals
    // reflect the dim interior rather than the bright open sky (which blows them out).
    void setIblProbe(bool enable) override {
        if (m_iblProbeScene != enable) { m_iblProbeScene = enable; m_iblDirty = true; }
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
        // IBL: if any sky term that feeds the environment radiance changed, flag the
        // IBL chain dirty so it rebakes the irradiance/prefilter cubes next frame.
        if (std::memcmp(&m_sky, &sp, sizeof(SkyParams)) != 0) m_iblDirty = true;
        m_sky = sp;
    }

    // Global sky-animation time (seconds). Cached + written into the sky UBO's
    // params.z by prepareFrameData(), driving the starfield rotation + any future
    // time-driven celestial motion. Live loop passes elapsed; screenshots a fixed value.
    void setSkyTime(float t) override { m_skyTime = t; }

    // Project a world point -> HUD pixel coords (top-left origin) using the cached render
    // viewProj. false if behind the camera / well off-screen. For monster health bars etc.
    bool rayTracingSupported() const override { return m_rtSupported; }

    void setRtaoParams(const RtaoParams& p) override {
        // Cache a snapshot (re-applied each frame, like setSsaoParams). When RT is
        // unsupported this is a harmless no-op store: the graph never adds the RT
        // chain because m_rtSupported is false. The first time it is enabled on an
        // RT device, beginFrame() lazily inits the AS module + RT-AO pipelines.
        m_rtao = p;
        m_rtao.rays = std::max(1, std::min(32, m_rtao.rays));
    }

    void setReflectionParams(const ReflectionParams& p) override {
        // Cache a snapshot (re-applied each frame, like setRtaoParams). The chain
        // is built LAZILY on first activation in prepareFrameData (a run that never
        // enables r_ssr pays zero init cost) and requires TAA to be active (the TAA
        // history image is the previous-frame color source). rtFallback additionally
        // requires m_rtSupported — Pascal-class devices get SSR-only automatically.
        m_refl = p;
        m_refl.intensity = std::max(0.0f, std::min(1.0f, m_refl.intensity));
    }

    void setDdgiParams(const DdgiParams& p) override {
        // Cache a snapshot (re-applied each frame, like setRtaoParams). The DDGI
        // chain is built LAZILY on first activation (a run that never enables
        // r_ddgi pays zero init cost) and requires ray-query + position-fetch
        // hardware — on anything else this is a harmless store and the graph
        // never adds the DDGI passes (raster ambient is byte-for-byte unchanged).
        m_ddgi = p;
        m_ddgi.countX = std::max(2, std::min(32, m_ddgi.countX));
        m_ddgi.countY = std::max(2, std::min(32, m_ddgi.countY));
        m_ddgi.countZ = std::max(2, std::min(32, m_ddgi.countZ));
        // Bound total probes so the ray buffer stays sane (<= 12288 * 128 rays).
        while (m_ddgi.countX * m_ddgi.countY * m_ddgi.countZ > 12288) {
            if (m_ddgi.countX >= m_ddgi.countZ && m_ddgi.countX > 2)      --m_ddgi.countX;
            else if (m_ddgi.countZ > 2)                                    --m_ddgi.countZ;
            else                                                           --m_ddgi.countY;
        }
        m_ddgi.raysPerProbe = std::max(16, std::min(128, m_ddgi.raysPerProbe));
        m_ddgi.hysteresis    = std::max(0.0f, std::min(0.995f, m_ddgi.hysteresis));
        m_ddgi.hysteresisVis = std::max(0.0f, std::min(0.995f, m_ddgi.hysteresisVis));
        m_ddgi.intensity  = std::max(0.0f, std::min(4.0f, m_ddgi.intensity));
        m_ddgi.bounceGain = std::max(0.0f, std::min(0.98f, m_ddgi.bounceGain));
        m_ddgi.normalBias = std::max(0.0f, std::min(4.0f, m_ddgi.normalBias));
    }

    void setRtShadowParams(const RtShadowParams& p) override {
        // Cache a snapshot (re-applied each frame, like setRtaoParams). On a
        // device without ray query this is a harmless store: the RT mesh
        // pipeline variants are never created, the want-gate below stays false
        // and the plain (bit-identical) pipelines are bound — the same auto-0
        // tier gating DDGI/reflections use.
        m_rtShadows = p;
        m_rtShadows.tier        = std::max(0, std::min(2, m_rtShadows.tier));
        m_rtShadows.sunSizeDeg  = std::max(0.0f, std::min(5.0f, m_rtShadows.sunSizeDeg));
        m_rtShadows.pointMax    = std::max(0, std::min(16, m_rtShadows.pointMax));
        m_rtShadows.pointRadius = std::max(0.0f, std::min(1.0f, m_rtShadows.pointRadius));
    }

    void setGlassDevParams(const GlassDevParams& p) override {
        // Cache a snapshot of the live r_glass_* dev overrides; the glass control
        // UBO picks them up in prepareFrameData each frame.
        m_glassDev = p;
    }

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
        // BOOT-TIME upload batching: SUBMIT any still-recording uploads (queue
        // order + the trailing barrier give this frame full visibility — no CPU
        // wait), and opportunistically retire an already-signaled batch fence so
        // staging buffers don't linger.
        submitUploadBatch();
        waitUploadBatch(false);
        // ZERO-STUTTER: from here on, ANY pipeline/module/pool creation outside a
        // declared boundary is LATE (the x3Create* wrappers count + assert it).
        m_firstFrameBegun = true;
        // Per-frame attribution counters for the spike log (consumed by
        // recordFramePacing() at the end of this frame's endFrame()).
        m_psoThisFrame = m_modulesThisFrame = m_poolsThisFrame = 0;
        m_allocsThisFrame = m_asBuildsThisFrame = 0;
        m_iblBakedThisFrame = m_recreatedThisFrame = false;
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
        m_planetDraws.clear();   // planet body draws (FORGE3D port) reset per frame
        // Particle/decal per-frame staging (capacity persists -> no heap churn).
        m_partAdd.clear();
        m_partAlpha.clear();
        m_decals.clear();
        m_partAddCount = m_partAlphaCount = m_decalCount = 0;
        // Debris compute/draw are re-armed per frame by gpuDebrisStep/gpuDebrisDraw.
        m_debrisStepPending = false;
        m_debrisDrawPending = false;
        // GPU skinning queue is NOT cleared here. The host uploads bone palettes via
        // game.tick()/applyLocomotion() -> setSkinnedPalette() which run BEFORE
        // beginFrame() in the main loop; clearing here wiped that queue before the
        // frame's skin-compute pass ran, freezing every skinned character in bind pose.
        // It is consumed + cleared in endFrame() right after buildAndExecuteGraph().
        // (m_frameIdx is stable across the frame, so the uploaded palette slot matches
        // the slot the dispatch reads.)
        m_framePrepared = false;
        m_frameCmdCount = 0; m_frameCmdOpaque = 0;
        m_asyncCullThisFrame = false;   // re-armed by the graph build (Tier 1)
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
        // Per-scene sun: rake the shadow box along the SAME direction the sky disk +
        // mesh.frag lighting use (m_sky.sunDir; defaults to (0.4,1,0.3) when no sky is set,
        // so Level1's shadows are unchanged).
        const glm::vec3 sunDir = glm::normalize(glm::vec3(m_sky.sunDir[0], m_sky.sunDir[1], m_sky.sunDir[2]));
        // Default: ~45 m box following the camera (Level1). Override (setShadowBounds): a fixed
        // box on a scene AABB so large scenes (showroom) fall inside the shadow map.
        const glm::vec3 center = m_shadowOverride ? m_shadowCenter : m_camPos;
        const float     ortho  = m_shadowOverride ? m_shadowOrtho : kShadowOrtho;
        const float     dHalf  = m_shadowOverride ? m_shadowDepthHalf : kShadowDepthHalf;
        const glm::vec3 eye = center + sunDir * dHalf;
        // Up vector not parallel to sunDir.
        const glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
        glm::vec3 upPick = (std::abs(glm::dot(sunDir, up)) > 0.99f) ? glm::vec3(0,0,1) : up;
        glm::mat4 view = glm::lookAt(eye, center, upPick);
        // Ortho with Vulkan's [0,1] Z (GLM_FORCE_DEPTH_ZERO_TO_ONE), reverse-Y clip.
        glm::mat4 proj = glm::ortho(-ortho, ortho, -ortho, ortho, 0.0f, 2.0f * dHalf);
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

        // Planet bodies (FORGE3D port): drawn AFTER the opaque meshes (so they
        // composite against the established opaque depth), before transparent/HUD.
        // Dedicated pipeline + push constant; no-op when no planet was submitted.
        recordPlanetDraws(cmd);
    }

    // Record the queued planet body draws into the (already-open) main color pass.
    // Reuses the mesh path's set0 (bindless textures) + set1 (object SSBO + camera
    // UBO) descriptor sets — rebinding them to the planet pipeline layout (which
    // declares the SAME set0/set1 layouts) so the bind is valid even though we just
    // bound them for the mesh path. Per planet: push the model + texture indices,
    // bind the sphere's vertex/index buffers, and one indexed draw.
    void recordPlanetDraws(VkCommandBuffer cmd) {
        if (m_planetDraws.empty() || !m_planetPipelines[PT_Moon]) return;
        auto& fr = m_frames[m_frameIdx];
        // set0 = bindless textures, set1 = object SSBO (unused) + camera UBO. All
        // per-type pipelines share m_planetPipelineLayout, so bind the sets ONCE.
        VkDescriptorSet sets[2] = { m_bindlessSet, fr.objSet };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_planetPipelineLayout,
                                0, 2, sets, 0, nullptr);
        // Single per-draw emitter (resolve mesh + pipeline, push constants, draw).
        auto emit = [&](const PlanetDraw& pd) {
            auto mit = m_meshes.find(pd.meshId);
            if (mit == m_meshes.end()) return;
            uint32_t ti = pd.typeIndex < (uint32_t)PT_Count ? pd.typeIndex : (uint32_t)PT_Moon;
            VkPipeline pipe = m_planetPipelines[ti];
            if (!pipe) pipe = m_planetPipelines[PT_Moon];   // fall back to Moon if a type failed
            const Mesh& mh = mit->second;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            PlanetPush push{};
            std::memcpy(push.model, pd.model, sizeof(push.model));
            std::memcpy(push.tex, pd.tex, sizeof(push.tex));
            push.uTime = pd.uTime;
            vkCmdPushConstants(cmd, m_planetPipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(PlanetPush), &push);
            VkDeviceSize off = 0;
            VkBuffer vb = mh.drawVbo(m_frameIdx);
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
            vkCmdBindIndexBuffer(cmd, mh.ibo, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, mh.indexCount, 1, 0, 0, 0);
        };
        // PASS 1: opaque bodies (typeIndex < PT_OpaqueCount) establish color + depth.
        for (const PlanetDraw& pd : m_planetDraws)
            if (pd.typeIndex < (uint32_t)PT_OpaqueCount) emit(pd);
        // PASS 2: transparent glow layers (atmosphere / corona / ring) composite OVER
        // the bodies (depth-test LEQUAL, depth-write OFF), so they read against the
        // bodies' depth without occluding each other.
        for (const PlanetDraw& pd : m_planetDraws)
            if (pd.typeIndex >= (uint32_t)PT_OpaqueCount) emit(pd);
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

        // ---- IBL rebake (default ON): if the sky changed (or first frame), rebuild
        // the irradiance/prefilter cubes from the analytic sky on a one-time submit
        // (its own fence) that completes BEFORE this frame's command buffer runs, so
        // mesh.frag set 4 samples a valid environment. Cheap + rare (only on sky
        // change), so per-frame cost is just the three texture fetches in the shader.
        if (m_iblReady && m_iblDirty) {
            m_iblBakedThisFrame = true;
            const bool firstBake = !m_iblBaked;
            const auto tb0 = std::chrono::steady_clock::now();
            regenIblFromSky();
            if (firstBake) {     // boot receipt only; later sky-change rebakes stay quiet
                char bb[96];
                std::snprintf(bb, sizeof(bb), "[boot] ibl first bake: %.1f ms (incl. pending upload retire)",
                              std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - tb0).count());
                logInfo(bb);
            }
        }

        // ---- Hardware ray-tracing (RT AO) build, gated + default OFF ----------
        // Only when r_rtao is on AND the device supports RT: lazily build the RT-AO
        // pipelines/targets, then (re)build the scene BLAS/TLAS from THIS frame's
        // draw list (still valid here) + fill the compute UBO. The AS builds are
        // synchronous one-time submits on the graphics queue (separate command pool
        // + fence) that complete BEFORE the frame command buffer is submitted, so
        // the TLAS is ready when the ray-query compute pass runs. If anything fails,
        // m_rtaoActiveThisFrame stays false and the graph adds NO RT passes — the
        // raster/SSAO path is byte-for-byte unchanged.
        m_rtaoActiveThisFrame = false;
        m_reflRtThisFrame = false;
        m_ddgiActiveThisFrame = false;
        m_rtShadowsActiveThisFrame = false;
        // Reflections' ray-query fallback, DDGI and RT soft shadows reuse the
        // SAME BLAS/TLAS this block builds — one AS, four consumers (RT AO +
        // RT reflections + DDGI + r_rtshadows).
        const bool reflRtWant = m_reflActiveThisFrame && m_refl.rtFallback
                             && (m_reflPipeRt != VK_NULL_HANDLE);
        const bool ddgiWant = m_ddgiWantThisFrame;   // decided in prepareFrameData
        const bool rtshWant = m_rtShadowsWantThisFrame;
        // RT ACOUSTICS: while traceAudioRays() keeps getting called (countdown
        // re-armed each call), keep the scene TLAS built even with every RT
        // SCREEN effect off — audio rays are a first-class TLAS consumer.
        const bool audioWant = m_audioRaysWantFrames > 0;
        if (m_audioRaysWantFrames > 0) --m_audioRaysWantFrames;
        if (m_rtSupported && (m_rtao.enabled || reflRtWant || ddgiWant || rtshWant || audioWant)) {
            const bool coreReady = m_rtao.enabled ? ensureRtaoReady() : ensureRtCore();
            if (coreReady && buildRtSceneAS() && m_rt.lastInstanceCount() > 0) {
                if (m_rtao.enabled) {
                    prepareRtaoUbo();
                    m_rtaoActiveThisFrame = true;
                }
                if (reflRtWant) m_reflRtThisFrame = true;
                if (ddgiWant && m_ddgiBuilt) {
                    prepareDdgiUbo();
                    m_ddgiActiveThisFrame = true;
                }
                // RT soft shadows: bind the mesh_rt pipelines this frame ONLY
                // once the set3 TLAS descriptor is live (written on the first
                // TLAS build / handle change; the variant statically uses it).
                if (rtshWant && m_meshTlasWritten)
                    m_rtShadowsActiveThisFrame = true;
            }
        }
        // Fill the per-frame reflection UBO (matrices captured by prepareFrameData).
        if (m_reflActiveThisFrame) prepareReflUbo();

        const bool wantCapture = (m_captureArmed && m_captureBuf &&
                                  m_captureW == m_extent.width && m_captureH == m_extent.height);
        buildAndExecuteGraph(fr.cmd, imageIndex, wantCapture);
        // GPU skinning queue consumed by the skin-compute pass recorded inside the graph
        // above. Clear it HERE (moved out of beginFrame): the host uploads palettes via
        // setSkinnedPalette() before beginFrame, so clearing in beginFrame wiped them and
        // every skinned character (enemies, Martinez, rescue companions) froze in bind
        // pose. Cleared post-dispatch, the armed queue survives to the compute pass.
        m_skinPending.clear();
        m_skinStepPending = false;
        const bool capturedThisFrame = wantCapture;
        m_captureArmed = false; // consume the arm regardless

        vkEndCommandBuffer(fr.cmd);

        // HEADLESS: no acquire semaphore to wait on and no present, so submit with
        // NO wait/signal semaphores. The inFlight fence alone serializes the ring
        // slot (beginFrame waits it before reusing the slot's offscreen image +
        // command buffer). WINDOWED keeps the acquire-wait + renderFinished-signal
        // that the present below consumes.
        // ---- D15 TIER 1: async cull on the dedicated compute queue ----------
        // Record + submit the cull dispatch FIRST (signals the timeline), then
        // make the graphics submit wait on it at the exact consuming stages
        // (DRAW_INDIRECT for instanceCount, VERTEX_SHADER for visibleInstance[]).
        // No barriers inside the compute cmd — the semaphore signal/wait pair is
        // the cross-queue execution + memory dependency (sync2 semantics).
        uint64_t cullWaitValue = 0;
        if (m_asyncCullThisFrame && m_asyncCullReady) {
            vkResetCommandPool(m_dev.device, fr.cullPool, 0);
            VkCommandBufferBeginInfo cbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(fr.cullCmd, &cbi);
            m_gpuCull.recordCullBody(fr.cullCmd, m_asyncCullInputs,
                                     /*useHzb=*/false, /*gfxQueueBarriers=*/false);
            vkEndCommandBuffer(fr.cullCmd);

            cullWaitValue = ++m_cullTimelineValue;
            VkSemaphoreSubmitInfo csig{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
            csig.semaphore = m_cullTimeline;
            csig.value = cullWaitValue;
            csig.stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            VkCommandBufferSubmitInfo ccmd{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
            ccmd.commandBuffer = fr.cullCmd;
            VkSubmitInfo2 csubmit{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
            csubmit.commandBufferInfoCount = 1; csubmit.pCommandBufferInfos = &ccmd;
            csubmit.signalSemaphoreInfoCount = 1; csubmit.pSignalSemaphoreInfos = &csig;
            vkQueueSubmit2(m_computeQueue, 1, &csubmit, VK_NULL_HANDLE);
        }

        VkSemaphoreSubmitInfo waits[2]{};
        uint32_t waitCount = 0;
        if (!m_headless) {
            waits[waitCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            waits[waitCount].semaphore = fr.imageAvailable;
            waits[waitCount].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            ++waitCount;
        }
        if (cullWaitValue) {
            waits[waitCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            waits[waitCount].semaphore = m_cullTimeline;
            waits[waitCount].value = cullWaitValue;
            waits[waitCount].stageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                                         VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
            ++waitCount;
        }

        VkSemaphoreSubmitInfo signalS{};
        signalS.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signalS.semaphore = m_headless ? VK_NULL_HANDLE : m_renderFinished[imageIndex];
        signalS.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;

        VkCommandBufferSubmitInfo cmdS{};
        cmdS.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmdS.commandBuffer = fr.cmd;

        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.waitSemaphoreInfoCount   = waitCount;
        submit.pWaitSemaphoreInfos      = waitCount ? waits : nullptr;
        submit.commandBufferInfoCount   = 1;   submit.pCommandBufferInfos = &cmdS;
        submit.signalSemaphoreInfoCount = m_headless ? 0u : 1u;
        submit.pSignalSemaphoreInfos    = m_headless ? nullptr : &signalS;
        vkQueueSubmit2(m_gfxQueue, 1, &submit, fr.inFlight);
        m_asyncCullThisFrame = false;

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
        // D15 GPU cull counters (read back with frames-in-flight latency).
        m_building.gpuCullPath    = m_cullPathActive;
        m_building.gpuCullTested  = m_lastCullStats.tested;
        m_building.gpuCullDrawn   = m_lastCullStats.drawn;
        m_building.gpuCullFrustum = m_lastCullStats.frustumCulled;
        m_building.gpuCullHzb     = m_lastCullStats.hzbCulled;
        m_building.gpuCullExpected        = m_lastCullExpected;
        m_building.gpuCullEquivFrames     = m_cullEquivFrames;
        m_building.gpuCullEquivMismatches = m_cullEquivMismatches;
        // ---- vis-unify: device caps + host PVS numbers + per-stage times ----
        m_building.gpuCullSupported   = m_gpuCullReady;
        m_building.asyncCullSupported = m_asyncCullReady;
        m_building.hzbSupported       = m_gpuCullReady && m_hzbReady;
        m_building.visRoomsCulled     = m_visRoomsCulled;
        m_building.visPvsMs           = m_visPvsMs;
        m_building.cullCpuMs          = m_cullCpuMs;
        m_building.cullGpuMs          = m_cullGpuMs;
        m_building.hzbGpuMs           = m_hzbGpuMs;
        // TLAS mutation instrumentation (folded base path: still sync-waits).
        m_building.tlasBuilds    = m_tlasBuilds;
        m_building.tlasSyncWaits = m_tlasSyncWaits;
        m_building.tlasGrows     = m_tlasGrows;
        m_building.tlasCpuMs     = m_tlasCpuMs;
        m_building.tlasCpuMsMax  = m_tlasCpuMsMax;
        m_lastStats = m_building;

        // ZERO-STUTTER: record this frame in the pacing ring + spike log.
        recordFramePacing();
    }

    RenderStats stats() const override { return m_lastStats; }

    // ---- ZERO-STUTTER telemetry snapshot (r_frametelemetry / --test-framepacing).
    // Percentiles over the post-warmup ring; the late-creation counters are the
    // strict-PSO audit receipts (see the x3Create* wrappers + recordFramePacing).
    FramePacing framePacing() const override {
        FramePacing p{};
        p.psoLate     = m_psoLate;
        p.modulesLate = m_modulesLate;
        p.poolsLate   = m_poolsLate;
        p.psoTotal    = m_psoTotal;
        p.psoBootMs   = (float)m_psoCreateMs;
        p.cacheLoaded = m_cacheLoadedBytes;
        p.spikes      = m_spikeCount;
        p.spikesUnattributed = m_spikeCleanCount;
        p.samples     = (uint32_t)m_paceRing.size();
        if (m_paceRing.empty()) return p;
        std::vector<float> cpu, gpu;
        cpu.reserve(m_paceRing.size()); gpu.reserve(m_paceRing.size());
        for (const PaceSample& s : m_paceRing) { cpu.push_back(s.cpuMs); gpu.push_back(s.gpuMs); }
        std::sort(cpu.begin(), cpu.end());
        std::sort(gpu.begin(), gpu.end());
        auto pct = [](const std::vector<float>& v, double q) {
            const size_t i = std::min(v.size() - 1, (size_t)(q * (double)(v.size() - 1) + 0.5));
            return v[i];
        };
        p.cpuP50 = pct(cpu, 0.50); p.cpuP95 = pct(cpu, 0.95); p.cpuP99 = pct(cpu, 0.99);
        p.cpuP999 = pct(cpu, 0.999); p.cpuMax = cpu.back();
        p.gpuP50 = pct(gpu, 0.50); p.gpuP95 = pct(gpu, 0.95); p.gpuP99 = pct(gpu, 0.99);
        p.gpuP999 = pct(gpu, 0.999); p.gpuMax = gpu.back();
        return p;
    }

    void setPacingParams(const PacingParams& pp) override {
        m_pacing = pp;
        if (m_pacing.warmupFrames < 1) m_pacing.warmupFrames = 1;
        if (m_pacing.spikeFactor < 1.0f) m_pacing.spikeFactor = 1.0f;
        if (m_pacing.floorMs < 0.0f) m_pacing.floorMs = 0.0f;
    }

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
            if (x3vmaCreateBuffer(&bci, &vaci, &m_captureBuf, &m_captureAlloc, &rinfo) != VK_SUCCESS) {
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
        // SSAA: box-downscale each ssaa x ssaa block of the supersampled render to one output
        // texel (true supersampling — antialiases geometry edges + texture detail, no softening).
        if (m_ssaa > 1 && (W % m_ssaa) == 0 && (H % m_ssaa) == 0) {
            const uint32_t ow = W / m_ssaa, oh = H / m_ssaa, n = m_ssaa * m_ssaa;
            std::vector<uint8_t> out((size_t)ow * oh * 4);
            for (uint32_t y = 0; y < oh; ++y)
                for (uint32_t x = 0; x < ow; ++x) {
                    uint32_t a0 = 0, a1 = 0, a2 = 0;
                    for (uint32_t sy = 0; sy < m_ssaa; ++sy)
                        for (uint32_t sx = 0; sx < m_ssaa; ++sx) {
                            const uint8_t* s = rgba.data() + (((size_t)(y * m_ssaa + sy)) * W + (x * m_ssaa + sx)) * 4;
                            a0 += s[0]; a1 += s[1]; a2 += s[2];
                        }
                    uint8_t* d = out.data() + (((size_t)y) * ow + x) * 4;
                    d[0] = (uint8_t)(a0 / n); d[1] = (uint8_t)(a1 / n); d[2] = (uint8_t)(a2 / n); d[3] = 255;
                }
            const int rcd = stbi_write_png(path, (int)ow, (int)oh, 4, out.data(), (int)(ow * 4));
            if (rcd == 0) { logError(std::string("[rhi] captureFrame: PNG write failed: ") + path); return false; }
            logInfo(std::string("[rhi] captureFrame: wrote ") + path + " (" + std::to_string(ow) + "x" +
                    std::to_string(oh) + " <- " + std::to_string(W) + "x" + std::to_string(H) + " SSAA " +
                    std::to_string(m_ssaa) + "x)");
            return true;
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
        if (x3vmaCreateBuffer(&bci, &vaci, &readback, &readbackAlloc, &rinfo) != VK_SUCCESS) {
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
        // Parallel preload safe: staging alloc + memcpy run UNLOCKED (VMA is
        // internally synchronized) so concurrent loaders overlap their copies;
        // the shared batch-record happens under the lock inside
        // createDeviceLocalBuffer, and the registry write locks below.
        Mesh m{};
        const VkDeviceSize vbBytes = (VkDeviceSize)vcount * sizeof(MeshVertex);
        const VkDeviceSize ibBytes = (VkDeviceSize)icount * sizeof(uint32_t);
        // When hardware RT is available, the mesh's vertex/index buffers must be
        // readable as BLAS build inputs (SHADER_DEVICE_ADDRESS) + flagged as AS
        // build input. These extra usage flags do NOT change how the raster path
        // binds/draws the buffers (a vertex buffer with extra usage is still a
        // vertex buffer), so the rasterized output is byte-for-byte identical; they
        // are only added at all when RT is supported (non-RT devices get the exact
        // original usage). The BLAS itself is built lazily on first RT use.
        VkBufferUsageFlags vbUsage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        VkBufferUsageFlags ibUsage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        if (m_rtSupported) {
            const VkBufferUsageFlags rtUsage =
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
            vbUsage |= rtUsage; ibUsage |= rtUsage;
        }
        if (!createDeviceLocalBuffer(verts, vbBytes, vbUsage, m.vbo, m.vboAlloc)) return {};
        if (!createDeviceLocalBuffer(idx, ibBytes, ibUsage, m.ibo, m.iboAlloc)) {
            vmaDestroyBuffer(m_alloc, m.vbo, m.vboAlloc); return {};
        }
        m.indexCount = icount;
        m.vertexCount = vcount;
        // Bake the model-space bounding sphere for the CPU frustum cull (r_frustumcull).
        computeLocalSphere(verts, vcount, m.boundsCenter, m.boundsRadius);
        std::lock_guard<std::recursive_mutex> lk(m_uploadMu);   // registry write
        // CPU local-space AABB, computed once from the submitted vertices (the
        // world-map tile bake reads it back via meshBounds — no GPU readback).
        m.bmin[0] = m.bmin[1] = m.bmin[2] =  std::numeric_limits<float>::max();
        m.bmax[0] = m.bmax[1] = m.bmax[2] = -std::numeric_limits<float>::max();
        for (uint32_t i = 0; i < vcount; ++i) {
            for (int a = 0; a < 3; ++a) {
                const float v = verts[i].pos[a];
                if (v < m.bmin[a]) m.bmin[a] = v;
                if (v > m.bmax[a]) m.bmax[a] = v;
            }
        }
        uint32_t id = m_nextMeshId++;
        m_meshes.emplace(id, m);
        return { id };
    }

    bool meshBounds(MeshHandle h, float outMin[3], float outMax[3]) const override {
        auto it = m_meshes.find(h.id);
        if (it == m_meshes.end() || it->second.vertexCount == 0) return false;
        for (int a = 0; a < 3; ++a) { outMin[a] = it->second.bmin[a]; outMax[a] = it->second.bmax[a]; }
        return true;
    }

    void destroyMesh(MeshHandle h) override {
        std::lock_guard<std::recursive_mutex> lk(m_uploadMu);   // parallel preload safe
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
        // Drop this mesh's RT BLAS (if one was built). The TLAS no longer references
        // a destroyed mesh because m_drawRecords for it stop arriving; the next
        // TLAS rebuild simply omits it. Safe to free now — the AS build is a
        // synchronous one-time submit, never in flight on the render queue.
        if (m_rtSupported && m_rt.hasBlas(h.id)) {
            if (m_dev.device) vkDeviceWaitIdle(m_dev.device);
            m_rt.destroyBlas(h.id);
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
        // Keep the frustum-cull bounds in sync with the new pose (cheap; skinned
        // meshes are typically marked ALWAYS_VISIBLE so this is mostly belt-and-braces).
        computeLocalSphere(verts, vcount, m.boundsCenter, m.boundsRadius);

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
                if (x3vmaCreateBuffer(&bci, &vaci, &nb, &na, &ai) != VK_SUCCESS) {
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
        // Parallel preload safe: the staging alloc + (up to ~67 MB) pixel memcpy in
        // createSampledTexture run UNLOCKED so concurrent loaders overlap; only the
        // shared batch-record (inside createSampledTexture) and the bindless/
        // registry writes below serialize.
        Texture t{};
        if (!createSampledTexture(rgba8, w, h, srgb, t)) return {};
        std::lock_guard<std::recursive_mutex> lk(m_uploadMu);
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
        std::lock_guard<std::recursive_mutex> lk(m_uploadMu);   // parallel preload safe
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

    // Glass / transparent draw. Same payload as drawMeshEmissive plus a GlassMaterial:
    // the per-object row is flagged GLASS so mesh.frag DISCARDs it (opaque pass) and
    // the transparent glass pass draws it (glass.frag). M1 uses opacity (-> alpha) +
    // tint/refraction/roughness/specular carried for later milestones. POD only.
    void drawMeshGlass(const FrameContext& fc, MeshHandle mesh, TextureHandle baseColor,
                       const float baseColorFactor[4], const float emissive[4],
                       const GlassMaterial& glass, const float model[16]) override {
        // baseColorFactor's alpha is overridden by the material opacity so the glass
        // body's see-through amount is the single primary dial (spec §2).
        float factor[4] = {
            baseColorFactor ? baseColorFactor[0] : 1.0f,
            baseColorFactor ? baseColorFactor[1] : 1.0f,
            baseColorFactor ? baseColorFactor[2] : 1.0f,
            glass.opacity };
        drawMeshInternal(fc, mesh, baseColor, TextureHandle{}, TextureHandle{}, factor, emissive,
                         model, /*alphaMask=*/false, /*alphaBlend=*/false, TextureHandle{},
                         TextureHandle{}, 1.0f, kFlagGlass, &glass);
    }

    void drawMeshEmissive(const FrameContext& fc, MeshHandle mesh, TextureHandle baseColor,
                          const float baseColorFactor[4], const float emissive[4],
                          const float model[16]) override {
        // Forward to the PBR path with no normal/MR maps (identical behaviour).
        drawMeshPBR(fc, mesh, baseColor, TextureHandle{}, TextureHandle{},
                    baseColorFactor, emissive, model);
    }

    // PBR public entry: forwards to the shared internal builder with no glass.
    void drawMeshPBR(const FrameContext& fc, MeshHandle mesh, TextureHandle baseColor,
                     TextureHandle normal, TextureHandle metalRough,
                     const float baseColorFactor[4], const float emissive[4],
                     const float model[16], bool alphaMask = false, bool alphaBlend = false, TextureHandle emissiveTex = {},
                     TextureHandle detailTex = {}, float detailUvScale = 1.0f,
                     float clearcoat = 0.0f, float clearcoatRough = 0.05f) override {
        drawMeshInternal(fc, mesh, baseColor, normal, metalRough, baseColorFactor, emissive,
                         model, alphaMask, alphaBlend, emissiveTex, detailTex, detailUvScale,
                         /*extraFlags=*/0u, /*glass=*/nullptr, clearcoat, clearcoatRough);
    }

    // Shared draw record append. The opaque/emissive/PBR/glass paths differ only by
    // the optional PBR maps, the alpha mode, `extraFlags` (GLASS bit), and the
    // optional GlassMaterial (nullptr for every non-glass path).
    void drawMeshInternal(const FrameContext& fc, MeshHandle mesh, TextureHandle baseColor,
                          TextureHandle normal, TextureHandle metalRough,
                          const float baseColorFactor[4], const float emissive[4],
                          const float model[16], bool alphaMask, bool alphaBlend,
                          TextureHandle emissiveTex, TextureHandle detailTex, float detailUvScale,
                          uint32_t extraFlags, const GlassMaterial* glass,
                          float clearcoat = 0.0f, float clearcoatRough = 0.05f) {
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
        // the pad fields. All other draws set the TERRAIN bit off and are unchanged.
        // `extraFlags` carries the GLASS bit from drawMeshGlass (mutually exclusive).
        uint32_t texIndex = 0;
        uint32_t flags = extraFlags;
        if (m_terrainMarkerId != 0 && baseColor.id == m_terrainMarkerId) {
            flags    |= kFlagTerrain;
            texIndex  = m_terrainTexIdx[0];   // grass index (sane default sample)
        } else if (baseColor.valid()) {
            auto tit = m_textures.find(baseColor.id);
            if (tit != m_textures.end()) texIndex = tit->second.bindlessIndex;
        }
        // Resolve the optional PBR maps to bindless indices (0 == none -> the
        // fragment shader skips its PBR branch and shades exactly as before).
        uint32_t normalIdx = 0, mrIdx = 0;
        if (normal.valid()) {
            auto it = m_textures.find(normal.id);
            if (it != m_textures.end()) normalIdx = it->second.bindlessIndex;
        }
        if (metalRough.valid()) {
            auto it = m_textures.find(metalRough.id);
            if (it != m_textures.end()) mrIdx = it->second.bindlessIndex;
        }

        DrawRecord r;
        r.meshId      = mesh.id;
        // texIndex high bits flag material alpha mode for the fragment shader (bindless indices
        // are < kMaxTextures = 4096, so bits 30/31 are free): bit31 = MASK (alpha-cutout discard),
        // bit30 = BLEND (apply the glass-opacity floor). mesh.frag masks them off before sampling.
        r.texIndex    = texIndex | (alphaMask ? 0x80000000u : 0u) | (alphaBlend ? 0x40000000u : 0u);
        // `flags` carries TERRAIN (bit0) + GLASS (bit1); terrain detail idx ride in the pack fields.
        r.flags       = flags;
        // Pack the four detail indices into two uints: pad1 = grass<<16 | rock,
        // pad2 = snow<<16 | sand (each well under 65535 — kMaxTextures = 4096).
        r.terrainPack1 = (m_terrainTexIdx[0] << 16) | (m_terrainTexIdx[1] & 0xFFFFu);
        r.terrainPack2 = (m_terrainTexIdx[2] << 16) | (m_terrainTexIdx[3] & 0xFFFFu);
        // CLEARCOAT (car paint): reuse the SPARE pack1 lane — a clearcoat draw is
        // never the terrain marker, so the lane is free. 8.8 fixed point:
        // low byte = intensity*255, next byte = roughness*255. flags bit2 gates
        // the fragment lobe; every non-clearcoat draw is byte-identical.
        if (clearcoat > 0.001f && (r.flags & kFlagTerrain) == 0u) {
            const float ccI = clearcoat      < 0.0f ? 0.0f : (clearcoat      > 1.0f ? 1.0f : clearcoat);
            const float ccR = clearcoatRough < 0.0f ? 0.0f : (clearcoatRough > 1.0f ? 1.0f : clearcoatRough);
            r.flags |= kFlagClearcoat;
            r.terrainPack1 = ((uint32_t)(ccR * 255.0f + 0.5f) << 8)
                           |  (uint32_t)(ccI * 255.0f + 0.5f);
        }
        uint32_t emisIdx = 0;
        if (emissiveTex.valid()) {
            auto it = m_textures.find(emissiveTex.id);
            if (it != m_textures.end()) emisIdx = it->second.bindlessIndex;
        }
        r.normalTexIndex   = normalIdx;
        r.mrTexIndex       = mrIdx;
        r.emissiveTexIndex = emisIdx;
        // HDRP micro-detail map: resolve to bindless + pack with the UV tiling. Low 20
        // bits = detail bindless idx, high 12 = uvScale*64 (tiling 0..63.98). 0 = none.
        uint32_t detailIdx = 0;
        if (detailTex.valid()) {
            auto it = m_textures.find(detailTex.id);
            if (it != m_textures.end()) detailIdx = it->second.bindlessIndex;
        }
        if (detailIdx != 0) {
            uint32_t uvf = (uint32_t)std::min(4095.0f, std::max(0.0f, detailUvScale * 64.0f));
            r.detailPacked = (detailIdx & 0xFFFFFu) | (uvf << 20);
        }
        r.alphaBlend       = alphaBlend;
        std::memcpy(r.model, model, sizeof(r.model));
        if (baseColorFactor) std::memcpy(r.factor, baseColorFactor, sizeof(r.factor));
        else { r.factor[0] = r.factor[1] = r.factor[2] = r.factor[3] = 1.0f; }
        if (emissive) std::memcpy(r.emissive, emissive, sizeof(r.emissive));
        else { r.emissive[0] = r.emissive[1] = r.emissive[2] = r.emissive[3] = 0.0f; }
        // GLASS material (M2-M4): refraction/roughness/specular + tint. Zeroed for
        // the opaque path so its SSBO rows carry no stray glass state.
        if (glass) {
            r.glassParams[0] = glass->refraction; r.glassParams[1] = glass->roughness;
            r.glassParams[2] = glass->specular;   r.glassParams[3] = 0.0f;
            r.glassTint[0]   = glass->tint[0];    r.glassTint[1]   = glass->tint[1];
            r.glassTint[2]   = glass->tint[2];    r.glassTint[3]   = 0.0f;
        } else {
            r.glassParams[0] = r.glassParams[1] = r.glassParams[2] = r.glassParams[3] = 0.0f;
            r.glassTint[0]   = r.glassTint[1]   = r.glassTint[2]   = r.glassTint[3]   = 0.0f;
        }
        m_drawRecords.push_back(r);
    }

    // ---- Procedural planet body (FORGE3D port) -----------------------------
    // Queue a planet draw for THIS frame: resolve each TextureHandle to its
    // bindless index (same map lookup drawMeshPBR uses), confirm the mesh exists,
    // and push a PlanetDraw entry. The actual draw is recorded in recordMainPassBody
    // AFTER the opaque mesh multidraw (dedicated planet pipeline + push constant).
    void drawPlanet(const FrameContext& fc, MeshHandle mesh, const float model[16],
                    uint32_t typeIndex, const TextureHandle* maps, uint32_t mapCount,
                    float uTime) override {
        if (!fc.valid || !m_planetPipelines[PT_Moon] || !model) return;
        if (m_meshes.find(mesh.id) == m_meshes.end()) return; // unknown mesh -> skip
        auto idx = [this](TextureHandle h) -> uint32_t {
            if (!h.valid()) return 0;                         // 0 == built-in white
            auto it = m_textures.find(h.id);
            return it != m_textures.end() ? it->second.bindlessIndex : 0u;
        };
        PlanetDraw pd{};
        std::memcpy(pd.model, model, sizeof(pd.model));
        uint32_t n = (mapCount > 12u) ? 12u : mapCount;       // clamp to the 12 slots
        for (uint32_t i = 0; i < 12u; ++i)
            pd.tex[i] = (maps && i < n) ? idx(maps[i]) : 0u;  // resolve; zero the rest
        pd.uTime     = uTime;
        pd.typeIndex = (typeIndex < (uint32_t)PT_Count) ? typeIndex : (uint32_t)PT_Moon;
        pd.meshId    = mesh.id;
        m_planetDraws.push_back(pd);
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

    // Textured HUD rectangle sampling an app-created texture (world-map tiles).
    // Same vertex ring / deferred-record path as drawHudQuad; the record carries
    // the texture id so recordHudDraws binds it instead of the white texel.
    void drawHudImage(const FrameContext& fc, TextureHandle tex,
                      float xPx, float yPx, float wPx, float hPx,
                      const float rgba[4],
                      float u0, float v0, float u1, float v1) override {
        if (!fc.valid || !m_hudPipeline || !tex.valid()) return;
        const float c[4] = { rgba ? rgba[0] : 1.0f, rgba ? rgba[1] : 1.0f,
                             rgba ? rgba[2] : 1.0f, rgba ? rgba[3] : 1.0f };
        HudVertex verts[6];
        emitQuad(verts, xPx, yPx, wPx, hPx, u0, v0, u1, v1, c);
        flushHud(verts, 6, /*texFont=*/-1, /*userTex=*/tex.id);
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
        // Local-space bounding sphere (model space), computed from the mesh's
        // vertices at create/update time. Used by the CPU per-object frustum cull
        // (r_frustumcull): transformed to world space per draw instance via the
        // instance model matrix (center * model; radius * maxScale). See
        // FrustumCull.h. boundsRadius == 0 means "no bounds" -> never culled.
        glm::vec3 boundsCenter{0.0f};   // model-space sphere center
        float     boundsRadius = 0.0f;  // model-space sphere radius (0 = unbounded)
        // CPU local-space AABB from createMesh's submitted vertices (meshBounds).
        float bmin[3] = {0, 0, 0};
        float bmax[3] = {0, 0, 0};
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
        glm::vec4 camPos;            // xyz = camera world position (PBR view vector); w unused
        glm::vec4 sunDir;            // xyz = per-scene direction TOWARD the sun (lighting + shadows)
    };
    static_assert(sizeof(FrameUBO) == 144 + kMaxPointLights * 32 + 32,
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
        glm::vec4 zenith;        // offset 128: rgb = overhead sky color (per-scene)
        glm::vec4 horizon;       // offset 144: rgb = horizon glow color (per-scene)
    };
    static_assert(sizeof(SkyUBO) == 160, "SkyUBO must match the std140 layout in sky.frag");

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
        uint32_t  flags;             // bitfield (was _pad0): bit0 = TERRAIN, bit1 = GLASS
        uint32_t  _pad1, _pad2;      // terrain detail-index packs (only when TERRAIN bit set)
        // ---- PBR maps (0 = none; the fragment shader skips its PBR branch) -----
        uint32_t  normalTexIndex;            // 0 = none (PBR normal-map bindless idx)
        uint32_t  mrTexIndex;                // 0 = none (metallic-roughness bindless idx)
        uint32_t  emissiveTexIndex;          // 0 = none (emissive bindless idx)
        uint32_t  detailPacked;              // HDRP micro-detail: (uvScale*64 << 20) | bindlessIdx; 0 = none
        // ---- GLASS material (only meaningful when the GLASS flag is set) -------
        // Carried per-object so each glass instance keeps its OWN material (the
        // holo-terminal panel/rim/scanline all differ — material-instance style,
        // spec §3.2). Opaque draws leave these zeroed (the opaque path never reads
        // them). glass.frag (M2-M4) consumes: refraction (screen-space bend),
        // roughness (frost mip), specular (shimmer), tint (rgb body color).
        glm::vec4 glassParams;       // x = refraction, y = roughness, z = specular, w = unused
        glm::vec4 glassTint;         // rgb = glass tint color, a = unused
    };
    static_assert(sizeof(ObjectData) == 160, "ObjectData must match std430 layout");

    // Per-object flag bits packed into ObjectData::flags. TERRAIN drives mesh.frag's
    // procedural splat (was the standalone terrainFlag); GLASS routes the fragment to
    // the transparent glass pass (mesh.frag DISCARDs it; glass.frag draws it). The
    // bits are mutually exclusive in practice but stored independently so the shaders
    // can test either without ambiguity.
    static constexpr uint32_t kFlagTerrain = 1u << 0;
    static constexpr uint32_t kFlagGlass   = 1u << 1;
    // CLEARCOAT (car paint): mesh.frag adds a second fixed-F0 specular lobe; the
    // packed {roughness<<8 | intensity} byte pair rides the SPARE terrain-pack1
    // lane (mutually exclusive with TERRAIN, which owns that lane when set).
    static constexpr uint32_t kFlagClearcoat = 1u << 2;

    // CPU-side per-draw record accumulated by drawMesh(), consumed by endFrame().
    struct DrawRecord {
        uint32_t meshId;
        uint32_t texIndex;
        float    model[16];
        float    factor[4];
        float    emissive[4];   // rgb = linear emissive color, a = strength
        uint32_t flags;         // bit0 = TERRAIN (mesh.frag splat), bit1 = GLASS (glass pass)
        uint32_t terrainPack1;  // grass<<16 | rock  (bindless detail indices)
        uint32_t terrainPack2;  // snow<<16  | sand
        uint32_t normalTexIndex = 0;  // 0 = none (PBR normal-map bindless idx)
        uint32_t mrTexIndex     = 0;  // 0 = none (metallic-roughness bindless idx)
        uint32_t emissiveTexIndex = 0; // 0 = none (emissive bindless idx)
        uint32_t detailPacked = 0;     // HDRP micro-detail: (uvScale*64 << 20) | bindlessIdx; 0 = none
        bool     alphaBlend = false;   // glTF BLEND -> blend batch (CPU partition)
        // ALWAYS_VISIBLE for the CPU frustum cull (r_frustumcull): when true this
        // instance is NEVER culled (sky / fullscreen / unbounded items). Mirrors
        // cull.comp's flags bit0. CPU-only — NOT uploaded to the object SSBO.
        bool     noCull = false;
        // GLASS material (only filled by drawMeshGlass; zeroed for opaque draws).
        float    glassParams[4]; // x = refraction, y = roughness, z = specular, w unused
        float    glassTint[4];   // rgb = tint, a unused
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

        // ---- D15 GPU cull per-frame ring (all persistent-mapped) -----------
        //   cullInstBuf : CullInstanceGpu[kMaxDrawsPerFrame] (CPU-written when on)
        //   visBuf      : uint32[kMaxDrawsPerFrame] visible-instance indirection.
        //                 IDENTITY-filled at creation; cull.comp compacts into it
        //                 when the GPU path is on (visDirty tracks the scribble so
        //                 a path toggle back to CPU restores identity once).
        //   cullStatsBuf: CullStatsGpu, GPU-written, read back on slot reuse.
        //   cullParamsBuf: CullParamsGpu UBO (planes/viewProj/hzb/instanceCount).
        VkBuffer      cullInstBuf = VK_NULL_HANDLE;  VmaAllocation cullInstAlloc = nullptr;  void* cullInstMapped = nullptr;
        VkBuffer      visBuf = VK_NULL_HANDLE;       VmaAllocation visAlloc = nullptr;       void* visMapped = nullptr;
        VkBuffer      cullStatsBuf = VK_NULL_HANDLE; VmaAllocation cullStatsAlloc = nullptr; void* cullStatsMapped = nullptr;
        VkBuffer      cullParamsBuf = VK_NULL_HANDLE; VmaAllocation cullParamsAlloc = nullptr; void* cullParamsMapped = nullptr;
        VkDescriptorSet cullSet = VK_NULL_HANDLE;    // cull.comp bindings 0..5
        bool          visDirty = false;       // GPU compaction scribbled visBuf
        bool          cullStatsPending = false; // a frame's cull stats await readback
        uint32_t      cullExpected = 0;       // CPU-evaluated survivors (equiv mode)
        bool          cullExpectedValid = false;
        // Tier 1: this slot's DEDICATED-COMPUTE-QUEUE command pool/buffer (the
        // async cull dispatch). Reset on slot reuse (fence-guarded transitively:
        // compute work <= timeline wait <= graphics work <= inFlight fence).
        VkCommandPool   cullPool = VK_NULL_HANDLE;
        VkCommandBuffer cullCmd  = VK_NULL_HANDLE;
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
        m_frameCmdCount = 0; m_frameCmdOpaque = 0;

        auto& fr = m_frames[m_frameIdx];
        if (!fr.objMapped || !fr.indirectMapped || !fr.camMapped) return;

        // ---- D15 GPU cull: read back the counters this ring slot produced
        // kFramesInFlight frames ago (the inFlight fence waited in beginFrame
        // guarantees the compute finished), then resolve THIS frame's path. ----
        if (fr.cullStatsPending && fr.cullStatsMapped) {
            std::memcpy(&m_lastCullStats, fr.cullStatsMapped, sizeof(CullStatsGpu));
            fr.cullStatsPending = false;
            if (fr.cullExpectedValid) {
                m_lastCullExpected = fr.cullExpected;
                ++m_cullEquivFrames;
                // CPU expectation is FRUSTUM-only; with HZB on the GPU's extra
                // hzbCulled are frustum-survivors, so drawn + hzbCulled must
                // still equal the CPU count exactly.
                if (m_lastCullStats.drawn + m_lastCullStats.hzbCulled != fr.cullExpected) {
                    ++m_cullEquivMismatches;
                    logError("[cull] EQUIVALENCE MISMATCH: gpu drawn=" +
                             std::to_string(m_lastCullStats.drawn) + " (+hzb " +
                             std::to_string(m_lastCullStats.hzbCulled) + ") cpu expected=" +
                             std::to_string(fr.cullExpected));
                }
                fr.cullExpectedValid = false;
            }
        }
        m_cullPathActive = 0;
        if (m_gpuCullReady && m_cullPathReq != 0) {
            int p = m_cullPathReq;
            if (p < 0) p = m_asyncCullReady ? 2 : 1;   // auto: best supported tier
            if (p > 2) p = m_asyncCullReady ? 2 : 1;   // Tier 2 (meshlets) not wired yet
            if (p == 2 && !m_asyncCullReady) p = 1;    // no dedicated queue -> Tier 0
            m_cullPathActive = p;
        }
        // The IBL probe bake replays this frame's indirect commands on a one-time
        // submit BEFORE the frame's command buffer (so before the cull dispatch
        // could bump instanceCount). Fall back to the CPU cull for that one frame
        // so the probe sees real instance counts. Rare (sky change only).
        if (m_cullPathActive >= 1 && m_iblReady && m_iblDirty) m_cullPathActive = 0;
        // HZB occlusion phase: needs the GPU path, the pyramid, and a VALID
        // last-frame depth (never reduce an unrendered depth image).
        m_hzbActiveThisFrame = (m_cullPathActive >= 1) && m_hzbEnabled &&
                               m_hzbReady && m_depthValid;
        // HZB + Tier 1 would put the pyramid reduce (which samples the GRAPHICS-
        // owned depth image) on the compute queue — cross-queue image sharing is
        // future work. With r_hzb on, the cull runs on the graphics queue (Tier 0
        // body) so occlusion keeps working; frustum-only Tier 1 stays fully async.
        if (m_cullPathActive == 2 && m_hzbActiveThisFrame) m_cullPathActive = 1;
        m_frameCullInstances = 0;

        // Camera viewProj (right-handed, reverse-Y for Vulkan clip) + the sun's
        // ortho lightViewProj, written together into the per-frame camera UBO.
        const float aspect = (float)m_extent.width / (float)std::max(1u, m_extent.height);
        const glm::vec3 fwd(std::cos(m_camPitch) * std::cos(m_camYaw),
                            std::sin(m_camPitch),
                            std::cos(m_camPitch) * std::sin(m_camYaw));
        glm::mat4 view = glm::lookAt(m_camPos, m_camPos + fwd, glm::vec3(0, 1, 0));
        glm::mat4 proj = glm::perspective(glm::radians(m_camFov), aspect, 0.1f, 200.0f);
        proj[1][1] *= -1.0f;

        // ---- TAA: sub-pixel jitter + reprojection matrices --------------------
        // The UNJITTERED view-proj is captured FIRST (it is what the resolve pass
        // reprojects history with next frame), then — when TAA is active — a
        // Halton(2,3) sub-pixel offset is folded into `proj` so EVERY raster pass
        // downstream (depth pre-pass, main, shadow-receive, water, glass, GI,
        // SSAO, particles, sky — they all derive from this proj/viewProj) rasters
        // the same jittered frame consistently. Adding to proj[2][0]/[2][1] (the
        // z column) yields a CONSTANT NDC shift because w_clip = -z_view: a clean
        // whole-screen sub-pixel translation. r_taa 0 -> zero jitter, matrices
        // byte-identical to the pre-TAA build.
        //
        // DETERMINISM (documented scheme): the jitter phase is driven purely by a
        // frame COUNTER (no wall clock), so any fixed-frame-count path — all the
        // --screenshot*/--test captures render a fixed number of settle frames —
        // produces bit-identical pixels run over run. Screenshots capture the
        // CONVERGED frame (settle counts exceed the 8-frame Halton cycle).
        const bool taaWant = m_post.taa && (m_taaPipe != VK_NULL_HANDLE)
                          && (m_taaOutImg != VK_NULL_HANDLE) && (m_taaHistImg != VK_NULL_HANDLE);
        const glm::mat4 unjitteredVP = proj * view;
        glm::vec2 jit(0.0f);
        if (taaWant) {
            // Camera CUT detection: a teleport / hard snap makes last frame's
            // history a lie — reset instead of smearing it across the new view.
            if (m_taaPrevCamValid) {
                const float posDelta = glm::length(m_camPos - m_taaPrevCamPos);
                const float yawDelta   = std::abs(m_camYaw   - m_taaPrevYaw);
                const float pitchDelta = std::abs(m_camPitch - m_taaPrevPitch);
                const float fovDelta   = std::abs(m_camFov   - m_taaPrevFov);
                if (posDelta > 2.0f || yawDelta > 1.5f || pitchDelta > 1.5f || fovDelta > 0.5f)
                    m_taaHistoryValid = false;
            }
            // Halton(2,3), 8-sample cycle, centered on the pixel: [-0.5, 0.5).
            auto halton = [](uint32_t i, uint32_t base) {
                float f = 1.0f, r = 0.0f;
                while (i > 0) { f /= (float)base; r += f * (float)(i % base); i /= base; }
                return r;
            };
            const uint32_t hi = (m_taaFrameNum % 8u) + 1u;
            jit = glm::vec2(halton(hi, 2) - 0.5f, halton(hi, 3) - 0.5f);
            proj[2][0] += jit.x * 2.0f / (float)std::max(1u, m_extent.width);
            proj[2][1] += jit.y * 2.0f / (float)std::max(1u, m_extent.height);
            m_taaFrameNum++;
        }

        FrameUBO ubo{};
        ubo.viewProj = proj * view;
        m_lastViewProj = ubo.viewProj;  // cached for the debris instanced draw UBO

        // CPU per-object frustum cull (r_frustumcull): extract the 6 normalized
        // world-space planes from THIS frame's camera viewProj (Gribb-Hartmann).
        // emitGroup() tests each instance's world bounding sphere against these.
        // Same planes (normalized) + same sphere test as the GPU cull.comp.
        m_frameFrustum = extractFrustumPlanes(ubo.viewProj);

        // TAA resolve UBO (per frame-in-flight): the CURRENT jittered inverse
        // viewProj (matches the depth buffer being rasterized this frame) + the
        // PREVIOUS frame's UNJITTERED viewProj (the history was resolved on
        // unjittered pixel centers). History-valid + blend ride in params0.
        if (taaWant && m_taaUboMapped[m_frameIdx]) {
            TaaUBO tu{};
            tu.invViewProjCur = glm::inverse(ubo.viewProj);
            tu.viewProjPrev   = m_taaHistoryValid ? m_taaPrevVP : unjitteredVP;
            tu.params0 = glm::vec4(1.0f / (float)std::max(1u, m_extent.width),
                                   1.0f / (float)std::max(1u, m_extent.height),
                                   m_taaHistoryValid ? 1.0f : 0.0f,
                                   0.9f /* history blend weight */);
            tu.params1 = glm::vec4(jit.x, jit.y, 0.0f, 0.0f);
            std::memcpy(m_taaUboMapped[m_frameIdx], &tu, sizeof(TaaUBO));
        }
        // ---- SSR/RT reflections: activate for THIS frame --------------------
        // Decided here (not in buildAndExecuteGraph) because (a) the SSAO control
        // UBO below carries the mesh.frag enable flag, (b) endFrame's AS-build
        // gate consults it, and (c) the PREVIOUS frame's unjittered viewProj must
        // be captured BEFORE the stash just below overwrites it. Reflections
        // REQUIRE TAA: its history image is the previous-frame color source and
        // its accumulation is the temporal denoiser. ensureReflReady() builds the
        // chain lazily (zero cost for runs that never enable r_ssr). When the
        // history is invalid (first frame / cut / toggle) the pass still runs but
        // writes confidence 0 (camPos.w gate in refl.comp) -> pure IBL fallback.
        m_reflActiveThisFrame = false;
        m_reflHistValid = false;
        if (m_refl.ssr && taaWant && ensureReflReady()) {
            m_reflActiveThisFrame = true;
            m_reflHistValid = m_taaHistoryValid;
            m_reflPrevVP = m_taaHistoryValid ? m_taaPrevVP : unjitteredVP;
        }

        // ---- DDGI: decide activation for THIS frame --------------------------
        // Decided here (like reflections) because the SSAO control UBO below
        // carries the mesh.frag gate + grid geometry. Requires ray-query AND
        // position-fetch hardware; ensureDdgiReady() lazily builds the chain
        // (and auto-fits the probe volume from this frame's draw records when no
        // explicit volume was set). endFrame's AS-build gate consults
        // m_ddgiWantThisFrame; if the TLAS build fails there (no instances) the
        // graph adds no DDGI passes — mesh.frag then blends the (valid, stale)
        // atlases for one frame, which is safe (SHADER_READ_ONLY, real views).
        m_ddgiWantThisFrame = false;
        if (m_ddgi.enabled && m_rtSupported && m_rtPosFetch && ensureDdgiReady())
            m_ddgiWantThisFrame = true;

        // ---- RT soft shadows (r_rtshadows): decide WANT for this frame -------
        // Tier-gated on ray query + the mesh_rt pipeline variants existing. The
        // UBO lanes below carry the gate; whether the RT pipelines are actually
        // BOUND is decided in endFrame (m_rtShadowsActiveThisFrame) once the
        // TLAS build for this frame has succeeded and the set3 TLAS descriptor
        // is written — until then the plain pipelines run and never read these
        // lanes. (Same want/active split DDGI uses.)
        m_rtShadowsWantThisFrame = (m_rtShadows.tier > 0) && m_rtSupported
                                && m_meshPipelineRt != VK_NULL_HANDLE
                                && m_meshPipelineNoSsaoRt != VK_NULL_HANDLE;

        if (!m_ddgi.enabled) {
            m_ddgiFrameCount = 0;      // toggle off -> full warm-up ramp on re-enable
            m_ddgiVolumeValid = false; // and a fresh auto-fit (scene may have changed)
        }

        // Stash this frame's UNJITTERED viewProj + camera pose for next frame's
        // reprojection + cut detection (kept current even with TAA off so a later
        // r_taa 1 doesn't compare against an ancient pose).
        m_taaPrevVP = unjitteredVP;
        m_taaPrevCamPos = m_camPos; m_taaPrevYaw = m_camYaw;
        m_taaPrevPitch = m_camPitch; m_taaPrevFov = m_camFov;
        m_taaPrevCamValid = true;

        // ---- GLASS control UBO (set 4, binding 1): camera world pos + time +
        // screen-space pixel->UV + the live dev-cvar overrides. glass.frag reads it
        // for refraction (M2), fresnel/specular shimmer (M3) and frost (M4). Filled
        // every frame so cvar scrubbing + the animated glint update immediately. ----
        if (m_glassCtrlMapped[m_frameIdx]) {
            const float t = std::chrono::duration<float>(
                std::chrono::steady_clock::now() - m_glassClockStart).count();
            const float invW = (m_extent.width  > 0) ? 1.0f / (float)m_extent.width  : 0.0f;
            const float invH = (m_extent.height > 0) ? 1.0f / (float)m_extent.height : 0.0f;
            GlassControl gc{};
            gc.camPos = glm::vec4(m_camPos, t);
            // screen.z = frost availability (M4): 1 when the frost-blur chain ran this
            // frame (so the shader may lerp to the blurred level by roughness), else 0.
            // screen.w = scene-copy valid (refraction enabled). Both 0 -> M1 fallback.
            const float frostReady = m_glassFrostPipe ? 1.0f : 0.0f;
            gc.screen = glm::vec4(invW, invH, frostReady, m_sceneCopyView ? 1.0f : 0.0f);
            gc.ctrl   = glm::vec4(
                m_glassDev.override ? m_glassDev.refractScale : 1.0f,
                m_glassDev.override ? m_glassDev.roughAdd     : 0.0f,
                m_glassDev.override ? m_glassDev.specScale    : 1.0f,
                m_glassDev.override ? 1.0f : 0.0f);
            // Camera RIGHT / UP world axes from the view matrix (glm is column-major:
            // row0 of view = right, row1 = up). Used to project the world-space glass
            // normal onto the screen plane for the refraction offset + fresnel.
            gc.camRight = glm::vec4(view[0][0], view[1][0], view[2][0], 0.0f);
            gc.camUp    = glm::vec4(view[0][1], view[1][1], view[2][1], 0.0f);
            std::memcpy(m_glassCtrlMapped[m_frameIdx], &gc, sizeof(GlassControl));
        }

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
            // IBL lane: valid only once an environment has been baked into the cubes.
            // .w = metal ambient-specular floor strength (r_metalambient, default 1).
            const float iblValid = (m_iblReady && m_iblBaked) ? 1.0f : 0.0f;
            sc.ibl = glm::vec4(iblValid, 1.0f, (float)(kIblPrefilterMips - 1), m_metalAmbient);
            // Reflections lane (mesh.frag set3): x gates the reflTex sample + IBL
            // blend; y is the live intensity. ONLY set when the refl pass actually
            // runs this frame, so mesh.frag never reads a stale/unwritten buffer.
            // (The IBL probe BAKE shares this UBO — like the SSAO lane, the bake's
            // gl_FragCoord-based UV is meaningless there, an accepted, tiny env-
            // bake approximation inherited from the existing SSAO precedent.)
            sc.refl = glm::vec4(m_reflActiveThisFrame ? 1.0f : 0.0f,
                                m_refl.intensity, 0.0f, 0.0f);
            // DDGI lane (r_ddgi): gate + the probe-grid geometry mesh.frag needs
            // to interpolate the atlases. The intensity RAMPS in over the first
            // ~16 updates after activation so cold (black) probes never read as
            // "ambient removed" — by ramp-end the hysteresis ramp (see
            // prepareDdgiUbo) has fully converged the field.
            if (m_ddgiWantThisFrame) {
                const float ramp = std::min(1.0f, (float)m_ddgiFrameCount / 16.0f);
                sc.ddgiCtrl    = glm::vec4(1.0f, m_ddgi.intensity * ramp,
                                           (float)m_ddgi.debug, m_ddgi.normalBias);
                sc.ddgiOrigin  = glm::vec4(m_ddgiOrigin, m_ddgiVisMaxDist);
                sc.ddgiSpacing = glm::vec4(m_ddgiSpacing, 0.0f);
                sc.ddgiCounts  = glm::vec4((float)m_ddgiCountX, (float)m_ddgiCountY,
                                           (float)m_ddgiCountZ, 0.0f);
            }
            // RT soft-shadow lanes (r_rtshadows): read ONLY by the mesh_rt.frag
            // pipelines (bound only when the TLAS is live), so writing them is
            // free for every other path. Per-frame jitter rotation only while
            // TAA can integrate it; with TAA off the seed pins to 0 so the
            // 1-spp penumbra dither is STATIC (no sizzle).
            if (m_rtShadowsWantThisFrame) {
                sc.rtsh0 = glm::vec4((float)m_rtShadows.tier,
                                     std::tan(glm::radians(m_rtShadows.sunSizeDeg)),
                                     (float)m_rtShadows.pointMax,
                                     m_rtShadows.pointRadius);
                sc.rtsh1 = glm::vec4(taaWant ? (float)(m_rtshFrameSeed++ & 16383u) : 0.0f,
                                     0.0f, 0.0f, 0.0f);
            }
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
        ubo.camPos = glm::vec4(m_camPos, 0.0f);   // PBR view vector (mesh.frag)
        // Per-scene sun direction for lighting + shadows (same source as the sky disk).
        ubo.sunDir = glm::vec4(glm::normalize(glm::vec3(m_sky.sunDir[0], m_sky.sunDir[1], m_sky.sunDir[2])), 0.0f);
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
            sky.params   = glm::vec4(m_sky.haze, m_sky.exposure, m_skyTime, 0.0f);
            sky.zenith   = glm::vec4(m_sky.zenith[0], m_sky.zenith[1], m_sky.zenith[2], 0.0f);
            sky.horizon  = glm::vec4(m_sky.horizon[0], m_sky.horizon[1], m_sky.horizon[2], 0.0f);
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
        m_frameGlassCount = 0;
        // D15 GPU cull path: the CPU writes EVERY instance (no CPU skip) plus one
        // CullInstanceGpu per row; the indirect commands go out with
        // instanceCount = 0 and cull.comp bumps/compacts on the GPU.
        const bool gpuCull = (m_cullPathActive >= 1) && fr.cullInstMapped;
        CullInstanceGpu* cullInst = gpuCull
            ? static_cast<CullInstanceGpu*>(fr.cullInstMapped) : nullptr;
        uint32_t cullExpectedCount = 0;   // CPU-evaluated survivors (equiv harness)
        // Record each draw record's SSBO row (the grouped write order differs from
        // the record order): the TLAS instanceCustomIndex carries this row so the
        // DDGI ray shader can fetch the hit object's albedo/emissive (capacity
        // persists; assign() is a memset-speed fill, no per-frame heap churn).
        m_recordSsboRow.assign(m_drawRecords.size(), 0u);
        // Emit one indirect cmd + its SSBO rows for a mesh group. Run in TWO passes so OPAQUE
        // groups are recorded first and BLEND (glass) groups last — the shadow/depth-prepass
        // replay only [0, m_frameCmdOpaque), and the color pass draws the blend tail with the
        // transparent pipeline AFTER opaque has established depth.
        auto emitGroup = [&](uint32_t mid) {
            auto mit = m_meshes.find(mid);
            if (mit == m_meshes.end()) return;
            const std::vector<uint32_t>& list = m_groups[mid];
            if (list.empty()) return;
            if (cmdCount >= kMaxDrawMeshes) return;
            const uint32_t baseRow = row;
            const glm::vec3 meshC = mit->second.boundsCenter;
            const float     meshR = mit->second.boundsRadius;
            bool anyCutout = false;   // any instance with texIndex bit31 (glTF alphaMode MASK)
            for (uint32_t ri : list) {
                const DrawRecord& dr = m_drawRecords[ri];
                // CPU per-object frustum cull (r_frustumcull). Skip an instance whose
                // world bounding sphere is fully outside the frustum. ALWAYS_VISIBLE
                // (dr.noCull) and unbounded meshes (meshR == 0) are never culled.
                // Same normalized planes + same conservative sphere test as cull.comp,
                // so the GPU statDrawn matches this objectsDrawn exactly.
                if (!gpuCull) {
                    if (m_frustumCull && !dr.noCull && meshR > 0.0f) {
                        const glm::mat4 model = glm::make_mat4(dr.model);
                        const CullSphere ws = worldSphere(model, meshC, meshR);
                        if (!sphereInFrustum(m_frameFrustum, ws)) continue;  // culled
                    }
                } else {
                    // GPU path: emit the cull-shader input for this row. The bypass
                    // condition is EXACTLY the complement of the CPU test's gate, so
                    // both paths keep the same survivor set (D15 equivalence).
                    const bool bypass = !m_frustumCull || dr.noCull || meshR <= 0.0f;
                    CullSphere ws(0.0f, 0.0f, 0.0f, 0.0f);
                    if (!bypass) {
                        const glm::mat4 model = glm::make_mat4(dr.model);
                        ws = worldSphere(model, meshC, meshR);
                    }
                    CullInstanceGpu& cg = cullInst[row];
                    cg.sphere[0] = ws.x; cg.sphere[1] = ws.y;
                    cg.sphere[2] = ws.z; cg.sphere[3] = ws.w;
                    cg.meshSlot = cmdCount;     // this group's indirect command
                    cg.instanceData = row;      // SSBO row the survivor maps back to
                    cg.flags = bypass ? 1u : 0u;
                    cg._pad = 0u;
                    if (m_cullEquivCheck &&
                        (bypass || sphereInFrustum(m_frameFrustum, ws)))
                        ++cullExpectedCount;
                }
                if (dr.texIndex & 0x80000000u) anyCutout = true;
                m_recordSsboRow[ri] = row;          // DDGI hit-shading lookup row
                ObjectData& o = objs[row++];
                std::memcpy(&o.model, dr.model, sizeof(o.model));
                o.baseColorFactor = glm::vec4(dr.factor[0], dr.factor[1], dr.factor[2], dr.factor[3]);
                o.emissive = glm::vec4(dr.emissive[0], dr.emissive[1], dr.emissive[2], dr.emissive[3]);
                o.texIndex = dr.texIndex;
                // flags = bit0 TERRAIN | bit1 GLASS; pad1/pad2 = the four packed
                // detail-texture indices (only meaningful when TERRAIN is set). See
                // mesh.{vert,frag} + glass.frag.
                o.flags  = dr.flags;
                o._pad1  = dr.terrainPack1;
                o._pad2  = dr.terrainPack2;
                o.normalTexIndex   = dr.normalTexIndex;
                o.mrTexIndex       = dr.mrTexIndex;
                o.emissiveTexIndex = dr.emissiveTexIndex;
                o.detailPacked = dr.detailPacked;   // HDRP micro-detail: (uvScale*64<<20)|bindlessIdx
                o.glassParams = glm::vec4(dr.glassParams[0], dr.glassParams[1],
                                          dr.glassParams[2], dr.glassParams[3]);
                o.glassTint   = glm::vec4(dr.glassTint[0], dr.glassTint[1],
                                          dr.glassTint[2], dr.glassTint[3]);
                if (dr.flags & kFlagGlass) ++m_frameGlassCount;
            }
            // Survivors actually written this group (== list.size() when cull is off).
            const uint32_t drawn = row - baseRow;
            // Whole group culled away -> emit NO indirect command (and no draw call),
            // exactly as an empty group is skipped above. Nothing references baseRow.
            if (drawn == 0) return;
            VkDrawIndexedIndirectCommand& c = cmds[cmdCount];
            c.indexCount    = mit->second.indexCount;
            // GPU path: 0 — cull.comp atomically bumps it per survivor.
            c.instanceCount = gpuCull ? 0u : drawn;
            c.firstIndex    = 0;
            c.vertexOffset  = 0;
            c.firstInstance = baseRow;
            m_drawMeshOrder[cmdCount] = mid;
            // Cutout groups need the alpha-testing depth pre-pass variant when
            // reflections force the pre-pass on (see recordDepthPrePassBody).
            m_drawMeshCutout[cmdCount] = anyCutout ? 1u : 0u;
            ++cmdCount;
            m_building.drawCalls += 1;
            m_building.objectsDrawn += drawn;
            m_building.triangles += (mit->second.indexCount / 3) * drawn;
        };
        auto isBlendGroup = [&](uint32_t mid) {
            auto it = m_groups.find(mid);
            return it != m_groups.end() && !it->second.empty() && m_drawRecords[it->second[0]].alphaBlend;
        };
        for (uint32_t mid : m_groupOrder) if (!isBlendGroup(mid)) emitGroup(mid);  // OPAQUE first
        m_frameCmdOpaque = cmdCount;                                               // [0,opaque) | [opaque,count)
        for (uint32_t mid : m_groupOrder) if ( isBlendGroup(mid)) emitGroup(mid);  // BLEND (glass) last
        m_frameCmdCount = cmdCount;

        // ---- D15 GPU cull frame finalize ------------------------------------
        if (gpuCull && row > 0) {
            // Params UBO: the SAME normalized planes the CPU test uses + this
            // frame's viewProj (HZB projection; hzbSize stays 0 until stage 2).
            CullParamsGpu cp{};
            for (int i = 0; i < 6; ++i) {
                cp.frustum[i][0] = m_frameFrustum.p[i].x;
                cp.frustum[i][1] = m_frameFrustum.p[i].y;
                cp.frustum[i][2] = m_frameFrustum.p[i].z;
                cp.frustum[i][3] = m_frameFrustum.p[i].w;
            }
            std::memcpy(cp.viewProj, &ubo.viewProj, sizeof(cp.viewProj));
            cp.hzbSize[0] = m_hzbActiveThisFrame ? (float)m_hzbW : 0.0f;
            cp.hzbSize[1] = m_hzbActiveThisFrame ? (float)m_hzbH : 0.0f;
            cp.instanceCount = row;
            std::memcpy(fr.cullParamsMapped, &cp, sizeof(cp));
            std::memset(fr.cullStatsMapped, 0, sizeof(CullStatsGpu));
            fr.cullStatsPending = true;
            fr.visDirty = true;               // compute will scribble visBuf
            if (m_cullEquivCheck) { fr.cullExpected = cullExpectedCount; fr.cullExpectedValid = true; }
            m_frameCullInstances = row;
        } else {
            m_cullPathActive = 0;             // nothing to cull -> graph adds no pass
            if (fr.visDirty && fr.visMapped) {
                // Path toggled back to CPU: restore the identity mapping once so
                // gl_InstanceIndex addresses object rows directly again.
                uint32_t* ids = static_cast<uint32_t*>(fr.visMapped);
                for (uint32_t k = 0; k < kMaxDrawsPerFrame; ++k) ids[k] = k;
                fr.visDirty = false;
            }
        }
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
            for (uint32_t i = 0; i < m_frameCmdOpaque; ++i) {
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
        // Alpha-CUTOUT groups (foliage / people billboards, texIndex bit31): the
        // plain pipeline has no fragment stage, so it writes depth for the FULL
        // quad — the color pass then alpha-discards those texels under EQUAL and
        // nothing ever fills them (flat clear-color rectangles around trees). The
        // cutout pipeline (depth_cutout.vert/.frag) replicates mesh.frag's exact
        // discard. ONLY engaged on reflections frames: SSAO/GI-only pre-passes
        // keep the historical full-quad depth bit-for-bit (r_ssr 0 + r_taa A/B
        // md5 guarantees vs the pre-reflections build stay intact; promoting the
        // cutout fix to SSAO/GI is a separate, deliberate change).
        const bool cutoutAware = m_reflActiveThisFrame
                              && (m_depthPreCutoutPipeline != VK_NULL_HANDLE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_depthPrePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowLayout,
                                0, 1, &fr.objSet, 0, nullptr);
        bool cutoutBound = false;   // which of the two pipelines is currently bound
        for (uint32_t i = 0; i < m_frameCmdOpaque; ++i) {
            const bool wantCutout = cutoutAware && (m_drawMeshCutout[i] != 0);
            if (wantCutout != cutoutBound) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    wantCutout ? m_depthPreCutoutPipeline : m_depthPrePipeline);
                if (wantCutout) {
                    // set 0 = objSet (layout-compatible with the plain pipeline's
                    // m_shadowLayout, stays bound), set 1 = bindless textures.
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        m_depthPreCutoutLayout, 0, 1, &fr.objSet, 0, nullptr);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        m_depthPreCutoutLayout, 1, 1, &m_bindlessSet, 0, nullptr);
                } else {
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        m_shadowLayout, 0, 1, &fr.objSet, 0, nullptr);
                }
                cutoutBound = wantCutout;
            }
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
        // Pre-pass on (SSAO, GI, OR reflections) -> the EQUAL/no-write pipeline (the
        // depth pre-pass already wrote depth). None on -> the original LESS/write
        // pipeline (main pass owns depth, no pre-pass ran). The mesh.frag AO sample is
        // independently gated by the SSAO control UBO, so the EQUAL pipeline is safe
        // when GI/reflections are on but SSAO is off (no AO is read in that case).
        // m_reflActiveThisFrame matches the graph's reflOn exactly (it is cleared in
        // buildAndExecuteGraph when TAA is off), so this never diverges from the
        // graph's depth-prepass / LOAD-vs-CLEAR decision.
        const bool prePassOn = m_ssao.enabled || m_gi.enabled || m_reflActiveThisFrame;
        // RT soft shadows (r_rtshadows): swap in the mesh_rt.frag variants —
        // identical fixed-function state, identical layout — only on frames
        // where endFrame confirmed the TLAS + its set3 descriptor are live.
        // Inactive/tier-0/non-RT frames bind the EXACT pre-existing pipelines.
        const bool rtsh = m_rtShadowsActiveThisFrame;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          prePassOn ? (rtsh ? m_meshPipelineRt       : m_meshPipeline)
                                    : (rtsh ? m_meshPipelineNoSsaoRt : m_meshPipelineNoSsao));
        // set 0 = bindless textures, set 1 = object SSBO + camera UBO, set 2 = shadow
        // map, set 3 = the SSAO AO texture + control UBO (this frame's set), set 4 =
        // IBL (irradiance + prefilter cubes + BRDF LUT). set 4 is always bound when
        // the IBL objects exist (they're cleared to neutral at init + rebaked on sky
        // change); mesh.frag gates the IBL math on the SSAO-ctrl ibl.x valid flag, so
        // an un-baked / failed env safely falls back to the flat ambient term.
        VkDescriptorSet sets[5] = { m_bindlessSet, fr.objSet, m_shadowSet, m_meshAoSet[m_frameIdx], m_iblMeshSet };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_meshLayout,
                                0, 5, sets, 0, nullptr);
        for (uint32_t i = 0; i < m_frameCmdOpaque; ++i) {
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
        // BLEND (glass) pass: the transparent pipeline (src-alpha over, depth-test LEQUAL,
        // NO depth-write, cull NONE), same color attachment + descriptor sets, drawn AFTER
        // opaque so glass composites over the established opaque depth. v1: unsorted.
        if (m_meshPipelineTransparent && m_frameCmdCount > m_frameCmdOpaque) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              (rtsh && m_meshPipelineTransparentRt) ? m_meshPipelineTransparentRt
                                                                    : m_meshPipelineTransparent);
            for (uint32_t i = m_frameCmdOpaque; i < m_frameCmdCount; ++i) {
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
    }

    // A deferred HUD draw: a span of this frame's HUD vertex ring + which texture
    // to bind. `texFont` selects the atlas: -1 => the 1x1 white texel (filled
    // quads); >=0 => that FontRole's glyph atlas (or the 8x8 bitmap fallback if the
    // role didn't bake). drawHudQuad/drawHudText(F) append these; they are replayed
    // (in order, after the meshes) by recordHudDraws inside the graph's color pass —
    // keeping HUD-on-top ordering identical to the hand-coded path.
    struct HudRecord { uint32_t first; uint32_t count; int texFont; uint32_t userTex = 0; };

    // Append `count` vertices to this frame's HUD ring and queue a deferred HUD
    // draw record (replayed inside the graph's color pass). No command recording
    // here — the color pass is not open yet (commands are recorded in endFrame).
    // `texFont`: -1 = white texel; otherwise a FontRole index whose atlas to bind.
    void flushHud(const HudVertex* verts, uint32_t count, int texFont, uint32_t userTex = 0) {
        auto& fr = m_frames[m_frameIdx];
        if (!fr.hudVboMapped || fr.hudVertsUsed + count > kMaxHudVerts) return; // ring full
        uint32_t first = fr.hudVertsUsed;
        std::memcpy(static_cast<HudVertex*>(fr.hudVboMapped) + first,
                    verts, (size_t)count * sizeof(HudVertex));
        fr.hudVertsUsed += count;
        // COALESCE with the previous record when it binds the same texture and is
        // contiguous in the ring (always true within a frame). Ordering is
        // unchanged (records replay in append order), and a quad-heavy screen
        // (the world map: grid + icons + markers) stays a handful of records —
        // each record costs one descriptor from the per-frame pool (kMaxHudDraws),
        // which a record-per-quad scheme exhausted (text after ~256 quads vanished).
        if (!m_hudRecords.empty()) {
            HudRecord& last = m_hudRecords.back();
            if (last.texFont == texFont && last.userTex == userTex &&
                last.first + last.count == first) {
                last.count += count;
                return;
            }
        }
        m_hudRecords.push_back(HudRecord{ first, count, texFont, userTex });
    }

    // Resolve a HudRecord's texFont to the Texture to bind: a live app texture for
    // userTex != 0 (drawHudImage — the world-map tile compositor), the white texel
    // for -1, the role's baked atlas if ready, else the 8x8 bitmap fallback, else white.
    const Texture* hudRecordTexture(int texFont, uint32_t userTex = 0) const {
        if (userTex != 0) {
            auto it = m_textures.find(userTex);
            if (it != m_textures.end() && it->second.view) return &it->second;
            return &m_whiteTex;
        }
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
            const Texture* tex = hudRecordTexture(hr.texFont, hr.userTex);

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
            // Source: the post source (TAA output when TAA ran this frame, the
            // raw HDR scene otherwise — `rgHdr` is already the right resource;
            // pick the matching pre-written descriptor set) or the previous mip.
            RgResource srcRes = firstPass ? rgHdr : rgMip[i - 1];
            VkDescriptorSet srcSet = firstPass
                ? (m_taaActiveThisFrame ? m_setTaaOut : m_setHdr)
                : m_setMip[i - 1];
            // Source resolution (1/texel) for the filter taps.
            VkExtent2D srcExt = firstPass ? m_extent : m_bloomMips[i - 1].extent;
            const VkExtent2D dstExt = m_bloomMips[i].extent;

            BloomPush& pc = m_bloomDownPush[i];
            pc.srcTexel[0] = 1.0f / (float)std::max(1u, srcExt.width);
            pc.srcTexel[1] = 1.0f / (float)std::max(1u, srcExt.height);
            pc.threshold = (m_post.bloomThreshold > 0.0f) ? m_post.bloomThreshold
                                                          : kBloomThreshold;   // r_bloomthreshold (live)
            pc.knee = kBloomKnee;
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

    // ---- Glass frost-blur chain (M4) --------------------------------------
    // Downsample the scene copy into m_glassFrostImg[] — separate single-mip images
    // (the render-graph tracks one layout per image, so distinct images, not mip
    // levels), each half the previous. level0 samples the scene copy; level i samples
    // level i-1. Reuses the bloom-down 13-tap filter (m_bloomDownPipe / m_bloomLayout,
    // BloomPush, firstPass=0 = plain downsample). The glass shader lerps the sharp
    // copy toward the deepest level by roughness. Stable per-level storage lives in
    // member arrays. The caller passes the scene-copy resource (rgSceneCopy) so the
    // first level orders after the copy; each frost level is imported here.
    void addGlassFrostPasses(RgResource rgSceneCopy) {
        for (uint32_t lvl = 0; lvl < kGlassFrostLevels; ++lvl) {
            if (!m_glassFrostSrcSet[lvl] || !m_glassFrostImg[lvl]) return;
            const VkExtent2D srcExt = (lvl == 0) ? m_extent : m_glassFrostExt[lvl - 1];
            const VkExtent2D dstExt = m_glassFrostExt[lvl];

            RgResource rgDst = m_graph.importImage("glass.frost", m_glassFrostImg[lvl],
                ResourceState{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 });
            m_glassFrostRg[lvl] = rgDst;   // remembered so the next level reads it

            BloomPush& pc = m_glassFrostPush[lvl];
            pc.srcTexel[0] = 1.0f / (float)std::max(1u, srcExt.width);
            pc.srcTexel[1] = 1.0f / (float)std::max(1u, srcExt.height);
            pc.threshold = 0.0f; pc.knee = 0.0f;
            pc.intensity = 1.0f; pc.firstPass = 0;   // plain 13-tap downsample (blur)

            m_glassFrostAttach[lvl] = VkRenderingAttachmentInfo{};
            m_glassFrostAttach[lvl].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            m_glassFrostAttach[lvl].imageView = m_glassFrostView[lvl];
            m_glassFrostAttach[lvl].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            m_glassFrostAttach[lvl].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            m_glassFrostAttach[lvl].storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            m_glassFrostRenderInfo[lvl] = VkRenderingInfo{};
            m_glassFrostRenderInfo[lvl].sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            m_glassFrostRenderInfo[lvl].renderArea = { {0,0}, dstExt };
            m_glassFrostRenderInfo[lvl].layerCount = 1;
            m_glassFrostRenderInfo[lvl].colorAttachmentCount = 1;
            m_glassFrostRenderInfo[lvl].pColorAttachments = &m_glassFrostAttach[lvl];

            RenderPassDesc fp{};
            fp.name = "glass-frost";
            fp.addUse(ResourceUse{
                rgDst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/true });
            // Source: the scene copy (level0) or the previous frost level. Declaring
            // the read transitions the source to SHADER_READ_ONLY before this pass.
            RgResource rgSrc = (lvl == 0) ? rgSceneCopy : m_glassFrostRg[lvl - 1];
            fp.addUse(ResourceUse{
                rgSrc, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/false });
            fp.usesDynamicRendering = true;
            fp.renderInfo = m_glassFrostRenderInfo[lvl];
            m_glassFrostCtx[lvl] = BloomPassCtx{ this, m_glassFrostSrcSet[lvl], dstExt, lvl };
            fp.recordCtx = &m_glassFrostCtx[lvl];
            fp.record = [](void* ctx, VkCommandBuffer c){
                auto* pc = static_cast<BloomPassCtx*>(ctx);
                auto* self = pc->self;
                self->postViewport(c, pc->dstExt);
                vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, self->m_glassFrostPipe);
                vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, self->m_bloomLayout,
                                        0, 1, &pc->srcSet, 0, nullptr);
                vkCmdPushConstants(c, self->m_bloomLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(BloomPush), &self->m_glassFrostPush[pc->mip]);
                vkCmdDraw(c, 3, 1, 0, 0);
            };
            m_graph.addPass(std::move(fp));
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
        // D15 HZB: when the pyramid reduces LAST frame's depth this frame, import
        // the depth with its preserved post-frame state (UNDEFINED would discard
        // the contents the reduce is about to read). Otherwise exactly as before.
        RgResource rgDepth = (m_hzbActiveThisFrame && m_depthValid)
            ? m_graph.importImage("scene.depth", m_depthImg, m_depthState)
            : m_graph.importImage("scene.depth", m_depthImg,
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
        // RT ambient occlusion (hardware ray query). Active only when r_rtao is on,
        // the device supports RT, and the TLAS built this frame (set in endFrame).
        const bool rtaoOn = m_rtaoActiveThisFrame;
        // TAA: active when enabled (r_taa) and the pipeline + targets exist. The
        // resolve pass reads the finished HDR scene + depth + history and writes
        // the TAA output; AE/bloom/composite then read the TAA output instead of
        // the raw HDR scene. OFF -> zero added passes, raw-HDR wiring unchanged.
        // (Computed up here — reflections and the depth pre-pass depend on it.)
        const bool taaOn = m_post.taa && (m_taaPipe != VK_NULL_HANDLE)
                        && (m_taaOutImg != VK_NULL_HANDLE) && (m_taaHistImg != VK_NULL_HANDLE);
        m_taaActiveThisFrame = taaOn;
        // SSR/RT reflections (refl.comp): decided in prepareFrameData; hard-gated
        // on TAA here too (its history image is the pass's color source) so the
        // recordMeshDraws pipeline choice stays consistent with this graph.
        if (!taaOn) m_reflActiveThisFrame = false;
        const bool reflOn = m_reflActiveThisFrame;
        // The camera depth PRE-PASS runs when SSAO, GI, RT AO, OR reflections need a
        // complete depth buffer before the main pass; the main pass then tests EQUAL
        // (no depth write).
        const bool prePassOn = ssaoOn || giOn || rtaoOn || reflOn;
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
        // Translucent GLASS: add a post-opaque transparent pass only when glass was
        // submitted this frame AND the pipeline + its set-4 resources exist (graceful
        // fallback, spec §5). It depth-tests (read-only) against the stored scene
        // depth, like water.
        const bool glassOn = (m_frameGlassCount > 0) && (m_glassPipeline != VK_NULL_HANDLE)
                             && (m_glassSet[m_frameIdx] != VK_NULL_HANDLE);
        // Screen-space refraction/frost (M2/M4) needs the scene-color copy target.
        // When it failed to create, glass still draws (M1 alpha + fresnel/specular)
        // but the copy pass + scene sampling are skipped (the shader reads the maxMip
        // flag = 0 from the control UBO and refracts nothing).
        const bool glassCopyOn = glassOn && (m_sceneCopyImg != VK_NULL_HANDLE);
        // Frost (M4): the blurred-mip chain on the scene copy. Needs the frost
        // downsample pipeline + per-mip descriptor sets (built in createGlassResources).
        const bool glassFrostOn = glassCopyOn && (m_glassFrostPipe != VK_NULL_HANDLE);
        // (TAA's taaOn + m_taaActiveThisFrame were computed above, before prePassOn,
        // because the reflections gate + depth pre-pass depend on them.)
        // The scene depth must be STORED (not transient) when water/GI/particles/debris/
        // glass OR RT AO (its compute + apply passes sample it) OR TAA (the resolve
        // reconstructs world position from it) read it. (reflOn implies taaOn.)
        const bool storeDepth = waterOn || giOn || particlesOn || debrisDraw || rtaoOn || glassOn || taaOn;
        RgResource rgSsaoRaw  = m_graph.importImage("ssao.raw",  m_ssaoRawImg,
            ResourceState{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 });
        RgResource rgSsaoBlur = m_graph.importImage("ssao.blur", m_ssaoBlurImg,
            ResourceState{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 });
        // RT-AO half-res storage image (compute writes GENERAL, apply samples it).
        // Imported UNDEFINED each frame (fully overwritten by the compute pass).
        RgResource rgRtao = {};
        if (rtaoOn) rgRtao = m_graph.importImage("rtao.ao", m_rtaoImg,
            ResourceState{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 });
        // Reflection storage image (compute writes GENERAL, mesh.frag samples it).
        // Imported UNDEFINED each frame (fully overwritten by the refl pass).
        RgResource rgRefl = {};
        if (reflOn) rgRefl = m_graph.importImage("refl.out", m_reflImg,
            ResourceState{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 });
        // DDGI probe atlases. PERSISTENT across frames (the probe field IS the
        // accumulated history), so they are imported with their tracked post-frame
        // state (the taa.hist pattern) — the graph derives the cross-frame
        // read/write transition barriers from it.
        const bool ddgiOn = m_ddgiActiveThisFrame;
        RgResource rgDdgiIrr = {}, rgDdgiVis = {};
        if (ddgiOn) {
            rgDdgiIrr = m_graph.importImage("ddgi.irr", m_ddgiIrrImg, m_ddgiIrrState);
            rgDdgiVis = m_graph.importImage("ddgi.vis", m_ddgiVisImg, m_ddgiVisState);
        }
        // Scene-color copy (glass refraction/frost). Imported UNDEFINED each frame —
        // its content is fully (re)written by the copy pass (mip0) + frost passes
        // (mips 1..N) before the glass pass samples it, so there is no cross-frame
        // dependency the graph must preserve.
        RgResource rgSceneCopy = {};
        if (glassCopyOn) rgSceneCopy = m_graph.importImage("scene.copy", m_sceneCopyImg,
            ResourceState{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 });
        // GI half-res buffers + the prev-depth copy. Imported each frame; the accum
        // buffers persist across frames (history), but the graph only tracks layout
        // within a frame so importing UNDEFINED is correct (each is fully written by
        // its producing pass before being read; the cross-frame data lives in the
        // image memory, not the graph's per-frame layout state).
        // TAA resolve output + persistent history. The output is fully overwritten
        // by the resolve each frame -> imported UNDEFINED. The HISTORY persists
        // across frames (its DATA must survive), so it is imported with its tracked
        // post-frame state (m_taaHistState, like the shadow map) so the graph
        // derives the cross-frame WAR/transition barriers correctly.
        RgResource rgTaaOut = {}, rgTaaHist = {};
        if (taaOn) {
            rgTaaOut = m_graph.importImage("taa.out", m_taaOutImg,
                ResourceState{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 });
            rgTaaHist = m_graph.importImage("taa.hist", m_taaHistImg, m_taaHistState);
        }
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

        // ---- Pass 0c: D15 GPU object cull (r_cullpath >= 1) ------------------
        // One compute dispatch over this frame's CullInstanceGpu rows: zero-init'd
        // indirect instanceCounts are bumped + survivors compacted into visBuf
        // BEFORE the first consumer (the shadow pass) replays the indirect draws.
        // Buffer hazards (compute write -> DRAW_INDIRECT / VERTEX_SHADER read) are
        // manual sync2 buffer barriers inside the record body — the graph tracks
        // images only, per its documented scope.
        if (m_cullPathActive >= 1 && m_frameCullInstances > 0 && m_frameCmdCount > 0) {
            const bool hzbThisFrame = m_hzbActiveThisFrame;
            // HZB reduce FIRST (samples last frame's depth into the pyramid), so
            // the cull dispatch right after it sees fresh occlusion data. The
            // pass declares the depth read so the graph derives the
            // (attachment -> DEPTH_READ_ONLY) transition; the pyramid's own
            // barriers are manual inside recordHzbBuild (mip granularity).
            if (hzbThisFrame) {
                m_hzbChain = GpuCullSystem::HzbChain{ m_hzbImg, m_hzbW, m_hzbH,
                                                      m_hzbMipCount, m_hzbMipSet };
                RenderPassDesc hp{};
                hp.name = "hzb-build";
                hp.queue = RgQueue::Compute;
                hp.addUse(ResourceUse{
                    rgDepth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT, /*isWrite=*/false });
                hp.recordCtx = this;
                hp.record = [](void* ctx, VkCommandBuffer c){
                    auto* self = static_cast<VulkanRenderDevice*>(ctx);
                    self->m_gpuCull.recordHzbBuild(c, self->m_hzbChain);
                };
                m_graph.addPass(std::move(hp));
            }
            auto& cfr = m_frames[m_frameIdx];
            CullFrameInputs ci{};
            ci.instances        = cfr.cullInstBuf;
            ci.drawCmds         = cfr.indirectBuf;
            ci.visibleInstances = cfr.visBuf;
            ci.stats            = cfr.cullStatsBuf;
            ci.params           = cfr.cullParamsBuf;
            ci.instanceCount    = m_frameCullInstances;
            ci.cullSet          = cfr.cullSet;
            if (m_cullPathActive == 2) {
                // TIER 1: the dispatch records into this slot's compute-queue
                // command buffer in endFrame (submitted BEFORE the graphics
                // submit, which waits the timeline at DRAW_INDIRECT|VERTEX_SHADER).
                m_asyncCullThisFrame = true;
                m_asyncCullInputs = ci;
            } else {
                m_gpuCull.addCullPass(m_graph, ci, hzbThisFrame);
            }
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

        // ---- RT-AO compute pass (hardware ray query) ------------------------
        // After the depth pre-pass populated the camera depth buffer, trace the
        // TLAS with rayQueryEXT from each pixel's depth-reconstructed world position
        // and write the half-res AO storage image. Reads depth (DEPTH_READ_ONLY) +
        // writes the AO image (GENERAL). The TLAS itself was built in endFrame() as
        // a synchronous submit before this command buffer; no graph dependency is
        // needed for it. Gated on rtaoOn (zero cost / no pass when off).
        if (rtaoOn) {
            RenderPassDesc rp{};
            rp.name = "rtao-compute";
            rp.queue = RgQueue::Compute;
            rp.usesDynamicRendering = false;
            rp.addUse(ResourceUse{
                rgRtao, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/true });
            rp.addUse(ResourceUse{
                rgDepth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_IMAGE_ASPECT_DEPTH_BIT, /*isWrite=*/false });
            rp.recordCtx = this;
            rp.record = [](void* ctx, VkCommandBuffer c){
                static_cast<VulkanRenderDevice*>(ctx)->recordRtaoComputeBody(c); };
            m_graph.addPass(std::move(rp));
        }

        // ---- SSR/RT REFLECTIONS compute (refl.comp) --------------------------
        // After the depth pre-pass (complete depth) and BEFORE the main color pass
        // (which samples the output in mesh.frag): march each pixel's reflection
        // ray against the depth buffer, sampling LAST frame's lit scene from the
        // TAA history image (prev-frame color = no same-frame hazards); optional
        // inline ray-query fallback against the TLAS built in endFrame (no graph
        // dependency needed for it — same as rtao-compute). Reads depth
        // (DEPTH_READ_ONLY) + taa.hist (SHADER_READ_ONLY), writes the rgba16f
        // reflection image (GENERAL). Gated on reflOn (zero cost / no pass off).
        if (reflOn) {
            RenderPassDesc rp{};
            rp.name = "refl-compute";
            rp.queue = RgQueue::Compute;
            rp.usesDynamicRendering = false;
            rp.addUse(ResourceUse{
                rgRefl, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/true });
            rp.addUse(ResourceUse{
                rgDepth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_IMAGE_ASPECT_DEPTH_BIT, /*isWrite=*/false });
            rp.addUse(ResourceUse{
                rgTaaHist, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/false });
            rp.recordCtx = this;
            rp.record = [](void* ctx, VkCommandBuffer c){
                static_cast<VulkanRenderDevice*>(ctx)->recordReflComputeBody(c); };
            m_graph.addPass(std::move(rp));
        }

        // ---- DDGI probe passes (ddgi_rays.comp + ddgi_update.comp) ----------
        // BEFORE the main color pass (mesh.frag samples the atlases). The RAY
        // pass traces N rays/probe against the TLAS built in endFrame (a fenced
        // pre-frame submit — no graph dependency needed, the rtao pattern) while
        // SAMPLING the previous frame's atlases (the infinite-bounce feedback);
        // the UPDATE pass then hysteresis-blends the ray results INTO the
        // atlases as storage images. The intermediate ray buffer is an SSBO —
        // its write->read barrier lives inside the update record body (buffers
        // are not graph resources). Gated on ddgiOn (zero cost / no pass off).
        if (ddgiOn) {
            {
                RenderPassDesc rp{};
                rp.name = "ddgi-rays";
                rp.queue = RgQueue::Compute;
                rp.usesDynamicRendering = false;
                rp.addUse(ResourceUse{
                    rgDdgiIrr, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/false });
                rp.addUse(ResourceUse{
                    rgDdgiVis, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/false });
                rp.recordCtx = this;
                rp.record = [](void* ctx, VkCommandBuffer c){
                    static_cast<VulkanRenderDevice*>(ctx)->recordDdgiRaysBody(c); };
                m_graph.addPass(std::move(rp));
            }
            {
                RenderPassDesc up{};
                up.name = "ddgi-update";
                up.queue = RgQueue::Compute;
                up.usesDynamicRendering = false;
                up.addUse(ResourceUse{
                    rgDdgiIrr, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/true });
                up.addUse(ResourceUse{
                    rgDdgiVis, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/true });
                up.recordCtx = this;
                up.record = [](void* ctx, VkCommandBuffer c){
                    static_cast<VulkanRenderDevice*>(ctx)->recordDdgiUpdateBody(c); };
                m_graph.addPass(std::move(up));
            }
        }

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
            // When reflections ran, mesh.frag samples the reflection buffer —
            // declare it so the graph transitions it GENERAL -> SHADER_READ_ONLY.
            if (reflOn) {
                colorPass.addUse(ResourceUse{
                    rgRefl, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/false });
            }
            // When DDGI ran, mesh.frag interpolates the probe atlases — declare
            // them so the graph transitions GENERAL -> SHADER_READ_ONLY after
            // the update pass.
            if (ddgiOn) {
                colorPass.addUse(ResourceUse{
                    rgDdgiIrr, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/false });
                colorPass.addUse(ResourceUse{
                    rgDdgiVis, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
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

        // ---- Scene-color COPY (glass refraction/frost capture, spec §3.1) ---
        // Snapshot the opaque (+ sky/water) HDR scene into m_sceneCopyImg mip0 with a
        // straight vkCmdCopyImage, AFTER the main(+water) pass and BEFORE the glass
        // pass — glass.frag samples this copy for the screen behind it (you cannot
        // sample + write one image in a single pass). Only when glass + the copy
        // target are live (glassCopyOn). The graph derives HDR COLOR_ATTACHMENT ->
        // TRANSFER_SRC and scene-copy UNDEFINED -> TRANSFER_DST.
        if (glassCopyOn) {
            RenderPassDesc cp{};
            cp.name = "glass-scenecopy";
            cp.addUse(ResourceUse{
                rgHdr, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/false });
            cp.addUse(ResourceUse{
                rgSceneCopy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/true });
            cp.recordCtx = this;
            cp.record = [](void* ctx, VkCommandBuffer c){
                auto* self = static_cast<VulkanRenderDevice*>(ctx);
                VkImageCopy region{};
                region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }; // mip0
                region.extent = { self->m_extent.width, self->m_extent.height, 1 };
                vkCmdCopyImage(c, self->m_hdrImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               self->m_sceneCopyImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            };
            m_graph.addPass(std::move(cp));

            // ---- Frost blur chain (M4): downsample mip0 -> mip1 -> ... so the glass
            // shader can pick a blurred LOD by roughness. Added here (right after the
            // copy, before glass) so the whole scene-copy chain is ready when glass
            // samples it. Each pass renders one mip from the previous, larger mip.
            if (glassFrostOn) addGlassFrostPasses(rgSceneCopy);
        }

        // ---- Translucent GLASS pass (transparent meshes) -------------------
        // Drawn AFTER the opaque mesh (+ water) pass into the SAME linear HDR target
        // (LOAD, so the lit scene stays), depth-testing LESS_OR_EQUAL against the
        // stored scene depth WITHOUT writing it, alpha-blended so glass reads as
        // see-through over what's behind. Mirrors the water pass's resource uses
        // (HDR write + read-only depth attachment). Only added when glass was
        // submitted this frame (glassOn) — zero cost otherwise (spec §5).
        if (glassOn) {
            m_glassColorAttach = VkRenderingAttachmentInfo{};
            m_glassColorAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            m_glassColorAttach.imageView = m_hdrView;
            m_glassColorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            m_glassColorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;   // keep the lit scene
            m_glassColorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            m_glassDepthAttach = VkRenderingAttachmentInfo{};
            m_glassDepthAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            m_glassDepthAttach.imageView = m_depthView;
            m_glassDepthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL; // read-only depth
            m_glassDepthAttach.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            m_glassDepthAttach.storeOp = VK_ATTACHMENT_STORE_OP_NONE;

            RenderPassDesc glassPass{};
            glassPass.name = "glass";
            glassPass.addUse(ResourceUse{
                rgHdr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/true });
            // Depth: read-only attachment (LEQUAL test, no write). The graph derives
            // the DEPTH_ATTACHMENT -> DEPTH_READ_ONLY transition from the main pass.
            glassPass.addUse(ResourceUse{
                rgDepth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                VK_IMAGE_ASPECT_DEPTH_BIT, /*isWrite=*/false });
            // glass.frag samples the shadow map (sun-lit glass) — declare it READ so
            // the graph keeps it in DEPTH_READ_ONLY through this pass.
            glassPass.addUse(ResourceUse{
                rgShadow, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_IMAGE_ASPECT_DEPTH_BIT, /*isWrite=*/false });
            if (ssaoOn) {
                glassPass.addUse(ResourceUse{
                    rgSsaoBlur, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/false });
            }
            // Scene-color copy (set 4, binding 0): glass.frag samples the scene
            // behind it (refraction M2 / frost M4). Declare it READ so the graph
            // transitions the copy chain TRANSFER_DST/COLOR_ATTACHMENT ->
            // SHADER_READ_ONLY before this pass. Only when the copy ran this frame.
            if (glassCopyOn) {
                glassPass.addUse(ResourceUse{
                    rgSceneCopy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/false });
            }
            // Frostiest blur level (set 4, binding 2): glass.frag samples it for the
            // M4 frost lerp. The deepest frost pass left it COLOR_ATTACHMENT, so
            // declare it READ here to transition it -> SHADER_READ_ONLY before draw.
            if (glassFrostOn && m_glassFrostRg[kGlassFrostLevels - 1].valid()) {
                glassPass.addUse(ResourceUse{
                    m_glassFrostRg[kGlassFrostLevels - 1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/false });
            }
            glassPass.usesDynamicRendering = true;
            m_glassRenderInfo = VkRenderingInfo{};
            m_glassRenderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            m_glassRenderInfo.renderArea = { {0,0}, m_extent };
            m_glassRenderInfo.layerCount = 1;
            m_glassRenderInfo.colorAttachmentCount = 1;
            m_glassRenderInfo.pColorAttachments = &m_glassColorAttach;
            m_glassRenderInfo.pDepthAttachment = &m_glassDepthAttach;
            glassPass.renderInfo = m_glassRenderInfo;
            glassPass.recordCtx = this;
            glassPass.record = [](void* ctx, VkCommandBuffer c){
                static_cast<VulkanRenderDevice*>(ctx)->recordGlassPassBody(c); };
            m_graph.addPass(std::move(glassPass));
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

        // ---- RT-AO apply pass (hardware ray query) --------------------------
        // After the lit scene (+ optional GI) exists, MULTIPLY the linear HDR target
        // by the ray-traced AO (depth-aware up-sampled from the half-res RT AO image).
        // The pipeline uses a dstColor*srcColor blend, so this pass writes the AO
        // darkening factor and the blender multiplies it into the HDR scene without
        // reading it back. Reads the AO image (SHADER_READ_ONLY) + depth; writes HDR.
        // Runs before bloom so the darkened scene drives the bloom chain correctly.
        if (rtaoOn) {
            m_rtaoApplyAttach = VkRenderingAttachmentInfo{};
            m_rtaoApplyAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            m_rtaoApplyAttach.imageView = m_hdrView;
            m_rtaoApplyAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            m_rtaoApplyAttach.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;     // keep the lit scene
            m_rtaoApplyAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            RenderPassDesc ap{};
            ap.name = "rtao-apply";
            ap.addUse(ResourceUse{
                rgHdr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/true });
            ap.addUse(ResourceUse{
                rgRtao, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/false });
            ap.addUse(ResourceUse{
                rgDepth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_IMAGE_ASPECT_DEPTH_BIT, /*isWrite=*/false });
            ap.usesDynamicRendering = true;
            m_rtaoApplyRenderInfo = VkRenderingInfo{};
            m_rtaoApplyRenderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            m_rtaoApplyRenderInfo.renderArea = { {0,0}, m_extent };
            m_rtaoApplyRenderInfo.layerCount = 1;
            m_rtaoApplyRenderInfo.colorAttachmentCount = 1;
            m_rtaoApplyRenderInfo.pColorAttachments = &m_rtaoApplyAttach;
            ap.renderInfo = m_rtaoApplyRenderInfo;
            ap.recordCtx = this;
            ap.record = [](void* ctx, VkCommandBuffer c){
                auto* self = static_cast<VulkanRenderDevice*>(ctx);
                VkViewport vp{ 0.0f, 0.0f, (float)self->m_extent.width, (float)self->m_extent.height, 0.0f, 1.0f };
                VkRect2D scis{ {0,0}, self->m_extent };
                vkCmdSetViewport(c, 0, 1, &vp);
                vkCmdSetScissor(c, 0, 1, &scis);
                vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, self->m_rtaoApplyPipe);
                vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, self->m_rtaoApplyLayout,
                                        0, 1, &self->m_rtaoApplySet[self->m_frameIdx], 0, nullptr);
                vkCmdPushConstants(c, self->m_rtaoApplyLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(RtaoApplyPush), &self->m_rtaoApplyPush);
                vkCmdDraw(c, 3, 1, 0, 0);
            };
            m_graph.addPass(std::move(ap));
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
        // TAA RESOLVE (temporal anti-aliasing) — after the LAST HDR writer
        // (particles), BEFORE auto-exposure / bloom / composite, the standard
        // order: scene -> TAA -> bloom -> AE -> tonemap -> UI. Reads the finished
        // jittered HDR scene + the scene depth + the persistent history image,
        // writes the resolved TAA output; a tiny copy pass then refreshes the
        // history from the output for next frame. Everything downstream (AE,
        // bloom bright-pass, composite) reads the TAA OUTPUT instead of the raw
        // HDR scene when TAA is on. Gated: r_taa 0 adds zero passes.
        // ----------------------------------------------------------------
        if (taaOn) {
            m_taaAttach = VkRenderingAttachmentInfo{};
            m_taaAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            m_taaAttach.imageView = m_taaOutView;
            m_taaAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            m_taaAttach.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // fully written
            m_taaAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            RenderPassDesc tp{};
            tp.name = "taa-resolve";
            tp.addUse(ResourceUse{
                rgTaaOut, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/true });
            tp.addUse(ResourceUse{
                rgHdr, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/false });
            tp.addUse(ResourceUse{
                rgTaaHist, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/false });
            tp.addUse(ResourceUse{
                rgDepth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_IMAGE_ASPECT_DEPTH_BIT, /*isWrite=*/false });
            tp.usesDynamicRendering = true;
            m_taaRenderInfo = VkRenderingInfo{};
            m_taaRenderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            m_taaRenderInfo.renderArea = { {0,0}, m_extent };
            m_taaRenderInfo.layerCount = 1;
            m_taaRenderInfo.colorAttachmentCount = 1;
            m_taaRenderInfo.pColorAttachments = &m_taaAttach;
            tp.renderInfo = m_taaRenderInfo;
            tp.recordCtx = this;
            tp.record = [](void* ctx, VkCommandBuffer c){
                auto* self = static_cast<VulkanRenderDevice*>(ctx);
                self->postViewport(c, self->m_extent);
                vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, self->m_taaPipe);
                vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, self->m_taaLayout,
                                        0, 1, &self->m_taaSet[self->m_frameIdx], 0, nullptr);
                vkCmdDraw(c, 3, 1, 0, 0);
            };
            m_graph.addPass(std::move(tp));

            // History refresh: copy the resolved output into the persistent
            // history image for next frame's reprojection. A full-image copy of
            // one RGBA16F target — trivial bandwidth, and it keeps every
            // downstream consumer reading ONE stable image (no per-frame
            // ping-pong descriptor rewrites).
            RenderPassDesc hc{};
            hc.name = "taa-history-copy";
            hc.addUse(ResourceUse{
                rgTaaOut, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/false });
            hc.addUse(ResourceUse{
                rgTaaHist, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/true });
            hc.recordCtx = this;
            hc.record = [](void* ctx, VkCommandBuffer c){
                auto* self = static_cast<VulkanRenderDevice*>(ctx);
                VkImageCopy region{};
                region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                region.extent = { self->m_extent.width, self->m_extent.height, 1 };
                vkCmdCopyImage(c, self->m_taaOutImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               self->m_taaHistImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &region);
            };
            m_graph.addPass(std::move(hc));
        }

        // The image AE/bloom/composite read: TAA output when on, raw HDR otherwise.
        const RgResource rgPostSrc = taaOn ? rgTaaOut : rgHdr;

        // ================================================================
        // AUTO-EXPOSURE + BLOOM CHAIN + HDR COMPOSITE (HDR pipeline).
        // ----------------------------------------------------------------
        // Auto-exposure: a single-workgroup compute reduce of the finished HDR
        // scene -> adapted exposure SSBO (read by the composite). Runs before the
        // bloom chain (both only READ the HDR target; order between them is
        // irrelevant, but AE must precede the composite). The SSBO is not a graph
        // resource (buffers are not graph-tracked, documented model); the record
        // body emits its own pre/post barriers on it. r_autoexposure-gated.
        const bool aeOn = m_post.autoExposure && (m_aePipe != VK_NULL_HANDLE);
        if (aeOn) {
            // Adaptation dt from a steady clock (renderer-owned so every caller —
            // interactive or headless — gets correct eye-adaptation pacing).
            const double tNow = std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            float aeDt = (m_aePrevTime >= 0.0) ? (float)(tNow - m_aePrevTime) : 0.0f;
            m_aePrevTime = tNow;
            if (aeDt < 0.0f)  aeDt = 0.0f;
            if (aeDt > 0.1f)  aeDt = 0.1f;   // stall guard: never adapt a huge step
            // Determinism: SNAP on the first frame / AE re-enable, and on EVERY
            // headless frame so --screenshot* captures are bit-reproducible.
            const bool snap = m_aeSnap || m_headless;
            m_aeSnap = false;
            m_aePush = AePush{ aeDt, m_post.aeSpeed, m_post.aeMin, m_post.aeMax,
                               m_post.aeKey, snap ? 1 : 0, 0.0f, 0.0f };

            RenderPassDesc ae{};
            ae.name = "auto-exposure";
            ae.queue = RgQueue::Compute;
            ae.usesDynamicRendering = false;
            ae.addUse(ResourceUse{
                rgPostSrc, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/false });
            ae.recordCtx = this;
            ae.record = [](void* ctx, VkCommandBuffer c){
                static_cast<VulkanRenderDevice*>(ctx)->recordAutoExposureBody(c); };
            m_graph.addPass(std::move(ae));
        }

        // Bloom: downsample pass 0 bright-passes the HDR scene into mip0
        // (Karis-average 13-tap), passes 1..N-1 progressively downsample
        // mip[i-1] -> mip[i]. Upsample: from the smallest mip back up, each step
        // tent-filters mip[i+1] and ADDITIVELY blends it onto mip[i] (pipeline
        // ONE,ONE blend). Result: mip0 holds the full accumulated bloom. The graph
        // derives every COLOR_ATTACHMENT <-> SHADER_READ_ONLY transition between
        // the mips. r_bloom 0 skips the WHOLE chain (the composite's intensity is
        // forced 0 and its shader guards the mip0 sample, so the untouched mip is
        // never read).
        const bool bloomOn = m_post.bloomEnabled;
        if (bloomOn) addBloomPasses(rgPostSrc, rgMip);

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
            // READ the post source (TAA output when on, raw HDR scene otherwise)
            // + bloom mip0 (both sampled in the fragment stage).
            comp.addUse(ResourceUse{
                rgPostSrc, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
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
                // TAA on: binding 0 samples the TAA RESOLVE output instead of the
                // raw HDR scene (same layout, alternate pre-written set).
                VkDescriptorSet compSet = self->m_taaActiveThisFrame
                    ? self->m_setCompositeTaa : self->m_setComposite;
                vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, self->m_compositeLayout,
                                        0, 1, &compSet, 0, nullptr);
                CompositePush cp{};
                // Effective bloom strength: r_bloom 0 forces 0 (chain skipped this
                // frame; the shader's >0 guard never samples the untouched mip);
                // r_bloomintensity >= 0 overrides the scene-tuned setBloom() value.
                cp.bloomIntensity = self->m_post.bloomEnabled
                    ? ((self->m_post.bloomIntensity >= 0.0f) ? self->m_post.bloomIntensity
                                                             : self->m_bloomIntensity)
                    : 0.0f;
                cp.exposure    = self->m_exposure;   // r_exposure (bias when AE on)
                cp.tonemapMode = self->m_post.tonemapMode;             // r_tonemap
                cp.aeEnabled   = (self->m_post.autoExposure && self->m_aePipe != VK_NULL_HANDLE) ? 1 : 0;
                // Post-TAA sharpen (r_taasharpen). FORCED 0 when TAA is off this
                // frame so the r_taa 0 path samples exactly one center tap —
                // byte-identical to the pre-TAA composite.
                cp.sharpen = self->m_taaActiveThisFrame
                    ? std::max(0.0f, self->m_post.taaSharpen) : 0.0f;
                cp.texelW  = 1.0f / (float)std::max(1u, self->m_extent.width);
                cp.texelH  = 1.0f / (float)std::max(1u, self->m_extent.height);
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

        // ================================================================
        // EDITOR UI (Dear ImGui) — EDITOR-ONLY. Inserted AFTER composite/HUD and
        // BEFORE the present-finalize/capture pass, so ImGui panels draw on TOP of
        // the fully composited scene + game HUD, and the present transition that
        // follows is unchanged. Gated on (m_imguiInit && draw data exists this
        // frame): a non-editor run never enters here (zero added passes/cost). The
        // pass loads (does NOT clear) the composited color, blends ImGui over it,
        // and leaves rgColor in COLOR_ATTACHMENT_OPTIMAL — exactly the state the
        // present-finalize pass expects, and the capture-copy path is untouched
        // (editor UI never inits in headless mode, so this never runs under
        // --screenshot). This is editor-only and separate from the FontRole HUD.
        if (m_imguiInit && m_editorDrawData && m_editorDrawData->CmdListsCount > 0) {
            m_editorUiAttach = VkRenderingAttachmentInfo{};
            m_editorUiAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            m_editorUiAttach.imageView = colorTargetView;
            m_editorUiAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            m_editorUiAttach.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;   // preserve scene+HUD
            m_editorUiAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            RenderPassDesc ui{};
            ui.name = "editor-ui";
            // WRITE the final color target (depend on the composite write so the
            // graph derives the COLOR_ATTACHMENT_OUTPUT -> COLOR_ATTACHMENT_OUTPUT
            // execution+memory dependency automatically — no hand-coded barrier).
            ui.addUse(ResourceUse{
                rgColor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/true });
            ui.usesDynamicRendering = true;
            m_editorUiRenderInfo = VkRenderingInfo{};
            m_editorUiRenderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            m_editorUiRenderInfo.renderArea = { {0,0}, m_extent };
            m_editorUiRenderInfo.layerCount = 1;
            m_editorUiRenderInfo.colorAttachmentCount = 1;
            m_editorUiRenderInfo.pColorAttachments = &m_editorUiAttach;
            ui.renderInfo = m_editorUiRenderInfo;
            ui.recordCtx = this;
            ui.record = [](void* ctx, VkCommandBuffer c){
                auto* self = static_cast<VulkanRenderDevice*>(ctx);
                VkViewport vp{ 0.0f, 0.0f, (float)self->m_extent.width,
                               (float)self->m_extent.height, 0.0f, 1.0f };
                VkRect2D scis{ {0,0}, self->m_extent };
                vkCmdSetViewport(c, 0, 1, &vp);
                vkCmdSetScissor(c, 0, 1, &scis);
                // ImGui records its own draws into the live (dynamic-rendering) pass.
                ImGui_ImplVulkan_RenderDrawData(self->m_editorDrawData, c);
            };
            m_graph.addPass(std::move(ui));
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

        // D15 HZB: persist the depth buffer's post-frame state too (next frame's
        // pyramid reduce imports it preserved) + mark its contents rendered.
        m_depthState = m_graph.stateOf(rgDepth);
        m_depthValid = true;

        // TAA history: persist its post-frame state (TRANSFER_DST after the
        // history-copy) so next frame's import derives the correct transition,
        // and mark the history VALID — the copy pass just refreshed it, so the
        // next resolve may reproject against it.
        if (taaOn) {
            m_taaHistState = m_graph.stateOf(rgTaaHist);
            m_taaHistoryValid = true;
        }

        // DDGI atlases: persist their post-frame state (SHADER_READ_ONLY after
        // the main pass sampled them) so next frame's import derives the correct
        // cross-frame transition. The probe field itself is the history.
        if (ddgiOn) {
            m_ddgiIrrState = m_graph.stateOf(rgDdgiIrr);
            m_ddgiVisState = m_graph.stateOf(rgDdgiVis);
            ++m_ddgiFrameCount;   // warm-up ramp progress (hysteresis + intensity)
        }

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
            /* 0 Console/HudMono */ { "Consolas.ttf",                                 false, "Consolas" }, // matches the BabylonJS x3-console (Consolas,Courier New,monospace); embedded Roboto Mono is the fallback
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

    // =====================================================================
    // IMAGE-BASED LIGHTING (IBL) — split-sum environment reflections.
    //
    // Bakes (from the analytic sky) a diffuse irradiance cube + a roughness-mipped
    // specular prefilter cube + a scene-independent BRDF LUT, then exposes them as
    // mesh.frag set 4. createIbl() builds all GPU objects (idempotent w.r.t. the
    // sky); regenIblFromSky() runs the four fullscreen bake passes on a one-time
    // submit (init + whenever the sky changes). Everything is additive: if the IBL
    // resources fail to build, m_iblReady stays false and mesh.frag falls back to
    // its old flat ambient term (the descriptor set is still bound but the shader
    // gates on an "iblValid" flag carried in the SSAO control UBO's spare lane).
    // =====================================================================

    // std140 mirror of IblSkyUBO in ibl_env.frag (5 vec4s = 80 bytes).
    struct IblSkyUBO {
        glm::vec4 sunDir;
        glm::vec4 sunColor;
        glm::vec4 params;   // x=haze y=exposure z=time w=enabled
        glm::vec4 zenith;
        glm::vec4 horizon;
    };
    // Per-face push constant for the env/irradiance pass (3 vec4) + prefilter (4 vec4).
    struct IblFacePush {
        glm::vec4 faceFwd;
        glm::vec4 faceRight;
        glm::vec4 faceUp;
        glm::vec4 misc;     // prefilter: x=roughness y=resolution; ignored by env/irradiance
    };

    // The six cube-face direction bases (matches Vulkan cube face order +X,-X,+Y,-Y,+Z,-Z).
    // fwd = face center dir; right/up span the face so dir = fwd + (2u-1)*right + (2v-1)*up.
    // Chosen so the assembled cube is consistent (standard GL/Vulkan cubemap convention).
    static void iblFaceBasis(int face, glm::vec3& fwd, glm::vec3& right, glm::vec3& up) {
        switch (face) {
            case 0: fwd={ 1, 0, 0}; right={ 0, 0,-1}; up={0,-1,0}; break; // +X
            case 1: fwd={-1, 0, 0}; right={ 0, 0, 1}; up={0,-1,0}; break; // -X
            case 2: fwd={ 0, 1, 0}; right={ 1, 0, 0}; up={0, 0,1}; break; // +Y
            case 3: fwd={ 0,-1, 0}; right={ 1, 0, 0}; up={0, 0,-1}; break;// -Y
            case 4: fwd={ 0, 0, 1}; right={ 1, 0, 0}; up={0,-1,0}; break; // +Z
            default:fwd={ 0, 0,-1}; right={-1, 0, 0}; up={0,-1,0}; break; // -Z
        }
    }

    // Create one cubemap image (6 layers) + a CUBE view + per-face single-mip RT
    // views (mip `rtMip` of each face). When mipLevels>1 the image is mip-complete.
    bool createIblCube(uint32_t size, uint32_t mipLevels, VkImageUsageFlags usage,
                       VkImage& outImg, VmaAllocation& outAlloc,
                       VkImageView& outCubeView, VkImageView* outFaceViews, uint32_t rtMip) {
        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        ici.imageType = VK_IMAGE_TYPE_2D; ici.format = kIblCubeFormat;
        ici.extent = { size, size, 1 }; ici.mipLevels = mipLevels; ici.arrayLayers = 6;
        ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = usage; ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        if (x3vmaCreateImage(&ici, &aci, &outImg, &outAlloc, nullptr) != VK_SUCCESS) return false;
        VkImageViewCreateInfo cv{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        cv.image = outImg; cv.viewType = VK_IMAGE_VIEW_TYPE_CUBE; cv.format = kIblCubeFormat;
        cv.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 6 };
        if (vkCreateImageView(m_dev.device, &cv, nullptr, &outCubeView) != VK_SUCCESS) return false;
        if (outFaceViews) {
            for (uint32_t f = 0; f < 6; ++f) {
                VkImageViewCreateInfo fv{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
                fv.image = outImg; fv.viewType = VK_IMAGE_VIEW_TYPE_2D; fv.format = kIblCubeFormat;
                fv.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, rtMip, 1, f, 1 };
                if (vkCreateImageView(m_dev.device, &fv, nullptr, &outFaceViews[f]) != VK_SUCCESS) return false;
            }
        }
        return true;
    }

    // Build all IBL GPU objects (images, views, samplers, descriptor layouts/sets,
    // and the four bake pipelines). Does NOT bake (that's regenIblFromSky()).
    bool createIbl() {
        m_iblEnvMips = (uint32_t)std::floor(std::log2((float)kIblEnvSize)) + 1u;

        // ---- Images + views ----
        const VkImageUsageFlags cubeUsage =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!createIblCube(kIblEnvSize, m_iblEnvMips, cubeUsage,
                           m_iblEnvImg, m_iblEnvAlloc, m_iblEnvCubeView, m_iblEnvFaceView, 0)) {
            logError("[rhi] IBL env cube create failed"); return false;
        }
        if (!createIblCube(kIblIrradSize, 1, cubeUsage,
                           m_iblIrradImg, m_iblIrradAlloc, m_iblIrradCubeView, m_iblIrradFaceView, 0)) {
            logError("[rhi] IBL irradiance cube create failed"); return false;
        }
        // Prefilter: kIblPrefilterMips mips; one RT view per (mip,face).
        {
            VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            ici.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
            ici.imageType = VK_IMAGE_TYPE_2D; ici.format = kIblCubeFormat;
            ici.extent = { kIblPrefilterSize, kIblPrefilterSize, 1 };
            ici.mipLevels = kIblPrefilterMips; ici.arrayLayers = 6;
            ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
            ici.usage = cubeUsage; ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
            if (x3vmaCreateImage(&ici, &aci, &m_iblPrefImg, &m_iblPrefAlloc, nullptr) != VK_SUCCESS) {
                logError("[rhi] IBL prefilter cube create failed"); return false;
            }
            VkImageViewCreateInfo cv{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            cv.image = m_iblPrefImg; cv.viewType = VK_IMAGE_VIEW_TYPE_CUBE; cv.format = kIblCubeFormat;
            cv.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, kIblPrefilterMips, 0, 6 };
            if (vkCreateImageView(m_dev.device, &cv, nullptr, &m_iblPrefCubeView) != VK_SUCCESS) return false;
            for (uint32_t mip = 0; mip < kIblPrefilterMips; ++mip)
                for (uint32_t f = 0; f < 6; ++f) {
                    VkImageViewCreateInfo fv{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
                    fv.image = m_iblPrefImg; fv.viewType = VK_IMAGE_VIEW_TYPE_2D; fv.format = kIblCubeFormat;
                    fv.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, f, 1 };
                    if (vkCreateImageView(m_dev.device, &fv, nullptr, &m_iblPrefFaceView[mip][f]) != VK_SUCCESS) return false;
                }
        }
        // BRDF LUT (2D RG16F).
        if (!createColorTarget(kIblBrdfFormat, kIblBrdfSize, kIblBrdfSize,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               m_iblBrdfImg, m_iblBrdfAlloc, m_iblBrdfView)) {
            logError("[rhi] IBL BRDF LUT create failed"); return false;
        }

        // ---- Samplers ----
        {
            VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
            sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sci.maxLod = VK_LOD_CLAMP_NONE;
            if (vkCreateSampler(m_dev.device, &sci, nullptr, &m_iblCubeSampler) != VK_SUCCESS) return false;
            VkSamplerCreateInfo bs = sci; bs.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; bs.maxLod = 0.0f;
            if (vkCreateSampler(m_dev.device, &bs, nullptr, &m_iblBrdfSampler) != VK_SUCCESS) return false;
        }

        // ---- Descriptor set layouts ----
        // set0 for env capture: one UBO (IblSkyUBO).
        {
            VkDescriptorSetLayoutBinding b{}; b.binding = 0; b.descriptorCount = 1;
            b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = 1; ci.pBindings = &b;
            if (vkCreateDescriptorSetLayout(m_dev.device, &ci, nullptr, &m_iblSkyUboSetLayout) != VK_SUCCESS) return false;
        }
        // set0 for convolve passes: one cube sampler.
        {
            VkDescriptorSetLayoutBinding b{}; b.binding = 0; b.descriptorCount = 1;
            b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = 1; ci.pBindings = &b;
            if (vkCreateDescriptorSetLayout(m_dev.device, &ci, nullptr, &m_iblCubeSetLayout) != VK_SUCCESS) return false;
        }
        // (mesh.frag set 4 layout m_iblMeshSetLayout was created in createGraphics so
        //  the mesh pipeline layout could include it; we only ALLOCATE its set here.)

        // ---- Sky UBO buffer (host-mapped, written before each bake) ----
        {
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = sizeof(IblSkyUBO); bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info{};
            if (x3vmaCreateBuffer(&bci, &aci, &m_iblSkyUboBuf, &m_iblSkyUboAlloc, &info) != VK_SUCCESS) return false;
            m_iblSkyUboMapped = info.pMappedData;
        }

        // ---- Descriptor pool + sets (bake-side: sky UBO set + env-cube set) ----
        {
            VkDescriptorPoolSize sizes[2]{};
            sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; sizes[0].descriptorCount = 1;
            sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sizes[1].descriptorCount = 2;
            VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.maxSets = 2; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
            if (x3CreateDescriptorPool(&pci, nullptr, &m_iblBakePool) != VK_SUCCESS) return false;
            VkDescriptorSetAllocateInfo a0{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            a0.descriptorPool = m_iblBakePool; a0.descriptorSetCount = 1; a0.pSetLayouts = &m_iblSkyUboSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &a0, &m_iblSkyUboSet) != VK_SUCCESS) return false;
            VkDescriptorSetAllocateInfo a1{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            a1.descriptorPool = m_iblBakePool; a1.descriptorSetCount = 1; a1.pSetLayouts = &m_iblCubeSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &a1, &m_iblEnvCubeSet) != VK_SUCCESS) return false;
            // Write the sky UBO into the env set; the env cube into the convolve set.
            VkDescriptorBufferInfo dbi{ m_iblSkyUboBuf, 0, sizeof(IblSkyUBO) };
            VkDescriptorImageInfo  dci{ m_iblCubeSampler, m_iblEnvCubeView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet w[2]{};
            w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = m_iblSkyUboSet;
            w[0].dstBinding = 0; w[0].descriptorCount = 1;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &dbi;
            w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = m_iblEnvCubeSet;
            w[1].dstBinding = 0; w[1].descriptorCount = 1;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo = &dci;
            vkUpdateDescriptorSets(m_dev.device, 2, w, 0, nullptr);
        }

        // ---- mesh.frag set 4 pool + set (written after the first bake) ----
        {
            VkDescriptorPoolSize sz{}; sz.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sz.descriptorCount = 3;
            VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.maxSets = 1; pci.poolSizeCount = 1; pci.pPoolSizes = &sz;
            if (x3CreateDescriptorPool(&pci, nullptr, &m_iblMeshPool) != VK_SUCCESS) return false;
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = m_iblMeshPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &m_iblMeshSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &ai, &m_iblMeshSet) != VK_SUCCESS) return false;
        }

        // ---- Pipelines (fullscreen-triangle vertex shader) ----
        // Env capture: set0 = sky UBO, push = IblFacePush.
        {
            VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(IblFacePush) };
            VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pl.setLayoutCount = 1; pl.pSetLayouts = &m_iblSkyUboSetLayout;
            pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr;
            if (vkCreatePipelineLayout(m_dev.device, &pl, nullptr, &m_iblEnvLayout) != VK_SUCCESS) return false;
            if (!createFullscreenPipeline("shaders\\fullscreen.vert.spv", "shaders\\ibl_env.frag.spv",
                                          m_iblEnvLayout, kIblCubeFormat, false, m_iblEnvPipe)) return false;
        }
        // Convolve (irradiance + prefilter share this layout): set0 = cube sampler, push = IblFacePush.
        {
            VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(IblFacePush) };
            VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pl.setLayoutCount = 1; pl.pSetLayouts = &m_iblCubeSetLayout;
            pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr;
            if (vkCreatePipelineLayout(m_dev.device, &pl, nullptr, &m_iblCubeLayout) != VK_SUCCESS) return false;
            if (!createFullscreenPipeline("shaders\\fullscreen.vert.spv", "shaders\\ibl_irradiance.frag.spv",
                                          m_iblCubeLayout, kIblCubeFormat, false, m_iblIrradPipe)) return false;
            if (!createFullscreenPipeline("shaders\\fullscreen.vert.spv", "shaders\\ibl_prefilter.frag.spv",
                                          m_iblCubeLayout, kIblCubeFormat, false, m_iblPrefPipe)) return false;
        }
        // BRDF LUT: no sets, no push.
        {
            VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            if (vkCreatePipelineLayout(m_dev.device, &pl, nullptr, &m_iblBrdfLayout) != VK_SUCCESS) return false;
            if (!createFullscreenPipeline("shaders\\fullscreen.vert.spv", "shaders\\ibl_brdf_lut.frag.spv",
                                          m_iblBrdfLayout, kIblBrdfFormat, false, m_iblBrdfPipe)) return false;
        }

        // ---- Initialize all sampled images to SHADER_READ_ONLY (contents undefined)
        // so mesh.frag set 4 is bindable from the very first draw even if a bake has
        // not run yet. A plain UNDEFINED->SHADER_READ layout transition is enough
        // (no clear / no TRANSFER_DST needed) since the gate flag (ssao.ibl.x) keeps
        // the shader from sampling these until a real bake completes; the first-frame
        // bake overwrites the contents regardless.
        bool clr = oneTimeSubmit([&](VkCommandBuffer cmd){
            iblBarrier(cmd, m_iblEnvImg, 0, m_iblEnvMips, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            iblBarrier(cmd, m_iblIrradImg, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            iblBarrier(cmd, m_iblPrefImg, 0, kIblPrefilterMips, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            iblBarrierTex2D(cmd, m_iblBrdfImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        });
        if (!clr) { logError("[rhi] IBL initial layout transition failed"); return false; }
        // Write mesh.frag set 4 now (points at the cleared cubes/LUT); regenIblFromSky
        // re-points it after each bake (identical views, so this is just safety).
        {
            VkDescriptorImageInfo di0{ m_iblCubeSampler, m_iblIrradCubeView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo di1{ m_iblCubeSampler, m_iblPrefCubeView,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo di2{ m_iblBrdfSampler, m_iblBrdfView,      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet w[3]{};
            for (int i = 0; i < 3; ++i) { w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = m_iblMeshSet;
                w[i].dstBinding = i; w[i].descriptorCount = 1; w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; }
            w[0].pImageInfo = &di0; w[1].pImageInfo = &di1; w[2].pImageInfo = &di2;
            vkUpdateDescriptorSets(m_dev.device, 3, w, 0, nullptr);
        }

        // Reflection-probe depth target (env-face sized) for the optional scene bake.
        {
            VkImageCreateInfo di{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            di.imageType = VK_IMAGE_TYPE_2D; di.format = m_depthFormat;
            di.extent = { kIblEnvSize, kIblEnvSize, 1 }; di.mipLevels = 1; di.arrayLayers = 1;
            di.samples = VK_SAMPLE_COUNT_1_BIT; di.tiling = VK_IMAGE_TILING_OPTIMAL;
            di.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            VmaAllocationCreateInfo da{}; da.usage = VMA_MEMORY_USAGE_AUTO;
            da.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
            if (x3vmaCreateImage(&di, &da, &m_probeDepthImg, &m_probeDepthAlloc, nullptr) == VK_SUCCESS) {
                VkImageViewCreateInfo dv{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
                dv.image = m_probeDepthImg; dv.viewType = VK_IMAGE_VIEW_TYPE_2D; dv.format = m_depthFormat;
                dv.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
                vkCreateImageView(m_dev.device, &dv, nullptr, &m_probeDepthView);
            } else {
                logError("[rhi] probe depth image create failed (reflection probe disabled)");
            }
        }

        m_iblReady = true;
        logInfo("[rhi] IBL ready (env 256 + irradiance 32 + prefilter 128/5mip + BRDF LUT 256, split-sum)");
        return true;
    }

    // Small helper: render the fullscreen triangle into a single image-view attachment.
    void iblRenderTo(VkCommandBuffer cmd, VkImageView target, uint32_t w, uint32_t h,
                     VkPipeline pipe, VkPipelineLayout layout, VkDescriptorSet set,
                     const IblFacePush* push) {
        VkRenderingAttachmentInfo att{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        att.imageView = target; att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        ri.renderArea = { {0,0}, {w,h} }; ri.layerCount = 1;
        ri.colorAttachmentCount = 1; ri.pColorAttachments = &att;
        vkCmdBeginRendering(cmd, &ri);
        VkViewport vp{ 0,0,(float)w,(float)h,0,1 }; VkRect2D sc{ {0,0},{w,h} };
        vkCmdSetViewport(cmd, 0, 1, &vp); vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        if (set) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
        if (push) vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(IblFacePush), push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
    }

    // sync2 image-layout barrier for an IBL image subresource range.
    void iblBarrier(VkCommandBuffer cmd, VkImage img, uint32_t baseMip, uint32_t mipCount,
                    VkImageLayout oldL, VkImageLayout newL,
                    VkPipelineStageFlags2 ss, VkAccessFlags2 sa,
                    VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
        VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        b.srcStageMask = ss; b.srcAccessMask = sa; b.dstStageMask = ds; b.dstAccessMask = da;
        b.oldLayout = oldL; b.newLayout = newL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img; b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mipCount, 0, 6 };
        VkDependencyInfo di{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &di);
    }

    // Reflection-probe scene bake: render opaque scene geometry into all 6 env-cube
    // faces from m_iblProbePos (90deg/face), so the IBL env captures the dim INTERIOR
    // instead of the bright sky. The env image is already in COLOR_ATTACHMENT (mip0, 6
    // layers). Each face clears to a dim ambient backdrop (window openings / gaps) then
    // draws the scene via the probe PSO (push-constant per-face viewProj). One shared
    // depth target, WAW-barriered between faces. Lighting reuses mesh.frag (direct +
    // fallback ambient on the first bake, since IBL isn't valid yet) — no recursion.
    void bakeProbeSceneIntoEnv(VkCommandBuffer cmd) {
        auto& fr = m_frames[m_frameIdx];
        const glm::vec3 clear = m_ambient * 0.5f;   // dim interior backdrop for gaps/openings
        // probe depth -> DEPTH_ATTACHMENT (contents discarded each bake).
        VkImageMemoryBarrier2 db{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        db.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT; db.srcAccessMask = 0;
        db.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        db.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        db.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; db.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        db.srcQueueFamilyIndex = db.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        db.image = m_probeDepthImg; db.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        VkDependencyInfo ddi{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO }; ddi.imageMemoryBarrierCount = 1; ddi.pImageMemoryBarriers = &db;
        vkCmdPipelineBarrier2(cmd, &ddi);

        VkDescriptorSet sets[5] = { m_bindlessSet, fr.objSet, m_shadowSet, m_meshAoSet[m_frameIdx], m_iblMeshSet };
        for (int f = 0; f < 6; ++f) {
            glm::vec3 fwd, right, up; iblFaceBasis(f, fwd, right, up);
            glm::mat4 view = glm::lookAt(m_iblProbePos, m_iblProbePos + fwd, up);
            glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 200.0f); proj[1][1] *= -1.0f;
            glm::mat4 vp = proj * view;

            VkRenderingAttachmentInfo col{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            col.imageView = m_iblEnvFaceView[f]; col.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            col.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            col.clearValue.color = { { clear.r, clear.g, clear.b, 1.0f } };
            VkRenderingAttachmentInfo dep{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            dep.imageView = m_probeDepthView; dep.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            dep.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; dep.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            dep.clearValue.depthStencil = { 1.0f, 0 };
            VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
            ri.renderArea = { {0,0}, {kIblEnvSize, kIblEnvSize} }; ri.layerCount = 1;
            ri.colorAttachmentCount = 1; ri.pColorAttachments = &col; ri.pDepthAttachment = &dep;

            vkCmdBeginRendering(cmd, &ri);
            VkViewport vpp{ 0,0,(float)kIblEnvSize,(float)kIblEnvSize,0,1 }; VkRect2D sc{ {0,0},{kIblEnvSize,kIblEnvSize} };
            vkCmdSetViewport(cmd, 0, 1, &vpp); vkCmdSetScissor(cmd, 0, 1, &sc);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_meshProbePipe);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_meshProbeLayout, 0, 5, sets, 0, nullptr);
            vkCmdPushConstants(cmd, m_meshProbeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &vp);
            for (uint32_t i = 0; i < m_frameCmdOpaque; ++i) {
                const Mesh& mh = m_meshes[m_drawMeshOrder[i]];
                VkDeviceSize off = 0; VkBuffer vb = mh.drawVbo(m_frameIdx);
                vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
                vkCmdBindIndexBuffer(cmd, mh.ibo, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexedIndirect(cmd, fr.indirectBuf,
                    (VkDeviceSize)i * sizeof(VkDrawIndexedIndirectCommand), 1, sizeof(VkDrawIndexedIndirectCommand));
            }
            vkCmdEndRendering(cmd);

            if (f < 5) {   // WAW on the shared probe depth before the next face's CLEAR
                VkImageMemoryBarrier2 wb{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
                wb.srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT; wb.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                wb.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT; wb.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                wb.oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL; wb.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                wb.srcQueueFamilyIndex = wb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                wb.image = m_probeDepthImg; wb.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
                VkDependencyInfo wdi{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO }; wdi.imageMemoryBarrierCount = 1; wdi.pImageMemoryBarriers = &wb;
                vkCmdPipelineBarrier2(cmd, &wdi);
            }
        }
    }

    // Bake the IBL chain from the CURRENT sky params. Runs the BRDF LUT (only once),
    // then env capture (6 faces) -> env mip generation -> irradiance convolve (6
    // faces) -> prefilter (5 mips x 6 faces), all on one one-time submit. Then
    // writes mesh.frag set 4 to point at the fresh irradiance/prefilter/LUT.
    bool regenIblFromSky() {
        if (!m_iblReady) return false;

        // Fill the sky UBO from the cached SkyParams (always 'enabled' for the bake:
        // even indoor levels get a sensible neutral env from their sky colors).
        glm::vec3 sd = glm::normalize(glm::vec3(m_sky.sunDir[0], m_sky.sunDir[1], m_sky.sunDir[2]));
        IblSkyUBO u{};
        u.sunDir   = glm::vec4(sd, 0.0f);
        u.sunColor = glm::vec4(m_sky.sunColor[0], m_sky.sunColor[1], m_sky.sunColor[2], m_sky.sunIntensity);
        u.params   = glm::vec4(m_sky.haze, m_sky.exposure, m_skyTime, m_sky.enabled ? 1.0f : 0.0f);
        u.zenith   = glm::vec4(m_sky.zenith[0], m_sky.zenith[1], m_sky.zenith[2], 0.0f);
        u.horizon  = glm::vec4(m_sky.horizon[0], m_sky.horizon[1], m_sky.horizon[2], 0.0f);
        std::memcpy(m_iblSkyUboMapped, &u, sizeof(u));

        bool ok = oneTimeSubmit([&](VkCommandBuffer cmd){
            // ---- BRDF LUT (only the first time; it never changes) ----
            if (!m_iblBaked) {
                iblBarrierTex2D(cmd, m_iblBrdfImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
                iblRenderTo(cmd, m_iblBrdfView, kIblBrdfSize, kIblBrdfSize, m_iblBrdfPipe, m_iblBrdfLayout, VK_NULL_HANDLE, nullptr);
                iblBarrierTex2D(cmd, m_iblBrdfImg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            }

            // ---- Env capture: render the analytic sky into mip0 of all 6 faces ----
            iblBarrier(cmd, m_iblEnvImg, 0, m_iblEnvMips, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            const bool probeScene = m_iblProbeScene && m_meshProbePipe && m_probeDepthView && m_frameCmdOpaque > 0;
            if (probeScene) {
                // Interior reflection probe: bake the SCENE (around the camera) into the
                // env instead of the sky, so glossy metals reflect the room, not open sky.
                m_iblProbePos = m_camPos;
                bakeProbeSceneIntoEnv(cmd);
            } else {
                for (int f = 0; f < 6; ++f) {
                    glm::vec3 fwd, right, up; iblFaceBasis(f, fwd, right, up);
                    IblFacePush p{}; p.faceFwd = glm::vec4(fwd, 0); p.faceRight = glm::vec4(right, 0); p.faceUp = glm::vec4(up, 0);
                    iblRenderTo(cmd, m_iblEnvFaceView[f], kIblEnvSize, kIblEnvSize, m_iblEnvPipe, m_iblEnvLayout, m_iblSkyUboSet, &p);
                }
            }
            // mip0 -> TRANSFER_SRC; generate the env mip chain by linear blits so the
            // prefilter pass can mip-bias rough lobes (anti-firefly). Then ALL mips -> SHADER_READ.
            iblBarrier(cmd, m_iblEnvImg, 0, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            int32_t mw = (int32_t)kIblEnvSize, mh = (int32_t)kIblEnvSize;
            for (uint32_t mip = 1; mip < m_iblEnvMips; ++mip) {
                iblBarrier(cmd, m_iblEnvImg, mip, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                           VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                int32_t nw = mw > 1 ? mw / 2 : 1, nh = mh > 1 ? mh / 2 : 1;
                VkImageBlit blit{};
                blit.srcOffsets[1] = { mw, mh, 1 }; blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, 6 };
                blit.dstOffsets[1] = { nw, nh, 1 }; blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 6 };
                vkCmdBlitImage(cmd, m_iblEnvImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               m_iblEnvImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
                iblBarrier(cmd, m_iblEnvImg, mip, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                mw = nw; mh = nh;
            }
            iblBarrier(cmd, m_iblEnvImg, 0, m_iblEnvMips, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

            // ---- Irradiance convolve (reads the env cube) ----
            iblBarrier(cmd, m_iblIrradImg, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            for (int f = 0; f < 6; ++f) {
                glm::vec3 fwd, right, up; iblFaceBasis(f, fwd, right, up);
                IblFacePush p{}; p.faceFwd = glm::vec4(fwd, 0); p.faceRight = glm::vec4(right, 0); p.faceUp = glm::vec4(up, 0);
                iblRenderTo(cmd, m_iblIrradFaceView[f], kIblIrradSize, kIblIrradSize, m_iblIrradPipe, m_iblCubeLayout, m_iblEnvCubeSet, &p);
            }
            iblBarrier(cmd, m_iblIrradImg, 0, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

            // ---- Prefilter (reads the env cube) into each roughness mip ----
            iblBarrier(cmd, m_iblPrefImg, 0, kIblPrefilterMips, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            for (uint32_t mip = 0; mip < kIblPrefilterMips; ++mip) {
                uint32_t mipSize = kIblPrefilterSize >> mip; if (mipSize == 0) mipSize = 1;
                float roughness = (kIblPrefilterMips > 1) ? (float)mip / (float)(kIblPrefilterMips - 1) : 0.0f;
                for (int f = 0; f < 6; ++f) {
                    glm::vec3 fwd, right, up; iblFaceBasis(f, fwd, right, up);
                    IblFacePush p{}; p.faceFwd = glm::vec4(fwd, 0); p.faceRight = glm::vec4(right, 0); p.faceUp = glm::vec4(up, 0);
                    p.misc = glm::vec4(roughness, (float)kIblEnvSize, 0, 0);
                    iblRenderTo(cmd, m_iblPrefFaceView[mip][f], mipSize, mipSize, m_iblPrefPipe, m_iblCubeLayout, m_iblEnvCubeSet, &p);
                }
            }
            iblBarrier(cmd, m_iblPrefImg, 0, kIblPrefilterMips, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        });
        if (!ok) { logError("[rhi] IBL bake submit failed"); return false; }

        // Point mesh.frag set 4 at the fresh irradiance + prefilter + BRDF LUT.
        VkDescriptorImageInfo di0{ m_iblCubeSampler, m_iblIrradCubeView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo di1{ m_iblCubeSampler, m_iblPrefCubeView,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo di2{ m_iblBrdfSampler, m_iblBrdfView,      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet w[3]{};
        for (int i = 0; i < 3; ++i) { w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = m_iblMeshSet;
            w[i].dstBinding = i; w[i].descriptorCount = 1; w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; }
        w[0].pImageInfo = &di0; w[1].pImageInfo = &di1; w[2].pImageInfo = &di2;
        vkUpdateDescriptorSets(m_dev.device, 3, w, 0, nullptr);

        m_iblBaked = true; m_iblDirty = false;
        return true;
    }

    // 2D version of iblBarrier (BRDF LUT, 1 layer).
    void iblBarrierTex2D(VkCommandBuffer cmd, VkImage img, VkImageLayout oldL, VkImageLayout newL,
                         VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
        VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        b.srcStageMask = ss; b.srcAccessMask = sa; b.dstStageMask = ds; b.dstAccessMask = da;
        b.oldLayout = oldL; b.newLayout = newL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img; b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkDependencyInfo di{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &di);
    }

    void destroyIbl() {
        auto killView = [&](VkImageView& v){ if (v) { vkDestroyImageView(m_dev.device, v, nullptr); v = VK_NULL_HANDLE; } };
        auto killImg  = [&](VkImage& i, VmaAllocation& a){ if (i) { vmaDestroyImage(m_alloc, i, a); i = VK_NULL_HANDLE; a = nullptr; } };
        auto killPipe = [&](VkPipeline& p){ if (p) { vkDestroyPipeline(m_dev.device, p, nullptr); p = VK_NULL_HANDLE; } };
        auto killPl   = [&](VkPipelineLayout& p){ if (p) { vkDestroyPipelineLayout(m_dev.device, p, nullptr); p = VK_NULL_HANDLE; } };
        auto killSl   = [&](VkDescriptorSetLayout& s){ if (s) { vkDestroyDescriptorSetLayout(m_dev.device, s, nullptr); s = VK_NULL_HANDLE; } };
        killPipe(m_iblEnvPipe); killPipe(m_iblIrradPipe); killPipe(m_iblPrefPipe); killPipe(m_iblBrdfPipe);
        killPl(m_iblEnvLayout); killPl(m_iblCubeLayout); killPl(m_iblBrdfLayout);
        if (m_iblMeshPool) { vkDestroyDescriptorPool(m_dev.device, m_iblMeshPool, nullptr); m_iblMeshPool = VK_NULL_HANDLE; }
        if (m_iblBakePool) { vkDestroyDescriptorPool(m_dev.device, m_iblBakePool, nullptr); m_iblBakePool = VK_NULL_HANDLE; }
        // m_iblMeshSetLayout is owned by createGraphics/destroyGraphics (it's baked
        // into the mesh pipeline layout), so it is NOT destroyed here.
        killSl(m_iblCubeSetLayout); killSl(m_iblSkyUboSetLayout);
        if (m_iblSkyUboBuf) { vmaDestroyBuffer(m_alloc, m_iblSkyUboBuf, m_iblSkyUboAlloc); m_iblSkyUboBuf = VK_NULL_HANDLE; m_iblSkyUboAlloc = nullptr; m_iblSkyUboMapped = nullptr; }
        if (m_iblCubeSampler) { vkDestroySampler(m_dev.device, m_iblCubeSampler, nullptr); m_iblCubeSampler = VK_NULL_HANDLE; }
        if (m_iblBrdfSampler) { vkDestroySampler(m_dev.device, m_iblBrdfSampler, nullptr); m_iblBrdfSampler = VK_NULL_HANDLE; }
        for (int f = 0; f < 6; ++f) { killView(m_iblEnvFaceView[f]); killView(m_iblIrradFaceView[f]); }
        for (uint32_t m = 0; m < kIblPrefilterMips; ++m) for (int f = 0; f < 6; ++f) killView(m_iblPrefFaceView[m][f]);
        killView(m_iblEnvCubeView); killView(m_iblIrradCubeView); killView(m_iblPrefCubeView); killView(m_iblBrdfView);
        killImg(m_iblEnvImg, m_iblEnvAlloc); killImg(m_iblIrradImg, m_iblIrradAlloc);
        killImg(m_iblPrefImg, m_iblPrefAlloc); killImg(m_iblBrdfImg, m_iblBrdfAlloc);
        killView(m_probeDepthView); killImg(m_probeDepthImg, m_probeDepthAlloc);  // reflection-probe depth
        m_iblReady = false; m_iblBaked = false;
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
        if (x3CreateDescriptorPool(&pci, nullptr, &m_skyPool) != VK_SUCCESS) {
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
            if (x3vmaCreateBuffer(&bci, &aci, &fr.skyBuf, &fr.skyAlloc, &info) != VK_SUCCESS) {
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
        VkResult pr = x3CreateGraphicsPipelines(1, &gpci, nullptr, &m_skyPipeline);

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
        if (x3CreateDescriptorPool(&pci, nullptr, &m_waterPool) != VK_SUCCESS) {
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
            if (x3vmaCreateBuffer(&bci, &aci, &m_waterUboBuf[i], &m_waterUboAlloc[i], &info) != VK_SUCCESS) {
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
        VkResult pr = x3CreateGraphicsPipelines(1, &gpci, nullptr, &m_waterPipeline);

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

    // ---- Translucent GLASS pass body (transparent meshes) ------------------
    // Recorded into the (already-open) post-opaque glass pass. Binds the glass
    // pipeline + the SAME descriptor sets the opaque mesh pass uses (bindless
    // textures, object SSBO + camera UBO, shadow map, SSAO) and REPLAYS the SAME
    // per-mesh indirect multidraw. glass.frag DISCARDs non-glass fragments and
    // mesh.frag DISCARDs glass fragments, so the two passes cleanly partition the
    // draw list — no separate glass index/SSBO buffer needed for M1. Only reached
    // when m_frameGlassCount > 0 (the pass isn't added otherwise).
    void recordGlassPassBody(VkCommandBuffer cmd) {
        // Need the glass pipeline AND its set-4 resources; without set 4 the bind
        // would be invalid, so the whole pass is skipped (M1 alpha still works on the
        // frames where set 4 exists — it always does if createGlassResources passed).
        if (!m_glassPipeline || !m_glassLayout || !m_glassSet[m_frameIdx] || m_frameCmdCount == 0) return;
        auto& fr = m_frames[m_frameIdx];
        postViewport(cmd, m_extent);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_glassPipeline);
        // The 4 shared mesh sets + the glass-only set 4 (scene-copy + GlassControl).
        VkDescriptorSet sets[5] = { m_bindlessSet, fr.objSet, m_shadowSet,
                                    m_meshAoSet[m_frameIdx], m_glassSet[m_frameIdx] };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_glassLayout,
                                0, 5, sets, 0, nullptr);
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
        if (x3CreateDescriptorPool(&pci, nullptr, &m_partPool) != VK_SUCCESS) {
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
                if (x3vmaCreateBuffer(&bci, &aci, &buf, &alloc, &info) != VK_SUCCESS) return false;
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
                return x3CreateGraphicsPipelines(1, &gpci, nullptr, &out) == VK_SUCCESS;
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
            VkResult pr = x3CreateGraphicsPipelines(1, &gpci, nullptr, &m_decalPipeline);
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
            if (x3vmaCreateBuffer(&bci, &aci, &m_debrisPoolBuf, &m_debrisPoolAlloc, &info) != VK_SUCCESS) {
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
            if (x3vmaCreateBuffer(&bci, &aci, &m_debrisCountBuf, &m_debrisCountAlloc, &info) != VK_SUCCESS) {
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
                if (x3vmaCreateBuffer(&bci, &aci, &buf, &alloc, &info) != VK_SUCCESS) return false;
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
        if (x3CreateDescriptorPool(&pci, nullptr, &m_debrisPool) != VK_SUCCESS) {
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
            VkResult cr = x3CreateComputePipelines(1, &cpci, nullptr, &m_debrisComputePipeline);
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
            VkResult pr = x3CreateGraphicsPipelines(1, &gpci, nullptr, &m_debrisDrawPipeline);
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
        if (x3CreateDescriptorPool(&pci, nullptr, &m_skinPool) != VK_SUCCESS) {
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
        VkResult cr = x3CreateComputePipelines(1, &cpci, nullptr, &m_skinPipeline);
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
            if (x3vmaCreateBuffer(&bci, &aci, &sm.palBuf[i], &sm.palAlloc[i], &info) != VK_SUCCESS) {
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
        if (x3vmaCreateBuffer(&bci, &aci, &rb, &rbA, &rbI) != VK_SUCCESS) {
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

    // ---- HDR scene + bloom render targets ----------------------------------
    // Create (or recreate) the linear HDR scene target + the bloom mip chain at
    // the current frame extent. Called after the swapchain/offscreen target exists
    // (so m_extent is valid) and on every resize. Images: COLOR_ATTACHMENT (render
    // target) | SAMPLED (read by the next post pass) | TRANSFER_DST is not needed.
    bool createBloomTargets() {
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

    // Create the single-mip scene-color copy image (refraction, M2) + the separate
    // downsampled frost-blur level images (M4). Returns false on any failure (caller
    // treats it as non-fatal: glass still draws, just without refraction/frost).
    bool createSceneCopyTarget(uint32_t W, uint32_t H) {
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

    void destroySceneCopyTarget() {
        if (m_sceneCopyView) { vkDestroyImageView(m_dev.device, m_sceneCopyView, nullptr); m_sceneCopyView = VK_NULL_HANDLE; }
        if (m_sceneCopyImg) { vmaDestroyImage(m_alloc, m_sceneCopyImg, m_sceneCopyAlloc); m_sceneCopyImg = VK_NULL_HANDLE; m_sceneCopyAlloc = nullptr; }
        for (uint32_t i = 0; i < kGlassFrostLevels; ++i) {
            if (m_glassFrostView[i]) { vkDestroyImageView(m_dev.device, m_glassFrostView[i], nullptr); m_glassFrostView[i] = VK_NULL_HANDLE; }
            if (m_glassFrostImg[i])  { vmaDestroyImage(m_alloc, m_glassFrostImg[i], m_glassFrostAlloc[i]); m_glassFrostImg[i] = VK_NULL_HANDLE; m_glassFrostAlloc[i] = nullptr; }
            m_glassFrostExt[i] = {};
        }
    }

    void destroyBloomTargets() {
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
        if (m_hdrView) { vkDestroyImageView(m_dev.device, m_hdrView, nullptr); m_hdrView = VK_NULL_HANDLE; }
        if (m_hdrImg)  { vmaDestroyImage(m_alloc, m_hdrImg, m_hdrAlloc); m_hdrImg = VK_NULL_HANDLE; m_hdrAlloc = nullptr; }
    }

    // ---- Glass resources (set 4 UBO + descriptor sets + scene-copy sampler) ----
    // Built once at init AFTER createBloomTargets (so the scene-copy view exists):
    // a mip-aware LINEAR sampler, per-frame GlassControl UBOs, a descriptor pool +
    // one glass set per frame. The set is (re)written by writeGlassDescriptors at
    // init + on every resize (the scene-copy view changes). The glass set layout
    // itself is built in createGraphics (needed by the glass pipeline layout).
    // Graceful: any failure leaves m_glassSet[*] null; recordGlassPassBody falls
    // back to binding nothing and the glass pass is skipped (opaque unaffected).
    bool createGlassResources() {
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

    // (Re)point the glass sets at the current scene-copy view + per-frame UBO.
    // Called at init + after every resize (the scene-copy view is recreated). If
    // the scene-copy target failed to create, the sampler points at the HDR view
    // as a harmless stand-in (glass simply won't refract — opacity path still reads).
    void writeGlassDescriptors() {
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

    void destroyGlassResources() {
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
        if (x3vmaCreateImage(&ici, &aci, &outImg, &outAlloc, nullptr) != VK_SUCCESS) return false;
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
        // (all fragment samplers) + b3 = the per-frame TAA UBO (matrices/params).
        VkDescriptorSetLayoutBinding tb[4]{};
        for (uint32_t i = 0; i < 3; ++i) {
            tb[i].binding = i; tb[i].descriptorCount = 1;
            tb[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            tb[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        tb[3].binding = 3; tb[3].descriptorCount = 1;
        tb[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        tb[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo st{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        st.bindingCount = 4; st.pBindings = tb;
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
        // per-frame TAA resolve sets (3 samplers + 1 UBO each). Sized exactly; no
        // UPDATE_AFTER_BIND needed.
        const uint32_t single = 1 + kBloomMips + 1;     // HDR + each mip + TAA out
        VkDescriptorPoolSize ps[3]{
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
              single + 2*2 + 1*2 + 3*kFramesInFlight },
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
        VkResult pr = x3CreateGraphicsPipelines(1, &gpci, nullptr, &outPipe);

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
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (!m_taaSet[i] || !m_taaUboBuf[i]) continue;
            VkDescriptorBufferInfo rb{ m_taaUboBuf[i], 0, VK_WHOLE_SIZE };
            VkWriteDescriptorSet rw[4]{};
            for (uint32_t b = 0; b < 3; ++b) {
                rw[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                rw[b].dstSet = m_taaSet[i]; rw[b].dstBinding = b; rw[b].descriptorCount = 1;
                rw[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            }
            rw[0].pImageInfo = &r0; rw[1].pImageInfo = &r1; rw[2].pImageInfo = &r2;
            rw[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            rw[3].dstSet = m_taaSet[i]; rw[3].dstBinding = 3; rw[3].descriptorCount = 1;
            rw[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; rw[3].pBufferInfo = &rb;
            vkUpdateDescriptorSets(m_dev.device, 4, rw, 0, nullptr);
        }
    }

    void destroyPost() {
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
            VkDescriptorPoolSize sizes[3]{};
            sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            sizes[0].descriptorCount = nFrames /*ssao depth*/ + nFrames * 4 /*mesh ao + refl + ddgi irr/vis*/ + 2 /*blur*/;
            sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            sizes[1].descriptorCount = nFrames /*ssao ubo*/ + nFrames /*ctrl ubo*/;
            // RT devices: mesh set3 carries the TLAS at binding 5 (r_rtshadows).
            sizes[2].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            sizes[2].descriptorCount = nFrames;
            VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.maxSets = nFrames + nFrames + 1;
            pci.poolSizeCount = m_rtSupported ? 3u : 2u; pci.pPoolSizes = sizes;
            if (x3CreateDescriptorPool(&pci, nullptr, &m_ssaoPool) != VK_SUCCESS) {
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
            if (x3vmaCreateBuffer(&ub, &aci, &m_ssaoUboBuf[i], &m_ssaoUboAlloc[i], &info) != VK_SUCCESS) {
                logError("[rhi] ssao ubo create failed"); return false;
            }
            m_ssaoUboMapped[i] = info.pMappedData;

            VkBufferCreateInfo cb{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            cb.size = sizeof(SsaoControl); cb.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationInfo cinfo{};
            if (x3vmaCreateBuffer(&cb, &aci, &m_ssaoCtrlBuf[i], &m_ssaoCtrlAlloc[i], &cinfo) != VK_SUCCESS) {
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
        VkResult pr = x3CreateGraphicsPipelines(1, &gpci, nullptr, &m_depthPrePipeline);
        vkDestroyShaderModule(m_dev.device, vs, nullptr);
        if (pr != VK_SUCCESS) { logError("[rhi] depth pre-pass pipeline create failed"); return false; }

        // ---- ALPHA-CUTOUT variant (depth_cutout.vert/.frag) -----------------
        // Identical fixed-function state; adds a fragment stage that replicates
        // mesh.frag's alphaMode==MASK discard so billboard depth matches the color
        // pass texel-for-texel. Used per-draw for cutout groups on reflections
        // frames (recordDepthPrePassBody). Set 0 = objSet (same bindings as
        // depth.vert -> layout-compatible with m_shadowLayout), set 1 = bindless.
        // NON-FATAL on failure: the plain full-quad pre-pass still works.
        {
            VkDescriptorSetLayout cutSets[2] = { m_objSetLayout, m_bindlessLayout };
            VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            plci.setLayoutCount = 2; plci.pSetLayouts = cutSets;
            if (vkCreatePipelineLayout(m_dev.device, &plci, nullptr, &m_depthPreCutoutLayout) == VK_SUCCESS) {
                VkShaderModule cvs = loadShaderModule("shaders\\depth_cutout.vert.spv");
                VkShaderModule cfs = loadShaderModule("shaders\\depth_cutout.frag.spv");
                if (cvs && cfs) {
                    VkPipelineShaderStageCreateInfo cstages[2]{};
                    cstages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                    cstages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   cstages[0].module = cvs; cstages[0].pName = "main";
                    cstages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                    cstages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; cstages[1].module = cfs; cstages[1].pName = "main";
                    gpci.stageCount = 2; gpci.pStages = cstages;
                    gpci.layout = m_depthPreCutoutLayout;
                    if (x3CreateGraphicsPipelines(1, &gpci, nullptr, &m_depthPreCutoutPipeline) != VK_SUCCESS)
                        m_depthPreCutoutPipeline = VK_NULL_HANDLE;
                }
                if (cvs) vkDestroyShaderModule(m_dev.device, cvs, nullptr);
                if (cfs) vkDestroyShaderModule(m_dev.device, cfs, nullptr);
            }
            if (!m_depthPreCutoutPipeline)
                logError("[rhi] depth pre-pass CUTOUT pipeline unavailable — billboards keep full-quad depth");
        }
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
        // mesh.frag set3: binding0 = blurred AO (linear), binding1 = per-frame ctrl,
        // binding2 = the SSR/RT reflection buffer (refl.comp output). Before the
        // reflection chain is built, binding2 points at the blurred-AO view as a
        // LAYOUT-VALID placeholder (mesh.frag never samples it then — the
        // ssao.refl.x gate is 0 — but the descriptor must reference a real view).
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkDescriptorImageInfo da{ m_ssaoLinearSampler, m_ssaoBlurView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo dr{ m_ssaoLinearSampler,
                                      m_reflView ? m_reflView : m_ssaoBlurView,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            // DDGI atlases (bindings 3/4). Until/unless the DDGI chain is built,
            // the blurred-AO view is a LAYOUT-VALID placeholder (never sampled —
            // the ssao.ddgiCtrl.x gate is 0 — but descriptors must be real).
            VkDescriptorImageInfo dgi{ m_ssaoLinearSampler,
                                       m_ddgiIrrView ? m_ddgiIrrView : m_ssaoBlurView,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo dgv{ m_ssaoLinearSampler,
                                       m_ddgiVisView ? m_ddgiVisView : m_ssaoBlurView,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorBufferInfo bi{ m_ssaoCtrlBuf[i], 0, sizeof(SsaoControl) };
            VkWriteDescriptorSet w[5]{};
            w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[0].dstSet = m_meshAoSet[i]; w[0].dstBinding = 0; w[0].descriptorCount = 1;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &da;
            w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[1].dstSet = m_meshAoSet[i]; w[1].dstBinding = 1; w[1].descriptorCount = 1;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[1].pBufferInfo = &bi;
            w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[2].dstSet = m_meshAoSet[i]; w[2].dstBinding = 2; w[2].descriptorCount = 1;
            w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[2].pImageInfo = &dr;
            w[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[3].dstSet = m_meshAoSet[i]; w[3].dstBinding = 3; w[3].descriptorCount = 1;
            w[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[3].pImageInfo = &dgi;
            w[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[4].dstSet = m_meshAoSet[i]; w[4].dstBinding = 4; w[4].descriptorCount = 1;
            w[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[4].pImageInfo = &dgv;
            vkUpdateDescriptorSets(m_dev.device, 5, w, 0, nullptr);
        }
        // RT soft shadows (r_rtshadows): re-point set3 binding5 at the TLAS when
        // one exists (resize path — the sets were just rewritten above; keep the
        // AS binding live so the next RT-shadow frame doesn't trace a stale
        // descriptor). Before the first TLAS build there is nothing to write —
        // the plain pipelines never reference binding 5.
        writeMeshTlasDescriptor();
    }

    // Write the scene TLAS into mesh set3 binding5 for ALL frames in flight
    // (the r_rtshadows ray origin). Callers must guarantee the sets are not in
    // use by a pending command buffer (first TLAS build + handle-grow rebuilds
    // idle the device; init/resize paths are idle by construction). No-op
    // without RT support / before the first TLAS exists.
    void writeMeshTlasDescriptor() {
        if (!m_rtSupported) return;
        VkAccelerationStructureKHR tlas = m_rt.tlas();
        if (!tlas || !m_meshAoSet[0]) return;
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkWriteDescriptorSetAccelerationStructureKHR asW{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
            asW.accelerationStructureCount = 1; asW.pAccelerationStructures = &tlas;
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.pNext = &asW; w.dstSet = m_meshAoSet[i]; w.dstBinding = 5;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            vkUpdateDescriptorSets(m_dev.device, 1, &w, 0, nullptr);
        }
        m_meshTlasWritten = true;
    }

    void destroySsao() {
        destroySsaoTargets();
        if (m_ssaoBlurPipe)   { vkDestroyPipeline(m_dev.device, m_ssaoBlurPipe, nullptr); m_ssaoBlurPipe = VK_NULL_HANDLE; }
        if (m_ssaoPipe)       { vkDestroyPipeline(m_dev.device, m_ssaoPipe, nullptr); m_ssaoPipe = VK_NULL_HANDLE; }
        if (m_depthPrePipeline){ vkDestroyPipeline(m_dev.device, m_depthPrePipeline, nullptr); m_depthPrePipeline = VK_NULL_HANDLE; }
        if (m_depthPreCutoutPipeline){ vkDestroyPipeline(m_dev.device, m_depthPreCutoutPipeline, nullptr); m_depthPreCutoutPipeline = VK_NULL_HANDLE; }
        if (m_depthPreCutoutLayout)  { vkDestroyPipelineLayout(m_dev.device, m_depthPreCutoutLayout, nullptr); m_depthPreCutoutLayout = VK_NULL_HANDLE; }
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

    // ======================================================================
    // RT AMBIENT OCCLUSION (hardware ray query) — lazy init / targets /
    // descriptors / per-frame TLAS build / compute dispatch / teardown.
    // ----------------------------------------------------------------------
    // Built LAZILY the first time RT AO is enabled on an RT-capable device (so a
    // run that never turns r_rtao on pays zero init cost). All teardown is in
    // destroyRt(), called from shutdown(). The compute pass writes a half-res R8 AO
    // image (GENERAL layout) via rayQueryEXT; a full-screen MULTIPLY apply pass then
    // darkens the linear HDR scene by the up-sampled AO before bloom.

    // Static trampolines so the C-style VulkanRT logger pointers reach x3::log*.
    static void rtLogInfo(const char* m)  { x3::logInfo(m ? m : ""); }
    static void rtLogError(const char* m) { x3::logError(m ? m : ""); }

    // Lazily init the shared AS module (BLAS/TLAS manager) — used by BOTH the
    // RT-AO chain and the RT-reflections fallback. Returns true when the module
    // is ready. No-op (returns false) without RT support.
    bool ensureRtCore() {
        if (!m_rtSupported) return false;
        if (!m_rtInitTried) {
            m_rtInitTried = true;
            if (!m_rt.init(m_dev.device, m_dev.physical_device, m_alloc, m_gfxQueue,
                           m_gfxFamily, &rtLogInfo, &rtLogError)) {
                logError("[rhi] RT: AS module init failed — staying on raster/SSAO");
                m_rtSupported = false;   // disable RT entirely; never retry
                return false;
            }
            // Position-fetch tier: BLAS builds carry ALLOW_DATA_ACCESS so DDGI's
            // ray-query shader may read committed-triangle vertex positions.
            m_rt.setAllowDataAccess(m_rtPosFetch);
        }
        return m_rt.ready();
    }

    // Lazily init the AS module + RT-AO pipelines/targets. Returns true when the RT
    // chain is ready to use this frame. No-op (returns false) without RT support.
    bool ensureRtaoReady() {
        if (!ensureRtCore()) return false;
        if (!m_rtaoBuilt) {
            if (!createRtao()) { logError("[rhi] RT AO: pipeline create failed"); m_rtSupported = false; return false; }
            if (!createRtaoTargets()) { logError("[rhi] RT AO: target create failed"); m_rtSupported = false; return false; }
            writeRtaoDescriptors();
            m_rtaoBuilt = true;
            logInfo("[rhi] RT AO ready (ray-query inline AO: BLAS/TLAS + half-res compute + multiply apply)");
        }
        return true;
    }

    bool createRtao() {
        // NEAREST sampler for reading depth as plain data; LINEAR for AO up-sample.
        VkSamplerCreateInfo n{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        n.magFilter = VK_FILTER_NEAREST; n.minFilter = VK_FILTER_NEAREST;
        n.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        n.addressModeU = n.addressModeV = n.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        n.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        if (vkCreateSampler(m_dev.device, &n, nullptr, &m_rtaoDepthSampler) != VK_SUCCESS) return false;
        VkSamplerCreateInfo l = n; l.magFilter = VK_FILTER_LINEAR; l.minFilter = VK_FILTER_LINEAR;
        if (vkCreateSampler(m_dev.device, &l, nullptr, &m_rtaoLinearSampler) != VK_SUCCESS) return false;

        // ---- Compute set layout: 0=depth sampler, 1=AO storage image, 2=TLAS,
        //      3=Rtao UBO. ----
        {
            VkDescriptorSetLayoutBinding b[4]{};
            b[0].binding = 0; b[0].descriptorCount = 1;
            b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[1].binding = 1; b[1].descriptorCount = 1;
            b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[2].binding = 2; b[2].descriptorCount = 1;
            b[2].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            b[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[3].binding = 3; b[3].descriptorCount = 1;
            b[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            b[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = 4; ci.pBindings = b;
            if (vkCreateDescriptorSetLayout(m_dev.device, &ci, nullptr, &m_rtaoSetLayout) != VK_SUCCESS) return false;
        }
        // ---- Apply set layout: 0=AO (linear), 1=depth (nearest). ----
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
            if (vkCreateDescriptorSetLayout(m_dev.device, &ci, nullptr, &m_rtaoApplySetLayout) != VK_SUCCESS) return false;
        }
        // ---- Descriptor pool: per-frame compute + apply sets. ----
        {
            const uint32_t nF = kFramesInFlight;
            VkDescriptorPoolSize sizes[4]{};
            sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sizes[0].descriptorCount = nF /*compute depth*/ + nF * 2 /*apply*/;
            sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          sizes[1].descriptorCount = nF;
            sizes[2].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; sizes[2].descriptorCount = nF;
            sizes[3].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;         sizes[3].descriptorCount = nF;
            VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.maxSets = nF * 2; pci.poolSizeCount = 4; pci.pPoolSizes = sizes;
            if (x3CreateDescriptorPool(&pci, nullptr, &m_rtaoPool) != VK_SUCCESS) return false;
        }
        // ---- Per-frame UBOs + sets. ----
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkBufferCreateInfo ub{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            ub.size = sizeof(RtaoUBO); ub.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info{};
            if (x3vmaCreateBuffer(&ub, &aci, &m_rtaoUboBuf[i], &m_rtaoUboAlloc[i], &info) != VK_SUCCESS) return false;
            m_rtaoUboMapped[i] = info.pMappedData;
            VkDescriptorSetAllocateInfo a0{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            a0.descriptorPool = m_rtaoPool; a0.descriptorSetCount = 1; a0.pSetLayouts = &m_rtaoSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &a0, &m_rtaoSet[i]) != VK_SUCCESS) return false;
            VkDescriptorSetAllocateInfo a1{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            a1.descriptorPool = m_rtaoPool; a1.descriptorSetCount = 1; a1.pSetLayouts = &m_rtaoApplySetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &a1, &m_rtaoApplySet[i]) != VK_SUCCESS) return false;
        }
        // ---- Compute pipeline (rtao.comp). ----
        {
            VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pl.setLayoutCount = 1; pl.pSetLayouts = &m_rtaoSetLayout;
            if (vkCreatePipelineLayout(m_dev.device, &pl, nullptr, &m_rtaoLayout) != VK_SUCCESS) return false;
            VkShaderModule cs = loadShaderModule("shaders\\rtao.comp.spv");
            if (!cs) return false;
            VkComputePipelineCreateInfo cpci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpci.stage.module = cs; cpci.stage.pName = "main";
            cpci.layout = m_rtaoLayout;
            VkResult pr = x3CreateComputePipelines(1, &cpci, nullptr, &m_rtaoPipe);
            vkDestroyShaderModule(m_dev.device, cs, nullptr);
            if (pr != VK_SUCCESS) return false;
        }
        // ---- Apply pipeline (rtao_apply.frag -> HDR, MULTIPLY blend). ----
        {
            VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(RtaoApplyPush) };
            VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pl.setLayoutCount = 1; pl.pSetLayouts = &m_rtaoApplySetLayout;
            pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr;
            if (vkCreatePipelineLayout(m_dev.device, &pl, nullptr, &m_rtaoApplyLayout) != VK_SUCCESS) return false;
            if (!createMultiplyFullscreenPipeline("shaders\\fullscreen.vert.spv", "shaders\\rtao_apply.frag.spv",
                                                  m_rtaoApplyLayout, kHdrFormat, m_rtaoApplyPipe))
                return false;
        }
        return true;
    }

    // Full-screen-triangle pipeline whose single color attachment uses a MULTIPLY
    // blend (dstColor * srcColor): the apply pass outputs the AO darkening factor
    // and the blender multiplies it into the existing HDR target (no read-back).
    bool createMultiplyFullscreenPipeline(const char* vsPath, const char* fsPath,
                                          VkPipelineLayout layout, VkFormat colorFmt,
                                          VkPipeline& outPipe) {
        VkShaderModule vs = loadShaderModule(vsPath);
        VkShaderModule fs = loadShaderModule(fsPath);
        if (!vs || !fs) { if (vs) vkDestroyShaderModule(m_dev.device, vs, nullptr); if (fs) vkDestroyShaderModule(m_dev.device, fs, nullptr); return false; }
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
        dss.depthTestEnable = VK_FALSE; dss.depthWriteEnable = VK_FALSE; dss.depthCompareOp = VK_COMPARE_OP_ALWAYS;
        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR; cba.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 1; cb.pAttachments = &cba;
        VkDynamicState dyn[2]{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfo prci{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        prci.colorAttachmentCount = 1; prci.pColorAttachmentFormats = &colorFmt;
        prci.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
        VkGraphicsPipelineCreateInfo gpci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gpci.pNext = &prci; gpci.stageCount = 2; gpci.pStages = stages;
        gpci.pVertexInputState = &vin; gpci.pInputAssemblyState = &ia;
        gpci.pViewportState = &vp; gpci.pRasterizationState = &rs;
        gpci.pMultisampleState = &ms; gpci.pDepthStencilState = &dss;
        gpci.pColorBlendState = &cb; gpci.pDynamicState = &ds; gpci.layout = layout;
        VkResult pr = x3CreateGraphicsPipelines(1, &gpci, nullptr, &outPipe);
        vkDestroyShaderModule(m_dev.device, vs, nullptr);
        vkDestroyShaderModule(m_dev.device, fs, nullptr);
        return pr == VK_SUCCESS;
    }

    // Create (or recreate) the half-res RT-AO R8 storage target at the current
    // extent. STORAGE (compute write) | SAMPLED (apply read).
    bool createRtaoTargets() {
        destroyRtaoTargets();
        m_rtaoExtent = { std::max(1u, m_extent.width / 2), std::max(1u, m_extent.height / 2) };
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (!createColorTarget(kRtaoFormat, m_rtaoExtent.width, m_rtaoExtent.height, usage,
                               m_rtaoImg, m_rtaoAlloc, m_rtaoView)) return false;
        return true;
    }

    void destroyRtaoTargets() {
        if (m_rtaoView) { vkDestroyImageView(m_dev.device, m_rtaoView, nullptr); m_rtaoView = VK_NULL_HANDLE; }
        if (m_rtaoImg)  { vmaDestroyImage(m_alloc, m_rtaoImg, m_rtaoAlloc); m_rtaoImg = VK_NULL_HANDLE; m_rtaoAlloc = nullptr; }
    }

    // (Re)write the RT-AO descriptor sets (depth/AO image views + TLAS + UBO).
    // The TLAS write is refreshed each time the TLAS handle changes; called once
    // at build + whenever targets/TLAS are recreated.
    void writeRtaoDescriptors() {
        VkAccelerationStructureKHR tlas = m_rt.tlas();
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkDescriptorImageInfo depthInfo{ m_rtaoDepthSampler, m_depthView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo aoInfo{ VK_NULL_HANDLE, m_rtaoView, VK_IMAGE_LAYOUT_GENERAL };
            VkDescriptorBufferInfo ubo{ m_rtaoUboBuf[i], 0, sizeof(RtaoUBO) };
            VkWriteDescriptorSetAccelerationStructureKHR asW{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
            asW.accelerationStructureCount = 1; asW.pAccelerationStructures = &tlas;
            VkWriteDescriptorSet w[4]{};
            w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = m_rtaoSet[i]; w[0].dstBinding = 0; w[0].descriptorCount = 1;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &depthInfo;
            w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = m_rtaoSet[i]; w[1].dstBinding = 1; w[1].descriptorCount = 1;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[1].pImageInfo = &aoInfo;
            w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[2].dstSet = m_rtaoSet[i]; w[2].dstBinding = 3; w[2].descriptorCount = 1;
            w[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[2].pBufferInfo = &ubo;
            // Always write the depth/AO/UBO bindings; add the TLAS binding only when
            // a TLAS exists (it's built later this frame, so rewriteRtaoTlas() fills
            // binding 2 once available — the set is never used before then).
            w[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[3].dstSet = m_rtaoSet[i]; w[3].dstBinding = 2; w[3].descriptorCount = 1;
            w[3].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; w[3].pNext = &asW;
            vkUpdateDescriptorSets(m_dev.device, tlas ? 4u : 3u, w, 0, nullptr);

            VkDescriptorImageInfo aoSampled{ m_rtaoLinearSampler, m_rtaoView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo depthSampled{ m_rtaoDepthSampler, m_depthView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet a[2]{};
            a[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; a[0].dstSet = m_rtaoApplySet[i]; a[0].dstBinding = 0; a[0].descriptorCount = 1;
            a[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; a[0].pImageInfo = &aoSampled;
            a[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; a[1].dstSet = m_rtaoApplySet[i]; a[1].dstBinding = 1; a[1].descriptorCount = 1;
            a[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; a[1].pImageInfo = &depthSampled;
            vkUpdateDescriptorSets(m_dev.device, 2, a, 0, nullptr);
        }
    }

    // Re-write ONLY the TLAS binding (binding 2) into each compute set. Called after
    // a TLAS rebuild when the TLAS handle changed (a grow); steady same-size
    // rebuilds keep the same handle so this is skipped.
    void rewriteRtaoTlas() {
        VkAccelerationStructureKHR tlas = m_rt.tlas();
        if (!tlas) return;
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkWriteDescriptorSetAccelerationStructureKHR asW{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
            asW.accelerationStructureCount = 1; asW.pAccelerationStructures = &tlas;
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; w.pNext = &asW;
            // RT-AO compute set (binding 2) — may not exist when only the
            // reflections fallback is using the AS (r_rtao 0).
            if (m_rtaoSet[i]) {
                w.dstSet = m_rtaoSet[i]; w.dstBinding = 2;
                vkUpdateDescriptorSets(m_dev.device, 1, &w, 0, nullptr);
            }
            // RT-reflections compute set (binding 4) — may not exist when only
            // RT AO is using the AS (r_ssr 0 / chain never built).
            if (m_reflSetRt[i]) {
                w.dstSet = m_reflSetRt[i]; w.dstBinding = 4;
                vkUpdateDescriptorSets(m_dev.device, 1, &w, 0, nullptr);
            }
            // DDGI ray-pass set (binding 0) — may not exist when DDGI was never
            // enabled (r_ddgi 0 / chain never built).
            if (m_ddgiRaySet[i]) {
                w.dstSet = m_ddgiRaySet[i]; w.dstBinding = 0;
                vkUpdateDescriptorSets(m_dev.device, 1, &w, 0, nullptr);
            }
        }
    }

    // Build (or refit) the scene acceleration structures for THIS frame from the
    // per-frame draw list (m_drawRecords, still valid in endFrame after
    // prepareFrameData). Ensures a BLAS exists for every distinct mesh, then
    // rebuilds the TLAS with one instance per draw. Returns true if a usable TLAS
    // is ready. Called from endFrame BEFORE the graph records the frame.
    bool buildRtSceneAS() {
        // Ensure a BLAS for each distinct mesh referenced this frame — BATCHED
        // (one submit for the whole set; ~8000 per-mesh one-shot submits used to
        // cost ~6.6 s on the legacy tower's first frame, docs/BOOT_TIME.md) and
        // BUDGETED (at most kBlasFrameBudget new BLAS per frame). If the budget
        // runs out, RT stays on the raster fallback (no TLAS) for a frame or two
        // more while the remaining BLAS build — a graceful warm-up, not a hitch.
        constexpr uint32_t kBlasFrameBudget = 4096;
        uint32_t built = 0;
        bool     deferred = false;
        m_rt.beginBlasBatch();
        for (uint32_t mid : m_groupOrder) {
            if (m_rt.hasBlas(mid)) continue;
            auto it = m_meshes.find(mid);
            if (it == m_meshes.end()) continue;
            const Mesh& m = it->second;
            // Dynamic (CPU-skinned) meshes change their vertex buffer each frame; the
            // per-frame skinned/updated VBO would need a per-frame BLAS rebuild. For
            // v1 (static-first) we BLAS only the static device-local meshes; dynamic
            // characters are simply absent from the TLAS (they don't cast RT AO yet —
            // a documented next tier). This keeps the build cheap + correct.
            if (m.dynamic || m.vbo == VK_NULL_HANDLE) continue;
            if (built >= kBlasFrameBudget) { deferred = true; break; }
            VkBufferDeviceAddressInfo vi{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO }; vi.buffer = m.vbo;
            VkBufferDeviceAddressInfo ii{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO }; ii.buffer = m.ibo;
            const VkDeviceAddress vbAddr = vkGetBufferDeviceAddress(m_dev.device, &vi);
            const VkDeviceAddress ibAddr = vkGetBufferDeviceAddress(m_dev.device, &ii);
            ++m_asBuildsThisFrame;   // ZERO-STUTTER spike-log attribution (new BLAS)
            if (m_rt.ensureBlas(mid, vbAddr, m.vertexCount, (uint32_t)sizeof(MeshVertex), ibAddr, m.indexCount))
                ++built;
        }
        m_rt.endBlasBatch();
        if (deferred) {
            // More BLAS than this frame's budget: raster fallback until complete.
            char db[128];
            std::snprintf(db, sizeof(db),
                "[rt] BLAS warm-up: %u built this frame (budget %u) — raster fallback until complete",
                built, kBlasFrameBudget);
            logInfo(db);
            return false;
        }

        // Build the TLAS from this frame's instances (skip dynamic meshes — no BLAS).
        m_rtInstScratch.clear();
        m_rtInstScratch.reserve(m_drawRecords.size());
        for (uint32_t i = 0; i < (uint32_t)m_drawRecords.size(); ++i) {
            const DrawRecord& dr = m_drawRecords[i];
            if (!m_rt.hasBlas(dr.meshId)) continue;
            VulkanRT::TlasInstance inst{};
            inst.meshId = dr.meshId;
            // instanceCustomIndex = the record's SSBO row this frame (filled by
            // prepareFrameData's grouped write) — the DDGI ray shader's material
            // lookup. Rows are stable while the draw list is stable; any shift
            // changes the signature below and triggers a rebuild.
            inst.customIndex = (i < m_recordSsboRow.size()) ? m_recordSsboRow[i] : 0u;
            std::memcpy(inst.model, dr.model, sizeof(inst.model));
            m_rtInstScratch.push_back(inst);
        }

        // Decide whether to (re)build the TLAS this frame. Rebuilding into the SAME
        // backing buffer while a previous frame's RT-AO compute may still be READING
        // it is a cross-frame WAR hazard. To stay correct + cheap:
        //   * STATIC-FIRST (default, rebuildTlasEachFrame=false): build only when the
        //     instance set CHANGES (a cheap signature over mesh ids + transforms). A
        //     change is rare, so we idle the device before that rare rebuild — no
        //     in-flight reader can touch the backing. The common static frame does NO
        //     build (and thus no hazard, no stall).
        //   * rebuildTlasEachFrame=true (moving geometry): rebuild every frame, idling
        //     first so the prior frame's compute has retired before the backing is
        //     overwritten. Correct (if heavier) — for dynamic scenes.
        const uint64_t sig = tlasSignature(m_rtInstScratch);
        const bool firstBuild = !m_rt.tlasBuilt();
        const bool changed    = (sig != m_rtTlasSig);
        if (!firstBuild && !changed && !m_rtao.rebuildTlasEachFrame)
            return m_rt.tlas() != VK_NULL_HANDLE;   // unchanged static TLAS: reuse as-is

        // A real (re)build follows: ensure no in-flight GPU work is still reading the
        // TLAS backing we may overwrite. Cheap because rebuilds are rare (static) —
        // and necessary for correctness when they do happen.
        // vis-unify instrumentation: this path is the documented scene-mutation hitch
        // (synchronous vkDeviceWaitIdle; async double-buffer is deferred work).
        const auto tlasT0 = std::chrono::steady_clock::now();
        ++m_tlasBuilds;
        if (!firstBuild) { vkDeviceWaitIdle(m_dev.device); ++m_tlasSyncWaits; }

        ++m_asBuildsThisFrame;   // ZERO-STUTTER spike-log attribution (TLAS (re)build)
        const VkAccelerationStructureKHR before = m_rt.tlas();
        if (!m_rt.buildTlas(m_rtInstScratch)) return false;
        m_rtTlasSig = sig;
        // If the TLAS handle changed (first build or a grow), re-point the descriptors.
        const bool grew = (m_rt.tlas() != before);
        if (grew) {
            ++m_tlasGrows;
            // Mesh set3 (r_rtshadows TLAS at binding 5) is ALWAYS BOUND, so its
            // sets may be referenced by pending command buffers — idle before
            // rewriting them on the FIRST build (rebuilds already idled above;
            // the refl chain's first build sets the same precedent).
            if (firstBuild) { vkDeviceWaitIdle(m_dev.device); ++m_tlasSyncWaits; }
            rewriteRtaoTlas();
            writeMeshTlasDescriptor();
        }
        m_tlasCpuMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - tlasT0).count();
        if (!grew && m_tlasCpuMs > m_tlasCpuMsMax) m_tlasCpuMsMax = m_tlasCpuMs;
        return m_rt.tlasBuilt() && m_rt.tlas() != VK_NULL_HANDLE;
    }

    // Cheap order-sensitive signature of the TLAS instance set (mesh ids + packed
    // transforms) so a static scene's TLAS is rebuilt only when it actually changes.
    static uint64_t tlasSignature(const std::vector<VulkanRT::TlasInstance>& inst) {
        uint64_t h = 1469598103934665603ull;        // FNV-1a 64
        auto mix = [&](uint64_t v){ h ^= v; h *= 1099511628211ull; };
        mix(inst.size());
        for (const auto& in : inst) {
            mix(in.meshId);
            mix(in.customIndex);   // SSBO-row shifts must trigger a TLAS rebuild
            for (int k = 0; k < 16; ++k) {
                uint32_t bits; std::memcpy(&bits, &in.model[k], sizeof(bits));
                mix(bits);
            }
        }
        return h;
    }

    // =====================================================================
    // RT ACOUSTICS — ASYNC batched ray queries against the SAME scene TLAS
    // the RT screen effects use. Submit records + queues ONE small compute
    // dispatch (audio_rays.comp) with a fence and returns immediately;
    // harvest polls the fence next update and copies the hit distances out.
    // Async on purpose: a synchronous fence wait on the graphics queue would
    // stall the game thread behind the in-flight frame's GPU work (tens of
    // ms on heavy scenes); submit+harvest costs ~microseconds and audio
    // tolerates one update of latency. The first submit ARMS the per-frame
    // TLAS build (m_audioRaysWantFrames) and returns false until it exists.
    // =====================================================================
    bool traceAudioRaysSubmit(const AudioRay* rays, int count) override {
        if (!m_rtSupported || !rays || count <= 0 ||
            (uint32_t)count > kAudioRayCapacity)
            return false;
        m_audioRaysWantFrames = 300;   // keep the TLAS alive ~5s past the last ask
        if (m_audioRayInFlight) return false;   // previous batch not harvested yet
        if (!ensureRtCore() || !m_rt.tlasBuilt() || m_rt.tlas() == VK_NULL_HANDLE)
            return false;              // TLAS comes up next endFrame — no data yet
        if (!ensureAudioRays()) return false;

        // (Re)point the TLAS descriptor when the handle changed (first build or
        // a grow recreated it). Safe: no batch is in flight (checked above), so
        // the set is never updated while bound to executing work.
        if (m_audioRayTlasBound != m_rt.tlas()) {
            VkAccelerationStructureKHR tlas = m_rt.tlas();
            VkWriteDescriptorSetAccelerationStructureKHR asW{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
            asW.accelerationStructureCount = 1; asW.pAccelerationStructures = &tlas;
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.pNext = &asW; w.dstSet = m_audioRaySet; w.dstBinding = 0;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            vkUpdateDescriptorSets(m_dev.device, 1, &w, 0, nullptr);
            m_audioRayTlasBound = tlas;
        }

        // Upload the ray batch (host-visible, mapped; flush for non-coherent).
        std::memcpy(m_audioRayInMapped, rays, (size_t)count * sizeof(AudioRay));
        vmaFlushAllocation(m_alloc, m_audioRayInAlloc, 0, (VkDeviceSize)count * sizeof(AudioRay));

        // Record + submit (NO wait — the fence is polled by harvest).
        vkResetCommandBuffer(m_audioRayCmd, 0);
        VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(m_audioRayCmd, &bi);
        vkCmdBindPipeline(m_audioRayCmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_audioRayPipe);
        vkCmdBindDescriptorSets(m_audioRayCmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                m_audioRayLayout, 0, 1, &m_audioRaySet, 0, nullptr);
        const uint32_t n = (uint32_t)count;
        vkCmdPushConstants(m_audioRayCmd, m_audioRayLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(uint32_t), &n);
        vkCmdDispatch(m_audioRayCmd, (n + 63u) / 64u, 1, 1);
        // Compute write -> host read of the hit buffer.
        VkMemoryBarrier2 mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        mb.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        mb.dstStageMask  = VK_PIPELINE_STAGE_2_HOST_BIT;
        mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.memoryBarrierCount = 1; dep.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(m_audioRayCmd, &dep);
        vkEndCommandBuffer(m_audioRayCmd);

        VkCommandBufferSubmitInfo cs{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
        cs.commandBuffer = m_audioRayCmd;
        VkSubmitInfo2 submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        submit.commandBufferInfoCount = 1; submit.pCommandBufferInfos = &cs;
        if (vkQueueSubmit2(m_gfxQueue, 1, &submit, m_audioRayFence) != VK_SUCCESS)
            return false;
        m_audioRayInFlight = true;
        m_audioRayInFlightCount = count;
        return true;
    }

    int traceAudioRaysHarvest(float* outHitT, int capacity) override {
        if (!m_audioRayInFlight) return -1;
        const VkResult fs = vkGetFenceStatus(m_dev.device, m_audioRayFence);
        if (fs == VK_NOT_READY) return 0;     // still on the GPU — poll again
        vkResetFences(m_dev.device, 1, &m_audioRayFence);
        m_audioRayInFlight = false;
        const int n = m_audioRayInFlightCount;
        m_audioRayInFlightCount = 0;
        if (fs != VK_SUCCESS || !outHitT || capacity < n) return -1;  // batch dropped
        vmaInvalidateAllocation(m_alloc, m_audioRayOutAlloc, 0, (VkDeviceSize)n * sizeof(float));
        std::memcpy(outHitT, m_audioRayOutMapped, (size_t)n * sizeof(float));
        return n;
    }

    // Lazily build the audio-ray batch chain: pipeline (audio_rays.comp) +
    // descriptor set {0 = TLAS, 1 = ray SSBO in, 2 = hit SSBO out} + the two
    // persistent host-visible buffers + a transient command buffer + fence.
    bool ensureAudioRays() {
        if (m_audioRayBuilt)  return true;
        if (m_audioRayFailed) return false;   // failed once — don't retry/spam
        auto fail = [&](const char* what) {
            logError(std::string("[rta] audio-ray chain create failed: ") + what);
            m_audioRayFailed = true;
            destroyAudioRays();
            return false;
        };
        // Set layout.
        {
            VkDescriptorSetLayoutBinding b[3]{};
            b[0].binding = 0; b[0].descriptorCount = 1;
            b[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[1].binding = 1; b[1].descriptorCount = 1;
            b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[2].binding = 2; b[2].descriptorCount = 1;
            b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = 3; ci.pBindings = b;
            if (vkCreateDescriptorSetLayout(m_dev.device, &ci, nullptr, &m_audioRaySetLayout) != VK_SUCCESS)
                return fail("set layout");
        }
        // Pipeline (push constant = ray count).
        {
            VkPushConstantRange pcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t) };
            VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pl.setLayoutCount = 1; pl.pSetLayouts = &m_audioRaySetLayout;
            pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr;
            if (vkCreatePipelineLayout(m_dev.device, &pl, nullptr, &m_audioRayLayout) != VK_SUCCESS)
                return fail("pipeline layout");
            VkShaderModule cs = loadShaderModule("shaders\\audio_rays.comp.spv");
            if (!cs) return fail("audio_rays.comp.spv load");
            VkComputePipelineCreateInfo cpci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            cpci.stage.module = cs; cpci.stage.pName = "main";
            cpci.layout = m_audioRayLayout;
            VkResult pr = vkCreateComputePipelines(m_dev.device, VK_NULL_HANDLE, 1, &cpci, nullptr, &m_audioRayPipe);
            vkDestroyShaderModule(m_dev.device, cs, nullptr);
            if (pr != VK_SUCCESS) return fail("compute pipeline");
        }
        // Buffers: ray batch in (sequential write) + hit distances out (random read).
        {
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = (VkDeviceSize)kAudioRayCapacity * sizeof(AudioRay);
            bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info{};
            if (vmaCreateBuffer(m_alloc, &bci, &aci, &m_audioRayInBuf, &m_audioRayInAlloc, &info) != VK_SUCCESS)
                return fail("ray-in buffer");
            m_audioRayInMapped = info.pMappedData;
            bci.size = (VkDeviceSize)kAudioRayCapacity * sizeof(float);
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            if (vmaCreateBuffer(m_alloc, &bci, &aci, &m_audioRayOutBuf, &m_audioRayOutAlloc, &info) != VK_SUCCESS)
                return fail("hit-out buffer");
            m_audioRayOutMapped = info.pMappedData;
        }
        // Descriptor pool + set; write the two SSBO bindings now (TLAS is bound
        // per-call when the handle changes).
        {
            VkDescriptorPoolSize sizes[2]{};
            sizes[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; sizes[0].descriptorCount = 1;
            sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;             sizes[1].descriptorCount = 2;
            VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.maxSets = 1; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
            if (vkCreateDescriptorPool(m_dev.device, &pci, nullptr, &m_audioRayPool) != VK_SUCCESS)
                return fail("descriptor pool");
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = m_audioRayPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &m_audioRaySetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &ai, &m_audioRaySet) != VK_SUCCESS)
                return fail("descriptor set");
            VkDescriptorBufferInfo inB{ m_audioRayInBuf, 0, VK_WHOLE_SIZE };
            VkDescriptorBufferInfo outB{ m_audioRayOutBuf, 0, VK_WHOLE_SIZE };
            VkWriteDescriptorSet w[2]{};
            w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[0].dstSet = m_audioRaySet; w[0].dstBinding = 1; w[0].descriptorCount = 1;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[0].pBufferInfo = &inB;
            w[1] = w[0]; w[1].dstBinding = 2; w[1].pBufferInfo = &outB;
            vkUpdateDescriptorSets(m_dev.device, 2, w, 0, nullptr);
        }
        // Transient command buffer + fence (own pool: self-contained, like VulkanRT).
        {
            VkCommandPoolCreateInfo cpci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            cpci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            cpci.queueFamilyIndex = m_gfxFamily;
            if (vkCreateCommandPool(m_dev.device, &cpci, nullptr, &m_audioRayCmdPool) != VK_SUCCESS)
                return fail("command pool");
            VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            ai.commandPool = m_audioRayCmdPool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(m_dev.device, &ai, &m_audioRayCmd) != VK_SUCCESS)
                return fail("command buffer");
            VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            if (vkCreateFence(m_dev.device, &fci, nullptr, &m_audioRayFence) != VK_SUCCESS)
                return fail("fence");
        }
        m_audioRayBuilt = true;
        logInfo("[rta] audio-ray chain ready (audio rays through the scene TLAS)");
        return true;
    }

    void destroyAudioRays() {
        if (!m_dev.device) return;
        if (m_audioRayInFlight && m_audioRayFence) {
            vkWaitForFences(m_dev.device, 1, &m_audioRayFence, VK_TRUE, UINT64_MAX);
            m_audioRayInFlight = false;
            m_audioRayInFlightCount = 0;
        }
        if (m_audioRayFence)   { vkDestroyFence(m_dev.device, m_audioRayFence, nullptr); m_audioRayFence = VK_NULL_HANDLE; }
        if (m_audioRayCmdPool) { vkDestroyCommandPool(m_dev.device, m_audioRayCmdPool, nullptr); m_audioRayCmdPool = VK_NULL_HANDLE; m_audioRayCmd = VK_NULL_HANDLE; }
        if (m_audioRayPool)    { vkDestroyDescriptorPool(m_dev.device, m_audioRayPool, nullptr); m_audioRayPool = VK_NULL_HANDLE; m_audioRaySet = VK_NULL_HANDLE; }
        if (m_audioRayInBuf)   { vmaDestroyBuffer(m_alloc, m_audioRayInBuf, m_audioRayInAlloc); m_audioRayInBuf = VK_NULL_HANDLE; m_audioRayInAlloc = nullptr; m_audioRayInMapped = nullptr; }
        if (m_audioRayOutBuf)  { vmaDestroyBuffer(m_alloc, m_audioRayOutBuf, m_audioRayOutAlloc); m_audioRayOutBuf = VK_NULL_HANDLE; m_audioRayOutAlloc = nullptr; m_audioRayOutMapped = nullptr; }
        if (m_audioRayPipe)      { vkDestroyPipeline(m_dev.device, m_audioRayPipe, nullptr); m_audioRayPipe = VK_NULL_HANDLE; }
        if (m_audioRayLayout)    { vkDestroyPipelineLayout(m_dev.device, m_audioRayLayout, nullptr); m_audioRayLayout = VK_NULL_HANDLE; }
        if (m_audioRaySetLayout) { vkDestroyDescriptorSetLayout(m_dev.device, m_audioRaySetLayout, nullptr); m_audioRaySetLayout = VK_NULL_HANDLE; }
        m_audioRayTlasBound = VK_NULL_HANDLE;
        m_audioRayBuilt = false;
    }

    // Fill the per-frame RT-AO compute UBO (invViewProj + camPos + tunables). Uses
    // the SAME viewProj prepareFrameData cached this frame.
    void prepareRtaoUbo() {
        auto& fr = m_frames[m_frameIdx];
        if (!m_rtaoUboMapped[m_frameIdx]) (void)fr;
        RtaoUBO u{};
        u.invViewProj = glm::inverse(m_lastViewProj);
        u.camPos = glm::vec4(m_camPos, 1.0f);
        u.params0 = glm::vec4(m_rtao.radius, (float)m_rtao.rays, m_rtao.bias, m_rtao.strength);
        u.params1 = glm::vec4((float)m_rtaoExtent.width, (float)m_rtaoExtent.height,
                              (float)(m_rtFrameSeed++), m_rtao.power);
        if (m_rtaoUboMapped[m_frameIdx])
            std::memcpy(m_rtaoUboMapped[m_frameIdx], &u, sizeof(u));
        m_rtaoApplyPush.aoTexel[0] = 1.0f / (float)std::max(1u, m_rtaoExtent.width);
        m_rtaoApplyPush.aoTexel[1] = 1.0f / (float)std::max(1u, m_rtaoExtent.height);
        m_rtaoApplyPush.strength = m_rtao.strength;
        m_rtaoApplyPush.pad0 = 0.0f;
    }

    // Record the RT-AO compute dispatch body (the graph has emitted the AO-image
    // GENERAL transition + the depth READ_ONLY transition). Traces rayQueryEXT
    // against the TLAS and writes the half-res AO image.
    void recordRtaoComputeBody(VkCommandBuffer c) {
        if (!m_rtaoPipe) return;
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_rtaoPipe);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_rtaoLayout,
                                0, 1, &m_rtaoSet[m_frameIdx], 0, nullptr);
        const uint32_t gx = (m_rtaoExtent.width  + 7) / 8;
        const uint32_t gy = (m_rtaoExtent.height + 7) / 8;
        vkCmdDispatch(c, gx, gy, 1);
    }

    // ======================================================================
    // SSR / RAY-QUERY REFLECTIONS (refl.comp) — lazy init / targets /
    // descriptors / per-frame UBO / compute dispatch / teardown.
    // ----------------------------------------------------------------------
    // Built LAZILY the first time r_ssr activates (TAA on + history targets
    // exist), so runs that never enable reflections pay zero init cost. The SSR
    // pipeline (refl.comp.spv) is created on EVERY device; the ray-query variant
    // (refl_rt.comp.spv, TLAS at binding 4) only on RT devices — Pascal-class
    // hardware is automatically SSR-only. The first build does a device wait:
    // the ALWAYS-BOUND mesh set3 (binding 2 = this pass's output) must be
    // rewritten for all frames in flight.
    // ======================================================================
    bool ensureReflReady() {
        if (!m_reflBuilt) {
            // mesh set3 binding2 is rewritten below for ALL frames in flight ->
            // those sets may still be referenced by executing frames. One-time
            // hitch on first enable only.
            vkDeviceWaitIdle(m_dev.device);
            if (!createRefl() || !createReflTargets()) {
                logError("[rhi] reflections: create failed — r_ssr disabled");
                destroyRefl();
                m_refl.ssr = false;
                return false;
            }
            writeReflDescriptors();
            writeSsaoDescriptors();   // re-point mesh set3 binding2 at the refl buffer
            m_reflBuilt = true;
            logInfo(m_reflPipeRt
                ? "[rhi] reflections ready (SSR depth-march vs prev-frame color + ray-query fallback)"
                : "[rhi] reflections ready (SSR depth-march vs prev-frame color; no RT fallback)");
        }
        // Live r_reflquality switch: recreate the target at the new resolution.
        if (m_reflFullRes != m_refl.fullRes) {
            vkDeviceWaitIdle(m_dev.device);
            if (!createReflTargets()) { m_refl.ssr = false; return false; }
            writeReflDescriptors();
            writeSsaoDescriptors();
        }
        return m_reflImg != VK_NULL_HANDLE && m_reflPipe != VK_NULL_HANDLE;
    }

    bool createRefl() {
        // ---- Set layouts: 0 = depth sampler, 1 = output storage image, 2 = prev
        // scene (TAA history) sampler, 3 = Refl UBO; the RT variant adds 4 = TLAS.
        {
            VkDescriptorSetLayoutBinding b[5]{};
            b[0].binding = 0; b[0].descriptorCount = 1;
            b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[1].binding = 1; b[1].descriptorCount = 1;
            b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[2].binding = 2; b[2].descriptorCount = 1;
            b[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[3].binding = 3; b[3].descriptorCount = 1;
            b[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            b[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = 4; ci.pBindings = b;
            if (vkCreateDescriptorSetLayout(m_dev.device, &ci, nullptr, &m_reflSetLayout) != VK_SUCCESS) return false;
            if (m_rtSupported) {
                b[4].binding = 4; b[4].descriptorCount = 1;
                b[4].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
                b[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                ci.bindingCount = 5;
                if (vkCreateDescriptorSetLayout(m_dev.device, &ci, nullptr, &m_reflSetLayoutRt) != VK_SUCCESS) return false;
            }
        }
        // ---- Pool + per-frame UBOs + sets (SSR always; RT sets on RT devices). ----
        {
            const uint32_t nFrames = kFramesInFlight;
            const uint32_t nVariants = m_rtSupported ? 2u : 1u;
            VkDescriptorPoolSize sizes[4]{};
            uint32_t nSizes = 3;
            sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            sizes[0].descriptorCount = nFrames * nVariants * 2;   // depth + prevScene
            sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            sizes[1].descriptorCount = nFrames * nVariants;
            sizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            sizes[2].descriptorCount = nFrames * nVariants;
            if (m_rtSupported) {
                sizes[3].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
                sizes[3].descriptorCount = nFrames;
                nSizes = 4;
            }
            VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.maxSets = nFrames * nVariants; pci.poolSizeCount = nSizes; pci.pPoolSizes = sizes;
            if (x3CreateDescriptorPool(&pci, nullptr, &m_reflPool) != VK_SUCCESS) return false;
        }
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkBufferCreateInfo ub{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            ub.size = sizeof(ReflUBO); ub.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info{};
            if (x3vmaCreateBuffer(&ub, &aci, &m_reflUboBuf[i], &m_reflUboAlloc[i], &info) != VK_SUCCESS) return false;
            m_reflUboMapped[i] = info.pMappedData;

            VkDescriptorSetAllocateInfo a0{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            a0.descriptorPool = m_reflPool; a0.descriptorSetCount = 1; a0.pSetLayouts = &m_reflSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &a0, &m_reflSet[i]) != VK_SUCCESS) return false;
            if (m_rtSupported) {
                VkDescriptorSetAllocateInfo a1{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
                a1.descriptorPool = m_reflPool; a1.descriptorSetCount = 1; a1.pSetLayouts = &m_reflSetLayoutRt;
                if (vkAllocateDescriptorSets(m_dev.device, &a1, &m_reflSetRt[i]) != VK_SUCCESS) return false;
            }
        }
        // ---- Compute pipelines: SSR-only (every device) + ray-query (RT only). ----
        {
            VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pl.setLayoutCount = 1; pl.pSetLayouts = &m_reflSetLayout;
            if (vkCreatePipelineLayout(m_dev.device, &pl, nullptr, &m_reflLayout) != VK_SUCCESS) return false;
            VkShaderModule cs = loadShaderModule("shaders\\refl.comp.spv");
            if (!cs) return false;
            VkComputePipelineCreateInfo cpci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpci.stage.module = cs; cpci.stage.pName = "main";
            cpci.layout = m_reflLayout;
            VkResult pr = x3CreateComputePipelines(1, &cpci, nullptr, &m_reflPipe);
            vkDestroyShaderModule(m_dev.device, cs, nullptr);
            if (pr != VK_SUCCESS) return false;
        }
        if (m_rtSupported) {
            // Non-fatal: an RT-pipeline failure degrades to SSR-only (logged).
            VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pl.setLayoutCount = 1; pl.pSetLayouts = &m_reflSetLayoutRt;
            if (vkCreatePipelineLayout(m_dev.device, &pl, nullptr, &m_reflLayoutRt) == VK_SUCCESS) {
                VkShaderModule cs = loadShaderModule("shaders\\refl_rt.comp.spv");
                if (cs) {
                    VkComputePipelineCreateInfo cpci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
                    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpci.stage.module = cs; cpci.stage.pName = "main";
                    cpci.layout = m_reflLayoutRt;
                    if (x3CreateComputePipelines(1, &cpci, nullptr, &m_reflPipeRt) != VK_SUCCESS)
                        m_reflPipeRt = VK_NULL_HANDLE;
                    vkDestroyShaderModule(m_dev.device, cs, nullptr);
                }
            }
            if (!m_reflPipeRt)
                logError("[rhi] reflections: ray-query pipeline unavailable — SSR-only");
        }
        return true;
    }

    // (Re)create the reflection storage target at the current extent and quality
    // (r_reflquality: half- or full-res). The image is transitioned ONCE to
    // SHADER_READ_ONLY so the always-bound mesh set3 binding2 is layout-valid even
    // on frames where the refl pass doesn't run (it is then never sampled — the
    // ssao.refl.x gate is 0 — but the descriptor must still match the layout).
    bool createReflTargets() {
        destroyReflTargets();
        m_reflFullRes = m_refl.fullRes;
        const uint32_t div = m_reflFullRes ? 1u : 2u;
        m_reflExtent = { std::max(1u, m_extent.width / div), std::max(1u, m_extent.height / div) };
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (!createColorTarget(kReflFormat, m_reflExtent.width, m_reflExtent.height, usage,
                               m_reflImg, m_reflAlloc, m_reflView)) return false;
        const bool ok = oneTimeSubmit([&](VkCommandBuffer cmd){
            iblBarrierTex2D(cmd, m_reflImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        });
        if (!ok) { logError("[rhi] reflections: target init transition failed"); return false; }
        return true;
    }

    void destroyReflTargets() {
        if (m_reflView) { vkDestroyImageView(m_dev.device, m_reflView, nullptr); m_reflView = VK_NULL_HANDLE; }
        if (m_reflImg)  { vmaDestroyImage(m_alloc, m_reflImg, m_reflAlloc); m_reflImg = VK_NULL_HANDLE; m_reflAlloc = nullptr; }
    }

    // (Re)write the refl compute sets (depth + output + TAA history + UBO; the RT
    // set also gets the TLAS when one exists). Called after target creation and on
    // resize (the depth/history/output views all change with the extent).
    void writeReflDescriptors() {
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkDescriptorImageInfo depthInfo{ m_depthSampler, m_depthView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo outInfo{ VK_NULL_HANDLE, m_reflView, VK_IMAGE_LAYOUT_GENERAL };
            VkDescriptorImageInfo histInfo{ m_ssaoLinearSampler,
                                            m_taaHistView ? m_taaHistView : m_reflView,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorBufferInfo ubo{ m_reflUboBuf[i], 0, sizeof(ReflUBO) };
            VkDescriptorSet targets[2] = { m_reflSet[i], m_reflSetRt[i] };
            for (int s = 0; s < 2; ++s) {
                if (!targets[s]) continue;
                VkWriteDescriptorSet w[4]{};
                w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = targets[s]; w[0].dstBinding = 0; w[0].descriptorCount = 1;
                w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &depthInfo;
                w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = targets[s]; w[1].dstBinding = 1; w[1].descriptorCount = 1;
                w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[1].pImageInfo = &outInfo;
                w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[2].dstSet = targets[s]; w[2].dstBinding = 2; w[2].descriptorCount = 1;
                w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[2].pImageInfo = &histInfo;
                w[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[3].dstSet = targets[s]; w[3].dstBinding = 3; w[3].descriptorCount = 1;
                w[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[3].pBufferInfo = &ubo;
                vkUpdateDescriptorSets(m_dev.device, 4, w, 0, nullptr);
            }
        }
        // The TLAS (RT sets, binding 4) may already exist (e.g. RT AO enabled
        // first): point the fresh sets at it now; otherwise rewriteRtaoTlas()
        // fills it after the first build.
        if (m_rt.tlasBuilt()) rewriteRtaoTlas();
    }

    // Fill the per-frame reflection UBO. Uses the SAME (jittered) viewProj this
    // frame's depth was rasterized with (m_lastViewProj, cached by
    // prepareFrameData) + the previous frame's UNJITTERED viewProj captured there.
    void prepareReflUbo() {
        ReflUBO u{};
        u.invViewProj  = glm::inverse(m_lastViewProj);
        u.viewProj     = m_lastViewProj;
        u.prevViewProj = m_reflPrevVP;
        u.camPos = glm::vec4(m_camPos, m_reflHistValid ? 1.0f : 0.0f);
        u.sunDir = glm::vec4(glm::normalize(glm::vec3(m_sky.sunDir[0], m_sky.sunDir[1], m_sky.sunDir[2])),
                             (float)(m_rtFrameSeed++));
        u.ambient = glm::vec4(m_ambient, 0.0f);
        // March tuning: 48 m reach, 0.5 m base thickness, 24 linear steps with the
        // shader's mild geometric growth + 5-iteration binary refine.
        u.params0 = glm::vec4((float)m_reflExtent.width, (float)m_reflExtent.height, 48.0f, 0.5f);
        u.params1 = glm::vec4(24.0f, 0.0f, 0.0f, 0.0f);
        if (m_reflUboMapped[m_frameIdx])
            std::memcpy(m_reflUboMapped[m_frameIdx], &u, sizeof(u));
    }

    // Record the reflections compute dispatch (the graph already emitted the
    // output GENERAL + depth READ_ONLY + history READ_ONLY transitions). Binds
    // the ray-query pipeline when the TLAS was built this frame, else SSR-only.
    void recordReflComputeBody(VkCommandBuffer c) {
        const bool rt = m_reflRtThisFrame && (m_reflPipeRt != VK_NULL_HANDLE)
                     && (m_reflSetRt[m_frameIdx] != VK_NULL_HANDLE);
        VkPipeline pipe = rt ? m_reflPipeRt : m_reflPipe;
        if (!pipe) return;
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE,
                                rt ? m_reflLayoutRt : m_reflLayout, 0, 1,
                                rt ? &m_reflSetRt[m_frameIdx] : &m_reflSet[m_frameIdx], 0, nullptr);
        const uint32_t gx = (m_reflExtent.width  + 7) / 8;
        const uint32_t gy = (m_reflExtent.height + 7) / 8;
        vkCmdDispatch(c, gx, gy, 1);
    }

    void destroyRefl() {
        if (!m_dev.device) return;
        destroyReflTargets();
        if (m_reflPipeRt)      { vkDestroyPipeline(m_dev.device, m_reflPipeRt, nullptr); m_reflPipeRt = VK_NULL_HANDLE; }
        if (m_reflPipe)        { vkDestroyPipeline(m_dev.device, m_reflPipe, nullptr); m_reflPipe = VK_NULL_HANDLE; }
        if (m_reflLayoutRt)    { vkDestroyPipelineLayout(m_dev.device, m_reflLayoutRt, nullptr); m_reflLayoutRt = VK_NULL_HANDLE; }
        if (m_reflLayout)      { vkDestroyPipelineLayout(m_dev.device, m_reflLayout, nullptr); m_reflLayout = VK_NULL_HANDLE; }
        if (m_reflPool)        { vkDestroyDescriptorPool(m_dev.device, m_reflPool, nullptr); m_reflPool = VK_NULL_HANDLE; }
        if (m_reflSetLayoutRt) { vkDestroyDescriptorSetLayout(m_dev.device, m_reflSetLayoutRt, nullptr); m_reflSetLayoutRt = VK_NULL_HANDLE; }
        if (m_reflSetLayout)   { vkDestroyDescriptorSetLayout(m_dev.device, m_reflSetLayout, nullptr); m_reflSetLayout = VK_NULL_HANDLE; }
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (m_reflUboBuf[i]) { vmaDestroyBuffer(m_alloc, m_reflUboBuf[i], m_reflUboAlloc[i]); m_reflUboBuf[i] = VK_NULL_HANDLE; m_reflUboMapped[i] = nullptr; }
            m_reflSet[i] = VK_NULL_HANDLE; m_reflSetRt[i] = VK_NULL_HANDLE;
        }
        m_reflBuilt = false;
        m_reflActiveThisFrame = false;
        m_reflRtThisFrame = false;
    }

    // ======================================================================
    // DDGI (r_ddgi) — lazy init / probe-volume fit / targets / descriptors /
    // per-frame UBO / compute dispatch / teardown. Tier: ray query + position
    // fetch ONLY (m_rtPosFetch); everything else never touches this.
    // ======================================================================
    bool ensureDdgiReady() {
        if (!ensureRtCore() || !m_rtPosFetch) return false;
        // The ray pass's MISS shading binds the IBL env cube; without the IBL
        // chain there is no cube view to bind (rare init failure) — stay off.
        if (m_iblEnvCubeView == VK_NULL_HANDLE) return false;
        if (!m_ddgiBuilt) {
            // mesh set3 bindings 3/4 are rewritten for ALL frames in flight ->
            // those sets may be referenced by executing frames. One-time hitch.
            vkDeviceWaitIdle(m_dev.device);
            if (!createDdgi() || !createDdgiTargets()) {
                logError("[rhi] DDGI: create failed — r_ddgi disabled");
                destroyDdgi();
                m_ddgi.enabled = false;
                return false;
            }
            writeDdgiDescriptors();
            writeSsaoDescriptors();   // re-point mesh set3 bindings 3/4 at the atlases
            m_ddgiBuilt = true;
            logInfo("[rhi] DDGI ready (ray-query probe grid: octahedral irradiance + Chebyshev visibility atlases)");
        }
        // Live grid-dimension change (r_ddgi_n*): recreate the atlases + ray buffer.
        if (m_ddgiCountX != m_ddgi.countX || m_ddgiCountY != m_ddgi.countY ||
            m_ddgiCountZ != m_ddgi.countZ) {
            vkDeviceWaitIdle(m_dev.device);
            if (!createDdgiTargets()) { m_ddgi.enabled = false; return false; }
            writeDdgiDescriptors();
            writeSsaoDescriptors();
            m_ddgiVolumeValid = false;   // spacing depends on counts
        }
        if (!m_ddgiVolumeValid) computeDdgiVolume();
        return m_ddgiRayPipe != VK_NULL_HANDLE && m_ddgiUpPipe != VK_NULL_HANDLE
            && m_ddgiIrrImg != VK_NULL_HANDLE;
    }

    // Fit the probe volume: an explicit volume from the params when given,
    // otherwise an AABB over THIS frame's static draw-record origins + padding
    // (instance origins approximate the playable volume well for the built
    // levels; cvar override available for exotic scenes). Sticky once fitted —
    // probes must be world-stable for the hysteresis to converge.
    void computeDdgiVolume() {
        glm::vec3 mn, mx;
        if (m_ddgi.sizeX > 0.0f && m_ddgi.sizeY > 0.0f && m_ddgi.sizeZ > 0.0f) {
            mn = glm::vec3(m_ddgi.originX, m_ddgi.originY, m_ddgi.originZ);
            mx = mn + glm::vec3(m_ddgi.sizeX, m_ddgi.sizeY, m_ddgi.sizeZ);
        } else {
            mn = glm::vec3(FLT_MAX); mx = glm::vec3(-FLT_MAX);
            uint32_t n = 0;
            for (const DrawRecord& dr : m_drawRecords) {
                auto it = m_meshes.find(dr.meshId);
                if (it == m_meshes.end() || it->second.dynamic) continue;
                const glm::vec3 t(dr.model[12], dr.model[13], dr.model[14]);
                mn = glm::min(mn, t); mx = glm::max(mx, t);
                ++n;
            }
            if (n == 0) { mn = m_camPos - glm::vec3(20.0f); mx = m_camPos + glm::vec3(20.0f); }
            mn -= glm::vec3(3.0f, 1.5f, 3.0f);
            mx += glm::vec3(3.0f, 4.0f, 3.0f);
            // Clamp pathological extents (a stray skybox-distance instance would
            // stretch the grid into uselessness): max 240 m per axis around center.
            const glm::vec3 c = (mn + mx) * 0.5f;
            const glm::vec3 he = glm::min((mx - mn) * 0.5f, glm::vec3(120.0f));
            mn = c - he; mx = c + he;
        }
        m_ddgiOrigin = mn;
        m_ddgiSpacing = (mx - mn) / glm::vec3((float)std::max(1, m_ddgiCountX - 1),
                                              (float)std::max(1, m_ddgiCountY - 1),
                                              (float)std::max(1, m_ddgiCountZ - 1));
        m_ddgiSpacing = glm::max(m_ddgiSpacing, glm::vec3(0.25f));
        m_ddgiVisMaxDist = 1.5f * glm::length(m_ddgiSpacing);
        m_ddgiVolumeValid = true;
        m_ddgiFrameCount = 0;   // fresh volume -> full warm-up ramp (fast reconverge)
        logInfo("[rhi] DDGI probe grid " + std::to_string(m_ddgiCountX) + "x" +
                std::to_string(m_ddgiCountY) + "x" + std::to_string(m_ddgiCountZ) +
                " over (" + std::to_string(mn.x) + "," + std::to_string(mn.y) + "," +
                std::to_string(mn.z) + ")..(" + std::to_string(mx.x) + "," +
                std::to_string(mx.y) + "," + std::to_string(mx.z) + ") spacing (" +
                std::to_string(m_ddgiSpacing.x) + "," + std::to_string(m_ddgiSpacing.y) +
                "," + std::to_string(m_ddgiSpacing.z) + ") m");
    }

    bool createDdgi() {
        // LINEAR/CLAMP sampler for the atlas reads (compute feedback + nothing else;
        // mesh.frag set3 uses m_ssaoLinearSampler like its other bindings).
        VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(m_dev.device, &sci, nullptr, &m_ddgiSampler) != VK_SUCCESS) return false;

        // ---- RAY set layout: 0=TLAS, 1=object SSBO, 2=ray SSBO, 3=UBO,
        //      4=prev irradiance, 5=prev visibility, 6=env cube. ----
        {
            VkDescriptorSetLayoutBinding b[7]{};
            b[0].binding = 0; b[0].descriptorCount = 1;
            b[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            b[1].binding = 1; b[1].descriptorCount = 1;
            b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[2].binding = 2; b[2].descriptorCount = 1;
            b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[3].binding = 3; b[3].descriptorCount = 1;
            b[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            b[4].binding = 4; b[4].descriptorCount = 1;
            b[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[5].binding = 5; b[5].descriptorCount = 1;
            b[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[6].binding = 6; b[6].descriptorCount = 1;
            b[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            for (int i = 0; i < 7; ++i) b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = 7; ci.pBindings = b;
            if (vkCreateDescriptorSetLayout(m_dev.device, &ci, nullptr, &m_ddgiRaySetLayout) != VK_SUCCESS) return false;
        }
        // ---- UPDATE set layout: 0=ray SSBO, 1=irr storage image, 2=vis storage
        //      image, 3=UBO. ----
        {
            VkDescriptorSetLayoutBinding b[4]{};
            b[0].binding = 0; b[0].descriptorCount = 1;
            b[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[1].binding = 1; b[1].descriptorCount = 1;
            b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            b[2].binding = 2; b[2].descriptorCount = 1;
            b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            b[3].binding = 3; b[3].descriptorCount = 1;
            b[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            for (int i = 0; i < 4; ++i) b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = 4; ci.pBindings = b;
            if (vkCreateDescriptorSetLayout(m_dev.device, &ci, nullptr, &m_ddgiUpSetLayout) != VK_SUCCESS) return false;
        }
        // ---- Pool + per-frame UBOs + sets. ----
        {
            const uint32_t nF = kFramesInFlight;
            VkDescriptorPoolSize sizes[5]{};
            sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;     sizes[0].descriptorCount = nF * 3;
            sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;             sizes[1].descriptorCount = nF * 3;
            sizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;             sizes[2].descriptorCount = nF * 2;
            sizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;              sizes[3].descriptorCount = nF * 2;
            sizes[4].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; sizes[4].descriptorCount = nF;
            VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.maxSets = nF * 2; pci.poolSizeCount = 5; pci.pPoolSizes = sizes;
            if (x3CreateDescriptorPool(&pci, nullptr, &m_ddgiPool) != VK_SUCCESS) return false;
        }
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkBufferCreateInfo ub{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            ub.size = sizeof(DdgiUBO); ub.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info{};
            if (x3vmaCreateBuffer(&ub, &aci, &m_ddgiUboBuf[i], &m_ddgiUboAlloc[i], &info) != VK_SUCCESS) return false;
            m_ddgiUboMapped[i] = info.pMappedData;
            VkDescriptorSetAllocateInfo a0{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            a0.descriptorPool = m_ddgiPool; a0.descriptorSetCount = 1; a0.pSetLayouts = &m_ddgiRaySetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &a0, &m_ddgiRaySet[i]) != VK_SUCCESS) return false;
            VkDescriptorSetAllocateInfo a1{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            a1.descriptorPool = m_ddgiPool; a1.descriptorSetCount = 1; a1.pSetLayouts = &m_ddgiUpSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &a1, &m_ddgiUpSet[i]) != VK_SUCCESS) return false;
        }
        // ---- Compute pipelines: ddgi_rays + ddgi_update (mode push constant). ----
        {
            VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pl.setLayoutCount = 1; pl.pSetLayouts = &m_ddgiRaySetLayout;
            if (vkCreatePipelineLayout(m_dev.device, &pl, nullptr, &m_ddgiRayLayout) != VK_SUCCESS) return false;
            VkShaderModule cs = loadShaderModule("shaders\\ddgi_rays.comp.spv");
            if (!cs) return false;
            VkComputePipelineCreateInfo cpci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpci.stage.module = cs; cpci.stage.pName = "main";
            cpci.layout = m_ddgiRayLayout;
            VkResult pr = x3CreateComputePipelines(1, &cpci, nullptr, &m_ddgiRayPipe);
            vkDestroyShaderModule(m_dev.device, cs, nullptr);
            if (pr != VK_SUCCESS) return false;
        }
        {
            VkPushConstantRange pcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t) };
            VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pl.setLayoutCount = 1; pl.pSetLayouts = &m_ddgiUpSetLayout;
            pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr;
            if (vkCreatePipelineLayout(m_dev.device, &pl, nullptr, &m_ddgiUpLayout) != VK_SUCCESS) return false;
            VkShaderModule cs = loadShaderModule("shaders\\ddgi_update.comp.spv");
            if (!cs) return false;
            VkComputePipelineCreateInfo cpci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpci.stage.module = cs; cpci.stage.pName = "main";
            cpci.layout = m_ddgiUpLayout;
            VkResult pr = x3CreateComputePipelines(1, &cpci, nullptr, &m_ddgiUpPipe);
            vkDestroyShaderModule(m_dev.device, cs, nullptr);
            if (pr != VK_SUCCESS) return false;
        }
        return true;
    }

    // (Re)create the octahedral atlases + the per-frame ray buffer for the
    // CURRENT grid counts. Atlas tile layout: tileU = px + py*countX (a
    // countX*countY tile row), tileV = pz; irradiance tiles are 8x8 texels
    // (6x6 interior + border), visibility 16x16 (14x14 + border). Both are
    // cleared to zero and left SHADER_READ_ONLY (the warm-up hysteresis ramp
    // fully overwrites them on the first update).
    bool createDdgiTargets() {
        destroyDdgiTargets();
        m_ddgiCountX = m_ddgi.countX; m_ddgiCountY = m_ddgi.countY; m_ddgiCountZ = m_ddgi.countZ;
        const uint32_t tilesU = (uint32_t)(m_ddgiCountX * m_ddgiCountY);
        const uint32_t tilesV = (uint32_t)m_ddgiCountZ;
        const uint32_t probeCount = (uint32_t)(m_ddgiCountX * m_ddgiCountY * m_ddgiCountZ);
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                                      | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!createColorTarget(kDdgiIrrFormat, tilesU * 8u, tilesV * 8u, usage,
                               m_ddgiIrrImg, m_ddgiIrrAlloc, m_ddgiIrrView)) return false;
        if (!createColorTarget(kDdgiVisFormat, tilesU * 16u, tilesV * 16u, usage,
                               m_ddgiVisImg, m_ddgiVisAlloc, m_ddgiVisView)) return false;

        // Ray results SSBO: probeCount * 128 (max rays) * vec4, device-local.
        m_ddgiRayBufSize = (VkDeviceSize)probeCount * 128u * 16u;
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = m_ddgiRayBufSize;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        if (x3vmaCreateBuffer(&bci, &aci, &m_ddgiRayBuf, &m_ddgiRayAlloc, nullptr) != VK_SUCCESS)
            return false;

        // Clear both atlases to zero + park them SHADER_READ_ONLY so the very
        // first ddgi-rays pass (which samples them as "previous frame") reads
        // defined black and the always-bound mesh set3 descriptors are valid.
        const bool ok = oneTimeSubmit([&](VkCommandBuffer cmd){
            VkImage imgs[2] = { m_ddgiIrrImg, m_ddgiVisImg };
            for (int i = 0; i < 2; ++i) {
                iblBarrierTex2D(cmd, imgs[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                VkClearColorValue zero{};
                VkImageSubresourceRange r{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                vkCmdClearColorImage(cmd, imgs[i], VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &r);
                iblBarrierTex2D(cmd, imgs[i], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            }
        });
        if (!ok) { logError("[rhi] DDGI: atlas init transition failed"); return false; }
        const ResourceState ready{ VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT };
        m_ddgiIrrState = ready;
        m_ddgiVisState = ready;
        m_ddgiFrameCount = 0;
        m_ddgiVolumeValid = false;   // new counts -> refit spacing
        return true;
    }

    void destroyDdgiTargets() {
        if (m_ddgiIrrView) { vkDestroyImageView(m_dev.device, m_ddgiIrrView, nullptr); m_ddgiIrrView = VK_NULL_HANDLE; }
        if (m_ddgiIrrImg)  { vmaDestroyImage(m_alloc, m_ddgiIrrImg, m_ddgiIrrAlloc); m_ddgiIrrImg = VK_NULL_HANDLE; m_ddgiIrrAlloc = nullptr; }
        if (m_ddgiVisView) { vkDestroyImageView(m_dev.device, m_ddgiVisView, nullptr); m_ddgiVisView = VK_NULL_HANDLE; }
        if (m_ddgiVisImg)  { vmaDestroyImage(m_alloc, m_ddgiVisImg, m_ddgiVisAlloc); m_ddgiVisImg = VK_NULL_HANDLE; m_ddgiVisAlloc = nullptr; }
        if (m_ddgiRayBuf)  { vmaDestroyBuffer(m_alloc, m_ddgiRayBuf, m_ddgiRayAlloc); m_ddgiRayBuf = VK_NULL_HANDLE; m_ddgiRayAlloc = nullptr; }
        m_ddgiCountX = m_ddgiCountY = m_ddgiCountZ = 0;
    }

    // (Re)write the per-frame DDGI compute sets. The TLAS binding (ray set,
    // binding 0) is written when one exists; otherwise rewriteRtaoTlas() fills
    // it after the first build (the set is never dispatched before then).
    void writeDdgiDescriptors() {
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkDescriptorBufferInfo objInfo{ m_frames[i].objBuf, 0, VK_WHOLE_SIZE };
            VkDescriptorBufferInfo rayInfo{ m_ddgiRayBuf, 0, VK_WHOLE_SIZE };
            VkDescriptorBufferInfo uboInfo{ m_ddgiUboBuf[i], 0, sizeof(DdgiUBO) };
            VkDescriptorImageInfo irrSampled{ m_ddgiSampler, m_ddgiIrrView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo visSampled{ m_ddgiSampler, m_ddgiVisView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo envSampled{ m_iblCubeSampler, m_iblEnvCubeView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo irrStorage{ VK_NULL_HANDLE, m_ddgiIrrView, VK_IMAGE_LAYOUT_GENERAL };
            VkDescriptorImageInfo visStorage{ VK_NULL_HANDLE, m_ddgiVisView, VK_IMAGE_LAYOUT_GENERAL };

            VkWriteDescriptorSet w[10]{};
            uint32_t n = 0;
            auto add = [&](VkDescriptorSet set, uint32_t binding, VkDescriptorType type,
                           const VkDescriptorImageInfo* ii, const VkDescriptorBufferInfo* bi) {
                w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[n].dstSet = set; w[n].dstBinding = binding; w[n].descriptorCount = 1;
                w[n].descriptorType = type; w[n].pImageInfo = ii; w[n].pBufferInfo = bi;
                ++n;
            };
            add(m_ddgiRaySet[i], 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &objInfo);
            add(m_ddgiRaySet[i], 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &rayInfo);
            add(m_ddgiRaySet[i], 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uboInfo);
            add(m_ddgiRaySet[i], 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &irrSampled, nullptr);
            add(m_ddgiRaySet[i], 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &visSampled, nullptr);
            add(m_ddgiRaySet[i], 6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &envSampled, nullptr);
            add(m_ddgiUpSet[i], 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &rayInfo);
            add(m_ddgiUpSet[i], 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &irrStorage, nullptr);
            add(m_ddgiUpSet[i], 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &visStorage, nullptr);
            add(m_ddgiUpSet[i], 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uboInfo);
            vkUpdateDescriptorSets(m_dev.device, n, w, 0, nullptr);
        }
        if (m_rt.tlasBuilt()) rewriteRtaoTlas();   // fills ray-set binding 0
    }

    // Fill the per-frame DDGI UBO: grid geometry, the per-frame random ray
    // rotation, sun/ambient/lights for hit shading, and the EFFECTIVE
    // hysteresis — a cumulative-moving-average ramp (h = n/(n+1), capped at
    // the target) so a fresh/black probe field converges in a handful of
    // frames instead of fading in over seconds.
    void prepareDdgiUbo() {
        DdgiUBO u{};
        u.gridOrigin  = glm::vec4(m_ddgiOrigin, (float)m_ddgi.raysPerProbe);
        u.gridSpacing = glm::vec4(m_ddgiSpacing, m_ddgiVisMaxDist);
        u.gridCounts  = glm::ivec4(m_ddgiCountX, m_ddgiCountY, m_ddgiCountZ, (int)m_ddgiFrameCount);

        // Uniform random rotation (Shoemake quaternion from a deterministic LCG
        // over the frame counter) — both compute shaders rebuild the SAME ray
        // directions from it.
        uint32_t s = m_ddgiFrameCount * 2654435761u + 0x9E3779B9u;
        auto rnd = [&]() { s = s * 1664525u + 1013904223u; return (float)(s >> 8) / (float)(1u << 24); };
        const float u1 = rnd(), u2 = rnd(), u3 = rnd();
        const float sq1 = std::sqrt(1.0f - u1), sq2 = std::sqrt(u1);
        const float twoPi = 6.28318530718f;
        glm::quat q(sq2 * std::cos(twoPi * u3),            // w
                    sq1 * std::sin(twoPi * u2),            // x
                    sq1 * std::cos(twoPi * u2),            // y
                    sq2 * std::sin(twoPi * u3));           // z
        const glm::mat3 R = glm::mat3_cast(glm::normalize(q));
        u.rotation0 = glm::vec4(R[0], 0.0f);
        u.rotation1 = glm::vec4(R[1], 0.0f);
        u.rotation2 = glm::vec4(R[2], 0.0f);

        // Sun: same direction the raster path lights/shadows with; 0.75 matches
        // mesh.frag's dielectric sun diffuse scale (consistent energy).
        u.sunDirIntensity = glm::vec4(glm::normalize(glm::vec3(
            m_sky.sunDir[0], m_sky.sunDir[1], m_sky.sunDir[2])), 0.75f);
        u.ambientSky = glm::vec4(m_ambient, (m_iblReady && m_iblBaked) ? 1.0f : 0.0f);

        const float n = (float)m_ddgiFrameCount;
        const float hystIrr = std::min(m_ddgi.hysteresis,    n / (n + 1.0f));
        const float hystVis = std::min(m_ddgi.hysteresisVis, n / (n + 1.0f));
        const uint32_t lc = std::min<uint32_t>((uint32_t)m_pointLights.size(), kMaxPointLights);
        u.params = glm::vec4(hystIrr, (float)lc, m_ddgi.bounceGain, hystVis);
        for (uint32_t i = 0; i < lc; ++i) {
            const PointLight& pl = m_pointLights[i];
            u.lights[i].posRange = glm::vec4(pl.pos[0], pl.pos[1], pl.pos[2], pl.range);
            u.lights[i].colorPad = glm::vec4(pl.color[0], pl.color[1], pl.color[2], 0.0f);
        }
        if (m_ddgiUboMapped[m_frameIdx])
            std::memcpy(m_ddgiUboMapped[m_frameIdx], &u, sizeof(u));
    }

    // Record the DDGI ray dispatch (the graph already parked both atlases
    // SHADER_READ_ONLY for the feedback sample). The ray buffer is NOT a graph
    // resource — emit its WAR barrier (last frame's update READ it) here.
    void recordDdgiRaysBody(VkCommandBuffer c) {
        if (!m_ddgiRayPipe || !m_ddgiRayBuf) return;
        VkBufferMemoryBarrier2 pre{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
        pre.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        pre.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        pre.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        pre.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        pre.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        pre.buffer = m_ddgiRayBuf; pre.offset = 0; pre.size = VK_WHOLE_SIZE;
        VkDependencyInfo di{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        di.bufferMemoryBarrierCount = 1; di.pBufferMemoryBarriers = &pre;
        vkCmdPipelineBarrier2(c, &di);

        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_ddgiRayPipe);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_ddgiRayLayout,
                                0, 1, &m_ddgiRaySet[m_frameIdx], 0, nullptr);
        const uint32_t probeCount = (uint32_t)(m_ddgiCountX * m_ddgiCountY * m_ddgiCountZ);
        vkCmdDispatch(c, probeCount, 1, 1);   // one workgroup (128 ray threads) per probe
    }

    // Record the DDGI update dispatches (the graph already transitioned both
    // atlases to GENERAL). Ray-buffer write -> read barrier first, then one
    // dispatch per atlas (push-constant mode selects irradiance/visibility).
    void recordDdgiUpdateBody(VkCommandBuffer c) {
        if (!m_ddgiUpPipe || !m_ddgiRayBuf) return;
        VkBufferMemoryBarrier2 pre{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
        pre.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        pre.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        pre.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        pre.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        pre.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        pre.buffer = m_ddgiRayBuf; pre.offset = 0; pre.size = VK_WHOLE_SIZE;
        VkDependencyInfo di{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        di.bufferMemoryBarrierCount = 1; di.pBufferMemoryBarriers = &pre;
        vkCmdPipelineBarrier2(c, &di);

        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_ddgiUpPipe);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_ddgiUpLayout,
                                0, 1, &m_ddgiUpSet[m_frameIdx], 0, nullptr);
        const uint32_t probeCount = (uint32_t)(m_ddgiCountX * m_ddgiCountY * m_ddgiCountZ);
        uint32_t mode = 0;   // irradiance (8x8 tiles)
        vkCmdPushConstants(c, m_ddgiUpLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mode), &mode);
        vkCmdDispatch(c, probeCount, 1, 1);
        mode = 1;            // visibility (16x16 tiles) — disjoint image, no hazard
        vkCmdPushConstants(c, m_ddgiUpLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mode), &mode);
        vkCmdDispatch(c, probeCount, 1, 1);
    }

    void destroyDdgi() {
        if (!m_dev.device) return;
        destroyDdgiTargets();
        if (m_ddgiUpPipe)       { vkDestroyPipeline(m_dev.device, m_ddgiUpPipe, nullptr); m_ddgiUpPipe = VK_NULL_HANDLE; }
        if (m_ddgiRayPipe)      { vkDestroyPipeline(m_dev.device, m_ddgiRayPipe, nullptr); m_ddgiRayPipe = VK_NULL_HANDLE; }
        if (m_ddgiUpLayout)     { vkDestroyPipelineLayout(m_dev.device, m_ddgiUpLayout, nullptr); m_ddgiUpLayout = VK_NULL_HANDLE; }
        if (m_ddgiRayLayout)    { vkDestroyPipelineLayout(m_dev.device, m_ddgiRayLayout, nullptr); m_ddgiRayLayout = VK_NULL_HANDLE; }
        if (m_ddgiPool)         { vkDestroyDescriptorPool(m_dev.device, m_ddgiPool, nullptr); m_ddgiPool = VK_NULL_HANDLE; }
        if (m_ddgiUpSetLayout)  { vkDestroyDescriptorSetLayout(m_dev.device, m_ddgiUpSetLayout, nullptr); m_ddgiUpSetLayout = VK_NULL_HANDLE; }
        if (m_ddgiRaySetLayout) { vkDestroyDescriptorSetLayout(m_dev.device, m_ddgiRaySetLayout, nullptr); m_ddgiRaySetLayout = VK_NULL_HANDLE; }
        if (m_ddgiSampler)      { vkDestroySampler(m_dev.device, m_ddgiSampler, nullptr); m_ddgiSampler = VK_NULL_HANDLE; }
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (m_ddgiUboBuf[i]) { vmaDestroyBuffer(m_alloc, m_ddgiUboBuf[i], m_ddgiUboAlloc[i]); m_ddgiUboBuf[i] = VK_NULL_HANDLE; m_ddgiUboMapped[i] = nullptr; }
            m_ddgiRaySet[i] = VK_NULL_HANDLE; m_ddgiUpSet[i] = VK_NULL_HANDLE;
        }
        m_ddgiBuilt = false;
        m_ddgiWantThisFrame = false;
        m_ddgiActiveThisFrame = false;
        m_ddgiVolumeValid = false;
        m_ddgiFrameCount = 0;
    }

    // Record the auto-exposure reduce/adapt dispatch (single 16x16 workgroup; the
    // graph has already transitioned the HDR scene to SHADER_READ_ONLY for the
    // compute sample). The exposure SSBO is NOT a graph resource, so this body
    // owns its hazards: a PRE barrier orders the dispatch after the previous
    // frame's composite fragment read (and any prior compute access — same
    // submission queue), and a POST barrier makes the new adapted value visible
    // to this frame's composite fragment read.
    void recordAutoExposureBody(VkCommandBuffer c) {
        if (!m_aePipe || !m_aeBuf) return;
        VkBufferMemoryBarrier2 pre{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
        pre.srcStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        pre.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        pre.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        pre.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        pre.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        pre.buffer = m_aeBuf; pre.offset = 0; pre.size = VK_WHOLE_SIZE;
        VkDependencyInfo di{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        di.bufferMemoryBarrierCount = 1; di.pBufferMemoryBarriers = &pre;
        vkCmdPipelineBarrier2(c, &di);

        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_aePipe);
        // TAA on: meter the TAA RESOLVE output (what the composite will show)
        // instead of the raw jittered HDR scene (alternate pre-written set).
        VkDescriptorSet aeSet = m_taaActiveThisFrame ? m_aeSetTaa : m_aeSet;
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_aeLayout,
                                0, 1, &aeSet, 0, nullptr);
        vkCmdPushConstants(c, m_aeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(AePush), &m_aePush);
        vkCmdDispatch(c, 1, 1, 1);

        VkBufferMemoryBarrier2 post = pre;
        post.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        post.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        post.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        post.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        VkDependencyInfo dp{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dp.bufferMemoryBarrierCount = 1; dp.pBufferMemoryBarriers = &post;
        vkCmdPipelineBarrier2(c, &dp);
    }

    void destroyRt() {
        if (!m_dev.device) { m_rt.shutdown(); return; }
        destroyRtaoTargets();
        if (m_rtaoApplyPipe) { vkDestroyPipeline(m_dev.device, m_rtaoApplyPipe, nullptr); m_rtaoApplyPipe = VK_NULL_HANDLE; }
        if (m_rtaoPipe)      { vkDestroyPipeline(m_dev.device, m_rtaoPipe, nullptr); m_rtaoPipe = VK_NULL_HANDLE; }
        if (m_rtaoApplyLayout){ vkDestroyPipelineLayout(m_dev.device, m_rtaoApplyLayout, nullptr); m_rtaoApplyLayout = VK_NULL_HANDLE; }
        if (m_rtaoLayout)    { vkDestroyPipelineLayout(m_dev.device, m_rtaoLayout, nullptr); m_rtaoLayout = VK_NULL_HANDLE; }
        if (m_rtaoPool)      { vkDestroyDescriptorPool(m_dev.device, m_rtaoPool, nullptr); m_rtaoPool = VK_NULL_HANDLE; }
        if (m_rtaoApplySetLayout){ vkDestroyDescriptorSetLayout(m_dev.device, m_rtaoApplySetLayout, nullptr); m_rtaoApplySetLayout = VK_NULL_HANDLE; }
        if (m_rtaoSetLayout) { vkDestroyDescriptorSetLayout(m_dev.device, m_rtaoSetLayout, nullptr); m_rtaoSetLayout = VK_NULL_HANDLE; }
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (m_rtaoUboBuf[i]) { vmaDestroyBuffer(m_alloc, m_rtaoUboBuf[i], m_rtaoUboAlloc[i]); m_rtaoUboBuf[i] = VK_NULL_HANDLE; }
        }
        if (m_rtaoLinearSampler){ vkDestroySampler(m_dev.device, m_rtaoLinearSampler, nullptr); m_rtaoLinearSampler = VK_NULL_HANDLE; }
        if (m_rtaoDepthSampler) { vkDestroySampler(m_dev.device, m_rtaoDepthSampler, nullptr); m_rtaoDepthSampler = VK_NULL_HANDLE; }
        m_rtaoBuilt = false;
        m_rt.shutdown();
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
            if (x3CreateDescriptorPool(&pci, nullptr, &m_giPool) != VK_SUCCESS) {
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
            if (x3vmaCreateBuffer(&ub, &aci, &m_giUboBuf[i], &m_giUboAlloc[i], &info) != VK_SUCCESS) {
                logError("[rhi] gi ubo create failed"); return false;
            }
            m_giUboMapped[i] = info.pMappedData;
            VkBufferCreateInfo tb{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            tb.size = sizeof(GiTemporalUBO); tb.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationInfo tinfo{};
            if (x3vmaCreateBuffer(&tb, &aci, &m_giTempUboBuf[i], &m_giTempUboAlloc[i], &tinfo) != VK_SUCCESS) {
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
        if (x3vmaCreateImage(&dici, &daci, &m_giPrevDepthImg, &m_giPrevDepthAlloc, nullptr) != VK_SUCCESS) {
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

    // =========================================================================
    // ZERO-STUTTER instrumentation (docs/ZERO_STUTTER.md).
    //
    // EVERY pipeline / shader-module / descriptor-pool creation and every VMA
    // buffer/image allocation in this file funnels through the x3Create* /
    // x3vmaCreate* wrappers below so the engine can PROVE nothing expensive is
    // created mid-frame:
    //   * total + per-frame counters feed the spike log + framePacing(),
    //   * any creation AFTER the first frame began counts as LATE and (under
    //     r_strictpso) logs a validation-style "[stutter]" error line,
    //   * pipeline creation goes through the persistent VkPipelineCache
    //     (loaded from disk at boot, saved at shutdown -> warm second boots).
    // Declared recreate boundaries (swapchain resize, editor-UI init, live
    // quality-setting rebuilds) set m_creationBoundary so they are exempt from
    // the strict assert — they are explicit hitch points, never mid-gameplay.
    // =========================================================================
    void noteCreate(const char* what, uint32_t& lateCounter, uint32_t& frameCounter) {
        ++frameCounter;
        if (m_firstFrameBegun && !m_creationBoundary) {
            ++lateCounter;
            if (m_pacing.strictPso)
                logError(std::string("[stutter] ") + what +
                         " created after first frame (frame " + std::to_string(m_totalFrames) +
                         ") — precompile it at boot or inside a declared recreate boundary");
        }
    }

    VkResult x3CreateGraphicsPipelines(uint32_t n, const VkGraphicsPipelineCreateInfo* ci,
                                       const VkAllocationCallbacks* ac, VkPipeline* out) {
        const auto t0 = std::chrono::steady_clock::now();
        VkResult r = vkCreateGraphicsPipelines(m_dev.device, m_pipelineCache, n, ci, ac, out);
        m_psoCreateMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (r == VK_SUCCESS) { m_psoTotal += n; noteCreate("graphics pipeline", m_psoLate, m_psoThisFrame); }
        return r;
    }
    VkResult x3CreateComputePipelines(uint32_t n, const VkComputePipelineCreateInfo* ci,
                                      const VkAllocationCallbacks* ac, VkPipeline* out) {
        const auto t0 = std::chrono::steady_clock::now();
        VkResult r = vkCreateComputePipelines(m_dev.device, m_pipelineCache, n, ci, ac, out);
        m_psoCreateMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (r == VK_SUCCESS) { m_psoTotal += n; noteCreate("compute pipeline", m_psoLate, m_psoThisFrame); }
        return r;
    }
    VkResult x3CreateDescriptorPool(const VkDescriptorPoolCreateInfo* ci,
                                    const VkAllocationCallbacks* ac, VkDescriptorPool* out) {
        VkResult r = vkCreateDescriptorPool(m_dev.device, ci, ac, out);
        if (r == VK_SUCCESS) noteCreate("descriptor pool", m_poolsLate, m_poolsThisFrame);
        return r;
    }
    VkResult x3vmaCreateBuffer(const VkBufferCreateInfo* bci, const VmaAllocationCreateInfo* aci,
                               VkBuffer* buf, VmaAllocation* alloc, VmaAllocationInfo* info) {
        VkResult r = vmaCreateBuffer(m_alloc,bci, aci, buf, alloc, info);
        if (r == VK_SUCCESS) ++m_allocsThisFrame;
        return r;
    }
    VkResult x3vmaCreateImage(const VkImageCreateInfo* ici, const VmaAllocationCreateInfo* aci,
                              VkImage* img, VmaAllocation* alloc, VmaAllocationInfo* info) {
        VkResult r = vmaCreateImage(m_alloc,ici, aci, img, alloc, info);
        if (r == VK_SUCCESS) ++m_allocsThisFrame;
        return r;
    }

    // ---- VkPipelineCache persistence (ZERO-STUTTER step 2) -----------------
    static std::string pipelineCachePath() { return exeDir() + "\\x3pipeline.cache"; }

    // Load the on-disk pipeline cache (if any) and create the VkPipelineCache all
    // x3Create*Pipelines calls feed. A stale/foreign blob (different GPU/driver)
    // is detected via the spec'd 32-byte header and ignored — cold boot compiles
    // everything and the save below replaces the file.
    void createPipelineCache() {
        std::vector<char> blob;
        std::ifstream f(pipelineCachePath(), std::ios::binary | std::ios::ate);
        if (f) {
            size_t sz = (size_t)f.tellg(); f.seekg(0);
            blob.resize(sz);
            if (sz) f.read(blob.data(), sz);
        }
        if (blob.size() >= 32) {
            // VkPipelineCacheHeaderVersionOne: u32 headerSize, u32 headerVersion,
            // u32 vendorID, u32 deviceID, u8 uuid[16].
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(m_dev.physical_device, &props);
            uint32_t vendor = 0, device = 0;
            std::memcpy(&vendor, blob.data() + 8, 4);
            std::memcpy(&device, blob.data() + 12, 4);
            if (vendor != props.vendorID || device != props.deviceID ||
                std::memcmp(blob.data() + 16, props.pipelineCacheUUID, VK_UUID_SIZE) != 0) {
                logInfo("[rhi] pipeline cache: on-disk blob is for a different GPU/driver — ignoring (cold boot)");
                blob.clear();
            }
        } else {
            blob.clear();
        }
        VkPipelineCacheCreateInfo pcc{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
        pcc.initialDataSize = blob.size();
        pcc.pInitialData    = blob.empty() ? nullptr : blob.data();
        if (vkCreatePipelineCache(m_dev.device, &pcc, nullptr, &m_pipelineCache) != VK_SUCCESS) {
            // Defensive: retry without initial data, then give up (cache==NULL is legal).
            pcc.initialDataSize = 0; pcc.pInitialData = nullptr;
            if (vkCreatePipelineCache(m_dev.device, &pcc, nullptr, &m_pipelineCache) != VK_SUCCESS)
                m_pipelineCache = VK_NULL_HANDLE;
            blob.clear();
        }
        m_cacheLoadedBytes = blob.size();
        logInfo("[rhi] pipeline cache: " + (blob.empty()
            ? std::string("COLD (no usable on-disk cache; full compile this boot)")
            : std::string("WARM — loaded ") + std::to_string(blob.size()) + " bytes from " + pipelineCachePath()));
    }

    // Persist the pipeline cache beside the exe (called from shutdown(), after
    // waitIdle and before the device dies). Second boots then compile near-zero.
    void savePipelineCache() {
        if (m_pipelineCache == VK_NULL_HANDLE) return;
        size_t sz = 0;
        if (vkGetPipelineCacheData(m_dev.device, m_pipelineCache, &sz, nullptr) != VK_SUCCESS || sz == 0) return;
        std::vector<char> blob(sz);
        if (vkGetPipelineCacheData(m_dev.device, m_pipelineCache, &sz, blob.data()) != VK_SUCCESS) return;
        std::ofstream f(pipelineCachePath(), std::ios::binary | std::ios::trunc);
        if (!f) { logError("[rhi] pipeline cache: save failed (cannot open " + pipelineCachePath() + ")"); return; }
        f.write(blob.data(), (std::streamsize)sz);
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "[rhi] pipeline cache: saved %zu bytes (this boot compiled %u pipelines in %.1f ms; loaded %llu bytes)",
            sz, m_psoTotal, m_psoCreateMs, (unsigned long long)m_cacheLoadedBytes);
        logInfo(buf);
    }

    // ---- Frame-pacing ring + spike log (ZERO-STUTTER step 3) ---------------
    // Called at the very end of endFrame(): records the endFrame->endFrame CPU
    // wall delta + the latest GPU timestamp time, and logs ONE attribution line
    // for any post-warmup frame above the spike threshold. Warmup frames are not
    // recorded (boot compile / first-bake noise stays out of the percentiles).
    void recordFramePacing() {
        const auto now = std::chrono::steady_clock::now();
        if (!m_paceHaveLast) { m_paceLast = now; m_paceHaveLast = true; return; }
        const float cpuMs = (float)std::chrono::duration<double, std::milli>(now - m_paceLast).count();
        m_paceLast = now;
        if (m_totalFrames <= (uint64_t)m_pacing.warmupFrames) return;   // warmup: excluded
        // Rolling median over the last <=128 recorded samples (the spike gate).
        float median = 0.0f;
        if (!m_paceRing.empty()) {
            const size_t n = std::min<size_t>(m_paceRing.size(), 128);
            float tmp[128];
            for (size_t i = 0; i < n; ++i)
                tmp[i] = m_paceRing[(m_paceWrite + (uint32_t)m_paceRing.size() - 1 - (uint32_t)i) % (uint32_t)m_paceRing.size()].cpuMs;
            std::nth_element(tmp, tmp + n / 2, tmp + n);
            median = tmp[n / 2];
        }
        if (m_paceRing.size() < kPaceRingCap) {
            m_paceRing.push_back({ cpuMs, m_lastGpuMs });
            m_paceWrite = (uint32_t)(m_paceRing.size() % kPaceRingCap);
        } else {
            m_paceRing[m_paceWrite] = { cpuMs, m_lastGpuMs };
            m_paceWrite = (m_paceWrite + 1) % kPaceRingCap;
        }
        // Spike: above BOTH the relative (spikeFactor x rolling median) and the
        // absolute (median + floorMs) thresholds. One attribution line per spike.
        if (median > 0.0f &&
            cpuMs > median * m_pacing.spikeFactor &&
            cpuMs > median + m_pacing.floorMs) {
            ++m_spikeCount;
            // Attribution: a spike with a known cause (AS rebuild on scene change,
            // streaming upload, IBL re-bake, resize) is a declared boundary; a
            // spike with NO cause is an unexplained pacing failure (the gate).
            const bool attributed = m_psoThisFrame || m_modulesThisFrame || m_poolsThisFrame ||
                                    m_allocsThisFrame || m_asBuildsThisFrame ||
                                    m_iblBakedThisFrame || m_recreatedThisFrame;
            if (!attributed) ++m_spikeCleanCount;
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "[pacing] SPIKE frame=%llu cpu=%.2fms (median %.2f) gpu=%.2fms | pso+%u mod+%u pools+%u allocs+%u asbuild+%u%s%s",
                (unsigned long long)m_totalFrames, cpuMs, median, m_lastGpuMs,
                m_psoThisFrame, m_modulesThisFrame, m_poolsThisFrame, m_allocsThisFrame,
                m_asBuildsThisFrame,
                m_iblBakedThisFrame ? " +iblbake" : "",
                m_recreatedThisFrame ? " +recreate" : "");
            logInfo(buf);
        }
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
        else
            noteCreate("shader module", m_modulesLate, m_modulesThisFrame);
        return m;
    }

    // Raw SPIR-V words off disk (GpuCullSystem creates its own modules).
    std::vector<uint32_t> loadSpvWords(const std::string& relPath) {
        std::string full = exeDir() + "\\" + relPath;
        std::ifstream f(full, std::ios::binary | std::ios::ate);
        if (!f) { logError(std::string("[rhi] shader not found: ") + full); return {}; }
        size_t sz = (size_t)f.tellg(); f.seekg(0);
        std::vector<uint32_t> words((sz + 3) / 4, 0u);
        f.read(reinterpret_cast<char*>(words.data()), sz);
        return words;
    }

    // ---- D15 GPU-driven culling bring-up -----------------------------------
    // Caps detect + cull/HZB pipelines + the per-frame cull descriptor sets
    // (bindings 0..5 of cull.comp). Non-fatal: on any failure m_gpuCullReady
    // stays false and r_cullpath >= 1 silently falls back to the CPU path.
    bool createGpuCull() {
        m_cullCaps = detectCullCaps(m_dev.physical_device, -1);

        std::vector<uint32_t> cullSpv = loadSpvWords("shaders\\cull.comp.spv");
        std::vector<uint32_t> hzbSpv  = loadSpvWords("shaders\\hzb_build.comp.spv");
        if (cullSpv.empty() || hzbSpv.empty()) return false;
        if (!m_gpuCull.init(m_dev.device, m_cullCaps, cullSpv, hzbSpv,
                            /*buildHzb=*/true, /*reversedZ=*/false)) // X3 = STANDARD Z (verified)
            return false;

        // HZB pyramid sampler (also bound as the harmless dummy when HZB is off):
        // NEAREST + clamp + nearest-mip with the full LOD range, as the cull's
        // textureLod(level) expects.
        {
            VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            sci.magFilter = VK_FILTER_NEAREST; sci.minFilter = VK_FILTER_NEAREST;
            sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            sci.addressModeU = sci.addressModeV = sci.addressModeW =
                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sci.maxLod = VK_LOD_CLAMP_NONE;
            if (vkCreateSampler(m_dev.device, &sci, nullptr, &m_hzbSampler) != VK_SUCCESS)
                return false;
        }

        // Per-frame cull descriptor sets.
        {
            VkDescriptorPoolSize sizes[3]{};
            sizes[0] = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * kFramesInFlight };
            sizes[1] = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight };
            sizes[2] = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight };
            VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.maxSets = kFramesInFlight; pci.poolSizeCount = 3; pci.pPoolSizes = sizes;
            if (vkCreateDescriptorPool(m_dev.device, &pci, nullptr, &m_cullPool) != VK_SUCCESS)
                return false;

            VkDescriptorSetLayout dsl = m_gpuCull.cullSetLayout();
            for (uint32_t i = 0; i < kFramesInFlight; ++i) {
                auto& fr = m_frames[i];
                VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
                dsai.descriptorPool = m_cullPool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &dsl;
                if (vkAllocateDescriptorSets(m_dev.device, &dsai, &fr.cullSet) != VK_SUCCESS)
                    return false;
                VkDescriptorBufferInfo bInst { fr.cullInstBuf,  0, VK_WHOLE_SIZE };
                VkDescriptorBufferInfo bCmds { fr.indirectBuf,  0, VK_WHOLE_SIZE };
                VkDescriptorBufferInfo bVis  { fr.visBuf,       0, VK_WHOLE_SIZE };
                VkDescriptorBufferInfo bStat { fr.cullStatsBuf, 0, VK_WHOLE_SIZE };
                VkDescriptorBufferInfo bPar  { fr.cullParamsBuf,0, sizeof(CullParamsGpu) };
                // Binding 5: the HZB pyramid once it exists; until then the 1x1
                // white default (never sampled with USE_HZB=0 — just keeps the
                // descriptor valid).
                VkDescriptorImageInfo iHzb{ m_hzbSampler, m_whiteTex.view,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                VkWriteDescriptorSet w[6]{};
                auto wb = [&](uint32_t bind, VkDescriptorType t, const VkDescriptorBufferInfo* bi) {
                    w[bind].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    w[bind].dstSet = fr.cullSet; w[bind].dstBinding = bind;
                    w[bind].descriptorCount = 1; w[bind].descriptorType = t;
                    w[bind].pBufferInfo = bi;
                };
                wb(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bInst);
                wb(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bCmds);
                wb(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bVis);
                wb(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bStat);
                wb(4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &bPar);
                w[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[5].dstSet = fr.cullSet; w[5].dstBinding = 5; w[5].descriptorCount = 1;
                w[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[5].pImageInfo = &iHzb;
                vkUpdateDescriptorSets(m_dev.device, 6, w, 0, nullptr);
            }
        }
        // ---- Tier 1 (async compute) objects: timeline semaphore + per-frame
        // command pools on the dedicated compute family. Optional.
        if (m_computeQueue) {
            VkSemaphoreTypeCreateInfo tci{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
            tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            tci.initialValue = 0;
            VkSemaphoreCreateInfo sci2{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &tci };
            bool ok = vkCreateSemaphore(m_dev.device, &sci2, nullptr, &m_cullTimeline) == VK_SUCCESS;
            for (uint32_t i = 0; ok && i < kFramesInFlight; ++i) {
                auto& fr = m_frames[i];
                VkCommandPoolCreateInfo pci2{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
                pci2.queueFamilyIndex = m_computeFamily;
                ok = vkCreateCommandPool(m_dev.device, &pci2, nullptr, &fr.cullPool) == VK_SUCCESS;
                if (ok) {
                    VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
                    cai.commandPool = fr.cullPool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                    cai.commandBufferCount = 1;
                    ok = vkAllocateCommandBuffers(m_dev.device, &cai, &fr.cullCmd) == VK_SUCCESS;
                }
            }
            m_asyncCullReady = ok;
            logInfo(m_asyncCullReady
                ? "[cull] Tier 1 async-compute path READY (timeline semaphore + compute pools)"
                : "[cull] Tier 1 setup failed — async path disabled (Tier 0 still available)");
        }

        logInfo(std::string("[cull] D15 GPU cull ready (caps tier ") +
                std::to_string((int)m_cullCaps.tier) + ", r_cullpath gates the path)");
        return true;
    }

    // ---- D15 stage 2: HZB depth pyramid (extent-tracking) -------------------
    // Mip 0 = half the render extent; full chain down to 1x1. Image lives in
    // GENERAL forever (transitioned once here); the per-frame write->read flips
    // are sync2 barriers inside recordHzbBuild. Rebuilt on resize (depth view
    // changes); marks last-frame depth invalid so the first post-resize frame
    // culls frustum-only.
    bool createHzbTargets() {
        destroyHzbTargets();
        if (!m_gpuCullReady) return false;
        m_hzbW = std::max(1u, m_extent.width  / 2u);
        m_hzbH = std::max(1u, m_extent.height / 2u);
        uint32_t mips = 1; { uint32_t w = std::max(m_hzbW, m_hzbH); while (w > 1) { w /= 2; ++mips; } }
        m_hzbMipCount = std::min(mips, kHzbMaxMips);

        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R32_SFLOAT;
        ici.extent = { m_hzbW, m_hzbH, 1 };
        ici.mipLevels = m_hzbMipCount; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO;
        if (vmaCreateImage(m_alloc, &ici, &aci, &m_hzbImg, &m_hzbAlloc, nullptr) != VK_SUCCESS) {
            logError("[cull] hzb pyramid create failed"); return false;
        }
        auto makeView = [&](uint32_t baseMip, uint32_t mipCount, VkImageView& out) {
            VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            vci.image = m_hzbImg; vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = VK_FORMAT_R32_SFLOAT;
            vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mipCount, 0, 1 };
            return vkCreateImageView(m_dev.device, &vci, nullptr, &out) == VK_SUCCESS;
        };
        if (!makeView(0, m_hzbMipCount, m_hzbViewAll)) return false;
        for (uint32_t m = 0; m < m_hzbMipCount; ++m)
            if (!makeView(m, 1, m_hzbMipView[m])) return false;

        // One-time UNDEFINED -> GENERAL for the whole chain.
        bool ok = oneTimeSubmit([&](VkCommandBuffer cmd){
            VkImageMemoryBarrier2 ib{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            ib.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT; ib.srcAccessMask = 0;
            ib.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            ib.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            ib.image = m_hzbImg;
            ib.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, m_hzbMipCount, 0, 1 };
            VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dep.imageMemoryBarrierCount = 1; dep.pImageMemoryBarriers = &ib;
            vkCmdPipelineBarrier2(cmd, &dep);
        });
        if (!ok) { logError("[cull] hzb layout init failed"); return false; }

        // Per-mip reduce sets: binding0 = src sampler (depth for mip 0, the
        // previous pyramid mip otherwise), binding1 = this mip as storage image.
        if (!m_hzbPool) {
            VkDescriptorPoolSize sizes[2]{};
            sizes[0] = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kHzbMaxMips };
            sizes[1] = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kHzbMaxMips };
            VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.maxSets = kHzbMaxMips; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
            if (vkCreateDescriptorPool(m_dev.device, &pci, nullptr, &m_hzbPool) != VK_SUCCESS)
                return false;
        } else {
            vkResetDescriptorPool(m_dev.device, m_hzbPool, 0);
        }
        VkDescriptorSetLayout hdsl = m_gpuCull.hzbSetLayout();
        for (uint32_t m = 0; m < m_hzbMipCount; ++m) {
            VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dsai.descriptorPool = m_hzbPool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &hdsl;
            if (vkAllocateDescriptorSets(m_dev.device, &dsai, &m_hzbMipSet[m]) != VK_SUCCESS)
                return false;
            VkDescriptorImageInfo src{};
            src.sampler = m_hzbSampler;
            if (m == 0) { src.imageView = m_depthView; src.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL; }
            else        { src.imageView = m_hzbMipView[m - 1]; src.imageLayout = VK_IMAGE_LAYOUT_GENERAL; }
            VkDescriptorImageInfo dst{ VK_NULL_HANDLE, m_hzbMipView[m], VK_IMAGE_LAYOUT_GENERAL };
            VkWriteDescriptorSet w[2]{};
            w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[0].dstSet = m_hzbMipSet[m]; w[0].dstBinding = 0; w[0].descriptorCount = 1;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &src;
            w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[1].dstSet = m_hzbMipSet[m]; w[1].dstBinding = 1; w[1].descriptorCount = 1;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[1].pImageInfo = &dst;
            vkUpdateDescriptorSets(m_dev.device, 2, w, 0, nullptr);
        }

        // Point every frame's cull set (binding 5) at the live pyramid.
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (!m_frames[i].cullSet) continue;
            VkDescriptorImageInfo iHzb{ m_hzbSampler, m_hzbViewAll, VK_IMAGE_LAYOUT_GENERAL };
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet = m_frames[i].cullSet; w.dstBinding = 5; w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &iHzb;
            vkUpdateDescriptorSets(m_dev.device, 1, &w, 0, nullptr);
        }

        m_depthValid = false;          // depth content is fresh/unrendered
        m_hzbReady = true;
        logInfo("[cull] HZB pyramid ready: " + std::to_string(m_hzbW) + "x" +
                std::to_string(m_hzbH) + " mips=" + std::to_string(m_hzbMipCount));
        return true;
    }

    void destroyHzbTargets() {
        m_hzbReady = false; m_depthValid = false;
        if (m_hzbViewAll) { vkDestroyImageView(m_dev.device, m_hzbViewAll, nullptr); m_hzbViewAll = VK_NULL_HANDLE; }
        for (uint32_t m = 0; m < kHzbMaxMips; ++m) {
            if (m_hzbMipView[m]) { vkDestroyImageView(m_dev.device, m_hzbMipView[m], nullptr); m_hzbMipView[m] = VK_NULL_HANDLE; }
            m_hzbMipSet[m] = VK_NULL_HANDLE;   // pool reset/destroy reclaims them
        }
        if (m_hzbImg) { vmaDestroyImage(m_alloc, m_hzbImg, m_hzbAlloc); m_hzbImg = VK_NULL_HANDLE; m_hzbAlloc = nullptr; }
        m_hzbMipCount = 0; m_hzbW = m_hzbH = 0;
    }

    void destroyGpuCull() {
        destroyHzbTargets();
        if (m_cullTimeline) { vkDestroySemaphore(m_dev.device, m_cullTimeline, nullptr); m_cullTimeline = VK_NULL_HANDLE; }
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            auto& fr = m_frames[i];
            if (fr.cullPool) { vkDestroyCommandPool(m_dev.device, fr.cullPool, nullptr); fr.cullPool = VK_NULL_HANDLE; fr.cullCmd = VK_NULL_HANDLE; }
        }
        m_asyncCullReady = false;
        if (m_hzbPool)    { vkDestroyDescriptorPool(m_dev.device, m_hzbPool, nullptr); m_hzbPool = VK_NULL_HANDLE; }
        if (m_cullPool)   { vkDestroyDescriptorPool(m_dev.device, m_cullPool, nullptr); m_cullPool = VK_NULL_HANDLE; }
        if (m_hzbSampler) { vkDestroySampler(m_dev.device, m_hzbSampler, nullptr); m_hzbSampler = VK_NULL_HANDLE; }
        m_gpuCull.shutdown(m_dev.device);
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            auto& fr = m_frames[i];
            if (fr.cullInstBuf)   { vmaDestroyBuffer(m_alloc, fr.cullInstBuf, fr.cullInstAlloc); fr.cullInstBuf = VK_NULL_HANDLE; fr.cullInstAlloc = nullptr; fr.cullInstMapped = nullptr; }
            if (fr.visBuf)        { vmaDestroyBuffer(m_alloc, fr.visBuf, fr.visAlloc); fr.visBuf = VK_NULL_HANDLE; fr.visAlloc = nullptr; fr.visMapped = nullptr; }
            if (fr.cullStatsBuf)  { vmaDestroyBuffer(m_alloc, fr.cullStatsBuf, fr.cullStatsAlloc); fr.cullStatsBuf = VK_NULL_HANDLE; fr.cullStatsAlloc = nullptr; fr.cullStatsMapped = nullptr; }
            if (fr.cullParamsBuf) { vmaDestroyBuffer(m_alloc, fr.cullParamsBuf, fr.cullParamsAlloc); fr.cullParamsBuf = VK_NULL_HANDLE; fr.cullParamsAlloc = nullptr; fr.cullParamsMapped = nullptr; }
            fr.cullSet = VK_NULL_HANDLE;
        }
        m_gpuCullReady = false;
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
        if (x3vmaCreateBuffer(&sbci, &svaci, &staging, &stagingAlloc, &si) != VK_SUCCESS) return false;
        std::memcpy(si.pMappedData, data, (size_t)bytes);

        // Device-local destination.
        VkBufferCreateInfo dbci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        dbci.size = bytes; dbci.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo dvaci{};
        dvaci.usage = VMA_MEMORY_USAGE_AUTO;
        if (x3vmaCreateBuffer(&dbci, &dvaci, &outBuf, &outAlloc, nullptr) != VK_SUCCESS) {
            vmaDestroyBuffer(m_alloc, staging, stagingAlloc); return false;
        }

        // BOOT-TIME upload batching: while a batch window is active, record the
        // copy into the shared batch command buffer (one submit for the whole
        // batch) instead of a blocking per-buffer submit + fence wait. The staging
        // buffer stays alive until the flush. Semantics are identical: anything
        // that could consume the data (beginFrame / any one-shot op) flushes first.
        if (m_batchActive) {
            std::lock_guard<std::recursive_mutex> lk(m_uploadMu);  // shared batch cmd
            VkCommandBuffer cmd = batchCmd();
            if (cmd) {
                VkBufferCopy region{ 0, 0, bytes };
                vkCmdCopyBuffer(cmd, staging, outBuf, 1, &region);
                m_batchStagings.emplace_back(staging, stagingAlloc);
                ++m_batchOps;
                return true;
            }
            // batch cmd alloc failed -> fall through to the blocking path
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
        if (x3vmaCreateBuffer(&sbci, &svaci, &staging, &stagingAlloc, &si) != VK_SUCCESS) return false;
        std::memcpy(si.pMappedData, rgba8, (size_t)bytes);

        // Full mip chain so minified/distant surfaces aren't aliased and (with aniso) not
        // blurry. mipLevels = floor(log2(max dim)) + 1. The image needs TRANSFER_SRC too so
        // each level can be blit-downscaled from the previous one.
        const uint32_t mipLevels = (uint32_t)std::floor(std::log2((float)std::max(w, h))) + 1u;

        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = fmt;
        ici.extent = { w, h, 1 };
        ici.mipLevels = mipLevels; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo ivaci{}; ivaci.usage = VMA_MEMORY_USAGE_AUTO;
        if (x3vmaCreateImage(&ici, &ivaci, &out.image, &out.alloc, nullptr) != VK_SUCCESS) {
            vmaDestroyBuffer(m_alloc, staging, stagingAlloc); return false;
        }

        auto recordTexUpload = [&, staging](VkCommandBuffer cmd){
            // Per-mip sync2 layout barrier helper.
            auto barrierMip = [&](uint32_t mip, VkImageLayout oldL, VkImageLayout newL,
                                  VkPipelineStageFlags2 ss, VkAccessFlags2 sa,
                                  VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
                VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
                b.srcStageMask = ss; b.srcAccessMask = sa; b.dstStageMask = ds; b.dstAccessMask = da;
                b.oldLayout = oldL; b.newLayout = newL;
                b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = out.image;
                b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1 };
                VkDependencyInfo di{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b;
                vkCmdPipelineBarrier2(cmd, &di);
            };
            // mip 0: UNDEFINED -> TRANSFER_DST, then copy the uploaded RGBA8.
            barrierMip(0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                       VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { w, h, 1 };
            vkCmdCopyBufferToImage(cmd, staging, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            // Generate each successive mip by a 2x linear downscale blit from the previous.
            int32_t mw = (int32_t)w, mh = (int32_t)h;
            for (uint32_t i = 1; i < mipLevels; ++i) {
                barrierMip(i - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                barrierMip(i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                           VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                const int32_t nw = mw > 1 ? mw / 2 : 1, nh = mh > 1 ? mh / 2 : 1;
                VkImageBlit blit{};
                blit.srcOffsets[1] = { mw, mh, 1 };
                blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1 };
                blit.dstOffsets[1] = { nw, nh, 1 };
                blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1 };
                vkCmdBlitImage(cmd, out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
                barrierMip(i - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                mw = nw; mh = nh;
            }
            // Last mip is still TRANSFER_DST -> SHADER_READ.
            barrierMip(mipLevels - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        };
        // BOOT-TIME upload batching: record the copy + mip-blit chain into the
        // shared batch command buffer (one submit per batch) when a batch window is
        // active; the staging buffer stays alive until the flush. Identical command
        // stream, just deferred to a single submit.
        bool ok;
        if (m_batchActive) {
            std::lock_guard<std::recursive_mutex> lk(m_uploadMu);  // shared batch cmd
            VkCommandBuffer cmd = batchCmd();
            if (cmd) {
                recordTexUpload(cmd);
                m_batchStagings.emplace_back(staging, stagingAlloc);
                ++m_batchOps;
                ok = true;
                staging = VK_NULL_HANDLE; stagingAlloc = nullptr;   // ownership moved to the batch
            } else {
                ok = oneTimeSubmit(recordTexUpload);
            }
        } else {
            ok = oneTimeSubmit(recordTexUpload);
        }
        if (staging) vmaDestroyBuffer(m_alloc, staging, stagingAlloc);
        if (!ok) { vmaDestroyImage(m_alloc, out.image, out.alloc); out.image = VK_NULL_HANDLE; out.alloc = nullptr; return false; }

        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = out.image; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 1 };
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
        sci.anisotropyEnable = VK_TRUE;   // sharpen grazing-angle surfaces (feature enabled at device init)
        sci.maxAnisotropy = 8.0f;         // well under the RTX limit (16)
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

    // ---- BOOT-TIME upload batching (docs/BOOT_TIME.md) ----------------------
    // Begin (or continue) the shared batch command buffer — DOUBLE-BUFFERED so a
    // new batch can record while the previous submit is still executing (the CPU
    // only blocks if BOTH slots are in flight). Returns VK_NULL_HANDLE only if
    // allocation fails (callers then fall back to oneTimeSubmit).
    VkCommandBuffer batchCmd() {
        if (m_batchOpen) return m_batchCmds[m_batchSlot];
        const uint32_t s = m_batchSlot;
        retireBatchSlot(s, /*blocking=*/true);   // this slot may still be in flight
        if (!m_batchCmds[s]) {
            VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            ai.commandPool = m_uploadPool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(m_dev.device, &ai, &m_batchCmds[s]) != VK_SUCCESS) {
                m_batchCmds[s] = VK_NULL_HANDLE;
                return VK_NULL_HANDLE;
            }
        }
        if (!m_batchFences[s]) {
            VkFenceCreateInfo fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            if (vkCreateFence(m_dev.device, &fi, nullptr, &m_batchFences[s]) != VK_SUCCESS) {
                m_batchFences[s] = VK_NULL_HANDLE;
                return VK_NULL_HANDLE;
            }
        }
        VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(m_batchCmds[s], &bi) != VK_SUCCESS) return VK_NULL_HANDLE;
        m_batchOpen = true;
        return m_batchCmds[s];
    }

    // Retire one submitted batch slot: wait its fence (or skip if not signaled and
    // non-blocking), free its staging buffers, reset its command buffer.
    void retireBatchSlot(uint32_t s, bool blocking) {
        if (!m_batchSubmittedSlot[s]) return;
        if (!blocking &&
            vkGetFenceStatus(m_dev.device, m_batchFences[s]) == VK_NOT_READY) return;
        vkWaitForFences(m_dev.device, 1, &m_batchFences[s], VK_TRUE, UINT64_MAX);
        vkResetFences(m_dev.device, 1, &m_batchFences[s]);
        vkResetCommandBuffer(m_batchCmds[s], 0);
        for (auto& st : m_batchInFlightSlot[s]) vmaDestroyBuffer(m_alloc, st.first, st.second);
        m_batchInFlightSlot[s].clear();
        m_batchSubmittedSlot[s] = false;
    }

    // SUBMIT every recorded batched upload in ONE submit — WITHOUT waiting. The
    // graphics queue executes in submission order, so any later frame/one-shot
    // submission sees the uploads complete on the GPU timeline; a trailing global
    // TRANSFER -> ALL_COMMANDS barrier makes the writes visible (the texture path
    // already ends in per-mip barriers). The staging buffers + fence stay pending
    // until waitUploadBatch() (forced before the fence/cmd are reused; opportunistic
    // non-blocking retire in beginFrame).
    void submitUploadBatch() {
        std::lock_guard<std::recursive_mutex> lk(m_uploadMu);   // parallel preload safe
        if (!m_batchOpen) return;
        const auto t0 = std::chrono::steady_clock::now();
        const uint32_t s = m_batchSlot;
        // Visibility for the buffer copies (vertex/index/SSBO reads downstream).
        VkMemoryBarrier2 mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        mb.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        VkDependencyInfo di{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(m_batchCmds[s], &di);
        vkEndCommandBuffer(m_batchCmds[s]);
        VkCommandBufferSubmitInfo cmdS{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
        cmdS.commandBuffer = m_batchCmds[s];
        VkSubmitInfo2 submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        submit.commandBufferInfoCount = 1; submit.pCommandBufferInfos = &cmdS;
        if (vkQueueSubmit2(m_gfxQueue, 1, &submit, m_batchFences[s]) == VK_SUCCESS) {
            m_batchSubmittedSlot[s] = true;
            m_batchInFlightSlot[s].swap(m_batchStagings);
            m_batchSlot = s ^ 1u;            // record the next batch in the other slot
        } else {
            logError("[rhi] upload batch: submit failed (uploads lost this batch)");
            for (auto& st : m_batchStagings) vmaDestroyBuffer(m_alloc, st.first, st.second);
        }
        m_batchOpen = false;
        m_batchStagings.clear();
        ++m_batchFlushes;
        m_batchMs += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
    }

    // Retire previously submitted batches: wait their fences (blocking=true) or
    // only those already signaled, freeing staging buffers + resetting cmds.
    void waitUploadBatch(bool blocking = true) {
        std::lock_guard<std::recursive_mutex> lk(m_uploadMu);
        retireBatchSlot(0, blocking);
        retireBatchSlot(1, blocking);
    }

    // Submit + fully retire (the original blocking semantics). Used by anything
    // that reuses the fence/cmd next (oneTimeSubmit) or tears down (shutdown).
    void flushUploadBatch() {
        std::lock_guard<std::recursive_mutex> lk(m_uploadMu);
        submitUploadBatch();
        waitUploadBatch(true);
    }

    void beginUploadBatch() override {
        if (m_batchActive) return;       // nestable-safe
        m_batchActive = true;
        m_batchOps = 0; m_batchFlushes = 0; m_batchMs = 0.0;
    }

    void endUploadBatch() override {
        if (!m_batchActive) return;
        submitUploadBatch();    // no CPU wait — the GPU finishes while boot continues
        m_batchActive = false;
        if (m_batchOps) {
            char b[160];
            std::snprintf(b, sizeof(b),
                "[rhi] upload batch: %u uploads in %u flush(es), %.1f ms total flush wait",
                m_batchOps, m_batchFlushes, m_batchMs);
            logInfo(b);
        }
    }

    // Record + submit a transient command buffer, wait on a one-shot fence.
    template <class Fn>
    bool oneTimeSubmit(Fn&& record) {
        std::lock_guard<std::recursive_mutex> lk(m_uploadMu);   // parallel preload safe
        // Ordering safety: any one-shot GPU op (BLAS build, readback, IBL bake…)
        // may depend on batched uploads — SUBMIT those first (no CPU wait needed:
        // same-queue submission order + the batch's trailing barrier make the
        // uploads land before this submit executes; this op's own fence wait
        // transitively covers them).
        submitUploadBatch();
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
            if (x3CreateDescriptorPool(&pci, nullptr, &m_bindlessPool) != VK_SUCCESS) {
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
            VkDescriptorSetLayoutBinding binds[3]{};
            binds[0].binding = 0; binds[0].descriptorCount = 1;
            binds[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            binds[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            binds[1].binding = 1; binds[1].descriptorCount = 1;
            binds[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            // VERTEX: camera viewProj (mesh.vert) + lightViewProj (shadow.vert).
            // FRAGMENT: lightViewProj for the per-pixel shadow projection (mesh.frag).
            binds[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            // D15 GPU cull: binding 2 = the visible-instance indirection SSBO
            // (identity when the cull is off; cull.comp's compaction when on).
            // All four objects[] vertex shaders read it.
            binds[2].binding = 2; binds[2].descriptorCount = 1;
            binds[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            binds[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            slci.bindingCount = 3; slci.pBindings = binds;
            if (vkCreateDescriptorSetLayout(m_dev.device, &slci, nullptr, &m_objSetLayout) != VK_SUCCESS) {
                logError("[rhi] object set layout failed"); return false;
            }

            VkDescriptorPoolSize sizes[2]{};
            sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; sizes[0].descriptorCount = 2 * kFramesInFlight;
            sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; sizes[1].descriptorCount = kFramesInFlight;
            VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.maxSets = kFramesInFlight; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
            if (x3CreateDescriptorPool(&pci, nullptr, &m_objPool) != VK_SUCCESS) {
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
                if (x3vmaCreateBuffer(&bci, &aci, &fr.objBuf, &fr.objAlloc, &info) != VK_SUCCESS) {
                    logError("[rhi] object SSBO create failed"); return false;
                }
                fr.objMapped = info.pMappedData;

                // Frame UBO (camera viewProj + sun lightViewProj + point lights).
                VkBufferCreateInfo cbci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                cbci.size = sizeof(FrameUBO);
                cbci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
                VmaAllocationInfo cinfo{};
                if (x3vmaCreateBuffer(&cbci, &aci, &fr.camBuf, &fr.camAlloc, &cinfo) != VK_SUCCESS) {
                    logError("[rhi] camera UBO create failed"); return false;
                }
                fr.camMapped = cinfo.pMappedData;

                // Indirect-command buffer (one VkDrawIndexedIndirectCommand per
                // distinct mesh; capped at kMaxTextures meshes which is plenty).
                // STORAGE usage added for D15: cull.comp atomically bumps each
                // command's instanceCount in place (binding 1).
                VkBufferCreateInfo ibci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                ibci.size = (VkDeviceSize)kMaxDrawMeshes * sizeof(VkDrawIndexedIndirectCommand);
                ibci.usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                // Tier 1: the dedicated compute queue writes this buffer while the
                // graphics queue reads it -> CONCURRENT sharing across the two
                // families (simpler than ownership transfers; revisit if profiled).
                const uint32_t cullFamilies[2] = { m_gfxFamily, m_computeFamily };
                if (m_computeQueue) {
                    ibci.sharingMode = VK_SHARING_MODE_CONCURRENT;
                    ibci.queueFamilyIndexCount = 2;
                    ibci.pQueueFamilyIndices = cullFamilies;
                }
                VmaAllocationInfo iinfo{};
                if (x3vmaCreateBuffer(&ibci, &aci, &fr.indirectBuf, &fr.indirectAlloc, &iinfo) != VK_SUCCESS) {
                    logError("[rhi] indirect buffer create failed"); return false;
                }
                fr.indirectMapped = iinfo.pMappedData;

                // ---- D15 GPU cull per-frame ring ---------------------------
                // visible-instance indirection (vertex shaders read binding 2;
                // cull.comp writes binding 2 of the CULL set). Identity-filled so
                // the CPU path is byte-identical to pre-D15 behavior.
                {
                    VkBufferCreateInfo vbci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                    vbci.size = (VkDeviceSize)kMaxDrawsPerFrame * sizeof(uint32_t);
                    vbci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                    if (m_computeQueue) {
                        vbci.sharingMode = VK_SHARING_MODE_CONCURRENT;
                        vbci.queueFamilyIndexCount = 2;
                        vbci.pQueueFamilyIndices = cullFamilies;
                    }
                    VmaAllocationInfo vinfo{};
                    if (vmaCreateBuffer(m_alloc, &vbci, &aci, &fr.visBuf, &fr.visAlloc, &vinfo) != VK_SUCCESS) {
                        logError("[rhi] visible-instance buffer create failed"); return false;
                    }
                    fr.visMapped = vinfo.pMappedData;
                    uint32_t* ids = static_cast<uint32_t*>(fr.visMapped);
                    for (uint32_t k = 0; k < kMaxDrawsPerFrame; ++k) ids[k] = k;

                    VkBufferCreateInfo cbci2{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                    cbci2.size = (VkDeviceSize)kMaxDrawsPerFrame * sizeof(CullInstanceGpu);
                    cbci2.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                    if (m_computeQueue) {
                        cbci2.sharingMode = VK_SHARING_MODE_CONCURRENT;
                        cbci2.queueFamilyIndexCount = 2;
                        cbci2.pQueueFamilyIndices = cullFamilies;
                    }
                    VmaAllocationInfo ciinfo{};
                    if (vmaCreateBuffer(m_alloc, &cbci2, &aci, &fr.cullInstBuf, &fr.cullInstAlloc, &ciinfo) != VK_SUCCESS) {
                        logError("[rhi] cull instance buffer create failed"); return false;
                    }
                    fr.cullInstMapped = ciinfo.pMappedData;

                    // Stats: GPU-written, host-READ on slot reuse -> RANDOM access
                    // (cached) memory so the readback isn't a WC-memory read.
                    VkBufferCreateInfo stbci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                    stbci.size = sizeof(CullStatsGpu);
                    stbci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                    if (m_computeQueue) {
                        stbci.sharingMode = VK_SHARING_MODE_CONCURRENT;
                        stbci.queueFamilyIndexCount = 2;
                        stbci.pQueueFamilyIndices = cullFamilies;
                    }
                    VmaAllocationCreateInfo staci{};
                    staci.usage = VMA_MEMORY_USAGE_AUTO;
                    staci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
                    VmaAllocationInfo stinfo{};
                    if (vmaCreateBuffer(m_alloc, &stbci, &staci, &fr.cullStatsBuf, &fr.cullStatsAlloc, &stinfo) != VK_SUCCESS) {
                        logError("[rhi] cull stats buffer create failed"); return false;
                    }
                    fr.cullStatsMapped = stinfo.pMappedData;
                    std::memset(fr.cullStatsMapped, 0, sizeof(CullStatsGpu));

                    VkBufferCreateInfo pbci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                    pbci.size = sizeof(CullParamsGpu);
                    pbci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
                    if (m_computeQueue) {
                        pbci.sharingMode = VK_SHARING_MODE_CONCURRENT;
                        pbci.queueFamilyIndexCount = 2;
                        pbci.pQueueFamilyIndices = cullFamilies;
                    }
                    VmaAllocationInfo pinfo{};
                    if (vmaCreateBuffer(m_alloc, &pbci, &aci, &fr.cullParamsBuf, &fr.cullParamsAlloc, &pinfo) != VK_SUCCESS) {
                        logError("[rhi] cull params buffer create failed"); return false;
                    }
                    fr.cullParamsMapped = pinfo.pMappedData;
                }

                // Allocate + write the set-1 descriptor (points at this frame's
                // SSBO + camera UBO + visible-index SSBO; written once, buffers
                // are persistent-mapped).
                VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
                dsai.descriptorPool = m_objPool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &m_objSetLayout;
                if (vkAllocateDescriptorSets(m_dev.device, &dsai, &fr.objSet) != VK_SUCCESS) {
                    logError("[rhi] object set alloc failed"); return false;
                }
                VkDescriptorBufferInfo sbi{ fr.objBuf, 0, VK_WHOLE_SIZE };
                VkDescriptorBufferInfo cbi{ fr.camBuf, 0, sizeof(FrameUBO) };
                VkDescriptorBufferInfo vbi{ fr.visBuf, 0, VK_WHOLE_SIZE };
                VkWriteDescriptorSet writes[3]{};
                writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[0].dstSet = fr.objSet; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
                writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[0].pBufferInfo = &sbi;
                writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[1].dstSet = fr.objSet; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
                writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; writes[1].pBufferInfo = &cbi;
                writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[2].dstSet = fr.objSet; writes[2].dstBinding = 2; writes[2].descriptorCount = 1;
                writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[2].pBufferInfo = &vbi;
                vkUpdateDescriptorSets(m_dev.device, 3, writes, 0, nullptr);
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

        // ---- mesh.frag SSAO set (set 3): AO sampler + SsaoControl UBO + the
        // SSR/RT reflection buffer (binding 2, refl.comp output — bound to the
        // blurred-AO view as a layout-valid placeholder until the refl chain is
        // built). Created here (only needs the device) so the mesh pipeline
        // layout can include it; the rest is built later in createSsao(). ----
        {
            // Bindings 3/4 = the DDGI irradiance + visibility atlases (r_ddgi).
            // mesh.frag statically references them, so they are part of the
            // layout on EVERY device; non-DDGI paths point them at the blurred-AO
            // view as a layout-valid placeholder (never sampled — gate 0).
            VkDescriptorSetLayoutBinding b[6]{};
            b[0].binding = 0; b[0].descriptorCount = 1;
            b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            b[1].binding = 1; b[1].descriptorCount = 1;
            b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            b[2].binding = 2; b[2].descriptorCount = 1;
            b[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            b[3].binding = 3; b[3].descriptorCount = 1;
            b[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            b[4].binding = 4; b[4].descriptorCount = 1;
            b[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            // Binding 5 = the scene TLAS for RT soft shadows (r_rtshadows) — in
            // the LAYOUT only on ray-query devices (the AS descriptor type needs
            // VK_KHR_acceleration_structure). Only the mesh_rt.frag variant
            // statically references it; the plain pipelines never touch it, so
            // it may stay unwritten until the first TLAS build lands.
            b[5].binding = 5; b[5].descriptorCount = 1;
            b[5].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            b[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = m_rtSupported ? 6u : 5u; ci.pBindings = b;
            if (vkCreateDescriptorSetLayout(m_dev.device, &ci, nullptr, &m_meshAoSetLayout) != VK_SUCCESS) {
                logError("[rhi] mesh ao set layout failed"); return false;
            }
        }
        // ---- mesh.frag IBL set (set 4): irradiance cube + prefilter cube + BRDF LUT.
        // Created here (device-only) so the mesh pipeline layout can declare it; the
        // images/sets are built later in createIbl(). 3 combined image samplers. ----
        {
            VkDescriptorSetLayoutBinding b[3]{};
            for (int i = 0; i < 3; ++i) {
                b[i].binding = i; b[i].descriptorCount = 1;
                b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            }
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = 3; ci.pBindings = b;
            if (vkCreateDescriptorSetLayout(m_dev.device, &ci, nullptr, &m_iblMeshSetLayout) != VK_SUCCESS) {
                logError("[rhi] mesh ibl set layout failed"); return false;
            }
        }

        // ---- glass.frag set 4: scene-color copy sampler + GlassControl UBO -----
        // The glass pipeline's EXTRA set (beyond the 4 shared with the opaque mesh
        // path), so glass.frag can sample the scene behind it (refraction M2/frost
        // M4) + read the per-frame camera pos / time / dev overrides. Created here
        // (only needs the device) so the glass pipeline layout can include it; the
        // UBOs + descriptor sets are built later (after the scene-copy target exists)
        // in createGlassResources / writeGlassDescriptors.
        {
            // binding0 = scene-color copy (sharp, refraction M2); binding1 =
            // GlassControl UBO; binding2 = frostiest blur level (M4 frost lerp).
            VkDescriptorSetLayoutBinding b[3]{};
            b[0].binding = 0; b[0].descriptorCount = 1;
            b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            b[1].binding = 1; b[1].descriptorCount = 1;
            b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            b[2].binding = 2; b[2].descriptorCount = 1;
            b[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = 3; ci.pBindings = b;
            if (vkCreateDescriptorSetLayout(m_dev.device, &ci, nullptr, &m_glassSetLayout) != VK_SUCCESS) {
                logError("[rhi] glass set layout failed"); return false;
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
        // texture + control UBO, set 4 = IBL (irradiance + prefilter cubes + BRDF
        // LUT). NO push constants (per-object data is in the SSBO).
        VkDescriptorSetLayout setLayouts[5] = { m_bindlessLayout, m_objSetLayout, m_shadowSetLayout, m_meshAoSetLayout, m_iblMeshSetLayout };
        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount = 5; plci.pSetLayouts = setLayouts;
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
        VkResult pr = x3CreateGraphicsPipelines(1, &gpci, nullptr, &m_meshPipeline);
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
        pr = x3CreateGraphicsPipelines(1, &gpci, nullptr, &m_meshPipelineNoSsao);

        // ---- Reflection-PROBE scene PSO: mesh_probe.vert (per-face viewProj via push
        // constant) + the SAME mesh.frag, opaque depth LESS+write into the HDR env-cube
        // face. regenIblFromSky() uses it to bake interior geometry into the IBL env so
        // glossy metals reflect the room, not the open sky. gpci is in opaque (dssNo/cb/rs).
        {
            VkShaderModule pvs = loadShaderModule("shaders\\mesh_probe.vert.spv");
            if (pvs) {
                VkPushConstantRange pcr{}; pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
                pcr.offset = 0; pcr.size = sizeof(glm::mat4);
                VkPipelineLayoutCreateInfo plp{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
                plp.setLayoutCount = 5; plp.pSetLayouts = setLayouts;       // same 5 sets as the mesh pass
                plp.pushConstantRangeCount = 1; plp.pPushConstantRanges = &pcr;
                if (vkCreatePipelineLayout(m_dev.device, &plp, nullptr, &m_meshProbeLayout) == VK_SUCCESS) {
                    VkPipelineShaderStageCreateInfo pst[2] = { stages[0], stages[1] };
                    pst[0].module = pvs;                                    // swap vertex -> probe (fs unchanged)
                    VkGraphicsPipelineCreateInfo ppgci = gpci;              // opaque state, HDR color + depth fmt
                    ppgci.pStages = pst; ppgci.layout = m_meshProbeLayout;
                    if (x3CreateGraphicsPipelines(1, &ppgci, nullptr, &m_meshProbePipe) != VK_SUCCESS)
                        logError("[rhi] reflection-probe pipeline create failed (probe disabled)");
                }
                vkDestroyShaderModule(m_dev.device, pvs, nullptr);
            } else {
                logError("[rhi] mesh_probe.vert.spv failed to load (reflection probe disabled)");
            }
        }

        // Transparent (BLEND/glass) variant — same shaders/layout/HDR target, vs/fs still alive.
        // src-alpha OVER blend, depth-test LESS_OR_EQUAL (works for both the EQUAL-prepass and the
        // LESS no-prepass opaque depth), NO depth-write, cull NONE (double-sided glass).
        VkResult prT = VK_SUCCESS;
        {
            VkPipelineColorBlendAttachmentState cbaT = cba;
            cbaT.blendEnable = VK_TRUE;
            cbaT.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            cbaT.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cbaT.colorBlendOp = VK_BLEND_OP_ADD;
            cbaT.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cbaT.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cbaT.alphaBlendOp = VK_BLEND_OP_ADD;
            VkPipelineColorBlendStateCreateInfo cbT = cb; cbT.pAttachments = &cbaT;
            VkPipelineDepthStencilStateCreateInfo dssT = dss;   // depthTest ON, write OFF
            dssT.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
            VkPipelineRasterizationStateCreateInfo rsT = rs; rsT.cullMode = VK_CULL_MODE_NONE;
            gpci.pColorBlendState = &cbT; gpci.pDepthStencilState = &dssT; gpci.pRasterizationState = &rsT;
            prT = x3CreateGraphicsPipelines(1, &gpci, nullptr, &m_meshPipelineTransparent);
            gpci.pColorBlendState = &cb; gpci.pDepthStencilState = &dssNo; gpci.pRasterizationState = &rs;  // restore
        }

        // ---- RT soft-shadow variants (r_rtshadows; ray-query devices only) ----
        // The SAME three mesh pipelines with mesh_rt.frag.spv (inline ray-query
        // shadow rays; TLAS at set3/binding5, which the layout above carries on
        // RT devices). Identical fixed-function state per-variant; bound at draw
        // time only on frames where the TLAS descriptor is valid. NON-FATAL on
        // failure: the want-gate checks the pipeline handle, so a load/create
        // failure simply leaves RT shadows off (plain raster path).
        if (m_rtSupported) {
            VkShaderModule fsRt = loadShaderModule("shaders\\mesh_rt.frag.spv");
            if (fsRt) {
                VkPipelineShaderStageCreateInfo rtStages[2] = { stages[0], stages[1] };
                rtStages[1].module = fsRt;
                gpci.pStages = rtStages;
                // EQUAL/no-write (depth pre-pass on) variant:
                gpci.pDepthStencilState = &dss;
                if (x3CreateGraphicsPipelines(1, &gpci, nullptr, &m_meshPipelineRt) != VK_SUCCESS)
                    m_meshPipelineRt = VK_NULL_HANDLE;
                // LESS/write (no pre-pass) variant:
                gpci.pDepthStencilState = &dssNo;
                if (x3CreateGraphicsPipelines(1, &gpci, nullptr, &m_meshPipelineNoSsaoRt) != VK_SUCCESS)
                    m_meshPipelineNoSsaoRt = VK_NULL_HANDLE;
                // Transparent (BLEND) variant — same state the block above used:
                {
                    VkPipelineColorBlendAttachmentState cbaT = cba;
                    cbaT.blendEnable = VK_TRUE;
                    cbaT.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                    cbaT.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                    cbaT.colorBlendOp = VK_BLEND_OP_ADD;
                    cbaT.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                    cbaT.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                    cbaT.alphaBlendOp = VK_BLEND_OP_ADD;
                    VkPipelineColorBlendStateCreateInfo cbT = cb; cbT.pAttachments = &cbaT;
                    VkPipelineDepthStencilStateCreateInfo dssT = dss;
                    dssT.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
                    VkPipelineRasterizationStateCreateInfo rsT = rs; rsT.cullMode = VK_CULL_MODE_NONE;
                    gpci.pColorBlendState = &cbT; gpci.pDepthStencilState = &dssT; gpci.pRasterizationState = &rsT;
                    if (x3CreateGraphicsPipelines(1, &gpci, nullptr, &m_meshPipelineTransparentRt) != VK_SUCCESS)
                        m_meshPipelineTransparentRt = VK_NULL_HANDLE;
                    gpci.pColorBlendState = &cb; gpci.pDepthStencilState = &dssNo; gpci.pRasterizationState = &rs;  // restore
                }
                gpci.pStages = stages;   // restore for the planet clones below
                vkDestroyShaderModule(m_dev.device, fsRt, nullptr);
                if (m_meshPipelineRt && m_meshPipelineNoSsaoRt)
                    logInfo("[rhi] RT soft-shadow mesh pipelines ready (mesh_rt.frag: inline ray-query sun + point shadows)");
                else
                    logError("[rhi] RT soft-shadow pipeline create failed — r_rtshadows stays raster-only");
            } else {
                logError("[rhi] mesh_rt.frag.spv failed to load — r_rtshadows stays raster-only");
            }
        }

        // ---- Planet body pipelines (FORGE3D port) — one OPAQUE PSO per type ----
        // Each is a CLONE of the OPAQUE mesh PSO (same MeshVertex input via `vin`,
        // depth LESS + write via `dssNo`, cull BACK via `rs`, no blend via `cb`, same
        // HDR target via `prci`) but with planet.vert + the per-type fragment shader,
        // all sharing ONE layout (set0 bindless + set1 object SSBO/camera UBO + a
        // 128-byte push-constant range, VERTEX|FRAGMENT). gpci is currently in the
        // restored opaque state (cb/dssNo/rs) — exactly the opaque depth/cull/blend.
        // Per-type fragment .spv in PlanetType enum order (Moon..Sun). Atmosphere /
        // suncorona / ring shells are DEFERRED (not wired this pass).
        VkResult prP = VK_SUCCESS;
        {
            static const char* kPlanetFrags[PT_OpaqueCount] = {
                "shaders\\planet_moon.frag.spv",
                "shaders\\planet_ice.frag.spv",
                "shaders\\planet_gas.frag.spv",
                "shaders\\planet_lava.frag.spv",
                "shaders\\planet_terrestrial.frag.spv",
                "shaders\\planet_oceanic.frag.spv",
                "shaders\\planet_sand.frag.spv",
                "shaders\\planet_thunderstorm.frag.spv",
                "shaders\\planet_sun.frag.spv",
            };
            VkShaderModule pvs = loadShaderModule("shaders\\planet.vert.spv");
            if (!pvs) {
                vkDestroyShaderModule(m_dev.device, vs, nullptr);
                vkDestroyShaderModule(m_dev.device, fs, nullptr);
                logError("[rhi] planet vertex shader module failed to load"); return false;
            }
            // Shared layout: SAME set0 (bindless) + set1 (object SSBO + camera UBO)
            // layouts the mesh pipeline uses, + a 128-byte push constant for both stages.
            VkDescriptorSetLayout planetSetLayouts[2] = { m_bindlessLayout, m_objSetLayout };
            VkPushConstantRange pcRange{};
            pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            pcRange.offset = 0; pcRange.size = 128;
            VkPipelineLayoutCreateInfo pplci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pplci.setLayoutCount = 2; pplci.pSetLayouts = planetSetLayouts;
            pplci.pushConstantRangeCount = 1; pplci.pPushConstantRanges = &pcRange;
            if (vkCreatePipelineLayout(m_dev.device, &pplci, nullptr, &m_planetPipelineLayout) != VK_SUCCESS) {
                vkDestroyShaderModule(m_dev.device, pvs, nullptr);
                vkDestroyShaderModule(m_dev.device, vs, nullptr);
                vkDestroyShaderModule(m_dev.device, fs, nullptr);
                logError("[rhi] planet pipeline layout failed"); return false;
            }
            for (uint32_t pt = 0; pt < (uint32_t)PT_OpaqueCount && prP == VK_SUCCESS; ++pt) {
                VkShaderModule pfs = loadShaderModule(kPlanetFrags[pt]);
                if (!pfs) { logError(std::string("[rhi] planet frag failed to load: ") + kPlanetFrags[pt]);
                            prP = VK_ERROR_INITIALIZATION_FAILED; break; }
                VkPipelineShaderStageCreateInfo pstages[2]{};
                pstages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                pstages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   pstages[0].module = pvs; pstages[0].pName = "main";
                pstages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                pstages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; pstages[1].module = pfs; pstages[1].pName = "main";
                VkGraphicsPipelineCreateInfo pgci = gpci;   // copies the opaque state set above
                pgci.pStages = pstages; pgci.layout = m_planetPipelineLayout;
                prP = x3CreateGraphicsPipelines(1, &pgci, nullptr, &m_planetPipelines[pt]);
                vkDestroyShaderModule(m_dev.device, pfs, nullptr);
            }

            // ---- TRANSPARENT glow shells (DEFERRED layers, now wired) ----------
            // Three more PSOs sharing m_planetPipelineLayout + planet.vert, drawn
            // AFTER the opaque bodies. They override the opaque blend/depth/cull:
            //   Atmosphere / SunCorona : ADDITIVE (srcRGB=ONE, dstRGB=ONE), depth
            //       test LEQUAL + write OFF, cull NONE (far limb hemisphere shows).
            //   Ring : ALPHA (SRC_ALPHA / ONE_MINUS_SRC_ALPHA), depth LEQUAL +
            //       write OFF, cull NONE (annulus visible from both faces).
            // The frags emit PREMULTIPLIED glow (atmosphere/corona) so srcRGB=ONE.
            struct TransP { PlanetType pt; const char* frag; bool additive; };
            static const TransP kTrans[] = {
                { PT_Atmosphere, "shaders\\planet_atmosphere.frag.spv", true  },
                { PT_SunCorona,  "shaders\\planet_suncorona.frag.spv",  true  },
                { PT_Ring,       "shaders\\planet_ring.frag.spv",       false },
            };
            // Shared depth (LEQUAL, write OFF) + raster (cull NONE) for all three.
            VkPipelineDepthStencilStateCreateInfo dssGlow{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
            dssGlow.depthTestEnable = VK_TRUE; dssGlow.depthWriteEnable = VK_FALSE;
            dssGlow.depthCompareOp  = VK_COMPARE_OP_LESS_OR_EQUAL;
            VkPipelineRasterizationStateCreateInfo rsGlow{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
            rsGlow.polygonMode = VK_POLYGON_MODE_FILL; rsGlow.cullMode = VK_CULL_MODE_NONE;
            rsGlow.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rsGlow.lineWidth = 1.0f;
            for (const TransP& tp : kTrans) {
                if (prP != VK_SUCCESS) break;
                VkShaderModule pfs = loadShaderModule(tp.frag);
                if (!pfs) { logError(std::string("[rhi] planet (transparent) frag failed to load: ") + tp.frag);
                            prP = VK_ERROR_INITIALIZATION_FAILED; break; }
                VkPipelineShaderStageCreateInfo pstages[2]{};
                pstages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                pstages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   pstages[0].module = pvs; pstages[0].pName = "main";
                pstages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                pstages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; pstages[1].module = pfs; pstages[1].pName = "main";
                VkPipelineColorBlendAttachmentState cbaG{};
                cbaG.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
                cbaG.blendEnable = VK_TRUE;
                cbaG.colorBlendOp = VK_BLEND_OP_ADD; cbaG.alphaBlendOp = VK_BLEND_OP_ADD;
                if (tp.additive) {                       // premultiplied glow: ONE/ONE
                    cbaG.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
                    cbaG.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
                    cbaG.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                    cbaG.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                } else {                                 // ring: SRC_ALPHA over
                    cbaG.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                    cbaG.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                    cbaG.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                    cbaG.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                }
                VkPipelineColorBlendStateCreateInfo cbG = cb; cbG.pAttachments = &cbaG;
                VkGraphicsPipelineCreateInfo pgci = gpci;   // opaque base; override below
                pgci.pStages = pstages; pgci.layout = m_planetPipelineLayout;
                pgci.pColorBlendState = &cbG; pgci.pDepthStencilState = &dssGlow; pgci.pRasterizationState = &rsGlow;
                prP = x3CreateGraphicsPipelines(1, &pgci, nullptr, &m_planetPipelines[tp.pt]);
                vkDestroyShaderModule(m_dev.device, pfs, nullptr);
            }
            vkDestroyShaderModule(m_dev.device, pvs, nullptr);
        }

        vkDestroyShaderModule(m_dev.device, vs, nullptr);
        vkDestroyShaderModule(m_dev.device, fs, nullptr);
        if (pr != VK_SUCCESS) { logError("[rhi] no-ssao graphics pipeline create failed"); return false; }
        if (prT != VK_SUCCESS) { logError("[rhi] transparent graphics pipeline create failed"); return false; }
        if (prP != VK_SUCCESS) { logError("[rhi] planet graphics pipeline create failed"); return false; }

        logInfo("[rhi] GPU-driven mesh pipeline ready (bindless textures + per-object SSBO + multidraw-indirect)");
        logInfo("[rhi] planet body pipeline ready (push-constant model + bindless triplanar PBR)");

        // ---- Translucent GLASS pipeline (transparent pass) ----
        // Shares mesh.vert + sets 0-3 with the opaque mesh path, but uses its OWN
        // pipeline layout m_glassLayout (sets 0-3 identical + set 4 = scene-color
        // copy sampler + GlassControl UBO) so glass.frag can sample the scene behind
        // it (refraction M2 / frost M4). glass.frag, alpha blend ON
        // (SRC_ALPHA/ONE_MINUS_SRC_ALPHA), depth-test LESS_OR_EQUAL with depth-write
        // OFF (composites over the opaque scene without disturbing depth), cull NONE
        // (double-sided glass). GRACEFUL FALLBACK (spec §5): on any failure the
        // pipeline stays NULL and the glass pass is skipped — opaque is never affected.
        {
            // Glass pipeline layout: the 4 shared mesh sets + the glass-only set 4.
            VkDescriptorSetLayout glassSets[5] = {
                m_bindlessLayout, m_objSetLayout, m_shadowSetLayout,
                m_meshAoSetLayout, m_glassSetLayout };
            VkPipelineLayoutCreateInfo gplci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            gplci.setLayoutCount = 5; gplci.pSetLayouts = glassSets;
            if (vkCreatePipelineLayout(m_dev.device, &gplci, nullptr, &m_glassLayout) != VK_SUCCESS) {
                m_glassLayout = VK_NULL_HANDLE;
                logError("[rhi] glass pipeline layout failed — glass pass disabled (opaque unaffected)");
            }

            VkShaderModule gvs = m_glassLayout ? loadShaderModule("shaders\\mesh.vert.spv") : VK_NULL_HANDLE;
            VkShaderModule gfs = m_glassLayout ? loadShaderModule("shaders\\glass.frag.spv") : VK_NULL_HANDLE;
            if (!m_glassLayout || !gvs || !gfs) {
                if (gvs) vkDestroyShaderModule(m_dev.device, gvs, nullptr);
                if (gfs) vkDestroyShaderModule(m_dev.device, gfs, nullptr);
                if (m_glassLayout) logError("[rhi] glass shader load failed — glass pass disabled (opaque unaffected)");
            } else {
                VkPipelineShaderStageCreateInfo gst[2];
                gst[0] = stages[0]; gst[0].module = gvs;   // mesh.vert (shared)
                gst[1] = stages[1]; gst[1].module = gfs;   // glass.frag

                // Double-sided glass: no back-face cull.
                VkPipelineRasterizationStateCreateInfo grs = rs;
                grs.cullMode = VK_CULL_MODE_NONE;

                // Depth: test LESS_OR_EQUAL against the opaque depth, no write.
                VkPipelineDepthStencilStateCreateInfo gdss = dss;
                gdss.depthTestEnable  = VK_TRUE;
                gdss.depthWriteEnable = VK_FALSE;
                gdss.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;

                // Standard straight-alpha blend.
                VkPipelineColorBlendAttachmentState gcba = cba;
                gcba.blendEnable         = VK_TRUE;
                gcba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                gcba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                gcba.colorBlendOp        = VK_BLEND_OP_ADD;
                gcba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                gcba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                gcba.alphaBlendOp        = VK_BLEND_OP_ADD;
                VkPipelineColorBlendStateCreateInfo gcb = cb;
                gcb.pAttachments = &gcba;

                VkGraphicsPipelineCreateInfo ggpci = gpci;
                ggpci.layout             = m_glassLayout;   // glass-specific (set 4)
                ggpci.pStages            = gst;
                ggpci.pRasterizationState = &grs;
                ggpci.pDepthStencilState  = &gdss;
                ggpci.pColorBlendState    = &gcb;
                VkResult gpr = x3CreateGraphicsPipelines(1,
                                                         &ggpci, nullptr, &m_glassPipeline);
                vkDestroyShaderModule(m_dev.device, gvs, nullptr);
                vkDestroyShaderModule(m_dev.device, gfs, nullptr);
                if (gpr != VK_SUCCESS) {
                    m_glassPipeline = VK_NULL_HANDLE;
                    logError("[rhi] glass pipeline create failed — glass pass disabled (opaque unaffected)");
                } else {
                    logInfo("[rhi] translucent glass pipeline ready (transparent pass)");
                }
            }
        }

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
        if (x3vmaCreateImage(&ici, &aci, &m_shadowImg, &m_shadowAlloc, nullptr) != VK_SUCCESS) {
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
        if (x3CreateDescriptorPool(&pci, nullptr, &m_shadowDescPool) != VK_SUCCESS) {
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
        VkResult pr = x3CreateGraphicsPipelines(1, &gpci, nullptr, &m_shadowPipeline);
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
        if (m_meshProbePipe) { vkDestroyPipeline(m_dev.device, m_meshProbePipe, nullptr); m_meshProbePipe = VK_NULL_HANDLE; }
        if (m_meshProbeLayout) { vkDestroyPipelineLayout(m_dev.device, m_meshProbeLayout, nullptr); m_meshProbeLayout = VK_NULL_HANDLE; }
        if (m_meshPipelineTransparent) vkDestroyPipeline(m_dev.device, m_meshPipelineTransparent, nullptr);
        if (m_meshPipelineRt)            vkDestroyPipeline(m_dev.device, m_meshPipelineRt, nullptr);
        if (m_meshPipelineNoSsaoRt)      vkDestroyPipeline(m_dev.device, m_meshPipelineNoSsaoRt, nullptr);
        if (m_meshPipelineTransparentRt) vkDestroyPipeline(m_dev.device, m_meshPipelineTransparentRt, nullptr);
        m_meshPipelineRt = VK_NULL_HANDLE; m_meshPipelineNoSsaoRt = VK_NULL_HANDLE;
        m_meshPipelineTransparentRt = VK_NULL_HANDLE;
        for (uint32_t pt = 0; pt < (uint32_t)PT_Count; ++pt) {
            if (m_planetPipelines[pt]) { vkDestroyPipeline(m_dev.device, m_planetPipelines[pt], nullptr); m_planetPipelines[pt] = VK_NULL_HANDLE; }
        }
        if (m_planetPipelineLayout) vkDestroyPipelineLayout(m_dev.device, m_planetPipelineLayout, nullptr);
        if (m_glassPipeline) vkDestroyPipeline(m_dev.device, m_glassPipeline, nullptr);
        if (m_glassLayout)   vkDestroyPipelineLayout(m_dev.device, m_glassLayout, nullptr);
        if (m_glassSetLayout) vkDestroyDescriptorSetLayout(m_dev.device, m_glassSetLayout, nullptr);
        if (m_meshLayout)    vkDestroyPipelineLayout(m_dev.device, m_meshLayout, nullptr);
        // mesh.frag set 4 layout (IBL): created in createGraphics, baked into m_meshLayout.
        if (m_iblMeshSetLayout) { vkDestroyDescriptorSetLayout(m_dev.device, m_iblMeshSetLayout, nullptr); m_iblMeshSetLayout = VK_NULL_HANDLE; }
        if (m_uploadFence)   vkDestroyFence(m_dev.device, m_uploadFence, nullptr);
        for (int s = 0; s < 2; ++s) {   // boot-time upload-batch fences (double-buffered)
            if (m_batchFences[s]) { vkDestroyFence(m_dev.device, m_batchFences[s], nullptr); m_batchFences[s] = VK_NULL_HANDLE; }
            m_batchCmds[s] = VK_NULL_HANDLE;   // freed with m_uploadPool
        }
        if (m_uploadPool)    vkDestroyCommandPool(m_dev.device, m_uploadPool, nullptr);
        m_meshPipeline = VK_NULL_HANDLE; m_meshPipelineNoSsao = VK_NULL_HANDLE;
        m_meshPipelineTransparent = VK_NULL_HANDLE; m_meshLayout = VK_NULL_HANDLE;
        m_planetPipelineLayout = VK_NULL_HANDLE;
        m_glassPipeline = VK_NULL_HANDLE;
        m_glassLayout = VK_NULL_HANDLE; m_glassSetLayout = VK_NULL_HANDLE;
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

    // ======================================================================
    // Hardware ray tracing (RT Phase 1+: BLAS/TLAS + ray-query RT AO).
    // ----------------------------------------------------------------------
    // The AS manager (BLAS per mesh + per-frame TLAS) lives in VulkanRT; the
    // ray-query RT-AO compute pass + its full-screen MULTIPLY apply pass live in
    // this class. EVERYTHING is gated behind m_rtao.enabled (the r_rtao cvar) AND
    // m_rtSupported; when off, none of this is built/dispatched and the raster +
    // SSAO/SSGI path is byte-for-byte unchanged.
    VulkanRT          m_rt;                                  // AS manager (ray-query)
    bool              m_rtInitTried = false;                 // lazy one-time module init
    RtaoParams        m_rtao{};                              // cached tunables (default OFF)
    bool              m_rtaoBuilt = false;                   // RT-AO pipelines created
    uint32_t          m_rtFrameSeed = 0;                     // per-frame noise seed
    // RT-AO half-res target (R8 storage image written by the compute pass; sampled
    // by the apply pass). GENERAL layout while the compute writes it.
    VkImage           m_rtaoImg   = VK_NULL_HANDLE; VmaAllocation m_rtaoAlloc = nullptr; VkImageView m_rtaoView = VK_NULL_HANDLE;
    VkExtent2D        m_rtaoExtent{};
    static constexpr VkFormat kRtaoFormat = VK_FORMAT_R8_UNORM;
    // RT-AO compute pass (rayQueryEXT): set0 = depth sampler + AO storage image +
    // TLAS + Rtao UBO.
    VkSampler             m_rtaoDepthSampler  = VK_NULL_HANDLE; // NEAREST depth
    VkSampler             m_rtaoLinearSampler = VK_NULL_HANDLE; // LINEAR AO up-sample
    VkDescriptorSetLayout m_rtaoSetLayout     = VK_NULL_HANDLE;
    VkPipelineLayout      m_rtaoLayout        = VK_NULL_HANDLE;
    VkPipeline            m_rtaoPipe          = VK_NULL_HANDLE;  // compute
    VkDescriptorSetLayout m_rtaoApplySetLayout = VK_NULL_HANDLE;
    VkPipelineLayout      m_rtaoApplyLayout    = VK_NULL_HANDLE;
    VkPipeline            m_rtaoApplyPipe      = VK_NULL_HANDLE; // full-screen MULTIPLY
    VkDescriptorPool      m_rtaoPool           = VK_NULL_HANDLE;
    VkDescriptorSet       m_rtaoSet[kFramesInFlight]      = {};  // compute set (per frame)
    VkDescriptorSet       m_rtaoApplySet[kFramesInFlight] = {};  // apply set (per frame)
    // Rtao compute UBO (std140; matches shaders/rtao.comp).
    struct RtaoUBO {
        glm::mat4 invViewProj;   // clip -> world
        glm::vec4 camPos;        // xyz = camera world pos
        glm::vec4 params0;       // x = radius, y = numRays, z = bias, w = strength
        glm::vec4 params1;       // x = outW, y = outH, z = frameSeed, w = power
    };
    static_assert(sizeof(RtaoUBO) == 64 + 16 * 3, "RtaoUBO std140 layout");
    VkBuffer      m_rtaoUboBuf[kFramesInFlight]  = {}; VmaAllocation m_rtaoUboAlloc[kFramesInFlight] = {}; void* m_rtaoUboMapped[kFramesInFlight] = {};
    // Apply-pass push constant (matches shaders/rtao_apply.frag).
    struct RtaoApplyPush { float aoTexel[2]; float strength; float pad0; };
    RtaoApplyPush m_rtaoApplyPush{};
    // Per-frame TLAS-instance scratch (capacity persists; no per-frame heap churn).
    std::vector<VulkanRT::TlasInstance> m_rtInstScratch;
    uint64_t m_rtTlasSig = 0;             // signature of the last-built TLAS instance set
    bool m_rtaoActiveThisFrame = false;   // RT-AO chain added to the graph this frame
    // Stable storage for the RT-AO apply pass's VkRenderingInfo + attachment (the
    // graph holds pointers into these across execute()).
    VkRenderingAttachmentInfo m_rtaoApplyAttach{};
    VkRenderingInfo           m_rtaoApplyRenderInfo{};

    // ======================================================================
    // RT ACOUSTICS (traceAudioRays) — batched audio ray queries vs the TLAS.
    static constexpr uint32_t kAudioRayCapacity = 1024;   // rays per call (hard cap)
    static_assert(sizeof(AudioRay) == 32, "AudioRay must match audio_rays.comp std430 (two vec4s)");
    VkDescriptorSetLayout m_audioRaySetLayout = VK_NULL_HANDLE;
    VkPipelineLayout      m_audioRayLayout    = VK_NULL_HANDLE;
    VkPipeline            m_audioRayPipe      = VK_NULL_HANDLE;
    VkDescriptorPool      m_audioRayPool      = VK_NULL_HANDLE;
    VkDescriptorSet       m_audioRaySet       = VK_NULL_HANDLE;
    VkBuffer        m_audioRayInBuf  = VK_NULL_HANDLE; VmaAllocation m_audioRayInAlloc  = nullptr; void* m_audioRayInMapped  = nullptr;
    VkBuffer        m_audioRayOutBuf = VK_NULL_HANDLE; VmaAllocation m_audioRayOutAlloc = nullptr; void* m_audioRayOutMapped = nullptr;
    VkCommandPool   m_audioRayCmdPool = VK_NULL_HANDLE;
    VkCommandBuffer m_audioRayCmd     = VK_NULL_HANDLE;
    VkFence         m_audioRayFence   = VK_NULL_HANDLE;
    VkAccelerationStructureKHR m_audioRayTlasBound = VK_NULL_HANDLE; // descriptor currency
    bool m_audioRayBuilt  = false;
    bool m_audioRayFailed = false;        // one-shot create failure latch (no retry spam)
    bool m_audioRayInFlight = false;      // a submitted batch awaits harvest
    int  m_audioRayInFlightCount = 0;     // ray count of the in-flight batch
    int  m_audioRaysWantFrames = 0;       // >0: keep the scene TLAS built for audio

    // ======================================================================
    // SSR / RAY-QUERY REFLECTIONS (r_ssr / r_rtreflections) — refl.comp.
    // ----------------------------------------------------------------------
    // A half-res (or full-res, r_reflquality) compute pass after the depth
    // pre-pass: marches each pixel's reflection ray against the depth buffer and
    // samples LAST frame's lit scene (the TAA history image), with an optional
    // inline ray-query fallback into the SAME BLAS/TLAS the RT-AO path builds.
    // Output rgba16f (radiance + confidence) is sampled by mesh.frag (set 3,
    // binding 2) and blended into the split-sum IBL specular. Built LAZILY on
    // first enable; everything gated — off = zero passes, render unchanged.
    // Cached tunables. DEVICE default = ON (like the PostFXParams TAA default):
    // headless screenshot/smoketest paths that never push cvars still get the
    // shipped look. The r_ssr/r_rtreflections cvars + the --norefl/--notaa/
    // --legacypost pins override it via setReflectionParams. Reflections only
    // ever ACTIVATE when TAA is on, so every TAA-off path is still bit-identical.
    ReflectionParams m_refl{ /*ssr=*/true, /*rtFallback=*/true, /*fullRes=*/false, /*intensity=*/1.0f };
    bool       m_reflBuilt = false;                  // pipelines/targets created
    bool       m_reflFullRes = false;                // resolution the targets were built at
    bool       m_reflActiveThisFrame = false;        // refl pass in the graph this frame
    bool       m_reflRtThisFrame = false;            // ray-query fallback pipeline this frame
    bool       m_reflHistValid = false;              // TAA history usable as color source
    glm::mat4  m_reflPrevVP{ 1.0f };                 // prev frame's UNJITTERED viewProj
    VkImage    m_reflImg = VK_NULL_HANDLE; VmaAllocation m_reflAlloc = nullptr; VkImageView m_reflView = VK_NULL_HANDLE;
    VkExtent2D m_reflExtent{};
    static constexpr VkFormat kReflFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkDescriptorSetLayout m_reflSetLayout   = VK_NULL_HANDLE;  // SSR-only (no TLAS binding)
    VkDescriptorSetLayout m_reflSetLayoutRt = VK_NULL_HANDLE;  // + TLAS at binding 4 (RT devices)
    VkPipelineLayout      m_reflLayout      = VK_NULL_HANDLE;
    VkPipelineLayout      m_reflLayoutRt    = VK_NULL_HANDLE;
    VkPipeline            m_reflPipe        = VK_NULL_HANDLE;  // refl.comp.spv (SSR-only)
    VkPipeline            m_reflPipeRt      = VK_NULL_HANDLE;  // refl_rt.comp.spv (+ ray query)
    VkDescriptorPool      m_reflPool        = VK_NULL_HANDLE;
    VkDescriptorSet       m_reflSet[kFramesInFlight]   = {};   // SSR set (per frame)
    VkDescriptorSet       m_reflSetRt[kFramesInFlight] = {};   // RT set (per frame; RT devices)
    // Refl compute UBO (std140; matches shaders/refl.comp).
    struct ReflUBO {
        glm::mat4 invViewProj;   // CURRENT (jittered) clip -> world
        glm::mat4 viewProj;      // CURRENT (jittered) world -> clip
        glm::mat4 prevViewProj;  // PREVIOUS frame's UNJITTERED world -> clip
        glm::vec4 camPos;        // xyz = camera, w = history valid
        glm::vec4 sunDir;        // xyz = toward sun, w = frame seed
        glm::vec4 ambient;       // rgb = scene ambient, w = unused
        glm::vec4 params0;       // x = outW, y = outH, z = max march dist, w = thickness
        glm::vec4 params1;       // x = march steps, y/z/w = unused
    };
    static_assert(sizeof(ReflUBO) == 64 * 3 + 16 * 5, "ReflUBO std140 layout");
    VkBuffer m_reflUboBuf[kFramesInFlight] = {}; VmaAllocation m_reflUboAlloc[kFramesInFlight] = {}; void* m_reflUboMapped[kFramesInFlight] = {};

    // ======================================================================
    // DDGI — dynamic diffuse global illumination (r_ddgi; ddgi_rays.comp +
    // ddgi_update.comp + mesh.frag set3 bindings 3/4).
    // ----------------------------------------------------------------------
    // Probe-grid GI (Majercik et al. 2019): per-frame inline-ray-query rays
    // from every probe against the SHARED scene TLAS, simple per-object hit
    // shading (instanceCustomIndex -> ObjectData SSBO row), hysteresis-blended
    // into octahedral irradiance (8x8/probe, RGBA16F) + visibility-depth
    // (16x16/probe, RG16F) atlases that mesh.frag interpolates with Chebyshev
    // leak rejection. Tier-gated on ray query + position fetch; built lazily.
    bool       m_rtPosFetch = false;          // VK_KHR_ray_tracing_position_fetch enabled
    DdgiParams m_ddgi{};                      // cached tunables (default OFF)
    bool       m_ddgiBuilt = false;           // pipelines/targets created
    bool       m_ddgiWantThisFrame = false;   // decided in prepareFrameData (UBO gate)
    bool       m_ddgiActiveThisFrame = false; // passes in the graph this frame (TLAS ok)
    uint32_t   m_ddgiFrameCount = 0;          // updates since (re)activation (warm-up ramp)
    // The fitted/configured probe volume (sticky once activated; refit on toggle
    // or when the caller passes an explicit volume).
    glm::vec3  m_ddgiOrigin{ 0.0f };          // grid min corner (world)
    glm::vec3  m_ddgiSpacing{ 1.0f };         // probe spacing (m)
    int        m_ddgiCountX = 0, m_ddgiCountY = 0, m_ddgiCountZ = 0; // built atlas dims
    bool       m_ddgiVolumeValid = false;
    float      m_ddgiVisMaxDist = 4.0f;       // visibility clamp (1.5 * spacing diagonal)
    // Octahedral atlases (persistent across frames; graph-imported with tracked
    // state like taa.hist). Irradiance: 8x8 texels/probe; visibility: 16x16.
    VkImage    m_ddgiIrrImg = VK_NULL_HANDLE; VmaAllocation m_ddgiIrrAlloc = nullptr; VkImageView m_ddgiIrrView = VK_NULL_HANDLE;
    VkImage    m_ddgiVisImg = VK_NULL_HANDLE; VmaAllocation m_ddgiVisAlloc = nullptr; VkImageView m_ddgiVisView = VK_NULL_HANDLE;
    ResourceState m_ddgiIrrState{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 };
    ResourceState m_ddgiVisState{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 };
    static constexpr VkFormat kDdgiIrrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat kDdgiVisFormat = VK_FORMAT_R16G16_SFLOAT;
    // Per-frame ray results: probeCount * 128 * vec4 (radiance + signed distance).
    VkBuffer   m_ddgiRayBuf = VK_NULL_HANDLE; VmaAllocation m_ddgiRayAlloc = nullptr;
    VkDeviceSize m_ddgiRayBufSize = 0;
    VkSampler  m_ddgiSampler = VK_NULL_HANDLE;            // LINEAR/CLAMP atlas sampler
    VkDescriptorSetLayout m_ddgiRaySetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_ddgiUpSetLayout  = VK_NULL_HANDLE;
    VkPipelineLayout      m_ddgiRayLayout    = VK_NULL_HANDLE;
    VkPipelineLayout      m_ddgiUpLayout     = VK_NULL_HANDLE;
    VkPipeline            m_ddgiRayPipe      = VK_NULL_HANDLE;  // ddgi_rays.comp
    VkPipeline            m_ddgiUpPipe       = VK_NULL_HANDLE;  // ddgi_update.comp
    VkDescriptorPool      m_ddgiPool         = VK_NULL_HANDLE;
    VkDescriptorSet       m_ddgiRaySet[kFramesInFlight] = {};
    VkDescriptorSet       m_ddgiUpSet[kFramesInFlight]  = {};
    // DDGI compute UBO (std140; matches the Ddgi block in ddgi_rays.comp AND
    // ddgi_update.comp — one layout, two consumers).
    struct DdgiUBO {
        glm::vec4  gridOrigin;       // xyz = grid min corner, w = raysPerProbe
        glm::vec4  gridSpacing;      // xyz = spacing (m), w = visMaxDist (m)
        glm::ivec4 gridCounts;       // xyz = probe counts, w = frame index
        glm::vec4  rotation0;        // per-frame random rotation rows (3x3)
        glm::vec4  rotation1;
        glm::vec4  rotation2;
        glm::vec4  sunDirIntensity;  // xyz = toward sun, w = sun diffuse scale
        glm::vec4  ambientSky;       // rgb = flat ambient, w = env cube valid
        glm::vec4  params;           // x = hystIrr, y = lightCount, z = bounceGain, w = hystVis
        GpuPointLight lights[kMaxPointLights];
    };
    static_assert(sizeof(DdgiUBO) == 16 * 9 + kMaxPointLights * 32, "DdgiUBO std140 layout");
    VkBuffer m_ddgiUboBuf[kFramesInFlight] = {}; VmaAllocation m_ddgiUboAlloc[kFramesInFlight] = {}; void* m_ddgiUboMapped[kFramesInFlight] = {};
    // SSBO row of each draw record this frame (TLAS instanceCustomIndex source —
    // the grouped SSBO write order differs from the draw-record order).
    std::vector<uint32_t> m_recordSsboRow;

    // Graphics
    VmaAllocator  m_alloc = nullptr;
    VkPipeline       m_meshPipeline = VK_NULL_HANDLE;        // SSAO path: depth EQUAL, no write
    VkPipeline       m_meshPipelineNoSsao = VK_NULL_HANDLE;  // no-SSAO path: depth LESS, write
    // Interior reflection probe: a clone of the no-SSAO mesh PSO using mesh_probe.vert
    // (per-face viewProj via push constant) to bake scene geometry into the IBL env cube.
    VkPipeline       m_meshProbePipe = VK_NULL_HANDLE;
    VkPipelineLayout m_meshProbeLayout = VK_NULL_HANDLE;     // 5 mesh sets + mat4 push (VERTEX)
    VkImage          m_probeDepthImg = VK_NULL_HANDLE; VmaAllocation m_probeDepthAlloc = nullptr;
    VkImageView      m_probeDepthView = VK_NULL_HANDLE;      // env-face-sized depth for the bake
    bool             m_iblProbeScene = false;                // bake scene into env (vs sky-only)
    glm::vec3        m_iblProbePos{ 0.0f, 1.6f, 0.0f };      // probe world position (set to cam at bake)
    VkPipeline       m_meshPipelineTransparent = VK_NULL_HANDLE;  // BLEND: src-alpha over, LEQUAL, no depth-write, cull NONE
    // RT soft-shadow variants (mesh_rt.frag.spv; created only on ray-query
    // devices, bound only on frames where the TLAS descriptor is valid). Same
    // pipeline LAYOUT as the plain set (set3 just has one extra binding on RT
    // devices); identical fixed-function state per-variant.
    VkPipeline       m_meshPipelineRt            = VK_NULL_HANDLE; // EQUAL/no-write (pre-pass on)
    VkPipeline       m_meshPipelineNoSsaoRt      = VK_NULL_HANDLE; // LESS/write (no pre-pass)
    VkPipeline       m_meshPipelineTransparentRt = VK_NULL_HANDLE; // BLEND variant
    RtShadowParams   m_rtShadows{};                    // cached r_rtshadows tunables (default tier 2)
    bool             m_rtShadowsWantThisFrame   = false; // decided in prepareFrameData (UBO gate)
    bool             m_rtShadowsActiveThisFrame = false; // decided in endFrame (TLAS ready + descriptor written)
    bool             m_meshTlasWritten = false;          // mesh set3 binding5 points at a real TLAS
    uint32_t         m_rtshFrameSeed = 0;                // per-frame jitter rotation counter
    VkPipelineLayout m_meshLayout = VK_NULL_HANDLE;
    // Translucent GLASS pipeline (transparent pass). Shares mesh.vert + sets 0-3
    // with the opaque mesh path, but uses its OWN pipeline layout m_glassLayout
    // (sets 0-3 identical, plus set 4 = glass-specific: scene-color copy sampler +
    // glass control UBO) so glass.frag can sample the scene behind it (refraction
    // M2 / frost M4) without touching the locked mesh layout. Depth-test
    // LESS_OR_EQUAL, depth-write OFF, alpha blend, cull NONE (double-sided glass).
    // Created in createMeshPipeline after the opaque ones. Graceful fallback
    // (spec §5): if any glass object fails to create it stays NULL and the glass
    // pass is skipped (the opaque path is never broken).
    VkPipeline       m_glassPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_glassLayout   = VK_NULL_HANDLE;
    // Glass set 4: binding0 = scene-color copy (mip-aware sampler), binding1 =
    // GlassControl UBO (per-frame: camera world pos, time, screen size, dev cvar
    // overrides). The layout is created once; the per-frame UBO + descriptor set
    // are written each frame / on resize (the scene-copy view changes on resize).
    VkDescriptorSetLayout m_glassSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_glassPool      = VK_NULL_HANDLE;
    VkDescriptorSet       m_glassSet[kFramesInFlight] = {};
    VkBuffer      m_glassCtrlBuf[kFramesInFlight]    = {};
    VmaAllocation m_glassCtrlAlloc[kFramesInFlight]  = {};
    void*         m_glassCtrlMapped[kFramesInFlight] = {};
    // Frost (M4): a fullscreen-triangle downsample pipeline + per-level descriptor
    // sets (one per blur-source image) that blur the scene copy into m_glassFrostImg[].
    // Reuses the bloom-down shader/layout (m_bloomLayout). NULL until M4 builds it,
    // which keeps the frost passes off (glassFrostOn=false) for M2/M3.
    VkPipeline      m_glassFrostPipe = VK_NULL_HANDLE;
    VkDescriptorSet m_glassFrostSrcSet[kGlassFrostLevels + 1] = {}; // [0]=scene copy, [1..]=levels
    // Per-frame GlassControl UBO (std140; matches glass.frag GlassControl block).
    // camPos.xyz = camera world position (fresnel/specular view vector), .w = time
    // (animated glint). screen.xy = 1/width, 1/height (gl_FragCoord -> screen UV);
    // .z = scene-copy max mip level (frost lod clamp), .w unused. ctrl = dev cvar
    // OVERRIDES: x = refraction scale, y = roughness add, z = specular scale, w = 1
    // when any override is active (else 0 -> shader uses the per-object material).
    struct GlassControl {
        glm::vec4 camPos;     // xyz = camera world pos, w = time (seconds)
        glm::vec4 screen;     // x = 1/W, y = 1/H, z = maxMip, w = sceneCopyValid (0/1)
        glm::vec4 ctrl;       // x = refractScale, y = roughAdd, z = specScale, w = overrideOn
        glm::vec4 camRight;   // xyz = camera RIGHT axis (world) — screen-space normal.x
        glm::vec4 camUp;      // xyz = camera UP axis (world)    — screen-space normal.y
    };
    // Live dev-cvar glass overrides (r_glass_*), pushed via setGlassDevParams.
    GlassDevParams m_glassDev{};
    // Wall-clock seconds since device init, for the animated specular glint (M3).
    std::chrono::steady_clock::time_point m_glassClockStart = std::chrono::steady_clock::now();

    // ---- Procedural planet body (FORGE3D port) — dedicated per-TYPE pipelines.
    // Each clones the OPAQUE mesh PSO (same MeshVertex input, depth LESS+write, cull
    // BACK, no blend) but with planet.vert + a per-type fragment shader, all sharing
    // the SAME layout (set0 bindless + set1 camera UBO + a 128-byte push constant).
    // The 9 OPAQUE planet types, in pipeline-index order:
    enum PlanetType : uint32_t {
        PT_Moon = 0, PT_Ice, PT_Gas, PT_Lava, PT_Terrestrial,
        PT_Oceanic, PT_Sand, PT_Thunderstorm, PT_Sun,
        // TRANSPARENT glow layers (drawn AFTER the opaque bodies, compositing OVER
        // them). Atmosphere/SunCorona = ADDITIVE shells; Ring = ALPHA annulus.
        PT_Atmosphere, PT_SunCorona, PT_Ring,
        PT_Count,
        PT_OpaqueCount = PT_Atmosphere   // [0..PT_OpaqueCount) are the 9 opaque bodies
    };
    VkPipeline       m_planetPipelines[PT_Count] = {};   // [0] is the old m_planetPipeline
    VkPipelineLayout m_planetPipelineLayout = VK_NULL_HANDLE;
    // Generalized push-constant block (mirrors PC {} in planet.vert + every per-type
    // frag): mat4 model (64B) + uint tex[12] (48B) + float uTime (4B) + float _p0 +
    // uint _p1 + uint _p2 (12B) = 128B exactly (the full push-constant range).
    struct PlanetPush {
        float    model[16];   // 64B
        uint32_t tex[12];     // 48B — bindless texture indices, per-type slot mapping
        float    uTime;       //  4B
        float    _p0;         //  4B
        uint32_t _p1;         //  4B
        uint32_t _p2;         //  4B
    };
    static_assert(sizeof(PlanetPush) == 128, "PlanetPush must be 128 bytes (push-constant range)");
    // One queued planet draw for THIS frame (resolved bindless indices + mesh id +
    // which per-type pipeline to bind). tex[] holds up to 12 resolved bindless idx.
    struct PlanetDraw {
        float    model[16];
        uint32_t tex[12];
        float    uTime;
        uint32_t typeIndex;   // PlanetType -> which m_planetPipelines[] to bind
        uint32_t meshId;
    };
    std::vector<PlanetDraw> m_planetDraws;

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
    // Per-indirect-command flag: group contains >=1 alphaMode==MASK (cutout)
    // instance -> the reflections depth pre-pass uses the alpha-testing pipeline
    // for it (recordDepthPrePassBody). Parallel to m_drawMeshOrder.
    uint8_t                 m_drawMeshCutout[kMaxDrawMeshes] = {};
    // Per-frame data preparation (camera/light UBO + SSBO + indirect) is shared by
    // the shadow depth pass AND the main color pass, so it runs once (guarded) and
    // caches the number of indirect commands produced.
    bool                    m_framePrepared = false;
    uint32_t                m_frameCmdCount = 0;
    uint32_t                m_frameCmdOpaque = 0;  // [0,opaque)=opaque cmds, [opaque,count)=BLEND (glass)
    // Number of GLASS-flagged instances submitted this frame (counted in the SSBO
    // fill). When 0 the transparent glass pass is skipped entirely (spec §5: zero
    // cost when no glass is visible).
    uint32_t                m_frameGlassCount = 0;

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
    float                 m_skyTime = 0.0f;  // sky-animation time (seconds), -> sky UBO params.z

    // ---- Image-based lighting (IBL) — split-sum environment reflections ----
    // Foundation for SSR + the RT tier. Three cube/2D resources baked from the
    // analytic sky: a diffuse IRRADIANCE cube (32px), a roughness-mipped specular
    // PREFILTER cube (128px base, kIblPrefilterMips), and a scene-independent BRDF
    // LUT (256px RG16F). An intermediate ENV cube (256px, mipped) is captured from
    // the analytic-sky math, then convolved. mesh.frag set 4 samples these for
    // split-sum IBL (replacing the flat ambient*Fresnel constant). The bakes run
    // on a one-time submit at init + whenever setSkyParams changes the sky, so the
    // per-frame cost is zero (only the three texture fetches in the shader).
    static constexpr uint32_t kIblEnvSize        = 256;  // env cube face edge
    static constexpr uint32_t kIblIrradSize      = 32;   // irradiance cube face edge
    static constexpr uint32_t kIblPrefilterSize  = 128;  // prefilter cube mip0 edge
    static constexpr uint32_t kIblPrefilterMips  = 5;    // roughness mip count (128->8)
    static constexpr uint32_t kIblBrdfSize       = 256;  // BRDF LUT edge
    static constexpr VkFormat kIblCubeFormat     = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat kIblBrdfFormat     = VK_FORMAT_R16G16_SFLOAT;

    bool m_iblReady = false;          // all resources created (mesh.frag may sample)
    bool m_iblBaked = false;          // a valid environment was baked at least once
    bool m_iblDirty = true;           // sky changed -> rebake on next frame prep
    // Env (intermediate) cube: captured analytic sky, full mip chain for prefilter.
    VkImage     m_iblEnvImg = VK_NULL_HANDLE;  VmaAllocation m_iblEnvAlloc = nullptr;
    VkImageView m_iblEnvCubeView = VK_NULL_HANDLE;            // CUBE view (all mips) for sampling
    VkImageView m_iblEnvFaceView[6] = {};                    // per-face mip0 RT views (capture)
    uint32_t    m_iblEnvMips = 1;
    // Irradiance cube (diffuse IBL).
    VkImage     m_iblIrradImg = VK_NULL_HANDLE; VmaAllocation m_iblIrradAlloc = nullptr;
    VkImageView m_iblIrradCubeView = VK_NULL_HANDLE;
    VkImageView m_iblIrradFaceView[6] = {};
    // Prefilter cube (specular IBL), one RT view per (mip,face).
    VkImage     m_iblPrefImg = VK_NULL_HANDLE;  VmaAllocation m_iblPrefAlloc = nullptr;
    VkImageView m_iblPrefCubeView = VK_NULL_HANDLE;
    VkImageView m_iblPrefFaceView[kIblPrefilterMips][6] = {};
    // BRDF LUT (2D RG16F).
    VkImage     m_iblBrdfImg = VK_NULL_HANDLE;  VmaAllocation m_iblBrdfAlloc = nullptr;
    VkImageView m_iblBrdfView = VK_NULL_HANDLE;
    // Samplers: cube (linear+mip, clamp) shared by env/irradiance/prefilter; 2D for the LUT.
    VkSampler   m_iblCubeSampler = VK_NULL_HANDLE;
    VkSampler   m_iblBrdfSampler = VK_NULL_HANDLE;
    // Pipelines (fullscreen-triangle, render to one cube face / mip at a time).
    VkPipeline       m_iblEnvPipe = VK_NULL_HANDLE;
    VkPipeline       m_iblIrradPipe = VK_NULL_HANDLE;
    VkPipeline       m_iblPrefPipe = VK_NULL_HANDLE;
    VkPipeline       m_iblBrdfPipe = VK_NULL_HANDLE;
    VkPipelineLayout m_iblEnvLayout = VK_NULL_HANDLE;     // UBO set0 + face push
    VkPipelineLayout m_iblCubeLayout = VK_NULL_HANDLE;    // cube sampler set0 + face/misc push
    VkPipelineLayout m_iblBrdfLayout = VK_NULL_HANDLE;    // no sets, no push
    // set0 for the IBL UBO (env capture) + set0 for a cube sampler (convolve passes).
    VkDescriptorSetLayout m_iblSkyUboSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_iblCubeSetLayout   = VK_NULL_HANDLE;
    VkDescriptorPool      m_iblBakePool = VK_NULL_HANDLE;
    VkDescriptorSet       m_iblSkyUboSet = VK_NULL_HANDLE;   // -> env capture
    VkDescriptorSet       m_iblEnvCubeSet = VK_NULL_HANDLE;  // env cube -> irradiance/prefilter
    VkBuffer m_iblSkyUboBuf = VK_NULL_HANDLE; VmaAllocation m_iblSkyUboAlloc = nullptr; void* m_iblSkyUboMapped = nullptr;
    // mesh.frag set 4 (IBL): binding0 irradiance cube, binding1 prefilter cube, binding2 BRDF LUT.
    VkDescriptorSetLayout m_iblMeshSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_iblMeshPool = VK_NULL_HANDLE;
    VkDescriptorSet       m_iblMeshSet = VK_NULL_HANDLE;

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

    // ---- BOOT-TIME upload batching (docs/BOOT_TIME.md) ----------------------
    // While m_batchActive, createDeviceLocalBuffer/createSampledTexture record
    // into m_batchCmd instead of one blocking oneTimeSubmit each; staging buffers
    // are kept alive in m_batchStagings until the single flush. oneTimeSubmit and
    // beginFrame auto-flush a pending batch first, so any GPU op that could read
    // batched data executes strictly after the uploads land.
    bool          m_batchActive = false;            // begin/endUploadBatch window
    bool          m_batchOpen   = false;            // current slot has recorded work
    uint32_t      m_batchSlot   = 0;                // recording slot (double-buffered)
    bool          m_batchSubmittedSlot[2] = { false, false };
    VkCommandBuffer m_batchCmds[2]  = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkFence         m_batchFences[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    std::vector<std::pair<VkBuffer, VmaAllocation>> m_batchStagings;        // recording
    std::vector<std::pair<VkBuffer, VmaAllocation>> m_batchInFlightSlot[2]; // submitted
    uint32_t      m_batchOps    = 0;                // uploads in the current batch
    uint32_t      m_batchFlushes = 0;               // receipts: flushes this batch window
    double        m_batchMs     = 0.0;              // receipts: wall ms spent flushing
    // Upload-path guard: createMesh/createTexture/destroyMesh/destroyTexture/
    // oneTimeSubmit may be called from the BOOT-TIME parallel model-preload
    // threads (x3::asset::preloadModels). Recursive: createMesh -> oneTimeSubmit
    // re-enters on the unbatched path. Frame/draw/skinning paths remain main-
    // thread-only (unchanged contract).
    std::recursive_mutex m_uploadMu;

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

    // ---- Scene-color COPY target (translucent GLASS, spec §3.1) ------------
    // A full-res, SINGLE-MIP copy of the opaque HDR scene, captured AFTER the main
    // (+water) pass and BEFORE the glass pass, so glass.frag can SAMPLE the scene
    // behind it (screen-space refraction M2) while WRITING the same HDR target — you
    // cannot sample + write one image in a single pass. HDR format; usage
    // TRANSFER_DST (vkCmdCopyImage target) + SAMPLED + TRANSFER_SRC (M4 frost reads
    // it as the blur source). The render-graph tracks ONE layout per image (mip0
    // only), so the frost blur uses SEPARATE images (m_glassFrostImg[], like the
    // bloom mips) rather than this image's deeper mips. Created/destroyed alongside
    // the bloom targets (tracks the swapchain extent; keeps allocationCount=0).
    VkImage       m_sceneCopyImg   = VK_NULL_HANDLE;
    VmaAllocation m_sceneCopyAlloc = nullptr;
    VkImageView   m_sceneCopyView  = VK_NULL_HANDLE;    // single-mip view (refraction sample)
    VkSampler     m_glassCopySampler = VK_NULL_HANDLE;  // LINEAR, CLAMP
    // Frost (M4): a short chain of separately-allocated, progressively-downsampled
    // blur images of the scene copy (kGlassFrostLevels). Each is single-mip (the
    // graph tracks per-image layout), like the bloom mips. The glass shader samples
    // the deepest level for the frosted look and lerps to the sharp copy by roughness.
    VkImage       m_glassFrostImg[kGlassFrostLevels]   = {};
    VmaAllocation m_glassFrostAlloc[kGlassFrostLevels] = {};
    VkImageView   m_glassFrostView[kGlassFrostLevels]  = {};
    VkExtent2D    m_glassFrostExt[kGlassFrostLevels]   = {};

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
    struct CompositePush { float bloomIntensity, exposure; int32_t tonemapMode, aeEnabled;
                           float sharpen, texelW, texelH, pad0; };
    BloomPush m_bloomDownPush[kBloomMips]{};
    BloomPush m_bloomUpPush[kBloomMips]{};

    // ---- TAA (temporal anti-aliasing) ---------------------------------------
    // Full-res HDR resolve OUTPUT (render target; AE/bloom/composite sample it;
    // TRANSFER_SRC for the history refresh) + persistent HISTORY (TRANSFER_DST +
    // sampled by the next frame's resolve). The history's cross-frame layout is
    // tracked in m_taaHistState (shadow-map pattern). Jitter phase is a pure
    // frame counter (deterministic headless captures); the previous UNJITTERED
    // view-proj + camera pose feed reprojection + camera-cut detection.
    VkImage       m_taaOutImg   = VK_NULL_HANDLE;
    VmaAllocation m_taaOutAlloc = nullptr;
    VkImageView   m_taaOutView  = VK_NULL_HANDLE;
    VkImage       m_taaHistImg   = VK_NULL_HANDLE;
    VmaAllocation m_taaHistAlloc = nullptr;
    VkImageView   m_taaHistView  = VK_NULL_HANDLE;
    ResourceState m_taaHistState{ VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 };
    bool          m_taaHistoryValid     = false;  // false -> resolve passes through
    bool          m_taaActiveThisFrame  = false;  // set per frame in buildAndExecuteGraph
    uint32_t      m_taaFrameNum         = 0;      // Halton phase (frame counter)
    glm::mat4     m_taaPrevVP{ 1.0f };            // previous UNJITTERED view-proj
    glm::vec3     m_taaPrevCamPos{ 0.0f };
    float         m_taaPrevYaw = 0.0f, m_taaPrevPitch = 0.0f, m_taaPrevFov = 0.0f;
    bool          m_taaPrevCamValid = false;
    struct TaaUBO {
        glm::mat4 invViewProjCur;   // current JITTERED clip -> world
        glm::mat4 viewProjPrev;     // world -> previous UNJITTERED clip
        glm::vec4 params0;          // texelW, texelH, historyValid, blend
        glm::vec4 params1;          // jitterX, jitterY (px, debug), unused
    };
    VkDescriptorSetLayout m_taaSetLayout = VK_NULL_HANDLE;  // b0-2 samplers + b3 UBO
    VkPipelineLayout      m_taaLayout    = VK_NULL_HANDLE;
    VkPipeline            m_taaPipe      = VK_NULL_HANDLE;
    VkSampler             m_taaDepthSampler = VK_NULL_HANDLE;  // NEAREST clamp (depth as data)
    VkDescriptorSet       m_taaSet[kFramesInFlight] = {};      // per-frame resolve inputs
    VkBuffer              m_taaUboBuf[kFramesInFlight] = {};
    VmaAllocation         m_taaUboAlloc[kFramesInFlight] = {};
    void*                 m_taaUboMapped[kFramesInFlight] = {};
    VkDescriptorSet       m_setTaaOut       = VK_NULL_HANDLE;  // bloom src (TAA output)
    VkDescriptorSet       m_setCompositeTaa = VK_NULL_HANDLE;  // composite b0 = TAA output
    VkDescriptorSet       m_aeSetTaa        = VK_NULL_HANDLE;  // AE b0 = TAA output
    VkRenderingAttachmentInfo m_taaAttach{};                   // stable storage for the graph
    VkRenderingInfo           m_taaRenderInfo{};

    // ---- AUTO-EXPOSURE (eye adaptation) ------------------------------------
    // A single-workgroup compute pass (autoexposure.comp) reduces a fixed 64x64
    // log-luminance sample grid over the HDR scene to an average, maps it to a
    // target exposure (key/avg, clamped), and temporally adapts a persistent
    // 16-byte SSBO value the composite multiplies in (exposure = adapted *
    // r_exposure bias). Buffers are not graph-tracked (documented model), so the
    // record body emits its own tiny SSBO barriers. Determinism: the adaptation
    // SNAPS to the target on the first frame, whenever AE is toggled on, and on
    // EVERY headless frame (reproducible --screenshot*/--test captures).
    VkDescriptorSetLayout m_aeSetLayout = VK_NULL_HANDLE;  // b0 = HDR sampler, b1 = SSBO (compute)
    VkDescriptorSet       m_aeSet       = VK_NULL_HANDLE;
    VkPipelineLayout      m_aeLayout    = VK_NULL_HANDLE;
    VkPipeline            m_aePipe      = VK_NULL_HANDLE;
    VkBuffer              m_aeBuf       = VK_NULL_HANDLE;  // { adapted, avgLog, pad, pad }
    VmaAllocation         m_aeAlloc     = nullptr;
    bool                  m_aeSnap      = true;            // snap-to-target on next AE frame
    double                m_aePrevTime  = -1.0;            // steady_clock seconds (dt source)
    struct AePush { float dt, speed, minExp, maxExp, key; int32_t snap; float pad0, pad1; };
    AePush                m_aePush{};
    PostFXParams          m_post{};                        // live r_* post-stack settings
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
    // Glass frost-blur chain (M4): stable per-level storage (one per frost level).
    VkRenderingAttachmentInfo m_glassFrostAttach[kGlassFrostLevels]{};
    VkRenderingInfo           m_glassFrostRenderInfo[kGlassFrostLevels]{};
    BloomPush                 m_glassFrostPush[kGlassFrostLevels]{};
    BloomPassCtx              m_glassFrostCtx[kGlassFrostLevels]{};
    RgResource                m_glassFrostRg[kGlassFrostLevels]{};   // per-frame graph handles

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
    // Tiny control block fed to mesh.frag (set3/binding1): ctrl = {x=enabled,
    // y=strength, z=1/screenW, w=1/screenH}; ibl = {x=IBL valid(0/1), y=IBL
    // intensity, z=prefilter max mip, w=reserved}. The ibl lane lets mesh.frag fall
    // back to the old flat ambient when no environment is baked (other paths/headless
    // failure) without a separate UBO.
    // mesh.frag set3/binding1 control block. The ddgi* lanes (r_ddgi) carry the
    // probe-grid gate + geometry so the fragment stage can interpolate the DDGI
    // atlases; all zero when inactive (the ambient math is then byte-identical).
    struct SsaoControl {
        glm::vec4 ctrl; glm::vec4 ibl; glm::vec4 refl;
        glm::vec4 ddgiCtrl;     // x = active, y = intensity (ramped), z = debug, w = bias scale
        glm::vec4 ddgiOrigin;   // xyz = grid min corner, w = visMaxDist
        glm::vec4 ddgiSpacing;  // xyz = probe spacing
        glm::vec4 ddgiCounts;   // xyz = probe counts (float)
        // RT soft shadows (r_rtshadows) — read ONLY by the mesh_rt.frag variant;
        // all zero when inactive (the plain variant never references them).
        glm::vec4 rtsh0;        // x = tier, y = tan(sun angular radius), z = point ray budget K, w = light source radius (m)
        glm::vec4 rtsh1;        // x = frame seed (0 when TAA is off -> static dither)
    };
    // Half-res AO targets: raw (ssao.frag output) + blurred (ssao_blur output,
    // sampled by mesh.frag). Both R8, recreated with the frame extent.
    VkImage       m_ssaoRawImg  = VK_NULL_HANDLE; VmaAllocation m_ssaoRawAlloc  = nullptr; VkImageView m_ssaoRawView  = VK_NULL_HANDLE;
    VkImage       m_ssaoBlurImg = VK_NULL_HANDLE; VmaAllocation m_ssaoBlurAlloc = nullptr; VkImageView m_ssaoBlurView = VK_NULL_HANDLE;
    VkExtent2D    m_ssaoExtent{};                       // half the frame extent
    // Depth pre-pass pipeline (depth.vert; set0 = objSet, reuses m_shadowLayout).
    VkPipeline    m_depthPrePipeline = VK_NULL_HANDLE;
    // Alpha-cutout depth pre-pass variant (depth_cutout.vert/.frag): fragment-
    // stage alpha test so billboard depth matches mesh.frag's cutout discard.
    // Own layout (set0 = objSet, set1 = bindless). Reflections frames only.
    VkPipeline       m_depthPreCutoutPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_depthPreCutoutLayout   = VK_NULL_HANDLE;
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

    // Stable storage for the GLASS (transparent) pass's VkRenderingInfo +
    // attachments (post-opaque; color = HDR LOAD, depth = read-only LEQUAL).
    VkRenderingAttachmentInfo m_glassColorAttach{};
    VkRenderingAttachmentInfo m_glassDepthAttach{};
    VkRenderingInfo           m_glassRenderInfo{};

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
    bool                  m_shadowOverride = false;        // setShadowBounds: fixed shadow box
    glm::vec3             m_shadowCenter{ 0.0f };
    float                 m_shadowOrtho = kShadowOrtho;
    float                 m_shadowDepthHalf = kShadowDepthHalf;
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

    // ---- Editor UI (Dear ImGui, docking) — EDITOR-ONLY ---------------------
    // All default null/false so a NON-editor run (no initEditorUI call) allocates
    // nothing and the editor-UI graph pass is never added. The descriptor pool is
    // DEDICATED to ImGui (never shared with the bindless/HUD pools — sharing would
    // corrupt them). m_editorDrawData is filled by endEditorUI() (CPU draw-data
    // build, OUTSIDE the command buffer) and consumed by the editor-UI graph pass
    // inside buildAndExecuteGraph() — mirrors prepareFrameData()->recordMeshDraws().
    VkDescriptorPool m_imguiPool      = VK_NULL_HANDLE;
    bool             m_imguiInit      = false;
    ImDrawData*      m_editorDrawData = nullptr;
    // Stable storage for the editor-UI pass's VkRenderingInfo + color attachment so
    // they outlive execute() (the graph holds pointers into these — same pattern as
    // the composite pass's m_compositeRenderInfo / m_compositeAttach above).
    VkRenderingInfo           m_editorUiRenderInfo{};
    VkRenderingAttachmentInfo m_editorUiAttach{};

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
        // Parallel-preload safe: the deferred queues are appended by destroyMesh/
        // destroyTexture, which the boot-time preload threads call via unload().
        std::lock_guard<std::recursive_mutex> lk(m_uploadMu);
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

    // ---- ZERO-STUTTER state (docs/ZERO_STUTTER.md) -------------------------
    // Persistent pipeline cache + the late-creation audit + frame-pacing ring.
    VkPipelineCache m_pipelineCache    = VK_NULL_HANDLE;
    uint64_t        m_cacheLoadedBytes = 0;       // bytes accepted from disk at boot
    double          m_psoCreateMs      = 0.0;     // wall ms spent in vkCreate*Pipelines
    bool            m_firstFrameBegun  = false;   // set by the first beginFrame()
    bool            m_creationBoundary = false;   // inside a declared recreate boundary
    PacingParams    m_pacing{};                   // cvar-driven (setPacingParams)
    // Totals + late-creation audit counters (late = after the first frame began,
    // outside a declared boundary — these are the zero-stutter receipts).
    uint32_t m_psoTotal = 0, m_psoLate = 0, m_modulesLate = 0, m_poolsLate = 0;
    // Per-frame attribution counters (reset in beginFrame, read by the spike log).
    uint32_t m_psoThisFrame = 0, m_modulesThisFrame = 0, m_poolsThisFrame = 0,
             m_allocsThisFrame = 0, m_asBuildsThisFrame = 0;
    bool m_iblBakedThisFrame = false, m_recreatedThisFrame = false;
    // Frame-time ring (post-warmup samples only; CPU = endFrame->endFrame wall).
    static constexpr uint32_t kPaceRingCap = 4096;
    struct PaceSample { float cpuMs; float gpuMs; };
    std::vector<PaceSample> m_paceRing;
    uint32_t m_paceWrite = 0;
    bool     m_paceHaveLast = false;
    std::chrono::steady_clock::time_point m_paceLast{};
    uint32_t m_spikeCount = 0;       // all post-warmup spikes (logged)
    uint32_t m_spikeCleanCount = 0;  // spikes with NO attributed cause (the gate)

    bool m_vsync = true;
    bool m_needsRecreate = false;
    uint32_t m_width = 0, m_height = 0;
    uint32_t m_ssaa = 1, m_outW = 0, m_outH = 0;   // SSAA: m_width = m_outW*ssaa; downscale on capture

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
    // Final additive bloom strength; defaults to the global subtle value, per-scene
    // override via setBloom() (the showroom raises it for the glowing-spire hero look).
    float                   m_bloomIntensity = kBloomIntensity;
    float                   m_exposure = 1.0f;   // whole-scene brightness (composite pre-tonemap)
    // CPU per-object frustum cull (r_frustumcull). Default ON. m_frameFrustum is the
    // 6 normalized world-space planes for the frame being prepared (filled from the
    // camera viewProj in prepareFrameData, consumed by emitGroup).
    bool                    m_frustumCull = true;
    FrustumPlanes           m_frameFrustum{};

    // ---- D15 GPU-driven culling (r_cullpath) -------------------------------
    // m_cullPathReq is the host request (-1 auto / 0 CPU / 1 Tier0 / 2 Tier1 /
    // 3 Tier2); m_cullPathActive is the per-frame RESOLVED path (clamped to what
    // the device + bring-up stage support), latched once in prepareFrameData and
    // consumed by emitGroup + the graph build so a mid-frame cvar change can
    // never tear the frame. Default 0: byte-identical to pre-D15 behavior.
    GpuCullSystem           m_gpuCull;
    CullDeviceCaps          m_cullCaps{};
    bool                    m_gpuCullReady = false;   // pipelines + sets built
    VkDescriptorPool        m_cullPool = VK_NULL_HANDLE;
    VkSampler               m_hzbSampler = VK_NULL_HANDLE;  // NEAREST, clamp, all mips
    int                     m_cullPathReq = 0;
    int                     m_cullPathActive = 0;
    uint32_t                m_frameCullInstances = 0;  // rows the cull pass dispatches over
    // Tier 1 (async compute): the dedicated queue + a timeline semaphore the
    // graphics submit waits on at DRAW_INDIRECT|VERTEX_SHADER.
    VkQueue                 m_computeQueue = VK_NULL_HANDLE;
    uint32_t                m_computeFamily = VK_QUEUE_FAMILY_IGNORED;
    bool                    m_asyncCullReady = false;
    VkSemaphore             m_cullTimeline = VK_NULL_HANDLE;
    uint64_t                m_cullTimelineValue = 0;
    bool                    m_asyncCullThisFrame = false;
    CullFrameInputs         m_asyncCullInputs{};
    bool                    m_hzbEnabled = false;     // r_hzb (needs path >= 1)
    bool                    m_hzbActiveThisFrame = false; // resolved per frame
    // HZB depth pyramid (R32_SFLOAT, full mip chain, GENERAL layout forever).
    // Built from LAST frame's depth at the top of this frame's command buffer
    // (one-frame occlusion latency — disocclusions resolve next frame).
    static constexpr uint32_t kHzbMaxMips = 16;
    VkImage                 m_hzbImg = VK_NULL_HANDLE; VmaAllocation m_hzbAlloc = nullptr;
    VkImageView             m_hzbViewAll = VK_NULL_HANDLE;          // all mips (cull samples)
    VkImageView             m_hzbMipView[kHzbMaxMips] = {};        // one mip each (reduce dst/src)
    VkDescriptorSet         m_hzbMipSet[kHzbMaxMips] = {};
    VkDescriptorPool        m_hzbPool = VK_NULL_HANDLE;
    uint32_t                m_hzbMipCount = 0;
    uint32_t                m_hzbW = 0, m_hzbH = 0;                 // mip 0 dims
    bool                    m_hzbReady = false;
    // Last frame's depth: tracked post-graph state + "has been rendered once"
    // (the pyramid must never reduce an UNDEFINED depth image).
    ResourceState           m_depthState{};
    bool                    m_depthValid = false;
    GpuCullSystem::HzbChain m_hzbChain{};   // stable record-ctx storage
    bool                    m_cullEquivCheck = false; // --test-gpucull harness
    // Latest read-back cull counters (frames-in-flight latency) + equivalence.
    CullStatsGpu            m_lastCullStats{};
    uint32_t                m_lastCullExpected = 0;
    uint32_t                m_cullEquivFrames = 0;
    uint32_t                m_cullEquivMismatches = 0;
    float                   m_metalAmbient = 1.0f; // metal ambient-spec floor strength (mesh.frag ibl.w; r_metalambient)
    // ---- vis-unify: host-injected PVS numbers + per-stage timing -----------
    uint32_t                m_visRoomsCulled = 0;   // setVisHostStats (this frame's room/portal skips)
    float                   m_visPvsMs = 0.0f;      // setVisHostStats (flood-fill ms)
    float                   m_cullCpuMs = 0.0f;     // device emit/cull walk CPU time (0 = not yet measured)
    float                   m_cullGpuMs = 0.0f;     // cull.comp dispatch GPU time
    float                   m_hzbGpuMs = 0.0f;      // HZB reduce GPU time
    // ---- vis-unify: TLAS mutation instrumentation (folded base still sync-waits) --
    uint32_t                m_tlasBuilds = 0;
    uint32_t                m_tlasSyncWaits = 0;
    uint32_t                m_tlasGrows = 0;
    float                   m_tlasCpuMs = 0.0f;
    float                   m_tlasCpuMsMax = 0.0f;
};

} // namespace

IRenderDevice* createRenderDevice() { return new VulkanRenderDevice(); }

} // namespace x3::rhi
