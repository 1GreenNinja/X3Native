#pragma once
// Damage type — a small tag carried alongside an `int damage` so the receiver
// can implement type-keyed mechanics (the canon-aliens SaurianWarlord's Adaptive
// Hide "rotate damage type" rhythm, future cryo/EMP resists, etc.).
//
// Pure data, zero deps — header-only enum. The mapping from a player weapon /
// melee event to a DamageType lives in the call site (WeaponSystem etc.); the
// receiving entity (MonsterSystem) reads it via the existing fire() and
// takeMeleeDamage() entry points where it now arrives as a defaulted argument.
//
// Backward compatible: every fire(damage) call still works — the type silently
// defaults to Kinetic/Melee at the entry point, and a row that doesn't opt into
// adaptive-resist (Tuning::adaptiveHideResist == 0) ignores the type entirely.
#include <cstdint>

namespace x3 {

// APPEND-ONLY. Values are stamped onto HitscanRay/ProjectileSpawn and compared for
// EQUALITY by MonsterSystem's Adaptive-Hide window, so renumbering an existing row
// silently re-keys every resist rhythm in the game. Add new rows at the END and
// bump Count.
enum class DamageType : uint32_t {
    None      = 0,   // sentinel — "no last-damage" (initial state)
    Kinetic   = 1,   // pistol, shotgun, melee-blade equivalents
    Energy    = 2,   // plasma, BFG, railgun, laser
    Explosive = 3,   // rocket, grenade
    Bio       = 4,   // chemical, toxic
    Melee     = 5,   // player melee (Phase-2b super-strength punch, etc.)
    // CRYO (2026-08-15, weapon-feel lane). The Freeze Ray's tag. It is NOT a
    // cosmetic re-label: MonsterSystem::onDamaged keys its timed chase-speed SLOW
    // off this value (see kCryoSlowFactor in monster.h), which is the whole payload
    // of the weapon — the port is explicit that its 5 damage is not the point. It
    // also finally separates the Freeze Ray from laser/railgun/BFG/plasma on the
    // Adaptive-Hide resist window, which it used to share as Energy.
    Cryo      = 6,   // freeze ray — chilling / slowing energy
    Count     = 7,
};

} // namespace x3
