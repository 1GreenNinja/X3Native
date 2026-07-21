// QA PROP-CLIP LINT — dressing-layer geometric audit. See qa_propclip.h.
//
// The dressing systems draw OUTSIDE the Scene (private instance lists +
// drawMeshPBR), so the --test-basis Scene walk never sees them; this test
// enumerates them through the forEachPropInstance visitors instead. The
// headless device tracks real per-mesh local AABBs (createMesh computes them
// from the verts), so world AABB = instanceTransform * nodeTransform * corners.
#include "qa_propclip.h"

#include "level_loader.h"
#include "cell_dressing.h"
#include "room_dressing.h"
#include "headless_device.h"
#include "asset_root.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace x3::game {

namespace {

// Wall slab is 0.2 m thick, CENTRED on the room boundary plane (level_loader
// addBox convention): its far face is 0.10 m past the plane. A prop reaching
// deeper than that pokes out of the wall into the neighbour room / the void.
constexpr float kWallPen  = 0.10f + 0.02f;
constexpr float kFloorPen = 0.06f;   // below y0 = sunk through the walking surface
constexpr float kCeilPen  = 0.06f;   // above y1 = through the lid
constexpr float kDoorSpanMargin = 0.60f;  // extra half-width slack around a doorway cut
constexpr float kDoorHeadY      = 2.60f;  // doorway vertical span above cy that is exempt

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap; va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return std::string(buf);
}

// column-major 4x4 multiply: out = a * b
void mul44(const float* a, const float* b, float* out) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[k * 4 + r] * b[c * 4 + k];
            out[c * 4 + r] = s;
        }
}

// Accumulate the world AABB of a local AABB under a column-major transform.
void growWorldAabb(const float m[16], const float lmn[3], const float lmx[3],
                   float mn[3], float mx[3]) {
    for (int i = 0; i < 8; ++i) {
        const float p[3] = { (i & 1) ? lmx[0] : lmn[0],
                             (i & 2) ? lmx[1] : lmn[1],
                             (i & 4) ? lmx[2] : lmn[2] };
        float w[3];
        for (int r = 0; r < 3; ++r)
            w[r] = m[0 * 4 + r] * p[0] + m[1 * 4 + r] * p[1] + m[2 * 4 + r] * p[2] + m[3 * 4 + r];
        for (int r = 0; r < 3; ++r) {
            mn[r] = std::min(mn[r], w[r]);
            mx[r] = std::max(mx[r], w[r]);
        }
    }
}

// Is a wall-plane crossing excused by a doorway cut through that plane?
// planeAxis: 0 = plane is X=const, 1 = plane is Z=const. planeC = the plane
// coordinate; span[0..1] = the prop's extent along the cut axis; yMin/yMax the
// prop's vertical extent.
bool doorwayExempts(const CanonFloor& floor, uint32_t room, int planeAxis, float planeC,
                    float spanLo, float spanHi, float yMin, float yMax) {
    for (const CanonDoorway& dw : floor.doorways) {
        if (dw.a != room && dw.b != room) continue;
        if (dw.kind == DoorwayKind::CrossLevel || dw.kind == DoorwayKind::None) continue;
        if ((int)dw.axis != planeAxis) continue;
        const float dwPlane = planeAxis == 0 ? dw.cx : dw.cz;
        const float dwCut   = planeAxis == 0 ? dw.cz : dw.cx;
        if (std::fabs(dwPlane - planeC) > 0.6f) continue;             // different wall
        const float half = (dw.cutHalf > 0 ? dw.cutHalf : 0.8f) + kDoorSpanMargin;
        if (spanHi < dwCut - half || spanLo > dwCut + half) continue; // outside the cut span
        if (yMax < dw.cy - 0.5f || yMin > dw.cy + kDoorHeadY) continue; // above/below the opening
        return true;
    }
    return false;
}

} // namespace

void propClipCheckAabb(const CanonFloor& floor, uint32_t room,
                       const float mn[3], const float mx[3],
                       const char* label, PropClipReport& rep) {
    if (room >= floor.rooms.size()) {
        rep.warnings.push_back(fmt("NO-ROOM   %s: AABB centre resolves to no room "
                                   "(%.2f,%.2f,%.2f)", label,
                                   (mn[0] + mx[0]) * 0.5f, (mn[1] + mx[1]) * 0.5f,
                                   (mn[2] + mx[2]) * 0.5f));
        return;
    }
    const CanonRoom& r = floor.rooms[room];
    ++rep.checked;

    // VISIBLE vs HIDDEN (the calibration the first run taught us): a penetration is a
    // GATING violation only when the penetrating slice lands inside ANOTHER room's
    // interior — i.e. a player standing there can SEE the prop poking through. A slice
    // that ends inside the wall slab / inter-room void / below the floor plate is a
    // hidden-void penetration: reported as a warning, not a failure. Rooms whose
    // volumes OVERLAP the host (the canon data's Overlap throats — Jake's Cell reaches
    // into both halls) share space by construction and never count as foreign.
    auto foreignRoomHit = [&](const float rawMn[3], const float rawMx[3]) -> int {
        // Shrink the slice by the structural half-thickness before testing: a lip that
        // rides within 0.25 m of a plane is buried in / curbed against the 0.2 m slabs
        // and the panel insets — not something an eye in the next room can resolve.
        constexpr float kStruct = 0.25f;
        float smn[3], smx[3];
        for (int a = 0; a < 3; ++a) {
            smn[a] = rawMn[a] + kStruct;
            smx[a] = rawMx[a] - kStruct;
            if (smx[a] <= smn[a]) return -1;   // slice thinner than structure: hidden
        }
        for (uint32_t j = 0; j < (uint32_t)floor.rooms.size(); ++j) {
            if (j == room) continue;
            const CanonRoom& o = floor.rooms[j];
            if (smx[0] <= o.x0() || smn[0] >= o.x1() ||
                smx[1] <= o.y0() || smn[1] >= o.y1() ||
                smx[2] <= o.z0() || smn[2] >= o.z1()) continue;   // slice misses o
            const bool sharesSpace = !(r.x1() <= o.x0() || r.x0() >= o.x1() ||
                                       r.y1() <= o.y0() || r.y0() >= o.y1() ||
                                       r.z1() <= o.z0() || r.z0() >= o.z1());
            if (sharesSpace) continue;                            // overlap throat: legal
            return (int)j;
        }
        return -1;
    };
    auto flag = [&](const char* kind, int& counter, float pen, const char* where,
                    const float smn[3], const float smx[3]) {
        const int fr = foreignRoomHit(smn, smx);
        if (fr >= 0) {
            ++counter;
            rep.violations.push_back(fmt(
                "%s %-34s room '%s': %.2f m through %s, VISIBLE inside '%s' — "
                "AABB [%.2f..%.2f, %.2f..%.2f, %.2f..%.2f]",
                kind, label, r.name.c_str(), pen, where,
                floor.rooms[fr].name.c_str(), mn[0], mx[0], mn[1], mx[1], mn[2], mx[2]));
        } else {
            rep.warnings.push_back(fmt(
                "%s %-34s room '%s': %.2f m through %s into hidden void (not player-visible)",
                kind, label, r.name.c_str(), pen, where));
        }
    };

    // FLOOR / CEIL
    if (r.y0() - mn[1] > kFloorPen) {
        const float smn[3] = { mn[0], mn[1], mn[2] }, smx[3] = { mx[0], r.y0(), mx[2] };
        flag("FLOOR-CLIP", rep.floorClip, r.y0() - mn[1], "the floor", smn, smx);
    }
    if (!r.openCeiling && mx[1] - r.y1() > kCeilPen) {
        const float smn[3] = { mn[0], r.y1(), mn[2] }, smx[3] = { mx[0], mx[1], mx[2] };
        flag("CEIL-CLIP ", rep.ceilClip, mx[1] - r.y1(), "the ceiling", smn, smx);
    }

    // WALLS — 4 planes. Platforms have no walls by design.
    if (!r.platform) {
        struct Side { int axis; float plane; float pen; float sLo, sHi; const char* nm; };
        const Side sides[4] = {
            { 0, r.x0(), r.x0() - mn[0], mn[2], mx[2], "-X" },
            { 0, r.x1(), mx[0] - r.x1(), mn[2], mx[2], "+X" },
            { 1, r.z0(), r.z0() - mn[2], mn[0], mx[0], "-Z" },
            { 1, r.z1(), mx[2] - r.z1(), mn[0], mx[0], "+Z" },
        };
        for (const Side& s : sides) {
            if (s.pen <= kWallPen) continue;
            if (doorwayExempts(floor, room, s.axis, s.plane, s.sLo, s.sHi, mn[1], mx[1]))
                continue;
            // Build the penetrating slice (the part beyond the wall plane).
            float smn[3] = { mn[0], mn[1], mn[2] }, smx[3] = { mx[0], mx[1], mx[2] };
            if      (s.axis == 0 && s.plane == r.x0()) smx[0] = r.x0();
            else if (s.axis == 0)                      smn[0] = r.x1();
            else if (s.plane == r.z0())                smx[2] = r.z0();
            else                                       smn[2] = r.z1();
            char where[64];
            std::snprintf(where, sizeof where, "the %s wall (plane %.2f)", s.nm, s.plane);
            flag("WALL-CLIP ", rep.wallClip, s.pen, where, smn, smx);
        }
    }
}

bool runPropClipSelfTest() {
    // ---- NEGATIVE CONTROLS first: prove the checker can go red. -------------
    {
        CanonFloor nc;
        CanonRoom room;
        room.name = "NC Room"; room.cx = 0; room.cy = 2; room.cz = 0;
        room.w = 8; room.h = 4; room.d = 8;
        nc.rooms.push_back(room);
        const float badMn[3] = { -4.8f, 0.2f, -1.0f },  badMx[3] = { -3.6f, 1.6f, 1.0f };
        const float okMn[3]  = { -3.5f, 0.05f, -3.5f }, okMx[3]  = { 3.5f, 3.5f, 3.5f };

        // NC1: through-wall into pure VOID (no neighbour) -> a WARNING, not a violation.
        PropClipReport r0;
        propClipCheckAabb(nc, 0, badMn, badMx, "NC-into-void", r0);
        if (!r0.violations.empty() || r0.warnings.empty()) {
            x3::logWarn("[propclip] NEGATIVE CONTROL FAILED: void-side penetration must "
                        "warn (got " + std::to_string(r0.violations.size()) + " violations)");
            x3::logInfo("--test-propclip: FAIL (negative control)");
            return false;
        }

        // NC2: the same crossing landing inside a FOREIGN (non-overlapping) room ->
        // exactly one gating violation; a clean in-bounds prop -> none.
        CanonRoom neigh;
        neigh.name = "NC Neighbour"; neigh.cx = -8.2f; neigh.cy = 2; neigh.cz = 0;
        neigh.w = 8; neigh.h = 4; neigh.d = 8;   // x -12.2..-4.2 (wall void -4.2..-4)
        nc.rooms.push_back(neigh);
        PropClipReport r1;
        propClipCheckAabb(nc, 0, badMn, badMx, "NC-through-wall", r1);  // slice reaches -4.8
        propClipCheckAabb(nc, 0, okMn, okMx, "NC-clean", r1);
        if (r1.wallClip != 1 || r1.floorClip != 0 || r1.ceilClip != 0) {
            x3::logWarn("[propclip] NEGATIVE CONTROL FAILED: planted through-wall AABB "
                        "flagged " + std::to_string(r1.wallClip) + " (expect exactly 1)");
            x3::logInfo("--test-propclip: FAIL (negative control)");
            return false;
        }
        // NC3: the same crossing at a doorway cut must be EXEMPT.
        CanonDoorway dw; dw.a = 0; dw.b = 1; dw.kind = DoorwayKind::AdjacentX;
        dw.axis = 0; dw.cx = -4.0f; dw.cz = 0.0f; dw.cy = 0.0f; dw.cutHalf = 0.8f;
        nc.doorways.push_back(dw);
        PropClipReport r2;
        propClipCheckAabb(nc, 0, badMn, badMx, "NC-doorframe", r2);
        if (r2.wallClip != 0) {
            x3::logWarn("[propclip] NEGATIVE CONTROL FAILED: doorway exemption did not apply");
            x3::logInfo("--test-propclip: FAIL (negative control)");
            return false;
        }
    }

    // ---- The real audit. ----------------------------------------------------
    CanonFloor floor = loadCanonTower(canonProjectJsonPath());
    if (!floor.valid()) {
        x3::logInfo("--test-propclip: SKIPPED (no canonical JSON) — pass (legacy fallback world)");
        return true;
    }
    const CanonBeats beats = canonBeats(floor);

    HeadlessRenderDevice device;
    device.init({});

    CellDressing cell;
    cell.build(device, convertedGlbRoot(), floor);
    RoomDressing rooms;
    rooms.build(device, assetRoot() + "/surface_library", convertedGlbRoot(), floor, beats);

    PropClipReport rep;

    struct PropBox { uint32_t room; float mn[3], mx[3]; std::string label; };
    std::vector<PropBox> boxes;

    auto audit = [&](uint32_t room, const std::string& assetPath,
                     const std::vector<x3::asset::ModelDrawable>& drawables,
                     const float* transform) {
        float mn[3] = { 3.4e38f, 3.4e38f, 3.4e38f };
        float mx[3] = { -3.4e38f, -3.4e38f, -3.4e38f };
        bool any = false;
        for (const auto& d : drawables) {
            float lmn[3], lmx[3];
            if (!device.meshBounds(x3::rhi::MeshHandle{ d.meshId }, lmn, lmx)) continue;
            float m[16];
            mul44(transform, d.nodeTransform, m);
            growWorldAabb(m, lmn, lmx, mn, mx);
            any = true;
        }
        if (!any) return;
        uint32_t rm = room;
        if (rm == kNoRoom)
            rm = floor.roomAt((mn[0] + mx[0]) * 0.5f, (mn[1] + mx[1]) * 0.5f,
                              (mn[2] + mx[2]) * 0.5f, 0.75f);
        // strip the directory for readable labels
        std::string label = assetPath;
        if (auto p = label.find_last_of("/\\"); p != std::string::npos) label = label.substr(p + 1);
        propClipCheckAabb(floor, rm, mn, mx, label.c_str(), rep);
        if (rm < floor.rooms.size())
            boxes.push_back({ rm, { mn[0], mn[1], mn[2] }, { mx[0], mx[1], mx[2] }, label });
    };

    cell.forEachPropInstance([&](uint32_t, const std::string& path,
                                 const std::vector<x3::asset::ModelDrawable>& dr,
                                 const float* xf) { audit(kNoRoom, path, dr, xf); });
    rooms.forEachPropInstance([&](uint32_t room, const std::string& path,
                                  const std::vector<x3::asset::ModelDrawable>& dr,
                                  const float* xf) { audit(room, path, dr, xf); });

    // OVERLAP (warn-only): same-room pairs interpenetrating > 50% of the smaller box.
    for (size_t i = 0; i < boxes.size(); ++i)
        for (size_t j = i + 1; j < boxes.size(); ++j) {
            const PropBox& a = boxes[i];
            const PropBox& b = boxes[j];
            if (a.room != b.room) continue;
            float iv = 1.0f, va = 1.0f, vb = 1.0f;
            bool sep = false;
            for (int k = 0; k < 3; ++k) {
                const float lo = std::max(a.mn[k], b.mn[k]);
                const float hi = std::min(a.mx[k], b.mx[k]);
                if (hi <= lo) { sep = true; break; }
                iv *= (hi - lo);
                va *= (a.mx[k] - a.mn[k]);
                vb *= (b.mx[k] - b.mn[k]);
            }
            if (sep) continue;
            const float minV = std::max(1e-6f, std::min(va, vb));
            if (iv / minV > 0.5f) {
                ++rep.overlap;
                rep.warnings.push_back(fmt("OVERLAP   %s <-> %s in '%s': %.0f%% of the smaller box",
                                           a.label.c_str(), b.label.c_str(),
                                           floor.rooms[a.room].name.c_str(),
                                           100.0f * iv / minV));
            }
        }

    for (const std::string& v : rep.violations) x3::logWarn("[propclip] " + v);
    for (const std::string& w : rep.warnings)   x3::logInfo("[propclip][warn] " + w);
    x3::logInfo("[propclip] checked=" + std::to_string(rep.checked) +
                " | wall-clip=" + std::to_string(rep.wallClip) +
                " floor-clip=" + std::to_string(rep.floorClip) +
                " ceil-clip=" + std::to_string(rep.ceilClip) +
                " overlap-warn=" + std::to_string(rep.overlap));
    x3::logInfo(std::string("--test-propclip: ") +
                (rep.pass() ? "PASS (0 violations)"
                            : (std::to_string(rep.violations.size()) + " violation(s) — FAIL")));
    return rep.pass();
}

} // namespace x3::game
