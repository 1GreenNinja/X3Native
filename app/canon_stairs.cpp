// THE FACILITY STAIRWELL — see canon_stairs.h. An open switchback stair tower south of
// the elevator-lobby column, joining the 7 normal floors with a see-through central well.
#include "canon_stairs.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace x3::game {

namespace {

// Doctrine stairs (LAW 3): riser ~0.18-0.22 m (auto-step clears 0.4), tread ~0.29 m.
constexpr float kRiser   = 0.20f;   // nominal; the real per-flight riser is h/nSteps (exact)
constexpr float kTreadMax = 0.30f;  // cap; the real tread is min(kTreadMax, run/nSteps)
constexpr float kStairW  = 1.5f;    // flight width (in Z)
constexpr float kMaxFlightH = 2.55f;   // per-flight rise cap so the run fits the tower width
constexpr float kRail    = 1.0f;    // balustrade height (fall protection along the well)
constexpr float kWall    = 0.2f;    // shaft wall / balustrade thickness

struct Ctx {
    Scene* scene; x3::rhi::IRenderDevice* device; x3::phys::IPhysicsWorld* physics;
    x3::rhi::TextureHandle steelTex, concreteTex;
};

// A solid box tagged kNoRoom (always-visible, like the elevator shaft). Returns entity id.
uint32_t box(Ctx& c, float hx, float hy, float hz, float cx, float cy, float cz,
             x3::rhi::TextureHandle tex, const float col[4], bool collide = true) {
    return canonAddBrush(*c.scene, *c.device, *c.physics, hx, hy, hz, cx, cy, cz,
                         tex, col, kNoRoom, collide, /*visible*/true);
}

// An emissive light-strip lying on a surface. No collision (a thin lip snags feet).
void strip(Ctx& c, float hx, float hy, float hz, float cx, float cy, float cz,
           float r, float g, float b, float strength) {
    const float col[4] = { r * 0.3f, g * 0.3f, b * 0.3f, 1.0f };
    uint32_t id = canonAddBrush(*c.scene, *c.device, *c.physics, hx, hy, hz, cx, cy, cz,
                                x3::rhi::TextureHandle{}, col, kNoRoom,
                                /*collide*/false, /*visible*/true);
    Entity& e = c.scene->get(id);
    e.emissive[0] = r; e.emissive[1] = g; e.emissive[2] = b; e.emissive[3] = strength;
}

// A flat landing deck: top flush at yTop, 0.15 m thick.
void deck(Ctx& c, float x0, float x1, float z0, float z1, float yTop,
          x3::rhi::TextureHandle tex, const float col[4]) {
    if (x1 - x0 < 0.05f || z1 - z0 < 0.05f) return;
    box(c, (x1 - x0) * 0.5f, 0.15f, (z1 - z0) * 0.5f,
        (x0 + x1) * 0.5f, yTop - 0.15f, (z0 + z1) * 0.5f, tex, col);
}

// One straight flight running along X from xStart, direction dir (+1/-1), band-centered at
// zc, climbing exactly `rise` over `nSteps` steps to top Y = y0+rise. Each step is a solid
// box from the tread top down (chunky, well-lit read, like canon_45's flight). Returns the
// X coordinate of the top tread edge (where the landing continues).
float flight(Ctx& c, float xStart, float zc, float y0, int dir, float rise, int nSteps,
             float run, x3::rhi::TextureHandle tex, const float col[4]) {
    const float riser = rise / nSteps;
    const float tread = run / nSteps;
    float x = xStart;
    for (int i = 0; i < nSteps; ++i) {
        const float yTop = y0 + (i + 1) * riser;
        x = xStart + dir * (i + 1) * tread;             // leading edge of this tread
        const float cx = x - dir * tread * 0.5f;        // tread center
        const float hy = 0.5f * std::min(0.9f, (i + 1) * riser + 0.3f);
        box(c, tread * 0.5f, hy, kStairW * 0.5f, cx, yTop - hy, zc, tex, col);
    }
    return x;
}

// A balustrade panel (fall guard) along a flight/landing's well-facing edge: a solid slab
// rising kRail with an emissive top cap so it reads as a railing.
void rail(Ctx& c, float x0, float x1, float zEdge, float yBase, x3::rhi::TextureHandle tex,
          const float col[4]) {
    if (x1 - x0 < 0.05f) return;
    box(c, (x1 - x0) * 0.5f, kRail * 0.5f, kWall * 0.5f,
        (x0 + x1) * 0.5f, yBase + kRail * 0.5f, zEdge, tex, col);
    strip(c, (x1 - x0) * 0.5f, 0.03f, kWall * 0.6f, (x0 + x1) * 0.5f, yBase + kRail, zEdge,
          0.55f, 0.75f, 1.0f, 1.1f);   // cool cyan cap line
}

// Even flight count for a rise so the climb returns to the start (west) side and each
// flight's run fits the tower width.
int flightCount(float rise) {
    int n = 2 * (int)std::ceil(rise / (2.0f * kMaxFlightH));
    return std::max(2, n);
}

} // namespace

StairPlan CanonStairwell::plan(const CanonFloor& floor) {
    StairPlan p;
    if (!floor.valid()) return p;

    // Gather the Elevator Lobby rooms (the normal floors), low -> high.
    std::vector<uint32_t> lobbies;
    for (uint32_t i = 0; i < (uint32_t)floor.rooms.size(); ++i)
        if (floor.rooms[i].type == "Elevator Lobby") lobbies.push_back(i);
    std::sort(lobbies.begin(), lobbies.end(),
              [&](uint32_t a, uint32_t b) { return floor.rooms[a].cy < floor.rooms[b].cy; });
    if (lobbies.size() < 2) return p;

    // Tower footprint: a fixed vertical column just SOUTH of the stacked lobby column.
    // The lobbies span x[19,25], their south wall at z ~= -28.5. The band z[-34,-28.6] is
    // clear on every floor (verified against the level data). Interior extents:
    p.x0t = 18.7f; p.x1t = 25.3f;      // interior X (walls at 18.5 / 25.5)
    p.zN  = -28.6f;                    // interior north face (just south of the lobby walls)
    p.zS  = -34.0f;                    // interior south face (inside the exterior facade)
    p.landingDepthX = 1.6f;
    // The open central well: the Z slot BETWEEN the north and south flight bands (open
    // all the way up — you see top-to-bottom through it).
    const float northBandZc = p.zN - kStairW * 0.5f - 0.05f;   // ~ -29.4
    const float southBandZc = p.zS + kStairW * 0.5f + 0.05f;   // ~ -33.2
    p.wellZ0 = southBandZc + kStairW * 0.5f;   // south edge of the well (~ -32.45)
    p.wellZ1 = northBandZc - kStairW * 0.5f;   // north edge of the well (~ -30.15)

    for (uint32_t lob : lobbies) {
        const CanonRoom& L = floor.rooms[lob];
        StairFloor sf;
        sf.lobby  = lob;
        sf.floorY = L.y0();
        sf.ceilY  = L.y1();
        // Opening in the lobby's -Z (south) wall, over the WEST landing (every floor
        // arrives on the west side — even flight counts). Keep it inside the wall span.
        const float westLandingCx = p.x0t + p.landingDepthX * 0.5f;   // ~19.5
        sf.openCenterX = std::min(std::max(westLandingCx, L.x0() + 0.9f), L.x1() - 0.9f);
        sf.openHalfX   = 0.8f;
        sf.openFace    = 2;    // -Z
        p.floors.push_back(sf);
    }

    p.bottomY = p.floors.front().floorY;
    p.topY    = floor.rooms[lobbies.back()].y1() + 0.5f;   // cap just above the top lobby ceiling
    p.valid   = true;
    return p;
}

std::vector<CanonBuildOpts::WallOpening> CanonStairwell::openings(const StairPlan& p) {
    std::vector<CanonBuildOpts::WallOpening> out;
    if (!p.valid) return out;
    for (const StairFloor& f : p.floors) {
        CanonBuildOpts::WallOpening o;
        o.room = f.lobby; o.face = f.openFace; o.center = f.openCenterX; o.half = f.openHalfX;
        out.push_back(o);
    }
    return out;
}

void CanonStairwell::build(const StairPlan& p, Scene& scene, x3::rhi::IRenderDevice& device,
                           x3::phys::IPhysicsWorld& physics, const std::string& surfaceLibRoot,
                           std::vector<CanonLight>& canonLights) {
    if (!p.valid) { x3::logInfo("[stairs] no lobby column — stairwell skipped"); return; }

    m_lib.mount(surfaceLibRoot);
    const SurfaceSet& steel = m_lib.get(device, "mw_metal_grate");
    const SurfaceSet& conc  = m_lib.get(device, "sr_concrete_01");
    Ctx c{ &scene, &device, &physics,
           steel.ok ? steel.albedo : x3::rhi::TextureHandle{},
           conc.ok  ? conc.albedo  : x3::rhi::TextureHandle{} };
    const float stepCol[4] = { 0.40f, 0.43f, 0.47f, 1.0f };   // galvanised steel
    const float deckCol[4] = { 0.44f, 0.46f, 0.50f, 1.0f };
    const float wallCol[4] = { 0.34f, 0.36f, 0.42f, 1.0f };   // shaft concrete, cool
    const float railCol[4] = { 0.30f, 0.33f, 0.40f, 1.0f };

    const float xMidW = p.x0t + p.landingDepthX;          // west flight-start x (~20.3)
    const float xMidE = p.x1t - p.landingDepthX;          // east flight-start x (~23.7)
    const float run   = xMidE - xMidW;                    // flight run in X
    const float northBandZc = p.zN - kStairW * 0.5f - 0.05f;
    const float southBandZc = p.zS + kStairW * 0.5f + 0.05f;
    const float exW = p.x0t - kWall * 0.5f, exE = p.x1t + kWall * 0.5f;   // wall plane X
    const float exN = p.zN - kWall * 0.5f, exS = p.zS + kWall * 0.5f;     // wall plane Z

    // ---- ENCLOSING SHELL ------------------------------------------------------------
    const float shaftH = p.topY - p.bottomY;
    const float shaftCy = p.bottomY + shaftH * 0.5f;
    // West / East / South walls: full height (north face is the lobby walls + gap-fills).
    box(c, kWall * 0.5f, shaftH * 0.5f, (p.zN - p.zS) * 0.5f + kWall,
        exW, shaftCy, (p.zN + p.zS) * 0.5f, c.concreteTex, wallCol);
    box(c, kWall * 0.5f, shaftH * 0.5f, (p.zN - p.zS) * 0.5f + kWall,
        exE, shaftCy, (p.zN + p.zS) * 0.5f, c.concreteTex, wallCol);
    box(c, (p.x1t - p.x0t) * 0.5f + kWall, shaftH * 0.5f, kWall * 0.5f,
        (p.x0t + p.x1t) * 0.5f, shaftCy, exS, c.concreteTex, wallCol);
    // North wall gap-fills: seal the north face ONLY in the Y ranges NOT already covered by
    // a lobby's own south wall (floorY..ceilY). The lobbies' south walls — with our cut
    // openings — are the north face at each floor band; we fill the voids between them so
    // the shaft is sealed with no coplanar double-face over any lobby wall.
    auto northFill = [&](float y0, float y1) {
        if (y1 - y0 < 0.05f) return;
        box(c, (p.x1t - p.x0t) * 0.5f + kWall, (y1 - y0) * 0.5f, kWall * 0.5f,
            (p.x0t + p.x1t) * 0.5f, (y0 + y1) * 0.5f, exN, c.concreteTex, wallCol);
    };
    {
        float y = p.bottomY;
        for (const StairFloor& sf : p.floors) {
            northFill(y, sf.floorY);   // void below this lobby's floor
            y = std::max(y, sf.ceilY); // skip over the lobby's own south-wall band
        }
        northFill(y, p.topY);          // void above the top lobby's ceiling
    }

    // ---- BOTTOM SLAB + TOP LID ------------------------------------------------------
    deck(c, exW, exE, exS, exN, p.bottomY, c.concreteTex, deckCol);
    box(c, (p.x1t - p.x0t) * 0.5f + kWall, kWall * 0.5f, (p.zN - p.zS) * 0.5f + kWall,
        (p.x0t + p.x1t) * 0.5f, p.topY + kWall * 0.5f, (p.zN + p.zS) * 0.5f,
        c.concreteTex, wallCol);

    // ---- THE SWITCHBACK CLIMB -------------------------------------------------------
    // Start each floor-pair at the WEST landing (floor Y). Even flight count returns to
    // west, one floor up. Flights alternate NORTH band (W->E) / SOUTH band (E->W), wrapping
    // the central open well; a landing joins the bands at each end.
    const float landW0 = p.x0t, landW1 = p.x0t + p.landingDepthX;    // west landing X span
    const float landE0 = p.x1t - p.landingDepthX, landE1 = p.x1t;    // east landing X span

    for (size_t i = 0; i + 1 < p.floors.size(); ++i) {
        const float y0 = p.floors[i].floorY;
        const float y1 = p.floors[i + 1].floorY;
        const float R  = y1 - y0;
        const int nFl  = flightCount(R);
        const float h  = R / nFl;                       // per-flight rise
        const int nSteps = std::max(2, (int)std::round(h / kRiser));
        const float tread = std::min(kTreadMax, run / nSteps);
        const float useRun = tread * nSteps;            // actual run used (<= run)

        float y = y0;
        for (int f = 0; f < nFl; ++f) {
            const bool east = (f % 2 == 0);             // even flight climbs West->East
            const float zc  = east ? northBandZc : southBandZc;
            const int   dir = east ? +1 : -1;
            const float xStart = east ? xMidW : xMidE;
            flight(c, xStart, zc, y, dir, h, nSteps, useRun, c.steelTex, stepCol);
            // well-facing balustrade along this flight
            const float railZ = east ? (northBandZc + kStairW * 0.5f) : (southBandZc - kStairW * 0.5f);
            rail(c, std::min(xStart, xStart + dir * useRun), std::max(xStart, xStart + dir * useRun),
                 railZ, y, c.steelTex, railCol);
            y += h;
            // landing at the arrival end (east on even flights, west on odd)
            if (east) deck(c, landE0, landE1, p.zS, p.zN, y, c.concreteTex, deckCol);
            else      deck(c, landW0, landW1, p.zS, p.zN, y, c.concreteTex, deckCol);
        }
    }
    // The very first (F1) west landing (the flights leave FROM it upward).
    deck(c, landW0, landW1, p.zS, p.zN, p.floors.front().floorY, c.concreteTex, deckCol);

    // ---- LIGHTS. Two layers so the shaft ALWAYS reads (the room-gated dynamic feed is
    // not guaranteed while the player is between lobbies / in a screenshot):
    //   (a) ALWAYS-DRAWN EMISSIVE fixtures (kNoRoom) — glowing bar lamps bolted to the east
    //       and west walls at regular Y, so the well is lit top-to-bottom even unlit.
    //   (b) a warm CanonLight pool per fixture, tagged to the nearest lobby, for real
    //       dynamic lighting when that floor is in the visible set.
    const float zMidT = (p.zN + p.zS) * 0.5f;
    auto nearestLobby = [&](float y) -> uint32_t {
        uint32_t best = p.floors.front().lobby; float bd = 1e9f;
        for (const StairFloor& sf : p.floors) {
            const float d = std::fabs(sf.floorY - y);
            if (d < bd) { bd = d; best = sf.lobby; }
        }
        return best;
    };
    auto wallLamp = [&](float wallX, float y, float r, float g, float b) {
        // an emissive bar lamp on the wall face (always drawn), + a matching dynamic pool
        const float col[4] = { r * 0.5f, g * 0.5f, b * 0.5f, 1.0f };
        const float inX = (wallX < (p.x0t + p.x1t) * 0.5f) ? +0.12f : -0.12f;
        uint32_t id = canonAddBrush(scene, device, physics, 0.10f, 0.35f, 0.10f,
                                    wallX + inX, y, zMidT, x3::rhi::TextureHandle{}, col,
                                    kNoRoom, /*collide*/false, /*visible*/true);
        Entity& e = scene.get(id);
        e.emissive[0] = r; e.emissive[1] = g; e.emissive[2] = b; e.emissive[3] = 2.2f;
        CanonLight cl; cl.room = nearestLobby(y);
        cl.light.pos[0] = wallX + inX * 6.0f; cl.light.pos[1] = y; cl.light.pos[2] = zMidT;
        cl.light.range  = 11.0f;
        cl.light.color[0] = r * 1.6f; cl.light.color[1] = g * 1.5f; cl.light.color[2] = b * 1.4f;
        canonLights.push_back(cl);
    };
    // Regular fixtures up the full shaft height (both walls, staggered) so the tall
    // F4->F5 stretch (~35 m, past hidden 4.5) never goes black.
    int lampRow = 0;
    for (float y = p.bottomY + 2.0f; y < p.topY - 1.0f; y += 5.0f, ++lampRow) {
        const bool west = (lampRow % 2 == 0);
        wallLamp(west ? (p.x0t + 0.15f) : (p.x1t - 0.15f), y, 1.0f, 0.86f, 0.62f);  // warm tungsten
    }
    // A brighter warm key at each floor landing so the arrival deck reads clearly.
    for (const StairFloor& sf : p.floors) {
        CanonLight cl; cl.room = sf.lobby;
        cl.light.pos[0] = (p.x0t + p.x1t) * 0.5f;
        cl.light.pos[1] = sf.floorY + 2.6f;
        cl.light.pos[2] = zMidT;
        cl.light.range  = 10.0f;
        cl.light.color[0] = 1.6f; cl.light.color[1] = 1.42f; cl.light.color[2] = 1.05f;
        canonLights.push_back(cl);
        strip(c, 0.06f, 0.22f, 0.06f, p.x1t - 0.22f, sf.floorY + 1.2f, zMidT,
              0.5f, 0.72f, 1.0f, 1.7f);
    }

    m_built = true;
    x3::logInfo("[stairs] SWITCHBACK stairwell built: " + std::to_string(p.floors.size()) +
                " floors, shaft " + std::to_string((int)shaftH) + " m, open well x[" +
                std::to_string((int)p.wellZ0) + "," + std::to_string((int)p.wellZ1) + "]");
}

} // namespace x3::game
