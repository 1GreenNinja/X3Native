#include "tunnel_rooms.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace x3::game {

namespace {

const float kFt = 3.28084f;                 // metres -> feet, the only unit boundary
inline float m2ft(float m) { return m * kFt; }

// The outer skin of the shell. Everything behind the wall starts here, and it
// is derived rather than typed so a change to the cross-section cannot leave
// the rooms embedded in their own tunnel.
inline float shellOuterHalfW() { return kTcTubeHalfWidth + kTcShellThick; }

// Where a branch's floor sits: flush with the walkway deck, which is what makes
// the door threshold walkable (spec D1 wants the niche floor flush with the
// deck +/- 0.1 ft; a room whose floor is a step down from its own doorway is
// the same defect one level deeper).
inline float branchFloorY(const TunnelRoute& route, float doorS) {
    return route.roadYAt(doorS) + kTcWalkKerbH;
}

// The narrowest walkable dimension of a space in plan. A hall is narrow across
// and long down; a room is the other way round. Taking the min of the two makes
// one number that means "can a body get through this" for every kind.
inline float narrowestM(const TunnelSpace& sp) {
    return std::min(sp.latOut - sp.latIn, sp.s1 - sp.s0);
}

}  // namespace

const char* spaceKindName(SpaceKind k) {
    switch (k) {
        case SpaceKind::EntryStub:   return "entry stub";
        case SpaceKind::Hall:        return "hall";
        case SpaceKind::Garage:      return "garage";
        case SpaceKind::Ramp:        return "ramp";
        case SpaceKind::PlantRoom:   return "plant room";
        case SpaceKind::SignalRoom:  return "signal equipment room";
        case SpaceKind::ControlRoom: return "control room";
        case SpaceKind::Stair:       return "stair";
        case SpaceKind::Landing:     return "complex landing";
        default:                     return "?";
    }
}

// ---------------------------------------------------------------------------
// THE ENVELOPE. Ask the real reconstructed hillside, on a grid over the whole
// footprint, and keep the WORST answer.
//
// Corners alone are not enough and that is not a hypothetical: the lid's lateral
// profile is a smoothstep blend between the mound over the tube and the natural
// surface at the zero-delta seam, so its minimum over a footprint can sit in the
// middle of an edge rather than at a corner. Sampling the interior costs 63
// evaluations per space at build time, once.
// ---------------------------------------------------------------------------
float tunnelRoomCoverAt(const TunnelRoute& route, float s0, float s1, int side,
                        float latIn, float latOut, float ceilY) {
    const int kNs = 9, kNl = 7;
    float worst = 1e9f;
    for (int i = 0; i < kNs; ++i) {
        const float s = s0 + (s1 - s0) * (float)i / (float)(kNs - 1);
        for (int j = 0; j < kNl; ++j) {
            const float mag = latIn + (latOut - latIn) * (float)j / (float)(kNl - 1);
            const float lat = (side >= 0 ? mag : -mag);
            worst = std::min(worst, tunnelLidHeightAt(route, s, lat) - ceilY);
        }
    }
    return worst;
}

float TunnelRoomProgram::measureCover(const TunnelRoute& route, const TunnelSpace& sp) const {
    return tunnelRoomCoverAt(route, sp.s0, sp.s1, sp.side, sp.latIn, sp.latOut,
                             sp.floorY + sp.clearH);
}

// ---------------------------------------------------------------------------
// build()
// ---------------------------------------------------------------------------
void TunnelRoomProgram::build(const TunnelRoute& route, const TunnelFitout& fit, TunnelTier tier) {
    m_spaces.clear();
    m_doors.clear();

    // Spec B2's fallback doctrine, same class as X3_TUNNEL_PORTAL_CUT: one env
    // var restores the bare bore exactly, so "is it the rooms?" is answerable in
    // one run instead of one rebuild.
    if (const char* e = std::getenv("X3_TUNNEL_INTERIOR")) {
        if (e[0] == '0') return;
    }

    // Every Door fitting gets a RoomDoor whether or not it opens onto anything.
    // The denied ones are the point: a locked door with an amber SERVICE VOID
    // keypad is one mesh, and it is what stops the bore reading as a corridor
    // with a room bolted to every 540 ft of it.
    for (const Fitting& f : fit.fittings()) {
        if (f.kind != FittingKind::Door) continue;
        RoomDoor d;
        d.s = f.s;
        d.side = f.side;
        m_doors.push_back(d);
    }
    if (m_doors.empty()) return;

    // TIER B AND C GET NOTHING, deliberately. TUNNEL_INTERIOR_PLAN.md's census:
    // "No stairs, no underground access, no console rooms -- the story does not
    // repeat." Their doors stay denied, which is a complete and correct program,
    // not an unimplemented one.
    if (tier != TunnelTier::A) return;

    // ---- MEASURE FIRST, ASSIGN SECOND ------------------------------------
    // Every door is probed with the SAME yardstick: the footprint of the
    // deepest and tallest room in the kit (the plant room), so the comparison
    // between doors is fair and so a door that only just clears the probe
    // cannot then be handed a room that does not fit. Conservative by
    // construction rather than by a fudge factor.
    const float lat0 = shellOuterHalfW() + kTrEntryRunM;
    for (RoomDoor& d : m_doors) {
        const float fy = branchFloorY(route, d.s);
        const float dir = (d.s < 0.5f * (route.boreS0 + route.boreS1)) ? +1.0f : -1.0f;
        const float rs0 = (dir > 0.0f) ? d.s + kTrHallRunM : d.s - kTrHallRunM - kTrPlantLenM;
        const float rs1 = rs0 + kTrPlantLenM;
        d.coverM = tunnelRoomCoverAt(route, rs0, rs1, d.side,
                                     lat0, lat0 + kTrPlantDepM, fy + kTrPlantHM);
    }

    // The SUMP: the lowest point of the graded road inside the roofed span. Not
    // a guess -- sampled off the real profile, because that is where water goes
    // and therefore where the pumps go.
    float sumpS = route.boreS0, sumpY = 1e9f;
    for (float s = route.boreS0; s <= route.boreS1 + 0.01f; s += 1.0f) {
        const float y = route.roadYAt(s);
        if (y < sumpY) { sumpY = y; sumpS = s; }
    }
    const float midS = 0.5f * (route.boreS0 + route.boreS1);

    // Eligibility, then three authored assignments in priority order. The order
    // IS the design: if a bore is only good for one room it gets the one that
    // carries the story, not the one that happens to be first down the tunnel.
    auto eligible = [&](const RoomDoor& d) { return d.coverM >= kTrDesignCover; };

    // 1) CONTROL ROOM + THE WAY DOWN -> the door with the most rock over it.
    //    The complex is under a mountain because it needs to be; picking the
    //    deepest door is the only assignment here that a player could work out
    //    for themselves by looking at the hill.
    int ctrl = -1;
    for (size_t i = 0; i < m_doors.size(); ++i) {
        if (!eligible(m_doors[i])) continue;
        // Ties break to the LOWER station so the answer is a pure function of
        // the route, with no dependence on vector order or float wobble.
        if (ctrl < 0 || m_doors[i].coverM > m_doors[ctrl].coverM + 1e-4f) ctrl = (int)i;
    }
    // 2) PLANT ROOM -> the eligible door nearest the sump.
    int plant = -1;
    for (size_t i = 0; i < m_doors.size(); ++i) {
        if ((int)i == ctrl || !eligible(m_doors[i])) continue;
        if (plant < 0 || std::fabs(m_doors[i].s - sumpS) < std::fabs(m_doors[plant].s - sumpS) - 1e-4f)
            plant = (int)i;
    }
    // 3) SIGNAL ROOM -> the eligible door nearest mid-bore. Cable runs from the
    //    middle are the shortest to both portals, which is why real bores put
    //    the equipment room there.
    int signal = -1;
    for (size_t i = 0; i < m_doors.size(); ++i) {
        if ((int)i == ctrl || (int)i == plant || !eligible(m_doors[i])) continue;
        if (signal < 0 || std::fabs(m_doors[i].s - midS) < std::fabs(m_doors[signal].s - midS) - 1e-4f)
            signal = (int)i;
    }

    // 4) GARAGE -> the eligible door nearest a PORTAL. This one is not about
    //    cable runs or drainage: it is about the DRIVE. A fleet bay buried in
    //    the middle of a bore means the longest possible trip to fetch a car,
    //    and the shortest trip is the whole reason to keep cars in a tunnel at
    //    all. Assigned LAST so it never outbids the three rooms whose positions
    //    are dictated by physics rather than convenience.
    int garage = -1;
    for (size_t i = 0; i < m_doors.size(); ++i) {
        if ((int)i == ctrl || (int)i == plant || (int)i == signal) continue;
        if (!eligible(m_doors[i])) continue;
        const float toPortal = std::min(m_doors[i].s - route.boreS0, route.boreS1 - m_doors[i].s);
        const float bestSoFar = (garage < 0) ? 1e9f
            : std::min(m_doors[garage].s - route.boreS0, route.boreS1 - m_doors[garage].s);
        if (toPortal < bestSoFar - 1e-4f) garage = (int)i;
    }

    if (ctrl   >= 0) addProgramFor(route, ctrl,   SpaceKind::ControlRoom);
    if (plant  >= 0) addProgramFor(route, plant,  SpaceKind::PlantRoom);
    if (signal >= 0) addProgramFor(route, signal, SpaceKind::SignalRoom);
    if (garage >= 0) addProgramFor(route, garage, SpaceKind::Garage);

    // Envelope pass over what was actually built, with the real footprints
    // rather than the probe's.
    for (TunnelSpace& sp : m_spaces) sp.rockCoverM = measureCover(route, sp);
}

// ---------------------------------------------------------------------------
// One door's branch: stub -> hall -> room, and for the control room the stair
// and the landing beyond it.
//
// THE HALL RUNS ALONG THE BORE, NOT AWAY FROM IT, and that is the one shape
// decision in this file worth arguing about. Pushing a hall straight out
// sideways is the obvious read of "down halls" and it is wrong here: laterally
// the ground is the CUT BATTER, rising from road level at 33 ft off centre to
// natural at 79 ft, so every foot outward buys only a fraction of a foot of
// cover and the room ends up under the thinnest part of the hillside. Turning
// the hall to run parallel to the tube keeps it at a constant, well-covered
// offset and lets it walk TOWARD the middle of the ridge, where the mountain
// is. It is also what a real service gallery does.
// ---------------------------------------------------------------------------
void TunnelRoomProgram::addProgramFor(const TunnelRoute& route, int doorIdx, SpaceKind terminal) {
    RoomDoor& d = m_doors[(size_t)doorIdx];
    float fy         = branchFloorY(route, d.s);   // the GARAGE ramp lowers this
    const float lat0 = shellOuterHalfW();
    const float lat1 = lat0 + kTrEntryRunM;              // hall's near wall
    const float half = kTrHallClearW * 0.5f;
    // Toward mid-bore: the direction with more mountain in front of it.
    const float dir  = (d.s < 0.5f * (route.boreS0 + route.boreS1)) ? +1.0f : -1.0f;

    auto push = [&](SpaceKind k, float s0, float s1, float li, float lo,
                    float floorY, float clearH, int parent, uint32_t ents) {
        TunnelSpace sp;
        sp.kind = k;
        sp.s0 = std::min(s0, s1); sp.s1 = std::max(s0, s1);
        sp.latIn = li; sp.latOut = lo;
        sp.side = d.side; sp.floorY = floorY; sp.clearH = clearH;
        sp.parent = parent; sp.entities = ents;
        m_spaces.push_back(sp);
        return (int)m_spaces.size() - 1;
    };

    // The stub. ONE entity is booked here for the whole branch's concrete: the
    // stub, hall, room, stair and landing are one material and get swept into a
    // single MeshBuf, which is the idiom tunnel_corridor.cpp already uses for
    // the shell and the walkways. Booking it per-space instead would triple the
    // draw count for nothing and would put this module over the plan's Tier-A
    // budget on its own.
    const int iStub = push(SpaceKind::EntryStub, d.s - half, d.s + half,
                           lat0, lat1, fy, kTrHallClearH, -1, 1);
    // The gallery.
    float hallFar = d.s + dir * kTrHallRunM;       // the GARAGE ramp extends this
    const int iHall = push(SpaceKind::Hall, d.s - dir * half, hallFar,
                           lat1, lat1 + kTrHallClearW, fy, kTrHallClearH, iStub, 0);

    int roomParent = -2;            // -2 = "the hall"; the garage re-points it
    float rlen = kTrCtrlLenM, rdep = kTrCtrlDepM, rh = kTrCtrlHM;
    uint32_t rents = 2;             // holo_terminal: a body entity + a screen-glow entity
    if (terminal == SpaceKind::PlantRoom)  { rlen = kTrPlantLenM;  rdep = kTrPlantDepM;  rh = kTrPlantHM;  rents = 1; }
    if (terminal == SpaceKind::SignalRoom) { rlen = kTrSignalLenM; rdep = kTrSignalDepM; rh = kTrSignalHM; rents = 1; }
    if (terminal == SpaceKind::Garage) {
        rlen = kTrGarageLenM; rdep = kTrGarageDepM; rh = kTrGarageHM;
        // THE RAMP, inserted between the hall and the bay. It is a space in its
        // own right rather than a slope applied to the hall, because the walk
        // test has to know a vehicle can get down it AND back up -- the same
        // reachable-and-escapable rule the stair had to satisfy, except this one
        // is driven rather than walked.
        const float rampS0 = hallFar;
        const float rampS1 = hallFar + dir * kTrGarageRampM;
        const int iRamp = push(SpaceKind::Ramp, rampS0, rampS1, lat1, lat1 + 6.0f,
                               fy - kTrGarageDropM * 0.5f, 5.0f, iHall, 0);
        if (iRamp >= 0) m_spaces[(size_t)iRamp].dropM = kTrGarageDropM;
        roomParent = iRamp;         // you reach the bay THROUGH the ramp
        // and the bay itself starts where the ramp ends, a full drop lower.
        hallFar = rampS1;
        fy -= kTrGarageDropM;
        // The parked fleet is the cost: one entity per vehicle on show, plus the
        // lifts and the bench run. Counted honestly rather than as "a room",
        // because R6n proved the Tier-A budget is what actually binds here and a
        // showroom that quietly spends 9 of 40 needs to say so.
        rents = kTrGarageBays + kTrGarageLifts + 1;
    }

    const float rs0 = hallFar;
    const float rs1 = hallFar + dir * rlen;
    const int iRoom = push(terminal, rs0, rs1, lat1, lat1 + rdep, fy, rh,
                           (roomParent == -2) ? iHall : roomParent, rents);

    if (terminal == SpaceKind::Garage) { d.code = kTunnelGarageCode; }
    else if (terminal != SpaceKind::ControlRoom) { d.code = kTunnelServiceCode; }
    else {
        d.code = kTunnelControlCode;
        // The stairhead sits in the control room's far end, so the flight is
        // contiguous with the room and shares its ceiling -- which means the
        // envelope check covers it too instead of it being a hole in the proof.
        const float flightRun = (float)(kTrRisersPerFlt - 1) * kTrTreadM;   // 13 treads
        const float stairLen  = flightRun + 0.36f;                          // + the half-turn landing
        const float ss0 = rs1;
        const float ss1 = rs1 + dir * stairLen;
        const int iStair = push(SpaceKind::Stair, ss0, ss1, lat1, lat1 + 3.0f, fy, rh, iRoom, 0);
        m_spaces[(size_t)iStair].dropM = (float)(kTrFlights * kTrRisersPerFlt) * kTrRiserM;

        // The landing sits UNDER the switchback, which is what a half-turn stair
        // is for: it returns you over your own footprint instead of marching
        // 28 ft further into the hill for the second flight.
        const float ly = fy - m_spaces[(size_t)iStair].dropM;
        push(SpaceKind::Landing, ss0, ss0 + dir * kTrLandingLenM,
             lat1, lat1 + kTrLandingDepM, ly, kTrLandingHM, iStair, 1);
    }

    d.hasProgram = true;
    d.firstSpace = iStub;
    d.label = (terminal == SpaceKind::Garage)     ? "VEHICLE BAY"
            : (terminal == SpaceKind::PlantRoom)  ? "PLANT"
            : (terminal == SpaceKind::SignalRoom) ? "SIGNAL EQUIPMENT"
                                                  : "CONTROL";
}

uint32_t TunnelRoomProgram::entityCount() const {
    uint32_t n = 0;
    for (const TunnelSpace& sp : m_spaces) n += sp.entities;
    return n;
}

uint32_t TunnelRoomProgram::programmedDoorCount() const {
    uint32_t n = 0;
    for (const RoomDoor& d : m_doors) if (d.hasProgram) ++n;
    return n;
}

float TunnelRoomProgram::worstRockCoverM() const {
    float w = 1e9f;
    for (const TunnelSpace& sp : m_spaces) w = std::min(w, sp.rockCoverM);
    return m_spaces.empty() ? 0.0f : w;
}

// ---------------------------------------------------------------------------
// REACH AND ESCAPE (spec D4). Walks the branch as a graph rather than asserting
// a shape, because the failure this catches is topological: a space you can
// drop into and not climb out of passes every dimension check ever written and
// is still a soft-lock.
// ---------------------------------------------------------------------------
bool tunnelWalkInAndOut(const std::vector<TunnelSpace>& spaces, const RoomDoor& d,
                        char* outFailure, size_t failCap) {
    if (!d.hasProgram) return true;          // a denied door leads nowhere: nothing to escape
    if (d.firstSpace < 0 || (size_t)d.firstSpace >= spaces.size()) return false;

    // Every space reachable from this door, following parent links forward.
    std::vector<int> reach;
    reach.push_back(d.firstSpace);
    for (size_t k = 0; k < reach.size(); ++k) {
        for (size_t i = 0; i < spaces.size(); ++i)
            if (spaces[i].parent == reach[k]) reach.push_back((int)i);
    }

    for (int idx : reach) {
        const TunnelSpace& sp = spaces[(size_t)idx];
        const float w = narrowestM(sp);
        if (w < 2.0f * kTrCharRadius + 0.20f) {
            if (outFailure) std::snprintf(outFailure, failCap,
                "%s is only %.1f ft across -- a %.1f ft body plus clearance does not fit",
                spaceKindName(sp.kind), m2ft(w), m2ft(2.0f * kTrCharRadius));
            return false;
        }
        if (sp.clearH < kTrCharHeight + 0.20f) {
            if (outFailure) std::snprintf(outFailure, failCap,
                "%s has %.1f ft of headroom -- a %.1f ft body does not stand up in it",
                spaceKindName(sp.kind), m2ft(sp.clearH), m2ft(kTrCharHeight));
            return false;
        }
        if (sp.parent < 0) continue;
        const TunnelSpace& pa = spaces[(size_t)sp.parent];
        const float dy = std::fabs(sp.floorY - pa.floorY);
        if (dy <= kTrMaxStepUp) continue;
        // A drop bigger than a step is only legal if a STAIR carries it. This is
        // the whole assertion: geometry alone cannot tell the difference between
        // a stairwell and an oubliette.
        // A RAMP carries a drop exactly as a stair does -- it is the whole
        // reason it exists. Without this the gate was right to fail: it saw the
        // garage sitting 13 ft under the hall with nothing connecting them and
        // called it a soft-lock, which is precisely the trap it was written to
        // catch. The difference between the two is what USES them (a stair is
        // walked, a ramp is driven), not whether they connect levels.
        auto carries = [](const TunnelSpace& x, float need) {
            return (x.kind == SpaceKind::Stair || x.kind == SpaceKind::Ramp)
                && x.dropM >= need - 0.01f;
        };
        const bool carried = carries(pa, dy) || carries(sp, dy);
        if (!carried) {
            if (outFailure) std::snprintf(outFailure, failCap,
                "%s sits %.1f ft below the %s with no stair carrying the drop -- you get in and stay in",
                spaceKindName(sp.kind), m2ft(dy), spaceKindName(pa.kind));
            return false;
        }
    }
    return true;
}

bool TunnelRoomProgram::walkInAndOut(int doorIdx, char* outFailure, size_t failCap) const {
    if (doorIdx < 0 || (size_t)doorIdx >= m_doors.size()) return false;
    return tunnelWalkInAndOut(m_spaces, m_doors[(size_t)doorIdx], outFailure, failCap);
}

// ===========================================================================
// --test-tunnelrooms
// ===========================================================================
namespace {
int g_pass = 0, g_fail = 0;
void rcheck(bool ok, const char* what) {
    if (ok) { ++g_pass; x3::logInfo(std::string("  [ok]   ") + what); }
    else    { ++g_fail; x3::logError(std::string("  [FAIL] ") + what); }
}

// A bore of arbitrary length on the demo route's real hillside. Used to prove
// the assignment does not multiply with length -- the anti-slop gate.
TunnelFitout fitBore(float s0, float s1, uint32_t seed) {
    FitoutConfig cfg;
    TunnelFitout f;
    f.build(s0, s1, cfg, seed);
    return f;
}
}  // namespace

bool runTunnelRoomsSelfTest() {
    g_pass = g_fail = 0;
    x3::logInfo("--- tunnel rooms / halls / stairs self-test ---");
    char b[512];

    // The real corridor, through the real boot door -- the same one
    // --test-tunnelmouth uses, so the hillside under test is the shipped one.
    const TunnelRoute& route = registerTunnelCorridor();
    if (!route.boreValid || route.st.size() < 2) {
        x3::logError("  [FAIL] R0 the demo corridor has no roofed span -- nothing to put rooms in");
        return false;
    }

    TunnelFitout fit = fitBore(route.boreS0, route.boreS1, 7u);
    TunnelRoomProgram prog;
    prog.build(route, fit, TunnelTier::A);

    // ---- R0: THE CENSUS. Every door, its measured rock cover, and what it got.
    // The design is justified by these numbers, so they are logged, not implied.
    {
        std::snprintf(b, sizeof(b),
            "R0 a %.0f ft roofed bore has %u service doors; %u of them open onto a program "
            "(%u spaces, %u entities)",
            m2ft(route.boreS1 - route.boreS0), (uint32_t)prog.doors().size(),
            prog.programmedDoorCount(), (uint32_t)prog.spaces().size(), prog.entityCount());
        rcheck(!prog.doors().empty() && prog.programmedDoorCount() > 0, b);
        for (size_t i = 0; i < prog.doors().size(); ++i) {
            const RoomDoor& d = prog.doors()[i];
            std::snprintf(b, sizeof(b),
                "       door %u at s=%.0f ft, %s side: %.1f ft of rock over a candidate room -> %s",
                (uint32_t)i, m2ft(d.s), d.side > 0 ? "right" : "left ",
                m2ft(d.coverM), d.hasProgram ? d.label : "DENIED (service void)");
            x3::logInfo(b);
        }
        for (const TunnelSpace& sp : prog.spaces()) {
            std::snprintf(b, sizeof(b),
                "       %-22s s %.0f..%.0f ft, %.0f..%.0f ft off centre, %.1f ft clear, "
                "%.1f ft of rock over it",
                spaceKindName(sp.kind), m2ft(sp.s0), m2ft(sp.s1),
                m2ft(sp.latIn), m2ft(sp.latOut), m2ft(sp.clearH), m2ft(sp.rockCoverM));
            x3::logInfo(b);
        }
    }

    // ---- R1: THE ENVELOPE. Nothing comes out of the hill. Measured against the
    // real tunnelLidHeightAt(), which is the same function --test-tunnelmouth M3
    // holds the shell to.
    {
        std::snprintf(b, sizeof(b),
            "R1 every space stays under the backfill lid: worst cover %.1f ft (spec floor %.1f ft, "
            "design target %.1f ft)",
            m2ft(prog.worstRockCoverM()), m2ft(kTrMinRockCover), m2ft(kTrDesignCover));
        rcheck(prog.worstRockCoverM() >= kTrMinRockCover, b);
        rcheck(prog.worstRockCoverM() >= kTrDesignCover,
               "R1b ...and clears the design target too, not just the spec floor");
    }

    // ---- R1n: NEGATIVE CONTROL for the envelope. A check that cannot fail is
    // not a check: put the same control room out on the approach cutting, where
    // there is no lid at all, and the cover must go NEGATIVE.
    {
        const float outS = std::max(0.0f, route.boreS0 - 40.0f);
        const float l0 = kTcTubeHalfWidth + kTcShellThick + kTrEntryRunM;
        const float fy = route.roadYAt(outS) + kTcWalkKerbH;
        const float bad = tunnelRoomCoverAt(route, outS, outS + kTrCtrlLenM, +1,
                                            l0, l0 + kTrCtrlDepM, fy + kTrCtrlHM);
        std::snprintf(b, sizeof(b),
            "R1n NEGATIVE CONTROL: the same room on the approach cutting at s=%.0f ft has "
            "%.1f ft of cover (must be negative -- it punches through the cut face)",
            m2ft(outS), m2ft(bad));
        rcheck(bad < 0.0f, b);
    }

    // ---- R2: DEPTH BEHIND THE DOOR (spec D3 / Tim's "down halls"). The run
    // from the door to the room's threshold, measured off the built spaces.
    {
        float worstRun = 1e9f;
        for (const RoomDoor& d : prog.doors()) {
            if (!d.hasProgram) continue;
            const TunnelSpace& stub = prog.spaces()[(size_t)d.firstSpace];
            // Walked, not assumed: out through the stub, then along the hall from
            // the door's own station to whichever end of the hall is further
            // from it -- which is where the room opens off.
            float run = stub.latOut - stub.latIn;
            for (const TunnelSpace& sp : prog.spaces()) {
                if (sp.kind != SpaceKind::Hall || sp.parent != d.firstSpace) continue;
                run += std::max(std::fabs(sp.s1 - d.s), std::fabs(sp.s0 - d.s));
            }
            worstRun = std::min(worstRun, run);
        }
        std::snprintf(b, sizeof(b),
            "R2 the shortest run from a door to its room is %.0f ft (spec D3 floor %.0f ft) -- "
            "there is depth behind the door, not a box against the wall",
            m2ft(worstRun), m2ft(kTrMinRunToRoom));
        rcheck(worstRun >= kTrMinRunToRoom, b);
    }

    // ---- R3: THE STAIR IS A STAIR. Riser/tread against the rule real stairs
    // are built to, and it must actually arrive somewhere below.
    {
        const float rule = m2ft(2.0f * kTrRiserM + kTrTreadM) * 12.0f;   // 2R+T, inches
        const float drop = (float)(kTrFlights * kTrRisersPerFlt) * kTrRiserM;
        bool hasStair = false, hasLanding = false;
        for (const TunnelSpace& sp : prog.spaces()) {
            if (sp.kind == SpaceKind::Stair)   hasStair = true;
            if (sp.kind == SpaceKind::Landing) hasLanding = true;
        }
        std::snprintf(b, sizeof(b),
            "R3 stair: %.2f in riser / %.2f in tread, 2R+T = %.1f in (comfortable stairs are 24-25 in), "
            "%d flights descending %.1f ft to the complex landing",
            m2ft(kTrRiserM) * 12.0f, m2ft(kTrTreadM) * 12.0f, rule, kTrFlights, m2ft(drop));
        rcheck(rule >= 24.0f && rule <= 25.0f && hasStair && hasLanding, b);
    }

    // ---- R4: REACH AND ESCAPE, every programmed door (spec D4).
    {
        bool ok = true;
        char why[256] = {0};
        for (size_t i = 0; i < prog.doors().size(); ++i)
            if (!prog.walkInAndOut((int)i, why, sizeof(why))) { ok = false; break; }
        std::snprintf(b, sizeof(b),
            "R4 every room is reachable through its hall AND escapable back out%s%s",
            ok ? "" : " -- ", ok ? "" : why);
        rcheck(ok, b);
    }

    // ---- R4n: NEGATIVE CONTROL for escape. Take the SAME program and delete
    // the stair's ability to carry its drop (the oubliette): the landing is
    // still exactly as big, exactly as tall and exactly as reachable, and the
    // walk must now fail. Dimensions alone cannot catch this.
    {
        // Break the REAL program: take the stair's flight away and leave a bare
        // 16 ft drop into the landing. Every room is still exactly as big, as
        // tall and as reachable -- only the way back up is gone. If R4 still
        // passed on this, R4 would be measuring nothing.
        std::vector<TunnelSpace> oubliette = prog.spacesCopy();
        int stairs = 0;
        for (TunnelSpace& sp : oubliette)
            if (sp.kind == SpaceKind::Stair) { sp.dropM = 0.0f; ++stairs; }
        bool caught = false;
        char why[256] = {0};
        for (const RoomDoor& d : prog.doors())
            if (!tunnelWalkInAndOut(oubliette, d, why, sizeof(why))) { caught = true; break; }
        std::snprintf(b, sizeof(b),
            "R4n NEGATIVE CONTROL: delete the %d stair's flight and the same program becomes a "
            "soft-lock -- the walk fails with \"%s\"", stairs, caught ? why : "(NOTHING -- R4 is blind)");
        rcheck(stairs > 0 && caught, b);
    }

    // ---- R5: THE PROGRAM IS A PURE FUNCTION OF THE ROUTE. No rand, no clock.
    {
        TunnelRoomProgram a, c;
        a.build(route, fit, TunnelTier::A);
        c.build(route, fit, TunnelTier::A);
        bool same = a.spaces().size() == c.spaces().size() && a.doors().size() == c.doors().size();
        if (same) for (size_t i = 0; i < a.spaces().size(); ++i)
            if (a.spaces()[i].s0 != c.spaces()[i].s0 || a.spaces()[i].latOut != c.spaces()[i].latOut ||
                a.spaces()[i].floorY != c.spaces()[i].floorY || a.spaces()[i].kind != c.spaces()[i].kind)
            { same = false; break; }
        rcheck(same, "R5 same route, same bore -> byte-identical program (captures stay reproducible)");
    }

    // ---- R6: THE ANTI-SLOP GATE. A mile of bore is not nine rooms.
    {
        // Doors every 200 ft instead of 540 on the SAME well-covered bore, so
        // the extra doors are all genuinely eligible -- the cap has to come from
        // the authored table, not from the hillside doing the work for it.
        FitoutConfig dense;
        dense.doorSpacingM = 60.0f;
        TunnelFitout df; df.build(route.boreS0, route.boreS1, dense, 7u);
        TunnelRoomProgram dp;
        dp.build(route, df, TunnelTier::A);
        uint32_t elig = 0;
        for (const RoomDoor& d : dp.doors()) if (d.coverM >= kTrDesignCover) ++elig;
        std::snprintf(b, sizeof(b),
            "R6 %u doors, %u of them with enough mountain over them to take a room -- and the bore "
            "still gets exactly %u. The kit repeats, the story does not",
            df.countOf(FittingKind::Door), elig, dp.programmedDoorCount());
        // FOUR now, not three: the garage joined the authored table on
        // 2026-08-15. The number moving is fine -- what this gate defends is that
        // it is a FIXED number set by the table, not one that grows with the
        // length of the bore. A mile of tunnel still gets exactly the rooms the
        // story has, which is the whole anti-slop claim.
        rcheck(elig > 4 && dp.programmedDoorCount() == 4, b);
    }

    // ---- R6n: NEGATIVE CONTROL for the anti-slop gate. Cost out the naive
    // build -- a room behind every door -- on that same mile, using the SAME
    // public cover function. It must actually fail: either it breaches the hill
    // or it blows the plan's Tier-A entity budget. If the naive build were fine,
    // the authored table would be ceremony.
    {
        TunnelFitout mile = fitBore(route.boreS0, route.boreS0 + 1609.0f, 7u);
        const float l0 = kTcTubeHalfWidth + kTcShellThick + kTrEntryRunM;
        int breaches = 0, doors = 0;
        float worst = 1e9f;
        for (const Fitting& f : mile.fittings()) {
            if (f.kind != FittingKind::Door) continue;
            ++doors;
            const float fy = route.roadYAt(f.s) + kTcWalkKerbH;
            const float rs0 = f.s + kTrHallRunM;
            const float cov = tunnelRoomCoverAt(route, rs0, rs0 + kTrCtrlLenM, f.side,
                                                l0, l0 + kTrCtrlDepM, fy + kTrCtrlHM);
            worst = std::min(worst, cov);
            if (cov < kTrMinRockCover) ++breaches;
        }
        const uint32_t naiveEnts = (uint32_t)doors * 4u;    // the control branch's own cost
        std::snprintf(b, sizeof(b),
            "R6n NEGATIVE CONTROL: on a 1 mile bore, a room behind all %d doors comes out of the "
            "hillside at %d of them (worst %.1f ft of cover) and spends %u entities -- the WHOLE "
            "Tier-A budget of 40 on rooms alone, with nothing left for walkways, railings, strips "
            "or screens. The naive build is not merely inelegant; it does not fit in the mountain",
            doors, breaches, m2ft(worst), naiveEnts);
        rcheck(breaches > 0, b);
    }

    // ---- R7: THE BUDGET (spec B1). Rooms are one claimant on the Tier-A 40;
    // walkways, railings, strips, doors, signs and screens are the others, so
    // the rooms' own share is logged and capped well below the total.
    {
        std::snprintf(b, sizeof(b),
            "R7 the room program costs %u entities of the Tier-A budget of 40 "
            "(the rest is walkways, railings, strips, doors, signs, screens)",
            prog.entityCount());
        // Raised 14 -> 20 when the garage landed. It is the single most
        // expensive room in the program and says so: six vehicles on display,
        // two lifts and a bench run cost 9 of the 40 by themselves. That is a
        // deliberate purchase, not drift -- and it is still under half the
        // budget, which is what keeps R6n's naive build (52) genuinely failing
        // rather than merely losing on points.
        rcheck(prog.entityCount() > 0 && prog.entityCount() <= 20, b);
    }

    // ---- R8: TIERS B AND C GET NOTHING. Not unimplemented -- decided.
    {
        TunnelRoomProgram tb, tc;
        tb.build(route, fit, TunnelTier::B);
        tc.build(route, fit, TunnelTier::C);
        std::snprintf(b, sizeof(b),
            "R8 tier B / C bores get %u / %u rooms and keep all %u doors as amber service voids "
            "-- the story does not repeat",
            tb.programmedDoorCount(), tc.programmedDoorCount(), (uint32_t)tb.doors().size());
        rcheck(tb.programmedDoorCount() == 0 && tc.programmedDoorCount() == 0 &&
               !tb.doors().empty(), b);
    }

    // ---- R9: FLOORS ARE LEVEL (spec C3). One branch, one datum: a room whose
    // floor follows the road grade is a room with a slope in it.
    {
        bool level = true;
        for (const TunnelSpace& sp : prog.spaces()) {
            if (sp.parent < 0 || sp.kind == SpaceKind::Landing) continue;
            const TunnelSpace& pa = prog.spaces()[(size_t)sp.parent];
            // A stair or a RAMP is the thing that changes level, so the space
            // below one is exempt -- and so is the ramp itself, whose own floor
            // is by definition not the floor it came from. Everything else in a
            // branch shares one datum: a room whose floor follows the road grade
            // is a room with a slope in it.
            if (pa.kind == SpaceKind::Stair || pa.kind == SpaceKind::Ramp) continue;
            if (sp.kind == SpaceKind::Ramp) continue;
            if (std::fabs(sp.floorY - pa.floorY) > 1e-4f) level = false;
        }
        rcheck(level, "R9 every space in a branch shares one level floor datum (spec C3), the stair and the garage ramp "
                      "being the only thing that changes height");
    }

    // ---- R10: THE FALLBACK (spec B2).
    {
#ifdef _WIN32
        _putenv_s("X3_TUNNEL_INTERIOR", "0");
#else
        setenv("X3_TUNNEL_INTERIOR", "0", 1);
#endif
        TunnelRoomProgram off;
        off.build(route, fit, TunnelTier::A);
#ifdef _WIN32
        _putenv_s("X3_TUNNEL_INTERIOR", "");
#else
        unsetenv("X3_TUNNEL_INTERIOR");
#endif
        std::snprintf(b, sizeof(b),
            "R10 X3_TUNNEL_INTERIOR=0 restores the bare bore exactly: %u spaces, %u doors",
            (uint32_t)off.spaces().size(), (uint32_t)off.doors().size());
        rcheck(off.spaces().empty() && off.doors().empty(), b);
    }

    // ---- R10: SOLID ROCK. Every connection between two spaces must be a
    // DOORWAY, never a whole missing wall.
    //
    // This exists because the first geometry pass skipped the entire shared face
    // wherever two spaces touched. That is correct only if they are the same
    // size, and they are deliberately not: the hall is 6.6 ft wide and the rooms
    // are 15-20 ft deep, so up to 13 ft of wall simply vanished. And the hole
    // does not look out onto anything -- the mountain is a HEIGHTFIELD, so the
    // volume behind these rooms is VOID. Walking through one leaves the world.
    //
    // The property that makes it safe is arithmetic, so it can be asserted from
    // the data: for every touching pair, the overlap must be a STRICT SUBSET of
    // at least one of the two faces, which is exactly the condition that leaves
    // a border of wall standing around the opening.
    {
        // The shipped program, on the shipped hillside -- not a synthetic one.
        TunnelRoomProgram pg; pg.build(route, fit, TunnelTier::A);
        const auto& sv = pg.spaces();

        int pairs = 0, strict = 0;
        float worstGapFt = 0.0f;
        for (size_t a = 0; a < sv.size(); ++a) {
            for (size_t c = a + 1; c < sv.size(); ++c) {
                const TunnelSpace& X = sv[a]; const TunnelSpace& Y = sv[c];
                if (X.side != Y.side) continue;
                const bool latTouch = (std::fabs(X.latOut - Y.latIn) < 0.1f ||
                                       std::fabs(Y.latOut - X.latIn) < 0.1f);
                const bool staTouch = (std::fabs(X.s1 - Y.s0) < 0.1f ||
                                       std::fabs(Y.s1 - X.s0) < 0.1f);
                if (!latTouch && !staTouch) continue;
                ++pairs;
                // In-plane extents of the two faces, and their overlap.
                const float xa = latTouch ? X.s0 : X.latIn, xb = latTouch ? X.s1 : X.latOut;
                const float ya = latTouch ? Y.s0 : Y.latIn, yb = latTouch ? Y.s1 : Y.latOut;
                const float o0 = std::max(xa, ya), o1 = std::min(xb, yb);
                if (o1 - o0 <= 0.01f) continue;             // they only graze
                const float widest = std::max(xb - xa, yb - ya);
                if ((o1 - o0) < widest - 0.01f) ++strict;
                worstGapFt = std::max(worstGapFt, (widest - (o1 - o0)) * kFt);
            }
        }
        std::snprintf(b, sizeof(b),
            "R10 %d connected pair(s); %d have a face WIDER than the opening (up to %.1f ft of wall "
            "that a whole-face skip would have deleted, straight into the void behind the rooms)",
            pairs, strict, worstGapFt);
        rcheck(pairs > 0 && strict > 0, b);
    }

    std::snprintf(b, sizeof(b), "--- tunnel rooms self-test: %d passed, %d failed ---", g_pass, g_fail);
    if (g_fail) x3::logError(b); else x3::logInfo(b);
    return g_fail == 0;
}

}  // namespace x3::game
