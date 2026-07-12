#pragma once
// EFLZ Act 1 "The Spire" — MID-FLOOR encounter content for F3 (Genetics Lab),
// F4 (Cybernetics Workshop) and F5 (Drone Manufacturing). Game/slice code only —
// engine/ stays pure.
//
// WAVE-2 RE-THEME (EFLZ_WORLD_STRUCTURE.md §2 + EFLZ_BESTIARY.md §2): the mid floors
// are brought to their canon Act-1 identities and given their designed bosses, on top
// of the EXISTING Wave-1 boss machine (monster.h: bossTuning(BossType) single-body
// bosses + ScriptedFightHook for Sarah's master hack). The multi-pod Chorus / the
// off-elevator F4.5 Nexus is a SEPARATE module (spire_nexus.*, another agent) — NOT
// placed here; this module only leaves a labeled F4 -> Floor 4.5 transition hook.
//
// CONTENT/LEVEL-SCRIPT ONLY. This module does NOT touch the renderer or any core
// engine system: it composes the EXISTING data-driven roster (monster.* —
// DominionTrooper/Verthani/Illuminated/BlueSynth + the Act-1 BossType roster), the
// rescue system (rescue.*), the door/keypad system (door.*) and the trigger system
// (trigger.*) onto the vertical Spire graybox stack already built by buildLevel1()
// (level1.*). The floor plates F3/F4/F5 already exist as geometry (footprints + base Y
// in the canonical floor table level1Rooms()); here we author the per-floor ENCOUNTERS:
//
//   * F3 Genetics Lab        — hybrid-horror research wing: infected/hybrid enemies
//                     (melee-led) + the floor BOSS **Failed Experiment #7 (Marcus
//                     Webb)**, the tragic 400% predecessor (3 phases, Memory-Flash
//                     vulnerability window — see bossTuning(BossType::FailedExperiment7)).
//                     A keypad door gates the inner spawning chamber (lab keycode).
//   * F4 Cybernetics Workshop — human-machine fusion: occupation cyborgs + cover, a
//                     ranged elite, a door-override (keypad) puzzle. Carries the
//                     **Humanity meter** (0..100) + an AUGMENTATION-CHAIR choice
//                     (use = power but a Humanity cost; refuse = keep Humanity). The
//                     floor boss "The Collective"/Chorus is the Nexus agent's lane —
//                     NOT placed here; a labeled F4 -> Floor 4.5 transition hook is left.
//   * F5 Drone Manufacturing  — THE drone level: a Swarm-drone set + the floor BOSS
//                     **Swarm Controller AI**. **Sarah's master hack** is a scripted
//                     pre-fight beat (gated on the F5 hub / a hack interact, NOT at
//                     load) that calls ScriptedFightHook::masterHack — stripping ~75%
//                     of the Swarm AI's HP and FLIPPING the drone set from hostile to
//                     allied (the drone-army payoff). Plus ONE rescue captive (a lab
//                     tech) on a hub-gated timer that transforms into a mini-boss on
//                     expiry (mirrors the F2 RescueSystem::activate() gating).
//
// Escalation by design (no unwinnable dogpiles): the standard enemy counts climb
// F3 -> F4 -> F5 and the species mix shifts from melee toward ranged + elite, but
// every enemy pulls its stats/cooldowns from the SAME combat-balance bands the rest
// of the level uses (monster.h namespace combat), and the manager's per-frame melee
// attacker cap (combat::kMaxMeleeAttackers) still arbitrates so the player is never
// melted. Enemies are placed off the elevator-doorway spine so an arriving player
// isn't ambushed inside the shaft.
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
#include <vector>

namespace x3::game {

// Mid-floor trigger event ids (kept distinct from L1Trigger so the two systems can
// share one TriggerSystem without colliding). Each floor has a HUB trigger placed
// where an elevator rider arrives; stepping into the F5 hub starts the rescue clock
// (mirrors L1Trigger::Hub on F2). F3/F4 hubs are reserved for objective/encounter
// activation hooks (alarm beats) and keep the pattern uniform across the wing.
enum class SpireMidTrigger : uint32_t {
    F3Hub = 30,   // F3 Genetics Lab hub reached (encounter armed)
    F4Hub = 40,   // F4 Cybernetics Workshop hub reached (encounter armed)
    F5Hub = 50,   // F5 Drone Manufacturing hub reached -> START the rescue timer (gated!)
};

// The mid floors this module authors, in elevator-stop order. Index maps to the
// L1Floor enum (F3=3, F4=4, F5=5) so the elevator stop index == (uint32_t)floor.
enum class SpireMidFloor : uint32_t { F3 = 0, F4 = 1, F5 = 2, Count = 3 };

// Default starting Humanity (F4 Cybernetics meter, EFLZ canon: too many augments lock
// out the good endings). 0..100; starts full (no augments taken yet).
constexpr int kHumanityMax   = 100;
// Humanity lost per augmentation chair the player USES (the F4 power-vs-self choice).
constexpr int kAugmentHumanityCost = 20;

// One floor's authored encounter summary (read by the host HUD + the self-test to
// assert placement counts/roles + reachability without re-deriving them).
struct SpireFloorPlan {
    L1Floor      floor      = L1Floor::F3;  // which Spire plate
    const char*  name       = "";           // canon floor identity ("Genetics Lab", ...)
    uint32_t     elevStop   = 3;            // elevator stop index (== (uint32_t)floor)
    float        baseY      = 0.0f;         // floor walkable Y (== layout floorBaseY[floor])
    x3::phys::Vec3 arrival{};               // where an elevator rider steps onto the plate
    uint32_t     meleeCount = 0;            // melee (Guard-archetype) enemies placed
    uint32_t     rangedCount= 0;            // ranged (Drone-archetype) enemies placed
    uint32_t     bossCount  = 0;            // Boss-archetype enemies placed (F3 FE#7, F5 Swarm AI)
    uint32_t     totalCount = 0;            // meleeCount + rangedCount + bossCount
    int          doorCode   = 0;            // keypad code on this floor's locked door (0 = none)
    bool         hasVictim  = false;        // a rescue captive is present on this floor
    bool         hasBoss    = false;        // a designed floor boss anchors this floor
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

    // Advance one frame: the enemy groups + the floor bosses (F3 FE#7, F5 Swarm AI),
    // movement + attacks against `player`, and the F5 rescue victim (timer/companion,
    // gated on its hub). `attackFx` (optional) spawns the per-attack beam FX, exactly
    // like Level1Game::tick. `player` may be null (geometry/headless movement only).
    // The host calls this once per frame after Level1Game::tick (independent groups).
    void tick(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
              const x3::phys::Vec3& eye, const x3::phys::Vec3& playerPos,
              IDamageSink* player, const AttackFxFn& attackFx);

    // Dispatch a fired SpireMidTrigger id (the host forwards ids it doesn't own from
    // its TriggerSystem::update() loop). The F5 hub starts the rescue clock; F3/F4
    // hubs latch their floor-reached flag (objective/alarm hook). Idempotent.
    void onTrigger(uint32_t triggerId);

    // Interact (E in range): try to rescue the F5 captive. Returns true iff rescued.
    bool onRescue(const x3::phys::Vec3& playerPos, float range = kRescueReach);

    // Fire one shot across all mid-floor enemy groups + the floor bosses (the first
    // live enemy hit takes it). The host folds this into its onFire path so the pistol
    // works on the mid floors too. Returns the result for FX/HUD. No arm-gate here
    // (the host owns the WeaponSystem::hasWeapon() gate, as it does for Level1Game).
    FireResult onFire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                      Scene& scene, x3::phys::IPhysicsWorld& physics,
                      int damage = kDamagePerShot,
                      x3::DamageType type = x3::DamageType::Kinetic);

    // Draw all mid-floor enemies + the floor bosses + the F5 victim/boss (host calls
    // in its draw block).
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
    // The authored plan for a mid floor (counts/roles/code/victim/boss/reachability).
    const SpireFloorPlan& plan(SpireMidFloor f) const { return m_plan[(uint32_t)f]; }

    // Enemy managers per mid floor (read for placement-count/role assertions). These
    // hold the STANDARD enemies; each floor's BOSS lives in its own manager (below).
    const MonsterManager& enemies(SpireMidFloor f) const { return m_enemies[(uint32_t)f]; }
    MonsterManager&       enemies(SpireMidFloor f)       { return m_enemies[(uint32_t)f]; }

    // ---- F3 boss: Failed Experiment #7 (Marcus Webb) ----------------------
    // The F3 boss is its own manager (like spire_top's Clone) so its count/role/phase
    // can be asserted distinctly from the standard hybrid pack.
    const MonsterManager& f3Boss() const { return m_f3Boss; }
    MonsterManager&       f3Boss()       { return m_f3Boss; }

    // ---- F4 Cybernetics — the Humanity meter (EFLZ canon) -----------------
    // A simple tracked stat in [0, kHumanityMax]. Starts full; each augmentation chair
    // the player USES on F4 deducts kAugmentHumanityCost. Refusing keeps it intact.
    int  humanity() const { return m_humanity; }
    // Adjust Humanity by `delta` (clamped to [0, kHumanityMax]); returns the new value.
    int  adjustHumanity(int delta);
    // The F4 augmentation-chair CHOICE. `accept`==true: the player sat in the chair and
    // took the augment (Humanity cost, the power-vs-self trade); false: refused (no
    // cost). Latched per-chair so a chair can only be used once. Returns true iff the
    // choice was newly applied this call. The host wires this to an E-interact at a
    // chair; the self-test drives it directly.
    bool augmentChairChoice(bool accept);
    bool augmentChairUsed() const { return m_augmentChairUsed; }
    // The labeled F4 -> Floor 4.5 (Nexus Chamber / The Chorus) transition hook. This
    // module does NOT build the Chorus (that is the spire_nexus agent's lane); it only
    // exposes a named, queryable transition point so the host can route the F4 exit to
    // the off-elevator Nexus. Position is the F4 plate's inner edge (post-puzzle).
    x3::phys::Vec3 nexusTransition() const { return m_nexusTransition; }

    // ---- F5 boss: Swarm Controller AI + Sarah's master hack ----------------
    // The Swarm Controller AI boss (its own manager). The drone set it commands lives
    // in enemies(F5) — those are the drones Sarah's hack flips to allied.
    const MonsterManager& swarmBoss() const { return m_swarmBoss; }
    MonsterManager&       swarmBoss()       { return m_swarmBoss; }
    // Has the player triggered Sarah's master hack yet? (false at load; flips true on
    // the hack interact / once the F5 hack beat fires — never at load).
    bool sarahHackDone() const { return m_sarahHackDone; }
    // The DRONE set Sarah's hack flips (the F5 standard enemies that are ranged drones).
    // Read by the self-test to assert they were hostile before / allied after the hack.
    uint32_t f5DroneCount() const;
    // Run Sarah's 90-second master hack as a scripted PRE-FIGHT beat: strip ~75% of the
    // Swarm Controller AI's HP and FLIP the F5 drone set from hostile to ALLIED (the
    // drone-army payoff). Uses ScriptedFightHook::masterHack. Idempotent (no-op after
    // the first call). GATED: the host calls this from a trigger/interact (the F5 hub /
    // a hack-console E-press), NOT at load. Returns the masterHack Result (HP stripped +
    // drones flipped); a no-op call returns {0,0}.
    ScriptedFightHook::Result runSarahMasterHack();

    // The F5 rescue victim (read to assert it is present but NOT active at load).
    bool   victimPresent() const { return m_victim != nullptr; }
    bool   victimCaptive() const;          // alive + still a captive (not rescued/expired)
    bool   victimTimerRunning() const { return m_f5HubReached; }   // clock gated on the F5 hub
    float  victimTimeLeft() const;
    // Borrow the F5 victim itself (chat-tree talk target: name/pos/state). May be null.
    const RescueVictim* victim() const { return m_victim.get(); }

    // Reachability: a floor is reachable iff its elevator stop index is inside the
    // elevator's stop range (one stop per floor, 0..kSpireFloorCount-1). The host
    // builds exactly that many stops from the layout, so this is a content-side
    // assertion that the floors we authored line up with the progression.
    bool reachableViaElevator(SpireMidFloor f, uint32_t elevatorStopCount) const;

    // Ragdoll-teardown gap fix: release every in-flight death-ragdoll (Jolt bodies)
    // across ALL of this host's enemy managers (per-floor packs + the F3/F5 bosses +
    // the F5 victim-boss) BEFORE the physics world is shut down. Idempotent; a no-op
    // when nothing is ragdolling. The owning host MUST call this before
    // physics->shutdown() (see app_run's shutdownGameSystems). Mirrors the game/nexus
    // teardown so a monster killed in the last ~0.7 s (mid-flop) on a mid floor never
    // touches a dead Jolt system when its IRagdoll is later destroyed.
    void shutdown();

private:
    bool m_built = false;
    std::string m_modelDir;

    SpireFloorPlan m_plan[(uint32_t)SpireMidFloor::Count];
    MonsterManager m_enemies[(uint32_t)SpireMidFloor::Count];
    DoorSystem     m_doors;            // the per-floor keypad doors (F3/F4/F5)

    // F3 floor boss: Failed Experiment #7 (its own manager, like the F7 Clone).
    MonsterManager m_f3Boss;

    // F4 Cybernetics: the Humanity meter + the augmentation-chair choice latch + the
    // labeled F4 -> Floor 4.5 (Nexus / Chorus) transition hook.
    int            m_humanity = kHumanityMax;   // 0..100, starts full
    bool           m_augmentChairUsed = false;  // the F4 chair has been chosen (used/refused) once
    x3::phys::Vec3 m_nexusTransition{};         // F4 -> Floor 4.5 hook (Nexus agent owns the boss)

    // F5 Drone Manufacturing: the Swarm Controller AI boss + Sarah's master-hack latch.
    MonsterManager m_swarmBoss;
    bool           m_sarahHackDone = false;     // Sarah's master hack performed (never at load)

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
//   * the expected enemy COUNT and the melee/ranged/boss ROLE split per floor;
//   * the canon floor identities (Genetics Lab / Cybernetics Workshop / Drone Mfg);
//   * F3 carries the Failed Experiment #7 boss; F5 carries the Swarm Controller AI;
//   * difficulty escalates (F3 < F4 < F5 standard-enemy totals);
//   * the F4 Humanity meter starts full, drops on a chair USE, and is unchanged on a
//     REFUSE; the F4 -> Floor 4.5 transition hook is exposed (Chorus NOT placed here);
//   * Sarah's master hack is gated (not at load), and when run strips the Swarm AI's
//     HP fraction + flips the F5 drone set to allied;
//   * the F5 rescue victim is PRESENT but its timer is NOT running at load (it only
//     starts once the F5 hub trigger fires) and it is still a captive;
//   * each floor is REACHABLE via the elevator (its stop index is within the stop
//     range the host builds, one per floor);
//   * the keypad door per floor is LOCKED with the authored code.
// Prints "spiremid: X/Y passed"; returns true iff all pass. No window/Vulkan.
bool runSpireMidSelfTest();

// Headless self-test (--test-dronehack). Focused on the F5 Drone Manufacturing
// signature beat — Sarah's master hack — built on the SpireMidFloors content. Asserts:
//   * the hack is GATED: not performed at load (sarahHackDone()==false) and the F5
//     drone set is HOSTILE (non-zero attack damage, not allied) + the Swarm Controller
//     AI boss is at FULL HP before the hack;
//   * running the hack (gated trigger/interact, NOT at load) STRIPS the Swarm AI's
//     ~75% HP fraction (boss survives at >= 1 HP, still fights) AND FLIPS the entire
//     drone set from hostile to ALLIED (attack damage zeroed, isAllied()==true);
//   * the hack is idempotent (a second call is a no-op).
// Prints "dronehack: X/Y passed"; returns true iff all pass. No window/Vulkan.
bool runDroneHackSelfTest();

} // namespace x3::game
