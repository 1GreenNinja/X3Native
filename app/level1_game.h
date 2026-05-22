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
#include "objective.h"
#include "trigger.h"
#include "level1.h"
#include "env_art.h"

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
                      Scene& scene, x3::phys::IPhysicsWorld& physics);

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
    bool complete() const { return m_complete; }
    ObjectiveSystem&       objectives()       { return m_objectives; }
    const ObjectiveSystem& objectives() const { return m_objectives; }

    // Door access by the spec letter (A=0..E=4) for the self-test / HUD.
    DoorState doorState(char letter) const;
    bool      doorLocked(char letter) const;

    // Monster groups (for the self-test / objective gating).
    MonsterManager&       corridorEnemies()       { return m_corridor; }
    MonsterManager&       checkpointEnemies()     { return m_checkpoint; }
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

private:
    // Map a door letter to its DoorSystem index (set in build()).
    uint32_t doorIndex(char letter) const;

    // Spawn the corridor enemies (beat 3) / Martinez (beat 9). Idempotent guards.
    void spawnCorridorEnemies(Scene& scene, x3::rhi::IRenderDevice& device,
                              x3::phys::IPhysicsWorld& physics);
    void spawnMartinez(Scene& scene, x3::rhi::IRenderDevice& device,
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
    MonsterManager m_corridor;     // 2 guards + 1 drone (beat 3)
    MonsterManager m_checkpoint;   // 1-2 guards (built at level build)
    MonsterSystem  m_martinez;     // boss (beat 9)
    MonsterManager m_bossAdds;     // Phase 3 summoned Guard adds (Phase 2b)
    ObjectiveSystem m_objectives;
    TriggerSystem  m_triggers;

    // Boss phase HUD flash (Phase 2b): banner text + countdown set on a transition.
    std::string    m_phaseBanner;          // "PHASE 2!" / "PHASE 3!" (empty = none)
    float          m_phaseBannerTimer = 0.0f;
    bool           m_bossSummoned = false;  // Phase 3 adds spawned once

    Level1Audio    m_audio;
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
