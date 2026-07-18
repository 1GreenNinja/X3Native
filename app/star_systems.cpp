// ============================================================================
// STAR SYSTEMS registry (x3.starsys/1) — see star_systems.h.
//
// The shipped systems TABLE lives here (one legible entry each). Real distances
// and star classes are period-accurate; Kethzar Prime is the fiction. Body sky
// layouts (az/el/diam) are hand-tuned so each system reads as a DISTINCT sky
// (the dogfight is set in Kethzar Prime — a dramatic amber-star sky, far from
// Sol) while every body stays above the horizon and inside the far plane.
// ============================================================================
#include "star_systems.h"

#include "engine/core/x3_log.h"

#include <cstring>
#include <string>

namespace x3::starsys {

// ---------------------------------------------------------------------------
// THE TABLE — the one spot to add/tune a system. Built once, cached in a static.
//   starColor : linear RGB used to tint the local sun / sky (roughly the star's
//               black-body colour by class: G ~ warm white, K ~ orange, M ~ red).
//   bodies    : the local star (a Sun entry) + its worlds, each hung at a sky
//               azimuth (0 = -Z "north", +90 = +X "east") / elevation / apparent
//               diameter. Keep elevation >= ~12 so nothing rides the horizon.
// ---------------------------------------------------------------------------
static std::vector<StarSystem> buildTable() {
    std::vector<StarSystem> t;

    // -- Sol (home; G2V) ----------------------------------------------------
    t.push_back({
        "sol", "Sol", 0.0f, { 1.00f, 0.96f, 0.86f }, "G2V",
        {
            { BodyType::Sun,          28.0f, 16.0f, 3.5f, "Sol" },
            { BodyType::Terrestrial, -22.0f, 22.0f, 7.0f, "Earth" },
            { BodyType::Moon,        -44.0f, 30.0f, 2.5f, "Luna" },
            { BodyType::Gas,        -147.0f, 24.0f, 9.0f, "Jupiter" },
        }
    });

    // -- Alpha Centauri (real, 4.37 ly; G2V + K1V pair) ---------------------
    t.push_back({
        "alpha_centauri", "Alpha Centauri", 4.37f, { 1.00f, 0.93f, 0.80f }, "G2V + K1V",
        {
            { BodyType::Sun,          10.0f, 20.0f, 3.8f, "Rigil Kentaurus" },
            { BodyType::Terrestrial,  62.0f, 26.0f, 5.5f, "Proxima b" },
            { BodyType::Moon,         88.0f, 34.0f, 2.0f, "Toliman Minor" },
        }
    });

    // -- Tau Ceti (real, 11.9 ly; G8.5V, several rocky worlds) --------------
    t.push_back({
        "tau_ceti", "Tau Ceti", 11.9f, { 1.00f, 0.90f, 0.72f }, "G8.5V",
        {
            { BodyType::Sun,         -18.0f, 18.0f, 3.4f, "Tau Ceti" },
            { BodyType::Terrestrial,  34.0f, 24.0f, 6.0f, "Tau Ceti e" },
            { BodyType::Terrestrial, 120.0f, 20.0f, 4.0f, "Tau Ceti f" },
            { BodyType::Moon,        -60.0f, 40.0f, 2.2f, "Tau Ceti e-I" },
        }
    });

    // -- Epsilon Eridani ("Eridani", real, 10.5 ly; K2V orange, a gas giant) -
    t.push_back({
        "epsilon_eridani", "Epsilon Eridani", 10.5f, { 1.00f, 0.72f, 0.42f }, "K2V",
        {
            { BodyType::Sun,          24.0f, 17.0f, 3.6f, "Eridani" },
            { BodyType::Gas,        -130.0f, 26.0f, 8.5f, "Eridani b (AEgir)" },
            { BodyType::Moon,       -100.0f, 42.0f, 2.0f, "AEgir-III" },
            { BodyType::Ice,          70.0f, 30.0f, 2.4f, "Eridani c" },
        }
    });

    // -- Wolf 359 (real, 7.86 ly; M6.5V red dwarf, dim) ---------------------
    t.push_back({
        "wolf_359", "Wolf 359", 7.86f, { 1.00f, 0.45f, 0.30f }, "M6.5V red dwarf",
        {
            { BodyType::Sun,          -8.0f, 15.0f, 2.6f, "Wolf 359" },
            { BodyType::Ice,          48.0f, 28.0f, 3.0f, "Wolf 359 b" },
            { BodyType::Moon,        -70.0f, 36.0f, 2.2f, "Wolf 359 b-I" },
        }
    });

    // -- TRAPPIST-1 (real, 40.7 ly; M8V ultracool dwarf, PACKED rocky worlds) -
    t.push_back({
        "trappist_1", "TRAPPIST-1", 40.7f, { 1.00f, 0.42f, 0.28f }, "M8V red dwarf",
        {
            { BodyType::Sun,           0.0f, 14.0f, 2.4f, "TRAPPIST-1" },
            { BodyType::Terrestrial,  30.0f, 22.0f, 5.5f, "TRAPPIST-1e" },
            { BodyType::Ice,          58.0f, 30.0f, 3.4f, "TRAPPIST-1f" },
            { BodyType::Terrestrial,  84.0f, 20.0f, 4.5f, "TRAPPIST-1g" },
            { BodyType::Lava,        -34.0f, 24.0f, 3.0f, "TRAPPIST-1b" },
        }
    });

    // -- Kethzar Prime (FICTION — the dogfight is HERE; amber hypergiant sky) -
    //    Far from Sol: an amber-gold star, a huge hero LAVA world, a ringed gas
    //    giant on the far side, an ice world high, a moon. The most dramatic sky
    //    in the registry — chosen for the interactive space beat.
    //    SPACE layout: unlike the facility night sky (which keeps bodies high so
    //    nothing rides the roofline), a deep-space dogfight has a 360 sky and the
    //    chase camera looks LEVEL at the enemies — so the bodies sit LOW (near the
    //    combat plane) and SPREAD around the azimuth, so whichever way the wing
    //    turns, a body drifts through the view. The hero lava world is huge.
    t.push_back({
        "kethzar_prime", "Kethzar Prime", 214.0f, { 1.00f, 0.62f, 0.20f }, "K0 hypergiant (Kethzar)",
        {
            { BodyType::Sun,          45.0f, 12.0f,  4.6f, "Kethzar" },       // amber star, near the plane
            { BodyType::Lava,        -35.0f,  9.0f, 12.0f, "Kethzar II" },    // the HERO world — dominant
            { BodyType::Gas,         160.0f, 14.0f,  9.0f, "Kethzar V" },     // ringed, opposite side
            { BodyType::Ice,        -110.0f, 16.0f,  3.2f, "Kethzar VII" },
            { BodyType::Moon,        105.0f,  7.0f,  2.6f, "Ashk (moon)" },
        }
    });

    return t;
}

const std::vector<StarSystem>& allSystems() {
    static const std::vector<StarSystem> table = buildTable();
    return table;
}

int systemCount() { return (int)allSystems().size(); }

const StarSystem* findSystem(const char* id) {
    if (!id) return nullptr;
    for (const StarSystem& s : allSystems())
        if (s.id && std::strcmp(s.id, id) == 0) return &s;
    return nullptr;
}

const StarSystem* findSystemByName(const char* name) {
    if (!name) return nullptr;
    for (const StarSystem& s : allSystems())
        if (s.name && std::strcmp(s.name, name) == 0) return &s;
    return nullptr;
}

const StarSystem& dogfightSystem() {
    const StarSystem* s = findSystem(kDogfightSystemId);
    // The validator guarantees this exists; fall back to Sol defensively.
    return s ? *s : allSystems().front();
}

static bool isKnownType(BodyType t) {
    switch (t) {
        case BodyType::Moon: case BodyType::Ice: case BodyType::Gas:
        case BodyType::Lava: case BodyType::Terrestrial: case BodyType::Sun:
            return true;
    }
    return false;
}

bool validateRegistry(std::string* err) {
    auto fail = [&](const std::string& why) { if (err) *err = why; return false; };
    const auto& sys = allSystems();
    if (sys.empty()) return fail("registry is empty");
    for (const StarSystem& s : sys) {
        const std::string tag = std::string("system '") + (s.id ? s.id : "<null>") + "': ";
        if (!s.id || !*s.id)     return fail(tag + "empty id");
        if (!s.name || !*s.name) return fail(tag + "empty name");
        if (!s.starClass || !*s.starClass) return fail(tag + "empty starClass");
        if (s.distanceLy < 0.0f) return fail(tag + "negative distance");
        for (int c = 0; c < 3; ++c)
            if (s.starColor[c] < 0.0f || s.starColor[c] > 1.0f)
                return fail(tag + "star colour out of [0,1]");
        if (s.bodies.empty())    return fail(tag + "no bodies");
        int nSun = 0, nOther = 0;
        for (const SystemBody& b : s.bodies) {
            if (!isKnownType(b.type)) return fail(tag + "unknown body type");
            if (!b.label || !*b.label) return fail(tag + "body with empty label");
            if (b.elevationDeg < 0.0f || b.elevationDeg > 90.0f)
                return fail(tag + std::string("body '") + b.label + "' elevation out of [0,90]");
            if (b.angularDiameterDeg <= 0.0f || b.angularDiameterDeg > 45.0f)
                return fail(tag + std::string("body '") + b.label + "' angular diameter out of (0,45]");
            if (b.type == BodyType::Sun) ++nSun; else ++nOther;
        }
        if (nSun < 1)   return fail(tag + "no Sun (star) body");
        if (nOther < 1) return fail(tag + "no non-star bodies");
    }
    // Ids must be unique.
    for (size_t i = 0; i < sys.size(); ++i)
        for (size_t j = i + 1; j < sys.size(); ++j)
            if (std::strcmp(sys[i].id, sys[j].id) == 0)
                return fail(std::string("duplicate id '") + sys[i].id + "'");
    // The dogfight system must resolve.
    if (!findSystem(kDogfightSystemId))
        return fail(std::string("dogfight system id '") + kDogfightSystemId + "' not in registry");
    return true;
}

// ---------------------------------------------------------------------------
// --test-starsystems : headless registry-integrity self-test.
// ---------------------------------------------------------------------------
namespace {
int ss_pass = 0, ss_fail = 0;
void ssCheck(bool cond, const char* name) {
    if (cond) { ++ss_pass; x3::logInfo(std::string("[starsys-test] PASS ") + name); }
    else      { ++ss_fail; x3::logError(std::string("[starsys-test] FAIL ") + name); }
}
} // namespace

bool runStarSystemsSelfTest() {
    ss_pass = ss_fail = 0;

    // S0 — the table is non-trivial (Sol + reals + Kethzar).
    ssCheck(systemCount() >= 7, "S0 registry ships >= 7 systems");

    // S1 — full registry validation passes.
    std::string err;
    bool valid = validateRegistry(&err);
    if (!valid) x3::logError(std::string("[starsys-test]   reason: ") + err);
    ssCheck(valid, "S1 validateRegistry() passes (every system has a star + bodies)");

    // S2 — every REQUIRED system is present by id.
    const char* required[] = { "sol", "alpha_centauri", "tau_ceti",
                               "epsilon_eridani", "wolf_359", "trappist_1",
                               "kethzar_prime" };
    bool allFound = true;
    for (const char* id : required)
        if (!findSystem(id)) { allFound = false; x3::logError(std::string("[starsys-test]   missing: ") + id); }
    ssCheck(allFound, "S2 all required systems present (Sol, A.Cen, Tau Ceti, Eridani, Wolf 359, TRAPPIST-1, Kethzar)");

    // S3 — lookup by id round-trips name.
    const StarSystem* tc = findSystem("tau_ceti");
    ssCheck(tc && std::strcmp(tc->name, "Tau Ceti") == 0, "S3 findSystem('tau_ceti') -> 'Tau Ceti'");

    // S4 — lookup by name round-trips id.
    const StarSystem* ke = findSystemByName("Kethzar Prime");
    ssCheck(ke && std::strcmp(ke->id, "kethzar_prime") == 0, "S4 findSystemByName('Kethzar Prime') -> 'kethzar_prime'");

    // S5 — NEGATIVE CONTROL: an unknown id/name resolves to nullptr.
    ssCheck(findSystem("betelgeuse_ix") == nullptr && findSystemByName("Nowhere") == nullptr,
            "S5 negative control: unknown id/name -> nullptr");

    // S6 — Sol is home (distance 0) and every other system is farther out.
    const StarSystem* sol = findSystem("sol");
    bool solHome = sol && sol->distanceLy == 0.0f;
    bool othersFar = true;
    for (const StarSystem& s : allSystems())
        if (std::strcmp(s.id, "sol") != 0 && s.distanceLy <= 0.0f) othersFar = false;
    ssCheck(solHome && othersFar, "S6 Sol at 0 ly; every other system is farther from Sol");

    // S7 — the dogfight is set in a NON-Sol system (owner: 'far from earth').
    const StarSystem& df = dogfightSystem();
    ssCheck(std::strcmp(df.id, "sol") != 0 && df.distanceLy > 50.0f,
            "S7 dogfight system is far from Sol (non-Sol, > 50 ly)");

    // S8 — Kethzar Prime (the dogfight) has a lava/ice pairing + a ringed gas giant.
    bool hasLava = false, hasIce = false, hasGas = false;
    if (ke) for (const SystemBody& b : ke->bodies) {
        if (b.type == BodyType::Lava) hasLava = true;
        if (b.type == BodyType::Ice)  hasIce  = true;
        if (b.type == BodyType::Gas)  hasGas  = true;
    }
    ssCheck(hasLava && hasIce && hasGas, "S8 Kethzar Prime has lava + ice + gas-giant bodies");

    x3::logInfo("[starsys-test] " + std::to_string(ss_pass) + "/" +
                std::to_string(ss_pass + ss_fail) + " passed");
    return ss_fail == 0;
}

} // namespace x3::starsys
