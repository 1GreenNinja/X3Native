#pragma once
// Super-strength melee verb (EFLZ Phase 2b, spec §6 item 7 + §6.7 fantasy).
//
// Game/slice code only — engine/ stays pure. Jake's signature unarmed verb: a
// short forward "super-punch" that, on a single press, deals HEAVY damage to every
// enemy in a short forward arc/range AND knocks them back with a physics impulse
// (applyImpulse). It is ALSO the strength-fantasy alternative to a button: a punch
// aimed at a closed/locked door BRUTE-FORCES it open (loud), per the bible's
// "brute-force a door". Works whether or not the player is armed (the pistol is a
// separate LMB verb); a brief cooldown gates it and the host spawns a melee FX.
//
// Built only from Scene / MonsterManager / DoorSystem / IPhysicsWorld + Vec3. No
// id Tech / RBDOOM source, no purchased C# copied — distilled from the design docs.
//
// Geometry of the arc: a forward CONE around the camera look direction. A target
// is "in the arc" iff it is within kMeleeRange meters AND the angle between the
// look dir and the eye->target vector is <= kMeleeHalfAngle (i.e. dot >= cos of
// that). This is the cheap, robust analog of the spec's "sphere or short raycast
// cone in front of the camera": it hits a cluster of enemies in front, ignores
// enemies behind or off to the sides, and needs no per-enemy ray. The door
// brute-force uses a short look-direction raycast (Layer::Static) since doors are
// static bodies the cone test can't resolve by entity.

#include "scene.h"
#include "monster.h"
#include "door.h"

#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>
#include <vector>

namespace x3::game {

// ---- Melee tuning (placeholders per docs/EFLZ_DESIGN.md; tuning targets). ----
// Reach (meters) of the super-punch, eye -> target center.
constexpr float kMeleeRange      = 2.6f;
// Half-angle (radians) of the forward cone. ~50 deg total spread (so things
// roughly in front are hit; things behind / hard sideways are not). cos(25deg).
constexpr float kMeleeHalfAngle  = 0.4363f; // 25 degrees
// Heavy damage per punch (super-strength): one-shots a basic 100-HP enemy.
constexpr int   kMeleeDamage     = 120;
// Knockback impulse magnitude applied along eye->target (Newton-seconds-ish; the
// enemy bodies are static-by-mass so this is mostly cosmetic for them, but it
// reads as a shove on any dynamic body and documents the "throw" intent).
constexpr float kMeleeKnockback  = 14.0f;
// Cooldown (seconds) between punches.
constexpr float kMeleeCooldown   = 0.45f;
// Max reach (meters) of the door brute-force raycast (a touch beyond melee range
// so you can punch a door you're standing right at).
constexpr float kMeleeDoorReach  = 2.6f;

// Pure arc test, factored out so it is testable headlessly. Returns true iff the
// point `target` is within `range` of `eye` AND within `halfAngle` of the `dir`
// look direction (a forward cone). `dir` need not be unit (normalized inside).
// A target exactly at the eye (distance ~0) counts as in-front (dot test skipped).
bool inMeleeArc(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                const x3::phys::Vec3& target, float range, float halfAngle);

// Result of a melee strike, for HUD/FX/logging + the self-test.
struct MeleeResult {
    bool onCooldown   = false;  // the punch was suppressed (cooldown not elapsed)
    int  enemiesHit   = 0;      // live enemies that took damage this punch
    int  enemiesKilled= 0;      // of those, how many it killed outright
    bool doorForced   = false;  // a closed/locked door was brute-forced open
    // Where to spawn the melee swing FX: eye + dir * kMeleeRange (the punch's far
    // point). Always set so the host can show a swing even on a whiff.
    x3::phys::Vec3 swingTo{};
};

// Super-strength melee system: owns only the cooldown timer (all state it touches
// — enemies, doors, physics — is passed in). Self-contained + headless-testable.
class MeleeSystem {
public:
    // Advance the cooldown timer. Call once per frame. No-op at dt <= 0.
    void update(float dt) { if (dt > 0.0f && m_cooldown > 0.0f) { m_cooldown -= dt; if (m_cooldown < 0.0f) m_cooldown = 0.0f; } }

    // True iff a punch can land right now (cooldown elapsed).
    bool ready() const { return m_cooldown <= 0.0f; }

    // Throw a super-punch from `eye` along `dir` (the camera look dir; need not be
    // unit). On a punch (not on cooldown):
    //   * every LIVE enemy in `groups` whose body-center is inside the forward arc
    //     (kMeleeRange / kMeleeHalfAngle) takes kMeleeDamage and a knockback
    //     impulse along eye->enemy (applyImpulse), dying on HP<=0;
    //   * a short look-dir raycast brute-forces the first CLOSED/locked door it
    //     hits open (DoorSystem::unlockAndOpen) — the loud strength alternative to
    //     a button;
    // then the cooldown is started. `groups`/`doors` may be empty/absent (pass an
    // empty vector / a doors ptr; nullptr doors = no door brute-force). Returns
    // what happened (see MeleeResult); onCooldown=true (and no effects) if not
    // ready. Arming has NO effect here — this is the unarmed verb.
    MeleeResult strike(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                       Scene& scene, x3::phys::IPhysicsWorld& physics,
                       const std::vector<MonsterManager*>& groups,
                       DoorSystem* doors);

private:
    float m_cooldown = 0.0f;  // seconds until the next punch can land
};

} // namespace x3::game
