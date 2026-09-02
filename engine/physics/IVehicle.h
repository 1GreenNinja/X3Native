#pragma once
// Vehicle / flight / buoyancy physics framework — GENERAL controllers on Jolt.
//
// CLEAN-ROOM, original work. Built ONLY from the engine's own IPhysicsWorld
// interface + the public Jolt Physics docs/headers (VehicleConstraint /
// WheeledVehicleController; buoyancy/drag is the standard Archimedes + quadratic
// drag force model) + public vehicle/flight references (Marco Monster "Car
// Physics for Games", which Jolt's own vehicle is based on). NO id Tech / RBDOOM
// or any other game-engine source was consulted.
//
// DESIGN — "every vehicle = rigid body + force-controller on Jolt":
//   The host creates a dynamic rigid body (a box / chassis) through the normal
//   IPhysicsWorld API, then attaches ONE controller to it. Each fixed physics
//   step the host feeds the controller driver INPUTS (throttle/brake/steer for a
//   car; pitch/yaw/roll/thrust for a plane; dive/thrust for a sub) and the
//   controller turns those into FORCES / constraints on the Jolt body. The body
//   is still a first-class citizen of the world (it collides, can be raycast,
//   reports its transform via getBodyPosition/getBodyRotation), so the renderer
//   draws it exactly like any other body.
//
// THREE controllers ship behind ONE opaque interface (IVehicleController):
//   1. WHEELED  — a true Jolt VehicleConstraint + WheeledVehicleController:
//                 engine/transmission/differential + raycast wheels + suspension
//                 springs. Drives on the streamed terrain (static mesh, layer
//                 Static). The headline.
//   2. BUOYANCY — Archimedes buoyancy + quadratic drag vs a flat water plane
//                 (sea level). A body floats + bobs and settles at the waterline;
//                 `dive` adds downward thrust so a sub can submerge.
//   3. FLIGHT   — a simple aircraft/space force model: forward thrust + a lift
//                 force proportional to forward speed + quadratic drag, plus
//                 pitch/yaw/roll control torques on the rigid body.
//
// OPAQUE: NO JPH:: types appear in this header. The Jolt implementation lives
// entirely in JoltVehicle.cpp (where the world's raw JPH::PhysicsSystem / JPH::Body
// are reached through IPhysicsWorld::nativeSystem()/nativeBody()).

#include "IPhysicsWorld.h"

#include <cstdint>

namespace x3::phys {

// ---------------------------------------------------------------------------
// Driver input — the per-step command for ANY controller. A field is ignored by
// a controller it does not apply to (e.g. a car ignores pitch/lift; a plane
// ignores brake/handBrake). All values are unitless [-1,1] or [0,1] as noted, so
// the same struct binds to a keyboard, a gamepad, or an AI.
// ---------------------------------------------------------------------------
struct VehicleInput {
    // Longitudinal command. WHEELED: [-1,1] (auto transmission picks the gear; >0
    // forward, <0 reverse, the magnitude is the gas pedal). FLIGHT: [0,1] engine
    // thrust. BUOYANCY (sub): unused (use `dive`).
    float throttle  = 0.0f;
    // Brake pedal [0,1] (WHEELED only).
    float brake     = 0.0f;
    // Hand brake [0,1] — locks the rear wheels for a drift/parking lock (WHEELED).
    float handBrake = 0.0f;
    // Steering [-1,1], +1 = steer right (WHEELED). Also used as YAW for FLIGHT.
    float steer     = 0.0f;
    // Aircraft attitude commands [-1,1] (FLIGHT only). pitch +1 = nose up,
    // roll +1 = roll right. (yaw reuses `steer`.)
    float pitch     = 0.0f;
    float roll      = 0.0f;
    // Vertical command [-1,1] for a submarine/airship (BUOYANCY): +1 = rise
    // (upward thrust), -1 = dive (downward thrust). Ignored by a pure boat.
    float dive      = 0.0f;
};

// Kind tag (also queryable at runtime for debugging / the demo HUD).
enum class VehicleKind : uint8_t { Wheeled, Buoyancy, Flight };

// ---------------------------------------------------------------------------
// One wheel of a wheeled vehicle. Positions are in the chassis's LOCAL space
// (RH, +Y up, -Z forward per CONVENTIONS.md). A typical 4-wheel car places the
// fronts at -Z (forward) and the rears at +Z, left at -X / right at +X.
// ---------------------------------------------------------------------------
struct WheelDesc {
    float position[3]   = { 0.0f, 0.0f, 0.0f }; // suspension attachment, chassis-local
    float radius        = 0.35f;                // wheel radius (m)
    float width         = 0.25f;                // wheel width (m)
    float suspensionMin = 0.10f;                // suspension length, max raised (m)
    float suspensionMax = 0.40f;                // suspension length, max droop (m)
    float suspensionFreq= 1.5f;                 // spring frequency (Hz)
    float suspensionDamp= 0.5f;                 // spring damping ratio
    bool  steered       = false;                // does this wheel turn with steering?
    bool  powered       = true;                 // does the engine drive this wheel?
    bool  handBraked    = false;                // does the hand brake lock this wheel?
    float maxSteerAngle = 0.5236f;              // max steer (rad) ~30deg, if steered
    float maxBrakeTorque= 1500.0f;              // brake torque (Nm)
    // BASELINE tire grip: multiplies Jolt's default longitudinal+lateral friction
    // curves at build time. The Jolt default is a generic economy tire whose high-
    // slip plateau is so low that a powerful RWD car lives in a permanent torque-
    // independent burnout (upshifts slip-blocked, engine pinned at redline) — set
    // > 1 for a sports-car compound so engine torque actually reaches the road.
    // A live WheeledTuning gripScale composes ON TOP of this baseline.
    float gripScale     = 1.0f;
};

// ---------------------------------------------------------------------------
// Configuration for a WHEELED controller. The chassis BodyId must already exist
// (a dynamic box created via IPhysicsWorld::addBox, layer Dynamic). Defaults make
// a ~1500 kg 4-wheel car that accelerates + steers out of the box.
// ---------------------------------------------------------------------------
struct WheeledVehicleDesc {
    BodyId chassis;                  // existing dynamic body (the car body)
    const WheelDesc* wheels = nullptr;
    uint32_t wheelCount = 0;
    float maxEngineTorque = 600.0f;  // engine peak torque (Nm)
    float maxEngineRPM    = 6000.0f; // engine redline (rpm)
    float clutchStrength  = 10.0f;   // transmission clutch strength
    // The object layer the wheel ground-rays FILTER ON. Jolt's
    // VehicleCollisionTesterRay uses DefaultObjectLayerFilter — the COLLISION MATRIX,
    // not exact-match — so a ray hits bodies the matrix says COLLIDE with this layer.
    // Per the engine matrix, Dynamic collides with Static (so Dynamic sees the Static
    // ground) while Static does NOT collide with Static (a Static filter misses the
    // ground). Leave at Dynamic. The chassis is excluded by the body filter.
    Layer groundLayer = Layer::Dynamic;
    // Forward / up of the chassis in its LOCAL frame (CONVENTIONS.md: -Z fwd, +Y up).
    float forward[3] = { 0.0f, 0.0f, -1.0f };
    float up[3]      = { 0.0f, 1.0f,  0.0f };
};

// ---------------------------------------------------------------------------
// Configuration for a BUOYANCY controller (boats / subs). The body floats at a
// flat water plane (a sea level Y), using Archimedes buoyancy + quadratic drag.
// ---------------------------------------------------------------------------
struct BuoyancyDesc {
    BodyId body;                     // existing dynamic body to float
    float seaLevel       = 0.0f;     // world Y of the (flat) water surface
    // The submerged-volume model: the body is approximated as a box of these
    // half-extents (m) for computing how much is under water. Use the body's box.
    float halfExtents[3] = { 1.0f, 0.5f, 2.0f };
    float fluidDensity   = 1000.0f;  // kg/m^3 (fresh water ~1000, sea ~1025)
    float linearDrag     = 2.0f;     // linear drag coefficient (1/s) when submerged
    float angularDrag    = 1.0f;     // angular drag coefficient (1/s) when submerged
    // Upward thrust force (N) at full `dive`=+1; downward at dive=-1. A sub uses
    // this to submerge against buoyancy and to surface. A pure boat sets 0.
    float diveThrust     = 0.0f;
    // Optional forward propulsion (N) at throttle=+1 (a powered boat); 0 = drifts.
    float propThrust     = 0.0f;
    // Yaw torque (N·m) at steer=+1 — lets a powered boat turn. 0 = no steering.
    float steerTorque    = 0.0f;
    // ---- SWELL (optional; default 0 = calm water, behavior byte-identical). ----
    // The buoyancy plane is FLAT, so a floating hull settles dead level — there is
    // no wave-induced rocking for a chase camera to follow. A non-zero swellTorque
    // applies a gentle periodic attitude torque THROUGH THE PHYSICS (roll about
    // the hull's horizontal forward axis at the amplitude below, plus a smaller
    // off-phase pitch component so the motion reads organic, not a metronome).
    // Applied only while submerged, scaled by the submerged fraction. The hull
    // then genuinely rocks; cameras read the REAL attitude off the body.
    float swellTorque    = 0.0f;    // N·m roll-torque amplitude (0 = calm)
    float swellFreqHz    = 0.15f;   // primary swell frequency (Hz)
    // METACENTRIC self-righting (N·m per radian of roll/pitch; default 0 =
    // byte-identical). The buoyancy force above acts at the COM, so the model
    // has NO restoring attitude moment — any transient (the spawn drop, a
    // swell) leaves the hull with a PERMANENT list, since zero-mean torque
    // only bounds angular velocity, not angle. Real hulls self-right (the
    // buoyancy centroid shifts to leeward of the COM); this linearized
    // righting torque about the horizontal axes restores level so the swell
    // rocks the hull ABOUT zero instead of about an accidental list.
    float rightingTorque = 0.0f;
};

// ---------------------------------------------------------------------------
// Configuration for a FLIGHT controller (aircraft / space). Thrust + lift + drag
// + attitude control torques on a rigid body.
// ---------------------------------------------------------------------------
struct FlightDesc {
    BodyId body;                     // existing dynamic body (the aircraft)
    float maxThrust      = 12000.0f; // forward thrust (N) at throttle=1
    float liftCoefficient= 30.0f;    // lift ~ liftCoefficient * forwardSpeed^? (see impl)
    float linearDrag     = 0.6f;     // quadratic drag coefficient
    float pitchTorque    = 8000.0f;  // control torque (N·m) at full pitch input
    float yawTorque      = 4000.0f;  // control torque (N·m) at full yaw (steer) input
    float rollTorque     = 8000.0f;  // control torque (N·m) at full roll input
    float angularDamping = 1.5f;     // attitude-rate damping (1/s) so it settles
    // Local forward / up of the airframe (CONVENTIONS.md: -Z fwd, +Y up).
    float forward[3] = { 0.0f, 0.0f, -1.0f };
    float up[3]      = { 0.0f, 1.0f,  0.0f };
    // Whether gravity acts on the body. A space-craft can set false (no gravity);
    // an atmospheric plane keeps it true and relies on lift to stay up.
    bool  gravity        = true;
};

// ---------------------------------------------------------------------------
// LIVE TUNING for a WHEELED controller (the performance-shop parts system).
// Applied to an EXISTING controller without tearing the constraint down: the
// Jolt engine/wheel SETTINGS objects are mutated in place (the simulation reads
// them every step), and the chassis mass is rescaled (inertia follows). Fields
// at their "leave" sentinel (noted per field) keep the current value, so a
// partial tuning is safe.
// ---------------------------------------------------------------------------
struct EngineCurvePoint {
    float rpmFrac    = 0.0f;   // X: engine RPM as a fraction of maxEngineRPM [0,1]
    float torqueFrac = 1.0f;   // Y: fraction of maxEngineTorque available there
};
struct WheeledTuning {
    static constexpr float kRideHeightLeave = -1000.0f;
    // Engine. maxEngineTorque <= 0 leaves torque; maxEngineRPM <= 0 leaves redline.
    float maxEngineTorque = 0.0f;     // peak torque (Nm)
    float maxEngineRPM    = 0.0f;     // redline (rpm)
    // Normalized torque CURVE (the camshaft/FI profile). curvePoints == 0 keeps the
    // current curve. Points must be ascending in rpmFrac. Max 8.
    EngineCurvePoint curve[8];
    uint32_t curvePoints  = 0;
    // Chassis mass (kg). <= 0 leaves mass. Inertia is rescaled proportionally.
    float massKg          = 0.0f;
    // Tire grip multiplier applied to BOTH the longitudinal and lateral friction
    // curves (1 = the Jolt default tire). <= 0 leaves grip.
    float gripScale       = 0.0f;
    // Suspension spring (all wheels). <= 0 leaves the respective value.
    float suspensionFreq  = 0.0f;     // spring frequency (Hz)
    float suspensionDamp  = 0.0f;     // damping ratio
    // Ride height delta (m) ADDED to each wheel's authored suspension min/max
    // lengths (negative lowers the car). 0 leaves; clamped so min stays >= 0.03.
    // NaN is not supported; use 0 to skip. Sentinel: kRideHeightLeave skips too.
    float rideHeightDelta = kRideHeightLeave;
    // Brake torque (Nm, all wheels; hand-braked wheels keep their 2.5x lock factor).
    // <= 0 leaves brakes.
    float brakeTorque     = 0.0f;
};

// Per-wheel render state (for drawing the wheel meshes at the right place). The
// transform is a column-major 4x4 in WORLD space that maps a unit cylinder
// aligned with +Y (radius 1, height 1) to the wheel — i.e. it already includes
// the wheel's radius/width scale, suspension travel, steer, and spin rotation.
struct WheelState {
    float worldTransform[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    float radius   = 0.35f;
    float width    = 0.25f;
    bool  hasContact = false;   // true if the wheel is touching the ground
    float suspensionLength = 0.0f;
};

// ---------------------------------------------------------------------------
// The controller interface. One per vehicle. Created via the factories below,
// owned by the caller (delete it BEFORE the world it was built on shuts down).
// ---------------------------------------------------------------------------
class IVehicleController {
public:
    virtual ~IVehicleController() = default;

    // Feed this step's driver command. Cheap; just stores the input.
    virtual void setInput(const VehicleInput& in) = 0;

    // Apply the controller's forces/constraint for ONE fixed step of length `dt`.
    // CALL ORDER (important): preStep() BEFORE IPhysicsWorld::step(), so the
    //   forces are integrated by that step. (The WHEELED controller is driven by
    //   Jolt's own step listener, so its preStep only pushes the driver input;
    //   the BUOYANCY/FLIGHT controllers add their forces here.)
    // postStep() AFTER the world step (refreshes cached render state). Most hosts
    //   can ignore the split and just call update(dt) which does pre+post for the
    //   force-model controllers — but the wheeled one needs the input set before
    //   the step. The demo uses preStep()/postStep() explicitly.
    virtual void preStep(float dt)  = 0;
    virtual void postStep(float dt) = 0;
    // Convenience: preStep + (host steps world) is NOT done here; update() applies
    // the force model immediately (pre+post) assuming the host steps right after.
    virtual void update(float dt) { preStep(dt); postStep(dt); }

    // The body this controller drives.
    virtual BodyId body() const = 0;
    virtual VehicleKind kind() const = 0;

    // Forward speed along the chassis's local forward axis (m/s). Negative = reverse.
    virtual float forwardSpeed() const = 0;

    // ---- WHEELED-only queries (return 0 / no-op for other kinds) ----
    virtual uint32_t wheelCount() const { return 0; }
    // Fill `out` with the world render transform + state of wheel `i`. Returns
    // false if i is out of range or the controller has no wheels.
    virtual bool wheelState(uint32_t i, WheelState& out) const { (void)i; (void)out; return false; }
    virtual float engineRPM() const { return 0.0f; }
    virtual int   gear() const { return 0; }

    // Longitudinal slip RATIO of wheel i: (wheelSurfaceSpeed - vehicleSpeed) /
    // max(|vehicleSpeed|, 1). ~0 = rolling in sync, >> 0 = wheelspin (burnout),
    // < 0 = locked under braking. Used by the game-layer traction control.
    virtual float longitudinalSlip(uint32_t i) const { (void)i; return 0.0f; }

    // Apply a live performance tuning (see WheeledTuning). Mutates the running
    // Jolt engine/wheel settings + the chassis mass IN PLACE — no constraint
    // rebuild, so a parked car can be re-tuned in the shop and immediately driven.
    // Returns false for non-wheeled controllers (default).
    virtual bool applyWheeledTuning(const WheeledTuning& t) { (void)t; return false; }

    // Temporary engine torque MULTIPLIER (nitrous). 1 = none. Applied on top of
    // the tuned maxEngineTorque each pre-step while set; the host owns the tank /
    // duration logic. No-op for non-wheeled controllers.
    virtual void setTorqueBoost(float mult) { (void)mult; }
    virtual float torqueBoost() const { return 1.0f; }

    // ---- BUOYANCY-only query ----
    // Fraction of the body currently under the water surface [0,1]. 0 for non-buoyant.
    virtual float submergedFraction() const { return 0.0f; }
};

// ---------------------------------------------------------------------------
// Factories. Each builds a controller on `world` for the (already-created) body
// in its desc. Returns nullptr on invalid input (bad body, no wheels, etc.).
// ---------------------------------------------------------------------------
IVehicleController* createWheeledVehicle(IPhysicsWorld& world, const WheeledVehicleDesc& desc);
IVehicleController* createBuoyancyController(IPhysicsWorld& world, const BuoyancyDesc& desc);
IVehicleController* createFlightController(IPhysicsWorld& world, const FlightDesc& desc);

// Headless self-test (--test-vehicle). Drives a wheeled vehicle under throttle on
// flat ground (asserts it accelerates forward + steering yaws it), and a buoyant
// body dropped onto water (asserts it settles near the waterline — neither sinks
// to the bottom nor launches into the air). Also exercises a flight body (thrust
// gives forward speed; lift opposes gravity). Logs PASS/FAIL V#, returns true iff
// all pass. No window / Vulkan. Implemented in JoltVehicle.cpp.
bool runVehicleSelfTest();

} // namespace x3::phys
