#pragma once
// S2 — Distance-LOD system (Act-3 space engine, spec §4 / 2.6).
//
// A tiny, RHI-free policy object: given a precomputed chain of mesh handles
// (LOD0 = highest detail .. LOD3 = lowest) and a set of ascending switch
// distances, select() returns the handle to draw for the current camera
// distance. The caller owns the actual rhi::createMesh handles; this layer
// only decides WHICH one to draw, so it adds zero RHI surface area and is
// trivially unit-testable headless (no GPU).
//
// The mesh chain itself is generated offline by tools/gen_lod.py (headless
// Blender decimation at ratios 0.5 / 0.25 / 0.1, mirroring
// tools/rodin_to_glb/downscale_glb.py). Assets ship LOD0-only per
// assets/converted_glb/CATALOG.md; gen_lod.py regenerates the chain.
//
// Clean-room: no idTech / Doom / Quake source consulted; the distance-band
// selection is the standard discrete-LOD scheme.

#include <cstdint>

namespace x3::space {

// A populated LOD chain for one drawable. `mesh[i]` is the rhi mesh handle id
// for level i (0 = highest detail). A 0 handle marks an unused slot. The
// `switchDist` thresholds are the camera distances at which the system steps
// DOWN one level: switchDist[0] = LOD0->LOD1, [1] = LOD1->LOD2, [2] = LOD2->LOD3.
// They must be ascending. `levels` is how many of the four slots are actually
// populated (1..4); select() never returns a level >= levels (it clamps to the
// highest populated level so a 2-LOD asset viewed from far away just stays on
// its lowest available mesh).
struct LodSet {
    uint32_t mesh[4]       = {0, 0, 0, 0};   // LOD0..LOD3 rhi mesh handle ids; 0 = unused
    float    switchDist[3] = {0, 0, 0};      // ascending thresholds: 0->1, 1->2, 2->3
    uint32_t levels        = 1;              // populated count 1..4
};

class LodSystem {
public:
    // Return the mesh handle id to draw for a camera `distance` from the object.
    // Walks the ascending switchDist thresholds and picks the matching band,
    // then clamps the chosen level to the highest populated one (levels-1) so a
    // chain with fewer than 4 LODs never indexes an empty slot. Negative /
    // sub-first-threshold distances map to LOD0. Returns 0 only if the set is
    // empty (levels == 0) — a defensive guard, not a normal path.
    uint32_t select(const LodSet& set, float distance) const;

    // Build an LodSet from an explicit chain of mesh handles + switch distances.
    // `levels` clamps to [1,4]; slots beyond `levels` are zeroed so an
    // accidentally-passed stale handle can't leak into select(). The three
    // distances are stored as given (caller is responsible for ascending order;
    // select() tolerates a non-ascending set but the bands degrade gracefully).
    LodSet makeFromChain(uint32_t lod0, uint32_t lod1, uint32_t lod2, uint32_t lod3,
                         float d01, float d12, float d23, uint32_t levels) const;
};

} // namespace x3::space
