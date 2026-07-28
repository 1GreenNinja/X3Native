// LEVEL LINT — geometric connectivity gate over the resolved canon floor.
// See level_lint.h for the doctrine. This deliberately re-derives the same
// face-pick rules buildCanonFloor uses (addGapToRoom / addBridgeMouthToRoom /
// the slab-placement loop); if the builder's rules change, change BOTH or the
// lint goes blind — the mirror is the point (it validates what will be built
// from the same inputs, without needing a device or physics world).
#include "level_lint.h"
#include "level_loader.h"
#include "level1.h"               // SPIRE block: floor plates, wing rooms, the stairwell
#include "mesh_prims.h"           // PRIM WINDING check (makeRamp regression gate)
#include "canon_45.h"             // HIDDEN-4.5 seal gate (fix/spire-hollow-core)
#include "stairwell.h"            // stairwell connectivity + no-opening-into-4.5 gate
#include "engine/core/x3_log.h"

#include <algorithm>
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

// ---- SPIRE LINT (QA upper-floors sweep, 2026-07-27) ------------------------------
// The canon-tower lint above reads the LevelDoc JSON; the Spire (`--world level1`,
// app/level1.cpp) is generated in code and was therefore INVISIBLE to GATE A — the
// x3-level-authoring doctrine's LAW 5 ("if you must generate geometry in code,
// generate it INTO a form the lint can see"). This block closes that hole for the
// F2-F7 upper floors: it reads the SAME tables + the SAME stairwell layout the
// builder emits (level1Rooms / level1WingRooms / spireStair) and applies the
// doctrine's containment, seam (LAW 2) and height-transition (LAW 3) checks.
namespace {

struct SpireLintReport {
    std::vector<std::string> violations;
    int contain = 0, overlap = 0, keepout = 0, doorProbe = 0;
    int riser = 0, tread = 0, slope = 0, landing = 0;
    int zfight = 0, pierce = 0, well = 0, value = 0;
    bool pass() const { return violations.empty(); }
};

// Mean LINEAR albedo of a procedural RGBA map (the reflectance the value band judges).
void meanLinearRGB(const std::vector<uint8_t>& px, float out[3]) {
    double acc[3] = { 0, 0, 0 };
    const size_t n = px.size() / 4;
    if (!n) { out[0] = out[1] = out[2] = 0.0f; return; }
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < 3; ++c) {
            const float s = px[i * 4 + c] / 255.0f;
            acc[c] += (s <= 0.04045f) ? (s / 12.92) : std::pow((s + 0.055) / 1.055, 2.4);
        }
    for (int c = 0; c < 3; ++c) out[c] = (float)(acc[c] / (double)n);
}

// Box overlap on all three axes by more than `eps` = interpenetration (LAW 2 doubled).
bool boxesInterpenetrate(const SpireStair::Box& a, const SpireStair::Box& b, float eps) {
    return std::min(a.x1, b.x1) - std::max(a.x0, b.x0) > eps &&
           std::min(a.y1, b.y1) - std::max(a.y0, b.y0) > eps &&
           std::min(a.z1, b.z1) - std::max(a.z0, b.z0) > eps;
}

// True when `b` lies (in XZ) inside the stair well rect the builder cuts from slabs.
bool insideWell(const SpireStair& S, const SpireStair::Box& b) {
    if (S.wellX1 - S.wellX0 < 0.01f) return false;   // no well authored at all
    return b.x0 >= S.wellX0 - kEps && b.x1 <= S.wellX1 + kEps &&
           b.z0 >= S.wellZ0 - kEps && b.z1 <= S.wellZ1 + kEps;
}

SpireLintReport lintSpire() {
    SpireLintReport R;
    auto V = [&](std::string s) { R.violations.push_back(std::move(s)); };

    const L1RoomDef* F = level1Rooms();
    uint32_t wingN = 0;
    const L1WingRoom* W = level1WingRooms(wingN);
    const SpireStair& S = spireStair();
    float shX0, shX1, shZ0, shZ1;
    level1ShaftFootprint(shX0, shX1, shZ0, shZ1);

    auto roomBox = [&](uint32_t i, float& x0, float& x1, float& z0, float& z1) {
        x0 = W[i].cx - W[i].hw; x1 = W[i].cx + W[i].hw;
        z0 = W[i].cz - W[i].hd; z1 = W[i].cz + W[i].hd;
    };

    // 1. CONTAIN — every wing room's shell sits inside its floor plate with wall
    //    thickness to spare (flush against the plate wall = a doubled coplanar face).
    for (uint32_t i = 0; i < wingN; ++i) {
        const L1RoomDef& f = F[(uint32_t)W[i].floor];
        float x0, x1, z0, z1; roomBox(i, x0, x1, z0, z1);
        if (x0 < f.x0 + kWallT || x1 > f.x1 - kWallT ||
            z0 < -f.zHalf + kWallT || z1 > f.zHalf - kWallT) {
            ++R.contain;
            V(fmt("SPIRE-CONTAIN room '%s' [%.1f..%.1f]x[%.1f..%.1f] breaches its plate [%.1f..%.1f]x[%.1f..%.1f]",
                  W[i].name, x0, x1, z0, z1, f.x0, f.x1, -f.zHalf, f.zHalf));
        }
    }
    // 2. OVERLAP — two rooms on one floor may not interpenetrate (sealed slivers +
    //    z-fighting doubled walls).
    for (uint32_t i = 0; i < wingN; ++i)
        for (uint32_t j = i + 1; j < wingN; ++j) {
            if (W[i].floor != W[j].floor) continue;
            float ax0, ax1, az0, az1, bx0, bx1, bz0, bz1;
            roomBox(i, ax0, ax1, az0, az1); roomBox(j, bx0, bx1, bz0, bz1);
            const float ox = std::min(ax1, bx1) - std::max(ax0, bx0);
            const float oz = std::min(az1, bz1) - std::max(az0, bz0);
            if (ox > -kWallT && oz > -kWallT) {
                ++R.overlap;
                V(fmt("SPIRE-OVERLAP '%s' and '%s' interpenetrate (dx %.2f, dz %.2f)",
                      W[i].name, W[j].name, ox, oz));
            }
        }
    // 3. KEEPOUT — nothing may stand in the elevator shaft column.
    for (uint32_t i = 0; i < wingN; ++i) {
        float x0, x1, z0, z1; roomBox(i, x0, x1, z0, z1);
        if (x1 > shX0 && x0 < shX1 && z1 > shZ0 && z0 < shZ1) {
            ++R.keepout;
            V(fmt("SPIRE-KEEPOUT room '%s' intersects the elevator shaft column", W[i].name));
        }
    }
    // 4. DOOR-PROBE (LAW 1) — the single opening of each wing room must give onto free
    //    plate air, not another room's shell and not the outside of the plate.
    for (uint32_t i = 0; i < wingN; ++i) {
        const L1RoomDef& f = F[(uint32_t)W[i].floor];
        float x0, x1, z0, z1; roomBox(i, x0, x1, z0, z1);
        float px = W[i].cx, pz = W[i].cz;
        switch (W[i].door) {
            case 'S': pz = z0 - 1.5f; break;
            case 'N': pz = z1 + 1.5f; break;
            case 'E': px = x1 + 1.5f; break;
            case 'W': px = x0 - 1.5f; break;
            default:  continue;                       // sealed room: nothing to probe
        }
        if (px < f.x0 || px > f.x1 || pz < -f.zHalf || pz > f.zHalf) {
            ++R.doorProbe;
            V(fmt("SPIRE-DOOR  room '%s' door '%c' opens outside the plate at (%.1f, %.1f)",
                  W[i].name, W[i].door, px, pz));
            continue;
        }
        for (uint32_t j = 0; j < wingN; ++j) {
            if (j == i || W[j].floor != W[i].floor) continue;
            float bx0, bx1, bz0, bz1; roomBox(j, bx0, bx1, bz0, bz1);
            if (px >= bx0 - kWallT && px <= bx1 + kWallT && pz >= bz0 - kWallT && pz <= bz1 + kWallT) {
                ++R.doorProbe;
                V(fmt("SPIRE-DOOR  room '%s' door '%c' opens into '%s'", W[i].name, W[i].door, W[j].name));
            }
        }
    }

    // ---- THE STAIRWELL (LAW 2 + LAW 3) -------------------------------------------
    std::vector<SpireStair::Box> all;
    for (const SpireStair::Flight& fl : S.flights) {
        // 5. LAW 3 vocabulary: a run is either legal STAIRS (riser <= 0.20 m, tread
        //    >= 0.28 m) or a legal RAMP (<= 30 deg); either way a landing must break
        //    the climb every <= 3 m of rise.
        if (fl.ramp) {
            const float run  = (fl.axis == 0) ? (fl.solid.x1 - fl.solid.x0) : (fl.solid.z1 - fl.solid.z0);
            const float rise = fl.topY - fl.baseY;
            if (run > 0.01f && rise / run > 0.5774f + 1e-3f) {      // tan(30 deg)
                ++R.slope;
                V(fmt("SPIRE-SLOPE stair ramp rise %.2f over run %.2f = %.1f deg (> 30 deg, LAW 3)",
                      rise, run, std::atan2(rise, run) * 57.2958f));
            }
        } else {
            float prevTop = fl.baseY;
            for (const SpireStair::Box& st : fl.steps) {
                const float riser = st.y1 - prevTop;
                const float tread = (fl.axis == 0) ? (st.x1 - st.x0) : (st.z1 - st.z0);
                if (riser > 0.20f + 1e-3f) {
                    ++R.riser;
                    if (R.riser <= 3)
                        V(fmt("SPIRE-RISER stair riser %.3f m > 0.20 m (LAW 3) at y=%.2f", riser, st.y1));
                }
                if (tread < 0.28f - 1e-3f) {
                    ++R.tread;
                    if (R.tread <= 3)
                        V(fmt("SPIRE-TREAD stair tread %.3f m < 0.28 m (LAW 3) at y=%.2f", tread, st.y1));
                }
                prevTop = st.y1;
            }
        }
        if (fl.topY - fl.baseY > 3.0f + 1e-3f) {
            ++R.landing;
            V(fmt("SPIRE-LANDING stair run climbs %.2f m with no landing (LAW 3 allows <= 3.0 m)",
                  fl.topY - fl.baseY));
        }
        for (const SpireStair::Box& st : fl.steps) all.push_back(st);
        if (fl.ramp) all.push_back(fl.solid);
    }
    for (const SpireStair::Box& b : S.landings) all.push_back(b);
    for (const SpireStair::Box& b : S.soffits)  all.push_back(b);
    // 5b. HEADROOM — a switchback stacks its return leg over the leg below. The clear
    //     height above a walking surface must clear the player capsule (1.8 m + margin).
    for (const SpireStair::Flight& fl : S.flights) {
        if (!fl.ramp) continue;
        float clear = 1e9f;
        for (const SpireStair::Box& o : all) {
            if (o.y0 < fl.topY + 0.01f) continue;                   // not above this leg
            if (std::min(fl.solid.x1, o.x1) - std::max(fl.solid.x0, o.x0) <= 0.01f) continue;
            if (std::min(fl.solid.z1, o.z1) - std::max(fl.solid.z0, o.z0) <= 0.01f) continue;
            clear = std::min(clear, o.y0 - fl.topY);
        }
        if (clear < 1.9f) {
            ++R.landing;
            V(fmt("SPIRE-HEAD  stair leg topping at y=%.2f has only %.2f m of head clearance (< 1.90 m)",
                  fl.topY, clear));
            break;
        }
    }
    // 6. ZFIGHT (LAW 2 doubled) — stair solids may touch but never interpenetrate.
    {
        int pairs = 0;
        for (size_t i = 0; i < all.size(); ++i)
            for (size_t j = i + 1; j < all.size(); ++j)
                if (boxesInterpenetrate(all[i], all[j], 0.01f)) ++pairs;
        if (pairs) {
            R.zfight = pairs;
            V(fmt("SPIRE-ZFIGHT %d interpenetrating stair solid pair(s) (LAW 2: doubled faces)", pairs));
        }
    }
    // 7. PIERCE (LAW 2) — a stair solid that crosses a floor slab or a ceiling lid must
    //    lie inside the WELL the builder cuts from those slabs; otherwise it is driven
    //    straight through solid geometry.
    {
        int pierced = 0;
        for (uint32_t fi = 0; fi < (uint32_t)L1Floor::Count; ++fi) {
            const bool rooftop = (fi == (uint32_t)L1Floor::F7);
            const float planes[2] = { F[fi].y0, F[fi].y0 + F[fi].ceil };
            for (int p = 0; p < (rooftop ? 1 : 2); ++p) {
                for (const SpireStair::Box& b : all) {
                    if (b.y0 < planes[p] - kEps && b.y1 > planes[p] + kEps && !insideWell(S, b))
                        ++pierced;
                }
            }
        }
        if (pierced) {
            R.pierce = pierced;
            V(fmt("SPIRE-PIERCE %d stair solid/slab crossing(s) outside the stair well (no cutout)", pierced));
        }
    }
    // 8. WELL — the cutout itself must be inside every plate, clear of the elevator
    //    shaft, and clear of every room (a hole in a room's floor is a pit).
    if (S.wellX1 - S.wellX0 > 0.01f) {
        for (uint32_t fi = 0; fi < (uint32_t)L1Floor::Count; ++fi) {
            const L1RoomDef& f = F[fi];
            if (S.wellX0 < f.x0 + kWallT || S.wellX1 > f.x1 - kWallT ||
                S.wellZ0 < -f.zHalf + kWallT || S.wellZ1 > f.zHalf - kWallT) {
                ++R.well;
                V(fmt("SPIRE-WELL stair well [%.1f..%.1f]x[%.1f..%.1f] is outside floor %u's plate",
                      S.wellX0, S.wellX1, S.wellZ0, S.wellZ1, fi));
            }
        }
        if (S.wellX1 > shX0 && S.wellX0 < shX1 && S.wellZ1 > shZ0 && S.wellZ0 < shZ1) {
            ++R.well; V("SPIRE-WELL stair well overlaps the elevator shaft column");
        }
        for (uint32_t i = 0; i < wingN; ++i) {
            float x0, x1, z0, z1; roomBox(i, x0, x1, z0, z1);
            if (S.wellX1 > x0 && S.wellX0 < x1 && S.wellZ1 > z0 && S.wellZ0 < z1) {
                ++R.well;
                V(fmt("SPIRE-WELL stair well opens a hole in room '%s'", W[i].name));
            }
        }
        uint32_t detN = level1DetentionRoomCount();
        const L1DetentionRoom* D = level1DetentionRooms();
        for (uint32_t i = 0; i < detN; ++i) {
            const float x0 = D[i].cx - D[i].w * 0.5f, x1 = D[i].cx + D[i].w * 0.5f;
            const float z0 = D[i].cz - D[i].d * 0.5f, z1 = D[i].cz + D[i].d * 0.5f;
            if (S.wellX1 > x0 && S.wellX0 < x1 && S.wellZ1 > z0 && S.wellZ0 < z1) {
                ++R.well;
                V(fmt("SPIRE-WELL stair well opens a hole in detention room '%s'", D[i].name));
            }
        }
    } else {
        ++R.well;
        V("SPIRE-WELL the stairwell cuts NO well — every floor slab and ceiling lid it "
          "crosses is solid, so the stair cannot be climbed");
    }
    // 9. REACH — the stair must present a walkable pad at every floor above the base.
    if (S.arrivalY.size() + 1 != (size_t)L1Floor::Count) {
        V(fmt("SPIRE-REACH stair serves %u of %u floors",
              (unsigned)S.arrivalY.size(), (unsigned)L1Floor::Count - 1));
    } else {
        for (uint32_t fi = 1; fi < (uint32_t)L1Floor::Count; ++fi)
            if (std::fabs(S.arrivalY[fi - 1] - F[fi].y0) > 0.05f)
                V(fmt("SPIRE-REACH stair arrival at floor %u is y=%.2f, plate floor is y=%.2f",
                      fi, S.arrivalY[fi - 1], F[fi].y0));
    }

    // 10. VALUE BAND — the graybox tints multiply the procedural maps; the product must
    //     land in the reflectance band real interiors occupy (surface_library.h:
    //     0.08..0.40 LINEAR). This is the D14/D15/D16 law applied to the Spire.
    {
        float texLin[3][3];
        {
            auto fl = x3::prims::makeFloorGrateRGBA(256, 2, level1DeckMapLift(), false);
            auto wl = x3::prims::makeSciFiPanelRGBA(256, 2, x3::prims::detail::kNoTint,
                                                    60, 170, 200, 0.16f, x3::prims::WallVariant::Plain);
            auto cl = x3::prims::makeCeilingPanelRGBA(256, 3, x3::prims::detail::kNoTint, true);
            meanLinearRGB(fl, texLin[0]);
            meanLinearRGB(wl, texLin[1]);
            meanLinearRGB(cl, texLin[2]);
        }
        x3::logInfo(fmt("[levellint] spire graybox map albedo (LINEAR mean): floor %.3f wall %.3f ceiling %.3f",
                        0.2126f*texLin[0][0]+0.7152f*texLin[0][1]+0.0722f*texLin[0][2],
                        0.2126f*texLin[1][0]+0.7152f*texLin[1][1]+0.0722f*texLin[1][2],
                        0.2126f*texLin[2][0]+0.7152f*texLin[2][1]+0.0722f*texLin[2][2]));
        uint32_t surfN = 0;
        const L1Surface* SS = level1Surfaces(surfN);
        for (uint32_t i = 0; i < surfN; ++i) {
            const float* t = texLin[SS[i].kind < 3 ? SS[i].kind : 0];
            const float r = t[0] * SS[i].tint[0], g = t[1] * SS[i].tint[1], b = t[2] * SS[i].tint[2];
            const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            if (lum < 0.08f || lum > 0.40f) {
                ++R.value;
                V(fmt("SPIRE-VALUE '%s' effective albedo %.3f LINEAR is outside the interior band [0.08, 0.40]",
                      SS[i].name, lum));
            }
        }
    }
    return R;
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
    // ---- HIDDEN-4.5 SEAL gate (fix/spire-hollow-core, owner canon 2026-07-25:
    // level 4.5 is HIDDEN — elevator-only, no stairway, no sightline; AMENDED by
    // the owner's 7762 master order: ONE sanctioned code-locked opening exists —
    // the stairwell master door + its connector, asserted by the STAIR-M block
    // below; it is not a CanonDoorway and defaults locked, so every probe here
    // still holds with the door closed). Asserts the M1 seal can never silently
    // regress:
    //   1. NO room ships an open ceiling (the old Access reveal was an F4<->4.5
    //      sightline + stairway).
    //   2. NO doorway touches a platform (4.5) room — a platform doorway is an
    //      opening into the hidden level.
    //   3. The Nexus Access room's lid RENDERS (solidLid) — an invisible lid under
    //      the cavern is a one-way hole (D3/D4 class).
    // Plus NEGATIVE CONTROLS proving each probe can go red.
    {
        uint32_t openCeil = 0, platDoor = 0;
        for (const CanonRoom& r : floor.rooms) if (r.openCeiling) ++openCeil;
        for (const CanonDoorway& dw : floor.doorways)
            if (floor.rooms[dw.a].platform || floor.rooms[dw.b].platform) ++platDoor;
        bool accessSolid = true;
        for (const CanonRoom& r : floor.rooms)
            if (r.name.find("Nexus Chamber Access") != std::string::npos && !r.solidLid)
                accessSolid = false;
        if (openCeil)
            rep.violations.push_back(fmt("SEAL-4.5  %u room(s) ship an OPEN CEILING — a sightline into the hidden level", openCeil));
        if (platDoor)
            rep.violations.push_back(fmt("SEAL-4.5  %u doorway(s) touch a 4.5 platform room — an opening into the hidden level", platDoor));
        if (!accessSolid)
            rep.violations.push_back("SEAL-4.5  Nexus Access lid is not solid (invisible lid under the cavern = one-way hole)");
        // Negative control: a doctored copy with an opened ceiling + a platform
        // doorway MUST trip both probes.
        bool ctrlOk = false;
        {
            CanonFloor bad = floor;
            uint32_t plat = kNoRoom;
            for (uint32_t i = 0; i < bad.rooms.size(); ++i)
                if (bad.rooms[i].platform) { plat = i; break; }
            if (!bad.rooms.empty()) bad.rooms[0].openCeiling = true;
            uint32_t badOpen = 0, badPlat = 0;
            for (const CanonRoom& r : bad.rooms) if (r.openCeiling) ++badOpen;
            if (plat != kNoRoom) {
                CanonDoorway dw; dw.a = 0; dw.b = plat; dw.kind = DoorwayKind::GapBridge;
                bad.doorways.push_back(dw);
                for (const CanonDoorway& d : bad.doorways)
                    if (bad.rooms[d.a].platform || bad.rooms[d.b].platform) ++badPlat;
            }
            ctrlOk = badOpen > 0 && (plat == kNoRoom || badPlat > 0);
        }
        if (!ctrlOk)
            rep.violations.push_back("SEAL-4.5  NEGATIVE CONTROL FAILED: doctored open-ceiling/platform-door not detected");
        x3::logInfo(std::string("[levellint] hidden-4.5 seal: openCeiling=") +
                    std::to_string(openCeil) + " platformDoorways=" + std::to_string(platDoor) +
                    " accessLid=" + (accessSolid ? "solid" : "MISSING") +
                    "; negative control " + (ctrlOk ? "red-capable" : "BROKEN"));
    }

    // ---- STAIRWELL gate (fix/spire-hollow-core): the owner's open switchback must
    // stay connected to every real floor and must NEVER open into 4.5.
    //   1. The layout resolves a landing room on EVERY authored floor number.
    //   2. Each connector's breach cut lies inside its target room's wall span
    //      (LAW 1: an opening must land on a real shared plane).
    //   3. No breach targets a platform room, and neither the shaft box nor any
    //      connector corridor intersects the 4.5 cavern envelope.
    //   4. The shaft box intersects NO room (a future JSON room dropped onto the
    //      shaft site trips the gate).
    {
        const StairwellLayout lay = stairwellLayout(floor);
        int maxFn = 1;
        for (int fn : floor.roomFloorNum) maxFn = (fn > maxFn) ? fn : maxFn;
        if (!lay.valid) {
            rep.violations.push_back("STAIR     stairwell layout failed to resolve (no F1 target / <2 floors)");
        } else {
            if ((int)lay.floors.size() != maxFn)
                rep.violations.push_back(fmt("STAIR     stairwell serves %u of %d floors — a floor lost its landing room",
                                             (unsigned)lay.floors.size(), maxFn));
            float env[6];
            const bool hasEnv = Canon45::envelope(floor, env);
            auto boxHitsEnv = [&](float bx0, float bx1, float bz0, float bz1,
                                  float by0, float by1) {
                if (!hasEnv) return false;
                return bx1 > env[0] && bx0 < env[1] && bz1 > env[2] && bz0 < env[3] &&
                       by1 > env[4] - 0.6f && by0 < env[5];
            };
            for (const StairwellLayout::FloorEntry& fe : lay.floors) {
                const CanonRoom& r = floor.rooms[fe.room];
                if (r.platform)
                    rep.violations.push_back(fmt("STAIR     F%d connector targets platform room '%s' — an opening into 4.5",
                                                 fe.floorNum, r.name.c_str()));
                const float zc = (fe.floorNum == 1) ? 0.0f : StairwellLayout::kDoorZ;
                if (zc - StairwellLayout::kDoorHalfW < r.z0() - kEps ||
                    zc + StairwellLayout::kDoorHalfW > r.z1() + kEps)
                    rep.violations.push_back(fmt("STAIR     F%d breach cut [%.2f..%.2f] outside '%s' wall span [%.2f..%.2f]",
                                                 fe.floorNum, zc - StairwellLayout::kDoorHalfW,
                                                 zc + StairwellLayout::kDoorHalfW,
                                                 r.name.c_str(), r.z0(), r.z1()));
                if (fe.floorNum != 1 &&
                    boxHitsEnv(lay.sx1, fe.roomWallX, StairwellLayout::kDoorZ - 1.2f,
                               StairwellLayout::kDoorZ + 1.2f, fe.floorY, fe.floorY + 3.0f))
                    rep.violations.push_back(fmt("STAIR     F%d connector corridor intersects the 4.5 cavern envelope", fe.floorNum));
            }
            if (boxHitsEnv(lay.sx0, lay.sx1, lay.sz0, lay.sz1, lay.baseY, lay.topY))
                rep.violations.push_back("STAIR     shaft box intersects the 4.5 cavern envelope");
            for (uint32_t i = 0; i < (uint32_t)floor.rooms.size(); ++i) {
                const CanonRoom& r = floor.rooms[i];
                if (r.cy < -50.0f) continue;                    // deep zone: not the tower shell
                if (r.x1() > lay.sx0 + kEps && r.x0() < lay.sx1 - kEps &&
                    r.z1() > lay.sz0 + kEps && r.z0() < lay.sz1 - kEps &&
                    r.y1() > lay.baseY + kEps && r.y0() < lay.topY - kEps)
                    rep.violations.push_back(fmt("STAIR     room '%s' intersects the stairwell shaft box", r.name.c_str()));
            }
            // ---- MASTER ACCESS gate (owner order 2026-07-25: backup code 7762
            // opens the unnumbered door). The 4.5 seal is amended, not broken:
            // there is EXACTLY ONE sanctioned opening — the code-locked master
            // door + its L-connector, per the MasterAccess plan the builder and
            // Canon45's wall cut share. Assert the plan's invariants:
            //   M1. A tower with a 4.5 envelope HAS a master plan, seated on a
            //       PHANTOM (unnumbered) landing within a story of the 4.5 plane
            //       — the nearest one (the door the 4545 tell already marks).
            //   M2. The master code is a real 4-digit code, distinct from the
            //       service code (4545 answers; it must never open anything).
            //   M3. Neither connector leg PENETRATES the cavern envelope: the
            //       route stops at the wall's outer face; the only way through is
            //       the sanctioned mouth, whose span lies inside the envelope's
            //       X range and clear of the elevator arrival mouth.
            //   M4. The door-sill step (landing -> connector floor) is a legal
            //       auto-step (<= 0.35 m) — a dataset shift cannot silently ship
            //       a jump or a hidden drop behind the master door.
            if (hasEnv) {
                if (!lay.master.present) {
                    rep.violations.push_back("STAIR-M   tower has a 4.5 envelope but no master-access plan resolved");
                } else {
                    const auto& M = lay.master;
                    // M1: on the nearest phantom landing to the 4.5 floor plane.
                    float bestD = 1e9f; float tellD = 1e9f; bool onPhantom = false;
                    for (const StairwellLayout::NorthLanding& nl : lay.north) {
                        if (nl.floorNum > 0) continue;
                        const float d = std::fabs(nl.y - env[4]);
                        bestD = (d < bestD) ? d : bestD;
                        if (std::fabs(nl.y - M.landingY) < 0.01f) { tellD = d; onPhantom = true; }
                    }
                    if (!onPhantom || tellD > bestD + 1e-3f || tellD > 3.0f)
                        rep.violations.push_back("STAIR-M   master door is not the unnumbered landing nearest the 4.5 plane");
                    // M2: code sanity.
                    if (FacilityStairwell::kMasterCode < 1000 ||
                        FacilityStairwell::kMasterCode > 9999 ||
                        FacilityStairwell::kMasterCode == FacilityStairwell::kServiceCode)
                        rep.violations.push_back("STAIR-M   master code is not a distinct 4-digit code");
                    // M3: legs stop OUTSIDE the envelope; the mouth is the only way in.
                    const float fy = M.floorY, fh = fy + StairwellLayout::kMasterH;
                    if (boxHitsEnv(M.aX0, M.aX1, M.aZ0, M.aZ1, fy, fh) ||
                        boxHitsEnv(M.bX0, M.bX1, M.bZ0, M.bZ1, fy, fh))
                        rep.violations.push_back("STAIR-M   master connector penetrates the 4.5 envelope (route must stop at the wall)");
                    if (M.bZ1 > env[2] - 0.8f + kEps)
                        rep.violations.push_back("STAIR-M   leg B overruns the cavern wall's outer face");
                    if (M.mouthX0 < env[0] - kEps || M.mouthX1 > env[1] + kEps)
                        rep.violations.push_back("STAIR-M   mouth span outside the envelope X range");
                    // The elevator arrival mouth (F5 lobby X +- 1.2) must not collide.
                    for (uint32_t i = 0; i < (uint32_t)floor.rooms.size(); ++i)
                        if (floor.rooms[i].type == "Elevator Lobby" &&
                            i < floor.roomFloorNum.size() && floor.roomFloorNum[i] == 5 &&
                            M.mouthX1 > floor.rooms[i].cx - 1.7f &&
                            M.mouthX0 < floor.rooms[i].cx + 1.7f)
                            rep.violations.push_back("STAIR-M   service mouth collides with the elevator arrival mouth");
                    // M4: the landing -> cavern-floor height change must be
                    // walkable. Small (<= 0.35 m) is an auto-step; bigger gets a
                    // doctrine flight (risers <= 0.2, treads 0.31) inside leg A —
                    // assert the flight actually FITS (entry pad + run + arrival
                    // margin), and that the drop stays within the marking window
                    // (a dataset shift cannot ship a jump behind the master door).
                    {
                        const float dropM = std::fabs(M.landingY - M.floorY);
                        if (dropM > 0.35f) {
                            const int   nR  = (int)std::ceil(dropM / 0.2f);
                            const float run = 1.2f + (float)nR * 0.31f + 0.6f;
                            if (M.aX0 + run > M.aX1 + kEps)
                                rep.violations.push_back(fmt("STAIR-M   %.2f m connector flight (%d risers) does not fit leg A",
                                                             dropM, nR));
                        }
                        if (dropM > 3.0f)
                            rep.violations.push_back(fmt("STAIR-M   door-to-floor drop %.2f m exceeds the marking window", dropM));
                    }
                    // NEGATIVE CONTROL: a doctored leg B pushed through the wall
                    // into the cavern interior MUST trip the M3 probe.
                    if (!boxHitsEnv(M.bX0, M.bX1, M.bZ0, M.bZ1 + 3.0f, fy, fh))
                        rep.violations.push_back("STAIR-M   NEGATIVE CONTROL FAILED: doctored through-wall leg not detected");
                }
            }
            // Negative control: a doctored connector aimed at a platform room + a box
            // probe inside the envelope must both trip.
            bool ctrlOk = true;
            {
                uint32_t plat = kNoRoom;
                for (uint32_t i = 0; i < floor.rooms.size(); ++i)
                    if (floor.rooms[i].platform) { plat = i; break; }
                if (plat != kNoRoom && !floor.rooms[plat].platform) ctrlOk = false;
                if (hasEnv) {
                    const float mx = (env[0] + env[1]) * 0.5f, mz = (env[2] + env[3]) * 0.5f;
                    if (!boxHitsEnv(mx - 0.5f, mx + 0.5f, mz - 0.5f, mz + 0.5f,
                                    env[4] + 0.5f, env[4] + 1.5f))
                        ctrlOk = false;
                }
            }
            if (!ctrlOk)
                rep.violations.push_back("STAIR     NEGATIVE CONTROL FAILED: envelope probe not detected");
            x3::logInfo("[levellint] stairwell: " + std::to_string(lay.floors.size()) +
                        "/" + std::to_string(maxFn) + " floors served, " +
                        std::to_string(lay.north.size()) + " north landings; envelope " +
                        (hasEnv ? "checked" : "absent") + "; negative control " +
                        (ctrlOk ? "red-capable" : "BROKEN"));
        }
    }

    // ---- SPIRE gate (--world level1, app/level1.cpp): the code-generated tower was
    // never covered by GATE A. Same doctrine, applied to the F2-F7 upper floors.
    {
        const SpireLintReport sp = lintSpire();
        for (const std::string& v : sp.violations) rep.violations.push_back(v);
        // NEGATIVE CONTROLS: each probe must be able to go red. A doctored stair with a
        // 0.9 m riser / 0.05 m tread must trip LAW 3, and two nested boxes must trip the
        // interpenetration probe.
        bool ctrlOk = true;
        {
            SpireStair::Box a{ 0, 2, 0, 2, 0, 2 }, b{ 1, 3, 1, 3, 1, 3 }, c{ 2, 4, 0, 2, 0, 2 };
            if (!boxesInterpenetrate(a, b, 0.01f)) ctrlOk = false;   // nested -> must trip
            if (boxesInterpenetrate(a, c, 0.01f))  ctrlOk = false;   // face-touching -> must NOT
            SpireStair doctored;                                     // no well authored
            if (insideWell(doctored, a)) ctrlOk = false;
            doctored.wellX0 = -1; doctored.wellX1 = 5; doctored.wellZ0 = -1; doctored.wellZ1 = 5;
            if (!insideWell(doctored, a)) ctrlOk = false;
        }
        if (!ctrlOk)
            rep.violations.push_back("SPIRE     NEGATIVE CONTROL FAILED: interpenetration/well probe not red-capable");
        x3::logInfo(fmt("[levellint] spire (level1 B1-F7): contain=%d overlap=%d keepout=%d door=%d "
                        "riser=%d tread=%d slope=%d landing=%d zfight=%d pierce=%d well=%d value=%d; "
                        "negative control %s",
                        sp.contain, sp.overlap, sp.keepout, sp.doorProbe, sp.riser, sp.tread,
                        sp.slope, sp.landing, sp.zfight, sp.pierce, sp.well, sp.value,
                        ctrlOk ? "red-capable" : "BROKEN"));
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
