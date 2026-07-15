#pragma once
// SKINNED CITIZENS — the crowd's skinned visual layer (living-world pillar 2,
// "the crowds become real people"). Game/slice code only — engine/ stays pure.
//
// CrowdSystem (app/crowd.h) stays the BRAIN: positions, states, facing, and the
// procedural gestures (converse nod, work lean, hand-game bob) are computed
// there exactly as before, on the shared blockout humanoid entities. CrowdSkin
// is a pure VISUAL layer on top: one inert MonsterSystem character per agent
// (the PROVEN Club 1127 pipeline — damage 0 / chaseSpeed 0 skinned GLB props,
// GPU compute skinning with a transparent CPU fallback on headless devices),
// pose-following its agent every frame:
//   * position/yaw    -> setPropPose (agent pos + the crowd's bob, agent yaw);
//   * clip selection  -> setPropMotion(speed): the agent's OWN planar speed
//     feeds the Skinner's Idle/Walk/Run locomotion blend (walk threshold
//     0.2 m/s — every crowd walk speed clears it, so no sliding feet);
//   * gestures        -> the crowd's visLean/visCrouch ride on top as a small
//     torso pitch + a Y dip (setPropMotion lean / the pose Y), so the converse
//     nod / crate-carry lean / console lean-in / cower huddle still read;
//   * conversations   -> rigs that carry a Talk clip (AnnaCasual_anim) play it
//     via the calm-loop hook while the agent is mid-chat and stationary.
//
// FALLBACK (never break the world): a missing/failed rig keeps that agent's
// BLOCKOUT visible and discards the character — the monster fallback-box
// pattern, applied at the crowd layer. A skinned agent's blockout entity is
// hidden (visible=false), not removed, so the swap is reversible.
//
// BUDGET (per-instance skinned spawn is ~15-25+ ms):
//   * build() only PLANS — no GLB loads, no Scene::add. Safe to call inside a
//     WorldStreamer region-realize hook (nothing lands in the region ledger).
//   * update() drains the spawn queue at spawnsPerFrame (default 1/frame — the
//     canonPlay.tickUpperSpawns pattern) and logs each spawn's cost. Spawns
//     therefore happen OUTSIDE any region capture window.
//   * the pool is HOST-OWNED and PERSISTENT (the parked-cars doctrine: a shared
//     GLB mesh must never land in a region ledger): region eviction calls
//     deactivate() (hide the skins, forget the agents), and the next realize's
//     build() re-attaches the SAME loaded characters to the fresh agents —
//     zero reloads on a stream-out/in cycle.
//
// DRAW: draw() renders the characters through the same drawMonster PBR fan the
// club's dancers use, gated by the deployment's room id (facility rooms /
// kStreamedExteriorRoom) exactly like CanonPlay::drawManagerCulled.

#include "crowd.h"
#include "monster.h"

#include <memory>
#include <string>
#include <vector>

namespace x3::game {

// Configuration for one crowd deployment's skin layer.
struct CrowdSkinConfig {
    std::string site;                    // log label ("Main Hall", "Dock crew", ...)
    std::string modelDir;                // rigged-GLB root (riggedGlbRoot())
    // Rig roster cycled round-robin (seed-offset) across the agents. Empty =>
    // defaultRigs() (the proven upright humanoids carrying Idle+Walk+Run).
    std::vector<std::string> rigs;
    uint32_t seed  = 0;                  // per-deployment rig-cycle offset
    float    scale = 1.0f;               // character scale (crowd agents are 1.0)
    uint32_t spawnsPerFrame = 1;         // deferred-spawn budget per update()
};

class CrowdSkin {
public:
    // The proven walking-citizen roster: upright Y-up rigs with real Idle/Walk/
    // Run clips (AnnaCasual_anim also carries Talk for conversations).
    static const std::vector<std::string>& defaultRigs();

    // Plan the layer over `crowd` (which must be built): size the pool to the
    // agent count and mark active. NO loads, NO Scene::add — safe inside a
    // region-realize hook. Re-calling after deactivate() re-attaches the
    // existing pool to the (rebuilt) crowd with zero reloads.
    void build(const CrowdSkinConfig& cfg, const CrowdSystem& crowd);

    // Per-frame (call AFTER crowd.update(), OUTSIDE any region capture window):
    // drain the deferred spawn queue, then pose-follow every skinned agent.
    // Pose/anim work is skipped while the deployment's room is not visible
    // (the PVS gate) — the spawn queue still drains so the pool fills.
    void update(float dt, const CrowdSystem& crowd, Scene& scene,
                x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics);

    // Draw the skinned characters (room-gated). Call alongside the other
    // character draw fans in the live + screenshot paths.
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
              const Scene& scene) const;

    // A CITIZEN WAS SHOT — flop agent `i` with a skinned death ragdoll (the harvested
    // hit-react/death-flop from feat/npc-characters, wired onto crowd-skin's per-agent
    // MonsterSystem). `shove` is the shot direction (world). No-op unless the agent is
    // skinned + attached + not already ragdolling. Once ragdolled, pose-follow stops
    // for that agent (the ragdoll drives its skin) and the corpse settles/despawns on
    // the MonsterSystem's usual death timers. Returns true iff the flop spawned.
    bool triggerRagdoll(uint32_t i, Scene& scene, x3::phys::IPhysicsWorld& physics,
                        const x3::phys::Vec3& shove);
    // True iff agent `i` is currently ragdolling (host HUD / self-test).
    bool agentRagdolled(uint32_t i) const;

    // Region eviction (call from the teardown hook BEFORE the crowd abandons):
    // hide every skinned character and detach from the agents. The pool (loaded
    // rigs + bookkeeping entities) survives for the next build().
    void deactivate(Scene& scene);

    // ---- Queries (host HUD / self-test) ----
    bool     active() const { return m_active; }
    uint32_t agentCount() const { return (uint32_t)m_slots.size(); }
    uint32_t skinnedCount() const;
    uint32_t pendingCount() const;     // spawns not yet attempted
    bool     agentSkinned(uint32_t i) const;
    const MonsterSystem* character(uint32_t i) const;
    float    lastFedSpeed(uint32_t i) const;   // locomotion speed fed last frame
    double   totalSpawnMs() const { return m_totalSpawnMs; }

private:
    struct Slot {
        std::unique_ptr<MonsterSystem> sys;   // null until spawned (or failed)
        bool  skinned  = false;   // real rig bound + skinnable
        bool  failed   = false;   // load failed once — blockout keeps this agent
        bool  attached = false;   // swapped in over the CURRENT crowd's blockout
        bool  talking  = false;   // Talk calm-loop currently engaged
        bool  ragdolled = false;  // shot dead — the death ragdoll drives the skin now
        float lastSpeed = 0.0f;
        x3::phys::Vec3 lastPos{};
        bool  hasLastPos = false;
    };

    void spawnOne(uint32_t i, const CrowdSystem& crowd, Scene& scene,
                  x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics);
    void attach(uint32_t i, const CrowdSystem& crowd, Scene& scene);

    CrowdSkinConfig  m_cfg;
    std::vector<Slot> m_slots;
    uint32_t m_nextSpawn = 0;      // deferred-spawn cursor
    uint32_t m_roomId = kNoRoom;   // deployment PVS room (from the crowd config)
    bool     m_active = false;
    double   m_totalSpawnMs = 0.0;
};

// Headless self-test section for --test-crowd (called from runCrowdSelfTest):
// (S1) the skinned layer binds real rigs over a living crowd (headless device
// => the CPU-skin fallback path) and hides the blockouts; (S2) clip selection
// follows the crowd's own speed (moving agents feed >= the 0.2 m/s walk
// threshold, stationary agents feed idle) and the skin pose tracks the agent;
// (S3) a bogus rig path falls back to the blockout (nothing skinned, blockouts
// stay visible, no crash); (S4) long ticking leaks nothing; (S5) deactivate()
// hides the skins and a re-build() re-attaches the SAME pool with zero new
// mesh uploads (the stream-cycle contract). Returns true iff all pass.
bool runCrowdSkinSelfTest();

} // namespace x3::game
