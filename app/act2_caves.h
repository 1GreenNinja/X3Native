#pragma once
// EFLZ Act 2 — MID-BIOMES content: L12 Advanced Cave System, L13 Toxic
// Swamplands Edge, L14 Research Station, L15 Tree Cities. Game/slice content
// code only — engine/ stays pure.
//
// CLEAN-ROOM, original work. Built ONLY from X3Native's OWN systems (Scene,
// monster.*, trigger.*, mesh_prims) + the engine interfaces + the EFLZ design
// docs in docs/design/ (Tim's own IP). NO RBDOOM / id Tech / Doom / Quake — or
// ANY other game-engine — source was forked, copied, or consulted.
//
// SCOPE / OWNERSHIP: this lane authors the Act-2 mid biomes (L12..L15). It does
// NOT touch the Act-2 open-world host (act2_world.*, owns L8/L9), the desert
// lane (act2_desert.*, owns L10/L11 — landing on a separate machine), the Act-1
// floor files, monster.* (the data-driven roster) or the renderer/engine. It
// COMPOSES the existing Act-2 enemy + boss roster (act2EnemyTuning() /
// act2BossTuning()), the trigger system, and a couple of plain Scene props
// (the Crystal Heart interactable, the Tree-Cities trading platform) onto a
// per-level Act2CaveAreaPlan descriptor:
//
//   * L12 ADVANCED CAVE SYSTEM — bioluminescent multi-layer caves. Upper
//     caves + glow-lake -> Salvari Archives reading-room (a few SalvariAlly
//     markers) -> the CRYSTAL HEART CHAMBER: a dual-gated interactable
//     (Jake-strength AND Sarah-hack BOTH required to activate; story-branch
//     flag). Beyond it, an abyss boss arena holding the Memory Hunter (from
//     act2BossTuning(Act2BossType::MemoryHunter)). The boss carries a
//     copy/feint phase tag the HUD/floor reads (data-driven, the boss machine
//     just labels the phase).
//
//   * L13 TOXIC SWAMPLANDS EDGE — mutated flora hazards (MutatedFlora rows,
//     stationary lash-reach hostiles) + a poison/exposure HazardZone modelled
//     as an AABB + a tracked exposure stat. PRESENT but INERT at load (mirrors
//     L9 hazard); arms only once the player enters or the trigger fires.
//
//   * L14 RESEARCH STATION — mutated scientists (MutatedScientist row, ranged
//     chemical attackers) + a TIMELINE-GATED Beta Siren ambush. The ambush is
//     present only if the F2 women WEREN'T saved (timeline flag from Act 1).
//     When the flag is true (women unsaved), Act2BossType::TheSiren is placed
//     and the floor reads it as a Beta encounter; when false (women saved),
//     the room contains a normal-encounter pack with NO Siren boss. The flag
//     lives on the Act2Caves instance and is read at build() time (so the
//     self-test can flip it and rebuild from a fresh world).
//
//   * L15 TREE CITIES — vertical canopy graybox (3 platforms at rising heights
//     so a host could wire a climb) + a trading/upgrade interact stub: a marked
//     Scene prop at a known world position the host wires an E-press to.
//
//   * REACHABILITY: L11 -> L12 -> L13 -> L14 -> L15 via labelled box triggers
//     in a fresh, non-colliding id range (kAct2CavesTrigBase = 100). Mirrors
//     the L8 -> L9 transition pattern from act2_world.* exactly.
//
// Authoring style mirrors act2_world.* exactly: build() once, tick() each frame
// for the per-level enemy/ally managers + the L13 hazard, onTrigger() dispatch,
// draw() helpers, and plan()/query accessors read by the headless self-test
// (--test-act2caves) so nothing is re-derived.

#include "scene.h"
#include "monster.h"
#include "trigger.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace x3::game {

// The Act-2 mid-biome levels this module authors. The enum value IS the canon
// level number so a plan lookup reads naturally. Only L12..L15 are owned here
// — L11 is included as the entry hub anchor (its name/objective are NOT set;
// that's act2_desert's lane); the L12 spawn is the L11 -> L12 cave portal.
enum class Act2CaveLevel : uint32_t {
    L12_AdvancedCaveSystem  = 12,
    L13_ToxicSwamplandsEdge = 13,
    L14_ResearchStation     = 14,
    L15_TreeCities          = 15,
};
constexpr uint32_t kAct2CavesFirstLevel = 12;
constexpr uint32_t kAct2CavesLastLevel  = 15;
constexpr uint32_t kAct2CavesLevelCount =
    kAct2CavesLastLevel - kAct2CavesFirstLevel + 1; // 4

// Act-2 mid-biome trigger event ids. A FRESH id range starting at 100 — well
// above the Act-1 L1Trigger (10..)/SpireMidTrigger (30/40/50) ranges AND above
// the Act2Trigger 80..82 range act2_world.* owns AND above any plausible Act-2
// desert (L10/L11) range (act2_desert lands on a separate machine; this leaves
// 83..99 as the desert's safe gap). All ids are contiguous so the host can
// switch over them in one place.
enum class Act2CavesTrigger : uint32_t {
    L11toL12Portal      = 100,  // entered the L12 cave portal from the Salvari Camp
    L12CrystalHeartRoom = 101,  // reached the Crystal Heart Chamber (interactable arm)
    L12MemoryHunterArena= 102,  // crossed into the L12 abyss boss arena (Memory Hunter)
    L12toL13Transition  = 103,  // crossed L12 -> L13 (cave exit -> swamp edge)
    L13PoisonHazard     = 104,  // entered the L13 poison hazard zone (arms exposure)
    L13toL14Transition  = 105,  // crossed L13 -> L14 (swamp -> research station)
    L14SirenAmbush      = 106,  // tripped the L14 Siren ambush (timeline-gated)
    L14toL15Transition  = 107,  // crossed L14 -> L15 (station -> tree cities)
    L15TradingPost      = 108,  // reached the L15 trading post (interactable arm)
};
constexpr uint32_t kAct2CavesTrigBase = 100;
constexpr uint32_t kAct2CavesTrigCount = 9;

// Body-center Y above ground for a placed humanoid/drone (same as act2_world).
constexpr float kAct2CavesEnemyYOff = 0.4f;

// L13 poison hazard exposure rate (units / sec while inside; matches the L9
// heat/sandstorm rate so the HUD reads consistently across Act-2 hazards).
constexpr float kPoisonExposureRate = 8.0f;

// L15 Tree-City platform count + the canopy lift each platform sits at. Three
// platforms at rising heights so a host can author a vertical-traversal climb.
constexpr uint32_t kTreeCityPlatformCount = 3;

// One Act-2-caves area's authored descriptor (read by the host HUD + the
// self-test so they never re-derive footprint/spawn/objective/counts).
struct Act2CaveAreaPlan {
    Act2CaveLevel level       = Act2CaveLevel::L12_AdvancedCaveSystem;
    const char*   name        = "";   // canon level name ("Advanced Cave System", ...)
    const char*   biome       = "";   // biome / setting one-liner
    const char*   objective   = "";   // primary objective text
    bool          implemented = false;

    x3::phys::Vec3 spawn{};            // where the player enters this area
    float        footprintX  = 0.0f;
    float        footprintZ  = 0.0f;

    uint32_t     meleeCount   = 0;     // melee hostiles placed
    uint32_t     rangedCount  = 0;     // ranged hostiles placed
    uint32_t     allyCount    = 0;     // allied markers placed (Salvari Archives, etc.)
    uint32_t     bossCount    = 0;     // boss-archetype hostiles placed
    uint32_t     propCount    = 0;     // plain Scene props (Crystal Heart / Tree-City / etc.)

    bool         hasHazard    = false; // an env-hazard zone is present at this level
    bool         hasInteract  = false; // an interactable (Crystal Heart / trading post) is here
    bool         sirenGated   = false; // L14 Siren ambush gate read at build time (true => present)
};

// L13 poison hazard zone (mirrors act2_world's HazardZone for the heat/sandstorm
// pattern — local copy so the caves module stays self-contained + the structs
// can diverge later without touching act2_world.h). INERT at load: active=false,
// exposure=0; arms when the player first enters OR the host fires the
// L13PoisonHazard trigger.
struct PoisonHazardZone {
    x3::phys::Vec3 min{};
    x3::phys::Vec3 max{};
    bool   active     = false;
    float  exposure   = 0.0f;
    float  ratePerSec = kPoisonExposureRate;
    bool contains(const x3::phys::Vec3& p) const { return pointInBox(p, min, max); }
};

// L12 Crystal Heart Chamber — a dual-gated interactable (Jake-strength AND
// Sarah-hack BOTH required to activate). At load both gates are FALSE and the
// chamber is INERT (canActivate()==false, activated==false). The host wires
// the strength gate (Jake's super-strength meter check) + the hack gate
// (Sarah's master-hack equivalent) to game-state flips here; activate() is a
// no-op until both are true. On activation the storyBranchLatched flag flips
// on (the story-branch the spec calls out: the Crystal Heart install becomes
// the Asteroid Rebel Base beat later). Idempotent.
struct CrystalHeartChamber {
    x3::phys::Vec3 worldPos{};         // chamber center (interact range checked here)
    uint32_t       propEntity = 0;     // the violet emissive heart prop in the Scene
    bool           strengthGate = false; // Jake-strength gate (false at load)
    bool           hackGate     = false; // Sarah-hack gate (false at load)
    bool           activated    = false; // latched true on the first valid activate() call
    bool           storyBranch  = false; // story-branch flag (set with `activated`)

    // True iff BOTH gates are currently satisfied (interactable is "armed").
    bool canActivate() const { return strengthGate && hackGate; }
    // Try to activate the chamber. No-op (returns false) unless canActivate().
    // On the first successful call, latches activated + storyBranch. Subsequent
    // calls return false (already activated).
    bool activate() {
        if (activated || !canActivate()) return false;
        activated = true;
        storyBranch = true;
        return true;
    }
};

// The Act-2 mid-biomes content system. Build once after the Act-2 host stands
// up; tick() each frame for the per-level enemy/ally groups + the L13 hazard.
// All interactables are GATED — never armed at load.
class Act2Caves {
public:
    // Set the L14 Siren ambush timeline flag BEFORE build(). True == "F2 women
    // were NOT saved" (transformed-women Beta path is canon) => the Siren
    // ambush is placed at L14. False == "F2 women were saved" => the L14 room
    // gets a normal mutated-scientist encounter with NO Siren boss. The flag
    // is read at build() time; the test flips it and rebuilds to assert both
    // halves of the gate.
    void setSirenAmbushGate(bool womenLostOnF2) { m_sirenGate = womenLostOnF2; }
    bool sirenAmbushGate() const { return m_sirenGate; }

    // Stand up L12..L15. `triggers` is the host's shared TriggerSystem (the
    // host forwards fired Act2CavesTrigger ids to onTrigger()). `modelDir` is
    // the loose rigged-GLB dir (same one the rest of Act-2 receives). Call once.
    void build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, TriggerSystem& triggers,
               std::string_view modelDir);

    // Advance one frame: each level's enemy/ally managers (attacks against
    // `player`), the L13 poison hazard exposure (only once armed, OR the
    // player is inside), and the L12 Memory Hunter boss phase machine.
    // `player` may be null (geometry/headless movement only).
    void tick(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
              const x3::phys::Vec3& eye, const x3::phys::Vec3& playerPos,
              IDamageSink* player, const AttackFxFn& attackFx);

    // Dispatch a fired Act2CavesTrigger id the host forwards from its
    // TriggerSystem. Latches the per-level beats + arms the L13 hazard +
    // arms the L14 Siren ambush flag. Idempotent.
    void onTrigger(uint32_t triggerId);

    // Fire one shot across every level's hostile groups (the first live hit
    // takes it). Allied SalvariAlly markers + Tree-City interact props are
    // NOT valid targets. Returns the result for FX/HUD. `damage` is the firing weapon's
    // per-shot damage; `type` is the canon-aliens DamageType tag (Kinetic/Energy/...)
    // that bosses with adaptiveHideResist > 0 react to. Both defaulted.
    FireResult onFire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                      Scene& scene, x3::phys::IPhysicsWorld& physics,
                      int damage = kDamagePerShot,
                      x3::DamageType type = x3::DamageType::Kinetic);

    // Draw every level's enemy/ally groups. (Plain Scene props — Crystal Heart,
    // Tree-City platforms, trading-post pillar — are drawn by the host's
    // scene.render().)
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
              const Scene& scene) const;

    // ---- Queries (host HUD + the self-test) -------------------------------
    bool built() const { return m_built; }
    const Act2CaveAreaPlan& plan(Act2CaveLevel l) const {
        return m_plan[(uint32_t)l - kAct2CavesFirstLevel];
    }

    // L12 cave-system content: hostile pack + the Salvari Archives allies +
    // the Memory Hunter boss.
    const MonsterManager& l12Enemies()  const { return m_l12Enemies; }
    MonsterManager&       l12Enemies()        { return m_l12Enemies; }
    const MonsterManager& l12Allies()   const { return m_l12Allies; }
    const MonsterManager& l12Boss()     const { return m_l12Boss; }
    MonsterManager&       l12Boss()           { return m_l12Boss; }
    bool memoryHunterPresent() const { return m_l12Boss.count() > 0; }

    // L12 Crystal Heart Chamber (the dual-gated interactable). Mutable view so
    // the host/test can flip the strength/hack gates and call activate().
    const CrystalHeartChamber& crystalHeart() const { return m_crystalHeart; }
    CrystalHeartChamber&       crystalHeart()       { return m_crystalHeart; }

    // L13 swamp content: mutated flora pack + the poison hazard zone.
    const MonsterManager& l13Enemies() const { return m_l13Enemies; }
    MonsterManager&       l13Enemies()       { return m_l13Enemies; }
    const PoisonHazardZone& poisonHazard() const { return m_poison; }
    bool  poisonHazardActive()   const { return m_poison.active; }
    float poisonHazardExposure() const { return m_poison.exposure; }

    // L14 station content: mutated scientists + the (timeline-gated) Siren.
    const MonsterManager& l14Enemies() const { return m_l14Enemies; }
    MonsterManager&       l14Enemies()       { return m_l14Enemies; }
    const MonsterManager& l14SirenBoss() const { return m_l14Siren; }
    MonsterManager&       l14SirenBoss()       { return m_l14Siren; }
    // True iff the L14 Siren ambush boss was placed at build() — i.e. the
    // sirenAmbushGate() was TRUE when build() ran (F2 women unsaved).
    bool sirenAmbushPresent() const { return m_l14Siren.count() > 0; }

    // L15 Tree-City content: 3 vertical platforms (rising Y) + a trading post
    // prop. propEntity ids are exposed for the host's E-interact range check.
    uint32_t treeCityPlatformCount() const { return (uint32_t)m_treePlatforms.size(); }
    x3::phys::Vec3 treeCityPlatformPos(uint32_t i) const {
        return (i < m_treePlatforms.size()) ? m_treePlatformPos[i] : x3::phys::Vec3{};
    }
    bool tradingPostPresent() const { return m_tradingPostEntity != 0; }
    x3::phys::Vec3 tradingPostPos() const { return m_tradingPostPos; }

    // Story beats latched by onTrigger().
    bool l12Reached() const { return m_l12Reached; }
    bool l13Reached() const { return m_l13Reached; }
    bool l14Reached() const { return m_l14Reached; }
    bool l15Reached() const { return m_l15Reached; }
    bool memoryHunterArenaReached() const { return m_memHunterArenaReached; }

    // Reachability: L11 -> L12 -> L13 -> L14 -> L15 via labelled triggers.
    // True iff each link's trigger is registered + enabled + spatially between
    // the source area's exit and the destination's spawn (the player can cross
    // it to progress).
    bool transitionReachable(const TriggerSystem& triggers, Act2CavesTrigger link) const;
    bool allTransitionsReachable(const TriggerSystem& triggers) const;

private:
    bool        m_built = false;
    std::string m_modelDir;

    // Timeline flag for L14 Siren ambush. True (default) == F2 women lost ==
    // Siren placed at L14. The host flips this before build() based on the
    // Act-1 timeline; the test flips it and rebuilds to exercise both halves.
    bool        m_sirenGate = true;

    Act2CaveAreaPlan m_plan[kAct2CavesLevelCount];

    // L12 — Advanced Cave System
    MonsterManager m_l12Enemies;       // cave hostiles (NativeDesertFauna-tinted, etc.)
    MonsterManager m_l12Allies;        // Salvari Archives readers (allied markers)
    MonsterManager m_l12Boss;          // Memory Hunter (single boss in the abyss arena)
    CrystalHeartChamber m_crystalHeart; // dual-gated interactable + story flag
    x3::phys::Vec3 m_l12CaveExit{};    // edge of L12 (the L12->L13 threshold)
    x3::phys::Vec3 m_l12HeartRoomPos{}; // Crystal Heart Chamber center

    // L13 — Toxic Swamplands Edge
    MonsterManager   m_l13Enemies;     // mutated flora (stationary lash) + scientists
    PoisonHazardZone m_poison;         // poison hazard zone (inert at load)
    x3::phys::Vec3   m_l13SwampExit{}; // edge of L13 (the L13->L14 threshold)

    // L14 — Research Station
    MonsterManager m_l14Enemies;       // mutated scientists (always)
    MonsterManager m_l14Siren;         // Beta Siren ambush (only if sirenGate==true)
    x3::phys::Vec3 m_l14StationExit{}; // edge of L14 (the L14->L15 threshold)

    // L15 — Tree Cities
    std::vector<uint32_t>   m_treePlatforms;   // Scene entity ids of the 3 canopy slabs
    std::vector<x3::phys::Vec3> m_treePlatformPos;
    uint32_t                m_tradingPostEntity = 0;   // the trading-post pillar prop
    x3::phys::Vec3          m_tradingPostPos{};

    // Per-level spawn anchors (cached for transitionReachable).
    x3::phys::Vec3 m_l12Spawn{};
    x3::phys::Vec3 m_l13Spawn{};
    x3::phys::Vec3 m_l14Spawn{};
    x3::phys::Vec3 m_l15Spawn{};

    // Story beat latches.
    bool m_l12Reached = false;
    bool m_l13Reached = false;
    bool m_l14Reached = false;
    bool m_l15Reached = false;
    bool m_memHunterArenaReached = false;
};

// Headless self-test (--test-act2caves). Builds the Act-2 caves module on a
// HeadlessDevice + Jolt world TWICE (once with the L14 Siren-ambush timeline
// flag = TRUE [F2 women lost] and once with FALSE [women saved]) and asserts:
//   * L12..L15 build with the expected areas/footprints/objectives;
//   * L12 carries the Memory Hunter boss in the abyss arena;
//   * the Crystal Heart Chamber is INERT at load (neither gate set, not
//     activated, canActivate()==false), STAYS INERT when only one gate is
//     flipped, ARMS when BOTH gates are flipped (canActivate()==true), and on
//     activate() flips activated + storyBranch latches (idempotent on a 2nd call);
//   * the L13 poison hazard is PRESENT but INERT at load, STAYS INERT while
//     the player is outside, and ARMS + accumulates exposure once the player
//     is inside (mirrors the L9 hazard pattern);
//   * the L14 Siren ambush is TIMELINE-GATED: build flag=false (women saved)
//     => NO Siren placed; build flag=true (women unsaved) => Siren present;
//   * L11 -> L12 -> L13 -> L14 -> L15 are all reachable via their triggers,
//     each link enabled + spatially between source-exit and dest-spawn;
//   * the L15 Tree Cities expose 3 platforms at RISING heights + a trading-post
//     interactable prop;
//   * none of the act2_caves trigger ids collide with the Act-2 host's
//     L8/L9 trigger range (act2_world.* owns 80..82; this module's 100..108
//     is well clear).
// Prints "act2caves: X/Y passed"; returns true iff all pass. No window/Vulkan.
bool runAct2CavesSelfTest();

} // namespace x3::game
