// 6DOF space-flight character controller — see app/space_pilot.h.
//
// CLEAN-ROOM, original work. Built from the public IPhysicsWorld interface +
// the app/player.cpp reference (Tim's own code). No RBDOOM / id Tech / Doom /
// Quake engine source consulted.

#include "space_pilot.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <string>

namespace x3::game {

// ===========================================================================
// Local math helpers (kept private to this TU; not worth a math/ header for
// a 6DOF demo). Quaternions are (x,y,z,w) per CONVENTIONS.md (w LAST).
// ===========================================================================
namespace {

constexpr float kPi          = 3.14159265358979323846f;
// Pitch clamp: identical feel to Player (avoid gimbal-locked straight up/down).
constexpr float kPitchClamp  = 89.0f * kPi / 180.0f;
constexpr float kMouseSens   = 1.9f;
constexpr float kPxToRad     = 0.00132f;   // matches Player/SwimController feel
// Body sphere radius (collision shape only; the visual ship is larger).
constexpr float kBodyRadius  = 1.2f;
// Laser tuning (showcase + test): cost + cooldown + bolt range.
constexpr float kLaserCdSec   = 0.18f;
constexpr float kLaserEnergy  = 8.0f;
constexpr float kLaserRange   = 400.0f;
// Antimatter-boost overspeed bleed-off rate (1/s) once Shift releases. The
// speed cap eases from the boosted ceiling back toward maxSpeed as
// 1-exp(-rate*dt); ~0.85 settles the overspeed in ~3.5 s (a glide-down, not a
// wall). Frame-rate independent.
constexpr float kBoostCapDecay = 0.85f;

// Quaternion (x,y,z,w) helpers ------------------------------------------------

// q = q1 * q2 (rotation-compose; q1 applied second, q2 first in world terms —
// here we always compose so q*delta means "rotate by delta in body-local
// frame", which is what we want for 6DOF inputs).
inline void quatMul(const float a[4], const float b[4], float out[4]) {
    const float ax=a[0], ay=a[1], az=a[2], aw=a[3];
    const float bx=b[0], by=b[1], bz=b[2], bw=b[3];
    out[0] = aw*bx + ax*bw + ay*bz - az*by;
    out[1] = aw*by - ax*bz + ay*bw + az*bx;
    out[2] = aw*bz + ax*by - ay*bx + az*bw;
    out[3] = aw*bw - ax*bx - ay*by - az*bz;
}

// Build a quaternion from axis-angle (axis assumed unit).
inline void quatFromAxisAngle(float ax, float ay, float az, float angle, float out[4]) {
    const float h = angle * 0.5f;
    const float s = std::sin(h);
    out[0] = ax * s; out[1] = ay * s; out[2] = az * s; out[3] = std::cos(h);
}

inline void quatNormalize(float q[4]) {
    float n = std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    if (n > 1e-7f) { q[0]/=n; q[1]/=n; q[2]/=n; q[3]/=n; }
    else           { q[0]=q[1]=q[2]=0; q[3]=1; }
}

// Rotate vec v by quat q (v' = q * v * q^-1 — formula expanded). v assumed
// arbitrary length; output overwrites out.
inline void quatRotate(const float q[4], const float v[3], float out[3]) {
    // t = 2 * cross(q.xyz, v)
    const float qx=q[0], qy=q[1], qz=q[2], qw=q[3];
    const float tx = 2.0f * (qy*v[2] - qz*v[1]);
    const float ty = 2.0f * (qz*v[0] - qx*v[2]);
    const float tz = 2.0f * (qx*v[1] - qy*v[0]);
    // v' = v + qw*t + cross(q.xyz, t)
    out[0] = v[0] + qw*tx + (qy*tz - qz*ty);
    out[1] = v[1] + qw*ty + (qz*tx - qx*tz);
    out[2] = v[2] + qw*tz + (qx*ty - qy*tx);
}

// Re-derive a quat from Euler (yaw around +Y, pitch around local +Z post-yaw,
// roll around local +X post-pitch). Right-handed.
inline void quatFromYawPitchRoll(float yaw, float pitch, float roll, float out[4]) {
    const float cy = std::cos(yaw   * 0.5f), sy = std::sin(yaw   * 0.5f);
    const float cp = std::cos(pitch * 0.5f), sp = std::sin(pitch * 0.5f);
    const float cr = std::cos(roll  * 0.5f), sr = std::sin(roll  * 0.5f);
    // Yaw (Y) * Pitch (Z) * Roll (X) — chosen so yaw is the "turn left/right"
    // axis we already read from mouseX, pitch is the cockpit "look up/down"
    // axis from mouseY, and roll is the wing-tilt from Q/E.
    // qY * qZ * qX, all (x,y,z,w):
    // qY = (0, sy, 0, cy); qZ = (0, 0, sp, cp); qX = (sr, 0, 0, cr).
    // Compose qY*qZ first, then *qX.
    float qY[4] = { 0, sy, 0, cy };
    float qZ[4] = { 0, 0, sp, cp };
    float qX[4] = { sr, 0, 0, cr };
    float qYZ[4]; quatMul(qY, qZ, qYZ);
    quatMul(qYZ, qX, out);
    quatNormalize(out);
}

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline float dot3(const float a[3], const float b[3]) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

inline float length3(const float v[3]) {
    return std::sqrt(dot3(v, v));
}

} // namespace

// ===========================================================================
// Flight modes — shared latch, names, and the three feel PRESETS
// ===========================================================================
namespace {
// Process-global requested mode (see space_pilot.h). Single-threaded game code.
FlightMode g_requestedMode = FlightMode::Arcade;
} // namespace

FlightMode requestedFlightMode()          { return g_requestedMode; }
void        setRequestedFlightMode(FlightMode m) { g_requestedMode = m; }

const char* flightModeName(FlightMode m) {
    switch (m) {
        case FlightMode::Arcade: return "Arcade";
        case FlightMode::Assist: return "Assist";
        case FlightMode::Loose:  return "Loose";
    }
    return "Arcade";
}

bool parseFlightMode(const std::string& name, FlightMode& out) {
    std::string s; s.reserve(name.size());
    for (char c : name) s += (char)std::tolower((unsigned char)c);
    if (s == "arcade" || s == "0") { out = FlightMode::Arcade; return true; }
    if (s == "assist" || s == "1") { out = FlightMode::Assist; return true; }
    if (s == "loose"  || s == "2") { out = FlightMode::Loose;  return true; }
    return false;
}

// The three presets. Numbers are DELIBERATE first-pass feel guesses — the owner
// playtests + retunes; documented here so the deltas between modes are legible.
// Only the FEEL fields are set; combat/health fields keep their struct defaults
// (setMode preserves the live pools, so mid-flight swaps never reset the ship).
SpacePilotController::Tuning SpacePilotController::preset(FlightMode m) {
    Tuning t{};   // struct defaults for hull/shield/energy/chase distances
    switch (m) {
        case FlightMode::Arcade:            // Star Fox: snappy, forgiving, crisp
            t.maxLinearAccel = 34.0f;  t.maxStrafeAccel = 16.0f; t.maxAngularAccel = 4.5f;
            t.linearDrag     = 1.00f;  t.angularDrag    = 3.0f;  // crisp stop + fast settle
            t.boostMul       = 2.2f;   t.maxSpeed       = 200.0f;
            t.boostAccelMul  = 2.5f;   t.boostSpeedCapMul = 2.0f; // antimatter: ~400 m/s boosted
            t.noseFollow     = 6.0f;                             // STRONG nose-follow
            t.lookSmoothing  = 22.0f;  t.autoBank = 0.90f; t.maxBank = 0.70f; t.autoLevel = 4.0f;
            t.fovBase        = 65.0f;  t.fovMax   = 78.0f;       // moderate FOV punch
            t.chaseFollow    = 12.0f;  t.lookAhead = 0.05f; t.shakeAmp = 0.05f;  // small shake
            break;
        case FlightMode::Assist:            // Elite: weighty inertia + glide/drift
            t.maxLinearAccel = 22.0f;  t.maxStrafeAccel = 11.0f; t.maxAngularAccel = 3.0f;
            t.linearDrag     = 0.12f;  t.angularDrag    = 2.0f;  // real momentum, low drag
            t.boostMul       = 2.5f;   t.maxSpeed       = 240.0f;
            t.boostAccelMul  = 3.0f;   t.boostSpeedCapMul = 2.5f; // antimatter: ~600 m/s boosted
            t.noseFollow     = 2.0f;                             // moderate nose-follow
            t.lookSmoothing  = 16.0f;  t.autoBank = 0.50f; t.maxBank = 0.50f; t.autoLevel = 1.2f;
            t.fovBase        = 62.0f;  t.fovMax   = 82.0f;       // bigger FOV punch at speed
            t.chaseFollow    = 7.0f;   t.lookAhead = 0.09f; t.shakeAmp = 0.09f;  // medium shake
            break;
        case FlightMode::Loose:             // drift/adrenaline: fast, loose, wild
            t.maxLinearAccel = 40.0f;  t.maxStrafeAccel = 20.0f; t.maxAngularAccel = 5.5f;
            t.linearDrag     = 0.04f;  t.angularDrag    = 1.0f;  // very drifty
            t.boostMul       = 3.0f;   t.maxSpeed       = 340.0f;// HIGH top speed
            t.boostAccelMul  = 3.5f;   t.boostSpeedCapMul = 3.0f; // antimatter: ~1020 m/s boosted
            t.noseFollow     = 1.0f;                             // low = the velocity drifts
            t.lookSmoothing  = 26.0f;  t.autoBank = 0.70f; t.maxBank = 0.80f; t.autoLevel = 0.30f;
            t.fovBase        = 60.0f;  t.fovMax   = 92.0f;       // BIG FOV punch
            t.chaseFollow    = 5.0f;   t.lookAhead = 0.13f; t.shakeAmp = 0.16f;  // BIG shake
            break;
    }
    return t;
}

void SpacePilotController::setMode(FlightMode m) {
    m_mode = m;
    const Tuning p = preset(m);
    // Copy ONLY the flight-feel fields — leave combat/health + chase distances
    // (and the live hull/shield/energy pools) untouched so a hot-swap is seamless.
    m_tuning.maxLinearAccel  = p.maxLinearAccel;
    m_tuning.maxStrafeAccel  = p.maxStrafeAccel;
    m_tuning.maxAngularAccel = p.maxAngularAccel;
    m_tuning.linearDrag      = p.linearDrag;
    m_tuning.angularDrag     = p.angularDrag;
    m_tuning.boostMul        = p.boostMul;
    m_tuning.maxSpeed        = p.maxSpeed;
    m_tuning.boostAccelMul   = p.boostAccelMul;
    m_tuning.boostSpeedCapMul= p.boostSpeedCapMul;
    m_tuning.noseFollow      = p.noseFollow;
    m_tuning.lookSmoothing   = p.lookSmoothing;
    m_tuning.autoBank        = p.autoBank;
    m_tuning.maxBank         = p.maxBank;
    m_tuning.autoLevel       = p.autoLevel;
    m_tuning.fovBase         = p.fovBase;
    m_tuning.fovMax          = p.fovMax;
    m_tuning.chaseFollow     = p.chaseFollow;
    m_tuning.lookAhead       = p.lookAhead;
    m_tuning.shakeAmp        = p.shakeAmp;
    setRequestedFlightMode(m);   // keep every selection surface in agreement
}

// ===========================================================================
// Lifecycle / spawn
// ===========================================================================

void SpacePilotController::spawn(x3::phys::IPhysicsWorld& phys,
                                  float x, float y, float z,
                                  const Tuning& t) {
    m_tuning = t;
    m_pos[0] = x; m_pos[1] = y; m_pos[2] = z;
    m_vel[0] = m_vel[1] = m_vel[2] = 0.0f;
    m_speedCap = t.maxSpeed;   // boost overspeed starts fully bled-off (at cruise cap)
    m_angVel[0] = m_angVel[1] = m_angVel[2] = 0.0f;
    m_yaw = m_pitch = m_roll = 0.0f;
    m_rollAxis = 0.0f;
    // Reset the smooth/juice runtime so a re-spawn starts clean.
    m_yawTarget = m_pitchTarget = 0.0f;
    m_yawPrev = 0.0f;
    m_boostPunch = 0.0f;
    m_juiceTime = 0.0f;
    m_camValid = false;
    m_camPos[0] = m_camPos[1] = m_camPos[2] = 0.0f;
    m_shakePos[0] = m_shakePos[1] = m_shakePos[2] = 0.0f;
    m_shakeYaw = m_shakePitch = 0.0f;
    m_prevVelForShake[0] = m_prevVelForShake[1] = m_prevVelForShake[2] = 0.0f;
    quatFromYawPitchRoll(0, 0, 0, m_quat);

    m_thirdPerson = t.defaultThirdPerson;

    m_hull = t.maxHull;
    m_shield = t.maxShield;
    m_energy = t.maxEnergy;
    m_shieldRegenTimer = 0.0f;
    m_laserCd = 0.0f;

    // Underlying body — a small sphere we drive ourselves each frame. The
    // collision layer is Dynamic so a future "tractor beam" / collision pass
    // can interact with it, but the controller is the source of truth for
    // pose (the physics world's pose is overwritten each update).
    m_body = phys.addSphere(kBodyRadius, x3::phys::Vec3{ x, y, z }, /*mass=*/1.0f,
                            x3::phys::Layer::Dynamic);
    if (m_body.valid()) {
        // Zero out any latent linear/angular velocity — we drive the body
        // manually and don't want Jolt integrating gravity onto it.
        float v0[3] = { 0, 0, 0 };
        phys.setBodyLinearVelocity(m_body, v0);
        phys.setBodyAngularVelocity(m_body, v0);
        phys.setBodyDamping(m_body, 0.0f, 0.0f);  // we own damping
    }
    m_spawned = true;
}

// ===========================================================================
// Per-frame update
// ===========================================================================

void SpacePilotController::update(const PlayerInput& in, float dt,
                                   x3::phys::IPhysicsWorld& phys) {
    if (!m_spawned || dt <= 0.0f) return;

    // ---- Orientation: integrate yaw/pitch/roll from input ------------------
    // LOOK SMOOTHING (the core "not jerky" fix): the raw mouse delta accumulates
    // INSTANTLY into the target angles; the APPLIED m_yaw/m_pitch then ease
    // toward the target at Tuning.lookSmoothing (1/s), frame-rate independent via
    // 1-exp(-rate*dt). Higher rate = snappier (less lag); lower = weightier.
    m_yawTarget   += in.lookDX * kMouseSens * kPxToRad;
    m_pitchTarget -= in.lookDY * kMouseSens * kPxToRad;
    const float lookK = 1.0f - std::exp(-std::max(0.0f, m_tuning.lookSmoothing) * dt);
    m_yaw   += (m_yawTarget   - m_yaw)   * lookK;
    m_pitch += (m_pitchTarget - m_pitch) * lookK;
    // FREE PITCH (merge with trunk 36bff8f): the camera is ROLL-CAPABLE
    // (cameraBasis feeds the ship quaternion's fwd+up), so pitching past vertical
    // banks the horizon instead of inverting/pinwheeling — a fighter can LOOP up
    // and over. No clamp; keep the angle normalized by wrapping the APPLIED and
    // TARGET pitch by the SAME 2*pi so the smoothing delta (target - applied)
    // is preserved across the wrap.
    while (m_pitch >  kPi) { m_pitch -= 2.0f * kPi; m_pitchTarget -= 2.0f * kPi; }
    while (m_pitch < -kPi) { m_pitch += 2.0f * kPi; m_pitchTarget += 2.0f * kPi; }

    // Yaw rate (rad/s) from the applied yaw change this frame — drives auto-bank.
    const float yawRate = (m_yaw - m_yawPrev) / dt;
    m_yawPrev = m_yaw;

    // ROLL: manual Q/E takes priority; otherwise AUTO-BANK into the turn +
    // AUTO-LEVEL hands-off. The bank target is the roll banked INTO the current
    // yaw (0 when not turning), so the single ease both banks into turns and
    // levels back to flat — autoLevel ~0 (LOOSE) means it barely self-levels.
    if (std::fabs(m_rollAxis) > 1e-4f) {
        // Manual roll: accumulate at maxAngularAccel*axis, then angular drag so
        // released Q/E doesn't leave the ship spinning (original behavior).
        m_roll += m_rollAxis * m_tuning.maxAngularAccel * dt;
        m_roll -= m_roll * std::min(1.0f, m_tuning.angularDrag * dt);
    } else {
        const float bankTarget = clampf(-m_tuning.autoBank * yawRate,
                                        -m_tuning.maxBank, m_tuning.maxBank);
        const float levelK = 1.0f - std::exp(-std::max(0.0f, m_tuning.autoLevel) * dt);
        m_roll += (bankTarget - m_roll) * levelK;
    }
    // Roll has no hard clamp (full 360 OK), but normalize into [-pi, pi] so
    // the HUD readout stays sensible.
    while (m_roll >  kPi) m_roll -= 2.0f * kPi;
    while (m_roll < -kPi) m_roll += 2.0f * kPi;

    // Consume the buffered roll axis (the showcase loop will re-set it next
    // frame; tests likewise re-set every step). This matches the contract
    // documented in space_pilot.h.
    m_rollAxis = 0.0f;

    // Rebuild the quaternion from Euler so the camera basis stays consistent
    // with the HUD readout (yaw/pitch/roll). YAW IS NEGATED going in: the
    // engine camera convention (CONVENTIONS §3, setCamera / the FPS player)
    // is fwd = (cos p cos y, sin p, cos p SIN y) — a LEFT-handed turn about
    // +Y — while quatFromYawPitchRoll's qY is the right-handed rotation
    // (fwd Z term = -sin y). Feeding +m_yaw MIRRORED the motion basis in Z:
    // the view turned right while thrust/strafe/nose-follow pushed toward
    // the mirrored heading (owner playtest: "mouse axes are reversed" — at
    // 90 deg heading the ship flew opposite the look). Negating yaw makes
    // fwdW/rightW/upW agree with the camera exactly (right = (-sin y, 0,
    // cos y), the FPS basis), for the 1P view, the 3P chase arm, W thrust,
    // D strafe, and nose-follow alike.
    quatFromYawPitchRoll(-m_yaw, m_pitch, m_roll, m_quat);

    // ---- Build local axes (used for both motion and camera) ----------------
    const float fwdLocal[3]   = { 1, 0, 0 };
    const float rightLocal[3] = { 0, 0, 1 };
    const float upLocal[3]    = { 0, 1, 0 };
    float fwdW[3], rightW[3], upW[3];
    quatRotate(m_quat, fwdLocal,   fwdW);
    quatRotate(m_quat, rightLocal, rightW);
    quatRotate(m_quat, upLocal,    upW);

    // ---- Build linear acceleration from input ------------------------------
    // W/S along forward, A/D along right, Space(jumpPressed)/Ctrl(sprint as
    // descent? — no: we map sprint to BOOST, and use a separate up/down axis
    // baked into PlayerInput's existing channels: we re-purpose jumpPressed
    // as "up impulse" since space has no real jump). For the showcase loop
    // the host fills lookDX/lookDY + moveFwd + moveStrafe + jumpPressed for
    // up + sprint for boost; for tests we drive synthetic input the same way.
    // ANTIMATTER BOOST: forward thrust gets boostMul * boostAccelMul while Shift
    // is held — a hard kick that makes the ship LEAP (strafe/up stay un-boosted).
    const float boost = in.sprint ? (m_tuning.boostMul * m_tuning.boostAccelMul) : 1.0f;

    float accel[3] = { 0, 0, 0 };
    // Forward / back (W/S).
    for (int k = 0; k < 3; ++k)
        accel[k] += fwdW[k] * in.moveFwd * m_tuning.maxLinearAccel * boost;
    // Strafe (A/D). BOOSTED like forward thrust — Shift + A/D is the ESCAPE move
    // (owner, live: "I need MASSIVE acceleration to get AWAY ... STRAFE needs to
    // work"). Before, boost only multiplied W/S, so a boosted dodge was 5x weaker
    // than a boosted charge and escaping a close contact felt impossible.
    for (int k = 0; k < 3; ++k)
        accel[k] += rightW[k] * in.moveStrafe * m_tuning.maxStrafeAccel * boost;
    // Up impulse (Space mapped to jumpPressed; we re-use the bool as a held
    // axis here — the showcase wraps the polling so press = +1, release = 0).
    if (in.jumpPressed) {
        for (int k = 0; k < 3; ++k)
            accel[k] += upW[k] * m_tuning.maxStrafeAccel * boost;
    }

    // ---- Integrate linear velocity + drag ----------------------------------
    for (int k = 0; k < 3; ++k) m_vel[k] += accel[k] * dt;
    // Drag: dv/dt = -drag * v.
    const float dragK = std::min(1.0f, m_tuning.linearDrag * dt);
    for (int k = 0; k < 3; ++k) m_vel[k] -= m_vel[k] * dragK;
    // Nose-follow (arcade steering, Tuning.noseFollow rad-equivalent per sec;
    // 0 = off, pure Newtonian — every existing caller unchanged). Swings the
    // VELOCITY DIRECTION toward the ship's facing while preserving speed, so
    // the ship goes where the nose points. Without it, turning while the old
    // velocity persists makes the starfield stream off-nose — which players
    // read as "the mouse axes are wrong" (owner playtest, intro dogfight).
    // STRAFE EXEMPTION: nose-follow renormalizes ALL velocity onto the nose every
    // frame, which eats a pure sideways dodge — press A, you curve instead of slide
    // (owner: "STRAFE DOES NOT WORK"). While A/D is held, nose-follow stands down so
    // the strafe thrust becomes a REAL lateral slide (drag-only decay). Released, it
    // resumes and the ship self-aligns to its heading.
    if (m_tuning.noseFollow > 0.0f && std::fabs(in.moveStrafe) < 0.01f) {
        const float spd0 = length3(m_vel);
        if (spd0 > 1e-3f) {
            // Steer toward the direction the pilot is COMMANDING, not the bare nose.
            // BUG (owner: "strafe does NOT work"): steering toward fwdW alone rotated
            // every bit of A/D strafe velocity back to straight-ahead within a fraction
            // of a second, so strafe added speed and then nose-follow immediately ate it.
            // The command direction = nose(forward) + right(strafe) + up; with no
            // translation input it reduces to fwdW, so the anti-drift feel (coast toward
            // the nose after a turn) is unchanged, while strafe now PERSISTS.
            float steer[3];
            for (int k = 0; k < 3; ++k)
                steer[k] = fwdW[k] * std::max(in.moveFwd, 0.0f)
                         + rightW[k] * in.moveStrafe
                         + upW[k] * (in.jumpPressed ? 1.0f : 0.0f);
            float sl = length3(steer);
            if (sl < 1e-3f) { steer[0] = fwdW[0]; steer[1] = fwdW[1]; steer[2] = fwdW[2]; sl = 1.0f; }
            else            { steer[0] /= sl; steer[1] /= sl; steer[2] /= sl; }

            const float k2 = 1.0f - std::exp(-m_tuning.noseFollow * dt);
            float nv[3];
            for (int k = 0; k < 3; ++k)
                nv[k] = m_vel[k] / spd0 + (steer[k] - m_vel[k] / spd0) * k2;
            const float nl = length3(nv);
            if (nl > 1e-4f)
                for (int k = 0; k < 3; ++k) m_vel[k] = nv[k] / nl * spd0;
        }
    }

    // ---- Dynamic speed cap: ANTIMATTER BOOST overspeed + smooth release decay ---
    // While boosting the cap jumps INSTANTLY to maxSpeed*boostSpeedCapMul (the
    // antimatter kick tops out far above cruise). On release it does NOT snap back
    // — the overspeed bleeds off exponentially toward maxSpeed over ~3-4 s so it
    // reads as a glide-down, never as hitting a wall. With the default 1.0 muls
    // (every existing default-Tuning caller + the --test-space cap check) this is
    // exactly the old hard clamp at maxSpeed.
    if (in.sprint) {
        m_speedCap = m_tuning.maxSpeed * m_tuning.boostSpeedCapMul;   // instant ceiling raise
    } else {
        const float decayK = 1.0f - std::exp(-kBoostCapDecay * dt);
        m_speedCap += (m_tuning.maxSpeed - m_speedCap) * decayK;
        if (m_speedCap < m_tuning.maxSpeed) m_speedCap = m_tuning.maxSpeed;  // never below base
    }
    const float spd = length3(m_vel);
    if (spd > m_speedCap) {
        const float s = m_speedCap / spd;
        m_vel[0] *= s; m_vel[1] *= s; m_vel[2] *= s;
    }

    // ---- Integrate position ------------------------------------------------
    for (int k = 0; k < 3; ++k) m_pos[k] += m_vel[k] * dt;

    // ---- SMOOTH / JUICE: boost-punch, chase-cam follow, screen-shake -------
    // Presentation only — NONE of this touches m_pos/m_vel (sim stays byte-
    // identical, so mode swaps + shake are deterministic). fwdW/upW/accel above
    // are still in scope and reflect this frame's (post-roll) orientation.
    m_juiceTime += dt;
    // Boost-punch weight (eased 0..1) feeding fov().
    {
        const float target = in.sprint ? 1.0f : 0.0f;
        m_boostPunch += (target - m_boostPunch) * (1.0f - std::exp(-8.0f * dt));
    }
    // CHASE-CAM FOLLOW-SMOOTHING + look-ahead: ease the 3P camera position toward
    // the rigid chase target plus a velocity lead. camera() reads m_camPos.
    {
        float target[3];
        for (int k = 0; k < 3; ++k)
            target[k] = m_pos[k] - fwdW[k] * m_tuning.chaseDistance
                                 + upW[k]  * m_tuning.chaseHeight
                                 + m_vel[k] * m_tuning.lookAhead;
        if (!m_camValid) {
            for (int k = 0; k < 3; ++k) m_camPos[k] = target[k];
            m_camValid = true;
        } else {
            const float ck = 1.0f - std::exp(-std::max(0.0f, m_tuning.chaseFollow) * dt);
            for (int k = 0; k < 3; ++k) m_camPos[k] += (target[k] - m_camPos[k]) * ck;
        }
    }
    // SCREEN-SHAKE: deterministic LOW-FREQUENCY rumble (sum of a few fixed
    // sines, NO rand()/per-frame hash — smooth, not pixel noise), driven by
    // the ship's ACTUAL instantaneous acceleration rather than raw thrust
    // input. FIX (owner playtest: "jittery while flying to the sun" — a long
    // steady cruise holding W): `accel[]` above is the raw input-derived
    // thrust force, which stays pinned at full magnitude for as long as W is
    // held even after the ship hits its speed cap and is no longer actually
    // accelerating — that read as a small constant wobble for the whole
    // cruise. Differencing m_vel frame-to-frame gives the REAL net accel
    // (drag/cap already applied), which settles to ~0 at steady cruise and
    // only spikes on genuine ramp-up/boost/hard maneuvers, so shake amplitude
    // is zero when just holding a steady heading.
    {
        float netAccel[3];
        for (int k = 0; k < 3; ++k)
            netAccel[k] = (dt > 1e-4f) ? (m_vel[k] - m_prevVelForShake[k]) / dt : 0.0f;
        for (int k = 0; k < 3; ++k) m_prevVelForShake[k] = m_vel[k];
        const float accelMag = length3(netAccel);
        const float refAccel = m_tuning.maxLinearAccel *
                               (in.sprint ? m_tuning.boostMul * m_tuning.boostAccelMul : 1.0f);
        const float drive = clampf(accelMag / (refAccel + 1e-3f), 0.0f, 1.0f);
        const float amp = m_tuning.shakeAmp * drive;
        const float t = m_juiceTime;
        auto noise = [](float x) {
            return std::sin(x) * 0.5f + std::sin(x * 1.7f + 1.3f) * 0.33f
                 + std::sin(x * 0.53f + 4.1f) * 0.17f;  // 3-sine low-freq rumble, [-1,1]-ish
        };
        m_shakePos[0] = amp * noise(t * 11.0f);
        m_shakePos[1] = amp * noise(t * 13.0f + 5.0f);
        m_shakePos[2] = amp * noise(t * 9.0f + 11.0f);
        m_shakeYaw    = amp * 0.03f * noise(t * 8.0f + 2.0f);
        m_shakePitch  = amp * 0.03f * noise(t * 8.7f + 7.0f);
    }

    // ---- Sync the physics body so queries / contacts see the ship ----------
    if (m_body.valid()) {
        phys.setBodyPosition(m_body, x3::phys::Vec3{ m_pos[0], m_pos[1], m_pos[2] });
        phys.setBodyRotation(m_body, m_quat);
        // Keep Jolt's idea of our linear/angular vel at zero so it does not
        // try to integrate gravity onto our kinematic pose (we own the truth).
        float v0[3] = { 0, 0, 0 };
        phys.setBodyLinearVelocity(m_body, v0);
        phys.setBodyAngularVelocity(m_body, v0);
    }

    // ---- Shield regen + energy regen + cooldown timers ---------------------
    if (m_shieldRegenTimer > 0.0f) m_shieldRegenTimer -= dt;
    if (m_shieldRegenTimer <= 0.0f && m_shield < m_tuning.maxShield) {
        m_shield = std::min(m_tuning.maxShield,
                            m_shield + (int)std::ceil(m_tuning.shieldRegenPerSec * dt));
    }
    if (m_energy < m_tuning.maxEnergy) {
        m_energy = std::min(m_tuning.maxEnergy,
                            m_energy + m_tuning.energyRegenPerSec * dt);
    }
    tickCooldowns(dt);
}

void SpacePilotController::tickCooldowns(float dt) {
    if (m_laserCd > 0.0f) {
        m_laserCd -= dt;
        if (m_laserCd < 0.0f) m_laserCd = 0.0f;
    }
}

// ===========================================================================
// Camera
// ===========================================================================

void SpacePilotController::camera(float& outX, float& outY, float& outZ,
                                   float& outYaw, float& outPitch) const {
    // Camera yaw/pitch match the ship's yaw/pitch — the device's setCamera()
    // takes Euler (no roll), and the game-wide convention is to expose the
    // look angles as yaw + pitch only. The ship's ROLL is reflected in the
    // ship-model rotation; the world doesn't visibly roll with it (a real
    // cockpit POV roll would need a renderer-side "view-up" axis, out of
    // scope for v1 — call out in the task report).
    // Small rotational screen-shake rides the look angles (zero until update()
    // runs, so the headless self-test reads clean angles).
    outYaw   = m_yaw   + m_shakeYaw;
    outPitch = m_pitch + m_shakePitch;

    if (m_thirdPerson) {
        // 3P chase: the SMOOTHED follow-camera position (eased toward the rigid
        // target + look-ahead in update()) plus positional shake. Before the
        // first update() (m_camValid false — headless camera() test) fall back to
        // the RIGID formula so the pose is defined without a sim step.
        // (Trunk's interim roll-less Euler chase here was superseded on its own
        // lane by cameraBasis() below — the flight paths that need a roll-capable
        // view use that; camera() keeps the flight-modes smoothed chase.)
        if (m_camValid) {
            outX = m_camPos[0] + m_shakePos[0];
            outY = m_camPos[1] + m_shakePos[1];
            outZ = m_camPos[2] + m_shakePos[2];
        } else {
            const float fwdLocal[3] = { 1, 0, 0 };
            const float upLocal[3]  = { 0, 1, 0 };
            float fwdW[3], upW[3];
            quatRotate(m_quat, fwdLocal, fwdW);
            quatRotate(m_quat, upLocal,  upW);
            outX = m_pos[0] - fwdW[0] * m_tuning.chaseDistance + upW[0] * m_tuning.chaseHeight;
            outY = m_pos[1] - fwdW[1] * m_tuning.chaseDistance + upW[1] * m_tuning.chaseHeight;
            outZ = m_pos[2] - fwdW[2] * m_tuning.chaseDistance + upW[2] * m_tuning.chaseHeight;
        }
    } else {
        // 1P cockpit: at the ship origin + a small forward offset (so the
        // ship model doesn't clip the near plane when the host renders it).
        // Eye stays as-is (no roll / view-up per v1); only tiny positional shake.
        const float fwdLocal[3]  = { 1, 0, 0 };
        float fwdW[3]; quatRotate(m_quat, fwdLocal, fwdW);
        outX = m_pos[0] + fwdW[0] * 0.4f + m_shakePos[0];
        outY = m_pos[1] + fwdW[1] * 0.4f + m_shakePos[1];
        outZ = m_pos[2] + fwdW[2] * 0.4f + m_shakePos[2];
    }
}

float SpacePilotController::fov() const {
    // Base FOV widened toward fovMax by the current speed fraction + the eased
    // boost punch. A smoothstep softens the low-speed response (no FOV twitch at
    // a crawl). Deterministic + const — the space host feeds it into setCamera.
    const float maxs = m_tuning.maxSpeed > 1e-3f ? m_tuning.maxSpeed : 1.0f;
    float sf = clampf(length3(m_vel) / maxs, 0.0f, 1.0f);
    float f  = clampf(sf + 0.18f * m_boostPunch, 0.0f, 1.0f);
    f = f * f * (3.0f - 2.0f * f);   // smoothstep
    return m_tuning.fovBase + (m_tuning.fovMax - m_tuning.fovBase) * f;
}

void SpacePilotController::cameraBasis(float outPos[3], float outFwd[3], float outUp[3]) const {
    // Full basis from the ship quaternion — fwd/up BOTH bank + loop with the hull,
    // so the horizon rolls correctly and there is no gimbal wall or pinwheel.
    const float fwdLocal[3] = { 1, 0, 0 };
    const float upLocal[3]  = { 0, 1, 0 };
    float fwdW[3], upW[3];
    quatRotate(m_quat, fwdLocal, fwdW);
    quatRotate(m_quat, upLocal,  upW);
    outFwd[0] = fwdW[0]; outFwd[1] = fwdW[1]; outFwd[2] = fwdW[2];
    outUp[0]  = upW[0];  outUp[1]  = upW[1];  outUp[2]  = upW[2];
    if (m_thirdPerson) {
        // Chase behind (-fwd) + above (+up), rolling with the ship.
        outPos[0] = m_pos[0] - fwdW[0] * m_tuning.chaseDistance + upW[0] * m_tuning.chaseHeight;
        outPos[1] = m_pos[1] - fwdW[1] * m_tuning.chaseDistance + upW[1] * m_tuning.chaseHeight;
        outPos[2] = m_pos[2] - fwdW[2] * m_tuning.chaseDistance + upW[2] * m_tuning.chaseHeight;
    } else {
        outPos[0] = m_pos[0] + fwdW[0] * 0.4f;
        outPos[1] = m_pos[1] + fwdW[1] * 0.4f;
        outPos[2] = m_pos[2] + fwdW[2] * 0.4f;
    }
}

bool SpacePilotController::pushOut(const float center[3], float radius) {
    float to[3] = { m_pos[0] - center[0], m_pos[1] - center[1], m_pos[2] - center[2] };
    const float d = length3(to);
    if (d >= radius || d < 1e-3f) return false;
    const float inv = 1.0f / d;
    for (int k = 0; k < 3; ++k) to[k] *= inv;              // outward normal
    for (int k = 0; k < 3; ++k) m_pos[k] = center[k] + to[k] * radius;
    // Cancel the inward velocity component (elastic-ish: reflect a little so the
    // field reads as a BOUNCE, not molasses).
    const float vin = m_vel[0]*to[0] + m_vel[1]*to[1] + m_vel[2]*to[2];
    if (vin < 0.0f)
        for (int k = 0; k < 3; ++k) m_vel[k] -= 1.35f * vin * to[k];
    return true;
}

void SpacePilotController::toggleCameraMode() {
    m_thirdPerson = !m_thirdPerson;
}

// ===========================================================================
// Read-only state
// ===========================================================================

x3::phys::Vec3 SpacePilotController::forward() const {
    const float fwdLocal[3] = { 1, 0, 0 };
    float w[3]; quatRotate(m_quat, fwdLocal, w);
    return x3::phys::Vec3{ w[0], w[1], w[2] };
}

x3::phys::Vec3 SpacePilotController::right() const {
    const float rightLocal[3] = { 0, 0, 1 };
    float w[3]; quatRotate(m_quat, rightLocal, w);
    return x3::phys::Vec3{ w[0], w[1], w[2] };
}

x3::phys::Vec3 SpacePilotController::up() const {
    const float upLocal[3] = { 0, 1, 0 };
    float w[3]; quatRotate(m_quat, upLocal, w);
    return x3::phys::Vec3{ w[0], w[1], w[2] };
}

float SpacePilotController::speed() const {
    return length3(m_vel);
}

// ===========================================================================
// Combat / damage
// ===========================================================================

void SpacePilotController::takeDamage(int amount) {
    if (amount <= 0)  return;
    if (m_hull <= 0)  return; // already dead — no further damage

    // Shield-first-then-hull two-pool order: damage drains the shield, and any
    // overflow bleeds through to the hull. A direct shield hit also resets the
    // regen-delay timer so shield doesn't tick back up instantly mid-fight.
    m_shieldRegenTimer = m_tuning.shieldRegenDelay;

    int remaining = amount;
    if (m_shield > 0) {
        const int absorbed = std::min(m_shield, remaining);
        m_shield   -= absorbed;
        remaining  -= absorbed;
    }
    if (remaining > 0) {
        m_hull -= remaining;
        if (m_hull < 0) m_hull = 0;
        if (m_hull == 0) {
            x3::logInfo("[space-pilot] HULL 0 — ship destroyed");
        }
    }
}

bool SpacePilotController::fireLaser(float dt) {
    tickCooldowns(dt);
    if (m_laserCd > 0.0f)              return false;   // on cooldown
    if (m_energy < kLaserEnergy)        return false;   // not enough juice
    if (m_hull <= 0)                    return false;   // dead ship

    m_laserCd = kLaserCdSec;
    m_energy -= kLaserEnergy;
    if (m_energy < 0.0f) m_energy = 0.0f;
    return true;
}

bool SpacePilotController::fireMissile(float /*dt*/) {
    // v1 stub: homing missile lands in a follow-up task. Documented in
    // space_pilot.h. Return false so the caller knows nothing fired.
    return false;
}

// ===========================================================================
// --test-space self-test (≥7 sub-checks, headless)
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[space-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[space-test] FAIL ") + name); }
}

constexpr float kDt = 1.0f / 60.0f;

// Make a minimal physics world (no static geometry — space is empty).
std::unique_ptr<x3::phys::IPhysicsWorld> makeEmptyWorld() {
    std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
    w->init();
    return w;
}

} // namespace

bool runSpaceSelfTest() {
    g_pass = g_fail = 0;

    // T1 — spawn: position cached, alive, default shield/hull/energy. ----
    {
        auto w = makeEmptyWorld();
        SpacePilotController s;
        s.spawn(*w, 10.0f, 50.0f, -20.0f);
        const auto p = s.pos();
        bool atSpawn = std::fabs(p.x - 10.0f) < 1e-3f &&
                       std::fabs(p.y - 50.0f) < 1e-3f &&
                       std::fabs(p.z + 20.0f) < 1e-3f;
        bool alive   = s.isAlive();
        bool fullHp  = s.hull() == s.maxHull() && s.shield() == s.maxShield();
        bool fullE   = std::fabs(s.energy() - s.maxEnergy()) < 1e-3f;
        check(atSpawn && alive && fullHp && fullE, "T1 spawn");
        w->shutdown();
    }

    // T2 — W/S accelerates along forward (yaw=0 -> ship faces +X). -------
    {
        auto w = makeEmptyWorld();
        SpacePilotController s;
        s.spawn(*w, 0.0f, 0.0f, 0.0f);
        const float x0 = s.pos().x;
        PlayerInput fwd{}; fwd.moveFwd = 1.0f;
        for (int i = 0; i < 60; ++i) { s.update(fwd, kDt, *w); w->step(kDt); }
        const float x1 = s.pos().x;
        const float vx = s.velocity().x;
        // Should have advanced clearly along +X and have +X velocity.
        bool advanced = (x1 - x0) > 4.0f;
        bool fwdVel   = vx > 5.0f;
        check(advanced && fwdVel, "T2 W/S accelerates along forward");
        w->shutdown();
    }

    // T3 — mouse-Y rotates pitch. ---------------------------------------
    {
        auto w = makeEmptyWorld();
        SpacePilotController s;
        s.spawn(*w, 0.0f, 0.0f, 0.0f);
        const float p0 = s.pitch();
        // Negative lookDY -> pitch UP (Player convention: pitch -= lookDY).
        // Use a positive lookDY so the pitch DECREASES; either direction is
        // a valid "rotation" — we just assert change.
        for (int i = 0; i < 30; ++i) {
            PlayerInput in{}; in.lookDY = 20.0f;
            s.update(in, kDt, *w);
            w->step(kDt);
        }
        const float p1 = s.pitch();
        bool rotated = std::fabs(p1 - p0) > 0.1f;        // ~5.7 deg minimum
        // FREE PITCH: pitch must always stay a sane wrapped angle in [-pi,pi] (no
        // wall, no runaway). A fighter loops; the only invariant is it never NaNs
        // or escapes the normalized range.
        bool sane = std::fabs(p1) <= kPi + 0.001f && p1 == p1;
        check(rotated && sane, "T3 mouse-Y rotates pitch (free, wrapped)");
        w->shutdown();
    }

    // T4 — Q/E rolls. ----------------------------------------------------
    {
        auto w = makeEmptyWorld();
        SpacePilotController s;
        s.spawn(*w, 0.0f, 0.0f, 0.0f);
        const float r0 = s.roll();
        for (int i = 0; i < 30; ++i) {
            s.setRollInput(+1.0f);
            PlayerInput in{};
            s.update(in, kDt, *w);
            w->step(kDt);
        }
        const float r1 = s.roll();
        bool rolled = std::fabs(r1 - r0) > 0.1f;
        check(rolled, "T4 Q/E rolls the ship");
        w->shutdown();
    }

    // T5 — speed cap holds even with sustained boost. -------------------
    {
        auto w = makeEmptyWorld();
        SpacePilotController s;
        SpacePilotController::Tuning t;
        // Lower the cap so the test settles fast; keep accel high.
        t.maxSpeed = 30.0f;
        t.linearDrag = 0.0f;     // no drag — easier to overshoot
        s.spawn(*w, 0.0f, 0.0f, 0.0f, t);
        PlayerInput fwd{}; fwd.moveFwd = 1.0f; fwd.sprint = true;  // boost
        for (int i = 0; i < 600; ++i) { s.update(fwd, kDt, *w); w->step(kDt); }
        const float v = s.speed();
        bool capped = v <= t.maxSpeed + 0.5f;     // never exceeded cap (+ tol)
        bool reached = v >= t.maxSpeed - 0.5f;    // actually reached it
        check(capped && reached, "T5 speed cap holds");
        w->shutdown();
    }

    // T6 — takeDamage drains shield BEFORE hull. ------------------------
    {
        auto w = makeEmptyWorld();
        SpacePilotController s;
        s.spawn(*w, 0.0f, 0.0f, 0.0f);
        const int s0 = s.shield(), h0 = s.hull();
        s.takeDamage(100);            // less than full shield -> all absorbed
        bool shieldTook = s.shield() == s0 - 100 && s.hull() == h0;
        // Now drain through the shield with a big hit (shield should hit 0,
        // overflow bleeds to hull).
        s.takeDamage(s.shield() + 50);
        bool bled = s.shield() == 0 && s.hull() == h0 - 50;
        // Lethal: drain remaining hull.
        s.takeDamage(s.hull() + 100);
        bool dead = s.hull() == 0 && !s.isAlive();
        check(shieldTook && bled && dead, "T6 takeDamage shield->hull order");
        w->shutdown();
    }

    // T7 — energy drain on fireLaser + refuse at 0 energy. --------------
    {
        auto w = makeEmptyWorld();
        SpacePilotController s;
        SpacePilotController::Tuning t;
        t.energyRegenPerSec = 0.0f;    // disable regen for a clean drain check
        s.spawn(*w, 0.0f, 0.0f, 0.0f, t);
        const float e0 = s.energy();
        bool firedFirst = s.fireLaser(0.0f);
        bool drained = s.energy() < e0;
        // Wait out the cooldown, then drain until refused.
        int fires = 1;
        for (int i = 0; i < 200; ++i) {
            // Step the per-frame cooldown timer so we can fire again. Skip
            // update() — we only care about fireLaser semantics here.
            s.tickCooldowns(kLaserCdSec * 1.01f);
            if (s.fireLaser(0.0f)) ++fires;
            if (s.energy() < 1.0f) break;
        }
        // Now energy should be drained; one more shot should refuse.
        s.tickCooldowns(kLaserCdSec * 1.01f);
        bool refused = !s.fireLaser(0.0f);
        check(firedFirst && drained && fires > 5 && refused,
              "T7 fireLaser drains energy + refuses at 0");
        w->shutdown();
    }

    // T8 — toggleCameraMode 1P <-> 3P. ---------------------------------
    {
        auto w = makeEmptyWorld();
        SpacePilotController s;
        s.spawn(*w, 0.0f, 10.0f, 0.0f);
        const bool was3p = s.isThirdPerson();
        s.toggleCameraMode();
        const bool after = s.isThirdPerson();
        s.toggleCameraMode();
        const bool back  = s.isThirdPerson();
        // Camera positions must differ between modes (3P sits BEHIND the
        // ship; 1P sits AT the ship origin + a tiny forward offset).
        // Toggle to 3P explicitly to read both.
        if (!s.isThirdPerson()) s.toggleCameraMode();
        float x3p,y3p,z3p,yawA,pitchA;
        s.camera(x3p,y3p,z3p,yawA,pitchA);
        s.toggleCameraMode();
        float x1p,y1p,z1p,yawB,pitchB;
        s.camera(x1p,y1p,z1p,yawB,pitchB);
        const float dxx = x3p - x1p, dyy = y3p - y1p, dzz = z3p - z1p;
        const float dist = std::sqrt(dxx*dxx + dyy*dyy + dzz*dzz);
        bool toggled = (after != was3p) && (back == was3p);
        bool different = dist > 5.0f;     // 3P chase puts the cam well away
        check(toggled && different, "T8 toggleCameraMode 1P<->3P");
        w->shutdown();
    }

    // T9 — setMode swaps the feel tuning (+ presets differ, health preserved). --
    {
        auto w = makeEmptyWorld();
        SpacePilotController s;
        s.spawn(*w, 0.0f, 0.0f, 0.0f);
        s.setMode(FlightMode::Arcade);
        const float arcadeSpeed = s.tuning().maxSpeed;
        const float arcadeDrag  = s.tuning().linearDrag;
        const int   hullBefore  = s.hull();
        s.setMode(FlightMode::Loose);
        const float looseSpeed  = s.tuning().maxSpeed;
        // Loose has a much higher top speed + looser drag than Arcade, and the
        // mode() accessor + shared latch must track. Health must NOT reset.
        bool speedSwapped = looseSpeed > arcadeSpeed + 50.0f;   // 340 vs 200
        bool dragSwapped  = s.tuning().linearDrag < arcadeDrag; // 0.04 vs 1.0
        // Antimatter-boost feel fields must ride the mode swap too (Loose kicks
        // harder + tops out higher than Arcade): proves setMode copies them.
        bool boostSwapped = s.tuning().boostAccelMul > 3.0f &&        // Loose 3.5
                            s.boostedMaxSpeed() > 900.0f;             // 340 * 3.0 ~ 1020
        bool modeTracks   = (s.mode() == FlightMode::Loose) &&
                            (requestedFlightMode() == FlightMode::Loose);
        bool healthKept   = s.hull() == hullBefore && s.isAlive();
        // fov() must sit within the active mode's [base,max] band and be finite.
        const float f = s.fov();
        bool fovBand = f >= s.tuning().fovBase - 0.01f && f <= s.tuning().fovMax + 0.01f;
        check(speedSwapped && dragSwapped && boostSwapped && modeTracks && healthKept && fovBand,
              "T9 setMode swaps feel tuning (health preserved)");
        // Restore the shared latch so an ordering-sensitive host isn't surprised.
        setRequestedFlightMode(FlightMode::Arcade);
        w->shutdown();
    }

    x3::logInfo(std::string("[space-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
