// app/space/eva.cpp — S12 EVA spacewalk controller. See app/space/eva.h.
//
// CLEAN-ROOM, original work. Built from the public IPhysicsWorld interface +
// the player.cpp / swim_controller.cpp references (Tim's own code). No RBDOOM /
// id Tech / Doom / Quake engine source consulted.

#include "eva.h"

#include "engine/core/x3_log.h"
#include "engine/physics/IPhysicsWorld.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace x3::space {

// ---------------------------------------------------------------------------
// Controller constants (everything authorable lives in Tuning).
// ---------------------------------------------------------------------------
namespace {
constexpr float kCapsuleRadius = 0.35f;  // m, matches Player
constexpr float kCapsuleHeight = 1.8f;   // m, matches Player kStandHeight
constexpr float kEyeHeight     = 1.0f;   // m above feet — EVA-suit tucked pose
constexpr float kPitchClamp    = 89.0f * 3.14159265358979f / 180.0f; // +/- 89 deg
constexpr float kMouseSens     = 1.9f;
constexpr float kPxToRad       = 0.00132f;  // == Player's: 1.9 * 0.00132 ~= 0.0025 rad/px
// Jolt integrates gravity at -9.81 m/s^2 inside step(). EVA is in VACUUM, so we
// pre-compensate it every move the way the swim controller cancelled gravity
// below the surface (send a +Y pulse so step's `vy += g*dt` nets the desired
// vertical velocity; apply any negative residual as a direct position nudge).
constexpr float kGravityAbs = 9.81f;
} // namespace

void EVAController::spawn(x3::phys::IPhysicsWorld& physics, float x, float y, float z,
                          const Tuning& t) {
    m_tuning  = t;
    m_body    = physics.createCharacter(kCapsuleRadius, kCapsuleHeight,
                                        x3::phys::Vec3{ x, y, z });
    m_feetX = x; m_feetY = y; m_feetZ = z;
    m_velX = m_velY = m_velZ = 0.0f;
    m_oxygen  = t.maxOxygenS;
    m_hp      = m_maxHp;
    m_alive   = true;
    m_spawned = true;
    m_stuck   = false;
}

void EVAController::takeDamage(int amount) {
    if (!m_alive || amount <= 0) return;
    m_hp -= amount;
    if (m_hp <= 0) {
        m_hp    = 0;
        m_alive = false;
        x3::logInfo("[eva] HP 0 — suit breach / suffocation");
    }
}

void EVAController::setMagBoots(bool on) {
    m_tuning.magBoots = on;
    if (!on) m_stuck = false;
}

void EVAController::setRepairPoint(const x3::phys::Vec3& worldPos, float reach) {
    m_hasRepair   = true;
    m_repairX     = worldPos.x;
    m_repairY     = worldPos.y;
    m_repairZ     = worldPos.z;
    m_repairReach = reach;
}

bool EVAController::tryRepair() {
    // v1 stub: a repair "tick" lands iff we're within the active point's reach.
    // The actual repair minigame is S7's domain (interior); here we only expose
    // the exterior proximity gate.
    return m_nearRepair;
}

void EVAController::setLook(float yaw, float pitch) {
    m_yaw = yaw;
    if (pitch >  kPitchClamp) pitch =  kPitchClamp;
    if (pitch < -kPitchClamp) pitch = -kPitchClamp;
    m_pitch = pitch;
}

void EVAController::setFeetPosition(x3::phys::IPhysicsWorld& physics,
                                    const x3::phys::Vec3& feet) {
    if (!m_spawned || !m_body.valid()) return;
    physics.setBodyPosition(m_body, feet);
    m_feetX = feet.x; m_feetY = feet.y; m_feetZ = feet.z;
}

void EVAController::camera(float& x, float& y, float& z,
                           float& yaw, float& pitch) const {
    x = m_feetX;
    y = m_feetY + kEyeHeight;
    z = m_feetZ;
    yaw = m_yaw;
    pitch = m_pitch;
}

void EVAController::update(const PlayerInput& in, float dt,
                           x3::phys::IPhysicsWorld& physics) {
    if (!m_spawned || !m_body.valid() || dt <= 0.0f) return;

    // ---- Mouse look. Same scaling as Player/Swim so swap-in is seamless.
    m_yaw   += in.lookDX * kMouseSens * kPxToRad;
    m_pitch -= in.lookDY * kMouseSens * kPxToRad;
    if (m_pitch >  kPitchClamp) m_pitch =  kPitchClamp;
    if (m_pitch < -kPitchClamp) m_pitch = -kPitchClamp;

    // While dead, still cache the feet so camera()/queries stay valid (mirrors
    // Player/Swim behaviour after HP 0).
    if (!m_alive) {
        const x3::phys::Vec3 feet = physics.getBodyPosition(m_body);
        m_feetX = feet.x; m_feetY = feet.y; m_feetZ = feet.z;
        return;
    }

    // ---- Mag-boot surface probe. Cast a short ray straight DOWN from the feet;
    // if a hull surface is within magStickDist the boots can engage. We probe the
    // Static layer (the hull is static collision). Only meaningful when the boots
    // are switched on.
    bool nearSurface = false;
    float surfaceY   = 0.0f;
    if (m_tuning.magBoots) {
        const x3::phys::Vec3 origin{ m_feetX, m_feetY + 0.05f, m_feetZ };
        const x3::phys::Vec3 down{ 0.0f, -1.0f, 0.0f };
        x3::phys::RayHit hit = physics.rayCast(origin, down,
                                               m_tuning.magStickDist + 0.05f,
                                               x3::phys::Layer::Static);
        if (hit.hit) { nearSurface = true; surfaceY = hit.point.y; }
    }
    m_stuck = m_tuning.magBoots && nearSurface;

    // ---- Look basis. Full 3D forward (pitch INCLUDED) so looking down + W
    // thrusts down-forward. Right is the horizontal-only XZ right.
    const float cp = std::cos(m_pitch), sp = std::sin(m_pitch);
    const float fX = std::cos(m_yaw) * cp;
    const float fY = sp;
    const float fZ = std::sin(m_yaw) * cp;
    const float rX = -std::sin(m_yaw);   // horizontal right (XZ)
    const float rZ =  std::cos(m_yaw);

    // ---- Thruster acceleration. Hold sprint = boost (consumes O2 faster).
    const bool  boosting = in.sprint;
    const float accel = boosting ? m_tuning.boostAccel : m_tuning.thrustAccel;

    // Ascend bit = Space (jumpPressed, held by the host). PlayerInput has no
    // descend bit; the host applies LeftCtrl as a separate -Y nudge AFTER
    // update() (see --world eva), mirroring the swim controller's convention.
    float ascend = 0.0f;
    if (in.jumpPressed) ascend += 1.0f;

    if (m_stuck) {
        // ===== MAG-BOOTS ENGAGED — hull walk. =============================
        // The boots clamp the suit to the surface: free-float drift is killed and
        // motion becomes surface-relative. v1 simplification (the hull surfaces in
        // the showcase are axis-aligned, so the surface normal is +Y): we move
        // ALONG the horizontal tangent (W/S on the yaw-forward plane, A/D strafe)
        // at a walk speed, and clamp the feet to sit on the surface (surfaceY).
        // Vertical thrust + boost are ignored while stuck (you're magnetized).
        const float fHX = std::cos(m_yaw), fHZ = std::sin(m_yaw);
        float wx = fHX * in.moveFwd + rX * in.moveStrafe;
        float wz = fHZ * in.moveFwd + rZ * in.moveStrafe;
        const float wlen = std::sqrt(wx*wx + wz*wz);
        // Walk speed = a fraction of maxSpeed (deliberate, magnetized gait).
        const float walk = m_tuning.maxSpeed * 0.5f;
        float vX = 0.0f, vZ = 0.0f;
        if (wlen > 1e-4f) { vX = wx / wlen * walk; vZ = wz / wlen * walk; }
        // Kill the coasting velocity — the magnets hold you.
        m_velX = vX; m_velY = 0.0f; m_velZ = vZ;
        // Move horizontally; cancel gravity for this step (vy pulse = g*dt so the
        // body neither rises nor falls), then clamp feet to the surface so the
        // boots "stick" flush to the hull.
        physics.moveCharacter(m_body, x3::phys::Vec3{ vX, kGravityAbs * dt, vZ }, dt);
        {
            x3::phys::Vec3 p = physics.getBodyPosition(m_body);
            p.y = surfaceY;   // clamp to the hull surface (boots flush)
            physics.setBodyPosition(m_body, p);
        }
    } else {
        // ===== FREE-FLOAT — zero-G thruster pack. =========================
        // Build the wish acceleration (3D), integrate it into the persisted
        // velocity (which COASTS — no water drag), clamp to maxSpeed, then move.
        float ax = fX * in.moveFwd + rX * in.moveStrafe;
        float ay = fY * in.moveFwd + ascend;
        float az = fZ * in.moveFwd + rZ * in.moveStrafe;
        const float alen = std::sqrt(ax*ax + ay*ay + az*az);
        if (alen > 1e-4f) {
            const float k = accel / alen;     // normalize so diagonal isn't faster
            m_velX += ax * k * dt;
            m_velY += ay * k * dt;
            m_velZ += az * k * dt;
        }

        // Optional passive drift (the zero-G analogue of buoyancy). Default 0.
        if (m_tuning.driftAccel > 0.0f) m_velY -= m_tuning.driftAccel * dt;

        // Velocity bleed (RCS auto-stabilizer). 0 == pure ballistic coasting.
        if (m_tuning.damping > 0.0f) {
            const float keep = std::max(0.0f, 1.0f - m_tuning.damping * dt);
            m_velX *= keep; m_velY *= keep; m_velZ *= keep;
        }

        // Clamp to maxSpeed.
        const float spd = std::sqrt(m_velX*m_velX + m_velY*m_velY + m_velZ*m_velZ);
        if (spd > m_tuning.maxSpeed && spd > 1e-5f) {
            const float s = m_tuning.maxSpeed / spd;
            m_velX *= s; m_velY *= s; m_velZ *= s;
        }

        // ---- Issue the move. VACUUM: cancel Jolt's internal gravity each step
        // (same trick the swim controller used below the surface). Send a +Y
        // pulse so step's `vy += g*dt` leaves vy == max(vY,0); apply any negative
        // residual as a direct position nudge so we can also descend.
        const float vyPulse = std::max(0.0f, m_velY) + kGravityAbs * dt;
        physics.moveCharacter(m_body, x3::phys::Vec3{ m_velX, vyPulse, m_velZ }, dt);
        if (m_velY < 0.0f) {
            x3::phys::Vec3 p = physics.getBodyPosition(m_body);
            p.y += m_velY * dt;
            physics.setBodyPosition(m_body, p);
        }
    }

    // ---- Oxygen / suffocation. O2 always drains in vacuum (no refill outside).
    const float drainRate = m_tuning.oxygenDrainS * (boosting ? 2.0f : 1.0f);
    m_oxygen -= drainRate * dt;
    if (m_oxygen < 0.0f) m_oxygen = 0.0f;
    if (m_oxygen <= 0.0f) {
        // Suffocation: HP bleeds (same shape as swim's drowning).
        const float dmg = m_tuning.damagePerSecNoO2 * dt;
        m_hp -= (int)std::ceil(dmg);
        if (m_hp <= 0) {
            m_hp    = 0;
            m_alive = false;
            x3::logInfo("[eva] suffocated (oxygen 0)");
        }
    }

    // ---- Cache feet position so camera() works without a physics arg.
    const x3::phys::Vec3 feet = physics.getBodyPosition(m_body);
    m_feetX = feet.x; m_feetY = feet.y; m_feetZ = feet.z;

    // ---- Hull-repair proximity (v1 stub).
    if (m_hasRepair) {
        const float dx = m_feetX - m_repairX;
        const float dy = m_feetY - m_repairY;
        const float dz = m_feetZ - m_repairZ;
        m_nearRepair = (dx*dx + dy*dy + dz*dz) <= (m_repairReach * m_repairReach);
    } else {
        m_nearRepair = false;
    }
}

// ===========================================================================
// Self-test (--test-eva)
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[eva-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[eva-test] FAIL ") + name); }
}

constexpr float kFixedDt = 1.0f / 60.0f;

// A flat "hull plate" (static mesh) at a chosen Y, used by the mag-boot test.
x3::phys::BodyId makeHullPlate(x3::phys::IPhysicsWorld& w, float half, float plateY) {
    float v[] = {
        -half, plateY, -half,
         half, plateY, -half,
         half, plateY,  half,
        -half, plateY,  half,
    };
    uint32_t idx[] = { 0, 2, 1, 0, 3, 2 };
    return w.addStaticMesh(v, 4, idx, 6);
}

void frame(EVAController& p, x3::phys::IPhysicsWorld& w, const PlayerInput& in) {
    p.update(in, kFixedDt, w);
    w.step(kFixedDt);
}

} // namespace

bool runEvaSelfTest() {
    g_pass = g_fail = 0;

    // ---- T1: spawn places the body at the requested pos + alive + full O2 -----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        EVAController e;
        EVAController::Tuning t; t.maxOxygenS = 120.0f;
        e.spawn(*w, 0.0f, 30.0f, 0.0f, t);
        const x3::phys::Vec3 p = e.pos();
        bool placed = std::fabs(p.x - 0.0f) < 0.01f &&
                      std::fabs(p.y - 30.0f) < 0.01f &&
                      std::fabs(p.z - 0.0f) < 0.01f;
        check(placed && e.isAlive() &&
              std::fabs(e.oxygenSeconds() - 120.0f) < 0.01f,
              "T1 spawn places body + alive + full O2");
        w->shutdown();
    }

    // ---- T2: WASD thrusts in 3D (forward along look) — moves in vacuum --------
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        EVAController e;
        e.spawn(*w, 0.0f, 30.0f, 0.0f);   // yaw=0 -> faces +X
        const x3::phys::Vec3 p0 = e.pos();
        PlayerInput fwd; fwd.moveFwd = 1.0f;
        for (int i = 0; i < 120; ++i) frame(e, *w, fwd);   // 2 s thrust
        const x3::phys::Vec3 p1 = e.pos();
        bool advanced = (p1.x - p0.x) > 1.0f;   // faces +X at pitch=0
        check(advanced, "T2 WASD forward thrusts along look (+X) in vacuum");
        w->shutdown();
    }

    // ---- T3: boost (Shift) accelerates faster than base thrust ----------------
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        EVAController::Tuning t;
        t.thrustAccel = 3.0f; t.boostAccel = 6.0f; t.maxSpeed = 100.0f; // no clamp
        t.damping = 0.0f;     // pure coasting so the accel difference is clean
        EVAController eb; eb.spawn(*w, 0.0f, 30.0f, 0.0f, t);
        EVAController en; en.spawn(*w, 0.0f, 30.0f, 0.0f, t);
        PlayerInput base; base.moveFwd = 1.0f;
        PlayerInput fast; fast.moveFwd = 1.0f; fast.sprint = true;
        for (int i = 0; i < 30; ++i) { eb.update(fast, kFixedDt, *w); en.update(base, kFixedDt, *w); w->step(kFixedDt); }
        const float vBoost = std::sqrt(eb.velocity().x*eb.velocity().x +
                                       eb.velocity().y*eb.velocity().y +
                                       eb.velocity().z*eb.velocity().z);
        const float vBase  = std::sqrt(en.velocity().x*en.velocity().x +
                                       en.velocity().y*en.velocity().y +
                                       en.velocity().z*en.velocity().z);
        check(vBoost > vBase * 1.5f, "T3 boost accelerates faster than base thrust");
        w->shutdown();
    }

    // ---- T4: oxygen drains in vacuum ------------------------------------------
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        EVAController e;
        EVAController::Tuning t; t.maxOxygenS = 10.0f; t.oxygenDrainS = 1.0f;
        e.spawn(*w, 0.0f, 30.0f, 0.0f, t);
        const float o0 = e.oxygenSeconds();
        for (int i = 0; i < 120; ++i) frame(e, *w, PlayerInput{});  // 2 s idle
        const float o1 = e.oxygenSeconds();
        bool drained = (o0 - o1) > 1.5f && o1 > 0.0f;  // ~2 s drained
        check(drained, "T4 oxygen drains in vacuum");
        w->shutdown();
    }

    // ---- T5: HP drains at 0 oxygen (suffocation) ------------------------------
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        EVAController e;
        EVAController::Tuning t;
        t.maxOxygenS = 0.1f;            // ~6 frames of O2
        t.oxygenDrainS = 1.0f;
        t.damagePerSecNoO2 = 50.0f;     // big rate so HP drops fast in the test
        e.spawn(*w, 0.0f, 30.0f, 0.0f, t);
        const int hp0 = e.hp();
        for (int i = 0; i < 120; ++i) frame(e, *w, PlayerInput{});  // 2 s
        const int hp1 = e.hp();
        check(hp1 < hp0, "T5 HP drains at 0 oxygen (suffocation)");
        w->shutdown();
    }

    // ---- T6: takeDamage() decrements HP (+ lethal hit kills) ------------------
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        EVAController e;
        e.spawn(*w, 0.0f, 30.0f, 0.0f);
        const int hp0 = e.hp();
        e.takeDamage(17);
        bool decremented = (hp0 - e.hp()) == 17 && e.isAlive();
        e.takeDamage(10000);
        bool dead = !e.isAlive() && e.hp() == 0;
        check(decremented && dead, "T6 takeDamage decrements HP + lethal hit kills");
        w->shutdown();
    }

    // ---- T7: NO passive gravity fall — zero-G hold with no input --------------
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        EVAController e;
        EVAController::Tuning t; t.driftAccel = 0.0f;   // true 0-G, no drift
        e.spawn(*w, 0.0f, 30.0f, 0.0f, t);
        const x3::phys::Vec3 p0 = e.pos();
        for (int i = 0; i < 120; ++i) frame(e, *w, PlayerInput{});  // 2 s idle
        const x3::phys::Vec3 p1 = e.pos();
        bool noFall = std::fabs(p1.y - p0.y) < 0.05f;   // did NOT sink under gravity
        check(noFall, "T7 zero-G: no passive gravity fall with no input");
        w->shutdown();
    }

    // ---- T8: mag-boots engage near a hull surface + change movement mode ------
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        // Hull plate at y = 0; spawn the suit just above it (within magStickDist).
        makeHullPlate(*w, 50.0f, 0.0f);
        EVAController e;
        EVAController::Tuning t;
        t.magBoots = false; t.magStickDist = 1.2f; t.driftAccel = 0.0f;
        e.spawn(*w, 0.0f, 0.6f, 0.0f, t);   // feet 0.6 m over the plate
        // Boots OFF: should be free-floating (not active) even near the surface.
        e.update(PlayerInput{}, kFixedDt, *w); w->step(kFixedDt);
        const bool offNotStuck = !e.magBootsActive();
        // Toggle boots ON; next update should ENGAGE (near the plate).
        e.setMagBoots(true);
        e.update(PlayerInput{}, kFixedDt, *w); w->step(kFixedDt);
        const bool onStuck = e.magBootsActive();
        // Movement-mode change: while STUCK, vertical (Space/ascend) thrust is
        // ignored — the suit stays clamped to the surface (feet ~ plate Y), and
        // does NOT fly upward off the hull.
        PlayerInput up; up.jumpPressed = true;     // try to ascend
        for (int i = 0; i < 60; ++i) { e.update(up, kFixedDt, *w); w->step(kFixedDt); }
        const bool clamped = std::fabs(e.pos().y - 0.0f) < 0.2f && e.magBootsActive();
        check(offNotStuck && onStuck && clamped,
              "T8 mag-boots engage near hull + clamp movement (no fly-off)");
        w->shutdown();
    }

    x3::logInfo(std::string("[eva-test] ") + std::to_string(g_pass) +
                " passed, " + std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::space
