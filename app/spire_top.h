#pragma once
// EFLZ Act 1 "The Spire" — TOP-FLOOR encounter content for F6 (Executive) and F7
// (Rooftop — the Act-1 summit / finale). Game/slice code only — engine/ stays pure.
//
// CONTENT/LEVEL-SCRIPT ONLY. This module does NOT touch the renderer or any core
// engine system: like spire_mid.* (the F3/F4/F5 mid-floor content it copies its
// authoring pattern from), it composes the EXISTING data-driven roster (monster.* —
// DominionTrooper/Verthani/Illuminated/BlueSynth + a Boss-type "Clone"), the rescue
// system (rescue.*), the door/keypad system (door.*) and the trigger system
// (trigger.*) onto the vertical Spire graybox stack already built by buildLevel1()
// (level1.*). The floor plates F6/F7 already exist as geometry (footprints + base Y
// in the canonical floor table level1Rooms()); here we author the per-floor
// ENCOUNTERS that close out Act 1 per docs/MASTER_GAME_PLAN.md:
//
//   * F6 Executive — the PENULTIMATE floor, the hardest STANDARD encounter of the
//                    Spire: an occupation strongpoint. 7 enemies escalating beyond
//                    F5's 6 — a mixed-species push (3 melee DominionTrooper/Verthani
//                    + 4 ranged BlueSynth/Illuminated) so the floor is dense without
//                    becoming an unwinnable melee dogpile (the melee cap still caps
//                    swingers). TWO keypad doors gate the exec suites (the master
//                    plan's "door-override puzzle"): an outer suite door + an inner
//                    vault door.
//   * F7 Rooftop   — the ACT-1 SUMMIT / FINALE (master plan L7 "Executive Laboratory
//                    -> boss: The Clone"). The climactic setpiece: a Boss-type "The
//                    Clone" (Jake's duplicate) anchoring the helipad, flanked by an
//                    Illuminated elite honor-guard pair (8 combatants total — boss +
//                    honor guard), PLUS the F7 rescue objective — Sarah, held in a
//                    rooftop holding cell, present-but-not-active-at-load and gated on
//                    the F7 hub (mirrors the F2 / F5 RescueSystem::activate() gating).
//                    Reaching the F7 hub starts her clock; rescuing her (E in range)
//                    makes her a companion (the canon "Sarah wakes, arms up, fights
//                    beside Jake" beat); if her timer expires she transforms into a
//                    mini-boss. A single keypad door gates the rooftop airlock.
//
// Escalation by design (no unwinnable dogpiles): the counts climb F5(6) -> F6(7) ->
// F7(8) and the species mix tilts toward ranged + elite + a Boss, but every enemy
// pulls its stats/cooldowns from the SAME combat-balance bands the rest of the level
// uses (monster.h namespace combat), and the manager's per-frame melee attacker cap
// (combat::kMaxMeleeAttackers) still arbitrates so the player is never melted.
// Enemies are placed off the elevator-doorway spine so an arriving player isn't
// ambushed inside the shaft.
//
// REACHABILITY: the floors are reached by the existing central elevator, which the
// host (main.cpp) builds with one stop per floor from the layout's per-floor base Y
// (Lb.floorBaseY[fi] + cab half-height). F7 is the elevator's TOP stop. This module
// exposes the F6/F7 stop indices + arrival positions so the progression wiring + the
// self-test can assert each floor is reachable via that elevator (top stop index ==
// floor index == kSpireFloorCount-1 for F7).

#include "scene.h"
#include "monster.h"
#include "rescue.h"
#include "trigger.h"
#include "door.h"
#include "level1.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace x3::game {

// Top-floor trigger event ids (kept distinct from L1Trigger AND SpireMidTrigger so
// all three systems can share one TriggerSystem without colliding). Each floor has a
// HUB trigger placed where an elevator rider arrives; stepping into the F7 hub starts
// Sarah's rescue clock (mirrors the F5 hub on the rescue captive). F6's hub is the
// encounter/alarm activation hook, uniform with F3/F4.
enum class SpireTopTrigger : uint32_t {
    F6Hub = 60,   // F6 Executive hub reached (encounter armed)
    F7Hub = 70,   // F7 Rooftop summit hub reached -> START Sarah's rescue clock (gated!)
};

// The top floors this module authors, in elevator-stop order. Index maps to the
// L1Floor enum (F6=6, F7=7) so the elevator stop index == (uint32_t)floor and F7's
// stop is the elevator's TOP stop (kSpireFloorCount-1).
enum class SpireTopFloor : uint32_t { F6 = 0, F7 = 1, Count = 2 };

// One floor's authored encounter summary (read by the host HUD + the self-test to
// assert placement counts/roles + reachability without re-deriving them). Mirrors
// SpireFloorPlan (spire_mid.h) but adds a 2nd door code slot for F6's two doors and
// a boss flag for F7's finale.
struct SpireTopPlan {
    L1Floor      floor      = L1Floor::F6;  // which Spire plate
    uint32_t     elevStop   = 6;            // elevator stop index (== (uint32_t)floor)
    float        baseY      = 0.0f;         // floor walkable Y (== layout floorBaseY[floor])
    x3::phys::Vec3 arrival{};               // where an elevator rider steps onto the plate
    uint32_t     meleeCount = 0;            // melee (Guard-archetype) enemies placed
    uint32_t     rangedCount= 0;            // ranged (Drone-archetype) enemies placed
    uint32_t     bossCount  = 0;            // Boss-archetype enemies placed (F7 finale)
    uint32_t     totalCount = 0;            // meleeCount + rangedCount + bossCount
    int          doorCode   = 0;            // keypad code on this floor's FIRST locked door (0 = none)
    int          doorCode2  = 0;            // keypad code on this floor's SECOND locked door (0 = none)
    bool         hasVictim  = false;        // a rescue captive is present on this floor (F7 Sarah)
    bool         hasBoss    = false;        // a Boss-type leader anchors this floor (F7 Clone)
};

// F6..F7 encounter authoring system. Build once after buildLevel1(); it places the
// enemies/doors/triggers/victim onto the existing plates and exposes queries for the
// host + the headless self-test. Mirrors SpireMidFloors' style exactly: build() once,
// tick() each frame, draw helpers, plan/query accessors.
class SpireTopFloors {
public:
    // Author the F6/F7 encounters onto the already-built Spire (buildLevel1's
    // Level1Layout). `triggers` is the host's shared TriggerSystem (the same one
    // Level1Game + SpireMidFloors use) so the floor hubs dispatch through the host's
    // single update loop; the host switches on the SpireTopTrigger ids and calls
    // onTrigger() (the F7 hub directly activates Sarah's rescue clock here). `modelDir`
    // is the loose rigged-GLB dir (same one SpireMidFloors::build receives). Call once.
    void build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, const Level1Layout& layout,
               TriggerSystem& triggers, std::string_view modelDir);

    // Advance one frame: the enemy groups (movement + attacks against `player`), the
    // F7 Clone boss, and the F7 rescue victim (timer/companion, gated on its hub).
    // `attackFx` (optional) spawns the per-attack beam FX, exactly like
    // SpireMidFloors::tick. `player` may be null (geometry/headless movement only).
    // The host calls this once per frame after SpireMidFloors::tick (independent —
    // different enemy groups).
    void tick(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
              const x3::phys::Vec3& eye, const x3::phys::Vec3& playerPos,
              IDamageSink* player, const AttackFxFn& attackFx);

    // Dispatch a fired SpireTopTrigger id (the host forwards ids it doesn't own from
    // its TriggerSystem::update() loop). The F7 hub starts Sarah's rescue clock; F6's
    // hub latches its floor-reached flag (objective/alarm hook). Idempotent.
    void onTrigger(uint32_t triggerId);

    // Interact (E in range): try to rescue the F7 captive (Sarah). Returns true iff
    // rescued.
    bool onRescue(const x3::phys::Vec3& playerPos, float range = kRescueReach);

    // Fire one shot across all top-floor enemy groups + the boss (the first live
    // enemy hit takes it). The host folds this into its onFire path so the pistol
    // works on the top floors too. Returns the result for FX/HUD. No arm-gate here
    // (the host owns the WeaponSystem::hasWeapon() gate).
    FireResult onFire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                      Scene& scene, x3::phys::IPhysicsWorld& physics);

    // Draw all top-floor enemies + the F7 boss + Sarah (host calls in its draw block).
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
              const Scene& scene) const;
    // Draw the top-floor keypad door slabs at their current (animating) transforms.
    void drawDoors(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const {
        m_doors.drawMeshes(device, frame);
    }

    // Door access (host HUD / self-test): the keypad door system for the top floors.
    DoorSystem&       doors()       { return m_doors; }
    const DoorSystem& doors() const { return m_doors; }
    // True if `playerPos` is within `range` of a LOCKED coded top-floor door.
    bool nearLockedCodedDoor(const x3::phys::Vec3& playerPos, float range = 3.5f) const;
    // Submit a keypad `code`: unlock+open the nearest locked coded top-floor door in
    // range whose code matches. Returns true if a door began opening.
    bool tryDoorCode(const x3::phys::Vec3& playerPos, int code, float range = 3.5f);

    // ---- Queries (host HUD + the self-test) -------------------------------
    bool built() const { return m_built; }
    // The authored plan for a top floor (counts/roles/codes/victim/boss/reachability).
    const SpireTopPlan& plan(SpireTopFloor f) const { return m_plan[(uint32_t)f]; }

    // Enemy managers per top floor (read for placement-count/role assertions). F7's
    // manager holds the honor-guard adds; the Clone boss is its own manager (boss()).
    const MonsterManager& enemies(SpireTopFloor f) const { return m_enemies[(uint32_t)f]; }
    MonsterManager&       enemies(SpireTopFloor f)       { return m_enemies[(uint32_t)f]; }

    // The F7 Clone boss manager (read to assert a Boss-type leader is present + alive).
    const MonsterManager& boss() const { return m_boss; }
    MonsterManager&       boss()       { return m_boss; }

    // The F7 rescue victim (read to assert it is present but NOT active at load).
    bool   victimPresent() const { return m_victim != nullptr; }
    bool   victimCaptive() const;          // alive + still a captive (not rescued/expired)
    bool   victimTimerRunning() const { return m_f7HubReached; }   // clock gated on the F7 hub
    float  victimTimeLeft() const;

    // Reachability: a floor is reachable iff its elevator stop index is inside the
    // elevator's stop range (one stop per floor, 0..kSpireFloorCount-1). The host
    // builds exactly that many stops from the layout, so this is a content-side
    // assertion that the floors we authored line up with the progression. F7's stop
    // is the TOP stop (== elevatorStopCount-1).
    bool reachableViaElevator(SpireTopFloor f, uint32_t elevatorStopCount) const;

private:
    bool m_built = false;
    std::string m_modelDir;

    SpireTopPlan   m_plan[(uint32_t)SpireTopFloor::Count];
    MonsterManager m_enemies[(uint32_t)SpireTopFloor::Count];
    MonsterManager m_boss;             // the F7 "Clone" Boss-type leader (its own group)
    DoorSystem     m_doors;            // the per-floor keypad doors (F6 x2, F7 x1)

    // F7 rescue captive (Sarah, gated on the F7 hub). Owned here; drawn via
    // RescueVictim::draw. Transforms into a mini-boss on timer expiry (canon: the
    // unsaved becomes a boss).
    std::unique_ptr<RescueVictim> m_victim;
    MonsterManager m_victimBoss;       // the mini-boss spawned if Sarah's timer expires
    x3::rhi::IRenderDevice* m_device = nullptr;  // cached for the on-expiry boss spawn

    // Floor-reached latches (F7 gates Sarah's rescue clock; F6 is an alarm hook).
    bool m_f6HubReached = false;
    bool m_f7HubReached = false;
};

// Headless self-test (--test-spiretop). Builds the Spire (buildLevel1) + the mid
// floors (spire_mid) + the top floors on a HeadlessDevice + Jolt world and asserts,
// per F6/F7:
//   * the expected enemy COUNT and the melee/ranged/boss ROLE split per floor;
//   * difficulty escalates F5 < F6 < F7 (across spire_mid + spire_top totals);
//   * the F7 finale carries a Boss-type leader (The Clone) alive at load;
//   * the F7 rescue victim (Sarah) is PRESENT but its timer is NOT running at load
//     (it only starts once the F7 hub trigger fires) and it is still a captive;
//   * each floor is REACHABLE via the elevator (its stop index is within the stop
//     range the host builds, one per floor) and F7 is the TOP stop;
//   * the keypad doors per floor are LOCKED with the authored codes (F6 has two).
// Prints "spiretop: X/Y passed"; returns true iff all pass. No window/Vulkan.
bool runSpireTopSelfTest();

} // namespace x3::game
