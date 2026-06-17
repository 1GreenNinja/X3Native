// Vulkan implementation of IRenderDevice — D1 (clean-room).
// Spec: specs/D1-render-device.spec.md
//
// #28 MONOLITH SPLIT: the ~13.8k-line inline class was carved into a shared
// declaration header (vk/VulkanRenderDevice_internal.h) so its method bodies can
// be defined across several focused translation units under engine/rhi/vk/:
//   * vk_resources.cpp — buffers/images/samplers/descriptor pools + the tracked
//     allocation wrappers (allocationCount=0 accounting lives there).
//   * vk_pipelines.cpp — ALL PSO creation (boot-time, r_strictpso, pipeline cache).
//   * vk_passes.cpp    — depth/cutout prepass, buildRtSceneAS, DDGI, reflections,
//     RT shadows, and the post stack in energy-conserving order.
//   * vk_stb_impl.cpp  — the stb single-header implementations (image-write + ttf).
//   * VulkanRenderDevice.cpp (THIS TU) — device/swapchain/frame lifecycle +
//     orchestration, plus the createRenderDevice() factory.
// The transform is behavior-preserving: every body was moved verbatim, only the
// inline->out-of-line mechanics (qualify names, hoist shared helpers) changed.

#include "vk/VulkanRenderDevice_internal.h"

namespace x3::rhi {

IRenderDevice* createRenderDevice() { return new VulkanRenderDevice(); }

bool VulkanRenderDevice::init(const DeviceDesc& desc) {
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
        // Velocity pre-pass (#4): per-object screen-space motion vectors -> TAA.
        // After createBloomTargets (velocity target exists) + createGraphics (obj
        // buffers exist). Graceful: a missing velocity.spv leaves it disabled.
        createVelocityResources();
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

void VulkanRenderDevice::shutdown() {
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
        destroyVelocityResources();   // #4: before destroyGraphics frees m_objPool
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

void VulkanRenderDevice::initEditorUI(void* glfwWindow) {
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

void VulkanRenderDevice::beginEditorUI() {
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

void VulkanRenderDevice::endEditorUI() {
        if (!m_imguiInit) return;
        // CPU draw-data build OUTSIDE the command buffer; the editor-UI graph pass in
        // buildAndExecuteGraph() records ImGui_ImplVulkan_RenderDrawData against this.
        // Must run BEFORE endFrame() so the draw data exists when the graph records.
        ImGui::Render();
        m_editorDrawData = ImGui::GetDrawData();
    }

void VulkanRenderDevice::shutdownEditorUI() {
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

void VulkanRenderDevice::editorWantsInput(bool& mouse, bool& kbd) const {
        if (!m_imguiInit) { mouse = false; kbd = false; return; }
        const ImGuiIO& io = ImGui::GetIO();
        mouse = io.WantCaptureMouse;
        kbd   = io.WantCaptureKeyboard;
    }

bool VulkanRenderDevice::editorUIActive() const { return m_imguiInit; }

void VulkanRenderDevice::onResize(uint32_t w, uint32_t h) {
        if (w == 0 || h == 0) return;
        m_width = w; m_height = h;
        m_needsRecreate = true;
    }

void VulkanRenderDevice::setVsync(bool enabled) {
        // Headless has no swapchain: just record the desired state. Windowed:
        // flag a recreate so createSwapchain() picks the new present mode. No-op if
        // the value is unchanged (avoids a pointless device-idle stall).
        if (enabled == m_vsync) return;
        m_vsync = enabled;
        if (!m_headless) m_needsRecreate = true;
    }

void VulkanRenderDevice::setCamera(float x, float y, float z, float yaw, float pitch, float fovDeg) {
        m_camPos = glm::vec3(x, y, z);
        m_camYaw = yaw; m_camPitch = pitch; m_camFov = fovDeg;
    }

void VulkanRenderDevice::setAmbient(float r, float g, float b) { m_ambient = glm::vec3(r, g, b); }

void VulkanRenderDevice::setFrustumCullEnabled(bool enabled) { m_frustumCull = enabled; }

void VulkanRenderDevice::setCullPath(int path) { m_cullPathReq = path; }

void VulkanRenderDevice::setHzbEnabled(bool enabled) { m_hzbEnabled = enabled; }

void VulkanRenderDevice::setGpuCullEquivalenceCheck(bool enabled) {
        if (enabled && !m_cullEquivCheck) { m_cullEquivFrames = 0; m_cullEquivMismatches = 0; }
        m_cullEquivCheck = enabled;
    }

void VulkanRenderDevice::setBloom(float intensity) { m_bloomIntensity = intensity; }

void VulkanRenderDevice::setExposure(float e) { m_exposure = (e > 0.0f) ? e : 1.0f; }

void VulkanRenderDevice::setMetalAmbient(float s) { m_metalAmbient = (s >= 0.0f) ? s : 1.0f; }

void VulkanRenderDevice::setPostFX(const PostFXParams& p) {
        if (p.autoExposure && !m_post.autoExposure) m_aeSnap = true;
        // TAA toggled ON: the history image holds stale (or never-written) data —
        // invalidate so the first TAA frame is a clean passthrough, not a blend
        // against garbage. Toggling OFF needs nothing (the passes simply stop).
        if (p.taa && !m_post.taa) m_taaHistoryValid = false;
        m_post = p;
    }

bool VulkanRenderDevice::velocityEnabled() const { return m_post.velocity; }
bool VulkanRenderDevice::velocityAvailable() const {
        return m_velPipe != VK_NULL_HANDLE && m_velImg != VK_NULL_HANDLE;
    }

void VulkanRenderDevice::setShadowBounds(float cx, float cy, float cz, float halfExtent) {
        m_shadowOverride  = true;
        m_shadowCenter    = glm::vec3(cx, cy, cz);
        m_shadowOrtho     = halfExtent;
        m_shadowDepthHalf = halfExtent * 1.6f;   // deep enough for tall geometry + sun setback
    }

void VulkanRenderDevice::setIblProbe(bool enable) {
        if (m_iblProbeScene != enable) { m_iblProbeScene = enable; m_iblDirty = true; }
    }

void VulkanRenderDevice::setPointLights(const PointLight* lights, uint32_t count) {
        // Copy a clamped snapshot (we never retain the caller's pointer). The
        // cached set is re-uploaded into each frame's UBO by prepareFrameData, so
        // static lights only need one call. count==0 clears them.
        const uint32_t n = std::min<uint32_t>(count, kMaxPointLights);
        m_pointLights.assign(lights, lights + n);
    }

void VulkanRenderDevice::setSkyParams(const SkyParams& sp) {
        // Cache a snapshot; prepareFrameData() writes it into the per-frame sky UBO
        // and ensureMainPass() draws the full-screen sky when enabled. Disabled by
        // default, so indoor levels + every existing flag are unchanged.
        // IBL: if any sky term that feeds the environment radiance changed, flag the
        // IBL chain dirty so it rebakes the irradiance/prefilter cubes next frame.
        if (std::memcmp(&m_sky, &sp, sizeof(SkyParams)) != 0) m_iblDirty = true;
        m_sky = sp;
    }

void VulkanRenderDevice::setSkyTime(float t) { m_skyTime = t; }

bool VulkanRenderDevice::rayTracingSupported() const { return m_rtSupported; }

void VulkanRenderDevice::setRtaoParams(const RtaoParams& p) {
        // Cache a snapshot (re-applied each frame, like setSsaoParams). When RT is
        // unsupported this is a harmless no-op store: the graph never adds the RT
        // chain because m_rtSupported is false. The first time it is enabled on an
        // RT device, beginFrame() lazily inits the AS module + RT-AO pipelines.
        m_rtao = p;
        m_rtao.rays = std::max(1, std::min(32, m_rtao.rays));
    }

void VulkanRenderDevice::setReflectionParams(const ReflectionParams& p) {
        // Cache a snapshot (re-applied each frame, like setRtaoParams). The chain
        // is built LAZILY on first activation in prepareFrameData (a run that never
        // enables r_ssr pays zero init cost) and requires TAA to be active (the TAA
        // history image is the previous-frame color source). rtFallback additionally
        // requires m_rtSupported — Pascal-class devices get SSR-only automatically.
        m_refl = p;
        m_refl.intensity = std::max(0.0f, std::min(1.0f, m_refl.intensity));
    }

void VulkanRenderDevice::setDdgiParams(const DdgiParams& p) {
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

void VulkanRenderDevice::setRtShadowParams(const RtShadowParams& p) {
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

void VulkanRenderDevice::setSkinnedRtEnabled(bool enabled) {
        // r_skinnedrt: toggle whether visible skinned characters are added to the
        // RT scene TLAS (so RT shadows/reflections/DDGI/acoustics see them). When
        // OFF, buildRtSceneAS skips the skinned-BLAS pass entirely and the static
        // RT path is byte-identical to the pre-feature behavior. Harmless store on a
        // non-RT GPU (the whole RT block is gated by m_rtSupported regardless).
        m_skinnedRtEnabled = enabled;
}
bool VulkanRenderDevice::skinnedRtEnabled() const { return m_skinnedRtEnabled; }
uint32_t VulkanRenderDevice::skinnedRtInstanceCount() const { return m_skinnedRtInstances; }

void VulkanRenderDevice::setGlassDevParams(const GlassDevParams& p) {
        // Cache a snapshot of the live r_glass_* dev overrides; the glass control
        // UBO picks them up in prepareFrameData each frame.
        m_glassDev = p;
    }

bool VulkanRenderDevice::worldToScreen(float wx, float wy, float wz, float& sx, float& sy) const {
        const glm::vec4 clip = m_lastViewProj * glm::vec4(wx, wy, wz, 1.0f);
        if (clip.w <= 1e-4f) return false;
        const float nx = clip.x / clip.w, ny = clip.y / clip.w;
        if (nx < -1.3f || nx > 1.3f || ny < -1.3f || ny > 1.3f) return false;
        sx = (nx * 0.5f + 0.5f) * (float)m_extent.width;
        sy = (ny * 0.5f + 0.5f) * (float)m_extent.height;
        return true;
    }

void VulkanRenderDevice::setSsaoParams(const SsaoParams& sp) {
        // Cache a snapshot; prepareFrameData() bakes radius/bias/intensity/power
        // into the per-frame SSAO UBO + the mesh.frag control block, and
        // buildAndExecuteGraph gates the SSAO chain on `enabled`. Enabled by
        // default with tasteful values (no app wiring required for it to work).
        m_ssao = sp;
    }

void VulkanRenderDevice::setWaterParams(const WaterParams& wp) {
        // Cache a snapshot; prepareFrameData() writes it into the per-frame water
        // UBO and buildAndExecuteGraph adds the water pass when enabled. Disabled
        // by default, so indoor levels + every existing flag are unchanged.
        m_water = wp;
    }

void VulkanRenderDevice::setGiParams(const GiParams& gp) {
        // Cache a snapshot; prepareFrameData() bakes the tunables into the per-frame
        // GI UBO + temporal UBO, and buildAndExecuteGraph gates the GI chain on
        // `enabled`. Enabled by default with tasteful values (no app wiring required
        // for it to work, exactly like SSAO).
        m_gi = gp;
    }

void VulkanRenderDevice::submitParticles(const ParticleInstance* instances, uint32_t count,
                     ParticleBlend mode) {
        if (!instances || count == 0) return;
        std::vector<ParticleInstance>& dst =
            (mode == ParticleBlend::Alpha) ? m_partAlpha : m_partAdd;
        for (uint32_t i = 0; i < count; ++i) {
            if (dst.size() >= kMaxParticles) break;   // bounded; drop the overflow
            dst.push_back(instances[i]);
        }
    }

void VulkanRenderDevice::submitDecals(const DecalInstance* decals, uint32_t count) {
        if (!decals || count == 0) return;
        for (uint32_t i = 0; i < count; ++i) {
            if (m_decals.size() >= kMaxDecals) break;
            m_decals.push_back(decals[i]);
        }
    }

FrameContext VulkanRenderDevice::beginFrame() {
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

void VulkanRenderDevice::endFrame(const FrameContext& fc) {
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
        m_lastStats = m_building;

        // ZERO-STUTTER: record this frame in the pacing ring + spike log.
        recordFramePacing();
    }

RenderStats VulkanRenderDevice::stats() const { return m_lastStats; }

VulkanRenderDevice::FramePacing VulkanRenderDevice::framePacing() const {
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
        // TLAS double-buffer receipts (#5 PART 1): the ring should drive the
        // device-wait-per-build ratio to ~0 (boot does ONE wait; steady-state
        // skinned-RT rebuilds add none). tlasWaitsPerKBuild = 1000*waits/builds.
        p.tlasBuilds    = m_rt.tlasBuilds();
        p.tlasSyncWaits = m_rt.tlasSyncWaits();
        p.tlasCpuMs     = m_rt.tlasCpuMs();
        p.tlasWaitsPerKBuild = p.tlasBuilds
            ? (uint32_t)((1000ull * p.tlasSyncWaits) / p.tlasBuilds) : 0u;
        return p;
    }

void VulkanRenderDevice::setPacingParams(const PacingParams& pp) {
        m_pacing = pp;
        if (m_pacing.warmupFrames < 1) m_pacing.warmupFrames = 1;
        if (m_pacing.spikeFactor < 1.0f) m_pacing.spikeFactor = 1.0f;
        if (m_pacing.floorMs < 0.0f) m_pacing.floorMs = 0.0f;
    }

void VulkanRenderDevice::armCapture(const char* path) {
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

bool VulkanRenderDevice::captureFrame(const char* path) {
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

bool VulkanRenderDevice::writeCapturePng(const char* path, const void* mapped, uint32_t W, uint32_t H) {
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

bool VulkanRenderDevice::legacyCaptureLastPresented(const char* path) {
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

bool VulkanRenderDevice::supportsDescriptorIndexing() const { return m_descriptorIndexing; }

bool VulkanRenderDevice::supportsMeshShaders() const { return false; }

void VulkanRenderDevice::hudSize(uint32_t& outW, uint32_t& outH) const {
        outW = m_extent.width; outH = m_extent.height;
    }

bool VulkanRenderDevice::supportsGpuSkinning() const { return m_skinPipeline != VK_NULL_HANDLE; }

} // namespace x3::rhi
