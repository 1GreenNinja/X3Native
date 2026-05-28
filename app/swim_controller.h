#pragma once
// Underwater swim/dive character controller (Act 4 tier8 undersea foundation).
//
// Game/slice code only — engine/ stays pure. The SwimController is the player's
// avatar WHEN IN/UNDER water (sharks, whale, giant squid, manta ray, undersea
// base in Act 4). It wraps a Jolt CharacterVirtual (via
// IPhysicsWorld::createCharacter) like Player does, but the controller drives
// FULL 3D motion (forward/strafe + up/down) with passive buoyancy + oxygen +
// pressure (crush) damage. Surface transition: the SAME controller handles
// above- and below-water motion (a feet+headOffset > surfaceY check restores
// land-like gravity behaviour above the surface, no oxygen drain).
//
// CLEAN-ROOM. Built from the public IPhysicsWorld interface + the player.cpp
// reference. Does NOT modify Player. No RBDOOM / id Tech / Doom / Quake engine
// source consulted.
//
// Input is abstracted (PlayerInput, defined in app/player.h) so the controller
// is testable headlessly with synthetic input + no GLFW/Vulkan — see
// runSwimSelfTest() driven by --test-swim.

#include "engine/physics/IPhysicsWorld.h"
#include "player.h"   // PlayerInput

namespace x3::game {

// ---------------------------------------------------------------------------
// Swim health tuning (Act 4 undersea tier). Mirrors Player's kPlayerMaxHp so a
// drowning/crush hit is on the same scale as land combat — a swim run can
// transition back to land combat without an HP scale jump.
// ---------------------------------------------------------------------------
constexpr int kSwimMaxHp = 100;

// First-person swim/dive controller. Wraps an underlying physics character
// (capsule) with 3D motion + oxygen + crush. Camera is at feet + eye offset
// (same convention as Player) so the host camera math is unchanged.
class SwimController {
public:
    // Movement / oxygen tuning. Defaults targeted at Act 4 tier8 sea fauna pacing.
    struct Tuning {
        float swimSpeed     = 3.5f;   // m/s — 3D freelook strafe/forward/up-down
        float boostSpeed    = 6.0f;   // m/s — hold-shift sprint (drains oxygen faster)
        float buoyancy      = 0.05f;  // m/s passive upward drift when no vertical input
        float maxOxygenS    = 90.0f;  // seconds before drowning
        float oxygenDrainS  = 1.0f;   // s drained per real second under water (boost doubles)
        float oxygenRefillS = 4.0f;   // s refilled per real second when head above water
        float damagePerSecBelowZero = 5.0f; // hp/s drained when oxygen has hit 0
        float surfaceY      = 0.0f;   // world-space water level Y; head above => above water
        float ascentLimit   = 18.0f;  // crush damage begins deeper than -ascentLimit (DCS)
        // The crush damage rate matches the drown rate, for simplicity (Tim's call:
        // both "you are dying" timers feel the same on the HUD).
        float crushDamagePerSec = 5.0f;
    };

    // Create the character capsule at feet position (x,y,z) with the given Tuning.
    // The body is a kinematic Player-style capsule (radius/height match Player's
    // standing capsule). Gravity is suppressed when the controller is BELOW the
    // surface; above the surface it lets the world's gravity reapply via a normal
    // moveCharacter() call (the controller falls back into the surface like a
    // jumped-out player). Call once.
    void spawn(x3::phys::IPhysicsWorld& physics, float x, float y, float z,
               const Tuning& t = {});

    // Advance one frame: integrate look (yaw/pitch from mouse delta), build 3D
    // swim desired velocity (W/S along the look forward; A/D along right; Space
    // ascend; LeftCtrl descend; LeftShift boost), apply buoyancy, drain/refill
    // oxygen, drain HP at 0 oxygen, apply crush damage past `ascentLimit`. Calls
    // moveCharacter(); does NOT call physics.step() (the caller advances the
    // world so all bodies advance together).
    void update(const PlayerInput& in, float dt, x3::phys::IPhysicsWorld& physics);

    // Eye-height camera state for IRenderDevice::setCamera. Position is the
    // capsule feet + a 1.0 m eye offset (slightly below Player's 1.6 m because
    // the body is "vertical" but the swim pose tilts the head closer to center);
    // yaw/pitch are the look angles (radians).
    void camera(float& x, float& y, float& z, float& yaw, float& pitch) const;

    // Cached feet (capsule reference) world position from the last update()/spawn().
    x3::phys::Vec3 pos() const { return x3::phys::Vec3{ m_feetX, m_feetY, m_feetZ }; }

    // Look angles (radians).
    float yaw() const   { return m_yaw; }
    float pitch() const { return m_pitch; }

    // Health.
    int  hp() const    { return m_hp; }
    int  maxHp() const { return m_maxHp; }
    void takeDamage(int amount);
    bool isAlive() const { return m_alive; }

    // True when the swimmer's HEAD (feet + eye offset) is at/above the water
    // surface (Tuning::surfaceY). Above water: oxygen refills, gravity applies,
    // crush damage cannot accumulate.
    bool isAboveWater() const;

    // Current oxygen reserve (seconds). 0 == drowning (HP drains).
    float oxygenSeconds()    const { return m_oxygen; }
    float maxOxygenSeconds() const { return m_tuning.maxOxygenS; }

    // The underlying physics character body (for debug / queries).
    x3::phys::BodyId body() const { return m_body; }

    // Direct setters for tests (not normally used at runtime).
    void setLook(float yaw, float pitch);
    void setFeetPosition(x3::phys::IPhysicsWorld& physics, const x3::phys::Vec3& feet);

private:
    x3::phys::BodyId m_body;
    Tuning           m_tuning{};

    float m_yaw   = 0.0f;
    float m_pitch = 0.0f;

    // Cached feet position (camera() takes no world arg).
    float m_feetX = 0.0f, m_feetY = 0.0f, m_feetZ = 0.0f;

    // Oxygen + HP state.
    float m_oxygen = 0.0f;   // initialized to maxOxygenS on spawn()
    int   m_hp     = kSwimMaxHp;
    int   m_maxHp  = kSwimMaxHp;
    bool  m_alive  = true;
    bool  m_spawned = false;
};

// Headless self-test (--test-swim). Asserts (at minimum):
//   T1 spawn at depth places body at requested pos.
//   T2 WASD moves in 3D (forward along look).
//   T3 holding LeftCtrl (descend) takes the body deeper.
//   T4 oxygen drains under water.
//   T5 oxygen refills above water (Y > surfaceY).
//   T6 HP drains at 0 oxygen.
//   T7 buoyancy causes upward drift with no input.
//   T8 takeDamage() actually decrements HP.
// Returns true iff all pass. Logs PASS/FAIL T# and a "swim-test: X/Y passed"
// summary. Mirrors runPlayerSelfTest().
bool runSwimSelfTest();

} // namespace x3::game
