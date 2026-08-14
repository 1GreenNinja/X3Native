#pragma once
// ===========================================================================
// GROUNDING — "character feet can NOT enter the floor unless its water, sand,
// or lava." (Tim, 2026-08.) This header is that rule, as code.
//
// THE BUG CLASS THIS CLOSES (three shipped instances, all silent)
// --------------------------------------------------------------
//   1. spawnDeathRagdoll passed originY=0 to makeHumanoidRagdollBones, which
//      places the PELVIS, not the feet -> corpse buried ~0.88 m. (fixed in
//      monster.cpp, but only there, and only after it shipped)
//   2. echoShipPose existed to seat hulls on their own water and was NEVER
//      CALLED -> hulls 0.606 m proud of the waterline.
//   3. The rescue girls: art authored feet-at-origin (verified: AnnaCasual.glb
//      spans y 0.000 .. 1.800), placed at a level-wide `kGroundY = 0`, standing
//      on a floor slab whose TOP is at +0.15 m. Boots in the concrete.
//
// Note what #3 is NOT: it is not a wrong model datum (the art is right, per
// docs/design/X3_WORLD_RULES.md rule 4) and it is not a heightfield sampling
// error (the sink is CONSTANT, not varying). It is a placement that used a
// LEVEL-WIDE GROUND CONSTANT where it needed THE ACTUAL SUPPORT SURFACE UNDER
// THIS XZ. The player never showed it because the player is physics-driven and
// simply rests on the slab; only AUTHORED placements sink.
//
// SO THE RULE IS ENFORCED IN THREE LAYERS, in this order:
//   1. ONE SHARED FUNCTION every placement path calls — groundCharacter(). A
//      rule each caller must remember is the trap we keep falling into.
//   2. AN ASSERTION THAT SHOUTS — names the character, the surface type, the
//      penetration in metres and the call site. Silence is how all three prior
//      instances survived; this follows the established loud-banner pattern
//      ("!!! THIS RUN DID NOT TEST THOSE").
//   3. A GATE (--test-grounding, runGroundingSelfTest below) covering an
//      interior floor, an exterior hard surface, a slope, stairs, a doorway
//      threshold, AND a legitimately penetrable surface where a bounded
//      penetration is EXPECTED — proving the exemption works, not just that
//      the rule fires.
//
// COST — asked and answered honestly. groundCharacter() is ONE downward
// rayCastStrict PER PLACEMENT, not per frame. Placements happen at build()/
// spawn time (tens to low hundreds per level load), so the cost is unmeasurable
// against a 46.6 FPS budget. There is NO per-frame component and none is
// proposed: a physics-driven character already rests on its collision, and
// re-probing every walker every frame would cost real time for no defect.
//
// HEADER-ONLY: app/CMakeLists.txt is owned by an in-flight 29-commit EXE split.
// ===========================================================================

#include "surface_type.h"

#include "engine/core/x3_log.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/asset/IModelLoader.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace x3::game {

// ---------------------------------------------------------------------------
// Tuning. All metres.
// ---------------------------------------------------------------------------
// How far ABOVE the wanted feet position the probe starts. Must clear a floor
// slab that sits proud of the authored constant (the 0.15 m defect) but must
// NOT clear a table/crate the character is standing under, or we would snap
// them up onto it. 0.60 m is comfortably above the first and below the second.
inline constexpr float kGroundProbeUp   = 0.60f;
// How far below to search for a floor before giving up.
inline constexpr float kGroundProbeDown = 3.00f;
// Slop before the rule fires. 8 mm — under a boot sole, over any float noise.
inline constexpr float kGroundTolerance = 0.008f;
// A correction larger than this is not a datum slip, it is a probe that found
// the wrong surface (a mezzanine below, a roof above, geometry not built yet).
// We REFUSE to move the character that far and shout instead of teleporting.
inline constexpr float kGroundMaxSnap   = 2.00f;

// ---------------------------------------------------------------------------
// What the probe found under an XZ. `found == false` means NOTHING static was
// under there — the caller must NOT move the character (there is no floor to
// ground against), and groundCharacter() says so out loud.
// ---------------------------------------------------------------------------
struct GroundProbe {
    bool        found    = false;
    float       surfaceY = 0.0f;
    float       normalY  = 1.0f;
    SurfaceType surf     = SurfaceType::Unknown;
};

// Probe the REAL support surface under (x, z), searching around `nearY`.
//
// rayCastStrict(Layer::Static) is deliberate: the plain rayCast() also reports
// dynamic bodies, which would let a character ground against ANOTHER character's
// collision box (every rescue victim owns one) or a loose prop.
//
// `hint` lets a caller that KNOWS the surface (a host that built a sand beach,
// a water plane) name it. Unhinted terrain worlds pass `terrain=true` to run the
// splat classifier; everything else resolves to Unknown, which is SOLID — an
// unclassified floor fails CLOSED, which is the behaviour that catches bugs.
inline GroundProbe probeGround(x3::phys::IPhysicsWorld& physics,
                               float x, float z, float nearY,
                               SurfaceType hint = SurfaceType::Unknown,
                               bool terrain = false,
                               float up = kGroundProbeUp,
                               float down = kGroundProbeDown) {
    GroundProbe g;
    const x3::phys::Vec3 origin{ x, nearY + up, z };
    const x3::phys::Vec3 dir{ 0.0f, -1.0f, 0.0f };
    const x3::phys::RayHit h = physics.rayCastStrict(origin, dir, up + down,
                                                     x3::phys::Layer::Static);
    if (!h.hit) return g;
    g.found    = true;
    g.surfaceY = h.point.y;
    g.normalY  = h.normal.y;
    if (hint != SurfaceType::Unknown)      g.surf = hint;
    else if (terrain)                      g.surf = classifyTerrainSurface(x, g.surfaceY, z, g.normalY);
    else                                   g.surf = SurfaceType::Unknown;   // solid, fails closed
    return g;
}

// ---------------------------------------------------------------------------
// THE ASSERTION. Returns true if the invariant HOLDS.
//
// `lowestY` is the character's LOWEST extent in world space — the lowest bone /
// capsule / bind-pose vertex, NOT the root. That distinction is the entire bug
// class: every prior instance measured the root and assumed the rest.
//
// Shouts once per violation, naming character, surface, depth and call site.
// ---------------------------------------------------------------------------
inline bool assertFeetNotInFloor(const char* who, float lowestY,
                                 const GroundProbe& g, const char* site,
                                 float tol = kGroundTolerance) {
    if (!g.found) return true;                       // nothing to violate
    const float allowance = surfaceFootAllowance(g.surf);
    const float depth     = (g.surfaceY - allowance) - lowestY;   // >0 == too deep
    if (depth <= tol) return true;

    char buf[640];
    std::snprintf(buf, sizeof(buf),
        "[grounding] !!! FEET IN THE FLOOR — THE RULE IS BROKEN !!!\n"
        "[grounding] !!!   character   : %s\n"
        "[grounding] !!!   lowest point: y=%.4f m (this is the LOWEST extent, not the root)\n"
        "[grounding] !!!   surface     : %s at y=%.4f m, legal penetration %.3f m\n"
        "[grounding] !!!   PENETRATION : %.4f m BELOW what this surface allows\n"
        "[grounding] !!!   placed from : %s\n"
        "[grounding] !!! Character feet may not enter a solid surface. Penetrable\n"
        "[grounding] !!! surfaces are water / sand / lava (see app/surface_type.h).\n"
        "[grounding] !!! This is the bug class that has now shipped THREE times.",
        who ? who : "<unnamed>", (double)lowestY,
        surfaceName(g.surf), (double)g.surfaceY, (double)allowance,
        (double)depth, site ? site : "<unknown call site>");
    x3::logError(buf);
    return false;
}

// ---------------------------------------------------------------------------
// THE SHARED PLACEMENT FUNCTION. Every character placement path goes through
// this. Returns the position the character should actually be placed at.
//
//   wanted      — the authored/desired position, interpreted as the FEET
//                 (docs/design/X3_WORLD_RULES.md rule 4: origin at the contact
//                 surface). If your system's datum is the pelvis or the body
//                 CENTER, convert BEFORE calling — see `artLowestBelowFeet`.
//   artLowestBelowFeet — metres the ART dips below `wanted` (0 for a model
//                 authored feet-at-origin; POSITIVE if the rig hangs lower, e.g.
//                 Jake_22_actions.glb's bind pose reaches -0.9647).
//
// BEHAVIOUR
//   * probe misses            -> return `wanted` UNCHANGED, and say so loudly.
//                                We never invent a floor.
//   * |correction| > 2 m      -> return `wanted` UNCHANGED and SHOUT. A snap
//                                that big means the probe found the wrong
//                                surface; silently teleporting would be worse
//                                than the sink.
//   * otherwise               -> lift/drop so the LOWEST extent lands exactly on
//                                the surface (minus any legal penetration).
//
// It is a SOFT CLAMP in every build, not just Debug: a Release player should not
// see boots in concrete. It is also LOUD in every build (once, at placement —
// not per frame), because the silent version is how all three prior instances
// survived review.
// ---------------------------------------------------------------------------
inline x3::phys::Vec3 groundCharacter(x3::phys::IPhysicsWorld& physics,
                                      const x3::phys::Vec3& wanted,
                                      float artLowestBelowFeet,
                                      const char* who, const char* site,
                                      SurfaceType hint = SurfaceType::Unknown,
                                      bool terrain = false,
                                      GroundProbe* outProbe = nullptr,
                                      float up = kGroundProbeUp,
                                      float down = kGroundProbeDown) {
    const GroundProbe g = probeGround(physics, wanted.x, wanted.z, wanted.y, hint, terrain,
                                      up, down);
    if (outProbe) *outProbe = g;

    if (!g.found) {
        x3::logWarn(std::string("[grounding] no static surface under '") +
                    (who ? who : "?") + "' at (" + std::to_string(wanted.x) + ", " +
                    std::to_string(wanted.z) + ") — LEFT AT THE AUTHORED Y (" +
                    std::to_string(wanted.y) + "). Grounding was NOT verified here. [" +
                    (site ? site : "?") + "]");
        return wanted;
    }

    // Where the art's lowest point WOULD land if we honoured `wanted` verbatim.
    const float lowestIfUnmoved = wanted.y - artLowestBelowFeet;
    // TARGET is the surface itself — the per-surface allowance is a PERMISSION
    // the assertion grants (so a walk cycle on sand or a wade in water is not a
    // defect), NOT a target to sink characters into. Seating them ON the sand is
    // right; deliberately burying them 5 cm in it is not.
    const float correction      = g.surfaceY - lowestIfUnmoved;

    if (std::fabs(correction) <= kGroundTolerance) return wanted;   // already right

    if (std::fabs(correction) > kGroundMaxSnap) {
        char buf[512];
        std::snprintf(buf, sizeof(buf),
            "[grounding] !!! REFUSING TO SNAP '%s' by %.3f m (limit %.2f m) at (%.2f, %.2f).\n"
            "[grounding] !!! The probe found a surface at y=%.3f but the authored feet are at\n"
            "[grounding] !!! y=%.3f. That is not a datum slip — the probe almost certainly hit\n"
            "[grounding] !!! the WRONG surface (a deck below, geometry not built yet, or this\n"
            "[grounding] !!! character is deliberately airborne). LEAVING IT WHERE IT WAS. [%s]",
            who ? who : "?", (double)correction, (double)kGroundMaxSnap,
            (double)wanted.x, (double)wanted.z, (double)g.surfaceY,
            (double)wanted.y, site ? site : "?");
        x3::logWarn(buf);
        return wanted;
    }

    x3::phys::Vec3 fixed = wanted;
    fixed.y = wanted.y + correction;
    const float allowance = surfaceFootAllowance(g.surf);

    // Only SHOUT for the direction that is the shipped defect — feet inside a
    // solid surface. A character floating above it is wrong too, but it is a
    // different (and visible) failure; report it at info volume.
    if (correction > kGroundTolerance) {
        char buf[560];
        std::snprintf(buf, sizeof(buf),
            "[grounding] !!! FEET IN THE FLOOR at placement — CORRECTED !!!\n"
            "[grounding] !!!   character : %s\n"
            "[grounding] !!!   authored  : y=%.4f  (lowest extent would be y=%.4f)\n"
            "[grounding] !!!   surface   : %s at y=%.4f, legal penetration %.3f m\n"
            "[grounding] !!!   SINK      : %.4f m  -> lifted to y=%.4f\n"
            "[grounding] !!!   call site : %s\n"
            "[grounding] !!! Fix the authored Y; this clamp is a safety net, not the answer.",
            who ? who : "?", (double)wanted.y, (double)lowestIfUnmoved,
            surfaceName(g.surf), (double)g.surfaceY, (double)allowance,
            (double)correction, (double)fixed.y, site ? site : "?");
        x3::logError(buf);
    } else {
        x3::logInfo(std::string("[grounding] '") + (who ? who : "?") + "' floated " +
                    std::to_string(-correction) + " m above " + surfaceName(g.surf) +
                    " — dropped to y=" + std::to_string(fixed.y) + " [" +
                    (site ? site : "?") + "]");
    }
    return fixed;
}

// ---------------------------------------------------------------------------
// ART DATUM MEASUREMENT — the number every caller needs and nobody had.
//
// Returns the lowest BIND-POSE vertex Y of a loaded model, in model space. For
// art authored per X3_WORLD_RULES rule 4 (origin at the contact surface) this is
// ~0. For the Jake armature-offset family it is ~-0.9647, and a caller that
// places that model at feet-height without accounting for it buries it — which
// is exactly instance #1 of this bug class.
//
// Only SKINNED primitives carry CPU bind-pose positions (MeshPrimitive::basePos),
// so `ok` is false for a purely static model and the caller must not guess.
// ---------------------------------------------------------------------------
struct ModelExtent { bool ok = false; float minY = 0.0f; float maxY = 0.0f; };

inline ModelExtent modelBindExtentY(const x3::asset::Model& m) {
    ModelExtent e;
    float lo = 1e30f, hi = -1e30f;
    for (const auto& p : m.primitives) {
        if (p.basePos.empty()) continue;
        for (size_t i = 1; i < p.basePos.size(); i += 3) {
            const float y = p.basePos[i];
            if (y < lo) lo = y;
            if (y > hi) hi = y;
        }
        e.ok = true;
    }
    if (!e.ok) return e;
    e.minY = lo; e.maxY = hi;
    return e;
}

// How far a model's art dips BELOW its own origin, in metres, at `scale`.
// 0 for correctly-authored feet-at-origin art. Feed this to groundCharacter().
inline float artLowestBelowOrigin(const x3::asset::Model& m, float scale = 1.0f) {
    const ModelExtent e = modelBindExtentY(m);
    if (!e.ok || e.minY >= 0.0f) return 0.0f;
    return -e.minY * scale;
}

// ===========================================================================
// --test-grounding  — THE GATE.
//
// This is the durable deliverable: the assertion that makes a FOURTH instance
// of this bug class impossible. It covers, headlessly and with no Vulkan:
//   G1  an INTERIOR floor slab whose top sits proud of the authored ground
//       constant — the exact shipped defect (0.15 m), asserted as detected AND
//       corrected.
//   G2  an EXTERIOR hard surface at the authored height — asserted NOT moved
//       (the rule must not "fix" correct placements).
//   G3  a legitimately PENETRABLE surface — bounded penetration EXPECTED and
//       asserted, proving the exemption works rather than only that the rule
//       fires.
//   G4  a SLOPE — grounding must follow the surface, not a plane.
//   G5  STAIRS — each tread grounds to its own top.
//   G6  a DOORWAY THRESHOLD — two floors of different heights meeting; a
//       character on each side grounds to its own side.
//   G7  the ART DATUM — a rig that hangs below its origin must be lifted by
//       exactly that much (instance #1 of the bug class, as a unit test).
//   G8  FAIL-SAFE — no floor under the XZ means DO NOT MOVE, and no floor means
//       the invariant cannot be claimed.
//   G9  REFUSE-TO-TELEPORT — an absurd correction is refused, not applied.
//   G10 the SURFACE TABLE — Unknown must be solid (fails closed), and Tim's
//       three named surfaces must be penetrable.
// ===========================================================================
namespace groundingtest {
inline int g_pass = 0, g_fail = 0;
inline void gcheck(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[grounding-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[grounding-test] FAIL ") + name); }
}
// NOT named `near` — that is a legacy Windows macro (minwindef.h) and would
// expand to nothing at the call sites below.
inline bool approx(float a, float b, float tol = 0.005f) { return std::fabs(a - b) <= tol; }
} // namespace groundingtest

inline bool runGroundingSelfTest() {
    using namespace groundingtest;
    g_pass = g_fail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();

    // ---- The test world -----------------------------------------------------
    // EXTERIOR ground plane, top at y = 0, out at x in [-40, -10].
    physics->addBox(x3::phys::Vec3{ 15.0f, 0.5f, 15.0f },
                    x3::phys::Vec3{ -25.0f, -0.5f, 0.0f }, 0.0f, x3::phys::Layer::Static);
    // INTERIOR floor slab — EXACTLY the surface facility's geometry:
    //   opaqueSlab(cx, baseY + 0.05, cz, hx, 0.10, hz) with baseY = 0
    // so its TOP is at +0.15 m while the authored spawn constant is kGroundY = 0.
    // This is the shipped defect, reproduced as a fixture.
    physics->addBox(x3::phys::Vec3{ 8.0f, 0.10f, 8.0f },
                    x3::phys::Vec3{ 0.0f, 0.05f, 0.0f }, 0.0f, x3::phys::Layer::Static);
    // PENETRABLE patch (beach), top at y = 0.
    physics->addBox(x3::phys::Vec3{ 4.0f, 0.5f, 4.0f },
                    x3::phys::Vec3{ 40.0f, -0.5f, 0.0f }, 0.0f, x3::phys::Layer::Static);
    // STAIRS — 4 treads, 0.18 m rise each, tops at 0.18/0.36/0.54/0.72.
    for (int i = 0; i < 4; ++i) {
        const float top = 0.18f * (float)(i + 1);
        physics->addBox(x3::phys::Vec3{ 1.0f, top * 0.5f, 0.35f },
                        x3::phys::Vec3{ 0.0f, top * 0.5f, 20.0f + 0.7f * (float)i },
                        0.0f, x3::phys::Layer::Static);
    }
    // DOORWAY THRESHOLD — a raised interior floor (top 0.30) butting an exterior
    // pad (top 0.00) at z = -20. Two characters, one either side of the seam.
    physics->addBox(x3::phys::Vec3{ 4.0f, 0.15f, 2.0f },
                    x3::phys::Vec3{ 0.0f, 0.15f, -22.0f }, 0.0f, x3::phys::Layer::Static);
    physics->addBox(x3::phys::Vec3{ 4.0f, 0.50f, 2.0f },
                    x3::phys::Vec3{ 0.0f, -0.50f, -18.0f }, 0.0f, x3::phys::Layer::Static);
    // SLOPE — a static triangle mesh ramp rising 1 m over 10 m along +X, at z=40.
    {
        const float v[] = {
            -5.0f, 0.0f, 35.0f,   5.0f, 1.0f, 35.0f,   5.0f, 1.0f, 45.0f,  -5.0f, 0.0f, 45.0f
        };
        const uint32_t idx[] = { 0, 2, 1, 0, 3, 2 };
        physics->addStaticMesh(v, 4, idx, 6);
    }
    physics->optimizeBroadphase();

    // ---- G1: the shipped defect ------------------------------------------
    {
        GroundProbe g;
        const x3::phys::Vec3 authored{ 0.0f, 0.0f, 0.0f };       // kGroundY, as shipped
        const x3::phys::Vec3 fixed = groundCharacter(*physics, authored, 0.0f,
                                                     "G1/interior-floor", "grounding.h:G1",
                                                     SurfaceType::Concrete, false, &g);
        gcheck(g.found, "G1 interior floor probe found a surface");
        gcheck(approx(g.surfaceY, 0.15f), "G1 interior floor top measured at +0.150 m");
        gcheck(approx(fixed.y, 0.15f), "G1 authored y=0 corrected to the floor top (+0.150 m)");
        // The raw invariant must have REPORTED the violation for the authored value.
        gcheck(!assertFeetNotInFloor("G1", authored.y, g, "grounding.h:G1"),
               "G1 assertion FIRES on the authored (sunk) position");
        gcheck(assertFeetNotInFloor("G1", fixed.y, g, "grounding.h:G1"),
               "G1 assertion HOLDS on the corrected position");
    }

    // ---- G2: exterior hard surface, already correct -> must not move -------
    {
        GroundProbe g;
        const x3::phys::Vec3 authored{ -25.0f, 0.0f, 0.0f };
        const x3::phys::Vec3 fixed = groundCharacter(*physics, authored, 0.0f,
                                                     "G2/exterior", "grounding.h:G2",
                                                     SurfaceType::Concrete, false, &g);
        gcheck(approx(g.surfaceY, 0.0f), "G2 exterior surface measured at 0.000 m");
        gcheck(approx(fixed.y, 0.0f), "G2 a correct placement is LEFT ALONE (no phantom lift)");
        gcheck(assertFeetNotInFloor("G2", fixed.y, g, "grounding.h:G2"),
               "G2 invariant holds on exterior hard ground");
    }

    // ---- G3: a legitimately penetrable surface ----------------------------
    // Sand allows 0.05 m. A character 0.05 m into sand is LEGAL and must not
    // trip the rule; 0.20 m into sand is still a bug and must.
    {
        GroundProbe g = probeGround(*physics, 40.0f, 0.0f, 0.0f, SurfaceType::Sand);
        gcheck(g.found && approx(g.surfaceY, 0.0f), "G3 sand patch probed at 0.000 m");
        gcheck(surfaceIsPenetrable(SurfaceType::Sand), "G3 sand is penetrable");
        gcheck(assertFeetNotInFloor("G3/legal-sink", -0.05f, g, "grounding.h:G3"),
               "G3 EXEMPTION WORKS: 0.05 m into sand is allowed, rule does NOT fire");
        gcheck(!assertFeetNotInFloor("G3/too-deep", -0.20f, g, "grounding.h:G3"),
               "G3 0.20 m into sand still FIRES (the exemption is bounded, not a blank cheque)");
        // ...and grounding a character onto sand seats them at the allowance.
        const x3::phys::Vec3 fixed = groundCharacter(*physics, x3::phys::Vec3{40.0f, 0.0f, 0.0f},
                                                     0.0f, "G3", "grounding.h:G3",
                                                     SurfaceType::Sand);
        gcheck(approx(fixed.y, 0.0f),
               "G3 grounding onto sand seats the feet ON the sand (the allowance is a "
               "permission for art/anim to dip, NOT a target to bury them at)");
    }

    // ---- G4: a slope ------------------------------------------------------
    // The ramp rises 1 m over 10 m in +X. Sampling three XZ must give three
    // different heights that track the ramp — a flat-plane assumption fails here.
    {
        const float xs[3]  = { -4.0f, 0.0f, 4.0f };
        const float exp[3] = {  0.10f, 0.50f, 0.90f };
        bool allOk = true, allDistinct = true;
        float prev = -999.0f;
        for (int i = 0; i < 3; ++i) {
            GroundProbe g = probeGround(*physics, xs[i], 40.0f, exp[i],
                                        SurfaceType::Rock, false, 1.0f, 3.0f);
            if (!g.found || !approx(g.surfaceY, exp[i], 0.03f)) allOk = false;
            if (i && !(g.surfaceY > prev + 0.2f)) allDistinct = false;
            prev = g.surfaceY;
        }
        gcheck(allOk, "G4 SLOPE: grounding follows the ramp height at 3 XZ samples");
        gcheck(allDistinct, "G4 SLOPE: the three samples genuinely differ (not a plane)");
    }

    // ---- G5: stairs -------------------------------------------------------
    {
        bool ok = true;
        for (int i = 0; i < 4; ++i) {
            const float top = 0.18f * (float)(i + 1);
            const x3::phys::Vec3 authored{ 0.0f, 0.0f, 20.0f + 0.7f * (float)i };
            const x3::phys::Vec3 fixed = groundCharacter(*physics, authored, 0.0f,
                                                         "G5/stairs", "grounding.h:G5",
                                                         SurfaceType::Concrete, false, nullptr,
                                                         1.0f, 3.0f);
            if (!approx(fixed.y, top, 0.02f)) ok = false;
        }
        gcheck(ok, "G5 STAIRS: each tread grounds to its OWN top (0.18/0.36/0.54/0.72)");
    }

    // ---- G6: doorway threshold -------------------------------------------
    {
        const x3::phys::Vec3 inside = groundCharacter(*physics,
            x3::phys::Vec3{ 0.0f, 0.0f, -22.0f }, 0.0f, "G6/inside", "grounding.h:G6",
            SurfaceType::Concrete, false, nullptr, 1.0f, 3.0f);
        const x3::phys::Vec3 outside = groundCharacter(*physics,
            x3::phys::Vec3{ 0.0f, 0.0f, -18.0f }, 0.0f, "G6/outside", "grounding.h:G6",
            SurfaceType::Concrete, false, nullptr, 1.0f, 3.0f);
        gcheck(approx(inside.y, 0.30f, 0.02f),  "G6 THRESHOLD: inside grounds to the raised floor (0.300)");
        gcheck(approx(outside.y, 0.00f, 0.02f), "G6 THRESHOLD: outside grounds to the pad (0.000)");
        gcheck(std::fabs(inside.y - outside.y) > 0.25f,
               "G6 THRESHOLD: the two sides do NOT share one ground height");
    }

    // ---- G7: the ART DATUM (bug-class instance #1, as a unit test) --------
    // A rig whose art hangs 0.9647 m below its origin (the Jake armature-offset
    // family) placed at feet height must be LIFTED by exactly that much, or its
    // lower body is buried — which is what spawnDeathRagdoll shipped.
    {
        GroundProbe g;
        const float hang = 0.9647f;
        const x3::phys::Vec3 fixed = groundCharacter(*physics,
            x3::phys::Vec3{ 0.0f, 0.15f, 0.0f }, hang, "G7/pelvis-datum-rig",
            "grounding.h:G7", SurfaceType::Concrete, false, &g);
        gcheck(approx(fixed.y, 0.15f + hang, 0.01f),
               "G7 ART DATUM: a rig hanging 0.9647 m below its origin is lifted by exactly that");
        gcheck(assertFeetNotInFloor("G7", fixed.y - hang, g, "grounding.h:G7"),
               "G7 ART DATUM: its LOWEST extent then sits on the floor");
        gcheck(!assertFeetNotInFloor("G7", 0.15f - hang, g, "grounding.h:G7"),
               "G7 ART DATUM: placing it WITHOUT the lift is caught (the shipped bug)");
    }

    // ---- G8: fail-safe when there is no floor -----------------------------
    {
        GroundProbe g;
        const x3::phys::Vec3 authored{ 500.0f, 3.0f, 500.0f };   // empty space
        const x3::phys::Vec3 fixed = groundCharacter(*physics, authored, 0.0f,
                                                     "G8/void", "grounding.h:G8",
                                                     SurfaceType::Unknown, false, &g);
        gcheck(!g.found, "G8 FAIL-SAFE: probe honestly reports NO surface over the void");
        gcheck(approx(fixed.y, 3.0f), "G8 FAIL-SAFE: a character over nothing is NOT moved");
        gcheck(assertFeetNotInFloor("G8", -100.0f, g, "grounding.h:G8"),
               "G8 FAIL-SAFE: with no surface the invariant makes no claim");
    }

    // ---- G9: refuse to teleport ------------------------------------------
    {
        // Authored 6 m above the interior floor: a deliberate catwalk/airborne
        // placement, not a datum slip. The rule must NOT yank them down.
        const x3::phys::Vec3 fixed = groundCharacter(*physics,
            x3::phys::Vec3{ 0.0f, 6.0f, 0.0f }, 0.0f, "G9/airborne", "grounding.h:G9",
            SurfaceType::Concrete, false, nullptr, 0.6f, 12.0f);
        gcheck(approx(fixed.y, 6.0f),
               "G9 REFUSE-TO-TELEPORT: a 5.85 m correction is refused, not silently applied");
    }

    // ---- G10: the surface table contract ---------------------------------
    {
        gcheck(!surfaceIsPenetrable(SurfaceType::Unknown),
               "G10 Unknown FAILS CLOSED: an unclassified surface is solid");
        gcheck(!surfaceIsPenetrable(SurfaceType::Concrete) &&
               !surfaceIsPenetrable(SurfaceType::Metal) &&
               !surfaceIsPenetrable(SurfaceType::Rock) &&
               !surfaceIsPenetrable(SurfaceType::Grass),
               "G10 hard surfaces are NOT penetrable");
        gcheck(surfaceIsPenetrable(SurfaceType::Water) &&
               surfaceIsPenetrable(SurfaceType::Sand) &&
               surfaceIsPenetrable(SurfaceType::Lava),
               "G10 Tim's three (water/sand/lava) ARE penetrable");
        gcheck(surfaceProps(SurfaceType::Water).liquid &&
               surfaceProps(SurfaceType::Lava).liquid,
               "G10 water/lava are flagged liquid");
        // The terrain classifier must agree with the splat's own bands.
        gcheck(classifyTerrainSurface(1100.0f, 6.0f, -1350.0f, 1.0f) == SurfaceType::Sand,
               "G10 terrain classifier: shoreline low ground -> Sand");
        gcheck(classifyTerrainSurface(0.0f, 20.0f, 0.0f, 1.0f) == SurfaceType::Grass,
               "G10 terrain classifier: inland flat low ground -> Grass");
        gcheck(classifyTerrainSurface(0.0f, 20.0f, 0.0f, 0.70f) == SurfaceType::Rock,
               "G10 terrain classifier: steep slope -> Rock (slope-rock override)");
        gcheck(classifyTerrainSurface(0.0f, 300.0f, 0.0f, 0.95f) == SurfaceType::Snow,
               "G10 terrain classifier: high flat peak -> Snow");
    }

    physics->shutdown();
    x3::logInfo("[grounding-test] " + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
