#pragma once
// ===========================================================================
// THE RIDEABLE WATERCRAFT ROSTER — one table, one source of truth.
//
// WHY (2026-08-28, owner: "speedboats and jetskis... we need to be fun and
// playable"). BoatDemo already floats a hull correctly — Archimedes buoyancy
// and quadratic drag, gated by the V3 self-test — but every number that makes
// a hull a PARTICULAR hull was a file-scope constant inside app/vehicle.cpp:
// m_hx/m_hy/m_hz 1.5/0.6/3.0, one shape for the river boats and the submarine
// alike. A jet ski and a speedboat are not the same craft and must not share
// one box (NO_SLOP rule 4: paired values are one value, named once).
//
// So the numbers that describe a CRAFT live here, next to the reason they were
// chosen. app/vehicle.cpp keeps only the numbers that describe the SIMULATION.
// This mirrors app/car_roster.h exactly — same file shape, same discipline, so
// anyone who has read one can read the other.
//
// THE FEEL, AND WHERE IT COMES FROM. An arcade watercraft is not a boat
// simulator; it is a car that leans. Three levers do nearly all the work, and
// two of them are borrowed technique (owner authorised reading the Unreal tree;
// CLAUDE.md's clean-room rule covers idTech/Doom/Quake, not Unreal):
//
//   1. PLANING. Unreal's BuoyancyComponent ramps its buoyancy coefficient with
//      forward speed (BuoyancyRampMinVelocity / MaxVelocity / RampMax): the
//      faster you go, the more the hull is held up, until it skims instead of
//      ploughing. That single curve is most of what "fast on water" feels like,
//      and it is why a jet ski at speed sits ON the surface rather than in it.
//   2. MULTI-POINT BUOYANCY. Force sampled at several hull points instead of
//      one, so the bow lifts under throttle and the craft leans into a turn as
//      a CONSEQUENCE rather than as an animation. A single centre-of-mass
//      Archimedes force floats correctly and feels dead.
//   3. RIVER FORCES. The current carries you, the shore pushes you off the
//      rocks, and the hull wants to align downstream. On a rushing underground
//      river that is the difference between a boat and a ride.
//
// planeSpeed is the ONE number a tuner should reach for first: below it the
// craft ploughs and turns tightly, above it it skims and runs long. Everything
// else is bounded by it.
// ===========================================================================
#include <cstddef>
#include <cstring>

namespace x3::game {

struct BoatSpec {
    const char* id;            // console / spawn id
    const char* name;          // HUD label
    const char* glb;           // path under convertedGlbRoot(); "" = graybox hull

    // ---- hull collision + displacement box, half-extents (m) ----
    // Also the submerged-volume model BuoyancyDesc integrates, so it must be
    // the real hull, not a bounding box with slack.
    float halfX, halfY, halfZ;
    float massKg;

    // ---- propulsion ----
    float propThrust;          // N at throttle = 1
    float steerTorque;         // N.m at steer = +-1
    float reverseFrac;         // fraction of propThrust available astern

    // ---- THE FEEL ----
    // planeSpeed: forward speed (m/s) at which the hull is fully up on plane.
    // planeLift : buoyancy multiplier at full plane (1.0 = no planing at all).
    // Below planeSpeed the craft ploughs; above it, it skims.
    float planeSpeed;
    float planeLift;
    // Attitude response. bowLift is how hard the nose comes up under throttle
    // (N.m per unit throttle); leanTorque is roll INTO a turn (N.m per unit
    // steer) — a jet ski leans hard, a speedboat leans less and later.
    float bowLift;
    float leanTorque;

    // ---- damping / stability ----
    float linearDrag;          // 1/s while submerged
    float angularDrag;         // 1/s while submerged
    float rightingTorque;      // N.m per radian — self-rights instead of listing

    const char* note;
};

// ---------------------------------------------------------------------------
// The roster. Index 0 is the default.
//
// Masses and hull boxes are real-world figures for the class of craft, in
// metres and kilograms (CLAUDE.md: 1 unit = 1 metre, no cm). A stand-up jet
// ski is ~3.3 m long, ~1.2 m across, ~350 kg wet; a small sport boat is ~6.5 m,
// ~2.2 m across, ~1400 kg. Those bounds set every other number here.
// ---------------------------------------------------------------------------
inline const BoatSpec* boatRoster(size_t& count) {
    static const BoatSpec kBoats[] = {
        // ---- JETSKI: light, flickable, jumps. The fun one. Low mass over a
        //      small hull means it planes almost immediately and changes
        //      direction on a thought; the high leanTorque is what sells it.
        { "jetski", "Jet Ski", "Vehicles/JetSki.glb",
          /*half*/ 0.62f, 0.42f, 1.65f, /*mass*/ 350.0f,
          /*prop*/ 9500.0f, /*steer*/ 5200.0f, /*reverse*/ 0.30f,
          /*planeSpeed*/ 9.0f, /*planeLift*/ 2.30f,
          /*bowLift*/ 2600.0f, /*leanTorque*/ 3400.0f,
          /*drag*/ 2.4f, /*angDrag*/ 1.6f, /*righting*/ 5200.0f,
          "Planes at ~20 mph and leans hard. 2.3x buoyancy on plane is what "
          "lifts it out of the water instead of dragging it through." },

        // ---- SPEEDBOAT: heavier, faster flat out, wider turn. Four times the
        //      mass needs far more thrust for a similar top end, and it planes
        //      later because a bigger hull has more to lift.
        { "speedboat", "Speedboat", "Vehicles/Speedboat.glb",
          /*half*/ 1.10f, 0.66f, 3.25f, /*mass*/ 1400.0f,
          /*prop*/ 30000.0f, /*steer*/ 15000.0f, /*reverse*/ 0.25f,
          /*planeSpeed*/ 13.0f, /*planeLift*/ 1.85f,
          /*bowLift*/ 7000.0f, /*leanTorque*/ 6200.0f,
          /*drag*/ 2.0f, /*angDrag*/ 1.3f, /*righting*/ 16000.0f,
          "Planes later (~29 mph) and holds a longer line. Leans less than the "
          "ski per unit of steer, which is what makes it read as the bigger boat." },
    };
    count = sizeof(kBoats) / sizeof(kBoats[0]);
    return kBoats;
}

// Lookup by id; nullptr if unknown. Empty/!id returns the default (index 0).
inline const BoatSpec* findBoat(const char* id) {
    size_t n = 0;
    const BoatSpec* r = boatRoster(n);
    if (!id || !id[0]) return &r[0];
    for (size_t i = 0; i < n; ++i)
        if (std::strcmp(r[i].id, id) == 0) return &r[i];
    return nullptr;
}

} // namespace x3::game
