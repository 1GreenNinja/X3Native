#include "destinations.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

namespace x3::game {

namespace {

// ---------------------------------------------------------------------------
// THE `--world` FLAGS THIS PROGRAM ACTUALLY DISPATCHES.
//
// Verified against, and must stay in step with, BOTH dispatch sites:
//   * app/world_hosts/world_hosts.cpp  — dispatchWorldHost()
//   * app/app_run.cpp                  — the default host's own world branches
//
// Note what is NOT here: `act2` and `act2caves`. rift_console.cpp's old kWorlds
// whitelist offered both as re-target options and runRifthubSelfTest() asserted
// they were "real --world targets" — but neither has had a host since the Act-2
// split. Two of the hub's eight gates were signposting worlds that do not exist.
// `city` and `perfshop` are in the same boat (a screenshot host forces --world
// drive / a region inside canonlevel; there is no `--world city`).
// ---------------------------------------------------------------------------
const char* const kDispatchedWorlds[] = {
    // default host (app_run.cpp)
    "canonlevel", "intro", "level1", "elevator", "terrain", "ocean", "fromdoc",
    // discrete world hosts (world_hosts.cpp)
    "destruct", "physjoint", "ragdoll", "drive", "boat", "fly", "club",
    "showroom", "valley", "cliffs", "streamed", "space", "surface", "strata",
    "elevator-showcase", "rifthub",
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
{ "showroom",     "Asset Showroom",           "The asset showroom family (models, lighting, companion staging).",      "showroom",          DestGroup::DevWorld,   false },
{ "drive",        "Drive (vehicles)",         "The vehicle framework: cars, the perf shop, LATE NIGHT SPEED.",         "drive",             DestGroup::DevWorld,   false },
{ "boat",         "Boat",                     "The vehicle framework on water.",                                       "boat",              DestGroup::DevWorld,   false },
{ "fly",          "Flyer",                    "The vehicle framework in the air.",                                     "fly",               DestGroup::DevWorld,   false },
{ "destruct",     "Destruction Bench",        "Physics test bench: destructible crates.",                              "destruct",          DestGroup::DevWorld,   false },
{ "physjoint",    "Joint Bench",              "Physics test bench: constraints and joints.",                           "physjoint",         DestGroup::DevWorld,   false },
{ "ragdoll",      "Ragdoll Bench",            "Physics test bench: ragdolls.",                                         "ragdoll",           DestGroup::DevWorld,   false },
{ "fromdoc",      "LevelDoc (live edit)",     "Boot straight into a LevelDoc JSON - the editor loop.",                 "fromdoc",           DestGroup::DevWorld,   false },
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
bool isDispatched(const char* flag) {
    for (const char* w : kDispatchedWorlds) if (std::strcmp(w, flag) == 0) return true;
    return false;
}
} // namespace

bool runDestinationsSelfTest() {
    dt_pass = dt_fail = 0;

    dtCheck(kDestCount >= 30, "D0 the registry lists every place (>= 30 entries)");

    // D1 — keys and names are unique and non-empty (a duplicate key would make the
    //      console's TARGET ambiguous and the menu lie).
    {
        bool ok = true;
        for (uint32_t i = 0; i < kDestCount; ++i) {
            if (!kDest[i].key[0] || !kDest[i].name[0] || !kDest[i].desc[0]) ok = false;
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
            if (f[0] && !isDispatched(f)) {
                ok = false;
                x3::logError(std::string("[dest-test]   '") + kDest[i].key +
                             "' claims --world " + f + " — NO SUCH HOST");
            }
        }
        dtCheck(ok, "D2 every worldFlag names a --world the program dispatches");
    }

    // D3 — a destination must be reachable SOME way: a canon anchor or a world.
    {
        bool ok = true;
        for (uint32_t i = 0; i < kDestCount; ++i)
            if (!kDest[i].canonAnchor && !kDest[i].worldFlag[0]) ok = false;
        dtCheck(ok, "D3 no orphan entries (each has a canon anchor or a --world)");
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

    x3::logInfo("destinations: " + std::to_string(dt_pass) + "/" +
                std::to_string(dt_pass + dt_fail) + " passed");
    return dt_fail == 0;
}

} // namespace x3::game
