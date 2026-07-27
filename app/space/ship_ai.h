#pragma once
// S8 enemy ship AI — the dogfight pillar's enemies (space-engine spec §S8 +
// decision 2.7 "full dogfighting").
//
// AI-driven enemy fighter ships that fly + attack the player's ship while it is
// in the DeepSpace context (see app/space/space_layer.h). Each enemy is a
// simplified 6DOF analogue of the player's SpacePilotController (app/space_pilot.h):
// a position + velocity + forward vector, a hull pool, a per-instance AI state
// machine, and a firing cooldown.
//
// CLEAN-ROOM, original work. The dense-array, per-instance manager shape follows
// X3's OWN MonsterManager / MonsterSystem pattern (app/monster.h) — data-driven
// instance arrays, a behaviour state machine, fire-event records the host draws.
// No RBDOOM / id Tech / Doom / Quake engine source consulted.
//
// SIMPLE FACTION MODEL (the faction.h system is on a separate branch and is NOT a
// dependency here): every enemy ship targets the single player ship. The player is
// the lone friendly; there is no friendly-fire or inter-enemy targeting.
//
// Headless logic only: the manager itself needs no GPU. The host (--world ship-ai)
// draws a ship mesh at each live ship's pos/fwd and renders the laser-fire events
// as tracers (via the existing CombatFx or a simple line).

#include <cstdint>
#include <vector>

namespace x3::space {

// Per-instance AI behaviour state. The machine decides the desired heading +
// whether to fire; the manager steers the ship's velocity toward that heading
// each tick.
//   * Patrol — no target in detect range: drift on the current velocity (idle).
//   * Engage — player within detect range, but not yet lined up for a shot:
//              steer toward a LEAD point ahead of the player and close distance.
//   * Strafe — the ATTACK RUN: steer at the lead point + fire on the cooldown.
//              Entered from long range when lined up (the classic approach) or
//              from Orbit when the per-ship attack timer expires (the pass).
//   * Evade  — own hull low, the ship overshot the target (player is behind
//              it), or the peel-off leg after an attack run: arc away to reset.
//   * Orbit  — DOGFIGHT CIRCLING (owner: "it hovers on top of me! it should be
//              circling!"): inside kOrbitEnterDist the ship steers TANGENTIALLY
//              around the player, holding an orbit band around kOrbitRadius, so
//              it carves arcs around you instead of parking on your canopy. A
//              seeded per-ship timer periodically breaks orbit into a Strafe
//              attack run, then a short Evade peel, then back to Orbit — and the
//              seeding staggers the ships so a wing never synchronizes into a
//              carousel.
enum class ShipAIState { Patrol, Engage, Evade, Strafe, Orbit };

// Human-readable state name (logs / --test-ship-ai trace).
const char* shipAIStateName(ShipAIState s);

// One enemy fighter. Simplified 6DOF: a position, a velocity, and a unit forward
// vector (the heading the ship is pointing — firing cone is measured off this).
struct EnemyShip {
    float pos[3];        // world position (m)
    float vel[3];        // world velocity (m/s)
    float fwd[3];        // unit forward heading (firing cone axis)
    int   hull;          // current hull; <= 0 => dead/removed
    int   maxHull;       // starting hull
    ShipAIState state;   // current AI behaviour state
    float fireCooldown;  // seconds remaining until the ship may fire again
    float hitFlash;      // seconds of hull hit-flash remaining (combat readability:
                         // damageShip sets it, tickShip decays it; the host tints
                         // the ship's draw brighter/warm while > 0 so a registered
                         // hit READS instantly at dogfight range)

    // ---- Orbit / attack-run bookkeeping (dogfight circling) ----------------
    uint32_t seed;        // per-ship spawn-order seed (staggers timers per ship)
    int      orbitSign;   // +1 / -1: clockwise vs counter-clockwise orbit
    float    orbitPhase;  // seeded per-ship clock for the vertical weave
    float    attackTimer; // orbit time left until the next attack run
    float    modeTimer;   // time left in the current run/peel leg
    uint8_t  runMode;     // 0 = orbiting, 1 = attack run (Strafe), 2 = peel (Evade)
};

// A single laser-fire event emitted by update() when a ship fires. The host draws
// a tracer from `from` along the firing line to `to` (a long-range point down the
// ship's forward toward the player) and resolves any damage to the player.
struct ShipFireEvent {
    uint32_t shooter;    // index of the firing ship (into the live array)
    float    from[3];    // muzzle world position
    float    to[3];      // tracer end (firing-line endpoint at laser range)
};

// ---------------------------------------------------------------------------
// Tuning. All gameplay/behaviour tuning — safe to retune for playtest; the
// self-test asserts the BEHAVIOUR (state flips, closing distance, cooldown
// gating, death removal), not the exact numbers.
// ---------------------------------------------------------------------------
namespace shipai {
constexpr float kDetectRange   = 600.0f;  // player within this -> leave Patrol
constexpr float kFireRange      = 300.0f;  // player within this + in cone -> Strafe/fire
constexpr float kFireConeCos    = 0.92f;   // cos of the half firing cone (~23 deg)
constexpr float kFireCooldown   = 0.7f;    // seconds between an enemy's shots
constexpr float kMaxSpeed       = 90.0f;   // hard speed cap (m/s)
constexpr float kAccel          = 60.0f;   // steering accel toward desired dir (m/s^2)
constexpr float kEvadeHullFrac  = 0.18f;   // hull at/below this fraction -> Evade.
                                           // BELOW the <25% burning-FX band
                                           // (combat readability): at the old
                                           // 0.30 every ship that reached the
                                           // ember/heavy-smoke stage instantly
                                           // fled the fight forever — the player
                                           // never SAW the burn he had earned.
                                           // 25%..18% is the visible last-stand
                                           // window; under 18% it still flees.
constexpr float kLeadFactor     = 1.0f;    // how strongly to lead the moving target
constexpr float kLaserRange     = 400.0f;  // tracer length / fire-line endpoint dist
constexpr int   kDefaultHull    = 60;      // starting hull for a spawned ship
constexpr int   kLaserDamage    = 12;      // damage a hit would deal (host resolves)

// ---- Dogfight circling (Orbit state) tuning --------------------------------
// The player's shield standoff (60 m, EnemyShipManager::update) stays the hard
// floor; the orbit band sits ABOVE it so the standoff is nearly unreachable in
// normal play — the ship holds a readable circling range instead of grinding on
// the bubble.
constexpr float kOrbitEnterDist = 130.0f;  // inside this (not on a run) -> Orbit
constexpr float kOrbitRadius    = 90.0f;   // target orbit range (band ~60..120)
constexpr float kOrbitBandHalf  = 30.0f;   // radial error normalizer (band half-width)
constexpr float kOrbitRadialGain= 0.85f;   // inward/outward blend to hold the band
constexpr float kOrbitRadialBleed = 1.5f;  // 1/s radial-velocity damp while circling
                                           // (peel momentum -> circulation, not lunges)
constexpr float kOrbitWeave     = 0.30f;   // vertical weave amplitude (3D, not a disc)
constexpr float kAttackRunSec   = 2.2f;    // length of a strafing pass off orbit
                                           // (long enough for 2 shots, short enough
                                           // not to grind on the shield standoff)
constexpr float kRunTurnBleed   = 2.5f;    // 1/s cross-track velocity bleed on a pass
                                           // (pulls the nose onto the firing line)
constexpr float kPeelSec        = 1.4f;    // Evade peel after the pass, then re-orbit
// Per-ship attack cadence (seeded, so 3 ships never synchronize): the FIRST run
// comes quickly (the fight starts hot), later runs are spaced wider.
constexpr float kFirstRunDelay  = 0.6f;    // + 0.45 * (seed % 6) — staggered
constexpr float kNextRunDelay   = 3.5f;    // + 0.5  * (hash % 6) — staggered; wide
                                           // enough that CIRCLING dominates the fight

// ---- Hull hit-flash (combat readability) -----------------------------------
constexpr float kHitFlashSec    = 0.10f;   // warm hull flash length after a hit

// ---- DISTANCE-COMPENSATED visual scale — ONE formula, TWO consumers ---------
// The intro flight beats draw an enemy fighter at kVisBaseScale, growing
// linearly past kVisCompStart (capped kVisCompMax at 600 m+) so a contact never
// falls below a readable on-screen size (owner: "I cannot SEE the enemy ship").
// The player therefore aims at what he SEES — so the HIT TEST's acceptance
// radius must scale by the SAME factor (owner: "allow me to hit the tiny enemy
// ships too!"). A shot whose ray passes within the DRAWN silhouette's radius at
// the target's distance is a hit. Do NOT fork this formula: draw + hit share it.
constexpr float kVisBaseScale   = 6.0f;    // base draw scale (close range)
constexpr float kVisCompStart   = 150.0f;  // beyond this the draw grows linearly
constexpr float kVisCompMax     = 4.0f;    // growth cap (reached at 600 m+)
constexpr float kHitBaseRadius  = 9.0f;    // acceptance radius at factor 1
                                           // (generous vs the 6 m draw scale)
inline float visCompFactor(float dist) {
    const float f = dist / kVisCompStart;
    return f < 1.0f ? 1.0f : (f > kVisCompMax ? kVisCompMax : f);
}

// ---- Damage-state FX staging (combat readability) ---------------------------
// Emission periods (in 60 Hz frames; 0 = OFF) for the persistent hull-damage FX
// a wounded fighter trails, keyed to hull fraction:
//   >= 0.75          : pristine — NO damage FX (the negative control).
//   <  0.75          : intermittent spark bursts.
//   <  0.50          : + continuous thin grey smoke trail.
//   <  0.25          : + heavy smoke + ember/fire glow at the hull.
// Pure + inline so the live emitters and the --test-ship-ai assertions run the
// exact same staging.
struct DamageFxProfile {
    int sparkPeriod;   // frames between spark bursts (0 = none)
    int smokePeriod;   // frames between smoke puffs  (0 = none)
    int emberPeriod;   // frames between fire embers  (0 = none)
};
inline DamageFxProfile damageFxProfile(float hullFrac) {
    if (hullFrac >= 0.75f) return { 0, 0, 0 };
    if (hullFrac >= 0.50f) return { 22, 0, 0 };
    if (hullFrac >= 0.25f) return { 14, 3, 0 };
    return { 8, 2, 3 };
}
}

// Dense-array, data-driven manager (MonsterManager-style). Owns the per-instance
// ship arrays via a std::vector<EnemyShip>; a dead ship is swap-removed so the
// live array stays packed (count()/aliveCount() == live ships).
class EnemyShipManager {
public:
    // Reserve storage for up to `count` ships (no ships spawned yet). Resets any
    // existing state. Cheap; no GPU.
    void init(uint32_t count);

    // Spawn one enemy ship at `pos`, facing toward the world origin-ish default
    // (its forward is initialized pointing along -its position so it heads
    // "inward"; the AI re-steers immediately). Starts in Patrol with full hull.
    void spawn(const float pos[3]);

    // AI tick. Each live ship runs its state machine against the single player
    // target (playerPos / playerVel): Patrol drifts; Engage steers toward a lead
    // point ahead of the player and closes; Strafe fires when the player is in the
    // firing cone + range (gated by fireCooldown); Evade peels off when hull is low
    // or the target is behind. Velocity is steered toward the desired direction and
    // clamped to kMaxSpeed; position integrates from velocity. Fire events are
    // recorded and returned via fireEvents() (cleared at the top of each update()).
    void update(float dt, const float playerPos[3], const float playerVel[3]);

    // Live ship count (== aliveCount(); a dead ship is removed from the array).
    uint32_t count() const;
    uint32_t aliveCount() const;

    // Read a live ship by dense index i in [0, count()). The host draws a mesh at
    // ship(i).pos / ship(i).fwd.
    const EnemyShip& ship(uint32_t i) const;

    // Apply `amount` hull damage to ship i. If hull drops to <= 0 the ship dies and
    // is swap-removed from the live array (count() drops by one). No-op for a
    // negative/zero amount or an out-of-range index. NOTE: swap-remove means indices
    // after i may shift — re-query after a damage call that could kill.
    void damageShip(uint32_t i, int amount);

    // Fire events produced by the most recent update() (one per ship that fired
    // this tick). The host draws a tracer per event + resolves player damage.
    const std::vector<ShipFireEvent>& fireEvents() const { return fireEvents_; }

private:
    std::vector<EnemyShip>      ships_;
    std::vector<ShipFireEvent>  fireEvents_;
    uint32_t                    spawnSeq_ = 0;   // per-spawn seed (orbit stagger)

    // Advance one ship's state machine + steering for this tick. Appends a fire
    // event to fireEvents_ if it fires. `i` is the dense index (for the event).
    void tickShip(uint32_t i, float dt,
                  const float playerPos[3], const float playerVel[3]);
};

// ---- --test-ship-ai self-test (>=7 sub-checks, headless, pure logic) --------
// Asserts: (1) init/spawn populates the live array; (2) a ship with the player in
// range flips Patrol->Engage; (3) an engaging ship steers its velocity toward the
// player so the distance DECREASES over ticks; (4) an out-of-range ship stays
// Patrol; (5) a lined-up ship in fire range fires (a fire event is produced);
// (6) the fire cooldown gates firing (no second shot before the cooldown elapses);
// (7) damageShip to 0 removes the ship from aliveCount(); (8) speed stays clamped
// to kMaxSpeed; (9) an in-band enemy ORBITS the player — its bearing angle around
// the player ADVANCES over time instead of converging onto the player's
// coordinates — with a negative control proving the bearing metric reads ~zero
// for a converge-and-hover trajectory. Deterministic, no GPU. Logs PASS/FAIL;
// returns true iff all pass.
bool runShipAiSelfTest();

} // namespace x3::space
