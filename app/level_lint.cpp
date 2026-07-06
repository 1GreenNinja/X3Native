// LEVEL LINT — geometric connectivity gate over the resolved canon floor.
// See level_lint.h for the doctrine. This deliberately re-derives the same
// face-pick rules buildCanonFloor uses (addGapToRoom / addBridgeMouthToRoom /
// the slab-placement loop); if the builder's rules change, change BOTH or the
// lint goes blind — the mirror is the point (it validates what will be built
// from the same inputs, without needing a device or physics world).
#include "level_lint.h"
#include "level_loader.h"
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
                ++rep.unreachable;
                rep.violations.push_back(fmt("REACH     room %u '%s' unreachable from '%s'",
                                             i, roomName(i).c_str(), roomName(0).c_str()));
            }
    }
    return rep;
}

bool runLevelLintSelfTest() {
    CanonFloor floor = loadCanonFloor(canonProjectJsonPath(), 1);
    if (!floor.valid()) {
        x3::logInfo("--test-levellint: SKIPPED (no canonical JSON) — pass (legacy fallback world)");
        return true;
    }
    LevelLintReport rep = lintCanonFloor(floor);
    for (const std::string& v : rep.violations) x3::logWarn("[levellint] " + v);
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
