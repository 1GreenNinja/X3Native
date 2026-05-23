#pragma once
// EFLZ Act 1 "The Spire" — MID-FLOOR encounter content for F3 (Labs), F4 (Offices)
// and F5 (R&D / Synth bay). Game/slice code only — engine/ stays pure.
//
// CONTENT/LEVEL-SCRIPT ONLY. This module does NOT touch the renderer or any core
// engine system: it composes the EXISTING data-driven roster (monster.* —
// DominionTrooper/Verthani/Illuminated/BlueSynth), the rescue system (rescue.*),
// the door/keypad system (door.*) and the trigger system (trigger.*) onto the
// vertical Spire graybox stack already built by buildLevel1() (level1.*). The
// floor plates F3/F4/F5 already exist as geometry (footprints + base Y in the
// canonical floor table level1Rooms()); here we author the per-floor ENCOUNTERS:
//
//   * F3 Labs       — research wing: infected enemies (melee-led), a keypad door
//                     into the inner lab (lab keycode). Difficulty floor (entry).
//   * F4 Offices    — cubicle combat sprawl: occupation troopers + cover, a ranged
//                     elite added, a door-override (keypad) puzzle. Escalates F3.
//   * F5 Synth bay  — high-bay synth waves: BlueSynth-led ranged pressure + an
//                     Illuminated elite, plus ONE rescue victim (a captured lab
//                     tech) on a timer that activates ONLY when the floor's hub
//                     trigger is reached — never armed at load (mirrors the F2
//                     RescueSystem::activate() gating). If the timer expires the
//                     victim transforms into a mini-boss (spec §2 F5 row).
//
// Escalation by design (no unwinnable dogpiles): the counts climb F3(4) -> F4(5)
// -> F5(6) and the species mix shifts from melee toward ranged + elite, but every
// enemy pulls its stats/cooldowns from the SAME combat-balance bands the rest of
// the level uses (monster.h namespace combat), and the manager's per-frame melee
// attacker cap (combat::kMaxMeleeAttackers) still arbitrates so the player is
// never melted. Enemies are placed off the elevator-doorway spine so an arriving
// player isn't ambushed inside the shaft.
//
// REACHABILITY: the floors are reached by the existing central elevator, which the
// host (main.cpp) builds with one stop per floor from the layout's per-floor base
// Y (Lb.floorBaseY[fi] + cab half-height). This module exposes the F3..F5 stop
// indices + arrival positions so the progression wiring + the self-test can assert
// each floor is reachable via that elevator.

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

// Mid-floor trigger event ids (kept distinct from L1Trigger so the two systems can
// share one TriggerSystem without colliding). Each floor has a HUB trigger placed
// where an elevator rider arrives; stepping into the F5 hub starts the rescue clock
// (mirrors L1Trigger::Hub on F2). F3/F4 hubs are reserved for objective/encounter
// activation hooks (alarm beats) and keep the pattern uniform across the wing.
enum class SpireMidTrigger : uint32_t {
    F3Hub = 30,   // F3 Labs hub reached (encounter armed)
    F4Hub = 40,   // F4 Offices hub reached (encounter armed)
    F5Hub = 50,   // F5 Synth bay hub reached -> START the rescue timer (gated!)
};

// The mid floors this module authors, in elevator-stop order. Index maps to the
// L1Floor enum (F3=3, F4=4, F5=5) so the elevator stop index == (uint32_t)floor.
enum class SpireMidFloor : uint32_t { F3 = 0, F4 = 1, F5 = 2, Count = 3 };

// One floor's authored encounter summary (read by the host HUD + the self-test to
// assert placement counts/roles + reachability without re-deriving them).
struct SpireFloorPlan {
    L1Floor      floor      = L1Floor::F3;  // which Spire plate
    uint32_t     elevStop   = 3;            // elevator stop index (== (uint32_t)floor)
    float        baseY      = 0.0f;         // floor walkable Y (== layout floorBaseY[floor])
    x3::phys::Vec3 arrival{};               // where an elevator rider steps onto the plate
    uint32_t     meleeCount = 0;            // melee (Guard-archetype) enemies placed
    uint32_t     rangedCount= 0;            // ranged (Drone-archetype) enemies placed
    uint32_t     totalCount = 0;            // meleeCount + rangedCount
    int          doorCode   = 0;            // keypad code on this floor's locked door (0 = none)
    bool         hasVictim  = false;        // a rescue captive is present on this floor
};

// F3..F5 encounter authoring system. Build once after buildLevel1(); it places the
// enemies/doors/triggers/victim onto the existing plates and exposes queries for
// the host + the headless self-test. Mirrors Level1Game's sub-system style: build()
// once, tick() each frame, draw helpers, and plan/query accessors.
class SpireMidFloors {
public:
    // Author the F3/F4/F5 encounters onto the already-built Spire (buildLevel1's
    // Level1Layout). `triggers` is the host's shared TriggerSystem (the same one
    // Level1Game uses) so the floor hubs dispatch through the host's single update
    // loop; the host switches on the SpireMidTrigger ids and calls onTrigger() (or
    // the F5 hub directly activates the rescue clock here). `modelDir` is the loose
    // rigged-GLB dir (same one Level1Game::build receives). Call once.
    void build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, const Level1Layout& layout,
               TriggerSystem& triggers, std::string_view modelDir);

    // Advance one frame: the enemy groups (movement + attacks against `player`) and
    // the F5 rescue victim (timer/companion, gated on its hub). `attackFx` (optional)
    // spawns the per-attack beam FX, exactly like Level1Game::tick. `player` may be
    // null (geometry/headless movement only). The host calls this once per frame
    // after Level1Game::tick (they are independent — different enemy groups).
    void tick(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
              const x3::phys::Vec3& eye, const x3::phys::Vec3& playerPos,
              IDamageSink* player, const AttackFxFn& attackFx);

    // Dispatch a fired SpireMidTrigger id (the host forwards ids it doesn't own from
    // its TriggerSystem::update() loop). The F5 hub starts the rescue clock; F3/F4
    // hubs latch their floor-reached flag (objective/alarm hook). Idempotent.
    void onTrigger(uint32_t triggerId);

    // Interact (E in range): try to rescue the F5 captive. Returns true iff rescued.
    bool onRescue(const x3::phys::Vec3& playerPos, float range = kRescueReach);

    // Fire one shot across all mid-floor enemy groups (the first live enemy hit
    // takes it). The host folds this into its onFire path so the pistol works on the
    // mid floors too. Returns the result for FX/HUD. No arm-gate here (the host
    // owns the WeaponSystem::hasWeapon() gate, as it does for Level1Game::onFire).
    FireResult onFire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                      Scene& scene, x3::phys::IPhysicsWorld& physics,
                      int damage = kDamagePerShot);

    // Draw all mid-floor enemies + the F5 victim/boss (host calls in its draw block).
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
              const Scene& scene) const;
    // Draw the mid-floor keypad door slabs at their current (animating) transforms.
    void drawDoors(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const {
        m_doors.drawMeshes(device, frame);
    }

    // Door access (host HUD / self-test): the keypad door system for the mid floors.
    DoorSystem&       doors()       { return m_doors; }
    const DoorSystem& doors() const { return m_doors; }
    // True if `playerPos` is within `range` of a LOCKED coded mid-floor door.
    bool nearLockedCodedDoor(const x3::phys::Vec3& playerPos, float range = 3.5f) const;
    // Submit a keypad `code`: unlock+open the nearest locked coded mid-floor door in
    // range whose code matches. Returns true if a door began opening.
    bool tryDoorCode(const x3::phys::Vec3& playerPos, int code, float range = 3.5f);

    // ---- Queries (host HUD + the self-test) -------------------------------
    bool built() const { return m_built; }
    // The authored plan for a mid floor (counts/roles/code/victim/reachability).
    const SpireFloorPlan& plan(SpireMidFloor f) const { return m_plan[(uint32_t)f]; }

    // Enemy managers per mid floor (read for placement-count/role assertions).
    const MonsterManager& enemies(SpireMidFloor f) const { return m_enemies[(uint32_t)f]; }
    MonsterManager&       enemies(SpireMidFloor f)       { return m_enemies[(uint32_t)f]; }

    // The F5 rescue victim (read to assert it is present but NOT active at load).
    bool   victimPresent() const { return m_victim != nullptr; }
    bool   victimCaptive() const;          // alive + still a captive (not rescued/expired)
    bool   victimTimerRunning() const { return m_f5HubReached; }   // clock gated on the F5 hub
    float  victimTimeLeft() const;

    // Reachability: a floor is reachable iff its elevator stop index is inside the
    // elevator's stop range (one stop per floor, 0..kSpireFloorCount-1). The host
    // builds exactly that many stops from the layout, so this is a content-side
    // assertion that the floors we authored line up with the progression.
    bool reachableViaElevator(SpireMidFloor f, uint32_t elevatorStopCount) const;

private:
    bool m_built = false;
    std::string m_modelDir;

    SpireFloorPlan m_plan[(uint32_t)SpireMidFloor::Count];
    MonsterManager m_enemies[(uint32_t)SpireMidFloor::Count];
    DoorSystem     m_doors;            // the per-floor keypad doors (F3/F4/F5)

    // F5 rescue captive (single victim, gated on the F5 hub). Owned here; drawn via
    // RescueVictim::draw. Transforms into a mini-boss on timer expiry (spec §2 F5).
    std::unique_ptr<RescueVictim> m_victim;
    MonsterManager m_victimBoss;       // the mini-boss spawned if the victim's timer expires
    x3::rhi::IRenderDevice* m_device = nullptr;  // cached for the on-expiry boss spawn

    // Floor-reached latches (F5 gates the rescue clock; F3/F4 are alarm hooks).
    bool m_f3HubReached = false;
    bool m_f4HubReached = false;
    bool m_f5HubReached = false;
};

// Headless self-test (--test-spiremid). Builds the Spire (buildLevel1) + the mid
// floors on a HeadlessDevice + Jolt world and asserts, per F3/F4/F5:
//   * the expected enemy COUNT and the melee/ranged ROLE split per floor;
//   * difficulty escalates (F3 < F4 < F5 total counts);
//   * the F5 rescue victim is PRESENT but its timer is NOT running at load (it only
//     starts once the F5 hub trigger fires) and it is still a captive;
//   * each floor is REACHABLE via the elevator (its stop index is within the stop
//     range the host builds, one per floor);
//   * the keypad door per floor is LOCKED with the authored code.
// Prints "spiremid: X/Y passed"; returns true iff all pass. No window/Vulkan.
bool runSpireMidSelfTest();

} // namespace x3::game
