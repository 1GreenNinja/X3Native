#pragma once
// DESC MECHANICS — TIER B + buildable TIER C (W9-2, docs/DESC_MECHANICS_TODO.md).
// Sibling of DescMechanics (W9-1, Tier A): same framework (Interactables +
// StoryFlags gates + the bark/status text paths), built/ticked BESIDE it so the
// W9-1 files stay frozen. Everything registers through Interactables — no new
// bespoke E-branches in the host beyond the single onUse() route.
//
//   #6  SALVARI ALLIES (F6 "Salvari Containment") — free-the-prisoners interact
//       at the cots -> 3 allied Salvari (SalvariAlly tuning + SalvariPrincess
//       GLB) follow Jake and emit periodic fire-support strikes at the nearest
//       hostile in range (CanonPlay::allyStrike + a host tracer). Flag
//       `f6.salvari_freed`. Goldenpath-safe: nothing exists until the interact.
//   #7  ENERGY NEXUS OVERLOAD (F6) — fusebox A -> fusebox B -> core, each gated
//       on the previous (requiresFlags). The core sets `f6.portal_sealed` and
//       raises the one-shot portal-glow-kill edge: the host kills the Portal
//       Chamber + Energy Nexus recipe lights (killRoomGlow) AND the dressed
//       emissives (RoomDressing::killRoomEmissives — the layered portal glass
//       dies). F6 has no reinforcement/wave spawner today, so the flag + barks
//       are the honest whole of "reinforcements stop".
//   #9  PROTOTYPE TESTING COURSE (F4) — position-trigger pair in the room (the
//       cold-room pattern): crossing the START marker arms a 45 s clock, the
//       hudStatusLine() carries the countdown, reaching the FINISH in time sets
//       `f4.course_beaten` + spawns the bonus cache (CanonPlay::spawnBonusCache).
//       Timeout -> failed; re-armable after stepping off the start marker.
//   #10 AUGMENTATION CHAIRS (F4) — three one-shot chair interacts (strength /
//       speed / armor); using ANY disables all three for the run (roguelite
//       choose-one). Effects surface as AugMods the host folds MULTIPLICATIVELY
//       into the W9-3 PlayerStatMods layer (applyRpgStats) — never applied at a
//       fire site directly, so skill effects can't double-apply.
//   #12 PORTAL CHAMBER minimal event — the seal's timed bark beat + the glow
//       death (part of #7; the full set-piece stays design-owned).
//   #13 FIRST CONTACT (F6) — a stationary Salvari speaker at the meeting circle;
//       E opens the authored chat tree (docs/design/narrative/chat_trees/
//       salvari_elder.json) through the SAME ChatTreeSystem path the girls and
//       VIGIL use (the host's chatTalkTarget gets an elder hook).
//   #14 CLONE-TANK PRE-SCENE (F7 Clone Lab) — one-shot proximity bark sequence
//       at the placed glass pane + cryo cot ("...that's ME in there"), flag
//       `f7.clone_seen` for the Clone boss bark to reference.
//   #15 COMMS BEACON (F7 Comms Center) — interact -> a timed radio-voice bark
//       chain (the evac tease) + `f7.beacon_activated`.
//   #16 MEMORY HOLO (F4 Neural Interface Lab) — a HoloPanel-style memory
//       station (the holo_terminal CPU text-on-glass bake, single-panel
//       variant): E cycles 4 victim-memory fragments re-baked onto the glass.
//
// Host seams (all flagged canonWorld && built()): build() after DescMechanics,
// tick() beside descMech.tick, onUse() one else-branch after descMech's, the
// bark/prompt/status text taps, draw() beside canonPlay.draw, the aug/portal
// one-shot edges, and the elder talk-target hook. Headless-testable:
// --test-descmech-bc (runDescMechBCSelfTest) drives every verb on a
// HeadlessDevice. Game/slice code only.

#include "interactables.h"
#include "canon_play.h"
#include "holo_terminal.h"

#include <string>
#include <vector>

namespace x3::game {

class DescMechanicsBC {
public:
    // Register the Tier-B/C mechanics onto the loaded tower. `floor`/`play`/
    // `flags` are host-owned and must outlive this object; `scene`/`device`/
    // `physics` are captured for the deferred ally/elder spawns + the memory
    // panel build. Rooms absent (single-floor load) register nothing for that
    // verb. Loaded-save flags are honored (aug chairs re-locked + re-applied,
    // portal already sealed = no re-edge). Returns true iff anything landed.
    bool build(const CanonFloor& floor, CanonPlay& play, StoryFlags& flags,
               Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, std::string_view modelDir);
    bool built() const { return m_floor != nullptr; }

    // Per-frame: ally follow + fire-support strikes, the course trigger pair +
    // clock, the clone-tank proximity scene, timed bark chains, and the memory
    // panel's shimmer/re-bake. `player` may be null (headless geometry ticks).
    void tick(float dt, const x3::phys::Vec3& eye, IDamageSink* player,
              Scene& scene, x3::phys::IPhysicsWorld& physics);

    // E-key hook (one else-branch after descMech.onUse in the host chain).
    bool onUse(const x3::phys::Vec3& eye, std::string* barkOut);

    // "[E] ..." HUD prompt for the nearest in-range point ("" when none).
    std::string prompt(const x3::phys::Vec3& eye) const { return m_points.prompt(eye); }

    // Queued bark from tick()/interacts. Pops one line per call ("" when none).
    std::string takeBark();

    // Status tag: the live course countdown ("COURSE 32s") + "SALVARI x3".
    // The host appends it to descMech.hudStatusLine()'s tag.
    std::string hudStatusLine() const;

    // Draw the allies + the elder (room-cull-friendly: they are few; the memory
    // panel draws itself through the Scene like every prop entity).
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
              const Scene& scene) const;

    // Ally tracer FX (host wires CombatFx::addTracer — same fn the enemies use).
    void setAttackFx(const AttackFxFn& fx) { m_attackFx = fx; }

    // ---- #10 aug chairs: the host-side stat fold ---------------------------
    // Additive-fraction contributions (the SkillNode vocabulary). The host folds
    // them MULTIPLICATIVELY over the skills/mods block in applyRpgStats:
    //   mods.damageMult *= 1 + aug.damageMult;  mods.speedMult *= 1 + aug.speedMult;
    //   mods.maxHpBonus += aug.maxHpBonus;
    struct AugMods { float damageMult = 0.0f, speedMult = 0.0f; int maxHpBonus = 0; };
    const AugMods& augMods() const { return m_aug; }
    // One-shot edge: true exactly once after a chair fires (or a loaded save
    // restored one) — the host re-runs applyRpgStats on it.
    bool augChangedPending();

    // ---- #7/#12 portal seal: the host-side glow kill -----------------------
    // One-shot edge after the core overload — the host kills the recipe lights
    // (killRoomGlow on portalRoom()+nexusRoom()) + the dressed emissives
    // (RoomDressing::killRoomEmissives on both).
    bool portalGlowKillPending();
    uint32_t portalRoom() const { return m_portalRoom; }
    uint32_t nexusRoom()  const { return m_nexusRoom; }

    // ---- #13 elder talk target (host chatTalkTarget hook) ------------------
    bool elderPresent() const { return m_elderSpawned; }
    x3::phys::Vec3 elderPos() const;

    // ---- #6 allies (test/diagnostic taps) -----------------------------------
    uint32_t allyCount() const { return m_allies.count(); }
    uint32_t allyAliveCount() const { return m_allies.aliveCount(); }
    uint32_t allyStrikesLanded() const { return m_allyStrikes; }
    MonsterManager& allies() { return m_allies; }

    // ---- #9 course (test taps) ----------------------------------------------
    enum class CourseState : uint32_t { Idle = 0, Running = 1, Cooldown = 2, Beaten = 3 };
    CourseState courseState() const { return m_course; }
    float courseClock() const { return m_courseT; }

    // ---- #16 memory panel (test taps) ---------------------------------------
    HoloTerminal&       memoryPanel()       { return m_memory; }
    const HoloTerminal& memoryPanel() const { return m_memory; }
    uint32_t memoryFragment() const { return m_memFragment; }

    Interactables& points() { return m_points; }

    // Tear down ally ragdolls/bodies BEFORE the physics world dies (mirrors
    // CanonPlay::shutdown). Idempotent.
    void shutdown();

    static constexpr float    kCourseSeconds     = 45.0f;
    static constexpr float    kAllyStrikePeriod  = 2.4f;   // s between fire-support ticks (per squad)
    static constexpr float    kAllyStrikeRadius  = 16.0f;  // hostile search reach from each ally
    static constexpr int      kAllyStrikeDamage  = 12;     // per strike (below Jake's 34/shot)
    static constexpr uint32_t kMemoryFragments   = 4;

private:
    void queueBark(std::string b, float delay = 0.0f);
    void spawnAllies(Scene& scene, x3::phys::IPhysicsWorld& physics);
    void spawnElder(Scene& scene, x3::phys::IPhysicsWorld& physics);
    void applyMemoryFragment(uint32_t idx);

    Interactables m_points;
    StoryFlags*       m_flags = nullptr;
    CanonPlay*        m_play  = nullptr;
    const CanonFloor* m_floor = nullptr;
    Scene*                     m_scene   = nullptr;
    x3::rhi::IRenderDevice*    m_device  = nullptr;
    x3::phys::IPhysicsWorld*   m_physics = nullptr;
    std::string m_modelDir;

    // #6 allies
    MonsterManager m_allies;
    bool     m_alliesPending = false;   // interact fired; spawn on the next tick
    bool     m_alliesSpawned = false;
    float    m_strikeT       = 0.0f;
    uint32_t m_nextStriker   = 0;       // round-robin so one ally fires per beat
    uint32_t m_allyStrikes   = 0;
    x3::phys::Vec3 m_cotPos[3]{};
    AttackFxFn m_attackFx;

    // #7/#12 portal seal
    uint32_t m_portalRoom = kNoRoom, m_nexusRoom = kNoRoom;
    bool     m_portalGlowKill = false;

    // #9 course
    uint32_t m_courseRoom = kNoRoom;
    x3::phys::Vec3 m_courseStart{}, m_courseFinish{};
    CourseState m_course = CourseState::Idle;
    float    m_courseT = 0.0f;
    bool     m_courseHint = false;   // one-shot room-entry hint bark

    // #10 aug chairs
    AugMods m_aug;
    bool    m_augChanged = false;
    bool    m_augApplied = false;   // a pick is live (build restore / interact / late sync)

    // #13 elder
    uint32_t m_contactRoom = kNoRoom;
    bool     m_elderPending = false, m_elderSpawned = false;
    int      m_elderIdx = -1;           // index into m_allies (shares the manager)

    // #14 clone tank
    uint32_t m_cloneRoom = kNoRoom;
    x3::phys::Vec3 m_tankPos{};
    bool     m_cloneSceneFired = false;

    // #16 memory holo
    uint32_t m_memRoom = kNoRoom;
    HoloTerminal m_memory;
    uint32_t m_memFragment = 0;
    bool     m_memBuilt = false;

    // barks: immediate queue + timed chain (delay-tagged)
    std::vector<std::string> m_barks;
    struct TimedBark { float t; std::string line; };
    std::vector<TimedBark> m_timed;
};

// Headless self-test (--test-descmech-bc): ally spawn + follow + strike, nexus
// chain gating (B-before-A refuses), course win/lose + reward cache, aug chair
// exclusivity + the stat fold, the salvari_elder tree loads/validates/walks,
// the clone-tank scene, the beacon chain, and the memory-panel bake cycling.
// Skips-as-PASS without the canonical JSON. Logs PASS/FAIL B#, returns true
// iff all pass. No window / Vulkan.
bool runDescMechBCSelfTest();

} // namespace x3::game
