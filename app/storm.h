#pragma once
// ============================================================================
// LIGHTNING AND THUNDER
//
// THE ONE THING THAT MATTERS: thunder is LATE. Light covers a mile in five
// microseconds and sound takes five SECONDS, so the gap between the flash and
// the bang is the only cue that tells you how far away the storm is — and
// counting it is something every person already knows how to do. Play the crack
// on the same frame as the flash and you have not made a storm, you have made a
// camera with a speaker taped to it. Everything else in this file exists to
// serve that delay:
//
//   * A strike is scheduled with a DISTANCE, in the same event that schedules
//     the flash. Distance sets brightness, sets the delay, and sets which of
//     the two thunder voices plays. One number, three consequences, all
//     consistent with each other for free.
//
//   * The flash is MULTI-STROKE. Real lightning is two to four return strokes
//     down the same channel over ~200 ms, which is why it FLICKERS rather than
//     fades. A single smooth ramp down is the unmistakable tell of a fake one,
//     and it costs nothing to do properly.
//
//   * Near strikes CRACK; far strikes RUMBLE. Air absorbs high frequencies
//     over distance, so a strike a mile off has had its edge stripped and
//     arrives as a low roll. Two samples, chosen by distance, and the storm
//     suddenly has depth instead of a single repeated noise.
//
//   * Thunder is fired in FLIGHT, not at schedule time. Strikes are held in a
//     small ring of pending events and their audio fires when their travel time
//     is up — which means a fast sequence of strikes overlaps the way a real
//     storm does, with the bang from the last flash arriving under the light of
//     the next one.
//
// DETERMINISM: an LCG seeded once, stepped only in tick(). No clock reads, no
// allocation, no rand(). Same seed + same dt sequence -> same storm, which is
// what makes it testable headless and safe in a replay.
// ============================================================================
#include <cstdint>

namespace x3::audio { class IAudioSystem; }

namespace x3::game {

// A strike in flight: it has flashed, and its sound has not arrived yet.
struct LightningStrike {
    float distanceM   = 0.0f;   // how far off the channel is
    float soundInS    = 0.0f;   // seconds until the thunder lands (counts down)
    float bearingRad  = 0.0f;   // where on the horizon, for 3D placement
    bool  played      = false;  // its thunder has fired
    // Is this an ECHO TAIL rather than the strike itself? Tails must not spawn
    // tails of their own -- without this flag the roll cascades and the sky
    // never stops rumbling. They also carry no flash: the light is long gone by
    // the time the far end of the channel is heard.
    bool  isEcho      = false;
};

struct StormConfig {
    // Mean seconds between strikes at FULL storm intensity. Real storms run
    // anywhere from a strike a minute to several a second; this is a lively
    // but not comical middle, and it stretches as intensity falls.
    float meanIntervalS   = 7.0f;
    // How near and far a strike can be, in METRES (internal SI; reported in
    // miles). The near bound is what puts one overhead and rattles the car.
    float minDistanceM    = 250.0f;      // ~0.16 mi
    float maxDistanceM    = 9000.0f;     // ~5.6 mi
    // Speed of sound, m/s. Named rather than inlined because it IS the effect.
    float soundSpeed      = 343.0f;
    // Peak sky brightness multiplier for a strike at minDistanceM.
    float flashStrength   = 3.2f;
    // Seconds the whole multi-stroke flash lasts.
    float flashSeconds    = 0.22f;
    // Distance (m) below which a strike CRACKS rather than rumbles.
    float crackDistanceM  = 1800.0f;     // ~1.1 mi
    uint32_t maxInFlight  = 8;
};

// Up to this many strikes may be travelling at once. A cap, not a guess: at the
// default mean interval and max distance roughly four are in the air, so eight
// leaves headroom without a heap allocation anywhere in the system.
constexpr uint32_t kMaxStrikesInFlight = 8;

class StormSystem {
public:
    void configure(const StormConfig& cfg) { m_cfg = cfg; }
    const StormConfig& config() const { return m_cfg; }

    // Seed the generator. Same seed + same tick sequence == same storm.
    void reset(uint32_t seed = 0x51A2Bu);

    // Advance. `intensity` is 0..1 storm strength — feed it
    // WeatherSample::hazardLevel when the state is Storm, and 0 otherwise; at 0
    // the system schedules nothing and decays what is already in the air, so a
    // storm that passes still gets its last thunder instead of being cut off
    // mid-sky. `audio` may be null (headless), in which case everything is
    // simulated and simply not heard.
    void tick(float dt, float intensity, x3::audio::IAudioSystem* audio,
              float listenerX = 0.0f, float listenerY = 0.0f, float listenerZ = 0.0f);

    // 0..N sky brightness ADD for this frame — multiply or add onto the sky's
    // exposure. Zero on the vast majority of frames, which is the point: a
    // lightning term that is always slightly on is just a brighter sky.
    float flash() const { return m_flash; }

    // Is a flash happening right now (for anything that wants to react)?
    bool flashing() const { return m_flashT > 0.0f; }

    // Strikes counted since reset(), and how many are currently in flight —
    // both for the self-test and the debug overlay.
    uint32_t strikeCount() const { return m_strikes; }
    uint32_t inFlight() const;

    // Seconds until the NEXT thunder lands, or -1 if nothing is in flight.
    // This is the number worth showing on a debug line: watching it count down
    // from four seconds after a flash is the whole system proving itself.
    float nextThunderIn() const;

    // Set the two sound handles. Named by role, not by file: the near voice is
    // a sharp crack, the far voice a long roll. Either may be invalid; the
    // system then flashes silently rather than refusing to run.
    void setVoices(uint32_t crackSound, uint32_t rumbleSound) {
        m_crack = crackSound; m_rumble = rumbleSound;
    }

private:
    uint32_t nextRand();
    float    randUnit();          // 0..1
    void     strike(float intensity);

    StormConfig m_cfg{};
    LightningStrike m_flight[kMaxStrikesInFlight]{};
    uint32_t m_rng     = 0x51A2Bu;
    uint32_t m_strikes = 0;
    uint32_t m_crack   = 0;
    uint32_t m_rumble  = 0;
    float    m_nextIn  = 3.0f;   // seconds to the next scheduled strike
    float    m_flash   = 0.0f;   // this frame's brightness add
    float    m_flashT  = 0.0f;   // seconds remaining in the current flash
    float    m_flashPeak = 0.0f; // that flash's peak, by distance
};

// --test-storm. Headless, no audio device: asserts the flash/thunder delay
// matches distance/speed-of-sound, that the flash is multi-stroke rather than a
// single ramp, that near strikes are brighter than far ones, that nothing is
// scheduled at zero intensity while in-flight thunder still lands, and that the
// whole thing is deterministic for a fixed seed. Returns true on pass.
bool runStormSelfTest();

} // namespace x3::game
