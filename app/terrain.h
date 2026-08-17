#pragma once
// Tiled procedural terrain world (B2) + STREAMING (B3 — perf-stack H).
//
// Game/slice code only — engine/ stays pure. A large procedural heightmap is
// split into a grid of square TILES, each an independent, addressable mesh +
// static-collision body. B2 built a fixed 32x32 grid up front; B3 makes the
// world EFFECTIVELY INFINITE: tiles are keyed by SIGNED grid coords (tileX,tileZ)
// and only a bounded, camera-centered RING of them is kept resident at any time.
// As the focus point (player/camera) moves, newly-in-range tiles STREAM IN and
// out-of-range ones STREAM OUT, so the resident count + memory stay constant no
// matter how far the player travels.
//
// CLEAN-ROOM, original work. The heightfield uses a self-implemented value-noise
// + fractal-Brownian-motion (fBm) sum — the standard public algorithm (Perlin's
// fBm idea; value noise via an integer hash + smoothstep interpolation), built
// from GPU Gems / Texturing & Modeling: A Procedural Approach references and the
// public noise literature. The streaming residency-ring + async-generation
// pipeline is built from the engine's OWN IJobSystem + IRenderDevice +
// IPhysicsWorld interfaces and public open-world streaming talks/papers. No
// game-engine source was consulted.
//
// Rendering: tiles are NORMAL meshes (createMesh) drawn through the existing
// GPU-driven path (drawMesh), so they automatically get batching + the
// directional sun + shadows + the render graph + the analytic sky. No renderer
// changes are needed.
//
// LOD: each tile is generated at 3 vertex-density levels; the active LOD is
// chosen by the tile's distance from the camera (near = full density, far =
// decimated). A downward "skirt" around each tile's border hides the cracks that
// would otherwise appear where two tiles of different LOD meet (the cheap,
// standard fix — see lodForDistance() / the skirt notes in the .cpp).
//
// Collision: ONE static-mesh body per tile, built from the tile's FULL-density
// (LOD0) triangles via IPhysicsWorld::addStaticMesh — reusing the existing
// static-mesh path rather than adding a new heightfield shape. Collision always
// uses LOD0 so the player never falls through a decimated far tile. On stream-out
// the body is removed via IPhysicsWorld::removeBody and the meshes destroyMesh'd.

#include "scene.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/core/IJobSystem.h"

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>

namespace x3::game {

// ---------------------------------------------------------------------------
// Terrain configuration. All sizes in meters. The fixed-grid (B2) path uses
// tilesX/tilesZ; the streamed (B3) path ignores them (the world is unbounded —
// tiles are addressed by SIGNED grid coords and generated on demand). Heights
// span [0, heightScale] meters.
// ---------------------------------------------------------------------------
struct TerrainConfig {
    uint32_t tilesX        = 32;      // grid columns (X) — fixed-grid path only
    uint32_t tilesZ        = 32;      // grid rows    (Z) — fixed-grid path only
    float    tileSize      = 32.0f;   // world meters per tile edge
    uint32_t tileVerts     = 33;      // LOD0 vertices per tile edge (=> 32 quads)
    float    heightScale   = 55.0f;   // peak of the BASE rolling field (meters)
    float    noiseFreq     = 0.0042f; // base noise frequency (cycles / meter)
    uint32_t octaves       = 5;       // fBm octaves (detail layers)
    uint32_t seed          = 1337u;   // deterministic generation seed
    // W8-3 (feat/babylon-world): the CANONICAL WORLD FEATURES layer — macro
    // relief (plains vs hill country), the 4 mountain ranges of the Babylon/EFLZ
    // world map (N snow / E volcanic / S mesa / W crystal-hills), the flattened
    // city/facility pads and the offshore ocean basin. OFF by default so every
    // self-test that builds a custom TerrainConfig keeps its exact legacy field
    // (bounded [0, heightScale]); worldTerrainConfig() turns it ON, so every
    // host/placement query built from the canonical config gets the full map.
    // With features on, MOUNTAIN heights intentionally exceed heightScale
    // (peaks ~400-500 m at 7-10 km out); the base field stays in [0, heightScale].
    bool     worldFeatures = false;
};

// ---------------------------------------------------------------------------
// CANONICAL WORLD CONFIG — the single source of truth for the streamed world.
// The interactive `--world terrain` / `--world ocean` setups AND the placement
// query helpers below all build from THIS config, so a height/normal query on
// the host side matches exactly what is rendered + streamed underfoot. It is the
// engine's TerrainConfig defaults (32 m tiles, heightScale 55 m, seed 1337, …);
// returned by const-reference so callers can also pass it to the lower-level
// terrainHeightAt(cfg, …) sampler when they need the raw form.
//
// Placement API (for the 14900k's building/Spire + cliffside pad anchoring):
//   worldTerrainConfig()                 — the active world's TerrainConfig.
//   terrainHeightAtWorld(x,z)            — surface Y at world (x,z).
//   terrainNormalAtWorld(x,z,out[3])     — unit surface normal (central diffs).
//   placeOnTerrain(x,z,out[3])           — world pos sitting ON the surface.
// All four are PURE (no GPU/physics state) and safe to call before any tile
// exists — they read the same procedural field the streamer generates from.
// ---------------------------------------------------------------------------
const TerrainConfig& worldTerrainConfig();

// Height (world Y) of the canonical world surface at world (x,z). Equivalent to
// terrainHeightAt(worldTerrainConfig(), x, z); the convenient form for placement.
float terrainHeightAtWorld(float x, float z);

// Unit surface normal of the canonical world at world (x,z), via central
// differences of the height field (same construction the tile mesher uses). RH,
// +Y up — the normal always points generally +Y (outNormal[1] > 0). Use it to
// align/orient a foundation or the cliffside pad to the local slope. Writes 3
// floats {nx,ny,nz}.
void terrainNormalAtWorld(float x, float z, float outNormal[3]);

// Fill outPos (3 floats {x, surfaceY, z}) with the world position sitting ON the
// canonical surface at (x,z) — the anchor a building/Spire base snaps to. The
// caller adds their own footprint/pivot offset on top (this stays minimal).
void placeOnTerrain(float x, float z, float outPos[3]);

// ---------------------------------------------------------------------------
// W9 (TERRAIN DRAMA) — THE RIVER's authored spline. ONE source of truth shared
// by the height-field carve (terrain.cpp kRiver*) and the water-surface ribbon
// (world_regions.cpp), so the water always lies inside its own channel.
// `waterY` is the water SURFACE at that node (bed = waterY - kWorldRiverBedDrop;
// bank crests are held at >= waterY + ~2.2 m by an authored levee term where the
// natural country is low). Node waterY DESCENDS monotonically downstream — the
// river flows NE hill country -> past the facility's east face -> SE into the
// ocean basin. Nodes [0..worldRiverCarveCount()) carve the terrain; the tail
// nodes only extend the WATER ribbon out over the (already deep) basin floor to
// meet the sea surface.
// ---------------------------------------------------------------------------
struct WorldRiverNode { float x, z, waterY; };
const WorldRiverNode* worldRiverNodes(uint32_t& count);   // full ribbon polyline
uint32_t worldRiverCarveCount();                          // leading nodes that carve
constexpr float kWorldRiverHalfWidth = 34.0f;             // water ribbon half-width (m)
// Bank-shelf bed depth below waterY (m) — the shallows a wader stands on.
constexpr float kWorldRiverBedDrop   = 3.2f;
// MID-CHANNEL bed depth below waterY (m) — the deep cut down the spine (owner:
// "TWICE or THREE TIMES AS DEEP"; 2.5x the shelf). Feathers back to the shelf
// by ~26 m out; waterline, levee and crests unchanged. See terrain.cpp's river
// carve.
constexpr float kWorldRiverMidDrop   = 8.0f;

// ---------------------------------------------------------------------------
// W10 (SWIMMING) — the world WATER SURFACE query. Pure, like the placement API
// above: worldWaterLevelAt(x,z) returns the Y of the water surface covering
// world (x,z), or kWorldWaterDry when the point is dry. Single source of truth:
//   * RIVER coverage = distance to the SAME working spline (the Chaikin chain
//     both the height-field carve and the water ribbon are built from) is
//     <= kWorldRiverHalfWidth -> that reach's interpolated waterY. Query wet
//     exactly where the ribbon mesh is, at the ribbon's own surface height.
//   * OCEAN coverage = inside the offshore basin where the terrain bowl has
//     dropped below the sea surface -> kWorldSeaLevel. The shore ring (-6)
//     stays a dry beach, exactly like the rendered ocean plane.
// ocean_base.cpp's kSurfaceY builds from kWorldSeaLevel so the plane and the
// query can never drift apart.
// ---------------------------------------------------------------------------
constexpr float kWorldSeaLevel = -10.0f;    // the ocean surface Y (W9 terrain drama)
constexpr float kWorldWaterDry = -3.0e38f;  // "no water here" sentinel (< any real Y)
float worldWaterLevelAt(float x, float z);

// ===========================================================================
// TERRAIN CORRIDOR DEPRESSION — the polyline generalization of the river carve,
// and the mechanism that makes freeway tunnels possible WITHOUT CSG, voxels or
// holes in the heightfield.
//
// PROVENANCE: the TECHNIQUE (do not raise the road onto piers and do not punch
// a hole in the ground — pin the road flat and LOWER THE TERRAIN to meet it,
// as a smoothstep depression stamped along the road's path) was learned by
// studying the behaviour of Tim's own Babylon/BL predecessor world
// (Q3Engine src/world/x3-world-terrain.js, analysed in
// docs/design/BL_WORLD_PORT.md §2.2). BL reached for the same heightfield
// depression even as its CSG fallback for cave mouths. The IMPLEMENTATION below
// is entirely our own: BL stamped per-sample circles into an ALREADY-BUILT
// vertex buffer after the fact (overlapping discs, one mesh, one pass); this is
// a pure closest-approach-to-polyline field folded into h(x,z) itself, so it is
// evaluated per vertex during STREAMED tile generation and is therefore
// tile-order independent by construction. No BL code was transcribed — see
// docs/CLEANROOM_PROCESS.md.
//
// WHY IT WORKS FOR TUNNELS: h(x,z) stays SINGLE-VALUED (no overhang, no hole,
// no manifold surgery), so every existing consumer — the streamer, the collision
// soup, the horizon ring, placeOnTerrain, worldWaterLevelAt — keeps working
// untouched. The tunnel tube is then a normal mesh sitting IN the depression;
// the hill visually closes over it because the depression only removes the
// ground the tube occupies.
//
// PROPERTIES (all load-bearing):
//   * PURE — terrainCorridorDepthAt() is a function of (corridor, x, z) only.
//     No allocation, no mutation, no statics touched on the evaluation path.
//   * DETERMINISTIC — pure float arithmetic; identical on every thread/run.
//   * SEAM-CORRECT — because the field is a function of WORLD (x,z) and never of
//     the tile origin/index, a corridor crossing a tile seam yields bit-identical
//     heights from either tile. This is the property that lets it run inside
//     streamed generation at all.
//   * CHEAP — corridors registered in the registry carry a precomputed XZ
//     bounding box (expanded by halfWidth+falloff); terrain outside every box
//     pays 4 float compares per corridor and nothing else.
//   * CREASE-FREE — the corridor is evaluated as the UNION (max) of one capsule
//     per segment, each built on the exact squared distance to that segment with
//     the projection clamped to [0,1]. Max-of-continuous is continuous, so there
//     is no step where the winning segment changes — unlike the obvious
//     "closest point on the polyline, then read the profile there", which steps
//     the ground on the medial axis inside a bend wherever the profile grades.
//     Overlapping corridors likewise combine by DEEPEST-WINS, never by summing,
//     so a joint is never dug twice as deep as its own node asks for.
// ===========================================================================
struct TerrainCorridor {
    static constexpr int kMaxNodes = 32;

    int   nodeCount = 0;                 // >= 2 to have any effect
    float x[kMaxNodes] = {};             // control points, world X (ordered)
    float z[kMaxNodes] = {};             // control points, world Z (ordered)
    // Depth PROFILE: how far below the natural surface the corridor floor sits
    // at each node, in meters (>= 0). Linearly interpolated along each segment,
    // so a corridor can ease in from 0 at a portal mouth and deepen under the
    // ridge. On the spine the ground is lowered by exactly this value, EXCEPT
    // within ~halfWidth of a node, where the deeper of the two adjacent reaches
    // wins (that is the union's flat end cap, not a kink — see the .cpp).
    float depth[kMaxNodes] = {};

    float halfWidth = 8.0f;   // full-depth floor half-width (m); flat bottom
    float falloff   = 16.0f;  // smoothstep run from halfWidth outward to zero (m)
};

// Pure primitive: the depression DEPTH (>= 0 m) this corridor asks for at world
// (x,z). 0 outside the corridor's influence. No registry involved — safe to call
// on a bare stack corridor from any thread.
float terrainCorridorDepthAt(const TerrainCorridor& c, float x, float z);

// ---- Registry -------------------------------------------------------------
// A small FIXED-CAPACITY array (no dynamic allocation, matching how the terrain
// layer already carries kRanges/kPads and how TerrainStreamer carries its
// keep-out rect). Register corridors at BOOT, BEFORE the first height query /
// TerrainStreamer::init(); the registry is then read-only for the rest of the
// run, which is what keeps generation on worker threads race-free. Mutating it
// while tiles are generating is NOT supported (tiles already built would keep
// the old field — the same rule as setKeepOut()).
// 192, not 16. The cap was sized for TUNNELS — a dressed bore registers up to
// three corridors (route + a portal plug per mouth), so the city's four freeway
// tunnels alone wanted 12 and 16 left a little headroom.
//
// ROADS changed the unit of demand. A corridor is a 32-node polyline, so a long
// route is not one corridor — it is a CHAIN of them sharing endpoint nodes. In
// the road network's units:
//
//     15-mile inner ring @ 200 ft node spacing = 396 nodes = 13 corridors
//     31-mile outer ring                       =            ~27 corridors
//     spokes + valley route + city bores        =            ~20 corridors
//     the decided ~62-mile scope               =           ~156 corridors
//
// MEMORY IS NOT THE COST: sizeof(TerrainCorridor) is ~396 bytes (three 32-float
// arrays plus a header), so 192 of them is ~76 KB — noise against a single
// terrain tile's vertex buffer.
//
// THE COST THAT MATTERS is per-height-query: the early-out is a handful of
// float compares per REGISTERED corridor, paid on every query whether or not a
// corridor is near. At 192 that is ~12x today's worst case, and terrain
// generation makes millions of queries. If that shows up in the P0 measurement,
// the answer is a spatial index over the registry (a coarse grid of corridor
// ids), NOT a smaller cap — because the cap is set by how much ROAD the world
// has, and that is a design decision, not a performance one.
constexpr uint32_t kMaxTerrainCorridors = 192;

// Returns false if the registry is full or the corridor is degenerate
// (nodeCount < 2 or > kMaxNodes, non-finite halfWidth/falloff, negative width).
bool     registerTerrainCorridor(const TerrainCorridor& c);
void     clearTerrainCorridors();
uint32_t terrainCorridorCount();

// Total lowering (<= 0 m) the registered corridors apply at world (x,z), the
// deepest one winning (min, never a sum). Exactly 0 when nothing is registered
// or the point is outside every corridor's bounding box — so with no corridors
// registered terrainHeightAt() is BIT-IDENTICAL to the pre-corridor field.
float    terrainCorridorDelta(float x, float z);

// True when (x,z) lies within the FOOTPRINT (halfWidth + falloff of the
// polyline) of any registered corridor, REGARDLESS of the depth profile there.
// This is not redundant with terrainCorridorDelta: a BORED reach carves
// nothing (depth 0 by design — a tunnel does not carve the mountain above it)
// yet still owns its footprint, and the tile mesher needs to know "a tube runs
// under here" — its border skirts must not hang ~55 m down through the bore
// (measured: 74 full-LOD skirt triangles inside the demo tube, down to 0.3 m
// above the road — the rock wall you could drive through). Exactly false when
// nothing is registered.
bool     terrainCorridorContains(float x, float z);

// ---- PORTAL HOLES ---------------------------------------------------------
// Mesh-level exclusion for tunnel mouths. The corridor primitive can steepen
// the ground's sweep at a portal but can never REMOVE it: h(x,z) is single-
// valued, so somewhere at each mouth the surface must pass from road level up
// over the tube's crown — and the tile mesher then emits a continuous CURTAIN
// of triangles (with collision) straight across the bore. No depth profile
// fixes that; the MESHER has to skip those triangles. That curtain is why the
// demo tunnel was "packed with grass": the field's portal-sweep residual is
// terrain collision across the carriageway.
//
// A registered hole is an oriented prism along a route spine: a terrain
// SURFACE triangle is dropped (from the render mesh AND the collision soup)
// when its XZ centroid lies inside the prism and its LOWEST vertex dips under
// `yTop` (the tube envelope's top). Lid triangles above the tube keep their
// full height and survive; the curtain — whose triangles all reach down toward
// road level — does not. The tunnel shell + portal headwall stand in the gap,
// so what shows is concrete, not a torn edge.
//
// Same contract as the corridors: register at BOOT, before the first tile is
// generated; read-only afterwards (worker-thread safe). Purely a function of
// world coordinates, so adjacent tiles agree and generation stays
// deterministic.
struct TerrainPortalHole {
    float x0 = 0.0f, z0 = 0.0f;    // spine segment start (world XZ)
    float x1 = 0.0f, z1 = 0.0f;    // spine segment end   (world XZ)
    float halfWidth = 8.0f;        // lateral half-width of the prism (m)
    float yTop = 0.0f;             // world Y of the tube envelope top
};
constexpr uint32_t kMaxTerrainPortalHoles = 16;

// Returns false when the registry is full or the hole is degenerate
// (zero-length spine, non-positive width, non-finite anything).
bool     registerTerrainPortalHole(const TerrainPortalHole& h);
void     clearTerrainPortalHoles();
uint32_t terrainPortalHoleCount();

// True when a terrain surface triangle (XZ centroid + lowest vertex Y) falls
// inside a registered hole and must not be emitted. Exactly false when no hole
// is registered — tile meshes are then BIT-IDENTICAL to the pre-hole build.
bool terrainPortalHoleDrops(float cx, float cz, float minY);

// ---------------------------------------------------------------------------
// W8-3 — HORIZON RING (the far-terrain stitch). A single static polar-grid mesh
// sampled from the SAME canonical height field the streamer generates from, so
// the mountains/city pads on the horizon match what streams in underfoot by
// construction. Geometric ring spacing: fine cells near rInner, ~hundreds of
// meters at rOuter — one mesh (~13k verts) covers 13 km of world. Drawn with
// the terrain splat marker so it shades through the same height/slope splat as
// the streamed tiles. `yBias` recesses the ring slightly so the full-detail
// streamed tiles always win where the two overlap (no poke-through).
// If `flattenY` is finite, heights blend FROM flattenY at rInner to the true
// field by flattenBlendR — used by flat-pad hosts (the facility apron) so the
// authored flat ground meets the countryside without a seam.
// Returns the scene entity id (kNoLink on failure). Visual only (no collision).
// ---------------------------------------------------------------------------
struct HorizonRingDesc {
    float    centerX = 0.0f, centerZ = 0.0f;
    float    rInner = 230.0f, rOuter = 13000.0f;
    uint32_t rings = 100, segments = 128;
    float    yBias = -2.5f;             // recess under streamed LOD0 tiles
    float    flattenY = 0.0f;           // blend-from height at rInner ...
    bool     flatten = false;           // ... only when this is true
    float    flattenBlendR = 600.0f;    // fully the true field beyond this radius
};
uint32_t addTerrainHorizonRing(Scene& scene, x3::rhi::IRenderDevice& device,
                               x3::rhi::TextureHandle splatMarker,
                               const HorizonRingDesc& desc);

// Register the 4 terrain splat albedos (grass/rock/snow/sand) and return the
// renderer's terrain MATERIAL MARKER handle — what a terrain-shaded entity's
// `tex` must be. Terrain/TerrainStreamer make one internally (see
// groundTexture()); ring-only hosts (no streamer) call this directly.
x3::rhi::TextureHandle makeTerrainSplatMarker(x3::rhi::IRenderDevice& device);

// The LOD level a tile is currently meshed at. 0 = full density (also used for
// collision), 1 = half, 2 = quarter. Increasing = coarser/cheaper.
enum class TerrainLod : uint8_t { Full = 0, Half = 1, Quarter = 2, Count = 3 };

// ---------------------------------------------------------------------------
// Pure procedural height sampler — usable BEFORE any tiles exist (to place the
// spawn/camera) and from worker threads (no shared state). Deterministic in
// (worldX, worldZ, cfg). Both the fixed-grid Terrain and the streamer use it.
// ---------------------------------------------------------------------------
float terrainHeightAt(const TerrainConfig& cfg, float worldX, float worldZ);

// Build one tile's render mesh at a given LOD from ABSOLUTE (signed) tile
// coords — the very mesher the streamer generates from (surface + LOD-crack
// skirt, portal holes applied). originX/originZ are the tile's min-corner
// world position; the surface triangles are the first *outSurfIdxCount
// indices (what collision uses). Pure / thread-safe. Exported so self-tests
// can survey the REAL emitted mesh — a field query cannot see mesh-level
// artifacts (skirts, LOD interpolation, hole drops), and this lane has been
// bitten by exactly that gap (the torn mountain shipped through a green
// field-level test).
void buildTileMeshAbs(const TerrainConfig& cfg, float originX, float originZ,
                      TerrainLod lod,
                      std::vector<x3::rhi::MeshVertex>& outVerts,
                      std::vector<uint32_t>& outIdx,
                      uint32_t* outSurfIdxCount = nullptr);

// One terrain tile: addressable by SIGNED grid coords (gx,gz), owns its 3 LOD
// render meshes, a collision body, and the scene entity that draws the active
// LOD. Identical layout for the fixed-grid and streamed paths.
struct TerrainTile {
    int32_t  gx = 0, gz = 0;                  // SIGNED grid coordinates (unbounded)
    float    originX = 0.0f, originZ = 0.0f;  // world position of the tile's min corner
    float    centerX = 0.0f, centerZ = 0.0f;  // world center (for LOD distance)
    float    minY = 0.0f, maxY = 0.0f;        // height bounds over the tile

    x3::rhi::MeshHandle lodMesh[(int)TerrainLod::Count]; // one mesh per LOD
    x3::phys::BodyId    body;                  // static collision (LOD0 triangles)
    uint32_t            entityId = kNoLink;    // scene entity drawing the active LOD
    TerrainLod          activeLod = TerrainLod::Full;
};

// ---------------------------------------------------------------------------
// The terrain world (B2): builds + owns a FIXED tile grid (all tiles created up
// front). Still used by --screenshot-terrain + --test-terrain. The streamed
// world uses TerrainStreamer below.
// ---------------------------------------------------------------------------
class Terrain {
public:
    void build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, const TerrainConfig& cfg);

    float heightAt(float worldX, float worldZ) const { return terrainHeightAt(m_cfg, worldX, worldZ); }

    uint32_t updateLod(Scene& scene, float camX, float camZ);

    const TerrainConfig& config() const { return m_cfg; }
    uint32_t tileCount() const { return (uint32_t)m_tiles.size(); }
    const TerrainTile& tile(uint32_t i) const { return m_tiles[i]; }
    const TerrainTile* tileAt(uint32_t gx, uint32_t gz) const;

    void worldBounds(float& minX, float& minZ, float& maxX, float& maxZ) const;

    TerrainLod lodForDistance(float dist) const;

    uint32_t activeTriangleCount() const;

private:
    TerrainConfig            m_cfg;
    std::vector<TerrainTile> m_tiles;        // row-major: gz * tilesX + gx
    x3::rhi::TextureHandle    m_groundTex;
    uint32_t                  m_lodIndexCount[(int)TerrainLod::Count] = {0,0,0};
    float                     m_worldMinX = 0.0f, m_worldMinZ = 0.0f;
};

// ===========================================================================
// TerrainStreamer (B3) — camera-centered residency ring over an unbounded,
// procedurally-generated tiled world.
//
// Lifecycle / threading model:
//   * The HEAVY per-tile CPU work — noise sampling + vertex/normal build for all
//     3 LOD meshes + the collision triangle soup — runs on the IJobSystem (off
//     the main thread). A finished job pushes a TileGenResult onto a thread-safe
//     completion queue.
//   * The main thread (single-threaded GPU/physics submit) drains that queue,
//     BUDGETED per frame (<= maxUploadsPerFrame): for each result it calls
//     createMesh x3 + addStaticMesh + scene.add(), producing a resident tile.
//   * Stream-out destroys a tile's 3 meshes (destroyMesh) + removes its body
//     (removeBody) + hides its scene entity (reused later).
//
// No fall-through: the immediate 3x3 neighborhood under the focus point is
// generated SYNCHRONOUSLY on the first update (and any newly-entered under-tile
// is forced synchronous), so the player always has collision beneath them.
// ===========================================================================
class TerrainStreamer {
public:
    // Bring up the streamer. Generates the immediate neighborhood under `focus`
    // SYNCHRONOUSLY so the player has ground on the first frame. `radius` is the
    // residency ring radius in TILES (Chebyshev). `jobs` may be null (then all
    // generation runs synchronously on the main thread — used by the headless
    // self-test without a job pool, still correct).
    void init(Scene& scene, x3::rhi::IRenderDevice& device,
              x3::phys::IPhysicsWorld& physics, x3::jobs::IJobSystem* jobs,
              const TerrainConfig& cfg, float focusX, float focusZ, int radius = 6);

    // Per-frame tick from the focus (player/camera) world position:
    //   1) if the focus crossed a tile boundary, enqueue stream-in for newly
    //      in-range tiles (nearest-first) and stream-out for now-out-of-range;
    //   2) drain up to maxUploadsPerFrame completed gen jobs into resident tiles;
    //   3) apply LOD to resident tiles by distance.
    // Returns the number of tiles uploaded (made resident) this frame.
    uint32_t update(Scene& scene, x3::rhi::IRenderDevice& device,
                    x3::phys::IPhysicsWorld& physics, float focusX, float focusZ);

    // Tear down: destroy every resident tile's GPU + physics resources and drain
    // any in-flight jobs. Safe to call once; init() can follow.
    void shutdown(Scene& scene, x3::rhi::IRenderDevice& device,
                  x3::phys::IPhysicsWorld& physics);

    float heightAt(float worldX, float worldZ) const { return terrainHeightAt(m_cfg, worldX, worldZ); }

    // The terrain splat MATERIAL MARKER this streamer registered (valid after
    // init on a real device) — hand it to addTerrainHorizonRing so the horizon
    // shades identically to the streamed tiles.
    x3::rhi::TextureHandle groundTexture() const { return m_groundTex; }

    // ---- Tuning ----------------------------------------------------------
    void setRadius(int r) { m_radius = r; }
    int  radius() const { return m_radius; }
    void setUploadBudget(uint32_t n) { m_maxUploadsPerFrame = n; }
    void setMaxInFlight(uint32_t n)  { m_maxInFlight = n; }
    // SEAM 3 (canon host): a world-rect KEEP-OUT — tiles whose footprint lies
    // FULLY inside it are never generated. The canon facility brings its own
    // ground there (interior floors at Y=0 + the apron ring + the 150 m soil
    // skirt, all with collision); the terrain's facility pad is ALSO Y=0, so
    // streaming tiles under the building would z-fight every F1 floor + the
    // apron coplanarly. Tiles merely INTERSECTING the rect still generate (the
    // soil skirt spans the whole skipped area, so the tile-grid hole is always
    // covered — no void, no missing collision). Call BEFORE init(). Default off.
    void setKeepOut(float x0, float z0, float x1, float z1) {
        m_keepOut[0] = x0; m_keepOut[1] = z0; m_keepOut[2] = x1; m_keepOut[3] = z1;
        m_keepOutOn = true;
    }

    // ---- Queries (host + self-test) --------------------------------------
    const TerrainConfig& config() const { return m_cfg; }
    // Number of currently-resident (GPU/physics-live) tiles.
    uint32_t residentCount() const { return (uint32_t)m_resident.size(); }
    // Theoretical max resident tiles for the current radius: (2R+1)^2.
    uint32_t maxResidentForRadius() const { uint32_t d = (uint32_t)(2*m_radius+1); return d*d; }
    // Lifetime counters (for the leak check): tiles whose GPU resources were
    // created vs destroyed. At a clean teardown created == destroyed.
    uint64_t tilesCreated() const { return m_tilesCreated; }
    uint64_t tilesDestroyed() const { return m_tilesDestroyed; }
    // In-flight async generation jobs not yet drained.
    uint32_t inFlight() const { return m_inFlight; }
    // True iff a resident tile covers the focus' current tile (collision present).
    bool focusTileResident(float worldX, float worldZ) const;

    TerrainLod lodForDistance(float dist) const;

    ~TerrainStreamer();

private:
    // 64-bit key packing two SIGNED 32-bit tile coords (for the resident map).
    static uint64_t key(int32_t gx, int32_t gz) {
        return ((uint64_t)(uint32_t)gx << 32) | (uint64_t)(uint32_t)gz;
    }
    int32_t tileFloor(float world) const; // world coord -> tile index (floor)

    // CPU-side result of an async generation job: everything needed to create
    // the GPU/physics resources on the main thread. Heap-owned vectors so the
    // job can fill them on a worker, then the main thread consumes + frees.
    struct TileGenResult {
        int32_t gx = 0, gz = 0;
        float originX = 0, originZ = 0, centerX = 0, centerZ = 0, minY = 0, maxY = 0;
        std::vector<x3::rhi::MeshVertex> lodVerts[(int)TerrainLod::Count];
        std::vector<uint32_t>            lodIdx  [(int)TerrainLod::Count];
        std::vector<float>               collVerts;   // position-only LOD0 surface
        std::vector<uint32_t>            collIdx;
    };

    // Generate one tile's CPU data (pure, thread-safe). Static so it can run on a
    // worker with only the config + coords captured.
    static void generate(const TerrainConfig& cfg, int32_t gx, int32_t gz,
                         TileGenResult& out);

    // Make a resident tile from a finished gen result (main thread: GPU+physics).
    void upload(Scene& scene, x3::rhi::IRenderDevice& device,
                x3::phys::IPhysicsWorld& physics, TileGenResult& r);

    // Destroy a resident tile's GPU + physics resources (main thread).
    void evict(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, TerrainTile& t);

    // Request generation of a tile (async if a job system is present, else
    // synchronous). `synchronous` forces immediate generation on the main thread
    // (used for the under-player neighborhood to prevent fall-through).
    void requestTile(int32_t gx, int32_t gz, bool synchronous);

    // Recycle / obtain a TileGenResult buffer (keeps vector capacity to avoid
    // steady-state heap churn).
    std::unique_ptr<TileGenResult> obtainResult();
    void recycleResult(std::unique_ptr<TileGenResult> r);

    // --- async plumbing -----------------------------------------------------
    struct GenJob;            // job payload (defined in the .cpp)
    static void jobThunk(void* user);

    TerrainConfig            m_cfg;
    x3::jobs::IJobSystem*    m_jobs = nullptr;   // may be null (synchronous mode)
    x3::jobs::Counter*       m_counter = nullptr;
    x3::rhi::TextureHandle    m_groundTex;
    int                       m_radius = 6;
    uint32_t                  m_maxUploadsPerFrame = 4;
    uint32_t                  m_maxInFlight = 24;

    // Resident tiles, keyed by packed (gx,gz). Pointer-stable (heap nodes) so a
    // tile reference stays valid while the map mutates.
    std::unordered_map<uint64_t, std::unique_ptr<TerrainTile>> m_resident;
    // Tiles requested (in-flight or queued) but not yet resident — dedupes
    // re-requests across frames.
    std::unordered_map<uint64_t, char> m_pending;

    // Completion queue: jobs push finished results here; the main thread drains.
    std::mutex                                  m_doneMtx;
    std::vector<std::unique_ptr<TileGenResult>> m_done;
    // Main-thread-only carry-over of results that were ready but exceeded this
    // frame's upload budget; consumed first next frame (kept off m_inFlight so the
    // count reflects only genuinely-outstanding async work).
    std::vector<std::unique_ptr<TileGenResult>> m_deferred;

    // Free-list of TileGenResult buffers to recycle (avoid per-tile heap churn in
    // the steady state — vectors keep their capacity).
    std::vector<std::unique_ptr<TileGenResult>> m_resultPool;
    std::mutex                                  m_poolMtx;

    // Free entity slots in the scene from evicted tiles, reused on the next
    // upload so the scene's entity vector doesn't grow unbounded as we roam.
    std::vector<uint32_t>     m_freeEntities;

    uint32_t                  m_inFlight = 0;
    // SEAM 3: keep-out rect {x0,z0,x1,z1} — see setKeepOut().
    float                     m_keepOut[4] = { 0, 0, 0, 0 };
    bool                      m_keepOutOn = false;
    int32_t                   m_lastFocusTX = INT32_MIN, m_lastFocusTZ = INT32_MIN;
    uint32_t                  m_lodIndexCount[(int)TerrainLod::Count] = {0,0,0};

    uint64_t                  m_tilesCreated = 0, m_tilesDestroyed = 0;
};

// Headless self-test (--test-terrain). T1 character settles, T2 heightAt
// bounded+varied, T3 LOD coarsens with distance. No window/Vulkan.
bool runTerrainSelfTest();

// Headless self-test (--test-terrainplace). Asserts the placement API agrees
// with the raw sampler (terrainHeightAtWorld == terrainHeightAt(cfg,…)), that a
// point placed via placeOnTerrain sits exactly on the surface (Y == height), and
// that terrainNormalAtWorld returns a unit-length normal that points generally
// +Y. Logs PASS/FAIL P#, returns true iff all pass. No window/Vulkan.
bool runTerrainPlaceSelfTest();

// Headless self-test (--test-streaming, B3). Drives a focus point on a long path
// across the unbounded world and asserts: (a) resident tile count stays bounded
// (<= (2R+1)^2, does NOT grow with distance), (b) tiles load ahead + unload
// behind, (c) no tile/mesh/body leak (created==destroyed at teardown), (d) a
// character stays on the surface the whole way (no fall-through). Logs PASS/FAIL
// T#, returns true iff all pass. No window/Vulkan. Lives in terrain.cpp.
bool runStreamingSelfTest();

// Headless self-test (--test-terraincorridor). Asserts the corridor-depression
// primitive: (C1) no global regression — height outside every corridor's bounds
// is BIT-identical to the unmodified field; (C2) the centreline is lowered by
// exactly the requested depth profile; (C3) TILE-SEAM CONSISTENCY — the shared
// edge of two adjacent generated tiles matches bit-for-bit where a corridor
// crosses the seam; (C4) no crease/spike at polyline joints (arc sweep around a
// joint stays inside the analytic Lipschitz bound, and the joint is never dug
// deeper than its node asks); (C5) determinism + registry hygiene. Logs PASS/FAIL
// C#, returns true iff all pass. No window/Vulkan. Lives in terrain.cpp.
bool runTerrainCorridorSelfTest();

} // namespace x3::game
