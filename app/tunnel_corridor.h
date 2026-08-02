#pragma once
// ===========================================================================
// TUNNEL CORRIDOR — the VISIBLE half of the terrain-corridor primitive.
//
// app/terrain.h's TerrainCorridor lowers h(x,z) along a polyline. That is a
// NUMBER. This module turns it into something you can look at and drive
// through: it picks a real hill by SAMPLING the canonical height field, grades
// a road profile through it, registers ONE corridor whose depth profile is
// derived from that grade, and then lays a road ribbon + an arched tunnel tube
// + portal headwalls into the depression the corridor just made.
//
// THE SHAPE OF THE TRICK (why there is no hole in the ground):
//   * On the APPROACHES the corridor is cut all the way down to the road, so
//     the road runs in an OPEN CUTTING with sloped shoulders. No shell.
//   * Under the HILL the corridor is only cut down to (road + tube + soil
//     cover), so the ground stays ABOVE the tube. That stretch is ENCLOSED and
//     gets walls + a crown. The terrain is the tunnel's lid; there is no hole,
//     no CSG, no voxel, and h(x,z) is still single-valued.
//   * The step between those two regimes is the PORTAL. Real tunnels have a
//     headwall there for exactly the same reason.
//
// PROVENANCE (clean-room — docs/CLEANROOM_PROCESS.md). The TECHNIQUE was
// learned by studying the behaviour of the Babylon/BL predecessor world, as
// analysed in docs/design/BL_WORLD_PORT.md:
//   * §2.2 — BL pins the road to a flat datum and LOWERS THE TERRAIN onto it
//     rather than putting the road on piers. That inversion is the whole idea,
//     and it is what app/terrain.h's corridor primitive already implements.
//   * §2.3 / §3.2 — BL has two tunnel cross-sections: a swept semi-elliptical
//     arch (auto-tunnels) and a five-box rectangular shell (named tunnels). We
//     port the CROSS-SECTION IDEA (arch, springing off short vertical walls,
//     with a constant-thickness shell and an annular portal face) and none of
//     BL's placement, dimensions or code. BL builds its bore with
//     ExtrudeShape + a CSG boolean; we emit the annulus directly (§4.3a), which
//     needs no boolean and gives the portal ring for free as the end cap.
//   * §3.3 — the DRESSING intervals (light strips ~15 m, lane markings on a 5 m
//     grid, solid edge lines) are ported AS DATA, deliberately thinned.
//   * §4.4 — the LIGHT BUDGET. BL's four named tunnels would want 48 of the
//     engine's 64 forward point lights. We do NOT port the "a PointLight on
//     every 3rd strip" ratio: the strips are EMISSIVE GEOMETRY (free) and only
//     a handful of real point lights are spent. See kMaxBoreLights.
// No BL code was transcribed; the implementation is entirely our own.
//
// Everything here is app-layer: prims + Scene entities + addStaticMesh. No
// engine/ change, no renderer change.
// ===========================================================================

#include "scene.h"
#include "terrain.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>
#include <vector>

namespace x3::game {

// --- Cross-section + corridor constants (metres) ---------------------------
// Road-scale, not BL's cavernous 72x34 auto-tunnels (BL_WORLD_PORT.md §2.3
// notes those are sized to swallow a hill, not to carry a road). These are in
// the range of BL's NAMED tunnels (14-16 wide, 8-9 tall) and of a real 2-lane
// motorway bore.
constexpr float kTcRoadHalfWidth  = 6.0f;   // drivable ribbon half-width
constexpr float kTcTubeHalfWidth  = 7.0f;   // interior half-width at the springing
constexpr float kTcTubeWallH      = 3.6f;   // vertical wall height before the arch
constexpr float kTcTubeCrownH     = 7.6f;   // interior crown height above the road datum
constexpr float kTcShellThick     = 0.9f;   // wall/crown thickness
constexpr float kTcMinSoilCover   = 3.5f;   // ground kept above the tube's outer crown
// Corridor footprint. The flat floor must be wider than the tube's outer shell
// so the tube sits INSIDE the depression, never straddling its shoulder.
constexpr float kTcCorridorHalfW  = 8.8f;
// The shoulder run is deliberately SHORT. It is not just a cosmetic slope: the
// corridor's depth step at a portal is rounded off over roughly
// (halfWidth + falloff) by the union's end caps, and over that run the ground
// sweeps from road level up past the tube's crown — i.e. straight through the
// bore. Every metre of falloff is a metre of earth ramp inside the tunnel
// mouth, so keep it tight and let the cutting walls be steep, rock-cut walls.
constexpr float kTcCorridorFall   = 10.0f;
// Road grading. kTcMinCut keeps the ribbon in a shallow groove even on open
// ground so it never fights the bumpy natural surface; kTcMaxGrade is the
// steepest the profile may climb (4.5 %, a real motorway limit).
constexpr float kTcMinCut         = 1.6f;
constexpr float kTcMaxGrade       = 0.045f;
// The honest cost of the technique (BL_WORLD_PORT.md §4.3b: "the hill above a
// tunnel gets a visible saddle"). kTcMaxScar CAPS how much material the bore
// reach may remove, so the ridge gets a shallow dip instead of being gutted.
constexpr float kTcMaxScar        = 9.0f;
// §4.4: keep at most a handful of REAL point lights in the bore. BL's own
// ratio (a PointLight on every 3rd 15 m strip) would be ~23 lights for a 350 m
// tunnel — 36 % of the engine's entire 64-light forward budget for one tunnel.
constexpr uint32_t kTcMaxBoreLights = 6;

// One sampled station along the corridor centreline.
struct TunnelStation {
    float x = 0.0f, z = 0.0f;    // world XZ on the spine
    float s = 0.0f;              // arc length from the first node (m)
    float ground = 0.0f;         // NATURAL height on the spine (pre-corridor)
    float latMin = 0.0f;         // min/max natural height across the corridor band
    float latMax = 0.0f;
    float roadY = 0.0f;          // graded road datum (the ribbon's top sits just above)
    float depth = 0.0f;          // what the corridor is asked to remove here
    bool  bore = false;          // design intent: this reach is enclosed
};

// The chosen route + everything derived from it. Filled by registerRoute().
struct TunnelRoute {
    std::vector<TunnelStation> st;
    float dirX = 1.0f, dirZ = 0.0f;   // unit XZ heading (constant — a straight run)
    float totalLen = 0.0f;
    // The SHELLED span [boreS0, boreS1] in arc length, measured against the
    // FINAL (post-corridor) height field rather than the design intent, so it
    // can never disagree with what the terrain actually does. It runs from
    // where the trench floor first LEAVES road level to where it comes back —
    // NOT merely the fully-buried reach. That distinction is the whole trick:
    // the corridor's depth step is rounded off over ~halfWidth by the union's
    // end caps, so there is always a ~25 m ramp where the ground is above the
    // road but below the crown. Roof only the buried part and that ramp buries
    // the road instead; roof the ramp too and the tube simply emerges from the
    // bank, which is what a real portal does anyway.
    float boreS0 = 0.0f, boreS1 = 0.0f;
    // The fully-buried sub-span (>= the required soil cover) — reported, and
    // used to decide whether there is a tunnel here at all.
    float coverS0 = 0.0f, coverS1 = 0.0f;
    // Total length of road INSIDE the shell that still has an earth ramp over
    // it. This is the technique's irreducible residual and is reported, not
    // hidden: a single-valued heightfield cannot step vertically, so at each
    // mouth the ground must sweep continuously from road level up past the
    // crown, and over that sweep it necessarily crosses the bore.
    float buriedRoadLen = 0.0f;
    bool  boreValid = false;

    // Interpolated queries (clamped to the ends).
    float roadYAt(float s) const;
    void  posAt(float s, float out[3]) const;   // {x, roadY, z}
};

// BOOT ENTRY POINT. Samples the canonical height field, grades the road,
// registers exactly ONE TerrainCorridor, then re-samples the FINAL field to
// find the genuinely enclosed span. MUST be called before the first
// TerrainStreamer::init() / horizon-ring build, per app/terrain.h's registry
// contract ("register at BOOT, before the first height query"). Idempotent:
// the second call returns the cached route without touching the registry.
const TunnelRoute& registerTunnelCorridor();

// The built world: road ribbon, tunnel shell, portals, markings, lights.
class TunnelCorridorWorld {
public:
    // `route` must come from registerTunnelCorridor().
    bool build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, const TunnelRoute& route);

    void shutdown(x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics);

    const std::vector<x3::rhi::PointLight>& lights() const { return m_lights; }
    uint32_t entityCount() const { return m_entities; }

    // Camera poses for the proof shots: {x,y,z,yaw,pitch}. `which`:
    //   0 approach (on the road, outside the entrance portal, looking in)
    //   1 inside   (mid-bore)
    //   2 far mouth (outside the exit portal, looking BACK at it)
    //   3 saddle   (above the ridge, showing the scar the corridor leaves)
    //   4 portal detail (three-quarter close-up of the concrete/terrain seam)
    void showcaseCamera(const TunnelRoute& route, int which, float cam[5]) const;

private:
    std::vector<x3::rhi::MeshHandle>    m_meshes;
    std::vector<x3::rhi::TextureHandle> m_textures;
    std::vector<x3::phys::BodyId>       m_bodies;
    std::vector<x3::rhi::PointLight>    m_lights;
    uint32_t m_entities = 0;
};

} // namespace x3::game
