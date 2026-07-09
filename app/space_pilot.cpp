// 6DOF space-flight character controller — see app/space_pilot.h.
//
// CLEAN-ROOM, original work. Built from the public IPhysicsWorld interface +
// the app/player.cpp reference (Tim's own code). No RBDOOM / id Tech / Doom /
// Quake engine source consulted.

#include "space_pilot.h"

#include "engine/core/x3_log.h"

#include <algorithm>
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
// Lifecycle / spawn
// ===========================================================================

void SpacePilotController::spawn(x3::phys::IPhysicsWorld& phys,
                                  float x, float y, float z,
                                  const Tuning& t) {
    m_tuning = t;
    m_pos[0] = x; m_pos[1] = y; m_pos[2] = z;
    m_vel[0] = m_vel[1] = m_vel[2] = 0.0f;
    m_angVel[0] = m_angVel[1] = m_angVel[2] = 0.0f;
    m_yaw = m_pitch = m_roll = 0.0f;
    m_rollAxis = 0.0f;
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
    // Mouse delta -> yaw (X) + pitch (Y, inverted-screen). Roll axis Q/E
    // (passed via setRollInput()) accumulates around the ship's forward.
    m_yaw   += in.lookDX * kMouseSens * kPxToRad;
    m_pitch -= in.lookDY * kMouseSens * kPxToRad;
    m_pitch = clampf(m_pitch, -kPitchClamp, kPitchClamp);

    // Roll accumulates over time at maxAngularAccel*rollAxis (scaled to a
    // reasonable per-second rotation rate). Decays toward 0 when no Q/E is
    // held (angularDrag), so the ship self-rights cinematically.
    m_roll += m_rollAxis * m_tuning.maxAngularAccel * dt;
    // Apply angular drag (so released Q/E doesn't leave the ship spinning).
    m_roll -= m_roll * std::min(1.0f, m_tuning.angularDrag * dt);
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
    const float boost = in.sprint ? m_tuning.boostMul : 1.0f;

    float accel[3] = { 0, 0, 0 };
    // Forward / back (W/S).
    for (int k = 0; k < 3; ++k)
        accel[k] += fwdW[k] * in.moveFwd * m_tuning.maxLinearAccel * boost;
    // Strafe (A/D).
    for (int k = 0; k < 3; ++k)
        accel[k] += rightW[k] * in.moveStrafe * m_tuning.maxStrafeAccel;
    // Up impulse (Space mapped to jumpPressed; we re-use the bool as a held
    // axis here — the showcase wraps the polling so press = +1, release = 0).
    if (in.jumpPressed) {
        for (int k = 0; k < 3; ++k)
            accel[k] += upW[k] * m_tuning.maxStrafeAccel;
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
    if (m_tuning.noseFollow > 0.0f) {
        const float spd0 = length3(m_vel);
        if (spd0 > 1e-3f) {
            const float k2 = 1.0f - std::exp(-m_tuning.noseFollow * dt);
            float nv[3];
            for (int k = 0; k < 3; ++k)
                nv[k] = m_vel[k] / spd0 + (fwdW[k] - m_vel[k] / spd0) * k2;
            const float nl = length3(nv);
            if (nl > 1e-4f)
                for (int k = 0; k < 3; ++k) m_vel[k] = nv[k] / nl * spd0;
        }
    }

    // Speed cap (hard clamp on |v|).
    const float spd = length3(m_vel);
    if (spd > m_tuning.maxSpeed) {
        const float s = m_tuning.maxSpeed / spd;
        m_vel[0] *= s; m_vel[1] *= s; m_vel[2] *= s;
    }

    // ---- Integrate position ------------------------------------------------
    for (int k = 0; k < 3; ++k) m_pos[k] += m_vel[k] * dt;

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
    outYaw   = m_yaw;
    outPitch = m_pitch;

    if (m_thirdPerson) {
        // 3P chase: pull the camera behind (along -forward) + up (along +up)
        // from the ship origin. So we look back at the ship.
        const float fwdLocal[3]   = { 1, 0, 0 };
        const float upLocal[3]    = { 0, 1, 0 };
        float fwdW[3], upW[3];
        quatRotate(m_quat, fwdLocal, fwdW);
        quatRotate(m_quat, upLocal,  upW);
        outX = m_pos[0] - fwdW[0] * m_tuning.chaseDistance + upW[0] * m_tuning.chaseHeight;
        outY = m_pos[1] - fwdW[1] * m_tuning.chaseDistance + upW[1] * m_tuning.chaseHeight;
        outZ = m_pos[2] - fwdW[2] * m_tuning.chaseDistance + upW[2] * m_tuning.chaseHeight;
    } else {
        // 1P cockpit: at the ship origin + a small forward offset (so the
        // ship model doesn't clip the near plane when the host renders it).
        const float fwdLocal[3]  = { 1, 0, 0 };
        float fwdW[3]; quatRotate(m_quat, fwdLocal, fwdW);
        outX = m_pos[0] + fwdW[0] * 0.4f;
        outY = m_pos[1] + fwdW[1] * 0.4f;
        outZ = m_pos[2] + fwdW[2] * 0.4f;
    }
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
        bool clamped = std::fabs(p1) < kPi * 0.5f + 0.01f; // never past ±90 deg
        check(rotated && clamped, "T3 mouse-Y rotates pitch (clamped)");
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

    x3::logInfo(std::string("[space-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
