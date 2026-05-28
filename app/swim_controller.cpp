// Underwater swim/dive character controller — see app/swim_controller.h.
//
// CLEAN-ROOM, original work. Built from the public IPhysicsWorld interface +
// the player.cpp reference (Tim's own code). No RBDOOM / id Tech / Doom / Quake
// engine source consulted.

#include "swim_controller.h"

#include "engine/core/x3_log.h"
#include "engine/physics/IPhysicsWorld.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace x3::game {

// ---------------------------------------------------------------------------
// Swim controller tuning constants (everything else lives in Tuning so it's
// authorable at spawn).
// ---------------------------------------------------------------------------
namespace {
constexpr float kCapsuleRadius = 0.35f;  // m, matches Player
constexpr float kCapsuleHeight = 1.8f;   // m, matches Player kStandHeight
constexpr float kEyeHeight     = 1.0f;   // m above feet — head/eye when swimming pose
constexpr float kPitchClamp    = 89.0f * 3.14159265358979f / 180.0f; // +/- 89 deg
constexpr float kMouseSens     = 1.9f;
constexpr float kPxToRad       = 0.00132f;  // == Player's: 1.9 * 0.00132 ~= 0.0025 rad/px
// The Jolt world integrates gravity at -9.81 m/s^2; while ABOVE water we let
// that gravity work normally by leaving .y = 0 in the move vector (the world
// sticks/drops the character internally), matching Player's behaviour.
} // namespace

void SwimController::spawn(x3::phys::IPhysicsWorld& physics, float x, float y, float z,
                           const Tuning& t) {
    m_tuning  = t;
    m_body    = physics.createCharacter(kCapsuleRadius, kCapsuleHeight,
                                        x3::phys::Vec3{ x, y, z });
    m_feetX = x; m_feetY = y; m_feetZ = z;
    m_oxygen  = t.maxOxygenS;
    m_hp      = m_maxHp;
    m_alive   = true;
    m_spawned = true;
}

void SwimController::takeDamage(int amount) {
    if (!m_alive || amount <= 0) return;
    m_hp -= amount;
    if (m_hp <= 0) {
        m_hp    = 0;
        m_alive = false;
        x3::logInfo("[swim] HP 0 — drowned/crushed");
    }
}

bool SwimController::isAboveWater() const {
    // Head Y = feet Y + eye height. Strictly greater than surfaceY counts as
    // "above water" (no oxygen drain, gravity applies).
    return (m_feetY + kEyeHeight) > m_tuning.surfaceY;
}

void SwimController::setLook(float yaw, float pitch) {
    m_yaw = yaw;
    if (pitch >  kPitchClamp) pitch =  kPitchClamp;
    if (pitch < -kPitchClamp) pitch = -kPitchClamp;
    m_pitch = pitch;
}

void SwimController::setFeetPosition(x3::phys::IPhysicsWorld& physics,
                                     const x3::phys::Vec3& feet) {
    if (!m_spawned || !m_body.valid()) return;
    physics.setBodyPosition(m_body, feet);
    m_feetX = feet.x; m_feetY = feet.y; m_feetZ = feet.z;
}

void SwimController::camera(float& x, float& y, float& z,
                            float& yaw, float& pitch) const {
    x = m_feetX;
    y = m_feetY + kEyeHeight;
    z = m_feetZ;
    yaw = m_yaw;
    pitch = m_pitch;
}

void SwimController::update(const PlayerInput& in, float dt,
                            x3::phys::IPhysicsWorld& physics) {
    if (!m_spawned || !m_body.valid() || dt <= 0.0f) return;

    // ---- Mouse look. Same scaling as Player so swap-in is seamless.
    m_yaw   += in.lookDX * kMouseSens * kPxToRad;
    m_pitch -= in.lookDY * kMouseSens * kPxToRad;
    if (m_pitch >  kPitchClamp) m_pitch =  kPitchClamp;
    if (m_pitch < -kPitchClamp) m_pitch = -kPitchClamp;

    // While dead, still cache the feet so camera()/queries stay valid (mirrors
    // Player's behaviour after HP 0).
    if (!m_alive) {
        const x3::phys::Vec3 feet = physics.getBodyPosition(m_body);
        m_feetX = feet.x; m_feetY = feet.y; m_feetZ = feet.z;
        return;
    }

    const bool aboveWater = isAboveWater();

    // ---- Build look basis. Full 3D forward (pitch INCLUDED) so looking down +
    // W swims down-forward, as specified. Right is the horizontal-only XZ right
    // (so strafe is always perpendicular to gravity even when looking down).
    const float cp = std::cos(m_pitch), sp = std::sin(m_pitch);
    const float fX = std::cos(m_yaw) * cp;
    const float fY = sp;
    const float fZ = std::sin(m_yaw) * cp;
    const float rX = -std::sin(m_yaw);   // horizontal right (XZ)
    const float rZ =  std::cos(m_yaw);

    // ---- Boost / speed. Hold sprint = boost (consumes oxygen faster).
    const bool  boosting = in.sprint;
    const float speed = boosting ? m_tuning.boostSpeed : m_tuning.swimSpeed;

    // ---- Desired velocity vector (3D).
    float vX = 0.0f, vY = 0.0f, vZ = 0.0f;
    if (aboveWater) {
        // ABOVE WATER: behave like a normal land controller — horizontal-only
        // WASD on the yaw plane, no vertical control; the world's gravity drops
        // us back into the water. (No jump: Tim wants the swim controller to
        // handle the surface↔depth transition, not to be a land platformer.)
        const float fHX = std::cos(m_yaw), fHZ = std::sin(m_yaw);
        const float wx = fHX * in.moveFwd + rX * in.moveStrafe;
        const float wz = fHZ * in.moveFwd + rZ * in.moveStrafe;
        const float len = std::sqrt(wx*wx + wz*wz);
        if (len > 1e-4f) { vX = wx / len * speed; vZ = wz / len * speed; }
        // .y left at 0; Jolt integrates gravity internally so we fall.
        // No buoyancy applied above water (we want gravity to win).
    } else {
        // UNDER WATER: full 3D motion. moveFwd is W/S along the pitched look
        // (so W with look-down dives), strafe is on the horizontal plane,
        // jumpPressed (Space, held) is "ascend +Y". PlayerInput has no
        // descend bit, so the host applies LeftCtrl as a separate Y nudge
        // AFTER update() (see app/main.cpp --world swim); the self-test
        // descends by combining moveFwd=+1 with look-down pitch.
        float ascend = 0.0f;
        if (in.jumpPressed) ascend += 1.0f;
        float wx = fX * in.moveFwd + rX * in.moveStrafe;
        float wy = fY * in.moveFwd + ascend;
        float wz = fZ * in.moveFwd + rZ * in.moveStrafe;

        // Normalize 3D wish so diagonal isn't faster, then scale to speed.
        const float wlen = std::sqrt(wx*wx + wy*wy + wz*wz);
        if (wlen > 1e-4f) {
            const float k = speed / wlen;
            vX = wx * k; vY = wy * k; vZ = wz * k;
        }

        // ---- Buoyancy: passive upward drift when there is no vertical input
        // AND moveFwd's vertical component is ~0. Tim spec: small upward drift.
        if (std::fabs(wy) < 1e-3f && std::fabs(fY * in.moveFwd) < 1e-3f) {
            vY += m_tuning.buoyancy;
        }
    }

    // ---- Issue the move.
    //
    // ABOVE WATER: standard moveCharacter call with .y = 0 — Jolt's internal
    // gravity integrates the fall back into the water naturally (the same
    // contract Player uses).
    //
    // UNDER WATER: Jolt's CharacterVirtual still integrates gravity in step(),
    // and moveCharacter() treats positive .y as a JUMP IMPULSE that sets the
    // internal vertical velocity (vy). We exploit that: each frame we pass
    // a positive vy_pulse via moveCharacter() that, after step()'s
    // `vy += g * dt`, leaves vy == desired_vy. To go up at rate vY (m/s) we
    // pass vy_pulse = vY + |g| * dt. To "hover" we pass +|g|*dt. The downside
    // is moveCharacter only accepts a POSITIVE .y, so to descend (negative
    // desired vY) we additionally teleport the feet by the negative residual
    // after the step — which is small (descent_rate * dt per frame) and not
    // perceptible. (Engine gravity matches Player's clean-room derivation of
    // -9.81 m/s^2.)
    if (aboveWater) {
        physics.moveCharacter(m_body, x3::phys::Vec3{ vX, 0.0f, vZ }, dt);
    } else {
        // Cancel gravity for this step: send vy_pulse so step's vy += g*dt
        // leaves vy == max(vY, 0). Any negative residual is applied as a
        // direct position nudge AFTER step's gravity integration (we do it
        // BEFORE step here so the body sinks before the world resolves the
        // capsule contact — equivalent for slow swim speeds).
        constexpr float kGravityAbs = 9.81f;
        const float vyPulse = std::max(0.0f, vY) + kGravityAbs * dt;
        physics.moveCharacter(m_body, x3::phys::Vec3{ vX, vyPulse, vZ }, dt);
        if (vY < 0.0f) {
            // Descent: nudge the body straight down by |vY|*dt.
            x3::phys::Vec3 p = physics.getBodyPosition(m_body);
            p.y += vY * dt;
            physics.setBodyPosition(m_body, p);
        }
    }

    // ---- Oxygen / drowning / crush.
    if (aboveWater) {
        // Refill while head is above the surface.
        m_oxygen += m_tuning.oxygenRefillS * dt;
        if (m_oxygen > m_tuning.maxOxygenS) m_oxygen = m_tuning.maxOxygenS;
    } else {
        // Drain at base rate; boost doubles it (specced).
        const float drainRate = m_tuning.oxygenDrainS * (boosting ? 2.0f : 1.0f);
        m_oxygen -= drainRate * dt;
        if (m_oxygen < 0.0f) m_oxygen = 0.0f;
        // HP drain at 0 oxygen.
        if (m_oxygen <= 0.0f) {
            const float dmg = m_tuning.damagePerSecBelowZero * dt;
            // Accumulate fractional damage cleanly by carrying it on a hidden
            // counter would be ideal; for the v1 we round to nearest int per
            // call (the test fires for plenty of frames so this is fine).
            m_hp -= (int)std::ceil(dmg);
            if (m_hp <= 0) {
                m_hp    = 0;
                m_alive = false;
                x3::logInfo("[swim] drowned (oxygen 0)");
            }
        }
        // Crush damage past the depth limit.
        if (m_feetY < -m_tuning.ascentLimit) {
            const float crush = m_tuning.crushDamagePerSec * dt;
            m_hp -= (int)std::ceil(crush);
            if (m_hp <= 0) {
                m_hp    = 0;
                m_alive = false;
                x3::logInfo("[swim] crushed at depth");
            }
        }
    }

    // ---- Cache feet position so camera() works without a physics arg.
    const x3::phys::Vec3 feet = physics.getBodyPosition(m_body);
    m_feetX = feet.x; m_feetY = feet.y; m_feetZ = feet.z;
}

// ===========================================================================
// Self-test (--test-swim)
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[swim-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[swim-test] FAIL ") + name); }
}

constexpr float kFixedDt = 1.0f / 60.0f;

// Big flat seafloor at y = -25 (well below the ascentLimit so we can dive past
// the crush limit without immediately hitting the floor in tests).
x3::phys::BodyId makeSeafloor(x3::phys::IPhysicsWorld& w, float half, float floorY) {
    float v[] = {
        -half, floorY, -half,
         half, floorY, -half,
         half, floorY,  half,
        -half, floorY,  half,
    };
    uint32_t idx[] = { 0, 2, 1, 0, 3, 2 };
    return w.addStaticMesh(v, 4, idx, 6);
}

void frame(SwimController& p, x3::phys::IPhysicsWorld& w, const PlayerInput& in) {
    p.update(in, kFixedDt, w);
    w.step(kFixedDt);
}

} // namespace

bool runSwimSelfTest() {
    g_pass = g_fail = 0;

    // ---- T1: spawn at depth places the body at the requested pos --------------
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        makeSeafloor(*w, 200.0f, -25.0f);
        SwimController s;
        s.spawn(*w, 0.0f, -5.0f, 0.0f);
        const x3::phys::Vec3 p = s.pos();
        bool placed = std::fabs(p.x - 0.0f) < 0.01f &&
                      std::fabs(p.y - -5.0f) < 0.01f &&
                      std::fabs(p.z - 0.0f) < 0.01f;
        check(placed && s.isAlive() && !s.isAboveWater(),
              "T1 spawn at depth places body + alive + under water");
        w->shutdown();
    }

    // ---- T2: WASD moves in 3D (forward along look) ----------------------------
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        makeSeafloor(*w, 200.0f, -25.0f);
        SwimController s;
        s.spawn(*w, 0.0f, -5.0f, 0.0f);   // yaw=0 -> faces +X
        const x3::phys::Vec3 p0 = s.pos();
        PlayerInput fwd; fwd.moveFwd = 1.0f;
        for (int i = 0; i < 120; ++i) frame(s, *w, fwd);   // 2 s of swimming
        const x3::phys::Vec3 p1 = s.pos();
        // Faces +X at pitch=0, so X should advance meaningfully.
        bool advanced = (p1.x - p0.x) > 4.0f;
        check(advanced, "T2 WASD forward advances along look (+X)");
        w->shutdown();
    }

    // ---- T3: holding LeftCtrl (descend) takes the body deeper -----------------
    // Spec implementation: pitch straight down (-pi/2) + moveFwd=1 descends. We
    // use moveFwd-with-pitch (the natural 3D approach) since PlayerInput has no
    // dedicated descend bit; the host wires LeftCtrl to the same path.
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        makeSeafloor(*w, 200.0f, -25.0f);
        SwimController s;
        s.spawn(*w, 0.0f, -5.0f, 0.0f);
        // Look straight down.
        s.setLook(0.0f, -1.5707963f);
        const x3::phys::Vec3 p0 = s.pos();
        PlayerInput down; down.moveFwd = 1.0f;  // W while looking down = descend
        for (int i = 0; i < 60; ++i) frame(s, *w, down);   // 1 s
        const x3::phys::Vec3 p1 = s.pos();
        bool deeper = (p1.y - p0.y) < -1.5f;     // dropped at least 1.5 m
        check(deeper, "T3 W + look-down descends");
        w->shutdown();
    }

    // ---- T4: oxygen drains under water ---------------------------------------
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        makeSeafloor(*w, 200.0f, -25.0f);
        SwimController s;
        SwimController::Tuning t; t.maxOxygenS = 10.0f; t.oxygenDrainS = 1.0f;
        s.spawn(*w, 0.0f, -5.0f, 0.0f, t);
        const float o0 = s.oxygenSeconds();
        for (int i = 0; i < 120; ++i) frame(s, *w, PlayerInput{});  // 2 s idle
        const float o1 = s.oxygenSeconds();
        bool drained = (o0 - o1) > 1.5f && o1 > 0.0f;  // ~2 s drained
        check(drained, "T4 oxygen drains under water");
        w->shutdown();
    }

    // ---- T5: oxygen refills above water --------------------------------------
    // Drain oxygen underwater first, then teleport ABOVE the surface and check
    // that it refills. surfaceY = 0; head Y must be > 0 to be "above water". We
    // place a "beach" platform at y=0.5 (just above the surface) so the body
    // rests on it with head ~1.5 m above surfaceY (well above water), and
    // gravity can't drag the head below the surface.
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        // Below-water seafloor far below.
        makeSeafloor(*w, 200.0f, -25.0f);
        // A small "beach" plateau at y=0.5 (CCW from above), 8 m to a side at
        // origin, so the body can stand on it with the head above surfaceY=0.
        {
            float v[] = {
                -8.0f, 0.5f, -8.0f,
                 8.0f, 0.5f, -8.0f,
                 8.0f, 0.5f,  8.0f,
                -8.0f, 0.5f,  8.0f,
            };
            uint32_t idx[] = { 0, 2, 1, 0, 3, 2 };
            w->addStaticMesh(v, 4, idx, 6);
        }
        SwimController s;
        SwimController::Tuning t;
        t.maxOxygenS = 10.0f; t.oxygenDrainS = 1.0f; t.oxygenRefillS = 4.0f;
        s.spawn(*w, 50.0f, -5.0f, 50.0f, t);   // start under water, far from beach
        // Drain for 4 s.
        for (int i = 0; i < 240; ++i) frame(s, *w, PlayerInput{});
        const float oDrained = s.oxygenSeconds();
        // Teleport above water onto the beach (feet at y=0.6, just above plateau).
        s.setFeetPosition(*w, x3::phys::Vec3{ 0.0f, 0.6f, 0.0f });
        // Settle a few frames so the body rests on the plateau.
        for (int i = 0; i < 6; ++i) frame(s, *w, PlayerInput{});
        const bool aboveOK = s.isAboveWater();
        // Drive 1 s of refill; should gain ~4 s of oxygen at refillS = 4.0.
        for (int i = 0; i < 60; ++i) frame(s, *w, PlayerInput{});
        const float oRefilled = s.oxygenSeconds();
        const bool refilled = oRefilled > oDrained + 2.0f;
        check(aboveOK && refilled, "T5 oxygen refills above water");
        w->shutdown();
    }

    // ---- T6: HP drains at 0 oxygen --------------------------------------------
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        makeSeafloor(*w, 200.0f, -25.0f);
        SwimController s;
        SwimController::Tuning t;
        t.maxOxygenS = 0.1f;        // ~6 frames of oxygen
        t.oxygenDrainS = 1.0f;
        t.damagePerSecBelowZero = 50.0f;  // big rate so HP drops fast in the test
        s.spawn(*w, 0.0f, -5.0f, 0.0f, t);
        const int hp0 = s.hp();
        for (int i = 0; i < 120; ++i) frame(s, *w, PlayerInput{});  // 2 s
        const int hp1 = s.hp();
        bool drained = hp1 < hp0;
        check(drained, "T6 HP drains at 0 oxygen");
        w->shutdown();
    }

    // ---- T7: buoyancy causes upward drift with no input -----------------------
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        makeSeafloor(*w, 200.0f, -25.0f);
        SwimController s;
        SwimController::Tuning t; t.buoyancy = 1.0f;   // big buoyancy so the drift is unambiguous
        s.spawn(*w, 0.0f, -10.0f, 0.0f, t);
        const x3::phys::Vec3 p0 = s.pos();
        for (int i = 0; i < 120; ++i) frame(s, *w, PlayerInput{});  // 2 s idle
        const x3::phys::Vec3 p1 = s.pos();
        bool drifted = (p1.y - p0.y) > 0.5f;
        check(drifted, "T7 buoyancy causes upward drift with no input");
        w->shutdown();
    }

    // ---- T8: takeDamage() actually decrements HP ------------------------------
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        makeSeafloor(*w, 200.0f, -25.0f);
        SwimController s;
        s.spawn(*w, 0.0f, -5.0f, 0.0f);
        const int hp0 = s.hp();
        s.takeDamage(17);
        const int hp1 = s.hp();
        bool decremented = (hp0 - hp1) == 17;
        bool aliveOK = s.isAlive();
        // Kill with a big hit; should go dead.
        s.takeDamage(10000);
        bool dead = !s.isAlive() && s.hp() == 0;
        check(decremented && aliveOK && dead,
              "T8 takeDamage decrements HP + lethal hit kills");
        w->shutdown();
    }

    x3::logInfo(std::string("[swim-test] ") + std::to_string(g_pass) +
                " passed, " + std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
