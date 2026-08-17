#pragma once
// ===========================================================================
// WORLD FORESTS — the owner's brown regions from the map, planted for real.
//
// THE SPEC IS THE SKETCH: docs/design/ROAD_NETWORK_SKETCH_V2.png. Brown =
// "This Color is All Forest": a belt across the whole north edge, a
// centre-north patch, the NE corner, a southern belt along the countryside
// road ("Tthick woods on much of the road!!!!"), forest skirting both
// mountains, and the strip along the CLIFFSIDE HIGHWAY beside the LARGE
// RIVER. Native compass: +Z = north, +X = east (terrain.cpp kRanges block).
//
// WHY THIS IS NOT road_trees.cpp: RoadTrees plants ~10^2 trees in a roadside
// band of ONE route and draws every LOD0 mesh every frame. A forest is 10^4
// trees over km^2 — it needs a far LOD and per-frame pruning, or it eats the
// frame. This module adds exactly that and nothing else:
//
//   * TWO TIERS PER TREE. Near (< ~90 m): the textured LOD0 mesh (bark +
//     alpha-masked leaves — the same drawables road_trees ships). Far: the
//     GLB's own "Billboard" CROSS-CARD node (two perpendicular quads, 4 tris,
//     atlas UVs authored by the pack) — textured 2026-08-16 by
//     tools/inject_billboard_tex.py from the source packs' real billboard
//     atlases (NO_SLOP rule 3: road_trees skips the card because it used to
//     be an untextured grey stand-in; it is not one any more). Hysteresis on
//     the tier switch so a tree parked at the threshold does not flicker.
//
//   * THE RENDERER IS ALREADY AN INSTANCER. Draw records are grouped by mesh
//     id and each group is ONE VkDrawIndexedIndirectCommand
//     (firstInstance = the group's SSBO base row, instanceCount = survivors;
//     see vk_passes.cpp) with GPU frustum + HZB cull compacting per instance.
//     So N thousand trees are ~6 indirect draws (oak bark/leaves/card +
//     poplar bark/leaves/card); what this module must bound is the CPU
//     record-submission count, which is what the chunk pruning below does.
//
//   * CHUNKED SUBMISSION. Trees are bucketed into 160 m chunks at build; per
//     frame a chunk is skipped whole when it is beyond the far draw radius or
//     entirely behind the camera (half-space on the camera forward). Only
//     surviving chunks walk their trees.
//
// PLACEMENT LAW (same contract as road_trees, extended to areas):
//   * DETERMINISTIC: every candidate position/yaw/scale/species derives from
//     an order-independent integer hash of its (grid cell, region seed) — no
//     rand(), no sequence coupling; the forest is identical every boot.
//   * CONTACT (X3_WORLD_RULES rule 4): trunk base at terrainHeightAtWorld
//     (the FINAL carved field — build runs AFTER every registerRoad /
//     registerTunnelCorridor*), sunk 0.15 m; steep-slope and water rejects.
//   * KEEP-OUTS: any registered corridor footprint (pavement + aprons +
//     falloff — terrainCorridorContains covers every road and bore), road
//     junctions (distToNearestRoadJunction), the demo route's roadside band
//     (road_trees owns |lat| 14-24 m; we stay >= 26 m off its spine), the
//     river/sea water, the facility rect + city pads + outposts (mirrored
//     from terrain.cpp's guards — PAIRED, see forest.cpp).
//   * THE OWNER'S 70-100 ft SPEC: scale rolls are byte-identical to
//     road_trees.cpp's (PAIRED — a change to one is a change to both).
// ===========================================================================

#include "tunnel_corridor.h"
#include "road_network.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/asset/IModelLoader.h"
#include "engine/asset/IAssetSource.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace x3::game {

class WorldForests {
public:
    struct Inputs {
        // The demo/spawn route — its roadside band is road_trees' lane.
        const TunnelRoute* demoRoute = nullptr;
        // The inner tour — the southern countryside belt follows its south arc.
        const RoadSpec* innerTour = nullptr;
    };

    // Plant every region. Call AFTER all corridor/road registration and after
    // RoadTrees::build (the keep-outs read the final registries). Returns
    // false if the tree GLBs failed to load (nothing draws — never fatal).
    bool build(x3::rhi::IRenderDevice& device, const Inputs& in);

    // Submit this frame's visible trees. cam = camera world pos; fwdX/fwdZ =
    // camera forward in XZ (need not be normalized; used for a conservative
    // behind-the-camera chunk reject). Returns records submitted.
    uint32_t draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                  const float cam[3], float fwdX, float fwdZ);

    void shutdown(x3::rhi::IRenderDevice& device);

    uint32_t treeCount() const { return (uint32_t)m_trees.size(); }

private:
    struct Drawable {                 // resolved once from the loaded model
        x3::asset::ModelDrawable d;
    };
    struct Species {
        x3::asset::Model model;
        std::vector<Drawable> lod0;   // bark + leaves (node "…LOD0")
        std::vector<Drawable> card;   // the textured billboard cross-card
        bool ok = false;
    };
    struct Tree {
        float t[16];                  // full world transform (yaw * scale + pos)
        uint8_t species;              // 0 oak, 1 poplar
        uint8_t nearTier;             // hysteresis state (1 = LOD0 last frame)
    };
    struct Chunk {
        float cx, cz;                 // centre
        uint32_t begin, end;          // range in m_trees
    };

    void submitTree(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                    Tree& tr, bool nearTier, uint32_t& drawn);

    std::unique_ptr<x3::asset::IAssetSource> m_assets;
    std::unique_ptr<x3::asset::IModelLoader> m_loader;
    Species m_species[2];
    std::vector<Tree>  m_trees;
    std::vector<Chunk> m_chunks;
    uint32_t m_oaks = 0, m_poplars = 0;
    bool m_built = false, m_destroyed = false;
};

} // namespace x3::game
