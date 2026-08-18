#pragma once
// ===========================================================================
// THE PLAYER'S CAR ROSTER — one table, one source of truth.
//
// WHY (W-HEROCAR, 2026-08-17). Everything that makes a hero car a *particular*
// car used to be file-scope constants inside app/vehicle.cpp: the wheel
// stations, the ride-height drop, the body-widen stretch, the chassis box.
// They were all measured off Vehicles/CTR.glb, so any second car drawn through
// DriveDemo::skin() got CTR's 2.274 m wheelbase — wheels planted inside the
// bodywork instead of in the arches. Adding a car meant editing five numbers
// in three functions and hoping (NO_SLOP rule 4: paired values are one value).
//
// So: the numbers that describe a CAR live here, once, next to the GLB they
// were measured from. app/vehicle.cpp holds only the numbers that describe the
// SIMULATION (grip curves, CoM, anti-roll, the torque map) — those are tuned
// against measured skidpad/slalom runs and are deliberately NOT per-car yet.
//
// EVERY NUMBER IN THE `ctr` ENTRY IS THE ONE app/vehicle.cpp SHIPPED BEFORE
// THIS FILE EXISTED. Default is `ctr`, so behaviour is unchanged unless a host
// explicitly selects another id.
//
// HOW TO MEASURE A NEW ENTRY (the GBX one was done exactly this way):
//   python tools/glb_node_bounds.py <glb>   -> wheel node AABBs
//   wheelX  = |x| of the wheel hub centre    (metres, in the GLB's own space)
//   wheelZ  = the hub's z NEGATED — the GLB is authored nose = +Z and the
//             engine drives nose = -Z, so the front station is NEGATIVE here
//   wheelRadius = half the wheel node's y extent
//   bodyDropY  = -(wheel attach 0.15 + rest suspension travel + wheelRadius)
//   bodyWiden  = 1.0 unless the owner asks for hips (CTR is stretched 1.18)
// ===========================================================================
#include <cstddef>
#include <cstring>

namespace x3::game {

struct CarSpec {
    const char* id;            // --car <id>
    const char* name;          // chooser / HUD label
    const char* glb;           // path under convertedGlbRoot()

    // ---- render skin (app/vehicle.cpp kBodySkin) ----
    float bodyWiden;           // X stretch applied to the BODY only, never the track
    float bodyDropY;           // GLB ground-origin -> physics chassis centre

    // ---- wheel stations, ENGINE space (nose = -Z), pre-bodyWiden ----
    float wheelXFront, wheelXRear;   // half-track, +-
    float wheelZFront, wheelZRear;   // front is negative (forward)
    float wheelRadius, wheelWidth;

    // ---- chassis collision box half-extents + mass ----
    float halfX, halfY, halfZ;
    float massKg;

    const char* note;
};

// ---------------------------------------------------------------------------
// The roster. Index 0 is the default.
// ---------------------------------------------------------------------------
inline const CarSpec* carRoster(size_t& count) {
    static const CarSpec kCars[] = {
        // ---- CTR: the incumbent. Numbers lifted VERBATIM out of
        //      app/vehicle.cpp's buildPhysics()/kBodySkin as of 2026-08-16 —
        //      the CoM/grip/anti-roll tuning receipts in that file were all
        //      measured against exactly these, so they must not drift.
        { "ctr", "CTR", "Vehicles/CTR.glb",
          /*bodyWiden*/ 1.18f, /*bodyDropY*/ -0.76f,
          /*wheelX*/ 0.677f, 0.723f, /*wheelZ*/ -1.186f, 1.088f,
          /*wheelR/W*/ 0.33f, 0.24f,
          /*half*/ 1.07f, 0.5f, 1.95f, /*mass*/ 1083.2f,
          "The 993-shaped baseline. bodyWiden 1.18 is the owner's 2026-08-14 "
          "\"the MODEL needs to be wider\" — the BODY stretches, the track does not." },

        // ---- GBX COUPE: the hero car built by tools/build_gbx_hero_car.py
        //      from the HDRP GBX COUPE pack. A front-engine GT coupe, NOT the
        //      mid-engine NSX that was asked for — the library has no NSX.
        //      Stations MEASURED off the built GLB (see the tool's log):
        //        hub FL (0.8197, 0.3307, +1.4708) m, RL (0.8151, .., -1.4118)
        //      z negated here for the nose flip; radius 0.3305, width 0.2704.
        //      bodyWiden 1.0: the body is already 1.926 m across, wider than
        //      CTR's stretched 2.13 would need. bodyDropY = -(0.15 attach +
        //      0.28 rest travel + 0.3305 radius) = -0.76, same as CTR because
        //      the wheels are the same size.
        { "gbx", "GBX COUPE", "Vehicles/GBX_Coupe.glb",
          /*bodyWiden*/ 1.0f, /*bodyDropY*/ -0.76f,
          /*wheelX*/ 0.8197f, 0.8151f, /*wheelZ*/ -1.4708f, 1.4118f,
          /*wheelR/W*/ 0.3305f, 0.27f,
          /*half*/ 0.963f, 0.5f, 2.36f, /*mass*/ 1620.0f,
          "Black clearcoat + satin-black rocker, 243 k tris. 2.882 m wheelbase "
          "(CTR is 2.274) — the reason the stations had to leave vehicle.cpp." },
    };
    count = sizeof(kCars) / sizeof(kCars[0]);
    return kCars;
}

inline const CarSpec& carSpecDefault() {
    size_t n = 0;
    return carRoster(n)[0];
}

// Look up by --car id. Unknown id -> the default (callers log it).
inline const CarSpec& carSpecById(const char* id) {
    size_t n = 0;
    const CarSpec* r = carRoster(n);
    if (id && *id) {
        for (size_t i = 0; i < n; ++i)
            if (std::strcmp(r[i].id, id) == 0) return r[i];
    }
    return r[0];
}

inline bool carSpecKnown(const char* id) {
    size_t n = 0;
    const CarSpec* r = carRoster(n);
    if (!id || !*id) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::strcmp(r[i].id, id) == 0) return true;
    return false;
}

} // namespace x3::game
