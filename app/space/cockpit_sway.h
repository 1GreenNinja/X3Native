// app/space/cockpit_sway.h
//
// COCKPIT MASS — the small positional/rotational lag of the ship INTERIOR
// behind the hull under acceleration, spring-damped back to neutral, plus a
// little roll into turns.
//
// Owner playtest (2026-08): "The cockpit stays put when the player ship moves."
// The intro cockpit rig was welded rigidly to the camera's yaw/pitch by
// poseIntroCockpit() — no roll at all (so the horizon banked while the canopy
// did not) and no acceleration response, which reads as a weightless painted
// backdrop rather than a cabin you are strapped into.
//
// CONTRACT — this only ever moves the cockpit MESH. The camera, the gun
// boresight and every HUD marker stay on the ship's ACTUAL pose, so the sway
// can never fight the aim (the reticle stays true to the hull's facing).
// Amplitudes are clamped small and the spring is critically damped by default,
// so it settles without oscillating — no motion-sickness pump.
//
// PURE + dt-CORRECT: the spring is integrated semi-implicitly with an internal
// substep cap, so a 5 ms frame and a 50 ms frame reach the same place. No
// per-frame `x *= k` anywhere (house rule: motion is scaled by dt, never by
// frame count). Headless-testable — see runCockpitSwaySelfTest().
#pragma once

namespace x3 { namespace space {

class CockpitSway {
public:
    struct Tuning {
        // Acceleration that counts as "full" (m/s^2). The rig's own
        // Tuning.maxLinearAccel is the natural value: normal thrust saturates
        // the sway, boost just holds it there.
        float refAccel   = 130.0f;
        // Peak offsets at full acceleration.
        float maxSurge   = 0.085f;  // metres along the ship's forward axis
        float maxSway    = 0.070f;  // metres along the ship's right axis
        float maxHeave   = 0.055f;  // metres along the ship's up axis
        float maxPitch   = 0.035f;  // radians (~2.0 deg) nose-up under thrust
        float maxYaw     = 0.030f;  // radians (~1.7 deg) under lateral thrust
        // Roll INTO the turn, driven by yaw rate (rad of roll per rad/s of yaw).
        float rollPerYawRate = 0.09f;
        float maxRoll        = 0.075f;  // radians (~4.3 deg)
        // Spring: natural frequency (rad/s) + damping ratio. 1.0 = critically
        // damped (settles fast, never overshoots -> never pumps).
        float freq       = 9.0f;
        float damping    = 1.0f;
        // Master strength. 0 disables the whole system (the old rigid cockpit),
        // 1 = the defaults above. Exposed so it can be dialled from settings.
        float strength   = 1.0f;
    };

    void setTuning(const Tuning& t) { m_t = t; }
    const Tuning& tuning() const { return m_t; }

    // Advance the spring. `accelLocal` is the ship's acceleration resolved into
    // its OWN axes: {forward, right, up} in m/s^2 (derive it by differencing the
    // velocity and dotting against the ship basis). `yawRateRad` is the ship's
    // yaw rate in rad/s (positive = turning one way; the sign only picks which
    // way it banks). Safe with dt <= 0 (no-op) and with huge dt (substepped).
    void update(float dt, const float accelLocal[3], float yawRateRad);

    // Reset to neutral (call on spawn / respawn / beat entry).
    void reset();

    // ---- Output: SHIP-LOCAL offsets to apply to the cockpit mesh -----------
    // The cockpit LAGS the hull, so these are opposite the acceleration.
    float surge() const { return m_pos[0]; }   // +forward, metres
    float swayR() const { return m_pos[1]; }   // +right,   metres
    float heave() const { return m_pos[2]; }   // +up,      metres
    float pitch() const { return m_rot[0]; }   // radians, + = nose up
    float yaw()   const { return m_rot[1]; }   // radians
    float roll()  const { return m_rot[2]; }   // radians, + = right wing down

private:
    Tuning m_t{};
    float  m_pos[3]  = { 0, 0, 0 };
    float  m_posV[3] = { 0, 0, 0 };
    float  m_rot[3]  = { 0, 0, 0 };
    float  m_rotV[3] = { 0, 0, 0 };
};

// Headless self-test (--test-cockpitsway, folded into --test-space too):
//   T1 neutral at rest (zero accel -> zero offsets);
//   T2 the cockpit lags OPPOSITE the acceleration (thrust forward -> surge back);
//   T3 amplitudes stay inside the clamps at 10x the reference acceleration;
//   T4 dt-CORRECTNESS: 1 x 1/60 s and 4 x 1/240 s land in the same place (the
//      house rule the whole project is bitten by);
//   T5 it SETTLES back to neutral once the acceleration stops (critically damped,
//      no oscillation -> no motion-sickness pump);
//   T6 strength = 0 is a hard bypass (byte-identical to the old rigid cockpit);
//   T7 yaw rate rolls the cabin INTO the turn, clamped.
bool runCockpitSwaySelfTest();

}} // namespace x3::space
