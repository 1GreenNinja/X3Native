#pragma once
// Navigation + pathfinding (GENERAL) — nav grid + A* + path-follow.
//
// Clean module: plain structs + an opaque INavGrid. NO JPH:: / Vulkan / glm types
// leak through here; the implementation (Navigation.cpp) talks to IPhysicsWorld via
// its public clean interface only. Serves every genre (FPS/MMORPG/TD/Adventure):
// NPCs route AROUND obstacles instead of walking straight into them.
//
// Representation: a bounded, axis-aligned NAV GRID over the XZ plane. The world is
// Y-up (docs/CONVENTIONS.md), so navigation is planar (XZ) with a sampled ground
// height per cell. Each cell is walkable or blocked. Walkability is sampled from
// the physics world (raycast down for ground + a vertical obstacle probe at agent
// height) OR stamped directly from level geometry (markBlockedBox / setWalkable).
//
// Pathfinding: 8-connected A* (orthogonal + diagonal, diagonal cost ~sqrt2, with a
// per-cell extra obstacle cost) between two world points, returning a list of
// world-space waypoints. Optional string-pull smoothing collapses colinear /
// line-of-sight-clear runs so the agent doesn't zig-zag along the grid. No path ->
// empty result (e.g. goal fully walled off / unreachable).
//
// Path following: PathFollower advances along the waypoint list toward a goal,
// emitting a desired planar velocity the existing movement (setBodyPosition /
// moveCharacter) consumes. Cheap + bounded: pooled A* nodes, no per-frame heap
// alloc in steady state — the host rebuilds the path on a cadence, not every frame.

#include <cstdint>
#include <vector>

namespace x3 {
namespace phys { class IPhysicsWorld; struct Vec3; }  // fwd (clean boundary)

namespace ai {

// Planar world point (XZ used for nav; Y carried for the sampled ground height so a
// waypoint can drive a 3D move target). Mirrors phys::Vec3 layout but kept separate
// so the AI module has no hard dependency on the physics struct in its signatures.
struct NavVec3 { float x = 0, y = 0, z = 0; };

// One cell of the grid (debug / introspection). Walkable cells are traversable;
// blocked cells are obstacles or off-ground (no floor found beneath them).
struct NavCell {
    bool  walkable = false;
    float groundY  = 0.0f;   // sampled floor height at the cell center (world Y)
    float cost     = 1.0f;   // extra traversal cost multiplier (>=1; soft-blocked > 1)
};

// Parameters for sampling a nav grid from the physics world over a bounded region.
// The region is an axis-aligned XZ rectangle [minX,maxX] x [minZ,maxZ]; cellSize is
// the square cell edge (meters). Walkability per cell:
//   * cast a DOWN ray from (cx, sampleTopY, cz) of length sampleDepth; a hit on the
//     Static layer is the floor (groundY = hit.y). No floor -> blocked.
//   * then probe for an obstacle occupying the agent's body: a short ray just above
//     the floor; if it hits within agentHeight the cell is blocked.
struct NavBuildParams {
    float minX = -16.0f, maxX = 16.0f;   // region bounds (world meters)
    float minZ = -16.0f, maxZ = 16.0f;
    float cellSize = 1.0f;               // cell edge (m); ~agent radius is a good size
    float sampleTopY = 8.0f;             // start the down-ray this high above the region
    float sampleDepth = 20.0f;           // down-ray length (must reach the floor)
    float agentHeight = 1.8f;            // obstacle probe height above the floor (m)
    float agentRadius = 0.4f;            // clearance: cells whose center+radius overlap
                                         // an obstacle are blocked (probed at 4 offsets)
    float maxStepHeight = 0.5f;          // (reserved) max floor delta between neighbors
};

// A finished A* path: world-space waypoints from (near) the start to (near) the
// goal, inclusive of both ends. Empty `points` => no path found. `cellCount` is the
// number of expanded cells (A* work done) for perf logging.
struct NavPath {
    std::vector<NavVec3> points;
    uint32_t cellCount = 0;              // A* nodes expanded (diagnostics)
    bool ok() const { return !points.empty(); }
};

// Opaque nav grid. Build it once for a level/region, then call findPath() as often
// as the host's pathfinding cadence requires (cheap; pooled nodes inside). It owns
// no physics/render handles after build — it's a pure CPU walkability bitmap.
class INavGrid {
public:
    virtual ~INavGrid() = default;

    // Grid extent (cells) + the geometry that maps cells <-> world.
    virtual uint32_t cols() const = 0;          // cells along +X
    virtual uint32_t rows() const = 0;          // cells along +Z
    virtual float    cellSize() const = 0;
    virtual float    originX() const = 0;        // world X of cell (0,*) min corner
    virtual float    originZ() const = 0;        // world Z of cell (*,0) min corner

    // Cell <-> world. cellCenter returns the XZ center + sampled groundY. worldToCell
    // clamps into range and returns false if the world point is outside the region.
    virtual NavVec3 cellCenter(uint32_t col, uint32_t row) const = 0;
    virtual bool    worldToCell(float wx, float wz, uint32_t& outCol, uint32_t& outRow) const = 0;

    // Per-cell walkability (out-of-range => not walkable). Direct introspection +
    // editing so a level can stamp blockers without re-sampling physics.
    virtual bool walkable(uint32_t col, uint32_t row) const = 0;
    virtual void setWalkable(uint32_t col, uint32_t row, bool w) = 0;
    virtual NavCell cell(uint32_t col, uint32_t row) const = 0;

    // Stamp an axis-aligned blocker box (world XZ rect, inflated by the build's agent
    // radius) as blocked. Lets a level mark a wall/prop without a physics probe.
    virtual void markBlockedBox(float minX, float minZ, float maxX, float maxZ) = 0;

    // A* from `start` to `goal` (world points). Snaps each to the nearest walkable
    // cell, runs 8-connected A* (diagonal cost ~sqrt2, per-cell extra cost honored),
    // optionally string-pulls the result. Returns waypoints (start..goal). Empty on
    // failure (either endpoint unreachable / fully walled). No per-call heap churn
    // beyond the output vector — the open/closed/score scratch is pooled in the grid.
    virtual NavPath findPath(const NavVec3& start, const NavVec3& goal,
                             bool smooth = true) const = 0;

    // True iff a straight XZ segment between two cells crosses only walkable cells
    // (grid line-of-sight; used by string-pull smoothing + callable for steering).
    virtual bool lineOfSightClear(uint32_t c0, uint32_t r0,
                                  uint32_t c1, uint32_t r1) const = 0;

    // Count of walkable cells (diagnostics / tests).
    virtual uint32_t walkableCount() const = 0;
};

// Build a nav grid by SAMPLING the physics world over a bounded region (raycast
// down for ground + obstacle probe). Never returns null. If the region produced no
// walkable cells the grid is still valid (every cell blocked) — callers should
// check walkableCount().
INavGrid* buildNavGridFromPhysics(phys::IPhysicsWorld& world, const NavBuildParams& p);

// Build an EMPTY nav grid (all cells walkable, flat groundY) over a region — for
// levels that stamp their own blockers via markBlockedBox / setWalkable instead of
// probing physics (TD maps, scripted arenas, headless tests). Never null.
INavGrid* buildEmptyNavGrid(const NavBuildParams& p);

// ---------------------------------------------------------------------------
// Path following. Steers an agent along a NavPath toward the final goal, emitting a
// desired planar velocity (XZ) the host feeds into its existing movement. Stateless
// w.r.t. physics: the host passes the agent's current position each tick. Bounded +
// allocation-free per tick (it only advances an index into the path's waypoints).
// ---------------------------------------------------------------------------
class PathFollower {
public:
    // Adopt a freshly-computed path. Resets progress to the first waypoint. A path
    // with < 2 points (or empty) makes the follower inert (zero velocity, arrived).
    void setPath(const NavPath& path);

    // Advance toward the current waypoint from `pos`. Returns the desired planar
    // velocity scaled to `speed` (m/s); the host applies it (e.g. pos += vel*dt or
    // moveCharacter). When within `arriveRadius` of a waypoint, advances to the next;
    // within `arriveRadius` of the FINAL waypoint, marks arrived() and returns zero.
    NavVec3 desiredVelocity(const NavVec3& pos, float speed,
                            float arriveRadius = 0.35f);

    bool     arrived() const { return m_arrived; }
    bool     hasPath() const { return m_count >= 2; }
    uint32_t waypointIndex() const { return m_index; }
    uint32_t waypointCount() const { return m_count; }
    // The current goal (final waypoint) — for re-path comparisons. Zeroed if empty.
    NavVec3  goal() const { return m_count ? m_points[m_count - 1] : NavVec3{}; }

private:
    std::vector<NavVec3> m_points;
    uint32_t m_count   = 0;
    uint32_t m_index   = 0;       // index of the waypoint we're steering toward
    bool     m_arrived = false;
};

// Headless self-test (--test-nav). Asserts (no window / Vulkan):
//   N1 A* finds a path around a blocking obstacle (avoids blocked cells, reaches goal);
//   N2 no path when fully walled off (empty result);
//   N3 path-follow advances an agent from start to goal;
//   N4 physics-sampled grid marks an open floor walkable + a wall blocked;
//   N5 string-pull smoothing shortens a detour vs the raw grid path.
// Logs PASS/FAIL N#, returns true iff all pass. Lives in Navigation.cpp.
bool runNavSelfTest();

} // namespace ai
} // namespace x3
