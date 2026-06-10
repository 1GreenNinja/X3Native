#pragma once
// EFLZ Act 1 "The Spire" — FLOOR 4.5: the Nexus Chamber / The Chorus (off-elevator
// boss). Game/slice code only — engine/ stays pure.
//
// CONTENT/LEVEL-SCRIPT ONLY. This module does NOT touch the renderer or any core
// engine system: it stages the EXISTING Wave-1 multi-pod boss machine (monster.* —
// MultiPodBoss + chorusConfig()) in a discrete arena and dispatches through the
// EXISTING trigger system (trigger.*). It mirrors SpireMidFloors' authoring/host
// shape exactly (build once / tick / draw / onFire / onTrigger / queries) so the
// host integrates it the same way.
//
// THE ENCOUNTER (canon, EFLZ_WORLD_STRUCTURE.md §3a + EFLZ_BESTIARY.md):
//   "The Collective / The Chorus" — FIVE scientists fused into one cyber-organism,
//   each personality fighting for control, staged as FIVE independently-damageable
//   PODS (Subject Zero/Maya at the core + Harmon/Patel/Vasquez/Klein). The fight is
//   a "save-the-voices" morality quest: each outer voice may be SPARED (freed) rather
//   than killed — SAVE UP TO 4 (Subject Zero/Maya is the un-sparable core that always
//   remains). The boss FALLS once all 5 pods are DOWNED (killed OR saved). This is the
//   exact instance MultiPodBoss::chorusConfig() encodes (5 pods, fallThreshold = all,
//   maxSaved = 4), used verbatim here.
//
// FLOOR 4.5 — OFF THE ELEVATOR SPINE, FOUND LATER (the crux Tim flagged):
//   The Nexus Chamber is a HALF-STEP "Floor 4.5" the elevator does NOT stop at. It
//   sits between the F4 and F5 plates in world space (world Y midway: F4 y0=20, F5
//   y0=25 -> Nexus at ~22.5), but OFF the central elevator spine (the spine runs at
//   x=21,z=0 with one stop per numbered floor; the Nexus arena is placed clear of it
//   in -Z, beyond the numbered plates' footprint). Reachability is via an F4->F5
//   CONNECTOR trigger ("found later") rather than a numbered elevator stop, and the
//   encounter is NOT ARMED AT LOAD: the connector trigger (default DISABLED in the
//   host's TriggerSystem) "discovers" the Nexus; only on dispatch (onTrigger) does
//   the Chorus arm and begin updating/attacking. This mirrors how SpireMidFloors'
//   F5 hub gates the rescue clock (never armed at load) and how mid-floor hubs
//   dispatch through the host's single TriggerSystem::update() loop.

#include "scene.h"
#include "monster.h"     // MultiPodBoss + chorusConfig() (the Wave-1 API we USE)
#include "trigger.h"
#include "player.h"       // IDamageSink (the boss attacks the player)
#include "rescue.h"       // kRescueReach (the interact reach for sparing a pod)
#include "level1.h"       // Level1Layout + L1Floor (floor base Y for the half-step)

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace x3::game {

// Nexus trigger event ids. Kept in their OWN numeric band (60+) so this system can
// share one TriggerSystem with L1Trigger / SpireMidTrigger (30/40/50) /
// SpireTopTrigger without colliding. The Connector is the F4->F5 path discovery
// that "finds" the Nexus and arms the Chorus; it is added DISABLED at load.
enum class NexusTrigger : uint32_t {
    Connector = 60,   // F4->F5 connector reached -> DISCOVER the Nexus + arm the Chorus
};

// The authored Nexus encounter summary (read by the host HUD + the self-test to
// assert placement / gating without re-deriving it).
struct NexusPlan {
    x3::phys::Vec3 arena{};            // arena floor center (off the elevator spine)
    float          baseY      = 0.0f;  // arena walkable Y (the "Floor 4.5" half-step)
    float          f4BaseY    = 0.0f;  // F4 plate base Y (below the half-step)
    float          f5BaseY    = 0.0f;  // F5 plate base Y (above the half-step)
    uint32_t       podCount   = 0;     // number of Chorus pods (5)
    uint32_t       maxSaved   = 0;     // morality budget: pods sparable (save up to 4)
    uint32_t       fallThresh = 0;     // pods that must be DOWNED for the boss to fall
    bool           isElevatorStop = false;  // ALWAYS false (off-elevator half-step)
};

// FLOOR 4.5 Nexus Chamber authoring system. Build once after the floors (mirrors
// SpireMidFloors' style): build() once, tick() each frame, draw helpers, onFire/
// onTrigger fold-ins, and plan/query accessors. The Chorus is GATED — it does not
// update/attack until the connector trigger discovers the Nexus (armed()).
class SpireNexus {
public:
    // Stage the Chorus multi-pod boss in the Nexus Chamber arena. Places the arena
    // OFF the elevator spine at the half-step Y between the F4 and F5 plates (from
    // `layout.floorBaseY`). Registers the F4->F5 CONNECTOR trigger DISABLED (the
    // encounter is not armed at load) in the shared `triggers` system; the host
    // dispatches its fired id back to onTrigger(). `modelDir` is the loose rigged-GLB
    // dir (same one the floors receive); pods fall back to the tinted box if absent.
    // Call once.
    void build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, const Level1Layout& layout,
               TriggerSystem& triggers, std::string_view modelDir);

    // Advance one frame. NO-OP until the Nexus is armed (the connector was reached):
    // the Chorus pods do not chase/attack/phase before discovery. Once armed, ticks
    // every live pod (movement + attacks against `player`) like SpireMidFloors::tick.
    // `attackFx` (optional) spawns the per-attack beam FX. `player` may be null
    // (geometry/headless movement only). The host calls this once per frame.
    void tick(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
              const x3::phys::Vec3& playerPos, IDamageSink* player,
              const AttackFxFn& attackFx);

    // Dispatch a fired NexusTrigger id (the host forwards ids it doesn't own from its
    // TriggerSystem::update() loop). The Connector ARMS the Chorus (discovers the
    // Nexus). Idempotent.
    void onTrigger(uint32_t triggerId);

    // INTERACT (E in range): SPARE the nearest live Chorus pod within `range` instead
    // of killing it — the "save up to 4" morality path. A spared pod increments
    // savedCount (NOT killedCount) and is removed from the fight; the per-config
    // maxSaved cap (4) is enforced by MultiPodBoss::sparePod (the core/Subject Zero
    // cannot be saved once the budget is spent). No-op (returns false) before the
    // Nexus is armed, out of range, when the cap is reached, or with no live pod near.
    // Returns true iff a pod was spared this call (the host logs / SFX). Takes the
    // scene + physics because sparing removes the pod's body + hides its model (the
    // boss machine owns that), exactly as a non-lethal incapacitation.
    bool onInteract(const x3::phys::Vec3& playerPos, Scene& scene,
                    x3::phys::IPhysicsWorld& physics, float range = kRescueReach);

    // Fire one shot across the Chorus pods (the first live pod the ray hits takes it;
    // a pod that DIES this way counts as KILLED, not saved). The host folds this into
    // its onFire path so the weapon works on the Nexus too. No-op (default miss)
    // before the Nexus is armed. No arm-gate on the WEAPON here (the host owns the
    // WeaponSystem::hasWeapon() gate, as it does for the floors).
    FireResult onFire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                      Scene& scene, x3::phys::IPhysicsWorld& physics,
                      int damage = kDamagePerShot);

    // Draw the Chorus pods (host calls in its draw block). Pods are placed at load so
    // the arena reads even before discovery; they simply don't act until armed.
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
              const Scene& scene) const;

    // TASK#12: tear down any in-flight Chorus death ragdolls (Jolt bodies) while the
    // physics world is still alive. A killed RIGGED pod spawns a physics ragdoll whose
    // bodies are auto-cleared only when its corpse-pop times out in tick(); if a pod is
    // still mid-flop when the physics world is shut down/destroyed, the IRagdoll dtor
    // would touch a DEAD Jolt system (an access violation in teardown). The host (and
    // the self-test) must call this BEFORE shutting down the shared physics world.
    // Idempotent / no-op when no pod is ragdolling. Forwards to MultiPodBoss::shutdown.
    void shutdown() { m_chorus.shutdown(); }

    // ---- Queries (host HUD + the self-test) -------------------------------
    bool built() const { return m_built; }
    // The Chorus is ARMED once the connector trigger discovered the Nexus. False at
    // load (the encounter is gated, off-elevator, found later).
    bool armed() const { return m_armed; }
    // The authored plan (arena placement / pod count / morality budget / gating).
    const NexusPlan& plan() const { return m_plan; }

    // The multi-pod boss (read for pod-count / fall / save / kill assertions).
    const MultiPodBoss& chorus() const { return m_chorus; }
    MultiPodBoss&       chorus()       { return m_chorus; }

    // Convenience pass-throughs onto the boss (the morality-quest counts).
    uint32_t podCount()    const { return m_chorus.podCount(); }
    uint32_t savedCount()  const { return m_chorus.savedCount(); }
    uint32_t killedCount() const { return m_chorus.killedCount(); }
    uint32_t downedCount() const { return m_chorus.downedCount(); }
    uint32_t aliveCount()  const { return m_chorus.aliveCount(); }
    uint32_t fallThreshold() const { return m_chorus.fallThreshold(); }
    // The boss has FALLEN once enough pods are downed (killed OR saved).
    bool     hasFallen()   const { return m_chorus.hasFallen(); }

    // OFF-ELEVATOR assertion: the Nexus is a half-step Floor 4.5 the elevator does
    // NOT stop at. ALWAYS false — there is no numbered elevator stop for it, and its
    // arena is placed clear of the elevator spine. (Contrast SpireMidFloors, whose
    // floors ARE reachable via numbered elevator stops.)
    bool isElevatorStop() const { return false; }
    // True iff the arena origin is OFF the elevator spine (clear of the shaft XZ).
    // Read by the self-test to prove the placement is not on the spine.
    bool offElevatorSpine(const x3::phys::Vec3& shaftCenter, float shaftHalfXZ) const;

private:
    bool        m_built = false;
    bool        m_armed = false;          // discovered via the connector (NOT at load)
    std::string m_modelDir;

    NexusPlan    m_plan;
    MultiPodBoss m_chorus;                // the 5-pod Chorus (chorusConfig)

    TriggerSystem* m_triggers = nullptr;  // borrowed: the host's shared trigger system
};

// Headless self-test (--test-nexus). Builds the Spire (buildLevel1) + the Nexus on a
// HeadlessDevice + Jolt world and asserts:
//   * the 5-pod Chorus BUILDS (podCount == 5; chorusConfig wired through);
//   * the Nexus is NOT ARMED at load (gated on the connector trigger) — the Chorus
//     does NOT act before discovery, and the connector is registered DISABLED;
//   * the Nexus is OFF the elevator (NOT an elevator stop; its arena is clear of the
//     elevator spine) and sits at the half-step Y between the F4 and F5 plates;
//   * reaching the connector trigger ARMS the encounter;
//   * the boss does NOT FALL until the pod fall-threshold is met;
//   * SPARING pods increments savedCount (not killedCount) UP TO the cap (save up
//     to 4; the 5th spare is refused — the core/Subject Zero remains).
// Prints "nexus: X/Y passed"; returns true iff all pass. No window/Vulkan. Mirrors
// the other self-tests (runSpireMidSelfTest in particular).
bool runNexusSelfTest();

} // namespace x3::game
