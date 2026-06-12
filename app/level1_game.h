#pragma once
// EFLZ Level 1 game controller — wires the §3 beat sequence on top of the Level 1
// graybox (level1.*), the door/weapon/monster/objective/trigger systems.
//
// Game/slice code only — engine/ stays pure. This is the single source of truth
// for Level 1 gameplay logic so BOTH the interactive host (app/main.cpp) and the
// headless --test-level1 self-test drive identical beats. The host feeds it
// per-frame input edges + the player position; it advances doors, runs triggers,
// spawns/clears enemies, flips objectives, and reports level completion.
//
// Beats wired (spec §3):
//   0  spawn: objective "Escape the detention cell"
//   1  strength trigger -> hide the "equipment" prop (strength discovery)
//   2  use Door A button -> Door A opens
//   3  Door A reaches Open -> spawn corridor enemies (2 guards + 1 drone),
//        objective -> "Find weapons in the armory"
//   5  use Door B button -> Door B opens
//   6  walk onto the armory pickup -> armed, objective -> "Reach the elevator..."
//   7  Door C is LOCKED until armed; unlocks the frame the player becomes armed
//   8  checkpoint guards (spawned at build, in the checkpoint room)
//   9  cross the arena trigger -> Door D opens + spawn Martinez (boss-tier)
//   10 Martinez dies -> Door E unlocks + opens, objective -> "Take the elevator"
//   11 enter the elevator trigger (armed gate cleared by progress) -> WIN:
//        log "LEVEL 1 COMPLETE" exactly once + set completed state

#include "scene.h"
#include "door.h"
#include "weapon.h"
#include "monster.h"
#include "melee.h"
#include "barrels.h"   // explosive barrels (shoot -> violent detonation + chain)
#include "objective.h"
#include "trigger.h"
#include "rescue.h"
#include "level1.h"
#include "env_art.h"
#include "secret_room.h"   // code-locked trapdoor -> secret room (cell HoloTerminal)

#include "save.h"      // engine-general checkpoint schema (the bridge maps onto it)

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/audio/IAudioSystem.h"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace x3::game {

// Optional audio hookups for Level 1 events (§9, nice-to-have). All fields may be
// null/invalid; the controller plays a sound only when both the system and the
// handle are valid (the audio system is itself graceful/silent without a device).
struct Level1Audio {
    x3::audio::IAudioSystem* sys    = nullptr;
    x3::audio::SoundHandle   door{};
    x3::audio::SoundHandle   pickup{};
    x3::audio::SoundHandle   gun{};
    x3::audio::SoundHandle   death{};
};

// Trigger event ids (TriggerVolume::id) used by Level 1.
enum class L1Trigger : uint32_t {
    Strength = 1,   // beat 1: hide equipment, strength discovery
    Arena    = 2,   // beat 9: spawn Martinez + open Door D
    Elevator = 3,   // beat 11: win
    Hub      = 4,   // playtest-fix: F2 ward hub reached -> start the rescue timers
};

// Pure keypad-entry buffer for the door-code keypad (no I/O, no Vulkan/GLFW). The
// host (app/main.cpp) maps GLFW key edges onto these calls; the headless
// --test-doorcode self-test drives the SAME state machine directly so both stay in
// lockstep. A digit appends (capped at maxLen); backspace removes the last digit;
// clear() resets. value() parses the buffer to an int (-1 when empty).
struct KeypadEntry {
    static constexpr int kMaxLen = 6;   // matches the host's 6-digit cap
    std::string buf;

    void pushDigit(int d) {             // d in [0..9]; ignored otherwise / when full
        if (d < 0 || d > 9) return;
        if ((int)buf.size() >= kMaxLen) return;
        buf += char('0' + d);
    }
    void backspace() { if (!buf.empty()) buf.pop_back(); }
    void clear()     { buf.clear(); }
    bool empty() const { return buf.empty(); }
    // Parsed numeric code, or -1 when nothing has been entered yet.
    int value() const { return buf.empty() ? -1 : std::atoi(buf.c_str()); }
    // HUD prompt string: "DOOR LOCKED   ENTER CODE: 11_" etc.
    std::string prompt() const { return "DOOR LOCKED   ENTER CODE: " + buf + "_"; }
};

class Level1Game {
public:
    // Build the whole level: graybox geometry, doors A-E, the armory pistol pickup,
    // checkpoint guards, the strength/arena/elevator trigger volumes, and the
    // objective list. Corridor enemies + Martinez spawn LATER on their beats. The
    // `modelDir` is the loose-GLB dir for the pistol + crawler models. Call once.
    void build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, std::string_view modelDir);

    // Optional: attach audio for event SFX (§9). Safe to skip (silent).
    void setAudio(const Level1Audio& audio) { m_audio = audio; }

    // Optional: wire a game-feel cue sink (footstep / impact) onto every enemy
    // group — current AND future spawns (corridor / checkpoint / Martinez / boss
    // adds / Chen). Empty => the per-monster throttled-log stub. The host maps cues
    // onto audio/FX. Stored so on-beat spawns inherit it; call after build(). See cues.h.
    void setCueSink(const GameCueFn& sink);

    // Optional: wire a DEATH FX sink (gib burst) onto every enemy group — current AND
    // future spawns (corridor / checkpoint / Martinez / boss adds / Chen). The monster
    // fires it ONCE the instant it is KILLED (HP->0), passing its body-center world
    // position + a flying flag. The host turns it into the gib explosion (GPU debris +
    // blood). Empty => no extra death FX (the topple/corpse path is unchanged). Stored
    // so on-beat spawns inherit it; call after build(). See app/monster.h DeathFxFn.
    void setDeathFxSink(const DeathFxFn& sink);

    // Cache the render device so tick() can spawn enemies on their beats (those
    // need to create GPU meshes for the crawler model). build() also records it,
    // but this lets the host/test set it explicitly. The pointer must outlive the
    // controller (the host owns the device).
    void setDevice(x3::rhi::IRenderDevice& device) { m_devicePtr = &device; }

    // Advance one frame of Level 1 logic. `eye` is the camera/eye world position,
    // `playerPos` the position used for triggers/pickup (feet or eye — XZ is what
    // matters). Doors + monsters + pickup + triggers are all stepped here; the host
    // still calls physics->step() and scene.update() around this (see main loop).
    //
    // `player` (Phase 2a) is the damage sink enemies attack — pass the Player so
    // guards/drone/Martinez can hurt it. May be null (no combat damage; legacy /
    // headless geometry tests). `attackFx`, if set, spawns a visible attack beam
    // per enemy attack (drone tracer / melee tell); the host wires it to CombatFx.
    void tick(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
              const x3::phys::Vec3& eye, const x3::phys::Vec3& playerPos,
              IDamageSink* player, const AttackFxFn& attackFx);

    // Legacy overload (no combat damage): forwards with a null player / empty fx.
    // Keeps the geometry-only --test-level1 + old call sites unchanged.
    void tick(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
              const x3::phys::Vec3& eye, const x3::phys::Vec3& playerPos);

    // Handle a "use" press (rising edge) aimed along `dir` from `eye`: opens a
    // door if the ray hits a button linked to an unlocked door. Returns true if a
    // door began opening. (Door C refuses until armed; that is enforced by lock.)
    bool onUse(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
               Scene& scene, x3::phys::IPhysicsWorld& physics);

    // Interaction-prompt query (HUD): is the player aiming at a door (or its
    // linked button) within `reach` that E can toggle? If so, fill `anchor` (a
    // stable world point at the doorway) and `isOpen` (true when Open/Opening ->
    // the prompt reads "close") and return true. A locked, still-closed door
    // returns false (the keypad path owns that). Pure query.
    bool aimedDoorPrompt(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                         Scene& scene, x3::phys::IPhysicsWorld& physics,
                         float reach, x3::phys::Vec3& anchor, bool& isOpen);

    // F2 rescue interact (spec §5): try to rescue the nearest captive within range
    // of `playerPos` (E-interact). Returns true iff a victim was rescued (the host
    // logs / plays SFX). Small additive hook around the existing onUse path.
    bool onRescue(const x3::phys::Vec3& playerPos, float range = kRescueReach);

    // Door-code keypad (§6.4 keypad gate): true if the player is within `range` of a
    // LOCKED door carrying a keypad code (gates the host's code-entry mode).
    bool nearLockedCodedDoor(const x3::phys::Vec3& playerPos, float range = 3.5f) const;
    // Submit a keypad `code`: unlock + open the nearest locked coded door in range whose
    // code matches. Returns true if a door began opening.
    bool tryDoorCode(const x3::phys::Vec3& playerPos, int code, float range = 3.5f);

    // Handle a FIRE press (rising edge) along `dir` from `eye`: only effective when
    // armed. Damages the first live monster the ray hits. Returns the result so the
    // host can spawn FX. No-op (default miss) when not armed.
    FireResult onFire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                      Scene& scene, x3::phys::IPhysicsWorld& physics,
                      int damage = kDamagePerShot);

    // Handle a MELEE press (rising edge) along `dir` from `eye` — the super-strength
    // punch (Phase 2b). Damages + knocks back every live enemy across all Level-1
    // enemy groups (corridor / checkpoint / Martinez) in the forward arc, and
    // brute-forces a closed door the punch is aimed at. Works whether or not the
    // player is armed (it is the unarmed-strength verb; the pistol is onFire).
    // Returns the result so the host can spawn the melee swing FX + SFX.
    MeleeResult onMelee(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                        Scene& scene, x3::phys::IPhysicsWorld& physics);

    // ---- Draw helpers (host calls inside beginFrame/endFrame) --------------
    void drawWorldExtras(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                         const Scene& scene) const;  // pickup + all monsters
    // Draw the real SM_Door_A slab at every door's CURRENT (animating) world
    // transform. The procedural door box is collision-only (hidden); this draws the
    // visual. Host calls it once per frame in the interactive draw block.
    void drawDoors(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const {
        m_doors.drawMeshes(device, frame);
    }
    void drawViewmodel(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                       float ex, float ey, float ez, float yaw, float pitch,
                       float yawOff, float pitchOff, float rollOff,
                       float fwd, float right, float down) const;
    void drawObjective(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const;

    // ---- Queries (host HUD + the self-test) -------------------------------
    const Level1Layout& layout() const { return m_layout; }

    // Respawn checkpoint (Phase 2a): the player respawns here (feet position) at
    // full HP after death. The simplest checkpoint is the level-start cell spawn;
    // documented in the header note below. Enemies are NOT reset on respawn —
    // killed enemies stay dead and survivors keep their state, so the player picks
    // the fight back up where they left off (a deliberate, documented choice).
    x3::phys::Vec3 checkpoint() const { return m_layout.spawn; }

    bool armed() const { return m_weapon.hasWeapon(); }
    // IDKFA/IDFA cheat: force-arm the player (gives the weapon; the arsenal weapons
    // start full, so this enables firing the whole arsenal). See app/main.cpp cmds.
    void cheatArm(Scene& scene);
    bool complete() const { return m_complete; }
    // Save/load restore: set the level-complete latch directly (does NOT re-run the
    // WIN beat / re-log). Used by applyCheckpoint() to restore the recorded flag.
    void setComplete(bool c) { m_complete = c; }
    ObjectiveSystem&       objectives()       { return m_objectives; }
    const ObjectiveSystem& objectives() const { return m_objectives; }

    // Door access by the spec letter (A=0..E=4) for the self-test / HUD.
    DoorState doorState(char letter) const;
    bool      doorLocked(char letter) const;

    // ---- LIVING WORLD alert hook (pillar 3) --------------------------------
    // Mutable door registry so the facility AlertSystem's AlertDoorLock can run
    // the level-3 LOCKDOWN over the real Level-1 doors (locks closed doors,
    // restores exactly its own locks on release — see app/alert.h).
    DoorSystem&       doors()       { return m_doors; }
    const DoorSystem& doors() const { return m_doors; }

    // Monster groups (for the self-test / objective gating + the save bridge).
    BarrelSystem&         barrels()                 { return m_barrels; }   // host wires FX/damage sinks
    // Static ceiling-fixture point lights (env-art Light_A). The host re-issues these
    // + a player FLASHLIGHT each frame so the light follows the player through dark halls.
    const std::vector<x3::rhi::PointLight>& lightFixtures() const { return m_envArt.lightFixtures(); }
    MonsterManager&       corridorEnemies()         { return m_corridor; }
    const MonsterManager& corridorEnemies()   const { return m_corridor; }
    MonsterManager&       checkpointEnemies()       { return m_checkpoint; }
    const MonsterManager& checkpointEnemies() const { return m_checkpoint; }
    bool martinezSpawned() const { return m_martinezSpawned; }
    bool martinezAlive()   const { return m_martinezSpawned && m_martinez.alive(); }
    bool martinezDead()    const { return m_martinezSpawned && !m_martinez.alive(); }

    // ---- Boss phase HUD (Phase 2b) ----------------------------------------
    // The boss's current phase (Phase1 until it spawns). Drives a boss HP/phase
    // HUD element if the host wants one.
    BossPhase martinezPhase() const { return m_martinezSpawned ? m_martinez.phase() : BossPhase::Phase1; }
    int  martinezHp()  const { return m_martinezSpawned ? m_martinez.hp() : 0; }
    int  martinezMaxHp() const { return m_martinezSpawned ? m_martinez.maxHp() : 0; }
    // Banner text + remaining time for the "PHASE 2!/PHASE 3!" HUD flash, set when
    // the boss enters a new phase and decayed by tick(). Empty / 0 when inactive.
    const std::string& phaseBanner() const { return m_phaseBanner; }
    float phaseBannerTime() const { return m_phaseBannerTimer; }

    // The weapon system (host reads usingRealModel() for logging).
    const WeaponSystem& weapon() const { return m_weapon; }

    // ---- Code-locked trapdoor -> SECRET ROOM (cell HoloTerminal) -----------
    // The secret-room feature: the cell holographic terminal, the floor-hatch
    // trapdoor (opened by the override code 1127), and the stocked room below.
    // The host (main.cpp) routes typed chars into secret().terminal() and draws
    // its readout/input over the panel; tick() ticks its blink + pickup logic.
    SecretRoom&       secret()       { return m_secretRoom; }
    const SecretRoom& secret() const { return m_secretRoom; }

    // ---- F2 rescue system (spec §5) ---------------------------------------
    // The rescue system (3 victims on 5-min timers; rescue -> companion, expire ->
    // boss). The host pokes tryRescue() on an E-interact edge and reads hudTimers()
    // for the HUD. Exposed mutable + const so main.cpp can drive + draw it.
    RescueSystem&       rescue()       { return m_rescue; }
    const RescueSystem& rescue() const { return m_rescue; }

    // ---- F2 Medical Bay boss: Dr. Chen (Corrupted) — Wave-2 placement ------
    // The F2 floor boss, placed ALONGSIDE the 3-victim rescue (the Medical Bay floor).
    // A single-body Boss via the Wave-1 roster (bossTuning(BossType::DrChen)): 3 phases
    // + the KILL-vs-CURE outcome (incapacitate+cure spares him -> 100% cure ally). His
    // own manager so his role/phase are distinct from the rescue victims' transformed
    // bosses. Spawned on the F2 plate gated on the F2 ward hub (NOT at load), mirroring
    // the rescue-clock gating, so he doesn't pursue a player who hasn't reached F2.
    const MonsterManager& chen() const { return m_chen; }
    MonsterManager&       chen()       { return m_chen; }

    // ---- TEARDOWN (death-ragdoll safety) ----------------------------------
    // Tear down every monster group's in-flight death ragdoll (and the single
    // Martinez boss + Phase-3 adds) while the physics world is STILL ALIVE. The
    // host (and the --test-level1 self-test) MUST call this BEFORE physics->
    // shutdown(): a boss/enemy killed late leaves a skinned death ragdoll whose
    // Jolt bodies are removed in ~MonsterSystem -> IRagdoll::removeFromWorld();
    // if the Jolt world was already shut down that is an access violation
    // (Release) / a Jolt assert (Debug). Mirrors MonsterManager::shutdown() +
    // SpireNexus's body-owner scoping. Idempotent and harmless if nothing died.
    void shutdown();

    // Total LIVE hostiles across every group (HUD "ENEMIES" counter). Includes the
    // Phase-3 boss adds + the bosses so the count never reads "AREA CLEAR" while
    // something is still alive (the bossAdds bug).
    int enemiesRemaining() const {
        int n = (int)(m_corridor.aliveCount() + m_checkpoint.aliveCount()
                    + m_bossAdds.aliveCount());
        if (m_martinezSpawned && m_martinez.alive()) n += 1;
        if (m_chenSpawned) n += (int)m_chen.aliveCount();
        return n;
    }
    bool chenSpawned() const { return m_chenSpawned; }
    bool chenAlive()   const { return m_chenSpawned && m_chen.count() > 0 && m_chen.at(0).alive(); }
    // True iff Chen is in his curable window (Phase3 "Monster"); the host offers the
    // "incapacitate + cure" prompt then. Drives the F2 KILL-vs-CURE choice.
    bool chenCanCure() const { return m_chenSpawned && m_chen.count() > 0 && m_chen.at(0).canCure(); }
    bool chenCured()   const { return m_chenSpawned && m_chen.count() > 0 && m_chen.at(0).wasCured(); }
    // CURE / spare Chen (vs. killing him). No-op unless chenCanCure(). Returns true if
    // cured. The host wires this to a "cure" interact at the downed-to-Phase3 boss.
    bool cureChen(Scene& scene, x3::phys::IPhysicsWorld& physics);

    // The canon F2 floor identity ("Medical Bay") for the HUD / objective text.
    const char* f2FloorName() const { return "Medical Bay"; }

    // ---- HUD radar/nameplate feed (read-only world-position enumeration) ----
    // One spot a hostile occupies on the radar / under a nameplate.
    struct EnemyMark {
        x3::phys::Vec3 pos;     // body-center world position
        const char*    label;   // short threat label (enemy type / boss name)
    };
    // Write up to `cap` LIVE hostile positions+labels (corridor guards/drone,
    // checkpoint guards, Phase-3 boss adds, Martinez, Dr. Chen) into `out`; returns
    // the count written. Read-only: enumerates the existing monster groups, no state
    // change. The host (main.cpp) feeds these into the HUD radar + enemy nameplates.
    uint32_t liveEnemyMarks(EnemyMark* out, uint32_t cap) const {
        uint32_t n = 0;
        auto add = [&](const x3::phys::Vec3& p, const char* lbl) {
            if (n < cap) { out[n].pos = p; out[n].label = lbl; ++n; }
        };
        auto addManager = [&](const MonsterManager& mm, const char* lbl) {
            for (uint32_t i = 0; i < mm.count() && n < cap; ++i)
                if (mm.at(i).alive()) add(mm.at(i).pos(), lbl);
        };
        addManager(m_corridor,   "HOSTILE");
        addManager(m_checkpoint, "GUARD");
        addManager(m_bossAdds,   "ADD");
        if (m_martinezSpawned && m_martinez.alive()) add(m_martinez.pos(), "MARTINEZ");
        if (m_chenSpawned) addManager(m_chen, "DR. CHEN");
        return n;
    }
    // Write up to `cap` LIVE COMPANION (rescued victim) world positions into `out`;
    // returns the count written. Read-only enumeration of the rescue system.
    uint32_t liveCompanionPositions(x3::phys::Vec3* out, uint32_t cap) const {
        uint32_t n = 0;
        for (uint32_t i = 0; i < m_rescue.victimCount() && n < cap; ++i) {
            const RescueVictim& v = m_rescue.victim(i);
            if (v.companion()) out[n++] = v.pos();
        }
        return n;
    }

private:
    // Map a door letter to its DoorSystem index (set in build()).
    uint32_t doorIndex(char letter) const;

    // Spawn the corridor enemies (beat 3) / Martinez (beat 9). Idempotent guards.
    void spawnCorridorEnemies(Scene& scene, x3::rhi::IRenderDevice& device,
                              x3::phys::IPhysicsWorld& physics);
    void spawnMartinez(Scene& scene, x3::rhi::IRenderDevice& device,
                       x3::phys::IPhysicsWorld& physics);
    // F2 Medical Bay boss: spawn Dr. Chen on the F2 plate (gated on the F2 hub). Idempotent.
    void spawnChen(Scene& scene, x3::rhi::IRenderDevice& device,
                   x3::phys::IPhysicsWorld& physics);
    // Phase 2b: summon the boss's Phase-3 Guard adds (once). Idempotent guard.
    void spawnBossAdds(Scene& scene, x3::rhi::IRenderDevice& device,
                       x3::phys::IPhysicsWorld& physics);

    void playSfx(x3::audio::SoundHandle h, const x3::phys::Vec3& at, float vol);

    Level1Layout   m_layout;
    EnvArtSystem   m_envArt;       // converted sci-fi GLB visuals over the graybox
    Level1ArtMask  m_artMask;      // which graybox surfaces real art covers
    DoorSystem     m_doors;
    WeaponSystem   m_weapon;
    MeleeSystem    m_melee;        // super-strength punch (Phase 2b)
    BarrelSystem   m_barrels;      // explosive barrels (shoot -> detonate + chain)
    MonsterManager m_corridor;     // 2 guards + 1 drone (beat 3)
    MonsterManager m_checkpoint;   // 1-2 guards (built at level build)
    MonsterSystem  m_martinez;     // boss (beat 9)
    MonsterManager m_bossAdds;     // Phase 3 summoned Guard adds (Phase 2b)
    ObjectiveSystem m_objectives;
    TriggerSystem  m_triggers;
    RescueSystem   m_rescue;       // F2 victims (Aria/Keisha/Emily) — spec §5
    SecretRoom     m_secretRoom;   // code-locked trapdoor -> secret room (cell HoloTerminal)
    MonsterManager m_chen;         // F2 Medical Bay boss: Dr. Chen (Wave-2; gated on the F2 hub)
    bool           m_chenSpawned = false;  // Dr. Chen placed on the F2 plate (on the F2 hub)

    // Boss phase HUD flash (Phase 2b): banner text + countdown set on a transition.
    std::string    m_phaseBanner;          // "PHASE 2!" / "PHASE 3!" (empty = none)
    float          m_phaseBannerTimer = 0.0f;
    bool           m_bossSummoned = false;  // Phase 3 adds spawned once

    Level1Audio    m_audio;
    GameCueFn      m_cueSink;       // game-feel footstep/impact sink (fanned to enemies)
    DeathFxFn      m_deathFx;       // gib-burst death FX sink (fanned to enemies)
    std::string    m_modelDir;
    x3::rhi::IRenderDevice* m_devicePtr = nullptr; // cached for event-time spawns

    // Door indices in m_doors for letters A..E (kNoLink until built).
    uint32_t m_doorIdx[5] = { kNoLink, kNoLink, kNoLink, kNoLink, kNoLink };

    // Beat latches.
    bool m_doorAWasOpen     = false; // beat 3: corridor spawn on Door A reaching Open
    bool m_corridorSpawned  = false;
    bool m_armedLatch       = false; // beat 7: unlock Door C the frame we arm
    bool m_checkpointClearLatch = false; // checkpoint cleared -> objective -> boss
    bool m_martinezSpawned  = false; // beat 9
    bool m_martinezDeadLatch= false; // beat 10: unlock+open Door E once
    bool m_complete         = false; // beat 11: WIN (logged once)
    bool m_built            = false;
};

// ===========================================================================
// EFLZ <-> save::SaveState BRIDGE (programmatic save/load API the host calls)
// ===========================================================================
// These map the LIVE EFLZ game systems onto the engine-general save::SaveState (and
// back). save.h itself stays game-agnostic (plain PODs + file I/O); this is the
// game-layer adapter. The host (app/main.cpp) calls captureCheckpoint() to snapshot
// then save::saveCheckpoint(), and on load calls save::loadCheckpoint() then
// applyCheckpoint().
//
// WHAT APPLY RESTORES (a pragmatic, resumable checkpoint):
//   * player feet transform + look angles + HP        (directly re-seated)
//   * arsenal: equipped weapon + per-weapon ammo/reserve
//   * objective cursor + level-complete flag
//   * rescue: hub-reached + every victim's lifecycle + remaining timer
//   * the recorded current floor (the host re-positions the elevator to it)
// The per-floor world FLAGS (doors opened / keypads solved / enemies cleared) are
// always CAPTURED + persisted (and exposed for HUD / "% cleared"); apply does NOT
// retroactively re-open doors or re-kill enemies in an already-built live level (that
// would mean re-simulating the beat graph) — a documented checkpoint limitation. The
// recorded flags are still authoritative for a future "rebuild the level at this
// progress" path. The round-trip of the SaveState struct itself is fully covered by
// --test-saveload.

class SpireMidFloors;   // fwd (spire_mid.h)
class SpireTopFloors;   // fwd (spire_top.h)
class Arsenal;          // fwd (weapon.h — already included, but keep symmetry)
class Player;           // fwd (player.h)

// Snapshot the live game into a save::SaveState. `currentFloor` is the L1Floor index
// the player is on (the host derives it from the elevator's current stop). The Spire
// floor systems may be unbuilt (built()==false) — their flags are simply omitted.
x3::save::SaveState captureCheckpoint(const Player& player, const Arsenal& arsenal,
                                      const Level1Game& game,
                                      const SpireMidFloors& mid, const SpireTopFloors& top,
                                      uint32_t currentFloor);

// Apply a loaded save::SaveState back onto the live game. Restores the player
// transform/health (re-seating the capsule via `physics`), the arsenal, the objective
// cursor + complete flag, and the rescue lifecycle/timers/hub. `outCurrentFloor`
// receives the recorded floor so the host can move the elevator. Returns true (the
// SaveState is assumed already validated by save::loadCheckpoint).
bool applyCheckpoint(const x3::save::SaveState& s, Player& player,
                     x3::phys::IPhysicsWorld& physics, Arsenal& arsenal,
                     Level1Game& game, SpireMidFloors& mid, SpireTopFloors& top,
                     uint32_t& outCurrentFloor);

// Headless self-test (--test-level1). Builds Level 1 on a HeadlessDevice + Jolt
// world, drives the player position + synthetic use/fire through the controller,
// and asserts T1-T6 (+ T7 objective flow) from spec §7. Logs PASS/FAIL T#, returns
// true iff all pass. No window/Vulkan. Mirrors runInteractSelfTest/runCombatSelfTest.
bool runLevel1SelfTest();

// Headless self-test (--test-doorcode). Builds Level 1 and exercises the keypad-
// coded locked door (Door C, code 1127): the wrong code does NOT open it, the right
// code DOES, and the host-side keypad state machine (digit entry / backspace /
// cancel) builds the entered code correctly. Logs PASS/FAIL K#, returns true iff
// all pass. No window/Vulkan.
bool runDoorCodeSelfTest();

// Headless self-test (--test-elevator). Builds an ElevatorSystem on a HeadlessDevice
// + Jolt world with two stops, then asserts E1-E6: it starts idle at the low stop;
// callNext() begins moving; update() carries the cab up and arrives at the high stop;
// playerRiding() detects feet on/off the cab footprint; calling again returns it to
// the low stop; the cab body tracks the cab center (mediates the level exit lift).
// Logs PASS/FAIL E#, returns true iff all pass. No window/Vulkan.
bool runElevatorSelfTest();

} // namespace x3::game
