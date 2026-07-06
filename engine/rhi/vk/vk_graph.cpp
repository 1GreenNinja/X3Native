// #28 monolith split — VulkanRenderDevice render-graph assembly (out-of-line).
// buildAndExecuteGraph: declares every pass + its read/write resources in the
// energy-conserving order and executes the RenderGraph. Pure graph structure +
// vkCmd orchestration (no per-pixel FP). Plus the bloom/glass-frost pass adders
// and the viewport helper. Bodies moved verbatim; see the internal header.
#include "VulkanRenderDevice_internal.h"
namespace x3::rhi {
void VulkanRenderDevice::addBloomPasses(RgResource rgHdr, RgResource* rgMip) {
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

void VulkanRenderDevice::addGlassFrostPasses(RgResource rgSceneCopy) {
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

void VulkanRenderDevice::postViewport(VkCommandBuffer c, VkExtent2D ext) {
        VkViewport vp{ 0.0f, 0.0f, (float)ext.width, (float)ext.height, 0.0f, 1.0f };
        VkRect2D scis{ {0,0}, ext };
        vkCmdSetViewport(c, 0, 1, &vp);
        vkCmdSetScissor(c, 0, 1, &scis);
    }

void VulkanRenderDevice::buildAndExecuteGraph(VkCommandBuffer cmd, uint32_t imageIndex, bool wantCapture) {
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
        // VELOCITY pre-pass (#4): per-object motion vectors for the TAA resolve.
        // Requires TAA (only consumer), the depth pre-pass (EQUAL needs the depth
        // buffer pre-populated), the velocity pipeline + target (graceful: absent
        // .spv => disabled), and r_velocity. When OFF, TAA reprojects camera-only,
        // byte-identical to the pre-velocity path (the b4 sampler is ignored via
        // the params1.z gate). The pass writes m_velImg between the depth pre-pass
        // and the TAA resolve.
        const bool velOn = taaOn && prePassOn && m_post.velocity
                        && (m_velPipe != VK_NULL_HANDLE) && (m_velImg != VK_NULL_HANDLE);
        m_velActiveThisFrame = velOn;
        RgResource rgVel = {};
        if (velOn) rgVel = m_graph.importImage("scene.velocity", m_velImg,
            ResourceState{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 });
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
            // Pass: VELOCITY pre-pass (#4) -> m_velImg (RG16F). Re-rasterizes the
            // opaque draws with depth-test EQUAL (read-only depth) and writes the
            // per-object screen-space motion vector. Runs right after the depth
            // pre-pass populated the depth buffer; the TAA resolve samples it.
            if (velOn) {
                m_velAttach = VkRenderingAttachmentInfo{};
                m_velAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                m_velAttach.imageView = m_velView;
                m_velAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                m_velAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;   // gaps (sky) = zero MV
                m_velAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                m_velAttach.clearValue.color = { {0.0f, 0.0f, 0.0f, 0.0f} };

                RenderPassDesc vpass{};
                vpass.name = "velocity-prepass";
                vpass.addUse(ResourceUse{
                    rgVel, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/true });
                // Depth read-only (EQUAL test): EARLY/LATE fragment-test read.
                vpass.addUse(ResourceUse{
                    rgDepth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT, /*isWrite=*/false });
                vpass.usesDynamicRendering = true;
                m_velDepthAttach = VkRenderingAttachmentInfo{};
                m_velDepthAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                m_velDepthAttach.imageView = m_depthView;
                m_velDepthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
                m_velDepthAttach.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;     // keep prepass depth
                m_velDepthAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;  // preserved for later passes
                m_velRenderInfo = VkRenderingInfo{};
                m_velRenderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                m_velRenderInfo.renderArea = { {0,0}, m_extent };
                m_velRenderInfo.layerCount = 1;
                m_velRenderInfo.colorAttachmentCount = 1;
                m_velRenderInfo.pColorAttachments = &m_velAttach;
                m_velRenderInfo.pDepthAttachment = &m_velDepthAttach;
                vpass.renderInfo = m_velRenderInfo;
                vpass.recordCtx = this;
                vpass.record = [](void* ctx, VkCommandBuffer c){
                    static_cast<VulkanRenderDevice*>(ctx)->recordVelocityPassBody(c); };
                m_graph.addPass(std::move(vpass));
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
        // DEPTH FOG (ART_BIBLE §5 — atmospheric perspective). A fullscreen
        // Beer-Lambert extinction blended over the finished HDR scene, AFTER the
        // last HDR writer (particles) and BEFORE the TAA resolve so fogged
        // radiance enters temporal history + bloom like real scene light. HOST
        // OPT-IN ONLY (setFog): when disabled the pass is never added — worlds
        // that don't opt in render byte-identical (r_bloom-0 discipline).
        // ----------------------------------------------------------------
        const bool fogOn = m_fogParams.enabled && m_fogParams.density > 0.0f
                        && (m_fogPipe != VK_NULL_HANDLE);
        if (fogOn) {
            m_fogAttach = VkRenderingAttachmentInfo{};
            m_fogAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            m_fogAttach.imageView = m_hdrView;
            m_fogAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            m_fogAttach.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;    // blend over the scene
            m_fogAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            RenderPassDesc fog{};
            fog.name = "depth-fog";
            fog.addUse(ResourceUse{
                rgHdr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, /*isWrite=*/true });
            fog.addUse(ResourceUse{
                rgDepth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_IMAGE_ASPECT_DEPTH_BIT, /*isWrite=*/false });
            fog.usesDynamicRendering = true;
            m_fogRenderInfo = VkRenderingInfo{};
            m_fogRenderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            m_fogRenderInfo.renderArea = { {0,0}, m_extent };
            m_fogRenderInfo.layerCount = 1;
            m_fogRenderInfo.colorAttachmentCount = 1;
            m_fogRenderInfo.pColorAttachments = &m_fogAttach;
            fog.renderInfo = m_fogRenderInfo;
            fog.recordCtx = this;
            fog.record = [](void* ctx, VkCommandBuffer c){
                auto* self = static_cast<VulkanRenderDevice*>(ctx);
                self->postViewport(c, self->m_extent);
                vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, self->m_fogPipe);
                vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, self->m_fogLayout,
                                        0, 1, &self->m_setFog, 0, nullptr);
                FogPush fp{};
                fp.invProj      = self->m_fogInvProjCPU;
                fp.colorDensity = glm::vec4(self->m_fogParams.color[0],
                                            self->m_fogParams.color[1],
                                            self->m_fogParams.color[2],
                                            self->m_fogParams.density);
                fp.startMax     = glm::vec4(self->m_fogParams.start,
                                            self->m_fogParams.maxOpacity, 0.0f, 0.0f);
                vkCmdPushConstants(c, self->m_fogLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(fp), &fp);
                vkCmdDraw(c, 3, 1, 0, 0);
            };
            m_graph.addPass(std::move(fog));
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
                // Filmic grade (ART_BIBLE §5): strength 0 (the default) keeps the
                // shader's grade block un-entered -> byte-identical composite.
                const auto& gp = self->m_gradeParams;
                cp.gradeStrength    = std::clamp(gp.strength, 0.0f, 1.0f);
                cp.shadowTint[0]    = gp.shadowTint[0];
                cp.shadowTint[1]    = gp.shadowTint[1];
                cp.shadowTint[2]    = gp.shadowTint[2];
                cp.shadowTint[3]    = gp.saturation;
                cp.highlightTint[0] = gp.highlightTint[0];
                cp.highlightTint[1] = gp.highlightTint[1];
                cp.highlightTint[2] = gp.highlightTint[2];
                cp.highlightTint[3] = std::clamp(gp.vignette, 0.0f, 0.25f);
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

} // namespace x3::rhi
