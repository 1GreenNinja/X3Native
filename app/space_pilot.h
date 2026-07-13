#pragma once
// 6DOF space-flight character controller — Act 3 ("Beyond the Stars") foundation.
//
// Game/slice code only — engine/ stays pure. The SpacePilotController is the
// player's avatar WHEN IN SPACE (Act 3 ~L36-75, the space tier per the EFLZ
// design corpus). Unlike Player (CharacterVirtual capsule + gravity, walks on
// the ground) or SwimController (capsule + buoyancy in a water column), space
// flight needs arbitrary 6DOF inertia-driven motion with NO gravity and a full
// quaternion ship orientation — the wrong shape for the character-controller
// path. So the controller wraps a Jolt KINEMATIC-style body (an addSphere
// dynamic body driven entirely by the controller — setBodyPosition +
// setBodyRotation each frame; gravity is irrelevant because Jolt's gravity is
// integrated only by character/dynamic bodies whose linear vel we don't read
// back, and we re-clamp the position ourselves) and we drive the pose
// manually each frame.
//
// CLEAN-ROOM, original work. Built from the public IPhysicsWorld interface +
// app/player.h (Tim's own PlayerInput abstraction). No RBDOOM / id Tech / Doom
// / Quake engine source consulted.
//
// Input is abstracted (PlayerInput, declared in app/player.h) so the controller
// is testable headlessly with synthetic input + no GLFW/Vulkan — see
// runSpaceSelfTest() driven by --test-space. PlayerInput does NOT carry a roll
// channel (rest of the game has no need), so the windowed showcase loop reads
// Q/E directly via GLFW and passes the roll axis to update() via the dedicated
// setRollInput() seam (kept off the abstracted struct so the rest of the game
// is undisturbed).

#include "engine/physics/IPhysicsWorld.h"
#include "player.h"   // PlayerInput

namespace x3::game {

class SpacePilotController {
public:
    // ----- Tuning (everything the host may want to override at spawn time) ----
    // 6DOF accel / drag, weapon energy, hull/shield two-pool damage, default
    // camera. Reasonable cinematic-arcade defaults; the host can pass a tuned
    // struct in via spawn(...).
    struct Tuning {
        float maxLinearAccel  = 25.0f;  // m/s^2 along forward (W/S)
        float maxStrafeAccel  = 12.0f;  // m/s^2 along right/up (A/D + Space/Ctrl)
        float maxAngularAccel = 3.5f;   // rad/s^2 (mouse + Q/E roll)
        float linearDrag      = 0.05f;  // per-second (light cinematic drag)
        float angularDrag     = 1.5f;   // per-second (snappier rotational settle)
        float boostMul        = 2.5f;   // sprint -> accel multiplier (eats energy)
        float maxSpeed        = 220.0f; // m/s hard speed cap
        float noseFollow      = 0.0f;   // arcade steering: velocity-direction chase
                                        // rate toward facing (1/s). 0 = pure
                                        // Newtonian drift (existing behavior).
        int   maxHull         = 1000;
        int   maxShield       = 500;
        float shieldRegenPerSec = 25.0f;
        float shieldRegenDelay  = 4.0f; // sec after a hit before shield ticks again
        float maxEnergy        = 100.0f;
        float energyRegenPerSec = 12.0f;
        bool  defaultThirdPerson = true;
        float chaseDistance    = 12.0f; // 3P chase camera distance behind ship
        float chaseHeight      = 4.0f;  // 3P chase camera height above ship
    };

    // ---- Lifecycle ---------------------------------------------------------
    // Spawn the ship at world position (x,y,z) facing +X, level (no pitch/roll).
    // Builds an underlying sphere body (kinematic-ish — we drive it manually).
    void spawn(x3::phys::IPhysicsWorld& phys, float x, float y, float z, const Tuning& t = {});

    // Advance one frame: integrate look (yaw from lookDX, pitch from lookDY,
    // roll from setRollInput()), build accel from W/S/A/D + Space/Ctrl,
    // integrate velocity + drag + speed cap, integrate position, advance shield
    // regen + energy regen + hit timers. ALSO calls physics.setBodyPosition +
    // setBodyRotation so the body tracks the controller. Does NOT step the
    // physics world (the caller does that afterwards, like Player::update).
    void update(const PlayerInput& in, float dt, x3::phys::IPhysicsWorld& phys);

    // Off-channel roll input (Q/E in the showcase). Buffered into the next
    // update(). Cleared automatically on the frame consumed.
    void setRollInput(float axis) { m_rollAxis = axis; }

    // ---- Camera ------------------------------------------------------------
    // Eye-space camera state for IRenderDevice::setCamera. In 1P (cockpit) the
    // eye sits at the ship origin with a small forward offset; in 3P (chase)
    // the eye is offset BEHIND + ABOVE along the ship's local axes.
    void camera(float& outX, float& outY, float& outZ, float& outYaw, float& outPitch) const;

    // ROLL-CAPABLE camera: full orientation basis from the ship's quaternion, so
    // the view banks + loops with the fighter (feed IRenderDevice::setCameraBasis).
    // 1P sits at the nose; 3P chases behind + above, both rolling with the hull.
    void cameraBasis(float outPos[3], float outFwd[3], float outUp[3]) const;

    // 1P / 3P toggle (showcase binds it to V).
    void toggleCameraMode();
    bool isThirdPerson() const { return m_thirdPerson; }

    // ---- Read-only state ---------------------------------------------------
    x3::phys::Vec3 pos() const      { return x3::phys::Vec3{ m_pos[0], m_pos[1], m_pos[2] }; }
    x3::phys::Vec3 velocity() const { return x3::phys::Vec3{ m_vel[0], m_vel[1], m_vel[2] }; }
    x3::phys::Vec3 forward() const;     // ship local +X in world space
    x3::phys::Vec3 right() const;       // ship local +Z in world space (right wing)
    x3::phys::Vec3 up() const;          // ship local +Y in world space (cockpit roof)
    float speed() const;                // m/s, magnitude of velocity()
    float yaw() const   { return m_yaw; }
    float pitch() const { return m_pitch; }
    float roll() const  { return m_roll; }

    // ---- Combat / health ---------------------------------------------------
    int   hull() const     { return m_hull; }
    int   maxHull() const  { return m_tuning.maxHull; }
    int   shield() const   { return m_shield; }
    int   maxShield() const{ return m_tuning.maxShield; }
    float energy() const   { return m_energy; }
    float maxEnergy() const{ return m_tuning.maxEnergy; }

    // Apply damage with shield-first-then-hull two-pool order. Resets the
    // shield-regen-delay timer. No-op when already dead (hull == 0). Negative
    // / zero amounts are ignored.
    void takeDamage(int amount);
    bool isAlive() const   { return m_hull > 0; }

    // Fire a laser bolt. Returns true iff the shot actually fired (off cooldown,
    // enough energy). When true, drains kLaserEnergy and starts the cooldown.
    // The showcase / host wires this up to call combatFx.addTracer(muzzle, hit)
    // on the returned true. `dt` advances the per-frame cooldown timer.
    bool fireLaser(float dt);

    // Missile launch — v1 stub. Always returns false; documented to keep the
    // API stable while Task #15+ (homing missile) lands.
    bool fireMissile(float dt);

    // Internal cooldown step (called from update + fireLaser to drain timers
    // toward zero). Exposed for tests.
    void tickCooldowns(float dt);

private:
    Tuning m_tuning{};
    bool   m_spawned = false;

    // Underlying physics body — driven each frame (setBodyPosition + rotation).
    x3::phys::BodyId m_body;

    // World-space pose (we own the truth; the body is a follower for queries
    // against the physics world). pos in world meters.
    float m_pos[3] = { 0, 0, 0 };
    float m_vel[3] = { 0, 0, 0 };

    // Orientation: stored as a quaternion (x,y,z,w) per CONVENTIONS.md so we
    // accumulate roll cleanly without gimbal-locking. We also keep Euler
    // (yaw/pitch/roll) updated from input for HUD readback + camera basis.
    float m_quat[4] = { 0, 0, 0, 1 };  // identity
    float m_angVel[3] = { 0, 0, 0 };   // body-local angular velocity (rad/s)
    float m_yaw   = 0;                 // around world +Y
    float m_pitch = 0;                 // around ship local +Z (after yaw)
    float m_roll  = 0;                 // around ship local +X (forward)
    float m_rollAxis = 0;              // buffered Q/E this frame

    // Camera mode (1P cockpit vs 3P chase). Default per Tuning.defaultThirdPerson.
    bool  m_thirdPerson = true;

    // Health / energy state (mutated by takeDamage / fireLaser).
    int   m_hull   = 0;
    int   m_shield = 0;
    float m_energy = 0;
    float m_shieldRegenTimer = 0; // counts down to 0 then shield ticks up
    float m_laserCd = 0;          // seconds remaining on the laser cooldown
};

// ---- --test-space self-test (≥7 sub-checks, no window/Vulkan) ---------------
// Builds a minimal IPhysicsWorld + drives the controller with synthetic input;
// asserts (1) spawn, (2) W/S accelerates along forward, (3) mouse-Y rotates
// pitch, (4) Q/E rolls, (5) speed cap holds, (6) takeDamage shield→hull order,
// (7) energy drain on fireLaser + refuse at 0 energy, (8) toggleCameraMode
// 1P↔3P. Logs PASS/FAIL T#, returns true iff all pass.
bool runSpaceSelfTest();

} // namespace x3::game
