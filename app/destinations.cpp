#include "destinations.h"

#include "engine/core/x3_log.h"
#include "world_hosts.h"   // dispatchedWorldModes() — the LIVE dispatch table

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace x3::game {

namespace {

// ---------------------------------------------------------------------------
// THE `--world` FLAGS THIS PROGRAM ACTUALLY DISPATCHES.
//
// [P0-2] Two dispatch sites, BOTH exported live — nothing is copied here:
//
//   * app/world_hosts/world_hosts.cpp — dispatchWorldHost(). Its route table
//     is exported (dispatchedWorldModes()) and the self-test walks the LIVE
//     table. The old hand copy drifted 8 worlds behind in one week; a copy
//     of a list is where honesty goes to die.
//   * app/app_run.cpp — the default host's own world branches, exported as
//     defaultHostWorldModes() from the SAME FILE as the `if (worldMode ==...)`
//     lines (this file used to keep a second hand copy of those 8 names —
//     same drift class, now dead too).
//
// Note what is NOT dispatched: `act2` and `act2caves`. rift_console.cpp's old
// kWorlds whitelist offered both as re-target options and runRifthubSelfTest()
// asserted they were "real --world targets" — but neither has had a host since
// the Act-2 split. `city` and `perfshop` are in the same boat (a screenshot
// host forces --world drive / a region inside canonlevel; there is no
// `--world city`). Never a flag that 404s.
// ---------------------------------------------------------------------------

// The union of both dispatch sites: the default host's exported world modes +
// the LIVE world_hosts route table. THIS is what "dispatchable" means to the
// self-test.
std::vector<const char*> dispatchedFlagUnion() {
    std::vector<const char*> all;
    unsigned n = 0;
    const char* const* defaults = x3::apphost::defaultHostWorldModes(n);
    for (unsigned i = 0; i < n; ++i) all.push_back(defaults[i]);
    n = 0;
    const char* const* hosted = x3::apphost::dispatchedWorldModes(n);
    for (unsigned i = 0; i < n; ++i) all.push_back(hosted[i]);
    return all;
}

// ---------------------------------------------------------------------------
// DISPATCH-SIDE EXCLUSIONS — dispatchable flags deliberately NOT given their
// own registry row. Every entry needs a reason a reviewer can check; the
// self-test (D8) fails on a stale or unreasoned exclusion, and (D7) fails on
// any dispatchable flag that is neither a row nor listed here. There is no
// third bucket — that is how the drift class dies.
// ---------------------------------------------------------------------------
struct DispatchExclusion { const char* flag; const char* why; };
const DispatchExclusion kRegistryExclusions[] = {
    { "ship-interior",
      "alias --world flag: routes to the SAME host as ship-windows "
      "(hostShipWindows); the registry lists the place once, as ship-windows" },
};

// ---------------------------------------------------------------------------
// THE TABLE. Ordered the way the menu reads it: the hub, then home (the tower),
// then down (the underworld), then out (the planet), then the dev shortcuts.
// ---------------------------------------------------------------------------
const Destination kDest[] = {
// key            name                        desc                                                                     worldFlag            group                  canonAnchor
{ "hub",          "The Rift Hub",             "Sub-level R1, 78 m under the tower: the gate chamber itself.",          "rifthub",           DestGroup::Hub,        true  },

{ "f1",           "Facility F1 - Detention",  "The cell block, the main hall and the elevator lobby. The way home.",   "canonlevel",        DestGroup::Facility,   true  },
{ "f2",           "Facility F2",              "Second floor of the canonical tower.",                                  "canonlevel",        DestGroup::Facility,   true  },
{ "f3",           "Facility F3",              "Third floor of the canonical tower.",                                   "canonlevel",        DestGroup::Facility,   true  },
{ "f4",           "Facility F4",              "Fourth floor of the canonical tower.",                                  "canonlevel",        DestGroup::Facility,   true  },
{ "f5",           "Facility F5",              "Fifth floor of the canonical tower.",                                   "canonlevel",        DestGroup::Facility,   true  },
{ "f6",           "Facility F6",              "Sixth floor of the canonical tower.",                                   "canonlevel",        DestGroup::Facility,   true  },
{ "f7",           "Facility F7 - Executive",  "The top floor. The executive landing.",                                 "canonlevel",        DestGroup::Facility,   true  },
// [P0-1 EFLZ-GP-1B] The SEAM-2 "Entrance" hallway on the F1 footprint edge — the
// tower's one real way in off the apron, and the ESCAPED-branch rescuer's arrival
// spawn (specs/EFLZ_SURFACE_FACILITY_HANDOFF.spec.md §2: the handoff's dest key).
{ "entrance",     "Facility Entrance",        "The F1 entrance hall - the tower's one real way in, off the apron.",    "canonlevel",        DestGroup::Facility,   true  },

{ "granite",      "The Descent - Granite",    "Strata offshoot pocket at -55 m.",                                      "strata",            DestGroup::Underworld, true  },
{ "basalt",       "The Descent - Basalt",     "Strata offshoot pocket at -95 m.",                                      "strata",            DestGroup::Underworld, true  },
{ "obsidian",     "The Descent - Obsidian",   "Strata offshoot pocket at -125 m.",                                     "strata",            DestGroup::Underworld, true  },
{ "crystal",      "The Crystal Veins",        "The glowing crystal offshoot at -155 m.",                               "strata",            DestGroup::Underworld, true  },
{ "magma",        "The Magma Zone",           "The molten offshoot at -180 m.",                                        "strata",            DestGroup::Underworld, true  },
{ "club",         "Club 1127",                "The disco at The Deep, Y=-200. Code 1127 in the cab is the long way.",  "club",              DestGroup::Underworld, true  },

{ "crash",        "The Crash Site",           "Where the ship came down, off the +Z breach face.",                     "",                  DestGroup::Planet,     true  },
{ "city",         "The City",                 "The streamed metropolis region, out past the apron.",                   "",                  DestGroup::Planet,     true  },
{ "river",        "The River Valley",         "The carved river - real, swimmable water. You land on the bank.",       "valley",            DestGroup::Planet,     true  },
{ "ridge",        "The Cliff Ridge",          "The highest ground on the ring out from the tower.",                    "cliffs",            DestGroup::Planet,     true  },

// ECHO HARBOR — the second product. Its host (--world echotropolis) FOLDED
// into this build at the converge (playtest-consolidated brought host_echotropolis
// in), so the flag is now LIVE and the row is reachable. The row exists because
// the directory claims to list every place the game has.
// (DESTINATIONS_REGISTRY.spec §3.3/§6.)
{ "echotropolis", "Echo Harbor",              "The island city - a second product, now folded into this build (F1 P1: island + open sea).", "echotropolis",  DestGroup::EchoHarbor, true },

{ "canonlevel",   "Canon World (spawn)",      "THE GAME: tower + elevator + exterior + streamed planet. From spawn.",  "canonlevel",        DestGroup::DevWorld,   false },
{ "intro",        "The Cold Open",            "The canon world, entered through the prologue cutscene.",               "intro",             DestGroup::DevWorld,   false },
{ "level1",       "Legacy Spire (level1)",    "The pre-canon hand-coded spire. Reference build.",                      "level1",            DestGroup::DevWorld,   false },
{ "elevator",     "Elevator (walkable)",      "The elevator showcase you can walk around in.",                         "elevator",          DestGroup::DevWorld,   false },
{ "elevator-showcase","Elevator (beauty)",    "The elevator beauty/screenshot slice.",                                 "elevator-showcase", DestGroup::DevWorld,   false },
{ "terrain",      "Terrain Slice",            "The playable outdoor terrain biome.",                                   "terrain",           DestGroup::DevWorld,   false },
{ "ocean",        "Ocean Slice",              "The terrain world with the animated sea at sea level.",                 "ocean",             DestGroup::DevWorld,   false },
{ "streamed",     "Streamed Tour World",      "The WorldStreamer's own tour graph (regions.json, with spire_f1).",     "streamed",          DestGroup::DevWorld,   false },
{ "surface",      "Act-1 Surface Landing",    "The cold-open landing slice (the exterior module's other caller).",     "surface",           DestGroup::DevWorld,   false },
{ "space",        "Space Combat",             "The intro space-combat slice: fly the ship.",                           "space",             DestGroup::DevWorld,   false },
{ "introcockpit", "Intro Cockpit",            "The cold-open cockpit interior: the GLB rig with live content screens.", "introcockpit",     DestGroup::DevWorld,   false },
{ "ship-windows", "Ship Interior",            "The walkable ship interior with true-portal space out the windows.",    "ship-windows",      DestGroup::DevWorld,   false },
{ "wormhole",     "Wormhole Tunnel",          "The Salvari crystal-matrix wormhole tunnel VFX slice.",                 "wormhole",          DestGroup::DevWorld,   false },
{ "wormhole-transit","Wormhole Transit",      "The S3 autopilot jump: ride the wormhole end to end.",                  "wormhole-transit",  DestGroup::DevWorld,   false },
{ "tractor",      "Tractor Beam",             "The intro capture: the capital ship's tractor beam takes you.",         "tractor",           DestGroup::DevWorld,   false },
{ "descentslide", "Descent Slide",            "The B1 to -178 m coaster-grade slide ride.",                            "descentslide",      DestGroup::DevWorld,   false },
{ "bodycontact",  "Body Contact Bench",       "Feature bench: rigid rest on surfaces + soft mattress indent.",         "bodycontact",       DestGroup::DevWorld,   false },
{ "showroom",     "Asset Showroom",           "The asset showroom family (models, lighting, companion staging).",      "showroom",          DestGroup::DevWorld,   false },
{ "drive",        "Drive (vehicles)",         "The vehicle framework: cars, the perf shop, LATE NIGHT SPEED.",         "drive",             DestGroup::DevWorld,   false },
{ "boat",         "Boat",                     "The vehicle framework on water.",                                       "boat",              DestGroup::DevWorld,   false },
{ "fly",          "Flyer",                    "The vehicle framework in the air.",                                     "fly",               DestGroup::DevWorld,   false },
{ "destruct",     "Destruction Bench",        "Physics test bench: destructible crates.",                              "destruct",          DestGroup::DevWorld,   false },
{ "physjoint",    "Joint Bench",              "Physics test bench: constraints and joints.",                           "physjoint",         DestGroup::DevWorld,   false },
{ "ragdoll",      "Ragdoll Bench",            "Physics test bench: ragdolls.",                                         "ragdoll",           DestGroup::DevWorld,   false },
{ "fromdoc",      "LevelDoc (live edit)",     "Boot straight into a LevelDoc JSON - the editor loop.",                 "fromdoc",           DestGroup::DevWorld,   false },
{ "spacestation", "The Deep-Space Station",   "Solar+fusion station far from Earth: hangar, corridor, stargate ring.", "spacestation",      DestGroup::DevWorld,   false },
{ "gallery",      "Character Gallery",        "The cast on pedestals - walk up, press E to cycle every clip.",         "gallery",           DestGroup::DevWorld,   false },
{ "labzero3d",    "Lab Zero Rail (P0)",       "The LAB ZERO side-scroller rail slice on the streamed mountains.",      "labzero3d",         DestGroup::DevWorld,   false },
};
constexpr uint32_t kDestCount = (uint32_t)(sizeof(kDest) / sizeof(kDest[0]));

char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

bool eqCI(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

bool containsCI(std::string_view hay, std::string_view needle) {
    if (needle.empty() || needle.size() > hay.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool hit = true;
        for (size_t j = 0; j < needle.size(); ++j)
            if (lower(hay[i + j]) != lower(needle[j])) { hit = false; break; }
        if (hit) return true;
    }
    return false;
}

} // namespace

const char* destGroupName(DestGroup g) {
    switch (g) {
    case DestGroup::Hub:        return "THE RIFT HUB";
    case DestGroup::Facility:   return "THE FACILITY";
    case DestGroup::Underworld: return "THE DESCENT";
    case DestGroup::Planet:     return "THE PLANET";
    case DestGroup::EchoHarbor: return "ECHO HARBOR";
    case DestGroup::DevWorld:   return "DEV WORLDS";
    default:                    return "?";
    }
}

uint32_t           destinationCount()      { return kDestCount; }
const Destination& destination(uint32_t i) { return kDest[i < kDestCount ? i : 0]; }

uint32_t destinationIndex(const Destination* d) {
    if (!d) return UINT32_MAX;
    for (uint32_t i = 0; i < kDestCount; ++i)
        if (&kDest[i] == d) return i;
    return UINT32_MAX;
}

const Destination* findDestination(std::string_view s) {
    if (s.empty()) return nullptr;
    // 1. exact key.
    for (uint32_t i = 0; i < kDestCount; ++i)
        if (eqCI(s, kDest[i].key)) return &kDest[i];
    // 2. exact --world flag (so `--world club` and the "club" key agree).
    for (uint32_t i = 0; i < kDestCount; ++i)
        if (kDest[i].worldFlag[0] && eqCI(s, kDest[i].worldFlag)) return &kDest[i];
    // 3. exact readable name.
    for (uint32_t i = 0; i < kDestCount; ++i)
        if (eqCI(s, kDest[i].name)) return &kDest[i];
    // 4. LOOSE: the query contains the key, or the name contains the query.
    //    This is what keeps the hub's shipped destination strings alive —
    //    "crystal caves" -> crystal, "facility F1" -> f1, "the river valley" ->
    //    river, "the cliffs" -> ridge (see the aliases below).
    struct Alias { const char* text; const char* key; };
    static const Alias kAliases[] = {
        { "crystal", "crystal" }, { "caves", "crystal" },
        { "1127", "club" },       { "deep", "club" },
        { "crash", "crash" },
        { "city", "city" },
        { "river", "river" },     { "valley", "river" },
        { "cliff", "ridge" },     { "ridge", "ridge" },
        { "lobby", "f1" },        { "detention", "f1" },
        { "executive", "f7" },
        { "rift", "hub" },        { "hub", "hub" },
        { "magma", "magma" },     { "obsidian", "obsidian" },
        { "basalt", "basalt" },   { "granite", "granite" },
        // `--world ship-interior` is a live dispatch alias for the ship-windows
        // host; typed TARGETs and old strings with "interior" land on the row.
        { "ship-interior", "ship-windows" }, { "interior", "ship-windows" },
        { "echo", "echotropolis" }, { "harbor", "echotropolis" },
    };
    // Facility floors first: "F3" inside a longer string must not be beaten by a
    // generic alias.
    for (uint32_t i = 0; i < kDestCount; ++i) {
        if (kDest[i].group != DestGroup::Facility) continue;
        if (containsCI(s, kDest[i].key)) return &kDest[i];
    }
    for (const Alias& a : kAliases)
        if (containsCI(s, a.text)) {
            for (uint32_t i = 0; i < kDestCount; ++i)
                if (std::strcmp(kDest[i].key, a.key) == 0) return &kDest[i];
        }
    // 5. last resort: the query is a substring of a name.
    for (uint32_t i = 0; i < kDestCount; ++i)
        if (containsCI(kDest[i].name, s)) return &kDest[i];
    return nullptr;
}

const Destination& cycleDestination(std::string_view from, int step) {
    uint32_t idx = destinationIndex(findDestination(from));
    if (idx == UINT32_MAX) idx = 0;
    int64_t n = (int64_t)idx + step;
    while (n < 0) n += kDestCount;
    return kDest[(uint32_t)(n % kDestCount)];
}

// ===========================================================================
// Self-test (folded into --test-rifthub).
// ===========================================================================
namespace {
int dt_pass = 0, dt_fail = 0;
void dtCheck(bool cond, const char* name) {
    if (cond) { ++dt_pass; x3::logInfo(std::string("[dest-test] PASS ") + name); }
    else      { ++dt_fail; x3::logError(std::string("[dest-test] FAIL ") + name); }
}

bool isDispatched(const std::vector<const char*>& dispatched, const char* flag) {
    for (const char* w : dispatched) if (std::strcmp(w, flag) == 0) return true;
    return false;
}

// Registry rows that are ALLOWED to be unreachable (no anchor, no world flag).
// Each needs a reviewer-checkable reason; D3 exempts exactly these, and D9
// fails if one goes stale (missing row, or the row became reachable).
struct KnownUnreachable { const char* key; const char* why; };
// Empty since the converge: echotropolis's host folded into this build, so its
// row is now reachable (worldFlag "echotropolis") and no longer belongs here.
// A std::vector so the list can legally be empty (a zero-size C array is
// ill-formed on MSVC); D9 iterates it and passes vacuously.
const std::vector<KnownUnreachable> kUnreachableAllowed = {};

bool isUnreachableAllowed(const char* key) {
    for (const KnownUnreachable& u : kUnreachableAllowed)
        if (std::strcmp(u.key, key) == 0) return true;
    return false;
}

// THE TOTALITY CHECK, dispatch -> registry direction: a dispatchable flag is
// covered iff some registry row claims it as worldFlag OR it is an explicit,
// reasoned exclusion. Factored out so D10 can prove the check REJECTS a fake
// dispatch-only world (the negative control), not just that it accepts today's.
bool dispatchedFlagCovered(const char* flag) {
    for (uint32_t i = 0; i < kDestCount; ++i)
        if (kDest[i].worldFlag[0] && std::strcmp(kDest[i].worldFlag, flag) == 0)
            return true;
    for (const DispatchExclusion& e : kRegistryExclusions)
        if (std::strcmp(e.flag, flag) == 0 && e.why[0]) return true;
    return false;
}
} // namespace

bool runDestinationsSelfTest() {
    dt_pass = dt_fail = 0;

    // The dispatchable-world set, straight off BOTH live dispatch exports
    // (world_hosts' route table + app_run's default-host world modes).
    // X3_DEST_TEST_INJECT=<flag> appends a pretend
    // dispatch-only world — a manual negative control: set it to any junk name
    // and D7 must go RED, proving this gate actually bites on drift.
    std::vector<const char*> dispatched = dispatchedFlagUnion();
    if (const char* inj = std::getenv("X3_DEST_TEST_INJECT")) {
        dispatched.push_back(inj);
        x3::logWarn(std::string("[dest-test] NEGATIVE CONTROL: injecting fake "
                                "dispatch-only world '") + inj + "'");
    }

    dtCheck(kDestCount >= 44, "D0 the registry lists every place (>= 44 entries)");

    // D1 — keys and names are unique and non-empty (a duplicate key would make the
    //      console's TARGET ambiguous and the menu lie).
    {
        bool ok = true;
        for (uint32_t i = 0; i < kDestCount; ++i) {
            if (!kDest[i].key[0] || !kDest[i].name[0] || !kDest[i].desc[0]) ok = false;
            // ...and its group renders a REAL section header, not the "?"
            // fallback (caught live: ECHO HARBOR's header shipped as "?").
            if (std::strcmp(destGroupName(kDest[i].group), "?") == 0) ok = false;
            for (uint32_t j = i + 1; j < kDestCount; ++j)
                if (eqCI(kDest[i].key, kDest[j].key) || eqCI(kDest[i].name, kDest[j].name))
                    ok = false;
        }
        dtCheck(ok, "D1 every entry has a unique non-empty key + name + description");
    }

    // D2 — THE HONESTY GATE: every worldFlag names a --world the program really
    //      dispatches. This is the test the OLD kWorlds list would have failed
    //      (act2 / act2caves have no host).
    {
        bool ok = true;
        for (uint32_t i = 0; i < kDestCount; ++i) {
            const char* f = kDest[i].worldFlag;
            if (f[0] && !isDispatched(dispatched, f)) {
                ok = false;
                x3::logError(std::string("[dest-test]   '") + kDest[i].key +
                             "' claims --world " + f + " — NO SUCH HOST");
            }
        }
        dtCheck(ok, "D2 every worldFlag names a --world the program dispatches");
    }

    // D3 — a destination must be reachable SOME way (a canon anchor or a world)
    //      UNLESS it is on the explicit, reasoned kUnreachableAllowed list
    //      (Echo Harbor: the row is the truth, the grey tag is the honesty).
    {
        bool ok = true;
        for (uint32_t i = 0; i < kDestCount; ++i)
            if (!kDest[i].canonAnchor && !kDest[i].worldFlag[0] &&
                !isUnreachableAllowed(kDest[i].key)) {
                ok = false;
                x3::logError(std::string("[dest-test]   '") + kDest[i].key +
                             "' is unreachable and NOT on kUnreachableAllowed");
            }
        dtCheck(ok, "D3 no orphan entries (anchor, world, or documented exception)");
    }

    // D4 — findDestination round-trips every key and every name.
    {
        bool ok = true;
        for (uint32_t i = 0; i < kDestCount; ++i) {
            if (findDestination(kDest[i].key)  != &kDest[i]) ok = false;
            if (findDestination(kDest[i].name) != &kDest[i]) ok = false;
        }
        dtCheck(ok, "D4 findDestination round-trips every key and name");
    }

    // D5 — the LEGACY destination strings the hub shipped with still resolve to the
    //      right place (the canon gates were seeded with these; saves carry them).
    {
        struct L { const char* s; const char* key; };
        const L legacy[] = {
            { "club 1127",        "club"    }, { "crystal caves",  "crystal" },
            { "the crash site",   "crash"   }, { "the city",       "city"    },
            { "the river valley", "river"   }, { "the cliffs",     "ridge"   },
            { "facility F1",      "f1"      }, { "facility F7",    "f7"      },
        };
        bool ok = true;
        for (const L& l : legacy) {
            const Destination* d = findDestination(l.s);
            if (!d || std::strcmp(d->key, l.key) != 0) {
                ok = false;
                x3::logError(std::string("[dest-test]   legacy '") + l.s + "' -> " +
                             (d ? d->key : "(null)") + ", wanted " + l.key);
            }
        }
        dtCheck(ok, "D5 the 8 legacy gate destination strings still resolve");
    }

    // D6 — cycling walks the WHOLE table and wraps (this is how one gate reaches
    //      every place: the console's PREV/NEXT step through the registry).
    {
        bool ok = true;
        std::vector<bool> seen(kDestCount, false);
        std::string cur = kDest[0].key;
        for (uint32_t i = 0; i < kDestCount; ++i) {
            const Destination& d = cycleDestination(cur, 1);
            const uint32_t idx = destinationIndex(&d);
            if (idx == UINT32_MAX || seen[idx]) ok = false;
            else seen[idx] = true;
            cur = d.key;
        }
        for (bool b : seen) if (!b) ok = false;                 // every place visited
        if (std::strcmp(cur.c_str(), kDest[0].key) != 0) ok = false;  // and it wrapped home
        // and it goes both ways
        if (std::strcmp(cycleDestination(kDest[0].key, -1).key,
                        kDest[kDestCount - 1].key) != 0) ok = false;
        dtCheck(ok, "D6 PREV/NEXT cycling reaches EVERY destination and wraps");
    }

    // D7 — THE OTHER DIRECTION, TOTAL: every world the program can dispatch is
    //      either a registry row or an explicit, reasoned exclusion. This is
    //      the check whose absence let 8 hosts land in one week with no menu
    //      row, no console target and no hub reach. Drift now FAILS the gate.
    {
        bool ok = true;
        for (const char* f : dispatched)
            if (!dispatchedFlagCovered(f)) {
                ok = false;
                x3::logError(std::string("[dest-test]   --world ") + f +
                             " is DISPATCHABLE but has no registry row and no "
                             "documented exclusion — the menu/hub cannot see it");
            }
        dtCheck(ok, "D7 every dispatchable world is a registry row or a reasoned exclusion");
    }

    // D8 — exclusion hygiene: each exclusion has a reason, names a flag that is
    //      REALLY dispatched (a stale exclusion is drift wearing a badge), and
    //      is not ALSO a registry row (an excluded row is a contradiction).
    {
        bool ok = true;
        for (const DispatchExclusion& e : kRegistryExclusions) {
            if (!e.why[0])                        ok = false;
            if (!isDispatched(dispatched, e.flag)) ok = false;
            for (uint32_t i = 0; i < kDestCount; ++i)
                if (kDest[i].worldFlag[0] &&
                    std::strcmp(kDest[i].worldFlag, e.flag) == 0) ok = false;
        }
        dtCheck(ok, "D8 exclusions are reasoned, live, and not double-listed");
    }

    // D9 — kUnreachableAllowed hygiene: every listed key exists, has a reason,
    //      and IS still unreachable (if it gained an anchor or a world, the
    //      exception is stale and must be deleted).
    {
        bool ok = true;
        for (const KnownUnreachable& u : kUnreachableAllowed) {
            if (!u.why[0]) ok = false;
            const Destination* d = findDestination(u.key);
            if (!d || std::strcmp(d->key, u.key) != 0) { ok = false; continue; }
            if (d->canonAnchor || d->worldFlag[0]) ok = false;
        }
        dtCheck(ok, "D9 unreachable exceptions are real rows and still unreachable");
    }

    // D10 — NEGATIVE CONTROL, in-suite: the coverage check must REJECT a world
    //       that is dispatch-only and unregistered. If this ever passes for the
    //       fake, D7 is a rubber stamp and the whole gate is theatre.
    dtCheck(!dispatchedFlagCovered("zz-fake-drifted-world"),
            "D10 negative control: a fake unregistered dispatch flag is CAUGHT");

    // D11 — THE PRODUCT FLOOR (spec §3.2): the known product worlds must each be
    //       dispatched AND carry a registry row naming that flag. D2/D7 keep the
    //       two lists equal, but only THIS check notices if a product world is
    //       dropped from BOTH sides at once (host deleted + row deleted is a
    //       consistent lie — for these six it must be a loud one).
    {
        const char* const kProductFloor[] = {
            "canonlevel",   // EFLZ main
            "intro",        // EFLZ cold-open entry
            "surface",      // Escaped landing
            "rifthub",      // the hub
            "echotropolis", // Echo Harbor
            "space",        // space-combat slice
        };
        bool ok = true;
        for (const char* f : kProductFloor) {
            bool hasRow = false;
            for (uint32_t i = 0; i < kDestCount; ++i)
                if (std::strcmp(kDest[i].worldFlag, f) == 0) { hasRow = true; break; }
            if (!hasRow || !isDispatched(dispatched, f)) {
                ok = false;
                x3::logError(std::string("[dest-test]   product world '") + f +
                             (hasRow ? "' is not dispatched" : "' has no registry row"));
            }
        }
        dtCheck(ok, "D11 product floor: the six product worlds are listed AND dispatched");
    }

    x3::logInfo("destinations: " + std::to_string(dt_pass) + "/" +
                std::to_string(dt_pass + dt_fail) + " passed");
    return dt_fail == 0;
}

} // namespace x3::game
