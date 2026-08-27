#pragma once
// THE CANON LEVEL, IN THE EDITOR — EscapeLab48_AllFloors_v2.project.json.
//
// Before this file existed, the Level Editor could author levels in its OWN LevelDoc
// schema and nothing else. The game's level — the facility, the 124 rooms, the 160
// doors, THE GAME — is a different schema entirely (app/level_loader.h: CanonRoom /
// CanonDoorway / CanonFloor), and it had NO write path at all. So the editor was a
// sandbox parked next to the game, not a tool for it.
//
// This is the OPEN / EDIT / SAVE path for the real thing.
//
// ---------------------------------------------------------------------------
// THE SCHEMA (measured against the shipping file, not guessed)
//   {
//     "version","type","name","engine","created","currentFloor",   <- project header
//     "floors": { "1".."7": {
//        "name": "Detention Level",
//        "rooms": [ { n,t,x,y,z,w,h,d,f[,desc] }, ... ],   <- 124 rooms across 7 floors
//        "doors": [ [a,b], ... ],                          <- 160 room-INDEX pairs
//        "entities": [], "triggers": []
//     } }
//   }
// The game reads: n, t, x, y, z, w, h, d and the door pairs. It does NOT read f, desc,
// entities or triggers — but 72 rooms carry a `desc` holding rescue timers, boss HP and
// the room's story, and an editor that eats those on save has destroyed the level just
// as surely as if it had eaten the geometry. So the rule here is stronger than "keep
// what the game reads": KEEP EVERYTHING.
//
// ---------------------------------------------------------------------------
// HOW LOSSLESSNESS IS ACHIEVED (and why the round-trip is BYTE-exact, not just
// semantically equal):
//   * rooms[]  -> LevelDoc::brushes (identity map: center+extents ARE a BlockoutBrush)
//                 with the canon-only fields carried inside the brush (CanonRoomFields)
//                 so the SNAPSHOT-based undo stack preserves them with no changes to it.
//   * doors[]  -> LevelDoc::canon.floors[].doors, as pairs of STABLE ROOM IDS. Not array
//                 indices — so deleting or undoing a room can never scramble connectivity.
//                 Indices are re-derived at save time from the ids.
//   * everything else (project header, floor names, entities, triggers, and any key a
//                 future LevelArchitect adds that we have never heard of) is kept as its
//                 VERBATIM JSON SOURCE TEXT and written back unchanged. We cannot lose a
//                 field we never parsed.
// The writer reproduces the source's formatting (JSON.stringify(v, null, 2)), so an
// unedited load->save is byte-for-byte the input file. The self-test asserts the weaker,
// honest invariant (semantic equality of the PARSED structures) and separately reports
// the byte-exactness.
//
// SAFETY: the canon level IS the game. canonSave() writes a .bak of the target on the
// first write of a session before touching it, and is only ever reached from an explicit
// user action (the "SAVE CANON LEVEL" button / the self-test's temp file). The path is
// resolved through x3::game::canonProjectJsonPath() -> assetRoot(); there is NO absolute-
// path fallback here and there must never be one (docs/KNOWN_BUGS.md L2).
//
// Game/slice code only; engine/ stays pure.

#include "editor.h"

#include <string>

namespace x3::editor {

// The canon level's path, resolved exactly the way the GAME resolves it (assetRoot()).
std::string canonLevelPath();

// Parse the canon project at `path` into `doc`: rooms become brushes (canon.room=true),
// everything else lands in doc.canon. CLEARS doc first. Returns false (and fills `err`)
// on IO / parse failure, leaving doc empty rather than half-loaded.
bool canonLoad(const std::string& path, LevelDoc& doc, std::string* err = nullptr);

// Serialize `doc` back into the canon schema. Rooms are grouped by their floorKey and
// ordered by their `order`; doors are re-indexed from stable ids (an edge whose room was
// deleted is dropped). Non-canon brushes and doc.entities are IGNORED (they have no home
// in the canon schema — the editor refuses to pretend otherwise; see canonSaveWarnings).
std::string canonToJson(const LevelDoc& doc);

// Anything in `doc` that canonToJson() CANNOT represent, one line each. Empty == the save
// is lossless. The UI shows this before it writes; the test asserts it is empty on a
// clean round-trip.
std::vector<std::string> canonSaveWarnings(const LevelDoc& doc);

// Write the canon schema to `path`. Backs the existing file up to `path + ".bak"` first
// (once — an existing .bak is not overwritten, so the ORIGINAL is never lost behind a
// chain of saves). Returns false + `err` on failure. EXPLICIT USER ACTION ONLY.
bool canonSave(const std::string& path, const LevelDoc& doc, std::string* err = nullptr);

// Headless self-test (--test-canonedit):
//   CE1 the real canon level OPENS (124 rooms / 160 doors / 7 floors / header intact)
//   CE2 ROUND-TRIP: load -> save with zero edits is SEMANTICALLY IDENTICAL to the input
//   CE3 NEGATIVE CONTROL: perturb one room's extent and the CE2 comparison goes RED
//   CE4 EDIT + RELOAD through the COMMAND STACK survives a save/load
//   CE5 UNDO restores the prior state exactly (geometry AND canon payload)
//   CE6 the GAME'S OWN LOADER (loadCanonFloor / loadCanonTower) reads the saved file and
//       gets the same rooms and doors back
// No window / Vulkan. Returns true iff all pass.
bool runCanonEditSelfTest();

} // namespace x3::editor
