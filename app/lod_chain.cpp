// LOD chain building — see lod_chain.h.
#include "lod_chain.h"

#include "mesh_decimate.h"
#include "engine/rhi/IRenderDevice.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <chrono>
#include <string>

namespace x3::game {

MeshLodChain buildLodChain(x3::rhi::IRenderDevice& device,
                           const x3::rhi::MeshVertex* verts, uint32_t vcount,
                           const uint32_t* idx, uint32_t icount,
                           uint32_t levels, const float* ratios,
                           LodChainStats* outStats) {
    MeshLodChain chain{};
    if (!verts || !idx || vcount == 0 || icount < 3) return chain;
    levels = std::clamp<uint32_t>(levels, 1u, kMaxLodLevels);

    const auto t0 = std::chrono::steady_clock::now();
    static const float kDefaultRatios[kMaxLodLevels - 1] = { 0.5f, 0.5f, 0.4f };
    if (!ratios) ratios = kDefaultRatios;

    meshBoundingSphere(verts, vcount, chain.center, chain.radius);

    // Level 0 is the input, verbatim.
    std::vector<std::vector<uint32_t>> lodIdx;
    std::vector<float>                 lodErr;
    lodIdx.emplace_back(idx, idx + icount);
    lodErr.push_back(0.0f);

    // Successive decimation: each level is decimated FROM the previous one and
    // carries the previous level's accumulated displacement forward, which makes
    // the error sequence monotonically non-decreasing by construction (the
    // property lodSelect() relies on to stop at the first level over budget).
    for (uint32_t l = 1; l < levels; ++l) {
        const std::vector<uint32_t>& prev = lodIdx.back();
        const DecimateResult r = decimateMesh(verts, vcount, prev.data(), (uint32_t)prev.size(),
                                              ratios[l - 1], lodErr.back());
        // Nothing gained (already minimal, or the collapse guards blocked
        // everything): stop the chain here rather than shipping a duplicate.
        if (r.indices.size() >= prev.size() || r.indices.size() < 3) break;
        lodIdx.push_back(r.indices);
        lodErr.push_back(r.maxError);
    }

    const uint32_t made = (uint32_t)lodIdx.size();
    std::vector<const uint32_t*> ptrs(made);
    std::vector<uint32_t>        counts(made);
    for (uint32_t l = 0; l < made; ++l) { ptrs[l] = lodIdx[l].data(); counts[l] = (uint32_t)lodIdx[l].size(); }

    x3::rhi::MeshHandle handles[kMaxLodLevels]{};
    const uint32_t up = device.createMeshLodChain(verts, vcount, ptrs.data(), counts.data(), made, handles);
    if (up == 0) return chain;

    chain.levels = up;
    for (uint32_t l = 0; l < up; ++l) {
        chain.mesh[l]      = handles[l];
        chain.error[l]     = lodErr[l];
        chain.triangles[l] = counts[l] / 3;
    }

    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
    if (outStats) {
        outStats->levels = up;
        outStats->buildMs = ms;
        for (uint32_t l = 0; l < up; ++l) {
            outStats->triangles[l] = chain.triangles[l];
            outStats->error[l]     = chain.error[l];
        }
    }
    return chain;
}

MeshLodChain makeLodChain(x3::rhi::IRenderDevice& device,
                          const x3::rhi::MeshVertex* verts, uint32_t vcount,
                          const uint32_t* const* idx, const uint32_t* icount,
                          const float* error, uint32_t levels) {
    MeshLodChain chain{};
    if (!verts || !idx || !icount || vcount == 0 || levels == 0) return chain;
    levels = std::min<uint32_t>(levels, kMaxLodLevels);
    meshBoundingSphere(verts, vcount, chain.center, chain.radius);

    x3::rhi::MeshHandle handles[kMaxLodLevels]{};
    const uint32_t up = device.createMeshLodChain(verts, vcount, idx, icount, levels, handles);
    if (up == 0) return chain;
    chain.levels = up;
    for (uint32_t l = 0; l < up; ++l) {
        chain.mesh[l]      = handles[l];
        chain.error[l]     = error ? error[l] : 0.0f;
        chain.triangles[l] = icount[l] / 3;
    }
    return chain;
}

void destroyLodChain(x3::rhi::IRenderDevice& device, MeshLodChain& chain) {
    for (uint32_t l = 0; l < chain.levels; ++l)
        if (chain.mesh[l].valid()) device.destroyMesh(chain.mesh[l]);
    chain = MeshLodChain{};
}

} // namespace x3::game
