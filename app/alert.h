#pragma once
// FACILITY ALERT LEVEL — the wanted system (living-world pillar 3).
// Game/slice code only — engine/ stays pure.
//
// A 0-4 facility-wide alert state machine driven by a HEAT economy. Stimuli the
// host reports (gunshots heard by guards, bodies discovered on patrol paths,
// LOS witness sightings, terminal hacks) add heat and impose LEVEL FLOORS; the
// level rises instantly through the heat thresholds and DECAYS while the player
// hides — after a quiet grace period (no stimuli, out of every guard's LOS) the
// heat drains at a per-level rate, stepping the level back down through a
// hysteresis band. So each level has an effective hide-out duration, exactly
// the "lay low until the heat dies" loop.
//
//   LEVEL 0  CALM        — nothing pending.
//   LEVEL 1  INVESTIGATE — something was heard; patrols route to the stimulus
//                          (AmbientEcology::commandInvestigate via the popped
//                          investigate position).
//   LEVEL 2  SEARCH      — a body was found / the player was seen: extra guards
//                          spawn (takeSpawnRequests), patrol speed tightens.
//   LEVEL 3  LOCKDOWN    — zone doors LOCK (AlertDoorLock / Level1Game's
//                          setAlertLockdown), the alarm flag raises, lights
//                          shift red (redShift()).
//   LEVEL 4  KILL SQUAD  — a kill squad spawns (takeSpawnRequests) and hunts.
//
// Every transition fires the event hook in the engine's scripting convention:
//   x3.fire("alert_changed",{level=N})   (logged; + an optional C++ sink for
// Lua/mission wiring when the trigger-script chain lands).
//
// PURITY: the system itself touches no Scene/physics/render state — the host
// feeds observations (observer positions, a player-seen flag) into update() and
// applies the effects it reads back. That keeps the whole machine headlessly
// testable (--test-alert) and reusable by any floor/world. Tunables load from
// assets/world/alert.json (json_mini.h); a missing file uses the defaults.

#include "door.h"   // AlertDoorLock locks/restores a DoorSystem on lockdown

#include "engine/physics/IPhysicsWorld.h"   // x3::phys::Vec3

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace x3::game {

// Tunables (assets/world/alert.json). Defaults are the shipped balance.
struct AlertConfig {
    // ---- Heat economy ----
    float heatGunshot       = 30.0f;   // a gunshot a guard HEARD
    float heatCorpse        = 60.0f;   // a body discovered by a guard
    float heatWitness       = 50.0f;   // a fresh LOS sighting engagement
    float heatTerminalHack  = 40.0f;   // a keypad/terminal tamper
    float heatWitnessPerSec = 22.0f;   // sustained LOS while alerted
    // Level thresholds: heat >= up[k] => at least level k+1. Hysteresis: the
    // level only drops back below level k+1 once heat <= up[k] * downFrac.
    float up[4]             = { 25.0f, 75.0f, 150.0f, 240.0f };
    float downFrac          = 0.60f;
    float heatMax           = 320.0f;
    // ---- Stimulus floors (a stimulus guarantees at least this level) ----
    int   floorGunshot      = 1;
    int   floorHack         = 1;
    int   floorCorpse       = 2;
    int   floorWitness      = 2;
    // ---- Perception radii ----
    float gunshotRadius     = 40.0f;   // a guard within this HEARS the shot
    float corpseRadius      = 6.0f;    // a guard within this of a body FINDS it
    // ---- Decay (the hide-out loop) ----
    float decayDelay        = 4.0f;    // quiet+unseen seconds before draining
    float decayPerSec[5]    = { 0.0f, 5.0f, 8.0f, 10.0f, 12.0f };
    // ---- Effects ----
    int   searchSpawns      = 2;       // extra guards on entering level 2
    int   killSquadSpawns   = 3;       // squad size on entering level 4
    float patrolSpeedMul[5] = { 1.0f, 1.25f, 1.6f, 1.6f, 2.0f };
};

// Built-in defaults (== the shipped assets/world/alert.json).
AlertConfig defaultAlertConfig();
// Load tunables from JSON (missing/unparseable -> defaults, logged).
AlertConfig loadAlertConfig(std::string_view jsonPath);
// Canonical on-disk path: <assetRoot>/world/alert.json.
std::string alertJsonPath();

// Level-change hook: fired once per transition, AFTER the internal state moved.
using AlertChangedFn = std::function<void(int newLevel, int oldLevel)>;

// Human-readable level name ("CALM" / "INVESTIGATE" / ... ). Clamped.
const char* alertLevelName(int level);

class AlertSystem {
public:
    void configure(const AlertConfig& cfg) { m_cfg = cfg; }
    const AlertConfig& config() const { return m_cfg; }

    // ---- Stimuli (host events; resolved against observers in update()) ----
    // A gunshot rang out at `pos`. It only raises the alert if a guard is close
    // enough to HEAR it (gunshotRadius at the next update — witness-vs-unseen).
    void reportGunshot(const x3::phys::Vec3& pos);
    // WEAPON ATTACHMENTS: the same shot, but this weapon is SUPPRESSED (or louder).
    // `noiseMult` scales gunshotRadius for THIS shot only — a suppressor (0.30) is
    // heard at 12 m instead of 40 m, so guards genuinely notice you later; a kinetic
    // sheath (1.35) carries further. This is the effective WeaponDef::noiseMult.
    void reportGunshot(const x3::phys::Vec3& pos, float noiseMult);
    // A keypad/terminal was tampered with at `pos` (always noticed — the system
    // logs the tamper itself).
    void reportTerminalHack(const x3::phys::Vec3& pos);
    // A body now lies at `pos`. Deduped (ignored within 1.5 m of a known body).
    // It raises the alert only when a guard PATROLS WITHIN corpseRadius of it.
    void registerCorpse(const x3::phys::Vec3& pos);

    // ---- Per-frame ----
    // `observers` are the LIVE guard/patrol positions (the facility's eyes and
    // ears); `playerSeen` is true iff any of them has LOS to the player THIS
    // frame (the host computes LOS — e.g. MonsterSystem::hasLineOfSight()).
    void update(float dt, const x3::phys::Vec3& playerPos,
                const x3::phys::Vec3* observers, uint32_t observerCount,
                bool playerSeen);

    // ---- State / effects (host reads + applies) ----
    int   level() const { return m_level; }
    float heat() const { return m_heat; }
    bool  lockdownActive() const { return m_level >= 3; }
    bool  alarmOn() const { return m_level >= 3; }
    // 0..1 red shift for lights/emissive (0 below lockdown, 1 at level 4).
    float redShift() const;
    // Patrol-route speed multiplier for the current level (AmbientEcology hook).
    float patrolSpeedMul() const { return m_cfg.patrolSpeedMul[m_level]; }
    // Pending extra-guard spawns (set on entering levels 2 and 4); POPS the
    // count — the host spawns that many via its MonsterManager.
    int   takeSpawnRequests();
    // Latest investigate-worthy stimulus position; POPS (true once per event).
    // The host routes patrols there (AmbientEcology::commandInvestigate).
    bool  takeInvestigatePos(x3::phys::Vec3& out);

    // Event hook (in ADDITION to the always-on x3.fire log line).
    void setEventSink(const AlertChangedFn& fn) { m_sink = fn; }

    // Corpse census (diagnostics / tests).
    uint32_t corpseCount() const { return (uint32_t)m_corpses.size(); }
    uint32_t discoveredCorpseCount() const;

    // Debug/staging: force a level (clamped 0-4); seeds matching heat and fires
    // the change events. Used by the lockdown proof shot + console cheats.
    void debugForceLevel(int level);

private:
    void addHeat(float h, int floorLevel, const x3::phys::Vec3& where, const char* what);
    void setLevel(int lv);

    struct Corpse { x3::phys::Vec3 pos; bool discovered = false; };
    struct PendingShot { x3::phys::Vec3 pos; float noiseMult = 1.0f; };

    AlertConfig m_cfg{};
    int     m_level = 0;
    float   m_heat = 0.0f;
    float   m_quietTime = 0.0f;     // seconds since the last stimulus/sighting
    bool    m_seenLastFrame = false;
    std::vector<Corpse>      m_corpses;
    std::vector<PendingShot> m_pendingShots;   // resolved vs observers in update()
    int     m_pendingSpawns = 0;
    bool    m_investigatePending = false;
    x3::phys::Vec3 m_investigatePos{};
    AlertChangedFn m_sink;
};

// Lockdown glue for a DoorSystem: while the alert is in LOCKDOWN it LOCKS every
// fully-closed door (remembering which ones IT locked), and when the alert
// drops below lockdown it unlocks exactly those — pre-existing locks (Door C's
// armed gate, keycard doors) are never touched. Pure door-flag logic, testable
// headlessly. Level1Game wraps one of these over its own doors.
class AlertDoorLock {
public:
    void update(const AlertSystem& alert, DoorSystem& doors);
    uint32_t lockedCount() const { return (uint32_t)m_locked.size(); }
private:
    bool m_active = false;
    std::vector<uint32_t> m_locked;   // door indices THIS system locked
};

// Headless self-test (--test-alert). Drives the machine with synthetic
// observers and asserts: (A1) a HEARD gunshot -> level 1 + investigate pos +
// the x3.fire event; (A2) an UNHEARD gunshot (no guard in radius) changes
// nothing (witness-vs-unseen); (A3) a registered corpse raises level 2 only
// when a patrol walks onto it + pops the search spawn request; (A4) sustained
// witness LOS escalates to 3 and the AlertDoorLock locks closed doors (and
// leaves pre-locked doors alone); (A5) hiding decays 3 -> 2 -> 1 -> 0 inside
// the expected per-level windows and the lockdown releases; (A6) continued
// contact at 3 reaches 4 and pops the kill-squad spawns; (A7) every transition
// fired the x3.fire event sink in order. Prints "alert: X/Y passed"; returns
// true iff all pass. No window/Vulkan.
bool runAlertSelfTest();

} // namespace x3::game
