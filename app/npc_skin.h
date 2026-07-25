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
        bool  talking  = false;   // Talk calm-loop engaged (mid-conversation)
        float lastSpeed = 0.0f;
        x3::phys::Vec3 lastPos{};
        bool  hasLastPos = false;
    };

    void spawnOne(uint32_t i, const NpcLife& life, Scene& scene,
                  x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics);

    NpcSkinConfig     m_cfg;
    std::vector<Slot> m_slots;
    uint32_t m_nextSpawn = 0;
    bool     m_active = false;
    double   m_totalSpawnMs = 0.0;
};

} // namespace x3::game
