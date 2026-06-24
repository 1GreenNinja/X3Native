#pragma once
// CAR ROSTER (x3.vehicle/1) — the drivable-fleet data layer over the proven
// single-hero DriveDemo + vehparts (x3.vehparts/1) systems.
//
// One roster.json (assets/vehicles/roster.json, format x3.vehicle/1) lists every
// drivable car: a display name, a class, the converted GLB (Vehicles/<name>.glb),
// a PAINT tint, the chassis footprint (half extents + ride height), and a BASELINE
// stat block that is FIELD-COMPATIBLE with vehparts::Baseline (torque/rpm/mass/
// brake/suspension + a normalized torque curve) plus the drivetrain + grip baseline.
//
// Because each car's baseline is exactly the vehparts Baseline shape, ANY roster
// car is a valid input to vehparts::compose() — so every car can be driven into
// LATE NIGHT SPEED and upgraded. The roster self-test (--test-roster) proves this:
// for each entry it composes a full parts build onto the car's baseline and asserts
// the resulting Jolt WheeledTuning resolves (finite, ordered power gain), the GLB is
// present (via the loose dir / store manifest) or gracefully skipped, the wheel
// nodes resolve for spinning, and the stats are sane.
//
// Game/slice code only; engine/ stays pure (only the public vehparts + WheeledTuning
// types are touched).

#include "vehparts.h"   // Baseline / Catalog / compose() — the parts-compat target

#include <string>
#include <vector>

namespace x3::game::roster {

// Drivetrain layout (informational + feeds the Jolt powered/steered wheel split
// when a car is spawned as a live DriveDemo; today the hero is RWD).
enum class Drivetrain { RWD, FWD, AWD };
const char* drivetrainName(Drivetrain d);
Drivetrain  drivetrainFromString(const std::string& s);

// Broad class buckets (for spawn tables + the carshow grouping + UI).
//   street  : everyday/sport coupes & sedans
//   muscle  : big-displacement RWD muscle
//   super   : exotics / high-power track cars
//   utility : trucks / vans / SUVs
//   bike    : motorcycles (2-wheel; spawned as a narrow chassis)
//   alien   : Act2+ alien-tech vehicles (reserved; hover/AWD-ish)
enum class VClass { Street, Muscle, Super, Utility, Bike, Alien };
const char* classNameOf(VClass c);
VClass      classFromString(const std::string& s);

// One roster entry. The `base` block is the vehparts Baseline (so compose() runs
// on it directly). `glb` is the relative path under convertedGlbRoot()
// ("Vehicles/CTR.glb"). `tint` multiplies the GLB clearcoat paint baseColor.
struct Car {
    std::string id;             // stable key ("ctr", "skyline", ...)
    std::string name;           // display ("RUF CTR Yellowbird")
    VClass      cls = VClass::Street;
    std::string source;         // provenance pack ("RCC v4")
    std::string glb;            // "Vehicles/CTR.glb" (relative to convertedGlbRoot)
    float       tint[3] = { 0.8f, 0.8f, 0.85f };

    Drivetrain  drivetrain = Drivetrain::RWD;
    // Chassis footprint (half extents, metres) + ride height — sizes the Jolt box
    // + the camera framing. Defaults are the CTR (hero) footprint.
    float       halfExtents[3] = { 0.84f, 0.5f, 1.95f };
    float       rideHeight     = 0.30f;

    // BASELINE stats — exactly vehparts::Baseline (torque/rpm/mass/brake/susp + curve).
    vehparts::Baseline base;

    // The composed parts build runs against `base`; this gives the parts system its
    // catalog (shared assets/vehicles/parts.json). Filled by load() from the same
    // catalog so every car is upgrade-compatible without per-car part lists.
};

// The loaded roster.
class Roster {
public:
    // Parse assets/vehicles/roster.json (format x3.vehicle/1). Returns false on
    // file/parse/format failure (roster left empty).
    bool loadFile(const std::string& path);
    bool loadJson(const std::string& json);

    bool ok() const { return m_ok; }
    const std::vector<Car>& cars() const { return m_cars; }
    const Car* find(const std::string& id) const;
    size_t size() const { return m_cars.size(); }

private:
    bool m_ok = false;
    std::vector<Car> m_cars;
};

// Default roster path (repo assets root + /vehicles/roster.json).
std::string defaultRosterPath();

// Headless self-test (--test-roster): loads roster.json + parts.json and, for EACH
// car, asserts: (1) stats are finite + sane (mass/torque/rpm/brake positive, curve
// ascending in rpmFrac, monotone-ish), (2) the car's baseline composes with a full
// parts build into a finite Jolt WheeledTuning whose peak power EXCEEDS the stock
// baseline (parts-system compatibility), (3) the converted GLB is present under
// convertedGlbRoot() OR listed in the asset-store manifest (graceful "skip"
// otherwise, logged — not a failure when the store hasn't been fetched), and
// (4) the GLB, when present, exposes the 4 wheel nodes (Wheel_FL/FR/RL/RR) needed
// for independent spin. Also asserts the spawn table instantiates (count > 0).
// Logs PASS/FAIL; returns true iff all hard checks pass. No window / Vulkan.
bool runRosterSelfTest();

} // namespace x3::game::roster
