// Crystal Valleys — Act 2, Level 15 (--world valley). See app/valley.h.
//
// Clean-room: built from the Scene + IRenderDevice + IPhysicsWorld + MonsterSystem
// + terrain placement (app/terrain.h) interfaces only — the same public seams
// app/club1127.cpp + app/env_art.cpp use. No purchased C# / id Tech source
// consulted.
//
// ---- Reaching this area ----------------------------------------------------
//   STANDALONE: `--world valley` (dispatched in app/main.cpp next to
//   `--world terrain` / `--world cliffs`). Walk it (WASD / mouse / Space / F
//   noclip); add `--screenshot <path>` to capture the showcase vantage headlessly.
//
// ---- Layout (Y-up, +X right, -Z forward; see docs/CONVENTIONS.md) ----------
//   The biome is CONTENT placed onto the engine's STREAMED procedural terrain
//   (the host brings up the streamer + sky exactly like `--world terrain`). Every
//   placement is anchored to the surface via the terrain placement API so it sits
//   ON the rolling hills, not on a flat plane:
//     * SALVARI SHIP crash site near the world origin region, dropped on the
//       terrain + TILTED to the local surface normal (terrainNormalAtWorld) as if
//       it skidded in. K'thara (the Salvari commander, ally) stands beside it.
//     * DOMINION PATROL — a few hostile enemies spread across the valley, each
//       anchored with placeOnTerrain (the MonsterSystem auto-binds the locomotion
//       blend; they chase + attack the player).
//     * A LAKE at the lowest sampled spot of the area (the host applies water at
//       this sea level), and a scatter of emissive CRYSTAL formations (each its own
//       point light) giving the valley its glow.
// ---------------------------------------------------------------------------
#include "valley.h"
#include "terrain.h"
#include "mesh_prims.h"
#include "headless_device.h"   // x3::game::HeadlessRenderDevice (--test-valley)
#include "asset_root.h"        // riggedGlbRoot() (--test-valley)

#include "engine/core/x3_log.h"

#include <algorithm>           // std::min / std::max

#include <cmath>
#include <cstdint>
#include <string>

namespace x3::game {

namespace {

constexpr float kPi = 3.14159265358979f;

// Small deterministic LCG so the scatter is reproducible run-to-run (same valley
// every launch -> stable screenshots). Seeded once in build().
struct Rng {
    uint32_t s = 0x0A11EE15u;   // "VALLEYS"
    float next01() { s = s * 1664525u + 1013904223u; return (float)((s >> 8) & 0xFFFFFF) / 16777215.0f; }
    float range(float a, float b) { return a + (b - a) * next01(); }
};

// Tints (linear-ish; the device tonemaps).
const float kRock[4]    = { 0.16f, 0.15f, 0.18f, 1.0f }; // crystal-base rock
const float kEmitOff[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
// Crystal emissive palette { r, g, b, strength }. strength > 1 => bright bloom.
const float kEmitCyan[4]   = { 0.20f, 0.90f, 1.00f, 5.0f };
const float kEmitViolet[4] = { 0.65f, 0.25f, 1.00f, 4.5f };
const float kEmitTeal[4]   = { 0.15f, 0.95f, 0.80f, 4.5f };
const float kEmitRose[4]   = { 1.00f, 0.35f, 0.70f, 4.0f };

// Push a point light (premultiplied color) into the set.
void addLight(std::vector<x3::rhi::PointLight>& v, float x, float y, float z,
              float r, float g, float b, float range) {
    x3::rhi::PointLight l;
    l.pos[0] = x; l.pos[1] = y; l.pos[2] = z; l.range = range;
    l.color[0] = r; l.color[1] = g; l.color[2] = b;
    v.push_back(l);
}

// Column-major TRS that places a model (default-facing local -Z) with a yaw about
// +Y AND a tilt that aligns its local +Y to the world surface NORMAL `n`. Used to
// lay the crashed ship down on the slope. Uniform scale `s`; world anchor (wx,wy,wz)
// (the model's pivot/foot at the origin maps there).
//
//   up    = normalize(n)                          (the surface normal)
//   fwd0  = ( -sin yaw, 0, -cos yaw )             (desired heading, local -Z)
//   right = normalize( cross(up, -fwd0) ) ... we Gram-Schmidt fwd0 against up so
//   the basis stays orthonormal even on a steep slope.
void placeTilted(float m[16], float yaw, float s,
                 const float n[3], float wx, float wy, float wz) {
    // Normalize the surface normal -> local +Y (col1).
    float uy[3] = { n[0], n[1], n[2] };
    float ul = std::sqrt(uy[0]*uy[0] + uy[1]*uy[1] + uy[2]*uy[2]);
    if (ul < 1e-5f) { uy[0] = 0; uy[1] = 1; uy[2] = 0; ul = 1.0f; }
    uy[0] /= ul; uy[1] /= ul; uy[2] /= ul;

    // Desired forward (local -Z) from yaw, in the XZ plane (see CONVENTIONS §3/§4).
    float fz[3] = { -std::sin(yaw), 0.0f, -std::cos(yaw) };
    // Right = up x (-forward) so the basis is right-handed; then re-orthogonalize.
    float rx[3] = {
        uy[1]*(-fz[2]) - uy[2]*(-fz[1]),
        uy[2]*(-fz[0]) - uy[0]*(-fz[2]),
        uy[0]*(-fz[1]) - uy[1]*(-fz[0])
    };
    float rl = std::sqrt(rx[0]*rx[0] + rx[1]*rx[1] + rx[2]*rx[2]);
    if (rl < 1e-5f) { rx[0] = 1; rx[1] = 0; rx[2] = 0; rl = 1.0f; }
    rx[0] /= rl; rx[1] /= rl; rx[2] /= rl;
    // Recompute a clean forward (local -Z column is -f, so col2 = f = right x up).
    float ff[3] = {
        rx[1]*uy[2] - rx[2]*uy[1],
        rx[2]*uy[0] - rx[0]*uy[2],
        rx[0]*uy[1] - rx[1]*uy[0]
    };
    // Columns (column-major), scaled by s. col0=right, col1=up, col2=forward(+Z).
    m[0]=rx[0]*s; m[1]=rx[1]*s; m[2]=rx[2]*s; m[3]=0;
    m[4]=uy[0]*s; m[5]=uy[1]*s; m[6]=uy[2]*s; m[7]=0;
    m[8]=ff[0]*s; m[9]=ff[1]*s; m[10]=ff[2]*s; m[11]=0;
    m[12]=wx; m[13]=wy; m[14]=wz; m[15]=1.0f;
}

} // namespace

uint32_t ValleyWorld::addCrystal(Scene& scene, x3::rhi::IRenderDevice& device,
                                 x3::phys::IPhysicsWorld& physics,
                                 float cx, float cy, float cz, float hx, float hy, float hz,
                                 const float color[4], const float emissive[4],
                                 float lr, float lg, float lb, float lrange, bool collide) {
    // World-space geometry (centered at cx,cy,cz) so the Entity transform is
    // identity (static prop — exactly like club1127's crystals / env-art boxes).
    x3::prims::PrimMesh geo = x3::prims::makeBox(hx, hy, hz, cx, cy, cz, 1.0f);
    Entity e;
    e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                               geo.index.data(), (uint32_t)geo.index.size());
    for (int i = 0; i < 4; ++i) e.baseColor[i] = color[i];
    if (emissive) for (int i = 0; i < 4; ++i) e.emissive[i] = emissive[i];
    e.tag = (uint32_t)Tag::Static;
    if (collide) {
        e.body = physics.addStaticMesh(geo.cverts.data(), (uint32_t)(geo.cverts.size() / 3),
                                       geo.cindex.data(), (uint32_t)geo.cindex.size());
    }
    uint32_t id = scene.add(e);
    if (lrange > 0.0f) addLight(m_lights, cx, cy + hy + 0.3f, cz, lr, lg, lb, lrange);
    return id;
}

uint32_t ValleyWorld::addCharacter(Scene& scene, x3::rhi::IRenderDevice& device,
                                   x3::phys::IPhysicsWorld& physics, std::string_view modelDir,
                                   const std::string& modelFile, const x3::phys::Vec3& pos,
                                   float scale, bool standUpZtoY, const float tint[4],
                                   bool hostile, MonsterType type) {
    auto sys = std::make_unique<MonsterSystem>();
    MonsterSystem::Tuning t;
    t.type        = type;
    t.hp          = hostile ? 100 : 100;
    // HOSTILE Dominion: chase + attack the player (the locomotion blend animates
    // as it moves). FRIENDLY K'thara: chaseSpeed 0 / damage 0 -> inert ally that
    // just idles (never pursues, never attacks the player).
    t.chaseSpeed  = hostile ? 2.2f : 0.0f;
    t.damage      = hostile ? 12   : 0;
    t.ranged      = hostile && (type == MonsterType::Drone);
    t.attackRange = t.ranged ? 8.0f : 1.8f;
    t.standoff    = 7.0f;
    t.modelFile   = modelFile;
    t.modelDirOverride = std::string(modelDir);
    t.standUpZtoY = standUpZtoY;
    t.modelScale  = scale;
    if (tint) for (int i = 0; i < 4; ++i) t.tint[i] = tint[i];
    sys->buildMonsterTuned(scene, device, physics, modelDir, pos, t);
    uint32_t idx = (uint32_t)m_chars.size();
    m_chars.push_back(std::move(sys));
    m_hostile.push_back(hostile);
    return idx;
}

void ValleyWorld::build(Scene& scene, x3::rhi::IRenderDevice& device,
                        x3::phys::IPhysicsWorld& physics, std::string_view modelDir) {
    if (m_built) return;
    m_built = true;
    Rng rng;

    // ===================================================================
    // WATER (lake). Find the LOWEST sampled surface point over the play area;
    // set the lake sea level a touch above it so the bowl floods into a lake the
    // host renders via setWaterParams. The spawn/ship/crystals all sit on dry
    // ground ABOVE this level. Search a grid around the origin region.
    // ===================================================================
    float lowX = 0.0f, lowZ = 0.0f, lowY = 1e9f, highY = -1e9f;
    for (float gx = -90.0f; gx <= 90.0f; gx += 12.0f)
        for (float gz = -90.0f; gz <= 90.0f; gz += 12.0f) {
            const float h = terrainHeightAtWorld(gx, gz);
            if (h < lowY) { lowY = h; lowX = gx; lowZ = gz; }
            if (h > highY) highY = h;
        }
    // Lake surface a little above the basin floor (so it actually pools, but stays
    // well below the surrounding ridges -> a visible lake, not a flood).
    m_seaLevel = lowY + 2.5f;

    // ===================================================================
    // SALVARI SHIP crash site. Pick a spot on a gentle rise near the origin,
    // ABOVE the lake, drop it ON the terrain, and TILT it to the local surface
    // normal as if it crashed/skidded. (Graceful box fallback if the GLB is
    // absent — same as every other character/prop in this engine.)
    // ===================================================================
    {
        // Choose a dry, elevated anchor for the wreck.
        float sx = 18.0f, sz = -6.0f;
        // Nudge to ground that's safely above the lake (scan outward if needed).
        for (float r = 0.0f; r < 80.0f; r += 8.0f) {
            if (terrainHeightAtWorld(sx + r, sz) > m_seaLevel + 4.0f) { sx += r; break; }
        }
        float anchor[3];
        placeOnTerrain(sx, sz, anchor);                  // sits ON the surface
        m_shipPos = x3::phys::Vec3{ anchor[0], anchor[1], anchor[2] };
        float n[3];
        terrainNormalAtWorld(sx, sz, n);                 // local slope normal

        // The ship as a MonsterSystem prop (loads SpaceShip.glb; box fallback).
        // It's inert (damage 0, chase 0). After build we OVERRIDE its scene
        // transform with the crash tilt (the model pivot maps to the anchor).
        const float steel[4] = { 0.85f, 0.88f, 0.95f, 1.0f };
        uint32_t shipIdx = addCharacter(scene, device, physics, modelDir, "SpaceShip.glb",
                                        m_shipPos, 3.5f, /*standUpZtoY=*/false, steel,
                                        /*hostile=*/false, MonsterType::Guard);
        // Mark the ship as NOT a character for the update/ally bookkeeping: it is
        // a prop, but it lives in m_chars so its GLB handles stay alive + it draws.
        m_hostile[shipIdx] = false;
        // Bake the crashed tilt: yaw it ~25deg off-axis, aligned to the slope.
        uint32_t shipEnt = m_chars[shipIdx]->entity();
        if (shipEnt != kNoLink && shipEnt < scene.size()) {
            float tilt[16];
            placeTilted(tilt, /*yaw=*/0.7f, /*scale=*/3.5f, n,
                        m_shipPos.x, m_shipPos.y, m_shipPos.z);
            for (int i = 0; i < 16; ++i) scene.get(shipEnt).transform[i] = tilt[i];
        }
        // Crash-glow point light + a couple of sparking emissive hull-breach
        // crystals embedded by the wreck.
        addLight(m_lights, m_shipPos.x, m_shipPos.y + 2.5f, m_shipPos.z, 1.4f, 1.1f, 0.7f, 16.0f);
    }

    // ===================================================================
    // K'THARA — the Salvari commander, friendly ALLY, standing beside the wreck.
    // Spawned like an enemy but marked non-hostile (damage 0 / chase 0): she
    // never attacks the player. Uses Oracle.glb (reads as an alien commander;
    // graceful box fallback). Placed ON the surface a few meters from the ship.
    // ===================================================================
    {
        const float kx = m_shipPos.x - 4.0f, kz = m_shipPos.z + 3.0f;
        float anchor[3];
        placeOnTerrain(kx, kz, anchor);
        m_ktharaPos = x3::phys::Vec3{ anchor[0], anchor[1], anchor[2] };
        const float salvariGlow[4] = { 0.8f, 1.1f, 1.3f, 1.0f }; // bioluminescent-ish
        addCharacter(scene, device, physics, modelDir, "Oracle.glb",
                     m_ktharaPos, 1.0f, /*standUpZtoY=*/true, salvariGlow,
                     /*hostile=*/false, MonsterType::Guard);
        m_ktharaAlly = true;
        // A soft friendly accent light on K'thara so she reads as the ally beacon.
        addLight(m_lights, m_ktharaPos.x, m_ktharaPos.y + 2.0f, m_ktharaPos.z,
                 0.7f, 1.1f, 1.4f, 9.0f);
    }

    // ===================================================================
    // DOMINION PATROL — a few hostile enemies spread across the valley, each
    // anchored ON the terrain via placeOnTerrain. They chase + attack the player;
    // the MonsterSystem auto-binds the locomotion blend as they move. Mix of
    // Guards (melee) and a Drone (ranged) for variety. alien_crawler / marcus_webb
    // are the rigged guards in the repo; a missing GLB falls back to a box.
    // ===================================================================
    {
        const float domTint[4] = { 0.75f, 0.80f, 0.95f, 1.0f }; // cold grey Dominion
        // Patrol anchors (XZ), spread away from the lake + crash. The Y is set by
        // placeOnTerrain so each stands on the rolling surface.
        struct Patrol { float x, z; bool drone; const char* glb; float scale; bool zUp; };
        const Patrol patrols[] = {
            { 30.0f,  18.0f, false, "marcus_webb.glb",   1.0f, false },
            { -22.0f, 26.0f, false, "alien_crawler.glb", 1.0f, false },
            { 44.0f, -14.0f, true,  "marcus_webb.glb",   1.0f, false },
            { -34.0f, -28.0f, false, "alien_crawler.glb", 1.0f, false },
        };
        for (const Patrol& p : patrols) {
            float anchor[3];
            placeOnTerrain(p.x, p.z, anchor);
            const x3::phys::Vec3 pos{ anchor[0], anchor[1], anchor[2] };
            addCharacter(scene, device, physics, modelDir, p.glb, pos,
                         p.scale, p.zUp, domTint, /*hostile=*/true,
                         p.drone ? MonsterType::Drone : MonsterType::Guard);
            ++m_dominionCount;
        }
    }

    // ===================================================================
    // CRYSTAL FORMATIONS — emissive shards scattered across the valley, each
    // anchored ON the terrain (placeOnTerrain) + carrying its own point light
    // (LevelArchitect-style, same construction the caves use). A few clustered
    // by the crash so the showcase frame glows.
    // ===================================================================
    {
        const float* palette[4] = { kEmitCyan, kEmitViolet, kEmitTeal, kEmitRose };
        const int kClusters = 9;
        for (int c = 0; c < kClusters; ++c) {
            // Cluster center: a couple near the crash, the rest scattered wide.
            float ccx, ccz;
            if (c < 3) {
                ccx = m_shipPos.x + rng.range(-7.0f, 7.0f);
                ccz = m_shipPos.z + rng.range(-7.0f, 7.0f);
            } else {
                ccx = rng.range(-75.0f, 75.0f);
                ccz = rng.range(-75.0f, 75.0f);
            }
            const int shards = 2 + (int)rng.range(0.0f, 3.0f);
            for (int sI = 0; sI < shards; ++sI) {
                const float sxp = ccx + rng.range(-2.5f, 2.5f);
                const float szp = ccz + rng.range(-2.5f, 2.5f);
                float anchor[3];
                placeOnTerrain(sxp, szp, anchor);
                // Skip shards that would be under the lake (keep crystals on land).
                if (anchor[1] < m_seaLevel + 0.2f) continue;
                const float ch = rng.range(1.2f, 3.2f);     // shard height
                const float hw = rng.range(0.18f, 0.45f);   // shard half-width
                const float* em = palette[(c + sI) & 3];
                addCrystal(scene, device, physics,
                           anchor[0], anchor[1] + ch * 0.5f, anchor[2],
                           hw, ch * 0.5f, hw, kRock, em,
                           em[0] * 0.5f, em[1] * 0.5f, em[2] * 0.5f, rng.range(5.0f, 8.0f),
                           /*collide=*/true);
            }
        }
    }

    // ===================================================================
    // PLAYER SPAWN — on dry ground a short distance from the crash, looking
    // toward the wreck + K'thara. placeOnTerrain anchors the feet on the surface;
    // raise a touch so the capsule settles cleanly on the first frames.
    // ===================================================================
    {
        const float px = m_shipPos.x - 12.0f, pz = m_shipPos.z + 8.0f;
        float anchor[3];
        placeOnTerrain(px, pz, anchor);
        m_spawn = x3::phys::Vec3{ anchor[0], std::max(anchor[1], m_seaLevel) + 2.0f, anchor[2] };
    }

    x3::logInfo("[valley] built Crystal Valleys (Act 2, L15): " +
                std::to_string(scene.size()) + " entities, " +
                std::to_string(m_lights.size()) + " point lights, " +
                std::to_string(m_dominionCount) + " Dominion enemies, K'thara ally=" +
                (m_ktharaAlly ? "yes" : "no") + ", lake seaLevel=" +
                std::to_string(m_seaLevel));
}

void ValleyWorld::update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                         const x3::phys::Vec3& playerPos, IDamageSink* target) {
    for (size_t i = 0; i < m_chars.size(); ++i) {
        if (m_hostile[i]) {
            // Hostile Dominion: chase + attack the player (locomotion blend animates).
            m_chars[i]->update(dt, scene, physics, playerPos, target, AttackFxFn{});
        } else {
            // Friendly ally / inert ship prop: never pursue, never attack. Tick
            // toward its own position so its idle clip plays in place.
            m_chars[i]->update(dt, scene, physics, m_chars[i]->pos());
        }
    }
}

void ValleyWorld::drawCharacters(x3::rhi::IRenderDevice& device,
                                 const x3::rhi::FrameContext& frame, const Scene& scene) const {
    for (const auto& c : m_chars)
        c->drawMonster(device, frame, scene);
}

void ValleyWorld::showcaseCamera(float out[5]) const {
    // Vantage backed up from the crash site, elevated, looking toward the wreck +
    // K'thara + the nearest crystal cluster (the lake + ridges fill the distance).
    out[0] = m_shipPos.x - 16.0f;          // x: back from the wreck
    out[1] = m_shipPos.y + 9.0f;           // y: elevated over the valley
    out[2] = m_shipPos.z + 11.0f;          // z: off to one side
    // Look from the camera toward the ship: yaw = atan2(dz, dx) in the §3 basis.
    const float dx = m_shipPos.x - out[0];
    const float dz = m_shipPos.z - out[2];
    out[3] = std::atan2(dz, dx);           // yaw toward the wreck
    out[4] = -0.32f;                       // pitch: down over the valley floor
}

// ===========================================================================
// Headless self-test (--test-valley). Build the valley content onto a SYNCHRONOUS
// (no-job) terrain streamer via a HeadlessRenderDevice + physics, then assert the
// placement / spawn / water invariants. No window / Vulkan.
// ===========================================================================
bool runValleySelfTest() {
    int pass = 0, fail = 0;
    auto check = [&](bool cond, const char* name) {
        if (cond) { ++pass; x3::logInfo(std::string("[valley-test] PASS ") + name); }
        else      { ++fail; x3::logError(std::string("[valley-test] FAIL ") + name); }
    };

    using HeadlessDevice = x3::game::HeadlessRenderDevice;

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();
    HeadlessDevice device;
    Scene scene;

    // Bring up the SAME streamed terrain the host uses (canonical config), but
    // synchronous (null jobs) so generation runs inline — no thread pool needed.
    const TerrainConfig& cfg = worldTerrainConfig();
    TerrainStreamer streamer;
    streamer.init(scene, device, *physics, /*jobs=*/nullptr, cfg, 0.0f, 0.0f, /*radius=*/4);

    // (1) The placement API returns finite, in-range heights over the play area.
    {
        bool allFinite = true;
        float minH = 1e9f, maxH = -1e9f;
        for (float gx = -90.0f; gx <= 90.0f; gx += 15.0f)
            for (float gz = -90.0f; gz <= 90.0f; gz += 15.0f) {
                const float h = terrainHeightAtWorld(gx, gz);
                if (!std::isfinite(h)) allFinite = false;
                minH = std::min(minH, h); maxH = std::max(maxH, h);
                // placeOnTerrain must agree with the raw height query.
                float p[3]; placeOnTerrain(gx, gz, p);
                if (std::fabs(p[1] - h) > 1e-3f || !std::isfinite(p[0]) || !std::isfinite(p[2]))
                    allFinite = false;
            }
        // Heights live within the configured band [0, heightScale] and vary.
        const bool inBand = (minH >= -0.5f) && (maxH <= cfg.heightScale + 0.5f);
        const bool varied = (maxH - minH) > 1.0f;
        check(allFinite && inBand && varied,
              "terrain placement queries are finite, in-band, and varied");
    }

    // (2) A surface NORMAL query is unit-length and points generally +Y.
    {
        bool ok = true;
        for (float gx = -60.0f; gx <= 60.0f; gx += 30.0f)
            for (float gz = -60.0f; gz <= 60.0f; gz += 30.0f) {
                float n[3]; terrainNormalAtWorld(gx, gz, n);
                const float len = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
                if (std::fabs(len - 1.0f) > 1e-2f || n[1] <= 0.0f) ok = false;
            }
        check(ok, "terrain normals are unit-length and point +Y");
    }

    // Build the valley content onto the streamed terrain.
    ValleyWorld valley;
    valley.build(scene, device, *physics, x3::game::riggedGlbRoot());

    // (3) The Salvari ship sits ON the surface (Y matches terrainHeightAtWorld).
    {
        const x3::phys::Vec3 s = valley.shipPos();
        const float surf = terrainHeightAtWorld(s.x, s.z);
        check(std::fabs(s.y - surf) < 0.05f, "Salvari ship is placed ON the terrain surface");
    }

    // (4) K'thara sits ON the surface AND is a friendly ally.
    {
        const x3::phys::Vec3 k = valley.ktharaPos();
        const float surf = terrainHeightAtWorld(k.x, k.z);
        check(std::fabs(k.y - surf) < 0.05f && valley.ktharaIsAlly(),
              "K'thara is placed ON the surface and is a non-hostile ally");
    }

    // (5) The expected number of Dominion enemies spawned.
    {
        check(valley.dominionCount() == 4, "the expected Dominion patrol (4) spawned");
    }

    // (6) Water params: a positive lake sea level is exposed AND can be applied to
    // the device (the host sets it each frame; here we prove the call path works).
    {
        const float sea = valley.waterSeaLevel();
        x3::rhi::IRenderDevice::WaterParams wp{};
        wp.enabled = true; wp.seaLevel = sea; wp.amplitude = 0.4f;
        device.setWaterParams(wp);   // no-op on the headless device, but exercises the seam
        check(std::isfinite(sea) && sea > 0.0f, "water sea level is valid and set");
    }

    // (7) The crash site + spawn are distinct and dry (spawn above the lake).
    {
        const x3::phys::Vec3 sp = valley.spawn();
        const bool drySpawn = sp.y >= valley.waterSeaLevel();
        const x3::phys::Vec3 s = valley.shipPos();
        const float d = std::sqrt((sp.x - s.x)*(sp.x - s.x) + (sp.z - s.z)*(sp.z - s.z));
        check(drySpawn && d > 3.0f, "player spawn is dry and offset from the crash site");
    }

    streamer.shutdown(scene, device, *physics);
    physics->shutdown();

    x3::logInfo(std::string("[valley-test] ") + std::to_string(pass) + " passed, " +
                std::to_string(fail) + " failed");
    return fail == 0;
}

} // namespace x3::game
