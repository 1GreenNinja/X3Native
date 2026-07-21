#pragma once
// ===========================================================================
// QA PROP-CLIP LINT (--test-propclip) — GATE A extension for the DRESSING layer.
//
// level_lint.cpp guards the ARCHITECTURE (door seats, cut spans, reachability).
// This lint guards the DRESSING: every kit prop CellDressing/RoomDressing places
// is audited as a world AABB (headless device meshBounds x instance transform x
// drawable nodeTransform) against its containing room's box:
//
//   WALL-CLIP   prop AABB crosses a room wall PLANE deeper than the graybox slab
//               (0.2 m thick, centred on the plane -> anything > 0.10 m past the
//               plane pokes OUT of the slab's far face: visible from the next
//               room or the void). Doorway cut spans are exempt (door frames
//               legitimately pierce the wall).
//   FLOOR-CLIP  prop AABB sinks below the room floor plane (y0) by > tolerance.
//   CEIL-CLIP   prop AABB rises above the ceiling plane (y1) by > tolerance.
//   OVERLAP     (warn-only, not gating) two props in one room whose AABBs
//               interpenetrate by > half the smaller prop's volume — AABBs are
//               conservative (a mug ON a table overlaps the table's box), so
//               this reports for the visual gate instead of failing the build.
//
// Ships with NEGATIVE CONTROLS (a planted through-wall AABB + a clean AABB)
// proving the checker can go red. Headless: no window, no Vulkan.
// ===========================================================================

#include <string>
#include <vector>

namespace x3::game {

struct CanonFloor;

struct PropClipReport {
    std::vector<std::string> violations;   // gating findings (wall/floor/ceil)
    std::vector<std::string> warnings;     // non-gating (overlap pairs, unresolved rooms)
    int checked   = 0;   // prop instances audited
    int wallClip  = 0;
    int floorClip = 0;
    int ceilClip  = 0;
    int overlap   = 0;   // warn-only
    bool pass() const { return violations.empty(); }
};

// One prop's world AABB vs its room + the doorway exemptions. Exposed so the
// negative controls exercise the EXACT production checker.
void propClipCheckAabb(const CanonFloor& floor, uint32_t room,
                       const float mn[3], const float mx[3],
                       const char* label, PropClipReport& rep);

// `--test-propclip` entry: loads the canon tower, builds CellDressing +
// RoomDressing on the headless device, audits every placed prop instance.
// Missing JSON = SKIP-as-pass (mirrors --test-levellint's fallback rule).
bool runPropClipSelfTest();

} // namespace x3::game
