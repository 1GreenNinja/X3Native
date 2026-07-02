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

enum class DamageType : uint32_t {
    None      = 0,   // sentinel — "no last-damage" (initial state)
    Kinetic   = 1,   // pistol, shotgun, melee-blade equivalents
    Energy    = 2,   // plasma, BFG, railgun, laser
    Explosive = 3,   // rocket, grenade
    Bio       = 4,   // chemical, toxic
    Melee     = 5,   // player melee (Phase-2b super-strength punch, etc.)
    Count     = 6,
};

} // namespace x3
