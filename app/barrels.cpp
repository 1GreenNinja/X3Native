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
// Barrel ~0.88 dia x 1.23 tall (env_art kBarrelAabb). Fracture into a small grid so
// it shatters into a believable scatter of chunks (not too many — perf + reads well).
constexpr float kHalfX = 0.42f, kHalfY = 0.60f, kHalfZ = 0.42f;
constexpr float kChainDelay = 0.06f;   // s between a blast and the chained barrel going
} // namespace

void BarrelSystem::init(x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics) {
    m_device = &device; m_physics = &physics;

    std::vector<x3::rhi::MeshVertex> cv; std::vector<uint32_t> ci;
    x3::prims::makeCube(0.5f, cv, ci);
    m_cube = device.createMesh(cv.data(), (uint32_t)cv.size(), ci.data(), (uint32_t)ci.size());
    auto px = x3::prims::makeCheckerRGBA(64, 8, 200, 120, 40, 90, 50, 20);  // rusty orange barrel
    m_tex = device.createTexture(px.data(), 64, 64, true);

    x3::phys::DestructionTuning t; t.maxActiveChunks = 256;
    m_destr.init(&physics, t);

    std::vector<x3::phys::FractureChunkDesc> chunks;
    x3::phys::makeGridFractureChunks(kHalfX, kHalfY, kHalfZ, 2, 3, 2, /*chunkMass*/0.5f, chunks);
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
    b.exploded = true;
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
        if (!b.exploded && m_destr.isBroken(b.id))
            detonate(b);
    }
    (void)kChainDelay;
}

void BarrelSystem::render(const x3::rhi::FrameContext& frame) const {
    if (!m_device) return;
    const float intact[4] = { 0.85f, 0.50f, 0.18f, 1.0f };   // rusty orange tint
    // Scorched charred-METAL debris (playtest "barrels look like red boxes" fix):
    // its OWN warm orange-brown tint, deliberately decoupled from the global red
    // gore gibTint so blasted barrel chunks read as burnt steel, not gore-red boxes.
    const float debris[4] = { 0.46f, 0.27f, 0.11f, 1.0f };   // scorched charred steel

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
        // Debris chunks from any broken barrel (only the scattered, non-intact pieces).
        m_destr.forEachActiveChunk([&](const x3::phys::ChunkView& v) {
            if (v.intact) return;   // intact barrels already drawn as the GLB above
            float m[16]; std::memcpy(m, v.xform, sizeof(m));
            const float sx = v.halfExtents[0]*2.0f, sy = v.halfExtents[1]*2.0f, sz = v.halfExtents[2]*2.0f;
            m[0]*=sx; m[1]*=sx; m[2]*=sx;  m[4]*=sy; m[5]*=sy; m[6]*=sy;  m[8]*=sz; m[9]*=sz; m[10]*=sz;
            m_device->drawMesh(frame, m_cube, m_tex, debris, m);
        });
        return;
    }

    // Cube fallback (Barrel.glb absent): the original chunk-cube render for all chunks.
    m_destr.forEachActiveChunk([&](const x3::phys::ChunkView& v) {
        float m[16]; std::memcpy(m, v.xform, sizeof(m));
        const float sx = v.halfExtents[0]*2.0f, sy = v.halfExtents[1]*2.0f, sz = v.halfExtents[2]*2.0f;
        m[0]*=sx; m[1]*=sx; m[2]*=sx;  m[4]*=sy; m[5]*=sy; m[6]*=sy;  m[8]*=sz; m[9]*=sz; m[10]*=sz;
        m_device->drawMesh(frame, m_cube, m_tex, v.intact ? intact : debris, m);
    });
}

void BarrelSystem::shutdown() {
    m_destr.shutdown();
    if (m_device) {
        if (m_cube.valid()) m_device->destroyMesh(m_cube);
        if (m_tex.valid())  m_device->destroyTexture(m_tex);
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
