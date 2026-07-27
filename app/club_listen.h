#pragma once
// ============================================================================
// CLUB LISTEN MODE — the club LISTENS to whatever audio is playing on the PC
// (Spotify / Apple Music / YouTube / anything) via Windows WASAPI *loopback*
// capture, detects the live beat in real time, and drives the ENTIRE club light
// show from it instead of the fixed house tempo (kClubBpm).
//
// Two pieces live here:
//   * BeatDetector — a self-contained, deterministic real-time beat detector
//     (band-pass to the kick range -> energy flux -> adaptive-threshold onset
//     pick -> inter-onset-interval tempo estimate -> a phase-locked continuous
//     "beatCount"). It has NO dependency on any audio device, so the
//     --test-listen self-test can push a synthetic click-track straight through
//     it and assert the recovered BPM/onsets.
//   * a module singleton — opens a miniaudio *loopback* device (captures the
//     system's own output on WASAPI), feeds its callback into one BeatDetector,
//     and exposes sample() for the club's update() to read each frame.
//
// The club's beat grid (app/club1127.cpp) calls sample() once per update(): when
// Listen Mode is ON (cvar snd_listen 1) and a live signal is present it returns
// a BeatFrame that REPLACES the internal kClubBpm clock — moving heads step on
// live onsets, subs/tiles/cones pump on live bass energy, the whole gel envelope
// rides the detected kick. When OFF or silent it returns false and the club
// plays its normal internal beat exactly as before.
//
// Personal-use feature. Graceful everywhere: no device / headless CI / no
// console -> Listen Mode simply stays off and nothing else changes.
// ============================================================================

#include <cstddef>
#include <cstdint>

namespace x3 { namespace con { class IConsole; } }

namespace x3::club_listen {

// One frame of live-beat drive handed to the club show. Fields are meaningful
// only when `active` is true.
struct BeatFrame {
    float beatCount = 0.0f;  // continuous beat position (advances at bpm, PLL-aligned to onsets)
    float bpm       = 0.0f;  // running tempo estimate (BPM)
    float thump     = 0.0f;  // 0..1 kick/bass envelope (drives subs/tiles/breathe/cones)
    float bass      = 0.0f;  // 0..1 low-band energy
    float mid       = 0.0f;  // 0..1 mid-band energy
    float high      = 0.0f;  // 0..1 high-band energy
    bool  beatNow   = false; // an onset landed on this frame
    bool  active    = false; // Listen Mode ON *and* a live signal is present
};

// ---------------------------------------------------------------------------
// Real-time beat detector. Testable in isolation — no audio device required.
//   processBlock() is the AUDIO-THREAD DSP (band-pass energy + onset pick +
//                  tempo estimate); publishes results to atomics.
//   advance()      is the FRAME-THREAD phase integrator; reads those atomics,
//                  advances the continuous beat phase, PLL-aligns it to onsets,
//                  and emits a BeatFrame.
// The two may run concurrently (real loopback device + render thread); all
// cross-thread state crosses via atomics.
// ---------------------------------------------------------------------------
class BeatDetector {
public:
    explicit BeatDetector(int sampleRate = 48000);
    ~BeatDetector();
    BeatDetector(const BeatDetector&) = delete;
    BeatDetector& operator=(const BeatDetector&) = delete;

    // AUDIO THREAD. Push a block of samples. channels>1 is downmixed to mono.
    // O(frameCount) — a few one-pole filters + a hop-energy onset picker.
    void processBlock(const float* samples, size_t frameCount, int channels);

    // FRAME THREAD. Advance the beat phase by dt seconds and snapshot the drive
    // state. offsetSec shifts the emitted phase (visual<->audio alignment);
    // gain scales the band energies / thump. `active` is left false here (the
    // module singleton sets it from signalRms()); the test reads it directly.
    BeatFrame advance(float dt, float offsetSec, float gain);

    float estimatedBpm() const;  // 0 until enough onsets are seen
    int   onsetCount()   const;  // total onsets detected (the test asserts this)
    float signalRms()    const;  // recent broadband RMS (silence detection)
    void  reset();

private:
    struct Impl;
    Impl* m_ = nullptr;
};

// ---------------------------------------------------------------------------
// Module singleton — real WASAPI loopback -> the club show.
// ---------------------------------------------------------------------------

// Register the three tunable cvars on `console` (call once at host startup):
//   snd_listen           0/1   Listen Mode off (default) / on
//   snd_listen_offset_ms  ms    visual<->audio alignment offset (default 0)
//   snd_listen_gain       x     band-energy / thump gain (default 1.0)
void registerCVars(x3::con::IConsole& console);

// Bind the game console so sample() can read the cvars above. Safe to pass null
// (the feature then stays off — used by headless hosts with no console).
void bindConsole(x3::con::IConsole* console);

// FRAME THREAD. Called once per club update(). dt = seconds. Fills `out` and
// returns true when Listen Mode is ON and a live signal is present; false
// otherwise (the club keeps its internal kClubBpm beat). Lazily opens the
// loopback device the first time it sees snd_listen=1; if the device can't be
// opened it logs once and disables itself for the session.
bool sample(float dt, BeatFrame& out);

// Close the loopback device + free the singleton (host shutdown). Idempotent.
void shutdown();

// ---------------------------------------------------------------------------
// Deterministic self-test (--test-listen): feeds a synthetic click-track at a
// known BPM through a BeatDetector (NO real device) and asserts it recovers the
// onsets + BPM within tolerance and that the beat pulse fires. Returns true on
// pass. Requires no audio hardware.
// ---------------------------------------------------------------------------
bool runListenSelfTest();

}  // namespace x3::club_listen
