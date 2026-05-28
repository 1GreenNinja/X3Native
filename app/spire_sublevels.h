#pragma once
// EFLZ Act 1 "The Spire" — HIDDEN FLOOR-7 SUB-LEVELS + the Dr. Chen RETURN MISSION.
// Game/slice code only — engine/ stays pure.
//
// CONTENT/LEVEL-SCRIPT ONLY. Like spire_mid.* / spire_top.* (the F3..F7 floor content
// it copies its authoring pattern from), this module does NOT touch the renderer or
// any core-engine system: it composes the EXISTING data-driven roster (monster.* —
// DominionTrooper/Verthani/Illuminated/BlueSynth + a single-body Boss for the mini-boss),
// the rescue system (rescue.*), the door/keypad system (door.*) and the trigger system
// (trigger.*) onto NEW graybox plates this module builds BELOW the Spire's basement.
//
// THE DESIGN (docs/design/EFLZ_WORLD_STRUCTURE.md §3b "The Floor-7 SUB-LEVELS"):
// AFTER the Clone is defeated AND Sarah is saved on F7, the executive terminal reveals
// Dr. Chen is alive, being tortured, and a PREVIOUSLY HIDDEN ELEVATOR behind the
// executive desk activates — a hidden vertical descent into three sub-levels found ONLY
// then:
//   * SUB-LEVEL 1 — WASTE DISPOSAL & FAILED EXPERIMENTS: incinerator/toxic-gas HAZARD
//     floor; Disposal Drones + Corrupted Janitors. (We read this as a melee-led pack
//     under a standing hazard volume.)
//   * SUB-LEVEL 2 — CRYOGENIC STORAGE: hundreds in stasis; mini-boss THE FROZEN
//     COLLECTIVE (merged cryo-subjects). A SINGLE-BODY Boss configured locally here
//     (reusing the Alien-Overseer/Chen-style phase-boss tuning — NOT a new monster.*
//     row, which is not this module's lane).
//   * SUB-LEVEL 3 — ENHANCED INTERROGATION: Dr. Chen's torture chamber. Chen is held
//     CAPTIVE — a RESCUE (E in range; mirrors RescueSystem gating) that FREES him: the
//     Return-Mission payoff.
//
// HIDDEN UNTIL GATED (the crux): the descent is UNAVAILABLE at load. Its hidden
// lift/trigger is NOT armed and the sub-level encounters do NOT tick/draw until the
// HOST reports — from spire_top's public queries — that the Clone has FALLEN and Sarah
// is SAVED on F7. The host computes those two booleans (Clone boss aliveCount()==0;
// Sarah no longer a captive after a successful F7 rescue) and calls openDescent() once;
// only then does armDescent latch and the hidden lift trigger arm. NEVER armed at load.
//
// Coords per docs/CONVENTIONS.md: +X right, +Y up, -Z forward. The three sub-level plates
// stack DOWNWARD (−Y) FAR below B1 at the REAL canon depth (X3_WORLD_BLUEPRINT §2.1/§2.5:
// SL1=-170, SL2=-174, SL3=-178 m — the "SUB hidden ≈ -170" plane + the -178 cave horizon;
// the old graybox put them at a token -5/-10/-15). They share B1's XZ footprint so a hidden
// lift column lines up vertically, and the gated hidden lift drops the player the full ~170
// m. They are NOT elevator stops on the main 0..kSpireFloorCount-1 list (the hidden descent
// is off that list — see WORLD_STRUCTURE §3b "the elevator's main floor-list does not
// advertise"), so they cannot be reached until the descent opens. SL3's -178 m horizon also
// hosts the FRESH-authored §2.5(a) Salvari caves (singing crystals + alien-markings obelisk).

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

// Sub-level trigger event ids. Kept DISTINCT from L1Trigger / SpireMidTrigger
// (30/40/50) / SpireTopTrigger (60/70) so all systems can share one TriggerSystem
// without colliding. The DESCENT trigger is the hidden lift behind the executive desk
// (armed only after openDescent()); each sub-level has a HUB trigger placed where the
// descent arrives. Stepping into the SL3 hub starts Chen's rescue clock (gated).
enum class SpireSubTrigger : uint32_t {
    Descent  = 80,   // hidden lift behind the exec desk reached (arms ONLY post-openDescent)
    SL1Hub   = 81,   // SL1 Waste Disposal hub reached (hazard/encounter armed)
    SL2Hub   = 82,   // SL2 Cryogenic Storage hub reached (Frozen Collective armed)
    SL3Hub   = 83,   // SL3 Enhanced Interrogation hub reached -> START Chen's rescue clock (gated!)
};

// The three sub-levels this module authors, descent order (SL1 highest, SL3 lowest).
enum class SpireSubLevel : uint32_t { SL1 = 0, SL2 = 1, SL3 = 2, Count = 3 };

// §2.5(a) Salvari-cave constants (X3_WORLD_BLUEPRINT). Public so the inline cave queries
// + the self-test can reference them. The caves sit at the canon -178 m horizon (== the
// deepest sub-level base), with a cluster of singing/lore crystals.
constexpr float kCaveBaseY           = -178.0f;  // the Salvari cave floor (canon -178 m)
constexpr int   kSalvariCrystalCount = 7;        // singing/lore crystals authored in the cavern

// One sub-level's authored encounter summary (read by the host HUD + the self-test to
// assert placement counts/roles + the descent gate without re-deriving them). Mirrors
// SpireFloorPlan / SpireTopPlan but is keyed off a sub-level base Y BELOW B1 (not an
// elevator stop) and adds a hazard flag (SL1) + a mini-boss flag (SL2) + a captive flag.
struct SpireSubPlan {
    SpireSubLevel sub        = SpireSubLevel::SL1;
    float         baseY      = 0.0f;  // sub-level walkable Y (BELOW B1's 0; descends in -Y)
    x3::phys::Vec3 arrival{};          // where the hidden lift sets the player down
    uint32_t      meleeCount = 0;      // melee (Guard-archetype) enemies placed
    uint32_t      rangedCount= 0;      // ranged (Drone-archetype) enemies placed
    uint32_t      bossCount  = 0;      // Boss-archetype enemies placed (SL2 Frozen Collective)
    uint32_t      totalCount = 0;      // meleeCount + rangedCount + bossCount
    int           doorCode   = 0;      // keypad code on this sub-level's locked door (0 = none)
    bool          hasHazard  = false;  // a standing hazard volume is present (SL1)
    bool          hasMiniBoss= false;  // a Boss-type leader anchors this sub-level (SL2)
    bool          hasCaptive = false;  // a rescue captive is present (SL3 Dr. Chen)
};

// Hidden Floor-7 sub-level authoring system. Build once after spire_top (the F7 finale).
// At BUILD time the geometry/enemies/captive are placed but the DESCENT IS HIDDEN: the
// hidden-lift trigger is NOT registered until openDescent() is called (gated by the host
// on the F7-complete state). Mirrors SpireTopFloors' shape: build()/tick()/draw()/onFire/
// onTrigger/onRescue + plan/query accessors.
class SpireSubLevels {
public:
    // Author the SL1/SL2/SL3 sub-levels as new graybox plates BELOW B1. `layout` gives
    // B1's XZ footprint + base Y to stack the sub-levels under (descending −Y). `triggers`
    // is the host's shared TriggerSystem (sub-level HUB triggers + the gated DESCENT
    // trigger dispatch through it; the host forwards their ids to onTrigger()). `modelDir`
    // is the loose rigged-GLB dir (same one SpireTopFloors::build receives). Call once,
    // after the top floors are built. The descent is HIDDEN at build (not armed).
    void build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, const Level1Layout& layout,
               TriggerSystem& triggers, std::string_view modelDir);

    // ---- The HIDDEN-DESCENT gate (the crux) -------------------------------
    // Open the hidden descent. Idempotent + ONE-WAY: arms the hidden-lift trigger and
    // marks the sub-levels reachable, but ONLY when BOTH gate conditions hold —
    // `cloneFallen` (the F7 Clone boss is down) AND `sarahSaved` (Sarah was rescued on
    // F7, not lost). The host computes both from spire_top's PUBLIC queries (it reads
    // spire_top, never modifies it). With either false this is a NO-OP and the descent
    // stays hidden. Returns true the call that actually opens it. NEVER call at load
    // with the gate satisfied — at load the Clone is alive + Sarah captive, so the gate
    // is naturally closed.
    bool openDescent(bool cloneFallen, bool sarahSaved);
    // True once the hidden descent has been opened (the gate was satisfied). Until then
    // the descent is hidden + the sub-level encounters are inert.
    bool descentOpen() const { return m_descentOpen; }

    // Advance one frame. While the descent is CLOSED this is INERT (no enemy movement,
    // no hazard ticking, no Chen clock) so the hidden sub-levels cost nothing + cannot
    // hurt a player who is still up top. Once open: the sub-level enemy groups + the
    // Frozen Collective mini-boss move/attack `player`, the SL1 hazard damages a player
    // standing in it, and Chen's rescue clock runs (gated on the SL3 hub). `attackFx`
    // (optional) spawns per-attack beam FX, exactly like SpireTopFloors::tick. `player`
    // may be null (geometry/headless movement only).
    void tick(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
              const x3::phys::Vec3& eye, const x3::phys::Vec3& playerPos,
              IDamageSink* player, const AttackFxFn& attackFx);

    // Dispatch a fired SpireSubTrigger id (the host forwards ids it doesn't own from its
    // TriggerSystem::update() loop). The DESCENT id latches descent-reached; the SL3 hub
    // starts Chen's rescue clock; SL1/SL2 hubs latch their floor-reached flags. ALL are
    // ignored while the descent is closed (defensive — the trigger isn't even armed yet).
    // Idempotent.
    void onTrigger(uint32_t triggerId);

    // Interact (E in range): try to rescue Dr. Chen (the Return-Mission payoff). Returns
    // true iff Chen was freed this call. No-op while the descent is closed.
    bool onRescue(const x3::phys::Vec3& playerPos, float range = kRescueReach);

    // Fire one shot across all sub-level enemy groups + the mini-boss (the first live
    // enemy hit takes it). The host folds this into its onFire path. No-op (a miss) while
    // the descent is closed (the sub-levels aren't in play). Returns the result for FX/HUD.
    FireResult onFire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                      Scene& scene, x3::phys::IPhysicsWorld& physics,
                      int damage = kDamagePerShot);

    // Draw all sub-level enemies + the Frozen Collective + Dr. Chen. No-op while the
    // descent is closed (nothing to show — the sub-levels are hidden). Host calls in its
    // draw block.
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
              const Scene& scene) const;
    // Draw the sub-level keypad door slabs at their current (animating) transforms.
    void drawDoors(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const {
        if (m_descentOpen) m_doors.drawMeshes(device, frame);
    }

    // Door access (host HUD / self-test): the keypad door system for the sub-levels.
    DoorSystem&       doors()       { return m_doors; }
    const DoorSystem& doors() const { return m_doors; }
    // True if `playerPos` is within `range` of a LOCKED coded sub-level door (only once
    // the descent is open).
    bool nearLockedCodedDoor(const x3::phys::Vec3& playerPos, float range = 3.5f) const;
    // Submit a keypad `code`: unlock+open the nearest locked coded sub-level door in
    // range whose code matches. Returns true if a door began opening. No-op while closed.
    bool tryDoorCode(const x3::phys::Vec3& playerPos, int code, float range = 3.5f);

    // ---- Queries (host HUD + the self-test) -------------------------------
    bool built() const { return m_built; }
    // The authored plan for a sub-level (counts/roles/code/hazard/mini-boss/captive).
    const SpireSubPlan& plan(SpireSubLevel s) const { return m_plan[(uint32_t)s]; }

    // Enemy managers per sub-level (read for placement-count/role assertions). SL2's
    // manager holds the cryo-pack adds; the Frozen Collective mini-boss is its own
    // manager (miniBoss()).
    const MonsterManager& enemies(SpireSubLevel s) const { return m_enemies[(uint32_t)s]; }
    MonsterManager&       enemies(SpireSubLevel s)       { return m_enemies[(uint32_t)s]; }

    // The SL2 Frozen Collective mini-boss manager (read to assert a Boss-type leader is
    // present + alive once the descent is open).
    const MonsterManager& miniBoss() const { return m_miniBoss; }
    MonsterManager&       miniBoss()       { return m_miniBoss; }

    // The SL3 Chen-timer-expired mini-boss manager (empty until Chen's rescue clock
    // expires and the floor spawns a transformed adversary). Exposed for the HUD
    // bar-loop / radar so this enemy gets a health bar like every other.
    const MonsterManager& chenBoss() const { return m_chenBoss; }
    MonsterManager&       chenBoss()       { return m_chenBoss; }

    // The SL3 Dr. Chen rescue captive.
    bool   chenPresent() const { return m_chen != nullptr; }
    bool   chenCaptive() const;          // alive + still a captive (not rescued/expired)
    bool   chenRescued() const;          // freed -> companion (the Return-Mission payoff)
    bool   chenTimerRunning() const { return m_sl3HubReached; }   // clock gated on the SL3 hub
    float  chenTimeLeft() const;

    // ---- §2.5(a) THE SALVARI CAVES (Y≈-178) queries (HUD + self-test) -------------
    // The caves are authored FRESH off the deepest sub-level. They are present at build
    // (collision graybox + glowing crystal props) but HIDDEN (render meshes invisible)
    // until the descent opens + the first tick reveals them.
    bool     cavesPresent() const { return m_cavesPresent; }
    bool     cavesRevealed() const { return m_cavesRevealed; }
    // Count of singing/lore Salvari crystals authored in the cavern.
    uint32_t salvariCrystalCount() const { return m_cavesPresent ? (uint32_t)kSalvariCrystalCount : 0u; }
    // The crystal-cluster center (the cavern focal point), at the -178 m cave horizon.
    const x3::phys::Vec3& salvariCrystalCenter() const { return m_salvariCrystalCenter; }
    // The cave floor Y (the canon -178 m horizon).
    float    caveBaseY() const { return kCaveBaseY; }

    // The SL1 standing hazard: present (authored) + active (only ticks once the descent
    // is open). Read by the HUD + the self-test.
    bool   hazardPresent() const { return m_hazardPresent; }
    bool   hazardActive()  const { return m_descentOpen && m_hazardPresent; }
    // True iff `pos` is inside the SL1 hazard volume (the toxic incinerator zone). The
    // HUD uses this for a "TOXIC" warning; the self-test uses it to assert the volume is
    // bounded (a point outside it is not "in hazard") without the enemy-attack confound.
    bool   inHazardVolume(const x3::phys::Vec3& pos) const {
        return m_hazardPresent && pointInBox(pos, m_hazardMin, m_hazardMax);
    }

private:
    // §2.5(a) Build the Salvari caves (collision graybox + emissive crystal props) at the
    // -178 m horizon off the SL3 plate. Called once from build(); render meshes start
    // invisible, revealed on the first tick after the descent opens. `b1` is B1's borrowed
    // footprint (the sub-levels share the tower XZ).
    void buildCaves(Scene& scene, x3::rhi::IRenderDevice& device,
                    x3::phys::IPhysicsWorld& physics, const L1RoomDef& b1);

    bool m_built = false;
    std::string m_modelDir;
    x3::rhi::IRenderDevice* m_device = nullptr;  // cached for the on-expiry boss spawn

    // §2.5(a) Salvari caves: collision graybox + glowing crystal/obelisk props at Y=-178,
    // authored at build but HIDDEN (render meshes invisible) until revealed the first tick
    // after the descent opens. m_caveEntities holds the scene ids so visibility can toggle.
    bool                  m_cavesPresent  = false;
    bool                  m_cavesRevealed = false;
    std::vector<uint32_t> m_caveEntities;
    x3::phys::Vec3        m_salvariCrystalCenter{};

    // The HIDDEN-DESCENT gate. False at build (never armed at load); set true ONCE by
    // openDescent() when the F7-complete gate is satisfied. While false the whole system
    // is inert (tick/draw/onFire/onRescue are no-ops) and the descent trigger is unarmed.
    bool m_descentOpen = false;
    TriggerSystem* m_triggers = nullptr;   // host's shared system; descent trigger added on open
    x3::phys::Vec3 m_descentVolMin{};      // the hidden-lift trigger volume (registered on open)
    x3::phys::Vec3 m_descentVolMax{};

    SpireSubPlan   m_plan[(uint32_t)SpireSubLevel::Count];
    MonsterManager m_enemies[(uint32_t)SpireSubLevel::Count];
    MonsterManager m_miniBoss;         // the SL2 "Frozen Collective" Boss-type leader (its own group)
    DoorSystem     m_doors;            // the per-sub-level keypad doors (SL1/SL2/SL3)

    // SL1 standing hazard volume (incinerator / toxic gas). A simple host-evaluated AABB
    // that drains the player's HP while they stand inside it (only when the descent is
    // open). Present-but-inert until the descent opens.
    bool           m_hazardPresent = false;
    x3::phys::Vec3 m_hazardMin{};
    x3::phys::Vec3 m_hazardMax{};
    float          m_hazardTick = 0.0f;   // accumulator so the hazard ticks at a fixed cadence

    // SL3 Dr. Chen rescue captive (gated on the SL3 hub). Owned here; drawn via
    // RescueVictim::draw. He carries a (never-reached) bossTuning for API symmetry with
    // the other captives; freeing him before the clock matters is the Return payoff.
    std::unique_ptr<RescueVictim> m_chen;
    MonsterManager m_chenBoss;         // a mini-boss spawned only if Chen's timer expires

    // Floor-reached latches (SL3 gates Chen's rescue clock; SL1/SL2 are alarm hooks; the
    // descent latch records the hidden lift was used).
    bool m_descentReached = false;
    bool m_sl1HubReached = false;
    bool m_sl2HubReached = false;
    bool m_sl3HubReached = false;
};

// Per-tick hazard damage + cadence for the SL1 incinerator/toxic-gas volume. Gameplay
// tuning — the self-test asserts the BEHAVIOUR (a player inside the volume loses HP only
// once the descent is open), not the exact numbers.
constexpr float kSubHazardInterval = 0.5f;   // seconds between hazard damage ticks
constexpr int   kSubHazardDamage   = 4;      // HP drained per tick while standing in it

// Headless self-test (--test-sublevels). Builds the Spire (buildLevel1) + the top floors
// (spire_top, for the F7 gate context) + the sub-levels on a HeadlessDevice + Jolt world
// and asserts:
//   * at LOAD the descent is HIDDEN/inert: descentOpen()==false, the descent trigger is
//     NOT armed, ticking many frames moves nothing + Chen never starts his clock;
//   * openDescent() is GATED: it is a NO-OP unless BOTH (Clone fallen) AND (Sarah saved)
//     hold — open with neither / only one stays closed; open with both opens;
//   * once OPEN, SL1/SL2/SL3 build with the expected enemy COUNTS + role split, SL1 has a
//     hazard, SL2 carries the Frozen Collective Boss-type mini-boss (alive), SL3 has Dr.
//     Chen present-but-captive (not active/rescued at load);
//   * the SL1 hazard drains a player standing in it ONLY after the descent is open;
//   * the SL3 hub starts Chen's clock; the rescue (E in range) FREES Chen (companion).
// Prints "sublevels: X/Y passed"; returns true iff all pass. No window/Vulkan.
bool runSubLevelsSelfTest();

} // namespace x3::game
