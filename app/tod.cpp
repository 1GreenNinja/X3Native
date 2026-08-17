// EFLZ Time-of-Day cycle — implementation. See tod.h for the contract.
//
// The day is modelled as a normalized clock t in [0,1). We derive two things
// from t:
//   * a SUN ARC — elevation peaks at midday and dips below the horizon at
//     night, azimuth sweeps east->west — so the sun DIRECTION animates;
//   * a set of per-keyframe SKY LOOKS (color/intensity/haze/exposure/ambient)
//     blended across the phases, so COLOR + INTENSITY + ambient animate.
// Everything is closed-form (lerps + a sine arc): deterministic + alloc-free.

#include "tod.h"
#include "engine/core/x3_log.h"

#include <cmath>
#include <string>

namespace x3::game {

namespace {

constexpr float kPi = 3.14159265358979323846f;

inline float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
inline float lerp(float a, float b, float u) { return a + (b - a) * u; }
inline void  lerp3(const float a[3], const float b[3], float u, float out[3]) {
    out[0] = lerp(a[0], b[0], u);
    out[1] = lerp(a[1], b[1], u);
    out[2] = lerp(a[2], b[2], u);
}
// Smoothstep easing so phase transitions have no velocity discontinuity.
inline float smooth(float u) { u = clamp01(u); return u * u * (3.0f - 2.0f * u); }

// Wrap a value into [0,1).
inline float wrap01(float t) {
    t -= std::floor(t);
    if (t < 0.0f) t += 1.0f;
    if (t >= 1.0f) t -= 1.0f;   // guard fp edge
    return t;
}

// A "sky look": the color/intensity/haze/exposure + ambient at a phase anchor.
// W-NIGHT growth: each look now also carries the sky DOME palette (zenith +
// horizon — the lane the analytic sky's whole brightness actually lives in;
// without it "night" was a dimmer sun under the same daylight-blue dome and
// the stars could never gate on) and the directional KEY scale (sunLight, the
// mesh.frag kSunColor multiplier — the half of darkness the ground obeys).
struct SkyLook {
    float sunColor[3];
    float sunIntensity;
    float haze;
    float exposure;
    float ambient[3];
    float zenith[3];
    float horizon[3];
    float sunLight;
};

// Keyframes anchored at each phase (and a wrap anchor == dawn). Tuned so DAY
// reproduces the engine's existing warm-white sun look. Linear RGB. Dawn/dusk
// are warm + low; day is bright neutral; night is dim + cool/blue.
// Zenith/horizon day values == SkyParams' own defaults, so a midday sample is
// the old global sky exactly. Night values are NEAR-BLACK with a whisper of
// blue: the sky shader's star gate keys on rendered sky luminance, so THIS is
// what actually switches the stars on; the phantom-daylight IBL ground fix in
// ibl_env.frag keys on the same palette.
constexpr SkyLook kLookDawn = {
    { 1.00f, 0.62f, 0.40f },  // warm orange sunrise
    0.55f, 0.70f, 1.05f,
    { 0.10f, 0.07f, 0.09f },
    { 0.050f, 0.095f, 0.280f }, { 0.940f, 0.520f, 0.300f },
    0.55f,
};
constexpr SkyLook kLookDay = {
    { 1.00f, 0.97f, 0.92f },  // engine default warm white
    1.00f, 0.40f, 1.00f,
    { 0.16f, 0.17f, 0.20f },
    { 0.10f, 0.28f, 0.66f }, { 0.62f, 0.74f, 0.92f },   // == SkyParams defaults
    1.00f,
};
constexpr SkyLook kLookDusk = {
    { 1.00f, 0.55f, 0.34f },  // deeper orange/red sunset
    0.55f, 0.72f, 1.05f,
    { 0.10f, 0.07f, 0.08f },
    { 0.042f, 0.070f, 0.210f }, { 1.000f, 0.440f, 0.190f },
    0.55f,
};
constexpr SkyLook kLookNight = {
    { 0.62f, 0.70f, 0.88f },  // the MOON's pale disc color (sunDir is the moon at night)
    0.30f,                    // disc/halo intensity — a moon, not a floodlight
    // haze 0.52, not the old 0.30: the sky shader treats haze < 0.5 as "shading
    // toward deep space" and starts drawing stars BELOW the horizon; a ground
    // world's night keeps its aerosol so the starfield stays celestial.
    0.52f, 1.12f,
    { 0.024f, 0.028f, 0.046f },
    { 0.0016f, 0.0024f, 0.0060f }, { 0.0070f, 0.0100f, 0.0200f },
    0.05f,                    // moonlight key: see the road faintly, nothing more
};

void lerpLook(const SkyLook& a, const SkyLook& b, float u, SkyLook& out) {
    lerp3(a.sunColor, b.sunColor, u, out.sunColor);
    out.sunIntensity = lerp(a.sunIntensity, b.sunIntensity, u);
    out.haze         = lerp(a.haze, b.haze, u);
    out.exposure     = lerp(a.exposure, b.exposure, u);
    lerp3(a.ambient, b.ambient, u, out.ambient);
    lerp3(a.zenith,  b.zenith,  u, out.zenith);
    lerp3(a.horizon, b.horizon, u, out.horizon);
    out.sunLight = lerp(a.sunLight, b.sunLight, u);
}

} // namespace

const char* todPhaseName(TodPhase p) {
    switch (p) {
        case TodPhase::Dawn:  return "dawn";
        case TodPhase::Day:   return "day";
        case TodPhase::Dusk:  return "dusk";
        case TodPhase::Night: return "night";
        default:              return "?";
    }
}

void TimeOfDay::setDayFraction(float t) { m_t = wrap01(t); }

void TimeOfDay::advance(float dt) {
    if (dt <= 0.0f || m_cfg.dayLengthSeconds <= 0.0f) return;
    m_t = wrap01(m_t + dt / m_cfg.dayLengthSeconds);
}

TodPhase TimeOfDay::phaseAt(float t) const {
    t = wrap01(t);
    if (t >= m_cfg.nightStart || t < m_cfg.dawnStart) return TodPhase::Night;
    if (t < m_cfg.dayStart)  return TodPhase::Dawn;
    if (t < m_cfg.duskStart) return TodPhase::Day;
    return TodPhase::Dusk;
}

TodSample TimeOfDay::sampleAt(float t) const {
    t = wrap01(t);
    TodSample s{};
    s.dayFraction = t;
    s.phase = phaseAt(t);

    // ---- Compute the blended SkyLook for the active phase. The "night" phase
    // wraps across t=1 -> t=0 back to dawn, so it is handled specially; the other
    // three are a straight smooth-blend from their start anchor to the next. ----
    SkyLook look = kLookDay;
    switch (s.phase) {
        case TodPhase::Dawn: {
            float span = m_cfg.dayStart - m_cfg.dawnStart;
            float u = span > 0.0f ? (t - m_cfg.dawnStart) / span : 0.0f;
            lerpLook(kLookDawn, kLookDay, smooth(u), look);
        } break;
        case TodPhase::Day: {
            // Hold neutral daylight across midday (a gentle self-blend keeps it flat).
            look = kLookDay;
        } break;
        case TodPhase::Dusk: {
            float span = m_cfg.nightStart - m_cfg.duskStart;
            float u = span > 0.0f ? (t - m_cfg.duskStart) / span : 0.0f;
            lerpLook(kLookDay, kLookDusk, smooth(u), look);
        } break;
        case TodPhase::Night:
        default: {
            // Night spans [nightStart, 1) U [0, dawnStart). A wrapped u runs
            // dusk-look -> night-look (first half) -> dawn-look (second half).
            float start = m_cfg.nightStart;
            float end   = m_cfg.dawnStart + 1.0f;   // unwrap across the seam
            float tt    = (t < m_cfg.dawnStart) ? t + 1.0f : t;
            float span  = end - start;
            float u     = span > 0.0f ? (tt - start) / span : 0.0f;
            if (u < 0.5f) lerpLook(kLookDusk,  kLookNight, smooth(u * 2.0f),         look);
            else          lerpLook(kLookNight, kLookDawn,  smooth((u - 0.5f) * 2.0f), look);
        } break;
    }

    // ---- THE SUN ARC, computed FIRST (W-NIGHT). ------------------------------
    // It used to be computed after the look was already written into s.sky,
    // which forced the phase table to be the ONLY thing deciding how dark the
    // sky is. That is a clock, and darkness is a fact about the SUN.
    //
    // THE RECEIPT: with the tunnel world's config, 22:00 lands at t=0.677. The
    // Night phase spans [0.604, 1.0) and its look only reaches kLookNight at
    // its MIDPOINT — so the blend at 22:00 was 69% DUSK, and the dome rendered
    // a warm cream sunset horizon (0.695, 0.308, 0.132) with the sun 28 DEGREES
    // BELOW THE HORIZON. Full dark did not arrive until ~01:00. The eye gate
    // caught it on the first night capture: bright sunset sky, no stars, and
    // a campfire invisible against it.
    float elev = 0.0f;
    float sunDirRaw[3] = { 0.0f, 1.0f, 0.0f };
    {
        float dayStart = m_cfg.dawnStart;
        float dayEnd   = m_cfg.nightStart;
        float daySpan  = dayEnd - dayStart;
        if (daySpan <= 0.0f) daySpan = 1.0f;

        float p;
        if (t >= dayStart && t <= dayEnd) {
            p = (t - dayStart) / daySpan;                  // 0..1 across the day
        } else {
            float tt = (t < dayStart) ? t + 1.0f : t;
            float nightSpan = (dayStart + 1.0f) - dayEnd;
            if (nightSpan <= 0.0f) nightSpan = 1.0f;
            p = 1.0f + (tt - dayEnd) / nightSpan;          // 1..2 across the night
        }

        float phi  = p * kPi;                              // 0..pi day, pi..2pi night
        elev       = std::sin(phi) * m_cfg.middayElevation;// + above horizon, - below
        float az   = lerp(m_cfg.sunAzimuthEast, m_cfg.sunAzimuthWest, clamp01(p));

        // Direction TOWARD the sun (engine convention; normalized internally).
        // `elev` IS the y component (a sine), so the horizontal magnitude must be
        // sqrt(1 - y^2). The old cos(|elev| * pi/2) treated a sine as an angle
        // fraction and UNDER-stated the horizontal term at every elevation (at
        // elev 0.99 by 12x), pinning midday to the zenith: with no horizontal sun
        // component every vertical facade gets N.L ~ 0 (flat, ambient-only towers)
        // and every cast shadow falls straight down inside its own footprint.
        float cosE = std::sqrt(std::max(0.0f, 1.0f - elev * elev));
        sunDirRaw[0] = std::sin(az) * cosE;
        sunDirRaw[1] = elev;
        sunDirRaw[2] = std::cos(az) * cosE * 0.6f + 0.2f;
    }

    // ---- TWILIGHT COLLAPSE: the whole look falls to NIGHT as the sun sinks. --
    // Keyed on real twilight geometry, not on the clock. elev is sin(altitude),
    // so the classical bands are: civil = 0 .. -0.105 (-6 deg), nautical
    // -0.208 (-12 deg), astronomical -0.309 (-18 deg) = full dark. The ramp
    // starts a hair below the horizon (-0.015) and is complete by -0.275
    // (~-16 deg), which puts the tunnel world's 22:00 (elev -0.482) at FULL
    // night and keeps a real, gradual dusk between.
    // Above the horizon darkK is 0, so every daytime sample — and every
    // existing calibrated daytime capture — is untouched.
    {
        const float darkK = smooth(clamp01((-elev - 0.015f) / 0.260f));
        if (darkK > 0.0f) {
            SkyLook nl;
            lerpLook(look, kLookNight, darkK, nl);
            look = nl;
        }
    }

    s.sky.enabled      = true;
    s.sky.sunColor[0]  = look.sunColor[0];
    s.sky.sunColor[1]  = look.sunColor[1];
    s.sky.sunColor[2]  = look.sunColor[2];
    s.sky.sunIntensity = look.sunIntensity;
    s.sky.haze         = look.haze;
    s.sky.exposure     = look.exposure;
    s.sky.zenith[0]    = look.zenith[0];
    s.sky.zenith[1]    = look.zenith[1];
    s.sky.zenith[2]    = look.zenith[2];
    s.sky.horizon[0]   = look.horizon[0];
    s.sky.horizon[1]   = look.horizon[1];
    s.sky.horizon[2]   = look.horizon[2];
    s.sky.sunLight     = look.sunLight;
    s.ambient[0]       = look.ambient[0];
    s.ambient[1]       = look.ambient[1];
    s.ambient[2]       = look.ambient[2];

    // ---- The arc's results land here (computed above, before the collapse). --
    {
        s.sky.sunDir[0] = sunDirRaw[0];
        s.sky.sunDir[1] = sunDirRaw[1];
        s.sky.sunDir[2] = sunDirRaw[2];
        s.sunElevation  = elev;
        // DUSTY-DAY FIX (Tim: "blue sky!") — placed AFTER sunElevation is set
        // (the first draft read it before assignment): a HIGH sun burns the
        // analytic haze off, so noon reads blue instead of milk; golden/low
        // hours keep the authored haze where the atmosphere belongs.
        if (elev > 0.30f)
            s.sky.haze *= clamp01(1.0f - (elev - 0.30f) / 0.42f * 0.65f);

        // ---- W-NIGHT: the horizon crossing, done honestly. ----------------
        // 1. The directional KEY dies WITH the sun, not with the phase table:
        //    full above elev 0.10, gone by -0.02 (civil-twilight fade). The
        //    look table's sunLight then only shapes the daytime curve.
        {
            float ku = clamp01((elev + 0.02f) / 0.12f);        // -0.02 .. 0.10
            s.sky.sunLight = look.sunLight * smooth(ku);
        }
        // 2. Once the sun is genuinely down, the MOON takes the luminary seat:
        //    sky.sunDir swings to the anti-solar point (above the horizon all
        //    night, riding the same arc), sky.moon tells the sky shader to draw
        //    a pale mottled disc + cool halo there instead of a sun, and a dim
        //    cool key (kLookNight.sunLight via the ramp) lights the world —
        //    lighting, shadows and the backdrop all agree the moon is the lamp.
        //    The swap happens inside the band where the key above is already
        //    ZERO from both sides, so the direction snap is invisible.
        if (elev < -0.03f) {
            float moonRamp = smooth(clamp01((-elev - 0.03f) / 0.10f));
            s.sky.sunDir[0] = -s.sky.sunDir[0];
            s.sky.sunDir[1] = -s.sky.sunDir[1];
            s.sky.sunDir[2] = -s.sky.sunDir[2];
            s.sky.moon      = moonRamp;
            s.sky.sunLight  = 0.05f * moonRamp;                // moonlight key
            // Disc color/intensity ease in with the ramp (the look table is
            // already the moon by deep night; early dusk holds its warm color
            // until the moon actually owns the sky).
            s.sky.sunIntensity = lerp(0.0f, look.sunIntensity > 0.30f ? look.sunIntensity : 0.30f, moonRamp);
        }
        // 3. The smooth lamp dial for towns/headlights/windows: 0 in daylight,
        //    1 once the sun is well down. (cityLightsOn keeps its legacy bool.)
        s.night = smooth(clamp01((0.06f - elev) / 0.14f));

        // City lights: on whenever the sun is below (or barely above) the horizon.
        s.cityLightsOn = m_cfg.enableCityLights && (elev < 0.08f);

        // Aurora: swells near solar midnight (deepest night), 0 by day. A faint
        // green/violet additive tint the host may fold into ambient/sky.
        if (m_cfg.enableAurora) {
            float night = clamp01(-elev / m_cfg.middayElevation);  // 0 at horizon -> 1 at midnight
            float a = night * night;                               // ease in
            s.auroraTint[0] = 0.04f * a;
            s.auroraTint[1] = 0.14f * a;
            s.auroraTint[2] = 0.10f * a;
        }
    }

    return s;
}

TodSample TimeOfDay::sampleAtHours(float hours) const {
    return sampleAt(wrap01((hours - m_cfg.sunriseHour) / 24.0f));
}

// ===========================================================================
// Headless self-test (--test-tod).
// ===========================================================================
namespace {

int t_pass = 0, t_fail = 0;
void tcheck(bool cond, const char* name) {
    if (cond) { ++t_pass; x3::logInfo(std::string("[tod-test] PASS ") + name); }
    else      { ++t_fail; x3::logError(std::string("[tod-test] FAIL ") + name); }
}

} // namespace

bool runTodSelfTest() {
    t_pass = t_fail = 0;

    TodConfig cfg{};                 // defaults: 360 s day, dawn/day/dusk/night @ .00/.25/.55/.75
    TimeOfDay tod(cfg);

    // ---- T0: a fresh cycle starts at first light (dawn) + day fraction 0. ----
    tcheck(tod.dayFraction() == 0.0f && tod.phase() == TodPhase::Dawn,
           "T0 cycle starts at dawn (t=0)");

    // ---- T1: advancing the clock visits ALL FOUR phases in order across one day,
    // and wraps back to dawn after a full day. ----
    {
        // Sample phase at the MIDPOINT of each phase span (robust to boundaries).
        TodPhase pDawn  = tod.phaseAt(0.5f * (cfg.dawnStart + cfg.dayStart));
        TodPhase pDay   = tod.phaseAt(0.5f * (cfg.dayStart  + cfg.duskStart));
        TodPhase pDusk  = tod.phaseAt(0.5f * (cfg.duskStart + cfg.nightStart));
        TodPhase pNight = tod.phaseAt(0.5f * (cfg.nightStart + 1.0f));
        tcheck(pDawn == TodPhase::Dawn && pDay == TodPhase::Day &&
               pDusk == TodPhase::Dusk && pNight == TodPhase::Night,
               "T1 all 4 phases (dawn/day/dusk/night) occur in clock order");

        // Walk the whole day in real-time steps + confirm every phase is observed.
        bool seen[(int)TodPhase::Count] = { false, false, false, false };
        TimeOfDay walk(cfg);
        const float dt = 1.0f;                       // 1 s steps
        const int steps = (int)cfg.dayLengthSeconds; // exactly one day
        for (int i = 0; i < steps; ++i) { seen[(int)walk.phase()] = true; walk.advance(dt); }
        tcheck(seen[0] && seen[1] && seen[2] && seen[3],
               "T2 a full real-time day advances through every phase");
        // The clock WRAPS: advancing by exactly one full day (in one call, no
        // accumulation drift) returns to the same fraction + phase; and advancing
        // 1.5 days from dusk lands in night again.
        TimeOfDay wrapA(cfg); wrapA.setDayFraction(0.40f);   // mid-day
        wrapA.advance(cfg.dayLengthSeconds);                  // exactly one day
        bool wrappedSame = std::fabs(wrapA.dayFraction() - 0.40f) < 1e-3f &&
                           wrapA.phase() == TodPhase::Day;
        // +1.5 days from dusk (0.56) -> +0.5 day -> 0.06 -> dawn region.
        TimeOfDay wrapB(cfg); wrapB.setDayFraction(cfg.duskStart + 0.01f); // dusk @ 0.56
        wrapB.advance(cfg.dayLengthSeconds * 1.5f);           // wraps to ~0.06
        bool wrappedPhase = wrapB.phase() == TodPhase::Dawn;
        tcheck(wrappedSame && wrappedPhase,
               "T3 the clock wraps: +1 day returns to the same phase, +1.5 days advances half a day");
    }

    // ---- T4: sun ELEVATION rises monotonically sunrise->midday, then falls
    // monotonically midday->sunset (a smooth arc). ----
    {
        const float sunrise = cfg.dawnStart;
        const float sunset  = cfg.nightStart;
        const float midday  = 0.5f * (sunrise + sunset);
        const int N = 24;
        bool risingOk = true, fallingOk = true;
        float prev = tod.sampleAt(sunrise).sunElevation;
        for (int i = 1; i <= N; ++i) {
            float tt = sunrise + (midday - sunrise) * (float)i / (float)N;
            float e = tod.sampleAt(tt).sunElevation;
            if (e <= prev) risingOk = false;
            prev = e;
        }
        prev = tod.sampleAt(midday).sunElevation;
        for (int i = 1; i <= N; ++i) {
            float tt = midday + (sunset - midday) * (float)i / (float)N;
            float e = tod.sampleAt(tt).sunElevation;
            if (e >= prev) fallingOk = false;
            prev = e;
        }
        tcheck(risingOk, "T4 sun elevation rises monotonically sunrise->midday");
        tcheck(fallingOk, "T5 sun elevation falls monotonically midday->sunset");

        // Sun is ABOVE the horizon at midday + BELOW at solar midnight.
        float eNoon = tod.sampleAt(midday).sunElevation;
        float eMidnight = tod.sampleAt(wrap01((sunset + sunrise + 1.0f) * 0.5f)).sunElevation;
        tcheck(eNoon > 0.5f && eMidnight < 0.0f,
               "T6 sun above horizon at midday, below at solar midnight");
    }

    // ---- T7: sun INTENSITY varies across the cycle: brightest by day, dimmest at
    // night (the cycle is NOT flat). ----
    {
        const float midday   = 0.5f * (cfg.dawnStart + cfg.nightStart);
        const float midnight = wrap01((cfg.nightStart + cfg.dawnStart + 1.0f) * 0.5f);
        float iDay   = tod.sampleAt(midday).sky.sunIntensity;
        float iNight = tod.sampleAt(midnight).sky.sunIntensity;
        float iDawn  = tod.sampleAt(0.5f * (cfg.dawnStart + cfg.dayStart)).sky.sunIntensity;
        tcheck(iDay > iDawn && iDawn > iNight,
               "T7 sun intensity: day > dawn/dusk > night (varies, not flat)");
    }

    // ---- T8: sun COLOR shifts warm at the horizon (dawn/dusk) and cool at night;
    // day is the engine's warm white. ----
    {
        TodSample dawn  = tod.sampleAt(0.5f * (cfg.dawnStart + cfg.dayStart));
        TodSample day   = tod.sampleAt(0.5f * (cfg.dayStart  + cfg.duskStart));
        TodSample night = tod.sampleAt(wrap01((cfg.nightStart + cfg.dawnStart + 1.0f) * 0.5f));
        // Dawn: red >> blue (warm). Night: blue > red (cool). Day: near-white.
        bool warmDawn = dawn.sky.sunColor[0] > dawn.sky.sunColor[2] + 0.3f;
        bool coolNight = night.sky.sunColor[2] > night.sky.sunColor[0];
        bool whiteDay = day.sky.sunColor[0] > 0.9f && day.sky.sunColor[1] > 0.9f && day.sky.sunColor[2] > 0.85f;
        tcheck(warmDawn && coolNight && whiteDay,
               "T8 sun color: warm at dawn, cool at night, warm-white by day");
    }

    // ---- T9: city lights + aurora gate on NIGHT only (off at midday). ----
    {
        const float midday   = 0.5f * (cfg.dawnStart + cfg.nightStart);
        const float midnight = wrap01((cfg.nightStart + cfg.dawnStart + 1.0f) * 0.5f);
        TodSample noon  = tod.sampleAt(midday);
        TodSample night = tod.sampleAt(midnight);
        bool lights = !noon.cityLightsOn && night.cityLightsOn;
        bool aurora = (noon.auroraTint[1] == 0.0f) && (night.auroraTint[1] > 0.0f);
        tcheck(lights, "T9 city lights off at midday, on at night");
        tcheck(aurora, "T10 aurora tint zero by day, present at solar midnight");
    }

    // ---- T11: DETERMINISTIC. Two cycles advanced by the SAME dt sequence produce
    // bit-identical samples (no RNG, no hidden state). ----
    {
        TimeOfDay a(cfg), b(cfg);
        const float dts[5] = { 0.7f, 13.3f, 0.016f, 90.0f, 250.0f };
        for (float dt : dts) { a.advance(dt); b.advance(dt); }
        TodSample sa = a.sample(), sb = b.sample();
        bool same = sa.dayFraction == sb.dayFraction &&
                    sa.sunElevation == sb.sunElevation &&
                    sa.sky.sunIntensity == sb.sky.sunIntensity &&
                    sa.sky.sunDir[0] == sb.sky.sunDir[0] &&
                    sa.sky.sunDir[1] == sb.sky.sunDir[1] &&
                    sa.sky.sunDir[2] == sb.sky.sunDir[2] &&
                    sa.phase == sb.phase;
        tcheck(same, "T11 deterministic: identical dt sequence -> identical sample");

        // setDayFraction is the inverse view: same fraction -> same sample.
        TimeOfDay c(cfg); c.setDayFraction(0.42f);
        TodSample s1 = c.sample(), s2 = tod.sampleAt(0.42f);
        tcheck(s1.sunElevation == s2.sunElevation && s1.sky.haze == s2.sky.haze,
               "T12 setDayFraction matches the pure sampler at the same clock");
    }

    // ---- T13: the sky snapshot is always renderer-ready (enabled) + the engine
    // default (midday warm-white sun pointing up) is reproduced near noon. ----
    {
        const float midday = 0.5f * (cfg.dawnStart + cfg.nightStart);
        TodSample noon = tod.sampleAt(midday);
        tcheck(noon.sky.enabled && noon.sky.sunDir[1] > 0.5f && noon.sky.haze >= 0.0f,
               "T13 sky snapshot enabled + sun overhead at midday (renderer-ready)");
    }

    x3::logInfo(std::string("tod: ") + std::to_string(t_pass) + "/" +
                std::to_string(t_pass + t_fail) + " passed");
    return t_fail == 0;
}

} // namespace x3::game
