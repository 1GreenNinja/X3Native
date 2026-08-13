#pragma once
// ============================================================================
// driftgrip — DRIFT FEEL + SURFACE-DEPENDENT TRACTION, one grip-composition
// layer over the wheeled controller.               [LANE: inspx/veh-cosmetics]
//
// This lane owns this file. Nothing else in the app modulates tire grip per
// wheel; wetness (inspx/wetness) composes onto this through the external
// scalar (setExternalGripScale), the same shape as its own gripScale() —
// the two MULTIPLY cleanly instead of fighting.
//
// WHY A LAYER AND NOT A TUNING:
//
// x3::phys::WheeledTuning::gripScale is a STATIC property of the installed
// tires — it multiplies both friction curves once, at install time. Drift and
// surface response are DYNAMIC: they depend on what the car is doing this
// tick (slip angle, throttle, handbrake) and what each wheel is standing on
// (asphalt / shoulder / dirt / grass) this tick. So this layer computes a
// per-wheel, per-tick grip MODULATION and pushes it through the engine's
// IVehicleController::setWheelGripMod, which composes it multiplicatively ON
// TOP of the baseline compound and the shop tuning. Everything at its default
// (1,1,0) leaves the friction curves bit-identical to today.
//
// THE DRIFT MODEL (NFS Hot Pursuit / Carbon target: throttle-and-steer
// oversteer that is CONTROLLABLE and RECOVERABLE, not a spin):
//   * ENTRY  — body slip angle beyond a threshold (or handbrake + steer at
//     speed) arms the drift state, with hysteresis so it doesn't strobe.
//   * WHILE DRIFTING — the REAR axle's lateral grip is retained at a fraction
//     (weight transfer: throttle unloads it further, lift-off restores it),
//     the front keeps nearly all of its bite so the nose still answers the
//     wheel. That is what makes the car ROTATE instead of plow.
//   * ANTI-SPIN — past a stabilization angle the rear grip ramps back toward
//     full, so the slide has a natural ceiling instead of swapping ends.
//   * COUNTERSTEER ASSIST — a bounded steer term proportional to the slip
//     angle is added INTO the slide, so catching it needs a human amount of
//     countersteer, not a robot's. Assist only acts while drifting.
//   * EXIT — below the exit angle the retained grips ease back to 1 over a
//     blend time (no snap-oversteer on recovery).
//
// SURFACE TRACTION (the ditch bug, 2026-08-13): grip was uniform regardless
// of what the tire was standing on. Each wheel now queries the host-injected
// SurfaceQuery at its contact point and gets that surface's multipliers.
// Loose surfaces (dirt/grass) get LESS peak grip than asphalt but a FLATTENED
// longitudinal curve: a spinning wheel on dirt keeps pushing (the surface
// shears and "spits out"), where asphalt past peak slip just polishes the
// tire. Ratios follow the same measured-mu discipline as app/wetness.h
// (dry asphalt ~0.9 -> packed dirt ~0.68 -> grass ~0.55 of asphalt).
//
// DETERMINISM: pure per-tick math over the controller's state; no clock, no
// randomness, no allocation after configure. Same inputs -> same outputs.
//
// CLEAN-ROOM, original work. Built only on the engine's own IVehicle.h
// interface + public vehicle-dynamics references (slip angle / slip ratio /
// load transfer are standard "Car Physics for Games"-level material). No
// other game or engine source consulted.
// ============================================================================
#include "engine/physics/IVehicle.h"
#include "vehparts.h"

#include <cstdint>
#include <functional>

namespace x3::game {

// What a wheel is standing on. The host injects the query; presentation
// (spray) and physics (grip) both branch on this so "is it dirt" has ONE
// answer everywhere.
enum class DriveSurface : uint32_t {
    Road     = 0,   // asphalt / the corridor's paved ribbon
    Shoulder = 1,   // concrete shoulder / corridor apron
    Dirt     = 2,   // graded earth, the cutting floor and batters
    Grass    = 3,   // untouched terrain
    Count    = 4,
};
const char* driveSurfaceName(DriveSurface s);

// Per-surface grip multipliers, applied on top of the tire compound.
struct SurfaceGrip {
    float longScale   = 1.0f;   // longitudinal friction multiplier
    float latScale    = 1.0f;   // lateral friction multiplier
    float longFlatten = 0.0f;   // 0..1 raise the high-slip plateau toward peak
    float spray       = 0.0f;   // 0..1 how much particulate this surface kicks
};
// The calibrated table (see the header block for the mu-ratio provenance).
SurfaceGrip surfaceGripFor(DriveSurface s);

// Drift tunables. Defaults are the "summer tire" feel; driftParamsFor()
// derives a set from the parts catalog so tires/suspension change the feel.
struct DriftParams {
    float entrySlipDeg   = 9.0f;    // body slip angle that arms the drift
    float exitSlipDeg    = 4.5f;    // below this the drift disarms (hysteresis)
    float minSpeed       = 6.0f;    // m/s — below this, no drift logic at all
    float rearLatRetain  = 0.66f;   // rear lateral grip retained mid-drift
    float frontLatRetain = 0.94f;   // front keeps nearly all its bite
    float throttleRearCut= 0.10f;   // extra rear retain drop at full throttle
    float stabSlipDeg    = 26.0f;   // past this, rear grip ramps back (anti-spin)
    float stabRangeDeg   = 10.0f;   // ...over this many degrees
    float counterGain    = 2.2f;    // assist steer per rad of slip angle
    float counterMax     = 0.80f;   // assist ceiling (fraction of full lock)
    float steerBlunt     = 0.45f;   // mid-drift, aggravating steer input is scaled
                                    // by (1 - steerBlunt): the assist WINDOW —
                                    // yanking further into the slide is damped,
                                    // countersteer passes through untouched
    float blendRate      = 6.0f;    // 1/s — grip retain ease-in/out rate
    bool  handbrakeEntry = true;    // handbrake + steer at speed arms the drift
};

// Derive drift params from the INSTALLED parts (tires compound + suspension
// stiffness), so the shop genuinely changes the drift feel: touring breaks
// away early and recovers soft; slicks need real provocation and hold a
// tighter, faster drift; stiff suspension sharpens the countersteer response.
DriftParams driftParamsFor(const vehparts::Catalog& cat,
                           const vehparts::VehicleBuild& build);

// Host-injected: what surface is under world (x,z). Unset => Road everywhere
// (grip modulation stays 1 — bit-identical to the pre-lane behaviour).
using SurfaceQuery = std::function<DriveSurface(float x, float z)>;

// ---------------------------------------------------------------------------
// The composition layer. Embed by value (DriveDemo owns one); call update()
// once per fixed step BEFORE the controller's preStep, with the post-TC
// driver input — it may add countersteer assist to eff.steer and pushes the
// per-wheel grip modulation for this tick.
// ---------------------------------------------------------------------------
class DriftGrip {
public:
    static constexpr uint32_t kMaxWheels = 8;

    void setDriftEnabled(bool on)    { m_driftOn = on; }
    void setSurfaceEnabled(bool on)  { m_surfaceOn = on; }
    bool driftEnabled() const        { return m_driftOn; }
    bool surfaceEnabled() const      { return m_surfaceOn; }

    void setParams(const DriftParams& p) { m_p = p; }
    const DriftParams& params() const    { return m_p; }

    void setSurfaceQuery(SurfaceQuery fn) { m_query = std::move(fn); }

    // Which wheel indices are the driven/rear axle (drift cuts these).
    // Default matches DriveDemo's order (0,1 front / 2,3 rear).
    void setRearWheels(uint32_t a, uint32_t b) { m_rearA = a; m_rearB = b; }

    // External scalar grip multiplier (wetness/ice); composes onto BOTH axes
    // of every wheel. 1 = dry. This is the wetness lane's composition point.
    void setExternalGripScale(float s) { m_external = (s > 0.0f) ? s : 1.0f; }

    // Per fixed step. Reads the controller's live state (slip, wheel contact
    // points), may modify eff.steer (countersteer assist), and pushes the
    // composed per-wheel WheelGripMod. With drift+surface disabled and the
    // external scalar at 1 this NEVER touches the controller (bit-exact off).
    void update(x3::phys::IVehicleController& ctl, float dt,
                x3::phys::VehicleInput& eff);

    // Zero the dynamic state (grips back to 1 next update).
    void reset();

    // ---- Queries (HUD / VFX / tests) ----
    bool  drifting() const        { return m_drifting; }
    float slipAngleDeg() const    { return m_slipDeg; }
    float rearLatScale() const    { return m_rearLat; }
    float assistSteer() const     { return m_assist; }
    DriveSurface wheelSurface(uint32_t i) const {
        return (i < kMaxWheels) ? m_surface[i] : DriveSurface::Road;
    }
    // 0..1 particulate kick for wheel i this tick (surface spray x slip x
    // speed). The tire-spray VFX reads this; deterministic.
    float spray(uint32_t i) const { return (i < kMaxWheels) ? m_spray[i] : 0.0f; }

private:
    bool m_driftOn   = false;
    bool m_surfaceOn = false;
    DriftParams m_p;
    SurfaceQuery m_query;
    uint32_t m_rearA = 2, m_rearB = 3;
    float m_external = 1.0f;

    // Dynamic state.
    bool  m_wasActive = false;   // pushed identity on deactivate already?
    bool  m_drifting = false;
    float m_slipDeg  = 0.0f;
    float m_rearLat  = 1.0f;
    float m_frontLat = 1.0f;
    float m_assist   = 0.0f;
    DriveSurface m_surface[kMaxWheels] = {};
    float m_spray[kMaxWheels] = {};
};

// Headless self-test (--test-driftgrip). Flat-slab Jolt world + the real
// DriveDemo rig. Asserts: OFF is bit-exact (identical trajectories with the
// layer disabled vs never constructed); drift entry on throttle+steer+
// handbrake reaches a real slip angle AND recovers to straight running
// (controllable, not a spin); a NEGATIVE CONTROL with the anti-spin ceiling
// removed spins out (proves the stabilizer earns its place); countersteer
// assist shortens recovery vs assist off; dirt brakes longer and corners
// wider than asphalt; the DITCH CLIMB acceptance (drop the rig beside the
// corridor road, throttle, assert it regains the roadway) with a bald-tire
// negative control that fails the climb. Logs PASS/FAIL D#; true iff all pass.
bool runDriftGripSelfTest();

} // namespace x3::game
