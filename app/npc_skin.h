#pragma once
// SKINNED NAMED CITIZENS — the NpcLife (living-city) skinned visual layer.
// Tim's live report drove this: the scheduled, scannable, playable-as citizens
// drew as the shared blockout humanoid ("cube box people") while the ambient
// residents crowd already had rigged AnnaCasual skins via CrowdSkin.
//
// Same doctrine as CrowdSkin (app/crowd_skin.h), which stays untouched because
// it is typed to CrowdSystem/CrowdAgent: NpcLife stays the BRAIN (schedules,
// routing, robbery, hackables), NpcSkin is a pure VISUAL layer — one inert
// MonsterSystem character per walker agent, pose-following every frame, with
// the agent's own planar speed feeding the Idle/Walk/Run locomotion blend.
//
// ARCHETYPE RIGS: the paid Meshy characters land here — StreetCop wears
// assets/meshy/characters/street_cop_rigged.glb (walking/running/idle/talk/
// alert/phone clips), HotDogVendor wears hotdog_vendor_rigged.glb; every other
// archetype cycles the proven Blender roster (AnnaCasual_anim & co) scaled by
// its persona (Kid shrinks, etc.).
//
// FALLBACK: a missing/failed rig keeps that agent's blockout — never break the
// world. Freeway riders (in cars) are skipped outright. Spawns are deferred at
// spawnsPerFrame per update() (the CrowdSkin budget pattern).

#include "npc_life.h"
#include "monster.h"

#include <memory>
#include <string>
#include <vector>

namespace x3::game {

struct NpcSkinConfig {
    std::string site = "npc-life";       // log label
    std::string rosterDir;               // Blender roster root (default riggedGlbRoot())
    std::string meshyDir;                // Meshy characters root (default assets/meshy/characters)
    uint32_t    roomId = kNoRoom;        // PVS room (match the NpcLifeConfig)
    uint32_t    spawnsPerFrame = 1;      // deferred-spawn budget per update()
};

class NpcSkin {
public:
    // Plan the pool over a built NpcLife. NO loads — spawns drain in update().
    void build(const NpcSkinConfig& cfg, const NpcLife& life);

    // Per-frame, AFTER life.update(): drain spawns, pose-follow skinned agents.
    void update(float dt, const NpcLife& life, Scene& scene,
                x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics);

    // Draw the characters (room-gated like every character fan).
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
              const Scene& scene) const;

    // TIER-2 STREAMING (WP-4): region eviction (call BEFORE the region ledger
    // that owns this world's persistent walkScene changes anything). Mirrors
    // CrowdSkin::deactivate()'s contract (app/crowd_skin.h/.cpp) exactly, with
    // one deliberate difference: CrowdSkin never touches its agents' blockout
    // entities in deactivate() because those entities are REGION-LEDGER-OWNED
    // and get destroyed/recreated by the ledger itself on the next build(). Here
    // npcLife/npcSkin are Lane C — PERSISTENT, never torn down (see
    // TIER2_STREAMING_PLAN.md §2) — so nothing else will ever restore the
    // blockout's visibility; deactivate() must do it itself. So: hide every
    // skinned character entity, RESTORE the matching blockout entity's
    // visibility (recorded per-slot at spawn time — no NpcLife reference
    // needed), and reset attach/pose state. The pool (loaded MonsterSystem
    // rigs) is untouched and SURVIVES — the next build() + update() cycle
    // re-attaches the SAME characters with zero reloads (skinnedCount()/rig
    // choice/tint are unchanged; only the swap-and-pose step re-runs).
    void deactivate(Scene& scene);

    bool     active() const { return m_active; }
    uint32_t skinnedCount() const;
    bool     agentSkinned(uint32_t i) const {
        return i < m_slots.size() && m_slots[i].skinned;
    }
    const MonsterSystem* character(uint32_t i) const {
        return (i < m_slots.size()) ? m_slots[i].sys.get() : nullptr;
    }

private:
    struct Slot {
        std::unique_ptr<MonsterSystem> sys;
        bool  skinned  = false;
        bool  failed   = false;   // load failed / freeway rider — blockout keeps it
        bool  attached = false;   // swapped in over the CURRENT agent (mirrors CrowdSkin::Slot)
        bool  talking  = false;   // Talk calm-loop engaged (mid-conversation)
        float lastSpeed = 0.0f;
        x3::phys::Vec3 lastPos{};
        bool  hasLastPos = false;
        // The npc-life blockout entity this slot swapped out at spawn time.
        // Recorded here (not re-looked-up via NpcLife) so deactivate(Scene&) can
        // restore its visibility with ONLY a Scene reference, matching
        // CrowdSkin::deactivate()'s signature exactly.
        uint32_t blockoutEntity = kNoLink;
    };

    void spawnOne(uint32_t i, const NpcLife& life, Scene& scene,
                  x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics);
    // Swap: hide the blockout, show the character, snap the pose to the agent.
    // Split out of spawnOne so a re-build() after deactivate() can re-run just
    // this step over the ALREADY-spawned MonsterSystem (mirrors
    // CrowdSkin::attach()) — no reload, no re-creation.
    void attach(uint32_t i, const NpcLife& life, Scene& scene);

    NpcSkinConfig     m_cfg;
    std::vector<Slot> m_slots;
    uint32_t m_nextSpawn = 0;
    bool     m_active = false;
    double   m_totalSpawnMs = 0.0;
};

} // namespace x3::game
