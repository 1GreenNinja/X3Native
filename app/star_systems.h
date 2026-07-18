// ============================================================================
// STAR SYSTEMS registry (x3.starsys/1) — the named-star-system DATA layer.
//
// A clean, HEADLESS-testable module: pure data + lookups + a validator, NO rhi
// / device dependency (so --test-starsystems runs with no Vulkan). It answers
// "which system, what star, which bodies" — the sky renderer (app/cinematic.cpp
// NightSkyPlanet machinery) and the dogfight HUD minimap read from here.
//
// Each system names a real (or, for Kethzar, fictional) star, its colour + class
// (drives the local sun tint), and a small set of BODIES. Every body maps onto a
// FORGE3D night-sky planet TYPE the sky renderer already loads (see
// loadNightSkyPlanets): Moon / Ice / Gas / Lava / Terrestrial / Sun. The bridge
// buildSystemSky() (cinematic.h) clones the loaded texture templates by type and
// hangs them at each body's sky azimuth/elevation/apparent-diameter.
//
// Coordination: the station-scene agent (feat/space-station-land) can later set
// `station.systemId = "kethzar_prime"` against these stable ids without pulling
// in any render code. Ids are the contract; do not rename them casually.
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace x3::starsys {

// A celestial-body kind that maps DIRECTLY onto the FORGE3D night-sky planet type
// indices the renderer uses (NightSkyPlanet::typeIndex). Values are load-bearing.
enum class BodyType : uint32_t {
    Moon        = 0,
    Ice         = 1,
    Gas         = 2,   // gas giant (gets the ring annulus in the renderer)
    Lava        = 3,
    Terrestrial = 4,
    Sun         = 8,   // the system's local star
};

// One body hanging in a system's sky. Position is a sky DIRECTION (az/el) + an
// apparent angular diameter, exactly like NightSkyPlanet — so it is translation-
// invariant (never approaches the player). `label` is what the minimap shows.
struct SystemBody {
    BodyType    type;
    float       azimuthDeg;         // world-space sky azimuth (0 = -Z / "north", +90 = +X / "east")
    float       elevationDeg;       // above the horizon
    float       angularDiameterDeg; // apparent full-disc size
    const char* label;              // display name, e.g. "Kethzar II"
};

// A named star system: its star (colour + class for the sun tint) + its bodies.
// The local star is ALSO present in `bodies` as a Sun-type entry (so the sky
// renderer draws it); `starColor`/`starClass` are the summary used for tinting
// and UI. `distanceLy` is from Sol (0.0 for Sol itself).
struct StarSystem {
    const char*             id;         // stable key, e.g. "kethzar_prime"
    const char*             name;       // display, e.g. "Kethzar Prime"
    float                   distanceLy; // light-years from Sol
    float                   starColor[3];
    const char*             starClass;  // e.g. "G2V", "K2V", "M6.5V red dwarf"
    std::vector<SystemBody> bodies;
};

// ---- Registry access -------------------------------------------------------

// All shipped systems, in a stable order (Sol first). Built once, cached.
const std::vector<StarSystem>& allSystems();

// Count of shipped systems.
int systemCount();

// Look a system up by its stable id (e.g. "tau_ceti"). Returns nullptr if none.
const StarSystem* findSystem(const char* id);

// Look a system up by display name (e.g. "Kethzar Prime"), case-sensitive.
// Returns nullptr if none. (findSystem by id is the primary path; this is for UI.)
const StarSystem* findSystemByName(const char* name);

// The system the interactive dogfight is set in (owner: "far from earth"): the
// far-distance backdrop + minimap read this. Convenience wrapper over findSystem.
const StarSystem& dogfightSystem();
// The id of the dogfight system (single source of truth).
inline constexpr const char* kDogfightSystemId = "kethzar_prime";

// The faint distant SOL pinpoint's sky slot, shared by the dogfight far-sky
// (buildSystemSky appends a tiny Sun-type body here when the system isn't Sol),
// its HUD "SOL" label, and the minimap SOL icon — so "far from Earth" reads the
// same everywhere. A single, small, high-in-the-sky point clear of the hero world.
inline constexpr float kSolPinpointAzDeg   = 96.0f;
inline constexpr float kSolPinpointElDeg   = 13.0f;   // low, near the combat plane so it's in view
inline constexpr float kSolPinpointDiamDeg = 0.70f;

// ---- Validation (drives --test-starsystems) --------------------------------

// Registry integrity: every system has a non-empty id/name, distance >= 0, a
// star colour in [0,1], AT LEAST ONE Sun body and at least one non-Sun body, and
// every body carries a known BodyType + a label. On failure, `*err` (if non-null)
// gets a human-readable reason. Returns true iff the whole registry is sound.
bool validateRegistry(std::string* err);

// Headless registry-integrity self-test (drives --test-starsystems). Returns
// true iff every check passes. No window / Vulkan.
bool runStarSystemsSelfTest();

} // namespace x3::starsys
