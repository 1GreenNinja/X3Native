#pragma once
// EFLZ Weather system (X3_WORLD_BLUEPRINT §3 "Weather" / x3-weather.js).
//
// Seven weather STATES — clear / cloudy / rain / storm / fog / sandstorm / snow
// — with smooth TIMED transitions, BIOME-GATED so only weather appropriate to a
// region can occur (e.g. sandstorm only in the desert, snow only in the snow
// mountains, poison-laden fog in the swamp). Each state nudges the EXISTING sky
// + "fog" params (the analytic sky's horizon haze + exposure + sun color/
// intensity) and a host-fed AMBIENT term, and exposes a HAZARD flag a HazardZone
// can read (the desert sandstorm + swamp poison reference hazards — this is the
// clean param). NO new renderer tech: output is a populated SkyParams snapshot
// (+ ambient + hazard fields) the host drives into setSkyParams / its fill rig.
//
// DETERMINISM: the scheduler is driven by a 64-bit LCG seeded by the caller, so
// a given seed + biome + dt sequence always produces the same state timeline.
// Pass autoSchedule=false (or force a state) for fully scripted control. No RNG
// is consulted DURING a transition, only at the moment a new state is chosen.
// Cheap + alloc-free: a couple of lerps per tick, fixed-size state.
//
// TRANSITIONS interpolate the sky/fog/ambient params between the OUTGOING and
// INCOMING state looks over a configurable transition time (default 30 s,
// matching the blueprint), with smoothstep easing — no visible pops. The hazard
// flag flips at the transition MIDPOINT so a clearing sandstorm stops being
// hazardous as it visibly thins (and vice-versa).

#include "engine/rhi/IRenderDevice.h"

#include <cstdint>

namespace x3::game {

// The seven weather states. Order is stable (used as table indices).
enum class WeatherState : uint32_t {
    Clear     = 0,
    Cloudy    = 1,
    Rain      = 2,
    Storm     = 3,
    Fog       = 4,
    Sandstorm = 5,
    Snow      = 6,
    Count     = 7,
};

const char* weatherStateName(WeatherState s);

// The biome a region sits in. The caller sets this PER REGION (the open world
// hands the active biome to the weather system as the player crosses regions).
// Each biome admits a different subset of weather states (biome gating).
enum class Biome : uint32_t {
    Temperate = 0,   // grass/city: clear/cloudy/rain/storm/fog
    Desert    = 1,   // sand: clear/cloudy/sandstorm  (NO rain/snow)
    Swamp     = 2,   // toxic: cloudy/rain/fog/storm  (fog = poison hazard)
    Snow      = 3,   // mountains: clear/cloudy/snow/fog (NO rain/sandstorm)
    Count     = 4,
};

const char* biomeName(Biome b);

// True iff `state` can occur in `biome` (the gating table). Clear + Cloudy are
// universal; the rest are biome-specific.
bool weatherAllowedInBiome(WeatherState state, Biome biome);

// Tunables. Defaults match the blueprint's "7 states, 30 s transitions".
struct WeatherConfig {
    float transitionSeconds = 30.0f;  // cross-fade time between states
    float minHoldSeconds    = 25.0f;  // minimum time a state holds before re-rolling
    float maxHoldSeconds    = 75.0f;  // maximum hold before a forced re-roll
    bool  autoSchedule      = true;   // auto-pick the next state (false = scripted only)
    uint64_t seed           = 0x9E3779B97F4A7C15ull; // LCG seed (deterministic)
};

// A sampled snapshot the host consumes each frame. POD.
struct WeatherSample {
    x3::rhi::IRenderDevice::SkyParams sky{};  // ready for setSkyParams() (haze == "fog")
    float        ambient[3]   = { 0, 0, 0 };  // linear-RGB ambient/fill nudge
    float        fogDensity   = 0.0f;         // 0..1 normalized fog/haze strength (== sky.haze)
    bool         hazardous    = false;        // HazardZone reads this (sandstorm/poison-fog/storm/blizzard)
    float        hazardLevel  = 0.0f;         // 0..1 hazard intensity (0 when not hazardous)
    float        precipitation = 0.0f;        // 0..1 rain/snow intensity (host particle hint)
    WeatherState state        = WeatherState::Clear;  // the dominant (incoming) state
    WeatherState fromState    = WeatherState::Clear;  // outgoing state during a transition
    float        transition   = 1.0f;         // 0 = just started transition, 1 = settled
};

// The weather system for one active region. Set the biome, tick(dt) each frame;
// it auto-schedules biome-legal states (or obeys forceState) with smooth
// transitions and exposes the current sample(). Deterministic for a fixed seed +
// biome + dt sequence. Embed by value; no heap allocation.
class Weather {
public:
    Weather() { reset(); }
    explicit Weather(const WeatherConfig& cfg) : m_cfg(cfg) { reset(); }

    void configure(const WeatherConfig& cfg) { m_cfg = cfg; reset(); }
    const WeatherConfig& config() const { return m_cfg; }

    // Reset to a settled Clear state and re-seed the scheduler.
    void reset();

    // Set the active biome. If the CURRENT/target state is illegal in the new
    // biome, an immediate transition to a legal state (Clear if nothing else) is
    // begun. Crossing regions is the intended trigger.
    void setBiome(Biome b);
    Biome biome() const { return m_biome; }

    // Force a transition to `state` (ignores the schedule timer; respects biome
    // gating — an illegal state is rejected and false returned). Begins a smooth
    // transition unless `instant` is true.
    bool forceState(WeatherState state, bool instant = false);

    // Advance the scheduler + any in-progress transition by `dt` seconds.
    void tick(float dt);

    // The current blended snapshot (alloc-free).
    WeatherSample sample() const { return m_sample; }

    // Convenience accessors mirroring the sample.
    WeatherState state() const { return m_target; }
    WeatherState fromState() const { return m_from; }
    bool  inTransition() const { return m_transT < m_cfg.transitionSeconds; }
    bool  hazardous() const { return m_sample.hazardous; }
    float hazardLevel() const { return m_sample.hazardLevel; }

private:
    void  beginTransition(WeatherState to, bool instant);
    void  rebuildSample();
    WeatherState rollNextState();      // pick a biome-legal next state (deterministic)
    uint32_t     nextRand();           // LCG step

    WeatherConfig m_cfg{};
    Biome         m_biome   = Biome::Temperate;

    WeatherState  m_from    = WeatherState::Clear;  // outgoing
    WeatherState  m_target  = WeatherState::Clear;  // incoming/current
    float         m_transT  = 1e9f;   // seconds INTO the current transition (>= transitionSeconds == settled)
    float         m_holdT   = 0.0f;   // seconds the current state has been settled
    float         m_holdFor = 0.0f;   // scheduled hold duration before re-roll

    uint64_t      m_rng     = 0;      // LCG state

    WeatherSample m_sample{};
};

// Headless self-test (--test-weather). Returns true iff all checks pass; prints
// "weather: X/Y passed".
bool runWeatherSelfTest();

} // namespace x3::game
