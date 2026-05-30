#pragma once
// Faction labeling for combatants (coop-NPCs PR).
//
// Tiny shared header: the existing combat code in monster.{h,cpp} hard-coded a
// "Player vs Monster" world view (every Tag::Monster was an enemy of the lone
// Tag::Player). To add cooperative ally NPCs that fight alongside the player
// against monsters, we need an explicit per-combatant Faction label so both
// MonsterSystem (already shipped) and the new AllySystem can ask the symmetric
// question "are these two on opposite sides?".
//
// Design:
//   * Faction is a plain enum class — cheap to copy, fits in 1 byte, switchable
//     in a hot loop without virtual calls. NOT a flag/mask — a combatant
//     belongs to exactly one faction at a time.
//   * Player + Ally share an "alliance" relationship (NOT hostile to each
//     other; friendly-fire OFF by default; see g_friendlyfire cvar wired in
//     ally.cpp). Enemy is hostile to BOTH. Neutral is hostile to nobody and
//     hostile-to-nobody (held in reserve for civilian NPCs / mission VIPs).
//   * The hostility predicate is the single source of truth — every targeting
//     decision (monster picking what to attack, ally picking what to attack,
//     raycast filtering when a projectile resolves a hit body to an entity)
//     funnels through factionsHostile() so flipping the rule (e.g. enabling
//     friendly-fire) is one site, not N.
//
// Clean-room: this header is brand-new for the coop-NPC lane. No third-party
// AI/combat source consulted.

#include <cstdint>

namespace x3::game {

// One byte per combatant — keep the storage small for the dense per-instance
// AI arrays in MonsterManager / AllyManager. Distinct from MonsterType /
// EnemyType (the WHAT — guard, drone, alien, etc.) — Faction is the WHO (which
// side am I on). Multiple EnemyType values can share Faction::Enemy.
enum class Faction : uint8_t {
    Player  = 0,  // The single player character (jake-style first-person).
    Ally    = 1,  // Cooperative NPC fighting alongside the player.
    Enemy   = 2,  // Hostile combatant (the existing monster/bestiary roster).
    Neutral = 3,  // Civilian / mission VIP — doesn't fight, isn't targeted.
};

// Human-readable Faction name (for logs / debug HUD / state traces). One short
// noun per side so the trace stays grep-able.
inline const char* factionName(Faction f) {
    switch (f) {
    case Faction::Player:  return "Player";
    case Faction::Ally:    return "Ally";
    case Faction::Enemy:   return "Enemy";
    case Faction::Neutral: return "Neutral";
    }
    return "?";
}

// THE hostility predicate. Returns true iff a unit of faction `a` should
// consider a unit of faction `b` as a valid target for damage / attack-state
// transitions / raycast resolution. Symmetric:
//
//   Player <-> Enemy  : hostile
//   Ally   <-> Enemy  : hostile
//   Player <-> Ally   : NOT hostile (friendly-fire OFF default)
//   Ally   <-> Ally   : NOT hostile
//   Neutral <-> *     : never hostile
//   * <-> Neutral     : never hostile
//
// Friendly-fire enable (cvar g_friendlyfire = 1) is layered ON TOP of this in
// the raycast/damage path — it makes Player projectiles ALSO damage Ally and
// vice-versa, but does NOT enable Ally-vs-Ally or Player-vs-Player infighting.
// See AllySystem::resolveHit / MonsterSystem::resolveHit for the layered rule.
inline bool factionsHostile(Faction a, Faction b) {
    if (a == Faction::Neutral || b == Faction::Neutral) return false;
    // The only hostile pair is (Player|Ally) <-> Enemy, in either direction.
    const bool aSide = (a == Faction::Player || a == Faction::Ally);
    const bool bSide = (b == Faction::Player || b == Faction::Ally);
    return aSide != bSide;   // exactly one side is "friendly-with-player" => hostile.
}

} // namespace x3::game
