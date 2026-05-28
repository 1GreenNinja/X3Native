// NPCSystem — non-combatant world NPCs. See app/npc.h.
//
// Clean-room: built from Scene / IRenderDevice / IPhysicsWorld / IModelLoader /
// IAssetSource only — the same interfaces MonsterSystem / RescueVictim use. No
// purchased C# / id Tech / RBDOOM source consulted.
#include "npc.h"
#include "mesh_prims.h"
#include "headless_device.h"
#include "asset_root.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace x3::game {

namespace {

// NPC collision box half-extents (a standing humanoid ~1.8 m tall). Matches
// rescue.cpp's kVictimHalf so the player blocks against an NPC the same way.
constexpr x3::phys::Vec3 kNpcHalf{ 0.4f, 0.9f, 0.4f };

// Patrol "arrived at waypoint" radius (m). Just under the kNpcHalf footprint so
// the NPC visibly reaches the waypoint without "drift".
constexpr float kPatrolArriveDist = 0.5f;

// Build a column-major 4x4 from a 3x3 basis (columns bx,by,bz), uniform scale s,
// and translation t. Identical to monster.cpp / rescue.cpp's composeTRS.
void composeTRS(float m[16],
                const x3::phys::Vec3& bx, const x3::phys::Vec3& by, const x3::phys::Vec3& bz,
                float s, const x3::phys::Vec3& t) {
    m[0]  = bx.x * s; m[1]  = bx.y * s; m[2]  = bx.z * s; m[3]  = 0.0f;
    m[4]  = by.x * s; m[5]  = by.y * s; m[6]  = by.z * s; m[7]  = 0.0f;
    m[8]  = bz.x * s; m[9]  = bz.y * s; m[10] = bz.z * s; m[11] = 0.0f;
    m[12] = t.x;      m[13] = t.y;      m[14] = t.z;      m[15] = 1.0f;
}

// Facing law (docs/CONVENTIONS.md / monster.cpp headingToFace + rescue.cpp): to
// point a model's local -Z along planar (dirX,dirZ), yaw = atan2(-dirX,-dirZ).
// NPCs face the player (dir = player - self) in LookAtPlayer / Interact; they
// face the next waypoint while patrolling (dir = waypoint - self).
float headingToFace(float dirX, float dirZ) {
    if (dirX * dirX + dirZ * dirZ < 1e-12f) return 0.0f;
    return std::atan2(-dirX, -dirZ);
}

} // namespace

// ===========================================================================
// Build
// ===========================================================================
void NPCSystem::build(Scene& scene, x3::rhi::IRenderDevice& dev,
                     x3::phys::IPhysicsWorld& phys, const x3::phys::Vec3& spawn,
                     const Tuning& t) {
    m_pos        = spawn;
    m_yaw        = 0.0f;
    m_baseYaw    = 0.0f;
    m_modelScale = t.modelScale > 0.0f ? t.modelScale : 1.0f;
    m_lookAtRange = t.lookAtRange;
    m_mode       = t.initialMode;
    // The "base mode" is what we restore to when the player leaves the look-at
    // range. Captive stays Captive (the rescue path flips it explicitly); the
    // others restore from the base.
    m_baseMode   = (t.initialMode == Mode::LookAtPlayer) ? Mode::Idle : t.initialMode;
    m_lookingAtPlayer = false;
    m_rescued    = false;
    m_alive      = true;
    m_built      = true;
    // Tint defaults to neutral white; Captive desaturates per draw (no need to
    // bake it here so a markRescued() flips render automatically).
    m_baseTint[0] = m_baseTint[1] = m_baseTint[2] = m_baseTint[3] = 1.0f;

    // Identity model-fixup; the optional Z-up stand-up is applied below.
    for (int i = 0; i < 16; ++i) m_modelFixup[i] = (i % 5 == 0) ? 1.0f : 0.0f;

    // ---- Load the GLB via a mounted loose-dir asset source (same path as
    // MonsterSystem::buildMonsterTuned / RescueVictim::build). modelDirOverride
    // takes precedence so the host can point at a non-default dir; empty falls
    // back to the rigged-GLB root the host implicitly used in the Tuning. ----
    const std::string useDir = t.modelDirOverride.empty() ? riggedGlbRoot()
                                                          : t.modelDirOverride;
    const std::string file   = t.modelFile;

    if (!file.empty()) {
        m_assets.reset(x3::asset::createAssetSource());
        bool mounted = m_assets->mountDir(useDir, 0);
        if (mounted) {
            m_loader.reset(x3::asset::createModelLoader(&dev, m_assets.get()));
            m_model = m_loader->load(file);
            if (m_model.ok)
                m_drawables = x3::asset::makeDrawables(m_model);
        } else {
            x3::logWarn("[npc] mountDir failed: " + useDir);
        }
    }

    if (!m_drawables.empty()) {
        m_usingReal = true;
        x3::logInfo("[npc] loaded " + file + " — " +
                    std::to_string(m_drawables.size()) + " primitive(s)");
    } else {
        // Fallback box so the NPC still exists in headless / clean-checkout.
        m_usingReal = false;
        if (!file.empty())
            x3::logWarn("[npc] " + file + " load failed; using fallback box");
        x3::prims::PrimMesh geo = x3::prims::makeBox(kNpcHalf.x, kNpcHalf.y,
                                                     kNpcHalf.z, 0.0f, 0.0f, 0.0f, 1.0f);
        x3::rhi::MeshHandle mesh = dev.createMesh(
            geo.verts.data(), (uint32_t)geo.verts.size(),
            geo.index.data(), (uint32_t)geo.index.size());
        x3::asset::ModelDrawable d;
        d.meshId = mesh.id;
        d.baseColorTexId = 0;                            // 0 -> default white
        // A neutral khaki so NPCs read distinct from the green monster box +
        // the cyan rescue victim box at a glance.
        d.baseColorFactor[0] = 0.78f; d.baseColorFactor[1] = 0.72f;
        d.baseColorFactor[2] = 0.55f; d.baseColorFactor[3] = 1.0f;
        m_drawables.push_back(d);
    }

    // ---- Optional Z-up stand-up fixup (mirrors MonsterSystem). The converted
    // character GLBs are authored lying flat (height along +Z); rotate -90deg
    // about X so they stand upright in our Y-up world. Rigged_glb humanoids
    // (BartenderDanny, DockWorker, ...) are already Y-up — leave identity. ----
    if (t.standUpZtoY) {
        m_modelFixup[0]=1; m_modelFixup[1]=0;  m_modelFixup[2]=0;
        m_modelFixup[4]=0; m_modelFixup[5]=0;  m_modelFixup[6]=-1;
        m_modelFixup[8]=0; m_modelFixup[9]=1;  m_modelFixup[10]=0;
    }

    // ---- Inert kinematic body: Enemy layer with mass 0 (static motion type
    // but movable by setBodyPosition — the same teleport trick MonsterSystem +
    // S4 door + RescueVictim companion-follow use). NPCs don't fall + don't get
    // pushed by combat, but they block player movement. ----
    m_body = phys.addBox(kNpcHalf, m_pos, 0.0f, x3::phys::Layer::Enemy);

    // ---- Tag::Prop entity: bookkeeping (tag/body/visibility/transform). Its
    // render mesh handle is left INVALID so Scene::render skips it; drawNPC()
    // owns the multi-primitive draw (mirrors MonsterSystem / RescueVictim). ----
    Entity e;
    e.tag     = (uint32_t)Tag::Prop;
    e.visible = true;
    e.body    = m_body;
    composeTRS(e.transform,
               x3::phys::Vec3{1, 0, 0}, x3::phys::Vec3{0, 1, 0}, x3::phys::Vec3{0, 0, 1},
               m_modelScale, m_pos);
    m_entity = scene.add(e);

    // Bake the initial visual yaw so the NPC reads correctly on the very first
    // frame even without an update() call (the screenshot path captures fast).
    bakeTransform(scene);

    x3::logInfo(std::string("[npc] entity ") + std::to_string(m_entity) +
                " placed at (" + std::to_string(spawn.x) + ", " +
                std::to_string(spawn.y) + ", " + std::to_string(spawn.z) + ")" +
                (m_usingReal ? " [real GLB]" : " [fallback box]") +
                " mode=" + std::to_string((int)m_mode));
}

// ===========================================================================
// Update
// ===========================================================================
void NPCSystem::update(float dt, Scene& scene, x3::phys::IPhysicsWorld& phys,
                      const x3::phys::Vec3& playerPos) {
    if (!m_built || !m_alive) return;

    // ---- 1) Look-at trigger (planar): if the player is within m_lookAtRange,
    // flip to LookAtPlayer; on leave restore the base mode. Captive does NOT
    // get auto-look-at (a captive should not pivot to track every passerby —
    // the host explicitly switches to Interact when the player presses E). ----
    const float dxp = playerPos.x - m_pos.x;
    const float dzp = playerPos.z - m_pos.z;
    const float dp2 = dxp * dxp + dzp * dzp;
    const float r2  = m_lookAtRange * m_lookAtRange;
    if (m_mode != Mode::Captive && m_mode != Mode::Interact) {
        const bool inRange = dp2 <= r2;
        if (inRange && !m_lookingAtPlayer) {
            // Enter LookAtPlayer; remember the base mode (it may already be the
            // Tuning's base, but if patrolling we want to restore Patrol).
            if (m_mode != Mode::LookAtPlayer) m_baseMode = m_mode;
            m_mode = Mode::LookAtPlayer;
            m_lookingAtPlayer = true;
        } else if (!inRange && m_lookingAtPlayer) {
            // Leave LookAtPlayer; restore the base mode.
            m_mode = m_baseMode;
            m_lookingAtPlayer = false;
        }
    }

    // ---- 2) Per-mode behaviour. -------------------------------------------
    if (m_mode == Mode::LookAtPlayer || m_mode == Mode::Interact) {
        // Face the player (no movement). Snap yaw — small turn radius is fine
        // for talking NPCs and the test asserts the heading post-update.
        m_yaw = headingToFace(dxp, dzp);
    } else if (m_mode == Mode::Patrol && m_patrol.size() >= 2) {
        // Walk toward the next waypoint at m_walkSpeed.
        const x3::phys::Vec3& wp = m_patrol[m_patrolNext];
        const float dwx = wp.x - m_pos.x;
        const float dwz = wp.z - m_pos.z;
        const float dist = std::sqrt(dwx * dwx + dwz * dwz);
        if (dist <= kPatrolArriveDist) {
            // Snap exactly to the waypoint and advance to the next (loop).
            m_pos.x = wp.x; m_pos.z = wp.z;
            m_patrolNext = (m_patrolNext + 1) % (uint32_t)m_patrol.size();
        } else {
            const float inv  = (dist > 1e-4f) ? 1.0f / dist : 0.0f;
            const float mx   = dwx * inv, mz = dwz * inv;
            const float step = m_walkSpeed * dt;
            // Don't overshoot past the waypoint.
            const float travel = std::min(step, dist);
            m_pos.x += mx * travel;
            m_pos.z += mz * travel;
            m_yaw = headingToFace(dwx, dwz);   // face the next waypoint while walking
        }
    } else {
        // Idle / Captive: stay put + hold the fixed pose.
        m_yaw = m_baseYaw;
    }

    // ---- 3) Sync the physics body + scene transform to the new pose. -------
    if (m_body.valid()) phys.setBodyPosition(m_body, m_pos);
    bakeTransform(scene);
}

// ===========================================================================
// Draw
// ===========================================================================
void NPCSystem::drawNPC(x3::rhi::IRenderDevice& dev, const x3::rhi::FrameContext& fr,
                       Scene& scene) {
    if (!m_built || !m_alive) return;
    if (m_entity == kNoLink || m_entity >= scene.size()) return;
    const Entity& e = scene.get(m_entity);
    if (!e.visible) return;

    // Captive mode desaturates the model (slightly cooler/dimmer) so a captive
    // reads distinct from a free NPC without an animation pass. Idle/Patrol/
    // LookAtPlayer/Interact use the neutral base tint.
    float tint[4] = { m_baseTint[0], m_baseTint[1], m_baseTint[2], m_baseTint[3] };
    if (m_mode == Mode::Captive && !m_rescued) {
        tint[0] = 0.65f; tint[1] = 0.70f; tint[2] = 0.78f; tint[3] = 1.0f;
    }
    drawAt(dev, fr, e.transform, tint);
}

void NPCSystem::drawAt(x3::rhi::IRenderDevice& dev, const x3::rhi::FrameContext& fr,
                      const float model[16], const float tint[4]) const {
    for (const auto& d : m_drawables) {
        float color[4] = {
            d.baseColorFactor[0] * tint[0],
            d.baseColorFactor[1] * tint[1],
            d.baseColorFactor[2] * tint[2],
            d.baseColorFactor[3] * tint[3],
        };
        // Compose: fin = model * fixup * nodeTransform. `model` is the gameplay
        // transform (yaw + scale + pos); `fixup` stands up Z-up character GLBs;
        // nodeTransform is the baked glTF node world matrix.
        float mf[16], fin[16];
        x3::asset::mulMat4(model, m_modelFixup, mf);
        x3::asset::mulMat4(mf, d.nodeTransform, fin);
        dev.drawMesh(fr,
                     x3::rhi::MeshHandle{ d.meshId },
                     x3::rhi::TextureHandle{ d.baseColorTexId },
                     color, fin);
    }
}

void NPCSystem::bakeTransform(Scene& scene) {
    if (m_entity == kNoLink || m_entity >= scene.size()) return;
    // FACING FIX (mirrors RescueVictim::bakeTransform / MonsterSystem facing):
    // the rigged character GLBs are authored facing +Z, but headingToFace() /
    // m_yaw assume local -Z forward (CONVENTIONS) — so the mesh renders with
    // its BACK to its target. Flip the VISUAL yaw 180deg here ONLY (m_yaw and
    // the look-at/patrol logic are unchanged, so the self-test stays correct).
    const float ry = m_yaw + 3.14159265358979323846f;
    const float c = std::cos(ry), s = std::sin(ry);
    Entity& me = scene.get(m_entity);
    composeTRS(me.transform,
               x3::phys::Vec3{ c, 0.0f, -s },
               x3::phys::Vec3{ 0.0f, 1.0f, 0.0f },
               x3::phys::Vec3{ s, 0.0f, c },
               m_modelScale, m_pos);
}

// ===========================================================================
// Authoring API
// ===========================================================================
void NPCSystem::setPatrol(std::initializer_list<x3::phys::Vec3> waypoints, float walkSpeed) {
    m_patrol.clear();
    m_patrol.reserve(waypoints.size());
    for (const auto& w : waypoints) m_patrol.push_back(w);
    if (m_patrol.size() < 2) {
        // Need at least two waypoints to walk a loop. Fall back to Idle.
        m_mode = Mode::Idle;
        m_baseMode = Mode::Idle;
        return;
    }
    m_walkSpeed = walkSpeed > 0.0f ? walkSpeed : 1.4f;
    m_patrolNext = 0;
    m_mode = Mode::Patrol;
    m_baseMode = Mode::Patrol;
    m_lookingAtPlayer = false;
}

void NPCSystem::setFixedPose(float yaw) {
    m_patrol.clear();
    m_patrolNext = 0;
    m_baseYaw = yaw;
    m_yaw     = yaw;
    m_mode    = Mode::Idle;
    m_baseMode = Mode::Idle;
    m_lookingAtPlayer = false;
}

void NPCSystem::setLookAtRange(float r) {
    m_lookAtRange = r > 0.0f ? r : 0.0f;
}

void NPCSystem::setOnInteract(InteractFn cb) {
    m_onInteract = std::move(cb);
}

bool NPCSystem::triggerInteract() {
    if (!m_onInteract) return false;
    m_onInteract(*this);
    return true;
}

void NPCSystem::setMode(Mode m) {
    m_mode = m;
    if (m != Mode::LookAtPlayer && m != Mode::Interact) {
        m_baseMode = m;
        m_lookingAtPlayer = false;
    }
}

// ===========================================================================
// Captive / rescue lifecycle
// ===========================================================================
bool NPCSystem::isCaptive() const {
    return m_alive && m_mode == Mode::Captive && !m_rescued;
}

bool NPCSystem::markRescued() {
    if (!isCaptive()) return false;
    m_rescued = true;
    m_mode    = Mode::Idle;
    m_baseMode = Mode::Idle;
    m_lookingAtPlayer = false;
    x3::logInfo("[npc] entity " + std::to_string(m_entity) + " RESCUED (captive -> idle)");
    return true;
}

bool NPCSystem::wasRescued() const {
    return m_rescued;
}

// ===========================================================================
// Cleanup
// ===========================================================================
void NPCSystem::shutdownRagdoll(x3::phys::IPhysicsWorld& phys) {
    if (!m_built) return;
    if (m_body.valid()) {
        phys.removeBody(m_body);
        m_body = x3::phys::BodyId{};
    }
    // Hide the entity (scene is not passed; we cannot reach the entity slot to
    // hide it without a Scene&. The host typically calls scene.get(entity).visible
    // = false; alternatively, drawNPC() short-circuits on m_alive so a follow-up
    // draw is silently a no-op).
    m_alive = false;
    m_drawables.clear();
    if (m_loader && m_model.ok) {
        m_loader->unload(m_model);
        m_model = x3::asset::Model{};
    }
    m_loader.reset();
    m_assets.reset();
}

// ===========================================================================
// Headless self-test (--test-npc).
// ===========================================================================
namespace {
int g_pass = 0;
int g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[test-npc] PASS  ") + name); }
    else      { ++g_fail; x3::logError(std::string("[test-npc] FAIL  ") + name); }
}
} // namespace

bool runNpcSelfTest() {
    g_pass = g_fail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
    phys->init();
    HeadlessRenderDevice dev;
    Scene scene;

    // ---- T1: build() places the NPC; entity + body are registered. ----------
    NPCSystem npc;
    {
        NPCSystem::Tuning t;
        t.modelFile      = "BartenderDanny.glb";   // falls back to a box if absent
        t.modelDirOverride = riggedGlbRoot();
        t.lookAtRange    = 3.0f;
        t.initialMode    = NPCSystem::Mode::Idle;
        const x3::phys::Vec3 spawn{ 5.0f, 0.0f, 5.0f };
        npc.build(scene, dev, *phys, spawn, t);
        const x3::phys::Vec3 p = npc.pos();
        const bool placed = npc.alive() && npc.builtModel()
                            && npc.entity() != kNoLink
                            && npc.body().valid()
                            && std::fabs(p.x - spawn.x) < 1e-4f
                            && std::fabs(p.z - spawn.z) < 1e-4f
                            && npc.mode() == NPCSystem::Mode::Idle;
        check(placed, "T1 build() places NPC at spawn with body+entity");
    }

    // ---- T2: update() with no player nearby + no patrol leaves the NPC put. -
    {
        const x3::phys::Vec3 farAway{ 100.0f, 0.0f, 100.0f };   // way outside lookAtRange
        const x3::phys::Vec3 before = npc.pos();
        npc.update(1.0f / 60.0f, scene, *phys, farAway);
        const x3::phys::Vec3 after = npc.pos();
        const bool stable = std::fabs(after.x - before.x) < 1e-4f
                            && std::fabs(after.z - before.z) < 1e-4f
                            && npc.mode() == NPCSystem::Mode::Idle;
        check(stable, "T2 update() with player far away leaves Idle NPC stable");
    }

    // ---- T3: LookAtPlayer triggers on approach; yaw faces the player. -------
    // Bring the player inside lookAtRange and step a frame; mode should flip,
    // and the yaw should face the player (headingToFace law).
    {
        const x3::phys::Vec3 spawn = npc.pos();
        // Player 1.5 m to the -Z (in front along the +Z facing — but since we
        // measure delta from NPC to player as dir = player - npc, putting the
        // player at (npc.x, _, npc.z - 1.5) gives dirZ = -1.5; headingToFace =
        // atan2(0, 1.5) == 0. Use a non-trivial direction so the yaw changes.
        const x3::phys::Vec3 player{ spawn.x + 2.0f, 0.0f, spawn.z + 0.0f };
        npc.update(1.0f / 60.0f, scene, *phys, player);
        const float dx = player.x - spawn.x;
        const float dz = player.z - spawn.z;
        const float expectYaw = std::atan2(-dx, -dz);
        const float yawErr = std::fabs(npc.yaw() - expectYaw);
        const bool looking = npc.mode() == NPCSystem::Mode::LookAtPlayer
                             && yawErr < 1e-3f;
        check(looking, "T3 LookAtPlayer triggers + yaw faces player");

        // Now move the player far away again: mode must restore to Idle.
        npc.update(1.0f / 60.0f, scene, *phys,
                   x3::phys::Vec3{ 100.0f, 0.0f, 100.0f });
        const bool restored = npc.mode() == NPCSystem::Mode::Idle;
        check(restored, "T3b LookAtPlayer restores base mode (Idle) on leave");
    }

    // ---- T4: Patrol cycles waypoints. ---------------------------------------
    // Build a fresh NPC at the origin with a closed 4-waypoint patrol around a
    // square; step forward at high walk speed and assert it reaches each waypoint
    // and the loop wraps to index 0 again.
    {
        NPCSystem patroller;
        NPCSystem::Tuning t;
        t.modelFile = "";                                  // force fallback box (test stays headless)
        t.modelDirOverride = riggedGlbRoot();
        t.lookAtRange = 0.0f;                              // disable look-at for the patrol test
        t.initialMode = NPCSystem::Mode::Idle;
        const x3::phys::Vec3 spawn{ 20.0f, 0.0f, 20.0f };  // far from previous NPC
        patroller.build(scene, dev, *phys, spawn, t);

        // 4-waypoint loop (a square of side 2). walkSpeed = 8 m/s so the test
        // converges quickly even with a 60 Hz step.
        patroller.setPatrol({
            x3::phys::Vec3{ spawn.x + 0.0f, 0.0f, spawn.z + 2.0f },
            x3::phys::Vec3{ spawn.x + 2.0f, 0.0f, spawn.z + 2.0f },
            x3::phys::Vec3{ spawn.x + 2.0f, 0.0f, spawn.z + 0.0f },
            x3::phys::Vec3{ spawn.x + 0.0f, 0.0f, spawn.z + 0.0f },
        }, 8.0f);

        const bool startedPatrol = patroller.mode() == NPCSystem::Mode::Patrol
                                    && patroller.patrolWaypointCount() == 4
                                    && patroller.patrolNextIndex() == 0;

        // Step long enough to traverse the WHOLE loop (perimeter 8 m at 8 m/s =
        // 1 s; give 2 s to be safe). Player held far away so look-at can't fire.
        const float dt = 1.0f / 60.0f;
        const int steps = (int)(2.0f / dt);
        const x3::phys::Vec3 farPlayer{ 1000.0f, 0.0f, 1000.0f };
        uint32_t indicesSeen = 0;
        for (int i = 0; i < steps; ++i) {
            patroller.update(dt, scene, *phys, farPlayer);
            indicesSeen |= (1u << patroller.patrolNextIndex());
        }
        // All 4 indices should have been visited (bits 0..3 all set), and the
        // NPC should have returned to a position near waypoint[0] or be heading
        // toward it again (i.e. the loop wrapped).
        const bool allHit = (indicesSeen & 0xFu) == 0xFu;
        check(startedPatrol && allHit, "T4 Patrol cycles all 4 waypoints (closed loop)");
    }

    // ---- T5: Captive lifecycle + markRescued() ------------------------------
    {
        NPCSystem captive;
        NPCSystem::Tuning t;
        t.modelFile = "";
        t.modelDirOverride = riggedGlbRoot();
        t.lookAtRange = 0.0f;
        t.initialMode = NPCSystem::Mode::Captive;
        captive.build(scene, dev, *phys, x3::phys::Vec3{ -10.0f, 0.0f, -10.0f }, t);
        const bool startCaptive = captive.isCaptive()
                                  && !captive.wasRescued()
                                  && captive.mode() == NPCSystem::Mode::Captive;
        const bool rescued      = captive.markRescued();
        const bool postRescue   = !captive.isCaptive()
                                  && captive.wasRescued()
                                  && captive.mode() == NPCSystem::Mode::Idle;
        // Calling markRescued() again must be a no-op (returns false).
        const bool idempotent   = !captive.markRescued() && captive.wasRescued();
        check(startCaptive && rescued && postRescue && idempotent,
              "T5 Captive lifecycle: isCaptive -> markRescued -> wasRescued (idempotent)");
    }

    // ---- T6: shutdownRagdoll() cleans up; idempotent + update() no-op. ------
    {
        NPCSystem doomed;
        NPCSystem::Tuning t;
        t.modelFile = "";
        t.modelDirOverride = riggedGlbRoot();
        t.initialMode = NPCSystem::Mode::Idle;
        const x3::phys::Vec3 spawn{ -30.0f, 0.0f, -30.0f };
        doomed.build(scene, dev, *phys, spawn, t);
        const bool builtOk = doomed.alive() && doomed.body().valid();
        doomed.shutdownRagdoll(*phys);
        const bool gone1   = !doomed.alive() && !doomed.body().valid();
        // Idempotent: a second call must not crash.
        doomed.shutdownRagdoll(*phys);
        const bool gone2   = !doomed.alive() && !doomed.body().valid();
        // update() after shutdown must be a no-op (no crash, pos unchanged).
        const x3::phys::Vec3 before = doomed.pos();
        doomed.update(1.0f / 60.0f, scene, *phys, x3::phys::Vec3{0,0,0});
        const x3::phys::Vec3 after  = doomed.pos();
        const bool noUpdate = std::fabs(after.x - before.x) < 1e-6f
                              && std::fabs(after.z - before.z) < 1e-6f;
        check(builtOk && gone1 && gone2 && noUpdate,
              "T6 shutdownRagdoll() cleans up (idempotent + update no-op)");
    }

    phys->shutdown();

    const int total = g_pass + g_fail;
    x3::logInfo(std::string("[test-npc] ") + std::to_string(g_pass) + "/" +
                std::to_string(total) + " passed");
    return g_fail == 0;
}

} // namespace x3::game
