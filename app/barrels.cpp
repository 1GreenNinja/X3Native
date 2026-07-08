// Explosive barrels — see app/barrels.h.
//
// Clean-room: DestructibleManager + IPhysicsWorld + IRenderDevice interfaces only.
#include "barrels.h"
#include "mesh_prims.h"
#include "headless_device.h"
#include "asset_root.h"   // convertedGlbRoot()

#include "engine/core/x3_log.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <string>

namespace x3::game {

namespace {
// Barrel ~0.88 dia x 1.23 tall (env_art kBarrelAabb). Fractures into barrel-anatomy
// shrapnel (staves/hoops/plates — makeBarrelShrapnelChunks below), not a cube grid.
constexpr float kHalfX = 0.42f, kHalfY = 0.60f, kHalfZ = 0.42f;
constexpr float kChainDelay = 0.06f;   // s between a blast and the chained barrel going
// Fresh shrapnel glows ember-hot right after the blast, then cools to charred
// dark metal over this many seconds (render-side tint only — no physics change).
constexpr float kEmberCoolTime = 1.0f;

// ---- Barrel-anatomy shrapnel (replaces the old 2x3x2 cube grid) -------------
// The uniform grid fracture read as "12 tidy red boxes". A real barrel blows into
// STAVES (tall wall slats), HOOP-RING arc fragments, and lid/base plates. The
// fracture API takes convex hulls for PHYSICS, but ChunkView reports each chunk
// to the renderer as a box (halfExtents only) — so every piece is authored as a
// ROTATED, IRREGULARLY-SIZED box that physics + render share: 8 uneven staves
// around the rim, 6 hoop arcs on three bands, 2 ragged lid halves + a base
// plate. 17 mismatched pieces that tumble like shrapnel, not a grid.
uint32_t makeBarrelShrapnelChunks(std::vector<x3::phys::FractureChunkDesc>& out) {
    out.clear();
    constexpr float kPi = 3.14159265f;
    // Tiny deterministic LCG for the per-piece size/placement jitter, so the
    // layout is repeatable (headless captures + tests see the same barrel).
    uint32_t rng = 0xBA221u;
    auto jit = [&rng](float lo, float hi) {
        rng = rng * 1664525u + 1013904223u;
        return lo + (hi - lo) * (float)((rng >> 8) & 0xFFFFu) / 65535.0f;
    };
    auto yawQuat = [](float a, float q[4]) {   // rotation about +Y (x,y,z,w)
        q[0] = 0.0f; q[1] = std::sin(a * 0.5f); q[2] = 0.0f; q[3] = std::cos(a * 0.5f);
    };
    // 8 wall STAVES: tall thin slats around the rim; local +Z is the radial axis
    // (the yaw points it outward), local X the slat width, local Y the height.
    const int nStave = 8;
    for (int i = 0; i < nStave; ++i) {
        const float a = (2.0f * kPi) * ((float)i + jit(-0.18f, 0.18f)) / (float)nStave;
        x3::phys::FractureChunkDesc c{};
        c.halfExtents[0] = jit(0.11f, 0.17f);   // uneven slat widths
        c.halfExtents[1] = jit(0.34f, 0.56f);   // ragged break heights
        c.halfExtents[2] = jit(0.035f, 0.06f);  // wall thickness
        const float rMid = kHalfX - c.halfExtents[2];
        c.localOffset[0] = rMid * std::sin(a);
        c.localOffset[1] = jit(-0.08f, 0.08f);
        c.localOffset[2] = rMid * std::cos(a);
        yawQuat(a, c.localRot);
        // NOTE masses here also set the scatter speed: the radial break kick is
        // impulse/mass, so a too-light piece becomes 150+ m/s hyperspeed shrapnel
        // (invisible + across the map). Keep every piece in the ~0.4-0.7 kg band
        // so the blast stays in the intended ~40-70 m/s "violent but visible" range.
        c.mass = 0.5f;
        out.push_back(c);
    }
    // 6 HOOP-RING fragments: short arc segments on the top/mid/bottom bands,
    // sitting proud of the wall; local X is the arc tangent (thin metal strips).
    const float bandY[3] = { 0.50f, 0.0f, -0.50f };
    for (int b = 0; b < 3; ++b) {
        for (int k = 0; k < 2; ++k) {
            const float a = (float)b * 0.7f + kPi * (float)k + jit(-0.5f, 0.5f);
            x3::phys::FractureChunkDesc c{};
            c.halfExtents[0] = jit(0.14f, 0.20f);   // arc length
            c.halfExtents[1] = jit(0.025f, 0.04f);
            c.halfExtents[2] = jit(0.02f, 0.035f);
            const float rHoop = kHalfX + 0.01f;
            c.localOffset[0] = rHoop * std::sin(a);
            c.localOffset[1] = bandY[b] + jit(-0.03f, 0.03f);
            c.localOffset[2] = rHoop * std::cos(a);
            yawQuat(a, c.localRot);
            c.mass = 0.4f;   // light metal, but see the speed note above
            out.push_back(c);
        }
    }
    // Lid: two ragged halves (so the top pops as plates, not one neat disc).
    for (int k = 0; k < 2; ++k) {
        x3::phys::FractureChunkDesc c{};
        c.halfExtents[0] = jit(0.24f, 0.32f);
        c.halfExtents[1] = 0.03f;
        c.halfExtents[2] = jit(0.12f, 0.17f);
        c.localOffset[0] = jit(-0.04f, 0.04f);
        c.localOffset[1] = kHalfY - 0.04f;
        c.localOffset[2] = (k == 0 ? -1.0f : 1.0f) * (c.halfExtents[2] + 0.01f);
        yawQuat(jit(-0.35f, 0.35f), c.localRot);
        c.mass = 0.55f;
        out.push_back(c);
    }
    // Base plate: one heavier disc-ish slab.
    {
        x3::phys::FractureChunkDesc c{};
        c.halfExtents[0] = jit(0.26f, 0.31f);
        c.halfExtents[1] = 0.035f;
        c.halfExtents[2] = jit(0.26f, 0.31f);
        c.localOffset[1] = -(kHalfY - 0.045f);
        yawQuat(jit(-0.4f, 0.4f), c.localRot);
        c.mass = 0.7f;
        out.push_back(c);
    }
    return (uint32_t)out.size();
}
} // namespace

void BarrelSystem::init(x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics) {
    m_device = &device; m_physics = &physics;

    std::vector<x3::rhi::MeshVertex> cv; std::vector<uint32_t> ci;
    x3::prims::makeCube(0.5f, cv, ci);
    m_cube = device.createMesh(cv.data(), (uint32_t)cv.size(), ci.data(), (uint32_t)ci.size());
    auto px = x3::prims::makeCheckerRGBA(64, 8, 200, 120, 40, 90, 50, 20);  // rusty orange barrel
    m_tex = device.createTexture(px.data(), 64, 64, true);
    auto cpx = x3::prims::makeCheckerRGBA(64, 8, 48, 44, 40, 26, 24, 22);   // charred dark metal
    m_charTex = device.createTexture(cpx.data(), 64, 64, true);

    x3::phys::DestructionTuning t; t.maxActiveChunks = 256;
    m_destr.init(&physics, t);

    // Barrel-anatomy shrapnel (staves + hoop arcs + lid/base plates) instead of
    // the old uniform 2x3x2 cube grid — see makeBarrelShrapnelChunks above.
    std::vector<x3::phys::FractureChunkDesc> chunks;
    makeBarrelShrapnelChunks(chunks);
    x3::phys::FractureAssetDesc ad{};
    ad.chunks = chunks.data();
    ad.chunkCount = (uint32_t)chunks.size();
    ad.breakImpulse = 12.0f;     // a solid hit pops it
    ad.breakRelVel  = 7.0f;
    m_asset = m_destr.loadFractureAsset(ad);
    x3::logInfo("[barrels] init — fracture asset " + std::to_string(m_asset) +
                " (" + std::to_string(chunks.size()) + " chunks)");

    // ---- Real round barrel model (overlay for INTACT barrels). Mirrors the
    // rescue.cpp / env_art asset-load pattern. Falls back to the cube if absent. ----
    m_assets.reset(x3::asset::createAssetSource());
    if (m_assets->mountDir(x3::game::convertedGlbRoot(), 0)) {
        m_loader.reset(x3::asset::createModelLoader(&device, m_assets.get()));
        m_barrelModel = m_loader->load("SciFi_Warehouse_Kit/Barrel.glb");
        if (m_barrelModel.ok) {
            m_barrelDrawables = x3::asset::makeDrawables(m_barrelModel);
            x3::logInfo("[barrels] loaded Barrel.glb — " +
                        std::to_string(m_barrelDrawables.size()) + " prim(s)");
        } else {
            x3::logWarn("[barrels] Barrel.glb load failed; using cube fallback");
        }
    } else {
        x3::logWarn("[barrels] mountDir failed (" + x3::game::convertedGlbRoot() +
                    "); using cube fallback");
    }
}

uint32_t BarrelSystem::spawn(float x, float floorY, float z) {
    const float cy = floorY + kHalfY;   // base on the floor
    float xf[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, x, cy, z, 1 };
    x3::phys::DestructibleId id = m_destr.spawnDestructible(m_asset, xf);
    uint32_t idx = (uint32_t)m_barrels.size();
    m_barrels.push_back(Barrel{ id, { x, cy, z }, false });
    return idx;
}

bool BarrelSystem::onShot(const float eye[3], const float dir[3], float range) {
    // applyHit breaks the first destructible the ray hits; detonation happens in
    // update() when we see it broke (so the blast + chain run on the sim step).
    // strength is the contact impulse — above the barrel's breakImpulse.
    return m_destr.applyHit(eye, dir, 40.0f);
    (void)range;
}

void BarrelSystem::detonate(Barrel& b) {
    b.exploded  = true;
    b.sinceBoom = 0.0f;   // shrapnel starts ember-hot and cools in render()
    // Violent scatter: a strong radial impulse at the barrel center throws its own
    // chunks AND breaks every other intact barrel within the blast radius (chain).
    m_destr.applyRadialImpulse(b.center, m_blastRadius, m_blastStrength);
    if (m_fx)     m_fx(b.center, m_blastRadius);
    if (m_damage) m_damage(b.center, m_blastRadius, m_blastDamage);
    x3::logInfo("[barrels] BOOM at (" + std::to_string((int)b.center[0]) + "," +
                std::to_string((int)b.center[1]) + "," + std::to_string((int)b.center[2]) + ")");
}

void BarrelSystem::update(float dt) {
    m_destr.update(dt);
    // Any barrel that has broken but not yet detonated -> blow it up now. The blast
    // breaks neighbors; THEY detonate on a subsequent update -> a cascading chain.
    for (Barrel& b : m_barrels) {
        if (b.exploded) { b.sinceBoom += dt; continue; }   // age the ember glow
        if (m_destr.isBroken(b.id))
            detonate(b);
    }
    (void)kChainDelay;
}

void BarrelSystem::render(const x3::rhi::FrameContext& frame) const {
    if (!m_device) return;
    const float intact[4] = { 0.85f, 0.50f, 0.18f, 1.0f };   // rusty orange tint

    // Debris tint: fresh shrapnel glows EMBER-HOT (HDR tint on the charred texture
    // feeds the bloom chain) then cools to dark char over kEmberCoolTime. Age comes
    // from the owning barrel's sinceBoom (ChunkView carries the owner id).
    auto debrisTint = [this](x3::phys::DestructibleId owner, float col[4]) {
        float age = kEmberCoolTime;
        for (const Barrel& b : m_barrels)
            if (b.id == owner) { age = b.exploded ? b.sinceBoom : kEmberCoolTime; break; }
        float t = age / kEmberCoolTime;
        if (t > 1.0f) t = 1.0f;
        const float s = t * t * (3.0f - 2.0f * t);        // smoothstep cool-down
        const float hot[3]  = { 11.0f, 4.0f, 0.9f };      // ember-hot (HDR -> bloom)
        const float cold[3] = { 0.9f, 0.82f, 0.75f };     // charred (texture is dark)
        col[0] = hot[0] + (cold[0] - hot[0]) * s;
        col[1] = hot[1] + (cold[1] - hot[1]) * s;
        col[2] = hot[2] + (cold[2] - hot[2]) * s;
        col[3] = 1.0f;
    };

    if (!m_barrelDrawables.empty()) {
        // INTACT barrels render as the real round Barrel.glb at each barrel's base;
        // exploded/scattered DEBRIS still renders as the fracture-chunk cubes (the GLB
        // has no shatter pieces). The barrel base sits at center.y - kHalfY (floor).
        for (const Barrel& b : m_barrels) {
            if (b.exploded) continue;   // shattered -> drawn as debris chunks below
            // Place the model base at the barrel base on the floor (column-major TRS).
            const float bx = b.center[0], by = b.center[1] - kHalfY, bz = b.center[2];
            float model[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, bx, by, bz, 1 };
            for (const auto& d : m_barrelDrawables) {
                float fin[16];
                x3::asset::mulMat4(model, d.nodeTransform, fin);
                // Use the model's own PBR color (modulated lightly toward the rusty tint).
                float col[4] = { d.baseColorFactor[0], d.baseColorFactor[1],
                                 d.baseColorFactor[2], d.baseColorFactor[3] };
                m_device->drawMesh(frame, x3::rhi::MeshHandle{ d.meshId },
                                   x3::rhi::TextureHandle{ d.baseColorTexId }, col, fin);
            }
        }
        // Debris shrapnel from any broken barrel (only the scattered pieces):
        // charred dark metal with a cooling ember glow.
        m_destr.forEachActiveChunk([&](const x3::phys::ChunkView& v) {
            if (v.intact) return;   // intact barrels already drawn as the GLB above
            float m[16]; std::memcpy(m, v.xform, sizeof(m));
            const float sx = v.halfExtents[0]*2.0f, sy = v.halfExtents[1]*2.0f, sz = v.halfExtents[2]*2.0f;
            m[0]*=sx; m[1]*=sx; m[2]*=sx;  m[4]*=sy; m[5]*=sy; m[6]*=sy;  m[8]*=sz; m[9]*=sz; m[10]*=sz;
            float col[4]; debrisTint(v.owner, col);
            m_device->drawMesh(frame, m_cube, m_charTex, col, m);
        });
        return;
    }

    // Cube fallback (Barrel.glb absent): the original chunk-cube render for all chunks.
    m_destr.forEachActiveChunk([&](const x3::phys::ChunkView& v) {
        float m[16]; std::memcpy(m, v.xform, sizeof(m));
        const float sx = v.halfExtents[0]*2.0f, sy = v.halfExtents[1]*2.0f, sz = v.halfExtents[2]*2.0f;
        m[0]*=sx; m[1]*=sx; m[2]*=sx;  m[4]*=sy; m[5]*=sy; m[6]*=sy;  m[8]*=sz; m[9]*=sz; m[10]*=sz;
        if (v.intact) {
            m_device->drawMesh(frame, m_cube, m_tex, intact, m);
        } else {
            float col[4]; debrisTint(v.owner, col);
            m_device->drawMesh(frame, m_cube, m_charTex, col, m);
        }
    });
}

void BarrelSystem::shutdown() {
    m_destr.shutdown();
    if (m_device) {
        if (m_cube.valid())    m_device->destroyMesh(m_cube);
        if (m_tex.valid())     m_device->destroyTexture(m_tex);
        if (m_charTex.valid()) m_device->destroyTexture(m_charTex);
    }
    m_barrels.clear();
    m_device = nullptr; m_physics = nullptr;
}

uint32_t BarrelSystem::explodedCount() const {
    uint32_t n = 0; for (const Barrel& b : m_barrels) if (b.exploded) ++n; return n;
}
uint32_t BarrelSystem::activeDebris() const {
    uint32_t n = 0;
    m_destr.forEachActiveChunk([&](const x3::phys::ChunkView& v){ if (!v.intact) ++n; });
    return n;
}

// ===========================================================================
// Headless self-test (--test-barrels). B0-B3.
// ===========================================================================
namespace {
int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[barrels-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[barrels-test] FAIL ") + name); }
}
constexpr float kDt = 1.0f / 60.0f;
using HeadlessDevice = x3::game::HeadlessRenderDevice;
}

bool runBarrelSelfTest() {
    g_pass = g_fail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
    w->init();
    HeadlessDevice device;

    // Damage sink counter.
    int blasts = 0; float lastCenter[3] = {0,0,0};
    BarrelSystem barrels;
    barrels.setDamageSink([&](const float c[3], float, int){ ++blasts; lastCenter[0]=c[0]; lastCenter[1]=c[1]; lastCenter[2]=c[2]; });
    barrels.init(device, *w);

    // A static floor so debris doesn't fall through infinity.
    w->addBox(x3::phys::Vec3{ 80.0f, 0.5f, 80.0f }, x3::phys::Vec3{ 0, -0.5f, 0 },
              0.0f, x3::phys::Layer::Static);
    // A cluster of 3 barrels within the blast/chain radius (~4.5 m), spaced 2 m,
    // plus 1 far barrel well outside it (and any shrapnel reach in the test window).
    barrels.spawn(0.0f, 0.0f, 0.0f);   // 0
    barrels.spawn(2.0f, 0.0f, 0.0f);   // 1
    barrels.spawn(4.0f, 0.0f, 0.0f);   // 2
    barrels.spawn(70.0f, 0.0f, 0.0f);  // 3 — far away
    w->optimizeBroadphase();
    check(barrels.count() == 4, "B0 spawned 4 barrels");

    uint32_t peakDebris = 0;
    auto stepN = [&](int n){ for (int i=0;i<n;++i){ barrels.update(kDt); w->step(kDt); peakDebris = std::max(peakDebris, barrels.activeDebris()); } };

    // ---- B1: shoot the first barrel -> it detonates (+ damage sink fires). ----
    float eye[3] = { 0.0f, 0.6f, -3.0f }, dir[3] = { 0, 0, 1 };  // aim +Z at barrel 0
    bool hit = barrels.onShot(eye, dir);
    stepN(15);
    check(hit && barrels.explodedCount() >= 1 && blasts >= 1,
          "B1 shooting a barrel detonates it (+ damage sink fired)");

    // ---- B2: the radius-based blast CHAINED through the cluster (all 3 go). ----
    stepN(20);   // short window: the radial chain is immediate; shrapnel can't reach 70 m yet
    check(barrels.explodedCount() >= 3, "B2 explosion chained through the cluster");

    // ---- B3: the FAR barrel (70 m) did NOT chain; the blasts threw debris. --
    bool farSafe = barrels.explodedCount() == 3;
    check(farSafe && peakDebris > 0, "B3 far barrel survives; blasts threw debris");

    barrels.shutdown();
    w->shutdown();
    x3::logInfo(std::string("[barrels-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
