#pragma once
// ============================================================================
// APRON LANDING — the ONE WORLD intro landing (feat/canon-apron-landing).
//
// Owner decision (2026-08-16, live play): "Why are we even landing in a
// different world than we play in?" The intro's flyable outcomes (Escaped /
// CapitalKilled) used to hand off to the SEPARATE `--world surface` slice and
// then [E]-switch into canonlevel at the breach. That world switch is gone:
// the landing now puts the player IN canonlevel — spawned on the canon
// facility's apron ring, ship set down beside the walk, facing the breach —
// and he simply WALKS in through the SEAM-2 exterior. `--world surface`
// remains a dev shortcut per docs/design/WORLDS.md; the GAME no longer routes
// through it.
//
// This header owns the PURE placement maths so app_run.cpp (the live caller)
// and the --test-apronlanding gate share one truth:
//   * introLandingSpawnKey()  — which destination key each intro outcome
//     lands at ("apron" for the flyable outcomes, "" = the canon cell).
//   * computeApronLanding()   — spawn point/facing, breach point, and the
//     ship set-down spot, all derived from the FacilityExterior::Desc the
//     canon world actually built (no duplicated constants).
//   * shipYForApron()         — the PLACEMENT-DATUM law: the hull's measured
//     AABB minY decides the set-down Y, never the model origin (the exact
//     bug class that shipped 0.6 m-proud boats).
//   * apronWalkToBreachClear()— the nav probe: the straight walk from the
//     spawn to the breach stays on the apron ring and enters through the
//     breach gap; the ship must be clear of that corridor.
//
// Header-only (grounding.h precedent): app/CMakeLists is shared with other
// lanes; no new TU. Game/slice code only; engine/ stays pure.
// ============================================================================

#include "facility_exterior.h"
#include "intro_orchestrator.h"
#include "destinations.h"        // the "apron" registry row (self-test AL4)

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace x3::game {

// The destination-registry key of the landing spot (destinations.cpp row).
inline constexpr const char* kApronDestKey = "apron";

// Placement tuning (metres, from the facade breach gap outward).
inline constexpr float kApronSpawnOutM = 10.0f;  // breach -> feet along the outward normal
inline constexpr float kApronShipOutM  = 13.0f;  // breach -> ship origin along the normal
// Lateral ship offset along the facade tangent. NEGATIVE = the -tangent side:
// on the canon +Z breach face that is WEST of the walk line, clear of the
// "apron_east" parked car (breachCenter+10) AND of the golden-path walk.
inline constexpr float kApronShipSideM = -9.0f;
// The walk corridor's required clearance to the parked ship (a body + wingtip).
inline constexpr float kApronWalkClearM = 3.5f;

// Which destination key an intro outcome lands the player at.
//   Escaped / CapitalKilled -> "apron" (he flew down; he stands at his ship)
//   ShotDown                -> ""      (canon: captured, wakes in the cell)
inline std::string introLandingSpawnKey(x3::intro::IntroOutcome o) {
    using x3::intro::IntroOutcome;
    return (o == IntroOutcome::Escaped || o == IntroOutcome::CapitalKilled)
               ? std::string(kApronDestKey)
               : std::string();
}

// The authored landing: everything is at the apron walk level (fd.baseY —
// the FacilityExterior ring apron's collision top is flush with baseY).
struct ApronLanding {
    bool  ok = false;
    float apronY   = 0.0f;               // the walk datum (== fd.baseY)
    float spawn[3] = { 0, 0, 0 };        // player FEET (y == apronY)
    float spawnYaw = 0.0f;               // player.setLook yaw: fwd=(cos,·,sin)
    float breach[3] = { 0, 0, 0 };       // breach gap centre on the facade plane
    float ship[3]  = { 0, 0, 0 };        // ship origin XZ (Y via shipYForApron)
    float shipYaw  = 0.0f;               // prop-matrix yaw {c,0,-s;0,1,0;s,0,c}
    float normal[2] = { 0, 0 };          // breach face outward normal (x, z)
};

// Derive the landing from the exterior the canon world ACTUALLY built.
inline ApronLanding computeApronLanding(const FacilityExterior::Desc& fd) {
    ApronLanding al;
    if (!(fd.x1 > fd.x0) || !(fd.z1 > fd.z0)) return al;          // no footprint
    if (fd.apron == FacilityExterior::Apron::None) return al;     // nothing to stand on
    float nx = 0.0f, nz = 0.0f, bx = 0.0f, bz = 0.0f;
    switch (fd.breachFace) {
        case FacilityExterior::Face::PlusZ:  nx = 0;  nz = +1; bx = fd.breachCenter; bz = fd.z1; break;
        case FacilityExterior::Face::MinusZ: nx = 0;  nz = -1; bx = fd.breachCenter; bz = fd.z0; break;
        case FacilityExterior::Face::PlusX:  nx = +1; nz = 0;  bx = fd.x1; bz = fd.breachCenter; break;
        case FacilityExterior::Face::MinusX: nx = -1; nz = 0;  bx = fd.x0; bz = fd.breachCenter; break;
    }
    al.apronY    = fd.baseY;
    al.normal[0] = nx; al.normal[1] = nz;
    al.breach[0] = bx; al.breach[1] = fd.baseY; al.breach[2] = bz;
    al.spawn[0]  = bx + nx * kApronSpawnOutM;
    al.spawn[1]  = fd.baseY;
    al.spawn[2]  = bz + nz * kApronSpawnOutM;
    // Face the breach: forward = -normal; player yaw convention fwd=(cos,·,sin).
    al.spawnYaw  = std::atan2(-nz, -nx);
    // Ship: further out along the normal, offset along the facade tangent
    // t = (nz, -nx) so the hull is clear of the walk corridor.
    const float tx = nz, tz = -nx;
    al.ship[0]   = bx + nx * kApronShipOutM + tx * kApronShipSideM;
    al.ship[1]   = fd.baseY;
    al.ship[2]   = bz + nz * kApronShipOutM + tz * kApronShipSideM;
    // Nose toward the facility (model nose = -Z at yaw 0; rotate -Z onto -n).
    al.shipYaw   = std::atan2(nx, nz);
    al.ok = true;
    return al;
}

// THE PLACEMENT-DATUM LAW: the set-down Y comes from the hull's MEASURED AABB,
// not the model origin. `modelMinY` is the raw GLB's lowest vertex Y (CPU
// measurement, node hierarchy applied — see app/glb_cpu_read.h); the returned
// origin Y seats that lowest point exactly ON the apron top.
inline float shipYForApron(float apronTopY, float modelMinY, float scale) {
    return apronTopY - modelMinY * scale;
}

// NAV/WALK PROBE: the straight walk from the spawn to the breach must stay on
// the apron ring the whole way and cross the facade only inside the breach gap;
// the parked ship must be clear of that corridor. Pure geometry against the
// SAME Desc the collision was built from (ring apron slabs span the footprint
// expanded by apronOut; the breach gap is breachHalfW about breachCenter).
inline bool apronWalkToBreachClear(const ApronLanding& al,
                                   const FacilityExterior::Desc& fd,
                                   int samples = 24) {
    if (!al.ok) return false;
    for (int i = 0; i <= samples; ++i) {
        const float t = (float)i / (float)samples;
        const float x = al.spawn[0] + (al.breach[0] - al.spawn[0]) * t;
        const float z = al.spawn[2] + (al.breach[2] - al.spawn[2]) * t;
        const bool insideFootprint = (x > fd.x0 && x < fd.x1 && z > fd.z0 && z < fd.z1);
        if (insideFootprint) {
            // Only legal within the breach gap (the vestibule carries it inward).
            const bool faceIsX = (fd.breachFace == FacilityExterior::Face::PlusX ||
                                  fd.breachFace == FacilityExterior::Face::MinusX);
            const float lateral = faceIsX ? z : x;
            if (std::fabs(lateral - fd.breachCenter) > fd.breachHalfW) return false;
            continue;
        }
        const bool onRing = x >= fd.x0 - fd.apronOut && x <= fd.x1 + fd.apronOut &&
                            z >= fd.z0 - fd.apronOut && z <= fd.z1 + fd.apronOut;
        if (!onRing) return false;
    }
    // Ship clearance: perpendicular distance from the ship to the walk segment.
    const float ax = al.spawn[0], az = al.spawn[2];
    const float dx = al.breach[0] - ax, dz = al.breach[2] - az;
    const float len2 = dx * dx + dz * dz;
    float u = len2 > 1e-6f ? ((al.ship[0] - ax) * dx + (al.ship[2] - az) * dz) / len2 : 0.0f;
    u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
    const float px = ax + dx * u - al.ship[0];
    const float pz = az + dz * u - al.ship[2];
    return std::sqrt(px * px + pz * pz) >= kApronWalkClearM;
}

// ===========================================================================
// --test-apronlanding — THE GATE (headless, deterministic, no window/Vulkan).
//
// Asserts the ONE WORLD landing contract:
//   AL1-3  outcome routing: the flyable outcomes land at "apron"; ShotDown
//          stays the canon cell (no spawn key).
//   AL4    the "apron" destination is a LIVE registry row (Facility group,
//          canon anchor, no standalone world).
//   AL5-8  placement on ALL FOUR breach faces of a synthetic footprint:
//          spawn OUTSIDE the footprint ON the ring at the walk datum, facing
//          the breach; ship clear of the walk; nav probe green.
//   AL9    the CANON tower itself (the shipped JSON, the same footprint/breach
//          derivation app_run's SEAM-2 build uses): the landing computes, the
//          spawn is outside every room, the walk to the breach is clear.
//          JSON absent on this machine -> SKIP-as-pass (the --test-canonlevel
//          fallback rule).
//   AL10   the placement-datum law: the hull AABB, not the origin, decides
//          the set-down Y (the 0.6 m-proud-boat bug class, as a unit test).
// ===========================================================================
inline bool runApronLandingSelfTest() {
    using x3::intro::IntroOutcome;
    int pass = 0, total = 0;
    auto check = [&](bool ok, const std::string& what) {
        ++total; if (ok) ++pass;
        x3::logInfo(std::string(ok ? "  [PASS] " : "  [FAIL] ") + what);
    };
    auto near1 = [](float a, float b, float tol = 0.01f) { return std::fabs(a - b) <= tol; };

    // ---- AL1-3: outcome -> landing key. ----
    check(introLandingSpawnKey(IntroOutcome::Escaped) == kApronDestKey,
          "AL1 ESCAPED lands at 'apron' (IN canonlevel — no world switch)");
    check(introLandingSpawnKey(IntroOutcome::CapitalKilled) == kApronDestKey,
          "AL2 CAPITAL_KILLED lands at 'apron' (wreck salvage starts at the ship)");
    check(introLandingSpawnKey(IntroOutcome::ShotDown).empty(),
          "AL3 SHOT_DOWN keeps the canon cell (no spawn key — byte-identical)");

    // ---- AL4: the registry row. ----
    {
        const Destination* d = findDestination(kApronDestKey);
        check(d != nullptr && d->group == DestGroup::Facility && d->canonAnchor &&
              d->worldFlag[0] == '\0',
              "AL4 'apron' is a LIVE registry row: Facility group, canon anchor, "
              "no standalone world");
    }

    // ---- AL5-8: all four breach faces of a synthetic footprint. ----
    {
        const FacilityExterior::Face faces[4] = {
            FacilityExterior::Face::PlusZ,  FacilityExterior::Face::MinusZ,
            FacilityExterior::Face::PlusX,  FacilityExterior::Face::MinusX };
        const char* names[4] = { "AL5 +Z", "AL6 -Z", "AL7 +X", "AL8 -X" };
        for (int f = 0; f < 4; ++f) {
            FacilityExterior::Desc fd;
            fd.x0 = -20.0f; fd.x1 = 20.0f; fd.z0 = -30.0f; fd.z1 = 10.0f;
            fd.baseY = -2.0f; fd.topY = 106.0f;
            fd.breachFace = faces[f];
            const bool faceIsX = (f >= 2);
            fd.breachCenter = faceIsX ? -4.0f : 5.0f;   // off-centre, in-range
            fd.breachHalfW  = 2.4f;
            fd.apron = FacilityExterior::Apron::Ring;
            fd.apronOut = 24.0f;
            const ApronLanding al = computeApronLanding(fd);
            const bool outside = !(al.spawn[0] > fd.x0 && al.spawn[0] < fd.x1 &&
                                   al.spawn[2] > fd.z0 && al.spawn[2] < fd.z1);
            const bool onRing = al.spawn[0] >= fd.x0 - fd.apronOut &&
                                al.spawn[0] <= fd.x1 + fd.apronOut &&
                                al.spawn[2] >= fd.z0 - fd.apronOut &&
                                al.spawn[2] <= fd.z1 + fd.apronOut;
            // Facing: forward (player convention) must point at the breach.
            const float fx = std::cos(al.spawnYaw), fz = std::sin(al.spawnYaw);
            float tx = al.breach[0] - al.spawn[0], tz = al.breach[2] - al.spawn[2];
            const float tl = std::sqrt(tx * tx + tz * tz);
            if (tl > 1e-4f) { tx /= tl; tz /= tl; }
            const bool facing = (fx * tx + fz * tz) > 0.99f;
            check(al.ok && outside && onRing && near1(al.spawn[1], fd.baseY) &&
                  near1(tl, kApronSpawnOutM) && facing &&
                  apronWalkToBreachClear(al, fd),
                  std::string(names[f]) + " face: spawn on the ring at the datum, "
                  "facing the breach, walk + ship clearance green");
        }
    }

    // ---- AL9: the SHIPPED canon tower (same derivation as app_run's SEAM 2). ----
    {
        const CanonFloor cf = loadCanonTower(canonProjectJsonPath());
        if (!cf.valid()) {
            x3::logInfo("  [SKIP] AL9 canonical JSON absent — treating as PASS "
                        "(the --test-canonlevel fallback rule)");
        } else {
            // Footprint + breach, exactly as app_run.cpp's SEAM-2 build derives
            // them (above-ground rooms only; the Entrance room's exterior face).
            float x0 = 1e9f, x1 = -1e9f, z0 = 1e9f, z1 = -1e9f, top = -1e9f;
            for (const CanonRoom& r : cf.rooms) {
                if (r.cy <= -50.0f) continue;
                x0 = std::min(x0, r.x0()); x1 = std::max(x1, r.x1());
                z0 = std::min(z0, r.z0()); z1 = std::max(z1, r.z1());
                top = std::max(top, r.y1());
            }
            constexpr float kExtPad = 3.0f;
            const uint32_t er = cf.roomByName("Entrance");
            check(er != kNoRoom && x1 > x0 && top > -1e8f,
                  "AL9a the canon data authors an Entrance room on a real footprint");
            if (er != kNoRoom && x1 > x0 && top > -1e8f) {
                const CanonRoom& e = cf.rooms[er];
                const float dist[4] = { e.x0() - x0, x1 - e.x1(), e.z0() - z0, z1 - e.z1() };
                int bf = 0;
                for (int i = 1; i < 4; ++i) if (dist[i] < dist[bf]) bf = i;
                const float halfCut = 1.5f;
                const bool faceIsX = bf < 2;
                const float lo = (faceIsX ? e.z0() : e.x0()) + halfCut + 0.2f;
                const float hi = (faceIsX ? e.z1() : e.x1()) - halfCut - 0.2f;
                FacilityExterior::Desc fd;
                fd.x0 = x0 - kExtPad; fd.x1 = x1 + kExtPad;
                fd.z0 = z0 - kExtPad; fd.z1 = z1 + kExtPad;
                fd.baseY = e.y0(); fd.topY = top;
                fd.breachFace   = (FacilityExterior::Face)bf;
                fd.breachCenter = std::min(std::max(faceIsX ? e.cz : e.cx, lo), hi);
                fd.breachHalfW  = 2.4f;
                fd.apron = FacilityExterior::Apron::Ring;
                fd.apronOut = 24.0f; fd.soilOut = 150.0f;
                const ApronLanding al = computeApronLanding(fd);
                check(al.ok, "AL9b the landing computes on the shipped tower");
                check(al.ok && cf.roomAt(al.spawn[0], al.spawn[1] + 1.0f, al.spawn[2]) == kNoRoom,
                      "AL9c the apron spawn is OUTSIDE every room (outdoors, free)");
                check(al.ok && apronWalkToBreachClear(al, fd),
                      "AL9d the walk to the breach is on the apron + clear of the ship "
                      "(the facility is REACHABLE)");
                check(al.ok && cf.roomAt(al.breach[0] - al.normal[0] * (kExtPad + 1.0f),
                                         al.breach[1] + 1.0f,
                                         al.breach[2] - al.normal[1] * (kExtPad + 1.0f)) == er,
                      "AL9e one step inside the breach is the Entrance room itself");
            }
        }
    }

    // ---- AL10: the placement-datum law (hull AABB, not origin). ----
    {
        // A hull whose art dips 0.27 m below its origin at scale 2.2 (the real
        // JakeFighterShip shape class): the set-down Y must LIFT the origin so
        // the lowest hull point sits exactly ON the apron.
        const float y = shipYForApron(/*apronTopY=*/-2.0f, /*modelMinY=*/-0.27f, 2.2f);
        check(near1(y + (-0.27f) * 2.2f, -2.0f),
              "AL10a hull minY lands exactly ON the apron (origin lifted by -minY*scale)");
        check(near1(shipYForApron(-2.0f, 0.0f, 2.2f), -2.0f),
              "AL10b contact-at-origin art sets down AT the apron top (no phantom lift)");
        check(shipYForApron(0.0f, 0.6f, 1.0f) < 0.0f,
              "AL10c art floating ABOVE its origin is pulled DOWN (never 0.6 m proud — "
              "the shipped boat bug, inverted)");
    }

    char sb[96];
    std::snprintf(sb, sizeof(sb), "apron-landing: %d/%d passed", pass, total);
    x3::logInfo(sb);
    return pass == total;
}

} // namespace x3::game
