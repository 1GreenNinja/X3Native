#pragma once
// Game-feel CUE hooks (footstep / impact / generic one-shots).
//
// Game/slice code only — engine/ stays pure. This is a tiny, dependency-free
// seam so gameplay systems (monsters, weapons, melee) can EMIT lightweight
// "something happened here" events — footsteps as a locomotion phase crosses,
// an impact when a shot connects — WITHOUT yet depending on the engine audio
// system. The host wires a single callback that maps a cue onto whatever it has
// (IAudioSystem::playSound3D today, richer FX later). If no callback is wired
// the default is a cheap throttled log so the trigger points are observable in
// headless tests + console output, and audio drops in by swapping the lambda.
//
// WHY a stub seam (not a direct audio call): the MonsterSystem has no audio
// pointer (kept clean for headless tests), and audio lives only in main.cpp /
// level1_game. Establishing the TRIGGER POINTS now — phase-synced footsteps and
// on-hit impacts — means the audio integration is a one-line lambda later, not
// a hunt through the AI/combat code for the right moment to play a sound.

#include "engine/physics/IPhysicsWorld.h"   // x3::phys::Vec3

#include <cstdint>
#include <functional>

namespace x3::game {

// What kind of cue fired. Kept small + flat; the host maps each to a sound /
// FX preset (or ignores it). New kinds append at the end (stable values).
enum class CueKind : uint32_t {
    Footstep    = 0,   // a locomotion phase crossing (foot plant) — pos = the foot/body
    BulletImpact= 1,   // a shot connected with an enemy/surface — pos = the hit point
    MeleeImpact = 2,   // a melee blow landed — pos = the hit point
    // ---- Enemy VOCALIZATIONS (enemy-SFX pass). Emitted by MonsterSystem so the host
    // can give enemies a voice (the playtest "enemies make NO sounds" fix). All new
    // kinds APPEND here (stable values); a host that doesn't map them is unaffected.
    EnemyTaunt  = 3,   // periodic idle/harass vocalization (alive, engaged) — pos = enemy
    EnemyAttack = 4,   // an attack swing/shot STARTS (the wind-up) — pos = enemy muzzle
    EnemyHit    = 5,   // the enemy TOOK damage (shot/melee) — pos = enemy body
    EnemyDeath  = 6,   // the enemy was KILLED (HP -> 0) — pos = enemy body
    // ---- Player audio hooks (audio-assets pass, W2-B). Emitted by Player so the
    // host can give the PLAYER a voice (pain grunt on a landed hit, a thud on
    // landing from a jump/fall). Same append-only contract as the enemy kinds.
    PlayerPain  = 7,   // Player::takeDamage() landed a real hit — pos = player eye
    PlayerLand  = 8,   // Player transitioned airborne -> grounded — pos = player eye
    PlayerSplash= 9,   // Player entered deep water (swim state began) — pos = player eye
};

// Species tag for enemy-emitted cues (guard-life pass, W4-3): the numeric value
// of x3::game::EnemyType for the EMITTING enemy, or kCueSpeciesNone for cues with
// no species (player cues, legacy emitters). Kept as a raw uint32_t so this
// header stays free of the monster.h include; hosts that don't read it are
// unaffected (same append-only contract as CueKind).
constexpr uint32_t kCueSpeciesNone = 0xFFFFFFFFu;

// One emitted cue. Pure data: what + where (+ a 0..1 intensity the host may map
// to volume/scale). No ownership, no allocation; passed by value.
struct GameCue {
    CueKind        kind = CueKind::Footstep;
    x3::phys::Vec3 pos{};
    float          intensity = 1.0f;   // 0..1 (e.g. faster locomotion -> louder step)
    uint32_t       species   = kCueSpeciesNone;   // emitting EnemyType (or None)
};

// The host-wired cue sink. Empty => the emitting system uses its built-in
// throttled-log default (see emitCueOrLog). Cheap to copy (a std::function).
using GameCueFn = std::function<void(const GameCue& cue)>;

// Human-readable cue name (logs / tests).
const char* cueKindName(CueKind k);

// Fire `cue` through `sink` if it's set; otherwise emit a lightweight throttled
// log line (so the trigger point is observable without an audio system, but a
// 60 Hz footstep stream doesn't flood the log). The throttle is per-kind and
// keyed off a tiny static budget — intended only for the no-sink fallback. The
// hot path with a real sink does NOT log. Safe to call every frame.
void emitCueOrLog(const GameCueFn& sink, const GameCue& cue);

} // namespace x3::game
