// #28 monolith split — VulkanRenderDevice render passes + per-frame recording (out-of-line).
// Bodies moved verbatim from the inline class; only inline->out-of-line mechanics
// (VulkanRenderDevice:: qualification, default-arg/override stripping) changed.
// See VulkanRenderDevice_internal.h for the class declaration.
#include "VulkanRenderDevice_internal.h"

namespace x3::rhi {


void VulkanRenderDevice::recordWaterPassBody(VkCommandBuffer cmd) {
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

void VulkanRenderDevice::recordGlassPassBody(VkCommandBuffer cmd) {
        // Need the glass pipeline AND its set-4 resources; without set 4 the bind
        // would be invalid, so the whole pass is skipped (M1 alpha still works on the
        // frames where set 4 exists — it always does if createGlassResources passed).
        // W8-2: set 5 = the IBL set (env reflection); if IBL alloc ever failed the
        // pass skips too (graceful, same contract as a failed glass pipeline).
        if (!m_glassPipeline || !m_glassLayout || !m_glassSet[m_frameIdx] ||
            !m_iblMeshSet || m_frameCmdCount == 0) return;
        auto& fr = m_frames[m_frameIdx];
        postViewport(cmd, m_extent);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_glassPipeline);
        // The 4 shared mesh sets + the glass-only set 4 (scene-copy + GlassControl)
        // + the IBL set at 5 (prefiltered env + BRDF LUT — glass env reflection).
        VkDescriptorSet sets[6] = { m_bindlessSet, fr.objSet, m_shadowSet[m_frameIdx],
                                    m_meshAoSet[m_frameIdx], m_glassSet[m_frameIdx],
                                    m_iblMeshSet };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_glassLayout,
                                0, 6, sets, 0, nullptr);
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

void VulkanRenderDevice::recordDebrisComputeBody(VkCommandBuffer cmd) {
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

void VulkanRenderDevice::recordDebrisDrawBody(VkCommandBuffer cmd) {
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

void VulkanRenderDevice::recordSkinComputeBody(VkCommandBuffer cmd) {
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

void VulkanRenderDevice::recordParticlePassBody(VkCommandBuffer cmd) {
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

void VulkanRenderDevice::rewriteRtaoTlas(uint32_t slot) {
        VkAccelerationStructureKHR tlas = m_rt.tlas();
        if (!tlas) return;
        const uint32_t lo = (slot == kAllFrameSlots) ? 0u : slot;
        const uint32_t hi = (slot == kAllFrameSlots) ? kFramesInFlight : slot + 1u;
        for (uint32_t i = lo; i < hi && i < kFramesInFlight; ++i) {
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

bool VulkanRenderDevice::buildRtSceneAS() {
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
        // ---- SKINNED-CHARACTER TLAS REFIT (#3, r_skinnedrt) -------------------
        // Per-frame, build/refit a BLAS for each visible skinned character from its
        // CURRENT pose so monsters/NPCs enter the multi-consumer TLAS (RT shadows +
        // reflections + DDGI + RT acoustics). Budgeted (kSkinnedBlasBudget per
        // frame); REFIT (VK_..._MODE_UPDATE) after the first build, far cheaper than
        // a rebuild. Gated by m_skinnedRtEnabled (r_skinnedrt, default ON) AND
        // m_rtSupported; on a non-RT GPU (Pascal) or with the cvar OFF, skinned
        // chars stay raster-only and this whole block is skipped -> the static-only
        // path below is byte-identical to the pre-feature behavior.
        //
        // POSE LATENCY (documented, intentional): THIS frame's compute-skinning pass
        // is recorded INSIDE buildAndExecuteGraph(), which runs AFTER this function.
        // So we BLAS the most-recently-COMPLETED skinned output — the slot the GPU
        // wrote last frame (sm.lastSkinnedFrame), already retired (its inFlight fence
        // was waited in beginFrame). RT shadows/reflections/DDGI/audio for skinned
        // chars therefore lag the raster pose by exactly ONE frame — imperceptible
        // for shadows/AO/audio, and it avoids ANY new mid-frame device wait (the
        // skinned BLAS ride the existing batched-AS submit boundary). Reading a
        // not-yet-skinned mesh just yields the bind pose (seeded at register time).
        m_skinnedRtThisFrame = false;
        m_rt.resetSkinnedCounters();
        const bool wantSkinned = m_skinnedRtEnabled && !m_skinnedMeshes.empty();
        if (wantSkinned) {
            constexpr uint32_t kSkinnedBlasBudget = 64;   // cap chars touched/frame
            uint32_t skBuilt = 0;
            for (uint32_t mid : m_groupOrder) {
                if (skBuilt >= kSkinnedBlasBudget) break;
                auto sk = m_skinnedMeshes.find(mid);
                if (sk == m_skinnedMeshes.end()) continue;     // not a skinned mesh
                auto it = m_meshes.find(mid);
                if (it == m_meshes.end()) continue;
                const Mesh& m = it->second;
                if (!m.dynamic || m.indexCount < 3) continue;
                // Read the slot the compute pass last WROTE (retired). Before the
                // first dispatch, lastSkinnedFrame is ~0u -> use the current slot
                // (bind-pose-seeded), which is harmless (T-pose shadow until skinned).
                const SkinnedMesh& smr = sk->second;
                const uint32_t slot = (smr.lastSkinnedFrame < kFramesInFlight)
                                          ? smr.lastSkinnedFrame : m_frameIdx;
                VkBuffer vbo = m.dynVbo[slot];
                if (vbo == VK_NULL_HANDLE) continue;
                VkBufferDeviceAddressInfo vi{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO }; vi.buffer = vbo;
                VkBufferDeviceAddressInfo ii{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO }; ii.buffer = m.ibo;
                const VkDeviceAddress vbAddr = vkGetBufferDeviceAddress(m_dev.device, &vi);
                const VkDeviceAddress ibAddr = vkGetBufferDeviceAddress(m_dev.device, &ii);
                ++m_asBuildsThisFrame;   // ZERO-STUTTER spike-log attribution (skinned BLAS)
                if (m_rt.ensureSkinnedBlas(mid, vbAddr, m.vertexCount,
                                           (uint32_t)sizeof(MeshVertex), ibAddr, m.indexCount)) {
                    ++skBuilt;
                    m_skinnedRtThisFrame = true;
                }
            }
        }
        for (uint32_t mid : m_groupOrder) {
            if (m_rt.hasBlas(mid)) continue;
            auto it = m_meshes.find(mid);
            if (it == m_meshes.end()) continue;
            const Mesh& m = it->second;
            // Dynamic skinned meshes are handled by the skinned-BLAS pass above
            // (when r_skinnedrt is on); otherwise they stay raster-only and are
            // skipped here (the static-first path BLASes only static device-local
            // meshes). A dynamic mesh with no skinned BLAS contributes nothing.
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
        // PERF (measured, 33 skinned chars, RT GPU): cold = ~30 ms for 33 full BLAS
        // BUILDS (one-time warm-up, attributed to the batched-AS boundary); steady
        // state = ~2.0-2.6 ms for 33 REFITS incl. the fence wait (~60-75 us/char) —
        // ~12-15x cheaper than rebuilding, which is the whole point of MODE_UPDATE.
        if (deferred) {
            // More BLAS than this frame's budget: raster fallback until complete.
            char db[128];
            std::snprintf(db, sizeof(db),
                "[rt] BLAS warm-up: %u built this frame (budget %u) — raster fallback until complete",
                built, kBlasFrameBudget);
            logInfo(db);
            return false;
        }

        // Build the TLAS from this frame's instances. hasBlas() now returns true for
        // skinned meshes too (their per-frame skinned BLAS built above), so skinned
        // characters enter the TLAS automatically alongside static geometry.
        m_rtInstScratch.clear();
        m_rtInstScratch.reserve(m_drawRecords.size());
        m_skinnedRtInstances = 0;
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
            // GLASS TRANSMITS THE SUN. Glass records get mask 0x7F (bit 7 clear)
            // so the RT shadow rays (cullMask 0x80, mesh.frag rtshVisibility)
            // pass through every glass surface — the WATER above all: a sun ray
            // from the riverbed used to hit the water ribbon as an OPAQUE TLAS
            // occluder and zero the entire submerged direct-sun term, silently
            // undoing the 24371e2 depth/shadow fix on RT-shadow hardware (the
            // default tier here). This also makes RT shadows CONSISTENT with
            // the raster CSM, which replays only [0, m_frameCmdOpaque) and has
            // never drawn glass. AO / reflections / DDGI / acoustics trace with
            // cullMask 0xFF and still see glass exactly as before.
            inst.mask = (dr.flags & kFlagGlass) ? 0x7Fu : 0xFFu;
            std::memcpy(inst.model, dr.model, sizeof(inst.model));
            m_rtInstScratch.push_back(inst);
            if (m_rt.hasSkinnedBlas(dr.meshId)) ++m_skinnedRtInstances;
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
        // SKINNED: a refit BLAS keeps its handle/address but its geometry (and thus
        // its bounds) moved this frame, so the TLAS must be rebuilt to refresh the
        // top-level bounding volumes — even if the instance set/transforms (the
        // signature) are unchanged. Only forces a rebuild on frames that actually
        // touched a skinned BLAS; a static frame still hits the no-build fast path.
        if (!firstBuild && !changed && !m_skinnedRtThisFrame && !m_rtao.rebuildTlasEachFrame)
            return m_rt.tlas() != VK_NULL_HANDLE;   // unchanged static TLAS: reuse as-is

        // A real (re)build follows.
        //
        // DOUBLE-BUFFER (#5 PART 1): the TLAS is now backed by a ring of independent
        // backings inside VulkanRT (ping-ponged per build). frame N+1's build targets
        // a DIFFERENT slot than the one frame N's in-flight consumers (RTAO / refl /
        // DDGI / rt-shadows / rt-acoustics) are still reading, so the rebuild no
        // longer races an in-flight reader on the TLAS backing — the per-frame
        // vkDeviceWaitIdle WAR-hazard guard that used to fire here EVERY frame (when
        // r_skinnedrt was on with skinned chars visible) is GONE. The consumer
        // descriptors are re-pointed below to the freshly-built slot's handle each
        // rebuild (the handle now changes value on every ping-pong), and the build's
        // own command buffer is fenced inside VulkanRT before the frame's command
        // buffer is recorded, so the new TLAS is ready when the ray-query passes run.
        //
        // The ONE remaining device wait is the very FIRST build: the mesh set3 TLAS
        // descriptor (binding 5, ALWAYS bound) may be referenced by a pending command
        // buffer recorded before any TLAS existed, so we idle once before its first
        // rewrite. That is a one-time boot boundary, not a per-frame cost. We count
        // every device wait the rebuild path pays so --test-framepacing can PROVE the
        // steady-state per-frame wait is zero.
        ++m_asBuildsThisFrame;   // ZERO-STUTTER spike-log attribution (TLAS (re)build)
        const VkAccelerationStructureKHR before = m_rt.tlas();
        if (!m_rt.buildTlas(m_rtInstScratch)) return false;
        m_rtTlasSig = sig;
        // SKINNED-RT telemetry/proof (one-shot edge log): the first frame any skinned
        // character actually enters the TLAS, report how many + the BLAS build/refit
        // split (the cheap-refit budget at work). Proof the feature is live without
        // log spam (logs once per 0->N transition).
        if (m_skinnedRtInstances > 0 && !m_skinnedRtLogged) {
            char sb[160];
            std::snprintf(sb, sizeof(sb),
                "[rt] skinned-TLAS: %u skinned char(s) now in the scene TLAS "
                "(this frame: %u BLAS build, %u refit) -> shadows/refl/DDGI/audio see them",
                m_skinnedRtInstances, m_rt.skinnedBuilds(), m_rt.skinnedRefits());
            logInfo(sb);
            m_skinnedRtLogged = true;
        } else if (m_skinnedRtInstances == 0) {
            m_skinnedRtLogged = false;   // re-arm so a later spawn logs again
        }
        // Re-point the consumer descriptors to the freshly-built ring slot. With the
        // TLAS ring (#5) the handle changes on EVERY build, so this runs every rebuild.
        //
        //  * FIRST build: the mesh set3 TLAS descriptor (binding 5) is ALWAYS bound,
        //    so a command buffer recorded before any TLAS existed may reference these
        //    sets. Idle ONCE here (one-time boot boundary) and rewrite ALL frame
        //    slots so every in-flight set is current from the start.
        //  * STEADY-STATE rebuild (the per-frame skinned-RT case): re-point ONLY the
        //    CURRENT frame slot's sets. beginFrame already waited m_frameIdx's
        //    inFlight fence, so that slot is NOT referenced by any pending command
        //    buffer — the update is hazard-free WITHOUT a device wait. Each frame
        //    rebuilds (skinned chars move every frame), so each slot is re-pointed on
        //    the frame it owns, before that frame's command buffer is recorded. This
        //    is what drives the per-frame device wait to ZERO.
        (void)before;
        if (firstBuild) {
            vkDeviceWaitIdle(m_dev.device);
            m_rt.addTlasSyncWait();   // counted: the ONE boot-time wait (not per-frame)
            rewriteRtaoTlas(kAllFrameSlots);
            writeMeshTlasDescriptor(kAllFrameSlots);
        } else {
            rewriteRtaoTlas(m_frameIdx);
            writeMeshTlasDescriptor(m_frameIdx);
        }
        return m_rt.tlasBuilt() && m_rt.tlas() != VK_NULL_HANDLE;
    }

uint64_t VulkanRenderDevice::tlasSignature(const std::vector<VulkanRT::TlasInstance>& inst) {
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

bool VulkanRenderDevice::traceAudioRaysSubmit(const AudioRay* rays, int count) {
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

int VulkanRenderDevice::traceAudioRaysHarvest(float* outHitT, int capacity) {
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

void VulkanRenderDevice::prepareRtaoUbo() {
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

void VulkanRenderDevice::recordRtaoComputeBody(VkCommandBuffer c) {
        if (!m_rtaoPipe) return;
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_rtaoPipe);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_rtaoLayout,
                                0, 1, &m_rtaoSet[m_frameIdx], 0, nullptr);
        const uint32_t gx = (m_rtaoExtent.width  + 7) / 8;
        const uint32_t gy = (m_rtaoExtent.height + 7) / 8;
        vkCmdDispatch(c, gx, gy, 1);
    }

void VulkanRenderDevice::prepareReflUbo() {
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

void VulkanRenderDevice::recordReflComputeBody(VkCommandBuffer c) {
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

void VulkanRenderDevice::computeDdgiVolume() {
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

void VulkanRenderDevice::prepareDdgiUbo() {
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

void VulkanRenderDevice::recordDdgiRaysBody(VkCommandBuffer c) {
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

void VulkanRenderDevice::recordDdgiUpdateBody(VkCommandBuffer c) {
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

void VulkanRenderDevice::recordAutoExposureBody(VkCommandBuffer c) {
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

void VulkanRenderDevice::recordFramePacing() {
        const auto now = std::chrono::steady_clock::now();
        if (!m_paceHaveLast) { m_paceLast = now; m_paceHaveLast = true; return; }
        const float cpuMs = (float)std::chrono::duration<double, std::milli>(now - m_paceLast).count();
        m_paceLast = now;
        if (m_totalFrames <= (uint64_t)m_pacing.warmupFrames) return;   // warmup: excluded
        // TLAS DOUBLE-BUFFER PROOF (#5 PART 1): once we have a healthy post-warmup
        // window AND the TLAS has rebuilt many times (the skinned-RT per-frame case),
        // emit a one-shot receipt. With the ring the device-wait-per-build ratio must
        // be ~0 (boot pays ONE wait; steady-state per-frame rebuilds pay none) — this
        // is the measurable proof the per-frame WAR-hazard wait is gone. Pre-ring this
        // line would have read tlasWaitsPerKBuild=1000 (a device wait EVERY rebuild).
        if (!m_tlasDbReceiptLogged && m_rt.tlasBuilds() >= 64) {
            const uint32_t b = m_rt.tlasBuilds(), w = m_rt.tlasSyncWaits();
            const uint32_t perK = b ? (uint32_t)((1000ull * w) / b) : 0u;
            char rb[224];
            std::snprintf(rb, sizeof(rb),
                "[tlas-db] TLAS double-buffer: builds=%u deviceWaits=%u "
                "(waits/1000builds=%u; boot=1, steady=0) lastBuildCpu=%.3fms ring=%u-slot",
                b, w, perK, m_rt.tlasCpuMs(), VulkanRT::tlasSlots());
            logInfo(rb);
            m_tlasDbReceiptLogged = true;
        }
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

glm::mat4 VulkanRenderDevice::computeLightViewProj() const {
        // Per-scene sun: rake the shadow box along the SAME direction the sky disk +
        // mesh.frag lighting use (m_sky.sunDir; defaults to (0.4,1,0.3) when no sky is set,
        // so Level1's shadows are unchanged).
        const glm::vec3 sunDir = glm::normalize(glm::vec3(m_sky.sunDir[0], m_sky.sunDir[1], m_sky.sunDir[2]));
        // Default: ~45 m box following the camera (Level1). Override (setShadowBounds): a fixed
        // box on a scene AABB so large scenes (showroom) fall inside the shadow map.
        //
        // r_shadowforward (the cheap interim + A/B reference for CSM): slide the
        // camera-following box FORWARD along the view axis so the shadowed region
        // leads the car instead of being centred on it. A host-PINNED box
        // (setShadowBounds) is left exactly where the scene put it. The branch is
        // skipped entirely at 0, so the historical centre is bit-for-bit intact.
        glm::vec3 followCenter = m_camPos;
        if (m_shadowForward != 0.0f) {
            const glm::vec3 fwd = m_camHasBasis ? m_camFwd
                                : glm::vec3(std::cos(m_camPitch) * std::cos(m_camYaw),
                                            std::sin(m_camPitch),
                                            std::cos(m_camPitch) * std::sin(m_camYaw));
            followCenter += glm::normalize(fwd) * m_shadowForward;
        }
        const glm::vec3 center = m_shadowOverride ? m_shadowCenter : followCenter;
        const float     ortho  = m_shadowOverride ? m_shadowOrtho : kShadowOrtho;
        const float     dHalf  = m_shadowOverride ? m_shadowDepthHalf : kShadowDepthHalf;
        // The matrix build itself lives in engine/rhi/Csm.cpp (moved VERBATIM), so
        // the renderer and --test-csm exercise the SAME code and the test can
        // assert it still matches the historical expression bit-for-bit.
        return csm::legacyOrthoViewProj(center, sunDir, ortho, dHalf);
    }

// ---- CASCADED SHADOW MAPS (r_csm; Lane 3) ---------------------------------
// Fit this frame's cascades and publish them to the shader. Returns the number
// of cascades to rasterize, or 0 for "run the legacy single-cascade path".
//
// The heavy lifting (splits, bounding spheres, texel snapping, per-cascade bias)
// lives in engine/rhi/Csm.h — Vulkan-free, so --test-csm asserts on exactly the
// numbers this function feeds the GPU, with no device involved.
uint32_t VulkanRenderDevice::prepareCsmCascades() {
        const uint32_t frame = m_frameIdx;
        // A host that pinned the box with setShadowBounds() has hand-tuned its
        // scene around a fixed ortho extent (the showroom does this). Cascades
        // would silently retarget that box, so CSM stands down and the pinned box
        // wins — the documented setShadowBounds contract, preserved.
        const bool active = m_csmEnabled && !m_shadowOverride;

        m_csm = csm::Result{};
        m_csmCascadesThisFrame = 0;

        // ctrl.x == 0 tells mesh.frag/glass.frag to take the LEGACY branch:
        // cam.lightViewProj against array layer 0, with the historical bias math.
        CsmUBO u{};
        for (uint32_t i = 0; i < kCsmCascades; ++i) u.viewProj[i] = glm::mat4(1.0f);
        u.ctrl = glm::vec4(0.0f);

        if (active) {
            csm::Params p{};
            p.camPos = m_camPos;
            p.camFwd = m_camHasBasis ? m_camFwd
                     : glm::vec3(std::cos(m_camPitch) * std::cos(m_camYaw),
                                 std::sin(m_camPitch),
                                 std::cos(m_camPitch) * std::sin(m_camYaw));
            p.camUp  = m_camHasBasis ? m_camUp : glm::vec3(0.0f, 1.0f, 0.0f);
            p.fovYDeg = m_camFov;
            p.aspect  = (float)m_extent.width / (float)std::max(1u, m_extent.height);
            p.zNear   = csm::kCascadeNear;
            // Never fit past the camera's own far plane — cascades beyond it would
            // shadow geometry that is never drawn.
            p.zFar    = std::min(m_csmDistance, m_camFar);
            p.sunDir  = glm::normalize(glm::vec3(m_sky.sunDir[0], m_sky.sunDir[1], m_sky.sunDir[2]));
            p.lambda  = m_csmLambda;
            p.shadowDim = kShadowDim;
            p.count   = (int)kCsmCascades;

            m_csm = csm::compute(p);
            m_csmCascadesThisFrame = (uint32_t)m_csm.count;

            for (int i = 0; i < m_csm.count; ++i) {
                u.viewProj[i]  = m_csm.c[i].viewProj;
                u.depthBias[i]  = m_csm.c[i].depthBias;
                u.normalBias[i] = m_csm.c[i].normalBias;
            }
            // Unused lanes get cascade 0's matrix rather than identity: if a
            // rounding edge ever selects them, they sample something sane.
            for (uint32_t i = m_csmCascadesThisFrame; i < kCsmCascades; ++i)
                u.viewProj[i] = m_csm.c[0].viewProj;
            u.splitFar = m_csm.splitFar;
            u.ctrl = glm::vec4((float)m_csm.count, m_csmBlend, 0.0f, 0.0f);
        }

        if (m_csmUboMapped[frame]) std::memcpy(m_csmUboMapped[frame], &u, sizeof(CsmUBO));
        return m_csmCascadesThisFrame;
    }

void VulkanRenderDevice::recordMainPassBody(VkCommandBuffer cmd) {
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

void VulkanRenderDevice::recordPlanetDraws(VkCommandBuffer cmd) {
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

void VulkanRenderDevice::drawMesh(const FrameContext& fc, MeshHandle mesh, TextureHandle baseColor,
              const float baseColorFactor[4], const float model[16]) {
        // The 5-arg form is the no-emissive case (emissive = {0,0,0,0}).
        const float noEmissive[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        drawMeshEmissive(fc, mesh, baseColor, baseColorFactor, noEmissive, model);
    }

void VulkanRenderDevice::drawMeshGlass(const FrameContext& fc, MeshHandle mesh, TextureHandle baseColor,
                   const float baseColorFactor[4], const float emissive[4],
                   const GlassMaterial& glass, const float model[16], bool alphaBlend) {
        // baseColorFactor's alpha is overridden by the material opacity so the glass
        // body's see-through amount is the single primary dial (spec §2).
        float factor[4] = {
            baseColorFactor ? baseColorFactor[0] : 1.0f,
            baseColorFactor ? baseColorFactor[1] : 1.0f,
            baseColorFactor ? baseColorFactor[2] : 1.0f,
            glass.opacity };
        // ALL glass rides the BLEND record tail — never the opaque range.
        // (This SUBSUMES the caller-facing `alphaBlend` param — flight-modes corona
        // shells — and the GlassMaterial::additive street-light routing: every
        // glass draw now lands in the BLEND partition unconditionally. The param
        // stays in the signature for source compatibility with those callers.)
        //
        // The opaque range is replayed by three passes that have NO fragment stage
        // and therefore CANNOT honour the glass discard that mesh.frag does at
        // mesh.frag:730:
        //   - the DEPTH PRE-PASS (depth.vert — depth-only, no .frag at all),
        //   - the SHADOW map    (shadow.vert — likewise depth-only),
        //   - the RT/TLAS range (vk_gi_rt.cpp:445).
        // So an opaque-range glass record writes SOLID depth and casts a SOLID
        // shadow, even though the colour pass never shades it as opaque. That is
        // one bug with two faces, and the water surface wears both:
        //   1. PRE-PASS: the water writes pre-pass depth, so every submerged
        //      surface fails the colour pass's EQUAL test and is never shaded —
        //      it is therefore absent from the scene copy that glass.frag samples
        //      for its see-through, so the river showed SKY where the fish were.
        //      (Standing on the bank, the water read empty and dead.)
        //   2. SHADOW: the water is a solid caster, so the whole submerged volume
        //      sat at shadow=0 — the entire direct sun term (mesh.frag:872/916)
        //      was zeroed and everything underwater was a black silhouette.
        // f967213 found face 1 for ADDITIVE glass (the street-light cone's
        // "solid funnel") and moved only that branch to the tail. The same cure
        // applies to every glass surface, and the water is the surface that
        // needed it most.
        //
        // This costs glass NOTHING: recordGlassPassBody (vk_passes.cpp:40) replays
        // the FULL [0, m_frameCmdCount) range through the glass pipeline, so glass
        // draws — and draws in the same relative order — regardless of which range
        // it records into. The opaque/blend split governs the depth/shadow/TLAS
        // replays and nothing else.
        drawMeshInternal(fc, mesh, baseColor, TextureHandle{}, TextureHandle{}, factor, emissive,
                         model, /*alphaMask=*/false, /*alphaBlend=*/true,
                         TextureHandle{},
                         TextureHandle{}, 1.0f, kFlagGlass, &glass);
    }

void VulkanRenderDevice::drawMeshEmissive(const FrameContext& fc, MeshHandle mesh, TextureHandle baseColor,
                      const float baseColorFactor[4], const float emissive[4],
                      const float model[16]) {
        // Forward to the PBR path with no normal/MR maps (identical behaviour).
        drawMeshPBR(fc, mesh, baseColor, TextureHandle{}, TextureHandle{},
                    baseColorFactor, emissive, model);
    }

void VulkanRenderDevice::drawMeshInternal(const FrameContext& fc, MeshHandle mesh, TextureHandle baseColor,
                      TextureHandle normal, TextureHandle metalRough,
                      const float baseColorFactor[4], const float emissive[4],
                      const float model[16], bool alphaMask, bool alphaBlend,
                      TextureHandle emissiveTex, TextureHandle detailTex, float detailUvScale,
                      uint32_t extraFlags, const GlassMaterial* glass,
                      float clearcoat , float clearcoatRough , float selfLight ,
                      float metallicScale ) {
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
        // SHIP SELF-LIGHT (canon: ships are self-lit) — same trick, the SPARE
        // pack2 lane (a self-lit ship is never the terrain marker, and clearcoat
        // owns pack1, so nothing collides). Low byte = intensity*255; flags bit3
        // gates the fragment term. Every non-ship draw is byte-identical.
        if (selfLight > 0.001f && (r.flags & kFlagTerrain) == 0u) {
            const float sl = selfLight < 0.0f ? 0.0f : (selfLight > 1.0f ? 1.0f : selfLight);
            r.flags |= kFlagShipSelfLit;
            r.terrainPack2 = (uint32_t)(sl * 255.0f + 0.5f);
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
            r.glassParams[2] = glass->specular;   r.glassParams[3] = glass->additive;
            r.glassTint[0]   = glass->tint[0];    r.glassTint[1]   = glass->tint[1];
            r.glassTint[2]   = glass->tint[2];    r.glassTint[3]   = glass->emissiveMap;
        } else {
            r.glassParams[0] = r.glassParams[1] = r.glassParams[2] = 0.0f;
            // BLACK-PROP FIX: opaque draws have no glass state, so the spare .w lane
            // carries the per-object metallic CLAMP (mesh.frag multiplies mr.b by it).
            // 1.0 (the default for every call site) is byte-identical to no clamp.
            r.glassParams[3] = metallicScale;
            r.glassTint[0]   = r.glassTint[1]   = r.glassTint[2]   = r.glassTint[3]   = 0.0f;
        }
        m_drawRecords.push_back(r);
    }

void VulkanRenderDevice::drawPlanet(const FrameContext& fc, MeshHandle mesh, const float model[16],
                uint32_t typeIndex, const TextureHandle* maps, uint32_t mapCount,
                float uTime) {
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

void VulkanRenderDevice::drawHudQuad(const FrameContext& fc, float xPx, float yPx,
                 float wPx, float hPx, const float rgba[4]) {
        if (!fc.valid || !m_hudPipeline) return;
        const float c[4] = { rgba ? rgba[0] : 1.0f, rgba ? rgba[1] : 1.0f,
                             rgba ? rgba[2] : 1.0f, rgba ? rgba[3] : 1.0f };
        // Whole-quad UV (0,0)-(1,1) samples the 1x1 white texel everywhere.
        HudVertex verts[6];
        emitQuad(verts, xPx, yPx, wPx, hPx, 0.0f, 0.0f, 1.0f, 1.0f, c);
        flushHud(verts, 6, /*texFont=*/-1);
    }

void VulkanRenderDevice::drawHudImage(const FrameContext& fc, TextureHandle tex,
                  float xPx, float yPx, float wPx, float hPx,
                  const float rgba[4],
                  float u0, float v0, float u1, float v1) {
        if (!fc.valid || !m_hudPipeline || !tex.valid()) return;
        const float c[4] = { rgba ? rgba[0] : 1.0f, rgba ? rgba[1] : 1.0f,
                             rgba ? rgba[2] : 1.0f, rgba ? rgba[3] : 1.0f };
        HudVertex verts[6];
        emitQuad(verts, xPx, yPx, wPx, hPx, u0, v0, u1, v1, c);
        flushHud(verts, 6, /*texFont=*/-1, /*userTex=*/tex.id);
    }

void VulkanRenderDevice::drawHudText(const FrameContext& fc, const char* text, float xPx,
                 float yPx, float pxPerGlyph, const float rgba[4]) {
        drawHudTextF(fc, x3::rhi::FontRole::Console, text, xPx, yPx, pxPerGlyph, rgba);
    }

void VulkanRenderDevice::drawHudTextF(const FrameContext& fc, x3::rhi::FontRole role, const char* text,
                  float xPx, float yPx, float px, const float rgba[4]) {
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

void VulkanRenderDevice::drawHudTextAtlas(int role, const char* text,
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

float VulkanRenderDevice::textAdvance(x3::rhi::FontRole role, const char* text, float px) const {
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

void VulkanRenderDevice::imageBarrier(VkCommandBuffer cmd, VkImage img,
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

void VulkanRenderDevice::depthBarrier(VkCommandBuffer cmd, VkImage img,
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

void VulkanRenderDevice::emitQuad(HudVertex out[6], float xPx, float yPx, float wPx, float hPx,
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

void VulkanRenderDevice::prepareFrameData() {
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
        const glm::vec3 fwd = m_camHasBasis ? m_camFwd
                            : glm::vec3(std::cos(m_camPitch) * std::cos(m_camYaw),
                                        std::sin(m_camPitch),
                                        std::cos(m_camPitch) * std::sin(m_camYaw));
        const glm::vec3 camUp = m_camHasBasis ? m_camUp : glm::vec3(0, 1, 0);
        glm::mat4 view = glm::lookAt(m_camPos, m_camPos + fwd, camUp);
        glm::mat4 proj = glm::perspective(glm::radians(m_camFov), aspect, 0.1f, m_camFar);
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
        // VELOCITY (#4): WANT this frame? Same gates the graph re-checks (velOn),
        // computed here so the prev-model SSBO fill below + the velocity UBO + the
        // TaaUBO velocityValid lane all agree within this frame. The depth pre-pass
        // requirement (prePassOn) is mirrored by the SSAO/GI/refl/rtao enables; we
        // approximate it here with the same predicates prepareFrameData can see.
        // buildAndExecuteGraph makes the final authoritative decision.
        const bool prePassWant = m_ssao.enabled || m_gi.enabled
                               || m_rtaoActiveThisFrame || m_reflActiveThisFrame
                               || (m_rtao.enabled && m_rtSupported);
        const bool velWant = taaWant && prePassWant && m_post.velocity
                          && (m_velPipe != VK_NULL_HANDLE) && (m_velImg != VK_NULL_HANDLE);
        m_velActiveThisFrame = velWant;

        if (taaWant && m_taaUboMapped[m_frameIdx]) {
            TaaUBO tu{};
            tu.invViewProjCur = glm::inverse(ubo.viewProj);
            tu.viewProjPrev   = m_taaHistoryValid ? m_taaPrevVP : unjitteredVP;
            tu.params0 = glm::vec4(1.0f / (float)std::max(1u, m_extent.width),
                                   1.0f / (float)std::max(1u, m_extent.height),
                                   m_taaHistoryValid ? 1.0f : 0.0f,
                                   0.9f /* history blend weight */);
            // params1.z = velocityValid: use the per-object MV reprojection only
            // when the velocity pass ran AND history is valid. Otherwise the
            // resolve takes the camera-only fallback (pre-velocity behavior).
            const float velValid = (velWant && m_taaHistoryValid) ? 1.0f : 0.0f;
            tu.params1 = glm::vec4(jit.x, jit.y, velValid, 0.0f);
            std::memcpy(m_taaUboMapped[m_frameIdx], &tu, sizeof(TaaUBO));
        }

        // Velocity UBO: UNJITTERED current + previous viewProj (the MV endpoints)
        // + the two frames' jitter in NDC (subtracted defensively in the shader;
        // zero against unjittered matrices). NDC jitter = pixel-jitter * 2 / extent.
        if (velWant && m_velUboMapped[m_frameIdx]) {
            const glm::vec2 curJitNdc(jit.x * 2.0f / (float)std::max(1u, m_extent.width),
                                      jit.y * 2.0f / (float)std::max(1u, m_extent.height));
            VelUBO vu{};
            vu.viewProjCurUnjit  = unjitteredVP;
            vu.viewProjPrevUnjit = m_taaHistoryValid ? m_taaPrevVP : unjitteredVP;
            vu.jitter = glm::vec4(curJitNdc.x, curJitNdc.y,
                                  m_velPrevJitterNdc.x, m_velPrevJitterNdc.y);
            std::memcpy(m_velUboMapped[m_frameIdx], &vu, sizeof(VelUBO));
            m_velPrevJitterNdc = curJitNdc;   // for next frame's prev-jitter lane
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
            // Depth-fog pass (ART_BIBLE §5): the SAME jitter-inclusive inverse
            // projection SSAO reconstructs with, captured for fog.frag's push.
            m_fogInvProjCPU = su.invProj;
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
            sc.ibl = glm::vec4(iblValid, m_iblIntensity, (float)(kIblPrefilterMips - 1), m_metalAmbient);
            // Reflections lane (mesh.frag set3): x gates the reflTex sample + IBL
            // blend; y is the live intensity. ONLY set when the refl pass actually
            // runs this frame, so mesh.frag never reads a stale/unwritten buffer.
            // (The IBL probe BAKE shares this UBO — like the SSAO lane, the bake's
            // gl_FragCoord-based UV is meaningless there, an accepted, tiny env-
            // bake approximation inherited from the existing SSAO precedent.)
            // .z = ENV-SPECULAR SCALE (r_iblspec, default 1 = byte-identical to the
            // pre-R10 math). The IBL intensity lane (ibl.y) scales env DIFFUSE and env
            // SPECULAR together, which makes a dark-interior mood and a reflective
            // METAL mutually exclusive: turning the environment up far enough for the
            // steel to reflect it also floods every dielectric in the room with
            // irradiance. They are different lobes and they need different knobs --
            // metals are kD ~ 0 (pure reflection), concrete is kD ~ 1 (pure diffuse).
            sc.refl = glm::vec4(m_reflActiveThisFrame ? 1.0f : 0.0f,
                                m_refl.intensity, m_iblSpecular, 0.0f);
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
            // r_debugview rides the reserved rtsh1.w lane (0 = off -> byte-identical).
            sc.rtsh1.w = (float)m_debugView;
            // UNDERWATER CAUSTICS lane (setCaustics): the host-owned local water
            // plane + animation clock. All zero when no host opted in, so the
            // mesh.frag gate never opens and dry worlds are byte-identical.
            sc.caustics = glm::vec4(m_caustics.enabled ? 1.0f : 0.0f,
                                    m_caustics.waterY, m_caustics.time,
                                    m_caustics.intensity);
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
        // CSM: fit + publish this frame's cascades (0 = the legacy path stays in
        // charge). Must run AFTER m_lightViewProj so the legacy matrix is the one
        // cascade 0 falls back to when CSM is off.
        prepareCsmCascades();
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
        // .w = the scene's SUN RADIANCE scale for mesh.frag's directional key
        // (SkyParams::sunLight; 1.0 by default == the old hardcoded kSunColor).
        ubo.sunDir = glm::vec4(glm::normalize(glm::vec3(m_sky.sunDir[0], m_sky.sunDir[1], m_sky.sunDir[2])),
                               std::max(m_sky.sunLight, 0.0f));

        // ====================================================================
        // CLUSTERED FORWARD LIGHTING (r_clusterlights) — build this frame's
        // froxel light lists and upload them alongside the camera UBO.
        //
        // r_clusterlights 0 leaves the whole tail at ZERO, which is what makes
        // the shader take the legacy 64-entry UBO loop. Nothing below runs, the
        // SSBOs are not touched, and the render is bit-for-bit the old path.
        // ====================================================================
        ubo.camFwd       = glm::vec4(0.0f);
        ubo.clusterCfg   = glm::vec4(0.0f);
        ubo.clusterGrid  = glm::vec4(0.0f);
        ubo.clusterSlice = glm::vec4(0.0f);
        m_clusterStats = ClusterBuildResult{};
        m_clusterCpuMs = 0.0f;   // NOT stale: a legacy frame must report 0, not last clustered frame's cost
        if (m_clusterLights && fr.lightMapped && fr.clusterMapped && !m_pointLights.empty()) {
            const auto t0 = std::chrono::high_resolution_clock::now();

            // The SAME basis the viewProj above was built from — fwd/camUp are
            // the locals used for glm::lookAt, and the near/far/fov are the
            // arguments to glm::perspective. Reusing them (rather than
            // re-deriving) is what keeps the CPU's froxel geometry and the
            // shader's fragment->froxel lookup on the same frustum.
            const float eye[3] = { m_camPos.x, m_camPos.y, m_camPos.z };
            const float fw[3]  = { fwd.x, fwd.y, fwd.z };
            const float up3[3] = { camUp.x, camUp.y, camUp.z };
            const ClusterView cv = makeClusterView(eye, fw, up3, m_camFov, aspect,
                                                   0.1f /* matches glm::perspective above */,
                                                   m_camFar);

            const uint32_t sceneLights = std::min<uint32_t>((uint32_t)m_pointLights.size(), kMaxSceneLights);

            // Light rows -> the SSBO (the whole set, not the legacy 64).
            auto* gl = static_cast<GpuPointLight*>(fr.lightMapped);
            for (uint32_t i = 0; i < sceneLights; ++i) {
                const PointLight& s = m_pointLights[i];
                gl[i].posRange = glm::vec4(s.pos[0], s.pos[1], s.pos[2], s.range);
                gl[i].colorPad = glm::vec4(s.color[0], s.color[1], s.color[2], 0.0f);
            }

            // Assign into CACHED staging (see m_clusterCounts), then copy out.
            if (m_clusterCounts.size() != kClusterCount) {
                m_clusterCounts.assign(kClusterCount, 0u);
                m_clusterIndices.assign((size_t)kClusterCount * kMaxLightsPerCluster, 0u);
            }
            m_clusterStats = buildClusterLightLists(cv, m_pointLights.data(), sceneLights,
                                                    m_clusterCounts.data(), m_clusterIndices.data());

            // Publish to the GPU. Counts are always copied whole (13.5 KB); index
            // rows are copied only for froxels that actually hold something, in
            // CONTIGUOUS RUNS — a night city fills a small fraction of the 3456
            // froxels, so this moves tens of KB, not the full 884 KB.
            auto* dstCounts  = static_cast<uint32_t*>(fr.clusterMapped);
            auto* dstIndices = dstCounts + kClusterCount;
            std::memcpy(dstCounts, m_clusterCounts.data(), sizeof(uint32_t) * kClusterCount);
            for (uint32_t c = 0; c < kClusterCount; ) {
                if (m_clusterCounts[c] == 0) { ++c; continue; }
                uint32_t e = c + 1;
                while (e < kClusterCount && m_clusterCounts[e] != 0) ++e;
                const size_t off = (size_t)c * kMaxLightsPerCluster;
                std::memcpy(dstIndices + off, m_clusterIndices.data() + off,
                            sizeof(uint32_t) * (size_t)(e - c) * kMaxLightsPerCluster);
                c = e;
            }

            const auto t1 = std::chrono::high_resolution_clock::now();
            m_clusterCpuMs = std::chrono::duration<float, std::milli>(t1 - t0).count();

            ubo.camFwd       = glm::vec4(cv.camFwd[0], cv.camFwd[1], cv.camFwd[2], cv.zNear);
            ubo.clusterCfg   = glm::vec4(1.0f, (float)sceneLights,
                                         1.0f / (float)std::max(1u, m_extent.width),
                                         1.0f / (float)std::max(1u, m_extent.height));
            ubo.clusterGrid  = glm::vec4((float)kClusterGridX, (float)kClusterGridY,
                                         (float)kClusterGridZ, (float)kMaxLightsPerCluster);
            ubo.clusterSlice = glm::vec4(cv.sliceScale, cv.sliceBias, (float)kClusterCount, 0.0f);

            // OVERFLOW IS NEVER SILENT. The bug this whole feature replaces was a
            // silent truncation (332 ceiling fixtures in, 64 out, not one word
            // logged, Level 1 lit by an ambient wash for a year). Rate-limited so a
            // genuinely over-dense scene does not spam, but it always says so.
            if (m_clusterStats.overflows > 0 && (m_clusterOverflowLogged++ % 300u) == 0u) {
                logWarn("[cluster] " + std::to_string(m_clusterStats.overflows) +
                        " light->froxel assignment(s) DROPPED across " +
                        std::to_string(m_clusterStats.clustersOverflowed) +
                        " froxel(s) at the " + std::to_string(kMaxLightsPerCluster) +
                        "-per-froxel cap (" + std::to_string(m_clusterStats.lightsVisible) +
                        " visible lights). Lowest light index wins; raise "
                        "kMaxLightsPerCluster or thin the scene. r_debugview 6 shows where.");
            }
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
                pu.params   = glm::vec4(invW, invH, 0.1f, m_camFar);  // near/far match the proj
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
                const uint32_t velRow = row;        // capture before the post-increment
                ObjectData& o = objs[row++];
                std::memcpy(&o.model, dr.model, sizeof(o.model));
                // VELOCITY (#4): record this row's prev-frame model (from last
                // frame's history, by row) into the prev-model SSBO the velocity
                // pass reads, and stash the current model for next frame's history.
                // velActive gates the work so non-velocity frames pay nothing.
                if (m_velActiveThisFrame && m_prevModelMapped[m_frameIdx]) {
                    const glm::mat4 curM = glm::make_mat4(dr.model);
                    glm::mat4 prevM = (velRow < m_velPrevModels.size())
                                    ? m_velPrevModels[velRow] : curM;
                    static_cast<glm::mat4*>(m_prevModelMapped[m_frameIdx])[velRow] = prevM;
                    if (velRow >= m_velCurModels.size()) m_velCurModels.resize(velRow + 1);
                    m_velCurModels[velRow] = curM;
                }
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

        // VELOCITY (#4): rotate this frame's per-row models into the history used
        // by next frame's prev-model SSBO fill. `row` is the total rows written.
        if (m_velActiveThisFrame) {
            m_velCurModels.resize(row);
            m_velPrevModels.swap(m_velCurModels);   // prev <- cur (cur kept as scratch)
        }

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

// Rasterize the sun's depth map(s). The graph does NOT drive dynamic rendering
// for this pass (usesDynamicRendering = false) because CSM needs ONE
// begin/endRendering per cascade — each attaching a different array layer. The
// graph still owns every barrier and has already put the whole image in
// DEPTH_ATTACHMENT_OPTIMAL before we are called.
//
// r_csm 0 runs this loop exactly once, into layer 0, with the legacy matrix and
// the legacy VkRenderingInfo — identical GPU work to the pre-cascade renderer.
void VulkanRenderDevice::recordShadowPassBody(VkCommandBuffer cmd) {
        if (!m_shadowPipeline) return;
        auto& fr = m_frames[m_frameIdx];

        const uint32_t cascades = (m_csmCascadesThisFrame > 0) ? m_csmCascadesThisFrame : 1u;

        VkViewport vp{ 0.0f, 0.0f, (float)kShadowDim, (float)kShadowDim, 0.0f, 1.0f };
        VkRect2D scis{ {0,0}, { kShadowDim, kShadowDim } };

        for (uint32_t c = 0; c < cascades; ++c) {
            // Attach THIS cascade's array layer as the depth target.
            VkRenderingAttachmentInfo att{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            att.imageView   = m_shadowLayerView[c];
            att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            att.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
            att.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
            att.clearValue.depthStencil = { 1.0f, 0 };
            VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
            ri.renderArea = { {0,0}, { kShadowDim, kShadowDim } };
            ri.layerCount = 1;
            ri.colorAttachmentCount = 0;
            ri.pDepthAttachment = &att;
            vkCmdBeginRendering(cmd, &ri);

            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &scis);

            if (m_frameCmdCount > 0) {
                // The cascade's light matrix travels as a push constant, so the
                // shared per-frame camera UBO (whose std140 layout ~20 shaders
                // mirror) never has to grow an array. With CSM off this IS
                // m_lightViewProj, so shadow.vert transforms by the same numbers
                // it used to read from the UBO.
                const glm::mat4 lvp = (m_csmCascadesThisFrame > 0) ? m_csm.c[c].viewProj
                                                                   : m_lightViewProj;
                // Alpha-CUTOUT groups (foliage/people billboards, texIndex bit31): the
                // plain pipeline has no fragment stage, so a fir billboard casts its FULL
                // QUAD as a shadow (hard black rectangles on snow). The cutout pipeline
                // discards exactly like mesh.frag. Opt-in per host (setShadowCutout) —
                // OFF leaves every other world's shadow map bit-for-bit unchanged.
                const bool cutoutAware = m_shadowCutout && (m_shadowCutoutPipeline != VK_NULL_HANDLE);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline);
                // set 0 = object SSBO + camera UBO (shadow.vert reads the model rows).
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowLayout,
                                        0, 1, &fr.objSet, 0, nullptr);
                vkCmdPushConstants(cmd, m_shadowLayout, VK_SHADER_STAGE_VERTEX_BIT,
                                   0, sizeof(glm::mat4), &lvp);
                bool cutoutBound = false;
                for (uint32_t i = 0; i < m_frameCmdOpaque; ++i) {
                    const bool wantCutout = cutoutAware && (m_drawMeshCutout[i] != 0);
                    if (wantCutout != cutoutBound) {
                        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            wantCutout ? m_shadowCutoutPipeline : m_shadowPipeline);
                        if (wantCutout) {
                            // set 0 = objSet (layout-compatible), set 1 = bindless textures.
                            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_shadowCutoutLayout, 0, 1, &fr.objSet, 0, nullptr);
                            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_shadowCutoutLayout, 1, 1, &m_bindlessSet, 0, nullptr);
                        } else {
                            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_shadowLayout, 0, 1, &fr.objSet, 0, nullptr);
                        }
                        // Both layouts declare the SAME push range at offset 0, but
                        // swapping pipeline layouts invalidates pushed values, so
                        // re-push against whichever layout is now bound.
                        vkCmdPushConstants(cmd,
                            wantCutout ? m_shadowCutoutLayout : m_shadowLayout,
                            VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &lvp);
                        cutoutBound = wantCutout;
                    }
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
            vkCmdEndRendering(cmd);
        }
    }

void VulkanRenderDevice::recordDepthPrePassBody(VkCommandBuffer cmd) {
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

void VulkanRenderDevice::recordMeshDraws(VkCommandBuffer cmd) {
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
        // RT-AO (m_rtaoActiveThisFrame) is a FIRST-CLASS depth-prepass consumer: the
        // rtao-compute pass reconstructs each pixel's world position from the camera
        // DEPTH buffer before tracing the TLAS. It was missing from this predicate while
        // being present in prepareFrameData's prePassWant (see ~line 1349), so the two
        // DID diverge — exactly what the comment above promises they never do. With RT AO
        // the only consumer (r_rtao 1, SSAO/GI/refl off — the default this build ships on
        // ray-tracing devices), the depth pre-pass was skipped while the graph still added
        // rtao-compute + rtao-apply, and the half-res AO image was sampled by rtao-apply in
        // VK_IMAGE_LAYOUT_UNDEFINED (VUID-vkCmdDraw-None-09600, 10x/frame in Debug).
        // Latent until 03b77ff turned RT AO on by default and finally exercised the path.
        const bool prePassOn = m_ssao.enabled || m_gi.enabled || m_reflActiveThisFrame
                            || m_rtaoActiveThisFrame;
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
        VkDescriptorSet sets[5] = { m_bindlessSet, fr.objSet, m_shadowSet[m_frameIdx], m_meshAoSet[m_frameIdx], m_iblMeshSet };
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

void VulkanRenderDevice::flushHud(const HudVertex* verts, uint32_t count, int texFont, uint32_t userTex ) {
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

const VulkanRenderDevice::Texture* VulkanRenderDevice::hudRecordTexture(int texFont, uint32_t userTex ) const {
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

void VulkanRenderDevice::recordHudDraws(VkCommandBuffer cmd) {
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

void VulkanRenderDevice::iblRenderTo(VkCommandBuffer cmd, VkImageView target, uint32_t w, uint32_t h,
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

void VulkanRenderDevice::iblBarrier(VkCommandBuffer cmd, VkImage img, uint32_t baseMip, uint32_t mipCount,
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

void VulkanRenderDevice::bakeProbeSceneIntoEnv(VkCommandBuffer cmd) {
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

        VkDescriptorSet sets[5] = { m_bindlessSet, fr.objSet, m_shadowSet[m_frameIdx], m_meshAoSet[m_frameIdx], m_iblMeshSet };
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

bool VulkanRenderDevice::regenIblFromSky() {
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

void VulkanRenderDevice::iblBarrierTex2D(VkCommandBuffer cmd, VkImage img, VkImageLayout oldL, VkImageLayout newL,
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

} // namespace x3::rhi
