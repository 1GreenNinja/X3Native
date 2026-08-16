#pragma once
// Vehicle demo worlds (drive / boat / fly) — game/slice layer over the engine's
// GENERAL vehicle framework (engine/physics/IVehicle.h).
//
// Game/slice code only; engine/ stays pure. Three self-contained demo scenes,
// each = a dynamic rigid body + one IVehicleController + render meshes:
//   * DriveDemo : a wheeled car (Jolt VehicleConstraint) on the STREAMED terrain.
//   * BoatDemo  : a buoyant hull floating on a flat ocean (sea level), + a sub
//                 mode that can dive.
//   * FlyDemo   : an aircraft (thrust + lift + drag + attitude control).
//
// Built ONLY through the public IRenderDevice + IPhysicsWorld + IVehicleController
// interfaces. No id Tech / RBDOOM source. Low-conflict with the rest of the app
// (own world, its own --world flags).

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/physics/IVehicle.h"
#include "engine/asset/IModelLoader.h"   // hero-car GLB skin (ModelDrawable)
#include "engine/asset/IAssetSource.h"
#include "mesh_prims.h"

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace x3::game {

// A simple Y-aligned cylinder mesh (radius 1, height 1: spans y in [-0.5,0.5]).
// Used for the car wheels; the vehicle controller's wheel transform already bakes
// the radius/width scale, so the renderer just draws this unit cylinder.
void makeUnitCylinderY(uint32_t segments,
                       std::vector<x3::rhi::MeshVertex>& verts,
                       std::vector<uint32_t>& idx);

// ---------------------------------------------------------------------------
// DriveDemo — a wheeled car on flat ground OR on streamed terrain. The car is a
// chassis box (dynamic, layer Dynamic) + a WheeledVehicleController with 4 raycast
// wheels. The host steps physics; this owns the controller + render meshes and
// draws the chassis (full rotation) + the 4 wheels at their live transforms.
// ---------------------------------------------------------------------------
class DriveDemo {
public:
    // Spawn the car at (x,y,z). The chassis is a ~1300 kg box sized to the HERO
    // CAR GLB (CTR: 1.81 x 1.3 x 4.3 m, wheel stations from the model); 4 wheels
    // (front steered, rear powered + handbrake). Returns false if the controller
    // failed. build() = buildPhysics() + the graybox render meshes.
    bool build(x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
               float x, float y, float z);

    // Physics-only build (no render device) — the headless self-test path.
    bool buildPhysics(x3::phys::IPhysicsWorld& physics, float x, float y, float z);

    // HERO-CAR SKIN: load the converted vehicle GLB (clearcoat paint + emissive
    // lights) and render IT instead of the graybox box+cylinders. The GLB's
    // Wheel_FL/FR/RL/RR node drawables follow the live physics wheel poses (spin +
    // steer + suspension); everything else rides the sprung chassis. The GLB is
    // authored nose = +Z / origin on the ground; the skin bakes the 180-degree yaw
    // to the engine's -Z forward + the ride-height drop. Returns false (graybox
    // render kept) if the GLB is missing — drive still works.
    bool skin(x3::rhi::IRenderDevice& device, std::string_view glbDir,
              std::string_view relPath);
    bool skinned() const { return m_skinned; }

    // All four wheels touching ground? (drive self-test assert)
    bool allWheelsInContact() const;

    // Feed driver input + advance one step. Call setInput()+preStep() BEFORE the
    // host's physics->step(), then postStep() AFTER. drive() is a convenience that
    // does setInput()+preStep() (host steps next).
    void setInput(const x3::phys::VehicleInput& in);
    void preStep(float dt);
    void postStep(float dt);

    // Draw the chassis + wheels. Call between beginFrame/endFrame.
    void render(const x3::rhi::FrameContext& frame) const;

    void shutdown();

    x3::phys::IVehicleController* controller() const { return m_ctl.get(); }
    x3::phys::BodyId chassis() const { return m_chassis; }
    // World position of the chassis (for chasing the camera).
    void chassisPos(float out[3]) const;
    float forwardSpeed() const { return m_ctl ? m_ctl->forwardSpeed() : 0.0f; }
    float engineRPM()    const { return m_ctl ? m_ctl->engineRPM() : 0.0f; }
    int   gear()         const { return m_ctl ? m_ctl->gear() : 0; }
    // Peak longitudinal slip across all wheels (0 = rolling, 1 = full spin) —
    // drives the tire-squeal audio and the wheel-spin FX.
    float maxSlip()      const {
        if (!m_ctl) return 0.0f;
        float s = 0.0f;
        for (uint32_t i = 0; i < m_ctl->wheelCount(); ++i)
            s = std::max(s, m_ctl->longitudinalSlip(i));
        return s;
    }
    // Raw driver throttle, pre-traction-control. For HUD + engine AUDIO: tone
    // must follow LOAD, and load starts with what the driver is asking for.
    float throttleInput() const { return m_lastIn.throttle; }
    // Throttle AFTER traction control — what the engine is actually given, and
    // therefore what the engine NOTE should follow. When TC trims for slip the
    // sound must trim with it, or the audio lies about what the car is doing.
    float effectiveThrottle() const { return m_effThrottle; }
    // The audio engine model's RPM (idle + flywheel lag, decoupled from Jolt's
    // road-speed-locked RPM). The engine NOTE follows THIS, not engineRPM().
    float audioRPM() const { return m_engineRpm; }

    // ---- Performance-shop live tuning passthrough (engine WheeledTuning). ----
    // Re-tunes the RUNNING Jolt vehicle in place (engine curve/torque, mass, grip,
    // suspension, ride height, brakes) — drive out of the shop and FEEL the parts.
    bool applyTuning(const x3::phys::WheeledTuning& t) {
        return m_ctl ? m_ctl->applyWheeledTuning(t) : false;
    }
    // Nitrous: temporary torque multiplier (1 = off). Host owns the tank timer.
    // NOTE this is now COMBINED with the turbo model's multiplier rather than
    // written straight through, so a nitrous bottle and a spooling turbo can
    // both be live without one clobbering the other every step.
    void  setTorqueBoost(float mult) { m_userTorqueMult = mult; }
    float torqueBoost() const { return m_userTorqueMult; }
    // Traction control (default ON — see setInput). Off = full burnout mode.
    void  setTractionControl(bool on) { m_tcEnabled = on; }
    bool  tractionControl() const { return m_tcEnabled; }

    // ---- TURBO: a manifold-pressure model (see updateTurbo in vehicle.cpp) --
    // The torque CURVE is the full-boost delivery; this supplies the transient
    // that a curve cannot express — the lag before it arrives and the shove
    // when it does. Peak power is unchanged, only its timing.
    struct TurboParams {
        float maxPsi        = 35.0f;   // peak manifold pressure over atmosphere (a hot 911 turbo)
        float spoolStartRpm = 1800.0f; // nothing below this
        float spoolFullRpm  = 4200.0f; // full compressor above this
        float spoolTau      = 0.45f;   // seconds to build (compressor inertia)
        float dumpTau       = 0.11f;   // seconds to bleed off (wastegate/BOV)
        float vacuumPsi     = 8.5f;    // depth of vacuum at a closed throttle
        float floorTorque   = 0.85f;   // off-boost torque fraction — high so the launch still pulls
    };
    TurboParams&       turbo()       { return m_turbo; }
    const TurboParams& turbo() const { return m_turbo; }
    void  setTurboEnabled(bool on)   { m_turboOn = on; }
    bool  turboEnabled() const       { return m_turboOn; }
    // Manifold pressure, psi. NEGATIVE is vacuum, which is where a real boost
    // gauge spends most of its life.
    float boostPsi() const { return m_boostPsi; }
    // Multiplier the turbo is currently applying to engine torque.
    float turboMult() const { return m_turboMult; }

    // ---- Per-instance paint tint (WORLD CARS variants) ----------------------
    // Replaces the CLEARCOAT drawables' baseColor RGB (the car-paint panels;
    // glass/tires/trim keep their authored look) so the one live rig matches
    // whichever parked variant was entered. Also tints the graybox fallback
    // body. Off by default — the authored GLB paint, byte-identical.
    void setPaintTint(const float rgb[3]) {
        m_tint[0] = rgb[0]; m_tint[1] = rgb[1]; m_tint[2] = rgb[2]; m_tintOn = true;
    }
    void clearPaintTint() { m_tintOn = false; }

private:
    x3::rhi::IRenderDevice*  m_device  = nullptr;
    x3::phys::IPhysicsWorld* m_physics = nullptr;
    std::unique_ptr<x3::phys::IVehicleController> m_ctl;
    x3::phys::BodyId m_chassis;
    // Chassis half extents — sized to the hero-car GLB footprint (CTR).
    // m_hx widened 0.84 -> 0.95 so the collision box spans the 1.808 m body
    // (it was 12 cm NARROWER than the bodywork) and the wider stance.
    // 1.07 = half the widened 2.13 m body (1.808 * 1.18), so the collision box
    // spans the bodywork rather than sitting inside it.
    float m_hx = 1.07f, m_hy = 0.5f, m_hz = 1.95f;

    x3::phys::VehicleInput m_lastIn;     // raw driver input (pre-TC; HUD)
    float m_effThrottle = 0.0f;          // post-TC throttle (engine audio load)
    bool m_tcEnabled = false;            // TC off — grip 10 hooks up and the box shifts; T toggles
    bool m_tcCutting = false;            // TC hysteresis latch — stays cut until slip falls (see setInput)
    float m_tcTrim = 1.0f;               // smoothed TC trim (one-pole, see setInput)

    void  updateTurbo(float dt);         // called from preStep
    void  updateEngineModel(float dt);   // called from preStep
    TurboParams m_turbo;
    bool  m_turboOn         = true;
    float m_boostPsi        = 0.0f;      // manifold pressure (negative = vacuum)
    float m_turboMult       = 1.0f;      // torque multiplier the turbo is applying
    float m_userTorqueMult  = 1.0f;      // nitrous / console override, multiplied in
    float m_engineRpm       = 800.0f;   // audio engine-model RPM (idle + flywheel lag)

    bool  m_tintOn = false;              // paint tint (see setPaintTint)
    float m_tint[3] = { 1, 1, 1 };

    x3::rhi::MeshHandle    m_chassisMesh;
    x3::rhi::MeshHandle    m_wheelMesh;
    x3::rhi::TextureHandle m_chassisTex;
    x3::rhi::TextureHandle m_wheelTex;
    std::vector<x3::phys::WheelDesc> m_wheels;

    // ---- Hero-car GLB skin (optional; graybox fallback when absent) ----
    bool m_skinned = false;
    std::unique_ptr<x3::asset::IAssetSource> m_skinSrc;
    std::unique_ptr<x3::asset::IModelLoader> m_skinLoader;
    x3::asset::Model m_skinModel;
    std::vector<x3::asset::ModelDrawable> m_bodyDraw;     // everything but the wheels
    std::vector<x3::asset::ModelDrawable> m_wheelDraw[4]; // per physics wheel slot
    void drawDrawable(const x3::rhi::FrameContext& f,
                      const x3::asset::ModelDrawable& d, const float world[16]) const;
};

// Headless game-layer DRIVE self-test (--test-vehicle): spawn the car on a flat
// static slab, ENTER it (player control handed to the car), throttle N fixed
// ticks, assert FORWARD DISPLACEMENT + all-wheel ground contact, then EXIT and
// assert player control is restored (on-foot position placed beside the car).
// Physics-only (no render device); returns true when every check passes.
bool runDriveEnterExitSelfTest();

// ---------------------------------------------------------------------------
// BoatDemo — a buoyant hull floating on a flat ocean at `seaLevel`. `isSub` makes
// it a submarine (can dive with the dive input). The hull is a box; buoyancy +
// drag keep it at the waterline. Renders the hull at its full transform.
// ---------------------------------------------------------------------------
class BoatDemo {
public:
    bool build(x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
               float x, float y, float z, float seaLevel, bool isSub = false);

    void setInput(const x3::phys::VehicleInput& in);
    void preStep(float dt);
    void postStep(float dt);

    void render(const x3::rhi::FrameContext& frame) const;
    void shutdown();

    x3::phys::IVehicleController* controller() const { return m_ctl.get(); }
    x3::phys::BodyId hull() const { return m_hull; }
    void hullPos(float out[3]) const;
    float submergedFraction() const { return m_ctl ? m_ctl->submergedFraction() : 0.0f; }

private:
    x3::rhi::IRenderDevice*  m_device  = nullptr;
    x3::phys::IPhysicsWorld* m_physics = nullptr;
    std::unique_ptr<x3::phys::IVehicleController> m_ctl;
    x3::phys::BodyId m_hull;
    float m_hx = 1.5f, m_hy = 0.6f, m_hz = 3.0f;

    x3::rhi::MeshHandle    m_hullMesh;
    x3::rhi::TextureHandle m_hullTex;
};

// ---------------------------------------------------------------------------
// FlyDemo — an aircraft body driven by the flight controller (thrust + lift +
// drag + pitch/yaw/roll). Renders the airframe at its full transform.
// ---------------------------------------------------------------------------
class FlyDemo {
public:
    bool build(x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
               float x, float y, float z);

    void setInput(const x3::phys::VehicleInput& in);
    void preStep(float dt);
    void postStep(float dt);

    void render(const x3::rhi::FrameContext& frame) const;
    void shutdown();

    x3::phys::IVehicleController* controller() const { return m_ctl.get(); }
    x3::phys::BodyId airframe() const { return m_body; }
    void airframePos(float out[3]) const;
    float forwardSpeed() const { return m_ctl ? m_ctl->forwardSpeed() : 0.0f; }

private:
    x3::rhi::IRenderDevice*  m_device  = nullptr;
    x3::phys::IPhysicsWorld* m_physics = nullptr;
    std::unique_ptr<x3::phys::IVehicleController> m_ctl;
    x3::phys::BodyId m_body;
    float m_hx = 2.0f, m_hy = 0.4f, m_hz = 2.6f;

    x3::rhi::MeshHandle    m_bodyMesh;
    x3::rhi::MeshHandle    m_wingMesh;
    x3::rhi::TextureHandle m_bodyTex;
};

// ---------------------------------------------------------------------------
// vehcam — hull-attitude chase-camera math for the REAL rigid-body vehicles
// (fly / boat). Unlike the car (which SYNTHESIZES bank from steer input), the
// flyer and boat are Jolt bodies whose orientation the physics already owns:
// these helpers read the body quaternion and derive a roll-capable camera
// basis (IRenderDevice::setCameraBasis) from the REAL attitude. Pure math +
// tiny persistent smoothing state, factored here so the host stays surgical
// and the math is unit-testable headlessly (--test-vehicle).
// Conventions (CONVENTIONS.md): body quat is (x,y,z,w); hull local forward =
// (0,0,-1), up = (0,1,0); setCamera fwd = (cos p cos y, sin p, cos p sin y).
// ---------------------------------------------------------------------------
namespace vehcam {

// World-space hull forward/up from the body quaternion (unit output).
void hullAxes(const float q[4], float outFwd[3], float outUp[3]);

// Signed hull roll (+ = starboard/right lean) and pitch (+ = nose up), radians,
// extracted from the world axes. Meaningful in the near-level band (the boat);
// a looping flyer should use the full basis (flyChase), not these Eulers.
void hullRollPitch(const float fwdW[3], const float upW[3],
                   float& outRoll, float& outPitch);

// FLY chase basis: the smoothed camera fwd/up trail the hull's genuine
// attitude. Target up is the hull up BLENDED toward world-up by upLevelBlend
// (a hard-mounted cam feels rigid; a chase plane lags slightly level); the
// whole basis eases at lerpRate (1/s, dt-scaled — never per-frame). Vector
// lerp on the basis handles past-vertical natively: a loop rolls the horizon
// over the top instead of pinwheeling through an Euler wall.
struct FlyCamState { float fwd[3] = {0,0,-1}; float up[3] = {0,1,0}; bool valid = false; };
void flyChase(FlyCamState& s, const float q[4], float dt,
              float upLevelBlend, float lerpRate);

// BOAT attitude-follow: smoothed FRACTIONS of the hull's wave roll and pitch
// (heavily damped — the camera is ON the water, not bolted to the hull).
struct BoatCamState { float roll = 0.0f; float pitch = 0.0f; };
void boatFollow(BoatCamState& s, const float q[4], float dt,
                float rollFrac, float pitchFrac, float lerpRate);

// Roll-capable basis from FPS yaw/pitch (the setCamera convention) + a roll
// about the view axis: up = cos(roll)*levelUp + sin(roll)*right (the car's
// banking construction, shared).
void basisYawPitchRoll(float yaw, float pitch, float roll,
                       float outFwd[3], float outUp[3]);

} // namespace vehcam

// Headless vehicle-camera self-test (--test-vehicle): asserts (1) the fly
// camera's up-vector tracks a rolled hull within tolerance (and stays level on
// a level hull — negative control), (2) the boat camera's roll is a FRACTION
// (< 0.5) of the hull roll with matching sign (level control likewise), and
// (3) the buoyancy SWELL genuinely rocks a floating hull (bounded amplitude;
// calm without it). Physics-only, no window/Vulkan.
bool runVehicleCamSelfTest();

} // namespace x3::game
