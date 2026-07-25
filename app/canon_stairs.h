#pragma once
// THE FACILITY STAIRWELL — an OPEN SWITCHBACK (dog-leg) stair tower connecting the
// NORMAL floors (F1..F7) of the canonical tower, built alongside the elevator spine.
//
// Tim's explicit design pick (2026-07): zigzag/switchback (NOT spiral), an OPEN central
// well you can see top-to-bottom, "nicely rendered." This is the walk-up alternative to
// the elevator.
//
// GEOMETRY: a self-contained enclosed shaft placed IMMEDIATELY SOUTH (-Z) of the stacked
// Elevator Lobby column (lobbies sit at x=22, z~=-26 on every floor). Straight flights run
// along X and REVERSE direction each half-flight, alternating between a NORTH band and a
// SOUTH band so they wrap a central OPEN WELL. A rectangular landing at each end joins the
// two bands; the landing at each floor's elevation is FLUSH with that floor's lobby floor
// and connects to it through a real cut opening in the lobby's south wall.
//
// LEVEL 4.5 (the hidden Nexus Chamber) is NOT a normal floor: it hangs in the F4-F5 void
// at z~=0..16 (canon_45.cpp), far from this shaft at z~=-31. The stairwell climbs PAST
// the 4.5 elevation with plain intermediate landings and NEVER opens onto it — exactly
// Tim's "no stairways get to it." Only the 7 Elevator Lobby floors get an opening.
//
// Like canon_45's scaffold climb, the geometry rides the loader's exported brush path
// (canonAddBrush) so every step/landing/wall gets scene + static collision + vis exactly
// like level geometry. Everything is tagged kNoRoom (always-visible, like the elevator
// shaft) so the open well reads top-to-bottom regardless of the per-room PVS cull.

#include "level_loader.h"
#include "surface_library.h"

#include <cstdint>
#include <vector>

namespace x3::game {

// One normal floor the stairwell serves: its lobby room + the flush landing elevation +
// the opening cut in the lobby's south wall.
struct StairFloor {
    uint32_t lobby = kNoRoom;
    float    floorY = 0.0f;     // lobby floor Y (== landing top Y at this floor)
    float    ceilY  = 0.0f;     // lobby ceiling Y (top of the lobby's south-wall band)
    float    openCenterX = 0.0f;
    float    openHalfX = 0.8f;
    int      openFace = 2;      // 2 == -Z (lobby's south wall)
};

// The resolved layout of the stair tower, derived from the tower's Elevator Lobby rooms.
// Computed once (plan()) and shared between: the pre-build opening list the host feeds to
// buildCanonFloor (CanonBuildOpts::stairOpenings) and the post-build geometry (build()).
struct StairPlan {
    bool  valid = false;
    float x0t = 0, x1t = 0;     // tower INTERIOR X extent (flight run + landings live here)
    float zN = 0, zS = 0;       // tower INTERIOR Z: north (lobby-facing) .. south
    float landingDepthX = 1.6f; // X depth of the end landings
    float wellZ0 = 0, wellZ1 = 0;   // the open central well's Z band
    float bottomY = 0, topY = 0;    // shaft floor + ceiling Y
    std::vector<StairFloor> floors;  // sorted low -> high (F1..F7)
};

class CanonStairwell {
public:
    // Resolve the switchback layout from the loaded tower (finds the Elevator Lobby rooms).
    // Returns valid()==false if fewer than two lobbies exist. Pure — no scene/device.
    static StairPlan plan(const CanonFloor& floor);

    // Convenience: the wall openings this plan needs cut in each lobby's south wall. Feed
    // to CanonBuildOpts::stairOpenings BEFORE buildCanonFloor so the openings are cut by
    // the same wall builders as every doorway (no floating lintels, doctrine LAW 1/2).
    static std::vector<CanonBuildOpts::WallOpening> openings(const StairPlan& p);

    // Build the shaft (enclosing walls, switchback flights, landings, balustrades,
    // dressing) into the scene. Appends motivated lights into `canonLights` (the same
    // per-room-gated feed the tower uses). No-op if the plan is invalid.
    void build(const StairPlan& p, Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, const std::string& surfaceLibRoot,
               std::vector<CanonLight>& canonLights);

    bool built() const { return m_built; }

private:
    bool m_built = false;
    SurfaceLibrary m_lib;
};

} // namespace x3::game
