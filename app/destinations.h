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
    Space,          // OFF-WORLD — the space product's roster (X3Space.exe). See
                    // the SPACE ROSTER block at the bottom of this header.
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

    // ---- WORMHOLE STABILITY (INTRODUCED BY feat/ship-comms, 2026-08-24) -----
    // Tim's comms spec: "the ship AI will notify you of unstable or stable
    // wormholes nearby". No stability concept existed anywhere in the data, so
    // this field is NEW. Stating that plainly rather than implying it was always
    // here: the ship comms device is currently its ONLY reader, and nothing about
    // routing, traversal or the menus consults it, so it is additive and inert
    // outside that device.
    //
    // DEFAULTS TO TRUE so all ~50 pre-existing rows keep a sane value without
    // being touched; the handful that are authored UNSTABLE say so explicitly in
    // the table, and the reason is written next to them.
    bool        stable = true;
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

// ===========================================================================
// THE SPACE ROSTER — every place X3Space.exe carries.
//
// Owner, 2026-08-24: "Can you have the space bit as a standalone, unconnected
// to the game at large?" — answered as a THIRD THIN LAUNCHER over the same
// x3app.dll (app/entry_space.cpp, alongside entry_game.cpp / entry_editor.cpp),
// NOT as a fork. One codebase, one engine, three front doors.
//
// The roster is DERIVED FROM THIS TABLE — every row whose group is
// DestGroup::Space — and never hand-listed anywhere. A hand list is exactly the
// drift that D2/D7 exist to kill one layer up: the moment a space world lands
// with a row and no roster entry (or the reverse), the product's contents are a
// lie no one notices. Add `DestGroup::Space` to a row and X3Space carries it.
//
// The gates that hold it honest live in runDestinationsSelfTest():
//   D14 — every roster row names a --world the program really dispatches, and
//         NONE of them is the facility (canonlevel), the campaign prologue
//         (intro) or the other product (echoharbor). The space product must not
//         require the game at large — that is the whole ask.
//   D15 — spaceDefaultWorld() is a real registry key, IS in the roster, and is
//         dispatchable: X3Space lands in a SPACE world, never canonlevel.
//   D16 — the CLI routes `--space` to that default world and `--dogfight` /
//         `--world dogfight` straight into the encounter with no facility build.
// ===========================================================================
uint32_t spaceRosterCount();
const Destination& spaceRosterEntry(uint32_t i);

// True iff `d` is part of the space product's roster.
bool isSpaceDestination(const Destination& d);

// The registry key X3Space.exe lands in when no --world is given (D15 pins it
// to a roster row). app/cli.cpp reads THIS — the default is not spelled twice.
const char* spaceDefaultWorld();

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
