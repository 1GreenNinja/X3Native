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

#include <string>

namespace x3::game {

// ---- Flight modes ----------------------------------------------------------
// Three named feel presets (see SpacePilotController::preset). ARCADE is the
// default (Star Fox: snappy, forgiving, strong auto-bank/level). ASSIST is the
// Elite-style weighty-inertia glide. LOOSE is the drifty/adrenaline high-top-
// speed profile. Each maps to a fully-populated Tuning + per-mode juice
// strengths; setMode() swaps the FEEL fields live without touching health.
enum class FlightMode { Arcade = 0, Assist = 1, Loose = 2 };

// Parse a mode name ("arcade"/"assist"/"loose", case-insensitive). Returns true
// and writes *out on a match; false (leaves *out untouched) otherwise.
bool parseFlightMode(const std::string& name, FlightMode& out);
const char* flightModeName(FlightMode m);   // "Arcade" / "Assist" / "Loose"

// PROCESS-GLOBAL "requested" flight mode. The console command + Settings menu
// live in the DEFAULT game host (app_run.cpp); the actual --world space flight
// runs in a SEPARATE standalone host (host_space.cpp) that never builds that
// console/menu. This shared latch is the bridge: any selection surface writes
// it, and host_space reads it at spawn + polls it each frame so a change flows
// to the live ship. Single-threaded game code — a plain static suffices.
FlightMode requestedFlightMode();
void        setRequestedFlightMode(FlightMode m);

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
        // ---- ANTIMATTER BOOST (Shift): a HARD kick well past cruise ----------
        // boostAccelMul stacks ON TOP of boostMul while Shift is held (total
        // forward accel = maxLinearAccel * boostMul * boostAccelMul) so the ship
        // LEAPS the instant boost lands. boostSpeedCapMul raises the speed cap to
        // maxSpeed * boostSpeedCapMul while boosting; on release the overspeed
        // bleeds off smoothly toward maxSpeed (see update()) instead of snapping.
        // Defaults 1.0/1.0 = NO boost overspeed (every existing default-Tuning
        // caller + the --test-space cap check behave exactly as before).
        float boostAccelMul    = 1.0f;  // extra accel mult while boosting (x boostMul)
        float boostSpeedCapMul = 1.0f;  // speed-cap mult while boosting (x maxSpeed)
        float noseFollow      = 0.0f;   // arcade steering: velocity-direction chase
                                        // rate toward facing (1/s). 0 = pure
                                        // Newtonian drift (existing behavior).
        int   maxHull         = 1000;
        int   maxShield       = 500;
        float shieldRegenPerSec = 25.0f;
        float shieldRegenDelay  = 4.0f; // sec after a hit before shield ticks again
        float maxEnergy        = 100.0f;
        float energyRegenPerSec = 12.0f;
        // Per-shot laser energy cost. Default 8 = the historical kLaserEnergy
        // constant (every existing caller + --test-space T7 unchanged). The
        // dogfight beats LOWER it + raise regen so the pool reads as a
        // REGENERATING weapon-energy bank: a long sustained burst (~9 s), a
        // fast recovery (~5 s), never a long dry lockout (owner: "it runs out
        // after a short time!") — the charge-pool model, not an ammo clip.
        float laserEnergyCost  = 8.0f;
        bool  defaultThirdPerson = true;
        float chaseDistance    = 12.0f; // 3P chase camera distance behind ship
        float chaseHeight      = 4.0f;  // 3P chase camera height above ship

        // ---- Shared SMOOTH / JUICE layer (tuned per FlightMode) -------------
        // These drive presentation + assist, NOT the core sim integration, so
        // swapping them mid-flight is deterministic and never desyncs velocity.
        float lookSmoothing  = 20.0f;   // yaw/pitch ease rate toward the raw-input
                                        // target (1/s). Higher = snappier / less
                                        // lag; lower = weightier. Frame-rate
                                        // independent (1-exp(-rate*dt)).
        float autoBank       = 0.0f;    // roll banked INTO a yaw turn: target roll
                                        // (rad) per (rad/s) of yaw rate. 0 = none.
        float maxBank        = 0.6f;    // clamp on the auto-bank target (rad).
        float autoLevel      = 2.0f;    // hands-off ease rate of roll toward the
                                        // bank target / level (1/s). ~0 = never
                                        // self-levels (LOOSE); high = snaps flat.
        float fovBase        = 65.0f;   // FOV (deg) at rest.
        float fovMax         = 80.0f;   // FOV (deg) at full speed + boost punch.
        float chaseFollow    = 10.0f;   // 3P camera position ease rate (1/s).
                                        // Higher = tighter/rigid; lower = laggy.
        float lookAhead      = 0.06f;   // 3P look-ahead: meters of camera lead per
                                        // (m/s) of speed along the velocity dir.
        float shakeAmp       = 0.05f;   // screen-shake amplitude (meters / ~rad)
                                        // at full boost+accel. Deterministic noise.
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

    // ---- Flight mode -------------------------------------------------------
    // Build the fully-populated feel Tuning for a mode (static — no instance
    // state; the presets are defined in space_pilot.cpp).
    static Tuning preset(FlightMode m);

    // Swap the LIVE flight mode. Copies the mode's FEEL fields (accel/drag/
    // speed/nose-follow + the whole smooth/juice layer) into m_tuning, leaving
    // the combat/health fields (maxHull/shield/energy + current pools) intact so
    // hot-swapping mid-flight never resets the ship. Also updates the shared
    // requestedFlightMode() latch so all selection surfaces agree.
    void setMode(FlightMode m);
    FlightMode mode() const { return m_mode; }
    const Tuning& tuning() const { return m_tuning; }

    // The TRUE ceiling while the antimatter boost is held (maxSpeed *
    // boostSpeedCapMul). Speed/maxSpeed HUD + FX fractions can exceed 1.0 during
    // an overspeed boost; consumers that want a fraction that stays in [0,1]
    // should divide by this instead of tuning().maxSpeed. Const + cheap.
    float boostedMaxSpeed() const { return m_tuning.maxSpeed * m_tuning.boostSpeedCapMul; }

    // ---- Camera ------------------------------------------------------------
    // Eye-space camera state for IRenderDevice::setCamera. In 1P (cockpit) the
    // eye sits at the ship origin with a small forward offset; in 3P (chase)
    // the eye is offset BEHIND + ABOVE along the ship's local axes.
    void camera(float& outX, float& outY, float& outZ, float& outYaw, float& outPitch) const;

    // FOV (degrees) widened by speed fraction (speed()/maxSpeed) + a boost
    // punch, between Tuning.fovBase and Tuning.fovMax. The space host feeds this
    // straight into IRenderDevice::setCamera(...) so the field-of-view swells
    // with velocity (the "speed rush"). Cheap + const; safe to call per frame.
    float fov() const;

    // ROLL-CAPABLE camera: full orientation basis from the ship's quaternion, so
    // the view banks + loops with the fighter (feed IRenderDevice::setCameraBasis).
    // 1P sits at the nose; 3P chases behind + above, both rolling with the hull.
    void cameraBasis(float outPos[3], float outFwd[3], float outUp[3]) const;

    // FORCE FIELD: if the ship is inside the sphere (center, radius), project it
    // back to the surface and cancel the inward velocity component — a shield
    // BOUNCE, not a wall glitch. Host calls this per frame against capital-ship /
    // hazard bubbles ("I fly right thru the enemy ship ... we SERIOUSLY NEED the
    // force field"). Returns true if it pushed (host can flash HUD / play a zap).
    bool pushOut(const float center[3], float radius);

    // TARGET-KEEPING LOOK (owner: "be able to look around and keep the enemy ship
    // in sight"): the host feeds the world-space direction to the locked target
    // each frame; the 3P camera's gaze BLENDS toward it (capped, eased) while the
    // ship keeps flying its own heading — you maneuver one way, the camera keeps
    // the fight in frame. amount 0 releases (eased back to pure ship-forward).
    void setCameraLookBias(const float dirWorld[3], float amount);

    // WING-MOUNTED GUNS (owner: "the fire needs to COme from weapons MOUNTED ON
    // THE Ship"): world-space muzzle position + bore direction for the left
    // (side=-1) / right (side=+1) wing hardpoint of the ~10 m fighter hull.
    void wingMuzzle(int side, float outPos[3], float outDir[3]) const;

    // HOLD-TO-FREELOOK (owner: "the player will be ABLE to keep the enemy ship in
    // sight by looking around while zipping around"): while the host routes mouse
    // deltas here (ALT held), the 3P camera ORBITS the ship — flight keeps its
    // heading, momentum carries. Released (no feed), the offsets ease back to
    // dead-astern. Clamped so you can look almost fully behind + well up/down.
    void addFreeLook(float dx, float dy);

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
    // enough energy). When true, drains Tuning.laserEnergyCost and starts the cooldown.
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
    // Dynamic speed cap: raised instantly to boostedMaxSpeed() while boosting,
    // then eased back toward Tuning.maxSpeed on release (antimatter overspeed
    // bleed-off — see update()). Seeded to maxSpeed at spawn.
    float m_speedCap = 0.0f;

    // Orientation: stored as a quaternion (x,y,z,w) per CONVENTIONS.md so we
    // accumulate roll cleanly without gimbal-locking. We also keep Euler
    // (yaw/pitch/roll) updated from input for HUD readback + camera basis.
    float m_quat[4] = { 0, 0, 0, 1 };  // identity
    // Elastic 3P chase camera (owner: "we need to visibly ZOOM left, right,
    // forward, back"): the cam position SPRINGS toward its ideal chase point
    // instead of welding to the hull, so thrust/strafe/boost visibly displace the
    // ship in frame. Updated in update(dt); consumed by cameraBasis().
    float m_chaseSm[3] = { 0, 0, 0 };
    bool  m_chaseSmValid = false;
    // Target-keeping look bias (world dir + eased amount).
    float m_lookBiasDir[3] = { 1, 0, 0 };
    float m_lookBiasAmt    = 0.0f;   // eased actual
    float m_lookBiasTgt    = 0.0f;   // host-requested
    // Hold-to-freelook orbit offsets (radians; ease to 0 when not fed).
    float m_freeYaw   = 0.0f;
    float m_freePitch = 0.0f;
    bool  m_freeFed   = false;   // fed this frame? (else update() eases home)
    float m_angVel[3] = { 0, 0, 0 };   // body-local angular velocity (rad/s)
    float m_yaw   = 0;                 // around world +Y (smoothed / applied)
    float m_pitch = 0;                 // around ship local +Z (after yaw)
    float m_roll  = 0;                 // around ship local +X (forward)
    float m_rollAxis = 0;              // buffered Q/E this frame

    // Active flight mode (default Arcade; changed via setMode).
    FlightMode m_mode = FlightMode::Arcade;

    // ---- Smooth / juice runtime state --------------------------------------
    // Look-smoothing: raw mouse input accumulates INSTANTLY into the target;
    // m_yaw/m_pitch ease toward it at Tuning.lookSmoothing (the "not jerky" fix).
    float m_yawTarget   = 0;
    float m_pitchTarget = 0;
    float m_yawPrev     = 0;           // last frame's applied yaw (auto-bank rate)
    float m_boostPunch  = 0;           // eased 0..1 boost weight for the FOV punch
    float m_juiceTime   = 0;           // accumulated dt for deterministic shake
    // Smoothed 3P chase-camera position (eased toward the rigid target + look-
    // ahead). m_camValid gates it: false until the first update() (so the
    // headless camera() self-test reads the rigid pose, not an unfilled zero).
    float m_camPos[3]   = { 0, 0, 0 };
    bool  m_camValid    = false;
    // Screen-shake offsets computed in update(), applied in camera() (kept out of
    // the sim so pos/vel stay byte-identical — determinism gate).
    float m_shakePos[3] = { 0, 0, 0 };
    float m_shakeYaw    = 0;
    float m_shakePitch  = 0;
    // Previous frame's velocity, used ONLY to derive the shake drive from the
    // ACTUAL instantaneous accel the ship is experiencing (see update()) —
    // NOT the raw thrust-input accel, which stays pinned at full magnitude for
    // as long as W is held even once the ship is capped at cruise speed (that
    // was reading as constant low-level jitter on long dives, e.g. the run to
    // the sun; see host_space.cpp REAL SUN work).
    float m_prevVelForShake[3] = { 0, 0, 0 };

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
// 1P↔3P, (9) setMode swaps the feel tuning (health preserved), (10) vertical
// thrust — Space rises / C drops along the ship up axis, boost-scaled. Logs
// PASS/FAIL T#, returns true iff all pass.
bool runSpaceSelfTest();

} // namespace x3::game
