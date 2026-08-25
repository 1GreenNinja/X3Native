// app/space/ship_damage.cpp — S10 ship damage model implementation + self-test.
//
// Pure value-type logic; no GPU, no physics. See ship_damage.h for the contract.
#include "ship_damage.h"

// The bay-launch acceptance test (T27) drives the real EnemyShipManager: a
// fighter that spawns AT a bay mouth and flies OUT along the launch vector is
// the whole point of item G, and asserting it against the actual manager (not
// a stand-in) is what makes it a test rather than a restatement.
#include "ship_ai.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

namespace x3::space {

namespace {
// Clamp a non-negative ceiling: keeps an int pool in [0, hi].
inline int clampPool(int v, int hi) { return v < 0 ? 0 : (v > hi ? hi : v); }
} // namespace

ShipDamageModel ShipDamage::makeFighter(int shield, int hull) {
    ShipDamageModel m{};
    m.shield = m.maxShield = std::max(0, shield);
    m.hull   = m.maxHull   = std::max(0, hull);
    // Mirror space-pilot's player defaults so player + enemy fighters feel alike.
    m.shieldRegenPerSec   = 25.0f;
    m.shieldRegenDelaySec = 4.0f;
    m.timeSinceHit        = m.shieldRegenDelaySec; // start "rested" (regen-ready)
    m.hasSubsystems       = false;
    return m;
}

ShipDamageModel ShipDamage::makeCapital(int shield, int hull, int subHp) {
    ShipDamageModel m{};
    m.shield = m.maxShield = std::max(0, shield);
    m.hull   = m.maxHull   = std::max(0, hull);
    // Capitals: bigger pools, slower & more delayed shield recovery.
    m.shieldRegenPerSec   = 15.0f;
    m.shieldRegenDelaySec = 6.0f;
    m.timeSinceHit        = m.shieldRegenDelaySec;
    m.hasSubsystems       = true;
    const int sh = std::max(0, subHp);
    for (int i = 0; i < (int)Subsystem::Count; ++i) {
        m.subHp[i] = m.subMaxHp[i] = sh;
    }
    return m;
}

void ShipDamage::applyDamage(ShipDamageModel& m, int amount, Subsystem hitSub) {
    if (amount <= 0)   return;          // no negative / zero damage
    if (m.hull <= 0)   return;          // already destroyed — inert

    // Any hit restarts the regen-delay clock (matches space_pilot::takeDamage).
    m.timeSinceHit = 0.0f;

    // Subsystem routing keys off whether the shield was ALREADY down when the
    // shot landed — a single hit cannot both break the shield AND damage a
    // subsystem (that overflow bleeds to hull). Capture it before absorption.
    const bool shieldWasDown = (m.shield == 0);

    int remaining = amount;

    // 1. Shield absorbs first.
    if (m.shield > 0) {
        const int absorbed = std::min(m.shield, remaining);
        m.shield  -= absorbed;
        remaining -= absorbed;
    }
    if (remaining <= 0) return; // fully soaked by the shield

    // 2/3. Overflow. If a real subsystem is targeted AND this ship HAS
    // subsystems AND the shield was ALREADY down, the overflow is routed into
    // that subsystem instead of the hull. Otherwise it bleeds to the hull.
    const bool routeToSub =
        m.hasSubsystems &&
        hitSub != Subsystem::Count &&
        (uint32_t)hitSub < (uint32_t)Subsystem::Count &&
        shieldWasDown;

    if (routeToSub) {
        int& sub = m.subHp[(int)hitSub];
        sub -= remaining;
        if (sub < 0) sub = 0;          // floored at 0 == "down"
    } else {
        // THE SHIELD GATE (item F). Overflow from a hit that landed while the
        // shield was still UP is scaled by hullBleedWhileShielded (1.0 ==
        // legacy: it all bleeds through, which is every fighter and every
        // pre-existing caller). A capital sets this LOW so hosing bare hull
        // through a live bubble is the SLOW path and dropping the shield — or
        // killing its generator — is the lesson the encounter teaches. Once
        // the shield is already down, the full amount lands unscaled exactly
        // as it always has.
        int toHull = remaining;
        if (!shieldWasDown) {
            float k = m.hullBleedWhileShielded;
            k = k < 0.0f ? 0.0f : (k > 1.0f ? 1.0f : k);
            toHull = (int)(remaining * k);
            // A non-zero gate never rounds a landed hit down to nothing: the
            // player must always see the hull bar move, however slowly.
            if (toHull <= 0 && k > 0.0f) toHull = 1;
        }
        if (toHull > 0) {
            m.hull -= toHull;
            if (m.hull < 0) m.hull = 0;    // floored at 0 == destroyed
            if (m.hull == 0) {
                x3::logInfo("[ship-damage] HULL 0 — ship destroyed");
            }
        }
    }
}

void ShipDamage::tick(ShipDamageModel& m, float dt) {
    if (dt <= 0.0f) return;
    if (m.hull <= 0) return; // dead ships don't regen

    m.timeSinceHit += dt;

    // Shield regen resumes only after the delay has fully elapsed since the
    // last hit, and never at all while regen is SUPPRESSED (the host raises
    // shieldRegenDisabled when the shield generator subsystem is destroyed).
    // Subsystems intentionally do NOT auto-regen (host must repair).
    if (!m.shieldRegenDisabled &&
        m.timeSinceHit >= m.shieldRegenDelaySec && m.shield < m.maxShield) {
        const int regen = (int)std::ceil(m.shieldRegenPerSec * dt);
        m.shield = clampPool(m.shield + regen, m.maxShield);
    }
}

bool ShipDamage::isDestroyed(const ShipDamageModel& m) { return m.hull <= 0; }

bool ShipDamage::subsystemDown(const ShipDamageModel& m, Subsystem s) {
    if (!m.hasSubsystems) return false;
    if ((uint32_t)s >= (uint32_t)Subsystem::Count) return false;
    return m.subHp[(int)s] <= 0;
}

float ShipDamage::shieldFrac(const ShipDamageModel& m) {
    if (m.maxShield <= 0) return 0.0f;
    return (float)m.shield / (float)m.maxShield;
}

float ShipDamage::hullFrac(const ShipDamageModel& m) {
    if (m.maxHull <= 0) return 0.0f;
    return (float)m.hull / (float)m.maxHull;
}

// ===========================================================================
// CAPITAL DEATH SEQUENCE
// ===========================================================================
namespace {

// One entry of the compile-time death schedule. Positions are expressed in a
// HULL-LOCAL frame so the table stays readable and the world resolve is one
// place: `u` walks the long axis in [-1, +1] (-1 = bow, +1 = stern) and
// (lat, vert) offset off the spine in metres, scaled by the hull half-length
// for `u` only. `radius` is the blast/plume size in metres.
struct DeathBeat {
    float   t;        // seconds after begin()
    DeathFx kind;
    float   u;        // position along the hull axis, [-1, +1]
    float   lat;      // lateral offset off the spine (m)
    float   vert;     // vertical offset off the spine (m)
    float   radius;   // blast / plume radius (m)
};

// THE SCHEDULE. Read it top to bottom and you are reading the shot list.
//
//   0.00  the hull ruptures at the ventral reactor wound — white-hot, a
//         shockwave shell, the loudest single event of the fight;
//   0.14  the cascade starts at the BOW and walks aft, one secondary every
//         ~0.19 s. Each is smaller than the rupture but there are twelve of
//         them and they are ordered in space, so the eye tracks the damage
//         RUNNING down 450 m of hull — that is the "structural" read;
//   0.30+ vents open between the detonations (atmosphere, coolant, plasma)
//         and keep streaming after the cascade passes;
//   1.45  hull sections start tearing free (heavy chunky debris);
//   2.60  the reactor lets go. Biggest blast, second shockwave, the heaviest
//         fragment shed. Everything after this is the wreck venting as it
//         rolls over and goes down.
constexpr DeathBeat kSchedule[] = {
    // t      kind                    u      lat    vert   radius
    { 0.00f, DeathFx::Rupture,      -0.10f,  0.0f, -32.0f, 175.0f },
    { 0.14f, DeathFx::Cascade,      -0.95f,  8.0f,   6.0f,  74.0f },
    { 0.30f, DeathFx::Vent,         -0.80f, 26.0f,  10.0f,  26.0f },
    { 0.33f, DeathFx::Cascade,      -0.78f, -14.0f, 12.0f,  70.0f },
    { 0.52f, DeathFx::Cascade,      -0.60f,  16.0f, -9.0f,  67.0f },
    { 0.68f, DeathFx::Vent,         -0.45f, -30.0f,  4.0f,  24.0f },
    { 0.71f, DeathFx::Cascade,      -0.42f, -11.0f, 15.0f,  64.0f },
    { 0.90f, DeathFx::Cascade,      -0.24f,  13.0f, -13.0f, 61.0f },
    { 1.09f, DeathFx::Cascade,      -0.06f, -17.0f,  7.0f,  58.0f },
    { 1.20f, DeathFx::Vent,          0.05f,  28.0f, -16.0f, 23.0f },
    { 1.28f, DeathFx::Cascade,       0.12f,  10.0f,  16.0f, 56.0f },
    { 1.45f, DeathFx::HullFragment,  0.20f, -22.0f,  2.0f,  40.0f },
    { 1.47f, DeathFx::Cascade,       0.30f, -15.0f, -11.0f, 53.0f },
    { 1.66f, DeathFx::Cascade,       0.48f,  18.0f,  9.0f,  50.0f },
    { 1.80f, DeathFx::Vent,          0.56f, -26.0f, 18.0f,  22.0f },
    { 1.85f, DeathFx::Cascade,       0.66f, -12.0f, -14.0f, 47.0f },
    { 2.04f, DeathFx::Cascade,       0.84f,  14.0f,  5.0f,  44.0f },
    { 2.15f, DeathFx::HullFragment,  0.92f,  20.0f, -8.0f,  38.0f },
    { 2.23f, DeathFx::Cascade,       0.97f, -9.0f,  -6.0f,  41.0f },
    { 2.60f, DeathFx::CoreDetonation, 0.0f,  0.0f, -18.0f, 265.0f },
    { 2.66f, DeathFx::HullFragment,  0.34f,  0.0f,  24.0f,  46.0f },
    { 2.74f, DeathFx::HullFragment, -0.38f,  0.0f, -24.0f,  46.0f },
    { 3.05f, DeathFx::Vent,         -0.55f, -24.0f, 12.0f,  28.0f },
    { 3.40f, DeathFx::Vent,          0.40f,  24.0f, -12.0f, 28.0f },
    { 3.55f, DeathFx::Cascade,       0.10f, -18.0f, 20.0f,  34.0f },
    { 3.95f, DeathFx::Vent,         -0.20f,  20.0f,  18.0f, 26.0f },
    { 4.30f, DeathFx::Cascade,      -0.70f,  12.0f, -18.0f, 30.0f },
    { 4.62f, DeathFx::Vent,          0.72f, -20.0f, -6.0f,  25.0f },
};
constexpr int kScheduleCount = (int)(sizeof(kSchedule) / sizeof(kSchedule[0]));

// THE DEORBIT SCHEDULE (DeathOutcome::DeorbitCrash). Same grammar, different
// story: the rupture and a SHORTENED bow->stern cascade kill her (0.0-1.5 s),
// the lights go out, and then instead of cooking off in place the wreck FALLS
// toward the planet (plunge ramps from kDeorbitFallStart), venting a burning
// entry trail, sheds two hull sections under aero-stress, and terminates in
// one distant CoreDetonation flash — the impact read on the disc — followed
// by the dissipating plume. "Crash land on a planet", visible end to end.
constexpr DeathBeat kDeorbitSchedule[] = {
    // t      kind                    u      lat    vert   radius
    { 0.00f, DeathFx::Rupture,      -0.10f,  0.0f, -32.0f, 175.0f },
    { 0.16f, DeathFx::Cascade,      -0.92f,  9.0f,   5.0f,  72.0f },
    { 0.36f, DeathFx::Cascade,      -0.66f, -13.0f, 11.0f,  67.0f },
    { 0.42f, DeathFx::Vent,         -0.52f,  27.0f,  8.0f,  25.0f },
    { 0.58f, DeathFx::Cascade,      -0.38f,  15.0f, -10.0f, 63.0f },
    { 0.80f, DeathFx::Cascade,      -0.10f, -16.0f,  9.0f,  59.0f },
    { 0.95f, DeathFx::Vent,          0.02f, -28.0f, -14.0f, 24.0f },
    { 1.06f, DeathFx::Cascade,       0.22f,  12.0f,  14.0f, 55.0f },
    { 1.30f, DeathFx::Cascade,       0.55f, -14.0f, -12.0f, 51.0f },
    { 1.50f, DeathFx::HullFragment,  0.30f, -20.0f,  4.0f,  42.0f },
    // ---- the fall: entry trail + stress shedding --------------------------
    { 2.60f, DeathFx::Vent,         -0.30f,  22.0f,  10.0f, 30.0f },
    { 3.30f, DeathFx::Vent,          0.20f, -22.0f, -10.0f, 30.0f },
    { 3.60f, DeathFx::Cascade,      -0.05f,  10.0f, -16.0f, 40.0f },
    { 4.00f, DeathFx::Vent,         -0.45f, -18.0f,  14.0f, 28.0f },
    { 4.40f, DeathFx::HullFragment,  0.55f,  16.0f,  -6.0f, 44.0f },
    { 4.80f, DeathFx::Vent,          0.35f,  20.0f,  12.0f, 28.0f },
    { 5.00f, DeathFx::Cascade,      -0.60f, -12.0f, -10.0f, 38.0f },
    { 5.60f, DeathFx::Vent,         -0.15f,  24.0f,  -8.0f, 26.0f },
    { 6.40f, DeathFx::Vent,          0.10f, -20.0f,  16.0f, 26.0f },
    // ---- terminal: the impact flash on the disc, then the plume dies ------
    { 7.90f, DeathFx::CoreDetonation, 0.0f,  0.0f, -12.0f, 300.0f },
    { 8.30f, DeathFx::Vent,         -0.20f,  18.0f,  10.0f, 30.0f },
    { 8.60f, DeathFx::Vent,          0.25f, -16.0f, -12.0f, 30.0f },
};
constexpr int kDeorbitScheduleCount =
    (int)(sizeof(kDeorbitSchedule) / sizeof(kDeorbitSchedule[0]));

// Deorbit fall shape: the wreck starts losing station at kDeorbitFallStart and
// accelerates toward the planet at kDeorbitFallAccel (a READ, not physics —
// ~790 m travelled by 9 s, which at the 2.6 km arena distance is a clearly
// visible recession toward the disc). The entry burn ramps over [2.8, 5.2] s.
constexpr float kDeorbitFallStart = 2.2f;
constexpr float kDeorbitFallAccel = 34.0f;   // m/s^2 along planetDir

// LIGHTS-OUT curve. Full brightness through the rupture, two stutters as the
// power net collapses, then a hard fall to the floor by ~1.9 s.
float lightsAt(float t) {
    if (t < 0.18f) return 1.0f;
    if (t < 0.34f) return 0.22f;                       // first blackout stutter
    if (t < 0.44f) return 0.80f;                       // it fights back
    if (t < 0.58f) return 0.15f;                       // second stutter
    if (t < 0.70f) return 0.55f;
    if (t >= 1.90f) return CapitalDeathSequence::kLightsOutFloor;
    // Final collapse: 0.55 -> floor over [0.70, 1.90].
    const float k = (t - 0.70f) / 1.20f;
    return 0.55f + (CapitalDeathSequence::kLightsOutFloor - 0.55f) * k;
}

} // namespace

int CapitalDeathSequence::scheduleSize() { return kScheduleCount; }

void CapitalDeathSequence::begin(CapitalDeathState& s, const float center[3],
                                 const float axis[3], float halfLen) {
    const float noDir[3] = { 0.0f, 0.0f, 0.0f };
    begin(s, center, axis, halfLen, DeathOutcome::BreakupInSpace, noDir);
}

void CapitalDeathSequence::begin(CapitalDeathState& s, const float center[3],
                                 const float axis[3], float halfLen,
                                 DeathOutcome outcome, const float planetDir[3]) {
    s = CapitalDeathState{};          // full reset — re-arming a used state is safe
    s.active = true;
    s.center[0] = center[0]; s.center[1] = center[1]; s.center[2] = center[2];
    float a[3] = { axis[0], axis[1], axis[2] };
    const float al = std::sqrt(a[0]*a[0] + a[1]*a[1] + a[2]*a[2]);
    if (al > 1e-5f) { a[0] /= al; a[1] /= al; a[2] /= al; }
    else            { a[0] = 1.0f; a[1] = 0.0f; a[2] = 0.0f; }
    s.axis[0] = a[0]; s.axis[1] = a[1]; s.axis[2] = a[2];
    s.halfLen = halfLen > 1.0f ? halfLen : 1.0f;
    s.lights = 1.0f;
    // Outcome: a crash with nowhere to crash demotes to the in-place breakup.
    float p[3] = { planetDir[0], planetDir[1], planetDir[2] };
    const float pl = std::sqrt(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
    if (outcome == DeathOutcome::DeorbitCrash && pl > 1e-5f) {
        s.outcome = DeathOutcome::DeorbitCrash;
        s.planetDir[0] = p[0] / pl;
        s.planetDir[1] = p[1] / pl;
        s.planetDir[2] = p[2] / pl;
    } else {
        s.outcome = DeathOutcome::BreakupInSpace;
    }
}

int CapitalDeathSequence::step(CapitalDeathState& s, float dt,
                               DeathEvent* out, int maxOut) {
    if (!s.active) return 0;
    if (dt > 0.0f) s.t += dt;

    const bool deorbit = (s.outcome == DeathOutcome::DeorbitCrash);
    const DeathBeat* sched = deorbit ? kDeorbitSchedule : kSchedule;
    const int schedCount   = deorbit ? kDeorbitScheduleCount : kScheduleCount;
    const float duration   = durationFor(s.outcome);

    // ---- Presentation scalars ------------------------------------------
    s.lights = lightsAt(s.t);
    // Dead-stick roll: angular rate eases in as the attitude thrusters die,
    // asymptoting near 0.5 rad/s, so the hull is visibly rolling by ~1 s and
    // has turned ~100 deg by the end of the window. The deorbit rolls slower
    // (0.22 rad/s cap): the fall + burn carry that ending, and a fast corkscrew
    // would fight the "she's going IN" read.
    {
        const float cap0 = deorbit ? 0.22f : 0.50f;
        const float rate = cap0 * (1.0f - std::exp(-s.t * 1.30f));
        s.tumble += rate * (dt > 0.0f ? dt : 0.0f);
    }
    if (!deorbit) {
        // She stops holding station and goes down: a gentle constant
        // acceleration (not physics — a READ). ~11 m by 3 s, ~31 m by 5 s.
        s.sag    = 1.25f * s.t * s.t;
        s.plunge = 0.0f;
        s.burn   = 0.0f;
    } else {
        // DEORBIT: the read is the FALL toward the planet, not the sag.
        s.sag = 0.30f * s.t * s.t;
        const float tf = s.t - kDeorbitFallStart;
        s.plunge = tf > 0.0f ? 0.5f * kDeorbitFallAccel * tf * tf : 0.0f;
        // Entry burn ramps over [2.8, 5.2] s and holds until the impact flash.
        float k = (s.t - 2.8f) / 2.4f;
        k = k < 0.0f ? 0.0f : (k > 1.0f ? 1.0f : k);
        s.burn = k * k * (3.0f - 2.0f * k);
    }

    // ---- Emit every beat that came due, up to the budget ----------------
    int n = 0;
    const int cap = maxOut < kMaxEventsPerStep ? maxOut : kMaxEventsPerStep;
    while (n < cap && s.cursor < schedCount && sched[s.cursor].t <= s.t) {
        const DeathBeat& b = sched[s.cursor];
        ++s.cursor;
        if (!out) continue;   // caller only wants the cursor advanced

        // Build an orthonormal frame around the hull axis so `lat`/`vert`
        // resolve consistently for any axis the host passes.
        const float* ax = s.axis;
        float up[3] = { 0.0f, 1.0f, 0.0f };
        if (std::fabs(ax[1]) > 0.95f) { up[0] = 1.0f; up[1] = 0.0f; }
        float rt[3] = { up[1]*ax[2] - up[2]*ax[1],
                        up[2]*ax[0] - up[0]*ax[2],
                        up[0]*ax[1] - up[1]*ax[0] };
        const float rl = std::sqrt(rt[0]*rt[0] + rt[1]*rt[1] + rt[2]*rt[2]);
        if (rl > 1e-5f) { rt[0] /= rl; rt[1] /= rl; rt[2] /= rl; }
        const float tu[3] = { ax[1]*rt[2] - ax[2]*rt[1],
                              ax[2]*rt[0] - ax[0]*rt[2],
                              ax[0]*rt[1] - ax[1]*rt[0] };

        DeathEvent& e = out[n++];
        e.kind   = b.kind;
        e.radius = b.radius;
        const float along = b.u * s.halfLen;
        for (int k = 0; k < 3; ++k) {
            // The wreck's EFFECTIVE centre rides the deorbit plunge (0 for
            // the in-place breakup), so the trail beats fire ON the falling
            // hull, not at the point where she died.
            e.pos[k] = s.center[k] + s.planetDir[k] * s.plunge
                                   + ax[k] * along
                                   + rt[k] * b.lat
                                   + tu[k] * b.vert;
            // The hull is already sagging when the later beats fire.
            if (k == 1) e.pos[k] -= s.sag;
            // Plumes and debris stream AWAY from the spine — and, in the
            // deorbit, BACKWARD along the fall (the entry trail).
            e.drift[k] = rt[k] * b.lat * 0.35f + tu[k] * b.vert * 0.35f
                       - s.planetDir[k] * s.burn * 18.0f;
        }
        // A vent with no lateral offset would have no drift at all; give it
        // a small outward push so the plume never hangs as a static bead.
        if (std::fabs(b.lat) < 1e-3f && std::fabs(b.vert) < 1e-3f)
            for (int k = 0; k < 3; ++k) e.drift[k] = tu[k] * 6.0f;
    }

    if (s.cursor >= schedCount && s.t >= duration) {
        s.finished = true;
        s.active   = false;
    }
    return n;
}

// ===========================================================================
// CAPITAL MOTION
// ===========================================================================
void CapitalMotion::begin(CapitalMotionState& s, const float startPos[3]) {
    s = CapitalMotionState{};
    // Anchor behind the start point on -Z so that arcPhase = +pi/2 puts the
    // hull EXACTLY at startPos with the arc tangent = (-1, 0, 0): the nose
    // faces the player's approach lane, matching the reveal framing.
    s.anchor[0] = startPos[0];
    s.anchor[1] = startPos[1];
    s.anchor[2] = startPos[2] - s.radius;
    s.arcPhase  = 1.57079632679f;   // pi/2
    s.speed     = kPatrolSpeed;     // she is ALREADY under way at the reveal
    s.pos[0] = startPos[0]; s.pos[1] = startPos[1]; s.pos[2] = startPos[2];
    s.fwd[0] = -1.0f; s.fwd[1] = 0.0f; s.fwd[2] = 0.0f;
    s.vel[0] = -kPatrolSpeed; s.vel[1] = 0.0f; s.vel[2] = 0.0f;
    s.phase  = CapitalMovePhase::Patrol;
}

void CapitalMotion::update(CapitalMotionState& s, float dt,
                           bool shieldsDown, bool adrift) {
    if (dt < 0.0f) dt = 0.0f;
    s.phase = adrift      ? CapitalMovePhase::Adrift
            : shieldsDown ? CapitalMovePhase::Combat
                          : CapitalMovePhase::Patrol;
    const float target = adrift ? 0.0f
                       : (shieldsDown ? kCombatSpeed : kPatrolSpeed);
    // dt-correct exponential ease (the 165 Hz rule: never per-frame).
    s.speed += (target - s.speed) * (1.0f - std::exp(-kSpeedEase * dt));
    // Advance along the arc. pos is DERIVED from the phase each step, so the
    // hull can never leave the circle no matter the dt stream: the arc IS the
    // arena bound.
    s.arcPhase += (s.speed / s.radius) * dt;
    const float c = std::cos(s.arcPhase), n = std::sin(s.arcPhase);
    s.pos[0] = s.anchor[0] + s.radius * c;
    s.pos[1] = s.anchor[1];
    s.pos[2] = s.anchor[2] + s.radius * n;
    // Tangent heading (d pos / d arcPhase, normalized) + true velocity.
    // Below a crawl the heading HOLDS (an adrift hull keeps its last aspect;
    // a zero-speed tangent would still be well-defined, but the freeze must
    // not swing the nose as the ease crosses zero asymptotically).
    if (s.speed > 0.05f) {
        s.fwd[0] = -n; s.fwd[1] = 0.0f; s.fwd[2] = c;
    }
    s.vel[0] = -n * s.speed; s.vel[1] = 0.0f; s.vel[2] = c * s.speed;
}

// ===========================================================================
// CAPITAL GUN BATTERY
// ===========================================================================
void CapitalBattery::init(CapitalBatteryState& s) {
    s = CapitalBatteryState{};
    for (int i = 0; i < kCapitalGunCount; ++i) {
        s.gun[i].hp = s.gun[i].maxHp = kGunHp;
        // Staggered opening: gun i's first spool completes at ~1.2 * (i+1) s.
        s.gun[i].cd     = (kGunPeriod / (float)kCapitalGunCount) * (float)(i + 1)
                          - kGunSpool;
        s.gun[i].spoolT = -1.0f;
    }
}

uint32_t CapitalBattery::update(CapitalBatteryState& s, float dt, bool inRange,
                                uint32_t* spooled) {
    uint32_t fired = 0u, sp = 0u;
    for (int i = 0; i < kCapitalGunCount; ++i) {
        CapitalGun& g = s.gun[i];
        if (g.hp <= 0) { g.spoolT = -1.0f; continue; }   // shot off: silent forever
        if (!inRange)  { g.spoolT = -1.0f; continue; }   // stand down, keep cd
        if (g.spoolT < 0.0f) {
            g.cd -= dt;
            if (g.cd <= 0.0f) {
                g.spoolT = kGunSpool;                    // telegraph starts
                sp |= (1u << i);
            }
        } else {
            g.spoolT -= dt;
            if (g.spoolT <= 0.0f) {                      // the bolt
                g.spoolT = -1.0f;
                g.cd     = kGunPeriod;
                fired |= (1u << i);
            }
        }
    }
    if (spooled) *spooled = sp;
    return fired;
}

bool CapitalBattery::gunAlive(const CapitalBatteryState& s, int i) {
    return i >= 0 && i < kCapitalGunCount && s.gun[i].hp > 0;
}

int CapitalBattery::aliveGuns(const CapitalBatteryState& s) {
    int n = 0;
    for (int i = 0; i < kCapitalGunCount; ++i) if (s.gun[i].hp > 0) ++n;
    return n;
}

bool CapitalBattery::damageGun(CapitalBatteryState& s, int i, int amount) {
    if (i < 0 || i >= kCapitalGunCount || amount <= 0) return false;
    CapitalGun& g = s.gun[i];
    if (g.hp <= 0) return false;
    g.hp -= amount;
    if (g.hp <= 0) { g.hp = 0; g.spoolT = -1.0f; return true; }
    return false;
}

// ===========================================================================
// CAPITAL AIM PICK — the ONE ray test shared by the fire path and the hover
// highlight (item F). See ship_damage.h for why they must be the same test.
// ===========================================================================
float CapitalAim::growFactor(float dist) {
    const float f = dist / kGrowStart;
    return f < 1.0f ? 1.0f : (f > kGrowMax ? kGrowMax : f);
}

CapitalAimResult CapitalAim::pick(const CapitalAimTarget* targets, uint32_t n,
                                  const float origin[3], const float dir[3]) {
    CapitalAimResult best{};
    if (!targets || n == 0 || !origin || !dir) return best;
    // Normalize defensively — callers hand us a nose vector built from angles.
    float d[3] = { dir[0], dir[1], dir[2] };
    const float dl = std::sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
    if (dl < 1e-6f) return best;
    d[0] /= dl; d[1] /= dl; d[2] /= dl;

    // Ray-vs-sphere entry parameter; < 0 == miss (or behind the nose).
    auto entry = [&](const float c[3], float r) -> float {
        const float oc[3] = { c[0]-origin[0], c[1]-origin[1], c[2]-origin[2] };
        const float tca = oc[0]*d[0] + oc[1]*d[1] + oc[2]*d[2];
        if (tca < 0.0f) return -1.0f;
        const float d2c = oc[0]*oc[0] + oc[1]*oc[1] + oc[2]*oc[2] - tca*tca;
        if (d2c > r * r) return -1.0f;
        const float e = tca - std::sqrt(r*r - d2c);
        return e < 0.0f ? 0.0f : e;   // origin inside the sphere => entry now
    };

    // ---- THE HULL, and what it is FOR ------------------------------------
    // The hull sphere is a crude BOUNDING volume for a long, thin ship, so it
    // encloses essentially every hardpoint, gun mount and bay on the vessel.
    // A naive nearest-entry sweep therefore lets the hull win every single
    // ray and NOTHING on the ship is ever hoverable or hittable — the parts
    // are visibly bolted to the outside of a hull the ray has already entered.
    // (That was live: at the shipped 1x anatomy the ventral gun mounts sat
    // 154 m out inside a 240 m hull sphere, so the battery could never be
    // shot off by the ray path at all.)
    //
    // So the hull is treated as an OCCLUDER, not a competitor. A part is
    // hidden by the hull exactly when it lies on the FAR hemisphere with
    // respect to the ray — behind the ship's own mass from where the player
    // is looking. That preserves the rule the encounter is built on ("you
    // cannot snipe the engines through 1.8 km of ship, you fly around") while
    // making every part on the near side pickable at any scale, which is what
    // item F needs. Parts within a small margin of the terminator stay
    // visible so a part sitting on the equator does not flicker.
    const CapitalAimTarget* hull = nullptr;
    for (uint32_t i = 0; i < n; ++i)
        if (targets[i].kind == CapitalAimKind::Hull && targets[i].alive &&
            targets[i].radius > 0.0f) { hull = &targets[i]; break; }

    float bestT = 1e30f;
    for (uint32_t i = 0; i < n; ++i) {
        const CapitalAimTarget& t = targets[i];
        // A DEAD part is not a target: it never highlights and never claims a
        // shot (hits there fall through to whatever is behind it — the hull).
        if (!t.alive || t.kind == CapitalAimKind::None || t.radius <= 0.0f)
            continue;
        if (t.kind == CapitalAimKind::Hull) continue;      // occluder, not a rival
        if (hull) {
            const float off[3] = { t.pos[0]-hull->pos[0], t.pos[1]-hull->pos[1],
                                   t.pos[2]-hull->pos[2] };
            if (off[0]*d[0] + off[1]*d[1] + off[2]*d[2] > 0.15f * hull->radius)
                continue;                                  // far side: occluded
        }
        const float oc[3] = { t.pos[0]-origin[0], t.pos[1]-origin[1],
                              t.pos[2]-origin[2] };
        const float dist = std::sqrt(oc[0]*oc[0] + oc[1]*oc[1] + oc[2]*oc[2]);
        // Aim-at-what-you-see: small parts grow their acceptance with range,
        // so a blister that is 6 px wide on screen is still hoverable.
        const float tEnter = entry(t.pos, t.radius * growFactor(dist));
        if (tEnter < 0.0f || tEnter >= bestT) continue;
        bestT = tEnter;
        best.kind = t.kind; best.index = t.index; best.t = tEnter;
    }
    if (best.kind != CapitalAimKind::None) return best;
    // Nothing aimable on the near side — did the ray hit the ship at all?
    if (hull) {
        const float tH = entry(hull->pos, hull->radius);
        if (tH >= 0.0f) { best.kind = CapitalAimKind::Hull; best.index = -1;
                          best.t = tH; }
    }
    return best;
}

// ===========================================================================
// CAPITAL LAUNCH BAYS (item G)
// ===========================================================================
void CapitalBays::init(CapitalBayState& s) {
    for (int i = 0; i < kCapitalBayCount; ++i) {
        s.bay[i].hp = s.bay[i].maxHp = kBayHp;
        // Staggered opening clocks: bay i throws its first fighter at
        // ~(i+1) * kLaunchPeriod/kCapitalBayCount seconds, so the three mouths
        // trickle rather than disgorging one synchronized lump.
        s.bay[i].cd = kLaunchPeriod * (float)(i + 1) / (float)kCapitalBayCount;
    }
}

float CapitalBays::periodFor(float hullFrac) {
    const float h = hullFrac < 0.0f ? 0.0f : (hullFrac > 1.0f ? 1.0f : hullFrac);
    // Full hull => the base period; zero hull => kRateFloor of it. Linear and
    // monotone, so the fight measurably escalates as she loses.
    return kLaunchPeriod * (kRateFloor + (1.0f - kRateFloor) * h);
}

uint32_t CapitalBays::update(CapitalBayState& s, float dt, int liveFighters,
                             float hullFrac, bool enabled) {
    uint32_t launched = 0u;
    if (dt <= 0.0f) return launched;
    int live = liveFighters < 0 ? 0 : liveFighters;
    const float period = periodFor(hullFrac);
    for (int i = 0; i < kCapitalBayCount; ++i) {
        CapitalBay& b = s.bay[i];
        if (b.hp <= 0) continue;                 // blown: this stream is off
        b.cd -= dt;                              // dt-integrated, never per-frame
        if (b.cd > 0.0f) continue;
        // AT THE CAP: hold the clock at zero (do NOT reset it) so the bay
        // launches the instant a slot frees — pressure resumes rather than
        // restarting, and the cap can never be exceeded.
        if (!enabled || live >= kLiveCap) { b.cd = 0.0f; continue; }
        launched |= (1u << i);
        ++live;
        b.cd = period;
    }
    return launched;
}

bool CapitalBays::bayAlive(const CapitalBayState& s, int i) {
    if (i < 0 || i >= kCapitalBayCount) return false;
    return s.bay[i].hp > 0;
}

int CapitalBays::aliveBays(const CapitalBayState& s) {
    int n = 0;
    for (int i = 0; i < kCapitalBayCount; ++i) if (s.bay[i].hp > 0) ++n;
    return n;
}

bool CapitalBays::damageBay(CapitalBayState& s, int i, int amount) {
    if (i < 0 || i >= kCapitalBayCount || amount <= 0) return false;
    CapitalBay& b = s.bay[i];
    if (b.hp <= 0) return false;
    b.hp -= amount;
    if (b.hp <= 0) { b.hp = 0; b.cd = 0.0f; return true; }
    return false;
}

// ----------------------------------------------------------------------------
// --test-ship-damage self-test. Deterministic, no GPU.
// ----------------------------------------------------------------------------
bool runShipDamageSelfTest() {
    int pass = 0, total = 0;
    auto check = [&](bool c, const char* name) {
        ++total;
        if (c) { ++pass; x3::logInfo(std::string("  [ok] ") + name); }
        else   {          x3::logError(std::string("  [FAIL] ") + name); }
    };

    // T1 — makeFighter seeds shield + hull full, no subsystems.
    {
        ShipDamageModel m = ShipDamage::makeFighter(50, 100);
        check(m.shield == 50 && m.maxShield == 50 &&
              m.hull == 100 && m.maxHull == 100 &&
              !m.hasSubsystems,
              "T1 makeFighter seeds shield+hull full, no subsystems");
        check(std::fabs(ShipDamage::shieldFrac(m) - 1.0f) < 1e-5f &&
              std::fabs(ShipDamage::hullFrac(m)   - 1.0f) < 1e-5f,
              "T1b fresh fighter shieldFrac/hullFrac == 1");
    }

    // T2 — damage drains shield BEFORE hull (partial shield hit).
    {
        ShipDamageModel m = ShipDamage::makeFighter(50, 100);
        ShipDamage::applyDamage(m, 30);
        check(m.shield == 20 && m.hull == 100,
              "T2 partial hit drains shield only (shield 20, hull 100)");
    }

    // T3 — overflow math EXACT: shield 50, dmg 80 -> shield 0, hull 70 (-30).
    {
        ShipDamageModel m = ShipDamage::makeFighter(50, 100);
        ShipDamage::applyDamage(m, 80);
        check(m.shield == 0 && m.hull == 70,
              "T3 overflow exact (shield 50, dmg 80 -> shield 0, hull 70)");
    }

    // T4 — isDestroyed flips at hull <= 0; dead ship ignores further damage.
    {
        ShipDamageModel m = ShipDamage::makeFighter(0, 40);
        check(!ShipDamage::isDestroyed(m), "T4 alive before lethal hit");
        ShipDamage::applyDamage(m, 100);     // 100 > 40 hull
        check(m.hull == 0 && ShipDamage::isDestroyed(m),
              "T4b lethal hit floors hull at 0 -> destroyed");
        ShipDamage::applyDamage(m, 50);      // no-op on a dead ship
        check(m.hull == 0, "T4c destroyed ship ignores further damage");
    }

    // T5 — tick regens shield ONLY after the delay (not before).
    {
        ShipDamageModel m = ShipDamage::makeFighter(50, 100);
        ShipDamage::applyDamage(m, 30);      // shield 20, resets timeSinceHit -> 0
        // Tick within the delay window: NO regen yet (delay is 4 s).
        ShipDamage::tick(m, 1.0f);
        ShipDamage::tick(m, 1.0f);
        ShipDamage::tick(m, 1.0f);           // total 3 s < 4 s delay
        check(m.shield == 20, "T5 no shield regen before the delay elapses");
        // Cross the delay, then a full second of regen (25/s -> +25, cap 50).
        ShipDamage::tick(m, 1.5f);           // total 4.5 s: delay crossed, +ceil(25*1.5)=+38 -> cap 50
        check(m.shield == 50, "T5b shield regens after the delay, clamped to max");
    }

    // T6 — makeCapital enables subsystems, all seeded full.
    {
        ShipDamageModel m = ShipDamage::makeCapital(200, 800, 100);
        check(m.hasSubsystems && m.shield == 200 && m.hull == 800,
              "T6 makeCapital seeds pools + enables subsystems");
        bool allFull = true;
        for (int i = 0; i < (int)Subsystem::Count; ++i)
            if (m.subHp[i] != 100 || m.subMaxHp[i] != 100) allFull = false;
        check(allFull, "T6b capital subsystems seeded to subHp");
        check(!ShipDamage::subsystemDown(m, Subsystem::Engines),
              "T6c fresh subsystem is not down");
    }

    // T7 — targeted subsystem damage routes ONLY when the shield is down.
    {
        ShipDamageModel m = ShipDamage::makeCapital(50, 800, 100);
        // Shield still up: targeting Engines should NOT touch the subsystem;
        // shield soaks it (40 <= 50).
        ShipDamage::applyDamage(m, 40, Subsystem::Engines);
        check(m.shield == 10 && m.subHp[(int)Subsystem::Engines] == 100,
              "T7 targeted hit absorbed by shield, subsystem untouched");
        // Now drop the shield with an untargeted hit (10 shield + 30 hull bleed).
        ShipDamage::applyDamage(m, 40);
        check(m.shield == 0 && m.hull == 770,
              "T7b untargeted overflow drops shield, bleeds hull");
        // Shield down: a targeted hit now routes overflow into Engines, NOT hull.
        const int hullBefore = m.hull;
        ShipDamage::applyDamage(m, 30, Subsystem::Engines);
        check(m.subHp[(int)Subsystem::Engines] == 70 && m.hull == hullBefore,
              "T7c shield-down targeted hit routes to subsystem, hull spared");
    }

    // T8 — subsystemDown flips at 0; fighters have no subsystems.
    {
        ShipDamageModel m = ShipDamage::makeCapital(0, 800, 50);
        ShipDamage::applyDamage(m, 50, Subsystem::Turrets); // shield 0 -> all into Turrets
        check(m.subHp[(int)Subsystem::Turrets] == 0 &&
              ShipDamage::subsystemDown(m, Subsystem::Turrets),
              "T8 subsystem reaching 0 reports down");
        check(!ShipDamage::subsystemDown(m, Subsystem::Engines),
              "T8b untouched subsystem still up");
        ShipDamageModel f = ShipDamage::makeFighter(10, 50);
        check(!ShipDamage::subsystemDown(f, Subsystem::Engines),
              "T8c fighter (no subsystems) never reports down");
    }

    // T9 — shieldFrac / hullFrac correct mid-fight.
    {
        ShipDamageModel m = ShipDamage::makeFighter(100, 200);
        ShipDamage::applyDamage(m, 25);   // shield 75
        ShipDamage::applyDamage(m, 175);  // shield 0, hull 200-100=100
        check(std::fabs(ShipDamage::shieldFrac(m) - 0.0f) < 1e-5f &&
              std::fabs(ShipDamage::hullFrac(m)   - 0.5f) < 1e-5f,
              "T9 shieldFrac=0, hullFrac=0.5 after scripted hits");
    }

    // T10 — capital targeted-but-shield-up overflow still hits HULL, not sub.
    {
        ShipDamageModel m = ShipDamage::makeCapital(20, 500, 100);
        // 60 dmg targeting ShieldGen: shield (20) absorbs, 40 overflow. Shield
        // was NOT already 0 when the hit landed, so overflow goes to HULL.
        ShipDamage::applyDamage(m, 60, Subsystem::ShieldGen);
        check(m.shield == 0 && m.hull == 460 &&
              m.subHp[(int)Subsystem::ShieldGen] == 100,
              "T10 targeted hit that itself breaks the shield bleeds to hull, not subsystem");
    }

    // ======================================================================
    // T11..T18 — CAPITAL DEATH SEQUENCE. The gate the owner playtest asked
    // for: prove the sequence FIRES when hull reaches 0 and that it is a
    // sequence (staged over seconds), not one frame of fireballs.
    // ======================================================================
    auto pumpToEnd = [](CapitalDeathState& s, std::vector<DeathEvent>& all,
                        int& maxBurst, float dtStep = 1.0f / 60.0f) {
        DeathEvent buf[CapitalDeathSequence::kMaxEventsPerStep];
        maxBurst = 0;
        // 6 s of 60 Hz covers the 5 s schedule with margin.
        for (int i = 0; i < 360 && !CapitalDeathSequence::isFinished(s); ++i) {
            const int n = CapitalDeathSequence::step(s, dtStep, buf,
                              CapitalDeathSequence::kMaxEventsPerStep);
            if (n > maxBurst) maxBurst = n;
            for (int k = 0; k < n; ++k) all.push_back(buf[k]);
        }
    };

    // T11 — the sequence arms off a REAL model whose hull reached 0. This is
    //       the wiring contract: isDestroyed() is the trigger, and an ARMED
    //       sequence reports active.
    {
        ShipDamageModel m = ShipDamage::makeCapital(400, 1500, 120);
        CapitalDeathState d{};
        check(!CapitalDeathSequence::isActive(d),
              "T11 death sequence is idle before the kill");
        ShipDamage::applyDamage(m, 100000);          // hull -> 0
        check(ShipDamage::isDestroyed(m), "T11b lethal damage destroys the capital");
        if (ShipDamage::isDestroyed(m)) {
            const float c[3] = { 2600.0f, 0.0f, 0.0f };
            const float ax[3] = { 1.0f, 0.0f, 0.0f };
            CapitalDeathSequence::begin(d, c, ax, 210.0f);
        }
        check(CapitalDeathSequence::isActive(d) && !CapitalDeathSequence::isFinished(d),
              "T11c hull 0 arms the capital death sequence");
    }

    // T12 — it is a SEQUENCE: the first event is the rupture, the last is the
    //       core detonation, and they are separated by seconds of cascade.
    {
        CapitalDeathState d{};
        const float c[3] = { 0.0f, 0.0f, 0.0f };
        const float ax[3] = { 1.0f, 0.0f, 0.0f };
        CapitalDeathSequence::begin(d, c, ax, 210.0f);
        std::vector<DeathEvent> all; int burst = 0;
        pumpToEnd(d, all, burst);
        check((int)all.size() == CapitalDeathSequence::scheduleSize() &&
              all.size() > 20,
              "T12 the whole schedule is emitted exactly once (>20 staged beats)");
        check(!all.empty() && all.front().kind == DeathFx::Rupture,
              "T12b the sequence OPENS on the hull rupture");
        bool sawCore = false, sawFrag = false, sawVent = false;
        int cascades = 0;
        for (const auto& e : all) {
            if (e.kind == DeathFx::CoreDetonation) sawCore = true;
            if (e.kind == DeathFx::HullFragment)   sawFrag = true;
            if (e.kind == DeathFx::Vent)           sawVent = true;
            if (e.kind == DeathFx::Cascade)        ++cascades;
        }
        check(sawCore && sawFrag && sawVent && cascades >= 10,
              "T12c cascade + venting + hull fragments + a core detonation all fire");
        check(CapitalDeathSequence::isFinished(d),
              "T12d the sequence finishes (the host can close the death window)");
    }

    // T13 — the CASCADE WALKS THE HULL: successive cascade beats advance
    //       monotonically along the ship's long axis (bow -> stern). This is
    //       what makes the break-up read as structural rather than as a pile
    //       of fireballs at one point.
    {
        CapitalDeathState d{};
        const float c[3] = { 100.0f, 50.0f, -20.0f };
        const float ax[3] = { 0.0f, 0.0f, 1.0f };   // hull along +Z this time
        CapitalDeathSequence::begin(d, c, ax, 200.0f);
        std::vector<DeathEvent> all; int burst = 0;
        pumpToEnd(d, all, burst);
        // The WALK is the pre-core phase: bow -> stern, one secondary at a
        // time. After the core lets go the wreck keeps cooking off wherever,
        // so only the beats before the CoreDetonation are required monotone.
        bool walks = true; float prev = -1e9f; int seen = 0; bool coreGone = false;
        for (const auto& e : all) {
            if (e.kind == DeathFx::CoreDetonation) { coreGone = true; continue; }
            if (e.kind != DeathFx::Cascade) continue;
            const float along = e.pos[2] - c[2];       // projection onto +Z
            // Everything must stay ON the ship, not out in open space.
            if (std::fabs(along) > 200.0f + 40.0f) walks = false;
            if (coreGone) continue;
            if (seen > 0 && along < prev - 1.0f) walks = false;
            prev = along; ++seen;
        }
        check(walks && seen >= 10,
              "T13 the secondary cascade walks bow->stern along the hull axis");
        // The rupture and the core go off near amidships, not out in the void.
        for (const auto& e : all) {
            if (e.kind != DeathFx::CoreDetonation) continue;
            check(std::fabs(e.pos[2] - c[2]) < 40.0f,
                  "T13b the core detonation is amidships");
        }
    }

    // T14 — THE LIGHTS GO OUT. A dead capital must stop glowing; a hull that
    //       stays lit reads as alive no matter how much fire is in front of it.
    {
        CapitalDeathState d{};
        const float c[3] = { 0, 0, 0 }, ax[3] = { 1, 0, 0 };
        CapitalDeathSequence::begin(d, c, ax, 210.0f);
        check(std::fabs(d.lights - 1.0f) < 1e-5f, "T14 hull is fully lit at the kill");
        std::vector<DeathEvent> all; int burst = 0;
        pumpToEnd(d, all, burst);
        check(std::fabs(d.lights - CapitalDeathSequence::kLightsOutFloor) < 1e-4f,
              "T14b the hull emissive falls to the lights-out floor");
        check(CapitalDeathSequence::kLightsOutFloor > 0.0f,
              "T14c the floor is above zero (a wreck, not a hole in the starfield)");
        check(d.tumble > 1.0f && d.sag > 10.0f,
              "T14d the dead hull rolls over and goes down");
    }

    // T15 — PER-FRAME BUDGET. A hitched frame (a whole second of dt at once)
    //       must NOT dump the entire cascade into one frame's particle pool.
    {
        CapitalDeathState d{};
        const float c[3] = { 0, 0, 0 }, ax[3] = { 1, 0, 0 };
        CapitalDeathSequence::begin(d, c, ax, 210.0f);
        DeathEvent buf[16];
        const int n = CapitalDeathSequence::step(d, 3.0f, buf, 16);
        check(n <= CapitalDeathSequence::kMaxEventsPerStep,
              "T15 a 3 s frame hitch still emits at most kMaxEventsPerStep events");
        // And nothing is LOST — the backlog drains over the following frames.
        std::vector<DeathEvent> all; int burst = 0;
        pumpToEnd(d, all, burst);
        check((int)all.size() + n == CapitalDeathSequence::scheduleSize(),
              "T15b the backlog drains — no scheduled beat is dropped");
        check(burst <= CapitalDeathSequence::kMaxEventsPerStep,
              "T15c the budget holds for every step of the drain");
    }

    // T16 — DETERMINISM. Two identical runs produce byte-identical event
    //       streams (the intro must stay reproducible for capture evidence).
    {
        const float c[3] = { 2600.0f, 0.0f, 0.0f }, ax[3] = { 1, 0, 0 };
        CapitalDeathState a{}, b{};
        CapitalDeathSequence::begin(a, c, ax, 210.0f);
        CapitalDeathSequence::begin(b, c, ax, 210.0f);
        std::vector<DeathEvent> ea, eb; int ba = 0, bb = 0;
        pumpToEnd(a, ea, ba); pumpToEnd(b, eb, bb);
        bool same = ea.size() == eb.size();
        for (size_t i = 0; same && i < ea.size(); ++i)
            same = ea[i].kind == eb[i].kind &&
                   std::fabs(ea[i].pos[0] - eb[i].pos[0]) < 1e-6f &&
                   std::fabs(ea[i].pos[1] - eb[i].pos[1]) < 1e-6f &&
                   std::fabs(ea[i].pos[2] - eb[i].pos[2]) < 1e-6f &&
                   std::fabs(ea[i].radius - eb[i].radius) < 1e-6f;
        check(same, "T16 the death sequence is bit-deterministic across runs");
    }

    // T17 — A DESTROYED SUBSYSTEM CHANGES STATE, and the change is one-way
    //       and per-system (the gate's second requirement). Killing the
    //       engines must not silence the turrets, and a dead system stays dead
    //       through shield regen and further hull damage.
    {
        ShipDamageModel m = ShipDamage::makeCapital(400, 1500, 120);
        ShipDamage::applyDamage(m, 400);                       // strip the shield
        check(m.shield == 0 && !ShipDamage::subsystemDown(m, Subsystem::Engines),
              "T17 shield stripped, every subsystem still UP");
        ShipDamage::applyDamage(m, 90, Subsystem::Engines);    // 120 -> 30
        check(!ShipDamage::subsystemDown(m, Subsystem::Engines),
              "T17b a partial hit does not down the subsystem");
        ShipDamage::applyDamage(m, 90, Subsystem::Engines);    // 30 -> 0
        check(ShipDamage::subsystemDown(m, Subsystem::Engines),
              "T17c the second hit DESTROYS the engines (state flips to down)");
        check(!ShipDamage::subsystemDown(m, Subsystem::Turrets) &&
              !ShipDamage::subsystemDown(m, Subsystem::ShieldGen) &&
              !ShipDamage::subsystemDown(m, Subsystem::Sensors),
              "T17d killing one subsystem leaves the other three untouched");
        // Dead stays dead: 10 s of regen ticks + more hull damage.
        for (int i = 0; i < 600; ++i) ShipDamage::tick(m, 1.0f / 60.0f);
        ShipDamage::applyDamage(m, 200);
        check(ShipDamage::subsystemDown(m, Subsystem::Engines),
              "T17e a destroyed subsystem never comes back (no auto-repair)");
    }

    // T18 — END TO END: the real capital tuning (400 shield / 1500 hull /
    //       4x120 subsystems) taken apart at the shipped 90 dmg per landed
    //       hit, hardpoints first, then the hull. Proves the fight is FINITE
    //       and pins the landed-hit cost of a kill so a retune can't silently
    //       make the dreadnought unkillable again.
    {
        ShipDamageModel m = ShipDamage::makeCapital(400, 1500, 120);
        int hits = 0, subsDown = 0;
        CapitalDeathState d{};
        while (!ShipDamage::isDestroyed(m) && hits < 500) {
            Subsystem tgt = Subsystem::Count;
            for (int i = 0; i < (int)Subsystem::Count; ++i)
                if (!ShipDamage::subsystemDown(m, (Subsystem)i)) { tgt = (Subsystem)i; break; }
            // Reactor phase: once every hardpoint is down, hull damage triples.
            const bool crippled = (tgt == Subsystem::Count);
            ShipDamage::applyDamage(m, crippled ? 90 * 3 : 90, tgt);
            ++hits;
        }
        for (int i = 0; i < (int)Subsystem::Count; ++i)
            if (ShipDamage::subsystemDown(m, (Subsystem)i)) ++subsDown;
        check(ShipDamage::isDestroyed(m) && subsDown == (int)Subsystem::Count,
              "T18 the shipped capital tuning IS killable (all 4 hardpoints + hull 0)");
        check(hits >= 15 && hits <= 30,
              "T18b a clean kill costs 15-30 landed hits (a fight, not a wall)");
        x3::logInfo("  [info] capital kill cost: " + std::to_string(hits) +
                    " landed hits at 90 dmg (x3 in the reactor phase)");
        if (ShipDamage::isDestroyed(m)) {
            const float c[3] = { 2600.0f, 0.0f, 0.0f }, ax[3] = { 1, 0, 0 };
            CapitalDeathSequence::begin(d, c, ax, 210.0f);
        }
        check(CapitalDeathSequence::isActive(d),
              "T18c the death sequence fires at the end of a real kill run");
    }

    // ======================================================================
    // T19..T21 — CAPITAL MOTION (owner: "The overlords ship should move
    // around"). Bounds, dt-stability, phase-driven speed, adrift freeze.
    // ======================================================================
    // T19 — she MOVES, stays ON the arc (the arena bound), mass-appropriate.
    {
        CapitalMotionState mo{};
        const float start[3] = { 2600.0f, 0.0f, 0.0f };
        CapitalMotion::begin(mo, start);
        check(std::fabs(mo.pos[0] - 2600.0f) < 1e-3f &&
              std::fabs(mo.pos[2]) < 1e-3f &&
              std::fabs(mo.fwd[0] + 1.0f) < 1e-4f,
              "T19 motion begins at the reveal pose (2600,0,0 heading -X)");
        float travelled = 0.0f, prev[3] = { mo.pos[0], mo.pos[1], mo.pos[2] };
        bool onArc = true, speedOk = true;
        for (int i = 0; i < 60 * 600; ++i) {          // ten full minutes
            CapitalMotion::update(mo, 1.0f / 60.0f, false, false);
            const float dx = mo.pos[0] - mo.anchor[0];
            const float dz = mo.pos[2] - mo.anchor[2];
            if (std::fabs(std::sqrt(dx*dx + dz*dz) - mo.radius) > 0.01f) onArc = false;
            const float sx = mo.pos[0]-prev[0], sz = mo.pos[2]-prev[2];
            travelled += std::sqrt(sx*sx + sz*sz);
            prev[0]=mo.pos[0]; prev[1]=mo.pos[1]; prev[2]=mo.pos[2];
            if (mo.speed > CapitalMotion::kCombatSpeed + 0.01f) speedOk = false;
        }
        check(onArc, "T19b ten minutes of patrol never leaves the arc (bounded)");
        check(travelled > 6000.0f && speedOk,
              "T19c she actually travels (>6 km in 10 min) at capped speed");
        const float expect = CapitalMotion::kPatrolSpeed * 600.0f;
        check(std::fabs(travelled - expect) < expect * 0.02f,
              "T19d patrol distance matches kPatrolSpeed * t (dt-correct)");
        check(std::fabs(mo.fwd[0]*mo.fwd[0] + mo.fwd[1]*mo.fwd[1] +
                        mo.fwd[2]*mo.fwd[2] - 1.0f) < 1e-3f,
              "T19e heading stays unit (the draw basis never degenerates)");
    }
    // T20 — dt-STABLE: 60 Hz and 240 Hz land within metres after a minute
    //       (165 Hz rule: motion scales by dt, never per-frame).
    {
        CapitalMotionState a{}, b{};
        const float start[3] = { 2600.0f, 0.0f, 0.0f };
        CapitalMotion::begin(a, start);
        CapitalMotion::begin(b, start);
        for (int i = 0; i < 60 * 60;  ++i) CapitalMotion::update(a, 1.0f/60.0f,  true, false);
        for (int i = 0; i < 240 * 60; ++i) CapitalMotion::update(b, 1.0f/240.0f, true, false);
        const float dx = a.pos[0]-b.pos[0], dz = a.pos[2]-b.pos[2];
        check(std::sqrt(dx*dx + dz*dz) < 2.0f,
              "T20 60 Hz vs 240 Hz motion converges (<2 m after 60 s)");
    }
    // T21 — PHASE-DRIVEN: shields-down raises speed; adrift eases it to a
    //       freeze WITHOUT snapping the heading.
    {
        CapitalMotionState mo{};
        const float start[3] = { 0.0f, 0.0f, 0.0f };
        CapitalMotion::begin(mo, start);
        for (int i = 0; i < 60 * 30; ++i) CapitalMotion::update(mo, 1.0f/60.0f, true, false);
        check(mo.phase == CapitalMovePhase::Combat &&
              mo.speed > CapitalMotion::kPatrolSpeed + 4.0f,
              "T21 shields down -> combat phase, speed rises toward kCombatSpeed");
        const float fBefore[3] = { mo.fwd[0], mo.fwd[1], mo.fwd[2] };
        float moved = 0.0f;
        for (int i = 0; i < 60 * 40; ++i) {
            const float p0[3] = { mo.pos[0], mo.pos[1], mo.pos[2] };
            CapitalMotion::update(mo, 1.0f/60.0f, true, true);   // engines dead
            if (i >= 60 * 35) {   // the last 5 s: she should be frozen
                const float mx = mo.pos[0]-p0[0], mz = mo.pos[2]-p0[2];
                moved += std::sqrt(mx*mx + mz*mz);
            }
        }
        check(mo.phase == CapitalMovePhase::Adrift && mo.speed < 0.05f &&
              moved < 0.5f,
              "T21b engines dead -> speed eases to a dead stop (adrift freeze)");
        const float dot = mo.fwd[0]*fBefore[0] + mo.fwd[1]*fBefore[1] +
                          mo.fwd[2]*fBefore[2];
        check(dot > 0.90f,
              "T21c the freeze keeps her aspect (no heading snap while easing out)");
    }

    // ======================================================================
    // T22 — CAPITAL GUN BATTERY (owner: "big mounted weapons that HURT.. but
    //       I can shoot off"). Fire ceases from DESTROYED mounts only.
    // ======================================================================
    {
        CapitalBatteryState bt{};
        CapitalBattery::init(bt);
        check(CapitalBattery::aliveGuns(bt) == kCapitalGunCount,
              "T22 battery seeds all gun mounts alive");
        // 12 s in range at 60 Hz: every gun spools + fires, staggered.
        int firesPerGun[kCapitalGunCount] = {};
        uint32_t spooled = 0u; bool spoolBeforeFire = true;
        bool spoolSeen[kCapitalGunCount] = {};
        for (int i = 0; i < 60 * 12; ++i) {
            uint32_t sp = 0u;
            const uint32_t f = CapitalBattery::update(bt, 1.0f/60.0f, true, &sp);
            spooled |= sp;
            for (int g = 0; g < kCapitalGunCount; ++g) {
                if (sp & (1u << g)) spoolSeen[g] = true;
                if (f & (1u << g)) {
                    if (!spoolSeen[g]) spoolBeforeFire = false;   // telegraph law
                    ++firesPerGun[g];
                }
            }
        }
        bool allFired = true;
        for (int g = 0; g < kCapitalGunCount; ++g)
            if (firesPerGun[g] < 2) allFired = false;
        check(allFired && spooled == 0xFu,
              "T22b every live mount telegraphs + fires on its own cadence");
        check(spoolBeforeFire, "T22c no bolt without its spool telegraph first");
        // Shoot off gun 1: it goes silent FOREVER; the others keep firing.
        check(CapitalBattery::damageGun(bt, 1, 90),
              "T22d a landed hit shears the mount (damageGun reports the kill)");
        check(!CapitalBattery::gunAlive(bt, 1) &&
              CapitalBattery::aliveGuns(bt) == kCapitalGunCount - 1,
              "T22e the shot-off mount reads dead, the rest read alive");
        int fires2[kCapitalGunCount] = {};
        for (int i = 0; i < 60 * 12; ++i) {
            const uint32_t f = CapitalBattery::update(bt, 1.0f/60.0f, true, nullptr);
            for (int g = 0; g < kCapitalGunCount; ++g)
                if (f & (1u << g)) ++fires2[g];
        }
        check(fires2[1] == 0 && fires2[0] > 0 && fires2[2] > 0 && fires2[3] > 0,
              "T22f fire ceases from the DESTROYED mount only");
        // Out of range: nothing fires, spools cancel.
        CapitalBatteryState bt2{}; CapitalBattery::init(bt2);
        uint32_t any = 0u;
        for (int i = 0; i < 60 * 12; ++i)
            any |= CapitalBattery::update(bt2, 1.0f/60.0f, false, nullptr);
        check(any == 0u, "T22g out of range the whole battery stands down");
        // Dead gun soaks no further kills; battery kGunHp * count == the
        // Turrets subsystem pool (the physical<->model bridge stays exact).
        check(!CapitalBattery::damageGun(bt, 1, 90),
              "T22h a dead mount cannot be killed twice");
        check(CapitalBattery::kSubQuarter * kCapitalGunCount == 120,
              "T22i four gun kills == the full 120 HP Turrets subsystem");
    }

    // ======================================================================
    // T23/T24 — THE TWO AUTHORED ENDINGS (owner: "either crash land on a
    //           planet OR break up in space").
    // ======================================================================
    auto pumpLong = [](CapitalDeathState& s, std::vector<DeathEvent>& all) {
        DeathEvent buf[CapitalDeathSequence::kMaxEventsPerStep];
        for (int i = 0; i < 60 * 12 && !CapitalDeathSequence::isFinished(s); ++i) {
            const int n = CapitalDeathSequence::step(s, 1.0f/60.0f, buf,
                              CapitalDeathSequence::kMaxEventsPerStep);
            for (int k = 0; k < n; ++k) all.push_back(buf[k]);
        }
    };
    // T23 — DEORBIT CRASH: the wreck FALLS toward the planet under an entry
    //       burn and terminates in the impact flash.
    {
        CapitalDeathState d{};
        const float c[3] = { 2600.0f, 0.0f, 0.0f }, ax[3] = { 1, 0, 0 };
        const float pd[3] = { -0.47f, 0.13f, -0.87f };   // hero-world sky ray
        CapitalDeathSequence::begin(d, c, ax, 210.0f,
                                    DeathOutcome::DeorbitCrash, pd);
        check(d.outcome == DeathOutcome::DeorbitCrash,
              "T23 deorbit arm keeps the crash outcome (planet dir valid)");
        float plungePrev = -1.0f; bool plungeMono = true;
        std::vector<DeathEvent> all;
        DeathEvent buf[CapitalDeathSequence::kMaxEventsPerStep];
        for (int i = 0; i < 60 * 12 && !CapitalDeathSequence::isFinished(d); ++i) {
            const int n = CapitalDeathSequence::step(d, 1.0f/60.0f, buf,
                              CapitalDeathSequence::kMaxEventsPerStep);
            for (int k = 0; k < n; ++k) all.push_back(buf[k]);
            if (d.plunge < plungePrev - 1e-4f) plungeMono = false;
            plungePrev = d.plunge;
        }
        check(CapitalDeathSequence::isFinished(d) && plungeMono &&
              d.plunge > 500.0f,
              "T23b the wreck plunges monotonically >500 m toward the planet");
        check(std::fabs(d.burn - 1.0f) < 1e-4f,
              "T23c the atmosphere-entry burn ramps to full");
        check(!all.empty() && all.front().kind == DeathFx::Rupture,
              "T23d the deorbit still OPENS on the kill rupture");
        // The terminal flash happens far DOWN the fall, not where she died.
        bool coreFar = false;
        for (const auto& e : all) {
            if (e.kind != DeathFx::CoreDetonation) continue;
            const float dx = e.pos[0]-c[0], dy = e.pos[1]-c[1], dz = e.pos[2]-c[2];
            if (std::sqrt(dx*dx + dy*dy + dz*dz) > 400.0f) coreFar = true;
        }
        check(coreFar, "T23e the impact flash fires >400 m down the fall line");
        // Determinism across two runs (same contract as T16).
        CapitalDeathState d2{};
        CapitalDeathSequence::begin(d2, c, ax, 210.0f,
                                    DeathOutcome::DeorbitCrash, pd);
        std::vector<DeathEvent> all2; pumpLong(d2, all2);
        bool same = all.size() == all2.size();
        for (size_t i = 0; same && i < all.size(); ++i)
            same = all[i].kind == all2[i].kind &&
                   std::fabs(all[i].pos[0]-all2[i].pos[0]) < 1e-5f &&
                   std::fabs(all[i].pos[1]-all2[i].pos[1]) < 1e-5f &&
                   std::fabs(all[i].pos[2]-all2[i].pos[2]) < 1e-5f;
        check(same, "T23f the deorbit is bit-deterministic across runs");
    }
    // T24 — BREAKUP stays the breakup: no plunge, no burn — and a degenerate
    //       planet dir DEMOTES a requested crash (nowhere to crash).
    {
        CapitalDeathState d{};
        const float c[3] = { 0, 0, 0 }, ax[3] = { 1, 0, 0 };
        CapitalDeathSequence::begin(d, c, ax, 210.0f);
        std::vector<DeathEvent> all; pumpLong(d, all);
        check(d.plunge == 0.0f && d.burn == 0.0f &&
              (int)all.size() == CapitalDeathSequence::scheduleSize(),
              "T24 the in-place breakup carries no plunge/burn (unchanged)");
        CapitalDeathState d2{};
        const float zero[3] = { 0, 0, 0 };
        CapitalDeathSequence::begin(d2, c, ax, 210.0f,
                                    DeathOutcome::DeorbitCrash, zero);
        check(d2.outcome == DeathOutcome::BreakupInSpace,
              "T24b a crash with no planet demotes to the space breakup");
    }

    // =======================================================================
    // ITEM F — HOVER HIGHLIGHT + "MAKE IT HARD" (owner, 2026-08-18)
    // =======================================================================
    // The scale the shipped encounter runs at (kCapScaleUp 4x): a 960 m hull
    // sphere standing off at 2600 m with 168 m hardpoint blisters. Building
    // the fixtures at the REAL scale is the point — a highlight test that
    // passes at toy dimensions proves nothing about the ship in the game.
    const float kHullR = 960.0f;
    const float kHardR = 168.0f;
    const float kCapCtr[3] = { 2600.0f, 0.0f, 0.0f };

    // T25 — the hover pick hits only LIVE mounts, and a destroyed one is
    //       never picked again (it stops highlighting, permanently).
    {
        // A blister hung on the NEAR face of the hull, dead ahead of a player
        // sitting at the origin: the ray reaches the blister before the hull.
        const float nearHp[3] = { 2600.0f - kHullR - 40.0f, 0.0f, 0.0f };
        CapitalAimTarget tg[2] = {
            { { kCapCtr[0], kCapCtr[1], kCapCtr[2] }, kHullR,
              CapitalAimKind::Hull, -1, true },
            { { nearHp[0], nearHp[1], nearHp[2] }, kHardR,
              CapitalAimKind::Hardpoint, 2, true },
        };
        const float eye[3] = { 0.0f, 0.0f, 0.0f };
        const float fwd[3] = { 1.0f, 0.0f, 0.0f };
        CapitalAimResult r = CapitalAim::pick(tg, 2, eye, fwd);
        check(r.kind == CapitalAimKind::Hardpoint && r.index == 2,
              "T25 the hover pick lands on a LIVE hardpoint, not the hull behind it");
        // KILL IT. The exact same ray must now fall through to the hull —
        // a destroyed mount is wreckage: it never highlights again.
        tg[1].alive = false;
        r = CapitalAim::pick(tg, 2, eye, fwd);
        check(r.kind == CapitalAimKind::Hull,
              "T25b a DESTROYED mount stops highlighting (the ray falls to the hull)");
        // And with nothing at all to hit, the pick is an honest whiff — the
        // highlight must not invent a target off to the side.
        const float away[3] = { -1.0f, 0.0f, 0.0f };
        r = CapitalAim::pick(tg, 2, eye, away);
        check(r.kind == CapitalAimKind::None && r.index == -1,
              "T25c aiming away picks NOTHING (no phantom highlight)");
    }

    // T25d — the hull OCCLUDES far-side parts: you cannot hover (or snipe)
    //        the engines through 1.8 km of ship, you fly around. This is the
    //        property that makes the highlight honest at capital scale.
    {
        const float farHp[3] = { 2600.0f + kHullR + 40.0f, 0.0f, 0.0f };  // beyond her
        CapitalAimTarget tg[2] = {
            { { kCapCtr[0], kCapCtr[1], kCapCtr[2] }, kHullR,
              CapitalAimKind::Hull, -1, true },
            { { farHp[0], farHp[1], farHp[2] }, kHardR,
              CapitalAimKind::Hardpoint, 0, true },
        };
        const float eye[3] = { 0.0f, 0.0f, 0.0f };
        const float fwd[3] = { 1.0f, 0.0f, 0.0f };
        const CapitalAimResult r = CapitalAim::pick(tg, 2, eye, fwd);
        check(r.kind == CapitalAimKind::Hull,
              "T25d the hull occludes a far-side hardpoint (fly around, don't snipe through)");
        // Come at the SAME hardpoint from the far side and it is pickable —
        // proving the occlusion is geometric, not a blanket ban.
        const float eye2[3] = { 5600.0f, 0.0f, 0.0f };
        const float back[3] = { -1.0f, 0.0f, 0.0f };
        const CapitalAimResult r2 = CapitalAim::pick(tg, 2, eye2, back);
        check(r2.kind == CapitalAimKind::Hardpoint && r2.index == 0,
              "T25e the same hardpoint IS pickable once you fly around to its side");
    }

    // T25p — THE REGRESSION GUARD. Real hardpoints are bolted to the OUTSIDE of
    //   a long thin ship, but the hull's bounding SPHERE encloses all of them
    //   (the shipped ventral gun mounts sit 617 m out inside a 960 m hull
    //   sphere). Under a naive nearest-entry sweep the hull wins every ray and
    //   nothing on the ship is hoverable or hittable at all. A part on the NEAR
    //   hemisphere must beat the hull even when it is entirely inside its
    //   bounding sphere — otherwise item F has nothing to highlight.
    {
        const float gun[3] = { 2600.0f - 592.0f, -136.0f, 104.0f };   // deep inside
        CapitalAimTarget tg[2] = {
            { { kCapCtr[0], kCapCtr[1], kCapCtr[2] }, kHullR,
              CapitalAimKind::Hull, -1, true },
            { { gun[0], gun[1], gun[2] }, 88.0f, CapitalAimKind::Gun, 1, true },
        };
        float eye[3] = { 0.0f, 0.0f, 0.0f };
        float fwd[3] = { gun[0]-eye[0], gun[1]-eye[1], gun[2]-eye[2] };
        const float l = std::sqrt(fwd[0]*fwd[0]+fwd[1]*fwd[1]+fwd[2]*fwd[2]);
        fwd[0]/=l; fwd[1]/=l; fwd[2]/=l;
        const CapitalAimResult r = CapitalAim::pick(tg, 2, eye, fwd);
        check(r.kind == CapitalAimKind::Gun && r.index == 1,
              "T25p a near-side mount INSIDE the hull bounding sphere is still pickable");
        // Order must not matter: the hull is an occluder, not a list rival.
        CapitalAimTarget rev[2] = { tg[1], tg[0] };
        const CapitalAimResult r2 = CapitalAim::pick(rev, 2, eye, fwd);
        check(r2.kind == CapitalAimKind::Gun && r2.index == 1,
              "T25q the pick is order-independent (hull occludes, never competes)");
    }

    // T25f — PER-HARDPOINT HEALTH: each mount takes sustained accurate fire.
    //        At the shipped 420 HP and 90 dmg/hit that is 5 landed hits per
    //        mount, and hits on one mount never bleed into another.
    {
        ShipDamageModel m = ShipDamage::makeCapital(2400, 12000, 420);
        m.shield = 0;                       // shield already down (routing gate)
        int hits = 0;
        while (!ShipDamage::subsystemDown(m, Subsystem::Sensors) && hits < 50) {
            ShipDamage::applyDamage(m, 90, Subsystem::Sensors);
            ++hits;
        }
        check(hits == 5,
              "T25f a 420 HP hardpoint takes 5 landed 90-dmg hits — sustained fire, not a tap");
        check(!ShipDamage::subsystemDown(m, Subsystem::Engines) &&
              !ShipDamage::subsystemDown(m, Subsystem::Turrets) &&
              !ShipDamage::subsystemDown(m, Subsystem::ShieldGen),
              "T25g killing one mount leaves the other three at full health");
        check(m.hull == m.maxHull,
              "T25h subsystem fire never leaks into the hull pool");
    }

    // T25i — THE SHIELD GATE + REGEN SUPPRESSION. Hull damage barely counts
    //        through a live shield; the bubble recovers in a lull; and the
    //        generator kill ends the recovery for good.
    {
        ShipDamageModel m = ShipDamage::makeCapital(2400, 12000, 420);
        m.hullBleedWhileShielded = 0.08f;
        m.shieldRegenPerSec   = 70.0f;
        m.shieldRegenDelaySec = 6.0f;
        // ONE enormous hit into a FULL bubble: the shield eats 2400 and the
        // 2600 of overflow reaches the hull at 8% — 208, not 2600. That factor
        // is the rule "the shield must drop before hull damage counts", and it
        // is what makes hosing the hull the slow path rather than the fast one.
        ShipDamage::applyDamage(m, 5000);
        check(m.shield == 0 && m.maxHull - m.hull == 208,
              "T25i overflow through a LIVE shield lands at 8% (2600 -> 208)");
        // Once the bubble is DOWN the gate is gone: full damage, unscaled, so
        // dropping the shield is a real and visible change of state.
        const int before = m.hull;
        ShipDamage::applyDamage(m, 90);
        check(before - m.hull == 90,
              "T25j with the shield down the SAME hit lands in full (the gate opens)");
        // 26 hits is exactly what a 2400 bubble costs at 90 dmg — the shield is
        // a real gate (~18 s of sustained accurate fire), not a speed bump.
        ShipDamageModel g = ShipDamage::makeCapital(2400, 12000, 420);
        g.hullBleedWhileShielded = 0.08f;
        int shots = 0;
        while (g.shield > 0 && shots < 200) { ShipDamage::applyDamage(g, 90); ++shots; }
        check(shots == 27 && g.maxHull - g.hull < 40,
              "T25j2 dropping the bubble costs 27 landed hits and barely scratches the hull");
        // A LULL: the shield comes back, so breaking off hands her the bubble.
        ShipDamageModel r = ShipDamage::makeCapital(2400, 12000, 420);
        r.shield = 0; r.timeSinceHit = 0.0f;
        r.shieldRegenPerSec = 70.0f; r.shieldRegenDelaySec = 6.0f;
        for (int i = 0; i < 60 * 12; ++i) ShipDamage::tick(r, 1.0f/60.0f);
        check(r.shield > 300,
              "T25k the shield REGENERATES through a 12 s lull (breaking off is punished)");
        // SUPPRESSED: with the generator dead the same lull returns nothing.
        ShipDamageModel s = r;
        s.shield = 0; s.timeSinceHit = 0.0f; s.shieldRegenDisabled = true;
        for (int i = 0; i < 60 * 12; ++i) ShipDamage::tick(s, 1.0f/60.0f);
        check(s.shield == 0,
              "T25l a dead shield generator SUPPRESSES regen permanently");
        // The gate is opt-in: fighters and every legacy caller are untouched.
        ShipDamageModel f = ShipDamage::makeFighter(50, 100);
        ShipDamage::applyDamage(f, 80);
        check(f.shield == 0 && f.hull == 70,
              "T25m the gate defaults OFF — legacy overflow math is unchanged");
    }

    // T25n/T25o — THE TWO TIME-TO-KILL PATHS. The gap between them IS the
    //   design, so it is pinned by a test rather than left to a spreadsheet.
    //   The player is ENERGY-bound, not cooldown-bound: 12 energy/s regen at
    //   8 per shot = 1.5 shots/s sustained x 90 dmg = 135 DPS. Both paths are
    //   simulated at that rate, at 60 Hz, with the shipped pools.
    {
        constexpr float kDt        = 1.0f / 60.0f;
        constexpr float kShotEvery = 1.0f / 1.5f;   // sustained, energy-bound
        constexpr int   kDmg       = 90;
        constexpr float kCycle = 13.0f, kOpen = 8.0f;   // reactor duty cycle
        constexpr int   kMult  = 6;                     // exposed-hull multiplier
        auto fresh = [] {
            ShipDamageModel m = ShipDamage::makeCapital(2400, 12000, 420);
            m.hullBleedWhileShielded = 0.08f;
            m.shieldRegenPerSec = 70.0f; m.shieldRegenDelaySec = 6.0f;
            return m;
        };
        // ---- HULL-ONLY: never touch a hardpoint, so she is never crippled
        //      and the reactor never opens. The slow path, by construction.
        float tHull = 0.0f;
        {
            ShipDamageModel m = fresh();
            float acc = 0.0f;
            while (!ShipDamage::isDestroyed(m) && tHull < 600.0f) {
                ShipDamage::tick(m, kDt); tHull += kDt; acc += kDt;
                if (acc >= kShotEvery) { acc -= kShotEvery;
                                         ShipDamage::applyDamage(m, kDmg); }
            }
        }
        // ---- HARDPOINT-FIRST: drop the shield, walk the four mounts, then
        //      ride the exposed reactor. The lesson the encounter teaches.
        float tHard = 0.0f;
        {
            ShipDamageModel m = fresh();
            float acc = 0.0f, reactorT2 = 0.0f;
            int   next = 0;                 // which mount is being worked
            while (!ShipDamage::isDestroyed(m) && tHard < 600.0f) {
                ShipDamage::tick(m, kDt); tHard += kDt; acc += kDt;
                bool crippled2 = true;
                for (int s2 = 0; s2 < (int)Subsystem::Count; ++s2)
                    if (!ShipDamage::subsystemDown(m, (Subsystem)s2)) crippled2 = false;
                if (crippled2) reactorT2 += kDt;
                if (acc < kShotEvery) continue;
                acc -= kShotEvery;
                if (m.shield > 0) { ShipDamage::applyDamage(m, kDmg); continue; }
                if (ShipDamage::subsystemDown(m, Subsystem::ShieldGen))
                    m.shieldRegenDisabled = true;
                while (next < (int)Subsystem::Count &&
                       ShipDamage::subsystemDown(m, (Subsystem)next)) ++next;
                if (next < (int)Subsystem::Count) {
                    ShipDamage::applyDamage(m, kDmg, (Subsystem)next);
                } else {
                    const bool open = std::fmod(reactorT2, kCycle) < kOpen;
                    ShipDamage::applyDamage(m, open ? kDmg * kMult : kDmg);
                }
            }
        }
        x3::logInfo("[ship-damage] TTK hardpoint-first " +
                    std::to_string((int)tHard) + " s vs hull-only " +
                    std::to_string((int)tHull) + " s");
        check(tHard > 25.0f && tHard < 90.0f,
              "T25n hardpoint-first kills the capital in a real boss-fight minute");
        check(tHull > tHard * 1.6f,
              "T25o hosing the HULL is >1.6x slower — hardpoint-first is the lesson");
    }

    // =======================================================================
    // T26 — ITEM G: THE LAUNCH BAYS
    // =======================================================================
    {
        CapitalBayState b{};
        CapitalBays::init(b);
        check(CapitalBays::aliveBays(b) == kCapitalBayCount,
              "T26 the bays seed alive with staggered launch clocks");
        // Cadence tightens as she dies, and never inverts.
        check(CapitalBays::periodFor(0.0f) < CapitalBays::periodFor(1.0f) &&
              std::fabs(CapitalBays::periodFor(1.0f) -
                        CapitalBays::kLaunchPeriod) < 1e-4f,
              "T26b the launch cadence tightens as her hull drops (never inverts)");
        // Every bay launches on its own clock over a long window.
        uint32_t seen = 0u; int launches = 0;
        for (int i = 0; i < 60 * 60; ++i) {
            const uint32_t mk = CapitalBays::update(b, 1.0f/60.0f, /*live*/0,
                                                    1.0f, true);
            seen |= mk;
            for (int k = 0; k < kCapitalBayCount; ++k) if (mk & (1u<<k)) ++launches;
        }
        check(seen == 0x7u && launches >= 12,
              "T26c all three bays launch, in waves over time (not one lump)");
        // A BLOWN bay stops launching — for good.
        CapitalBayState b2{}; CapitalBays::init(b2);
        check(CapitalBays::damageBay(b2, 1, CapitalBays::kBayHp),
              "T26d sustained fire blows a bay (damageBay reports the kill)");
        check(!CapitalBays::bayAlive(b2, 1) &&
              CapitalBays::aliveBays(b2) == kCapitalBayCount - 1,
              "T26e the blown bay reads dead, the others read alive");
        uint32_t after = 0u;
        for (int i = 0; i < 60 * 60; ++i)
            after |= CapitalBays::update(b2, 1.0f/60.0f, 0, 1.0f, true);
        check((after & 0x2u) == 0u && (after & 0x5u) == 0x5u,
              "T26f a DESTROYED bay never launches again; the rest keep going");
        check(!CapitalBays::damageBay(b2, 1, CapitalBays::kBayHp),
              "T26g a dead bay cannot be killed twice");
        // THE CAP. At the ceiling nothing launches, however long you wait —
        // escalation is bounded and cannot run away.
        CapitalBayState b3{}; CapitalBays::init(b3);
        uint32_t capped = 0u;
        for (int i = 0; i < 60 * 60; ++i)
            capped |= CapitalBays::update(b3, 1.0f/60.0f,
                                          CapitalBays::kLiveCap, 0.0f, true);
        check(capped == 0u,
              "T26h at the live-fighter cap the bays hold fire (escalation is bounded)");
        // And one tick can never blow PAST the cap even with every bay due.
        CapitalBayState b4{}; CapitalBays::init(b4);
        for (int i = 0; i < 60 * 60; ++i)
            CapitalBays::update(b4, 1.0f/60.0f, CapitalBays::kLiveCap - 1, 0.0f, true);
        int fired = 0;
        for (int k = 0; k < kCapitalBayCount; ++k)
            if (CapitalBays::update(b4, 1.0f/60.0f, CapitalBays::kLiveCap - 1,
                                    0.0f, true) & (1u<<k)) ++fired;
        check(fired <= 1,
              "T26i one tick can never launch past the cap (in-tick launches count)");
    }

    // T27 — a launched fighter is born AT the bay mouth heading OUT along the
    //       launch vector, never popped into empty space. This is the whole
    //       difference between "more little ships" and "it has bays".
    {
        EnemyShipManager em; em.init(4);
        const float mouth[3] = { 2600.0f, -180.0f, 470.0f };
        float dir[3] = { 0.10f, -0.22f, 0.97f };
        const float dl = std::sqrt(dir[0]*dir[0]+dir[1]*dir[1]+dir[2]*dir[2]);
        dir[0]/=dl; dir[1]/=dl; dir[2]/=dl;
        em.spawnLaunched(mouth, dir, 55.0f);
        check(em.count() == 1u, "T27 a bay launch produces exactly one fighter");
        const EnemyShip& s = em.ship(0);
        const float dp = std::sqrt((s.pos[0]-mouth[0])*(s.pos[0]-mouth[0]) +
                                   (s.pos[1]-mouth[1])*(s.pos[1]-mouth[1]) +
                                   (s.pos[2]-mouth[2])*(s.pos[2]-mouth[2]));
        check(dp < 1e-3f, "T27b it spawns AT the bay mouth, not in empty space");
        const float align = s.fwd[0]*dir[0] + s.fwd[1]*dir[1] + s.fwd[2]*dir[2];
        const float spd = std::sqrt(s.vel[0]*s.vel[0] + s.vel[1]*s.vel[1] +
                                    s.vel[2]*s.vel[2]);
        check(align > 0.999f && std::fabs(spd - 55.0f) < 0.01f,
              "T27c it leaves ALONG the launch vector with real exit speed");
        // It must actually clear the hull: after a second of flight it is
        // measurably farther out along the launch line than it started.
        const float p0[3] = { s.pos[0], s.pos[1], s.pos[2] };
        const float far[3] = { 100000.0f, 0.0f, 0.0f };   // player miles away:
        for (int i = 0; i < 60; ++i) em.update(1.0f/60.0f, far, far);  // stays on course
        const EnemyShip& s2 = em.ship(0);
        const float travelled = (s2.pos[0]-p0[0])*dir[0] + (s2.pos[1]-p0[1])*dir[1] +
                                (s2.pos[2]-p0[2])*dir[2];
        check(travelled > 30.0f,
              "T27d it FLIES OUT along the vector (clears the hull, not a hover)");
        // Degenerate launch vectors are survivable, not a crash or a NaN.
        const float zero[3] = { 0.0f, 0.0f, 0.0f };
        em.spawnLaunched(mouth, zero, 55.0f);
        const EnemyShip& s3 = em.ship(em.count() - 1u);
        const float l3 = std::sqrt(s3.fwd[0]*s3.fwd[0] + s3.fwd[1]*s3.fwd[1] +
                                   s3.fwd[2]*s3.fwd[2]);
        check(std::fabs(l3 - 1.0f) < 1e-4f,
              "T27e a degenerate launch vector still yields a unit heading");
    }

    x3::logInfo("ship-damage: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
    std::printf("ship-damage: %d/%d passed\n", pass, total);
    std::fflush(stdout);
    return pass == total;
}

} // namespace x3::space
