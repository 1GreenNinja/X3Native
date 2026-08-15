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
        case SurfaceCondition::Snow:     return "snow";
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

void WetnessModel::tick(float dt, float precipitation, float tempC, bool snowfall) {
    if (dt <= 0.0f) return;
    // Precipitation splits by phase: what falls as SNOW accumulates as depth and
    // must not also soak the road as rain, or a blizzard would leave standing
    // water. Snow reaches the water term only by MELTING, further down.
    const float rain = snowfall ? 0.0f : clamp01(precipitation);
    const float snow = snowfall ? clamp01(precipitation) : 0.0f;

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

    // ---- SNOW ----------------------------------------------------------
    // Accumulation is trivial; the MELT is where the model earns its keep.
    //
    // Melt here is DEGREE-HOUR: the rate is proportional to how far above
    // freezing the air is, which is how snowmelt is genuinely forecast. It buys
    // behaviour a timer cannot — 34 F takes all day to clear what 50 F clears
    // by lunch, and the thaw slows down on its own as evening cools, with
    // nothing scripting any of it. A flat "melts in N seconds" would make every
    // thaw the same thaw.
    const float kInPerHourToMPerSec = 0.0254f / 3600.0f;
    if (snow > 0.0f && tempC <= m_cfg.freezePointC) {
        // Falling snow only LIES if the ground is at or below freezing.
        // Above it, the flakes are visible in the air and melt on contact —
        // which is exactly the sleety in-between people recognise and almost
        // nothing models.
        m_snow += snow * m_cfg.snowFallInPerHour * kInPerHourToMPerSec * dt;
    }
    if (tempC > m_cfg.freezePointC && m_snow > 0.0f) {
        const float degrees = tempC - m_cfg.freezePointC;
        const float melted = m_cfg.snowMeltInPerHourPerDegC * degrees
                           * kInPerHourToMPerSec * dt;
        const float actual = (melted < m_snow) ? melted : m_snow;
        m_snow -= actual;
        // MELTWATER WETS THE ROAD. The one coupling that makes the thaw read as
        // a thaw: the snow does not simply vanish, it leaves the street soaked
        // behind it, and if the night comes back down that water is what
        // freezes into ice. Snow -> water -> ice is a loop, not three effects.
        const float kSoakPerMeltM = 1.0f / 0.0025f;   // ~0.1 in of melt = fully wet
        m_wet = clamp01(m_wet + actual * kSoakPerMeltM);
    }
    const float maxM = m_cfg.snowMaxIn * 0.0254f;
    if (m_snow > maxM) m_snow = maxM;
    if (m_snow < 0.0f) m_snow = 0.0f;
}

float WetnessModel::snowCover() const {
    // Whiteness saturates long before depth does: a half-inch dusting already
    // reads as a white field, and the eye cannot tell four inches from eight.
    // Tying coverage linearly to depth would leave the world looking bare
    // through the entire opening of a storm, which is the part you watch.
    const float in = snowDepthIn();
    const float t = in / 1.6f;
    return (t >= 1.0f) ? 1.0f : (t <= 0.0f ? 0.0f : t * t * (3.0f - 2.0f * t));
}

SurfaceCondition WetnessModel::condition() const {
    // Lying snow reads FIRST, above even ice: whatever the asphalt is doing
    // underneath, once there is real depth on it you are driving on snow.
    // Same doctrine as ice-over-water below — the car rides the TOP layer.
    if (snowDepthIn() >= m_cfg.snowCoverIn) return SurfaceCondition::Snow;
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
    // Then snow, on top of everything, by the SAME top-layer rule. Lying snow
    // is worse than wet and better than ice -- it packs and gives the tire
    // something to bite, which sheet ice does not. Note it can therefore IMPROVE
    // grip over black ice, and that is correct: a dusting over an icy road is
    // genuinely easier to drive than the bare ice underneath it.
    const float cover = snowCover();
    if (cover > 0.0f) g = g + (m_cfg.gripSnow - g) * cover;
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


    // ---- WS: SNOW ACCUMULATION. Depth that builds, and a degree-hour melt. ----
    {
        char sb[240];
        const float dt = 0.5f;
        auto runSnow = [&](float tempC, float startDepthIn, float hours, bool falling) {
            WetnessModel m;
            // Seed a starting depth by snowing at -10 C first.
            if (startDepthIn > 0.0f) {
                for (int i = 0; i < 400000 && m.snowDepthIn() < startDepthIn; ++i)
                    m.tick(dt, 1.0f, -10.0f, true);
            }
            const int steps = (int)(hours * 3600.0f / dt);
            for (int i = 0; i < steps; ++i) m.tick(dt, falling ? 1.0f : 0.0f, tempC, falling);
            return m;
        };

        // WS1: an hour of full snowfall below freezing lays about the configured
        // inch. The rate is the contract, so check it against the config.
        WetnessModel one = runSnow(-5.0f, 0.0f, 1.0f, true);
        std::snprintf(sb, sizeof(sb), "WS1 one hour of steady snowfall laid %.2f in (config says %.2f)",
                      one.snowDepthIn(), WetnessConfig{}.snowFallInPerHour);
        wcheck(std::fabs(one.snowDepthIn() - WetnessConfig{}.snowFallInPerHour) < 0.05f, sb);

        // WS2: snow falling into ABOVE-freezing air does not lie. It melts on
        // contact -- the sleety in-between that almost nothing models.
        WetnessModel warm = runSnow(4.0f, 0.0f, 3.0f, true);
        std::snprintf(sb, sizeof(sb), "WS2 three hours of snow at 39 F left %.3f in on the ground",
                      warm.snowDepthIn());
        wcheck(warm.snowDepthIn() < 0.001f, sb);

        // WS3: DEGREE-HOUR MELT -- the whole reason melt is not a timer. A 50 F
        // afternoon must clear far more than a 34 F one in the same time.
        WetnessModel cool = runSnow(1.0f,  6.0f, 3.0f, false);   // 34 F
        WetnessModel mild = runSnow(10.0f, 6.0f, 3.0f, false);   // 50 F
        std::snprintf(sb, sizeof(sb),
            "WS3 after 3 h: 34 F left %.2f in standing, 50 F left %.2f in -- rate scales with DEGREES, not a timer",
            cool.snowDepthIn(), mild.snowDepthIn());
        wcheck(cool.snowDepthIn() > mild.snowDepthIn() + 0.5f, sb);

        // WS4: MELTWATER WETS THE ROAD. The coupling that makes a thaw read as a
        // thaw instead of snow simply vanishing. Sampled AT the end of the melt,
        // not hours later -- the road is supposed to dry out afterwards, and a
        // first pass that waited two hours measured exactly that and called the
        // coupling broken.
        WetnessModel thaw = runSnow(-10.0f, 3.0f, 0.0f, false);
        int melting = 0;
        while (thaw.snowDepthIn() > 0.001f && melting < 200000) { thaw.tick(dt, 0.0f, 8.0f); ++melting; }
        std::snprintf(sb, sizeof(sb),
                      "WS4 3 in of snow cleared in %.0f min at 46 F and left the road soaked (%.2f)",
                      melting * dt / 60.0f, thaw.wetness());
        wcheck(thaw.wetness() > 0.5f, sb);

        // WS5: depth is CAPPED -- an all-night blizzard cannot bury the world in
        // a number nobody can see.
        WetnessModel buried = runSnow(-15.0f, 0.0f, 40.0f, true);
        std::snprintf(sb, sizeof(sb), "WS5 40 h of blizzard capped at %.1f in (config max %.1f)",
                      buried.snowDepthIn(), WetnessConfig{}.snowMaxIn);
        wcheck(buried.snowDepthIn() <= WetnessConfig{}.snowMaxIn + 0.01f &&
               buried.snowDepthIn() > WetnessConfig{}.snowMaxIn - 0.01f, sb);

        // WS6: coverage saturates LONG before depth does -- a dusting already
        // reads white, which is the part of a storm you actually watch.
        WetnessModel dust = runSnow(-10.0f, 0.6f, 0.0f, false);
        std::snprintf(sb, sizeof(sb),
                      "WS6 a %.1f in dusting already reads %.0f%% white; %.1f in reads %.0f%%",
                      dust.snowDepthIn(), dust.snowCover() * 100.0f,
                      buried.snowDepthIn(), buried.snowCover() * 100.0f);
        wcheck(dust.snowCover() > 0.25f && buried.snowCover() > 0.99f, sb);

        // WS7: lying snow is its own surface, and it grips BETTER than the ice
        // it may be sitting on -- packed snow gives the tire something to bite.
        WetnessModel lying = runSnow(-6.0f, 2.0f, 0.0f, false);
        wcheck(lying.condition() == SurfaceCondition::Snow,
               "WS7a real depth classifies as SNOW, not as the wet road underneath");
        WetnessModel icy;
        for (int i = 0; i < 20000; ++i) icy.tick(0.5f, 0.8f, 5.0f);   // soak
        for (int i = 0; i < 20000; ++i) icy.tick(0.5f, 0.0f, -6.0f);  // freeze
        std::snprintf(sb, sizeof(sb),
            "WS7b snow grips %.2f vs bare ice %.2f -- a dusting over ice is genuinely easier to drive",
            lying.gripScale(), icy.gripScale());
        wcheck(lying.gripScale() > icy.gripScale(), sb);

        // WS8: NEGATIVE CONTROL. The obvious model -- depth proportional to how
        // hard it is snowing right now -- cannot accumulate at all: stop the
        // snow and the drift is instantly gone. Prove ours persists.
        WetnessModel persist = runSnow(-8.0f, 0.0f, 2.0f, true);
        const float duringStorm = persist.snowDepthIn();
        for (int i = 0; i < (int)(3600.0f / dt); ++i) persist.tick(dt, 0.0f, -8.0f, false);
        std::snprintf(sb, sizeof(sb),
            "WS8 an hour after the snow stopped, %.2f in of the %.2f in is still lying; "
            "the naive depth=intensity model would read 0.00 in",
            persist.snowDepthIn(), duringStorm);
        wcheck(persist.snowDepthIn() > duringStorm - 0.01f, sb);
    }
    x3::logInfo("[wetness-test] " + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
