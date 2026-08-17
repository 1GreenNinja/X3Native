#pragma once
// ===========================================================================
// ROAD TREES — tall broadleaf GROVES along the tunnel-corridor road.
//
// Tim (2026-08): "We need somE Tall Trees!! Shading the road... In some
// areas."  SOME areas — clustered stands with open country between them, not
// a continuous hedge. This module plants the oak + poplar published in
// 53a2b560 ("assets: tall broadleaf trees — oak + poplar, published to the
// store", assets/converted_glb/nature/) in deterministic groves along the
// TunnelRoute from app/tunnel_corridor.h.
//
// MECHANISM (reused, not invented): EnvArtSystem's district API — the exact
// path echo_woodlands.cpp uses for its pine belts (beginFromDir +
// addGlbInstance per placement, one GPU upload per GLB, per-instance world
// transforms, setFoliage vegetation shading). Placement rules:
//   * OUTSIDE the pavement + apron: |lat| >= 14 m from the centreline
//     (pavement half-width 6 m, corridor floor half-width 8.8 m, retaining
//     walls at the batter toe ~9 m — trees stand beyond all of it, on the
//     upper batter / natural ground).
//   * SUN SIDE FIRST. The host's sun is at (0.35, 0.92, 0.18), so shadows
//     fall toward +lat across this route; groves sit mostly on the -lat
//     shoulder so their crowns actually shade the pavement (the mission's
//     whole point). A minority of trees take the +lat side for enclosure.
//   * NOT at the portals: the roofed span [boreS0, boreS1] plus a margin for
//     headwalls/wingwalls/backfill taper is excluded.
//   * FEET ON THE GROUND (X3_WORLD_RULES rule 4): base at
//     terrainHeightAtWorld (the FINAL field, corridor cut included), sunk
//     0.15 m so no root plate floats on a slope; steep-batter and
//     below-water and below-road positions are rejected.
//   * DETERMINISTIC: one seeded LCG, no rand(), same forest every boot.
//
// KNOWN LIMIT (say it, don't hide it): EnvArtSystem issues one drawMeshPBR
// per tree (no GPU instancing, no LOD chain), so counts are kept modest
// (~100-200 trees). 2026-08-16: both GLBs were reharvested with REAL bark +
// alpha-masked leaf textures (see road_trees.cpp), replacing the earlier
// textureless flat-tint placeholders. Their far-LOD "Billboard" card nodes
// are still skipped — the plain PBR LOD0 mesh is the only draw.
// ===========================================================================

#include "env_art.h"
#include "tunnel_corridor.h"

#include "engine/rhi/IRenderDevice.h"

#include <cstdint>
#include <vector>

namespace x3::game {

class RoadTrees {
public:
    // XZ disc no trunk may stand in. The host passes one per showcase camera:
    // the exit-portal three-quarter pose (shot 08) sits ON the bank at lat -21
    // — inside the planting band — and without the keep-out a crown swallows
    // the camera and the whole frame is leaf cards (first capture round).
    struct KeepOut { float x = 0.0f, z = 0.0f, r = 12.0f; };

    // Plant the groves. `route` must come from registerTunnelCorridor() (the
    // heights this reads include the corridor cut, so call AFTER it). Returns
    // false if the tree GLBs failed to load (nothing draws; the road is
    // simply treeless — never fatal).
    // (The old minBenchY shim is gone: since task #32 the DRAWN river surface
    // follows the same worldWaterLevelAt table, so the water-table check IS
    // the drawn waterline — one truth. Benches additionally clear the rain-
    // runoff head-room, kWorldRiverRainRiseMax.)
    bool build(x3::rhi::IRenderDevice& device, const TunnelRoute& route,
               const std::vector<KeepOut>& keepOut = {});

    // Draw all trees. Call each frame alongside scene.render() — same pattern
    // as DriveDemo::render / EnvArtSystem::draw. No-op before build / after
    // shutdown. Returns drawables issued.
    uint32_t draw(x3::rhi::IRenderDevice& device,
                  const x3::rhi::FrameContext& frame) const;

    // Release every GPU resource (terminal — see EnvArtSystem::destroy).
    void shutdown(x3::rhi::IRenderDevice& device);

    uint32_t treeCount()  const { return m_trees; }
    uint32_t groveCount() const { return m_groves; }
    uint32_t benchCount() const { return m_benches; }

private:
    EnvArtSystem m_art;
    uint32_t m_trees   = 0;
    uint32_t m_groves  = 0;
    uint32_t m_benches = 0;   // armory bench models seated under the groves
    bool     m_built   = false;
};

} // namespace x3::game
