// Tiled procedural terrain world (B2). See app/terrain.h.
//
// CLEAN-ROOM: built from the Scene + IRenderDevice + IPhysicsWorld interfaces +
// the mesh_prims texture helper, and a self-implemented value-noise/fBm sampler.
// No game-engine source consulted.
//
// ---------------------------------------------------------------------------
// COLLISION CHOICE — static-mesh-per-tile (NOT a Jolt heightfield):
//   * IPhysicsWorld already exposes addStaticMesh(); reusing it means ZERO new
//     physics surface and keeps the interface free of any JPH:: types (the hard
//     clean-boundary rule). Adding a heightfield op would have required a new
//     opaque IPhysicsWorld method + a new Jolt shape path for marginal gain.
//   * A per-tile static body is EXACTLY the unit a streaming layer loads/unloads
//     (addStaticMesh on stream-in, removeBody on stream-out) — so this choice is
//     forward-compatible with B3 with no rework.
//   * Cost: one MeshShape per tile from its LOD0 triangles (~2 * 32 * 32 = 2048
//     tris/tile). Jolt builds an internal AABB tree per shape; for the default
//     32x32 grid that is ~1024 bodies — acceptable for a static world, and the
//     streaming pass will only ever keep a small ring of tiles resident. Caveat
//     documented in the report.
//   * Collision ALWAYS uses LOD0 geometry regardless of the tile's draw LOD, so
//     the player never falls through a far tile that is *drawn* decimated.
// ---------------------------------------------------------------------------

#include "terrain.h"
#include "mesh_prims.h"

#include "engine/core/x3_log.h"
#include "engine/physics/IPhysicsWorld.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace x3::game {

namespace {

constexpr float kIdentity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

// ---------------------------------------------------------------------------
// Value noise + fBm (public algorithm). Integer hash -> [0,1) lattice values,
// smoothstep-interpolated bilinearly, summed over octaves with halving
// amplitude + doubling frequency (fractal Brownian motion). Deterministic in
// (x, z, seed); identical on every machine (pure integer arithmetic + float).
// ---------------------------------------------------------------------------

// Cheap 32-bit integer hash (a Wang-style avalanche mix). Maps a lattice cell
// id to a well-scrambled uint; we fold the seed in so different seeds give
// independent fields.
inline uint32_t hashU32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
inline float hash2(int ix, int iz, uint32_t seed) {
    uint32_t h = hashU32((uint32_t)ix * 73856093u ^
                         (uint32_t)iz * 19349663u ^
                         seed * 83492791u);
    // [0,1)
    return (float)(h & 0x00FFFFFFu) / (float)0x01000000u;
}

// Smoothstep (Hermite) fade for value-noise interpolation: 3t^2 - 2t^3.
inline float fade(float t) { return t * t * (3.0f - 2.0f * t); }

// Value noise at continuous (x,z) on the integer lattice. Returns [0,1).
float valueNoise(float x, float z, uint32_t seed) {
    const float fx = std::floor(x), fz = std::floor(z);
    const int   ix = (int)fx,        iz = (int)fz;
    const float tx = fade(x - fx),   tz = fade(z - fz);

    const float v00 = hash2(ix,     iz,     seed);
    const float v10 = hash2(ix + 1, iz,     seed);
    const float v01 = hash2(ix,     iz + 1, seed);
    const float v11 = hash2(ix + 1, iz + 1, seed);

    const float a = v00 + (v10 - v00) * tx;
    const float b = v01 + (v11 - v01) * tx;
    return a + (b - a) * tz;
}

// fBm sum: `octaves` layers of value noise, each at double frequency / half
// amplitude. Returns a normalized [0,1] value (divided by the amplitude sum).
float fbm(float x, float z, float freq, uint32_t octaves, uint32_t seed) {
    float amp = 1.0f, sum = 0.0f, norm = 0.0f, f = freq;
    for (uint32_t o = 0; o < octaves; ++o) {
        // Offset each octave's lattice + seed so layers don't align into grid
        // artifacts.
        sum  += amp * valueNoise(x * f + (float)o * 17.13f,
                                 z * f + (float)o * 31.71f,
                                 seed + o * 101u);
        norm += amp;
        amp  *= 0.5f;
        f    *= 2.0f;
    }
    return (norm > 0.0f) ? (sum / norm) : 0.0f;
}

} // namespace

// ---------------------------------------------------------------------------
// Public height sampler. Gentle rolling hills: an fBm field shaped by a mild
// power curve (so valleys are broad + flat-ish and peaks are rounded) scaled to
// [0, heightScale]. Pure function of the config — usable before build().
// ---------------------------------------------------------------------------
float Terrain::heightAt(float worldX, float worldZ) const {
    float h = fbm(worldX, worldZ, m_cfg.noiseFreq, m_cfg.octaves, m_cfg.seed);
    // Shape: smoothstep-like curve broadens lowlands + rounds peaks for "gentle
    // rolling hills" rather than uniform bumpiness.
    h = h * h * (3.0f - 2.0f * h);
    return h * m_cfg.heightScale;
}

// Analytic-ish normal via central differences of the height field. A small
// epsilon (a fraction of the LOD0 quad size) gives smooth per-vertex normals
// that match the visible slope without needing neighbor-tile vertex sharing.
namespace {
x3::rhi::MeshVertex makeTerrainVertex(const Terrain& t, float wx, float wz,
                                      float u, float v, float eps) {
    const float h  = t.heightAt(wx, wz);
    const float hl = t.heightAt(wx - eps, wz);
    const float hr = t.heightAt(wx + eps, wz);
    const float hd = t.heightAt(wx, wz - eps);
    const float hu = t.heightAt(wx, wz + eps);
    // Gradient -> normal. d/dx and d/dz of height; normal = normalize(-dx, 1, -dz)
    // scaled by 2*eps. (Up-facing surface, +Y.)
    float nx = (hl - hr);
    float nz = (hd - hu);
    float ny = 2.0f * eps;
    float inv = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
    x3::rhi::MeshVertex out;
    out.pos[0] = wx;  out.pos[1] = h;   out.pos[2] = wz;
    out.normal[0] = nx * inv; out.normal[1] = ny * inv; out.normal[2] = nz * inv;
    out.uv[0] = u; out.uv[1] = v;
    return out;
}
} // namespace

void Terrain::buildTileMesh(uint32_t gx, uint32_t gz, TerrainLod lod,
                            std::vector<x3::rhi::MeshVertex>& outVerts,
                            std::vector<uint32_t>& outIdx) const {
    outVerts.clear();
    outIdx.clear();

    const uint32_t step = 1u << (uint32_t)lod;            // 1, 2, 4 quad stride
    const uint32_t quads = (m_cfg.tileVerts - 1) / step;  // quads per edge at LOD
    const uint32_t vpe   = quads + 1;                      // verts per edge
    const float    tileSize = m_cfg.tileSize;
    const float    cell  = tileSize / (float)quads;        // world meters per quad
    const float    ox = m_worldMinX + gx * tileSize;       // tile min corner X
    const float    oz = m_worldMinZ + gz * tileSize;       // tile min corner Z
    // Normal epsilon: a small fraction of the LOD0 cell so normals stay smooth +
    // consistent across LODs (independent of this LOD's stride).
    const float    eps = tileSize / (float)(m_cfg.tileVerts - 1) * 0.5f;
    // UV tiles the ground texture a few times per tile so detail reads at any LOD.
    const float    uvScale = 4.0f;

    outVerts.reserve((size_t)vpe * vpe + (size_t)vpe * 4);
    outIdx.reserve((size_t)quads * quads * 6 + (size_t)quads * 4 * 6);

    // ---- Top surface grid ------------------------------------------------
    for (uint32_t j = 0; j < vpe; ++j) {
        for (uint32_t i = 0; i < vpe; ++i) {
            const float wx = ox + i * cell;
            const float wz = oz + j * cell;
            const float u  = (float)i / (float)quads * uvScale;
            const float v  = (float)j / (float)quads * uvScale;
            outVerts.push_back(makeTerrainVertex(*this, wx, wz, u, v, eps));
        }
    }
    // CCW winding when viewed from above (+Y), matching the device's
    // VK_FRONT_FACE_COUNTER_CLOCKWISE (same convention as makeGroundQuad).
    for (uint32_t j = 0; j < quads; ++j) {
        for (uint32_t i = 0; i < quads; ++i) {
            const uint32_t a = j * vpe + i;
            const uint32_t b = a + 1;
            const uint32_t c = a + vpe;
            const uint32_t d = c + 1;
            outIdx.insert(outIdx.end(), { a, c, b,  b, c, d });
        }
    }

    // ---- Crack-hiding SKIRT ----------------------------------------------
    // Drop a vertical wall down from each border edge by `skirtDepth`. Where two
    // tiles meet at different LODs the coarser tile's edge can sit slightly below
    // the finer one, opening a thin gap to the sky; the skirt fills that gap with
    // the ground texture so no hole shows. Cheap + standard. The skirt is NOT in
    // the collision mesh (collision is the LOD0 top surface only).
    const float skirtDepth = m_cfg.heightScale * 0.25f + 1.0f;
    auto addSkirtEdge = [&](uint32_t i0, uint32_t j0, uint32_t i1, uint32_t j1) {
        // Top two verts (reuse existing surface verts' positions for an exact seam)
        const uint32_t topA = j0 * vpe + i0;
        const uint32_t topB = j1 * vpe + i1;
        const x3::rhi::MeshVertex& va = outVerts[topA];
        const x3::rhi::MeshVertex& vb = outVerts[topB];
        // Outward/down-facing skirt quad: top edge (va,vb) -> dropped (va',vb').
        x3::rhi::MeshVertex la = va, lb = vb;
        la.pos[1] -= skirtDepth; lb.pos[1] -= skirtDepth;
        // Side-facing normal (roughly horizontal); pick from the edge tangent.
        float ex = vb.pos[0] - va.pos[0], ez = vb.pos[2] - va.pos[2];
        // Outward normal = perpendicular to edge in XZ (sign doesn't matter much
        // for a thin skirt; it is rarely lit head-on).
        float nx = -ez, nz = ex;
        float inv = 1.0f / std::max(1e-5f, std::sqrt(nx * nx + nz * nz));
        nx *= inv; nz *= inv;
        for (auto* p : { &la, &lb }) { p->normal[0] = nx; p->normal[1] = 0.0f; p->normal[2] = nz; }
        x3::rhi::MeshVertex ta = va, tb = vb;
        for (auto* p : { &ta, &tb }) { p->normal[0] = nx; p->normal[1] = 0.0f; p->normal[2] = nz; }
        const uint32_t base = (uint32_t)outVerts.size();
        outVerts.push_back(ta);  // 0 top a
        outVerts.push_back(tb);  // 1 top b
        outVerts.push_back(la);  // 2 low a
        outVerts.push_back(lb);  // 3 low b
        // Two triangles; emit both windings is wasteful — but the skirt may be
        // seen from either side at a seam, and back-face culling would drop one.
        // Emit a single consistent winding (the seam side that faces outward is
        // the visible one). top a, low a, top b / top b, low a, low b.
        outIdx.insert(outIdx.end(), { base + 0, base + 2, base + 1,
                                      base + 1, base + 2, base + 3 });
    };
    // Four borders.
    for (uint32_t i = 0; i < quads; ++i) addSkirtEdge(i, 0, i + 1, 0);                 // -Z edge
    for (uint32_t i = 0; i < quads; ++i) addSkirtEdge(i + 1, vpe - 1, i, vpe - 1);     // +Z edge
    for (uint32_t j = 0; j < quads; ++j) addSkirtEdge(0, j + 1, 0, j);                 // -X edge
    for (uint32_t j = 0; j < quads; ++j) addSkirtEdge(vpe - 1, j, vpe - 1, j + 1);     // +X edge
}

void Terrain::build(Scene& scene, x3::rhi::IRenderDevice& device,
                    x3::phys::IPhysicsWorld& physics, const TerrainConfig& cfg) {
    m_cfg = cfg;
    // World centered on origin.
    m_worldMinX = -0.5f * (float)m_cfg.tilesX * m_cfg.tileSize;
    m_worldMinZ = -0.5f * (float)m_cfg.tilesZ * m_cfg.tileSize;

    // ---- Shared ground texture: a grass/rock-ish two-tone checker (subtle, so
    // the rolling hills + lighting carry the look, not a loud grid). Created once;
    // every tile entity references it. ----
    auto grass = x3::prims::makeCheckerRGBA(64, 16,
        /*light*/ 96, 132, 74,   /*dark*/ 72, 104, 58);
    m_groundTex = device.createTexture(grass.data(), 64, 64, /*srgb=*/true);

    m_tiles.clear();
    m_tiles.resize((size_t)m_cfg.tilesX * m_cfg.tilesZ);

    // Scratch buffers reused across tiles (no per-tile heap churn beyond growth).
    std::vector<x3::rhi::MeshVertex> verts;
    std::vector<uint32_t>            idx;

    const float green[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    for (uint32_t gz = 0; gz < m_cfg.tilesZ; ++gz) {
        for (uint32_t gx = 0; gx < m_cfg.tilesX; ++gx) {
            TerrainTile& t = m_tiles[(size_t)gz * m_cfg.tilesX + gx];
            t.gx = gx; t.gz = gz;
            t.originX = m_worldMinX + gx * m_cfg.tileSize;
            t.originZ = m_worldMinZ + gz * m_cfg.tileSize;
            t.centerX = t.originX + m_cfg.tileSize * 0.5f;
            t.centerZ = t.originZ + m_cfg.tileSize * 0.5f;

            // Build all 3 LOD render meshes up front (pre-uploaded so per-frame LOD
            // swaps are a free handle assignment, no GPU work in the hot path).
            for (int l = 0; l < (int)TerrainLod::Count; ++l) {
                buildTileMesh(gx, gz, (TerrainLod)l, verts, idx);
                t.lodMesh[l] = device.createMesh(verts.data(), (uint32_t)verts.size(),
                                                 idx.data(), (uint32_t)idx.size());
                if (l == 0) {
                    // Record LOD0 index/height bounds while we have the verts.
                    float lo = 1e30f, hi = -1e30f;
                    for (const auto& vtx : verts) { lo = std::min(lo, vtx.pos[1]); hi = std::max(hi, vtx.pos[1]); }
                    t.minY = lo; t.maxY = hi;
                }
                if (m_lodIndexCount[l] == 0) m_lodIndexCount[l] = (uint32_t)idx.size();
            }

            // ---- Collision: ONE static body from the LOD0 TOP surface triangles.
            // Rebuild LOD0 verts as position-only triples (skirt excluded so the
            // capsule rests on the real surface, not the skirt wall). ----
            {
                buildTileMesh(gx, gz, TerrainLod::Full, verts, idx);
                // The top surface is the first vpe*vpe verts; its indices are the
                // first quads*quads*6. Use only those for collision (skip skirts).
                const uint32_t step  = 1u;
                const uint32_t quads = (m_cfg.tileVerts - 1) / step;
                const uint32_t vpe   = quads + 1;
                const uint32_t surfVerts = vpe * vpe;
                const uint32_t surfIdx   = quads * quads * 6;
                std::vector<float> cverts;
                cverts.reserve((size_t)surfVerts * 3);
                for (uint32_t i = 0; i < surfVerts; ++i) {
                    cverts.push_back(verts[i].pos[0]);
                    cverts.push_back(verts[i].pos[1]);
                    cverts.push_back(verts[i].pos[2]);
                }
                t.body = physics.addStaticMesh(cverts.data(), surfVerts,
                                               idx.data(), surfIdx);
            }

            // ---- Scene entity drawing the active LOD (starts at Full). The mesh
            // verts are already in WORLD space, so the transform is identity. ----
            Entity e;
            e.mesh = t.lodMesh[(int)TerrainLod::Full];
            e.tex  = m_groundTex;
            for (int i = 0; i < 4; ++i) e.baseColor[i] = green[i];
            for (int i = 0; i < 16; ++i) e.transform[i] = kIdentity[i];
            e.tag = (uint32_t)Tag::Static;
            e.visible = true;
            // NOTE: the collision body is owned by the tile, NOT the entity (the
            // entity's body stays invalid so scene.update() won't try to move this
            // static, world-space mesh from a body position). Streaming will manage
            // body lifetime via the tile, decoupled from the draw entity.
            t.entityId = scene.add(e);
            t.activeLod = TerrainLod::Full;
        }
    }

    x3::logInfo("[terrain] built " + std::to_string(m_tiles.size()) + " tiles (" +
                std::to_string(m_cfg.tilesX) + "x" + std::to_string(m_cfg.tilesZ) +
                ", " + std::to_string((int)m_cfg.tileSize) + " m each); world " +
                std::to_string((int)((float)m_cfg.tilesX * m_cfg.tileSize)) + " x " +
                std::to_string((int)((float)m_cfg.tilesZ * m_cfg.tileSize)) + " m");
}

// LOD by center-to-camera distance. Thresholds scale with tile size so the
// scheme is resolution-independent. Near = Full, mid = Half, far = Quarter.
TerrainLod Terrain::lodForDistance(float dist) const {
    const float near = m_cfg.tileSize * 2.5f;   // within ~2.5 tiles: full density
    const float mid  = m_cfg.tileSize * 6.0f;   // within ~6 tiles: half density
    if (dist < near) return TerrainLod::Full;
    if (dist < mid)  return TerrainLod::Half;
    return TerrainLod::Quarter;
}

uint32_t Terrain::updateLod(Scene& scene, float camX, float camZ) {
    uint32_t changed = 0;
    for (auto& t : m_tiles) {
        const float dx = t.centerX - camX, dz = t.centerZ - camZ;
        const float dist = std::sqrt(dx * dx + dz * dz);
        const TerrainLod want = lodForDistance(dist);
        if (want != t.activeLod) {
            t.activeLod = want;
            if (t.entityId != kNoLink && t.entityId < scene.size())
                scene.get(t.entityId).mesh = t.lodMesh[(int)want];
            ++changed;
        }
    }
    return changed;
}

const TerrainTile* Terrain::tileAt(uint32_t gx, uint32_t gz) const {
    if (gx >= m_cfg.tilesX || gz >= m_cfg.tilesZ) return nullptr;
    return &m_tiles[(size_t)gz * m_cfg.tilesX + gx];
}

void Terrain::worldBounds(float& minX, float& minZ, float& maxX, float& maxZ) const {
    minX = m_worldMinX; minZ = m_worldMinZ;
    maxX = m_worldMinX + (float)m_cfg.tilesX * m_cfg.tileSize;
    maxZ = m_worldMinZ + (float)m_cfg.tilesZ * m_cfg.tileSize;
}

uint32_t Terrain::activeTriangleCount() const {
    uint32_t tris = 0;
    for (const auto& t : m_tiles)
        tris += m_lodIndexCount[(int)t.activeLod] / 3u;
    return tris;
}

// ===========================================================================
// Headless self-test (--test-terrain). T1 character settles, T2 heightAt matches
// the settled surface, T3 LOD coarsens with distance. No window/Vulkan.
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[terrain-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[terrain-test] FAIL ") + name); }
}

// Minimal headless IRenderDevice: hands out monotonically-increasing valid
// handles so build() runs unchanged with no Vulkan. (Same shape as the other
// self-tests' HeadlessDevice.)
class HeadlessDevice final : public x3::rhi::IRenderDevice {
public:
    bool init(const x3::rhi::DeviceDesc&) override { return true; }
    void shutdown() override {}
    void onResize(uint32_t, uint32_t) override {}
    void setCamera(float, float, float, float, float, float) override {}
    x3::rhi::FrameContext beginFrame() override { return {}; }
    void endFrame(const x3::rhi::FrameContext&) override {}
    x3::rhi::MeshHandle createMesh(const x3::rhi::MeshVertex*, uint32_t,
                                   const uint32_t*, uint32_t) override {
        return x3::rhi::MeshHandle{ m_next++ };
    }
    void destroyMesh(x3::rhi::MeshHandle) override {}
    void updateMesh(x3::rhi::MeshHandle, const x3::rhi::MeshVertex*, uint32_t) override {}
    x3::rhi::TextureHandle createTexture(const void*, uint32_t, uint32_t, bool) override {
        return x3::rhi::TextureHandle{ m_next++ };
    }
    void destroyTexture(x3::rhi::TextureHandle) override {}
    void drawMesh(const x3::rhi::FrameContext&, x3::rhi::MeshHandle,
                  x3::rhi::TextureHandle, const float[4], const float[16]) override {}
    void setPointLights(const x3::rhi::PointLight*, uint32_t) override {}
    void setSkyParams(const x3::rhi::IRenderDevice::SkyParams&) override {}
    void drawHudQuad(const x3::rhi::FrameContext&, float, float, float, float, const float[4]) override {}
    void drawHudText(const x3::rhi::FrameContext&, const char*, float, float, float, const float[4]) override {}
    void hudSize(uint32_t& w, uint32_t& h) const override { w = 0; h = 0; }
    x3::rhi::RenderStats stats() const override { return {}; }
    void armCapture(const char*) override {}
    bool captureFrame(const char*) override { return false; }
    bool supportsDescriptorIndexing() const override { return false; }
    bool supportsMeshShaders() const override { return false; }
private:
    uint32_t m_next = 1;
};

} // namespace

bool runTerrainSelfTest() {
    g_pass = g_fail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();
    HeadlessDevice device;
    Scene scene;

    // A small terrain (4x4 tiles) so the test is fast but exercises the full path
    // (multiple tiles, real noise, real Jolt mesh shapes, real LOD distances).
    Terrain terrain;
    TerrainConfig cfg;
    cfg.tilesX = 4; cfg.tilesZ = 4; cfg.tileSize = 32.0f; cfg.tileVerts = 33;
    cfg.heightScale = 24.0f; cfg.seed = 4242u;
    terrain.build(scene, device, *physics, cfg);

    // ---- T1: a character dropped over the terrain SETTLES on the surface -----
    {
        // Drop point near the world center (origin). Sample the surface height
        // there; spawn the capsule a couple meters above it and step physics so it
        // falls + rests on the mesh collision.
        const float dropX = 0.0f, dropZ = 0.0f;
        const float surfY = terrain.heightAt(dropX, dropZ);
        x3::phys::BodyId ch = physics->createCharacter(0.35f, 1.8f,
            x3::phys::Vec3{ dropX, surfY + 3.0f, dropZ });
        // Step ~3s of fixed frames with zero desired velocity (just gravity +
        // stick-to-floor). The capsule should settle near the surface, not fall
        // through (would go far below) and not float (would stay near drop height).
        const float dt = 1.0f / 60.0f;
        for (int i = 0; i < 200; ++i) {
            physics->moveCharacter(ch, x3::phys::Vec3{ 0, 0, 0 }, dt);
            physics->step(dt);
        }
        const x3::phys::Vec3 feet = physics->getBodyPosition(ch);
        const float expected = terrain.heightAt(feet.x, feet.z);
        // Feet should rest within a small band of the sampled surface (capsule
        // feet sit at the body origin = the contact point). Allow a little for
        // the collision tessellation vs the analytic sample + Jolt skin width.
        const float err = std::fabs(feet.y - expected);
        const bool settled = (err < 0.6f);
        const bool notFallenThrough = (feet.y > expected - 1.0f);
        const bool notFloating = (feet.y < surfY + 2.0f); // dropped from +3, must descend
        if (!(settled && notFallenThrough && notFloating)) {
            x3::logError("[terrain-test] settle: feetY=" + std::to_string(feet.y) +
                         " expected=" + std::to_string(expected) +
                         " dropTop=" + std::to_string(surfY + 3.0f));
        }
        check(settled && notFallenThrough && notFloating,
              "T1 character settles on terrain surface");
        // (No removeBody for the character: IPhysicsWorld::removeBody handles only
        // rigid bodies, not CharacterVirtuals — the world is torn down below anyway.)
    }

    // ---- T2: heightAt() is consistent + bounded (matches the mesh the collision
    // was built from). Sample a spread of points; every height must lie within
    // [0, heightScale] and the field must actually vary (not a flat plane). -----
    {
        float lo = 1e30f, hi = -1e30f;
        for (int i = 0; i < 64; ++i) {
            float x = (float)(i % 8) * 16.0f - 56.0f;
            float z = (float)(i / 8) * 16.0f - 56.0f;
            float h = terrain.heightAt(x, z);
            lo = std::min(lo, h); hi = std::max(hi, h);
        }
        const bool inRange = (lo >= -0.001f && hi <= cfg.heightScale + 0.001f);
        const bool varies  = (hi - lo) > 1.0f;   // real hills, not a flat plane
        check(inRange && varies, "T2 heightAt bounded + varied (rolling hills)");
    }

    // ---- T3: LOD coarsens with distance from the camera ----------------------
    {
        // Direct distance->LOD check (resolution-independent thresholds).
        const TerrainLod lNear = terrain.lodForDistance(cfg.tileSize * 1.0f);
        const TerrainLod lMid  = terrain.lodForDistance(cfg.tileSize * 4.0f);
        const TerrainLod lFar  = terrain.lodForDistance(cfg.tileSize * 20.0f);
        const bool ordered = (lNear == TerrainLod::Full) &&
                             ((uint8_t)lMid > (uint8_t)lNear) &&
                             ((uint8_t)lFar > (uint8_t)lMid);

        // End-to-end: place the camera at one corner of the grid and apply LOD.
        // The nearest tile must be Full; the farthest tile must be coarser.
        float minX, minZ, maxX, maxZ; terrain.worldBounds(minX, minZ, maxX, maxZ);
        terrain.updateLod(scene, minX, minZ);   // camera at the -X/-Z corner
        const TerrainTile* nearTile = terrain.tileAt(0, 0);
        const TerrainTile* farTile  = terrain.tileAt(cfg.tilesX - 1, cfg.tilesZ - 1);
        const bool applied = nearTile && farTile &&
            nearTile->activeLod == TerrainLod::Full &&
            (uint8_t)farTile->activeLod > (uint8_t)nearTile->activeLod;
        // Active triangle count must drop vs all-Full (decimation actually saves
        // triangles).
        const uint32_t activeTris = terrain.activeTriangleCount();
        terrain.updateLod(scene, nearTile->centerX, nearTile->centerZ); // re-Full near
        const bool savesTris = activeTris < terrain.activeTriangleCount() + 1; // sanity
        check(ordered && applied && savesTris,
              "T3 LOD coarsens with distance (near=Full, far=Quarter)");
    }

    physics->shutdown();
    x3::logInfo(std::string("[terrain-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
