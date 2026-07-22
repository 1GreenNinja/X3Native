// LEVEL LINT — geometric connectivity gate over the resolved canon floor.
// See level_lint.h for the doctrine. This deliberately re-derives the same
// face-pick rules buildCanonFloor uses (addGapToRoom / addBridgeMouthToRoom /
// the slab-placement loop); if the builder's rules change, change BOTH or the
// lint goes blind — the mirror is the point (it validates what will be built
// from the same inputs, without needing a device or physics world).
#include "level_lint.h"
#include "level_loader.h"
#include "mesh_prims.h"           // PRIM WINDING check (makeRamp regression gate)
#include "engine/core/x3_log.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <vector>

namespace x3::game {

namespace {

// Mirrors of the builder's constants (level_loader.cpp).
constexpr float kWallT    = 0.2f;   // wall thickness — a seated slab sits within this of a plane
constexpr float kSeatTol  = kWallT * 0.5f + 0.15f;   // slab-to-plane tolerance (0.25)
constexpr float kEps      = 0.02f;

const char* kindName(DoorwayKind k) {
    switch (k) {
        case DoorwayKind::AdjacentX:  return "AdjacentX";
        case DoorwayKind::AdjacentZ:  return "AdjacentZ";
        case DoorwayKind::GapBridge:  return "GapBridge";
        case DoorwayKind::Overlap:    return "Overlap";
        case DoorwayKind::CrossLevel: return "CrossLevel";
        default:                      return "None";
    }
}

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap; va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return std::string(buf);
}

} // namespace

LevelLintReport lintCanonFloor(const CanonFloor& floor) {
    LevelLintReport rep;
    const auto& rooms = floor.rooms;

    auto roomName = [&](uint32_t i) {
        return i < rooms.size() ? rooms[i].name : std::string("<bad>");
    };

    for (uint32_t di = 0; di < (uint32_t)floor.doorways.size(); ++di) {
        const CanonDoorway& dw = floor.doorways[di];
        if (dw.a >= rooms.size() || dw.b >= rooms.size()) continue;
        const CanonRoom& A = rooms[dw.a];
        const CanonRoom& B = rooms[dw.b];
        if (dw.kind == DoorwayKind::CrossLevel) continue;   // vertical tube: no wall cut

        // Axis mapping: axis 0 = wall plane X=const (plane coord = cx, cut runs in Z);
        //               axis 1 = wall plane Z=const (plane coord = cz, cut runs in X).
        const bool planeIsX = (dw.axis == 0);
        const float planeC  = planeIsX ? dw.cx : dw.cz;
        const float cutC    = planeIsX ? dw.cz : dw.cx;
        const float half    = dw.cutHalf > 0.0f ? dw.cutHalf : 0.8f;

        auto faceSpan = [&](const CanonRoom& r, float& lo, float& hi, float& plane) {
            if (planeIsX) {
                lo = r.z0(); hi = r.z1();
                plane = (std::fabs(planeC - r.x0()) < std::fabs(planeC - r.x1())) ? r.x0() : r.x1();
            } else {
                lo = r.x0(); hi = r.x1();
                plane = (std::fabs(planeC - r.z0()) < std::fabs(planeC - r.z1())) ? r.z0() : r.z1();
            }
        };
        float aLo, aHi, aPlane, bLo, bHi, bPlane;
        faceSpan(A, aLo, aHi, aPlane);
        faceSpan(B, bLo, bHi, bPlane);

        if (dw.kind == DoorwayKind::AdjacentX || dw.kind == DoorwayKind::AdjacentZ ||
            dw.kind == DoorwayKind::Overlap || dw.kind == DoorwayKind::GapBridge) {
            // CUT-SPAN: the cut coordinate must sit inside BOTH host faces' spans with
            // half-width margin — a cut outside the span builds a lintel floating in
            // void (the doors_hall.png fragments).
            auto checkSpan = [&](const CanonRoom& r, float lo, float hi, uint32_t ri) {
                if (cutC < lo + half - kEps || cutC > hi - half + kEps) {
                    ++rep.cutSpan;
                    rep.violations.push_back(fmt(
                        "CUT-SPAN  dw#%u %s [%s <-> %s]: cut %.2f outside face span [%.2f..%.2f] (half %.2f) of '%s'",
                        di, kindName(dw.kind), roomName(dw.a).c_str(), roomName(dw.b).c_str(),
                        cutC, lo, hi, half, roomName(ri).c_str()));
                }
            };
            checkSpan(A, aLo, aHi, dw.a);
            checkSpan(B, bLo, bHi, dw.b);
        }

        if (dw.kind == DoorwayKind::Overlap && !dw.junction) {
            // JUNCTION: interpenetrating corridors are OPEN THROATS. A slab-eligible
            // Overlap doorway puts a door in the middle of the junction (the playtest
            // "doors sitting in the middle of the rooms").
            ++rep.junctionSlab;
            rep.violations.push_back(fmt(
                "JUNCTION  dw#%u Overlap [%s <-> %s] is slab-eligible (junction flag unset) — door would stand mid-corridor at (%.2f, %.2f)",
                di, roomName(dw.a).c_str(), roomName(dw.b).c_str(), dw.cx, dw.cz));
        }

        if ((dw.kind == DoorwayKind::AdjacentX || dw.kind == DoorwayKind::AdjacentZ ||
             (dw.kind == DoorwayKind::Overlap && !dw.junction))) {
            // DOOR-SEAT: the slab's plane coordinate must lie ON one host face plane
            // (within wall thickness) — otherwise the slab floats between/off walls.
            const float dA = std::fabs(planeC - aPlane);
            const float dB = std::fabs(planeC - bPlane);
            if (dA > kSeatTol && dB > kSeatTol) {
                ++rep.doorSeat;
                rep.violations.push_back(fmt(
                    "DOOR-SEAT dw#%u %s [%s <-> %s]: slab plane %.2f is %.2f/%.2f off the host planes (%.2f / %.2f) — floating slab",
                    di, kindName(dw.kind), roomName(dw.a).c_str(), roomName(dw.b).c_str(),
                    planeC, dA, dB, aPlane, bPlane));
            }
        }
    }

    // REACH: BFS over doorways from room 0 — every room must be walkable-reachable
    // (all doorway kinds traversable; CrossLevel tubes count).
    if (!rooms.empty()) {
        std::vector<char> seen(rooms.size(), 0);
        std::deque<uint32_t> q{ 0 };
        seen[0] = 1;
        while (!q.empty()) {
            uint32_t r = q.front(); q.pop_front();
            for (const CanonDoorway& dw : floor.doorways) {
                uint32_t o = (dw.a == r) ? dw.b : (dw.b == r) ? dw.a : kNoRoom;
                if (o == kNoRoom || o >= rooms.size() || seen[o]) continue;
                seen[o] = 1; q.push_back(o);
            }
        }
        for (uint32_t i = 0; i < rooms.size(); ++i)
            if (!seen[i]) {
                // W5-1: open platforms (Nexus tiers) carry NO doorways by design — they
                // hang inside the cavern and are reached by canon_45's scaffold stairs,
                // which the doorway BFS cannot see. Exempt, not unreachable.
                if (rooms[i].platform) continue;
                ++rep.unreachable;
                rep.violations.push_back(fmt("REACH     room %u '%s' unreachable from '%s'",
                                             i, roomName(i).c_str(), roomName(0).c_str()));
            }
    }
    return rep;
}

// ---- PRIM WINDING lint (QA mainlevel sweep, D10 root cause) -----------------------
// makeRamp shipped faces whose geometric winding flipped with `dir`/`axis` (mixed
// front/back parity). Invisible on the no-cull emissive route; on the backface-culling
// PBR route a wrong-parity ramp TOP disappears and the threshold opens a fog-void
// window. This gate builds every (axis, dir) ramp variant and asserts each triangle's
// geometric normal agrees with its authored outward vertex normal, so the class can
// never regress silently again.
namespace {
uint32_t primWindingViolations(const x3::prims::PrimMesh& m) {
    uint32_t bad = 0;
    for (size_t t = 0; t + 2 < m.index.size(); t += 3) {
        const auto& a = m.verts[m.index[t]];
        const auto& b = m.verts[m.index[t+1]];
        const auto& c = m.verts[m.index[t+2]];
        const float e1x = b.pos[0]-a.pos[0], e1y = b.pos[1]-a.pos[1], e1z = b.pos[2]-a.pos[2];
        const float e2x = c.pos[0]-a.pos[0], e2y = c.pos[1]-a.pos[1], e2z = c.pos[2]-a.pos[2];
        const float gnx = e1y*e2z - e1z*e2y, gny = e1z*e2x - e1x*e2z, gnz = e1x*e2y - e1y*e2x;
        if (gnx*a.normal[0] + gny*a.normal[1] + gnz*a.normal[2] < 1e-6f) ++bad;
    }
    return bad;
}
} // namespace

bool runLevelLintSelfTest() {
    // W3-2: lint the WHOLE TOWER (all floors merged + the elevator spine) — the same
    // CanonFloor shape the game now builds, so the gate checks what ships.
    CanonFloor floor = loadCanonTower(canonProjectJsonPath());
    if (!floor.valid()) {
        x3::logInfo("--test-levellint: SKIPPED (no canonical JSON) — pass (legacy fallback world)");
        return true;
    }
    LevelLintReport rep = lintCanonFloor(floor);

    // PRIM WINDING gate: every ramp variant the threshold builder can emit, plus a
    // NEGATIVE CONTROL (a deliberately flipped triangle must be caught red).
    {
        uint32_t windingBad = 0;
        for (uint32_t axis = 0; axis <= 1; ++axis)
            for (int d = -1; d <= 1; d += 2) {
                x3::prims::PrimMesh ramp = x3::prims::makeRamp(
                    2.0f, 0.0f, 3.0f, 0.6f, 1.07f, 0.75f, axis, (float)d, 0.5f);
                const uint32_t bad = primWindingViolations(ramp);
                if (bad) {
                    windingBad += bad;
                    rep.violations.push_back(fmt("WINDING   makeRamp axis=%u dir=%+d: %u backward tri(s)",
                                                 axis, d, bad));
                }
            }
        // Negative control: swap one triangle of a known-good ramp — must be detected.
        x3::prims::PrimMesh ctrl = x3::prims::makeRamp(0, 0, 0, 0.6f, 1.0f, 0.7f, 0, 1.0f, 0.5f);
        if (ctrl.index.size() >= 3) std::swap(ctrl.index[1], ctrl.index[2]);
        const bool ctrlCaught = primWindingViolations(ctrl) > 0;
        if (!ctrlCaught)
            rep.violations.push_back("WINDING   NEGATIVE CONTROL FAILED: flipped tri not detected");
        x3::logInfo("[levellint] prim-winding: " + std::to_string(windingBad) +
                    " backward tris across ramp variants; negative control " +
                    (ctrlCaught ? "red-capable" : "BROKEN"));
    }
    for (const std::string& v : rep.violations) x3::logWarn("[levellint] " + v);
    // Per-floor room counts (roomFloorNum is filled by loadCanonTower).
    if (!floor.roomFloorNum.empty()) {
        std::string byFloor;
        int cur = 0, cnt = 0;
        for (size_t i = 0; i <= floor.roomFloorNum.size(); ++i) {
            const int f = (i < floor.roomFloorNum.size()) ? floor.roomFloorNum[i] : -1;
            if (f != cur) {
                if (cur != 0) byFloor += " F" + std::to_string(cur) + ":" + std::to_string(cnt);
                cur = f; cnt = 0;
            }
            ++cnt;
        }
        x3::logInfo("[levellint] per-floor rooms:" + byFloor);
    }
    x3::logInfo("[levellint] rooms=" + std::to_string(floor.rooms.size()) +
                " doorways=" + std::to_string(floor.doorways.size()) +
                " | door-seat=" + std::to_string(rep.doorSeat) +
                " junction=" + std::to_string(rep.junctionSlab) +
                " cut-span=" + std::to_string(rep.cutSpan) +
                " unreachable=" + std::to_string(rep.unreachable));
    x3::logInfo(std::string("--test-levellint: ") + (rep.pass() ? "PASS (0 violations)"
                : (std::to_string(rep.violations.size()) + " violation(s) — FAIL")));
    return rep.pass();
}

} // namespace x3::game
