#pragma once
// ============================================================================
// LOD chain building — decimate a mesh and upload the chain to the device.
//
// CLEAN-ROOM, original work (see app/mesh_decimate.h and app/mesh_lod.h for the
// technique provenance). No GPL / id Tech / RBDOOM / Unreal source consulted.
//
// This is the ONE place that turns "a mesh someone built" into a populated
// MeshLodChain. Everything else in the app either has a chain or does not; the
// selector treats "no chain" as "always LOD0", which is exactly today's
// behaviour for every mesh in the engine.
//
// AUTHORED vs GENERATED: buildLodChain() generates the chain by decimating LOD0
// (nothing in this repo ships authored LOD chains yet — the gen_lod.py referred
// to by app/space/lod.h does not exist). makeLodChain() takes index lists you
// supply instead, which is the hook for an authored/loaded chain later; the
// selector cannot tell them apart.
// ============================================================================

#include "mesh_lod.h"

#include <cstdint>
#include <vector>

namespace x3 { namespace rhi { class IRenderDevice; } }

namespace x3::game {

struct LodChainStats {
    uint32_t triangles[kMaxLodLevels] = { 0, 0, 0, 0 };
    float    error[kMaxLodLevels]     = { 0, 0, 0, 0 };   // metres, model space
    uint32_t levels = 0;
    double   buildMs = 0.0;
};

// Decimate `verts`/`idx` into up to `levels` levels at the given triangle
// ratios (relative to the PREVIOUS level, applied successively) and upload them
// as one shared-vertex-buffer chain. Returns a chain with levels == 1 (LOD0
// only, indistinguishable from an ordinary createMesh) if decimation cannot
// usefully reduce the mesh — e.g. a 12-triangle box.
//
// `ratios` has `levels-1` entries. Default 0.5 / 0.5 / 0.4 gives roughly
// 100% / 50% / 25% / 10% of the original triangle count.
MeshLodChain buildLodChain(x3::rhi::IRenderDevice& device,
                           const x3::rhi::MeshVertex* verts, uint32_t vcount,
                           const uint32_t* idx, uint32_t icount,
                           uint32_t levels = 4,
                           const float* ratios = nullptr,
                           LodChainStats* outStats = nullptr);

// Upload an ALREADY-AUTHORED chain: `idx[i]`/`icount[i]` for level i, with the
// per-level model-space geometric error supplied by the author. No decimation.
MeshLodChain makeLodChain(x3::rhi::IRenderDevice& device,
                          const x3::rhi::MeshVertex* verts, uint32_t vcount,
                          const uint32_t* const* idx, const uint32_t* icount,
                          const float* error, uint32_t levels);

// Destroy every level of a chain (safe in any order; the shared vertex buffer is
// refcounted by the device).
void destroyLodChain(x3::rhi::IRenderDevice& device, MeshLodChain& chain);

} // namespace x3::game
