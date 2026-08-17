#pragma once
// EFLZ Time-of-Day cycle (X3_WORLD_BLUEPRINT §3 "Time-of-day" / x3-tod.js).
//
// A configurable day-length cycle that drives the engine's EXISTING analytic
// sky/sun via IRenderDevice::SkyParams (sun DIRECTION + COLOR + INTENSITY +
// horizon haze + exposure) plus an AMBIENT term the host can feed to its fill
// lighting. NO new renderer tech: the only output is a populated SkyParams
// snapshot (+ an ambient color) the host passes straight to setSkyParams().
//
// The cycle has 4 named phases — DAWN -> DAY -> DUSK -> NIGHT — over a single
// wrap-around day. Phase boundaries are configurable fractions of the day. The
// sun rides a smooth arc: it climbs from the eastern horizon at dawn, peaks
// overhead at midday, sinks west at dusk, and dips below the horizon at night
// (where a faint "moon" fill replaces it). Color/intensity/haze are smoothly
// interpolated between per-phase keyframes so there are no visible pops.
//
// DETERMINISM: state is a single normalized clock `t in [0,1)`. advance(dt)
// adds dt/dayLength and wraps; the same accumulated time always yields the same
// SkyParams. No RNG, no per-frame heap allocation, no global/static mutable
// state (everything lives in the instance). Cheap: a handful of lerps per
// sample, evaluated on demand (the host samples once per frame).
//
// Optional extras (blueprint §3): city-light enable at night (a bool the host
// can gate streetlights on) + a simple aurora tint that swells near midnight
// (an additive sky-color nudge the host may add to its ambient/sky).

#include "engine/rhi/IRenderDevice.h"

#include <cstdint>

namespace x3::game {

// The four canonical phases of the day. Order matches their progression across
// the normalized clock t in [0,1): DAWN -> DAY -> DUSK -> NIGHT -> (wrap) DAWN.
enum class TodPhase : uint32_t {
    Dawn = 0,   // sun rising, warm low-angle light, thinning haze
    Day  = 1,   // sun high, neutral bright light, clear
    Dusk = 2,   // sun setting, warm low-angle light, thickening haze
    Night = 3,  // sun below horizon, dim cool moonlight, city lights on
    Count = 4,
};

const char* todPhaseName(TodPhase p);

// Tunables for the cycle. Defaults match the blueprint's "4-phase 6-min cycle".
// Phase boundaries are fractions of the day in [0,1), strictly increasing; the
// day is [dawnStart, dayStart) = dawn, [dayStart, duskStart) = day, etc., and
// night wraps from nightStart back around to dawnStart.
struct TodConfig {
    float dayLengthSeconds = 360.0f;  // full day-night cycle length (6 min default)

    // Phase-start fractions of the day (must be 0 <= dawn < day < dusk < night < 1).
    float dawnStart  = 0.00f;   // dawn begins at t=0 (clock origin = first light)
    float dayStart   = 0.25f;   // full daytime
    float duskStart  = 0.55f;   // sun starts setting
    float nightStart = 0.75f;   // sun below horizon

    // The sun's horizontal travel: azimuth (radians) it sweeps east->west across
    // the day. The arc is a smooth half-dome; at night the sun direction points
    // below the horizon (negative Y) so the host's sun contributes nothing and
    // the moon fill takes over.
    float sunAzimuthEast = -1.20f;  // azimuth at first light (radians, around +Y)
    float sunAzimuthWest =  1.20f;  // azimuth at last light
    float middayElevation = 1.0f;   // sin of peak sun elevation (1 = straight up-ish)

    bool  enableCityLights = true;  // expose cityLightsOn() true during night
    bool  enableAurora     = true;  // additive aurora tint swelling near midnight

    // W-NIGHT: wall-clock anchoring for hosts that keep a 0..24 h clock (the
    // tunnel world's todHours / wx_hour). sampleAtHours(h) maps hour ->
    // dayFraction as wrap01((h - sunriseHour) / 24): the fraction clock's t=0
    // (first light) lands at this hour. Defaults to 6 AM.
    float sunriseHour = 6.0f;
};

// A sampled snapshot of the sky/lighting at a given clock value. POD; the host
// reads `sky` straight into setSkyParams() and `ambient*` into its fill rig.
struct TodSample {
    x3::rhi::IRenderDevice::SkyParams sky{};   // ready for setSkyParams()
    float    ambient[3] = { 0, 0, 0 };         // linear-RGB ambient/fill suggestion
    float    sunElevation = 0.0f;              // sin(elev): >0 above horizon, <0 below
    bool     cityLightsOn = false;             // night city-light gate
    float    auroraTint[3] = { 0, 0, 0 };      // additive aurora color (0 by day)
    TodPhase phase = TodPhase::Day;            // active phase at this clock
    float    dayFraction = 0.0f;               // the normalized clock t in [0,1)
    // W-NIGHT additions. `night` is the smooth 0..1 lamp dial (0 in daylight,
    // ramping through civil twilight to 1 once the sun is well down) — feed it
    // to Town::setNight, headlight auto-on, window glows. When the sun drops
    // below the horizon the sample swings sky.sunDir onto the MOON (opposite
    // arc, above the horizon at night), sets sky.moon so the sky draws a moon
    // disc there, and hands lighting a dim cool moon key via sky.sunLight —
    // enough to see the road faintly, never enough to read as daylight.
    float    night = 0.0f;
};

// The Time-of-Day cycle. Deterministic: identical accumulated time -> identical
// sample. No heap allocation anywhere; safe to embed by value.
class TimeOfDay {
public:
    TimeOfDay() = default;
    explicit TimeOfDay(const TodConfig& cfg) : m_cfg(cfg) {}

    void  configure(const TodConfig& cfg) { m_cfg = cfg; }
    const TodConfig& config() const { return m_cfg; }

    // Set the clock directly to a fraction of the day in [0,1) (wraps). Useful for
    // deterministic tests + scripted "set to noon" beats.
    void  setDayFraction(float t);
    float dayFraction() const { return m_t; }

    // Advance the clock by `dt` real seconds (scaled by dayLength + wrapped).
    // `dt` may be any non-negative value (large dt still wraps correctly).
    void  advance(float dt);

    // The active phase at the current clock.
    TodPhase phase() const { return phaseAt(m_t); }

    // Sample the sky/lighting at the current clock (no allocation).
    TodSample sample() const { return sampleAt(m_t); }

    // Pure sampler at an arbitrary clock value (clamped/wrapped to [0,1)). The
    // engine's existing default sun (normalize(0.4,1,0.3), warm white) is what a
    // sample near midday reproduces, so turning ToD on at noon matches the
    // pre-ToD look.
    TodSample sampleAt(float t) const;

    // Wall-clock sampler (W-NIGHT): hours in [0,24) (wraps), anchored so first
    // light falls at cfg.sunriseHour. The tunnel world's todHours/wx_hour clock
    // feeds this directly.
    TodSample sampleAtHours(float hours) const;

    // Which phase a given clock fraction falls in.
    TodPhase phaseAt(float t) const;

private:
    TodConfig m_cfg{};
    float     m_t = 0.0f;   // normalized clock in [0,1)
};

// Headless self-test (--test-tod). Returns true iff all checks pass; prints
// "tod: X/Y passed".
bool runTodSelfTest();

} // namespace x3::game
