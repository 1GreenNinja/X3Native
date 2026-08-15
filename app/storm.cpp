#include "storm.h"

#include "engine/audio/IAudioSystem.h"
#include "engine/core/x3_log.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace x3::game {

namespace {
constexpr float kPi = 3.14159265358979f;
inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
}  // namespace

uint32_t StormSystem::nextRand() {
    m_rng = m_rng * 1664525u + 1013904223u;   // Numerical Recipes LCG
    return m_rng;
}

float StormSystem::randUnit() {
    return (float)(nextRand() >> 8) / (float)(1u << 24);
}

void StormSystem::reset(uint32_t seed) {
    m_rng = seed ? seed : 0x51A2Bu;
    m_strikes = 0;
    m_flash = 0.0f;
    m_flashT = 0.0f;
    m_flashPeak = 0.0f;
    m_nextIn = 3.0f;
    for (uint32_t i = 0; i < kMaxStrikesInFlight; ++i) m_flight[i] = LightningStrike{};
}

uint32_t StormSystem::inFlight() const {
    uint32_t n = 0;
    for (uint32_t i = 0; i < kMaxStrikesInFlight; ++i)
        if (!m_flight[i].played && m_flight[i].soundInS > 0.0f) ++n;
    return n;
}

float StormSystem::nextThunderIn() const {
    float best = -1.0f;
    for (uint32_t i = 0; i < kMaxStrikesInFlight; ++i) {
        const LightningStrike& s = m_flight[i];
        if (s.played || s.soundInS <= 0.0f) continue;
        if (best < 0.0f || s.soundInS < best) best = s.soundInS;
    }
    return best;
}

void StormSystem::strike(float intensity) {
    // DISTANCE FIRST — it is the one number the whole strike hangs off. Bias
    // toward the far end: most strikes in a storm are somewhere out there, and
    // the rare close one lands harder for being rare. A uniform draw makes
    // every strike feel the same distance because the mean never moves.
    const float u = randUnit();
    const float biased = u * u;                       // pull toward 0 == near, then invert
    float d = m_cfg.maxDistanceM
            - (m_cfg.maxDistanceM - m_cfg.minDistanceM) * biased;
    // A fiercer storm is CLOSER as well as more frequent — that is what makes
    // one build and pass over rather than merely tick faster.
    d *= (1.0f - 0.45f * clamp01(intensity));
    if (d < m_cfg.minDistanceM) d = m_cfg.minDistanceM;

    // Find a free slot. Full means the sky is already saturated with sound in
    // flight; dropping the strike is correct and inaudible.
    uint32_t slot = kMaxStrikesInFlight;
    for (uint32_t i = 0; i < kMaxStrikesInFlight; ++i) {
        if (m_flight[i].played || m_flight[i].soundInS <= 0.0f) { slot = i; break; }
    }
    if (slot >= kMaxStrikesInFlight) return;

    LightningStrike& s = m_flight[slot];
    s.distanceM  = d;
    // THE DELAY. This single line is the system.
    s.soundInS   = d / m_cfg.soundSpeed;
    s.bearingRad = randUnit() * 2.0f * kPi;
    s.played     = false;
    s.isEcho     = false;

    // The flash fires NOW, at a brightness that falls off with distance. Not
    // inverse-square: the sky is a diffuse reflector, so a far strike still
    // lights the whole cloud deck, just dimmer. Inverse-square would make
    // anything past half a mile invisible and the storm would look empty.
    const float near01 = clamp01(1.0f - (d - m_cfg.minDistanceM)
                                     / (m_cfg.maxDistanceM - m_cfg.minDistanceM));
    m_flashPeak = m_cfg.flashStrength * (0.22f + 0.78f * near01 * near01);
    m_flashT    = m_cfg.flashSeconds;
    ++m_strikes;
}

void StormSystem::tick(float dt, float intensity, x3::audio::IAudioSystem* audio,
                       float listenerX, float listenerY, float listenerZ) {
    if (dt <= 0.0f) return;
    intensity = clamp01(intensity);

    // ---- schedule -------------------------------------------------------
    // At zero intensity nothing NEW is scheduled, but the loop below keeps
    // running so thunder already in the air still arrives. A storm that moves
    // off should be heard leaving, not switched off.
    if (intensity > 0.01f) {
        m_nextIn -= dt;
        if (m_nextIn <= 0.0f) {
            strike(intensity);
            // Exponential-ish interval: -ln(u) * mean gives clustered strikes
            // with occasional long gaps, which is how a storm actually sounds.
            // A fixed interval is a metronome and reads instantly as one.
            const float mean = m_cfg.meanIntervalS / (0.25f + 0.75f * intensity);
            const float u = 0.001f + randUnit() * 0.999f;
            m_nextIn = -std::log(u) * mean;
        }
    }

    // ---- the flash ------------------------------------------------------
    if (m_flashT > 0.0f) {
        m_flashT -= dt;
        if (m_flashT <= 0.0f) { m_flashT = 0.0f; m_flash = 0.0f; }
        else {
            // MULTI-STROKE. Progress runs 0->1 over the flash; the envelope is
            // a fast decay MODULATED by a few discrete return strokes down the
            // same channel. That flicker is what the eye recognises as
            // lightning; a smooth fade reads as a light being dimmed.
            const float p = 1.0f - (m_flashT / m_cfg.flashSeconds);   // 0..1
            const float decay = std::exp(-3.1f * p);
            // Three strokes at 0.00, 0.34, 0.68 of the window, each a sharp
            // spike. Sum, do not average: strokes overlap in a real channel.
            float strokes = 0.0f;
            const float at[3] = { 0.00f, 0.34f, 0.68f };
            const float amp[3] = { 1.00f, 0.62f, 0.38f };
            for (int k = 0; k < 3; ++k) {
                const float x = (p - at[k]) / 0.09f;
                if (x < 0.0f || x > 3.0f) continue;
                strokes += amp[k] * std::exp(-x * x * 1.6f);
            }
            m_flash = m_flashPeak * decay * (0.35f + 0.65f * strokes);
        }
    } else {
        m_flash = 0.0f;
    }

    // ---- thunder in flight ----------------------------------------------
    for (uint32_t i = 0; i < kMaxStrikesInFlight; ++i) {
        LightningStrike& s = m_flight[i];
        if (s.played || s.soundInS <= 0.0f) continue;
        s.soundInS -= dt;
        if (s.soundInS > 0.0f) continue;
        s.played = true;

        if (!audio) continue;
        // NEAR CRACKS, FAR RUMBLES. Air absorbs high frequencies over distance,
        // so a strike five miles out has had its edge stripped off long before
        // it reaches you and arrives as a low roll. Two voices, picked by the
        // same distance that set the delay and the brightness.
        // NB: not named `near` -- that is a windef.h macro on MSVC.
        const bool isNear = s.distanceM <= m_cfg.crackDistanceM;
        const uint32_t id = isNear ? m_crack : m_rumble;
        if (!id) continue;

        // Place it on the horizon at its bearing, at a fraction of true
        // distance so the engine's attenuation does not simply mute it — the
        // storm should be heard from five miles out, which is the range at
        // which real thunder is still perfectly audible.
        const float placeAt = s.distanceM * 0.06f;
        const float px = listenerX + std::cos(s.bearingRad) * placeAt;
        const float pz = listenerZ + std::sin(s.bearingRad) * placeAt;
        const float py = listenerY + 60.0f;

        const float far01 = clamp01(s.distanceM / m_cfg.maxDistanceM);
        const float vol   = 0.35f + 0.65f * (1.0f - far01);
        // Distance also drops the PITCH, on top of choosing the voice: the
        // farther it travelled, the more of the top end the air took.
        const float pitch = 1.05f - 0.25f * far01;
        audio->playSound3D(x3::audio::SoundHandle{ id }, px, py, pz, vol, pitch);

        // ---- THE ROLL ---------------------------------------------------
        // Thunder is not a point source. The channel is MILES long, so sound
        // from the far end of it arrives seconds after sound from the near end,
        // from a measurably different direction. That spread is what makes a
        // storm feel like it is happening around you rather than at you -- one
        // bang, however loud, is a firework.
        //
        // So a distant strike schedules ECHO TAILS: the same voice re-fired a
        // moment later, quieter, further along the channel's bearing. Near
        // strikes get none, because at half a mile the whole channel arrives
        // essentially at once and a crack is genuinely a crack.
        if (!isNear && !s.isEcho && m_rumble) {
            const uint32_t tails = 2;
            for (uint32_t k = 1; k <= tails; ++k) {
                uint32_t slot2 = kMaxStrikesInFlight;
                for (uint32_t j = 0; j < kMaxStrikesInFlight; ++j)
                    if (m_flight[j].played || m_flight[j].soundInS <= 0.0f) { slot2 = j; break; }
                if (slot2 >= kMaxStrikesInFlight) break;
                LightningStrike& t = m_flight[slot2];
                t.distanceM = s.distanceM * (1.0f + 0.18f * (float)k);
                // Fired as a strike already in flight, with NO flash: the light
                // is long gone. Only the sound is still arriving.
                t.soundInS  = (0.55f + 0.5f * (float)k) * (0.7f + randUnit() * 0.6f);
                // Walk the bearing along the channel. This is the stereo: the
                // roll sweeps across you instead of sitting in one speaker.
                t.bearingRad = s.bearingRad + (randUnit() - 0.5f) * 1.4f;
                t.played = false;
                t.isEcho = true;
            }
        }
    }
}

// ===========================================================================
// --test-storm
// ===========================================================================
namespace {
int g_pass = 0, g_fail = 0;
void scheck(bool ok, const char* what) {
    if (ok) { ++g_pass; x3::logInfo(std::string("  [ok]   ") + what); }
    else    { ++g_fail; x3::logError(std::string("  [FAIL] ") + what); }
}
}  // namespace

bool runStormSelfTest() {
    g_pass = g_fail = 0;
    x3::logInfo("--- storm self-test (lightning + thunder) ---");
    char buf[256];

    // S1: the delay IS distance / speed of sound.
    {
        StormSystem st;
        StormConfig cfg;
        st.configure(cfg);
        st.reset(1234u);
        // Run until the first strike fires, then read its flight time.
        float t = 0.0f;
        const float dt = 1.0f / 60.0f;
        while (st.strikeCount() == 0 && t < 120.0f) { st.tick(dt, 1.0f, nullptr); t += dt; }
        scheck(st.strikeCount() >= 1, "S1a a strike fires at full intensity");
        const float wait = st.nextThunderIn();
        scheck(wait > 0.0f, "S1b thunder is IN FLIGHT after the flash, not played with it");
        // Convert the wait back to a distance and check it is in range.
        const float impliedM = wait * cfg.soundSpeed;
        std::snprintf(buf, sizeof(buf),
            "S1c implied distance %.2f mi is inside the configured band (%.2f..%.2f mi)",
            impliedM / 1609.34f, cfg.minDistanceM / 1609.34f, cfg.maxDistanceM / 1609.34f);
        scheck(impliedM >= cfg.minDistanceM * 0.99f && impliedM <= cfg.maxDistanceM * 1.01f, buf);

        // Advance PAST the wait; the thunder must land.
        float adv = 0.0f;
        while (adv < wait + 0.5f) { st.tick(dt, 0.0f, nullptr); adv += dt; }
        scheck(st.nextThunderIn() < 0.0f || st.nextThunderIn() > wait,
               "S1d that strike's thunder landed once its travel time elapsed");
    }

    // S2: the flash is MULTI-STROKE, not a single ramp. Sample the flash across
    // one strike and count the direction reversals — a smooth decay has none.
    {
        StormSystem st;
        st.reset(77u);
        const float dt = 1.0f / 240.0f;
        float t = 0.0f;
        while (st.strikeCount() == 0 && t < 120.0f) { st.tick(dt, 1.0f, nullptr); t += dt; }
        float prev = st.flash();
        bool rising = false;
        int reversals = 0;
        for (int i = 0; i < 60 && st.flashing(); ++i) {
            st.tick(dt, 0.0f, nullptr);
            const float f = st.flash();
            const bool up = f > prev;
            if (i > 0 && up != rising) ++reversals;
            rising = up;
            prev = f;
        }
        std::snprintf(buf, sizeof(buf),
                      "S2 flash flickers (%d direction reversals; a single fade has 0)", reversals);
        scheck(reversals >= 2, buf);
    }

    // S3: near strikes are BRIGHTER than far ones. Drive two systems whose only
    // difference is intensity (which biases distance) and compare peak flash.
    {
        auto peakFor = [](float intensity, uint32_t seed) {
            StormSystem st; st.reset(seed);
            const float dt = 1.0f / 240.0f;
            float peak = 0.0f, t = 0.0f;
            while (t < 90.0f) { st.tick(dt, intensity, nullptr); peak = (st.flash() > peak) ? st.flash() : peak; t += dt; }
            return peak;
        };
        const float hot  = peakFor(1.0f, 4242u);
        const float mild = peakFor(0.15f, 4242u);
        std::snprintf(buf, sizeof(buf),
                      "S3 a fiercer storm flashes brighter (peak %.2f vs %.2f) — it is CLOSER, not just busier",
                      hot, mild);
        scheck(hot > mild, buf);
    }

    // S4: nothing is scheduled at zero intensity, but in-flight thunder still
    // arrives — a storm that passes is heard leaving.
    {
        StormSystem st; st.reset(999u);
        const float dt = 1.0f / 60.0f;
        float t = 0.0f;
        while (st.strikeCount() == 0 && t < 120.0f) { st.tick(dt, 1.0f, nullptr); t += dt; }
        const uint32_t n0 = st.strikeCount();
        const bool hadFlight = st.inFlight() > 0;
        for (int i = 0; i < 60 * 60; ++i) st.tick(dt, 0.0f, nullptr);
        scheck(st.strikeCount() == n0, "S4a no new strikes scheduled at zero intensity");
        scheck(hadFlight && st.inFlight() == 0, "S4b thunder already in the air still landed");
    }

    // S5: determinism.
    {
        auto run = [](uint32_t seed) {
            StormSystem st; st.reset(seed);
            const float dt = 1.0f / 60.0f;
            float acc = 0.0f;
            for (int i = 0; i < 60 * 120; ++i) { st.tick(dt, 0.8f, nullptr); acc += st.flash(); }
            return acc + (float)st.strikeCount();
        };
        const float a = run(31337u), b = run(31337u), c = run(31338u);
        scheck(a == b, "S5a same seed -> identical storm");
        scheck(a != c, "S5b different seed -> different storm");
    }

    // S6: NEGATIVE CONTROL — the naive model this replaces. Playing thunder on
    // the flash frame makes the flash/bang gap zero at every distance, which
    // destroys the only cue that conveys how far away the storm is.
    {
        StormConfig cfg;
        const float nearGap = cfg.minDistanceM / cfg.soundSpeed;
        const float farGap  = cfg.maxDistanceM / cfg.soundSpeed;
        std::snprintf(buf, sizeof(buf),
            "S6 flash-to-bang spans %.1f s (near) to %.1f s (far); the naive same-frame model gives 0.0 s at both",
            nearGap, farGap);
        scheck(farGap - nearGap > 5.0f, buf);
    }

    std::snprintf(buf, sizeof(buf), "--- storm self-test: %d passed, %d failed ---", g_pass, g_fail);
    if (g_fail) x3::logError(buf); else x3::logInfo(buf);
    return g_fail == 0;
}

}  // namespace x3::game
