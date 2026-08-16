#pragma once
// EngineNote — the multi-RPM engine-note bank player.
//
// WHY A BANK. Three diagnostic passes measured the single-loop approach to
// death: one wav re-pitched 0.75x..7.0x across the powerband cannot sound like
// an engine at both ends (the source material's formants shift with the pitch,
// so 7000 rpm is idle-timbre chipmunked, and the loop seam is pitched up into
// audibility). The fix every racing title uses: a bank of loops recorded (here:
// synthesized — tools/gen_engine_bank.py) at several steady RPM points, the
// runtime crossfading the two points that bracket the live RPM and pitching
// each only a little (never more than +/-20%).
//
// FIVE LOOP VOICES per car:
//   - on-load pair   : bank point below rpm + bank point above rpm
//   - overrun pair   : the same bracket from the OVERRUN family (off-throttle
//                      character: quieter, darker, sub-harmonic burble+crackle)
//   - noise bed      : broadband intake/exhaust turbulence, gain riding LOAD
//                      (SND-OPUS: the synth stack is ~99% pure harmonics while
//                      a real engine is ~half broadband — this voice is the
//                      missing half, and it scales with load at runtime, which
//                      baked-in noise cannot)
// Bracket crossfade is equal-power (gA=cos(t*pi/2), gB=sin(t*pi/2)); a one-pole
// smoothed onLoadWeight (tau ~0.2 s, driven by the host's load estimate)
// crossfades the on-load pair against the overrun pair, so lifting off swaps
// TIMBRE, not just volume. Total volume follows the tunnel host's load math,
// including its collapsed off-load floor (off-throttle stays far behind the
// tire/wind bed — SND-FABLE's "Gosh AWful Loop" fix is preserved).
//
// Voices are startLoop3D voices PARENTED TO THE CAR: update() takes the
// chassis position and moves every voice via setLoopPosition, so the note
// pans/attenuates as the chase camera swings and rides the acoustics path
// (occlusion lowpass + room reverb) the moment a host drives it. Min distance
// is set to the chase-boom radius so the driver hears the note un-attenuated.
// Voices are started lazily and re-bound only when the bracket changes; the
// outgoing voice is at gain ~0 exactly when it is swapped, so rebinding is
// inaudible. 5 loop voices — loop voices are uncapped in MiniaudioSystem
// (only one-shots contend for kMaxVoices).
//
// Clean-room: standard library + the engine's own IAudioSystem only.

#include "engine/audio/IAudioSystem.h"

#include <string>

namespace x3::game {

class EngineNote {
public:
    static constexpr int kPoints = 6;

    // Load the 12 bank loops from bankDir (absolute path to
    // .../assets/audio/vehicles/engine_bank). Returns true only if EVERY
    // member loaded — a partial bank refuses, so the caller can fall back to
    // the legacy single-loop path. `audio` must outlive this object (or call
    // shutdown() first).
    bool init(x3::audio::IAudioSystem* audio, const std::string& bankDir,
              float redlineRpm = 7500.0f);

    // Per-frame. rpm = live crank speed (apply any idle-hold BEFORE calling);
    // load = the host's load estimate in [0,1] (throttle * torque-curve *
    // boost — same input the old vol math took); (x,y,z) = the chassis
    // position (every voice is moved there). dt in seconds.
    void update(float rpm, float load, float dt, float x, float y, float z);

    // snd_bank A/B: true silences all four voices immediately (state keeps
    // tracking so un-muting is seamless).
    void setMuted(bool m);

    // Live redline override (parts/tuning can move it).
    void setRedline(float rpm) { if (rpm > 1000.0f) m_redline = rpm; }

    void shutdown();
    bool ready() const { return m_ready; }

    // The tunnel host's normalized torque curve — shared here so every wiring
    // site derives `load` from the same shape instead of re-typing the table.
    static float torqueCurve(float rpmFrac);

private:
    struct Slot {
        x3::audio::LoopHandle h{};
        int pt = -1;                       // bank point this voice is playing
    };

    // Ensure the two slots play bank points lo and lo+1 of `family`
    // (0 = on-load, 1 = overrun). Voices being replaced are at ~0 gain.
    void bindPair(Slot s[2], int lo, int family);
    x3::audio::LoopHandle startVoice(x3::audio::SoundHandle snd);

    x3::audio::IAudioSystem* m_audio = nullptr;
    x3::audio::SoundHandle m_snd[2][kPoints];   // [family][point]
    x3::audio::SoundHandle m_noiseSnd{};         // broadband turbulence bed
    Slot  m_pair[2][2];                          // [family][A/B]
    x3::audio::LoopHandle m_noise{};             // the 5th (bed) voice
    float m_pos[3] = { 0, 0, 0 };                // last chassis position
    float m_redline = 7500.0f;
    float m_sRpm = 900.0f;                       // smoothed rpm (the note glides)
    float m_sVol = 0.0f;                         // smoothed total volume
    float m_sOn  = 0.0f;                         // smoothed on-load weight
    bool  m_ready = false;
    bool  m_muted = false;
};

} // namespace x3::game
