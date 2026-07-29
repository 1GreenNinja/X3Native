#pragma once
// ECHO ROADS — the procedural road network for Echotropolis (Tim's order:
// "Curving freeway aerial structures... interchanges... streets that flow
// around the harbor in nicely angled grid sections"). Target bar: GTA5-class
// freeway GEOMETRY (curved banked deck, barriers, lane paint, pillar rows,
// trumpet interchanges) and CP2077-class street bones (angled harbor grids
// with curbs + sidewalks + lamp light slices). Game/slice code only.
//
// REPLACES (at integration time): the host's FREEWAY NETWORK kRoute block
// (straight 40 m GLB deck segments), its hand-mirrored kFreeway copy in the
// woodlands keep-out, and the glowing kRoute ribbon — the curved ring below
// deliberately shadows the SAME ten waypoints so the Recife/Urban gates, the
// crown crossing, and the woodlands corridor keep-out all still line up.
//
// ============================= INTEGRATION =============================
//  V7 SURFACE PASS (new obligations):
//   7. TEXTURES: assets/roads/{asphalt,concrete,sidewalk,grime}_tile.png are
//      loaded at build() via stb_image + device.createTexture (sRGB). Missing
//      files log a warning and fall back to the flat v6 colors — ship the
//      four PNGs with the world.
//   8. NIGHT GLOW: after roads.draw(...), ALSO call
//          if (tod.sample().cityLightsOn) roads.drawNightGlow(*device, frame);
//      in BOTH the live and headless fans — lamp heads glow at night only.
//   9. The CA sweep on-ramps replaced the trumpet curls; law exemption dropped
//      (they must PASS the ramp curvature law — check the boot PASS line).
// (for WP-0 / the host integrator — this module is complete but UNWIRED)
//  1. CMake: add world_hosts/echo_roads.cpp to app/CMakeLists.txt.
//  2. Build once at boot (after hf.load, before first frame):
//         x3::game::EchoRoads roads;
//         roads.build(*device, hf);          // ~6-9 meshes, one-time
//  3. Draw every frame (both live + screenshot paths), persistent lane for
//     now — regionization can slice by edge class later:
//         roads.draw(*device, frame);
//  4. Lights: merge into the host's per-frame light selection with the SAME
//     day/night gate the street lamps use (tod.sample().cityLightsOn) — the
//     module NEVER bakes emissive into materials (Tim's "neon never sleeps"
//     bug class). appendNearLights-style: roads.lights() is a static slice;
//     nearest-K select from it alongside lamp/district lights.
//  5. RETIRE from the host once this draws: the FREEWAY NETWORK block
//     (placeDeckP/placePillar loop over kRoute), the kRoute glow ribbon, and
//     switch the woodlands keep-out corridor + car AI to graph() centerlines
//     (RoadGraph is built for exactly those consumers — see below).
//  6. Do NOT wire before milestone A's byte-compare is banked: this module
//     intentionally CHANGES pixels (it replaces the old freeway).
//  7. FIRST-RUN CHECK (v2 harbor): the boulevard/blocks find the shore by
//     probing the heightfield at build() — grep the boot log for "[roads]"
//     lines: "nudged inland" is normal near coves; "skipped"/"no waterline"
//     means a shore seed or the land threshold needs a second look before
//     shipping the capture set.
//  8. COLLISION (v3 — the "fall through the bridge" fix): after build(),
//     hand the drivable surface to physics ONCE:
//         const auto& rc = roads.collisionMesh();
//         if (!rc.indices.empty())
//             phys->addStaticMesh(rc.verts.data(),  (uint32_t)(rc.verts.size()/3),
//                                 rc.indices.data(), (uint32_t)rc.indices.size());
//     (match the exact addStaticMesh signature at the terrain-mesh call site —
//     the shape is the same one the 2600 m terrain collider uses). Asphalt
//     top surface only; barriers/paint/sidewalks are cosmetic by design.
//  9. JUNCTIONS (v3): build logs "[roads] junctions: N patches". Zero patches
//     on a real build = the detector regressed; eyeball a harbor-grid
//     crossing + a gate tee in the first capture set.
// 10. V4.1 TOPOLOGY (the reroute): the ring is now a MESA-RIM + FLATS loop —
//     fixed NE/E arc (crown crossing -> gentle NE slope -> Recife gate ->
//     flats sweep) plus RADIALLY PROBED rim waypoints (bearings 320 -> 140
//     deg from the crown center; rim = LAST outward sample at 80% of crown
//     elevation, waypoint 45 m inset; degenerate bearings + backtrack
//     pockets dropped with logs — grep "[roads] V4.1 rim route: K/N").
//     The old shore-bowl legs are GONE — the shanty shore belongs to the
//     ground-level Harbor Boulevard; the freeway overlooks it from the rim.
//     Consequences for consumers: the woodlands keep-out corridor and any
//     car-AI route MUST come from graph() (the probed route is not knowable
//     statically); the Urban gate trumpet auto-reattaches to the new nearest
//     deck leg (east approach). Acceptance: "[roads] deck profile: max pier
//     height N m" <= 45 on a real heightfield; taller means a rim probe
//     regressed (check the rim-seed skip logs first).
// 11. V5: (a) MINE SPUR — rim deck -> mini tee-ramp -> terrain-conformed
//     avenue -> truck-lot cul-de-sac (log "[roads] V5 mine spur: ...");
//     (b) RIM-EDGE INSET — deck samples hanging >25 m over air inside the
//     rim zone migrate inboard until they sit over mesa again (log
//     "[roads] V5 rim inset: N deck samples migrated inboard"). Expect the
//     pier colonnade below the west cliff to be GONE in captures; the
//     max-pier log remains the acceptance gate.
// 12. V6 — THE ANTI-ZIGZAG LAW + WELDED RIBBONS: every edge passes a final
//     smooth + per-class curvature clamp (PHASE 1.9; per-edge worst logged,
//     "[roads] zigzag law: PASS|FAIL"; intentional loops carry lawExempt;
//     violators respline once then DROP — the owner's law: a missing road
//     beats a zigzag). The V5 inset is now arc-filtered (80 m ramps — no
//     staircase switchbacks) and the boulevard east tie is a quadratic
//     Bezier (the woodlands-zigzag root cause: mismatched hermite tangent
//     magnitudes). Ribbons/barriers WELD (shared verts, smooth bank
//     normals, continuous UVs, single-sided tops + index-only deck
//     undersides; "[roads] weld: ..." logs the savings). If tops cull
//     wrong on a device: flip kFlipTopWinding in the .cpp — one constant.
// =======================================================================
//
// Deterministic by construction: no rand — the same hash discipline as the
// woodlands scatter. All tunables are named constants in the .cpp.

#include "echo_heightfield.h"

#include "engine/rhi/IRenderDevice.h"

#include <cstdint>
#include <vector>

namespace x3::game {

// ---------------------------------------------------------------------------
// Road graph — the data shape is designed for the two roadmapped consumers:
//   * CAR AI (pillar: drivable cars / traffic): follow RoadEdge::center
//     samples; laneCenterOffset() turns a lane index into a lateral offset so
//     a vehicle can hold lane 0/1 in either direction. Ramps/streets connect
//     POSITIONALLY (node xz == a point on/near another edge) — consumers snap
//     by proximity, no cross-edge index bookkeeping to go stale.
//   * HOUSE/STREET ALIGNMENT (pillar: real placement, not hash-yaw rings):
//     walk an Avenue/HarborStreet edge's samples; a lot faces the road when
//     its yaw = atan2(tangent) ± 90°, its frontage sits at
//     sample ± perp * (width/2 + setback).
// ---------------------------------------------------------------------------
enum class RoadClass : uint8_t {
    Freeway = 0,     // elevated 2+2 deck, barriers, banked curves, pillars
    Ramp,            // interchange link, grades deck <-> ground
    Avenue,          // ground trunk road, curbs + sidewalks
    HarborStreet,    // harbor-grid street, curbs + sidewalks, lamp-lit
};

struct RoadSample {
    float x = 0, y = 0, z = 0;   // centerline point (world; y = finished deck/tarmac)
    float tx = 0, tz = 1;        // unit tangent (XZ)
    float bank = 0;              // radians; + = right edge dips (superelevation)
};

struct RoadNode { float x = 0, z = 0; };   // graph endpoints (positional joins)

struct RoadEdge {
    RoadClass cls = RoadClass::Avenue;
    float width = 9.0f;          // full paved width (m)
    int   lanesF = 1, lanesB = 1;// forward / backward lane counts
    uint32_t a = 0, b = 0;       // RoadNode indices (a==b for the closed ring)
    std::vector<RoadSample> center;   // arc-length-even samples (~4 m; 2 m ramps)
    float length = 0.0f;         // meters along center
};

struct RoadGraph {
    std::vector<RoadNode> nodes;
    std::vector<RoadEdge> edges;

    // Lateral offset (meters, along right-perp (tz,-tx) of the tangent) of the
    // CENTER of lane `lane` (0 = innermost). Right-hand traffic: forward lanes
    // sit on the right of the centerline, backward lanes mirror negative.
    // kLaneWidth lives in the .cpp (3.4 m) and is baked in here.
    static float laneCenterOffset(const RoadEdge& e, int lane, bool forward);
};

// ---------------------------------------------------------------------------
// EchoRoads — build once, draw every frame. One mesh per material bucket
// (asphalt / paint / concrete / sidewalk), identity transform, flat PBR colors
// (textures are a v2 swap — UVs are already road-metric: u across, v = meters
// along / 10, so a tiling asphalt/paint atlas drops in without regeometry).
// ---------------------------------------------------------------------------
// V3: the drivable-surface collision export. World-space triangles of the
// ASPHALT TOP SURFACE only (freeway deck, ramps, avenues, harbor streets,
// junction patches — no paint/barriers/sidewalks/pillars). The INTEGRATOR
// feeds this to phys->addStaticMesh; this module never touches physics
// itself (Tim: "Collision is not enabled on it, you can fall through").
struct RoadCollisionMesh {
    std::vector<float>    verts;    // x,y,z triplets, world space
    std::vector<uint32_t> indices;  // single-sided tris (up-facing winding)
};

class EchoRoads {
public:
    // Generates graph + geometry. Requires a loaded heightfield (returns false
    // and builds nothing if !hf.ok() — the host should log and keep the old
    // freeway in that case). Safe to call exactly once per instance.
    //
    // V3 build order (junction-aware): 1) COLLECT every edge's centerline
    // (ring / ramps / avenues / boulevard / blocks) without emitting;
    // 2) DETECT junctions (endpoint captures + interior crossings of ground
    // edges; stop-short endpoints are EXTENDED to their target corridor);
    // 3) EMIT with per-sample suppression inside junction patches (ribbons
    // trim to the patch, paint breaks, stop bars at entries) + the filled
    // junction polygons themselves.
    bool build(x3::rhi::IRenderDevice& device, const Heightfield& hf);

    // Draw all buckets (no-op before build). Identity model; one drawMeshPBR
    // per bucket, alphaMask/Blend off, zero emissive (lighting is lights()).
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const;
    // V7: lamp-head glow (warm emissive quads). Call ONLY at night
    // (tod.sample().cityLightsOn) — the module never day-gates itself.
    void drawNightGlow(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const;

    // Static lamp slice (freeway poles + harbor street lamps). Host gates by
    // cityLightsOn and merges into its nearest-K per-frame selection. V3:
    // every lamp also gets a pole+arm fixture mesh (concrete bucket).
    const std::vector<x3::rhi::PointLight>& lights() const { return m_lights; }

    const RoadGraph& graph() const { return m_graph; }

    // V3: drivable-surface collision (see RoadCollisionMesh). Valid after
    // build(); empty before. The integrator owns the physics body.
    const RoadCollisionMesh& collisionMesh() const { return m_collision; }

    // Diagnostics (log/HUD): totals reported by the build log too.
    float    pavedMeters()   const { return m_pavedMeters; }
    uint32_t vertexCount()   const { return m_vertexCount; }
    uint32_t junctionCount() const { return m_junctionCount; }

private:
    struct Bucket {                      // one material = one mesh = one draw
        std::vector<x3::rhi::MeshVertex> v;
        std::vector<uint32_t>            i;
        x3::rhi::MeshHandle              mesh;
        x3::rhi::TextureHandle           tex;    // V7: albedo tile (invalid = flat color)
        float                            color[4] = { 1, 1, 1, 1 };
    };
    // V7 SURFACE PASS buckets: Shoulder = worn light asphalt bands flanking the
    // freeway deck; Grime = skid marks + oil stains (dark-tinted grime tile);
    // NightGlow = lamp-head glow quads — NOT drawn by draw(): the integrator
    // calls drawNightGlow() only when tod.cityLightsOn (see INTEGRATION notes).
    enum { kBucketAsphalt = 0, kBucketPaint, kBucketConcrete, kBucketSidewalk,
           kBucketShoulder, kBucketGrime, kBucketNightGlow,
           kBucketCount };

    Bucket m_buckets[kBucketCount];
    RoadGraph m_graph;
    std::vector<x3::rhi::PointLight> m_lights;
    RoadCollisionMesh m_collision;
    float    m_pavedMeters   = 0.0f;
    uint32_t m_vertexCount   = 0;
    uint32_t m_junctionCount = 0;
    bool     m_built = false;
};

} // namespace x3::game
