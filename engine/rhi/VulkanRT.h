#pragma once
// VulkanRT — hardware ray-tracing acceleration-structure manager for the X3Native
// Vulkan backend (RT Phase 1+). CLEAN-ROOM original work: built from the Vulkan
// 1.3 spec + the public VK_KHR_acceleration_structure / VK_KHR_ray_query
// extension specs (Khronos) and the LunarG SDK headers. No GPL / id Tech /
// RBDOOM source consulted.
//
// SCOPE / DESIGN
// --------------
//  * RAY QUERY ONLY. This module builds the BLAS/TLAS that an INLINE ray-query
//    pass (rayQueryEXT in compute/fragment) traces against. There is NO
//    ray-tracing pipeline and NO shader binding table — by deliberate project
//    constraint. The structures are plain VK_KHR_acceleration_structure objects,
//    so a future RT-pipeline path could reuse them verbatim (no lock-in).
//  * The extension entry points (vkCmdBuildAccelerationStructuresKHR, ...) are
//    NOT exported by the Vulkan loader import lib the engine links, so they are
//    resolved here via vkGetDeviceProcAddr at init().
//  * EVERYTHING here is opt-in: the device only constructs + drives this module
//    when the RT extensions were enabled AND an RT cvar is on. When off, this file
//    is never touched and the raster path is byte-for-byte unchanged.
//
//  * BLAS per mesh: one bottom-level AS per distinct mesh, built once from that
//    mesh's existing vertex+index buffers (which the device now also creates with
//    the SHADER_DEVICE_ADDRESS + ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY
//    usage flags when RT is supported). Cached by mesh id; rebuilt only if the
//    mesh's geometry changes.
//  * TLAS per frame: a top-level AS of instances, fed from the SAME per-frame
//    draw list the GPU-driven multidraw path already gathers (mesh + transform).
//    Static-first: rebuilt when the instance set/topology changes; the device may
//    request a rebuild each frame (cheap for this scene size) — full refit is a
//    documented next tier.
//
// This header includes <vulkan/vulkan.h> + VMA and is therefore an INTERNAL
// renderer detail (consumed only by VulkanRenderDevice.cpp). IRenderDevice.h
// stays graphics-API-free.

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include "../core/x3_cpuzones.h"   // as.* CPU sub-zones (fence wait vs pack)
#include <chrono>
#include <cstdint>
#include <cstring>
#include <vector>
#include <unordered_map>

namespace x3::rhi {

// A device-address-bearing buffer used for AS storage / scratch / instances.
struct RtBuffer {
    VkBuffer        buf   = VK_NULL_HANDLE;
    VmaAllocation   alloc = nullptr;
    VkDeviceAddress addr  = 0;
    VkDeviceSize    size  = 0;
};

class VulkanRT {
public:
    // Resolve the KHR ray-tracing entry points + query the AS build/scratch
    // alignment properties. Returns false if any required pointer is missing
    // (the caller then leaves RT disabled and stays on the raster path).
    bool init(VkDevice dev, VkPhysicalDevice phys, VmaAllocator alloc,
              VkQueue queue, uint32_t queueFamily,
              void (*logInfo)(const char*), void (*logError)(const char*)) {
        m_dev = dev; m_phys = phys; m_alloc = alloc; m_queue = queue; m_family = queueFamily;
        m_logInfo = logInfo; m_logError = logError;

        auto load = [&](const char* name) -> PFN_vkVoidFunction {
            return vkGetDeviceProcAddr(dev, name);
        };
        m_pfnCreateAS  = (PFN_vkCreateAccelerationStructureKHR)              load("vkCreateAccelerationStructureKHR");
        m_pfnDestroyAS = (PFN_vkDestroyAccelerationStructureKHR)             load("vkDestroyAccelerationStructureKHR");
        m_pfnGetASBuildSizes = (PFN_vkGetAccelerationStructureBuildSizesKHR) load("vkGetAccelerationStructureBuildSizesKHR");
        m_pfnCmdBuildAS = (PFN_vkCmdBuildAccelerationStructuresKHR)          load("vkCmdBuildAccelerationStructuresKHR");
        m_pfnGetASAddr  = (PFN_vkGetAccelerationStructureDeviceAddressKHR)   load("vkGetAccelerationStructureDeviceAddressKHR");
        if (!m_pfnCreateAS || !m_pfnDestroyAS || !m_pfnGetASBuildSizes ||
            !m_pfnCmdBuildAS || !m_pfnGetASAddr) {
            if (m_logError) m_logError("[rt] failed to resolve VK_KHR_acceleration_structure entry points");
            return false;
        }

        // AS build scratch alignment (spec requires scratch addresses be aligned
        // to minAccelerationStructureScratchOffsetAlignment).
        m_asProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
        VkPhysicalDeviceProperties2 p2{};
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &m_asProps;
        vkGetPhysicalDeviceProperties2(phys, &p2);

        // Transient command pool + fence for the one-time AS builds (kept separate
        // from the renderer's upload pool so this module is self-contained).
        VkCommandPoolCreateInfo cpci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        cpci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpci.queueFamilyIndex = queueFamily;
        if (vkCreateCommandPool(dev, &cpci, nullptr, &m_pool) != VK_SUCCESS) {
            if (m_logError) m_logError("[rt] AS command pool create failed");
            return false;
        }
        VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (vkCreateFence(dev, &fci, nullptr, &m_fence) != VK_SUCCESS) {
            if (m_logError) m_logError("[rt] AS fence create failed");
            return false;
        }
        m_ready = true;
        if (m_logInfo) m_logInfo("[rt] acceleration-structure module ready (ray-query inline RT)");
        return true;
    }

    bool ready() const { return m_ready; }

    // When the device enabled VK_KHR_ray_tracing_position_fetch, BLAS builds add
    // VK_BUILD_ACCELERATION_STRUCTURE_FLAG_ALLOW_DATA_ACCESS_KHR so ray-query
    // shaders may fetch the committed triangle's vertex positions (DDGI hit
    // normals). Must be set BEFORE the first ensureBlas (cached BLAS keep the
    // flags they were built with). No-op cost when unsupported/false.
    void setAllowDataAccess(bool v) { m_allowDataAccess = v; }

    // ---- BLAS: build once per mesh from its vertex+index buffers --------------
    // `vbAddr`/`ibAddr` are the SHADER_DEVICE_ADDRESS of the mesh's vertex/index
    // buffers (the device creates them with the AS-input usage when RT is on).
    // `vertexStride` is sizeof(MeshVertex); the position is the first 3 floats.
    // Returns true + caches the BLAS keyed by meshId. Idempotent (no-op if cached).
    bool ensureBlas(uint32_t meshId, VkDeviceAddress vbAddr, uint32_t vertexCount,
                    uint32_t vertexStride, VkDeviceAddress ibAddr, uint32_t indexCount) {
        if (!m_ready) return false;
        if (m_blas.find(meshId) != m_blas.end()) return true;
        if (!vbAddr || !ibAddr || vertexCount == 0 || indexCount < 3) return false;

        VkAccelerationStructureGeometryKHR geo{};
        geo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;   // no any-hit needed (opaque scene)
        geo.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        geo.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        geo.geometry.triangles.vertexData.deviceAddress = vbAddr;
        geo.geometry.triangles.vertexStride = vertexStride;
        geo.geometry.triangles.maxVertex = vertexCount - 1;
        geo.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
        geo.geometry.triangles.indexData.deviceAddress = ibAddr;
        geo.geometry.triangles.transformData.deviceAddress = 0;

        const uint32_t triCount = indexCount / 3;

        VkAccelerationStructureBuildGeometryInfoKHR bgi{};
        bgi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        bgi.type  = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        bgi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        if (m_allowDataAccess)
            bgi.flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_DATA_ACCESS_BIT_KHR;
        bgi.mode  = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        bgi.geometryCount = 1;
        bgi.pGeometries = &geo;

        VkAccelerationStructureBuildSizesInfoKHR sizes{};
        sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        m_pfnGetASBuildSizes(m_dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                             &bgi, &triCount, &sizes);

        Blas b{};
        if (!createAsBackingBuffer(sizes.accelerationStructureSize, b.backing)) {
            if (m_logError) m_logError("[rt] BLAS backing buffer alloc failed");
            return false;
        }
        VkAccelerationStructureCreateInfoKHR aci{};
        aci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        aci.buffer = b.backing.buf;
        aci.offset = 0;
        aci.size   = sizes.accelerationStructureSize;
        aci.type   = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        if (m_pfnCreateAS(m_dev, &aci, nullptr, &b.handle) != VK_SUCCESS) {
            destroyBuffer(b.backing);
            if (m_logError) m_logError("[rt] vkCreateAccelerationStructureKHR (BLAS) failed");
            return false;
        }

        RtBuffer scratch{};
        if (!createScratchBuffer(sizes.buildScratchSize, scratch)) {
            m_pfnDestroyAS(m_dev, b.handle, nullptr);
            destroyBuffer(b.backing);
            if (m_logError) m_logError("[rt] BLAS scratch alloc failed");
            return false;
        }

        bgi.dstAccelerationStructure = b.handle;
        bgi.scratchData.deviceAddress = scratch.addr;
        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = triCount;
        range.primitiveOffset = 0; range.firstVertex = 0; range.transformOffset = 0;
        const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;

        // BOOT-TIME batched BLAS builds (docs/BOOT_TIME.md): inside a
        // beginBlasBatch()/endBlasBatch() window, RECORD the build into the shared
        // batch command buffer (one submit for the whole set) instead of a
        // blocking submit+fence PER MESH — ~8000 one-shot submits took ~6.6 s on
        // the legacy tower's first frame. The scratch buffer stays alive until
        // the batch flush. Outside a batch the original blocking path is used.
        if (m_blasBatchWanted) {
            VkCommandBuffer cmd = blasBatchCmd();
            if (cmd) {
                m_pfnCmdBuildAS(cmd, 1, &bgi, &pRange);
                m_blasBatchDirty = true;
                m_blasBatchScratch.push_back(scratch);
                VkAccelerationStructureDeviceAddressInfoKHR adi{};
                adi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
                adi.accelerationStructure = b.handle;
                b.addr = m_pfnGetASAddr(m_dev, &adi);
                m_blas.emplace(meshId, b);
                return true;
            }
            // batch cmd alloc failed -> fall through to the blocking path
        }

        const bool ok = oneTimeSubmit([&](VkCommandBuffer cmd){
            m_pfnCmdBuildAS(cmd, 1, &bgi, &pRange);
        });
        destroyBuffer(scratch);
        if (!ok) {
            m_pfnDestroyAS(m_dev, b.handle, nullptr);
            destroyBuffer(b.backing);
            return false;
        }

        VkAccelerationStructureDeviceAddressInfoKHR adi{};
        adi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        adi.accelerationStructure = b.handle;
        b.addr = m_pfnGetASAddr(m_dev, &adi);
        m_blas.emplace(meshId, b);
        return true;
    }

    // ---- BOOT-TIME batched BLAS builds ------------------------------------
    // beginBlasBatch(): subsequent ensureBlas calls record into ONE shared
    // command buffer. endBlasBatch(): submit once + wait the fence + free every
    // pending scratch. A BLAS recorded in the batch is NOT usable until
    // endBlasBatch returns (the caller builds the TLAS after the flush).
    void beginBlasBatch() {
        if (!m_ready) return;
        // A batch left in flight from a previous frame must be retired before its
        // command buffer / fence / scratch are reused. Normally the frame's own
        // drainBlasBatch() has already done this; this is the belt-and-braces path
        // for any caller that opens a batch outside the frame loop (asset load).
        drainBlasBatch();
        m_blasBatchWanted = true;
    }

    // ---- SUBMIT NOW, WAIT LATER (TLAS-split lane 2026-08-11) ------------------
    // endBlasBatch() used to submit AND block on the fence in one call, right in
    // the middle of endFrame — so the CPU sat idle for the whole AS build while
    // ~1.2 ms of render-graph recording waited its turn behind it.
    //
    // It is now split. submitBlasBatch() ends + submits the command buffer and
    // returns immediately; drainBlasBatch() waits the fence and releases the
    // batch's resources. The frame calls submit before recording the graph and
    // drain immediately before the frame's own vkQueueSubmit2, so the AS build
    // executes on the GPU *while the CPU records the frame*.
    //
    // THIS IS NOT the "assume submission order and drop the wait" shortcut. The
    // wait still happens, on the same thread, in the same frame, BEFORE the submit
    // that consumes the AS — so every lifetime the old code guaranteed is still
    // guaranteed: the batch command buffer is not reset while pending, the shared
    // scratch is not rewritten while in flight, the instance buffer is not
    // rewritten while the build reads it, and the ray-query passes cannot execute
    // before the build completes. Only the CPU's idle time moved.
    bool submitBlasBatch() {
        m_blasBatchWanted = false;
        if (!m_blasBatchOpen || m_blasBatchPending) return true;   // nothing recorded
        vkEndCommandBuffer(m_blasBatchCmd);
        VkCommandBufferSubmitInfo cs{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
        cs.commandBuffer = m_blasBatchCmd;
        VkSubmitInfo2 submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        submit.commandBufferInfoCount = 1; submit.pCommandBufferInfos = &cs;
        const VkResult sr = vkQueueSubmit2(m_queue, 1, &submit, m_fence);
        m_blasBatchOpen = false;
        if (sr != VK_SUCCESS) {
            // Never submitted: the fence will never signal, so do not arm the wait.
            vkResetCommandBuffer(m_blasBatchCmd, 0);
            for (RtBuffer& s : m_blasBatchScratch) destroyBuffer(s);
            m_blasBatchScratch.clear();
            return false;
        }
        m_blasBatchPending = true;
        return true;
    }

    // Wait the pending batch and release its command buffer + scratch. Idempotent.
    void drainBlasBatch() {
        if (!m_blasBatchPending) return;
        X3_CPU_ZONE(Z_AsDrain);
        vkWaitForFences(m_dev, 1, &m_fence, VK_TRUE, UINT64_MAX);
        vkResetFences(m_dev, 1, &m_fence);
        m_blasBatchPending = false;
        vkResetCommandBuffer(m_blasBatchCmd, 0);
        for (RtBuffer& s : m_blasBatchScratch) destroyBuffer(s);
        m_blasBatchScratch.clear();
    }

    // Legacy all-in-one (submit + wait). Kept for the non-frame paths.
    bool endBlasBatch() {
        const bool ok = submitBlasBatch();
        drainBlasBatch();
        return ok;
    }

    bool hasBlas(uint32_t meshId) const {
        return m_blas.find(meshId) != m_blas.end()
            || m_skinnedBlas.find(meshId) != m_skinnedBlas.end();
    }
    bool hasSkinnedBlas(uint32_t meshId) const { return m_skinnedBlas.find(meshId) != m_skinnedBlas.end(); }
    uint32_t skinnedBlasCount() const { return (uint32_t)m_skinnedBlas.size(); }

    void destroyBlas(uint32_t meshId) {
        auto it = m_blas.find(meshId);
        if (it != m_blas.end()) {
            if (it->second.handle) m_pfnDestroyAS(m_dev, it->second.handle, nullptr);
            destroyBuffer(it->second.backing);
            m_blas.erase(it);
        }
        destroySkinnedBlas(meshId);
    }

    void destroySkinnedBlas(uint32_t meshId) {
        auto it = m_skinnedBlas.find(meshId);
        if (it == m_skinnedBlas.end()) return;
        if (it->second.handle) m_pfnDestroyAS(m_dev, it->second.handle, nullptr);
        destroyBuffer(it->second.backing);
        destroyBuffer(it->second.scratch);
        m_skinnedBlas.erase(it);
    }

    // ---- SKINNED BLAS: per-frame build/refit from a compute-skinned VBO -------
    // Skinned characters change their vertex positions every frame (the compute
    // skinning pass writes m.dynVbo[slot]). Unlike the static ensureBlas (cached
    // once), this BLAS is built ALLOW_UPDATE + PREFER_FAST_BUILD so subsequent
    // frames can REFIT it (mode=UPDATE) in place — far cheaper than a full
    // rebuild — when only vertex positions move (topology is fixed). The first
    // call for a mesh does a full build; later calls refit. The scratch buffer is
    // kept resident with the BLAS (sized to max(build,update) scratch) so a refit
    // costs ZERO allocation. Records into the active BLAS batch command buffer
    // (one submit for the whole skinned set alongside the static warm-up), so it
    // adds no extra device submit/stall beyond the existing batched-AS boundary.
    //   vbAddr : device address of THIS frame's skinned output vbo (positions are
    //            the first 3 floats of each MeshVertex row, stride = sizeof row).
    //   ibAddr : the mesh's (static, shared) index buffer.
    // Returns true if the BLAS exists + is (re)built this batch. Requires an open
    // beginBlasBatch()/endBlasBatch() window (the caller opens one).
    bool ensureSkinnedBlas(uint32_t meshId, VkDeviceAddress vbAddr, uint32_t vertexCount,
                           uint32_t vertexStride, VkDeviceAddress ibAddr, uint32_t indexCount) {
        if (!m_ready) return false;
        if (!vbAddr || !ibAddr || vertexCount == 0 || indexCount < 3) return false;
        VkCommandBuffer cmd = blasBatchCmd();
        if (!cmd) return false;   // skinned BLAS only build inside a batch window

        const uint32_t triCount = indexCount / 3;
        auto existing = m_skinnedBlas.find(meshId);
        const bool haveExisting = (existing != m_skinnedBlas.end());
        // A refit (UPDATE) is only valid if the geometry topology is unchanged
        // (same tri count) AND we have a prior build to update from.
        const bool refit = haveExisting && existing->second.triCount == triCount
                                        && existing->second.handle != VK_NULL_HANDLE;

        VkAccelerationStructureGeometryKHR geo{};
        geo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        geo.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        geo.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        geo.geometry.triangles.vertexData.deviceAddress = vbAddr;
        geo.geometry.triangles.vertexStride = vertexStride;
        geo.geometry.triangles.maxVertex = vertexCount - 1;
        geo.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
        geo.geometry.triangles.indexData.deviceAddress = ibAddr;
        geo.geometry.triangles.transformData.deviceAddress = 0;

        VkAccelerationStructureBuildGeometryInfoKHR bgi{};
        bgi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        bgi.type  = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        // PREFER_FAST_BUILD + ALLOW_UPDATE: skinned geometry is rebuilt/refit every
        // frame, so cheap builds beat the static path's PREFER_FAST_TRACE.
        bgi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR
                  | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
        if (m_allowDataAccess)
            bgi.flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_DATA_ACCESS_BIT_KHR;
        bgi.geometryCount = 1;
        bgi.pGeometries = &geo;

        if (refit) {
            SkinnedBlas& b = existing->second;
            bgi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
            bgi.srcAccelerationStructure = b.handle;
            bgi.dstAccelerationStructure = b.handle;
            bgi.scratchData.deviceAddress = b.scratch.addr;
            VkAccelerationStructureBuildRangeInfoKHR range{};
            range.primitiveCount = triCount;
            const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;
            m_pfnCmdBuildAS(cmd, 1, &bgi, &pRange);
            // TLAS-SPLIT LANE (2026-08-11): there used to be a FULL
            // AS_BUILD->AS_BUILD memory barrier here, after EVERY refit. It was
            // never needed. Each skinned BLAS refit reads+writes ITS OWN backing
            // through ITS OWN resident scratch — two refits of different meshes
            // touch disjoint memory and the spec imposes no ordering requirement
            // between them. The barrier only forced 15 characters' worth of tiny
            // AS updates to execute strictly one-at-a-time with a pipeline flush
            // between each. The ONE ordering that IS required — every BLAS write
            // before the TLAS build reads them — is now a single barrier emitted
            // by recordTlasBuild() at the end of the batch (see blasBatchBarrier).
            m_blasBatchDirty = true;
            ++m_skinnedRefits;
            return true;
        }

        // Full build (first time for this mesh, or tri count changed): (re)create
        // the backing + a resident scratch sized for the larger of build/update.
        bgi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        VkAccelerationStructureBuildSizesInfoKHR sizes{};
        sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        m_pfnGetASBuildSizes(m_dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                             &bgi, &triCount, &sizes);
        VkDeviceSize scratchNeed = sizes.buildScratchSize;
        if (sizes.updateScratchSize > scratchNeed) scratchNeed = sizes.updateScratchSize;

        if (haveExisting) destroySkinnedBlas(meshId);   // topology changed: rebuild fresh

        SkinnedBlas b{};
        b.triCount = triCount;
        if (!createAsBackingBuffer(sizes.accelerationStructureSize, b.backing)) {
            if (m_logError) m_logError("[rt] skinned BLAS backing alloc failed");
            return false;
        }
        if (!createScratchBuffer(scratchNeed, b.scratch)) {
            destroyBuffer(b.backing);
            if (m_logError) m_logError("[rt] skinned BLAS scratch alloc failed");
            return false;
        }
        VkAccelerationStructureCreateInfoKHR aci{};
        aci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        aci.buffer = b.backing.buf; aci.offset = 0;
        aci.size = sizes.accelerationStructureSize;
        aci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        if (m_pfnCreateAS(m_dev, &aci, nullptr, &b.handle) != VK_SUCCESS) {
            destroyBuffer(b.backing); destroyBuffer(b.scratch);
            if (m_logError) m_logError("[rt] vkCreateAccelerationStructureKHR (skinned BLAS) failed");
            return false;
        }
        bgi.dstAccelerationStructure = b.handle;
        bgi.scratchData.deviceAddress = b.scratch.addr;
        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = triCount;
        const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;
        m_pfnCmdBuildAS(cmd, 1, &bgi, &pRange);
        m_blasBatchDirty = true;
        VkAccelerationStructureDeviceAddressInfoKHR adi{};
        adi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        adi.accelerationStructure = b.handle;
        b.addr = m_pfnGetASAddr(m_dev, &adi);
        m_skinnedBlas.emplace(meshId, b);
        ++m_skinnedBuilds;
        return true;
    }

    // Per-frame skinned-AS work counters (perf telemetry; reset by the caller).
    uint32_t skinnedBuilds() const { return m_skinnedBuilds; }
    uint32_t skinnedRefits() const { return m_skinnedRefits; }
    void     resetSkinnedCounters() { m_skinnedBuilds = 0; m_skinnedRefits = 0; }

    // ---- TLAS: one instance per (meshId, transform) ---------------------------
    struct TlasInstance {
        uint32_t meshId;
        uint32_t customIndex = 0;   // 24-bit instanceCustomIndex: the instance's
                                    // ObjectData SSBO row (DDGI hit-shading lookup)
        // 8-bit instance visibility mask (rayQuery cullMask & mask != 0 -> hit).
        // 0xFF = every ray sees it (the default, all prior behaviour). GLASS
        // instances carry 0x7F (bit 7 clear) so the RT SHADOW rays — which trace
        // with cullMask 0x80 — pass through glass (sun through the water/panes,
        // matching the raster CSM that draws only the opaque range), while
        // AO / reflections / DDGI / acoustics (cullMask 0xFF) still see glass
        // exactly as before.
        uint8_t  mask = 0xFF;
        float    model[16];   // column-major 4x4 (the renderer's ObjectData::model)
        // LANE 6: the caller has ALREADY resolved this mesh's BLAS address (it has
        // to, to decide whether the instance is admissible at all), so carrying it
        // here removes a SECOND pair of unordered_map lookups per instance inside
        // buildTlas. With ~90k draw records in echotropolis that pair of lookups
        // was pure duplicated work. 0 = "not resolved, look it up" (old behaviour).
        VkDeviceAddress blasAddr = 0;
    };

    // Resolve a mesh's BLAS device address (static first, then skinned). 0 = no
    // BLAS. `isSkinned` (optional) reports which table answered, so a caller that
    // needs BOTH facts pays ONE lookup instead of hasBlas()+hasSkinnedBlas().
    VkDeviceAddress blasAddrOf(uint32_t meshId, bool* isSkinned = nullptr) const {
        auto it = m_blas.find(meshId);
        if (it != m_blas.end()) { if (isSkinned) *isSkinned = false; return it->second.addr; }
        auto sk = m_skinnedBlas.find(meshId);
        if (sk != m_skinnedBlas.end()) { if (isSkinned) *isSkinned = true; return sk->second.addr; }
        if (isSkinned) *isSkinned = false;
        return 0;
    }

    // ---- PARTIAL INSTANCE-BUFFER UPDATE (TLAS-split lane 2026-08-11) ---------
    // Reserve room for `maxRows` instance rows and hand back the PERSISTENTLY
    // MAPPED CPU pointer to the array. The caller writes rows directly into it —
    // there is no staging vector and no bulk memcpy — and, crucially, writes ONLY
    // the rows that actually changed since the previous frame: the buffer is
    // never cleared, so every untouched row keeps the value the last frame left
    // there. That is the whole static/dynamic split: the STATIC portion of the
    // instance array is written once and then simply persists, while the DYNAMIC
    // rows are the only thing the CPU touches per frame.
    //
    // `*outInvalidated` is set true when the buffer had to be (re)allocated — its
    // contents are then undefined and the caller MUST rewrite every row, not just
    // the changed ones. WRITE-ONLY memory (VMA SEQUENTIAL_WRITE / write-combined
    // on discrete GPUs): never read through this pointer, keep a CPU-side shadow.
    VkAccelerationStructureInstanceKHR* beginInstanceWrite(uint32_t maxRows,
                                                           bool* outInvalidated) {
        if (outInvalidated) *outInvalidated = false;
        if (!m_ready) return nullptr;
        const VkDeviceSize need =
            (VkDeviceSize)(maxRows ? maxRows : 1) * sizeof(VkAccelerationStructureInstanceKHR);
        if (m_instBuf.size < need || !m_instMapped) {
            // Grow (never shrink — a shrink would only trade a realloc for bytes).
            if (m_instMapped) { vmaUnmapMemory(m_alloc, m_instBuf.alloc); m_instMapped = nullptr; }
            destroyBuffer(m_instBuf);
            VkDeviceSize alloc = need + need / 4u;   // 25% headroom against churn
            if (!createInstanceBuffer(alloc, m_instBuf)) {
                if (m_logError) m_logError("[rt] TLAS instance buffer alloc failed");
                return nullptr;
            }
            if (vmaMapMemory(m_alloc, m_instBuf.alloc, &m_instMapped) != VK_SUCCESS) {
                m_instMapped = nullptr;
                if (m_logError) m_logError("[rt] TLAS instance buffer map failed");
                return nullptr;
            }
            if (outInvalidated) *outInvalidated = true;
        }
        return (VkAccelerationStructureInstanceKHR*)m_instMapped;
    }

    // Record a TLAS build over the first `instCount` rows of the mapped instance
    // buffer INTO THE OPEN BLAS BATCH command buffer. One submit now carries the
    // skinned BLAS refits AND the TLAS build (it used to be two submits, each with
    // its own blocking fence wait). Requires an open beginBlasBatch() window; the
    // caller calls endBlasBatch() afterwards, which is the single submit + wait.
    bool recordTlasBuild(uint32_t instCount) {
        if (!m_ready) return false;
        const auto cpuT0 = std::chrono::steady_clock::now();
        VkCommandBuffer cmd = blasBatchCmd();
        if (!cmd) return false;

        m_lastInstanceCount = instCount;
        // Advance the ring to the next slot BEFORE building so this build never
        // targets the slot the just-bound previous TLAS occupies.
        m_tlasSlot = (m_tlasSlot + 1u) % kTlasSlots;
        Tlas& tl = m_tlasRing[m_tlasSlot];

        VkAccelerationStructureGeometryKHR geo{};
        geo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        geo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        geo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        geo.geometry.instances.arrayOfPointers = VK_FALSE;
        geo.geometry.instances.data.deviceAddress = m_instBuf.addr;

        VkAccelerationStructureBuildGeometryInfoKHR bgi{};
        bgi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        bgi.type  = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        bgi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        bgi.mode  = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        bgi.geometryCount = 1;
        bgi.pGeometries = &geo;

        VkAccelerationStructureBuildSizesInfoKHR sizes{};
        sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        m_pfnGetASBuildSizes(m_dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                             &bgi, &instCount, &sizes);

        if (tl.backing.size < sizes.accelerationStructureSize || tl.handle == VK_NULL_HANDLE) {
            if (tl.handle) { m_pfnDestroyAS(m_dev, tl.handle, nullptr); tl.handle = VK_NULL_HANDLE; }
            destroyBuffer(tl.backing);
            if (!createAsBackingBuffer(sizes.accelerationStructureSize, tl.backing)) {
                if (m_logError) m_logError("[rt] TLAS backing buffer alloc failed");
                return false;
            }
            VkAccelerationStructureCreateInfoKHR aci{};
            aci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
            aci.buffer = tl.backing.buf; aci.offset = 0;
            aci.size   = sizes.accelerationStructureSize;
            aci.type   = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
            if (m_pfnCreateAS(m_dev, &aci, nullptr, &tl.handle) != VK_SUCCESS) {
                if (m_logError) m_logError("[rt] vkCreateAccelerationStructureKHR (TLAS) failed");
                return false;
            }
        }

        // PERSISTENT scratch (grow-only). The old code did a vmaCreateBuffer +
        // vmaDestroyBuffer of a multi-megabyte scratch EVERY FRAME. The buffer is
        // reused only after the previous build finished — guaranteed by
        // endBlasBatch()'s fence wait, which is the batch's whole contract.
        if (m_tlasScratch.size < sizes.buildScratchSize + m_asProps.minAccelerationStructureScratchOffsetAlignment) {
            destroyBuffer(m_tlasScratch);
            if (!createScratchBuffer(sizes.buildScratchSize + sizes.buildScratchSize / 4u,
                                     m_tlasScratch)) {
                if (m_logError) m_logError("[rt] TLAS scratch alloc failed");
                return false;
            }
        }

        // THE ONE ORDERING THAT IS REQUIRED: every BLAS build/refit recorded into
        // this batch must complete before the TLAS build reads their storage.
        // (Replaces the per-refit barrier that used to serialize all 15 of them.)
        if (m_blasBatchDirty) { blasBatchAsBarrier(cmd); m_blasBatchDirty = false; }

        bgi.dstAccelerationStructure = tl.handle;
        bgi.scratchData.deviceAddress = m_tlasScratch.addr;
        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = instCount;
        range.primitiveOffset = 0; range.firstVertex = 0; range.transformOffset = 0;
        const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;
        m_pfnCmdBuildAS(cmd, 1, &bgi, &pRange);

        m_tlasBuilt = true;
        ++m_tlasBuilds;
        m_tlasCpuMs = (float)std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - cpuT0).count();
        return true;
    }

    // (The old std::vector-taking buildTlas() lived here. It has been replaced by
    // beginInstanceWrite() + recordTlasBuild() above: it packed a 96,076-row
    // staging vector and bulk-memcpy'd 6.1 MB into the instance buffer EVERY
    // frame, then paid its OWN submit + blocking fence wait on top of the BLAS
    // batch's. Both walks and one of the two round trips are gone.

    bool tlasBuilt() const { return m_tlasBuilt; }
    // Current (most-recently-built) TLAS handle — what every consumer descriptor
    // re-points to each rebuild. With the ring, this changes value on every build.
    VkAccelerationStructureKHR tlas() const { return m_tlasRing[m_tlasSlot].handle; }
    uint32_t lastInstanceCount() const { return m_lastInstanceCount; }
    uint32_t blasCount() const { return (uint32_t)m_blas.size(); }

    // ---- DOUBLE-BUFFER instrumentation (#5 PART 1) ------------------------
    // tlasBuilds   : total TLAS (re)builds since reset (per-frame in skinned-RT).
    // tlasSyncWaits: device-level waits the rebuild path paid to guard the backing
    //                — the metric we drive to ZERO with the ring (caller increments
    //                it on any vkDeviceWaitIdle it still issues around a build).
    // tlasCpuMs    : CPU wall time of the most recent buildTlas (record+submit+own
    //                fence wait), EXCLUDING any caller-side device wait.
    uint32_t tlasBuilds()    const { return m_tlasBuilds; }
    uint32_t tlasSyncWaits() const { return m_tlasSyncWaits; }
    float    tlasCpuMs()     const { return m_tlasCpuMs; }
    void     addTlasSyncWait() { ++m_tlasSyncWaits; }
    void     resetTlasCounters() { m_tlasBuilds = 0; m_tlasSyncWaits = 0; m_tlasCpuMs = 0.0f; }
    static constexpr uint32_t tlasSlots() { return kTlasSlots; }

    void shutdown() {
        if (!m_dev) return;
        drainBlasBatch();   // never destroy resources a pending submit still reads
        for (Tlas& t : m_tlasRing) {
            if (t.handle) { m_pfnDestroyAS(m_dev, t.handle, nullptr); t.handle = VK_NULL_HANDLE; }
            destroyBuffer(t.backing);
        }
        if (m_instMapped) { vmaUnmapMemory(m_alloc, m_instBuf.alloc); m_instMapped = nullptr; }
        destroyBuffer(m_instBuf);
        destroyBuffer(m_tlasScratch);
        for (auto& kv : m_blas) {
            if (kv.second.handle) m_pfnDestroyAS(m_dev, kv.second.handle, nullptr);
            destroyBuffer(kv.second.backing);
        }
        m_blas.clear();
        for (auto& kv : m_skinnedBlas) {
            if (kv.second.handle) m_pfnDestroyAS(m_dev, kv.second.handle, nullptr);
            destroyBuffer(kv.second.backing);
            destroyBuffer(kv.second.scratch);
        }
        m_skinnedBlas.clear();
        if (m_fence) { vkDestroyFence(m_dev, m_fence, nullptr); m_fence = VK_NULL_HANDLE; }
        if (m_pool)  { vkDestroyCommandPool(m_dev, m_pool, nullptr); m_pool = VK_NULL_HANDLE; }
        m_ready = false; m_tlasBuilt = false;
    }

private:
    struct Blas { VkAccelerationStructureKHR handle = VK_NULL_HANDLE; RtBuffer backing; VkDeviceAddress addr = 0; };
    struct Tlas { VkAccelerationStructureKHR handle = VK_NULL_HANDLE; RtBuffer backing; };
    // A skinned BLAS keeps its scratch RESIDENT (sized for max(build,update)) so a
    // per-frame refit costs zero allocation. triCount detects topology changes
    // (which force a full rebuild rather than an invalid UPDATE).
    struct SkinnedBlas {
        VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
        RtBuffer backing; RtBuffer scratch;
        VkDeviceAddress addr = 0; uint32_t triCount = 0;
    };

    // Barrier between consecutive AS builds/refits recorded into the SAME batch
    // command buffer: a skinned UPDATE reads + writes its AS storage, and the TLAS
    // build later reads every BLAS storage — serialize on ACCELERATION_STRUCTURE
    // build read/write so one refit doesn't race the next (they share scratch only
    // per-mesh, but the spec requires AS-build ordering be made explicit).
    void blasBatchAsBarrier(VkCommandBuffer cmd) {
        VkMemoryBarrier2 mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        mb.srcStageMask  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        mb.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        mb.dstStageMask  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        mb.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR
                         | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        VkDependencyInfo di{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &di);
    }

    VkDeviceAddress bufferAddress(VkBuffer b) const {
        VkBufferDeviceAddressInfo i{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        i.buffer = b;
        return vkGetBufferDeviceAddress(m_dev, &i);
    }

    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VmaAllocationCreateFlags allocFlags, RtBuffer& out) {
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = size;
        bci.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = allocFlags;
        if (vmaCreateBuffer(m_alloc, &bci, &aci, &out.buf, &out.alloc, nullptr) != VK_SUCCESS)
            return false;
        out.addr = bufferAddress(out.buf);
        out.size = size;
        return true;
    }

    // AS storage buffer: VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_KHR.
    bool createAsBackingBuffer(VkDeviceSize size, RtBuffer& out) {
        return createBuffer(size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, 0, out);
    }
    // Scratch buffer: STORAGE_BUFFER usage; address must satisfy the AS scratch
    // alignment. We over-allocate by the alignment and round the base address up.
    bool createScratchBuffer(VkDeviceSize size, RtBuffer& out) {
        const VkDeviceSize align = m_asProps.minAccelerationStructureScratchOffsetAlignment;
        const VkDeviceSize padded = size + (align ? align : 0);
        if (!createBuffer(padded, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0, out)) return false;
        if (align > 1 && (out.addr % align) != 0) {
            // Round the usable scratch address up to the required alignment. The
            // buffer was over-allocated by `align` bytes so the rounded range fits.
            out.addr = (out.addr + align - 1) & ~(align - 1);
        }
        return true;
    }
    // Instance buffer: ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY + host-visible
    // so the rows can be memcpy'd in before the TLAS build.
    bool createInstanceBuffer(VkDeviceSize size, RtBuffer& out) {
        return createBuffer(size,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT, out);
    }

    void destroyBuffer(RtBuffer& b) {
        if (b.buf) vmaDestroyBuffer(m_alloc, b.buf, b.alloc);
        b.buf = VK_NULL_HANDLE; b.alloc = nullptr; b.addr = 0; b.size = 0;
    }

    template <typename Fn>
    bool oneTimeSubmit(Fn&& record) {
        VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = m_pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(m_dev, &ai, &cmd) != VK_SUCCESS) return false;
        VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        record(cmd);
        vkEndCommandBuffer(cmd);
        VkCommandBufferSubmitInfo cs{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
        cs.commandBuffer = cmd;
        VkSubmitInfo2 submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        submit.commandBufferInfoCount = 1; submit.pCommandBufferInfos = &cs;
        VkResult sr = vkQueueSubmit2(m_queue, 1, &submit, m_fence);
        if (sr == VK_SUCCESS) {
            vkWaitForFences(m_dev, 1, &m_fence, VK_TRUE, UINT64_MAX);
            vkResetFences(m_dev, 1, &m_fence);
        }
        vkFreeCommandBuffers(m_dev, m_pool, 1, &cmd);
        return sr == VK_SUCCESS;
    }

    // Vulkan handles (not owned: device/phys/alloc/queue belong to the renderer).
    VkDevice         m_dev   = VK_NULL_HANDLE;
    VkPhysicalDevice m_phys  = VK_NULL_HANDLE;
    VmaAllocator     m_alloc = nullptr;
    VkQueue          m_queue = VK_NULL_HANDLE;
    uint32_t         m_family = 0;

    // Batched BLAS builds (boot-time): lazily allocated shared command buffer.
    VkCommandBuffer blasBatchCmd() {
        if (m_blasBatchOpen) return m_blasBatchCmd;
        if (!m_blasBatchCmd) {
            VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            ai.commandPool = m_pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(m_dev, &ai, &m_blasBatchCmd) != VK_SUCCESS) {
                m_blasBatchCmd = VK_NULL_HANDLE;
                return VK_NULL_HANDLE;
            }
        }
        VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(m_blasBatchCmd, &bi) != VK_SUCCESS) return VK_NULL_HANDLE;
        m_blasBatchOpen = true;
        return m_blasBatchCmd;
    }
    bool            m_blasBatchWanted = false;
    bool            m_blasBatchOpen   = false;
    bool            m_blasBatchPending = false;  // submitted, fence not yet waited
    // Set by every BLAS build/refit recorded into the open batch; consumed by
    // recordTlasBuild, which emits the single AS-write -> AS-read barrier that
    // orders all of them before the TLAS reads their storage. (This replaces the
    // barrier that used to fire after EVERY refit and serialized all of them.)
    bool            m_blasBatchDirty  = false;
    VkCommandBuffer m_blasBatchCmd    = VK_NULL_HANDLE;
    std::vector<RtBuffer> m_blasBatchScratch;

    VkCommandPool m_pool  = VK_NULL_HANDLE;
    VkFence       m_fence = VK_NULL_HANDLE;
    bool          m_ready = false;
    bool          m_tlasBuilt = false;
    bool          m_allowDataAccess = false;   // BLAS ALLOW_DATA_ACCESS (position fetch)
    uint32_t      m_lastInstanceCount = 0;

    VkPhysicalDeviceAccelerationStructurePropertiesKHR m_asProps{};

    std::unordered_map<uint32_t, Blas> m_blas;   // keyed by mesh id (static, cached)
    std::unordered_map<uint32_t, SkinnedBlas> m_skinnedBlas; // per-frame refit BLAS
    uint32_t  m_skinnedBuilds = 0;   // skinned BLAS full builds this batch (telemetry)
    uint32_t  m_skinnedRefits = 0;   // skinned BLAS refits this batch (telemetry)
    // DOUBLE-BUFFER TLAS ring (#5 PART 1): kTlasSlots independent backings/handles
    // ping-ponged per build so a build never overwrites a slot still being read by
    // an in-flight consumer. kFramesInFlight=2 needs 2; +1 headroom = 3.
    static constexpr uint32_t kTlasSlots = 3;
    Tlas      m_tlasRing[kTlasSlots];
    uint32_t  m_tlasSlot = kTlasSlots - 1u;   // first build advances to slot 0
    uint32_t  m_tlasBuilds    = 0;            // TLAS (re)builds since reset (telemetry)
    uint32_t  m_tlasSyncWaits = 0;            // device waits the rebuild path paid (-> 0)
    float     m_tlasCpuMs     = 0.0f;         // CPU ms of the most recent buildTlas
    // TLAS-SPLIT LANE: the instance buffer is now PERSISTENTLY MAPPED and only
    // PARTIALLY rewritten each frame (see beginInstanceWrite), so its contents ARE
    // the static portion of the TLAS input — they survive between frames. The
    // staging vector + 6.1 MB bulk memcpy that used to sit in front of it are gone.
    RtBuffer  m_instBuf;                          // persistent host-visible instance buffer
    void*     m_instMapped = nullptr;             // permanent map (write-combined: never read)
    // Persistent TLAS build scratch (grow-only). The old path created + destroyed a
    // multi-megabyte scratch buffer on EVERY frame's build.
    RtBuffer  m_tlasScratch;

    // Resolved KHR entry points (the loader import lib does not export these).
    PFN_vkCreateAccelerationStructureKHR            m_pfnCreateAS = nullptr;
    PFN_vkDestroyAccelerationStructureKHR           m_pfnDestroyAS = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR     m_pfnGetASBuildSizes = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR         m_pfnCmdBuildAS = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR  m_pfnGetASAddr = nullptr;

    void (*m_logInfo)(const char*)  = nullptr;
    void (*m_logError)(const char*) = nullptr;
};

} // namespace x3::rhi
