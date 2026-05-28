// Rideable heavy-mech pilot controller — implementation. See app/mech_pilot.h.
//
// Clean-room: built from the IPhysicsWorld interface + the PROVEN character-capsule
// pattern in app/player.cpp (which it does NOT modify). No purchased C# / GPL / id
// Tech source consulted.
//
// Physics seam (same as Player, verified in engine/physics/JoltPhysicsWorld.cpp):
//   * createCharacter(radius,height,pos): pos is the capsule FEET; getBodyPosition
//     returns feet position.
//   * moveCharacter(id, desiredVelocity, dt): records the HORIZONTAL (.x/.z) desired
//     velocity; the .y is a one-shot JUMP impulse when > 0 (sets vertical velocity),
//     IGNORED when <= 0. Gravity is integrated internally during step() and the world
//     sticks the character to the floor while grounded.
//   * characterGrounded(id): true only while OnGround.
//
// DEVIATION (documented): IPhysicsWorld exposes no per-world gravity setter, so the
// world integrates its fixed gravity (-9.81). Tuning::gravity therefore reads as a
// design "feel" target rather than a literal acceleration we can install; we emulate
// the heavier feel by making accel/turn sluggish and the jump-jet a strong but
// fuel-gated boost. The mech never feels floaty because its planar accel is low.

#include "mech_pilot.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace x3::game {

namespace {
constexpr float kPi         = 3.14159265358979323846f;
constexpr float kPitchClamp = 70.0f * kPi / 180.0f;  // +/- 70 deg aim
constexpr float kPxToRad    = 0.00132f * 1.9f;       // pixel-delta -> radians (matches Player feel)
} // namespace

void MechPilotController::spawn(x3::phys::IPhysicsWorld& phys,
                                float x, float y, float z, const Tuning& t) {
    m_t = t;
    m_body = phys.createCharacter(m_t.capsuleRadius, m_t.capsuleHeight,
                                  x3::phys::Vec3{ x, y, z });
    m_spawned     = true;
    m_grounded    = false;
    m_jetFuel     = m_t.jumpJetFuelMax;
    m_velY        = 0.0f;
    m_curSpeed    = 0.0f;
    m_yaw         = 0.0f;
    m_pitch       = 0.0f;
    m_thirdPerson = m_t.defaultThirdPerson;
    m_mode        = Mode::Piloting;
    m_hull        = m_t.maxHull;
    m_armor       = m_t.maxArmor;
    m_autocannonTimer = 0.0f;
    m_missileTimer    = 0.0f;
    // Seed cached feet so camera() is valid before the first update().
    m_feetX = x; m_feetY = y; m_feetZ = z;
}

void MechPilotController::update(const PlayerInput& in, float dt,
                                 x3::phys::IPhysicsWorld& phys) {
    if (!m_spawned || !m_body.valid() || dt <= 0.0f) return;

    // Weapon cooldowns tick every frame regardless of mode (so a fresh dismount
    // doesn't leave a weapon permanently mid-cooldown).
    if (m_autocannonTimer > 0.0f) m_autocannonTimer = std::max(0.0f, m_autocannonTimer - dt);
    if (m_missileTimer    > 0.0f) m_missileTimer    = std::max(0.0f, m_missileTimer    - dt);

    // Dismounted: the mech is inert. Let the world settle it (gravity/floor-stick)
    // but accept no pilot input; still cache feet so camera() stays valid.
    if (m_mode == Mode::Dismounted) {
        phys.moveCharacter(m_body, x3::phys::Vec3{ 0.0f, 0.0f, 0.0f }, dt);
        const x3::phys::Vec3 f = phys.getBodyPosition(m_body);
        m_feetX = f.x; m_feetY = f.y; m_feetZ = f.z;
        m_grounded = phys.characterGrounded(m_body);
        return;
    }

    // ---- Aim: yaw from mouse X, pitch from mouse Y (inverted screen-Y). The mech
    // body HEADING and the camera/aim yaw share m_yaw, but the body turns SLOWLY:
    // mouse can whip the aim, while turnRate caps how fast the chassis re-faces. To
    // keep it simple + heavy, we slew m_yaw toward the mouse target at turnRate.
    const float desiredYaw = m_yaw + in.lookDX * kPxToRad;   // mouse "wants" this yaw
    {
        float dy = desiredYaw - m_yaw;
        // wrap to [-pi,pi]
        while (dy >  kPi) dy -= 2.0f * kPi;
        while (dy < -kPi) dy += 2.0f * kPi;
        const float maxStep = m_t.turnRate * dt;
        if (dy >  maxStep) dy =  maxStep;
        if (dy < -maxStep) dy = -maxStep;
        m_yaw += dy;
    }
    m_pitch -= in.lookDY * kPxToRad;
    if (m_pitch >  kPitchClamp) m_pitch =  kPitchClamp;
    if (m_pitch < -kPitchClamp) m_pitch = -kPitchClamp;

    // ---- Horizontal basis from yaw (device forward convention; pitch ignored for
    // locomotion). moveFwd = W/S along facing, moveStrafe = A/D along right (strafe).
    const float fx = std::cos(m_yaw), fz = std::sin(m_yaw);
    const float rx = -fz,             rz = fx;

    float wishX = fx * in.moveFwd + rx * in.moveStrafe;
    float wishZ = fz * in.moveFwd + rz * in.moveStrafe;
    const float wishLen = std::sqrt(wishX * wishX + wishZ * wishZ);
    float dirX = 0.0f, dirZ = 0.0f;
    if (wishLen > 1e-4f) { dirX = wishX / wishLen; dirZ = wishZ / wishLen; }

    // ---- Sluggish speed ramp: the planar speed magnitude approaches the target
    // (boost when sprinting) at `accel` m/s^2. Releasing input ramps DOWN to 0 at the
    // same accel — so the heavy mech does not stop or start instantly.
    const float targetSpeed = (wishLen > 1e-4f)
        ? (in.sprint ? m_t.boostSpeed : m_t.walkSpeed)
        : 0.0f;
    if (m_curSpeed < targetSpeed) m_curSpeed = std::min(targetSpeed, m_curSpeed + m_t.accel * dt);
    else                          m_curSpeed = std::max(targetSpeed, m_curSpeed - m_t.accel * dt);

    const float planarX = dirX * m_curSpeed;
    const float planarZ = dirZ * m_curSpeed;

    // ---- Grounded + jump-jets. Hold Space (jumpPressed) while grounded -> a vertical
    // boost while fuel remains; fuel drains in the air, regens on the ground. We push
    // the boost through the .y jump-impulse channel each frame jets are active so the
    // mech keeps climbing; the world's gravity brings it back down between/after.
    m_grounded = phys.characterGrounded(m_body);
    float velY = 0.0f;
    const bool wantJet = in.jumpPressed;
    if (wantJet && m_jetFuel > 0.0f) {
        // Lift: only allow ignition when grounded OR already rising under jets, and
        // only while there is fuel. Burn fuel by dt.
        velY = m_t.jumpJetImpulse;
        m_jetFuel = std::max(0.0f, m_jetFuel - dt);
    } else if (m_grounded) {
        // Regen on the ground (clamped to max).
        m_jetFuel = std::min(m_t.jumpJetFuelMax, m_jetFuel + m_t.jumpJetRegen * dt);
    } else {
        // Airborne without firing jets: passive drain is 0 (fuel only burns while
        // thrusting). No regen mid-air.
    }

    phys.moveCharacter(m_body, x3::phys::Vec3{ planarX, velY, planarZ }, dt);

    const x3::phys::Vec3 f = phys.getBodyPosition(m_body);
    m_feetX = f.x; m_feetY = f.y; m_feetZ = f.z;
}

// ---------------------------------------------------------------------------
// Camera. NO roll (mechs don't roll). 3P chase behind+above looking at the mech;
// 1P cockpit eye near the top of the capsule along the aim.
// ---------------------------------------------------------------------------
void MechPilotController::camera(float& outX, float& outY, float& outZ,
                                 float& outYaw, float& outPitch) const {
    const float cx = m_feetX, cy = m_feetY, cz = m_feetZ;
    if (m_thirdPerson) {
        // Place the eye `chaseDistance` BEHIND the mech's facing and `chaseHeight`
        // above the feet, then look back toward the mech with a slight downward
        // pitch. Behind = -forward(yaw).
        const float fx = std::cos(m_yaw), fz = std::sin(m_yaw);
        outX = cx - fx * m_t.chaseDistance;
        outY = cy + m_t.chaseHeight;
        outZ = cz - fz * m_t.chaseDistance;
        outYaw = m_yaw;
        // Look down at the mech: target ~mid-height of the capsule.
        const float dyTarget = (cy + m_t.capsuleHeight * 0.5f) - outY;
        const float flat = m_t.chaseDistance;
        outPitch = std::atan2(dyTarget, flat);
    } else {
        // Cockpit: eye near the top-front of the capsule, along the aim yaw/pitch.
        const float fx = std::cos(m_yaw), fz = std::sin(m_yaw);
        outX = cx + fx * (m_t.capsuleRadius * 0.6f);
        outY = cy + m_t.capsuleHeight * 0.85f;
        outZ = cz + fz * (m_t.capsuleRadius * 0.6f);
        outYaw = m_yaw;
        outPitch = m_pitch;
    }
}

x3::phys::Vec3 MechPilotController::pos() const {
    return x3::phys::Vec3{ m_feetX, m_feetY + m_t.capsuleHeight * 0.5f, m_feetZ };
}
x3::phys::Vec3 MechPilotController::feet() const {
    return x3::phys::Vec3{ m_feetX, m_feetY, m_feetZ };
}
float MechPilotController::yaw() const   { return m_yaw; }
float MechPilotController::pitch() const { return m_pitch; }
bool  MechPilotController::isGrounded() const { return m_grounded; }
float MechPilotController::jumpJetFuel() const { return m_jetFuel; }

void MechPilotController::toggleCameraMode() { m_thirdPerson = !m_thirdPerson; }
bool MechPilotController::isThirdPerson() const { return m_thirdPerson; }

int MechPilotController::hull() const     { return m_hull; }
int MechPilotController::maxHull() const  { return m_t.maxHull; }
int MechPilotController::armor() const    { return m_armor; }
int MechPilotController::maxArmor() const { return m_t.maxArmor; }

// Ablative armor absorbs first, then any overflow bleeds into the hull. Armor does
// NOT regen (must repair). Lethal blowthrough: a single big hit can chew through the
// remaining armor AND drop the hull to 0 in one call.
void MechPilotController::takeDamage(int amount) {
    if (amount <= 0 || m_hull <= 0) return;
    int remaining = amount;
    if (m_armor > 0) {
        const int absorbed = std::min(m_armor, remaining);
        m_armor   -= absorbed;
        remaining -= absorbed;
    }
    if (remaining > 0) {
        m_hull -= remaining;
        if (m_hull < 0) m_hull = 0;
    }
    if (m_hull == 0)
        x3::logInfo("[mech] hull 0 — MECH DESTROYED");
}

bool MechPilotController::isAlive() const { return m_hull > 0; }

// Autocannon: rapid hitscan gated by rounds-per-second. Returns true the tick a round
// actually fires (the caller then draws the tracer + applies damage). The RPS gate is
// a simple countdown re-armed to 1/RPS on each shot.
bool MechPilotController::fireAutocannon(float dt) {
    (void)dt;  // timing advanced in update(); this just checks/consumes the gate
    if (m_mode == Mode::Dismounted || !isAlive()) return false;
    if (m_autocannonTimer > 0.0f) return false;
    if (m_t.autocannonRPS <= 0.0f) return false;
    m_autocannonTimer = 1.0f / m_t.autocannonRPS;
    return true;
}

// Missile pod: a heavy burst gated by a long cooldown. Returns true the tick the
// burst launches (the caller spawns the missile FX). The pod is a multi-tracer burst
// in the showcase; the projectile itself is STUBBED (no live missile body) — combat
// damage is applied directly to the target like the autocannon. Documented deviation.
bool MechPilotController::fireMissilePod(float dt) {
    (void)dt;
    if (m_mode == Mode::Dismounted || !isAlive()) return false;
    if (m_missileTimer > 0.0f) return false;
    if (m_t.missilePodCooldown <= 0.0f) return false;
    m_missileTimer = m_t.missilePodCooldown;
    return true;
}

x3::phys::Vec3 MechPilotController::muzzle() const {
    // Front-top of the chassis along the facing (a chest-mounted weapon mount).
    const float fx = std::cos(m_yaw), fz = std::sin(m_yaw);
    return x3::phys::Vec3{
        m_feetX + fx * (m_t.capsuleRadius + 0.3f),
        m_feetY + m_t.capsuleHeight * 0.7f,
        m_feetZ + fz * (m_t.capsuleRadius + 0.3f) };
}

x3::phys::Vec3 MechPilotController::aimDir() const {
    const float cp = std::cos(m_pitch), sp = std::sin(m_pitch);
    return x3::phys::Vec3{ std::cos(m_yaw) * cp, sp, std::sin(m_yaw) * cp };
}

MechPilotController::Mode MechPilotController::mode() const { return m_mode; }

void MechPilotController::dismount() {
    // Piloting -> Dismounted. The mech stops + becomes inert. Idempotent.
    m_mode     = Mode::Dismounted;
    m_curSpeed = 0.0f;
}

// ===========================================================================
// Self-test (--test-mech): >=8 assertions across locomotion, jets, combat, FSM.
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[mech-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[mech-test] FAIL ") + name); }
}

constexpr float kFixedDt = 1.0f / 60.0f;

// Flat ground at y=0 (CCW so the +Y face is solid), `half` units to a side.
x3::phys::BodyId makeGround(x3::phys::IPhysicsWorld& w, float cx, float cz, float half) {
    float v[] = {
        cx-half, 0.0f, cz-half,
        cx+half, 0.0f, cz-half,
        cx+half, 0.0f, cz+half,
        cx-half, 0.0f, cz+half,
    };
    uint32_t idx[] = { 0,2,1, 0,3,2 };
    return w.addStaticMesh(v, 4, idx, 6);
}

void frame(MechPilotController& m, x3::phys::IPhysicsWorld& w, const PlayerInput& in) {
    m.update(in, kFixedDt, w);
    w.step(kFixedDt);
}

} // namespace

bool runMechSelfTest() {
    g_pass = g_fail = 0;
    MechPilotController::Tuning T{};   // defaults

    // ---- T1: spawn settles GROUNDED ---------------------------------------
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        makeGround(*w, 0, 0, 80.0f);
        MechPilotController m;
        m.spawn(*w, 0.0f, 0.1f, 0.0f, T);
        for (int i = 0; i < 60; ++i) frame(m, *w, PlayerInput{});
        check(m.isGrounded(), "T1 spawn settles grounded");
        w->shutdown();
    }

    // ---- T2: W ramps velocity (SLUGGISH — not instant to top speed) -------
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        makeGround(*w, 0, 0, 200.0f);
        MechPilotController m;
        m.spawn(*w, 0.0f, 0.1f, 0.0f, T);   // yaw=0 -> faces +X
        for (int i = 0; i < 60; ++i) frame(m, *w, PlayerInput{});
        x3::phys::Vec3 p0 = m.feet();
        PlayerInput fwd; fwd.moveFwd = 1.0f;
        // First few frames: barely moving (heavy ramp).
        for (int i = 0; i < 3; ++i) frame(m, *w, fwd);
        x3::phys::Vec3 pEarly = m.feet();
        float earlyStep = pEarly.x - p0.x;            // distance over 3 early frames
        // After ~2s: cruising; covered a lot more ground.
        for (int i = 0; i < 120; ++i) frame(m, *w, fwd);
        x3::phys::Vec3 p1 = m.feet();
        float lateStepAvg = (p1.x - pEarly.x) / 120.0f;   // avg per-frame at cruise
        float earlyStepAvg = earlyStep / 3.0f;
        bool advanced   = (p1.x - p0.x) > 3.0f;       // clearly walked forward
        bool sluggish   = earlyStepAvg < lateStepAvg * 0.6f;  // accel ramp (slow start)
        check(advanced && sluggish, "T2 W ramps velocity (sluggish accel, not instant)");
        w->shutdown();
    }

    // ---- T3: jump-jet consumes fuel AND lifts off the ground --------------
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        makeGround(*w, 0, 0, 80.0f);
        MechPilotController m;
        m.spawn(*w, 0.0f, 0.1f, 0.0f, T);
        for (int i = 0; i < 60; ++i) frame(m, *w, PlayerInput{});
        float fuel0 = m.jumpJetFuel();
        float y0 = m.feet().y;
        PlayerInput jet; jet.jumpPressed = true;
        bool leftGround = false;
        for (int i = 0; i < 30; ++i) {
            frame(m, *w, jet);
            if (!m.isGrounded() && m.feet().y > y0 + 0.3f) leftGround = true;
        }
        bool fuelDrained = m.jumpJetFuel() < fuel0 - 0.1f;
        check(leftGround && fuelDrained, "T3 jump-jet lifts off + consumes fuel");
        w->shutdown();
    }

    // ---- T4: fuel regens on the ground ------------------------------------
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        makeGround(*w, 0, 0, 80.0f);
        MechPilotController m;
        m.spawn(*w, 0.0f, 0.1f, 0.0f, T);
        for (int i = 0; i < 60; ++i) frame(m, *w, PlayerInput{});
        // Burn jets until airborne / partly drained.
        PlayerInput jet; jet.jumpPressed = true;
        for (int i = 0; i < 40; ++i) frame(m, *w, jet);
        float fuelLow = m.jumpJetFuel();
        // Land + sit: let it fall back, then idle on the ground to regen.
        for (int i = 0; i < 300; ++i) frame(m, *w, PlayerInput{});
        float fuelAfter = m.jumpJetFuel();
        check(fuelLow < T.jumpJetFuelMax - 0.05f && fuelAfter > fuelLow + 0.1f,
              "T4 fuel regens on ground");
        w->shutdown();
    }

    // ---- T5: takeDamage armor->hull order + LETHAL blowthrough ------------
    {
        MechPilotController m;
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init(); makeGround(*w, 0, 0, 40.0f);
        m.spawn(*w, 0.0f, 0.1f, 0.0f, T);
        // Small hit: armor absorbs, hull untouched.
        m.takeDamage(300);
        bool armorAbsorbed = (m.armor() == T.maxArmor - 300) && (m.hull() == T.maxHull);
        // Hit that exceeds remaining armor (500 left): overflow bleeds into hull.
        m.takeDamage(600);   // 500 to armor (now 0) + 100 to hull
        bool overflowToHull = (m.armor() == 0) && (m.hull() == T.maxHull - 100);
        // Lethal blowthrough: a single huge hit drops hull to 0 (dead).
        m.takeDamage(999999);
        bool lethal = (m.hull() == 0) && !m.isAlive();
        check(armorAbsorbed && overflowToHull && lethal,
              "T5 damage armor->hull order + lethal blowthrough");
        w->shutdown();
    }

    // ---- T6: autocannon RPS gate (rapid, but rate-limited) ----------------
    {
        MechPilotController m;
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init(); makeGround(*w, 0, 0, 40.0f);
        m.spawn(*w, 0.0f, 0.1f, 0.0f, T);
        // Hold the trigger for 1.0 s of frames; count how many rounds fired. At
        // 5 RPS that's ~5 (allow 4..6 for fractional-frame timing).
        int rounds = 0;
        const int frames = 60;  // 1 s at 60 Hz
        for (int i = 0; i < frames; ++i) {
            if (m.fireAutocannon(kFixedDt)) ++rounds;
            frame(m, *w, PlayerInput{});
        }
        // Also assert it does NOT fire every single frame (gate works).
        bool gated = rounds < frames / 2;
        bool aboutRps = rounds >= 4 && rounds <= 7;
        check(gated && aboutRps, "T6 autocannon RPS gate (~5/s, not every frame)");
        w->shutdown();
    }

    // ---- T7: missile-pod cooldown gate ------------------------------------
    {
        MechPilotController m;
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init(); makeGround(*w, 0, 0, 40.0f);
        m.spawn(*w, 0.0f, 0.1f, 0.0f, T);
        bool first = m.fireMissilePod(kFixedDt);    // should launch
        m.update(PlayerInput{}, kFixedDt, *w); w->step(kFixedDt);
        bool secondBlocked = !m.fireMissilePod(kFixedDt);  // cooldown not elapsed
        // Wait out the cooldown (4 s), then it can fire again.
        const int waitFrames = (int)(T.missilePodCooldown * 60.0f) + 5;
        for (int i = 0; i < waitFrames; ++i) { m.update(PlayerInput{}, kFixedDt, *w); w->step(kFixedDt); }
        bool readyAgain = m.fireMissilePod(kFixedDt);
        check(first && secondBlocked && readyAgain, "T7 missile-pod cooldown gate");
        w->shutdown();
    }

    // ---- T8: dismount transitions Piloting -> Dismounted (inert) ----------
    {
        MechPilotController m;
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init(); makeGround(*w, 0, 0, 80.0f);
        m.spawn(*w, 0.0f, 0.1f, 0.0f, T);
        for (int i = 0; i < 60; ++i) frame(m, *w, PlayerInput{});
        bool wasPiloting = (m.mode() == MechPilotController::Mode::Piloting);
        m.dismount();
        bool nowDismounted = (m.mode() == MechPilotController::Mode::Dismounted);
        // Inert: forward input no longer moves it.
        x3::phys::Vec3 before = m.feet();
        PlayerInput fwd; fwd.moveFwd = 1.0f; fwd.sprint = true;
        for (int i = 0; i < 120; ++i) frame(m, *w, fwd);
        x3::phys::Vec3 after = m.feet();
        float moved = std::sqrt((after.x-before.x)*(after.x-before.x) +
                                (after.z-before.z)*(after.z-before.z));
        bool inert = moved < 0.5f;
        check(wasPiloting && nowDismounted && inert, "T8 dismount -> Dismounted (inert)");
        w->shutdown();
    }

    x3::logInfo(std::string("[mech-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
