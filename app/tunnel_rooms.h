#pragma once
// ============================================================================
// WHAT IS BEHIND THE SERVICE DOORS — rooms, halls, stairs, and the way down.
//
// tunnel_fitout.cpp decides WHERE the doors are. This module decides what a
// door OPENS ONTO, and it is a separate module for the same reason fitout is:
// the answer is pure data, so it can be proved headless instead of squinted at
// in a capture. `--test-tunnelrooms` is that proof.
//
// Tim's brief, verbatim: "command consoles in rooms behind keypad access
// doors... down halls" and "stairs and underground complex access". Three
// things in that sentence are load-bearing and easy to lose:
//   * the rooms have a PURPOSE (a console to use, plant to look at) -- they are
//     not empty volume;
//   * there is a HALL first -- depth behind the door, not a box bolted to the
//     back of the wall;
//   * the halls GO SOMEWHERE -- down, into the complex (GAME_BACKLOG §3).
//
// THE ANTI-SLOP LINE, applied. TUNNEL_INTERIOR_PLAN.md is blunt about it: the
// kit repeats, the STORY does not. A room behind every door is exactly the
// procedural corridor spam this project refuses, and it is also physically
// wrong -- see the envelope below. So this module hands out program to a small
// AUTHORED set of doors and leaves every other door an amber DENIED service
// void (one mesh + one keypad, which is what a locked door costs). On a mile of
// bore that is still three rooms, not nine. R6 asserts it.
//
// THE ENVELOPE IS WHAT ACTUALLY DECIDES. Under cut-and-cover the corridor is
// cut to road level for its whole length and the hillside is put back as the
// backfill LID mesh. A room hung off the wall is free ONLY where there is real
// ground over it. The plan's constraint 3 says "under the ROOFED span"; the
// code disagrees with the plan and the code is right -- the roofed span
// includes the cut-and-cover EXTENSION at each end, where the lid is a thin
// battered mound over the tube and a room's ceiling comes out through the top
// of it. The route already carries the honest number (`coverS0/coverS1`, the
// sub-span the natural hillside covers on its own) and even that is only a
// centreline figure; a room sits 40-60 ft OFF the centreline where the hill has
// already started to fall away. So nothing here trusts a span at all: every
// candidate footprint is measured against the real `tunnelLidHeightAt()`, and
// the door with the most rock over it is the one that gets the stair down.
// The mountain picks where the complex is. That is a reason, not a dice roll.
//
// UNITS: metres in the data (engine SI), FEET in every log line and message.
// ============================================================================
#include "tunnel_corridor.h"
#include "tunnel_fitout.h"

#include <cstdint>
#include <vector>

namespace x3::game {

// Program tier, straight from TUNNEL_INTERIOR_PLAN.md's census. Only ONE bore
// in the whole world is ever Tier A; B and C get no rooms at all, because "the
// story does not repeat" is the plan's rule and eight copies of one secret
// command room is the exact failure mode it was written against.
enum class TunnelTier : uint32_t { A = 0, B = 1, C = 2 };

// A space you can stand in. The whole program is a tree of these hanging off a
// door, so `parent` is the space you came from and the door is the root.
enum class SpaceKind : uint32_t {
    EntryStub  = 0,  // through the shell + niche reveal, perpendicular to the bore
    Hall       = 1,  // the service gallery, running ALONG the bore behind the wall
    PlantRoom  = 2,  // drainage pumps + vent plant
    SignalRoom = 3,  // relay/comms racks
    ControlRoom= 4,  // the command console, and the stairhead in its far end
    Stair      = 5,  // switchback flight down to the complex
    Landing    = 6,  // underground-complex landing (the task #9 elevator tie-in)
    Count      = 7,
};

const char* spaceKindName(SpaceKind k);

// One space, in ROUTE coordinates: stations along the bore and lateral offset
// from the centreline. Route space (not world XZ) is deliberate -- it is the
// frame `tunnelLidHeightAt()` and `TunnelRoute::worldAt()` both speak, so the
// envelope check never has to convert, and a curved bore needs no special case
// (spec C4: every fixture places via the local frame).
struct TunnelSpace {
    SpaceKind kind = SpaceKind::EntryStub;
    float s0 = 0.0f, s1 = 0.0f;      // station extent, metres (s0 < s1)
    float latIn = 0.0f, latOut = 0.0f;  // |lat| of the near and far walls, metres
    int   side = +1;                 // +1 right of travel, -1 left
    float floorY = 0.0f;             // world Y of the floor
    float clearH = 0.0f;             // clear internal height, metres
    float dropM  = 0.0f;             // Stair only: how far it descends
    int   parent = -1;               // index into spaces(); -1 == reached from the door
    uint32_t entities = 0;           // render entities this space will cost
    // Filled by the envelope pass: the least ground between this space's ceiling
    // and the reconstructed hillside anywhere over its footprint. Negative means
    // the space comes out of the hill.
    float rockCoverM = 0.0f;
};

// One service door, as the rooms module sees it. Every Door fitting gets one of
// these; most of them are DENIED.
struct RoomDoor {
    float s = 0.0f;
    int   side = +1;
    bool  hasProgram = false;   // false => amber "SERVICE VOID - NO ATMOSPHERE"
    int   firstSpace = -1;      // index of its EntryStub, -1 when denied
    int   code = 0;             // keypad code; 0 on a denied door (it never opens)
    const char* label = "SERVICE VOID";
    float coverM = 0.0f;        // rock measured over its candidate room footprint
};

// ---------------------------------------------------------------------------
// Geometry of the kit. Shared by every room in every bore -- uniformity of
// HARDWARE is realism (the plan's words), so these are constants, never seeded.
// ---------------------------------------------------------------------------
constexpr float kTrHallClearW   = 2.00f;   // 6.6 ft -- spec D3 wants >= 6.5 ft
constexpr float kTrHallClearH   = 2.44f;   // 8.0 ft -- spec D3
constexpr float kTrEntryRunM    = 3.00f;   // 9.8 ft of stub through the shell/niche
constexpr float kTrHallRunM     = 11.00f;  // 36.1 ft of gallery before the room
constexpr float kTrMinRunToRoom = 6.10f;   // 20.0 ft, spec D3's floor (entry + hall)
// Rooms. Each size has a reason; see the authored table in the .cpp.
constexpr float kTrPlantLenM    = 9.00f,  kTrPlantDepM = 6.00f,  kTrPlantHM = 3.20f;
constexpr float kTrSignalLenM   = 7.00f,  kTrSignalDepM= 4.50f,  kTrSignalHM= 2.44f;
constexpr float kTrCtrlLenM     = 8.00f,  kTrCtrlDepM  = 6.00f,  kTrCtrlHM  = 2.60f;
// The stair. Riser 6.9 in / tread 11.0 in gives 2R+T = 24.8 in, which is the
// comfortable-stair rule real stairs are built to; anything steeper reads as a
// ladder and anything shallower eats footprint we do not have in a hillside.
constexpr float kTrRiserM       = 0.175f;  // 6.89 in
constexpr float kTrTreadM       = 0.280f;  // 11.02 in
constexpr int   kTrRisersPerFlt = 14;      // 14 x 6.89 in = 8.0 ft per flight
constexpr int   kTrFlights      = 2;       // -> 16.1 ft down to the landing
constexpr float kTrLandingLenM  = 6.00f,  kTrLandingDepM = 5.00f, kTrLandingHM = 2.60f;
// Clearance the spec demands over a room ceiling (1 ft under the lid's
// underside) and the number this module actually designs to. They are different
// on purpose: 1 ft of soil over a room is not a room, it is a bump you can see
// from the ridge, so the spec figure is a hard floor and 6.6 ft is the target.
constexpr float kTrMinRockCover = 0.305f;  // 1.0 ft -- spec D3 hard gate
constexpr float kTrDesignCover  = 2.00f;   // 6.6 ft -- what a door must have to
                                           // be given a room at all
// The character the spaces are built for: app/door.cpp and level_loader.cpp
// both create the controller as radius 0.35 m / height 1.80 m.
constexpr float kTrCharRadius   = 0.35f;
constexpr float kTrCharHeight   = 1.80f;
// The tallest step a controller climbs. Used ONLY by the escapability walk: a
// space you drop into and cannot climb out of is a soft-lock, which is what
// spec D4 exists to catch.
constexpr float kTrMaxStepUp    = 0.45f;   // 18 in

// Keypad codes. PROVISIONAL -- TUNNEL_INTERIOR_PLAN.md's open question 2 (are
// codes discoverable in-world or new canon?) is Tim's to answer, and these are
// placeholders that at least do not collide with anything already canon
// (2742 showroom hatch, 1127 descent, 4545/7762 stairwell). TWO codes, not
// three, because a real highway authority keys its plant and equipment rooms
// alike and keys the room that leads DOWN separately -- which is also the only
// interesting thing a code can say here.
constexpr int kTunnelServiceCode = 1361;   // plant + signal
constexpr int kTunnelControlCode = 3902;   // the control room, and the way down

// ---------------------------------------------------------------------------
// The program for ONE bore.
// ---------------------------------------------------------------------------
class TunnelRoomProgram {
public:
    // `route` supplies the real hillside (the envelope is measured, not
    // assumed); `fit` supplies the door stations -- this module never invents a
    // door. Tier B and C build an empty program by design, not by accident.
    // Honours X3_TUNNEL_INTERIOR=0 (spec B2) by building nothing at all.
    void build(const TunnelRoute& route, const TunnelFitout& fit, TunnelTier tier);

    const std::vector<TunnelSpace>& spaces() const { return m_spaces; }
    const std::vector<RoomDoor>&    doors()  const { return m_doors;  }

    uint32_t entityCount() const;              // what the rooms add, for the budget log
    uint32_t programmedDoorCount() const;      // doors that open onto something

    // Does a walkable route exist from `doorIdx` to every space it leads to, AND
    // back out again? `outFailure` gets a feet-flavoured reason on failure. This
    // is spec D4 (reachable AND escapable) as a function rather than a comment.
    bool walkInAndOut(int doorIdx, char* outFailure, size_t failCap) const;

    // Hand out a mutable copy of the space list. EXISTS FOR THE NEGATIVE
    // CONTROL and for nothing else: R4n has to build a program that is correct
    // in every dimension and still a soft-lock, and the only way to do that
    // honestly is to break a real one rather than describe a broken one.
    std::vector<TunnelSpace> spacesCopy() const { return m_spaces; }

    // Least rock over any space's ceiling, in metres. The number the envelope
    // stands or falls on; exposed so a caller can log it per bore.
    float worstRockCoverM() const;

private:
    void  addProgramFor(const TunnelRoute& route, int doorIdx, SpaceKind terminal);
    float measureCover(const TunnelRoute& route, const TunnelSpace& sp) const;

    std::vector<TunnelSpace> m_spaces;
    std::vector<RoomDoor>    m_doors;
};

// The reach-and-escape walk, as a free function over a space list so a test can
// run it on a DELIBERATELY BROKEN copy (see spacesCopy). Returns false with a
// reason in feet.
bool tunnelWalkInAndOut(const std::vector<TunnelSpace>& spaces, const RoomDoor& door,
                        char* outFailure, size_t failCap);

// Rock over the ceiling of a candidate room at (station, side), in metres --
// the primitive the door assignment is decided on. Public because the self-test
// must ask the SAME function the builder asks, or it is testing a copy.
float tunnelRoomCoverAt(const TunnelRoute& route, float s0, float s1, int side,
                        float latIn, float latOut, float ceilY);

// --test-tunnelrooms. Headless, no GPU: registers the real demo corridor and
// interrogates the real backfill lid. Asserts the envelope, the hall depth, the
// stair's climbability, reach-and-escape, determinism, the fallback, the
// entity budget, and -- the point of the whole module -- that a mile of bore
// still gets three rooms. Carries three negative controls.
bool runTunnelRoomsSelfTest();

} // namespace x3::game
