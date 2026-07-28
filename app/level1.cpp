// EFLZ Level 1 "The Spire" — vertical B1->F7 graybox stack. See app/level1.h.
//
// Clean-room: built from the Scene + IRenderDevice + IPhysicsWorld interfaces and
// the mesh_prims box builder only. No purchased C# / id Tech source consulted.
#include "level1.h"
#include "mesh_prims.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace x3::game {

namespace {

constexpr float kIdentity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

constexpr float kWallT = 0.2f;        // wall thickness
constexpr float kDoorHalf = 0.6f;     // doorway opening half-width (1.2 m wide)
constexpr float kLintelBottom = 2.1f; // head clearance under a doorway lintel
constexpr float kCeilT = 0.2f;        // ceiling cap thickness (collision lid)

// ---- Canonical Spire FLOOR table (footprints + base Y + RAISED ceiling heights).
// Single source of truth shared with env_art.cpp (via level1Rooms()): the art
// overlay tiles its GLB floors/walls/ceilings/lights to these EXACT bounds + base
// height + ceiling, so geometry, art and lights stay in lockstep across the whole
// tower. Floors stack along +Y at the REAL NON-UNIFORM pitch from the authoritative
// Task9D floor table (docs/design/X3_WORLD_BLUEPRINT.md §2.1) — the elevator builds
// one stop per floor from floorBaseY[] so the stops auto-follow. All floors share the
// SAME XZ footprint at the +X end so the elevator + stair shafts line up vertically.
//
// REAL VERTICAL RE-SCALE (2026-05-23, X3_WORLD_BLUEPRINT §2.1): the Spire was an
// 8x-compressed UNIFORM 5 m pitch (B1..F7 = 0..35 m). It is now the REAL canon
// NON-UNIFORM stack: B1 Detention=0, F1 Atrium=5 (breather), F2 Medical=10, F3
// Genetics=20, F4 Cybernetics=30, F5 Drone=65, F6 Alien=78, F7 Executive=91 m. (The
// off-elevator F4.5 Nexus auto-follows spire_nexus.cpp midway between F4 and F5; the
// canon ROOF/Helipad at 104 m is a future stop, not one of the 8 enum floors.) Per-
// floor ceilings are RAISED to exploit the now-generous non-uniform clearance while
// always leaving >=1 m for the slab above so stacked plates never overlap (the tight
// 5 m B1->F1->F2 gaps keep their <5 m ceilings; the wide 10/13/35 m gaps get taller
// halls). Footprints are UNCHANGED (Tim 2026-05-23: the big 75-97 m plates stay).
// The rooftop F7 is open to the sky so it gets a genuinely tall cap.
//
// FLOOR-1 RELAY (2026-05): the B1 plate was grown from the old 24x16 m placeholder to
// the REAL Floor-1 detention footprint (docs/design/SPIRE_LEVELARCHITECT_DIMS.md):
// X -24..+60 (~84 m, covering the detention core X -20..+18 AND the eastern stairs->
// caves arm out to the Side Grotto at x=55) and zHalf=38 (covering the detention Z span
// -36..+7 with margin). The B1 plate carries the full 29-room interior (built in
// buildLevel1). The DETENTION content bounding box is ~75 x 43 m (asserted by the
// self-test via level1DetentionFootprint()); the raw plate is a superset.
//
// FLOORS 2-7 DIMENSIONING (2026-05): F2-F7 no longer share B1's plate — each gets its
// OWN identity-appropriate footprint at REAL LevelArchitect scale (Floor 1 is ~75x43 m;
// the v10.9 source has NO authored geometry for F2-7, so they are authored fresh at that
// scale or larger — see SPIRE_LEVELARCHITECT_DIMS.md). KEY LAYOUT INVARIANT: the elevator
// shaft (kShaftCx=21) stays at the EAST edge of every floor (x1 ~= 25, just past the
// shaft), so a rider always arrives at x~=17.5 right at the shaft mouth AND the per-floor
// "hub" arrival trigger (spire_mid/top place it at [x1-8, x1]) lands ON the arrival point
// (it was detached at x~52-60 while F2-7 shared B1's x1=60 plate). Each floor then grows
// WESTWARD (x0 negative) + DEEP (zHalf) to its real size; the existing encounters sit in
// the eastern arrival third (content is authored at absolute x in [0,18], z in [-6.5,6.5]
// — all inside every new plate) and the western space is partitioned into identity rooms
// (see SS4). F5 Drone Manufacturing is the largest (a high-bay assembly hall). B1 + F1 are
// UNCHANGED (F1 Atrium has no LevelArchitect source; B1's footprint is read by the
// sub-levels). The 5 m vertical pitch is UNCHANGED (elevator + spire_*/sublevel content
// that reads floorBaseY[] stay in lockstep); ceilings stay <= 4.8 m (except the open-sky
// rooftop F7) so stacked plates never overlap.
//   x0,    x1,    zHalf,  ceil,  y0 (REAL non-uniform canon Y, X3_WORLD_BLUEPRINT §2.1)
const L1RoomDef kFloors[(uint32_t)L1Floor::Count] = {
    { -24.0f, 60.0f, 38.0f,  4.0f,  0.0f },  // B1 — Detention Level (canon F1; 29-room interior); gap->F1 = 5 m
    { -24.0f, 60.0f, 38.0f,  4.6f,  5.0f },  // F1 — Atrium / lobby (breather);                    gap->F2 = 5 m
    { -50.0f, 25.0f, 22.0f,  8.0f, 10.0f },  // F2 — Medical Bay          (75 x 44; wards wing);   gap->F3 = 10 m
    { -50.0f, 25.0f, 22.0f,  8.0f, 20.0f },  // F3 — Genetics Lab         (75 x 44; gene-vats);    gap->F4 = 10 m
    { -54.0f, 25.0f, 23.0f,  9.0f, 30.0f },  // F4 — Cybernetics Workshop (79 x 46; +Nexus W);     gap->F5 = 35 m (Nexus@~47.5)
    { -72.0f, 25.0f, 32.0f, 11.0f, 65.0f },  // F5 — Drone Manufacturing  (97 x 64; high-bay);     gap->F6 = 13 m
    { -58.0f, 25.0f, 26.0f, 11.0f, 78.0f },  // F6 — Alien Technology Lab (83 x 52; tech halls);   gap->F7 = 13 m
    { -54.0f, 25.0f, 23.0f, 12.0f, 91.0f },  // F7 — Executive Laboratory (79 x 46; open finale, sky cap)
};

// ---- F2-F7 WEST-WING IDENTITY ROOMS (single source of truth; see level1.h L1WingRoom).
// buildLevel1 builds each room's collision graybox (roomBox) from THIS table, and
// wing_dressing.cpp reads the SAME table for the art pass, so collision and dressing can
// never drift. Each room's floor Y + ceiling come from kFloors[room.floor]. The `name`
// routes the dressing recipe (matches a room_dressing.cpp desc branch — labs/servers/
// bays/etc.), chosen so every room on a floor reads as a DISTINCT AAA-dressed space.
const L1WingRoom kWingRooms[] = {
    // F2 Medical Bay — the SIGNATURE three-captive RESCUE WING (docs/design/EFLZ_NARRATIVE
    // Floor-2). THREE SIDE-BY-SIDE white clinical rescue rooms in a row along X, each with
    // its OWN door ('S') onto the F2 west corridor (the z in [-3,3] lane). Each is 6.5w x
    // 7.5d — enough for a 2.3 m hospital bed CENTERED with >=1.2 m walk-around on all sides
    // plus advanced-medical-equipment walls. The captives are RESTRAINED on the beds
    // (Aria / Keisha / Emily); saved -> companions, failed -> bosses (Siren / Breeder Queen
    // / Oracle). Room B (Keisha) is the MAGNETICALLY SEALED room — the dressing gives its
    // door a red locked tell; the LOCK MECHANIC is a host hook (see report). The names
    // route the rescue-room dressing recipe (room_dressing.cpp classify -> ZMedical +
    // the "Rescue Room" desc-gold branch: bed, captive, straps, monitors, surgical light).
    { L1Floor::F2, -27.0f, 6.75f, 3.25f, 3.75f, 'S', "Rescue Room A (Aria)" },
    { L1Floor::F2, -35.0f, 6.75f, 3.25f, 3.75f, 'S', "Rescue Room B (Keisha)" },
    { L1Floor::F2, -43.0f, 6.75f, 3.25f, 3.75f, 'S', "Rescue Room C (Emily)" },
    // F3 Genetics Lab (vat green): specimen hall + clone/growth/hybrid/DNA labs.
    { L1Floor::F3, -38.0f,  0.0f, 10.0f, 14.0f, 'E', "Gene Vat Gallery" },     // big signature hall
    { L1Floor::F3, -18.0f,  8.0f,  6.0f,  5.0f, 'S', "Clone Storage" },
    { L1Floor::F3, -18.0f, -8.0f,  6.0f,  5.0f, 'N', "Growth Tank Array" },
    { L1Floor::F3,  -7.0f,  8.0f,  4.0f,  5.0f, 'S', "Hybridization Chamber" },
    { L1Floor::F3,  -7.0f, -8.0f,  4.0f,  5.0f, 'N', "DNA Sequencing Lab" },
    // F4 Cybernetics Workshop (cold cyan): server room + aug bays + workshop + power.
    { L1Floor::F4, -40.0f,  0.0f, 11.0f, 15.0f, 'E', "Server Room" },          // big signature hall
    { L1Floor::F4, -18.0f,  9.0f,  6.0f,  6.0f, 'S', "Augmentation Bay" },
    { L1Floor::F4, -18.0f, -9.0f,  6.0f,  6.0f, 'N', "Neural Interface Lab" },
    { L1Floor::F4,  -7.0f,  9.0f,  4.0f,  5.0f, 'S', "Workshop" },
    { L1Floor::F4,  -7.0f, -9.0f,  4.0f,  5.0f, 'N', "Power Junction" },
    // F5 Drone Manufacturing (amber industrial): the huge assembly hangar + control/power.
    { L1Floor::F5, -44.0f,  0.0f, 26.0f, 26.0f, 'E', "Assembly Bay" },         // huge hangar
    { L1Floor::F5,  -8.0f, 12.0f,  5.0f,  6.0f, 'S', "Central Control Hub" },
    { L1Floor::F5,  -8.0f,-12.0f,  5.0f,  6.0f, 'N', "Recharge Station" },
    // F6 Alien Technology Lab (dark biolume green): portal hall + artifact/analysis/pods.
    { L1Floor::F6, -42.0f,  0.0f, 13.0f, 18.0f, 'E', "Portal Chamber" },       // big signature hall
    { L1Floor::F6, -18.0f, 10.0f,  6.0f,  6.0f, 'S', "Artifact Storage" },
    { L1Floor::F6, -18.0f,-10.0f,  6.0f,  6.0f, 'N', "Analysis Lab" },
    { L1Floor::F6,  -7.0f, 10.0f,  4.0f,  5.0f, 'S', "First Contact Chamber" },
    { L1Floor::F6,  -7.0f,-10.0f,  4.0f,  5.0f, 'N', "Transformation Pods" },
    // F7 Executive Laboratory (dark luxury + brass): boardroom + offices/comms/security/server.
    { L1Floor::F7, -40.0f,  0.0f, 11.0f, 15.0f, 'E', "Boardroom" },            // big signature hall
    { L1Floor::F7, -18.0f,  8.0f,  6.0f,  5.0f, 'S', "Executive Offices" },
    { L1Floor::F7, -18.0f, -8.0f,  6.0f,  5.0f, 'N', "Comms Center" },
    { L1Floor::F7,  -7.0f,  8.0f,  4.0f,  5.0f, 'S', "Security Checkpoint" },
    { L1Floor::F7,  -7.0f, -8.0f,  4.0f,  5.0f, 'N', "Server Room" },
};
constexpr uint32_t kWingRoomCount = sizeof(kWingRooms) / sizeof(kWingRooms[0]);

// ---- FLOOR 1 "Detention Level" — the authoritative LevelArchitect transcription
// (docs/design/SPIRE_LEVELARCHITECT_DIMS.md). 29 rooms, transcribed DIRECTLY (no axis
// flip): center (x,z) + full extents (w,h,d). floorY = the room's floor relative to the
// B1 plate base (0 = ground; the descending stairs/caves dip below into negative Y, but
// CLAMPED above the sub-levels at y=-5 — see buildLevel1's cave-arm note). monster/npc
// carry the Cell(Monster) / Sarah's-cell flags. Laid on the native B1 plate.
const L1DetentionRoom kDetention[] = {
    // name                          cx,    cz,   floorY, w,    h,   d,    monster, npc
    { "Jake's Cell",                  0.0f,   0.0f,  0.0f,  7.0f, 4.0f, 6.0f, false, false },
    { "Cell 2 (Abandoned)",           0.0f,  -8.0f,  0.0f,  6.0f, 3.5f, 5.0f, false, false },
    { "Cell 3 (Failed Exp)",          0.0f, -15.0f,  0.0f,  6.0f, 3.5f, 5.0f, true,  false },
    { "Cell 4 (Skeleton)",            0.0f, -22.0f,  0.0f,  6.0f, 3.5f, 5.0f, false, false },
    { "Main Hallway",                 5.5f, -12.0f,  0.0f,  3.0f, 3.5f, 26.0f, false, false },
    { "Guard Station",               11.0f,  -2.0f,  0.0f,  5.0f, 3.5f, 5.0f, false, false },
    { "Storage",                     11.0f,  -9.0f,  0.0f,  5.0f, 3.5f, 5.0f, false, false },
    { "Medical Bay",                 11.0f, -16.0f,  0.0f,  5.0f, 3.5f, 5.0f, false, false },
    { "Armory",                      11.0f, -23.0f,  0.0f,  5.0f, 3.5f, 5.0f, false, false },
    { "Elevator Lobby",               5.5f, -27.0f,  0.0f,  5.0f, 4.0f, 4.0f, false, false },
    { "Adjacent Cell",                5.5f,   5.0f,  0.0f,  5.0f, 3.5f, 4.0f, false, false },
    { "Old Armory",                  -1.0f,   7.0f,  0.0f,  7.0f, 3.5f, 5.0f, false, false },
    { "Creepy Passage",              16.0f,  -2.0f,  0.0f,  4.0f, 3.0f, 3.0f, false, false },
    { "Cell 5 (Vacated)",           -20.0f,  -4.0f,  0.0f,  6.0f, 3.5f, 5.0f, false, false },
    { "Cell 6 (Infected)",          -20.0f, -11.0f,  0.0f,  6.0f, 3.5f, 5.0f, true,  false },
    { "Cell 7 (Dead Guard)",        -20.0f, -18.0f,  0.0f,  6.0f, 3.5f, 5.0f, false, false },
    { "Cell 8 (Containment)",       -20.0f, -25.0f,  0.0f,  6.0f, 3.5f, 5.0f, false, false },
    { "Cell 9 (Collapsed)",         -20.0f, -32.0f,  0.0f,  6.0f, 3.5f, 5.0f, false, false },
    { "Cell 10 (Sarah's - Empty)",   -7.0f,  -4.0f,  0.0f,  6.0f, 3.5f, 5.0f, false, true  },
    { "Cell 11 (Feral)",             -7.0f, -11.0f,  0.0f,  6.0f, 3.5f, 5.0f, true,  false },
    { "Cell 12 (Flooded)",           -7.0f, -18.0f,  0.0f,  6.0f, 3.5f, 5.0f, false, false },
    { "Cell 13 (Mutation)",          -7.0f, -25.0f,  0.0f,  6.0f, 3.5f, 5.0f, true,  false },
    { "Cell 14 (Blood Trail)",       -7.0f, -32.0f,  0.0f,  6.0f, 3.5f, 5.0f, false, false },
    { "Cell Block B Hallway",       -13.5f, -18.0f,  0.0f,  4.0f, 3.5f, 32.0f, false, false },
    { "CB South Connector",         -13.5f, -36.0f,  0.0f, 14.0f, 3.5f, 3.0f, false, false },
    { "Descending Stairs",           20.0f,  -2.0f, -1.0f,  4.0f, 5.0f, 3.0f, false, false },
    { "Cave Tunnel",                 27.0f,  -2.0f, -2.0f, 10.0f, 3.5f, 3.0f, false, false },
    { "Crystal Cavern",              41.0f,  -2.0f, -3.0f, 18.0f, 8.0f, 16.0f, false, false },
    { "Side Grotto",                 55.0f,   1.0f, -2.5f,  8.0f, 6.0f, 8.0f, false, false },
};
constexpr uint32_t kDetCount = sizeof(kDetention) / sizeof(kDetention[0]);

// Door / connection pairs (room-index pairs into kDetention) — LA.FLOOR1_DOORS.
const uint32_t kDetDoors[] = {
    0,4, 1,4, 2,4, 3,4, 4,5, 4,6, 4,7, 4,8, 4,9, 4,10,
    0,18, 1,18, 1,19, 2,19, 2,20, 3,20, 3,21,
    5,12, 12,25, 25,26, 26,27, 27,28,
    0,11, 10,11,
    13,23, 14,23, 15,23, 16,23, 17,23,
    18,23, 19,23, 20,23, 21,23, 22,23,
    17,24, 22,24, 23,24,
};
constexpr uint32_t kDetDoorPairCount = (sizeof(kDetDoors) / sizeof(kDetDoors[0])) / 2;

// ---- Central elevator shaft (a vertical column shared by every floor). XZ is
// constant up the whole tower; the cab rides it. The shaft sits at the +X end of
// each plate; a doorway opens from the shaft (its -X face, at x=shaftX0, z=0) into
// each floor plate. The stairwell mirrors it at the -X end (see buildLevel1).
constexpr float kShaftCx = 21.0f;     // shaft center X
constexpr float kShaftCz = 0.0f;      // shaft center Z (on the spine)
constexpr float kShaftHx = 1.5f;      // shaft half-width X (3 m wide)
constexpr float kShaftHz = 1.5f;      // shaft half-depth Z (3 m deep)
constexpr float kShaftX0 = kShaftCx - kShaftHx; // 19.5 — shaft -X face (door into plate)

// Per-floor tints so each plate reads as a distinct wing in the graybox fallback.
const float kFloorTints[(uint32_t)L1Floor::Count][4] = {
    { 0.55f, 0.60f, 0.75f, 1.0f }, // B1 — cool blue (detention/security)
    { 0.78f, 0.82f, 0.90f, 1.0f }, // F1 — bright glass atrium
    { 0.65f, 0.80f, 0.78f, 1.0f }, // F2 — clinical teal (medical)
    { 0.55f, 0.72f, 0.55f, 1.0f }, // F3 — lab green
    { 0.75f, 0.70f, 0.55f, 1.0f }, // F4 — office tan
    { 0.50f, 0.55f, 0.78f, 1.0f }, // F5 — synth blue
    { 0.72f, 0.62f, 0.45f, 1.0f }, // F6 — executive warm
    { 0.60f, 0.66f, 0.80f, 1.0f }, // F7 — sky/rooftop
};

// ---- THE WALLS WERE A 4% BLUE MIRROR OF AN AMBIENT THAT NO LONGER EXISTS -------
// (2026-07-12, fix/prim-point-light. Measured, flashlight OFF, under a working B1
// ceiling fixture: ceiling luma 62.8 WARM, floor 32.3 WARM, WALL 4.3 and BLUE.)
// The wall was NOT off the point-light path — `r_debugview 2` (the point-light
// diffuse term alone) shows these walls catching HALF the floor's irradiance. They
// were multiplying that warm light by a near-black, BLUE-DOMINANT albedo:
//     panel texture (78,84,94) sRGB = (0.078, 0.089, 0.111) linear
//   x kWallTint      (0.62, 0.66, 0.78)      = (0.048, 0.059, 0.087) linear
//   x the fixture    (1.00, 0.86, 0.62)      -> (0.048, 0.051, 0.054) -> B > G > R.
// A 4-9% reflector is ASPHALT, not a wall; and the albedo's blue tilt (B/R = 1.8)
// OVERTURNS the tungsten key's warm tilt (R/B = 1.6), so the surface reads BLUE
// under a warm lamp. That is why the hue "proved" the wall got no point light: the
// tell lied, because the albedo was the liar.
// These tints were authored against the 0.42/0.44/0.50 BLUE AMBIENT WASH (KNOWN_BUGS
// R2) — a blue wall in a blue flood looks neutral. Kill the wash and the crutch walks
// out. NEUTRALISED: the wall's value + hue now come from its texture and the lamp on
// it, which is the only honest source for either. No light was touched.
const float kWallTint[4]  = { 1.00f, 1.00f, 1.00f, 1.0f };
const float kShaftTint[4] = { 0.85f, 0.85f, 0.85f, 1.0f };   // shaft reads a shade darker; NOT bluer
const float kStairTint[4] = { 0.90f, 0.90f, 0.90f, 1.0f };

// ---- THE EMERGENCY STAIRWELL LAYOUT (see level1.h SpireStair) ---------------------
// Pure arithmetic, no device: buildLevel1 renders it and --test-levellint measures it.
//
// D19/D20/D21 (QA upper-floors sweep, 2026-07-27). What shipped before was not a
// stairwell: each "step" was a solid COLUMN from the B1 floor (y=0) up to its own top,
// every floor transition reused the SAME 4 m of X, and no well was ever cut. Measured
// by the new SPIRE lint: 182 step columns, **920 interpenetrating pairs** (LAW 2
// doubled faces — a z-fighting mass), **1423 slab/lid crossings** (LAW 2: driven
// straight through every floor plate and ceiling lid on F1-F6), risers of 0.50 m and
// treads down to 0.057 m on the 35 m F4->F5 gap (LAW 3: unclimbable — it was a wall).
// The net effect on EVERY upper floor was a 4 x 2.8 m grey mass standing in the plate
// at (x 10-14, z 15) that shimmered and could not be walked.
//
// REBUILT as the doctrine's legal vocabulary (x3-level-authoring LAW 3): a SWITCHBACK
// of <= 30 deg RAMP flights broken by level landings every <= 3 m of rise, inside a
// walled well that is CUT OUT of every floor slab and ceiling lid it passes through
// (LAW 2), with a 1.2 m doorway onto each floor's arrival pad (LAW 1).
constexpr float kStairWellX0 = 8.5f,  kStairWellX1 = 15.5f;   // well footprint (X), 7 m
constexpr float kStairWellZ0 = 12.5f, kStairWellZ1 = 17.5f;   // well footprint (Z), 5 m
constexpr float kStairPad    = 1.0f;    // turn-landing depth at each end of the run
constexpr float kStairPadT   = 0.25f;   // landing slab thickness
constexpr float kStairSoffit = 0.20f;   // closed underside beneath each ramp wedge
constexpr float kStairDoorZ  = 15.0f;   // doorway center Z on the -X enclosure face
constexpr float kStairFlightRise = 2.5f;  // target rise per flight (slope + headroom)
constexpr float kStairWallT  = 0.20f;

SpireStair buildSpireStair() {
    SpireStair S;
    S.baseY  = kFloors[0].y0;
    S.topY   = kFloors[(uint32_t)L1Floor::F7].y0 + 2.5f;   // a head above the top arrival
    S.doorZ  = kStairDoorZ;
    S.wellX0 = kStairWellX0; S.wellX1 = kStairWellX1;
    S.wellZ0 = kStairWellZ0; S.wellZ1 = kStairWellZ1;

    // Two run lanes (the switchback's up-leg and its return leg) and the turn pads.
    const float runX0 = kStairWellX0 + kStairPad;     //  9.5
    const float runX1 = kStairWellX1 - kStairPad;     // 14.5
    const float run   = runX1 - runX0;                //  5.0 m
    const float zMid  = (kStairWellZ0 + kStairWellZ1) * 0.5f;
    const float laneZ[2][2] = { { kStairWellZ0, zMid }, { zMid, kStairWellZ1 } };

    for (uint32_t fi = 0; fi + 1 < (uint32_t)L1Floor::Count; ++fi) {
        const float baseY = kFloors[fi].y0;
        const float gap   = kFloors[fi + 1].y0 - baseY;
        // An EVEN flight count means the last leg always returns to the WEST pad —
        // the side the doorway is on — so every floor is entered off the same landing.
        int nFlights = (int)std::lround(gap / kStairFlightRise);
        if (nFlights < 2) nFlights = 2;
        if (nFlights & 1) ++nFlights;
        const float rise = gap / (float)nFlights;
        for (int f = 0; f < nFlights; ++f) {
            const bool east = ((f & 1) == 0);         // even legs climb toward +X
            const float y0 = baseY + (float)f * rise;
            const float y1 = y0 + rise;
            const int   ln = (f & 1);
            SpireStair::Flight fl;
            fl.baseY = y0; fl.topY = y1; fl.axis = 0; fl.dir = east ? 1.0f : -1.0f;
            fl.ramp  = true;
            fl.solid = { runX0, runX1, laneZ[ln][0], laneZ[ln][1], y0, y1 };
            S.flights.push_back(std::move(fl));
            // Closed underside (makeRamp emits no bottom face — from below a bare wedge
            // is see-through). Skipped on the bottom flight, which rests on the B1 slab.
            if (y0 > S.baseY + 0.01f)
                S.soffits.push_back({ runX0, runX1, laneZ[ln][0], laneZ[ln][1],
                                      y0 - kStairSoffit, y0 });
            // Turn landing at the top of this leg (the last one IS the floor's arrival).
            const float px0 = east ? runX1 : kStairWellX0;
            const float px1 = east ? kStairWellX1 : runX0;
            S.landings.push_back({ px0, px1, kStairWellZ0, kStairWellZ1, y1 - kStairPadT, y1 });
        }
        S.arrivalY.push_back(kFloors[fi + 1].y0);
    }

    // ---- Enclosure (LAW 2 containment: the well is a hole in seven floor slabs; the
    // player must not be able to walk into it except through a doorway). The two
    // Z-faces span the full X including the corners; the X-faces stop at the well so
    // no two wall boxes ever overlap. The -X face carries the per-floor doorway.
    const float wx0 = kStairWellX0 - kStairWallT, wx1 = kStairWellX1 + kStairWallT;
    const float wz0 = kStairWellZ0 - kStairWallT, wz1 = kStairWellZ1 + kStairWallT;
    S.walls.push_back({ wx0, wx1, wz0, kStairWellZ0, S.baseY, S.topY });   // -Z face
    S.walls.push_back({ wx0, wx1, kStairWellZ1, wz1, S.baseY, S.topY });   // +Z face
    S.walls.push_back({ kStairWellX1, wx1, kStairWellZ0, kStairWellZ1, S.baseY, S.topY });  // +X face
    // -X face: two full-height side bands plus the header/spandrel stack between the
    // per-floor doorways (1.2 m wide, 2.1 m head clearance).
    const float dz0 = kStairDoorZ - kDoorHalf, dz1 = kStairDoorZ + kDoorHalf;
    S.walls.push_back({ wx0, kStairWellX0, kStairWellZ0, dz0, S.baseY, S.topY });
    S.walls.push_back({ wx0, kStairWellX0, dz1, kStairWellZ1, S.baseY, S.topY });
    for (uint32_t fi = 0; fi < (uint32_t)L1Floor::Count; ++fi) {
        const float head = kFloors[fi].y0 + kLintelBottom;
        const float next = (fi + 1 < (uint32_t)L1Floor::Count) ? kFloors[fi + 1].y0 : S.topY;
        if (next > head + 0.01f)
            S.walls.push_back({ wx0, kStairWellX0, dz0, dz1, head, next });
    }
    return S;
}

// Add one world-baked graybox box (render mesh + optional static collision) to the
// scene. `visible`: when false the render mesh is omitted (collision-only) so real
// GLB art can be drawn over this volume without z-fighting (EFLZ art pass).
uint32_t addBox(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
                float hx, float hy, float hz, float cx, float cy, float cz,
                x3::rhi::TextureHandle tex, const float color[4],
                uint32_t tag = (uint32_t)Tag::Static, bool collide = true,
                bool visible = true) {
    x3::prims::PrimMesh geo = x3::prims::makeBox(hx, hy, hz, cx, cy, cz, 0.5f);
    Entity e;
    if (visible)
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
    e.tex = tex;
    e.baseColor[0] = color[0]; e.baseColor[1] = color[1];
    e.baseColor[2] = color[2]; e.baseColor[3] = color[3];
    for (int i = 0; i < 16; ++i) e.transform[i] = kIdentity[i];
    e.tag = tag;
    e.visible = visible;
    if (collide)
        e.body = physics.addStaticMesh(geo.cverts.data(), (uint32_t)(geo.cverts.size() / 3),
                                       geo.cindex.data(), (uint32_t)geo.cindex.size());
    return scene.add(e);
}

// Floor slab for a plate (thin slab whose TOP surface is flush with floorY).
void addFloor(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
              float cx, float cz, float hx, float hz, float floorY,
              x3::rhi::TextureHandle tex, const float color[4], bool visible = true) {
    addBox(scene, device, physics, hx, 0.05f, hz, cx, floorY - 0.05f, cz, tex, color,
           (uint32_t)Tag::Static, /*collide*/true, visible);
}

// A rectangular opening to subtract from a slab / lid (world XZ).
struct SlabHole { float x0, x1, z0, z1; };

// Tile a horizontal slab (floor or ceiling lid) MINUS a list of rectangular holes.
// Rect subtraction: the first hole that bites the rect splits it into up to 4 pieces,
// each re-tested against the REMAINING holes (holes already passed cannot bite a
// subset of a rect they did not bite). Used for the B1 cell trapdoor and — since the
// QA upper-floors sweep (D19) — the emergency stairwell well, which must be open
// through every slab and ceiling lid it passes rather than driven through them.
void addSlabMinusHoles(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
                       float x0, float x1, float z0, float z1,
                       float cy, float halfY,
                       x3::rhi::TextureHandle tex, const float color[4],
                       bool visible, const SlabHole* holes, uint32_t nHoles,
                       bool collide = true) {
    if (x1 - x0 < 0.01f || z1 - z0 < 0.01f) return;
    for (uint32_t i = 0; i < nHoles; ++i) {
        const float ix0 = std::max(x0, holes[i].x0), ix1 = std::min(x1, holes[i].x1);
        const float iz0 = std::max(z0, holes[i].z0), iz1 = std::min(z1, holes[i].z1);
        if (ix1 - ix0 <= 0.001f || iz1 - iz0 <= 0.001f) continue;   // this hole misses
        const SlabHole* rest = holes + i + 1;
        const uint32_t  nRest = nHoles - i - 1;
        addSlabMinusHoles(scene, device, physics, x0,  ix0, z0,  z1,  cy, halfY, tex, color, visible, rest, nRest, collide);
        addSlabMinusHoles(scene, device, physics, ix1, x1,  z0,  z1,  cy, halfY, tex, color, visible, rest, nRest, collide);
        addSlabMinusHoles(scene, device, physics, ix0, ix1, z0,  iz0, cy, halfY, tex, color, visible, rest, nRest, collide);
        addSlabMinusHoles(scene, device, physics, ix0, ix1, iz1, z1,  cy, halfY, tex, color, visible, rest, nRest, collide);
        return;
    }
    addBox(scene, device, physics, (x1 - x0) * 0.5f, halfY, (z1 - z0) * 0.5f,
           (x0 + x1) * 0.5f, cy, (z0 + z1) * 0.5f, tex, color,
           (uint32_t)Tag::Static, collide, visible);
}

// A solid wall running along X (its plane is z = const). Spans x in [x0,x1] and
// rises from floorY to floorY+wallH (floor-to-ceiling on this plate).
void addWallX(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
              float x0, float x1, float z, float floorY, float wallH,
              x3::rhi::TextureHandle tex, const float color[4], bool visible = true) {
    const float hx = (x1 - x0) * 0.5f, cx = (x0 + x1) * 0.5f;
    addBox(scene, device, physics, hx, wallH * 0.5f, kWallT * 0.5f, cx, floorY + wallH * 0.5f, z,
           tex, color, (uint32_t)Tag::Static, /*collide*/true, visible);
}

// A cross-wall running along Z (its plane is x = const), spanning z in [z0,z1] and
// rising from floorY to floorY+wallH, optionally WITH a 1.2 m doorway gap centered
// at z=zDoor (the wall above the doorway lintel stays solid to the ceiling). If
// withDoorway is false, builds a fully solid wall (end cap).
void addCrossWall(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
                  float x, float z0, float z1, float zDoor, bool withDoorway, float floorY,
                  float wallH, x3::rhi::TextureHandle tex, const float color[4], bool visible = true) {
    if (!withDoorway) {
        const float hz = (z1 - z0) * 0.5f, cz = (z0 + z1) * 0.5f;
        addBox(scene, device, physics, kWallT * 0.5f, wallH * 0.5f, hz, x, floorY + wallH * 0.5f, cz,
               tex, color, (uint32_t)Tag::Static, /*collide*/true, visible);
        return;
    }
    // Left segment: z in [z0, zDoor - kDoorHalf]
    {
        const float lo = z0, hi = zDoor - kDoorHalf;
        if (hi > lo) {
            const float hz = (hi - lo) * 0.5f, cz = (lo + hi) * 0.5f;
            addBox(scene, device, physics, kWallT * 0.5f, wallH * 0.5f, hz, x, floorY + wallH * 0.5f, cz,
                   tex, color, (uint32_t)Tag::Static, /*collide*/true, visible);
        }
    }
    // Right segment: z in [zDoor + kDoorHalf, z1]
    {
        const float lo = zDoor + kDoorHalf, hi = z1;
        if (hi > lo) {
            const float hz = (hi - lo) * 0.5f, cz = (lo + hi) * 0.5f;
            addBox(scene, device, physics, kWallT * 0.5f, wallH * 0.5f, hz, x, floorY + wallH * 0.5f, cz,
                   tex, color, (uint32_t)Tag::Static, /*collide*/true, visible);
        }
    }
    // Solid header above the doorway, lintel-bottom up to the ceiling.
    {
        const float lh = (wallH - kLintelBottom) * 0.5f;
        const float lcy = floorY + kLintelBottom + lh;
        if (lh > 0.0f)
            addBox(scene, device, physics, kWallT * 0.5f, lh, kDoorHalf, x, lcy, zDoor, tex, color,
                   (uint32_t)Tag::Static, /*collide*/true, visible);
    }
}

// (The plate ceiling lid is built by addSlabMinusHoles — collision-only + invisible,
// the GLB ceiling panels from env_art provide the visible ceiling — so it can carry
// the stairwell well cutout like the floor slabs do.)

// ---- Detention-room interior builder (Floor 1). Each room is an axis-aligned box;
// we build its 4 perimeter walls (running along Z at x=cx±w/2, and along X at z=cz±d/2)
// to the room's ceiling height. Where a CONNECTED neighbor (per the door list) abuts a
// given face (its box touches/overlaps that face's plane within the shared span), that
// wall gets a 1.2 m doorway gap centered on the overlap so the rooms link. The shared
// graybox helpers (addCrossWall / addWallX) handle the gap + lintel. Adjacent rooms
// each build their own wall (a thin double-wall on shared faces) — graybox-acceptable.

// True if rooms a,b are listed as connected in the detention door table.
bool detConnected(const uint32_t* doors, uint32_t pairs, uint32_t a, uint32_t b) {
    for (uint32_t i = 0; i < pairs; ++i) {
        uint32_t r0 = doors[2*i], r1 = doors[2*i+1];
        if ((r0 == a && r1 == b) || (r0 == b && r1 == a)) return true;
    }
    return false;
}

// The legacy Awakening-spine corridor carve-out (B1 only): the z=0 playable lane
// x in [carveX0,carveX1], z in [-carveZ,carveZ]. Detention room walls that fall inside
// this lane are suppressed / given a z=0 doorway so the corridor passes through any
// room in its way (Jake's Cell, Guard Station, Creepy Passage), keeping the Awakening
// route clear while the rooms' other walls stay. Set carveX1 < carveX0 to disable.
struct SpineCarve { float x0, x1, zHalf; };

// Build the 4 walls of one detention room `ri`, cutting a doorway on any face that a
// connected neighbor abuts. `floorY` is the room floor (world); `ceilH` the (clamped)
// ceiling height. `carve` suppresses walls inside the legacy spine lane (B1).
void buildDetentionRoom(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
                        const L1DetentionRoom* rooms, uint32_t roomCount,
                        const uint32_t* doors, uint32_t pairs, uint32_t ri,
                        float floorY, float ceilH,
                        x3::rhi::TextureHandle wallTex, const float tint[4], bool vis,
                        const SpineCarve& carve) {
    const L1DetentionRoom& r = rooms[ri];
    const float x0 = r.cx - r.w * 0.5f, x1 = r.cx + r.w * 0.5f;
    const float z0 = r.cz - r.d * 0.5f, z1 = r.cz + r.d * 0.5f;
    const bool carveOn = (carve.x1 > carve.x0);
    // Does this room's footprint overlap the spine lane (so the corridor runs through)?
    const bool inLane = carveOn && (x1 > carve.x0 && x0 < carve.x1 &&
                                    z1 > -carve.zHalf && z0 < carve.zHalf);
    // A Z-wall (at x=xFace) lies inside the lane x-span (so it would block the corridor).
    auto zWallInLane = [&](float xFace) {
        return carveOn && xFace > carve.x0 - 0.01f && xFace < carve.x1 + 0.01f;
    };
    // An X-wall (at z=zFace) lies inside the lane (so it would block the corridor).
    auto xWallInLane = [&](float zFace) {
        return carveOn && zFace > -carve.zHalf - 0.01f && zFace < carve.zHalf + 0.01f &&
               x1 > carve.x0 && x0 < carve.x1;
    };

    // For a face, find a connected neighbor abutting it and return the doorway center
    // coordinate (along the wall run) inside the overlap; returns false if no doorway.
    auto doorwayOnZWall = [&](float xFace, float& outZDoor) -> bool {  // wall at x=xFace (runs along Z)
        for (uint32_t j = 0; j < roomCount; ++j) {
            if (j == ri || !detConnected(doors, pairs, ri, j)) continue;
            const L1DetentionRoom& n = rooms[j];
            const float nx0 = n.cx - n.w * 0.5f, nx1 = n.cx + n.w * 0.5f;
            const float nz0 = n.cz - n.d * 0.5f, nz1 = n.cz + n.d * 0.5f;
            if (nx0 - 0.5f <= xFace && xFace <= nx1 + 0.5f) {     // neighbor spans this x plane
                const float lo = std::max(z0, nz0), hi = std::min(z1, nz1);
                if (hi - lo >= 1.3f) { outZDoor = (lo + hi) * 0.5f; return true; }
            }
        }
        return false;
    };
    auto doorwayOnXWall = [&](float zFace, float& outXDoor) -> bool {  // wall at z=zFace (runs along X)
        for (uint32_t j = 0; j < roomCount; ++j) {
            if (j == ri || !detConnected(doors, pairs, ri, j)) continue;
            const L1DetentionRoom& n = rooms[j];
            const float nx0 = n.cx - n.w * 0.5f, nx1 = n.cx + n.w * 0.5f;
            const float nz0 = n.cz - n.d * 0.5f, nz1 = n.cz + n.d * 0.5f;
            if (nz0 - 0.5f <= zFace && zFace <= nz1 + 0.5f) {     // neighbor spans this z plane
                const float lo = std::max(x0, nx0), hi = std::min(x1, nx1);
                if (hi - lo >= 1.3f) { outXDoor = (lo + hi) * 0.5f; return true; }
            }
        }
        return false;
    };

    float dCoord;
    // -X / +X walls (run along Z). If the wall sits inside the spine lane, SUPPRESS it
    // entirely — the legacy spine's own partition walls (at the door X positions) gate
    // the Awakening corridor, so a doubled detention Z-wall here would just block it.
    auto buildZWall = [&](float xFace) {
        if (zWallInLane(xFace))
            return;
        else if (doorwayOnZWall(xFace, dCoord))
            addCrossWall(scene, device, physics, xFace, z0, z1, dCoord, true, floorY, ceilH, wallTex, tint, vis);
        else
            addCrossWall(scene, device, physics, xFace, z0, z1, 0.0f, false, floorY, ceilH, wallTex, tint, vis);
    };
    buildZWall(x0);
    buildZWall(x1);
    // -Z / +Z walls (run along X). addWallX has no doorway variant; split manually. If
    // the wall lies inside the spine lane, suppress the segment within the lane x-span
    // (the corridor's own long walls confine it) so the corridor runs through.
    auto wallXWithDoor = [&](float zFace) {
        const bool laneCut = xWallInLane(zFace);
        float xd; bool haveDoor = doorwayOnXWall(zFace, xd);
        // Build a sub-segment [a,b] of this X-wall, skipping the lane carve-out.
        auto seg = [&](float a, float b) {
            if (b - a < 0.05f) return;
            if (laneCut) {
                const float cl = std::max(a, carve.x0), cr = std::min(b, carve.x1);
                if (cr > cl) {   // overlaps the lane: build the parts outside it
                    if (carve.x0 > a) addWallX(scene, device, physics, a, carve.x0, zFace, floorY, ceilH, wallTex, tint, vis);
                    if (b > carve.x1) addWallX(scene, device, physics, carve.x1, b, zFace, floorY, ceilH, wallTex, tint, vis);
                    return;
                }
            }
            addWallX(scene, device, physics, a, b, zFace, floorY, ceilH, wallTex, tint, vis);
        };
        if (haveDoor) {
            seg(x0, xd - kDoorHalf);
            seg(xd + kDoorHalf, x1);
            const float lh = (ceilH - kLintelBottom) * 0.5f;
            if (lh > 0.0f)
                addBox(scene, device, physics, kDoorHalf, lh, kWallT * 0.5f, xd,
                       floorY + kLintelBottom + lh, zFace, wallTex, tint,
                       (uint32_t)Tag::Static, true, vis);
        } else {
            seg(x0, x1);
        }
    };
    wallXWithDoor(z0);
    wallXWithDoor(z1);
    (void)inLane;
}

} // namespace

Level1Layout buildLevel1(Scene& scene,
                         x3::rhi::IRenderDevice& device,
                         x3::phys::IPhysicsWorld& physics,
                         const Level1ArtMask& artMask) {
    x3::logInfo(std::string("buildLevel1: EFLZ 'The Spire' — vertical B1->F7 stack (8 floors, 5 m pitch)")
                + std::string(artMask.walls ? " [GLB walls]" : "")
                + std::string(artMask.floors ? " [GLB floors]" : ""));
    const bool wallVis  = !artMask.walls;   // graybox SIDE-wall render on iff no GLB wall art
    const bool crossWallVis = true;         // cross-walls aren't covered by GLB art (no see-through)
    const bool floorVis = !artMask.floors;  // graybox floor render on iff no GLB floor art

    // ---- Shared graybox textures. ----
    // RICHER PROCEDURAL SCI-FI surfaces (S2 art uplift) replacing the old flat blue/
    // grey CHECKER. These are NEUTRAL/untinted maps: the per-floor + per-surface tint
    // (kFloorTints / kWallTint / detTint / kShaftTint, passed as the entity baseColor)
    // is MULTIPLIED over them by the mesh shader (mesh.frag: albedo = texture * vFactor),
    // so each plate still reads as a distinct wing while every surface now shows real
    // detention-facility detail instead of a checkerboard. 512px + mips for crispness;
    // all three generators are SEAMLESS so they tile across the big plates.
    //   - FLOORS  : dark grated/tiled deck with seams + tread + edge hazard trim.
    //   - WALLS   : gunmetal inset metal panels (seam grooves, corner bolts, grime) +
    //               a faint cool emissive accent conduit line.
    //   - CEILINGS: recessed panel coffers with a soft central light-fixture motif.
    // FUTURE (real PBR): swap any of these for SD-3.5-generated tiling albedo PNGs —
    // generate via the diffusers script (model C:\GameDev\SD_Models\sd35), save under
    // assets/textures/, load with stbi_load, feed the RGBA8 to createTexture here.
    constexpr uint32_t kTexN = 512;
    // FLOOR: BIG top-down deck plates (2x2 → large plates, deliberately a LARGER scale
    // than the wall panels so the floor reads as a walkable deck, not a wall). Hazard
    // trim is OFF for the main fill: the texture tiles many times across each big plate,
    // so an edge-of-texture caution band would repeat on every tile (a loud yellow grid).
    // The deep seams + diamond tread already read unmistakably as a deck.
    // D22 (QA upper-floors sweep): the deck map as authored is a **3.2% reflector** —
    // darker than asphalt, the exact sr_rubberfloor pathology surface_library.h was
    // written about, and the per-floor identity tints only ever DARKEN it further
    // (measured effective albedo 0.018-0.026 LINEAR on all eight plates, against an
    // interior band of 0.08-0.40). A light lands on the Spire's floors and nothing
    // comes back. Lift the MAP itself by a neutral, hue-preserving factor at generation
    // time (kept in sRGB bytes, so no baseColor ever exceeds 1) — the identity tints
    // then land every plate deck inside the band. --test-levellint's SPIRE-VALUE probe
    // reads level1DeckMapLift() so the gate and the world can never disagree.
    auto floorPx = x3::prims::makeFloorGrateRGBA(kTexN, /*tiles*/2, level1DeckMapLift(), /*hazard*/false);
    x3::rhi::TextureHandle floorTex = device.createTexture(floorPx.data(), kTexN, kTexN, true);
    // WALLS: three calm, large-scale (2x2 panel) variants so adjacent corridor surfaces
    // don't read as one repeating tile (the "all walls identical" complaint). The accent
    // conduit line is kept subtle. Variant A = plain panel (default everywhere a specific
    // variant isn't requested), B = floor-to-ceiling cable conduit, C = louvered vent.
    auto wallPxA = x3::prims::makeSciFiPanelRGBA(kTexN, /*panels*/2, x3::prims::detail::kNoTint,
                                                 /*accent*/60, 170, 200, /*accentH*/0.16f,
                                                 x3::prims::WallVariant::Plain);
    x3::rhi::TextureHandle wallTexA = device.createTexture(wallPxA.data(), kTexN, kTexN, true);
    auto wallPxB = x3::prims::makeSciFiPanelRGBA(kTexN, /*panels*/2, x3::prims::detail::kNoTint,
                                                 /*accent*/60, 170, 200, /*accentH*/0.0f,
                                                 x3::prims::WallVariant::Conduit);
    x3::rhi::TextureHandle wallTexB = device.createTexture(wallPxB.data(), kTexN, kTexN, true);
    auto wallPxC = x3::prims::makeSciFiPanelRGBA(kTexN, /*panels*/2, x3::prims::detail::kNoTint,
                                                 /*accent*/60, 170, 200, /*accentH*/0.0f,
                                                 x3::prims::WallVariant::Vent);
    x3::rhi::TextureHandle wallTexC = device.createTexture(wallPxC.data(), kTexN, kTexN, true);
    // The 3 wall variants in a small table; surfaces pick one by index so neighbors differ.
    const x3::rhi::TextureHandle wallVariants[3] = { wallTexA, wallTexB, wallTexC };
    // Default wall handle (the plain variant) for the few surfaces that don't vary.
    x3::rhi::TextureHandle wallTex = wallTexA;
    auto ceilPx = x3::prims::makeCeilingPanelRGBA(kTexN, /*coffers*/3, x3::prims::detail::kNoTint, /*lit*/true);
    x3::rhi::TextureHandle ceilTex = device.createTexture(ceilPx.data(), kTexN, kTexN, true);

    Level1Layout L;

    // The stairwell layout is resolved BEFORE the plates: every floor slab and ceiling
    // lid is built with its well cut out (D19).
    const SpireStair& stair = spireStair();
    // Neutral value-normalized deck tint for the stair's own walking surfaces (they
    // span every floor, so they carry no floor identity hue).
    const float deckTint[4] = { 0.72f, 0.72f, 0.72f, 1.0f };

    // ===================================================================
    // 1) FLOOR PLATES — for each floor: floor slab, the 4 SOLID perimeter walls (two
    //    long side walls along X at z=±zHalf, plus the -X and +X end caps), and a
    //    ceiling lid. The plate is the real ~75x43 m detention footprint (grown from
    //    the old 24x16 placeholder). The elevator shaft is now an INTERIOR free-
    //    standing column (built in §2) at (kShaftCx,kShaftCz) with its own -X doorway
    //    opening straight into the plate, so the perimeter end caps are fully solid.
    // ===================================================================
    for (uint32_t fi = 0; fi < (uint32_t)L1Floor::Count; ++fi) {
        const L1RoomDef& f = kFloors[fi];
        const float cx = (f.x0 + f.x1) * 0.5f;
        const float* tint = kFloorTints[fi];
        const bool isRooftop = (fi == (uint32_t)L1Floor::F7);

        // Floor slab (every floor including B1; the rooftop still has a deck). B1 gets
        // a HOLE carved under Jake's cell for the code-locked trapdoor (secret_room.*),
        // so an open hatch actually drops the player into the secret room below. Every
        // floor ABOVE B1 gets the stairwell well cut out of it (D19) — B1's slab is the
        // bottom of that well, so it stays solid.
        {
            SlabHole holes[2];
            uint32_t nh = 0;
            if (fi == (uint32_t)L1Floor::B1)
                holes[nh++] = { kCellHatchCx - kCellHatchHalf, kCellHatchCx + kCellHatchHalf,
                                kCellHatchCz - kCellHatchHalf, kCellHatchCz + kCellHatchHalf };
            else
                holes[nh++] = { stair.wellX0, stair.wellX1, stair.wellZ0, stair.wellZ1 };
            addSlabMinusHoles(scene, device, physics, f.x0, f.x1, -f.zHalf, f.zHalf,
                              f.y0 - 0.05f, 0.05f, floorTex, tint, floorVis, holes, nh);
            // APRON: env_art skips every GLB floor tile the well touches (otherwise it
            // paints a walk-through-able floor over the shaft). Lay the graybox deck
            // back over exactly those skipped tiles, minus the well — render only, the
            // slab above already carries the collision.
            if (artMask.floors && fi != (uint32_t)L1Floor::B1) {
                const SpireStair::Box sp = spireWellTileSpan(fi);
                const SlabHole wellHole{ stair.wellX0, stair.wellX1, stair.wellZ0, stair.wellZ1 };
                addSlabMinusHoles(scene, device, physics, sp.x0, sp.x1, sp.z0, sp.z1,
                                  f.y0 - 0.05f, 0.05f, floorTex, tint, /*visible*/true,
                                  &wellHole, 1, /*collide*/false);
            }
        }
        // Two long side walls (z = ±zHalf), floor-to-ceiling. Pick DIFFERENT wall
        // variants for the two facing side walls AND stagger by floor so no two adjacent
        // corridor walls read as the same tile (fixes the "all walls identical" look).
        const x3::rhi::TextureHandle sideZneg = wallVariants[fi % 3];
        const x3::rhi::TextureHandle sideZpos = wallVariants[(fi + 1) % 3];
        addWallX(scene, device, physics, f.x0, f.x1, -f.zHalf, f.y0, f.ceil, sideZneg, tint, wallVis);
        addWallX(scene, device, physics, f.x0, f.x1,  f.zHalf, f.y0, f.ceil, sideZpos, tint, wallVis);
        // -X and +X end caps (now fully SOLID perimeter walls — the shaft is interior).
        // Give the two end caps the remaining two variants so all four perimeter walls
        // of a plate differ from one another.
        addCrossWall(scene, device, physics, f.x0, -f.zHalf, f.zHalf, 0.0f,
                     /*withDoorway*/false, f.y0, f.ceil, wallVariants[(fi + 2) % 3], kWallTint, crossWallVis);
        addCrossWall(scene, device, physics, f.x1, -f.zHalf, f.zHalf, 0.0f,
                     /*withDoorway*/false, f.y0, f.ceil, wallVariants[fi % 3], kWallTint, crossWallVis);
        // Ceiling lid (skip the rooftop: F7 is open to the sky). Collision-only, and
        // holed at the stair well so the stairwell is a continuous shaft (D19).
        if (!isRooftop) {
            const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            const SlabHole lidHole{ stair.wellX0, stair.wellX1, stair.wellZ0, stair.wellZ1 };
            addSlabMinusHoles(scene, device, physics, f.x0, f.x1, -f.zHalf, f.zHalf,
                              f.y0 + f.ceil + kCeilT * 0.5f, kCeilT * 0.5f,
                              ceilTex, white, /*visible*/false, &lidHole, 1);
            // Ceiling apron over the tiles env_art skipped (same contract as the floor).
            if (artMask.ceilings) {
                const SpireStair::Box sp = spireWellTileSpan(fi);
                addSlabMinusHoles(scene, device, physics, sp.x0, sp.x1, sp.z0, sp.z1,
                                  f.y0 + f.ceil + kCeilT * 0.5f, kCeilT * 0.5f,
                                  ceilTex, white, /*visible*/true, &lidHole, 1, /*collide*/false);
            }
        }

        // Fill the per-floor layout result.
        L.floorBaseY[fi]  = f.y0;
        L.floorCeil[fi]   = f.ceil;
        L.floorCenter[fi] = x3::phys::Vec3{ cx, f.y0, 0.0f };
        // Elevator doorway: the shaft's -X face at z=0 on this floor (consistent XZ up
        // the whole tower so a ride always lands on walkable floor at the shaft mouth).
        L.elevatorDoor[fi] = x3::phys::Vec3{ kShaftX0, f.y0, 0.0f };
    }

    // ===================================================================
    // 2) CENTRAL ELEVATOR SHAFT — a vertical hollow column at (kShaftCx,kShaftCz)
    //    spanning the full tower height. Two side walls (z=±kShaftHz) and a +X back
    //    wall enclose it; the -X face is open per floor (the doorway into the plate,
    //    already a gap in each floor's +X end cap at z=0). The cab platform itself
    //    is built by the host (main.cpp) so it can be animated. The shaft top is
    //    capped above F7 so the cab can't fly off.
    // ===================================================================
    const float shaftBottom = kFloors[(uint32_t)L1Floor::B1].y0;
    const float shaftTop    = kFloors[(uint32_t)L1Floor::F7].y0 + kFloors[(uint32_t)L1Floor::F7].ceil;
    const float shaftH      = shaftTop - shaftBottom;
    // Shaft side walls (run along X, plane z=±kShaftHz), from bottom to top.
    addBox(scene, device, physics, kShaftHx, shaftH * 0.5f, kWallT * 0.5f,
           kShaftCx, shaftBottom + shaftH * 0.5f, kShaftCz - kShaftHz,
           wallTex, kShaftTint, (uint32_t)Tag::Static, true, crossWallVis);
    addBox(scene, device, physics, kShaftHx, shaftH * 0.5f, kWallT * 0.5f,
           kShaftCx, shaftBottom + shaftH * 0.5f, kShaftCz + kShaftHz,
           wallTex, kShaftTint, (uint32_t)Tag::Static, true, crossWallVis);
    // Shaft +X back wall (plane x=kShaftCx+kShaftHx), full height.
    addBox(scene, device, physics, kWallT * 0.5f, shaftH * 0.5f, kShaftHz,
           kShaftCx + kShaftHx, shaftBottom + shaftH * 0.5f, kShaftCz,
           wallTex, kShaftTint, (uint32_t)Tag::Static, true, crossWallVis);
    // Shaft top cap (collision lid above F7's ceiling).
    addBox(scene, device, physics, kShaftHx, kCeilT * 0.5f, kShaftHz,
           kShaftCx, shaftTop + kCeilT * 0.5f, kShaftCz,
           wallTex, kShaftTint, (uint32_t)Tag::Static, true, /*visible*/false);

    // ===================================================================
    // 3) EMERGENCY STAIRWELL — a straight ramp column linking every adjacent pair of
    //    floors. With the REAL non-uniform pitch the inter-floor gap VARIES (5/5/10/10/
    //    35/13/13 m), so each transition's run is split into a number of steps PROPORTIONAL
    //    to its gap (a fixed ~0.5 m rise/step), and the top step always lands EXACTLY on
    //    the next floor's plate — keeping the stairs a connected, walkable ramp regardless
    //    of pitch (the elevator is the primary path; these are reachability stairs). Placed
    //    in a NORTH BAND (x in [10,14], z=15) that is inside EVERY floor's plate — B1's big
    //    detention plate (rooms all at z<=9.5) AND the resized F2-F7 plates (zHalf>=22) —
    //    and clear of the elevator shaft (x=21,z=0) + all encounter content (z in
    //    [-6.5,6.5]). Purely collision graybox + tint.
    // ===================================================================
    {
        const SpireStair& S = spireStair();
        auto emit = [&](const SpireStair::Box& b, bool vis) {
            addBox(scene, device, physics, (b.x1 - b.x0) * 0.5f, (b.y1 - b.y0) * 0.5f, (b.z1 - b.z0) * 0.5f,
                   (b.x0 + b.x1) * 0.5f, (b.y0 + b.y1) * 0.5f, (b.z0 + b.z1) * 0.5f,
                   floorTex, deckTint, (uint32_t)Tag::Static, /*collide*/true, vis);
        };
        // The RAMP flights (makeRamp wedges — the D13 winding orient applies, so the
        // walking surface survives backface culling). Deck-surfaced like every other
        // walkable plane in the tower (the D10 law: ramps wear the floor set, not the
        // wall set on the unnormalized route).
        for (const SpireStair::Flight& fl : S.flights) {
            const float rise = fl.topY - fl.baseY;
            const float runL = fl.solid.x1 - fl.solid.x0;
            const float cz   = (fl.solid.z0 + fl.solid.z1) * 0.5f;
            const float cx   = (fl.dir > 0.0f) ? fl.solid.x0 : fl.solid.x1;
            x3::prims::PrimMesh geo = x3::prims::makeRamp(cx, fl.baseY, cz,
                                                          (fl.solid.z1 - fl.solid.z0) * 0.5f,
                                                          runL, rise, fl.axis, fl.dir, 0.5f);
            Entity e;
            e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                       geo.index.data(), (uint32_t)geo.index.size());
            e.tex = floorTex;
            for (int i = 0; i < 4; ++i) e.baseColor[i] = deckTint[i];
            for (int i = 0; i < 16; ++i) e.transform[i] = kIdentity[i];
            e.tag = (uint32_t)Tag::Static;
            e.visible = true;
            e.body = physics.addStaticMesh(geo.cverts.data(), (uint32_t)(geo.cverts.size() / 3),
                                           geo.cindex.data(), (uint32_t)geo.cindex.size());
            scene.add(e);
            for (const SpireStair::Box& b : fl.steps) emit(b, /*visible*/true);
        }
        for (const SpireStair::Box& b : S.landings) emit(b, /*visible*/true);
        for (const SpireStair::Box& b : S.soffits) emit(b, /*visible*/true);
        for (const SpireStair::Box& b : S.walls)
            addBox(scene, device, physics, (b.x1 - b.x0) * 0.5f, (b.y1 - b.y0) * 0.5f, (b.z1 - b.z0) * 0.5f,
                   (b.x0 + b.x1) * 0.5f, (b.y0 + b.y1) * 0.5f, (b.z0 + b.z1) * 0.5f,
                   wallTex, kStairTint, (uint32_t)Tag::Static, /*collide*/true, crossWallVis);
    }

    // ===================================================================
    // 4) F2-F7 PER-FLOOR IDENTITY INTERIORS (collision graybox). Each floor now has its
    //    OWN real-scale plate (kFloors). The EASTERN third (x in [-2,25]) is kept OPEN as
    //    the arrival + combat hall where the spire_mid/top encounters play (content is
    //    authored at absolute x in [0,18]); the WESTERN space (x < -2) is partitioned into
    //    a wing of identity rooms from the LevelArchitect room vocabulary (cells/labs
    //    ~6-8 m, a big signature hall, a large open bay for the drone floor). Rooms open
    //    onto the central west corridor (the z in [-3,3] lane, itself open at its east end
    //    into the arrival hall), so every room is reachable and nothing is sealed.
    // ===================================================================
    // One graybox room: 4 floor-to-ceiling walls with an optional 1.2 m doorway on ONE
    // side ('W'=-X, 'E'=+X, 'S'=-Z, 'N'=+Z; 0 = sealed). Absolute world coords. Reuses
    // the shared wall helpers (addCrossWall handles the Z-running walls + doorway/lintel;
    // the X-running walls are split manually for a doorway, mirroring buildDetentionRoom).
    auto roomBox = [&](float rx0, float rx1, float rz0, float rz1, float ry0, float rh, char door) {
        const float zc = (rz0 + rz1) * 0.5f, xc = (rx0 + rx1) * 0.5f;
        addCrossWall(scene, device, physics, rx0, rz0, rz1, zc, door == 'W', ry0, rh, wallTex, kWallTint, crossWallVis);
        addCrossWall(scene, device, physics, rx1, rz0, rz1, zc, door == 'E', ry0, rh, wallTex, kWallTint, crossWallVis);
        auto wallXDoor = [&](float zf, bool d) {
            if (!d) { addWallX(scene, device, physics, rx0, rx1, zf, ry0, rh, wallTex, kWallTint, crossWallVis); return; }
            addWallX(scene, device, physics, rx0, xc - kDoorHalf, zf, ry0, rh, wallTex, kWallTint, crossWallVis);
            addWallX(scene, device, physics, xc + kDoorHalf, rx1, zf, ry0, rh, wallTex, kWallTint, crossWallVis);
            const float lh = (rh - kLintelBottom) * 0.5f;
            if (lh > 0.0f)
                addBox(scene, device, physics, kDoorHalf, lh, kWallT * 0.5f, xc, ry0 + kLintelBottom + lh, zf,
                       wallTex, kWallTint, (uint32_t)Tag::Static, true, crossWallVis);
        };
        wallXDoor(rz0, door == 'S');
        wallXDoor(rz1, door == 'N');
    };
    // ---- F2 Medical Bay: publish the three RESCUE-ROOM bed centers (Aria/Keisha/Emily)
    //      as the rescue-hub victim markers (the host's RescueSystem reads L.wardA/B/C and
    //      places the live rescuable captive there). These now land ON the beds in the
    //      three side-by-side west-wing rescue rooms (built from the shared kWingRooms
    //      table below) — the bed sits centered in each room and the staged captive lies
    //      restrained on it (room_dressing.cpp). HOST HOOK: reconcile the live standing
    //      RescueSystem victim with the staged lying dressing captive (suppress/pose one).
    //      The two retained arrival-hall partitions split the +X arrival half into medical
    //      bays for the spire_mid encounter cover (unchanged).
    {
        const L1RoomDef& f2 = kFloors[(uint32_t)L1Floor::F2];
        const float y0 = f2.y0, h = f2.ceil;
        const float wx1 = 8.0f, wx2 = 15.0f;     // arrival-hall partition X positions
        addCrossWall(scene, device, physics, wx1, -6.0f, 6.0f, 0.0f, true, y0, h,
                     wallVariants[2], kWallTint, crossWallVis);
        addCrossWall(scene, device, physics, wx2, -6.0f, 6.0f, 0.0f, true, y0, h,
                     wallVariants[1], kWallTint, crossWallVis);
        L.wardA = x3::phys::Vec3{ -27.0f, y0, 6.75f };  // Rescue Room A — Aria
        L.wardB = x3::phys::Vec3{ -35.0f, y0, 6.75f };  // Rescue Room B — Keisha (sealed)
        L.wardC = x3::phys::Vec3{ -43.0f, y0, 6.75f };  // Rescue Room C — Emily
    }
    // ---- F2-F7 west-wing identity rooms: built from the SHARED kWingRooms table (the
    //      SAME source wing_dressing.cpp reads for the recipe art pass, so collision and
    //      dressing never drift). Each room's floor Y + ceiling come from kFloors[floor].
    for (uint32_t wi = 0; wi < kWingRoomCount; ++wi) {
        const L1WingRoom& r = kWingRooms[wi];
        const L1RoomDef&  f = kFloors[(uint32_t)r.floor];
        roomBox(r.cx - r.hw, r.cx + r.hw, r.cz - r.hd, r.cz + r.hd, f.y0, f.ceil, r.door);
    }
    // ---- F6 Executive holding office (Sarah's 4th-rescue marker) in a -Z pocket of the
    //      east hall + F7 rooftop finale-arena center (the open eastern plate). ----
    {
        const L1RoomDef& f6 = kFloors[(uint32_t)L1Floor::F6];
        const float y0 = f6.y0, h = f6.ceil;
        addWallX(scene, device, physics, 0.0f, 8.0f, -3.0f, y0, h, wallVariants[1], kWallTint, crossWallVis);
        addCrossWall(scene, device, physics, 8.0f, -8.0f, -3.0f, -5.5f, true, y0, h,
                     wallVariants[2], kWallTint, crossWallVis);
        L.execOffice = x3::phys::Vec3{ 4.0f, y0, -5.5f };
    }
    {
        const L1RoomDef& f7 = kFloors[(uint32_t)L1Floor::F7];
        L.rooftopCenter = x3::phys::Vec3{ 10.0f, f7.y0, 0.0f };  // finale arena (east hall, by the Clone at x=8)
    }

    // ===================================================================
    // 5a) FLOOR-1 DETENTION INTERIOR (the authoritative LevelArchitect layout). The
    //    29 rooms are built on the B1 plate (Jake's spawn) from kDetention/kDetDoors:
    //    each room's 4 walls with 1.2 m doorway gaps on every face a CONNECTED neighbor
    //    abuts (the door pairs). 3 cell blocks (Jake's 0-3, west 13-17, mid 18-22) hang
    //    off the Main Hallway (4) + the Cell Block B Hallway (23); the support rooms
    //    (Guard Station 5 / Storage 6 / Medical Bay 7 / Armory 8) + the Elevator Lobby
    //    (9) line the east of the main hall; the eastern arm runs Guard Station ->
    //    Creepy Passage (12) -> Descending Stairs (25) -> Cave Tunnel (26) -> Crystal
    //    Cavern (27) -> Side Grotto (28) — the Act-1 -> caves bridge. Cave-arm rooms dip
    //    below the plate base but are CLAMPED above the y=-5 sub-levels and their
    //    ceilings clamped under the plate top so nothing pokes into F1 / collides with
    //    SL1. Built as collision graybox + the env_art GLB tiling overlays it.
    // ===================================================================
    const float b1y    = kFloors[(uint32_t)L1Floor::B1].y0;
    const float b1ceil = kFloors[(uint32_t)L1Floor::B1].ceil;
    const float plateTop = b1y + b1ceil;     // detention ceilings clamp under this
    {
        // Detention concrete. Was { 0.50, 0.55, 0.68 } — the same blue-ambient crutch as
        // kWallTint above (and the darkest of the lot: it took the panel down to a 4%
        // reflector). Neutral: value comes from the texture, hue comes from the lamp.
        const float detTint[4] = { 1.00f, 1.00f, 1.00f, 1.0f };
        // The legacy Awakening-spine corridor carve (x in [0,19.5], z in [-4,4]) so the
        // playable z=0 lane punches through any detention room in its way.
        const SpineCarve carve{ 0.0f, 19.5f, 4.0f };
        for (uint32_t ri = 0; ri < kDetCount; ++ri) {
            const L1DetentionRoom& r = kDetention[ri];
            const float floorY = b1y + r.floorY;
            // Per-room ceiling: the authored h, clamped so the room top never pokes
            // above the B1 plate top (avoids clipping F1's floor at y=5 / the lid).
            float ceilH = r.h;
            if (floorY + ceilH > plateTop - 0.3f) ceilH = (plateTop - 0.3f) - floorY;
            if (ceilH < 2.4f) ceilH = 2.4f;   // keep head clearance even when clamped
            // A floor slab for cave-arm rooms that dip below the plate base (the rest
            // share the B1 plate slab). Avoid a second slab at y=b1y (z-fight the plate).
            if (r.floorY < -0.01f)
                addFloor(scene, device, physics, r.cx, r.cz, r.w * 0.5f, r.d * 0.5f, floorY,
                         floorTex, detTint, floorVis);
            // Vary the wall motif per room so neighbouring detention cells/halls don't
            // all read as the same panel (each room's 4 walls share one variant).
            buildDetentionRoom(scene, device, physics, kDetention, kDetCount,
                               kDetDoors, kDetDoorPairCount, ri, floorY, ceilH,
                               wallVariants[ri % 3], detTint, crossWallVis, carve);
        }
    }

    // ===================================================================
    // 5b) B1 LEGACY AWAKENING SPINE + props — the existing §3 beats. Re-anchored as a
    //    self-contained walled CORRIDOR along z=0, x in [0,19.5], confined to a narrow
    //    z in [-4,4] band (so its partition cross-walls do NOT slice the detention cell
    //    blocks at z<-4). It runs from Jake's Cell out toward the elevator shaft. The
    //    legacy door/room accessors map onto this spine so level1_game.cpp keeps
    //    building + the Awakening beat sequence (cell -> corridor -> armory ->
    //    checkpoint -> arena -> elevator) plays unchanged. The spine is the playable
    //    route; the §5a detention rooms are the authored complex around it.
    // ===================================================================
    const float kSpineZ = 4.0f;   // half-depth of the Awakening corridor lane
    // Cell props (medical pod + the strength-target "equipment" prop, beat 1).
    const float podTint[4] = { 0.30f, 0.45f, 0.65f, 1.0f };
    addBox(scene, device, physics, 1.0f, 0.25f, 0.5f, 2.0f, b1y + 0.25f, 1.8f, floorTex, podTint,
           (uint32_t)Tag::Prop, /*collide*/false);
    const float equipTint[4] = { 0.85f, 0.75f, 0.30f, 1.0f };
    uint32_t equip = addBox(scene, device, physics, 0.3f, 0.4f, 0.3f, 1.5f, b1y + 0.4f, -1.8f,
                            floorTex, equipTint, (uint32_t)Tag::Prop, /*collide*/false);

    // ---- Legacy door + room mapping (all on B1; the doorway centers are placed
    //      along the B1 spine, and the elevator gate is the shaft doorway). ----
    L.spawn = x3::phys::Vec3{ 1.5f, b1y + 0.05f, 0.0f };

    // B1 spine zones laid left->right, ending in a real arena in front of the elevator
    // shaft (x>=19.5) so Martinez has clearance (not inside the shaft). Doors A-D
    // partition the sub-rooms; Door E is the shaft doorway.
    L.doorA = x3::phys::Vec3{  5.0f, b1y, 0.0f };
    L.doorB = x3::phys::Vec3{  9.0f, b1y, 0.0f };
    L.doorC = x3::phys::Vec3{ 12.5f, b1y, 0.0f };
    L.doorD = x3::phys::Vec3{ 15.0f, b1y, 0.0f };
    L.doorE = L.elevatorDoor[(uint32_t)L1Floor::B1];  // arena -> elevator shaft (x=19.5)

    L.cellCenter       = x3::phys::Vec3{  3.0f, b1y, 0.0f };
    L.corridorCenter   = x3::phys::Vec3{  7.0f, b1y, 0.0f };
    L.armoryCenter     = x3::phys::Vec3{ 11.0f, b1y, 0.0f };
    L.checkpointCenter = x3::phys::Vec3{ 13.7f, b1y, 0.0f };
    L.arenaCenter      = x3::phys::Vec3{ 17.5f, b1y, 0.0f };

    // ---- B1 spine corridor walls: the two long lane walls at z=±kSpineZ (x in
    //      [0,19.5]) confine the Awakening encounters to the z=0 lane (so the alarm
    //      enemies can't roam into the later checkpoint fight, and the firing rays
    //      along z=0 hit only the intended group), plus cross-wall partitions (with a
    //      1.2 m doorway gap at z=0) at each spine door X. The DoorSystem slab fills
    //      each z=0 gap. The lane walls span only ±kSpineZ so they leave the detention
    //      cell blocks (at |z|>4) intact. ----
    {
        const float bh = b1ceil;
        // The two long spine lane walls get different variants; the door partitions cycle
        // through all three so each Awakening sub-room reads a little different.
        addWallX(scene, device, physics, 0.0f, 19.5f, -kSpineZ, b1y, bh, wallVariants[1], kWallTint, crossWallVis);
        addWallX(scene, device, physics, 0.0f, 19.5f,  kSpineZ, b1y, bh, wallVariants[2], kWallTint, crossWallVis);
        const float partX[4] = { L.doorA.x, L.doorB.x, L.doorC.x, L.doorD.x };
        for (uint32_t pi = 0; pi < 4; ++pi)
            addCrossWall(scene, device, physics, partX[pi], -kSpineZ, kSpineZ, 0.0f, true, b1y, bh,
                         wallVariants[pi % 3], kWallTint, crossWallVis);
    }

    L.cellHalf       = x3::phys::Vec3{ 3.0f, b1ceil, kSpineZ };
    L.corridorHalf   = x3::phys::Vec3{ 2.5f, b1ceil, kSpineZ };
    L.armoryHalf     = x3::phys::Vec3{ 2.0f, b1ceil, kSpineZ };
    L.checkpointHalf = x3::phys::Vec3{ 2.0f, b1ceil, kSpineZ };
    L.arenaHalf      = x3::phys::Vec3{ 2.0f, b1ceil, kSpineZ };

    L.ceilCell = L.ceilCorridor = L.ceilArmory = L.ceilCheckpoint = b1ceil;
    L.ceilArena = b1ceil;
    L.ceilElevator = kFloors[(uint32_t)L1Floor::B1].ceil;

    // ---- Elevator shaft layout (the host builds the cab here). ----
    L.elevatorCenter = x3::phys::Vec3{ kShaftCx, b1y, kShaftCz };
    L.elevatorHalf   = x3::phys::Vec3{ kShaftHx, shaftTop, kShaftHz };

    L.equipmentProp = equip;

    x3::logInfo("buildLevel1: " + std::to_string(scene.size()) + " entities; spawn ("
                + std::to_string(L.spawn.x) + ", " + std::to_string(L.spawn.y) + ", "
                + std::to_string(L.spawn.z) + "); floors B1..F7 baseY = "
                + std::to_string((int)L.floorBaseY[0]) + ".."
                + std::to_string((int)L.floorBaseY[(uint32_t)L1Floor::F7]) + " m (REAL non-uniform"
                " canon stack: 0/5/10/20/30/65/78/91); shaft @ ("
                + std::to_string((int)kShaftCx) + "," + std::to_string((int)kShaftCz) + ")");
    return L;
}

// Single source of truth for the floor table (shared with env_art.cpp).
const L1RoomDef* level1Rooms() { return kFloors; }

// The resolved stairwell layout (built once). Shared by buildLevel1 + --test-levellint.
const SpireStair& spireStair() {
    static const SpireStair s = buildSpireStair();
    return s;
}

// D22: the neutral, hue-preserving lift applied to the procedural deck map at
// generation time (see buildLevel1). Exported so --test-levellint measures the map the
// world actually ships, not the unlifted one.
const float* level1DeckMapLift() {
    static const float kLift[3] = { 2.6f, 2.6f, 2.6f };
    return kLift;
}

// The tile-aligned stair-well opening on one floor (see level1.h). env_art skips these
// tiles; buildLevel1 aprons the difference so the skip never leaves a void ring.
SpireStair::Box spireWellTileSpan(uint32_t floorIndex) {
    const L1RoomDef& f = kFloors[floorIndex < (uint32_t)L1Floor::Count ? floorIndex : 0];
    const SpireStair& S = spireStair();
    const float gx = f.x0, gz = -f.zHalf;                  // env_art's tile-grid origin
    const int ix0 = (int)std::floor((S.wellX0 - gx) / kSpireArtTileX);
    const int ix1 = (int)std::ceil ((S.wellX1 - gx) / kSpireArtTileX);
    const int iz0 = (int)std::floor((S.wellZ0 - gz) / kSpireArtTileZ);
    const int iz1 = (int)std::ceil ((S.wellZ1 - gz) / kSpireArtTileZ);
    return { gx + (float)ix0 * kSpireArtTileX, gx + (float)ix1 * kSpireArtTileX,
             gz + (float)iz0 * kSpireArtTileZ, gz + (float)iz1 * kSpireArtTileZ, 0.0f, 0.0f };
}

void level1ShaftFootprint(float& x0, float& x1, float& z0, float& z1) {
    x0 = kShaftCx - kShaftHx; x1 = kShaftCx + kShaftHx;
    z0 = kShaftCz - kShaftHz; z1 = kShaftCz + kShaftHz;
}

// The tint set buildLevel1 multiplies over the procedural graybox maps (kind routes the
// probe to the right texture: 0 = floor grate, 1 = wall panel, 2 = ceiling panel).
const L1Surface* level1Surfaces(uint32_t& outCount) {
    static const L1Surface kSurf[] = {
        { "B1 plate deck", { kFloorTints[0][0], kFloorTints[0][1], kFloorTints[0][2], 1.0f }, 0 },
        { "F1 plate deck", { kFloorTints[1][0], kFloorTints[1][1], kFloorTints[1][2], 1.0f }, 0 },
        { "F2 plate deck", { kFloorTints[2][0], kFloorTints[2][1], kFloorTints[2][2], 1.0f }, 0 },
        { "F3 plate deck", { kFloorTints[3][0], kFloorTints[3][1], kFloorTints[3][2], 1.0f }, 0 },
        { "F4 plate deck", { kFloorTints[4][0], kFloorTints[4][1], kFloorTints[4][2], 1.0f }, 0 },
        { "F5 plate deck", { kFloorTints[5][0], kFloorTints[5][1], kFloorTints[5][2], 1.0f }, 0 },
        { "F6 plate deck", { kFloorTints[6][0], kFloorTints[6][1], kFloorTints[6][2], 1.0f }, 0 },
        { "F7 plate deck", { kFloorTints[7][0], kFloorTints[7][1], kFloorTints[7][2], 1.0f }, 0 },
        { "B1 side wall",  { kFloorTints[0][0], kFloorTints[0][1], kFloorTints[0][2], 1.0f }, 1 },
        { "F2 side wall",  { kFloorTints[2][0], kFloorTints[2][1], kFloorTints[2][2], 1.0f }, 1 },
        { "F3 side wall",  { kFloorTints[3][0], kFloorTints[3][1], kFloorTints[3][2], 1.0f }, 1 },
        { "F4 side wall",  { kFloorTints[4][0], kFloorTints[4][1], kFloorTints[4][2], 1.0f }, 1 },
        { "F5 side wall",  { kFloorTints[5][0], kFloorTints[5][1], kFloorTints[5][2], 1.0f }, 1 },
        { "F6 side wall",  { kFloorTints[6][0], kFloorTints[6][1], kFloorTints[6][2], 1.0f }, 1 },
        { "F7 side wall",  { kFloorTints[7][0], kFloorTints[7][1], kFloorTints[7][2], 1.0f }, 1 },
        { "interior wall", { kWallTint[0], kWallTint[1], kWallTint[2], 1.0f }, 1 },
        { "elevator shaft",{ kShaftTint[0], kShaftTint[1], kShaftTint[2], 1.0f }, 1 },
        { "stairwell wall",{ kStairTint[0], kStairTint[1], kStairTint[2], 1.0f }, 1 },
        { "stairwell deck",{ 0.72f, 0.72f, 0.72f, 1.0f }, 0 },
    };
    outCount = (uint32_t)(sizeof(kSurf) / sizeof(kSurf[0]));
    return kSurf;
}

const L1WingRoom* level1WingRooms(uint32_t& outCount) {
    outCount = kWingRoomCount;
    return kWingRooms;
}

// Floor-1 detention table accessors (shared with the self-test).
const L1DetentionRoom* level1DetentionRooms()       { return kDetention; }
uint32_t               level1DetentionRoomCount()   { return kDetCount; }
const uint32_t*        level1DetentionDoors()        { return kDetDoors; }
uint32_t               level1DetentionDoorPairCount(){ return kDetDoorPairCount; }

// The bounding box of all 29 detention rooms (XZ, meters) — the authored ~75x43 m
// Floor-1 footprint (distinct from the larger raw B1 plate).
L1Footprint level1DetentionFootprint() {
    L1Footprint fp{ 1e9f, -1e9f, 1e9f, -1e9f };
    for (uint32_t i = 0; i < kDetCount; ++i) {
        const L1DetentionRoom& r = kDetention[i];
        fp.minX = std::min(fp.minX, r.cx - r.w * 0.5f);
        fp.maxX = std::max(fp.maxX, r.cx + r.w * 0.5f);
        fp.minZ = std::min(fp.minZ, r.cz - r.d * 0.5f);
        fp.maxZ = std::max(fp.maxZ, r.cz + r.d * 0.5f);
    }
    return fp;
}

} // namespace x3::game
