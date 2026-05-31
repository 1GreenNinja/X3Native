#pragma once
// app/space/eva.h
//
// S12 EVA spacewalk — zero-G exterior locomotion (thruster pack + mag-boots),
// the player's avatar WHEN OUTSIDE THE SHIP, on the hull, in the `EVA` context
// (space-engine design spec §2.9 / S12). The ship is at rest/drifting; the
// player exits an airlock and free-floats outside to repair the hull.
//
// REUSE: this is ~80% the shipped SwimController (app/swim_controller.h). The
// 3D free-move + boost + oxygen of swimming maps almost 1:1 to a thruster pack
// + boost + O2 in vacuum. We ADAPTED (copied the pattern, did NOT modify the
// swim controller):
//   * buoyancy  -> zero-G : no passive upward drift; instead an OPTIONAL tiny
//                  configurable drift (Tuning::driftAccel, default 0 = true 0-G).
//   * swim stroke (speed-clamped velocity) -> thruster-pack IMPULSE: WASD +
//                  ascend/descend + boost build an acceleration, integrated
//                  into a persisted velocity that COASTS in vacuum (no water
//                  drag to bleed it off) and is clamped to maxSpeed.
//   * surface/depth water transition -> MAG-BOOTS: when `magBoots` is on AND a
//                  hull surface is within `magStickDist` (downward ray probe),
//                  the player STICKS to the surface (drift killed, motion
//                  becomes surface-relative "hull walking" along the surface
//                  tangent — a simplified clamp to the surface normal).
//   * drown/crush damage -> SUFFOCATION: oxygen drains in vacuum, and at 0 O2
//                  HP bleeds (damagePerSecNoO2), same as swim's drowning model.
//
// Game/slice code only — engine/ stays pure. Wraps a Jolt kinematic
// CharacterVirtual (via IPhysicsWorld::createCharacter) exactly like Player /
// SwimController. No GRAVITY is wanted (vacuum), so — like the swim controller
// did below the surface — we pre-compensate Jolt's internal gravity each move.
//
// CLEAN-ROOM. Built from the public IPhysicsWorld interface + the player.cpp /
// swim_controller.cpp references (Tim's own code). No RBDOOM / id Tech / Doom /
// Quake engine source consulted.
//
// Input is abstracted (PlayerInput, from app/player.h) so the controller is
// testable headlessly with synthetic input + no GLFW/Vulkan — see
// runEvaSelfTest() driven by --test-eva. Mirrors runSwimSelfTest().

#include "engine/physics/IPhysicsWorld.h"
#include "../player.h"   // PlayerInput (app/player.h — this header lives in app/space/)

namespace x3::space {

// The EVA controller reuses the swim controller's abstracted input struct
// (x3::game::PlayerInput, app/player.h) verbatim — WASD + sprint(boost) +
// jump(ascend) + mouse delta map 1:1 onto the thruster pack. Alias it into this
// namespace so EVA code reads naturally without modifying player.h.
using PlayerInput = x3::game::PlayerInput;

// EVA suit HP. Mirrors the swim/land HP scale (kPlayerMaxHp == 100) so a
// suffocation/impact hit is on the same scale as land + swim combat.
constexpr int kEvaMaxHp = 100;

// First-person zero-G EVA controller. Wraps an underlying physics character
// (capsule) with 6DOF thruster motion + oxygen + suffocation + mag-boots.
// Camera is at feet + eye offset (same convention as Player/SwimController) so
// the host camera math is unchanged.
class EVAController {
public:
    // Movement / oxygen tuning. Defaults targeted at Act-3 EVA hull-repair pacing.
    struct Tuning {
        float thrustAccel  = 3.0f;    // m/s^2 — WASD/up-down thruster acceleration
        float boostAccel   = 6.0f;    // m/s^2 — hold-shift boost (drains O2 faster)
        float maxSpeed     = 5.0f;    // m/s — velocity clamp (vacuum coasting)
        float maxOxygenS   = 120.0f;  // s — EVA suits carry more air than a swim
        float oxygenDrainS = 1.0f;    // s of O2 drained per real second (boost doubles)
        float damagePerSecNoO2 = 4.0f;// hp/s drained when O2 has hit 0 (suffocation)
        bool  magBoots     = false;   // when true + near surface, stick/hull-walk
        float magStickDist = 1.2f;    // m — hull-surface proximity to engage mag-boots
        // Optional passive drift (the zero-G analogue of buoyancy). 0 == TRUE
        // zero-G (no passive drift, the spec's default). A tiny non-zero value
        // gives a faint "the ship is rotating under you" pull.
        float driftAccel   = 0.0f;    // m/s^2 along -Y when no input + not stuck
        // Velocity bleed (1/s) applied each frame even in vacuum. Real vacuum is
        // 0; a small value (e.g. the boot RCS auto-stabilizer) makes hand-flying
        // less twitchy. 0 == pure ballistic coasting.
        float damping      = 0.6f;    // per-second fractional velocity bleed
    };

    // Create the character capsule at feet position (x,y,z). Body is a kinematic
    // Player-style capsule (radius/height match Player's standing capsule).
    // Gravity is suppressed every move (vacuum). Call once.
    void spawn(x3::phys::IPhysicsWorld& physics, float x, float y, float z,
               const Tuning& t = {});

    // Advance one frame: integrate look (yaw/pitch from mouse delta), build a 3D
    // thruster acceleration (W/S along look forward; A/D along right; Space
    // ascend; jumpPressed/down via host; LeftShift boost), integrate it into a
    // persisted velocity that COASTS (clamped to maxSpeed), drain O2, drain HP at
    // 0 O2, and — if mag-boots are engaged — clamp to the nearby hull surface
    // ("walk on hull"). Calls moveCharacter(); does NOT call physics.step() (the
    // caller advances the world so all bodies advance together).
    void update(const PlayerInput& in, float dt, x3::phys::IPhysicsWorld& physics);

    // Eye-height camera state for IRenderDevice::setCamera. Position is the
    // capsule feet + a 1.0 m eye offset (matches the swim controller's tucked
    // EVA-suit pose); yaw/pitch are the look angles (radians) in the device's
    // forward convention.
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

    // Current oxygen reserve (seconds). 0 == suffocating (HP drains).
    float oxygenSeconds()    const { return m_oxygen; }
    float maxOxygenSeconds() const { return m_tuning.maxOxygenS; }

    // ---- Mag-boots --------------------------------------------------------
    // Toggle the mag-boot capability. When ON and a hull surface is within
    // magStickDist (probed each update), the boots ENGAGE: passive drift is
    // killed, free-float thrust is damped, and motion becomes surface-relative
    // (simplified hull-walk — clamp to the surface, glide along its tangent).
    void  setMagBoots(bool on);
    bool  magBootsEnabled() const { return m_tuning.magBoots; }
    // True iff the boots are ON *and* currently STUCK to a hull surface (i.e.
    // mag-boots actually engaged this frame). False while free-floating.
    bool  magBootsActive() const { return m_stuck; }

    // ---- Hull-repair interaction (v1 stub) --------------------------------
    // The full repair minigame is S7's domain (interior). Here we expose just
    // the EXTERIOR proximity + a stub: a repair point is registered at a world
    // position; the controller flags when the suit is within reach.
    //
    // Register (or move) the single active hull-repair point at a world pos with
    // a reach radius. (v1: one point; a vector can come later.)
    void setRepairPoint(const x3::phys::Vec3& worldPos, float reach = 1.5f);
    // True iff the suit (feet) is within the active repair point's reach.
    bool nearRepairPoint() const { return m_nearRepair; }
    // Attempt a repair tick at the active point. Returns true iff in range (and
    // thus a repair "tick" landed). v1 just reports proximity success; the real
    // minigame wiring is S7's job. Safe to call every frame.
    bool tryRepair();

    // The underlying physics character body (for debug / queries).
    x3::phys::BodyId body() const { return m_body; }

    // Direct setters for tests (not normally used at runtime).
    void setLook(float yaw, float pitch);
    void setFeetPosition(x3::phys::IPhysicsWorld& physics, const x3::phys::Vec3& feet);
    // Read/clear the persisted velocity (tests assert coasting).
    x3::phys::Vec3 velocity() const { return x3::phys::Vec3{ m_velX, m_velY, m_velZ }; }

private:
    x3::phys::BodyId m_body;
    Tuning           m_tuning{};

    float m_yaw   = 0.0f;
    float m_pitch = 0.0f;

    // Cached feet position (camera() takes no world arg).
    float m_feetX = 0.0f, m_feetY = 0.0f, m_feetZ = 0.0f;

    // Persisted thruster velocity (m/s). Coasts in vacuum (no water drag).
    float m_velX = 0.0f, m_velY = 0.0f, m_velZ = 0.0f;

    // Oxygen + HP state.
    float m_oxygen = 0.0f;   // initialized to maxOxygenS on spawn()
    int   m_hp     = kEvaMaxHp;
    int   m_maxHp  = kEvaMaxHp;
    bool  m_alive  = true;
    bool  m_spawned = false;

    // Mag-boot stuck state (true the frame the boots are engaged to a surface).
    bool  m_stuck = false;

    // Hull-repair point (v1: single point).
    bool  m_hasRepair  = false;
    bool  m_nearRepair = false;
    float m_repairX = 0.0f, m_repairY = 0.0f, m_repairZ = 0.0f, m_repairReach = 1.5f;
};

// Headless self-test (--test-eva). Asserts (at minimum):
//   T1 spawn places the body at the requested pos + alive + full O2.
//   T2 WASD thrusts in 3D (forward along look) — moves in vacuum.
//   T3 boost (Shift) accelerates faster than base thrust.
//   T4 oxygen drains in vacuum.
//   T5 HP drains at 0 oxygen (suffocation).
//   T6 takeDamage() actually decrements HP (+ lethal hit kills).
//   T7 NO passive gravity fall — with no input the body does not sink (zero-G).
//   T8 mag-boots: toggle ON near a hull surface engages (magBootsActive) and
//      changes the movement mode (kills free drift / clamps to the surface).
// Returns true iff all pass. Logs PASS/FAIL T# and an "eva-test: X/Y passed"
// summary. Mirrors runSwimSelfTest().
bool runEvaSelfTest();

} // namespace x3::space
