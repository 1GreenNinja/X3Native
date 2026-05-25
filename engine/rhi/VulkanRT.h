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

    bool hasBlas(uint32_t meshId) const { return m_blas.find(meshId) != m_blas.end(); }

    void destroyBlas(uint32_t meshId) {
        auto it = m_blas.find(meshId);
        if (it == m_blas.end()) return;
        if (it->second.handle) m_pfnDestroyAS(m_dev, it->second.handle, nullptr);
        destroyBuffer(it->second.backing);
        m_blas.erase(it);
    }

    // ---- TLAS: one instance per (meshId, transform) ---------------------------
    struct TlasInstance {
        uint32_t meshId;
        float    model[16];   // column-major 4x4 (the renderer's ObjectData::model)
    };

    // Rebuild the TLAS from the given instance list (instances whose mesh has no
    // BLAS are skipped). Recorded as a one-time submit (static-first v1). The TLAS
    // device address is then bound by the ray-query pass. Returns true on success;
    // an empty (zero usable instances) list still produces a valid empty TLAS.
    bool buildTlas(const std::vector<TlasInstance>& instances) {
        if (!m_ready) return false;

        // Pack the VkAccelerationStructureInstanceKHR rows for every instance whose
        // BLAS exists. The transform is the top 3 rows of the column-major model,
        // stored ROW-MAJOR (VkTransformMatrixKHR is a 3x4 row-major matrix).
        m_instScratch.clear();
        m_instScratch.reserve(instances.size());
        for (const TlasInstance& in : instances) {
            auto it = m_blas.find(in.meshId);
            if (it == m_blas.end()) continue;
            VkAccelerationStructureInstanceKHR row{};
            // column-major model[c*4+r]; row-major transform[r][c] = model[c*4+r].
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c)
                    row.transform.matrix[r][c] = in.model[c * 4 + r];
            row.instanceCustomIndex = 0;
            row.mask = 0xFF;
            row.instanceShaderBindingTableRecordOffset = 0;
            row.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            row.accelerationStructureReference = it->second.addr;
            m_instScratch.push_back(row);
        }
        const uint32_t instCount = (uint32_t)m_instScratch.size();
        m_lastInstanceCount = instCount;

        // Upload the instance rows into a device-address instance buffer. A valid
        // (even empty) TLAS is built so the descriptor binding is always live.
        const VkDeviceSize instBytes =
            (VkDeviceSize)(instCount ? instCount : 1) * sizeof(VkAccelerationStructureInstanceKHR);
        if (m_instBuf.size < instBytes) {
            destroyBuffer(m_instBuf);
            if (!createInstanceBuffer(instBytes, m_instBuf)) {
                if (m_logError) m_logError("[rt] TLAS instance buffer alloc failed");
                return false;
            }
        }
        if (instCount) {
            void* mapped = nullptr;
            if (vmaMapMemory(m_alloc, m_instBuf.alloc, &mapped) != VK_SUCCESS) return false;
            std::memcpy(mapped, m_instScratch.data(),
                        (size_t)instCount * sizeof(VkAccelerationStructureInstanceKHR));
            vmaFlushAllocation(m_alloc, m_instBuf.alloc, 0, instBytes);
            vmaUnmapMemory(m_alloc, m_instBuf.alloc);
        }

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

        // (Re)create the TLAS backing if it must grow. The TLAS handle is recreated
        // each time the backing changes; for steady same-size rebuilds we reuse it.
        if (m_tlas.backing.size < sizes.accelerationStructureSize || m_tlas.handle == VK_NULL_HANDLE) {
            if (m_tlas.handle) { m_pfnDestroyAS(m_dev, m_tlas.handle, nullptr); m_tlas.handle = VK_NULL_HANDLE; }
            destroyBuffer(m_tlas.backing);
            if (!createAsBackingBuffer(sizes.accelerationStructureSize, m_tlas.backing)) {
                if (m_logError) m_logError("[rt] TLAS backing buffer alloc failed");
                return false;
            }
            VkAccelerationStructureCreateInfoKHR aci{};
            aci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
            aci.buffer = m_tlas.backing.buf;
            aci.offset = 0;
            aci.size   = sizes.accelerationStructureSize;
            aci.type   = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
            if (m_pfnCreateAS(m_dev, &aci, nullptr, &m_tlas.handle) != VK_SUCCESS) {
                if (m_logError) m_logError("[rt] vkCreateAccelerationStructureKHR (TLAS) failed");
                return false;
            }
        }

        RtBuffer scratch{};
        if (!createScratchBuffer(sizes.buildScratchSize, scratch)) {
            if (m_logError) m_logError("[rt] TLAS scratch alloc failed");
            return false;
        }

        bgi.dstAccelerationStructure = m_tlas.handle;
        bgi.scratchData.deviceAddress = scratch.addr;
        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = instCount;
        range.primitiveOffset = 0; range.firstVertex = 0; range.transformOffset = 0;
        const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;

        const bool ok = oneTimeSubmit([&](VkCommandBuffer cmd){
            m_pfnCmdBuildAS(cmd, 1, &bgi, &pRange);
        });
        destroyBuffer(scratch);
        if (!ok) return false;

        m_tlasBuilt = true;
        return true;
    }

    bool tlasBuilt() const { return m_tlasBuilt; }
    VkAccelerationStructureKHR tlas() const { return m_tlas.handle; }
    uint32_t lastInstanceCount() const { return m_lastInstanceCount; }
    uint32_t blasCount() const { return (uint32_t)m_blas.size(); }

    void shutdown() {
        if (!m_dev) return;
        if (m_tlas.handle) { m_pfnDestroyAS(m_dev, m_tlas.handle, nullptr); m_tlas.handle = VK_NULL_HANDLE; }
        destroyBuffer(m_tlas.backing);
        destroyBuffer(m_instBuf);
        for (auto& kv : m_blas) {
            if (kv.second.handle) m_pfnDestroyAS(m_dev, kv.second.handle, nullptr);
            destroyBuffer(kv.second.backing);
        }
        m_blas.clear();
        if (m_fence) { vkDestroyFence(m_dev, m_fence, nullptr); m_fence = VK_NULL_HANDLE; }
        if (m_pool)  { vkDestroyCommandPool(m_dev, m_pool, nullptr); m_pool = VK_NULL_HANDLE; }
        m_ready = false; m_tlasBuilt = false;
    }

private:
    struct Blas { VkAccelerationStructureKHR handle = VK_NULL_HANDLE; RtBuffer backing; VkDeviceAddress addr = 0; };
    struct Tlas { VkAccelerationStructureKHR handle = VK_NULL_HANDLE; RtBuffer backing; };

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

    VkCommandPool m_pool  = VK_NULL_HANDLE;
    VkFence       m_fence = VK_NULL_HANDLE;
    bool          m_ready = false;
    bool          m_tlasBuilt = false;
    uint32_t      m_lastInstanceCount = 0;

    VkPhysicalDeviceAccelerationStructurePropertiesKHR m_asProps{};

    std::unordered_map<uint32_t, Blas> m_blas;   // keyed by mesh id
    Tlas      m_tlas;
    RtBuffer  m_instBuf;                          // persistent host-visible instance buffer
    std::vector<VkAccelerationStructureInstanceKHR> m_instScratch;

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
