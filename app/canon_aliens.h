#pragma once
// EFLZ canon-alien roster — the four "most reported" species (Mantis / Grey /
// Reptilian / Nordic, per the Davis-Puthoff visualisation) ported into X3Native's
// data-driven roster idiom. Each species is a MonsterSystem::Tuning factory you
// spawn via MonsterManager::spawn (or buildMonsterTuned) just like the existing
// Act-1 / Act-2 roster rows.
//
// CLEAN-ROOM, original work. Built ONLY from X3Native's own monster.* + engine
// interfaces + EFLZ design intent. NO RBDOOM / id Tech / Doom / Quake — or any
// other game-engine — source consulted.
//
// FACTION MAPPING into EFLZ lore (so the canon species slot into the existing world):
//   * SaurianSoldier / SaurianWarlord -> the **Overlord** invader enforcers (Act 2+).
//   * GreyTasked     -> synthetic worker-drone serving the Reptilians (BlueSynth tier).
//   * NordicSteward  -> peaceful mentor ally (Salvari-adjacent — startAllied + 0 dmg).
//   * MantisArbiter  -> wildcard insectoid assassin (Verthani-adjacent — karma-driven).
//
// VISUAL CANON: the per-species single-figure references live alongside the
// source infographic ("4 Most Reported Alien Beings" — 1–8 ft height scale). The
// 14900K rigs each GLB against its reference strip; this roster gives the
// `Tuning.modelFile` slot for the GLB the moment it lands.
//
// ENGINE EXTENSIONS — *flagged* but NOT implemented here (other lanes' work):
//   * **Adaptive Hide** (SaurianWarlord) — last-damage-type 60% resist for 8 s.
//     Requires a new `MonsterSystem::Tuning::adaptiveHideResist` + per-frame
//     last-damage-type tracker on MonsterSystem (monster.* extension).
//   * **Aegis Field / Foresight / Uplift** (NordicSteward) — ally shield/heal/buff.
//   * **Override** (GreyTasked) — hack enemy tech (turret/drone faction-flip).
//   * **Veil** (MantisArbiter) — stealth + first-strike-from-invisible 3× opener.
//
// What this module DOES today: produces SPAWN-READY Tunings using the EXISTING
// engine fields (hp / chaseSpeed / damage / attackRange / attackCooldown / type /
// ranged / standoff / aiStrafeBias / tint / modelScale / startAllied / Boss-phase
// fields / memoryFlash). The species spawn + fight in the existing combat lane
// today; the engine extensions above land as separate PRs.

#include "monster.h"

#include <cstdint>
#include <vector>

namespace x3::game {

// One species enum value per canon-alien Tuning row. The roster's order is the
// canonical iteration order (matches the per-species crop file naming).
enum class CanonAlien : uint32_t {
    SaurianSoldier = 0,  // Reptilian melee bruiser — apex predator, Overlord enforcer
    SaurianWarlord = 1,  // Reptilian BOSS — 3 phases + memory-flash (Adaptive-Hide TODO)
    GreyTasked     = 2,  // Grey ranged worker-drone — fragile, kites, recon-leaning
    NordicSteward  = 3,  // Nordic ALLY — peaceful mentor, startAllied + 0 damage
    MantisArbiter  = 4,  // Mantis assassin — fast, melee-burst, extreme strafe
    Count          = 5
};

// Human-readable canon-alien name (logs / --test-canonaliens trace / HUD).
const char* canonAlienTypeName(CanonAlien t);

// One row of the canon-alien roster. Same shape as MonsterDef / Act2EnemyDef.
struct CanonAlienDef {
    CanonAlien            type;
    const char*           name;
    MonsterSystem::Tuning tuning;
};

// The full canon table (one row per CanonAlien, in enum order). Built once.
const std::vector<CanonAlienDef>& canonAlienDefs();
// Fetch one row by enum id (asserts enum order; defensive linear fallback).
const CanonAlienDef& canonAlienDef(CanonAlien t);
// Convenience: a spawn-ready Tuning copy. Pass to MonsterManager::spawn() /
// buildMonsterTuned() to place the species.
MonsterSystem::Tuning canonAlienTuning(CanonAlien t);

// Headless self-test (--test-canonaliens). Builds each canon-alien on a
// HeadlessDevice + Jolt world and asserts:
//   (T0) the 5-row roster is complete + ordered.
//   (T1) each species BUILDS via buildMonsterTuned with its table stats
//        (hp/damage/type/ranged match the row; alive at load).
//   (T2) SaurianSoldier: hostile melee Guard, HP >= 100.
//   (T3) SaurianWarlord: Boss + HP >= 400 + phase machine armed + memory-flash
//        amplified-damage window (the Adaptive-Hide "rotate damage type" beat).
//   (T4) GreyTasked: ranged Drone, fragile (HP < 100), high standoff.
//   (T5) NordicSteward: startAllied + stationary (chaseSpeed == 0).
//   (T6) MantisArbiter: hostile, fast (chase > default), aggressive strafe (>0.8).
//   (T7) rows are DISTINCT (no two identical stat blocks).
// Prints "canonaliens: X/Y passed"; returns true iff all pass. No window/Vulkan.
bool runCanonAliensSelfTest();

} // namespace x3::game
