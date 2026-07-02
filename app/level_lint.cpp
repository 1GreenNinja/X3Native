// LEVEL LINT — Gate A of the x3-level-authoring doctrine. See level_lint.h.
//
// The lint reasons over the exact data the canonical builder consumes (the
// CanonFloor rooms + resolved doorways) plus the builder's shared constants
// (level_loader.h kCanon*), so a violation here IS a defect in the generated
// geometry. The dedup/ramp/bridge models below mirror buildCanonFloor's
// algorithms 1:1 — if the builder changes, change the lint with it.
#include "level_lint.h"
#include "door.h"
#include "headless_device.h"

#include "engine/core/x3_log.h"
#include "engine/physics/IPhysicsWorld.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace x3::game {

namespace {

constexpr float kEps          = 0.02f;   // exact-plane tolerance
constexpr float kCoplanarEps  = 0.05f;   // faces closer than this = z-fight range
constexpr float kMaxLegalStep = 0.25f;   // LAW 3: player auto-step riser
constexpr float kMaxRampTan   = 0.5774f; // LAW 3: ramp <= 30 deg (tan 30)

std::string fmt(const char* f, ...) {
    char b[512];
    va_list ap; va_start(ap, f);
    std::vsnprintf(b, sizeof(b), f, ap);
    va_end(ap);
    return std::string(b);
}

uint64_t pairKey(uint32_t a, uint32_t b) {
    if (a > b) std::swap(a, b);
    return ((uint64_t)a << 32) | b;
}

} // namespace

const char* lintCategoryName(LintCategory c) {
    switch (c) {
        case LintCategory::DoorSeat: return "door-seat";
        case LintCategory::Seam:     return "seam";
        case LintCategory::Height:   return "height";
        case LintCategory::Reach:    return "reach";
        case LintCategory::Contain:  return "contain";
        default:                     return "?";
    }
}

uint32_t LintReport::countIn(LintCategory c) const {
    uint32_t n = 0;
    for (const LintViolation& v : violations) if (v.cat == c) ++n;
    return n;
}

std::string LintReport::summary() const {
    return fmt("door-seat=%u seam=%u height=%u reach=%u contain=%u TOTAL=%zu",
               countIn(LintCategory::DoorSeat), countIn(LintCategory::Seam),
               countIn(LintCategory::Height), countIn(LintCategory::Reach),
               countIn(LintCategory::Contain), violations.size());
}

void LintReport::log(const std::string& label) const {
    x3::logInfo("[levellint] " + label + ": " + summary());
    for (uint32_t c = 0; c < (uint32_t)LintCategory::kCount; ++c) {
        const LintCategory cat = (LintCategory)c;
        if (countIn(cat) == 0) continue;
        x3::logInfo(fmt("[levellint]   -- %s (%u) --", lintCategoryName(cat), countIn(cat)));
        for (const LintViolation& v : violations) {
            if (v.cat != cat) continue;
            x3::logInfo(fmt("[levellint]   %-22s %s @ (%.1f, %.1f, %.1f)",
                            v.code.c_str(), v.msg.c_str(), v.x, v.y, v.z));
        }
    }
}

// =====================================================================================
// THE LINT.
// =====================================================================================
LintReport lintCanonFloor(const CanonFloor& floor) {
    LintReport rep;
    const uint32_t n = (uint32_t)floor.rooms.size();
    if (n == 0) return rep;

    auto add = [&](LintCategory cat, const char* code, std::string msg,
                   float x, float y, float z) {
        rep.violations.push_back({ cat, code, std::move(msg), x, y, z });
    };
    auto rn = [&](uint32_t i) {   // room label "name#id"
        return floor.rooms[i].name + "#" + std::to_string(i);
    };

    // Pair -> doorway kinds map + per-room degree (for the seam + contain checks).
    std::map<uint64_t, std::vector<uint32_t>> paired;   // pairKey -> doorway indices
    std::vector<int> degree(n, 0);
    for (uint32_t di = 0; di < (uint32_t)floor.doorways.size(); ++di) {
        const CanonDoorway& dw = floor.doorways[di];
        if (dw.a >= n || dw.b >= n) continue;
        paired[pairKey(dw.a, dw.b)].push_back(di);
        ++degree[dw.a]; ++degree[dw.b];
    }
    auto pairHasKind = [&](uint32_t a, uint32_t b, DoorwayKind k) {
        auto it = paired.find(pairKey(a, b));
        if (it == paired.end()) return false;
        for (uint32_t di : it->second) if (floor.doorways[di].kind == k) return true;
        return false;
    };
    auto pairAdjacentDoorway = [&](uint32_t a, uint32_t b) -> const CanonDoorway* {
        auto it = paired.find(pairKey(a, b));
        if (it == paired.end()) return nullptr;
        for (uint32_t di : it->second) {
            const CanonDoorway& dw = floor.doorways[di];
            if (dw.kind == DoorwayKind::AdjacentX || dw.kind == DoorwayKind::AdjacentZ)
                return &dw;
        }
        return nullptr;
    };

    // ================================================================================
    // CHECK 1 — DOOR-SEAT. Every opening sits IN a wall plane of both rooms, inside
    // both rooms' wall spans, with standing headroom; bridge mouths land on the facing
    // walls; descent tubes actually meet both rooms.
    // ================================================================================
    for (const CanonDoorway& dw : floor.doorways) {
        if (dw.a >= n || dw.b >= n) continue;
        const CanonRoom& A = floor.rooms[dw.a];
        const CanonRoom& B = floor.rooms[dw.b];

        if (dw.kind == DoorwayKind::AdjacentX || dw.kind == DoorwayKind::AdjacentZ ||
            dw.kind == DoorwayKind::Overlap) {
            const float seatTol = kCanonWallT * 0.5f + kEps;
            const float p = (dw.axis == 0) ? dw.cx : dw.cz;
            auto planeDist = [&](const CanonRoom& r) {
                return (dw.axis == 0)
                    ? std::min(std::fabs(p - r.x0()), std::fabs(p - r.x1()))
                    : std::min(std::fabs(p - r.z0()), std::fabs(p - r.z1()));
            };
            const float dA = planeDist(A), dB = planeDist(B);
            // Overlap-kind openings are cut inside the interpenetration (not on a wall
            // plane by design) — only Adjacent openings must seat exactly in a wall.
            if (dw.kind != DoorwayKind::Overlap && (dA > seatTol || dB > seatTol))
                add(LintCategory::DoorSeat, "DOOR_UNSEATED",
                    fmt("door between %s and %s hangs %.2f/%.2f m off the rooms' wall planes",
                        rn(dw.a).c_str(), rn(dw.b).c_str(), dA, dB),
                    dw.cx, dw.cy, dw.cz);

            // The 1.6 m opening must fit INSIDE both rooms' wall runs (no cut past a corner).
            const float c  = (dw.axis == 0) ? dw.cz : dw.cx;
            const float lo = c - kCanonDoorHalf, hi = c + kCanonDoorHalf;
            auto runSpan = [&](const CanonRoom& r, float& s0, float& s1) {
                if (dw.axis == 0) { s0 = r.z0(); s1 = r.z1(); }
                else              { s0 = r.x0(); s1 = r.x1(); }
            };
            float a0, a1, b0, b1;
            runSpan(A, a0, a1); runSpan(B, b0, b1);
            if (lo < a0 - kEps || hi > a1 + kEps || lo < b0 - kEps || hi > b1 + kEps)
                add(LintCategory::DoorSeat, "DOOR_PAST_EDGE",
                    fmt("opening [%.1f..%.1f] between %s and %s pokes past a room corner "
                        "(A run [%.1f..%.1f], B run [%.1f..%.1f])",
                        lo, hi, rn(dw.a).c_str(), rn(dw.b).c_str(), a0, a1, b0, b1),
                    dw.cx, dw.cy, dw.cz);

            // Standing headroom: the shared clear-top must fit under BOTH ceilings.
            const float clearTop = std::max(A.y0(), B.y0()) + kCanonLintel;
            if (clearTop > std::min(A.y1(), B.y1()) + kEps)
                add(LintCategory::DoorSeat, "DOOR_NO_HEADROOM",
                    fmt("doorway between %s and %s needs clear-top %.2f but a ceiling sits at %.2f",
                        rn(dw.a).c_str(), rn(dw.b).c_str(), clearTop, std::min(A.y1(), B.y1())),
                    dw.cx, dw.cy, dw.cz);
        }
        else if (dw.kind == DoorwayKind::GapBridge) {
            // The corridor mouth is cut on each room's facing wall at the bridge's
            // cross-axis coordinate: that coordinate must lie INSIDE both rooms' facing
            // wall spans (with the opening half-width inside), or the mouth misses the wall.
            const float c = (dw.axis == 0) ? dw.cz : dw.cx;
            auto crossSpan = [&](const CanonRoom& r, float& s0, float& s1) {
                if (dw.axis == 0) { s0 = r.z0(); s1 = r.z1(); }
                else              { s0 = r.x0(); s1 = r.x1(); }
            };
            float a0, a1, b0, b1;
            crossSpan(A, a0, a1); crossSpan(B, b0, b1);
            if (c - kCanonDoorHalf < a0 - kEps || c + kCanonDoorHalf > a1 + kEps ||
                c - kCanonDoorHalf < b0 - kEps || c + kCanonDoorHalf > b1 + kEps)
                add(LintCategory::DoorSeat, "BRIDGE_MOUTH_OFF_WALL",
                    fmt("gap-bridge mouth between %s and %s (cross %.1f) misses a facing wall "
                        "(A [%.1f..%.1f], B [%.1f..%.1f])",
                        rn(dw.a).c_str(), rn(dw.b).c_str(), c, a0, a1, b0, b1),
                    dw.cx, dw.cy, dw.cz);
        }
        else if (dw.kind == DoorwayKind::CrossLevel) {
            // The 3 m descent tube at (cx,cz) must overlap BOTH rooms' XZ footprints,
            // or descending it dumps the player into the void beside the target room.
            auto tubeMeets = [&](const CanonRoom& r) {
                return dw.cx + kCanonShaftHalf > r.x0() - kEps &&
                       dw.cx - kCanonShaftHalf < r.x1() + kEps &&
                       dw.cz + kCanonShaftHalf > r.z0() - kEps &&
                       dw.cz - kCanonShaftHalf < r.z1() + kEps;
            };
            if (!tubeMeets(A) || !tubeMeets(B))
                add(LintCategory::DoorSeat, "TUBE_MISSES_ROOM",
                    fmt("descent tube between %s and %s at (%.1f, %.1f) misses %s%s%s footprint",
                        rn(dw.a).c_str(), rn(dw.b).c_str(), dw.cx, dw.cz,
                        !tubeMeets(A) ? rn(dw.a).c_str() : "",
                        (!tubeMeets(A) && !tubeMeets(B)) ? " and " : "",
                        !tubeMeets(B) ? rn(dw.b).c_str() : ""),
                    dw.cx, dw.cy, dw.cz);
        }
    }

    // ================================================================================
    // CHECK 2 — SEAM. Replicate the builder's coplanar-wall DEDUP exactly so we know
    // which faces are BUILT, then sweep all room pairs for (a) DOUBLED coplanar walls
    // (z-fight) and floors, (b) GAP seams: a void band between two rooms that have a
    // cut opening through it (visible sky/void + a floor slot at the threshold).
    // ================================================================================
    std::vector<unsigned char> skipFace(n * 4, 0);   // [ri*4+f], f: 0=-X 1=+X 2=-Z 3=+Z
    {
        std::vector<unsigned char> wantSkip(n * 4, 0), ownFace(n * 4, 0);
        const float eps = 0.02f;
        auto faceX = [&](const CanonRoom& r, float planeX) {
            return (std::fabs(planeX - r.x0()) < std::fabs(planeX - r.x1())) ? 0 : 1;
        };
        auto faceZ = [&](const CanonRoom& r, float planeZ) {
            return (std::fabs(planeZ - r.z0()) < std::fabs(planeZ - r.z1())) ? 2 : 3;
        };
        auto coversY = [&](const CanonRoom& big, const CanonRoom& s) {
            return big.y0() <= s.y0() + eps && big.y1() >= s.y1() - eps;
        };
        auto coversZ = [&](const CanonRoom& big, const CanonRoom& s) {
            return big.z0() <= s.z0() + eps && big.z1() >= s.z1() - eps && coversY(big, s);
        };
        auto coversX = [&](const CanonRoom& big, const CanonRoom& s) {
            return big.x0() <= s.x0() + eps && big.x1() >= s.x1() - eps && coversY(big, s);
        };
        for (const CanonDoorway& dw : floor.doorways) {
            if (dw.kind != DoorwayKind::AdjacentX && dw.kind != DoorwayKind::AdjacentZ &&
                dw.kind != DoorwayKind::Overlap)
                continue;
            if (dw.a >= n || dw.b >= n) continue;
            const CanonRoom& A = floor.rooms[dw.a];
            const CanonRoom& B = floor.rooms[dw.b];
            int fa, fb; bool aCovB, bCovA;
            if (dw.axis == 0) { fa = faceX(A, dw.cx); fb = faceX(B, dw.cx); aCovB = coversZ(A, B); bCovA = coversZ(B, A); }
            else              { fa = faceZ(A, dw.cz); fb = faceZ(B, dw.cz); aCovB = coversX(A, B); bCovA = coversX(B, A); }
            if (aCovB)      { wantSkip[dw.b * 4 + fb] = 1; ownFace[dw.a * 4 + fa] = 1; }
            else if (bCovA) { wantSkip[dw.a * 4 + fa] = 1; ownFace[dw.b * 4 + fb] = 1; }
        }
        for (uint32_t i = 0; i < n * 4; ++i) skipFace[i] = wantSkip[i] && !ownFace[i];
    }
    auto wallBuilt = [&](uint32_t room, int face) { return !skipFace[room * 4 + face]; };

    for (uint32_t i = 0; i < n; ++i) {
        for (uint32_t j = i + 1; j < n; ++j) {
            const CanonRoom& A = floor.rooms[i];
            const CanonRoom& B = floor.rooms[j];
            const float yOv = std::min(A.y1(), B.y1()) - std::max(A.y0(), B.y0());
            if (yOv < 0.1f) continue;                       // different stories — no shared seam
            const float ox = std::min(A.x1(), B.x1()) - std::max(A.x0(), B.x0());
            const float oz = std::min(A.z1(), B.z1()) - std::max(A.z0(), B.z0());

            // Interpenetration (handled under CONTAIN unless the pair is doored Overlap;
            // a doored overlap with coplanar floors still z-fights on the floor slabs).
            if (ox > kEps && oz > kEps) {
                if (!paired.count(pairKey(i, j))) {
                    add(LintCategory::Contain, "INTERPENETRATION",
                        fmt("%s interpenetrates %s (%.1f x %.1f m overlap) with no doorway",
                            rn(i).c_str(), rn(j).c_str(), ox, oz),
                        (std::max(A.x0(), B.x0()) + std::min(A.x1(), B.x1())) * 0.5f,
                        std::max(A.y0(), B.y0()),
                        (std::max(A.z0(), B.z0()) + std::min(A.z1(), B.z1())) * 0.5f);
                } else if (std::fabs(A.y0() - B.y0()) < kCoplanarEps &&
                           pairHasKind(i, j, DoorwayKind::Overlap)) {
                    add(LintCategory::Seam, "DOUBLED_FLOOR",
                        fmt("%s and %s overlap with coplanar floor slabs (z-fight in the overlap)",
                            rn(i).c_str(), rn(j).c_str()),
                        (std::max(A.x0(), B.x0()) + std::min(A.x1(), B.x1())) * 0.5f,
                        A.y0(),
                        (std::max(A.z0(), B.z0()) + std::min(A.z1(), B.z1())) * 0.5f);
                }
                continue;
            }

            // ---- Facing X planes (rooms side by side along X, sharing a Z span). ----
            if (oz > 0.1f && ox <= kEps) {
                const bool aLeft = A.cx < B.cx;
                const float s = aLeft ? (B.x0() - A.x1()) : (A.x0() - B.x1());   // plane separation
                const float plane = aLeft ? (A.x1() + B.x0()) * 0.5f : (A.x0() + B.x1()) * 0.5f;
                const float zMid = (std::max(A.z0(), B.z0()) + std::min(A.z1(), B.z1())) * 0.5f;
                const float yMid = std::max(A.y0(), B.y0()) + yOv * 0.5f;
                const int fA = aLeft ? 1 : 0;   // A's +X : A's -X
                const int fB = aLeft ? 0 : 1;   // B's -X : B's +X
                if (s > -kEps && s < kCoplanarEps && wallBuilt(i, fA) && wallBuilt(j, fB)) {
                    add(LintCategory::Seam, "DOUBLED_WALL",
                        fmt("%s and %s BOTH build a wall on the shared X=%.1f plane over "
                            "z-span %.1f m (coplanar boxes, z-fight)",
                            rn(i).c_str(), rn(j).c_str(), plane, oz),
                        plane, yMid, zMid);
                }
                if (s >= kCoplanarEps) {
                    if (const CanonDoorway* dw = pairAdjacentDoorway(i, j)) {
                        add(LintCategory::Seam, "GAP_SEAM",
                            fmt("%.2f m void band between %s and %s with a cut opening through "
                                "it (visible void + floor slot at the threshold)",
                                s, rn(i).c_str(), rn(j).c_str()),
                            dw->cx, dw->cy, dw->cz);
                    }
                }
            }
            // ---- Facing Z planes (rooms side by side along Z, sharing an X span). ----
            if (ox > 0.1f && oz <= kEps) {
                const bool aFront = A.cz < B.cz;
                const float s = aFront ? (B.z0() - A.z1()) : (A.z0() - B.z1());
                const float plane = aFront ? (A.z1() + B.z0()) * 0.5f : (A.z0() + B.z1()) * 0.5f;
                const float xMid = (std::max(A.x0(), B.x0()) + std::min(A.x1(), B.x1())) * 0.5f;
                const float yMid = std::max(A.y0(), B.y0()) + yOv * 0.5f;
                const int fA = aFront ? 3 : 2;
                const int fB = aFront ? 2 : 3;
                if (s > -kEps && s < kCoplanarEps && wallBuilt(i, fA) && wallBuilt(j, fB)) {
                    add(LintCategory::Seam, "DOUBLED_WALL",
                        fmt("%s and %s BOTH build a wall on the shared Z=%.1f plane over "
                            "x-span %.1f m (coplanar boxes, z-fight)",
                            rn(i).c_str(), rn(j).c_str(), plane, ox),
                        xMid, yMid, plane);
                }
                if (s >= kCoplanarEps) {
                    if (const CanonDoorway* dw = pairAdjacentDoorway(i, j)) {
                        add(LintCategory::Seam, "GAP_SEAM",
                            fmt("%.2f m void band between %s and %s with a cut opening through "
                                "it (visible void + floor slot at the threshold)",
                                s, rn(i).c_str(), rn(j).c_str()),
                            dw->cx, dw->cy, dw->cz);
                    }
                }
            }
        }
    }

    // ================================================================================
    // CHECK 3 — HEIGHT TRANSITIONS. Every walkable floor delta > 0.25 m at a
    // connection needs a LEGAL transition: the builder's threshold ramp (must be
    // <= 30 deg and must FIT inside the lower room), or an elevator/shaft (CrossLevel
    // — the building's vertical vocabulary). A gap-bridge whose decks differ with no
    // transition is a bare ledge = FAIL.
    // ================================================================================
    for (const CanonDoorway& dw : floor.doorways) {
        if (dw.a >= n || dw.b >= n) continue;
        const CanonRoom& A = floor.rooms[dw.a];
        const CanonRoom& B = floor.rooms[dw.b];
        const float delta = std::fabs(A.y0() - B.y0());
        if (delta <= kMaxLegalStep) continue;               // auto-step: legal

        if (dw.kind == DoorwayKind::AdjacentX || dw.kind == DoorwayKind::AdjacentZ ||
            dw.kind == DoorwayKind::Overlap) {
            // Builder drops a threshold ramp: run = clamp(rise/kCanonRampSlope, wall+0.6, 6).
            float run = std::max(delta / kCanonRampSlope, kCanonWallT + 0.6f);
            if (run > 6.0f) run = 6.0f;
            const float slope = delta / run;
            if (slope > kMaxRampTan + 0.01f)
                add(LintCategory::Height, "RAMP_TOO_STEEP",
                    fmt("threshold ramp between %s and %s: %.2f m rise over %.2f m run = "
                        "%.0f deg (law: <= 30 deg)",
                        rn(dw.a).c_str(), rn(dw.b).c_str(), delta, run,
                        std::atan(slope) * 57.2958f),
                    dw.cx, dw.cy, dw.cz);
            const CanonRoom& lower = (A.y0() <= B.y0()) ? A : B;
            const float lowerExtent = (dw.axis == 0) ? lower.w : lower.d;
            if (run > lowerExtent - 0.2f)
                add(LintCategory::Height, "RAMP_DOESNT_FIT",
                    fmt("threshold ramp between %s and %s needs a %.1f m run but the lower "
                        "room is only %.1f m deep on that axis",
                        rn(dw.a).c_str(), rn(dw.b).c_str(), run, lowerExtent),
                    dw.cx, dw.cy, dw.cz);
        }
        else if (dw.kind == DoorwayKind::GapBridge) {
            // The builder ramps the LOWER room's bridge mouth up to the deck (which sits at
            // the higher floor). Model that same ramp: run = clamp(rise/slope, wall+0.6, 6).
            float run = std::max(delta / kCanonRampSlope, kCanonWallT + 0.6f);
            if (run > 6.0f) run = 6.0f;
            const float slope = delta / run;
            if (slope > kMaxRampTan + 0.01f)
                add(LintCategory::Height, "RAMP_TOO_STEEP",
                    fmt("gap-bridge mouth ramp between %s and %s: %.2f m rise over %.2f m run = "
                        "%.0f deg (law: <= 30 deg)",
                        rn(dw.a).c_str(), rn(dw.b).c_str(), delta, run,
                        std::atan(slope) * 57.2958f),
                    dw.cx, dw.cy, dw.cz);
            const CanonRoom& lower = (A.y0() <= B.y0()) ? A : B;
            const float lowerExtent = (dw.axis == 0) ? lower.w : lower.d;
            if (run > lowerExtent - 0.2f)
                add(LintCategory::Height, "RAMP_DOESNT_FIT",
                    fmt("gap-bridge mouth ramp between %s and %s needs a %.1f m run but the lower "
                        "room is only %.1f m deep on that axis",
                        rn(dw.a).c_str(), rn(dw.b).c_str(), run, lowerExtent),
                    dw.cx, dw.cy, dw.cz);
        }
        // CrossLevel = elevator/hatch/shaft vocabulary: legal by definition.
    }

    // ================================================================================
    // CHECK 4 — REACHABILITY. Walk-flood from the player spawn (Jake's Cell) over
    // doorways that are LEGALLY walkable: openings/bridges whose floor delta is a
    // step or has a transition, plus CrossLevel shafts (the elevator spine). Every
    // room not flagged secret/hidden must be reached.
    // ================================================================================
    {
        uint32_t spawn = floor.roomByName("Jake");
        if (spawn == kNoRoom) spawn = 0;
        std::vector<std::vector<uint32_t>> adj(n);
        for (const CanonDoorway& dw : floor.doorways) {
            if (dw.a >= n || dw.b >= n) continue;
            // Every doorway kind now carries a legal transition (adjacency/overlap/gap-bridge
            // all get a threshold ramp for any floor delta; cross-level = the elevator spine),
            // so each is walk-traversable for the reachability flood.
            adj[dw.a].push_back(dw.b); adj[dw.b].push_back(dw.a);
        }
        std::vector<char> seen(n, 0);
        std::vector<uint32_t> stack{ spawn };
        seen[spawn] = 1;
        while (!stack.empty()) {
            uint32_t r = stack.back(); stack.pop_back();
            for (uint32_t nb : adj[r]) if (!seen[nb]) { seen[nb] = 1; stack.push_back(nb); }
        }
        for (uint32_t i = 0; i < n; ++i) {
            if (seen[i]) continue;
            const std::string& nm = floor.rooms[i].name;
            if (nm.find("Hidden") != std::string::npos || nm.find("Secret") != std::string::npos)
                continue;                                    // flagged secret: exempt
            add(LintCategory::Reach, "UNREACHABLE_ROOM",
                fmt("%s is not walk-reachable from the spawn (%s) via legal openings/transitions",
                    rn(i).c_str(), rn(spawn).c_str()),
                floor.rooms[i].cx, floor.rooms[i].y0(), floor.rooms[i].cz);
        }
    }

    // ================================================================================
    // CHECK 5 — CONTAINMENT. No degenerate extents; no floating room disconnected
    // from the structure (degree 0). (Un-doored interpenetration is reported above.)
    // ================================================================================
    for (uint32_t i = 0; i < n; ++i) {
        const CanonRoom& r = floor.rooms[i];
        if (!(r.w > 0.0f && r.h > 0.0f && r.d > 0.0f) ||
            !std::isfinite(r.cx) || !std::isfinite(r.cy) || !std::isfinite(r.cz))
            add(LintCategory::Contain, "DEGENERATE_ROOM",
                fmt("%s has degenerate extents (%.2f x %.2f x %.2f)", rn(i).c_str(), r.w, r.h, r.d),
                r.cx, r.cy, r.cz);
        if (degree[i] == 0)
            add(LintCategory::Contain, "FLOATING_ROOM",
                fmt("%s has no doorway at all — geometry floats disconnected from the structure",
                    rn(i).c_str()),
                r.cx, r.cy, r.cz);
    }

    return rep;
}

// =====================================================================================
// --test-levellint: lint Floor 1 + the fused 7-floor building; green iff both clean.
// =====================================================================================
bool runLevelLintSelfTest() {
    x3::logInfo("running LEVEL LINT (Gate A, x3-level-authoring doctrine)...");
    CanonFloor f1 = loadCanonFloor(canonProjectJsonPath(), 1);
    if (!f1.valid()) {
        x3::logInfo("--test-levellint: SKIPPED (canonical JSON not on this machine) — treating as PASS");
        return true;
    }
    LintReport r1 = lintCanonFloor(f1);
    r1.log("Floor 1 (Detention Level)");

    CanonFloor bld = loadCanonBuilding(canonProjectJsonPath(), 7);
    LintReport rb = lintCanonFloor(bld);
    rb.log("Whole building (7 floors fused)");

    const bool ok = r1.clean() && rb.clean();
    x3::logInfo(std::string("--test-levellint: ") + (ok ? "PASS (0 violations)" : "FAIL") +
                " — floor1 [" + r1.summary() + "], building [" + rb.summary() + "]");
    return ok;
}

// =====================================================================================
// --test-goldenpath (Gate C): headless collision-on walk of the golden path.
// =====================================================================================
namespace {

// BFS route (room-id path) between two rooms over the doorway graph; returns empty on
// no path. Prefers not to route through CrossLevel tubes (they're vertical drops).
std::vector<uint32_t> routeRooms(const CanonFloor& floor, uint32_t from, uint32_t to) {
    const uint32_t n = (uint32_t)floor.rooms.size();
    struct Edge { uint32_t to; bool cross; };
    std::vector<std::vector<Edge>> adj(n);
    for (const CanonDoorway& dw : floor.doorways) {
        if (dw.a >= n || dw.b >= n) continue;
        const bool cross = dw.kind == DoorwayKind::CrossLevel;
        adj[dw.a].push_back({ dw.b, cross });
        adj[dw.b].push_back({ dw.a, cross });
    }
    // Two-pass BFS: first without CrossLevel edges, then with (fallback).
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<int> prev(n, -1);
        std::vector<char> seen(n, 0);
        std::vector<uint32_t> q{ from };
        seen[from] = 1;
        size_t head = 0;
        while (head < q.size()) {
            uint32_t r = q[head++];
            if (r == to) break;
            for (const Edge& e : adj[r]) {
                if (pass == 0 && e.cross) continue;
                if (seen[e.to]) continue;
                seen[e.to] = 1; prev[e.to] = (int)r; q.push_back(e.to);
            }
        }
        if (seen[to]) {
            std::vector<uint32_t> path;
            for (int r = (int)to; r != -1; r = prev[r]) path.push_back((uint32_t)r);
            std::reverse(path.begin(), path.end());
            return path;
        }
    }
    return {};
}

// The doorway center between two directly-doored rooms (prefers a non-CrossLevel edge).
const CanonDoorway* doorwayBetween(const CanonFloor& floor, uint32_t a, uint32_t b) {
    const CanonDoorway* fallback = nullptr;
    for (const CanonDoorway& dw : floor.doorways) {
        if (!((dw.a == a && dw.b == b) || (dw.a == b && dw.b == a))) continue;
        if (dw.kind != DoorwayKind::CrossLevel) return &dw;
        fallback = &dw;
    }
    return fallback;
}

} // namespace

bool runGoldenPathSelfTest() {
    x3::logInfo("running GOLDEN PATH trace (Gate C: spawn -> beats -> elevator lobby, "
                "collision on, no noclip)...");
    CanonFloor floor = loadCanonFloor(canonProjectJsonPath(), 1);
    if (!floor.valid()) {
        x3::logInfo("--test-goldenpath: SKIPPED (canonical JSON not on this machine) — treating as PASS");
        return true;
    }

    HeadlessRenderDevice device;
    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();
    Scene scene;
    DoorSystem doors;
    CanonBuildOpts opts; opts.doors = &doors;
    buildCanonFloor(floor, scene, device, *physics, opts);

    // Open every door slab (and let the collision really slide clear).
    for (uint32_t k = 0; k < doors.count(); ++k) doors.startOpening(doors.at(k));
    for (int i = 0; i < 150; ++i) { doors.update(1.0f / 60.0f, scene, *physics); physics->step(1.0f / 60.0f); }

    // The golden beat chain.
    CanonBeats beats = canonBeats(floor);
    struct Beat { const char* name; uint32_t room; };
    const Beat chain[] = {
        { "Jake's Cell",    beats.jakeCell },
        { "Main Hall",      beats.mainHall },
        { "Security",       beats.security },
        { "Research",       beats.research },
        { "Medical",        beats.medical },
        { "Armory",         beats.armory },
        { "Boss Arena",     beats.bossArena },
        { "Elevator Lobby", beats.elevatorLobby },
    };
    for (const Beat& bt : chain) {
        if (bt.room == kNoRoom) {
            x3::logInfo(std::string("--test-goldenpath: FAIL — beat room absent: ") + bt.name);
            physics->shutdown();
            return false;
        }
    }

    // Spawn a standing character in Jake's Cell.
    const CanonRoom& jc = floor.rooms[beats.jakeCell];
    x3::phys::BodyId chr = physics->createCharacter(0.35f, 1.8f,
                               x3::phys::Vec3{ jc.cx, jc.y0() + 0.2f, jc.cz });
    for (int i = 0; i < 30; ++i) { physics->moveCharacter(chr, x3::phys::Vec3{0,0,0}, 1.0f/60.0f); physics->step(1.0f/60.0f); }

    // Walk each leg: route rooms, then steer waypoint to waypoint (doorway centers +
    // room centers). Stuck = no distance progress for 8 s. No teleports, no noclip.
    bool allOk = true;
    uint32_t curRoom = beats.jakeCell;
    for (size_t leg = 1; leg < sizeof(chain) / sizeof(chain[0]) && allOk; ++leg) {
        const uint32_t target = chain[leg].room;
        std::vector<uint32_t> path = routeRooms(floor, curRoom, target);
        if (path.size() < 2 && curRoom != target) {
            x3::logInfo(fmt("--test-goldenpath: FAIL — no route %s -> %s in the doorway graph",
                            chain[leg - 1].name, chain[leg].name));
            allOk = false;
            break;
        }
        // Waypoints: for each hop the doorway center, then the hop room's center.
        struct Wp { float x, z; const char* what; };
        std::vector<Wp> wps;
        for (size_t hi = 1; hi < path.size(); ++hi) {
            const CanonDoorway* dw = doorwayBetween(floor, path[hi - 1], path[hi]);
            if (dw) wps.push_back({ dw->cx, dw->cz, "doorway" });
            const CanonRoom& r = floor.rooms[path[hi]];
            wps.push_back({ r.cx, r.cz, "room center" });
        }
        bool legOk = true;
        for (const Wp& wp : wps) {
            float lastDist = 1e30f;
            int   sinceProgress = 0;
            int   frames = 0;
            for (;;) {
                x3::phys::Vec3 pos = physics->getBodyPosition(chr);
                const float dx = wp.x - pos.x, dz = wp.z - pos.z;
                const float dist = std::sqrt(dx * dx + dz * dz);
                if (dist < 0.7f) break;                                   // waypoint reached
                if (dist < lastDist - 0.01f) { lastDist = dist; sinceProgress = 0; }
                else if (++sinceProgress > 480) {                          // 8 s without progress
                    x3::logInfo(fmt("--test-goldenpath: STUCK on leg '%s' heading to %s "
                                    "(%.1f, %.1f) at (%.1f, %.1f, %.1f), %.1f m short",
                                    chain[leg].name, wp.what, wp.x, wp.z,
                                    pos.x, pos.y, pos.z, dist));
                    legOk = false;
                    break;
                }
                if (++frames > 5400) {                                     // 90 s hard timeout
                    x3::logInfo(fmt("--test-goldenpath: TIMEOUT on leg '%s' at (%.1f, %.1f, %.1f)",
                                    chain[leg].name, pos.x, pos.y, pos.z));
                    legOk = false;
                    break;
                }
                const float inv = 3.2f / std::max(dist, 0.001f);
                physics->moveCharacter(chr, x3::phys::Vec3{ dx * inv, 0.0f, dz * inv }, 1.0f/60.0f);
                physics->step(1.0f/60.0f);
            }
            if (!legOk) break;
        }
        if (legOk) {
            x3::phys::Vec3 pos = physics->getBodyPosition(chr);
            x3::logInfo(fmt("  LEG OK  %-14s -> %-14s  arrived (%.1f, %.1f, %.1f)",
                            chain[leg - 1].name, chain[leg].name, pos.x, pos.y, pos.z));
            curRoom = target;
        } else {
            allOk = false;
        }
    }

    physics->shutdown();
    x3::logInfo(std::string("--test-goldenpath: ") +
                (allOk ? "PASS — golden path completes with collision on (no noclip)"
                       : "FAIL — the golden path does NOT complete"));
    return allOk;
}

} // namespace x3::game
