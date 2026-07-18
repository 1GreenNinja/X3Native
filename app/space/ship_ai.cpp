// S8 enemy ship AI — see app/space/ship_ai.h.
//
// CLEAN-ROOM, original work. Dense-array manager shaped after X3's own
// MonsterManager (app/monster.h). No RBDOOM / id Tech / Doom / Quake source.

#include "ship_ai.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <string>

namespace x3::space {

// ===========================================================================
// Local vec3 helpers (kept private to this TU).
// ===========================================================================
namespace {

inline float dot3(const float a[3], const float b[3]) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}
inline float len3(const float v[3]) { return std::sqrt(dot3(v, v)); }

inline void sub3(const float a[3], const float b[3], float out[3]) {
    out[0] = a[0]-b[0]; out[1] = a[1]-b[1]; out[2] = a[2]-b[2];
}

// Normalize v in place; returns the original length. Leaves a unit +X if the
// input is ~zero (a safe, deterministic default heading).
inline float normalize3(float v[3]) {
    const float n = len3(v);
    if (n > 1e-6f) { v[0]/=n; v[1]/=n; v[2]/=n; }
    else           { v[0]=1.0f; v[1]=0.0f; v[2]=0.0f; }
    return n;
}

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace

const char* shipAIStateName(ShipAIState s) {
    switch (s) {
        case ShipAIState::Patrol: return "Patrol";
        case ShipAIState::Engage: return "Engage";
        case ShipAIState::Evade:  return "Evade";
        case ShipAIState::Strafe: return "Strafe";
        case ShipAIState::Orbit:  return "Orbit";
    }
    return "?";
}

// ===========================================================================
// EnemyShipManager
// ===========================================================================

void EnemyShipManager::init(uint32_t count) {
    ships_.clear();
    ships_.reserve(count);
    fireEvents_.clear();
    fireEvents_.reserve(count);
    spawnSeq_ = 0;
}

void EnemyShipManager::spawn(const float pos[3]) {
    EnemyShip s{};
    s.pos[0] = pos[0]; s.pos[1] = pos[1]; s.pos[2] = pos[2];
    s.vel[0] = s.vel[1] = s.vel[2] = 0.0f;
    // Initial forward points "inward" (toward the origin), so a ship spawned out
    // on a sphere faces roughly toward the action; the AI re-steers immediately.
    s.fwd[0] = -pos[0]; s.fwd[1] = -pos[1]; s.fwd[2] = -pos[2];
    normalize3(s.fwd);
    s.hull = shipai::kDefaultHull;
    s.maxHull = shipai::kDefaultHull;
    s.state = ShipAIState::Patrol;
    s.fireCooldown = 0.0f;
    // Orbit bookkeeping: seed by SPAWN ORDER so every ship in a wing gets its own
    // orbit direction, weave phase, and attack cadence — three fighters circle as
    // individuals, never a synchronized carousel.
    s.seed        = spawnSeq_++;
    s.orbitSign   = (s.seed & 1u) ? -1 : +1;
    s.orbitPhase  = (float)s.seed * 1.7f;
    s.attackTimer = shipai::kFirstRunDelay + 0.45f * (float)(s.seed % 6u);
    s.modeTimer   = 0.0f;
    s.runMode     = 0;
    ships_.push_back(s);
}

uint32_t EnemyShipManager::count() const { return (uint32_t)ships_.size(); }
uint32_t EnemyShipManager::aliveCount() const { return (uint32_t)ships_.size(); }

const EnemyShip& EnemyShipManager::ship(uint32_t i) const { return ships_[i]; }

void EnemyShipManager::damageShip(uint32_t i, int amount) {
    if (amount <= 0 || i >= ships_.size()) return;
    EnemyShip& s = ships_[i];
    s.hull -= amount;
    if (s.hull <= 0) {
        s.hull = 0;
        // Swap-remove: keep the live array packed (MonsterManager pattern).
        ships_[i] = ships_.back();
        ships_.pop_back();
    }
}

void EnemyShipManager::update(float dt, const float playerPos[3], const float playerVel[3]) {
    fireEvents_.clear();
    if (dt <= 0.0f) return;
    for (uint32_t i = 0; i < (uint32_t)ships_.size(); ++i)
        tickShip(i, dt, playerPos, playerVel);

    // ---- SHIELD STANDOFF (owner: "make my shield push it away to viewable
    //      distance ... 500 meters"). The AI kept closing to the player's EXACT
    //      coordinates and flying ON TOP OF the camera. The player's shield bubble
    //      repels any enemy that penetrates it back out to kShieldStandoff, so a
    //      contact is always at a readable range instead of clipping through you.
    // 60 m, NOT 500: at 500 every fighter lived beyond visual range (a 10 m ship
    // subtends ~15 px at 500 m) — "the enemy just disappears!". 60 m keeps the
    // dogfight CLOSE and readable while still refusing hull overlap ("I fly right
    // thru the enemy ship").
    constexpr float kShieldStandoff = 60.0f;
    for (auto& s : ships_) {
        float to[3] = { s.pos[0] - playerPos[0], s.pos[1] - playerPos[1], s.pos[2] - playerPos[2] };
        const float d = len3(to);
        if (d > 1e-3f && d < kShieldStandoff) {
            const float k = kShieldStandoff / d;   // push out along the contact line
            s.pos[0] = playerPos[0] + to[0] * k;
            s.pos[1] = playerPos[1] + to[1] * k;
            s.pos[2] = playerPos[2] + to[2] * k;
        }
    }
}

void EnemyShipManager::tickShip(uint32_t i, float dt,
                                const float playerPos[3], const float playerVel[3]) {
    EnemyShip& s = ships_[i];

    if (s.fireCooldown > 0.0f) {
        s.fireCooldown -= dt;
        if (s.fireCooldown < 0.0f) s.fireCooldown = 0.0f;
    }

    // ---- Geometry vs. the player target ------------------------------------
    float toPlayer[3];
    sub3(playerPos, s.pos, toPlayer);
    const float dist = len3(toPlayer);
    float dirToPlayer[3] = { toPlayer[0], toPlayer[1], toPlayer[2] };
    normalize3(dirToPlayer);

    // Forward alignment toward the player (cos of the angle between fwd & dir).
    const float aim = dot3(s.fwd, dirToPlayer);
    const bool  inRange  = dist <= shipai::kFireRange;
    const bool  inDetect = dist <= shipai::kDetectRange;
    const bool  inCone   = aim >= shipai::kFireConeCos;
    const bool  behind   = aim < 0.0f;   // player is behind the ship -> overshoot
    const float hullFrac = s.maxHull > 0 ? (float)s.hull / (float)s.maxHull : 0.0f;
    const bool  lowHull  = hullFrac <= shipai::kEvadeHullFrac;

    // ---- Orbit / attack-run timers -----------------------------------------
    if (s.modeTimer > 0.0f)   { s.modeTimer -= dt;   if (s.modeTimer < 0.0f)   s.modeTimer = 0.0f; }
    if (s.runMode == 0 && s.attackTimer > 0.0f && inDetect)
        s.attackTimer -= dt;                    // the next pass only cooks while engaged
    s.orbitPhase += dt * (0.7f + 0.13f * (float)(s.seed % 5u));   // per-ship weave clock

    // ---- State decision ----------------------------------------------------
    // The orbit/run/peel cycle (owner: "it hovers on top of me! it should be
    // circling! I need to zip and zoom around it"): inside kOrbitEnterDist the
    // ship CIRCLES the player (Orbit) until its seeded attack timer expires, then
    // breaks into a straight strafing pass (Strafe, kAttackRunSec), peels off
    // (Evade, kPeelSec), and re-enters orbit with a fresh staggered timer. The
    // long-range states (Patrol/Engage + the classic lined-up Strafe) keep their
    // pre-orbit shape.
    if (!inDetect) {
        s.state = ShipAIState::Patrol;
        s.runMode = 0; s.modeTimer = 0.0f;
    } else if (lowHull) {
        s.state = ShipAIState::Evade;          // peel off + reset the pass
        s.runMode = 0; s.modeTimer = 0.0f;
    } else if (s.runMode == 1) {
        // ATTACK RUN: hold the pass until the leg expires or the ship TRULY
        // overshoots (aim well negative — the nose starts ~tangential coming out
        // of orbit, so a plain `behind` (aim < 0) would abort the run on tick 1).
        s.state = ShipAIState::Strafe;
        if (s.modeTimer <= 0.0f || aim < -0.35f) { s.runMode = 2; s.modeTimer = shipai::kPeelSec; }
    } else if (s.runMode == 2) {
        // PEEL: arc away, then re-orbit with a fresh (seeded, varied) cadence.
        s.state = ShipAIState::Evade;
        if (s.modeTimer <= 0.0f) {
            s.runMode = 0;
            s.attackTimer = shipai::kNextRunDelay +
                            0.5f * (float)((s.seed * 7u + (uint32_t)(s.orbitPhase * 13.7f)) % 6u);
        }
    } else if (dist < shipai::kOrbitEnterDist) {
        // CLOSE: circle the player; break into a pass when the timer expires.
        if (s.attackTimer <= 0.0f) {
            s.runMode = 1; s.modeTimer = shipai::kAttackRunSec;
            s.state = ShipAIState::Strafe;
        } else {
            s.state = ShipAIState::Orbit;
        }
    } else if (inRange && inCone) {
        s.state = ShipAIState::Strafe;         // lined up -> attack (long-range pass)
    } else if (inRange && behind && dist < shipai::kOrbitEnterDist) {
        // Overshot AT CLOSE RANGE: swing around. Farther out, Engage turns the
        // ship back toward the fight instead — the unconditional inRange+behind
        // Evade used to chain with the peel and run the ship 300+ m off the
        // fight before it ever turned around.
        s.state = ShipAIState::Evade;
    } else {
        s.state = ShipAIState::Engage;         // close + line up
    }

    // ---- Desired steering direction per state ------------------------------
    float desired[3] = { s.fwd[0], s.fwd[1], s.fwd[2] };  // default: hold heading
    switch (s.state) {
        case ShipAIState::Patrol:
            // Drift: keep the current heading, no steering toward a target.
            break;
        case ShipAIState::Engage:
        case ShipAIState::Strafe: {
            // Lead the moving target: aim at where the player WILL be, approximated
            // by extrapolating its position by (dist / closing-ish speed) seconds.
            // A fixed lead time scaled by distance keeps it simple + deterministic.
            const float leadTime = clampf(dist / shipai::kMaxSpeed, 0.0f, 3.0f)
                                   * shipai::kLeadFactor;
            float aimPoint[3] = {
                playerPos[0] + playerVel[0] * leadTime,
                playerPos[1] + playerVel[1] * leadTime,
                playerPos[2] + playerVel[2] * leadTime,
            };
            sub3(aimPoint, s.pos, desired);
            normalize3(desired);
            break;
        }
        case ShipAIState::Evade: {
            if (s.runMode == 2) {
                // POST-PASS PEEL: arc out ALONG the ship's own orbit direction so
                // the peel flows straight back into the same circulation — peeling
                // against it reversed the accumulated sweep and read as aimless
                // wandering instead of a fighter's racetrack pattern.
                float tang[3] = { dirToPlayer[2] * (float)s.orbitSign, 0.0f,
                                  -dirToPlayer[0] * (float)s.orbitSign };
                if (len3(tang) < 1e-4f) { tang[0] = (float)s.orbitSign; tang[2] = 0.0f; }
                normalize3(tang);
                desired[0] = -dirToPlayer[0] + 0.9f * tang[0];
                desired[1] = -dirToPlayer[1] + 0.9f * tang[1];
                desired[2] = -dirToPlayer[2] + 0.9f * tang[2];
            } else {
                // Damage/overshoot evade: peel along the reverse of the approach,
                // with a lateral component so it arcs out instead of braking dead.
                desired[0] = -dirToPlayer[0] + s.fwd[1];
                desired[1] = -dirToPlayer[1] + s.fwd[2];
                desired[2] = -dirToPlayer[2] + s.fwd[0];
            }
            normalize3(desired);
            break;
        }
        case ShipAIState::Orbit: {
            // CIRCLE the player: steer along the TANGENT of the circle around the
            // player (cross(worldUp, dirToPlayer), signed per ship), blended with
            // an inward/outward radial term that holds the orbit band around
            // kOrbitRadius, plus a small seeded vertical weave so the pass reads
            // as a 3D fighter arc, not a flat carousel disc.
            float tang[3] = { dirToPlayer[2] * (float)s.orbitSign, 0.0f,
                              -dirToPlayer[0] * (float)s.orbitSign };
            if (len3(tang) < 1e-4f) {           // player dead above/below: degenerate
                tang[0] = (float)s.orbitSign; tang[1] = 0.0f; tang[2] = 0.0f;
            }
            normalize3(tang);
            // radialErr > 0 -> too far out -> blend TOWARD the player (inward).
            const float radialErr = clampf((dist - shipai::kOrbitRadius) /
                                           shipai::kOrbitBandHalf, -1.0f, 1.0f);
            desired[0] = tang[0] + dirToPlayer[0] * radialErr * shipai::kOrbitRadialGain;
            desired[1] = tang[1] + dirToPlayer[1] * radialErr * shipai::kOrbitRadialGain
                         + shipai::kOrbitWeave * std::sin(s.orbitPhase);
            desired[2] = tang[2] + dirToPlayer[2] * radialErr * shipai::kOrbitRadialGain;
            normalize3(desired);
            break;
        }
    }

    // ---- Steer velocity toward `desired`, integrate, clamp -----------------
    // ATTACK-RUN NOSE PULL: on a strafing pass the ship BLEEDS its cross-track
    // velocity (the tangential speed it carried out of orbit) so the nose comes
    // around onto the firing line fast — without this the orbit's sideways
    // momentum keeps the target outside the fire cone for most of the pass.
    if (s.state == ShipAIState::Strafe) {
        const float vAlong = dot3(s.vel, desired);
        const float keep = std::max(0.0f, 1.0f - shipai::kRunTurnBleed * dt);
        for (int k = 0; k < 3; ++k) {
            const float cross = s.vel[k] - desired[k] * vAlong;
            s.vel[k] = desired[k] * vAlong + cross * keep;
        }
    }
    // ORBIT RADIAL SETTLE: bleed the velocity component along the player axis
    // while circling, so the peel's outward momentum converts into circulation
    // instead of porpoising the ship across the band (60 <-> 130 in/out swings
    // that read as lunging, not circling).
    if (s.state == ShipAIState::Orbit) {
        const float vRad = dot3(s.vel, dirToPlayer);   // +ve = closing on player
        const float bleed = std::min(1.0f, shipai::kOrbitRadialBleed * dt);
        for (int k = 0; k < 3; ++k)
            s.vel[k] -= dirToPlayer[k] * vRad * bleed;
    }
    for (int k = 0; k < 3; ++k)
        s.vel[k] += desired[k] * shipai::kAccel * dt;

    // Speed cap.
    const float spd = len3(s.vel);
    if (spd > shipai::kMaxSpeed) {
        const float sc = shipai::kMaxSpeed / spd;
        s.vel[0] *= sc; s.vel[1] *= sc; s.vel[2] *= sc;
    }

    // Heading follows the velocity when moving (so fwd / firing cone track the
    // ship's actual travel); otherwise it slews toward the desired direction.
    float headRef[3] = { s.vel[0], s.vel[1], s.vel[2] };
    if (len3(headRef) > 1e-3f) {
        normalize3(headRef);
        s.fwd[0] = headRef[0]; s.fwd[1] = headRef[1]; s.fwd[2] = headRef[2];
    } else {
        s.fwd[0] = desired[0]; s.fwd[1] = desired[1]; s.fwd[2] = desired[2];
    }

    // Integrate position.
    for (int k = 0; k < 3; ++k) s.pos[k] += s.vel[k] * dt;

    // ---- Fire (Strafe only, cooldown-gated, re-check cone after steering) --
    if (s.state == ShipAIState::Strafe && s.fireCooldown <= 0.0f) {
        // Re-evaluate the aim with the post-steer heading + position.
        float td[3]; sub3(playerPos, s.pos, td); normalize3(td);
        if (dot3(s.fwd, td) >= shipai::kFireConeCos && len3(td) >= 0.0f) {
            s.fireCooldown = shipai::kFireCooldown;
            ShipFireEvent ev{};
            ev.shooter = i;
            // Muzzle a little ahead of the ship along its forward.
            ev.from[0] = s.pos[0] + s.fwd[0] * 3.0f;
            ev.from[1] = s.pos[1] + s.fwd[1] * 3.0f;
            ev.from[2] = s.pos[2] + s.fwd[2] * 3.0f;
            ev.to[0] = s.pos[0] + s.fwd[0] * shipai::kLaserRange;
            ev.to[1] = s.pos[1] + s.fwd[1] * shipai::kLaserRange;
            ev.to[2] = s.pos[2] + s.fwd[2] * shipai::kLaserRange;
            fireEvents_.push_back(ev);
        }
    }
}

// ===========================================================================
// --test-ship-ai self-test (>=7 sub-checks, headless, pure logic)
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[shipai-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[shipai-test] FAIL ") + name); }
}

// Find the index of the (single) live ship, or -1.
int firstShip(const EnemyShipManager& m) { return m.count() > 0 ? 0 : -1; }

} // namespace

bool runShipAiSelfTest() {
    g_pass = g_fail = 0;
    const float dt = 1.0f / 60.0f;

    // T1 — init + spawn populates the live array. ---------------------------
    {
        EnemyShipManager m;
        m.init(8);
        check(m.count() == 0 && m.aliveCount() == 0, "T1a init -> empty");
        const float p0[3] = { 100.0f, 0.0f, 0.0f };
        const float p1[3] = { 0.0f, 40.0f, 80.0f };
        m.spawn(p0); m.spawn(p1);
        check(m.count() == 2 && m.aliveCount() == 2, "T1b spawn x2 -> count 2");
    }

    // T2 — player in detect range flips Patrol -> Engage. -------------------
    {
        EnemyShipManager m; m.init(4);
        const float sp[3] = { 200.0f, 0.0f, 0.0f };  // within kDetectRange (600)
        m.spawn(sp);
        const float pp[3] = { 0.0f, 0.0f, 0.0f };
        const float pv[3] = { 0.0f, 0.0f, 0.0f };
        // Before update the ship is Patrol.
        bool startedPatrol = m.ship(0).state == ShipAIState::Patrol;
        m.update(dt, pp, pv);
        const ShipAIState st = m.ship(0).state;
        bool engaged = st == ShipAIState::Engage || st == ShipAIState::Strafe;
        check(startedPatrol && engaged, "T2 in-range -> leaves Patrol (Engage/Strafe)");
    }

    // T3 — an engaging ship steers toward the player (distance decreases). --
    {
        EnemyShipManager m; m.init(4);
        const float sp[3] = { 400.0f, 0.0f, 0.0f };   // in detect, out of fire range
        m.spawn(sp);
        const float pp[3] = { 0.0f, 0.0f, 0.0f };
        const float pv[3] = { 0.0f, 0.0f, 0.0f };
        auto distNow = [&]() {
            const auto& s = m.ship(0);
            float d[3] = { s.pos[0]-pp[0], s.pos[1]-pp[1], s.pos[2]-pp[2] };
            return std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
        };
        const float d0 = distNow();
        for (int i = 0; i < 120; ++i) m.update(dt, pp, pv);
        const float d1 = distNow();
        check(d1 < d0 - 5.0f, "T3 Engage closes distance");
    }

    // T4 — out-of-range ship stays Patrol. ----------------------------------
    {
        EnemyShipManager m; m.init(4);
        const float sp[3] = { 5000.0f, 0.0f, 0.0f };  // way past kDetectRange
        m.spawn(sp);
        const float pp[3] = { 0.0f, 0.0f, 0.0f };
        const float pv[3] = { 0.0f, 0.0f, 0.0f };
        for (int i = 0; i < 30; ++i) m.update(dt, pp, pv);
        check(m.ship(0).state == ShipAIState::Patrol, "T4 out-of-range stays Patrol");
    }

    // T5 — a lined-up ship in fire range fires (a fire event is produced). --
    {
        EnemyShipManager m; m.init(4);
        const float sp[3] = { 100.0f, 0.0f, 0.0f };   // inside kFireRange (300)
        m.spawn(sp);
        const float pp[3] = { 0.0f, 0.0f, 0.0f };
        const float pv[3] = { 0.0f, 0.0f, 0.0f };
        // Run enough ticks for the ship to line up its cone and fire at least once.
        bool fired = false;
        for (int i = 0; i < 240 && !fired; ++i) {
            m.update(dt, pp, pv);
            if (!m.fireEvents().empty()) fired = true;
        }
        check(fired, "T5 lined-up in-range ship fires (fire event produced)");
    }

    // T6 — fire cooldown gates firing (no second shot before cooldown). -----
    {
        EnemyShipManager m; m.init(4);
        const float sp[3] = { 80.0f, 0.0f, 0.0f };
        m.spawn(sp);
        const float pp[3] = { 0.0f, 0.0f, 0.0f };
        const float pv[3] = { 0.0f, 0.0f, 0.0f };
        // Advance to the first shot.
        int shotTick = -1;
        for (int i = 0; i < 240; ++i) {
            m.update(dt, pp, pv);
            if (!m.fireEvents().empty()) { shotTick = i; break; }
        }
        bool gotFirst = shotTick >= 0;
        // The very next tick must NOT fire (cooldown just started).
        m.update(dt, pp, pv);
        bool gatedNext = m.fireEvents().empty();
        // After the full cooldown elapses, it may fire again.
        bool firedAgain = false;
        const int cdTicks = (int)(shipai::kFireCooldown / dt) + 2;
        for (int i = 0; i < cdTicks; ++i) {
            m.update(dt, pp, pv);
            if (!m.fireEvents().empty()) { firedAgain = true; break; }
        }
        check(gotFirst && gatedNext && firedAgain,
              "T6 fire cooldown gates the next shot then re-fires");
    }

    // T7 — damageShip to 0 removes the ship from aliveCount(). ---------------
    {
        EnemyShipManager m; m.init(4);
        const float a[3] = { 100.0f, 0.0f, 0.0f };
        const float b[3] = { -100.0f, 0.0f, 0.0f };
        m.spawn(a); m.spawn(b);
        const int idx = firstShip(m);
        const int maxHull = m.ship(idx).maxHull;
        bool two = m.aliveCount() == 2;
        m.damageShip(idx, maxHull / 2);            // partial — survives
        bool survived = m.aliveCount() == 2 && m.ship(idx).hull > 0;
        m.damageShip(idx, maxHull);                // lethal — removed
        bool one = m.aliveCount() == 1 && m.count() == 1;
        check(two && survived && one, "T7 damageShip to 0 removes from aliveCount");
    }

    // T8 — speed stays clamped to kMaxSpeed. --------------------------------
    {
        EnemyShipManager m; m.init(4);
        const float sp[3] = { 500.0f, 0.0f, 0.0f };
        m.spawn(sp);
        const float pp[3] = { 0.0f, 0.0f, 0.0f };
        const float pv[3] = { 0.0f, 0.0f, 0.0f };
        float maxObserved = 0.0f;
        for (int i = 0; i < 600; ++i) {
            m.update(dt, pp, pv);
            const auto& s = m.ship(0);
            const float v = std::sqrt(s.vel[0]*s.vel[0]+s.vel[1]*s.vel[1]+s.vel[2]*s.vel[2]);
            if (v > maxObserved) maxObserved = v;
        }
        check(maxObserved <= shipai::kMaxSpeed + 0.5f && maxObserved > 1.0f,
              "T8 speed clamped to kMaxSpeed");
    }

    // T9 — an in-range enemy ORBITS the player: its bearing angle around the
    //      player ADVANCES over time (it carves circles/arcs) instead of
    //      converging onto the player's coordinates and hovering. -------------
    {
        EnemyShipManager m; m.init(4);
        const float sp[3] = { 100.0f, 0.0f, 0.0f };   // inside the orbit band
        m.spawn(sp);
        const float pp[3] = { 0.0f, 0.0f, 0.0f };
        const float pv[3] = { 0.0f, 0.0f, 0.0f };
        // Accumulate the UNWRAPPED bearing (atan2 around the player in the XZ
        // plane) so full revolutions keep counting instead of wrapping to zero.
        auto bearing = [&]() {
            const auto& s = m.ship(0);
            return std::atan2(s.pos[2] - pp[2], s.pos[0] - pp[0]);
        };
        float prevB = bearing();
        double cumAngle = 0.0;          // signed, unwrapped
        float  minDist = 1e9f, maxDist = 0.0f;
        bool   sawOrbitState = false;
        const float kPi = 3.14159265358979f;
        for (int i = 0; i < 1200; ++i) {            // 20 s of flight
            m.update(dt, pp, pv);
            float b = bearing();
            float db = b - prevB;
            if (db >  kPi) db -= 2.0f * kPi;        // unwrap the seam
            if (db < -kPi) db += 2.0f * kPi;
            cumAngle += db;
            prevB = b;
            const auto& s = m.ship(0);
            const float d = std::sqrt(s.pos[0]*s.pos[0] + s.pos[1]*s.pos[1] +
                                      s.pos[2]*s.pos[2]);
            if (d < minDist) minDist = d;
            if (d > maxDist) maxDist = d;
            if (s.state == ShipAIState::Orbit) sawOrbitState = true;
        }
        // Over 20 s the ship must sweep well past a half revolution in total
        // bearing (attack runs interrupt the circle, so demand > ~pi, not 2*pi).
        check(std::fabs(cumAngle) > 3.0, "T9 in-range enemy's bearing ADVANCES (orbits)");
        check(sawOrbitState, "T9b Orbit state was entered while in the band");
        // It never converges onto the player (the standoff is 60; assert with
        // slack) and never wanders out of the fight either.
        check(minDist > 40.0f && maxDist < 400.0f,
              "T9c orbiting enemy holds a band (no convergence, no fly-away)");
        x3::logInfo("  [info] T9 cumBearing=" + std::to_string(cumAngle) +
                    " rad over 20 s, dist band [" + std::to_string(minDist) +
                    ", " + std::to_string(maxDist) + "] m");
    }

    // T10 — NEGATIVE CONTROL for the bearing metric: a converge-and-hover
    //       trajectory (the OLD behaviour: close on the player, park on the
    //       standoff sphere) accumulates ~zero bearing. Proves T9's assertion
    //       actually discriminates orbiting from hovering. -------------------
    {
        const float pp[3] = { 0.0f, 0.0f, 0.0f };
        float pos[3] = { 100.0f, 0.0f, 0.0f };
        float prevB = std::atan2(pos[2], pos[0]);
        double cumAngle = 0.0;
        const float kPi = 3.14159265358979f;
        for (int i = 0; i < 1200; ++i) {
            // Straight-line converge at 60 m/s until the 60 m standoff, then park.
            const float d = std::sqrt(pos[0]*pos[0] + pos[1]*pos[1] + pos[2]*pos[2]);
            if (d > 60.0f) {
                const float step = 60.0f * dt / d;
                pos[0] -= pos[0] * step; pos[1] -= pos[1] * step; pos[2] -= pos[2] * step;
            }
            float b = std::atan2(pos[2], pos[0]);
            float db = b - prevB;
            if (db >  kPi) db -= 2.0f * kPi;
            if (db < -kPi) db += 2.0f * kPi;
            cumAngle += db;
            prevB = b;
        }
        check(std::fabs(cumAngle) < 0.3,
              "T10 negative control: converge-and-hover reads ~zero bearing sweep");
    }

    x3::logInfo(std::string("[shipai-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::space
