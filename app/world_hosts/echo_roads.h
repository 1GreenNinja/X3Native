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

// ===========================================================================
// CITY BLOCKS / LOTS / FRONTAGE — V8, "the missing consumer".
//
// The RoadClass comment above (HOUSE/STREET ALIGNMENT) has specified this
// since v1 and nothing ever implemented it: the graph was a real road network
// and the buildings were four independent hash scatters reconciled afterwards
// by DELETION (corridorHits). Everything below turns the graph into PLACES TO
// BUILD, so placement is correct BY CONSTRUCTION and there is nothing to veto.
//
//   sampleFrontage()   TIER 0 — walk an edge, emit the points where a lot's
//                      front door goes. Same math the roadside-debris scatter
//                      already used correctly (echo_roads.cpp, V7 ROADSIDE
//                      DEBRIS) — promoted to a public API.
//   buildCityPlan()    TIER 1 — planarize the graph, extract the minimal
//                      cycles (= CITY BLOCKS), inset each block by its own
//                      ring's road half-widths (= the BUILDABLE polygon), and
//                      recursively split it into LOTS that each keep street
//                      frontage. A lot carries frontYaw; a building takes it
//                      with ZERO jitter and is inside the lot by construction.
//
// Pure geometry over a RoadGraph: no device, no assets, no rand, no globals.
// Deterministic AND insertion-order independent (position-derived seeds) —
// see --test-cityblocks.
// ===========================================================================

// Class selector for sampleFrontage()/buildCityPlan(). One bit per RoadClass.
enum RoadClassMask : uint32_t {
    kRcFreeway      = 1u << (uint32_t)RoadClass::Freeway,
    kRcRamp         = 1u << (uint32_t)RoadClass::Ramp,
    kRcAvenue       = 1u << (uint32_t)RoadClass::Avenue,
    kRcHarborStreet = 1u << (uint32_t)RoadClass::HarborStreet,
    kRcGround       = kRcAvenue | kRcHarborStreet,   // the classes that bound blocks
    kRcAll          = kRcFreeway | kRcRamp | kRcGround,
};
inline uint32_t roadClassBit(RoadClass c) { return 1u << (uint32_t)c; }

// One buildable point on a street edge. `yaw` is the ENGINE yaw convention
// used by every placement transform in this project (local +Z maps to
// (sin yaw, 0, cos yaw)), so a prop placed with this yaw FACES THE ROAD —
// literally the header's own spec: yaw = atan2(tangent) +/- 90 degrees,
// position = sample +/- perp * (width/2 + curb + walk + setback).
struct Frontage {
    float x = 0, z = 0;          // world XZ of the frontage point (lot side of the walk)
    float yaw = 0;               // building yaw: +Z local points at the road
    float tx = 0, tz = 1;        // road centerline tangent at the parent sample
    float nx = 0, nz = 0;        // unit outward normal (road -> lot)
    float roadY = 0;             // finished road surface height at the parent sample
    float width = 9.0f;          // parent edge's paved width
    float arc = 0;               // meters along the parent edge
    uint32_t edge = 0;           // RoadGraph edge index
    uint32_t sample = 0;         // index into RoadEdge::center
    int   side = 1;              // +1 = right of travel, -1 = left
    RoadClass cls = RoadClass::Avenue;
};

// One segment of a block's ring (ring[i] -> ring[i+1]) and the road it came
// from. `width`/`cls` drive that segment's own inset, so a block bounded by a
// 9 m harbor street on one side and a 16 m boulevard on the other insets
// correctly on each side instead of by one global number.
struct CityBlockEdge {
    uint32_t  roadEdge = 0;
    RoadClass cls   = RoadClass::Avenue;
    float     width = 9.0f;
    float     tx = 0, tz = 1;    // road tangent along this segment
    float     len = 0;
};

// A minimal cycle of the planarized road graph: one city block.
struct CityBlock {
    std::vector<float>         ring;      // x,z pairs — CCW in (x,z), road CENTERLINES
    std::vector<CityBlockEdge> edge;      // one per ring segment
    std::vector<float>         build;     // x,z pairs — ring inset to the BUILDABLE polygon
    std::vector<int>           buildTag;  // per build segment: index into `edge`, or -1
    float area = 0;                       // ring area (m^2, positive)
    float cx = 0, cz = 0;                 // ring centroid
};

// One lot. The building's transform is fully determined here: `frontYaw` is
// the yaw (no jitter, ever), and the footprint must fit inside the OBB
// (halfW along `ax`, halfD along the inward normal) which is guaranteed to lie
// inside the lot polygon — which is inside the buildable polygon — which
// excludes every road corridor. That chain is the whole point: it is what
// corridorHits() used to enforce by deleting buildings afterwards.
struct CityLot {
    uint32_t  block    = 0;
    uint32_t  roadEdge = 0;
    RoadClass roadCls  = RoadClass::Avenue;
    float cx = 0, cz = 0;        // OBB centre (world XZ)
    float frontYaw = 0;          // faces its street
    float ax = 0, az = 1;        // unit "along street" axis
    float nx = 0, nz = 0;        // unit inward normal (street -> block interior)
    float halfW = 0, halfD = 0;  // OBB half-extents (W along ax, D along nx)
    float frontX = 0, frontZ = 0;// midpoint of the fronting edge
    float frontLen = 0;          // meters of street frontage (ALWAYS > 0)
    float area = 0;              // lot polygon area
    bool  corner = false;        // fronts two or more distinct roads
    std::vector<float> poly;     // lot polygon, x,z pairs, CCW
    std::vector<int>   tag;      // per poly segment: CityBlock::edge index, or -1
};

struct CityPlanRules {
    float setback       = 3.5f;   // building face to the far edge of the sidewalk
    float sideGap       = 2.5f;   // guaranteed gap between neighbouring buildings
    float minFrontage   = 9.0f;   // a lot narrower than this is not a lot
    float minLotArea    = 130.0f;
    float targetLotArea = 620.0f; // stop splitting below ~2x this
    float minBlockArea  = 420.0f;
    float ringStep      = 8.0f;   // ring decimation (m) — keeps curved blocks curved
    int   maxDepth      = 7;
    float splitJitter   = 0.14f;  // +/- fraction of the span, seeded FROM POSITION
};

struct CityPlan {
    std::vector<CityBlock> blocks;
    std::vector<CityLot>   lots;
    uint32_t rejectedFaces = 0;   // faces dropped (outer / too small / degenerate)
    uint32_t rejectedSplits = 0;  // splits refused for killing a child's frontage
};

// --- the two public entry points (free functions: no device, self-test-able) --
void sampleFrontage(const RoadGraph& g, uint32_t clsMask, float pitch,
                    float setback, std::vector<Frontage>& out);
void buildCityPlan(const RoadGraph& g, uint32_t clsMask,
                   const CityPlanRules& rules, CityPlan& out);

// --- placement primitives every call site is routed through -----------------
// POSITION-DERIVED SEED. Replaces index-derived seeds (`r*101 + k`) so that
// inserting/removing one building does not re-roll every other one, and so the
// same spot always draws the same asset regardless of iteration order.
uint32_t seedAt(float x, float z);
uint32_t seedMix(uint32_t seed, uint32_t k);
float    seedFloat(uint32_t seed);                       // [0,1)
// Seeded WEIGHTED draw from a palette — replaces the `% 5` / `% 8` periodic
// ABCDE cycles. Returns [0,n).
int      seedWeighted(uint32_t seed, const float* weights, int n);

// FOOTPRINT-CORNER TERRAIN SEATING. One heightAt() probe at the pivot is what
// made houses float a corner or sink one; this probes the four footprint
// corners plus the centre and seats at the MAX so nothing floats, reporting
// the spread so the caller can drop a plinth under the overhang.
// A footprint whose corners disagree by more than kMaxSeatGrade of its own
// diagonal is not a building site, it is a cliff edge. Seating at MAX and
// plinthing the overhang is right for a curb or a hillside, but on Echo
// Harbor's 190 m crown wall it grew a full-height pedestal from the rim tower
// down to the sea. Placement rejects those outright; kMaxPlinthDrop bounds
// what a plinth may ever be even when the grade test passes.
constexpr float kMaxSeatGrade  = 0.50f;   // rise/run across the footprint (~27 deg)
constexpr float kMaxPlinthDrop = 14.0f;   // metres; deeper is terrain, not a plinth
struct FootprintSeat {
    bool  ok        = false; // false when the heightfield is not loaded
    float y         = 0;     // seat height (MAX of the 5 probes)
    float yMin      = 0;     // lowest probe
    float spread    = 0;     // y - yMin
    float grade     = 0;     // spread / footprint diagonal — scale-free steepness
    bool  plinth    = false; // spread exceeded the threshold
    bool  buildable = false; // grade within kMaxSeatGrade (false => do not place)
};
FootprintSeat seatFootprint(const Heightfield& hf, float cx, float cz, float yaw,
                            float halfX, float halfZ, float plinthThresh = 0.30f);

// --test-cityblocks. Headless, no device, no assets: builds known synthetic
// road graphs and asserts face count, frontage, non-overlap, road clearance,
// determinism and insertion-order independence, and — as a permanent negative
// control — that the SAME checkers reject the polar-hash-ring layout this
// work replaced. Returns true on pass.
bool runCityBlocksSelfTest();

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

    // RIM SPLICE SEAM CLEARANCE (metres), applied by the NEXT build(). A rim
    // waypoint probed closer than this to either fixed-arc splice node is
    // skipped, because a waypoint that near the splice folds the ring into a
    // U-turn the zigzag law cannot flatten — on the regenerated fjord that cost
    // the entire freeway spine. It is NOT terrain-neutral, so it is off by
    // default: 0 reproduces the Lift A road graph bit-for-bit (roads_test's
    // golden checksums pin exactly that). Hosts building on the regenerated
    // island pass kRimSeamClearRegenIsland. Must be called BEFORE build().
    void setRimSeamClearance(float metres) { m_rimSeamClear = metres; }
    // The value the regenerated-fjord host needs; see echo_roads.cpp for why
    // 260 m, and why it is not the default.
    static constexpr float kRimSeamClearRegenIsland = 260.0f;

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

    // ---- V8 CITY BLOCKS ---------------------------------------------------
    // TIER 0: walk the selected edges' arc-even centerline samples and emit a
    // frontage point every `pitch` meters on BOTH sides. See Frontage.
    void sampleFrontage(uint32_t clsMask, float pitch, float setback,
                        std::vector<Frontage>& out) const;

    // TIER 1: the block/lot plan for this graph, built once and cached (the
    // first call with a given rules struct builds; later calls reuse). Ground
    // classes only by default — a freeway deck is grade-separated and does not
    // bound a block.
    const CityPlan& cityPlan(const CityPlanRules& rules = CityPlanRules(),
                             uint32_t clsMask = kRcGround) const;

    // CITY EXTRAS (concrete bucket, lazily uploaded on the first draw() after
    // they are added — placement runs AFTER build(), so this cannot go through
    // the build() upload). `addPlinth` is what seatFootprint's `spread` result
    // feeds: a base slab under a building whose footprint overhangs its slope.
    // `addMassingBox` is the ECHO_CITY_PROXY=1 blockout used to photograph the
    // layout on checkouts where the building GLBs are absent — OFF by default,
    // it emits nothing unless a caller asks for it.
    // (const + mutable: the placement builders hold `const EchoRoads*`.)
    void addPlinth(float cx, float cz, float yaw, float halfX, float halfZ,
                   float y0, float y1) const;
    void addMassingBox(float cx, float cz, float yaw, float halfX, float halfZ,
                       float y0, float y1) const;

    // (V8: `corridorHits()` / `corridorHitsAABB()` used to live here — the
    // #34a audit that four placement passes called to DELETE whatever their
    // hash scatters dropped onto a road. Placement is structural now
    // (cityPlan/sampleFrontage above), so there is nothing to veto and the
    // API is gone rather than left lying around for a fifth veto site to
    // rediscover. The ECHO_EXPORT_CORRIDORS dump for the OFFLINE bakes is
    // unaffected — that is a separate export in build().)

    // V3: drivable-surface collision (see RoadCollisionMesh). Valid after
    // build(); empty before. The integrator owns the physics body.
    const RoadCollisionMesh& collisionMesh() const { return m_collision; }

    // Diagnostics (log/HUD): totals reported by the build log too.
    float    pavedMeters()   const { return m_pavedMeters; }
    uint32_t vertexCount()   const { return m_vertexCount; }
    uint32_t junctionCount() const { return m_junctionCount; }

private:
    // 0 = historic behaviour (see setRimSeamClearance).
    float m_rimSeamClear = 0.0f;

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
    // V8 CITY EXTRAS — plinths (+ the ECHO_CITY_PROXY massing blockout). NOT
    // one of the kBucket* buckets because it is filled AFTER build() by the
    // region placement pass (which holds a `const EchoRoads*`), so it uploads
    // lazily on the first draw() that sees it dirty.
    mutable Bucket   m_cityExtra;
    mutable bool     m_extraDirty = false;
    mutable CityPlan m_plan;
    mutable bool     m_planBuilt = false;
    RoadGraph m_graph;
    std::vector<x3::rhi::PointLight> m_lights;
    RoadCollisionMesh m_collision;
    float    m_pavedMeters   = 0.0f;
    uint32_t m_vertexCount   = 0;
    uint32_t m_junctionCount = 0;
    bool     m_built = false;
};

} // namespace x3::game
