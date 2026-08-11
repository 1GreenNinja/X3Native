// #28 monolith split — VulkanRenderDevice resource methods (out-of-line).
// Bodies moved verbatim from the inline class; only inline->out-of-line mechanics
// (VulkanRenderDevice:: qualification, default-arg/override stripping) changed.
// See VulkanRenderDevice_internal.h for the class declaration.
#include "VulkanRenderDevice_internal.h"
#include "../VertexPack.h"
#include <unordered_set>

namespace x3::rhi {

// ---------------------------------------------------------------------------
// VERTEX COMPRESSION (Lane 5) — the two helpers every upload/PSO site shares.
// See engine/rhi/VertexPack.h for the formats and why this is device-wide.
// ---------------------------------------------------------------------------
void VulkanRenderDevice::meshVertexInput(VkVertexInputBindingDescription& bind,
                                         VkVertexInputAttributeDescription attrs[3]) const {
        bind = { 0, m_vtxStride, VK_VERTEX_INPUT_RATE_VERTEX };
        // Position is float3 at offset 0 in EVERY format (the RT BLAS build and
        // the velocity pass's prev-position binding both rely on that).
        attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };
        switch (m_vtxFmt) {
            case kVtxFmtNormal10:
                attrs[1] = { 1, 0, VK_FORMAT_A2B10G10R10_SNORM_PACK32, 12 };
                attrs[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT,            16 };
                break;
            case kVtxFmtNormal10Uv16:
                attrs[1] = { 1, 0, VK_FORMAT_A2B10G10R10_SNORM_PACK32, 12 };
                attrs[2] = { 2, 0, VK_FORMAT_R16G16_SFLOAT,            16 };
                break;
            default:
                attrs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, (uint32_t)offsetof(MeshVertex, normal) };
                attrs[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT,    (uint32_t)offsetof(MeshVertex, uv)     };
                break;
        }
    }

size_t VulkanRenderDevice::packMeshVertices(const MeshVertex* verts, uint32_t vcount,
                                            std::vector<uint8_t>& staging) const {
        const size_t bytes = (size_t)vcount * m_vtxStride;
        staging.resize(bytes);
        if (m_vtxFmt == kVtxFmtLegacy) {
            std::memcpy(staging.data(), verts, bytes);   // byte-identical to before
            return bytes;
        }
        return packVertices(verts, vcount, m_vtxFmt, staging.data());
    }

MeshHandle VulkanRenderDevice::createMesh(const MeshVertex* verts, uint32_t vcount,
                      const uint32_t* idx, uint32_t icount) {
        if (!verts || vcount == 0 || !idx || icount == 0) return {};
        // Parallel preload safe: staging alloc + memcpy run UNLOCKED (VMA is
        // internally synchronized) so concurrent loaders overlap their copies;
        // the shared batch-record happens under the lock inside
        // createDeviceLocalBuffer, and the registry write locks below.
        Mesh m{};
        // VERTEX COMPRESSION: the public API still takes 32 B MeshVertex (214 call
        // sites are untouched); the packing happens HERE, once, on upload.
        std::vector<uint8_t> vbStage;
        const VkDeviceSize vbBytes = (VkDeviceSize)packMeshVertices(verts, vcount, vbStage);
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
        if (!createDeviceLocalBuffer(vbStage.data(), vbBytes, vbUsage, m.vbo, m.vboAlloc)) return {};
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

// ---------------------------------------------------------------------------
// LOD CHAIN — N index buffers over ONE shared vertex buffer (Lane 5).
//
// The decimator (app/mesh_decimate.h) uses SUBSET placement, so every coarse
// level's indices address vertices level 0 already contains. That is what makes
// one vertex buffer sufficient: the chain costs one VBO plus N small IBOs
// instead of N full copies of the geometry.
//
// Each level becomes an ordinary Mesh with its own id, so the group/indirect
// draw path, the GPU cull and the CSM shadow loop need NO changes at all — an
// LOD switch is just the app submitting a different handle, and every instance
// that picked the same level batches into one indirect draw for free. The only
// thing that distinguishes a chain member is Mesh::vboShare, which refcounts the
// shared vertex buffer so the levels can be destroyed in any order.
//
// Bounds are computed PER LEVEL from the vertices that level actually
// references, not from the whole vertex array: a coarse level that dropped the
// tip of a spire genuinely has a smaller sphere, and the frustum cull should see
// the real one.
// ---------------------------------------------------------------------------
uint32_t VulkanRenderDevice::createMeshLodChain(const MeshVertex* verts, uint32_t vcount,
                                                const uint32_t* const* idx, const uint32_t* icount,
                                                uint32_t levels, MeshHandle* outMeshes) {
        if (!verts || vcount == 0 || !idx || !icount || levels == 0 || !outMeshes) return 0;
        if (levels > 8) levels = 8;
        for (uint32_t i = 0; i < levels; ++i) outMeshes[i] = MeshHandle{};

        // Validate before allocating anything: every index in range, every level
        // at least one triangle.
        for (uint32_t l = 0; l < levels; ++l) {
            if (!idx[l] || icount[l] < 3) return 0;
            for (uint32_t k = 0; k < icount[l]; ++k)
                if (idx[l][k] >= vcount) return 0;
        }

        std::vector<uint8_t> vbStage;
        const VkDeviceSize vbBytes = (VkDeviceSize)packMeshVertices(verts, vcount, vbStage);
        VkBufferUsageFlags vbUsage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        VkBufferUsageFlags ibUsage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        if (m_rtSupported) {
            const VkBufferUsageFlags rtUsage =
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
            vbUsage |= rtUsage; ibUsage |= rtUsage;
        }
        VkBuffer      sharedVbo   = VK_NULL_HANDLE;
        VmaAllocation sharedAlloc = nullptr;
        if (!createDeviceLocalBuffer(vbStage.data(), vbBytes, vbUsage, sharedVbo, sharedAlloc)) return 0;

        std::lock_guard<std::recursive_mutex> lk(m_uploadMu);
        const uint32_t share = m_nextVboShare++;
        uint32_t made = 0;
        for (uint32_t l = 0; l < levels; ++l) {
            Mesh m{};
            m.vbo      = sharedVbo;
            m.vboAlloc = sharedAlloc;
            m.vboShare = share;                 // aliased: destroyMesh must refcount
            const VkDeviceSize ibBytes = (VkDeviceSize)icount[l] * sizeof(uint32_t);
            if (!createDeviceLocalBuffer(idx[l], ibBytes, ibUsage, m.ibo, m.iboAlloc)) break;
            m.indexCount  = icount[l];
            m.vertexCount = vcount;

            float bmin[3] = {  std::numeric_limits<float>::max(),
                               std::numeric_limits<float>::max(),
                               std::numeric_limits<float>::max() };
            float bmax[3] = { -std::numeric_limits<float>::max(),
                              -std::numeric_limits<float>::max(),
                              -std::numeric_limits<float>::max() };
            for (uint32_t k = 0; k < icount[l]; ++k) {
                const MeshVertex& v = verts[idx[l][k]];
                for (int a = 0; a < 3; ++a) {
                    if (v.pos[a] < bmin[a]) bmin[a] = v.pos[a];
                    if (v.pos[a] > bmax[a]) bmax[a] = v.pos[a];
                }
            }
            for (int a = 0; a < 3; ++a) { m.bmin[a] = bmin[a]; m.bmax[a] = bmax[a]; }
            m.boundsCenter = glm::vec3(0.5f * (bmin[0] + bmax[0]),
                                       0.5f * (bmin[1] + bmax[1]),
                                       0.5f * (bmin[2] + bmax[2]));
            float r2 = 0.0f;
            for (uint32_t k = 0; k < icount[l]; ++k) {
                const MeshVertex& v = verts[idx[l][k]];
                const float dx = v.pos[0] - m.boundsCenter.x;
                const float dy = v.pos[1] - m.boundsCenter.y;
                const float dz = v.pos[2] - m.boundsCenter.z;
                r2 = std::max(r2, dx * dx + dy * dy + dz * dz);
            }
            m.boundsRadius = std::sqrt(r2);

            const uint32_t id = m_nextMeshId++;
            m_meshes.emplace(id, m);
            outMeshes[l] = MeshHandle{ id };
            ++made;
        }

        if (made == 0) {                        // nothing took: don't leak the VBO
            vmaDestroyBuffer(m_alloc, sharedVbo, sharedAlloc);
            return 0;
        }
        VboShare vs{};
        vs.buf = sharedVbo; vs.alloc = sharedAlloc; vs.refs = made;
        m_vboShares.emplace(share, vs);
        return made;
    }

void VulkanRenderDevice::cameraLodInfo(float outEye[3], float& outFovYDeg,
                                       uint32_t& outHeightPx) const {
        outEye[0] = m_camPos.x; outEye[1] = m_camPos.y; outEye[2] = m_camPos.z;
        // m_camFov is the VERTICAL field of view: vk_passes.cpp builds the
        // projection with glm::perspective(glm::radians(m_camFov), aspect, ...).
        outFovYDeg = m_camFov;
        // m_height is the INTERNAL render height (m_outH * ssaa) — the resolution
        // the geometry is actually rasterized at, which is what a pixel-error
        // budget must be measured against.
        outHeightPx = (m_height > 0) ? m_height : 1080u;
    }

uint64_t VulkanRenderDevice::meshVertexBytes() const {
        uint64_t total = 0;
        std::unordered_set<uint32_t> countedShares;
        for (const auto& kv : m_meshes) {
            const Mesh& m = kv.second;
            if (m.vboShare != 0) {
                if (!countedShares.insert(m.vboShare).second) continue;   // shared: count once
            }
            const uint32_t slots = m.dynamic ? kFramesInFlight : 1u;
            total += (uint64_t)m.vertexCount * m_vtxStride * slots;
        }
        return total;
    }

bool VulkanRenderDevice::meshBounds(MeshHandle h, float outMin[3], float outMax[3]) const {
        auto it = m_meshes.find(h.id);
        if (it == m_meshes.end() || it->second.vertexCount == 0) return false;
        for (int a = 0; a < 3; ++a) { outMin[a] = it->second.bmin[a]; outMax[a] = it->second.bmax[a]; }
        return true;
    }

void VulkanRenderDevice::destroyMesh(MeshHandle h) {
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
        } else if (m.vboShare != 0) {
            // LOD-chain member: the vertex buffer is SHARED with the chain's other
            // levels. Release one reference and only defer the free when this was
            // the last level standing, so the levels may be destroyed in any order.
            auto sit = m_vboShares.find(m.vboShare);
            if (sit != m_vboShares.end()) {
                if (sit->second.refs > 0) --sit->second.refs;
                if (sit->second.refs == 0) {
                    deferDestroyBuffer(sit->second.buf, sit->second.alloc);
                    m_vboShares.erase(sit);
                }
            }
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

void VulkanRenderDevice::updateMesh(MeshHandle h, const MeshVertex* verts, uint32_t vcount) {
        if (!verts || vcount == 0) return;
        auto it = m_meshes.find(h.id);
        if (it == m_meshes.end()) return;
        Mesh& m = it->second;
        if (vcount != m.vertexCount) return;  // count must match the original mesh
        // LOD-chain members ALIAS one vertex buffer. Promoting one of them to
        // dynamic would free a buffer the other levels still bind, so a chain is
        // static by contract: refuse rather than corrupt. (Nothing in the engine
        // does this; the guard exists so a future caller fails loudly, not subtly.)
        if (m.vboShare != 0) {
            logError("[rhi] updateMesh on an LOD-chain mesh is not supported (shared vertex buffer)");
            return;
        }
        std::vector<uint8_t> vbStage;
        const VkDeviceSize bytes = (VkDeviceSize)packMeshVertices(verts, vcount, vbStage);
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
                // CPU-skinned (updateMesh) dynamic vbos are also a per-frame BLAS
                // build input for r_skinnedrt when RT is supported (same rationale +
                // raster-identity guarantee as the GPU-skinning output above).
                if (m_rtSupported)
                    bci.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                               | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
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
                std::memcpy(mapped, vbStage.data(), (size_t)bytes);   // seed all slots (packed)
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
        std::memcpy(m.dynMapped[fi], vbStage.data(), (size_t)bytes);
        // HOST_VISIBLE allocations from VMA_MEMORY_USAGE_AUTO may not be coherent;
        // flush so the GPU sees the write (no-op if host-coherent).
        vmaFlushAllocation(m_alloc, m.dynVboAlloc[fi], 0, bytes);
    }

TextureHandle VulkanRenderDevice::createTexture(const void* rgba8, uint32_t w, uint32_t h, bool srgb) {
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

void VulkanRenderDevice::destroyTexture(TextureHandle h) {
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

TextureHandle VulkanRenderDevice::registerTerrainMaterial(TextureHandle grass, TextureHandle rock,
                                      TextureHandle snow,  TextureHandle sand) {
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

bool VulkanRenderDevice::createDebris() {
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

void VulkanRenderDevice::destroyDebris() {
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

void VulkanRenderDevice::gpuDebrisConfig(const IRenderDevice::GpuDebrisParams& p) { m_debrisParams = p; }

uint32_t VulkanRenderDevice::gpuDebrisAliveCount() const {
        if (m_debrisCountMapped) return ((const uint32_t*)m_debrisCountMapped)[0];
        return m_debrisAlive;
    }

uint32_t VulkanRenderDevice::gpuDebrisCapacity() const { return kDebrisCapacity; }

uint32_t VulkanRenderDevice::gpuDebrisSpawnBurst(const float pos[3], uint32_t count, float speed,
                             float lifetime, float halfExtent, uint32_t seed) {
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

void VulkanRenderDevice::gpuDebrisStep(float dt) {
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

void VulkanRenderDevice::gpuDebrisDraw(const FrameContext& fc, const float tint[4]) {
        if (!fc.valid || !m_debrisDrawPipeline) return;
        GpuDebrisDrawUBO u{};
        u.viewProj = m_lastViewProj;
        u.color = tint ? glm::vec4(tint[0], tint[1], tint[2], tint[3]) : glm::vec4(0.7f, 0.55f, 0.4f, 1.0f);
        if (m_debrisDrawUboMapped[m_frameIdx])
            std::memcpy(m_debrisDrawUboMapped[m_frameIdx], &u, sizeof(u));
        m_debrisDrawPending = true;  // buildAndExecuteGraph records the instanced cube draw
    }

IRenderDevice::GpuDebrisStats VulkanRenderDevice::gpuDebrisReadback(float boundsLimit) const {
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

bool VulkanRenderDevice::createSkinning() {
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

void VulkanRenderDevice::destroySkinning() {
        // Free all registered skinned meshes' resources first.
        for (auto& kv : m_skinnedMeshes) destroySkinnedMeshResources(kv.second);
        m_skinnedMeshes.clear();
        m_skinPending.clear();
        if (m_skinPipeline)       { vkDestroyPipeline(m_dev.device, m_skinPipeline, nullptr); m_skinPipeline = VK_NULL_HANDLE; }
        if (m_skinPipelineLayout) { vkDestroyPipelineLayout(m_dev.device, m_skinPipelineLayout, nullptr); m_skinPipelineLayout = VK_NULL_HANDLE; }
        if (m_skinSetLayout)      { vkDestroyDescriptorSetLayout(m_dev.device, m_skinSetLayout, nullptr); m_skinSetLayout = VK_NULL_HANDLE; }
        if (m_skinPool)           { vkDestroyDescriptorPool(m_dev.device, m_skinPool, nullptr); m_skinPool = VK_NULL_HANDLE; }
    }

void VulkanRenderDevice::destroySkinnedMeshResources(SkinnedMesh& sm) {
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

bool VulkanRenderDevice::promoteMeshForSkinning(Mesh& m, const MeshVertex* bindVerts, uint32_t vcount) {
        const VkDeviceSize bytes = (VkDeviceSize)vcount * sizeof(MeshVertex);
        VkBuffer made[kFramesInFlight] = {}; VmaAllocation madeA[kFramesInFlight] = {};
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            // Seed from the bind pose via a staging copy so a draw before the first
            // dispatch (or a pass that reads a not-yet-dispatched slot) shows valid
            // geometry rather than garbage.
            // The skinned OUTPUT vbo is also the per-frame BLAS build input for the
            // skinned-TLAS feature (r_skinnedrt): when RT is supported, add the
            // device-address + AS-build-input usage so VulkanRT::ensureSkinnedBlas
            // can read it. These flags don't change raster binding (still a vertex
            // buffer) -> raster output byte-identical; only added when RT is on.
            VkBufferUsageFlags outUsage =
                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                    | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;   // readbackSkinnedMesh copies from it (test path)
            if (m_rtSupported)
                outUsage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                          | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
            if (!createDeviceLocalBuffer(bindVerts, bytes, outUsage,
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

bool VulkanRenderDevice::registerSkinnedMesh(MeshHandle mesh, const MeshVertex* bindVerts, uint32_t vcount,
                         const uint16_t* jointIdx4, const float* jointWt4) {
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

void VulkanRenderDevice::unregisterSkinnedMesh(MeshHandle mesh) {
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

void VulkanRenderDevice::setSkinnedPalette(MeshHandle mesh, const float* palette, uint32_t jointCount) {
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

bool VulkanRenderDevice::readbackSkinnedMesh(MeshHandle mesh, MeshVertex* out, uint32_t vcount) {
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

bool VulkanRenderDevice::createPerFrame() {
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

void VulkanRenderDevice::destroyPerFrame() {
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            auto& fr = m_frames[i];
            if (fr.tsPool) vkDestroyQueryPool(m_dev.device, fr.tsPool, nullptr);
            if (fr.inFlight) vkDestroyFence(m_dev.device, fr.inFlight, nullptr);
            if (fr.imageAvailable) vkDestroySemaphore(m_dev.device, fr.imageAvailable, nullptr);
            if (fr.pool) vkDestroyCommandPool(m_dev.device, fr.pool, nullptr);
            fr = Frame{};
        }
    }

void VulkanRenderDevice::noteCreate(const char* what, uint32_t& lateCounter, uint32_t& frameCounter) {
        ++frameCounter;
        if (m_firstFrameBegun && !m_creationBoundary) {
            ++lateCounter;
            if (m_pacing.strictPso)
                logError(std::string("[stutter] ") + what +
                         " created after first frame (frame " + std::to_string(m_totalFrames) +
                         ") — precompile it at boot or inside a declared recreate boundary");
        }
    }

VkResult VulkanRenderDevice::x3CreateGraphicsPipelines(uint32_t n, const VkGraphicsPipelineCreateInfo* ci,
                                   const VkAllocationCallbacks* ac, VkPipeline* out) {
        const auto t0 = std::chrono::steady_clock::now();
        VkResult r = vkCreateGraphicsPipelines(m_dev.device, m_pipelineCache, n, ci, ac, out);
        m_psoCreateMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (r == VK_SUCCESS) { m_psoTotal += n; noteCreate("graphics pipeline", m_psoLate, m_psoThisFrame); }
        return r;
    }

VkResult VulkanRenderDevice::x3CreateComputePipelines(uint32_t n, const VkComputePipelineCreateInfo* ci,
                                  const VkAllocationCallbacks* ac, VkPipeline* out) {
        const auto t0 = std::chrono::steady_clock::now();
        VkResult r = vkCreateComputePipelines(m_dev.device, m_pipelineCache, n, ci, ac, out);
        m_psoCreateMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (r == VK_SUCCESS) { m_psoTotal += n; noteCreate("compute pipeline", m_psoLate, m_psoThisFrame); }
        return r;
    }

VkResult VulkanRenderDevice::x3CreateDescriptorPool(const VkDescriptorPoolCreateInfo* ci,
                                const VkAllocationCallbacks* ac, VkDescriptorPool* out) {
        VkResult r = vkCreateDescriptorPool(m_dev.device, ci, ac, out);
        if (r == VK_SUCCESS) noteCreate("descriptor pool", m_poolsLate, m_poolsThisFrame);
        return r;
    }

VkResult VulkanRenderDevice::x3vmaCreateBuffer(const VkBufferCreateInfo* bci, const VmaAllocationCreateInfo* aci,
                           VkBuffer* buf, VmaAllocation* alloc, VmaAllocationInfo* info) {
        VkResult r = vmaCreateBuffer(m_alloc,bci, aci, buf, alloc, info);
        if (r == VK_SUCCESS) ++m_allocsThisFrame;
        return r;
    }

VkResult VulkanRenderDevice::x3vmaCreateImage(const VkImageCreateInfo* ici, const VmaAllocationCreateInfo* aci,
                          VkImage* img, VmaAllocation* alloc, VmaAllocationInfo* info) {
        VkResult r = vmaCreateImage(m_alloc,ici, aci, img, alloc, info);
        if (r == VK_SUCCESS) ++m_allocsThisFrame;
        return r;
    }

bool VulkanRenderDevice::createDeviceLocalBuffer(const void* data, VkDeviceSize bytes,
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

bool VulkanRenderDevice::createSampledTexture(const void* rgba8, uint32_t w, uint32_t h, bool srgb, Texture& out) {
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
                // SYNC (WAW): src STAGE must name the command that actually wrote
                // this mip. Mip 0 came from vkCmdCopyBufferToImage (COPY_BIT); every
                // mip after it came from vkCmdBlitImage, which runs in BLIT_BIT —
                // and COPY_BIT does NOT include BLIT_BIT. Hard-coding COPY_BIT left
                // the blit's TRANSFER_WRITE outside this barrier's src scope, so the
                // layout transition (itself a write) was unordered against it:
                // SYNC-HAZARD-WRITE-AFTER-WRITE, "previously written by
                // vkCmdBlitImage ... at VK_PIPELINE_STAGE_2_BLIT_BIT". The access
                // mask was already correct; only the stage was wrong.
                barrierMip(i - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           (i == 1) ? VK_PIPELINE_STAGE_2_COPY_BIT : VK_PIPELINE_STAGE_2_BLIT_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT,
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
            // Last mip is still TRANSFER_DST -> SHADER_READ. SAME src-stage rule as
            // the loop above: with a mip chain the last mip was produced by the
            // final vkCmdBlitImage (BLIT_BIT); only a single-mip texture was
            // produced by vkCmdCopyBufferToImage (COPY_BIT).
            barrierMip(mipLevels - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       (mipLevels == 1) ? VK_PIPELINE_STAGE_2_COPY_BIT : VK_PIPELINE_STAGE_2_BLIT_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT,
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

void VulkanRenderDevice::destroyTextureObj(Texture& t) {
        if (t.sampler) vkDestroySampler(m_dev.device, t.sampler, nullptr);
        if (t.view)    vkDestroyImageView(m_dev.device, t.view, nullptr);
        if (t.image)   vmaDestroyImage(m_alloc, t.image, t.alloc);
        t = Texture{};
    }

VkCommandBuffer VulkanRenderDevice::batchCmd() {
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

void VulkanRenderDevice::retireBatchSlot(uint32_t s, bool blocking) {
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

void VulkanRenderDevice::submitUploadBatch() {
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

void VulkanRenderDevice::waitUploadBatch(bool blocking ) {
        std::lock_guard<std::recursive_mutex> lk(m_uploadMu);
        retireBatchSlot(0, blocking);
        retireBatchSlot(1, blocking);
    }

void VulkanRenderDevice::flushUploadBatch() {
        std::lock_guard<std::recursive_mutex> lk(m_uploadMu);
        submitUploadBatch();
        waitUploadBatch(true);
    }

void VulkanRenderDevice::beginUploadBatch() {
        if (m_batchActive) return;       // nestable-safe
        m_batchActive = true;
        m_batchOps = 0; m_batchFlushes = 0; m_batchMs = 0.0;
    }

void VulkanRenderDevice::endUploadBatch() {
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

void VulkanRenderDevice::writeBindlessSlot(uint32_t slot, const Texture& tex) {
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

bool VulkanRenderDevice::registerBindless(Texture& tex) {
        if (m_nextBindless >= kMaxTextures) return false;
        tex.bindlessIndex = m_nextBindless++;
        writeBindlessSlot(tex.bindlessIndex, tex);
        return true;
    }

void VulkanRenderDevice::deferDestroyBuffer(VkBuffer buf, VmaAllocation alloc) {
        if (!buf) return;
        // Safe to free once kFramesInFlight frames have begun past the current one
        // (every in-flight cmd buffer that could reference it will have retired).
        m_pendingFrees.push_back({ buf, alloc, m_totalFrames + kFramesInFlight });
    }

void VulkanRenderDevice::deferDestroyImage(VkImage image, VmaAllocation alloc,
                       VkImageView view, VkSampler sampler) {
        if (!image && !view && !sampler) return;
        m_pendingImageFrees.push_back({ image, alloc, view, sampler,
                                        m_totalFrames + kFramesInFlight });
    }

void VulkanRenderDevice::drainPendingFrees() {
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

void VulkanRenderDevice::flushPendingFrees() {
        for (auto& pf : m_pendingFrees) vmaDestroyBuffer(m_alloc, pf.buf, pf.alloc);
        m_pendingFrees.clear();
        for (auto& p : m_pendingImageFrees) {
            if (p.sampler) vkDestroySampler(m_dev.device, p.sampler, nullptr);
            if (p.view)    vkDestroyImageView(m_dev.device, p.view, nullptr);
            if (p.image)   vmaDestroyImage(m_alloc, p.image, p.alloc);
        }
        m_pendingImageFrees.clear();
    }

} // namespace x3::rhi
