// Navigation + pathfinding implementation — nav grid + A* + path-follow.
// See engine/ai/INavigation.h. Clean-room: built from public pathfinding refs
// (A* over a uniform grid, 8-connected; string-pull / line-of-sight smoothing) and
// the engine's own IPhysicsWorld clean interface. No third-party nav source read.
//
// Talks to physics ONLY through IPhysicsWorld::rayCast (declared in
// engine/physics/IPhysicsWorld.h). No JPH:: types appear here.

#include "engine/ai/INavigation.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <queue>
#include <string>
#include <vector>

namespace x3::ai {

namespace {

constexpr float kSqrt2 = 1.41421356237f;
constexpr uint32_t kInvalidIdx = 0xFFFFFFFFu;

inline phys::Vec3 toPhys(const NavVec3& v) { return phys::Vec3{ v.x, v.y, v.z }; }

float planarDist(const NavVec3& a, const NavVec3& b) {
    const float dx = a.x - b.x, dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

// ---------------------------------------------------------------------------
// Concrete grid. Row-major flat arrays: index = row*cols + col. The A* scratch
// (came-from, g/f scores, closed flag, open-heap generation) is POOLED as members,
// sized once at build, and only reset for the touched cells per query so steady-
// state pathfinding does no heap allocation beyond the output waypoint vector.
// ---------------------------------------------------------------------------
class NavGrid final : public INavGrid {
public:
    NavGrid(const NavBuildParams& p) {
        m_cellSize = (p.cellSize > 1e-3f) ? p.cellSize : 1.0f;
        m_originX  = p.minX;
        m_originZ  = p.minZ;
        const float spanX = std::max(0.0f, p.maxX - p.minX);
        const float spanZ = std::max(0.0f, p.maxZ - p.minZ);
        m_cols = std::max<uint32_t>(1u, (uint32_t)std::ceil(spanX / m_cellSize));
        m_rows = std::max<uint32_t>(1u, (uint32_t)std::ceil(spanZ / m_cellSize));
        m_agentRadius = p.agentRadius;
        const size_t n = (size_t)m_cols * m_rows;
        m_cells.assign(n, NavCell{ true, 0.0f, 1.0f });
        // A* scratch pools.
        m_gScore.assign(n, 0.0f);
        m_fScore.assign(n, 0.0f);
        m_cameFrom.assign(n, kInvalidIdx);
        m_visitGen.assign(n, 0u);
        m_closedGen.assign(n, 0u);
        m_gen = 1;
    }

    uint32_t cols() const override { return m_cols; }
    uint32_t rows() const override { return m_rows; }
    float    cellSize() const override { return m_cellSize; }
    float    originX() const override { return m_originX; }
    float    originZ() const override { return m_originZ; }

    NavVec3 cellCenter(uint32_t col, uint32_t row) const override {
        NavVec3 v;
        v.x = m_originX + (col + 0.5f) * m_cellSize;
        v.z = m_originZ + (row + 0.5f) * m_cellSize;
        v.y = inRange(col, row) ? m_cells[idx(col, row)].groundY : 0.0f;
        return v;
    }

    bool worldToCell(float wx, float wz, uint32_t& outCol, uint32_t& outRow) const override {
        const float fx = (wx - m_originX) / m_cellSize;
        const float fz = (wz - m_originZ) / m_cellSize;
        const bool inside = fx >= 0.0f && fz >= 0.0f &&
                            fx < (float)m_cols && fz < (float)m_rows;
        int c = (int)std::floor(fx);
        int r = (int)std::floor(fz);
        c = std::clamp(c, 0, (int)m_cols - 1);
        r = std::clamp(r, 0, (int)m_rows - 1);
        outCol = (uint32_t)c;
        outRow = (uint32_t)r;
        return inside;
    }

    bool walkable(uint32_t col, uint32_t row) const override {
        return inRange(col, row) && m_cells[idx(col, row)].walkable;
    }
    void setWalkable(uint32_t col, uint32_t row, bool w) override {
        if (inRange(col, row)) m_cells[idx(col, row)].walkable = w;
    }
    NavCell cell(uint32_t col, uint32_t row) const override {
        return inRange(col, row) ? m_cells[idx(col, row)] : NavCell{};
    }

    void markBlockedBox(float minX, float minZ, float maxX, float maxZ) override {
        // Inflate by the agent radius so an agent's body can't clip the corner.
        const float inflate = m_agentRadius;
        const float x0 = minX - inflate, x1 = maxX + inflate;
        const float z0 = minZ - inflate, z1 = maxZ + inflate;
        for (uint32_t r = 0; r < m_rows; ++r) {
            for (uint32_t c = 0; c < m_cols; ++c) {
                const float cx = m_originX + (c + 0.5f) * m_cellSize;
                const float cz = m_originZ + (r + 0.5f) * m_cellSize;
                if (cx >= x0 && cx <= x1 && cz >= z0 && cz <= z1)
                    m_cells[idx(c, r)].walkable = false;
            }
        }
    }

    uint32_t walkableCount() const override {
        uint32_t n = 0;
        for (const auto& c : m_cells) if (c.walkable) ++n;
        return n;
    }

    bool lineOfSightClear(uint32_t c0, uint32_t r0,
                          uint32_t c1, uint32_t r1) const override {
        return losClear(c0, r0, c1, r1);
    }

    NavPath findPath(const NavVec3& start, const NavVec3& goal,
                     bool smooth) const override;

    // ---- physics sampling (called once after construction) ----
    void sampleFromPhysics(phys::IPhysicsWorld& world, const NavBuildParams& p);

private:
    size_t idx(uint32_t c, uint32_t r) const { return (size_t)r * m_cols + c; }
    bool inRange(uint32_t c, uint32_t r) const { return c < m_cols && r < m_rows; }

    // Snap a world point to the nearest WALKABLE cell (spiral search outward from the
    // clamped cell). Returns kInvalidIdx if no walkable cell exists at all.
    uint32_t snapToWalkable(const NavVec3& w) const {
        uint32_t c, r;
        worldToCell(w.x, w.z, c, r);
        if (walkable(c, r)) return (uint32_t)idx(c, r);
        // Expand a square ring outward until a walkable cell is found.
        const int maxRing = (int)std::max(m_cols, m_rows);
        for (int ring = 1; ring <= maxRing; ++ring) {
            for (int dr = -ring; dr <= ring; ++dr) {
                for (int dc = -ring; dc <= ring; ++dc) {
                    if (std::max(std::abs(dr), std::abs(dc)) != ring) continue; // ring edge only
                    const int nc = (int)c + dc, nr = (int)r + dr;
                    if (nc < 0 || nr < 0 || nc >= (int)m_cols || nr >= (int)m_rows) continue;
                    if (m_cells[idx((uint32_t)nc, (uint32_t)nr)].walkable)
                        return (uint32_t)idx((uint32_t)nc, (uint32_t)nr);
                }
            }
        }
        return kInvalidIdx;
    }

    // Grid line-of-sight (supercover-ish DDA): every cell the segment between two
    // cell centers passes through must be walkable. Used by string-pull smoothing.
    bool losClear(uint32_t c0, uint32_t r0, uint32_t c1, uint32_t r1) const {
        if (!inRange(c0, r0) || !inRange(c1, r1)) return false;
        int x0 = (int)c0, y0 = (int)r0, x1 = (int)c1, y1 = (int)r1;
        int dx = std::abs(x1 - x0), dy = std::abs(y1 - y0);
        int x = x0, y = y0;
        int n = 1 + dx + dy;
        const int xInc = (x1 > x0) ? 1 : -1;
        const int yInc = (y1 > y0) ? 1 : -1;
        int err = dx - dy;
        dx *= 2; dy *= 2;
        for (; n > 0; --n) {
            if (!m_cells[idx((uint32_t)x, (uint32_t)y)].walkable) return false;
            if (err > 0) { x += xInc; err -= dy; }
            else if (err < 0) { y += yInc; err += dx; }
            else { // exact diagonal: also require the two shared corner cells to be
                   // walkable so the path can't squeeze through a diagonal gap.
                const int cx = x + xInc, cy = y + yInc;
                if (cx >= 0 && cx < (int)m_cols &&
                    !m_cells[idx((uint32_t)cx, (uint32_t)y)].walkable) return false;
                if (cy >= 0 && cy < (int)m_rows &&
                    !m_cells[idx((uint32_t)x, (uint32_t)cy)].walkable) return false;
                x += xInc; y += yInc; err -= dy; err += dx; --n;
            }
        }
        return true;
    }

    uint32_t m_cols = 1, m_rows = 1;
    float    m_cellSize = 1.0f;
    float    m_originX = 0.0f, m_originZ = 0.0f;
    float    m_agentRadius = 0.4f;
    std::vector<NavCell> m_cells;

    // A* scratch (pooled, mutable so findPath can stay const for callers).
    mutable std::vector<float>    m_gScore;
    mutable std::vector<float>    m_fScore;
    mutable std::vector<uint32_t> m_cameFrom;
    mutable std::vector<uint32_t> m_visitGen;  // generation stamp -> "g/f valid this query"
    mutable std::vector<uint32_t> m_closedGen; // generation a cell was closed in
    mutable uint32_t m_gen = 1;
};

// ---------------------------------------------------------------------------
// Physics sampling: for each cell center, raycast straight down to find the floor
// (Static layer), then probe upward through the agent's body height for an obstacle.
// ---------------------------------------------------------------------------
void NavGrid::sampleFromPhysics(phys::IPhysicsWorld& world, const NavBuildParams& p) {
    const phys::Vec3 down{ 0.0f, -1.0f, 0.0f };
    // 4 clearance offsets at +/- agentRadius along X/Z so a cell adjacent to a wall
    // (but whose center is clear) is still blocked if the agent's body would clip it.
    const float rr = std::max(0.0f, p.agentRadius);
    const float offs[5][2] = {
        { 0.0f, 0.0f }, { rr, 0.0f }, { -rr, 0.0f }, { 0.0f, rr }, { 0.0f, -rr }
    };
    const size_t n = (size_t)m_cols * m_rows;

    // ---- Pass 1: per cell, find the HIGHEST solid surface under the agent footprint
    // (center + 4 clearance offsets). A down-ray from high above first meets the floor
    // on open ground (y~floor) but the OBSTACLE TOP over a wall/crate (the agent can't
    // stand on a 3 m wall as floor). Cast on the Dynamic mask so the down-ray sees
    // Static walls AND dynamic props/crates (Dynamic matches Static/Dynamic/Player/
    // Enemy per the engine layer matrix). Cells with no surface (a hole) are blocked.
    // We record each cell's surface height and the GLOBAL minimum (= the floor plane),
    // then pass 2 blocks any cell whose surface rises more than maxStepHeight above it
    // — that surface is an obstacle column intersecting the agent's body. This is the
    // robust test (it does not depend on ray-from-inside-a-solid behaviour). ----
    std::vector<float> surfY(n, 0.0f);
    std::vector<uint8_t> hasSurf(n, 0u);
    float floorMin = std::numeric_limits<float>::max();
    for (uint32_t r = 0; r < m_rows; ++r) {
        for (uint32_t c = 0; c < m_cols; ++c) {
            const float cx = m_originX + (c + 0.5f) * m_cellSize;
            const float cz = m_originZ + (r + 0.5f) * m_cellSize;
            float lowest = std::numeric_limits<float>::max();
            float highest = -std::numeric_limits<float>::max();
            bool any = false, hole = false;
            for (int o = 0; o < 5; ++o) {
                phys::Vec3 from{ cx + offs[o][0], p.sampleTopY, cz + offs[o][1] };
                phys::RayHit s = world.rayCast(from, down, p.sampleDepth, phys::Layer::Dynamic);
                if (!s.hit) { hole = true; continue; }
                any = true;
                lowest = std::min(lowest, s.point.y);
                highest = std::max(highest, s.point.y);
            }
            const size_t i = idx(c, r);
            if (!any) { hasSurf[i] = 0u; m_cells[i].walkable = false; m_cells[i].groundY = 0.0f; continue; }
            // A hole anywhere in the footprint => the agent can fall off; treat as
            // blocked but still record the floor height we did find.
            hasSurf[i] = hole ? 0u : 1u;
            surfY[i]   = highest;          // the highest surface decides clearance
            m_cells[i].groundY = lowest;   // stand on the lowest sampled floor
            floorMin = std::min(floorMin, lowest);
        }
    }
    if (floorMin == std::numeric_limits<float>::max()) floorMin = 0.0f;

    // ---- Pass 2: walkable iff a real floor was found across the whole footprint AND
    // its highest surface is within a step of the floor plane (no obstacle column). --
    uint32_t walk = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!hasSurf[i]) { m_cells[i].walkable = false; continue; }
        const bool obstacle = (surfY[i] - floorMin) > p.maxStepHeight;
        m_cells[i].walkable = !obstacle;
        if (m_cells[i].walkable) ++walk;
    }
    x3::logInfo("[nav] sampled grid " + std::to_string(m_cols) + "x" +
                std::to_string(m_rows) + " (" + std::to_string(n) +
                " cells), floorY=" + std::to_string(floorMin) +
                " walkable=" + std::to_string(walk));
}

// ---------------------------------------------------------------------------
// A* over the 8-connected grid.
// ---------------------------------------------------------------------------
NavPath NavGrid::findPath(const NavVec3& start, const NavVec3& goal, bool smooth) const {
    NavPath out;
    const uint32_t s = snapToWalkable(start);
    const uint32_t g = snapToWalkable(goal);
    if (s == kInvalidIdx || g == kInvalidIdx) return out;   // no walkable cells

    // Bump the generation so stale g/f/closed stamps are ignored — no full clear.
    if (++m_gen == 0) {  // wrapped: hard reset (extremely rare)
        std::fill(m_visitGen.begin(), m_visitGen.end(), 0u);
        std::fill(m_closedGen.begin(), m_closedGen.end(), 0u);
        m_gen = 1;
    }
    const uint32_t gen = m_gen;

    auto colOf = [&](uint32_t i) { return (uint32_t)(i % m_cols); };
    auto rowOf = [&](uint32_t i) { return (uint32_t)(i / m_cols); };
    const uint32_t gc = colOf(g), gr = rowOf(g);

    auto heuristic = [&](uint32_t i) -> float {
        // Octile distance (admissible for 8-connected movement) in cell units * size.
        const float dx = (float)std::abs((int)colOf(i) - (int)gc);
        const float dz = (float)std::abs((int)rowOf(i) - (int)gr);
        const float hi = std::max(dx, dz), lo = std::min(dx, dz);
        return (hi - lo + kSqrt2 * lo) * m_cellSize;
    };

    // Min-heap of (f, cellIndex). Use a fresh local priority_queue per query (its
    // storage reuses the small-ish heap; the heavy per-cell scratch is pooled).
    struct Node { float f; uint32_t i; };
    struct Cmp { bool operator()(const Node& a, const Node& b) const { return a.f > b.f; } };
    std::priority_queue<Node, std::vector<Node>, Cmp> open;

    m_visitGen[s] = gen; m_gScore[s] = 0.0f; m_fScore[s] = heuristic(s);
    m_cameFrom[s] = kInvalidIdx;
    open.push({ m_fScore[s], s });

    // 8 neighbor offsets (dc, dr) with step cost.
    static const int NB[8][2] = {
        { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 },
        { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 }
    };

    uint32_t expanded = 0;
    bool found = false;
    while (!open.empty()) {
        const Node cur = open.top(); open.pop();
        const uint32_t ci = cur.i;
        if (m_closedGen[ci] == gen) continue;       // already finalized
        m_closedGen[ci] = gen;
        ++expanded;
        if (ci == g) { found = true; break; }

        const int cc = (int)colOf(ci), cr = (int)rowOf(ci);
        const float curG = m_gScore[ci];
        for (int k = 0; k < 8; ++k) {
            const int nc = cc + NB[k][0], nr = cr + NB[k][1];
            if (nc < 0 || nr < 0 || nc >= (int)m_cols || nr >= (int)m_rows) continue;
            const uint32_t ni = (uint32_t)idx((uint32_t)nc, (uint32_t)nr);
            const NavCell& ncell = m_cells[ni];
            if (!ncell.walkable) continue;
            const bool diag = (NB[k][0] != 0 && NB[k][1] != 0);
            if (diag) {
                // Disallow cutting a blocked corner: both orthogonal neighbors shared
                // by the diagonal must be walkable.
                if (!m_cells[idx((uint32_t)nc, (uint32_t)cr)].walkable) continue;
                if (!m_cells[idx((uint32_t)cc, (uint32_t)nr)].walkable) continue;
            }
            if (m_closedGen[ni] == gen) continue;
            const float stepBase = (diag ? kSqrt2 : 1.0f) * m_cellSize;
            const float step = stepBase * std::max(1.0f, ncell.cost);
            const float tentative = curG + step;
            const bool seen = (m_visitGen[ni] == gen);
            if (!seen || tentative < m_gScore[ni]) {
                m_visitGen[ni] = gen;
                m_cameFrom[ni] = ci;
                m_gScore[ni] = tentative;
                m_fScore[ni] = tentative + heuristic(ni);
                open.push({ m_fScore[ni], ni });
            }
        }
    }

    out.cellCount = expanded;
    if (!found) return out;   // unreachable

    // Reconstruct (goal -> start) then reverse into cell-index list.
    std::vector<uint32_t> cellPath;
    for (uint32_t i = g; i != kInvalidIdx; i = m_cameFrom[i]) {
        cellPath.push_back(i);
        if (i == s) break;
    }
    std::reverse(cellPath.begin(), cellPath.end());

    // Optional string-pull: keep a waypoint only when the line of sight from the last
    // kept cell to the next-next cell is blocked (collapse colinear / clear runs).
    if (smooth && cellPath.size() > 2) {
        std::vector<uint32_t> pulled;
        pulled.push_back(cellPath.front());
        size_t anchor = 0;
        for (size_t i = 2; i < cellPath.size(); ++i) {
            const uint32_t a = cellPath[anchor];
            const uint32_t b = cellPath[i];
            if (!losClear(colOf(a), rowOf(a), colOf(b), rowOf(b))) {
                pulled.push_back(cellPath[i - 1]);   // last visible cell becomes a corner
                anchor = i - 1;
            }
        }
        pulled.push_back(cellPath.back());
        cellPath.swap(pulled);
    }

    // Cells -> world waypoints. Replace the first/last with the true start/goal XZ so
    // the agent steers to the actual requested points (Y from the cell's ground).
    out.points.reserve(cellPath.size());
    for (size_t i = 0; i < cellPath.size(); ++i) {
        const uint32_t ci = cellPath[i];
        NavVec3 w = cellCenter(colOf(ci), rowOf(ci));
        if (i == 0)                       { w.x = start.x; w.z = start.z; }
        else if (i + 1 == cellPath.size()) { w.x = goal.x;  w.z = goal.z; }
        out.points.push_back(w);
    }
    return out;
}

} // namespace

INavGrid* buildNavGridFromPhysics(phys::IPhysicsWorld& world, const NavBuildParams& p) {
    NavGrid* grid = new NavGrid(p);
    grid->sampleFromPhysics(world, p);
    return grid;
}

INavGrid* buildEmptyNavGrid(const NavBuildParams& p) {
    return new NavGrid(p);   // all cells walkable, groundY 0 (ctor default)
}

// ---------------------------------------------------------------------------
// PathFollower
// ---------------------------------------------------------------------------
void PathFollower::setPath(const NavPath& path) {
    m_points = path.points;
    m_count  = (uint32_t)m_points.size();
    m_index  = (m_count >= 2) ? 1u : 0u;   // steer toward the 2nd point (1st = start)
    m_arrived = (m_count < 2);
}

NavVec3 PathFollower::desiredVelocity(const NavVec3& pos, float speed, float arriveRadius) {
    if (m_count < 2 || m_arrived) return NavVec3{};

    // Advance past any waypoints we've reached (handles overshoot / fast agents).
    while (m_index < m_count) {
        const NavVec3& wp = m_points[m_index];
        if (planarDist(pos, wp) > arriveRadius) break;
        if (m_index + 1 >= m_count) {   // reached the final waypoint
            m_arrived = true;
            return NavVec3{};
        }
        ++m_index;
    }
    if (m_index >= m_count) { m_arrived = true; return NavVec3{}; }

    const NavVec3& target = m_points[m_index];
    float dx = target.x - pos.x, dz = target.z - pos.z;
    const float d = std::sqrt(dx * dx + dz * dz);
    if (d < 1e-5f) return NavVec3{};
    return NavVec3{ (dx / d) * speed, 0.0f, (dz / d) * speed };
}

// ===========================================================================
// Headless self-test (--test-nav).
// ===========================================================================
namespace {

int nav_pass = 0, nav_fail = 0;
void navcheck(bool cond, const char* name) {
    if (cond) { ++nav_pass; x3::logInfo(std::string("[nav-test] PASS ") + name); }
    else      { ++nav_fail; x3::logError(std::string("[nav-test] FAIL ") + name); }
}

// Flat ground quad at y=0 (CCW so +Y is solid), half-extent `half` (m).
phys::BodyId navGround(phys::IPhysicsWorld& w, float half) {
    float v[] = { -half, 0, -half,  half, 0, -half,  half, 0, half,  -half, 0, half };
    uint32_t idx[] = { 0, 2, 1, 0, 3, 2 };
    return w.addStaticMesh(v, 4, idx, 6);
}

// Verify a path: non-empty, ends near the goal, and never visits a blocked cell.
bool pathValid(const INavGrid& grid, const NavPath& path, const NavVec3& goal) {
    if (path.points.empty()) return false;
    const NavVec3& end = path.points.back();
    if (std::fabs(end.x - goal.x) > grid.cellSize() * 1.5f ||
        std::fabs(end.z - goal.z) > grid.cellSize() * 1.5f) return false;
    for (const auto& p : path.points) {
        uint32_t c, r;
        grid.worldToCell(p.x, p.z, c, r);
        if (!grid.walkable(c, r)) return false;
    }
    return true;
}

} // namespace

bool runNavSelfTest() {
    nav_pass = nav_fail = 0;

    // ---- N1: A* routes AROUND a blocking wall (empty grid + stamped wall). ----
    // 20x20 m region, 1 m cells. A vertical wall down the middle (x in [-0.5,0.5])
    // with a gap near the +Z edge forces a detour; the straight line is blocked.
    {
        NavBuildParams p;
        p.minX = -10; p.maxX = 10; p.minZ = -10; p.maxZ = 10; p.cellSize = 1.0f;
        p.agentRadius = 0.0f;   // exact cell stamping for the test geometry
        std::unique_ptr<INavGrid> grid(buildEmptyNavGrid(p));
        // Wall spanning z in [-10, 8] at x in [-0.5,0.5] (leaves a gap at the top).
        grid->markBlockedBox(-0.5f, -10.0f, 0.5f, 8.0f);
        const NavVec3 start{ -8.0f, 0.0f, 0.0f };
        const NavVec3 goal{ 8.0f, 0.0f, 0.0f };
        NavPath raw = grid->findPath(start, goal, /*smooth*/ false);
        bool reaches = pathValid(*grid, raw, goal);
        // The path must detour toward +Z (through the gap) — some waypoint z > 6.
        bool detoured = false;
        for (const auto& w : raw.points) if (w.z > 6.0f) detoured = true;
        x3::logInfo(std::string("[nav-test] N1 pts=") + std::to_string(raw.points.size()) +
                    " cells=" + std::to_string(raw.cellCount) +
                    " reaches=" + (reaches ? "1" : "0") +
                    " detoured=" + (detoured ? "1" : "0"));
        navcheck(reaches && detoured, "N1 A* routes AROUND a wall (avoids blocked cells, reaches goal)");
    }

    // ---- N2: no path when fully walled off (empty result). ----
    {
        NavBuildParams p;
        p.minX = -10; p.maxX = 10; p.minZ = -10; p.maxZ = 10; p.cellSize = 1.0f;
        p.agentRadius = 0.0f;
        std::unique_ptr<INavGrid> grid(buildEmptyNavGrid(p));
        // FULL wall across the region (no gap) -> goal on the far side is unreachable.
        grid->markBlockedBox(-0.5f, -10.0f, 0.5f, 10.0f);
        const NavVec3 start{ -8.0f, 0.0f, 0.0f };
        const NavVec3 goal{ 8.0f, 0.0f, 0.0f };
        NavPath path = grid->findPath(start, goal);
        x3::logInfo(std::string("[nav-test] N2 pts=") + std::to_string(path.points.size()) +
                    " ok=" + (path.ok() ? "1" : "0"));
        navcheck(!path.ok() && path.points.empty(), "N2 fully walled -> no path (empty)");
    }

    // ---- N3: path-follow advances an agent from start to goal. ----
    {
        NavBuildParams p;
        p.minX = -10; p.maxX = 10; p.minZ = -10; p.maxZ = 10; p.cellSize = 1.0f;
        p.agentRadius = 0.0f;
        std::unique_ptr<INavGrid> grid(buildEmptyNavGrid(p));
        grid->markBlockedBox(-0.5f, -10.0f, 0.5f, 8.0f);   // same detour as N1
        const NavVec3 start{ -8.0f, 0.0f, 0.0f };
        const NavVec3 goal{ 8.0f, 0.0f, 0.0f };
        NavPath path = grid->findPath(start, goal, true);
        PathFollower follower;
        follower.setPath(path);
        NavVec3 agent = start;
        const float dt = 1.0f / 60.0f, speed = 4.0f;
        int steps = 0;
        for (; steps < 4000 && !follower.arrived(); ++steps) {
            NavVec3 v = follower.desiredVelocity(agent, speed);
            agent.x += v.x * dt; agent.z += v.z * dt;
            // Sanity: the agent must never enter a blocked cell mid-follow.
            uint32_t c, r; grid->worldToCell(agent.x, agent.z, c, r);
            if (!grid->walkable(c, r)) { x3::logError("[nav-test] N3 agent entered blocked cell!"); break; }
        }
        const float distToGoal = planarDist(agent, goal);
        x3::logInfo(std::string("[nav-test] N3 arrived=") + (follower.arrived() ? "1" : "0") +
                    " steps=" + std::to_string(steps) +
                    " distToGoal=" + std::to_string(distToGoal));
        navcheck(follower.arrived() && distToGoal < 0.5f,
                 "N3 path-follow advances agent to goal");
    }

    // ---- N4: physics-sampled grid marks open floor walkable + a wall blocked. ----
    {
        std::unique_ptr<phys::IPhysicsWorld> w(phys::createPhysicsWorld());
        w->init();
        navGround(*w, 20.0f);
        // A static wall box across x=0: half-extents (0.5, 1.5, 6) centered at origin
        // (spans x in [-0.5,0.5], y in [0,3], z in [-6,6]), tall enough for the probe.
        w->addBox(phys::Vec3{ 0.5f, 1.5f, 6.0f }, phys::Vec3{ 0.0f, 1.5f, 0.0f },
                  0.0f, phys::Layer::Static);
        NavBuildParams p;
        p.minX = -10; p.maxX = 10; p.minZ = -10; p.maxZ = 10; p.cellSize = 1.0f;
        p.sampleTopY = 8.0f; p.sampleDepth = 20.0f; p.agentHeight = 1.8f; p.agentRadius = 0.4f;
        std::unique_ptr<INavGrid> grid(buildNavGridFromPhysics(*w, p));
        // Open floor cell at (-6,0): should be walkable, ground ~y=0.
        uint32_t oc, orr; grid->worldToCell(-6.0f, 0.0f, oc, orr);
        bool openWalk = grid->walkable(oc, orr);
        float gy = grid->cellCenter(oc, orr).y;
        // Wall cell at (0,0): should be blocked (obstacle probe hits the box).
        uint32_t wc, wr; grid->worldToCell(0.0f, 0.0f, wc, wr);
        bool wallBlocked = !grid->walkable(wc, wr);
        // And a path from one side to the other still exists (around the z-ends).
        NavPath around = grid->findPath(NavVec3{ -8, 0, 0 }, NavVec3{ 8, 0, 0 });
        x3::logInfo(std::string("[nav-test] N4 openWalk=") + (openWalk ? "1" : "0") +
                    " groundY=" + std::to_string(gy) +
                    " wallBlocked=" + (wallBlocked ? "1" : "0") +
                    " walkable=" + std::to_string(grid->walkableCount()) +
                    " aroundPts=" + std::to_string(around.points.size()));
        navcheck(openWalk && std::fabs(gy) < 0.2f && wallBlocked && around.ok(),
                 "N4 physics-sampled: open floor walkable, wall blocked, path routes around");
        w->shutdown();
    }

    // ---- N5: string-pull smoothing shortens the path vs the raw grid path. ----
    {
        NavBuildParams p;
        p.minX = -10; p.maxX = 10; p.minZ = -10; p.maxZ = 10; p.cellSize = 1.0f;
        p.agentRadius = 0.0f;
        std::unique_ptr<INavGrid> grid(buildEmptyNavGrid(p));
        grid->markBlockedBox(-0.5f, -10.0f, 0.5f, 4.0f);   // partial wall -> a bend
        const NavVec3 start{ -8.0f, 0.0f, -2.0f };
        const NavVec3 goal{ 8.0f, 0.0f, -2.0f };
        NavPath raw = grid->findPath(start, goal, false);
        NavPath smooth = grid->findPath(start, goal, true);
        auto pathLen = [](const NavPath& pp) {
            float L = 0.0f;
            for (size_t i = 1; i < pp.points.size(); ++i) L += planarDist(pp.points[i - 1], pp.points[i]);
            return L;
        };
        const float rawLen = pathLen(raw), smLen = pathLen(smooth);
        bool fewer = smooth.points.size() <= raw.points.size();
        bool shorter = smLen <= rawLen + 1e-3f;   // never longer; usually shorter
        bool stillReaches = pathValid(*grid, smooth, goal);
        x3::logInfo(std::string("[nav-test] N5 rawPts=") + std::to_string(raw.points.size()) +
                    " rawLen=" + std::to_string(rawLen) +
                    " smPts=" + std::to_string(smooth.points.size()) +
                    " smLen=" + std::to_string(smLen));
        navcheck(fewer && shorter && stillReaches,
                 "N5 string-pull smoothing shortens/keeps the path (still reaches)");
    }

    x3::logInfo(std::string("[nav-test] ") + std::to_string(nav_pass) + " passed, " +
                std::to_string(nav_fail) + " failed");
    return nav_fail == 0;
}

} // namespace x3::ai
