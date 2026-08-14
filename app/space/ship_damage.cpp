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
}

int CapitalDeathSequence::step(CapitalDeathState& s, float dt,
                               DeathEvent* out, int maxOut) {
    if (!s.active) return 0;
    if (dt > 0.0f) s.t += dt;

    // ---- Presentation scalars ------------------------------------------
    s.lights = lightsAt(s.t);
    // Dead-stick roll: angular rate eases in as the attitude thrusters die,
    // asymptoting near 0.5 rad/s, so the hull is visibly rolling by ~1 s and
    // has turned ~100 deg by the end of the window.
    {
        const float rate = 0.50f * (1.0f - std::exp(-s.t * 1.30f));
        s.tumble += rate * (dt > 0.0f ? dt : 0.0f);
    }
    // She stops holding station and goes down: a gentle constant acceleration
    // (not physics — a READ). ~11 m by 3 s, ~31 m by 5 s.
    s.sag = 1.25f * s.t * s.t;

    // ---- Emit every beat that came due, up to the budget ----------------
    int n = 0;
    const int cap = maxOut < kMaxEventsPerStep ? maxOut : kMaxEventsPerStep;
    while (n < cap && s.cursor < kScheduleCount && kSchedule[s.cursor].t <= s.t) {
        const DeathBeat& b = kSchedule[s.cursor];
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
            e.pos[k] = s.center[k] + ax[k] * along
                                   + rt[k] * b.lat
                                   + tu[k] * b.vert;
            // The hull is already sagging when the later beats fire.
            if (k == 1) e.pos[k] -= s.sag;
            // Plumes and debris stream AWAY from the spine.
            e.drift[k] = rt[k] * b.lat * 0.35f + tu[k] * b.vert * 0.35f;
        }
        // A vent with no lateral offset would have no drift at all; give it
        // a small outward push so the plume never hangs as a static bead.
        if (std::fabs(b.lat) < 1e-3f && std::fabs(b.vert) < 1e-3f)
            for (int k = 0; k < 3; ++k) e.drift[k] = tu[k] * 6.0f;
    }

    if (s.cursor >= kScheduleCount && s.t >= kDurationSec) {
        s.finished = true;
        s.active   = false;
    }
    return n;
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

    x3::logInfo("ship-damage: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
    std::printf("ship-damage: %d/%d passed\n", pass, total);
    std::fflush(stdout);
    return pass == total;
}

} // namespace x3::space
