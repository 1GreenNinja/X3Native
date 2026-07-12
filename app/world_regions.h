#pragma once
// EFLZ Act-2 open world — SURROUNDING SURFACE REGIONS + the 4 MOUNTAIN RANGES.
// Game/slice content only — engine/ stays pure.
//
// CLEAN-ROOM, original work. Built ONLY from X3Native's OWN systems (Scene,
// terrain.*, mesh_prims) + the engine interfaces + the EFLZ design (Tim's IP, incl.
// his own Q3Engine world modules x3-world-surface.js / x3-mountains.js). NO RBDOOM /
// id Tech / Doom / Quake engine source consulted.
//
// SCOPE / OWNERSHIP (open-world lane): the LANDMARK regions of the Keth'zar surface
// that the Act-2 LEVEL lanes (act2_world L8/9, act2_desert L10/11, act2_caves
// L12-15) sit IN — the crash site, the two outposts, and the 4 distant mountain
// ranges — placed as graybox geometry on the engine's procedural terrain surface
// (via the PURE placeOnTerrain sampler, no tiles needed). Read-only on the terrain;
// touches no other lane's files. Mirrors the act2_world.* authoring pattern
// (build / queries / a headless --test-worldregions). Coordinates come from the
// blueprint gazetteer (docs/design/X3_WORLD_BLUEPRINT.md §1) so regions do not
// overlap the desert / caves / swamp the sibling lanes place.

#include "scene.h"
#include "terrain.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>
#include <string>
#include <vector>

namespace x3::game {

// The surrounding open-world regions (landmarks; NOT the numbered Act-2 levels).
enum class WorldRegion : uint32_t {
    CrashSite = 0,       // Jake's shuttle wreck — the surface start point
    EastOutpost,         // military camp + antenna farm
    WestOutpost,         // drill rig + processing plant (industrial)
    NorthernRange,       // jagged snow-capped peaks
    EasternRange,        // volcanic basalt + lava veins
    SouthernRange,       // mesa / plateau sandstone + ancient ruins
    WesternHighlands,    // mossy rolling hills + crystal formations
    Count
};
constexpr uint32_t kWorldRegionCount = (uint32_t)WorldRegion::Count;

// One region's authored descriptor (read by the host HUD + the self-test so neither
// re-derives footprint / position / kind).
struct WorldRegionPlan {
    WorldRegion region   = WorldRegion::CrashSite;
    const char* name     = "";
    const char* biome    = "";
    float       cx       = 0.0f;   // world center X
    float       cz       = 0.0f;   // world center Z
    float       radius   = 0.0f;   // footprint radius (m)
    float       peakH    = 0.0f;   // peak height above the surface (m); 0 => flat outpost
    bool        isMountain = false;
    uint32_t    propCount = 0;     // graybox boxes placed for this region
};

// Open-world surface regions host. Build once after the terrain world exists; the
// graybox props are plain Scene entities (drawn by the host's scene.render()).
class WorldRegions {
public:
    // Place the region graybox geometry onto the (canonical) terrain surface. Pure
    // placement (placeOnTerrain) — safe before any tile is resident. Call once.
    void build(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics);

    // ---- Queries (host HUD + self-test) ----
    bool built() const { return m_built; }
    const WorldRegionPlan& plan(WorldRegion r) const { return m_plan[(uint32_t)r]; }
    uint32_t propCount() const { return (uint32_t)m_props.size(); }   // total graybox props
    // Count of regions flagged as mountain ranges (must be 4).
    uint32_t mountainCount() const;
    // W9 (terrain drama): number of water-ribbon segments THE RIVER placed
    // (one mitred strip mesh; segments = worldRiverNodes()-1). 0 => no river.
    uint32_t riverSegmentCount() const { return m_riverSegments; }

private:
    bool m_built = false;
    WorldRegionPlan m_plan[kWorldRegionCount];
    std::vector<uint32_t> m_props;   // Scene entity ids of all placed graybox boxes
    uint32_t m_riverSegments = 0;    // W9: river water ribbon segments
};

// Headless self-test (--test-worldregions). Builds the regions on a HeadlessDevice +
// Jolt world and asserts: all 7 regions present with their authored center/biome; the
// 3 outposts are flat (peakH==0) and the 4 mountain ranges are tall (peakH>0) at the
// expected radii (6-9 km out); every region placed >=1 graybox prop sitting ON the
// surface; the regions don't overlap the inner Act-2 level band. Prints
// "worldregions: X/Y passed"; returns true iff all pass. No window/Vulkan.
bool runWorldRegionsSelfTest();

} // namespace x3::game
