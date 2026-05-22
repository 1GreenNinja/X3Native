// F2 RESCUE SYSTEM (EFLZ Spire spec §5). See app/rescue.h.
//
// Clean-room: built from Scene/MonsterManager + the engine interfaces
// (IRenderDevice / IPhysicsWorld / IModelLoader / IAssetSource) only. No
// purchased C# / id Tech / RBDOOM source consulted. Mirrors monster.cpp.
#include "rescue.h"
#include "mesh_prims.h"
#include "headless_device.h"
#include "asset_root.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <memory>
#include <string>

namespace x3::game {

namespace {

// Victim collision box half-extents (a standing humanoid ~1.8 m tall).
constexpr x3::phys::Vec3 kVictimHalf{ 0.4f, 0.9f, 0.4f };

// Default live-character scale (rigged humanoid GLBs read ~1.8 m at scale 1).
constexpr float kVictimScale = 1.0f;
constexpr float kBoxScale    = 1.0f;

// Build a column-major 4x4 from a 3x3 basis (columns bx,by,bz), uniform scale s,
// and translation t. Identical to monster.cpp's composeTRS.
void composeTRS(float m[16],
                const x3::phys::Vec3& bx, const x3::phys::Vec3& by, const x3::phys::Vec3& bz,
                float s, const x3::phys::Vec3& t) {
    m[0]  = bx.x * s; m[1]  = bx.y * s; m[2]  = bx.z * s; m[3]  = 0.0f;
    m[4]  = by.x * s; m[5]  = by.y * s; m[6]  = by.z * s; m[7]  = 0.0f;
    m[8]  = bz.x * s; m[9]  = bz.y * s; m[10] = bz.z * s; m[11] = 0.0f;
    m[12] = t.x;      m[13] = t.y;      m[14] = t.z;      m[15] = 1.0f;
}

// Facing law (CONVENTIONS.md / monster.cpp headingToFace): to point a model's
// local -Z along planar (dirX,dirZ), yaw = atan2(-dirX,-dirZ). Companion faces the
// player (dir = player - self).
float headingToFace(float dirX, float dirZ) {
    if (dirX * dirX + dirZ * dirZ < 1e-12f) return 0.0f;
    return std::atan2(-dirX, -dirZ);
}

} // namespace

// ===========================================================================
// RescueVictim
// ===========================================================================
void RescueVictim::build(Scene& scene, x3::rhi::IRenderDevice& device,
                         x3::phys::IPhysicsWorld& physics,
                         std::string_view modelDir, const x3::phys::Vec3& pos,
                         VictimId id, std::string_view name, std::string_view liveModel,
                         float timer, const MonsterSystem::Tuning& bossTuning) {
    m_id        = id;
    m_name      = std::string(name);
    m_pos       = pos;
    m_state     = VictimState::Captive;
    m_timerMax  = timer;
    m_timeLeft  = timer;
    m_bossTuning= bossTuning;
    // A friendly cyan-ish tint so a captive/companion reads distinct from enemies.
    m_tint[0] = 0.7f; m_tint[1] = 0.9f; m_tint[2] = 1.0f; m_tint[3] = 1.0f;

    // ---- Load the live character GLB via a mounted loose-dir asset source (same
    // path as MonsterSystem::buildMonsterTuned). The rigged humanoids are Y-up, so
    // no Z->Y stand-up. On any failure a procedural box stands in. ----
    const std::string file = std::string(liveModel);
    m_assets.reset(x3::asset::createAssetSource());
    bool mounted = m_assets->mountDir(std::string(modelDir), 0);
    if (mounted) {
        m_loader.reset(x3::asset::createModelLoader(&device, m_assets.get()));
        m_model = m_loader->load(file);
        if (m_model.ok)
            m_drawables = x3::asset::makeDrawables(m_model);
    } else {
        x3::logWarn("[rescue] mountDir failed: " + std::string(modelDir));
    }

    for (int i = 0; i < 16; ++i) m_modelFixup[i] = (i % 5 == 0) ? 1.0f : 0.0f;

    if (!m_drawables.empty()) {
        m_usingReal  = true;
        m_modelScale = kVictimScale;
        x3::logInfo("[rescue] " + m_name + ": loaded " + file + " — " +
                    std::to_string(m_drawables.size()) + " primitive(s)");
    } else {
        // Fallback box so the victim still exists + is rescuable headlessly.
        m_usingReal  = false;
        m_modelScale = kBoxScale;
        x3::logWarn("[rescue] " + m_name + ": " + file + " load failed; using fallback box");
        x3::prims::PrimMesh geo = x3::prims::makeBox(kVictimHalf.x, kVictimHalf.y,
                                                     kVictimHalf.z, 0.0f, 0.0f, 0.0f, 1.0f);
        x3::rhi::MeshHandle mesh = device.createMesh(
            geo.verts.data(), (uint32_t)geo.verts.size(),
            geo.index.data(), (uint32_t)geo.index.size());
        x3::asset::ModelDrawable d;
        d.meshId = mesh.id;
        d.baseColorTexId = 0;
        d.baseColorFactor[0] = 0.6f; d.baseColorFactor[1] = 0.85f;
        d.baseColorFactor[2] = 1.0f; d.baseColorFactor[3] = 1.0f;  // friendly blue
        m_drawables.push_back(d);
    }

    // ---- Static-by-mass Enemy-layer box: lets the host (future) raycast it and
    // lets the companion move via setBodyPosition (same teleport trick the monster
    // chase + S4 door use). Mass 0 so it stays put in the ward. ----
    m_body = physics.addBox(kVictimHalf, m_pos, 0.0f, x3::phys::Layer::Enemy);

    // ---- Tag::Prop entity: bookkeeping only; render mesh left invalid so
    // Scene::render skips it and draw() owns the multi-primitive draw. ----
    Entity e;
    e.tag     = (uint32_t)Tag::Prop;
    e.visible = true;
    e.body    = m_body;
    composeTRS(e.transform,
               x3::phys::Vec3{1,0,0}, x3::phys::Vec3{0,1,0}, x3::phys::Vec3{0,0,1},
               m_modelScale, m_pos);
    m_entity = scene.add(e);

    x3::logInfo("[rescue] victim " + m_name + " (entity " + std::to_string(m_entity) +
                ") placed at (" + std::to_string(pos.x) + ", " + std::to_string(pos.y) +
                ", " + std::to_string(pos.z) + ") timer=" +
                std::to_string((int)timer) + "s" +
                (m_usingReal ? " [real GLB]" : " [fallback box]"));
}

void RescueVictim::bakeTransform(Scene& scene) {
    if (m_entity == kNoLink || m_entity >= scene.size()) return;
    const float c = std::cos(m_yaw), s = std::sin(m_yaw);
    Entity& me = scene.get(m_entity);
    composeTRS(me.transform,
               x3::phys::Vec3{ c, 0.0f, -s },
               x3::phys::Vec3{ 0.0f, 1.0f, 0.0f },
               x3::phys::Vec3{ s, 0.0f, c },
               m_modelScale, m_pos);
}

bool RescueVictim::tick(float dt, bool hubReached, Scene& scene,
                        x3::phys::IPhysicsWorld& physics,
                        const x3::phys::Vec3& playerPos) {
    if (m_state == VictimState::Expired) return false;

    // ---- Companion: light follow AI (mirror the monster chase movement). Hold a
    // standoff so it trails the player rather than crowding it; snap-catch up if
    // left way behind (e.g. after an elevator ride). ----
    if (m_state == VictimState::Companion) {
        const float dx = playerPos.x - m_pos.x, dz = playerPos.z - m_pos.z;
        const float horiz = std::sqrt(dx * dx + dz * dz);
        if (horiz > kCompanionTeleport) {
            // Lost too far behind — snap just behind the player.
            m_pos.x = playerPos.x; m_pos.z = playerPos.z;
        } else if (horiz > kCompanionStop) {
            const float inv = (horiz > 1e-4f) ? 1.0f / horiz : 0.0f;
            const float mx = dx * inv, mz = dz * inv;
            const float step = kCompanionSpeed * dt;
            // Don't overshoot past the standoff ring.
            const float travel = (horiz - kCompanionStop < step) ? (horiz - kCompanionStop) : step;
            m_pos.x += mx * travel; m_pos.z += mz * travel;
            m_yaw = headingToFace(dx, dz);   // face the player while following
        }
        if (m_body.valid()) physics.setBodyPosition(m_body, m_pos);
        bakeTransform(scene);
        return false;
    }

    // ---- Captive: run the countdown once the hub is reached. ----
    if (!hubReached) return false;
    if (m_timeLeft > 0.0f) {
        m_timeLeft -= dt;
        if (m_timeLeft <= 0.0f) {
            // ---- Timer expired: the captive transforms into a boss. Hide the
            // captive (its entity + body go away; the host spawns the boss via the
            // bossTuning getter so the new boss owns its own mesh/body). ----
            m_timeLeft = 0.0f;
            m_state = VictimState::Expired;
            if (m_entity != kNoLink && m_entity < scene.size()) {
                Entity& me = scene.get(m_entity);
                me.visible = false;
                me.body = x3::phys::BodyId{};
            }
            if (m_body.valid()) { physics.removeBody(m_body); m_body = x3::phys::BodyId{}; }
            x3::logInfo("[rescue] " + m_name + ": TIMER EXPIRED — transforming into a boss");
            return true;   // signal the host to spawn the boss this frame
        }
    }
    return false;
}

bool RescueVictim::tryRescue(const x3::phys::Vec3& playerPos, float reach) {
    if (m_state != VictimState::Captive) return false;
    const float dx = playerPos.x - m_pos.x, dz = playerPos.z - m_pos.z;
    const float horiz = std::sqrt(dx * dx + dz * dz);
    if (horiz > reach) return false;
    m_state = VictimState::Companion;
    m_timeLeft = m_timerMax;   // freeze the displayed time (timer stops on rescue)
    m_yaw = headingToFace(dx, dz);
    x3::logInfo("[rescue] " + m_name + ": RESCUED — now a friendly companion");
    return true;
}

void RescueVictim::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                        const Scene& scene) const {
    if (m_state == VictimState::Expired) return;     // the captive is gone (a boss now)
    if (m_entity == kNoLink || m_entity >= scene.size()) return;
    const Entity& e = scene.get(m_entity);
    if (!e.visible) return;
    drawAt(device, frame, e.transform);
}

void RescueVictim::drawAt(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                          const float model[16]) const {
    for (const auto& d : m_drawables) {
        float color[4] = {
            d.baseColorFactor[0] * m_tint[0],
            d.baseColorFactor[1] * m_tint[1],
            d.baseColorFactor[2] * m_tint[2],
            d.baseColorFactor[3] * m_tint[3],
        };
        float mf[16], fin[16];
        x3::asset::mulMat4(model, m_modelFixup, mf);
        x3::asset::mulMat4(mf, d.nodeTransform, fin);
        device.drawMesh(frame,
                        x3::rhi::MeshHandle{ d.meshId },
                        x3::rhi::TextureHandle{ d.baseColorTexId },
                        color, fin);
    }
}

// ===========================================================================
// RescueSystem
// ===========================================================================
void RescueSystem::build(Scene& scene, x3::rhi::IRenderDevice& device,
                         x3::phys::IPhysicsWorld& physics, std::string_view modelDir,
                         const x3::phys::Vec3& wardA, const x3::phys::Vec3& wardB,
                         const x3::phys::Vec3& wardC, float timer) {
    m_modelDir = std::string(modelDir);
    m_device   = &device;

    struct Spec { VictimId id; const char* name; const char* live; const char* boss;
                  float bossHp; x3::phys::Vec3 pos; };
    // Live victims map to Anna/character GLBs (spec §7); each transforms into its
    // boss on expiry: Aria->The Siren, Keisha->Breeder Queen, Emily->Oracle (§5).
    const Spec specs[3] = {
        { VictimId::Aria,   "Aria",   "AnnaCasual.glb",   "BossTheSiren.glb",     520.0f, wardA },
        { VictimId::Keisha, "Keisha", "AnnaBodySuit.glb", "BossBreederQueen.glb", 600.0f, wardB },
        { VictimId::Emily,  "Emily",  "AnnaTactical.glb", "Oracle.glb",           480.0f, wardC },
    };

    for (const Spec& sp : specs) {
        // Boss tuning the victim becomes on expiry — a Boss-type so it runs the same
        // phase machine as Martinez. Rigged sources are Y-up (standUpZtoY=false).
        MonsterSystem::Tuning bt;
        bt.type           = MonsterType::Boss;
        bt.hp             = (int)sp.bossHp;
        bt.chaseSpeed     = 3.2f;
        bt.damage         = 14;
        bt.attackRange    = 2.3f;
        bt.attackCooldown = 1.1f;
        bt.attackWindup   = 0.30f;
        bt.ranged         = false;
        bt.tint[0] = 1.0f; bt.tint[1] = 0.45f; bt.tint[2] = 0.60f; bt.tint[3] = 1.0f; // sinister magenta
        bt.modelFile        = sp.boss;
        bt.modelDirOverride = m_modelDir;
        bt.standUpZtoY      = false;     // rigged bosses authored Y-up
        bt.modelScale       = 1.4f;      // bosses read taller

        auto v = std::make_unique<RescueVictim>();
        v->build(scene, device, physics, m_modelDir, sp.pos,
                 sp.id, sp.name, sp.live, timer, bt);
        m_victims.push_back(std::move(v));
    }

    m_built = true;
    x3::logInfo("RescueSystem::build complete — 3 victims (Aria/Keisha/Emily) on " +
                std::to_string((int)timer) + "s timers (run once the F2 hub is reached)");
}

void RescueSystem::tick(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                        const x3::phys::Vec3& playerPos) {
    if (!m_built) return;

    for (auto& v : m_victims) {
        const bool expiredNow = v->tick(dt, m_hubReached, scene, physics, playerPos);
        if (expiredNow && m_device) {
            // Spawn the boss the victim transforms into, at the victim's ward spot.
            const x3::phys::Vec3 at{ v->pos().x, 0.4f, v->pos().z };
            m_bosses.spawn(scene, *m_device, physics, m_modelDir, at, v->bossTuning());
            x3::logInfo("[rescue] " + v->name() + " transformed — boss spawned (" +
                        std::to_string(m_bosses.count()) + " rescue boss(es) active)");
        }
    }

    // Advance the transformed-victim bosses (movement-only here; the host wires the
    // attack target/FX/phase callbacks when it folds these into the combat loop —
    // for now they chase, exactly like the legacy single-monster path).
    m_bosses.update(dt, scene, physics, playerPos);
}

bool RescueSystem::tryRescue(const x3::phys::Vec3& playerPos, float reach) {
    if (!m_built) return false;
    // Rescue the NEAREST in-range captive (so an E press near two wards is unambiguous).
    RescueVictim* best = nullptr;
    float bestD = reach * reach;
    for (auto& v : m_victims) {
        if (!v->captive()) continue;
        const float dx = playerPos.x - v->pos().x, dz = playerPos.z - v->pos().z;
        const float d2 = dx * dx + dz * dz;
        if (d2 <= bestD) { bestD = d2; best = v.get(); }
    }
    if (!best) return false;
    return best->tryRescue(playerPos, reach);
}

void RescueSystem::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                        const Scene& scene) const {
    for (const auto& v : m_victims) v->draw(device, frame, scene);
    m_bosses.drawAll(device, frame, scene);
}

std::vector<RescueTimerHud> RescueSystem::hudTimers() const {
    std::vector<RescueTimerHud> rows;
    for (const auto& v : m_victims) {
        if (!v->captive()) continue;          // only running countdowns show
        RescueTimerHud r;
        r.name    = v->name();
        r.seconds = v->timeLeft();
        r.urgent  = (v->timeLeft() < 60.0f);
        rows.push_back(std::move(r));
    }
    return rows;
}

uint32_t RescueSystem::rescuedCount() const {
    uint32_t n = 0;
    for (const auto& v : m_victims) if (v->companion()) ++n;
    return n;
}

uint32_t RescueSystem::expiredCount() const {
    uint32_t n = 0;
    for (const auto& v : m_victims) if (v->expired()) ++n;
    return n;
}

RescueSystem::SaveState RescueSystem::serialize() const {
    SaveState s;
    s.hubReached = m_hubReached;
    s.victims.reserve(m_victims.size());
    for (const auto& v : m_victims) s.victims.push_back(v->save());
    return s;
}

void RescueSystem::deserialize(const SaveState& s) {
    m_hubReached = s.hubReached;
    const size_t n = (s.victims.size() < m_victims.size()) ? s.victims.size() : m_victims.size();
    for (size_t i = 0; i < n; ++i) m_victims[i]->load(s.victims[i]);
}

// ===========================================================================
// Headless self-test (--test-rescue). No window / Vulkan.
// ===========================================================================
namespace {

int g_rpass = 0, g_rfail = 0;
void rcheck(bool cond, const char* name) {
    if (cond) { ++g_rpass; x3::logInfo(std::string("[rescue-test] PASS ") + name); }
    else      { ++g_rfail; x3::logError(std::string("[rescue-test] FAIL ") + name); }
}

// Headless IRenderDevice: the shared no-op test-double (app/headless_device.h).
// Mints valid handles so build() runs without Vulkan; draw/frame calls are
// no-ops.
using HeadlessDevice = x3::game::HeadlessRenderDevice;

} // namespace

bool runRescueSelfTest() {
    g_rpass = g_rfail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();

    HeadlessDevice device;
    Scene scene;
    RescueSystem rescue;

    // Three wards spread apart so range-tests are unambiguous.
    const x3::phys::Vec3 wA{  0.0f, 0.4f, 0.0f };
    const x3::phys::Vec3 wB{ 20.0f, 0.4f, 0.0f };
    const x3::phys::Vec3 wC{ 40.0f, 0.4f, 0.0f };
    // Short timer so the expiry test runs fast (the 5-min default is a build param).
    rescue.build(scene, device, *physics, riggedGlbRoot(), wA, wB, wC, /*timer*/2.0f);
    rcheck(rescue.victimCount() == 3, "R0 three victims built");

    // ---- R6 (playtest-fix): the timers are GATED — NO countdown / NO transform
    // before activate(). Right after build the hub is NOT reached (default off), so
    // ticking for well past the 2 s timer must NOT count any victim down or spawn a
    // boss. This is the bug we fixed: timers must not run from load. ----
    {
        const bool gatedOff = !rescue.hubReached() && !rescue.active();
        const float t0 = rescue.victim(0).timeLeft();
        const x3::phys::Vec3 farPlayer{ 999.0f, 0.4f, 999.0f };  // nowhere near a ward
        for (int i = 0; i < 240; ++i) rescue.tick(1.0f / 60.0f, scene, *physics, farPlayer); // 4 s
        const bool noCountdown = std::abs(rescue.victim(0).timeLeft() - t0) < 1e-3f;
        const bool noBoss      = rescue.bosses().count() == 0 && rescue.expiredCount() == 0;
        rcheck(gatedOff && noCountdown && noBoss,
               "R6 no countdown / no transform before activate() (timers gated at load)");
    }

    // Now reach the hub via the explicit activate() path: the timers start here.
    rescue.activate();
    rcheck(rescue.hubReached() && rescue.active(), "R7 activate() starts the rescue clocks");

    // R4a: cannot rescue an OUT-OF-RANGE captive (player far from ward A).
    {
        bool got = rescue.tryRescue(x3::phys::Vec3{ 0.0f, 0.4f, 20.0f }, kRescueReach);
        rcheck(!got, "R4 out-of-range rescue rejected");
    }

    // R1: rescuing an IN-RANGE captive (Aria) yields a companion.
    {
        bool got = rescue.tryRescue(x3::phys::Vec3{ wA.x + 1.0f, 0.4f, wA.z }, kRescueReach);
        rcheck(got && rescue.rescuedCount() == 1, "R1 in-range rescue -> companion");
    }

    // R2: the companion follows the player. Tick toward a far player; the companion
    // should close the XZ distance toward the standoff ring.
    {
        const RescueVictim& aria = rescue.victim(0);
        const x3::phys::Vec3 start = aria.pos();
        const x3::phys::Vec3 player{ wA.x + 15.0f, 0.4f, wA.z };
        for (int i = 0; i < 120; ++i) rescue.tick(1.0f / 60.0f, scene, *physics, player);
        const x3::phys::Vec3 end = aria.pos();
        const float d0 = std::abs(player.x - start.x);
        const float d1 = std::abs(player.x - end.x);
        rcheck(d1 < d0 - 1.0f, "R2 companion follows the player");
    }

    // R3: a captive whose timer expires transforms into a boss. Keisha (ward B) is
    // never rescued; tick past the 2 s timer with the player far away.
    {
        const uint32_t bossesBefore = rescue.bosses().count();
        const x3::phys::Vec3 player{ wA.x + 15.0f, 0.4f, wA.z };
        for (int i = 0; i < 240; ++i) rescue.tick(1.0f / 60.0f, scene, *physics, player); // 4 s
        const bool grew = rescue.bosses().count() > bossesBefore;
        const bool expired = rescue.expiredCount() >= 1;
        bool bossType = false;
        if (rescue.bosses().count() > 0)
            bossType = (rescue.bosses().at(0).type() == MonsterType::Boss);
        rcheck(grew && expired && bossType, "R3 expired victim transforms into a Boss");
    }

    // R4b: a now-EXPIRED victim cannot be rescued.
    {
        // Keisha is at ward B and now expired; standing on her spot must NOT rescue.
        bool got = rescue.tryRescue(x3::phys::Vec3{ wB.x, 0.4f, wB.z }, kRescueReach);
        rcheck(!got, "R4 expired victim not rescuable");
    }

    // R5: serialize/deserialize round-trips the lifecycle + timers + hub flag.
    {
        RescueSystem::SaveState snap = rescue.serialize();
        rcheck(snap.hubReached && snap.victims.size() == 3, "R5a serialize captured state");
        // Mutate Emily (still a captive) then restore from the snapshot.
        const float emilyBefore = rescue.victim(2).timeLeft();
        rescue.deserialize(snap);
        const float emilyAfter = rescue.victim(2).timeLeft();
        const bool ariaCompanion = rescue.victim(0).companion();   // restored companion
        const bool keishaExpired = rescue.victim(1).expired();     // restored expired
        rcheck(std::abs(emilyBefore - emilyAfter) < 1e-3f && ariaCompanion && keishaExpired,
               "R5 deserialize restores lifecycle + timers");
    }

    physics->shutdown();
    x3::logInfo("[rescue-test] " + std::to_string(g_rpass) + " passed, " +
                std::to_string(g_rfail) + " failed");
    return g_rfail == 0;
}

} // namespace x3::game
