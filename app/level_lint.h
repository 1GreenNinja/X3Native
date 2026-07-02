#pragma once
// LEVEL LINT — Gate A of the x3-level-authoring doctrine (.claude/skills/
// x3-level-authoring/SKILL.md). Validates the BUILT canonical world (the
// CanonFloor rooms + resolved doorways the loader hands to buildCanonFloor,
// i.e. exactly the data the geometry is generated from, using the builder's
// own constants) against the five laws:
//
//   1. DOOR-SEAT     every door/opening sits IN a wall plane of both rooms,
//                    inside the shared wall span, with standing headroom.
//   2. SEAM          adjacent boundaries flush/shared — report GAPS (void
//                    visible through an opening across a seam band) and
//                    DOUBLED coplanar faces (z-fight) with world positions.
//   3. HEIGHT        every walkable floor delta > 0.25 m has a legal
//                    transition (step <=0.25 / ramp <=30 deg that fits /
//                    stairs / elevator-shaft) at the connection.
//   4. REACH         flood-fill from the player spawn: every non-secret room
//                    is walk-reachable (openings + legal transitions only).
//   5. CONTAIN       no floating/disconnected rooms, no un-doored room
//                    interpenetration, no descent tube that misses its
//                    target room, no degenerate extents.
//
// The lint is game/slice code only (no device, no window): it reads the same
// CanonFloor the builder consumes plus the builder's shared constants
// (kCanonWallT / kCanonDoorHalf / kCanonLintel in level_loader.h), so every
// violation it reports corresponds 1:1 to generated geometry. The PHYSICAL
// complement is Gate C (runGoldenPathSelfTest): a collision-on headless walk
// of the golden path (spawn -> beat chain -> elevator lobby), no noclip.
//
// CLI: X3Engine --test-levellint   (lint Floor 1 + the fused 7-floor building)
//      X3Engine --test-goldenpath  (Gate C playthrough trace)

#include "level_loader.h"

#include <string>
#include <vector>

namespace x3::game {

enum class LintCategory : uint32_t {
    DoorSeat = 0,
    Seam,
    Height,
    Reach,
    Contain,
    kCount
};
const char* lintCategoryName(LintCategory c);

// One violation: category + a short stable CODE (grep/delta-friendly), the
// human message naming the rooms, and the world position of the defect.
struct LintViolation {
    LintCategory cat;
    std::string  code;    // e.g. GAP_SEAM, DOUBLED_WALL, DOOR_UNSEATED, ...
    std::string  msg;
    float x = 0, y = 0, z = 0;
};

struct LintReport {
    std::vector<LintViolation> violations;

    uint32_t countIn(LintCategory c) const;
    bool     clean() const { return violations.empty(); }

    // Log the categorized violation list (counts per category, then each
    // violation with its position). `label` names the linted world.
    void log(const std::string& label) const;

    // One-line per-category summary, e.g. "door-seat=3 seam=47 height=12
    // reach=0 contain=2 TOTAL=64" — the commit-message delta string.
    std::string summary() const;
};

// Lint a parsed+resolved canonical floor (or the fused building). Pure data.
LintReport lintCanonFloor(const CanonFloor& floor);

// --test-levellint: lint canonical Floor 1 AND the fused 7-floor building.
// Logs both categorized reports; returns true iff BOTH are clean. SKIP-pass
// if the canonical JSON is absent on this machine (mirrors --test-canonlevel).
bool runLevelLintSelfTest();

// --test-goldenpath (Gate C): build Floor 1 WITH door slabs, open the doors,
// then walk a physics character (collision on, no noclip) along the golden
// path: Jake's Cell -> Main Hall -> Security -> Research -> Medical ->
// Armory -> Boss Arena -> Elevator Lobby, routed through the doorway graph.
// Fails on any stuck leg (position + leg logged). Returns true iff the whole
// chain completes.
bool runGoldenPathSelfTest();

} // namespace x3::game
