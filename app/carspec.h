#pragma once
// ===========================================================================
// CarSpec — the PER-CAR character table.
//
// Tim, 2026-08-16: "ALL the cars NEED his fixes ... Every car gets the method,
// with its own variables."
//
// DS's vehicle lane (laexe/vehicle-feel-gauges) built the METHOD: a real engine
// model, forced induction with manifold pressure and lag, a boost gauge, TC
// deadband, clutch lock. What it did NOT build is a second car. Every figure it
// runs on is a 993/992 Turbo's — 800 Nm, 7500 rpm, 1300 kg, a flat-six's light
// flywheel and its turbo step — and those constants are spread across three
// files (app/vehicle.cpp buildPhysics, engine/physics/JoltVehicle.cpp's two
// defaults, host_tunnel.cpp's car_reset). Load an E46 GLB and you get a BMW
// body making Porsche noises, because the ONLY thing that changed was the mesh.
//
// The infrastructure for this already existed and was stuck at quantity one:
// app/vehparts.h's `Baseline` is documented as "the stock car the parts modify"
// and its defaults are 700 Nm / 6500 rpm / 1300 kg -- which is EXACTLY the
// `ctr` row of assets/vehicles/cars.json. So the bug was never "there is no
// base spec", it was "there is one base spec, globally, for eleven cars".
//
// WHERE THE NUMBERS COME FROM. assets/vehicles/cars.json, authored by Tim on
// 2026-08-14 and — until now — read by nothing at all: zero references in the
// tree. It carries four cars and, more importantly, the design thesis:
//
//     "WHY comHeight MATTERS MOST: it is the single parameter that separates
//      these cars in feel. The Plaid's floor battery is why it corners flat;
//      the Cobra's iron blower motor sitting high is why it leans. Torque
//      alone will not do it."
//
// This table is built to honour that. comHeight and trackM are first-class
// fields, not an afterthought behind the horsepower.
//
// TWO SOURCES, ONE TRUTH. The JSON is authorable and wins. The compiled table
// below is a FALLBACK so a missing/renamed asset file cannot leave the game
// with no cars and so --test-carspec runs with no file dependency at all
// (determinism: no I/O in the test path). --test-carspec asserts the two AGREE
// on every car they share, which is what stops them drifting apart silently —
// the failure mode this codebase keeps hitting.
//
// ANTI-SLOP. Eleven cars, each grounded in a real one: its own torque peak,
// redline, mass, flywheel inertia, gearing, grip, centre-of-mass height and
// engine voice. A truck is NOT a sports car with the numbers scaled down — it
// is heavy, low-revving, high-torque, soft-sprung, and it idles a long way
// below where a flat-six does. If this table ever reads as one row multiplied,
// it has failed and should be sent back.
// ===========================================================================

#include <cstdint>
#include <string>
#include <vector>

#include "engine/physics/IVehicle.h"   // WheeledTuning / WheeledVehicleDesc

namespace x3::game {

// How the drive is laid out. Affects which wheels are powered and how the
// launch hooks up -- an AWD car puts its torque down where a RWD one lights
// the rears up.
enum class Drivetrain : uint8_t { RWD, FWD, AWD };

// ---------------------------------------------------------------------------
// One car. Everything that makes it feel like ITSELF rather than like the
// hero car with a different mesh.
// ---------------------------------------------------------------------------
struct CarSpec {
    // ---- identity ----
    std::string id;            // stable key ("e46"), matches cars.json
    std::string name;          // what the garage shows ("E46 SPORT")
    std::string glb;           // "Vehicles/E46_New.glb" ("" = spec with no art yet)
    std::string note;          // why this car feels the way it does (design intent)

    // ---- chassis ----
    Drivetrain drive   = Drivetrain::RWD;
    float massKg       = 1300.0f;
    // CENTRE OF MASS above the ground (m). Tim's thesis field: rollover
    // threshold ~ atan(halfTrack / comHeight), so this and trackM together are
    // what decide whether a car corners flat or leans onto its outside wheel.
    float comHeight    = 0.46f;
    float trackM       = 1.354f;   // full axle width (m)
    float halfExtents[3] = { 0.84f, 0.50f, 1.95f };   // chassis box (m)

    // ---- engine ----
    float torqueNm     = 700.0f;   // peak crank torque
    float maxRpm       = 6500.0f;  // redline
    // Flywheel inertia (kg m^2). How fast the crank can CHANGE speed — felt
    // long before peak torque is. A light-valvetrain six is ~0.20; a big iron
    // V8 is ~0.55; a diesel lorry is measured in whole units.
    float engineInertia = 0.35f;
    // Normalised torque curve (rpmFrac -> torqueFrac), ascending, max 8 points.
    // The SHAPE is the character: a turbo's step, a V8's fat plateau off idle,
    // an NA screamer still climbing at the limiter.
    x3::phys::EngineCurvePoint curve[8];
    uint32_t curvePoints = 0;

    // ---- drivetrain ----
    float gearRatios[8] = { 0,0,0,0,0,0,0,0 };
    uint32_t gearCount  = 0;
    float finalDrive    = 0.0f;    // <= 0 leaves Jolt's differential ratio
    float clutchStrength = 100.0f;
    bool  singleSpeed   = false;   // EVs: one ratio, no shift points
    bool  evPowerband   = false;   // instant torque from 0 rpm, flat curve

    // ---- tyres / brakes / springs ----
    float gripScale    = 1.7f;     // multiplies Jolt's economy tyre curve
    float brakeTorque  = 2200.0f;
    float suspFreq     = 2.2f;     // Hz
    float suspDamp     = 0.7f;

    // ---- forced induction (the transient the curve cannot express) ----
    bool  turbo        = false;
    float turboMaxPsi  = 16.0f;
    float turboSpoolS  = 0.45f;

    // ---- VOICE -----------------------------------------------------------
    // With ONE engine loop sample, a car's voice is where its rpm range lands
    // on the playback rate. host_tunnel.cpp had this hardcoded three times
    // (redline 7500, rpm/1071, idle 0.75) — calibrated to the flat-six, which
    // is precisely why every GLB sounded like a Porsche.
    //
    // Expressed as DESIGN intent rather than a magic divisor: what playback
    // rate the note should reach at the redline, and where it sits at idle.
    // The engine derives the divisor. Keep pitchAtRedline <= 7.5 (the mixer
    // clamps at 8.0x).
    float idleRpm        = 800.0f;
    float pitchAtRedline = 7.0f;   // 993 calibration: 7500 rpm -> 7.0x
    float idlePitch      = 0.75f;
    float exhaustTimbre  = 0.0f;   // reserved for the multi-sample pass

    // Derived: the rpm that plays the loop at unity rate.
    float pitchUnityRpm() const {
        return (pitchAtRedline > 0.01f) ? (maxRpm / pitchAtRedline) : 1071.0f;
    }

    // Lower this spec onto a fresh Jolt build (engine, gearing, curve).
    // Does NOT touch chassis/wheels — the caller owns body creation.
    void applyTo(x3::phys::WheeledVehicleDesc& vd) const;

    // The same spec as a LIVE retune (what a car swap applies to a running
    // rig, and the STOCK figure the performance shop composes on top of).
    x3::phys::WheeledTuning asTuning() const;
};

// ---------------------------------------------------------------------------
// The roster.
// ---------------------------------------------------------------------------
class CarCatalog {
public:
    // Compiled-in table (11 cars). Always succeeds, no I/O — this is what the
    // headless tests run against and what the game falls back to.
    static const CarCatalog& builtin();

    // Load assets/vehicles/cars.json over a copy of the builtin: entries are
    // matched by `id` and OVERRIDE field-by-field, unknown ids are appended.
    // Returns false (and leaves the builtin contents intact) on a missing or
    // malformed file — a bad edit degrades to the shipped roster, it does not
    // leave the player with no cars.
    bool loadJson(const std::string& json);
    bool loadFile();                       // resolves the asset path, then loadJson

    const CarSpec* find(const std::string& id) const;
    const CarSpec* forGlb(const std::string& glbRelPath) const;
    const std::vector<CarSpec>& all() const { return m_cars; }
    size_t size() const { return m_cars.size(); }

    // Process-wide roster: builtin, with cars.json applied over it if present.
    // Loaded once on first use.
    static const CarCatalog& game();

private:
    std::vector<CarSpec> m_cars;
};

// --test-carspec — the table's headless suite (negative control included).
bool runCarSpecSelfTest();

} // namespace x3::game
