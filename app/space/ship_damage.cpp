// app/space/ship_damage.cpp — S10 ship damage model implementation + self-test.
//
// Pure value-type logic; no GPU, no physics. See ship_damage.h for the contract.
#include "ship_damage.h"

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
        m.hull -= remaining;
        if (m.hull < 0) m.hull = 0;    // floored at 0 == destroyed
        if (m.hull == 0) {
            x3::logInfo("[ship-damage] HULL 0 — ship destroyed");
        }
    }
}

void ShipDamage::tick(ShipDamageModel& m, float dt) {
    if (dt <= 0.0f) return;
    if (m.hull <= 0) return; // dead ships don't regen

    m.timeSinceHit += dt;

    // Shield regen resumes only after the delay has fully elapsed since the
    // last hit. Subsystems intentionally do NOT auto-regen (host must repair).
    if (m.timeSinceHit >= m.shieldRegenDelaySec && m.shield < m.maxShield) {
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

    x3::logInfo("ship-damage: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
    std::printf("ship-damage: %d/%d passed\n", pass, total);
    std::fflush(stdout);
    return pass == total;
}

} // namespace x3::space
