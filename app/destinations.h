#pragma once
// ============================================================================
// THE DESTINATION REGISTRY — one table, every place the game has.
//
// Owner (2026-07-11): "I want a menu for world selection, but also... the rift
// hub SHOULD TAKE US TO ALL THE WORLD PLACES".
//
// Before this, three lists disagreed about what exists:
//   * app_run.cpp's `riftDestination()` lambda (substring-matched anchors),
//   * rift_console.cpp's `kWorlds[8]` (the typed TARGET whitelist — which still
//     listed act2caves/act2/destruct/ragdoll as re-target options, two of which
//     have had NO host since the Act-2 split),
//   * app/world_hosts/world_hosts.cpp + app_run.cpp's `--world` dispatch.
// This header is now the ONE list all three read. Add a place here and it shows
// up in the world menu, in the rift consoles' target cycle, and in the hub's
// fast-travel resolver at once.
//
// CLEAN-ROOM: our own table, our own model. No third-party engine source.
//
// THE MODEL — a destination is a PLACE, not a world.
// -------------------------------------------------
// A place can be reachable two ways, and the honest UI has to say which:
//
//   * `canonAnchor` — the ONE WORLD (--world canonlevel, the game) can put you
//     there RIGHT NOW with a teleport. No load. app_run.cpp's resolver owns the
//     actual anchor maths (it is the only thing that knows the live world).
//   * `worldFlag` — the place also exists as a standalone `--world <flag>` dev
//     slice. Reaching it from a world that cannot anchor it means a WORLD LOAD:
//     the host tears the current world down and builds that one (HostContext::
//     switchWorldTo). `""` = no standalone world exists.
//
// A destination with NEITHER a live anchor nor a world flag is UNREACHABLE, and
// the menu greys it out and says why. Nothing here ever pretends.
// ============================================================================

#include <cstdint>
#include <string>
#include <string_view>

namespace x3::game {

// Where a place sits in the world — drives the menu's section headers, nothing else.
enum class DestGroup : uint8_t {
    Hub = 0,        // sub-level R1, the rift chamber
    Facility,       // the 7-floor canonical tower
    Underworld,     // THE DESCENT's strata offshoots + Club 1127
    Planet,         // the streamed exterior (crash site / city / river / ridge)
    EchoHarbor,     // ECHO HARBOR — the second product (its host ships on the
                    // echotropolis line; listed here so the directory tells the truth)
    DevWorld,       // a `--world` dev shortcut with no place in the one world
    Count
};

const char* destGroupName(DestGroup g);

struct Destination {
    const char* key;         // canonical id — what the console's TARGET field takes
    const char* name;        // readable ("Club 1127")
    const char* desc;        // one line, shown in the menu
    const char* worldFlag;   // `--world <flag>` that builds it standalone ("" = none)
    DestGroup   group;
    bool        canonAnchor; // the canon world can teleport you here (no load)
};

uint32_t           destinationCount();
const Destination& destination(uint32_t i);

// Resolve a free-form string (a typed TARGET, an old saved destination string, a
// `--world` name) to a registry entry, or nullptr. Matching is deliberately loose,
// in priority order: exact key -> exact worldFlag -> exact name -> case-insensitive
// substring of the name/key. That is what lets the OLD destination strings the hub
// shipped with ("the river valley", "facility F1", "crystal caves") keep resolving.
const Destination* findDestination(std::string_view s);

// Index of a registry entry (or UINT32_MAX). Used by the console's PREV/NEXT
// destination cycle, which walks the table.
uint32_t destinationIndex(const Destination* d);

// Cycle helper for the rift console: the entry `step` places after `from` (wrapping).
// `from` may be any string the registry can resolve; an unresolvable one starts at 0.
const Destination& cycleDestination(std::string_view from, int step);

// Headless self-test (folded into --test-rifthub): asserts the table is non-empty,
// every key/name is unique and non-empty, findDestination round-trips every
// key/name, and the legacy destination strings the hub shipped with still resolve.
//
// [P0-2] TOTAL in BOTH directions against the LIVE dispatch:
//   registry -> dispatch (D2): every non-empty worldFlag names a --world the
//     program really dispatches (default host list + world_hosts' exported
//     route table). No row may signpost a world that 404s.
//   dispatch -> registry (D7): every dispatchable --world is a registry row OR
//     an explicit, reasoned entry in kRegistryExclusions. A host added without
//     a row FAILS the gate — silent drift is the bug class this kills.
// Hygiene: exclusions must be live and reasoned (D8); unreachable rows must be
// on the documented kUnreachableAllowed list and stay unreachable (D9); and a
// negative control proves the coverage check rejects a fake dispatch-only
// world (D10). Manual RED proof: run with X3_DEST_TEST_INJECT=<junk> and D7
// must fail. The product floor (D11, spec §3.2) pins the six product worlds
// (canonlevel/intro/surface/rifthub/echotropolis/space) as listed AND
// dispatched — deleting one from both sides at once is consistent, but loud.
bool runDestinationsSelfTest();

} // namespace x3::game
