#pragma once
// Destruction demo world (Subsystem K, T1) — `--world destruct` /
// `--screenshot-destruct`. Game/slice code only; engine/ stays pure.
//
// A small self-contained showcase: a lit ground + a row of destructible crates the
// player can SHOOT (weapon raycast -> applyHit) or BLOW UP (explosion ->
// applyRadialImpulse). Each crate is one intact dynamic compound body that, on a
// break above threshold, shatters into convex chunks with split linear+angular
// velocity (the K-T1 fracture path). Chunks render directly from the
// DestructibleManager's live transforms (full rotation, so they visibly tumble) —
// NOT through Scene (which only syncs translation).
//
// Built ONLY through the public IRenderDevice + IPhysicsWorld + DestructibleManager
// interfaces. No id Tech / RBDOOM source. Low-conflict with Level 1 (own world).

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/physics/Destruction.h"
#include "mesh_prims.h"

#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace x3::game {

// One destructible demo world. Owns the fracture asset, the crate destructibles,
// a shared chunk cube mesh + a couple of textures, and a per-chunk render-mesh map
// (so a chunk body can be drawn at its tumbling transform).
class DestructDemo {
public:
    struct CrateInfo { x3::phys::DestructibleId id = 0; float center[3] = {0,0,0}; };

    // Build the world: ground (static), N crates spaced along +X, the fracture
    // asset (a 3x3x3 = 27-chunk crate), and the shared render meshes/textures.
    void build(x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
               int numCrates = 4) {
        m_device  = &device;
        m_physics = &physics;

        // --- Shared render resources ---
        std::vector<x3::rhi::MeshVertex> cv; std::vector<uint32_t> ci;
        x3::prims::makeCube(0.5f, cv, ci);            // unit cube, half-extent 0.5
        m_cube = device.createMesh(cv.data(), (uint32_t)cv.size(), ci.data(), (uint32_t)ci.size());

        auto crateTex = x3::prims::makeCheckerRGBA(64, 8, 196, 150, 96, 120, 84, 48); // crate wood-ish
        m_crateTex = device.createTexture(crateTex.data(), 64, 64, true);
        auto groundTex = x3::prims::makeCheckerRGBA(64, 8, 150, 150, 160, 60, 62, 74);
        m_groundTex = device.createTexture(groundTex.data(), 64, 64, true);

        // --- Ground (static collision + a render quad) ---
        x3::prims::PrimMesh g = x3::prims::makeBox(20.0f, 0.25f, 20.0f, 0.0f, -0.25f, 0.0f, 0.25f);
        m_groundMesh = device.createMesh(g.verts.data(), (uint32_t)g.verts.size(),
                                         g.index.data(), (uint32_t)g.index.size());
        physics.addStaticMesh(g.cverts.data(), (uint32_t)(g.cverts.size() / 3),
                              g.cindex.data(), (uint32_t)g.cindex.size());

        // --- Destruction manager + fracture asset ---
        x3::phys::DestructionTuning t;       // spec §15 defaults; cap is generous
        t.maxActiveChunks = 256;
        m_destr.init(&physics, t);
        // Render hooks: create/free a per-chunk mesh handle so debris draws. The
        // chunk geometry is the same shared cube scaled per-draw, so we just track
        // which bodies are live (handle == the shared cube). We DON'T allocate a new
        // GPU mesh per chunk — all chunks share m_cube, scaled by halfExtents.
        m_destr.setChunkCallbacks(
            [this](const x3::phys::ChunkView& v) { m_liveChunks[v.body.id] = true; },
            [this](x3::phys::BodyId b)           { m_liveChunks.erase(b.id); });

        std::vector<x3::phys::FractureChunkDesc> chunks;
        x3::phys::makeGridFractureChunks(0.5f, 0.5f, 0.5f, 3, 3, 3, /*chunkMass*/0.4f, chunks);
        x3::phys::FractureAssetDesc ad{};
        ad.chunks = chunks.data();
        ad.chunkCount = (uint32_t)chunks.size();
        ad.breakImpulse = 15.0f;             // spec §15
        ad.breakRelVel  = 8.0f;
        m_asset = m_destr.loadFractureAsset(ad);

        // --- Crates along +X, resting on the ground (center at y=0.5) ---
        for (int i = 0; i < numCrates; ++i) {
            float cx = -3.0f + i * 2.5f, cy = 0.5f, cz = 0.0f;
            float xf[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0,  cx,cy,cz,1};
            x3::phys::DestructibleId id = m_destr.spawnDestructible(m_asset, xf);
            m_crates.push_back({ id, { cx, cy, cz } });
        }
        physics.optimizeBroadphase();
    }

    // Advance: step the manager AFTER physics->step() (the host does the step).
    void update(float dt) { m_destr.update(dt); }

    // Fire a weapon ray from `eye` along `dir`: breaks the first destructible hit.
    bool fire(const float eye[3], const float dir[3], float strength = 60.0f) {
        return m_destr.applyHit(eye, dir, strength);
    }

    // Detonate an explosion at `center`.
    void explode(const float center[3], float radius = 4.0f, float strength = 40.0f) {
        m_destr.applyRadialImpulse(center, radius, strength);
    }

    // Draw the ground + every live destructible chunk (intact crates AND tumbling
    // debris) at their current transforms. Call between beginFrame/endFrame.
    void render(const x3::rhi::FrameContext& frame) const {
        if (!m_device) return;
        const float white[4] = { 1, 1, 1, 1 };
        // Ground.
        const float idG[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        m_device->drawMesh(frame, m_groundMesh, m_groundTex, white, idG);

        // Chunks: each ChunkView.xform is translate*rotate (no scale). Bake in a
        // per-chunk scale so the shared unit cube (half-extent 0.5) becomes the
        // chunk's box size (scale = halfExtents / 0.5 = halfExtents * 2).
        m_destr.forEachActiveChunk([&](const x3::phys::ChunkView& v) {
            float m[16];
            std::memcpy(m, v.xform, sizeof(m));
            const float sx = v.halfExtents[0] * 2.0f;
            const float sy = v.halfExtents[1] * 2.0f;
            const float sz = v.halfExtents[2] * 2.0f;
            // Column-major: scale the rotation columns 0,1,2 in place.
            m[0]*=sx; m[1]*=sx; m[2]*=sx;
            m[4]*=sy; m[5]*=sy; m[6]*=sy;
            m[8]*=sz; m[9]*=sz; m[10]*=sz;
            // Broken debris reads slightly warmer/darker so the shatter is obvious.
            const float intactCol[4] = { 1.0f, 0.95f, 0.85f, 1.0f };
            const float debrisCol[4] = { 0.85f, 0.55f, 0.40f, 1.0f };
            m_device->drawMesh(frame, m_cube, m_crateTex,
                               v.intact ? intactCol : debrisCol, m);
        });
    }

    void shutdown() {
        m_destr.shutdown();
        if (m_device) {
            if (m_cube.valid())       m_device->destroyMesh(m_cube);
            if (m_groundMesh.valid()) m_device->destroyMesh(m_groundMesh);
            if (m_crateTex.valid())   m_device->destroyTexture(m_crateTex);
            if (m_groundTex.valid())  m_device->destroyTexture(m_groundTex);
        }
        m_crates.clear();
        m_liveChunks.clear();
        m_device = nullptr; m_physics = nullptr;
    }

    const std::vector<CrateInfo>& crates() const { return m_crates; }
    x3::phys::DestructibleManager& manager() { return m_destr; }
    x3::phys::FractureAssetId fractureAsset() const { return m_asset; }
    // Spawn one more destructible crate at (x,y,z) using the shared fracture asset.
    x3::phys::DestructibleId spawnCrate(float x, float y, float z) {
        float xf[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, x, y, z, 1};
        return m_destr.spawnDestructible(m_asset, xf);
    }
    uint32_t activeDebris() const {
        uint32_t n = 0;
        m_destr.forEachActiveChunk([&](const x3::phys::ChunkView& v){ if (!v.intact) ++n; });
        return n;
    }

private:
    x3::rhi::IRenderDevice*    m_device  = nullptr;
    x3::phys::IPhysicsWorld*   m_physics = nullptr;
    x3::phys::DestructibleManager m_destr;
    x3::phys::FractureAssetId  m_asset = 0;

    x3::rhi::MeshHandle    m_cube;
    x3::rhi::MeshHandle    m_groundMesh;
    x3::rhi::TextureHandle m_crateTex;
    x3::rhi::TextureHandle m_groundTex;

    std::vector<CrateInfo> m_crates;
    std::unordered_map<uint32_t, bool> m_liveChunks;  // body.id -> live (debug/track)
};

} // namespace x3::game
