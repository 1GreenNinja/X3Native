// SKINNED NAMED CITIZENS implementation — see app/npc_skin.h for the stance.

#include "npc_skin.h"
#include "asset_root.h"

#include "engine/core/x3_log.h"
#include "engine/physics/IPhysicsWorld.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace x3::game {

namespace {

constexpr float kMaxFedSpeed = 6.0f;   // pose-snap must never read as a sprint

// The Blender roster for archetypes without a bespoke Meshy rig. Same proven
// Idle/Walk/Run set as CrowdSkin::defaultRigs (AnnaCasual also carries Talk).
const char* kRoster[] = {
    "AnnaCasual_anim.glb",
    "marcus_webb_anim.glb",
    "chief_martinez_anim.glb",
};
constexpr uint32_t kRosterCount = (uint32_t)(sizeof(kRoster) / sizeof(kRoster[0]));

} // namespace

void NpcSkin::build(const NpcSkinConfig& cfg, const NpcLife& life) {
    if (!life.built()) return;
    if (m_slots.empty()) {
        m_cfg = cfg;
        if (m_cfg.rosterDir.empty()) m_cfg.rosterDir = riggedGlbRoot();
        if (m_cfg.meshyDir.empty())  m_cfg.meshyDir  = assetRoot() + "/meshy/characters";
        if (m_cfg.spawnsPerFrame == 0) m_cfg.spawnsPerFrame = 1;
        m_slots.resize(life.agentCount());
        m_nextSpawn = 0;
    }
    // TIER-2 STREAMING (WP-4) — (Re)attach contract, mirrors CrowdSkin::build()
    // exactly: every slot is marked "needs attach" so update()'s pose-follow
    // loop re-swaps it in. On the VERY FIRST build() this is a no-op (the slots
    // above were just default-constructed/resized, already false) — it only
    // does real work on the deactivate() -> build() re-build cycle, where it
    // flips already-skinned slots back so the SAME MonsterSystem pool
    // re-attaches with zero reloads.
    for (Slot& s : m_slots) {
        s.attached = false;
        s.hasLastPos = false;
        s.talking = false;
    }
    m_active = true;
}

void NpcSkin::spawnOne(uint32_t i, const NpcLife& life, Scene& scene,
                       x3::rhi::IRenderDevice& device,
                       x3::phys::IPhysicsWorld& physics) {
    Slot& s = m_slots[i];
    const NpcAgent& a = life.agent(i);
    if (a.onFreeway) { s.failed = true; return; }   // in a car — nothing to wear

    // Archetype -> rig. The paid Meshy characters carry their archetype's whole
    // clip set (walking/running/idle/talk/...); everyone else cycles the roster.
    std::string dir = m_cfg.rosterDir;
    std::string rig;
    switch (a.arch) {
    case Archetype::StreetCop:    dir = m_cfg.meshyDir; rig = "street_cop_rigged.glb";    break;
    case Archetype::HotDogVendor: dir = m_cfg.meshyDir; rig = "hotdog_vendor_rigged.glb"; break;
    default:                      rig = kRoster[(i + a.seed) % kRosterCount];             break;
    }

    const Persona& p = persona(a.arch);
    MonsterSystem::Tuning t;
    t.type       = MonsterType::Guard;
    t.chaseSpeed = 0.0f;               // INERT prop: NpcLife owns all motion
    t.damage     = 0;
    t.ranged     = false;
    t.noBody     = true;               // pure visual — no hitbox
    t.modelFile  = rig;
    t.modelDirOverride = dir;
    t.standUpZtoY = false;             // roster + Meshy rigs are Y-up
    t.modelScale  = p.scale;           // persona scale (Kid shrinks, cop 1.0, ...)
    // Wardrobe wash from the persona tint, softened toward white (CrowdSkin's
    // trick so rig repeats don't read as twins).
    for (int c = 0; c < 3; ++c) t.tint[c] = 1.0f - 0.35f * (1.0f - p.tint[c]);
    t.tint[3] = 1.0f;

    const auto t0 = std::chrono::steady_clock::now();
    s.sys = std::make_unique<MonsterSystem>();
    s.sys->buildMonsterTuned(scene, device, physics, dir, a.pos, t);
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    m_totalSpawnMs += ms;

    if (!s.sys->usingRealModel() || !s.sys->skinnable()) {
        const uint32_t ent = s.sys->entity();
        if (ent != kNoLink && ent < scene.size()) scene.get(ent).visible = false;
        s.sys.reset();
        s.failed = true;
        x3::logWarn("[npc-skin] " + m_cfg.site + ": agent " + std::to_string(i) +
                    " rig '" + rig + "' unavailable — blockout kept (" +
                    std::to_string(ms) + " ms)");
        return;
    }
    s.skinned = true;
    s.blockoutEntity = a.entity;   // recorded so deactivate() can restore it later
    attach(i, life, scene);        // swap: hide blockout, show character, snap pose
    x3::logInfo("[npc-skin] " + m_cfg.site + ": agent " + std::to_string(i) +
                " (" + archetypeName(a.arch) + ") -> " + rig + " (" +
                std::to_string(ms) + " ms, total " + std::to_string(m_totalSpawnMs) + " ms)");
}

void NpcSkin::attach(uint32_t i, const NpcLife& life, Scene& scene) {
    Slot& s = m_slots[i];
    const NpcAgent& a = life.agent(i);
    // Swap: hide the blockout body, show the character, stamp the room, snap the
    // pose. Uses s.blockoutEntity (recorded at spawn) rather than a.entity so
    // this also works correctly on the deactivate()->build() re-attach cycle,
    // matching CrowdSkin::attach()'s split of "create" vs "swap".
    if (s.blockoutEntity != kNoLink && s.blockoutEntity < scene.size())
        scene.get(s.blockoutEntity).visible = false;
    const uint32_t ent = s.sys->entity();
    if (ent != kNoLink && ent < scene.size()) {
        Entity& e = scene.get(ent);
        e.visible = true;
        e.roomId = m_cfg.roomId;
    }
    s.sys->setPropPose(a.pos, a.yaw);
    s.sys->setPropMotion(0.0f, 0.0f);
    s.hasLastPos = false;
    s.attached = true;
}

void NpcSkin::update(float dt, const NpcLife& life, Scene& scene,
                     x3::rhi::IRenderDevice& device,
                     x3::phys::IPhysicsWorld& physics) {
    if (!m_active || !life.built()) return;
    const uint32_t n = std::min((uint32_t)m_slots.size(), life.agentCount());

    uint32_t spawned = 0;
    while (m_nextSpawn < (uint32_t)m_slots.size() && spawned < m_cfg.spawnsPerFrame) {
        const uint32_t i = m_nextSpawn;
        Slot& s = m_slots[i];
        if (s.sys || s.failed || i >= n) { ++m_nextSpawn; continue; }
        spawnOne(i, life, scene, device, physics);
        ++m_nextSpawn;
        ++spawned;
        if (m_nextSpawn >= (uint32_t)m_slots.size())
            x3::logInfo("[npc-skin] " + m_cfg.site + ": pool complete — " +
                        std::to_string(skinnedCount()) + "/" + std::to_string(n) +
                        " skinned, " + std::to_string(m_totalSpawnMs) + " ms total");
    }

    if (!scene.roomVisible(m_cfg.roomId)) return;
    for (uint32_t i = 0; i < n; ++i) {
        Slot& s = m_slots[i];
        if (!s.skinned || !s.sys) continue;
        // TIER-2 STREAMING (WP-4): a slot coming back from deactivate() has
        // s.sys/s.skinned still set but s.attached was reset to false by
        // build() — re-run the swap over the SAME MonsterSystem (mirrors
        // CrowdSkin::update()'s `if (!s.attached) attach(...)`). No-op on the
        // normal first-spawn path (spawnOne() already attached it inline).
        if (!s.attached) attach(i, life, scene);
        const NpcAgent& a = life.agent(i);

        float speed = 0.0f;
        if (s.hasLastPos && dt > 1e-5f) {
            const float dx = a.pos.x - s.lastPos.x, dz = a.pos.z - s.lastPos.z;
            speed = std::min(std::sqrt(dx * dx + dz * dz) / dt, kMaxFedSpeed);
        }
        s.lastPos = a.pos;
        s.hasLastPos = true;
        s.lastSpeed = speed;

        s.sys->setPropPose(a.pos, a.yaw);
        s.sys->setPropMotion(speed, 0.0f);

        // Rigs with a Talk clip play it while stationary at leisure (street
        // conversations); movement always wins the locomotion blend back.
        const bool wantTalk = (a.activity == NpcActivity::AtLeisure) && speed < 0.15f;
        if (wantTalk && !s.talking) { s.sys->setCalmLoop("talk"); s.talking = true; }
        else if (!wantTalk && s.talking) { s.sys->clearCalmLoop(); s.talking = false; }

        s.sys->update(dt, scene, physics, s.sys->pos());
    }
}

void NpcSkin::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                   const Scene& scene) const {
    if (!m_active) return;
    if (!scene.roomVisible(m_cfg.roomId)) return;
    for (const Slot& s : m_slots)
        if (s.skinned && s.sys) s.sys->drawMonster(device, frame, scene);
}

void NpcSkin::deactivate(Scene& scene) {
    // TIER-2 STREAMING (WP-4): mirrors CrowdSkin::deactivate() with one
    // deliberate difference — see npc_skin.h's doc comment on this method.
    // npcLife/npcSkin are Lane C PERSISTENT (never torn down by any region
    // ledger — TIER2_STREAMING_PLAN.md §2), so unlike CrowdSkin's blockouts
    // (region-ledger-owned, destroyed/recreated for us), nothing else will ever
    // restore an npc-life blockout's visibility once this layer hides it. So:
    // hide every skinned character, explicitly restore its recorded blockout's
    // visibility, and reset attach/pose state. The pool (s.sys/s.skinned/
    // s.failed, i.e. every loaded MonsterSystem + its rig choice) is left
    // completely untouched — the next build()+update() cycle re-attaches the
    // SAME pool with zero reloads (the stream-cycle contract).
    for (Slot& s : m_slots) {
        if (s.sys) {
            const uint32_t ent = s.sys->entity();
            if (ent != kNoLink && ent < scene.size()) scene.get(ent).visible = false;
        }
        if (s.blockoutEntity != kNoLink && s.blockoutEntity < scene.size())
            scene.get(s.blockoutEntity).visible = true;
        s.attached = false;
        s.hasLastPos = false;
        s.talking = false;
    }
    m_active = false;
}

uint32_t NpcSkin::skinnedCount() const {
    uint32_t n = 0;
    for (const Slot& s : m_slots) if (s.skinned) ++n;
    return n;
}

} // namespace x3::game
