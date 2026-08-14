// app/space/cockpit_sway.cpp — see cockpit_sway.h.

#include "cockpit_sway.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace x3 { namespace space {

namespace {
inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
// Semi-implicit Euler on a damped spring toward `target`:
//     v += (-w^2 * (x - target) - 2*z*w*v) * dt ;  x += v * dt
// Stable for w*dt <= ~1, which the substep cap in update() guarantees.
inline void springStep(float& x, float& v, float target, float w, float z, float dt) {
    const float a = -(w * w) * (x - target) - 2.0f * z * w * v;
    v += a * dt;
    x += v * dt;
}
constexpr float kMaxSubstep = 1.0f / 120.0f;   // w*dt <= 0.25 at the default w=9
constexpr int   kMaxSubsteps = 16;             // hard cap (a 133 ms hitch)
} // namespace

void CockpitSway::reset() {
    for (int k = 0; k < 3; ++k) { m_pos[k] = m_posV[k] = m_rot[k] = m_rotV[k] = 0.0f; }
}

void CockpitSway::update(float dt, const float accelLocal[3], float yawRateRad) {
    if (dt <= 0.0f) return;
    if (m_t.strength <= 0.0f) { reset(); return; }

    const float ref = (m_t.refAccel > 1e-3f) ? m_t.refAccel : 1.0f;
    const float af = clampf(accelLocal ? accelLocal[0] / ref : 0.0f, -1.0f, 1.0f);
    const float ar = clampf(accelLocal ? accelLocal[1] / ref : 0.0f, -1.0f, 1.0f);
    const float au = clampf(accelLocal ? accelLocal[2] / ref : 0.0f, -1.0f, 1.0f);
    const float s  = m_t.strength;

    // The cabin LAGS: thrust forward pushes the interior BACK relative to the
    // hull (and the pilot into his seat), so every positional target is negated.
    const float tgtPos[3] = { -af * m_t.maxSurge * s,
                              -ar * m_t.maxSway  * s,
                              -au * m_t.maxHeave * s };
    // Rotational: forward thrust pitches the nose UP in frame; lateral thrust
    // yaws slightly; the turn banks the cabin (the classic "roll into it").
    const float tgtRot[3] = { +af * m_t.maxPitch * s,
                              -ar * m_t.maxYaw   * s,
                              clampf(yawRateRad * m_t.rollPerYawRate,
                                     -m_t.maxRoll, m_t.maxRoll) * s };

    const float w = (m_t.freq > 0.1f) ? m_t.freq : 0.1f;
    const float z = (m_t.damping > 0.0f) ? m_t.damping : 1.0f;

    // dt-CORRECT: fixed-size substeps, so the same wall-clock interval integrates
    // to the same state whether it arrives as one 50 ms frame or ten 5 ms frames.
    int   steps = (int)std::ceil(dt / kMaxSubstep);
    if (steps < 1) steps = 1;
    if (steps > kMaxSubsteps) steps = kMaxSubsteps;
    const float h = dt / (float)steps;
    for (int i = 0; i < steps; ++i) {
        for (int k = 0; k < 3; ++k) springStep(m_pos[k], m_posV[k], tgtPos[k], w, z, h);
        for (int k = 0; k < 3; ++k) springStep(m_rot[k], m_rotV[k], tgtRot[k], w, z, h);
    }

    // Hard clamps: the spring may transiently exceed its target, and the whole
    // promise here is "subtle" — a cockpit that swings is a sick cockpit.
    const float lim[3] = { m_t.maxSurge * s * 1.35f,
                           m_t.maxSway  * s * 1.35f,
                           m_t.maxHeave * s * 1.35f };
    const float rlim[3] = { m_t.maxPitch * s * 1.35f,
                            m_t.maxYaw   * s * 1.35f,
                            m_t.maxRoll  * s * 1.35f };
    for (int k = 0; k < 3; ++k) {
        if (m_pos[k] >  lim[k]) { m_pos[k] =  lim[k]; if (m_posV[k] > 0) m_posV[k] = 0; }
        if (m_pos[k] < -lim[k]) { m_pos[k] = -lim[k]; if (m_posV[k] < 0) m_posV[k] = 0; }
        if (m_rot[k] >  rlim[k]) { m_rot[k] =  rlim[k]; if (m_rotV[k] > 0) m_rotV[k] = 0; }
        if (m_rot[k] < -rlim[k]) { m_rot[k] = -rlim[k]; if (m_rotV[k] < 0) m_rotV[k] = 0; }
    }
}

// ---------------------------------------------------------------------------
// --test-cockpitsway
// ---------------------------------------------------------------------------
namespace {
int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[cockpitsway-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[cockpitsway-test] FAIL ") + name); }
}
} // namespace

bool runCockpitSwaySelfTest() {
    g_pass = g_fail = 0;

    // T1 — at rest, neutral.
    {
        CockpitSway s;
        const float a[3] = { 0, 0, 0 };
        for (int i = 0; i < 120; ++i) s.update(1.0f / 60.0f, a, 0.0f);
        check(std::fabs(s.surge()) < 1e-5f && std::fabs(s.swayR()) < 1e-5f &&
              std::fabs(s.heave()) < 1e-5f && std::fabs(s.roll()) < 1e-5f,
              "T1 neutral at rest");
    }

    // T2 — the cabin LAGS: forward thrust drives surge NEGATIVE (interior slides
    // aft) and pitches the nose up; right thrust drives sway NEGATIVE.
    {
        CockpitSway s;
        const float a[3] = { 130.0f, 0.0f, 0.0f };
        for (int i = 0; i < 60; ++i) s.update(1.0f / 60.0f, a, 0.0f);
        const bool surgeBack = s.surge() < -0.02f;
        const bool noseUp    = s.pitch() > 0.005f;
        CockpitSway s2;
        const float b[3] = { 0.0f, 130.0f, 0.0f };
        for (int i = 0; i < 60; ++i) s2.update(1.0f / 60.0f, b, 0.0f);
        check(surgeBack && noseUp && s2.swayR() < -0.015f,
              "T2 interior lags OPPOSITE the acceleration vector");
    }

    // T3 — clamped at 10x the reference acceleration (a boost slam).
    {
        CockpitSway s;
        const CockpitSway::Tuning t{};
        const float a[3] = { 1300.0f, 1300.0f, 1300.0f };
        for (int i = 0; i < 240; ++i) s.update(1.0f / 60.0f, a, 12.0f);
        check(std::fabs(s.surge()) <= t.maxSurge * 1.36f &&
              std::fabs(s.swayR()) <= t.maxSway  * 1.36f &&
              std::fabs(s.heave()) <= t.maxHeave * 1.36f &&
              std::fabs(s.roll())  <= t.maxRoll  * 1.36f,
              "T3 amplitudes clamped under a 10x accel slam");
    }

    // T4 — dt-CORRECTNESS (the house rule): the same 1 s of wall clock at 60 Hz
    // and at 240 Hz lands in the same place. A per-frame damping term fails this
    // by a mile — which is exactly how "feels very dampened" gets shipped.
    {
        CockpitSway a, b;
        const float acc[3] = { 90.0f, -40.0f, 15.0f };
        for (int i = 0; i < 60; ++i)  a.update(1.0f / 60.0f,  acc, 0.8f);
        for (int i = 0; i < 240; ++i) b.update(1.0f / 240.0f, acc, 0.8f);
        const bool same = std::fabs(a.surge() - b.surge()) < 2e-3f &&
                          std::fabs(a.swayR() - b.swayR()) < 2e-3f &&
                          std::fabs(a.pitch() - b.pitch()) < 2e-3f &&
                          std::fabs(a.roll()  - b.roll())  < 2e-3f;
        check(same, "T4 frame-rate independent (60 Hz == 240 Hz over 1 s)");
    }

    // T5 — SETTLES back to neutral with no oscillation once thrust stops.
    {
        CockpitSway s;
        const float on[3]  = { 130.0f, 0, 0 };
        const float off[3] = { 0, 0, 0 };
        for (int i = 0; i < 60; ++i) s.update(1.0f / 60.0f, on, 0.0f);
        const float peak = s.surge();
        float worstOvershoot = 0.0f;
        for (int i = 0; i < 90; ++i) {                 // 1.5 s of coast
            s.update(1.0f / 60.0f, off, 0.0f);
            worstOvershoot = std::max(worstOvershoot, s.surge());  // must not cross +
        }
        check(peak < -0.02f && std::fabs(s.surge()) < 2e-3f && worstOvershoot < 3e-3f,
              "T5 settles to neutral, critically damped (no pump)");
    }

    // T6 — strength 0 is a hard bypass (the old rigid cockpit, exactly).
    {
        CockpitSway s;
        CockpitSway::Tuning t{}; t.strength = 0.0f; s.setTuning(t);
        const float a[3] = { 500.0f, 500.0f, 500.0f };
        for (int i = 0; i < 60; ++i) s.update(1.0f / 60.0f, a, 5.0f);
        check(s.surge() == 0.0f && s.swayR() == 0.0f && s.heave() == 0.0f &&
              s.pitch() == 0.0f && s.yaw() == 0.0f && s.roll() == 0.0f,
              "T6 strength 0 == rigid cockpit (hard bypass)");
    }

    // T7 — yaw rate banks the cabin into the turn, and flips with the turn.
    {
        CockpitSway l, r;
        const float a[3] = { 0, 0, 0 };
        for (int i = 0; i < 60; ++i) { l.update(1.0f/60.0f, a, +1.2f);
                                       r.update(1.0f/60.0f, a, -1.2f); }
        const CockpitSway::Tuning t{};
        check(l.roll() > 0.01f && r.roll() < -0.01f &&
              std::fabs(l.roll()) <= t.maxRoll * 1.36f,
              "T7 banks INTO the turn, sign-correct + clamped");
    }

    x3::logInfo("[cockpitsway-test] " + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

}} // namespace x3::space
