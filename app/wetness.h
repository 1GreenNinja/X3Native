#pragma once
// ============================================================================
// wetness — SURFACE WETNESS + ICE, and what they do to tire grip.
//                                                        [LANE: inspx/wetness]
//
// This lane owns this file. It is the CPU half of the rain work; the GPU half
// is shaders/inc/mesh_wetness.glsl. Nothing else in the app models wetness.
//
// WHY A MODEL AND NOT JUST `precipitation`:
//
// Weather already reports precipitation (0..1, the instantaneous rainfall).
// Feeding that straight to the shader would be wrong in the way that reads as
// cheap the moment you look at it: the street would dry the instant the rain
// stopped, and soak the instant it started. Real surfaces LAG. They take tens
// of seconds to wet through and MINUTES to dry, and that asymmetry is most of
// what sells rain — the long glistening tail after a shower passes is the part
// players actually notice.
//
// So this integrates precipitation over time with different rise and fall
// rates, and hands the result to both the renderer and the vehicle physics.
//
// ICE. Below freezing the film stops being water and becomes ice, which is
// dramatically more slippery than wet asphalt — roughly mu 0.15 against 0.6,
// against 0.9 dry. Ice also LAGS its own trigger: it takes a while to form and
// does not vanish the moment the air creeps above zero. That hysteresis is
// what stops a temperature hovering at 0 from strobing the road between grip
// states, which would be both ugly and lethal to drive on.
//
// DETERMINISM: pure integration over (dt, precipitation, temperature). No
// randomness, no clock reads, no allocation. Same inputs -> same state, which
// is what makes it testable headless and safe in a replay.
// ============================================================================
#include <cstdint>

namespace x3::game {

// What the surface currently IS. Presentation and gameplay both branch on this
// rather than on raw floats, so "is it icy" has ONE answer everywhere.
enum class SurfaceCondition : uint32_t {
    Dry      = 0,
    Damp     = 1,   // drying out, or just starting to catch rain
    Wet      = 2,   // a continuous film — full reflections
    Standing = 3,   // saturated; puddles, spray, hydroplaning territory
    Ice      = 4,   // frozen film — the dangerous one
    Snow     = 5,   // lying snow deep enough to drive ON rather than through
    Count    = 6,
};

const char* surfaceConditionName(SurfaceCondition c);

// Tunables. Defaults are calibrated so a steady downpour soaks a street in
// about half a minute and it stays visibly damp for several minutes after.
struct WetnessConfig {
    // Seconds of steady full-intensity rain to go from dry to fully soaked.
    float soakSeconds    = 32.0f;
    // Seconds to go from fully soaked back to dry with no rain, at a warm
    // ~25 C. Deliberately an order of magnitude longer than soaking — this
    // asymmetry IS the effect.
    float drySeconds     = 260.0f;
    // Fraction of the warm drying RATE that survives at freezing. Cold air
    // holds almost no vapour, so a road just above 0 C stays wet roughly four
    // times as long as the same road at 25 C.
    float dryColdFactor  = 0.25f;
    // Seconds at freezing to convert a wet surface to ice, and back.
    float freezeSeconds  = 45.0f;
    float thawSeconds    = 90.0f;
    // Air temperature (C) at/below which ice forms, and above which it thaws.
    // The gap between them is hysteresis: without it, a temperature sitting on
    // the boundary flickers the road between grip states every frame.
    float freezePointC   = 0.0f;
    float thawPointC     = 2.0f;
    // Grip multipliers, applied to the vehicle's baseline tire friction.
    // Ratios follow measured road-surface mu: dry ~0.9, wet ~0.6, ice ~0.15.
    float gripWet        = 0.67f;   // fully wet, no standing water
    float gripStanding   = 0.55f;   // saturated, spray and aquaplaning risk
    float gripIce        = 0.18f;   // fully iced
    float gripSnow       = 0.42f;   // lying snow: worse than wet, better than ice

    // ---- SNOW ACCUMULATION -------------------------------------------------
    // Depth in INCHES per hour of steady full-intensity snowfall. 1 in/hr is a
    // solid, believable storm — heavy enough that you watch it build over a
    // drive, not so heavy that the world is buried in ninety seconds.
    float snowFallInPerHour = 1.0f;
    // MELT. This is a degree-hour model, which is how snowmelt is actually
    // forecast: melt rate is proportional to how far ABOVE freezing the air is,
    // not a flat timer. It is why a 34 F afternoon takes all day to clear a
    // driveway and a 50 F one clears it by lunch — and it comes out of the
    // integrator for free instead of being scripted.
    float snowMeltInPerHourPerDegC = 0.55f;
    // Cap. Past this the surface just reads "deep"; unbounded accumulation only
    // produces a number nobody can see.
    float snowMaxIn = 14.0f;
    // Depth at which the surface stops being a road with snow on it and starts
    // being snow. Below this it is a dusting: visible, but you drive the road.
    float snowCoverIn = 0.4f;
};

// The integrator. Embed by value; tick it once per frame with the weather's
// precipitation and the local air temperature.
class WetnessModel {
public:
    WetnessModel() = default;
    explicit WetnessModel(const WetnessConfig& cfg) : m_cfg(cfg) {}

    void configure(const WetnessConfig& cfg) { m_cfg = cfg; }
    const WetnessConfig& config() const { return m_cfg; }

    // Back to bone dry, unfrozen.
    void reset() { m_wet = 0.0f; m_ice = 0.0f; m_snow = 0.0f; }

    // Advance. `precipitation` is WeatherSample::precipitation (0..1);
    // `tempC` is local air temperature in Celsius. dt in seconds.
    //
    // Ice only forms where there is water to freeze, and it can never exceed
    // the wetness it froze out of — otherwise a dry road in a cold snap would
    // turn to sheet ice, which is not a thing.
    // `snowfall` is WeatherSample::snowfall — whether what is coming down is
    // snow rather than rain. Defaults false so every existing caller is
    // unchanged and simply never accumulates.
    void tick(float dt, float precipitation, float tempC, bool snowfall = false);

    // 0..1 soak. This is what drives IRenderDevice::WetnessParams::amount.
    float wetness() const { return m_wet; }
    // 0..1 how much of that film has frozen.
    float iciness() const { return m_ice; }

    // LYING SNOW DEPTH. Stored in metres to match every other length in the
    // engine; reported in inches because that is the only unit a snow depth is
    // ever quoted in. Convert at the boundary, never in the middle.
    float snowDepthM() const { return m_snow; }
    float snowDepthIn() const { return m_snow * 39.3701f; }
    // 0..1 visual coverage — how white the ground reads. Rises fast off zero
    // (a dusting already turns a field white) and saturates well before the
    // depth cap, because past an inch or two more snow does not look whiter.
    float snowCover() const;

    // The classification. Standing water needs a genuinely saturated surface,
    // so the threshold is high.
    SurfaceCondition condition() const;

    // Tire-friction multiplier for the CURRENT surface, to compose onto
    // WheeledTuning::gripScale. 1.0 when dry. Ice dominates water when both
    // are present, because a car rides on the top layer, not the average.
    float gripScale() const;

private:
    WetnessConfig m_cfg{};
    float m_wet  = 0.0f;
    float m_ice  = 0.0f;
    float m_snow = 0.0f;   // lying depth, METRES
};

// --test-wetness. Headless, no device: asserts the soak/dry asymmetry, the
// freeze/thaw hysteresis, that ice cannot exceed the water it came from, the
// grip ordering (dry > wet > standing > ice), determinism, and — as a negative
// control — that the naive "wetness = precipitation" model this replaces FAILS
// the same asymmetry check. Returns true on pass.
bool runWetnessSelfTest();

} // namespace x3::game
