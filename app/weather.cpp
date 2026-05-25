// EFLZ Weather system — implementation. See weather.h for the contract.
//
// Per-state "looks" nudge the EXISTING sky/fog params (haze == fog density,
// exposure, sun color/intensity) + a host ambient term + precipitation +
// hazard. The scheduler holds a state for a randomized (but deterministic)
// duration, then cross-fades to a biome-legal next state over transitionSeconds.

#include "weather.h"
#include "engine/core/x3_log.h"

#include <cmath>
#include <string>

namespace x3::game {

namespace {

inline float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
inline float lerp(float a, float b, float u) { return a + (b - a) * u; }
inline void  lerp3(const float a[3], const float b[3], float u, float out[3]) {
    out[0] = lerp(a[0], b[0], u);
    out[1] = lerp(a[1], b[1], u);
    out[2] = lerp(a[2], b[2], u);
}
inline float smooth(float u) { u = clamp01(u); return u * u * (3.0f - 2.0f * u); }

// The per-state weather "look": how it nudges the sky/fog/ambient + its hazard /
// precipitation profile. Sun color/intensity here are MULTIPLIERS the host can
// fold onto the base (ToD) sun, but we also write absolute-ish values into the
// sample's SkyParams so a host with no ToD still gets a plausible sky. Linear RGB.
struct WeatherLook {
    float haze;            // -> sky.haze (fog density 0..1)
    float exposure;        // -> sky.exposure (storms/fog dim the scene)
    float sunIntensity;    // -> sky.sunIntensity (clouds/fog cut the sun)
    float sunTint[3];      // multiplies sun color (grey skies desaturate)
    float ambient[3];      // ambient/fill nudge
    float precipitation;   // 0..1 rain/snow particle intensity hint
    float hazardLevel;     // 0..1 (0 == non-hazardous state)
};

// Table indexed by WeatherState. Hazard states: Storm (lightning), Sandstorm
// (desert hazard), Snow (blizzard/cold), and Fog is conditionally hazardous in
// the Swamp (poison) — handled at sample time via the biome. Base table marks
// the unconditional hazards; swamp-fog poison is layered on in rebuildSample().
const WeatherLook kLooks[(int)WeatherState::Count] = {
    /* Clear     */ { 0.30f, 1.00f, 1.00f, { 1.00f, 1.00f, 1.00f }, { 0.16f, 0.17f, 0.20f }, 0.00f, 0.00f },
    /* Cloudy    */ { 0.45f, 0.92f, 0.70f, { 0.92f, 0.93f, 0.95f }, { 0.15f, 0.16f, 0.18f }, 0.00f, 0.00f },
    /* Rain      */ { 0.60f, 0.80f, 0.50f, { 0.80f, 0.84f, 0.90f }, { 0.13f, 0.14f, 0.17f }, 0.65f, 0.00f },
    /* Storm     */ { 0.72f, 0.62f, 0.32f, { 0.62f, 0.66f, 0.78f }, { 0.10f, 0.11f, 0.15f }, 0.90f, 0.70f },
    /* Fog       */ { 0.92f, 0.78f, 0.45f, { 0.85f, 0.86f, 0.88f }, { 0.15f, 0.15f, 0.16f }, 0.00f, 0.00f },
    /* Sandstorm */ { 0.95f, 0.70f, 0.40f, { 1.05f, 0.85f, 0.55f }, { 0.18f, 0.13f, 0.08f }, 0.40f, 0.85f },
    /* Snow      */ { 0.70f, 0.95f, 0.65f, { 0.95f, 0.97f, 1.05f }, { 0.18f, 0.19f, 0.22f }, 0.70f, 0.55f },
};

// Biome -> the set of legal states (Clear + Cloudy universal). A bit per state.
inline uint32_t bit(WeatherState s) { return 1u << (uint32_t)s; }

uint32_t biomeMask(Biome b) {
    const uint32_t base = bit(WeatherState::Clear) | bit(WeatherState::Cloudy);
    switch (b) {
        case Biome::Temperate:
            return base | bit(WeatherState::Rain) | bit(WeatherState::Storm) | bit(WeatherState::Fog);
        case Biome::Desert:
            return base | bit(WeatherState::Sandstorm);
        case Biome::Swamp:
            return base | bit(WeatherState::Rain) | bit(WeatherState::Fog) | bit(WeatherState::Storm);
        case Biome::Snow:
            return base | bit(WeatherState::Snow) | bit(WeatherState::Fog);
        default:
            return base;
    }
}

} // namespace

const char* weatherStateName(WeatherState s) {
    switch (s) {
        case WeatherState::Clear:     return "clear";
        case WeatherState::Cloudy:    return "cloudy";
        case WeatherState::Rain:      return "rain";
        case WeatherState::Storm:     return "storm";
        case WeatherState::Fog:       return "fog";
        case WeatherState::Sandstorm: return "sandstorm";
        case WeatherState::Snow:      return "snow";
        default:                      return "?";
    }
}

const char* biomeName(Biome b) {
    switch (b) {
        case Biome::Temperate: return "temperate";
        case Biome::Desert:    return "desert";
        case Biome::Swamp:     return "swamp";
        case Biome::Snow:      return "snow";
        default:               return "?";
    }
}

bool weatherAllowedInBiome(WeatherState state, Biome biome) {
    if (state >= WeatherState::Count || biome >= Biome::Count) return false;
    return (biomeMask(biome) & bit(state)) != 0;
}

uint32_t Weather::nextRand() {
    // 64-bit LCG (Knuth MMIX constants); return the high bits.
    m_rng = m_rng * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(m_rng >> 32);
}

void Weather::reset() {
    m_biome  = Biome::Temperate;
    m_from   = WeatherState::Clear;
    m_target = WeatherState::Clear;
    m_transT = m_cfg.transitionSeconds;   // settled
    m_holdT  = 0.0f;
    m_rng    = m_cfg.seed ? m_cfg.seed : 0x9E3779B97F4A7C15ull;
    // First hold duration (deterministic).
    float span = m_cfg.maxHoldSeconds - m_cfg.minHoldSeconds;
    m_holdFor = m_cfg.minHoldSeconds + (span > 0.0f ? (nextRand() / 4294967296.0f) * span : 0.0f);
    rebuildSample();
}

void Weather::setBiome(Biome b) {
    if (b >= Biome::Count) return;
    m_biome = b;
    // If the current target is now illegal, transition to a legal state.
    if (!weatherAllowedInBiome(m_target, m_biome)) {
        WeatherState to = weatherAllowedInBiome(WeatherState::Clear, m_biome)
                              ? WeatherState::Clear : rollNextState();
        beginTransition(to, /*instant*/ false);
    } else {
        rebuildSample();
    }
}

bool Weather::forceState(WeatherState state, bool instant) {
    if (!weatherAllowedInBiome(state, m_biome)) return false;
    beginTransition(state, instant);
    return true;
}

void Weather::beginTransition(WeatherState to, bool instant) {
    // Snap the current blended state as the new "from" so a transition that
    // interrupts another still eases smoothly from where it visually is.
    m_from   = m_target;
    m_target = to;
    m_transT = instant ? m_cfg.transitionSeconds : 0.0f;
    m_holdT  = 0.0f;
    // Schedule the next hold duration.
    float span = m_cfg.maxHoldSeconds - m_cfg.minHoldSeconds;
    m_holdFor = m_cfg.minHoldSeconds + (span > 0.0f ? (nextRand() / 4294967296.0f) * span : 0.0f);
    rebuildSample();
}

WeatherState Weather::rollNextState() {
    // Gather the legal states for the biome, then pick one deterministically that
    // differs from the current target when possible.
    WeatherState legal[(int)WeatherState::Count];
    int n = 0;
    uint32_t mask = biomeMask(m_biome);
    for (uint32_t i = 0; i < (uint32_t)WeatherState::Count; ++i)
        if (mask & (1u << i)) legal[n++] = (WeatherState)i;
    if (n == 0) return WeatherState::Clear;

    WeatherState pick = legal[nextRand() % (uint32_t)n];
    if (pick == m_target && n > 1) {
        // Re-roll once to avoid a no-op transition (still deterministic).
        pick = legal[nextRand() % (uint32_t)n];
        if (pick == m_target) pick = legal[((uint32_t)pick + 1) % (uint32_t)n];
    }
    return pick;
}

void Weather::tick(float dt) {
    if (dt < 0.0f) return;

    // Advance an in-progress transition.
    if (m_transT < m_cfg.transitionSeconds) {
        m_transT += dt;
        if (m_transT >= m_cfg.transitionSeconds) m_transT = m_cfg.transitionSeconds;
        rebuildSample();
        // Any leftover dt past the transition spills into the hold timer below.
        float settled = m_transT - m_cfg.transitionSeconds;
        if (settled < 0.0f) return;        // still mid-transition
        dt = 0.0f;                          // already consumed by the transition this tick
    }

    // Settled: accumulate hold time + re-roll when the schedule expires.
    m_holdT += dt;
    if (m_cfg.autoSchedule && m_holdT >= m_holdFor) {
        WeatherState next = rollNextState();
        beginTransition(next, /*instant*/ false);
    }
}

void Weather::rebuildSample() {
    float u = (m_cfg.transitionSeconds > 0.0f)
                  ? clamp01(m_transT / m_cfg.transitionSeconds) : 1.0f;
    float e = smooth(u);   // eased transition factor: 0 -> from, 1 -> target

    const WeatherLook& a = kLooks[(int)m_from];
    const WeatherLook& b = kLooks[(int)m_target];

    float haze = lerp(a.haze, b.haze, e);
    float expo = lerp(a.exposure, b.exposure, e);
    float sint = lerp(a.sunIntensity, b.sunIntensity, e);
    float tint[3]; lerp3(a.sunTint, b.sunTint, e, tint);
    float amb[3]; lerp3(a.ambient, b.ambient, e, amb);
    float precip = lerp(a.precipitation, b.precipitation, e);

    // Hazard flips at the transition MIDPOINT (e >= 0.5 -> use target hazard,
    // else outgoing) so the flag tracks the visible dominant state. Swamp fog is
    // poison-hazardous even though base Fog is not.
    auto effectiveHazard = [this](WeatherState s) -> float {
        float h = kLooks[(int)s].hazardLevel;
        if (s == WeatherState::Fog && m_biome == Biome::Swamp) h = 0.60f;  // poison fog
        return h;
    };
    WeatherState dominant = (e >= 0.5f) ? m_target : m_from;
    float hazard = effectiveHazard(dominant);

    // ---- Write the renderer-ready sky snapshot. The host with a ToD sun should
    // MULTIPLY tint/intensity onto the ToD sun; standalone we provide a plausible
    // neutral base so weather alone still renders. ----
    m_sample = WeatherSample{};
    m_sample.sky.enabled      = true;
    m_sample.sky.sunColor[0]  = 1.00f * tint[0];
    m_sample.sky.sunColor[1]  = 0.97f * tint[1];
    m_sample.sky.sunColor[2]  = 0.92f * tint[2];
    m_sample.sky.sunIntensity = sint;
    m_sample.sky.haze         = haze;
    m_sample.sky.exposure     = expo;
    m_sample.sky.sunDir[0]    = 0.4f;     // host overrides from ToD; sensible default
    m_sample.sky.sunDir[1]    = 1.0f;
    m_sample.sky.sunDir[2]    = 0.3f;

    m_sample.ambient[0]   = amb[0];
    m_sample.ambient[1]   = amb[1];
    m_sample.ambient[2]   = amb[2];
    m_sample.fogDensity   = haze;
    m_sample.precipitation = precip;
    m_sample.hazardLevel  = hazard;
    m_sample.hazardous    = hazard > 0.0f;
    m_sample.state        = m_target;
    m_sample.fromState    = m_from;
    m_sample.transition   = u;
}

// ===========================================================================
// Headless self-test (--test-weather).
// ===========================================================================
namespace {

int w_pass = 0, w_fail = 0;
void wcheck(bool cond, const char* name) {
    if (cond) { ++w_pass; x3::logInfo(std::string("[weather-test] PASS ") + name); }
    else      { ++w_fail; x3::logError(std::string("[weather-test] FAIL ") + name); }
}

} // namespace

bool runWeatherSelfTest() {
    w_pass = w_fail = 0;

    // ---- W0: a fresh system is settled on Clear (non-hazardous). ----
    {
        Weather w;
        WeatherSample s = w.sample();
        wcheck(s.state == WeatherState::Clear && !s.hazardous && s.transition >= 1.0f,
               "W0 fresh weather settled on Clear, non-hazardous");
    }

    // ---- W1: biome gating table — sandstorm only desert, snow only snow,
    // rain not in desert, clear/cloudy everywhere. ----
    {
        bool ok =
            weatherAllowedInBiome(WeatherState::Sandstorm, Biome::Desert) &&
            !weatherAllowedInBiome(WeatherState::Sandstorm, Biome::Temperate) &&
            !weatherAllowedInBiome(WeatherState::Sandstorm, Biome::Snow) &&
            weatherAllowedInBiome(WeatherState::Snow, Biome::Snow) &&
            !weatherAllowedInBiome(WeatherState::Snow, Biome::Desert) &&
            !weatherAllowedInBiome(WeatherState::Rain, Biome::Desert) &&
            weatherAllowedInBiome(WeatherState::Rain, Biome::Temperate) &&
            weatherAllowedInBiome(WeatherState::Clear, Biome::Desert) &&
            weatherAllowedInBiome(WeatherState::Cloudy, Biome::Snow);
        wcheck(ok, "W1 biome gating: sandstorm=desert, snow=snow, rain!=desert, clear/cloudy universal");
    }

    // ---- W2: forceState respects biome gating — illegal states are rejected. ----
    {
        Weather w; w.setBiome(Biome::Desert);
        bool rejected = !w.forceState(WeatherState::Snow) && !w.forceState(WeatherState::Rain);
        bool accepted = w.forceState(WeatherState::Sandstorm);
        wcheck(rejected && accepted,
               "W2 forceState rejects biome-illegal states, accepts legal ones");
    }

    // ---- W3: a forced (non-instant) transition INTERPOLATES the params over
    // transitionSeconds — haze ramps monotonically from the outgoing to incoming
    // look, and the transition factor goes 0 -> 1. ----
    {
        WeatherConfig cfg{}; cfg.transitionSeconds = 30.0f; cfg.autoSchedule = false;
        Weather w(cfg);                       // starts Clear (haze ~0.30)
        w.forceState(WeatherState::Fog);      // -> Fog (haze ~0.92)
        float h0 = w.sample().fogDensity;
        float prevHaze = h0, prevTrans = w.sample().transition;
        bool mono = true;
        const float dt = 1.0f;
        for (int i = 0; i < 30; ++i) {
            w.tick(dt);
            WeatherSample s = w.sample();
            if (s.fogDensity < prevHaze - 1e-5f) mono = false;        // haze should rise toward fog
            if (s.transition < prevTrans - 1e-5f) mono = false;       // factor should rise
            prevHaze = s.fogDensity; prevTrans = s.transition;
        }
        WeatherSample fin = w.sample();
        wcheck(mono, "W3 transition interpolates: fog haze + transition factor rise monotonically");
        wcheck(fin.state == WeatherState::Fog && fin.transition >= 1.0f &&
               std::fabs(fin.fogDensity - kLooks[(int)WeatherState::Fog].haze) < 0.02f,
               "W3b transition settles on Fog at full strength");
        wcheck(h0 < fin.fogDensity, "W3c fog density ended higher than it started (clear->fog)");
    }

    // ---- W4: hazard flag set ONLY in hazardous states (sandstorm/storm/snow,
    // swamp-fog poison) and NOT in benign states (clear/cloudy/rain/plain fog). ----
    {
        // Sandstorm in desert: hazardous.
        Weather d({ 30.0f, 25.0f, 75.0f, false, 1 });
        d.setBiome(Biome::Desert);
        d.forceState(WeatherState::Sandstorm, /*instant*/ true);
        wcheck(d.hazardous() && d.hazardLevel() > 0.0f, "W4 sandstorm (desert) is hazardous");

        // Clear/Cloudy/Rain: benign.
        Weather t({ 30.0f, 25.0f, 75.0f, false, 1 });
        t.forceState(WeatherState::Clear, true);
        bool clearOk = !t.hazardous();
        t.forceState(WeatherState::Cloudy, true);
        bool cloudyOk = !t.hazardous();
        t.forceState(WeatherState::Rain, true);
        bool rainOk = !t.hazardous();
        wcheck(clearOk && cloudyOk && rainOk, "W5 clear/cloudy/rain are NOT hazardous");

        // Storm: hazardous (lightning).
        t.forceState(WeatherState::Storm, true);
        wcheck(t.hazardous(), "W6 storm is hazardous");

        // Fog: benign in temperate, but POISON-hazardous in swamp.
        Weather f({ 30.0f, 25.0f, 75.0f, false, 1 });
        f.setBiome(Biome::Temperate);
        f.forceState(WeatherState::Fog, true);
        bool benignFog = !f.hazardous();
        f.setBiome(Biome::Swamp);
        f.forceState(WeatherState::Fog, true);
        bool poisonFog = f.hazardous() && f.hazardLevel() > 0.0f;
        wcheck(benignFog && poisonFog,
               "W7 fog benign in temperate, poison-hazardous in swamp");

        // Snow: hazardous (blizzard/cold).
        Weather sn({ 30.0f, 25.0f, 75.0f, false, 1 });
        sn.setBiome(Biome::Snow);
        sn.forceState(WeatherState::Snow, true);
        wcheck(sn.hazardous(), "W8 snow (blizzard) is hazardous");
    }

    // ---- W9: hazard flips at the transition MIDPOINT, not the start, when
    // transitioning OUT of a hazardous state into a benign one. ----
    {
        WeatherConfig cfg{}; cfg.transitionSeconds = 30.0f; cfg.autoSchedule = false;
        Weather w(cfg); w.setBiome(Biome::Desert);
        w.forceState(WeatherState::Sandstorm, true);   // hazardous, settled
        wcheck(w.hazardous(), "W9a sandstorm settled hazardous");
        w.forceState(WeatherState::Clear);             // begin clearing (30 s)
        w.tick(5.0f);                                   // ~17% in: still dominated by sandstorm
        bool stillHaz = w.hazardous();
        w.tick(20.0f);                                  // ~83% in: now past midpoint -> clear
        bool nowSafe = !w.hazardous();
        wcheck(stillHaz && nowSafe,
               "W9 hazard persists past transition start, clears after midpoint");
    }

    // ---- W10: setBiome forces a legal state when the current one becomes illegal
    // (cross-region behavior). ----
    {
        WeatherConfig cfg{}; cfg.autoSchedule = false;
        Weather w(cfg); w.setBiome(Biome::Desert);
        w.forceState(WeatherState::Sandstorm, true);
        // Walk into the snow biome: sandstorm is illegal there -> must leave it.
        w.setBiome(Biome::Snow);
        // Settle the forced transition.
        for (int i = 0; i < 35; ++i) w.tick(1.0f);
        WeatherState st = w.state();
        wcheck(weatherAllowedInBiome(st, Biome::Snow) && st != WeatherState::Sandstorm,
               "W10 crossing into a new biome leaves an illegal state for a legal one");
    }

    // ---- W11: auto-schedule advances through MULTIPLE distinct states over time,
    // all biome-legal, with finite holds (it does not freeze). ----
    {
        WeatherConfig cfg{}; cfg.transitionSeconds = 10.0f; cfg.minHoldSeconds = 20.0f;
        cfg.maxHoldSeconds = 40.0f; cfg.autoSchedule = true; cfg.seed = 12345;
        Weather w(cfg); w.setBiome(Biome::Temperate);
        bool seen[(int)WeatherState::Count] = {};
        int distinct = 0; WeatherState last = w.state();
        seen[(int)last] = true; distinct = 1;
        bool allLegal = true;
        const float dt = 1.0f;
        for (int i = 0; i < 1200; ++i) {     // 20 minutes
            w.tick(dt);
            WeatherState s = w.state();
            if (!weatherAllowedInBiome(s, Biome::Temperate)) allLegal = false;
            if (s != last) { last = s; if (!seen[(int)s]) { seen[(int)s] = true; ++distinct; } }
        }
        wcheck(distinct >= 3 && allLegal,
               "W11 auto-schedule cycles through multiple biome-legal states over time");
    }

    // ---- W12: DETERMINISTIC — two systems with the same seed + biome + dt
    // sequence produce identical state timelines + samples. ----
    {
        WeatherConfig cfg{}; cfg.autoSchedule = true; cfg.seed = 0xABCDEF12;
        Weather a(cfg), b(cfg);
        a.setBiome(Biome::Temperate); b.setBiome(Biome::Temperate);
        bool same = true;
        const float dt = 0.5f;
        for (int i = 0; i < 4000; ++i) {
            a.tick(dt); b.tick(dt);
            WeatherSample sa = a.sample(), sb = b.sample();
            if (sa.state != sb.state || sa.fromState != sb.fromState ||
                sa.fogDensity != sb.fogDensity || sa.hazardous != sb.hazardous ||
                sa.transition != sb.transition) { same = false; break; }
        }
        wcheck(same, "W12 deterministic: same seed+biome+dt -> identical timeline");
    }

    x3::logInfo(std::string("weather: ") + std::to_string(w_pass) + "/" +
                std::to_string(w_pass + w_fail) + " passed");
    return w_fail == 0;
}

} // namespace x3::game
