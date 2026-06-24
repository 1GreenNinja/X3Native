#pragma once
// PERFORMANCE PARTS (x3.vehparts/1) — the data + composition core of the
// performance shop ("drive in, build your car").
//
// Three layers, all headless-testable (no Vulkan):
//   * Catalog      — the parts catalog parsed from assets/vehicles/parts.json
//                    (format docs/design/VEHPARTS_FORMAT.md): 11 categories with
//                    REAL stat semantics (cam curves, FI boost, ECU knock data,
//                    grip/mass/brake/suspension numbers, nitrous).
//   * VehicleBuild — what's INSTALLED + the ECU tune + credits + engine damage +
//                    the nitrous tank. JSON save/load beside the checkpoint saves
//                    (the dialog-flags-adjacent persistence pattern).
//   * compose()    — Catalog + VehicleBuild -> ComposedBuild: the engine-layer
//                    x3::phys::WheeledTuning (lowered onto the LIVE Jolt vehicle
//                    via DriveDemo::applyTuning), the audio profile (exhaust note
//                    variant, SC whine, turbo spool/whistle), the nitrous spec,
//                    and the dyno model (torque/power curve sampling + the knock/
//                    LIMIT-POP thresholds).
//
// Game/slice code only — engine/ stays pure (the only engine type used is the
// public WheeledTuning POD).

#include "engine/physics/IVehicle.h"   // WheeledTuning (the composition target)

#include <string>
#include <vector>

namespace x3::game::vehparts {

// ---------------------------------------------------------------------------
// Catalog
// ---------------------------------------------------------------------------
struct CurvePt { float x = 0.0f, y = 1.0f; };

// One purchasable part. Stat fields are a pragmatic union across categories —
// each category reads only its own fields (defaults are inert). See the format doc.
struct Part {
    std::string id;          // stable unique id ("exh_titanium")
    std::string category;    // owning category id ("exhaust")
    std::string name;        // display name
    int   tier  = 1;         // 1..4, ascending = better
    int   price = 0;         // credits

    // camshaft
    std::vector<CurvePt> camCurve;   // replaces the baseline normalized torque curve
    float redlineBonus = 0.0f;       // rpm added to maxRpm
    // exhaust (+ shared power% with intake/intercooler)
    float powerPct    = 0.0f;
    int   noteId      = 0;
    float pitchOffset = 0.0f;
    float timbre      = 0.0f;
    // intercooler
    float safeBoostBonus = 0.0f;     // bar added to the ECU safe-boost threshold
    // forced induction
    std::string fiType;              // "supercharger" | "turbo" | ""
    float boostPowerPct = 0.0f;      // torque % at full ECU boost
    float spoolLagS     = 0.0f;      // turbo spool time at WOT
    float topEndBias    = 0.0f;      // 0..1 — how top-end-heavy the turbo gain is
    bool  whine         = false;     // SC whine layer
    bool  whistle       = false;     // turbo whistle + blowoff
    // ecu
    float maxBoost      = 0.0f;      // bar (boost slider max)
    float safeBoost     = 0.0f;      // bar (knock builds above this + IC bonus)
    float safeLean      = 0.0f;      // fuel-slider knock threshold (lean side)
    float safeTiming    = 0.0f;      // timing-slider knock threshold
    float knockLimit    = 0.0f;      // pop at/above this knock index
    float powerPerBoost = 0.0f;      // % torque per bar effective boost
    float powerPerTiming= 0.0f;      // % torque at full timing advance
    float leanPowerPct  = 0.0f;      // % torque bonus at the lean edge
    int   repairCost    = 0;         // credits to fix a popped engine
    // tires
    float gripScale = 0.0f;          // 0 = not a tire part
    std::string compound;
    // suspension
    float rideHeightDelta = 0.0f;
    float suspFreq = 0.0f, suspDamp = 0.0f;
    // brakes
    float brakeTorque = 0.0f;        // 0 = not a brake part
    // weight
    float massDelta = 0.0f;          // kg (negative)
    // nitrous
    float nitrousMult = 0.0f;        // torque multiplier while spraying (0 = none)
    float tankSeconds = 0.0f;
    int   refillCost  = 0;
};

// The stock car the parts modify (the `baseline` block).
struct Baseline {
    float torqueNm    = 700.0f;
    float maxRpm      = 6500.0f;
    float massKg      = 1300.0f;
    float brakeTorque = 2200.0f;
    float suspFreq    = 2.2f;
    float suspDamp    = 0.7f;
    std::vector<CurvePt> curve;      // stock normalized torque curve
};

struct Category { std::string id, label; };

class Catalog {
public:
    // Parse assets/vehicles/parts.json (format x3.vehparts/1). Returns false on
    // file/parse/format failure (catalog left empty).
    bool loadFile(const std::string& path);
    bool loadJson(const std::string& json);

    bool ok() const { return m_ok; }
    const Baseline& baseline() const { return m_base; }
    // Swap the baseline the parts compose against — lets the CAR ROSTER reuse the
    // shared parts catalog while substituting each car's own stock stats
    // (app/roster.cpp). Pure data swap; the parts list is untouched.
    void setBaseline(const Baseline& b) { m_base = b; }
    const std::vector<Category>& categories() const { return m_categories; }
    const std::vector<Part>&     parts() const { return m_parts; }
    const Part* find(const std::string& id) const;
    std::vector<const Part*> inCategory(const std::string& cat) const;

private:
    bool m_ok = false;
    Baseline m_base;
    std::vector<Category> m_categories;
    std::vector<Part> m_parts;
};

// Default catalog path (repo assets root + /vehicles/parts.json).
std::string defaultCatalogPath();

// ---------------------------------------------------------------------------
// VehicleBuild — what the player owns/installed + the live ECU tune.
// ---------------------------------------------------------------------------
struct EcuTune {
    float boost  = 0.0f;   // bar [0 .. ecu.maxBoost]
    float fuel   = 1.0f;   // mixture scale: <1 rich, 1 stoich, >1 lean
    float timing = 0.3f;   // advance [0..1]
};

struct VehicleBuild {
    // category id -> installed part id ("" / absent = stock).
    std::vector<std::pair<std::string, std::string>> installed;
    EcuTune tune;
    bool  engineDamaged   = false;   // popped on the dyno; x0.85 power until repaired
    float nitrousRemaining= 0.0f;    // seconds left in the tank
    int   credits         = 12000;   // the wallet (granted by the world / seeded)

    const std::string* installedIn(const std::string& category) const;
    void  install(const std::string& category, const std::string& partId);
    void  removeFrom(const std::string& category);

    // JSON persistence beside the checkpoint saves (vehbuild.json). Versioned by
    // the x3.vehparts/1 tag; load fails gracefully (false, *this untouched).
    std::string toJson() const;
    bool        fromJson(const std::string& json);
    bool        saveFile(const std::string& path) const;
    bool        loadFile(const std::string& path);
};

// Default build save path (working dir, beside the engine's checkpoint file).
std::string defaultBuildSavePath();

// ---------------------------------------------------------------------------
// Composition — Catalog + VehicleBuild -> physics + audio + dyno.
// ---------------------------------------------------------------------------
struct ComposedBuild {
    x3::phys::WheeledTuning tuning;  // lower this onto the live Jolt vehicle
    float massKg     = 1300.0f;      // final curb mass (also in tuning.massKg)
    float peakTorque = 700.0f;       // Nm (over the final curve)
    float peakPowerKw= 0.0f;         // kW (torque * omega)
    float peakTorqueRpm = 0.0f, peakPowerRpm = 0.0f;
    // audio profile
    int   exhaustNote = 0;           // 0 = stock loop
    float exhaustPitchOffset = 0.0f;
    float exhaustTimbre      = 0.0f;
    bool  scWhine     = false;       // supercharger whine layer
    bool  turboWhistle= false;       // turbo whistle + blowoff
    float turboSpoolS = 0.0f;
    // nitrous
    float nitrousMult = 0.0f;        // 0 = no kit installed
    float nitrousTankS= 0.0f;
    int   nitrousRefillCost = 0;
    // ECU / dyno data
    float ecuMaxBoost = 0.0f;        // 0 = no ECU installed (sliders locked)
    float knockIndex  = 0.0f;        // knock at the CURRENT tune
    float knockLimit  = 1e9f;        // pop threshold (1e9 = can't pop, no ECU)
    bool  willPop     = false;       // knockIndex >= knockLimit (a pull pops it)
    int   repairCost  = 0;
    // Final normalized torque curve, sampled (for the dyno trace).
    float torqueAtRpmFrac(float rpmFrac) const;    // Nm at rpmFrac of maxRpm
    float powerKwAtRpmFrac(float rpmFrac) const;   // kW at rpmFrac
    float maxRpm = 6500.0f;
    std::vector<CurvePt> finalCurve; // normalized (x = rpmFrac, y = torqueFrac)
};

// Compose the build. Pure math; never fails (missing parts = stock).
ComposedBuild compose(const Catalog& cat, const VehicleBuild& build);

// Knock index for an arbitrary tune on this build's ECU/IC (the dyno PULL check).
// Returns 0 when no ECU is installed (sliders locked -> can't pop).
float knockIndexFor(const Catalog& cat, const VehicleBuild& build, const EcuTune& tune);

// Headless self-test (--test-vehparts): catalog parse (categories/tiers/prices),
// composition math (power/mass/grip ordering), REAL Jolt physics deltas (stock vs
// street vs full-built 0->target-speed tick counts strictly ordered; weight+brakes
// shorten braking distance; tires raise the lateral-grip heading change; nitrous
// accelerates harder), dyno knock/POP thresholds (safe tune never pops, abusive
// tune pops, damage costs power, repair restores), and the VehicleBuild JSON
// round-trip. Logs PASS/FAIL P#; returns true iff all pass. No window / Vulkan.
bool runVehPartsSelfTest();

} // namespace x3::game::vehparts
