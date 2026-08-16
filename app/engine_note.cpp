#include "engine_note.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace x3::game {

namespace {

// The six synthesized bank points (tools/gen_engine_bank.py). Unity pitch of
// file k IS rpm k — the generator derives f0 = rpm/60*3 — so voice pitch is
// simply rpm / pointRpm.
constexpr float kPointRpm[EngineNote::kPoints] = { 900, 1500, 2500, 4000, 5500, 7000 };
constexpr float kPi = 3.14159265358979f;

// Each voice bends at most +/-20% from its recorded point. In the widest
// brackets (900->1500 is 1.67x) the middle revs land with both voices clamped
// slightly toward each other; the equal-power blend still reads as the
// intermediate note (with a little width), which beats the alternative — a
// voice stretched 60% out of its formant range.
constexpr float kPitchMin = 1.0f / 1.2f;
constexpr float kPitchMax = 1.2f;

// Attenuation starts beyond the chase-camera boom (~9-10 m): the driver hears
// the note at authored level; bystanders/other cameras get rolloff + panning.
constexpr float kMinDistance = 10.0f;

} // namespace

float EngineNote::torqueCurve(float f) {
    // Same normalized curve the physics runs: [0,0.78] [0.3,0.97] [0.55,1.0]
    // [0.8,0.95] [1,0.82] — the note thickens through the midrange and thins
    // at the top exactly where the engine does.
    static constexpr float xs[5] = { 0.00f, 0.30f, 0.55f, 0.80f, 1.00f };
    static constexpr float ys[5] = { 0.78f, 0.97f, 1.00f, 0.95f, 0.82f };
    if (f <= xs[0]) return ys[0];
    for (int k = 1; k < 5; ++k)
        if (f <= xs[k]) {
            const float t = (f - xs[k - 1]) / (xs[k] - xs[k - 1]);
            return ys[k - 1] + (ys[k] - ys[k - 1]) * t;
        }
    return ys[4];
}

bool EngineNote::init(x3::audio::IAudioSystem* audio, const std::string& bankDir,
                      float redlineRpm) {
    m_audio = audio;
    if (redlineRpm > 1000.0f) m_redline = redlineRpm;
    if (!audio) return false;

    namespace fs = std::filesystem;
    static const char* kFamily[2] = { "onload", "overrun" };
    bool all = true;
    for (int f = 0; f < 2; ++f)
        for (int p = 0; p < kPoints; ++p) {
            char name[64];
            std::snprintf(name, sizeof(name), "flat6_%s_%04d.wav",
                          kFamily[f], (int)kPointRpm[p]);
            m_snd[f][p] = audio->load((fs::path(bankDir) / name).string());
            all = all && m_snd[f][p].valid();
        }
    m_noiseSnd = audio->load((fs::path(bankDir) / "engine_noise_bed.wav").string());
    all = all && m_noiseSnd.valid();
    m_ready = all;
    if (all)
        x3::logInfo("[engine_note] bank online: 6 RPM points x on-load/overrun + noise bed");
    else
        x3::logWarn("[engine_note] bank incomplete under " + bankDir +
                    " — refusing (caller should fall back to the single loop)");
    return m_ready;
}

x3::audio::LoopHandle EngineNote::startVoice(x3::audio::SoundHandle snd) {
    // 3D at the chassis, gain 0 (the caller fades it in), full level inside
    // the chase boom. setLoopPosition keeps it glued to the car per frame.
    x3::audio::LoopHandle h =
        m_audio->startLoop3D(snd, m_pos[0], m_pos[1], m_pos[2], 0.0f, 1.0f);
    if (h.valid()) m_audio->setLoopDistance(h, kMinDistance);
    return h;
}

void EngineNote::bindPair(Slot s[2], int lo, int family) {
    const int want[2] = { lo, lo + 1 };
    for (int i = 0; i < 2; ++i) {
        if (s[i].pt == want[i] && s[i].h.valid()) continue;
        // Prefer stealing the OTHER slot if it already plays the wanted point
        // (moving up/down one bracket keeps the shared point's voice alive —
        // no phase restart on the voice that is currently audible).
        const int j = 1 - i;
        if (s[j].pt == want[i] && s[j].h.valid() && s[j].pt != want[j]) {
            std::swap(s[i], s[j]);
            if (s[i].pt == want[i]) continue;
        }
        if (s[i].h.valid()) m_audio->stopLoop(s[i].h);
        s[i].h  = startVoice(m_snd[family][want[i]]);
        s[i].pt = want[i];
    }
}

void EngineNote::update(float rpm, float load, float dt, float x, float y, float z) {
    if (!m_ready || !m_audio) return;
    load = std::clamp(load, 0.0f, 1.5f);
    m_pos[0] = x; m_pos[1] = y; m_pos[2] = z;

    // The note GLIDES: one-pole on rpm (~0.11 s, the tunnel host's constant)
    // so physics-side clutch/gear hunt never stutters the pitch.
    const float k = 1.0f - std::exp(-9.0f * dt);
    m_sRpm += (rpm - m_sRpm) * k;

    // On-load weight: tau ~0.2 s toward "is the engine driving or being
    // driven". Same 6x collapse as the host's old onLoad term, then smoothed —
    // this is the on-load <-> overrun TIMBRE crossfade.
    const float onTarget = std::min(1.0f, load * 6.0f);
    const float kOn = 1.0f - std::exp(-dt / 0.2f);
    m_sOn += (onTarget - m_sOn) * kOn;

    // TOTAL volume: exactly the tunnel host's load math, low off-load floor
    // included (off-throttle the note sits far behind the tire/wind bed).
    const float frac = std::clamp(m_sRpm / m_redline, 0.0f, 1.0f);
    const float vol = 0.05f + 0.11f * onTarget + 0.62f * load
                    + 0.10f * frac * (0.35f + 0.65f * onTarget);
    m_sVol += (vol - m_sVol) * k;

    // Bracket the smoothed rpm between adjacent bank points.
    int lo = 0;
    while (lo < kPoints - 2 && m_sRpm > kPointRpm[lo + 1]) ++lo;
    bindPair(m_pair[0], lo, 0);
    bindPair(m_pair[1], lo, 1);
    if (!m_noise.valid()) m_noise = startVoice(m_noiseSnd);

    const float a = kPointRpm[lo], b = kPointRpm[lo + 1];
    const float t = std::clamp((m_sRpm - a) / (b - a), 0.0f, 1.0f);
    const float gA = std::cos(t * kPi * 0.5f);       // equal-power bracket fade
    const float gB = std::sin(t * kPi * 0.5f);
    const float gOn   = std::sin(m_sOn * kPi * 0.5f); // equal-power family fade
    const float gOver = std::cos(m_sOn * kPi * 0.5f);
    const float pA = std::clamp(m_sRpm / a, kPitchMin, kPitchMax);
    const float pB = std::clamp(m_sRpm / b, kPitchMin, kPitchMax);
    const float mute = m_muted ? 0.0f : 1.0f;

    m_audio->setLoopParams(m_pair[0][0].h, mute * m_sVol * gA * gOn,   pA);
    m_audio->setLoopParams(m_pair[0][1].h, mute * m_sVol * gB * gOn,   pB);
    m_audio->setLoopParams(m_pair[1][0].h, mute * m_sVol * gA * gOver, pA);
    m_audio->setLoopParams(m_pair[1][1].h, mute * m_sVol * gB * gOver, pB);

    // NOISE BED: the broadband half of the engine (SND-OPUS). Gain rides load
    // hard (turbulence is air moving = the engine actually working), with a
    // small floor so idle isn't sterile; pitch tilts the spectrum up with rpm.
    m_audio->setLoopParams(m_noise,
        mute * m_sVol * (0.18f + 0.82f * m_sOn) * 0.55f,
        0.70f + 0.80f * frac);

    // Glue every voice to the car.
    for (auto& fam : m_pair)
        for (auto& s : fam)
            if (s.h.valid()) m_audio->setLoopPosition(s.h, x, y, z);
    if (m_noise.valid()) m_audio->setLoopPosition(m_noise, x, y, z);
}

void EngineNote::setMuted(bool m) {
    if (m == m_muted) return;
    m_muted = m;
    if (m && m_audio) {
        for (auto& fam : m_pair)
            for (auto& s : fam)
                if (s.h.valid()) m_audio->setLoopParams(s.h, 0.0f, 1.0f);
        if (m_noise.valid()) m_audio->setLoopParams(m_noise, 0.0f, 1.0f);
    }
}

void EngineNote::shutdown() {
    if (m_audio) {
        for (auto& fam : m_pair)
            for (auto& s : fam) {
                if (s.h.valid()) m_audio->stopLoop(s.h);
                s = {};
            }
        if (m_noise.valid()) m_audio->stopLoop(m_noise);
        m_noise = {};
    }
    m_ready = false;
    m_audio = nullptr;
}

} // namespace x3::game
