#pragma once
// ===========================================================================
// LEVEL LINT — the geometric connectivity gate (x3-level-authoring GATE A).
//
// Born from the 2026-07-05 playtest: door slabs standing mid-corridor at the
// cell-block junctions, lintel fragments floating outside wall runs, void
// visible between rooms. Cause: the doorway resolver derives every opening
// position from room-pair geometry (the project JSON carries only [a,b]
// index pairs), and three classes of derived positions were illegal:
//   1. OVERLAP junctions (two corridors interpenetrating) were treated as
//      doored rooms — slab at the overlap CENTER = a door in open space.
//   2. Cut/mouth coordinates could land OUTSIDE the host face's span
//      (diagonal gap-bridge partners) = lintels floating in void.
//   3. Gap-separated "adjacent" rooms put the slab at the MIDPOINT between
//      the two wall planes = a slab floating in the interstice.
//
// The lint validates the RESOLVED doorway set against the same room faces
// the builder cuts, so any future data or resolver change that would ship a
// floating door fails `--test-levellint` before a human ever walks it.
//
// Checks (LAW 1 + containment + reachability):
//   DOOR-SEAT   every slab-bearing doorway's plane coordinate lies ON a host
//               room's face plane (within wall thickness).
//   JUNCTION    Overlap doorways carry NO slab (junctions are open throats).
//   CUT-SPAN    every wall cut / bridge-mouth coordinate lies inside its host
//               face's span with door-half margin (no floating lintels).
//   REACH       BFS over doorways reaches every room from room 0.
//   SEAL-SHELL  every opening in the shell (dropped wall face / doorway cut / bridge
//               mouth) gives onto SEALED interior space — probes stepped outward
//               through the opening must land in a room, corridor, throat or tube,
//               never in open void. This is the hole/enclosure gate; without it the
//               lint reported 0 violations while ten rooms leaked to the skybox
//               (owner playtest 2026-08-04).
// ===========================================================================

#include <string>
#include <vector>

namespace x3::game {

struct CanonFloor;

struct LevelLintReport {
    std::vector<std::string> violations;   // human-readable, one per finding
    int doorSeat = 0;      // slabs off any host wall plane
    int junctionSlab = 0;  // Overlap doorways that would receive a slab
    int cutSpan = 0;       // cut/mouth coordinates outside their face span
    int unreachable = 0;   // rooms BFS cannot reach from room 0
    int sealEscape = 0;    // SEAL-SHELL: openings whose outward probes escape to void
    bool pass() const { return violations.empty(); }
};

// Pure-model lint over a resolved floor (no device/physics needed).
LevelLintReport lintCanonFloor(const CanonFloor& floor);

// `--test-levellint` entry: loads the canonical JSON (same path resolution as
// the game), lints floor 1, prints every violation. True = 0 violations.
// Missing JSON = SKIP-as-pass (mirrors --test-canonlevel's fallback rule).
bool runLevelLintSelfTest();

} // namespace x3::game
