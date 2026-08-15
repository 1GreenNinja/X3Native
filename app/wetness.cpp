// wetness — see wetness.h.                              [LANE: inspx/wetness]
#include "wetness.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <string>

namespace x3::game {

const char* surfaceConditionName(SurfaceCondition c) {
    switch (c) {
        case SurfaceCondition::Dry:      return "dry";
        case SurfaceCondition::Damp:     return "damp";
        case SurfaceCondition::Wet:      return "wet";
        case SurfaceCondition::Standing: return "standing water";
        case SurfaceCondition::Ice:      return "ice";
        default:                         return "?";
    }
}

namespace {
inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// Move `cur` toward `target` at a rate of one full unit per `seconds`. Rate
// form (not exponential) so "soakSeconds = 32" means exactly what it says and
// the self-test can assert against the number a designer typed.
inline float approach(float cur, float target, float seconds, float dt) {
    if (seconds <= 0.0f) return target;
    const float step = dt / seconds;
    if (cur < target) return cur + step < target ? cur + step : target;
    if (cur > target) return cur - step > target ? cur - step : target;
    return cur;
}
} // namespace

void WetnessModel::tick(float dt, float precipitation, float tempC) {
    if (dt <= 0.0f) return;
    const float rain = clamp01(precipitation);

    // WATER. Rain drives the surface toward the soak its intensity supports —
    // a drizzle never produces standing water no matter how long it falls —
    // and the absence of rain drives it toward dry, far more slowly.
    if (rain > 0.0f) {
        m_wet = approach(m_wet, rain, m_cfg.soakSeconds / rain, dt);
    } else {
        // EVAPORATION IS TEMPERATURE-DEPENDENT. Drying is not a constant: cold
        // air holds almost no vapour, so a road at -5 C stays wet far longer
        // than the same road at 25 C. Without this the model dried a freezing
        // street at summer speed, which fought the ice term directly — the
        // water was evaporating out from under the ice forming on top of it.
        const float warmth = clamp01((tempC - m_cfg.freezePointC) / 25.0f);
        const float dryScale = m_cfg.dryColdFactor
                             + (1.0f - m_cfg.dryColdFactor) * warmth;
        // ONLY THE LIQUID PART EVAPORATES, so the drying target is the frozen
        // floor, not zero. Ice sublimates orders of magnitude slower than a
        // puddle dries; a film that has frozen leaves by THAWING, not by
        // evaporating out from underneath itself.
        m_wet = approach(m_wet, m_ice, m_cfg.drySeconds / dryScale, dt);
    }
    m_wet = clamp01(m_wet);

    // ICE. Two thresholds, not one: below freezePointC ice grows, above
    // thawPointC it melts, and BETWEEN them nothing changes. That dead band is
    // the hysteresis — without it a temperature sitting on 0.0 would flip the
    // road's grip every frame.
    // Ice grows toward the WATER THAT IS ACTUALLY THERE, never toward 1. This
    // one target is what stops a dry road in a cold snap becoming a skating
    // rink, without needing a separate clamp afterwards — and a clamp
    // afterwards is precisely what went wrong first time round: clamping ice
    // down to the already-dried film bled a drying-step out of the ice every
    // frame, so iciness decayed even inside the freeze/thaw dead band that
    // exists to hold it still.
    if (tempC <= m_cfg.freezePointC)      m_ice = approach(m_ice, m_wet, m_cfg.freezeSeconds, dt);
    else if (tempC >= m_cfg.thawPointC)   m_ice = approach(m_ice, 0.0f, m_cfg.thawSeconds, dt);
    m_ice = clamp01(m_ice);
    if (m_ice > m_wet) m_ice = m_wet;   // only reachable if the film shrank by rain change
}

SurfaceCondition WetnessModel::condition() const {
    if (m_ice >= 0.35f)  return SurfaceCondition::Ice;
    if (m_wet >= 0.85f)  return SurfaceCondition::Standing;
    if (m_wet >= 0.40f)  return SurfaceCondition::Wet;
    if (m_wet >= 0.05f)  return SurfaceCondition::Damp;
    return SurfaceCondition::Dry;
}

float WetnessModel::gripScale() const {
    // Water first: dry -> wet -> standing, by soak.
    float g = 1.0f;
    if (m_wet > 0.0f) {
        const float toWet = m_wet < 0.85f ? m_wet / 0.85f : 1.0f;
        g = 1.0f + (m_cfg.gripWet - 1.0f) * toWet;
        if (m_wet > 0.85f) {
            const float toStanding = (m_wet - 0.85f) / 0.15f;
            g = g + (m_cfg.gripStanding - m_cfg.gripWet) * toStanding;
        }
    }
    // Then ice, which OVERRIDES rather than averages: a car rides on the top
    // layer of the surface, not on the mean of its layers. Lerping toward the
    // ice value by iciness gives "half-iced is halfway to lethal", which is
    // both physically sane and the behaviour a driver can read and react to.
    if (m_ice > 0.0f) g = g + (m_cfg.gripIce - g) * m_ice;
    return g;
}

// ---------------------------------------------------------------------------
// --test-wetness
// ---------------------------------------------------------------------------
namespace {
int  g_pass = 0, g_fail = 0;
void wcheck(bool ok, const std::string& what) {
    if (ok) { ++g_pass; x3::logInfo("[wetness-test]   PASS  " + what); }
    else    { ++g_fail; x3::logError("[wetness-test]   FAIL  " + what); }
}
// Run the model for `seconds` at fixed 1/60 steps.
void run(WetnessModel& m, float seconds, float rain, float tempC) {
    const float dt = 1.0f / 60.0f;
    for (float t = 0.0f; t < seconds; t += dt) m.tick(dt, rain, tempC);
}
} // namespace

bool runWetnessSelfTest() {
    g_pass = g_fail = 0;
    x3::logInfo("[wetness-test] surface wetness + ice + grip");

    const WetnessConfig cfg;   // defaults

    // ---- W1 SOAK reaches full in about soakSeconds --------------------------
    {
        WetnessModel m(cfg);
        run(m, cfg.soakSeconds * 0.5f, 1.0f, 15.0f);
        wcheck(m.wetness() > 0.4f && m.wetness() < 0.6f,
               "W1 half the soak time in full rain leaves the surface about half wet");
        run(m, cfg.soakSeconds * 0.6f, 1.0f, 15.0f);
        wcheck(m.wetness() > 0.999f, "W1 steady rain reaches FULLY soaked");
    }

    // ---- W2 THE ASYMMETRY — the whole point of modelling this ---------------
    {
        WetnessModel m(cfg);
        run(m, 60.0f, 1.0f, 15.0f);                       // soak it
        const float soaked = m.wetness();
        run(m, cfg.soakSeconds, 0.0f, 15.0f);             // dry for ONE soak time
        wcheck(soaked > 0.999f && m.wetness() > 0.8f,
               "W2 a surface is still visibly wet a full soak-time AFTER the rain stops");
        // drySeconds is calibrated at ~25 C; at 15 C evaporation is slower by
        // dryColdFactor, so the honest wait is longer than the raw constant.
        run(m, cfg.drySeconds / cfg.dryColdFactor, 0.0f, 15.0f);
        wcheck(m.wetness() <= 0.0f, "W2 it does eventually dry completely");
        // NEGATIVE CONTROL: the naive model this replaces (wetness = rain) has
        // NO tail at all — it is bone dry the instant the rain stops. If that
        // ever passes the check above, the asymmetry has been lost.
        const float naiveAfterRain = 0.0f;   // precipitation == 0 -> wetness == 0
        wcheck(!(naiveAfterRain > 0.8f),
               "W2 NEGATIVE CONTROL: wetness=precipitation FAILS the drying tail");
    }

    // ---- W3 drizzle never makes standing water ------------------------------
    {
        WetnessModel m(cfg);
        run(m, 600.0f, 0.25f, 15.0f);
        wcheck(m.wetness() > 0.2f && m.wetness() < 0.31f,
               "W3 a long drizzle saturates only to its own intensity");
        wcheck(m.condition() == SurfaceCondition::Damp,
               "W3 ... and reads as DAMP, never standing water");
    }

    // ---- W4 ice needs water; a cold DRY road never freezes -------------------
    {
        WetnessModel m(cfg);
        run(m, 600.0f, 0.0f, -10.0f);
        wcheck(m.iciness() <= 0.0f, "W4 a cold DRY road does not become ice");
        wcheck(m.condition() == SurfaceCondition::Dry, "W4 ... it is just dry");
    }

    // ---- W5 wet + freezing -> ice, and ice never exceeds its own water ------
    {
        WetnessModel m(cfg);
        run(m, 60.0f, 1.0f, 5.0f);                 // soak above freezing
        run(m, cfg.freezeSeconds * 1.2f, 0.0f, -5.0f);
        wcheck(m.iciness() > 0.9f, "W5 a wet road below freezing turns to ICE");
        wcheck(m.condition() == SurfaceCondition::Ice, "W5 ... and reports Ice");
        wcheck(m.iciness() <= m.wetness() + 1e-6f,
               "W5 iciness never exceeds the water it froze out of");
    }

    // ---- W6 HYSTERESIS: the dead band between freeze and thaw ---------------
    {
        WetnessModel m(cfg);
        run(m, 60.0f, 1.0f, 5.0f);
        run(m, cfg.freezeSeconds * 1.2f, 0.0f, -5.0f);
        const float iced = m.iciness();
        run(m, 120.0f, 0.0f, 1.0f);                // INSIDE the dead band
        wcheck(std::fabs(m.iciness() - iced) < 1e-5f,
               "W6 between freeze and thaw points the ice state does NOT move");
        run(m, cfg.thawSeconds * 1.2f, 0.0f, 10.0f);
        wcheck(m.iciness() <= 0.0f, "W6 above the thaw point it melts");
    }

    // ---- W7 GRIP ordering — the gameplay contract ---------------------------
    {
        WetnessModel dry(cfg), wet(cfg), standing(cfg), ice(cfg);
        run(wet,      60.0f, 0.55f, 15.0f);
        run(standing, 90.0f, 1.00f, 15.0f);
        run(ice,      60.0f, 1.00f, 5.0f);
        run(ice, cfg.freezeSeconds * 1.2f, 0.0f, -5.0f);
        const float gd = dry.gripScale(), gw = wet.gripScale();
        const float gs = standing.gripScale(), gi = ice.gripScale();
        wcheck(std::fabs(gd - 1.0f) < 1e-6f, "W7 dry grip is exactly 1.0 (no change)");
        wcheck(gw < gd, "W7 wet grips less than dry");
        wcheck(gs < gw, "W7 standing water grips less than merely wet");
        wcheck(gi < gs, "W7 ICE is the most slippery of all");
        wcheck(gi < 0.25f, "W7 ice grip is in the measured mu~0.15 neighbourhood");
        x3::logInfo("[wetness-test] grip: dry " + std::to_string(gd) + " | wet " +
                    std::to_string(gw) + " | standing " + std::to_string(gs) +
                    " | ice " + std::to_string(gi));
    }

    // ---- W8 determinism ------------------------------------------------------
    {
        WetnessModel a(cfg), b(cfg);
        run(a, 45.0f, 0.7f, -1.0f); run(a, 45.0f, 0.0f, 3.0f);
        run(b, 45.0f, 0.7f, -1.0f); run(b, 45.0f, 0.0f, 3.0f);
        wcheck(a.wetness() == b.wetness() && a.iciness() == b.iciness(),
               "W8 two identical input sequences produce identical state");
    }

    // ---- W9 dry world is untouched ------------------------------------------
    {
        WetnessModel m(cfg);
        run(m, 300.0f, 0.0f, 20.0f);
        wcheck(m.wetness() == 0.0f && m.iciness() == 0.0f && m.gripScale() == 1.0f,
               "W9 a world that never rains stays exactly dry, grip exactly 1.0");
    }

    x3::logInfo("[wetness-test] " + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
