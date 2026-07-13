#pragma once
// ---------------------------------------------------------------------------
// OPEN THE REAL GAME LEVEL IN THE EDITOR.
//
// The editor speaks LevelDoc (name/biome/playerStart + entities[] + brushes[]). The
// GAME's canon level is a different file entirely — EscapeLab48_AllFloors_v2.project.json,
// the LevelArchitect 10.7 "project" schema: floors -> rooms/doors/entities/triggers,
// with rooms shaped { n(ame), t(ype), x/y/z center, w/h/d full-size }. Until now the
// editor could build a level NEXT TO the game but could not open the game's OWN level.
// That made it a sandbox, not a tool.
//
// The bridge is small because a canon ROOM is already a BOX: (x,y,z) center + (w,h,d)
// full extents is exactly a BlockoutBrush. So importing the facility = every room in a
// chosen floor becomes a Box brush the editor can already render, pick, move and save.
//
// SCOPE, STATED HONESTLY:
//   * OPEN (this file) — canon project.json -> LevelDoc, ONE floor at a time (124 rooms
//     across 7 floors is too much to edit as one soup, and the game itself loads/streams
//     per floor). Read + display + navigate + edit-in-the-editor's-own-format.
//   * SAVE BACK TO THE CANON SCHEMA is deliberately NOT here. That file drives the
//     shipping game through level_loader.cpp; a lossy or wrong write corrupts the level
//     for everyone. Round-tripping into the 10.7 project format is its own task with its
//     own tests. The editor can already Save its LevelDoc; exporting a LevelDoc back to
//     canon is phase 2, and pretending otherwise would be how you lose a level.
//
// Pure logic — no ImGui, no Vulkan. Headless-testable (folded into --test-editor).
// ---------------------------------------------------------------------------
#include "editor.h"

#include <string>
#include <vector>

namespace x3::editor {

// A floor's identity, for the editor's "which floor?" chooser.
struct CanonFloorInfo {
    std::string key;     // "1".."7" — the key in the project's floors{} object
    std::string name;    // "Detention Level"
    int         rooms = 0;
    int         doors = 0;
};

struct CanonProject {
    bool                        ok = false;
    std::string                 name;      // the project's display name
    std::string                 error;
    std::vector<CanonFloorInfo> floors;    // in numeric key order
    std::string                 rawJson;   // kept so importFloor() need not re-read disk
};

// The canonical level path, resolved the SAME way the game resolves it (assetRoot()).
// NEVER a hardcoded absolute path — that was KNOWN_BUGS L2, a baked-in C:\ path that
// silently overrode the repo's level. One resolver, one source of truth.
std::string canonLevelPath();

// Parse just enough of the project to list its floors (the chooser). Cheap; does not
// build geometry. `ok == false` with a reason if the file is missing/garbage — the
// editor stays usable, it just cannot offer "Open Game Level".
CanonProject openCanonProject(const std::string& path);
CanonProject openCanonProjectFromString(const std::string& json);

// Import ONE floor of an already-parsed project into a LevelDoc: every room becomes a
// Box brush (name = the room's name, tinted by room TYPE so the layout reads at a
// glance), the doc's name/biome are set, and playerStart is seeded from the floor's
// "Jake Cell" if present. Returns false (and leaves `out` untouched) if the floor key
// is unknown. Doors are connectivity edges, not geometry, so they are NOT imported as
// brushes (a later pass can draw them as gizmo lines).
bool importCanonFloor(const CanonProject& proj, const std::string& floorKey, LevelDoc& out);

// A stable tint for a room type, so an imported floor is not a field of identical grey
// boxes — cells read one colour, labs another, halls another. Purely visual.
void roomTypeTint(const std::string& type, float outRgb[3]);

// Folded into --test-editor.
bool runCanonImportSelfTest();

} // namespace x3::editor
