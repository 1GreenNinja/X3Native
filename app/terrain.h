#pragma once
// Tiled procedural terrain world (B2 — the open-world substrate).
//
// Game/slice code only — engine/ stays pure. A large procedural heightmap is
// split into a grid of square TILES, each an independent, addressable mesh +
// static-collision body. This is the foundation the streaming pass (B3) builds
// on: tiles are keyed by (tileX,tileZ) grid coords and own all their GPU/physics
// resources, so a streaming layer can create/destroy them one at a time without
// touching the rest of the world.
//
// CLEAN-ROOM, original work. The heightfield uses a self-implemented value-noise
// + fractal-Brownian-motion (fBm) sum — the standard public algorithm (Perlin's
// fBm idea; value noise via an integer hash + smoothstep interpolation), built
// from GPU Gems / Texturing & Modeling: A Procedural Approach references and the
// public noise literature. No game-engine source was consulted.
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
// static-mesh path rather than adding a new heightfield shape. Justification in
// the .cpp header: it keeps IPhysicsWorld unchanged + opaque, the per-tile body
// is exactly the unit a streaming layer loads/unloads, and the triangle count
// per tile stays modest. Collision always uses LOD0 so the player never falls
// through a decimated far tile that later draws at a coarser LOD.

#include "scene.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>
#include <vector>

namespace x3::game {

// ---------------------------------------------------------------------------
// Terrain configuration. All sizes in meters. Defaults: a 32x32 grid of 32 m
// tiles => ~1.05 km x 1.05 km world (~1.1 km^2), each tile 32x32 quads at LOD0.
// Tunable for larger/denser worlds. Heights span [0, heightScale] meters.
// ---------------------------------------------------------------------------
struct TerrainConfig {
    uint32_t tilesX        = 32;      // grid columns (X)
    uint32_t tilesZ        = 32;      // grid rows    (Z)
    float    tileSize      = 32.0f;   // world meters per tile edge
    uint32_t tileVerts     = 33;      // LOD0 vertices per tile edge (=> 32 quads)
    float    heightScale   = 55.0f;   // peak terrain height (meters)
    float    noiseFreq     = 0.0042f; // base noise frequency (cycles / meter)
    uint32_t octaves       = 5;       // fBm octaves (detail layers)
    uint32_t seed          = 1337u;   // deterministic generation seed
    // World is centered on the origin so the player can spawn near (0,_,0).
    // worldMin = -0.5 * (tiles * tileSize); worldMax = +that.
};

// The LOD level a tile is currently meshed at. 0 = full density (also used for
// collision), 1 = half, 2 = quarter. Increasing = coarser/cheaper.
enum class TerrainLod : uint8_t { Full = 0, Half = 1, Quarter = 2, Count = 3 };

// One terrain tile: addressable by grid coords (gx,gz), owns its 3 LOD render
// meshes, a collision body, and the scene entity that draws the active LOD.
struct TerrainTile {
    uint32_t gx = 0, gz = 0;                 // grid coordinates (addressable)
    float    originX = 0.0f, originZ = 0.0f; // world position of the tile's min corner
    float    centerX = 0.0f, centerZ = 0.0f; // world center (for LOD distance)
    float    minY = 0.0f, maxY = 0.0f;       // height bounds over the tile

    x3::rhi::MeshHandle lodMesh[(int)TerrainLod::Count]; // one mesh per LOD
    x3::phys::BodyId    body;                 // static collision (LOD0 triangles)
    uint32_t            entityId = kNoLink;   // scene entity drawing the active LOD
    TerrainLod          activeLod = TerrainLod::Full;
};

// ---------------------------------------------------------------------------
// The terrain world: builds + owns the tile grid. Static for this pass (all
// tiles created up front); the tile structure is deliberately streaming-ready.
// ---------------------------------------------------------------------------
class Terrain {
public:
    // Build the full tile grid into `scene` (render meshes via `device`, static
    // collision via `physics`). One scene entity per tile (drawing LOD0 at first).
    // A shared ground texture is created once and reused by every tile. Call once.
    void build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, const TerrainConfig& cfg);

    // Sample the terrain surface height (world Y) at world (x,z). Pure function of
    // the config + seed — usable BEFORE build() to place the spawn/camera on the
    // ground. Bilinear over the same noise the meshes use, so it matches the mesh.
    float heightAt(float worldX, float worldZ) const;

    // Choose + apply the LOD for every tile from the camera world position. Swaps
    // each tile's scene entity mesh to the chosen LOD level. Cheap (no GPU work —
    // the 3 LOD meshes are pre-uploaded); call once per frame (or on a cadence).
    // Returns the number of tiles whose LOD changed this call.
    uint32_t updateLod(Scene& scene, float camX, float camZ);

    // ---- Queries (host + self-test) --------------------------------------
    const TerrainConfig& config() const { return m_cfg; }
    uint32_t tileCount() const { return (uint32_t)m_tiles.size(); }
    const TerrainTile& tile(uint32_t i) const { return m_tiles[i]; }
    // Tile by grid coords (returns nullptr if out of range).
    const TerrainTile* tileAt(uint32_t gx, uint32_t gz) const;

    // World extent (meters): min/max corner on each axis (Y is height bounds).
    void worldBounds(float& minX, float& minZ, float& maxX, float& maxZ) const;

    // The LOD a tile at `dist` meters from the camera (center-to-center) would
    // use. Exposed for the self-test (asserts LOD coarsens with distance).
    TerrainLod lodForDistance(float dist) const;

    // Total triangle count across all tiles at their CURRENT active LOD (for the
    // perf report). Recomputed from the stored per-LOD index counts.
    uint32_t activeTriangleCount() const;

private:
    // Generate one tile's render mesh at a given LOD (step = 1<<lod quad stride).
    // Fills MeshVertex (pos/normal/uv) with a downward skirt around the border to
    // hide LOD cracks. Returns the index count via outIdx.
    void buildTileMesh(uint32_t gx, uint32_t gz, TerrainLod lod,
                       std::vector<x3::rhi::MeshVertex>& outVerts,
                       std::vector<uint32_t>& outIdx) const;

    TerrainConfig            m_cfg;
    std::vector<TerrainTile> m_tiles;        // row-major: gz * tilesX + gx
    x3::rhi::TextureHandle    m_groundTex;    // shared grass/rock checker
    // Cached per-LOD index counts (same for every tile of that LOD) for the
    // active-triangle report. [lod] = indices.
    uint32_t                  m_lodIndexCount[(int)TerrainLod::Count] = {0,0,0};
    float                     m_worldMinX = 0.0f, m_worldMinZ = 0.0f;
};

// Headless self-test (--test-terrain). Builds a small terrain on a real Jolt
// world + a headless device, drops a character capsule onto it, steps physics,
// and asserts: (T1) the character SETTLES on the surface (doesn't fall through /
// float), (T2) heightAt() matches the mesh under the settled character, and
// (T3) LOD coarsens with distance (near tile = Full, far tile = Quarter). Logs
// PASS/FAIL T#, returns true iff all pass. No window/Vulkan. Lives in
// terrain.cpp (mirrors runPlayerSelfTest et al.).
bool runTerrainSelfTest();

} // namespace x3::game
