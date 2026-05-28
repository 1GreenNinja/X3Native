#pragma once
// Rideable heavy-mech pilot controller (mech pass).
//
// Game/slice code only — engine/ stays pure. The MechPilotController wraps a Jolt
// CharacterVirtual (via IPhysicsWorld::createCharacter) like the Player, but tuned
// for a stompy ~3-4 m heavy mech: a big capsule, sluggish acceleration, slow yaw,
// heavier gravity, jump-jets (a fuel-gated vertical boost), and a two-pool combat
// model (ablative armor that does NOT regen, absorbed BEFORE hull). It exposes a
// 3rd-person chase camera (default — mechs look best 3P) with a 1P cockpit toggle.
//
// STRUCTURAL REFERENCE: this mirrors the SpacePilotController / Player controller
// SHAPE (spawn/update/camera/state-queries + a --test-mech + a --world mech
// showcase), but is a DIFFERENT controller — ground-based heavy locomotion, not
// 6DOF flight. It does NOT modify Player; it reuses the proven character-capsule
// pattern from app/player.cpp.
//
// Input is abstracted (PlayerInput, declared in player.h) so the controller is
// testable headlessly with synthetic input and no GLFW/Vulkan — see runMechSelfTest.

#include "engine/physics/IPhysicsWorld.h"
#include "player.h"   // PlayerInput

namespace x3::game {

class MechPilotController {
public:
    struct Tuning {
        // Heavy ground locomotion — stompy, high mass, slow accel
        float walkSpeed      = 4.0f;    // m/s
        float boostSpeed     = 8.0f;    // dash/boost
        float accel          = 6.0f;    // m/s^2 — deliberately sluggish (heavy)
        float turnRate       = 1.2f;    // rad/s yaw (mechs turn slow)
        float jumpJetImpulse = 9.0f;    // jump-jet vertical
        float jumpJetFuelMax = 3.0f;    // seconds of jet
        float jumpJetRegen   = 1.0f;    // sec/sec on ground
        float gravity        = -18.0f;  // heavier-feeling gravity
        float capsuleRadius  = 1.2f;    // big collision (~3-4m tall mech)
        float capsuleHeight  = 3.6f;
        // Combat — mech-scale
        int   maxHull        = 2000;
        int   maxArmor       = 800;     // ablative armor absorbs before hull
        float armorRegenPerSec = 0.0f;  // armor does NOT regen (must repair)
        int   autocannonDmg  = 40;
        float autocannonRPS  = 5.0f;    // rounds/sec
        int   missilePodDmg  = 120;
        float missilePodCooldown = 4.0f;
        // Camera
        bool  defaultThirdPerson = true;  // mechs look best 3P
        float chaseDistance  = 14.0f;
        float chaseHeight    = 6.0f;
    };

    // Create the mech capsule at feet position (x,y,z). Call once.
    void spawn(x3::phys::IPhysicsWorld& phys, float x, float y, float z, const Tuning& t = {});

    // Advance one frame: integrate look/turn, build a sluggish desired velocity,
    // run jump-jets, manage combat cooldowns, issue moveCharacter(). Does NOT call
    // physics.step() (the caller steps the world afterwards). A no-op while
    // Dismounted (the mech becomes inert).
    void update(const PlayerInput& in, float dt, x3::phys::IPhysicsWorld& phys);

    // Camera state for IRenderDevice::setCamera (NO roll — mechs don't roll). In
    // 3rd-person this is a chase point behind+above the mech looking at it; in 1st
    // person it is a cockpit eye near the top of the capsule looking along yaw/pitch.
    void camera(float& outX, float& outY, float& outZ, float& outYaw, float& outPitch) const;

    phys::Vec3 pos() const;     // capsule CENTER world position
    phys::Vec3 feet() const;    // capsule feet world position
    float yaw() const;
    float pitch() const;
    bool  isGrounded() const;
    float jumpJetFuel() const;  // remaining jet fuel (seconds)

    void  toggleCameraMode();
    bool  isThirdPerson() const;

    int   hull() const;        int maxHull() const;
    int   armor() const;       int maxArmor() const;
    void  takeDamage(int amount);   // armor absorbs first, then hull
    bool  isAlive() const;          // hull > 0

    bool  fireAutocannon(float dt); // rapid, returns true if fired this tick (RPS gate)
    bool  fireMissilePod(float dt); // burst, returns true if launched (cooldown ok)

    // Muzzle origin (world) for the weapon tracer/FX, and the current aim
    // direction (unit, from yaw/pitch) — used by the showcase to draw tracers.
    phys::Vec3 muzzle() const;
    phys::Vec3 aimDir() const;

    // State machine
    enum class Mode { Piloting, Dismounted };
    Mode  mode() const;
    void  dismount();   // Piloting -> Dismounted (mech stops, becomes inert)

    const Tuning& tuning() const { return m_t; }

private:
    Tuning           m_t{};
    x3::phys::BodyId m_body;
    bool             m_spawned = false;

    float m_yaw   = 0.0f;   // around +Y; 0 looks toward +X (heading the mech walks/faces)
    float m_pitch = 0.0f;   // up/down aim; clamped

    bool  m_grounded   = false;
    float m_jetFuel    = 0.0f;   // remaining jump-jet fuel (s)
    float m_velY       = 0.0f;   // vertical velocity carried for jet lift-off
    float m_curSpeed   = 0.0f;   // current planar speed magnitude (ramps via accel)

    // Cached feet position from the last update()/spawn() (camera() has no world arg).
    float m_feetX = 0.0f, m_feetY = 0.0f, m_feetZ = 0.0f;

    bool  m_thirdPerson = true;
    Mode  m_mode        = Mode::Piloting;

    // Combat two-pool: ablative armor absorbs first, then hull. Armor does NOT regen.
    int   m_hull  = 0;
    int   m_armor = 0;

    float m_autocannonTimer = 0.0f;   // counts down to next allowed autocannon round
    float m_missileTimer    = 0.0f;   // counts down to next allowed missile burst
};

// Headless self-test (--test-mech). Builds its own physics world (floor) and drives
// synthetic input. Asserts: spawn grounded; W ramps velocity (sluggish, not instant);
// jump-jet consumes fuel + lifts off; fuel regens on ground; takeDamage armor->hull
// order + lethal blowthrough; autocannon RPS gate; missile-pod cooldown gate; dismount
// transition. Logs PASS/FAIL T#, returns true iff all pass. Mirrors runPlayerSelfTest().
bool runMechSelfTest();

} // namespace x3::game
