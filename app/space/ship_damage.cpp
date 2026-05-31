// app/space/ship_damage.cpp — S10 ship damage model implementation + self-test.
//
// Pure value-type logic; no GPU, no physics. See ship_damage.h for the contract.
#include "ship_damage.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <string>

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

    x3::logInfo("ship-damage: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
    std::printf("ship-damage: %d/%d passed\n", pass, total);
    std::fflush(stdout);
    return pass == total;
}

} // namespace x3::space
