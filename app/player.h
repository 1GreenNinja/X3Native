#pragma once
// First-person walking character controller (S3).
//
// Game/slice code only — engine/ stays pure. The Player wraps a Jolt
// CharacterVirtual (via IPhysicsWorld::createCharacter) and turns abstract
// PlayerInput into a desired horizontal velocity + jump, manages mouse-look
// (yaw/pitch), and exposes an eye-height camera. Movement parameters are the
// values extracted in docs/ASSET_INVENTORY.md "Character Controller Parameters".
//
// Input is abstracted (PlayerInput) so the controller is testable headlessly
// with synthetic input and no GLFW/Vulkan — see --test-player.
//
// S4-S6 build on this: use camera() forward for the weapon raycast, body() for
// physics queries, and grounded() for gameplay state.

#include "engine/physics/IPhysicsWorld.h"

namespace x3::game {

// One frame of abstracted player input. Decoupled from GLFW so the controller
// can be driven by synthetic input in tests.
struct PlayerInput {
    float moveFwd    = 0;      // -1..1  (W = +1, S = -1) along facing
    float moveStrafe = 0;      // -1..1  (D = +1, A = -1) along right
    bool  sprint     = false;  // hold to move at sprint speed
    bool  jumpPressed = false; // rising edge only (true the frame Space goes down)
    float lookDX = 0;          // mouse delta X this frame (pixels)
    float lookDY = 0;          // mouse delta Y this frame (pixels)
};

class Player {
public:
    // Create the character capsule at feet position (x,y,z). Call once.
    void spawn(x3::phys::IPhysicsWorld& physics, float x, float y, float z);

    // Advance one frame: integrate look, build desired velocity, manage coyote
    // time + jump buffer, issue moveCharacter(). Does NOT call physics.step()
    // (the caller steps the world afterwards so all bodies advance together).
    void update(const PlayerInput& in, float dt, x3::phys::IPhysicsWorld& physics);

    // Eye-height camera state for IRenderDevice::setCamera. Position is the
    // capsule feet + eye height; yaw/pitch are the look angles (radians) in the
    // device's forward convention: fwd = (cos p cos y, sin p, cos p sin y).
    void camera(float& x, float& y, float& z, float& yaw, float& pitch) const;

    // Grounded this frame (cached from the last update()).
    bool grounded() const { return m_grounded; }

    // The underlying physics character body.
    x3::phys::BodyId body() const { return m_body; }

    // Look angles (radians). Exposed for tests / debug HUD.
    float yaw() const { return m_yaw; }
    float pitch() const { return m_pitch; }

private:
    x3::phys::BodyId m_body;
    float m_yaw   = 0.0f;   // around +Y; 0 looks toward +X
    float m_pitch = 0.0f;   // up/down; clamped to +/- kPitchClamp

    bool  m_grounded   = false;
    float m_coyote     = 0.0f;   // time-since-grounded countdown (s)
    float m_jumpBuffer = 0.0f;   // remaining jump-buffer window (s)
    bool  m_spawned    = false;

    // Cached feet position from the last update() (camera() has no world arg).
    float m_feetX = 0.0f, m_feetY = 0.0f, m_feetZ = 0.0f;
    // Horizontal velocity carried between frames for air control blending.
    float m_lastHorizX = 0.0f, m_lastHorizZ = 0.0f;
};

// Headless self-test (T1 walk, T2 wall-stop, T3 jump, T4 coyote). Builds its own
// physics world (floor + wall) and drives synthetic input. Logs PASS/FAIL T#.
// Returns true iff all pass. Mirrors runPhysicsSelfTest() et al.
bool runPlayerSelfTest();

} // namespace x3::game
