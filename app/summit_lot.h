#pragma once
// ===========================================================================
// SUMMIT PARKING LOT — the destination at the top of the summit spur.
//
// docs/design/ROAD_NETWORK_SKETCH_V2.png is route-spec LAW and it labels the
// high ground "Parking Lot on Top of Mountain". Tim's lane brief (SEVEN_LANE
// PLAN lane 1) asks for the loop that ends there: "ramp exit to garage.. exit
// from garage to mountain top parking lot.. with exit down to the other
// tunnel."
//
// WHAT THIS MODULE IS, EXACTLY. road_network.cpp already builds the road up:
// registerSummitSpur() hill-climbs the natural field for a peak with real
// prominence and lays 1.4 miles of switchback at a 14 % cap to reach it
// (--test-roadnetwork K5/K6). What did not exist was anything AT the top — the
// spur simply stopped on a hillside. This is the place it stops at: a flat
// paved pad, kerbed, marked out in bays, that a car can drive onto, park on,
// and get out and stand on.
//
// IT IS A DESTINATION, NOT A DECORATION, and the difference is testable:
//   * the pad is CARVED (a registered TerrainCorridor, so the height field
//     itself is flat under it — no slab hovering over a dome),
//   * the slab COLLIDES and so does the kerb (NO_SLOP rule 11, the CONTACT
//     LAW: you can drive on it and you can walk on it),
//   * the ENTRY MOUTH is a gap in the kerb aligned with the spur's own last
//     leg, and --test-summitlot measures the step a car climbs there against
//     chassis clearance. A lot you cannot drive into is scenery.
//
// WHY A CORRIDOR AND NOT A NEW PRIMITIVE. A TerrainCorridor is a polyline
// dilated by halfWidth with a smoothstep falloff — a stadium. A short, wide
// one IS a pad, and reusing it means the lot inherits the carve machinery that
// --test-terraincorridor already gates (C1 monotone lowering, C3 tile-seam
// identity, C7 LOD parity), costs one slot of kMaxTerrainCorridors, and needs
// no new code in terrain.cpp at all. Registration must happen at BOOT, before
// the first TerrainStreamer::init() — the terrain.h registry contract.
//
// UNBUILT, AND SAID PLAINLY: the two legs that would close Tim's full loop —
// tunnel garage UP to this lot, and this lot DOWN into the second bore — are
// not here. The lot is reached by the summit spur, which is a real road from
// the connector, so it is connected and drivable today; it is not yet a loop.
// ===========================================================================

#include "scene.h"
#include "road_network.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

namespace x3::game {

// --- The pad, in feet, converted once (the road's unit — road_network.h) ----
// A real overlook lot: one two-way aisle with 90-degree bays down both sides.
//   bay          9.0 ft wide x 18.0 ft deep   (a US standard stall)
//   aisle       24.0 ft                        (two-way, so a car can turn out)
//   width       18 + 24 + 18 = 60.0 ft        (half 30.0 ft = 9.14 m)
//   length     144.0 ft                        (half 72.0 ft = 21.95 m)
// 144 / 9 = 16 bays a side, 32 stalls. Enough that the lot reads as a place
// people come to, small enough that it does not flatten the peak it sits on.
constexpr float kSlBayW      = 2.743f;   //  9.0 ft
constexpr float kSlBayDepth  = 5.486f;   // 18.0 ft
constexpr float kSlAisleW    = 7.315f;   // 24.0 ft
constexpr float kSlHalfW     = kSlBayDepth + kSlAisleW * 0.5f;   // 9.14 m
constexpr float kSlHalfLen   = 21.95f;   // 72.0 ft
constexpr int   kSlBaysPerSide = 16;
static_assert(kSlHalfW > kSlBayDepth + 0.5f,
              "the aisle must be wider than nothing: bays each side of a real lane");

// Kerb around the rim. 6 in — a step, not a wall: it stops a car rolling off
// the mountain and it is the thing that makes the pad read as built rather
// than as a patch of tarmac. The ENTRY MOUTH is a gap in it.
constexpr float kSlKerbH     = 0.152f;   // 6.0 in
constexpr float kSlKerbW     = 0.30f;    // 12 in of poured section
constexpr float kSlMouthW    = 9.0f;     // gap the spur drives through (m)

// The carve. The flat floor must cover the pad with margin, or the slab's rim
// lands on uncut ground and the kerb tilts into the hillside — the same rule
// kTcCorridorHalfW obeys for the bore and spec.halfWidth for a road.
constexpr float kSlCarveMargin = 2.0f;
constexpr float kSlCarveFall   = 14.0f;  // cut batter run, as the roads use
// Never gouge the peak away to seat the lot. If the ground under the pad needs
// more than this removed, the lot is in the wrong place and says so.
constexpr float kSlMaxCutM     = 14.0f;

struct SummitLotResult {
    bool        built  = false;
    const char* whyNot = "";
    // Pad frame: centre, long axis (unit XZ), and the level the slab sits at.
    float cx = 0.0f, cz = 0.0f, y = 0.0f;
    float dirX = 1.0f, dirZ = 0.0f;      // along the pad's LENGTH
    // The mouth's centre, on the pad rim nearest the spur's approach.
    float mouthX = 0.0f, mouthZ = 0.0f;
    float cutM = 0.0f;                   // deepest ground removed under the pad
    float fillM = 0.0f;                  // worst drop from slab rim to carved floor
    int   stalls = 0;
};

// Register the lot's carve. BOOT ONLY, before the first height query /
// TerrainStreamer::init(), and AFTER the spur it sits on top of (the pad's
// datum is the spur's last node, and its cut is measured against the field the
// spur has already carved its own way through).
SummitLotResult registerSummitLot(const SummitSpurResult& spur);

// Emit the slab, kerb, bay markings and lamp masts. Slab and kerb collide.
void buildSummitLot(const SummitLotResult& lot, Scene& scene,
                    x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& phys);

// --test-summitlot. Registers the real spur on the real field, puts the lot on
// top of it, and asserts the pad is flat, carved, inside the cut ceiling,
// entered at a step a car can climb, and marked out in the stalls it claims.
bool runSummitLotSelfTest();

} // namespace x3::game
