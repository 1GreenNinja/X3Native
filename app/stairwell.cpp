// THE FACILITY STAIRWELL — build (see stairwell.h). Geometry rides the loader's
// exported brush path (canonAddBrush) with the dressing surface sets applied
// entity-side (albedo+normal+MR — the D10/D15 law: nothing ships on the blown
// Lambert prim route). All coordinates derive from the loaded tower data through
// stairwellLayout(); the builder, the host's breach wiring and the lint gate share
// that one plan.
#include "stairwell.h"

#include "door.h"        // DoorSystem + buildLevelDoor (locked phantom-landing doors)
#include "keypad.h"      // realistic wall keypad beside each locked door
#include "holo_panel.h"  // holo::drawText — painted floor numbers
#include "canon_45.h"    // floorPlaneY — marks the UNNUMBERED (4.5-height) door

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace x3::game {

namespace {

// ---- Fixed plan constants (world meters). The shaft sits on the tower's WEST edge
// (x < the westmost room walls, inside the glass facade), z-centred on the band every
// floor's west room covers. Chosen against the level data: no room on any floor
// intersects this box (F5's Drone Bay Alpha comes closest, flush at x=2.0), and the
// 4.5 cavern envelope (x >= ~6.2) is well clear — the canon guard.
constexpr float kSX0 = -3.6f, kSX1 = 0.8f;      // shaft exterior X
constexpr float kSZ0 = -24.6f, kSZ1 = -16.2f;   // shaft exterior Z
constexpr float kWallT   = 0.25f;               // shaft/connector wall thickness
constexpr float kLandD   = 2.4f;                // landing depth (Z)
constexpr float kTread   = 0.31f;               // tread depth
constexpr int   kTreads  = 10;                  // treads per flight (11 risers)
constexpr float kRun     = kTreads * kTread;    // 3.1 m flight run
constexpr float kLaneW   = 1.56f;               // flight lane width (X)
constexpr float kParH    = 1.02f;               // parapet height above local walking surface
constexpr float kDoorH   = StairwellLayout::kDoorH;
constexpr float kDoorZ   = StairwellLayout::kDoorZ;
constexpr float kDoorHalfW = StairwellLayout::kDoorHalfW;
// Interior derived
constexpr float kIX0 = kSX0 + kWallT, kIX1 = kSX1 - kWallT;
constexpr float kIZ0 = kSZ0 + kWallT, kIZ1 = kSZ1 - kWallT;
constexpr float kRunZ0 = kIZ0 + kLandD;         // -21.95  (S edge of the run)
constexpr float kRunZ1 = kIZ1 - kLandD;         // -18.85  (N edge of the run)
constexpr float kLaneWC = kIX0 + kLaneW * 0.5f; // west lane center
constexpr float kLaneEC = kIX1 - kLaneW * 0.5f; // east lane center
constexpr float kWellX0 = kIX0 + kLaneW;        // open well X span (between the lanes)
constexpr float kWellX1 = kIX1 - kLaneW;
// F1 L-connector to Bottom Hall (data: x[4..40], z[-1..3], west wall at x=4).
constexpr float kLegNX0 = -1.8f, kLegNX1 = 0.4f;   // north leg interior X
constexpr float kLegEZ  = 0.0f;                    // east-leg / breach center Z
constexpr float kConnH  = 2.6f;                    // connector interior height

// Max riser the doctrine allows (LAW 3), used to pick flights-per-story.
constexpr float kMaxFlightRise = 11.0f * 0.20f;   // 2.2 m per flight

int flightsForRise(float rise) {
    int pairs = (int)std::ceil(rise / (2.0f * kMaxFlightRise));
    if (pairs < 1) pairs = 1;
    return pairs * 2;                              // even => every floor lands NORTH
}

struct Ctx {
    Scene* scene; x3::rhi::IRenderDevice* device; x3::phys::IPhysicsWorld* physics;
    const SurfaceSet* deck;   // hh_floor_01a — landings/connector floors
    const SurfaceSet* wall;   // hh_wall_01a  — every wall face
    const SurfaceSet* step;   // sr_concrete_01 — treads
    const SurfaceSet* steel;  // mw_metal_grate — parapets
    x3::rhi::TextureHandle matteMr;    // 1x1 fallback (rough .85, metal 0)
    x3::rhi::TextureHandle rubberAlb;  // 1x1 near-black
    x3::rhi::TextureHandle rubberMr;   // 1x1 rough .88, metal 0
};

uint32_t sbrush(Ctx& c, const SurfaceSet* s, const float tint[4],
                float hx, float hy, float hz, float cx, float cy, float cz,
                bool collide = true, bool visible = true) {
    const bool ok = s && s->ok;
    uint32_t id = canonAddBrush(*c.scene, *c.device, *c.physics, hx, hy, hz, cx, cy, cz,
                                ok ? s->albedo : x3::rhi::TextureHandle{}, tint,
                                kNoRoom, collide, visible);
    Entity& e = c.scene->get(id);
    if (ok) { e.mrTex = s->mr; e.normalTex = s->normal; }
    if (!e.mrTex.valid()) e.mrTex = c.matteMr;     // never the unnormalized Lambert route
    return id;
}

// Panelized wall slab (so the wall set tiles at a sane density instead of one UV
// square stretched over 90 m). axis 0: plane x=const (run along Z); axis 1: plane
// z=const (run along X).
void wallPanels(Ctx& c, const float tint[4], int axis, float plane,
                float u0, float u1, float y0, float y1) {
    if (u1 - u0 < 0.02f || y1 - y0 < 0.02f) return;
    const float kP = 3.2f;
    const int nu = std::max(1, (int)std::ceil((u1 - u0) / kP));
    const int ny = std::max(1, (int)std::ceil((y1 - y0) / kP));
    const float du = (u1 - u0) / (float)nu, dy = (y1 - y0) / (float)ny;
    for (int iu = 0; iu < nu; ++iu)
        for (int iy = 0; iy < ny; ++iy) {
            const float uc = u0 + (iu + 0.5f) * du, yc = y0 + (iy + 0.5f) * dy;
            if (axis == 0)
                sbrush(c, c.wall, tint, kWallT * 0.5f, dy * 0.5f, du * 0.5f,
                       plane, yc, uc);
            else
                sbrush(c, c.wall, tint, du * 0.5f, dy * 0.5f, kWallT * 0.5f,
                       uc, yc, plane);
        }
}

// Bake a painted floor-number plate texture ("1".."7"). Matte paint on a dark
// plate — honest albedo, no emissive. Returns invalid handle when the font is
// unavailable (caller skips the plate).
x3::rhi::TextureHandle bakeNumberPlate(x3::rhi::IRenderDevice& device,
                                       const std::string& label) {
    if (!holo::fontReady()) return {};
    constexpr uint32_t N = 128;
    holo::Canvas cv(N);
    const float px = 96.0f;
    const float w = holo::textWidth(label, px);
    holo::drawText(cv, label, std::max(4.0f, (N - w) * 0.5f), 16.0f, px,
                   holo::Ink{ 1.0f, 1.0f, 1.0f }, 1.0f);
    std::vector<uint8_t> rgba((size_t)N * N * 4);
    for (uint32_t i = 0; i < N * N; ++i) {
        const float a = std::min(1.0f, cv.r[i] + cv.g[i] + cv.b[i]);
        const float base[3] = { 40, 42, 45 }, paint[3] = { 216, 218, 222 };
        for (int k = 0; k < 3; ++k)
            rgba[(size_t)i * 4 + k] = (uint8_t)(base[k] + (paint[k] - base[k]) * a);
        rgba[(size_t)i * 4 + 3] = 255;
    }
    return device.createTexture(rgba.data(), N, N, true);
}

} // namespace

// =====================================================================================
// LAYOUT (pure data — shared by builder / host breach wiring / lint gate).
// =====================================================================================
StairwellLayout stairwellLayout(const CanonFloor& floor) {
    StairwellLayout L;
    L.sx0 = kSX0; L.sx1 = kSX1; L.sz0 = kSZ0; L.sz1 = kSZ1;
    if (!floor.valid() || floor.roomFloorNum.size() != floor.rooms.size()) return L;

    int maxFn = 1;
    for (int fn : floor.roomFloorNum) maxFn = std::max(maxFn, fn);

    for (int fn = 1; fn <= maxFn; ++fn) {
        StairwellLayout::FloorEntry fe; fe.floorNum = fn;
        if (fn == 1) {
            for (uint32_t i = 0; i < floor.rooms.size(); ++i)
                if (floor.roomFloorNum[i] == 1 && floor.rooms[i].name == "Bottom Hall") {
                    fe.room = i; break;
                }
        } else {
            float bestX0 = 6.6f;   // must be a WEST-edge room (x0 <= 6.5)
            for (uint32_t i = 0; i < floor.rooms.size(); ++i) {
                const CanonRoom& r = floor.rooms[i];
                if (floor.roomFloorNum[i] != fn) continue;
                if (r.platform || r.type == "Elevator Lobby") continue;
                if (r.z0() > kDoorZ - 0.9f || r.z1() < kDoorZ + 0.9f) continue;
                if (r.x0() < bestX0) { bestX0 = r.x0(); fe.room = i; }
            }
        }
        if (fe.room == kNoRoom) continue;          // this floor gets no landing door
        fe.floorY    = floor.rooms[fe.room].y0();
        fe.roomWallX = floor.rooms[fe.room].x0();
        L.floors.push_back(fe);
    }
    if (L.floors.size() < 2 || L.floors.front().floorNum != 1) return L;

    L.baseY = L.floors.front().floorY;
    L.topY  = L.floors.back().floorY + 3.4f;

    // North landings: F1, then per story every second flight arrival (phantom) + the
    // next real floor.
    L.north.push_back({ L.baseY, L.floors.front().floorNum });
    for (size_t s = 0; s + 1 < L.floors.size(); ++s) {
        const float yA = L.floors[s].floorY, yB = L.floors[s + 1].floorY;
        const int n = flightsForRise(yB - yA);
        const float fRise = (yB - yA) / (float)n;
        for (int k = 1; k < n / 2; ++k)
            L.north.push_back({ yA + fRise * 2.0f * (float)k, -1 });
        L.north.push_back({ yB, L.floors[s + 1].floorNum });
    }
    L.valid = true;

    // ---- THE MASTER ACCESS PLAN (owner order: backup code 7762 opens the
    // unnumbered door). Derived purely from the tower data so builder, Canon45's
    // wall cut and the lint gate agree. Route: leg A east from the unnumbered
    // landing's door cut, leg B north along the cavern's west flank, sealing onto
    // the cavern -Z rock wall's OUTER face where Canon45 cuts the mouth.
    {
        float env[6];
        if (Canon45::envelope(floor, env)) {
            const float y45 = env[4];
            int   best = -1; float bestD = 3.0f;   // within a story of the 4.5 plane
            for (int i = 0; i < (int)L.north.size(); ++i) {
                if (L.north[(size_t)i].floorNum > 0) continue;      // phantoms only
                const float d = std::fabs(L.north[(size_t)i].y - y45);
                if (d < bestD) { bestD = d; best = i; }
            }
            if (best >= 0) {
                StairwellLayout::MasterAccess& M = L.master;
                M.present  = true;
                M.landingY = L.north[(size_t)best].y;
                M.floorY   = y45;
                M.envZ0    = env[2];
                M.mouthX0  = env[0] + 0.3f;
                M.mouthX1  = M.mouthX0 + 1.8f;
                M.aZ0 = kDoorZ - (kDoorHalfW + 0.125f);
                M.aZ1 = kDoorZ + (kDoorHalfW + 0.125f);
                M.aX0 = L.sx1;
                M.aX1 = M.mouthX1;
                M.bX0 = M.mouthX0;
                M.bX1 = M.mouthX1;
                M.bZ0 = M.aZ1;
                M.bZ1 = env[2] - 0.8f;   // the cavern -Z wall's OUTER face
            }
        }
    }
    return L;
}

// =====================================================================================
// STAIR NAV (feat/stair-nav) — the walkable waypoint chain over the plan above.
// Shares the flight constants with the builder so the chain rides the REAL treads.
// =====================================================================================
namespace {
// Landing standoff from the run edge: keeps a landing waypoint clear of both the
// first tread's nosing and the well parapet (landing depth is 2.4 m; 0.7 m in).
constexpr float kNavLandIn = 0.7f;

StairNavChain::Wp navWp(float x, float y, float z) { return StairNavChain::Wp{ x, y, z }; }
} // namespace

StairNavChain stairwellNavChain(const StairwellLayout& lay) {
    StairNavChain c;
    if (!lay.valid) return c;

    // ---- Spine: every story's flights in zigzag order, bottom -> top. ----
    for (size_t s = 0; s + 1 < lay.floors.size(); ++s) {
        const float yA = lay.floors[s].floorY, yB = lay.floors[s + 1].floorY;
        const int   n = flightsForRise(yB - yA);
        const float fRise = (yB - yA) / (float)n;
        const float riser = fRise / (float)(kTreads + 1);
        for (int j = 0; j < n; ++j) {
            const float base  = yA + fRise * (float)j;
            const bool  toS   = (j % 2) == 0;               // even: N->S in the WEST lane
            const float laneC = toS ? kLaneWC : kLaneEC;
            const float zStart = toS ? kRunZ1 : kRunZ0;     // run edge at the start landing
            const float zEnd   = toS ? kRunZ0 : kRunZ1;
            const float inS    = toS ? kNavLandIn : -kNavLandIn;
            // Approach on the start landing (lane X, landing interior), the flight's
            // nosing line bottom -> top, then the arrival on the far landing.
            c.spine.push_back(navWp(laneC, base, zStart + inS));
            c.spine.push_back(navWp(laneC, base + 0.5f * riser, zStart));
            c.spine.push_back(navWp(laneC, base + ((float)kTreads + 0.5f) * riser, zEnd));
            c.spine.push_back(navWp(laneC, base + fRise, zEnd - inS));
        }
    }
    if (c.spine.empty()) return c;

    // ---- Exits: REAL floors only (phantom landings never get one — the 4.5 seal
    // is structural in the chain exactly like the geometry). ----
    for (size_t f = 0; f < lay.floors.size(); ++f) {
        const StairwellLayout::FloorEntry& fe = lay.floors[f];
        StairNavChain::Exit ex;
        ex.floorNum = fe.floorNum;
        ex.room     = fe.room;
        ex.floorY   = fe.floorY;
        // The spine waypoint at this floor's NORTH landing: nearest spine point at
        // the floor's Y on the north landing band (every floor lands north; flights
        // are even per story so both the story-start approach and the story-end
        // arrival sit at z = kRunZ1 + kNavLandIn).
        uint32_t best = 0; float bestD = 1e9f;
        for (uint32_t i = 0; i < (uint32_t)c.spine.size(); ++i) {
            const StairNavChain::Wp& w = c.spine[i];
            if (w.z < kRunZ1 - 0.01f) continue;             // north landing band only
            const float d = std::fabs(w.y - fe.floorY);
            if (d < bestD) { bestD = d; best = i; }
        }
        if (bestD > 0.05f) continue;                        // no landing at this floor (defensive)
        ex.spineIdx = best;
        if (fe.floorNum == 1) {
            // F1 exits NORTH through the L-connector into Bottom Hall.
            const float legX = (kLegNX0 + kLegNX1) * 0.5f;  // -0.7, the leg center
            ex.spur.push_back(navWp(legX, fe.floorY, kIZ1 - 0.6f));
            ex.spur.push_back(navWp(legX, fe.floorY, kLegEZ));
            ex.spur.push_back(navWp(fe.roomWallX + kNavLandIn, fe.floorY, kLegEZ));
        } else {
            // Upper floors exit EAST through the connector at kDoorZ.
            ex.spur.push_back(navWp(kIX1 - 0.6f, fe.floorY, kDoorZ));
            if (fe.roomWallX > kSX1 + 0.05f)
                ex.spur.push_back(navWp((kSX1 + fe.roomWallX) * 0.5f, fe.floorY, kDoorZ));
            ex.spur.push_back(navWp(fe.roomWallX + kNavLandIn, fe.floorY, kDoorZ));
        }
        c.exits.push_back(ex);
    }
    c.valid = c.exits.size() >= 2;
    return c;
}

bool stairNavRoute(const StairNavChain& chain, int fromFloor, int toFloor,
                   std::vector<StairNavChain::Wp>& out) {
    if (!chain.valid || fromFloor == toFloor) return false;
    const StairNavChain::Exit* a = chain.exitFor(fromFloor);
    const StairNavChain::Exit* b = chain.exitFor(toFloor);
    if (!a || !b) return false;                  // 4.5 / phantom / unserved: REFUSED
    out.clear();
    // Room-side entry -> connector -> the from-floor landing (spur reversed) ...
    for (size_t i = a->spur.size(); i-- > 0;) out.push_back(a->spur[i]);
    // ... up/down the spine between the two landings ...
    if (a->spineIdx <= b->spineIdx)
        for (uint32_t i = a->spineIdx; i <= b->spineIdx; ++i) out.push_back(chain.spine[i]);
    else
        for (uint32_t i = a->spineIdx + 1; i-- > b->spineIdx;) out.push_back(chain.spine[i]);
    // ... then out the to-floor connector to its room-side entry.
    for (const StairNavChain::Wp& w : b->spur) out.push_back(w);
    return out.size() >= 2;
}

// =====================================================================================
// BUILD.
// =====================================================================================
void FacilityStairwell::build(const StairwellLayout& lay, CanonFloor& floor, Scene& scene,
                              x3::rhi::IRenderDevice& device,
                              x3::phys::IPhysicsWorld& physics,
                              DoorSystem* doors, const std::string& surfaceLibRoot,
                              std::vector<CanonLight>& canonLights) {
    if (!lay.valid) {
        x3::logInfo("[stairwell] layout invalid — skipped");
        return;
    }
    m_phantoms.clear();
    m_flashIdx = -1;
    m_lib.mount(surfaceLibRoot);
    const SurfaceSet& deck  = m_lib.get(device, "hh_floor_01a");
    const SurfaceSet& wallS = m_lib.get(device, "hh_wall_01a");
    const SurfaceSet& conc  = m_lib.get(device, "sr_concrete_01");
    const SurfaceSet& grate = m_lib.get(device, "mw_metal_grate");

    Ctx c{ &scene, &device, &physics, &deck, &wallS, &conc, &grate, {}, {}, {} };
    {   // 1x1 utility texels: matte MR fallback + the black rubber nosing material.
        const uint8_t mr[4]  = { 0, 217, 0, 255 };       // rough .85, metal 0
        const uint8_t rmr[4] = { 0, 224, 0, 255 };       // rough .88, metal 0
        const uint8_t ralb[4] = { 14, 14, 15, 255 };     // ~0.05 linear — rubber
        c.matteMr  = device.createTexture(mr, 1, 1, false);
        c.rubberMr = device.createTexture(rmr, 1, 1, false);
        c.rubberAlb = device.createTexture(ralb, 1, 1, false);
    }
    const float deckTint[4]  = { 0.40f, 0.41f, 0.40f, 1.0f };  // the judged deck value
    const float wallTint[4]  = { 0.34f, 0.34f, 0.35f, 1.0f };
    const float stepTint[4]  = { 0.56f, 0.56f, 0.55f, 1.0f };
    const float steelTint[4] = { 0.40f, 0.42f, 0.44f, 1.0f };
    const float white[4]     = { 1, 1, 1, 1 };

    auto rubberNosing = [&](float cx, float cy, float cz, float hx, float hz) {
        // The institutional black safety strip on a tread lip: proud of the tread by
        // 6 mm, embedded into its front edge (no float, no coplanar face). No
        // collision — a 4 cm lip that snags feet is worse than none.
        uint32_t id = canonAddBrush(scene, device, physics, hx, 0.023f, hz,
                                    cx, cy, cz, c.rubberAlb, white, kNoRoom,
                                    /*collide*/false, /*visible*/true);
        Entity& e = scene.get(id);
        e.mrTex = c.rubberMr;
    };

    auto addLight = [&](float lx, float ly, float lz, float range,
                        float r, float g, float b) {
        CanonLight cl; cl.room = kNoRoom;   // un-roomed: range-gated by the feed
        cl.light.pos[0] = lx; cl.light.pos[1] = ly; cl.light.pos[2] = lz;
        cl.light.range = range;
        cl.light.color[0] = r; cl.light.color[1] = g; cl.light.color[2] = b;
        canonLights.push_back(cl);
    };
    auto lightHousing = [&](float hx0, float hy0, float hz0) {
        uint32_t id = sbrush(c, c.steel, steelTint, 0.05f, 0.045f, 0.17f,
                             hx0, hy0, hz0, /*collide*/false);
        Entity& e = scene.get(id);
        e.emissive[0] = 1.0f; e.emissive[1] = 0.85f; e.emissive[2] = 0.62f;
        e.emissive[3] = 1.3f;
    };

    // ---- Base slab + top lid. ----
    sbrush(c, c.deck, deckTint, (kIX1 - kIX0) * 0.5f, 0.15f, (kIZ1 - kIZ0) * 0.5f,
           (kIX0 + kIX1) * 0.5f, lay.baseY - 0.15f, (kIZ0 + kIZ1) * 0.5f);
    sbrush(c, c.wall, wallTint, (kSX1 - kSX0) * 0.5f, 0.125f, (kSZ1 - kSZ0) * 0.5f,
           (kSX0 + kSX1) * 0.5f, lay.topY + 0.125f, (kSZ0 + kSZ1) * 0.5f);

    // ---- Shaft walls. West + south full; north with the F1 connector opening;
    // east with a door opening at EVERY north landing. ----
    wallPanels(c, wallTint, 0, kSX0 + kWallT * 0.5f, kSZ0, kSZ1, lay.baseY, lay.topY);   // west
    wallPanels(c, wallTint, 1, kSZ0 + kWallT * 0.5f, kSX0, kSX1, lay.baseY, lay.topY);   // south
    {   // north wall: opening x[-1.6..0.2], y[baseY..baseY+kDoorH] for the F1 leg.
        const float p = kSZ1 - kWallT * 0.5f;
        wallPanels(c, wallTint, 1, p, kSX0, -1.6f, lay.baseY, lay.baseY + kDoorH);
        wallPanels(c, wallTint, 1, p, 0.2f, kSX1, lay.baseY, lay.baseY + kDoorH);
        wallPanels(c, wallTint, 1, p, kSX0, kSX1, lay.baseY + kDoorH, lay.topY);
    }
    {   // east wall: vertical bands between door openings + side fillers at each.
        const float p = kSX1 - kWallT * 0.5f;
        float cursor = lay.baseY;
        for (const StairwellLayout::NorthLanding& nl : lay.north) {
            if (nl.floorNum == 1) continue;   // F1 exits NORTH (the L-leg), no east door
            if (nl.y > cursor + 0.01f)
                wallPanels(c, wallTint, 0, p, kSZ0, kSZ1, cursor, nl.y);
            // Opening band: z-side fillers around [kDoorZ +- kDoorHalfW].
            wallPanels(c, wallTint, 0, p, kSZ0, kDoorZ - kDoorHalfW, nl.y, nl.y + kDoorH);
            wallPanels(c, wallTint, 0, p, kDoorZ + kDoorHalfW, kSZ1, nl.y, nl.y + kDoorH);
            cursor = nl.y + kDoorH;
        }
        if (lay.topY > cursor) wallPanels(c, wallTint, 0, p, kSZ0, kSZ1, cursor, lay.topY);
    }

    // ---- Stories: flights + landings + the open railed well. ----
    uint32_t nFlights = 0;
    for (size_t s = 0; s + 1 < lay.floors.size(); ++s) {
        const float yA = lay.floors[s].floorY, yB = lay.floors[s + 1].floorY;
        const int n = flightsForRise(yB - yA);
        const float fRise = (yB - yA) / (float)n;
        const float riser = fRise / (float)(kTreads + 1);

        for (int j = 0; j < n; ++j, ++nFlights) {
            const float base = yA + fRise * (float)j;
            const bool  toS  = (j % 2) == 0;                 // even: N->S in the WEST lane
            const float laneC = toS ? kLaneWC : kLaneEC;
            const float zEdge = toS ? kRunZ1 : kRunZ0;       // start edge
            const float dir   = toS ? -1.0f : +1.0f;         // tread advance in Z
            // Treads (thin-waist boxes: 0.35 solid below each top — headroom under the
            // flight two turns up stays > 2 m) + the rubber nosing on every lip.
            for (int i = 1; i <= kTreads; ++i) {
                const float top = base + riser * (float)i;
                const float zc  = zEdge + dir * ((float)i - 0.5f) * kTread;
                sbrush(c, c.step, stepTint, kLaneW * 0.5f, 0.175f, kTread * 0.5f,
                       laneC, top - 0.175f, zc);
                const float lipZ = zEdge + dir * (float)(i - 1) * kTread;   // approach edge
                rubberNosing(laneC, top - 0.017f, lipZ + dir * 0.035f,
                             kLaneW * 0.5f - 0.02f, 0.035f);
            }
            // Final riser face up to the far landing is covered by the landing slab lip.
            // Parapet on the WELL side of the flight: 3 stepped segments.
            const float parX = toS ? (kWellX0 + 0.035f) : (kWellX1 - 0.035f);
            for (int seg = 0; seg < 3; ++seg) {
                const int i0 = seg * 3 + 1, i1 = (seg == 2) ? kTreads : (seg + 1) * 3;
                const float zA = zEdge + dir * (float)(i0 - 1) * kTread;
                const float zB = zEdge + dir * (float)i1 * kTread;
                const float lo = base + riser * (float)i0 - 0.45f;
                const float hi = base + riser * (float)i1 + kParH;
                sbrush(c, c.steel, steelTint, 0.035f, (hi - lo) * 0.5f,
                       std::fabs(zB - zA) * 0.5f, parX, (lo + hi) * 0.5f,
                       (zA + zB) * 0.5f);
            }
        }

        // Landings inside the story: north (even arrivals, phantom-doored) + south
        // (odd arrivals, plain turns). The real floor landings' slabs are handled by
        // the same math (k == 0 is floor A itself; the k == n/2 north arrival is
        // floor B, whose slab the next story or the floor entry below lays).
        for (int j = 1; j < n; ++j) {
            const float ly = yA + fRise * (float)j;
            const bool north = (j % 2) == 0;
            const float z0 = north ? kRunZ1 : kIZ0;
            const float z1 = north ? kIZ1 : kRunZ0;
            sbrush(c, c.deck, deckTint, (kIX1 - kIX0) * 0.5f, 0.15f, (z1 - z0) * 0.5f,
                   (kIX0 + kIX1) * 0.5f, ly - 0.15f, (z0 + z1) * 0.5f);
            // Well-edge parapet across the landing's open edge.
            const float pz = north ? (kRunZ1 - 0.035f) : (kRunZ0 + 0.035f);
            sbrush(c, c.steel, steelTint, (kWellX1 - kWellX0) * 0.5f,
                   (kParH + 0.35f) * 0.5f, 0.035f,
                   (kWellX0 + kWellX1) * 0.5f, ly + (kParH - 0.35f) * 0.5f, pz);
            // South turn landings carry their own practical so the runs read lit
            // (the north lights alone left the flights near-black).
            if (!north) {
                lightHousing((kIX0 + kIX1) * 0.5f, ly + 2.52f, kIZ0 + 0.35f);
                addLight((kIX0 + kIX1) * 0.5f, ly + 2.35f, kIZ0 + 1.0f, 5.5f,
                         1.25f, 1.05f, 0.75f);
            }
        }
    }
    // Real-floor landing slabs (north end) + their well parapets.
    for (const StairwellLayout::FloorEntry& fe : lay.floors) {
        if (fe.floorNum == 1) continue;                    // base slab already spans F1
        sbrush(c, c.deck, deckTint, (kIX1 - kIX0) * 0.5f, 0.15f, (kIZ1 - kRunZ1) * 0.5f,
               (kIX0 + kIX1) * 0.5f, fe.floorY - 0.15f, (kRunZ1 + kIZ1) * 0.5f);
        sbrush(c, c.steel, steelTint, (kWellX1 - kWellX0) * 0.5f,
               (kParH + 0.35f) * 0.5f, 0.035f,
               (kWellX0 + kWellX1) * 0.5f, fe.floorY + (kParH - 0.35f) * 0.5f,
               kRunZ1 - 0.035f);
    }
    // F1 base: the well reaches the base slab (a vaulted fall LANDS here — real
    // floor, walk out the F1 connector; no void, no softlock). Rail the well mouth
    // at the base too so nobody wanders under the first flights into a head-knock.
    sbrush(c, c.steel, steelTint, (kWellX1 - kWellX0) * 0.5f, (kParH + 0.35f) * 0.5f,
           0.035f, (kWellX0 + kWellX1) * 0.5f, lay.baseY + (kParH - 0.35f) * 0.5f,
           kRunZ1 - 0.035f);

    // ---- Per-landing doors, keypads, numbers, practicals. ----
    const float doorPlaneX = kSX1 - kWallT * 0.5f;         // door slab center plane
    uint32_t lockedDoors = 0;
    for (const StairwellLayout::NorthLanding& nl : lay.north) {
        const bool real = nl.floorNum > 0;
        if (nl.floorNum == 1) {
            // F1: the exit is the NORTH opening (L-leg to Bottom Hall) — practical +
            // painted number over THAT mouth instead of the east wall.
            lightHousing(-0.7f, nl.y + kDoorH + 0.18f, kIZ1 - 0.06f);
            addLight(-0.7f, nl.y + kDoorH + 0.05f, kIZ1 - 0.5f, 6.0f, 1.35f, 1.10f, 0.75f);
            x3::rhi::TextureHandle plate1 = bakeNumberPlate(device, "1");
            if (plate1.valid()) {
                uint32_t id = canonAddBrush(scene, device, physics, 0.26f, 0.20f, 0.022f,
                                            -0.7f, nl.y + kDoorH + 0.42f, kIZ1 - 0.015f,
                                            plate1, white, kNoRoom, false, true);
                scene.get(id).mrTex = c.matteMr;
            }
            continue;
        }
        // Practical: warm housing over the door + an un-roomed ranged light.
        lightHousing(kIX1 - 0.06f, nl.y + kDoorH + 0.18f, kDoorZ);
        addLight(kIX1 - 0.5f, nl.y + kDoorH + 0.05f, kDoorZ, 6.0f, 1.35f, 1.10f, 0.75f);

        if (real) {
            // Painted floor number above the opening (numbering skips 4.5 by
            // construction — phantom landings are unnumbered).
            x3::rhi::TextureHandle plate =
                bakeNumberPlate(device, std::to_string(nl.floorNum));
            if (plate.valid()) {
                uint32_t id = canonAddBrush(scene, device, physics, 0.022f, 0.20f, 0.26f,
                                            kIX1 - 0.015f, nl.y + kDoorH + 0.42f, kDoorZ,
                                            plate, white, kNoRoom, false, true);
                scene.get(id).mrTex = c.matteMr;
            }
            continue;                                       // real floors: open mouth
        }
        // PHANTOM landing: locked SM_Door_A slab in the opening + a keypad. The
        // service code 4545 (taught by Okafor's work order at the F1 entrance) is
        // handled by THIS module (submitCode): the keypad answers, the door stays
        // shut. The UNNUMBERED landing at 4.5's height is the MASTER DOOR: coded
        // 7762 (the owner's undocumented backup — it really opens), with the
        // master L-connector behind it instead of a sealed backing box.
        const bool isTell = lay.master.present &&
                            std::fabs(nl.y - lay.master.landingY) < 0.01f;
        PhantomDoor pd;
        if (doors) {
            DoorSpec spec;
            spec.doorwayCenter = x3::phys::Vec3{ doorPlaneX, nl.y, kDoorZ };
            spec.axis        = DoorAxis::AlongZ;            // slab thin in X
            spec.halfWidth   = kDoorHalfW;
            spec.height      = kDoorH - 0.2f;
            spec.withButton  = false;
            spec.locked      = true;
            // Ordinary phantoms carry a NEGATIVE sentinel, deliberately: nonzero
            // => the E-aim path still offers the keypad, but NO 4-digit entry
            // (0..9999) can ever match in DoorSystem::tryDoorCode — those slabs
            // are unopenable by code, period (their voids are real voids). 4545
            // never reaches DoorSystem: the host offers it to submitCode() first,
            // which answers WITHOUT opening. The MASTER DOOR alone carries 7762
            // and opens through the standard door machinery.
            spec.code        = isTell ? kMasterCode : -kServiceCode;
            spec.tint[0] = 0.35f; spec.tint[1] = 0.37f; spec.tint[2] = 0.40f;
            const uint32_t di = buildLevelDoor(scene, *doors, device, physics, spec);
            const uint32_t ent = doors->at(di).entity;
            if (ent != kNoLink && ent < scene.size())
                scene.get(ent).roomId = kNoRoom;            // always-visible
            pd.doorIndex = di;
            pd.entity    = ent;
            pd.center    = spec.doorwayCenter;
            pd.sublevelTell = isTell;
            ++lockedDoors;
        }
        if (!isTell) {
            // Sealed backing (dark) just outside the wall plane — nothing exists there.
            sbrush(c, c.wall, wallTint, 0.125f, (kDoorH + 0.2f) * 0.5f, kDoorHalfW + 0.15f,
                   kSX1 + 0.125f, nl.y + kDoorH * 0.5f, kDoorZ);
        }
        pd.keypad = buildKeypad(scene, device, kIX1, nl.y + 1.40f,
                                kDoorZ - kDoorHalfW - 0.28f,
                                KeypadFacing::MinusX, KeypadStatus::Locked, kNoRoom);
        if (doors) m_phantoms.push_back(pd);
    }

    // ---- THE MASTER CONNECTOR (owner order: 7762 opens the unnumbered door).
    // An L-shaped service tunnel from the master door east along the door cut,
    // then north along the cavern's west flank, sealing flush onto the 4.5
    // cavern's -Z rock wall OUTER face (Canon45 cuts the sanctioned mouth there
    // from the same MasterAccess plan). Dressed like the 4.5 arrival tunnel:
    // dark concrete, one dim practical, ominous. Floor top rides the cavern
    // floor plane (the sill from the landing is a small auto-step; the lint
    // gate bounds it at 0.35 m).
    if (lay.master.present && doors) {
        const StairwellLayout::MasterAccess& M = lay.master;
        const float fy = M.floorY, ly = M.landingY;
        const float topY = std::max(fy, ly) + StairwellLayout::kMasterH;  // one lid plane
        const float darkTint[4] = { 0.24f, 0.23f, 0.22f, 1.0f };   // swallowed light
        auto seg = [&](float x0, float x1, float y0, float y1, float z0, float z1) {
            if (x1 - x0 < 0.02f || y1 - y0 < 0.02f || z1 - z0 < 0.02f) return;
            sbrush(c, c.step, darkTint, (x1 - x0) * 0.5f, (y1 - y0) * 0.5f,
                   (z1 - z0) * 0.5f, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f,
                   (z0 + z1) * 0.5f);
        };
        // ---- Floor profile in leg A: a pad at the door sill (landing y), then a
        // doctrine flight (risers <= 0.2, LAW 3) down/up to the cavern floor
        // plane, then flat to the corner. On this dataset the landing sits ~1.4 m
        // ABOVE the 4.5 floor — the descent into the dark is the arrival beat.
        const float drop = ly - fy;                 // + = steps descend going east
        float lowX0 = M.aX0 + 1.2f;                 // where the flat-at-fy floor starts
        if (std::fabs(drop) > 0.05f) {
            const int   nR    = std::max(1, (int)std::ceil(std::fabs(drop) / 0.2f));
            const float rise  = drop / (float)nR;   // signed per-step drop
            const float tread = 0.31f;
            // Entry pad at the sill.
            seg(M.aX0, lowX0, ly - 0.3f, ly, M.aZ0, M.aZ1);
            // Treads 1..nR-1 (the base floor at fy IS the final step — building
            // tread nR would lay a second top face coplanar with it).
            for (int i = 1; i < nR; ++i) {
                const float tx0 = lowX0 + (float)(i - 1) * tread;
                const float top = ly - rise * (float)i;
                seg(tx0, tx0 + tread, top - 0.35f, top, M.aZ0, M.aZ1);
            }
            lowX0 += (float)nR * tread;
        } else {
            lowX0 = M.aX0;                          // flush enough: one flat floor
        }
        // Flat floor at fy: the FULL leg A span (it also seals the tube's underside
        // beneath the entry pad + treads — no open face into the structural void)
        // + all of leg B.
        seg(M.aX0, M.aX1, fy - 0.3f, fy, M.aZ0, M.aZ1);
        seg(M.bX0, M.bX1, fy - 0.3f, fy, M.aZ1, M.bZ1);
        (void)lowX0;
        // Lids (bottom at topY — tall over the low section: an honest service duct).
        seg(M.aX0 - 0.25f, M.aX1 + 0.25f, topY, topY + 0.25f, M.aZ0 - 0.25f, M.aZ1 + 0.25f);
        seg(M.bX0 - 0.25f, M.bX1 + 0.25f, topY, topY + 0.25f, M.aZ1, M.bZ1);
        // Walls (fy up to the lid — they cover both floor levels).
        // South wall of the L (leg A, z = aZ0 plane).
        seg(M.aX0, M.aX1 + 0.25f, fy, topY, M.aZ0 - 0.25f, M.aZ0);
        // North wall of leg A up to leg B's west side (east of that leg B opens).
        seg(M.aX0, M.bX0, fy, topY, M.aZ1, M.aZ1 + 0.25f);
        // East wall of the L (x = aX1 == bX1 plane), full run to the cavern wall.
        seg(M.aX1, M.aX1 + 0.25f, fy, topY, M.aZ0 - 0.25f, M.bZ1);
        // West wall of leg B (x = bX0 plane), north of leg A.
        seg(M.bX0 - 0.25f, M.bX0, fy, topY, M.aZ1 + 0.25f, M.bZ1);
        // Two dim practicals — one over the entry flight (the door reveal must
        // read as a REAL PLACE, not a void), one mid leg B to find the turn.
        // Enough to walk by, not enough to feel safe (honest housings +
        // un-roomed ranged lights, the stairwell pattern).
        lightHousing(M.aX0 + 2.0f, topY - 0.10f, kDoorZ);
        addLight(M.aX0 + 2.0f, topY - 0.35f, kDoorZ, 4.5f, 0.90f, 0.74f, 0.52f);
        lightHousing((M.bX0 + M.bX1) * 0.5f, topY - 0.10f, (M.bZ0 + M.bZ1) * 0.5f);
        addLight((M.bX0 + M.bX1) * 0.5f, topY - 0.35f, (M.bZ0 + M.bZ1) * 0.5f,
                 4.0f, 0.85f, 0.70f, 0.50f);
        x3::logInfo("[stairwell] MASTER CONNECTOR built: door y=" +
                    std::to_string(M.landingY) + " -> cavern mouth x[" +
                    std::to_string(M.mouthX0) + ".." + std::to_string(M.mouthX1) +
                    "] at z=" + std::to_string(M.envZ0) + " (code-locked, owner key)");
    }

    // ---- Per-floor CONNECTORS (east, straight) + the F1 L-connector. ----
    for (const StairwellLayout::FloorEntry& fe : lay.floors) {
        if (fe.floorNum == 1) continue;
        const float ly = fe.floorY, rx = fe.roomWallX;
        if (rx <= kSX1 + 0.05f) continue;                   // flush room: opening only
        // Floor / lid / side walls sealing shaft mouth -> room breach (LAW 2: the
        // corridor's walls land flush on both cut planes).
        sbrush(c, c.deck, deckTint, (rx - kIX1) * 0.5f, 0.15f, 1.1f,
               (kIX1 + rx) * 0.5f, ly - 0.15f, kDoorZ);
        sbrush(c, c.wall, wallTint, (rx - kSX1) * 0.5f, 0.10f, 1.1f,
               (kSX1 + rx) * 0.5f, ly + kConnH + 0.10f, kDoorZ);
        wallPanels(c, wallTint, 1, kDoorZ - kDoorHalfW - kWallT * 0.5f,
                   kSX1, rx, ly, ly + kConnH);
        wallPanels(c, wallTint, 1, kDoorZ + kDoorHalfW + kWallT * 0.5f,
                   kSX1, rx, ly, ly + kConnH);
        addLight((kSX1 + rx) * 0.5f, ly + kConnH - 0.25f, kDoorZ, 4.5f,
                 1.25f, 1.05f, 0.75f);
    }
    {   // F1: north leg (shaft -> z of Bottom Hall) + east leg (-> its west wall).
        const float ly = lay.baseY;
        const float ez0 = kLegEZ - kDoorHalfW - 0.125f, ez1 = kLegEZ + kDoorHalfW + 0.125f;
        // North leg floor + walls + lid.
        sbrush(c, c.deck, deckTint, (kLegNX1 - kLegNX0) * 0.5f, 0.15f,
               (ez1 + kWallT - kSZ1) * 0.5f,
               (kLegNX0 + kLegNX1) * 0.5f, ly - 0.15f, (kSZ1 + ez1 + kWallT) * 0.5f);
        wallPanels(c, wallTint, 0, kLegNX0 - kWallT * 0.5f, kSZ1, ez1 + kWallT, ly, ly + kConnH);
        wallPanels(c, wallTint, 0, kLegNX1 + kWallT * 0.5f, kSZ1, ez0, ly, ly + kConnH);
        wallPanels(c, wallTint, 1, ez1 + kWallT * 0.5f, kLegNX0, kLegNX1, ly, ly + kConnH);
        sbrush(c, c.wall, wallTint, (kLegNX1 - kLegNX0) * 0.5f + kWallT, 0.10f,
               (ez1 + kWallT - kSZ1) * 0.5f,
               (kLegNX0 + kLegNX1) * 0.5f, ly + kConnH + 0.10f, (kSZ1 + ez1 + kWallT) * 0.5f);
        // East leg floor + walls + lid (breach cut in Bottom Hall's west wall at rx).
        const float rx = lay.floors.front().roomWallX;
        sbrush(c, c.deck, deckTint, (rx - kLegNX1) * 0.5f, 0.15f, (ez1 - ez0) * 0.5f,
               (kLegNX1 + rx) * 0.5f, ly - 0.15f, kLegEZ);
        wallPanels(c, wallTint, 1, ez0 - kWallT * 0.5f, kLegNX1, rx, ly, ly + kConnH);
        wallPanels(c, wallTint, 1, ez1 + kWallT * 0.5f, kLegNX1, rx, ly, ly + kConnH);
        sbrush(c, c.wall, wallTint, (rx - kLegNX1) * 0.5f, 0.10f, (ez1 - ez0) * 0.5f + kWallT,
               (kLegNX1 + rx) * 0.5f, ly + kConnH + 0.10f, kLegEZ);
        // Service-corridor practicals down the long leg.
        addLight((kLegNX0 + kLegNX1) * 0.5f, ly + kConnH - 0.25f, -11.0f, 5.5f, 1.25f, 1.05f, 0.75f);
        addLight((kLegNX0 + kLegNX1) * 0.5f, ly + kConnH - 0.25f, -4.0f, 5.5f, 1.25f, 1.05f, 0.75f);
        addLight(2.2f, ly + kConnH - 0.25f, kLegEZ, 4.5f, 1.25f, 1.05f, 0.75f);
        lightHousing((kLegNX0 + kLegNX1) * 0.5f, ly + kConnH - 0.06f, -11.0f);
        lightHousing((kLegNX0 + kLegNX1) * 0.5f, ly + kConnH - 0.06f, -4.0f);
    }

    m_built = true;
    int tells = 0;
    for (const PhantomDoor& pd : m_phantoms) if (pd.sublevelTell) ++tells;
    x3::logInfo("[stairwell] built: " + std::to_string(lay.floors.size()) +
                " floor landings, " + std::to_string(lay.north.size()) +
                " north landings (" + std::to_string(lockedDoors) +
                " locked keypad doors, " + std::to_string(tells) +
                " unnumbered 4.5-height tell), " + std::to_string(nFlights) +
                " flights, base y=" + std::to_string(lay.baseY) +
                " top y=" + std::to_string(lay.topY));
    (void)floor;
}

// =====================================================================================
// THE SERVICE-VOID CODE (feat/secret-code-clues) — 4545 answers; nothing opens.
// =====================================================================================
bool FacilityStairwell::isPhantomDoorEntity(uint32_t entity) const {
    if (entity == kNoLink) return false;
    for (const PhantomDoor& pd : m_phantoms)
        if (pd.entity == entity) return true;
    return false;
}

void FacilityStairwell::startFlash(int idx, Scene& scene) {
    // A previous flash mid-sequence snaps back to red first (one keypad answers
    // at a time; the sequencer owns exactly one screen).
    if (m_flashIdx >= 0 && m_flashIdx != idx &&
        m_flashIdx < (int)m_phantoms.size())
        setKeypadStatus(scene, m_phantoms[(size_t)m_flashIdx].keypad,
                        KeypadStatus::Locked);
    m_flashIdx = idx;
    m_flashT   = 0.0f;
    setKeypadStatus(scene, m_phantoms[(size_t)idx].keypad, KeypadStatus::Unlocked);
}

FacilityStairwell::CodeResponse FacilityStairwell::submitCode(
        const x3::phys::Vec3& eye, int code, Scene& scene, float range) {
    if (!m_built || m_phantoms.empty()) return CodeResponse::NotHandled;
    // Nearest phantom door within range (full 3D distance: landings stack
    // vertically every ~2.2 m, so an XZ-only match would grab the whole column).
    int best = -1; float bestD2 = range * range;
    for (int i = 0; i < (int)m_phantoms.size(); ++i) {
        const x3::phys::Vec3& p = m_phantoms[(size_t)i].center;
        const float dx = eye.x - p.x, dy = eye.y - (p.y + 1.4f), dz = eye.z - p.z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bestD2) { bestD2 = d2; best = i; }
    }
    if (best < 0) return CodeResponse::NotHandled;
    const CodeResponse r = classifyCode(code, m_phantoms[(size_t)best].sublevelTell);
    if (r == CodeResponse::NotHandled) return r;     // wrong code: host red-reject path
    startFlash(best, scene);                          // GREEN -> AMBER -> red; door stays shut
    x3::logInfo(std::string("[stairwell] service code 4545 accepted at ") +
                (r == CodeResponse::SublevelTell ? "the UNNUMBERED door — "
                 "SUBLEVEL ACCESS VIA PRIMARY LIFT ONLY - SEE CHIEF ENGINEER"
                 : "a service-void door — "
                 "SERVICE VOID - NO ATMOSPHERE - ENTRY DENIED") +
                " (door stays sealed)");
    return r;
}

void FacilityStairwell::update(float dt, Scene& scene, DoorSystem* doors,
                               const x3::phys::Vec3& playerPos) {
    // ---- Keypad flash sequencing (GREEN 0.7 s -> AMBER 3 s -> red). ----
    if (m_flashIdx >= 0) {
        if (m_flashIdx >= (int)m_phantoms.size()) { m_flashIdx = -1; return; }
        m_flashT += dt;
        const PhantomDoor& pd = m_phantoms[(size_t)m_flashIdx];
        if (m_flashT >= 3.7f) {                       // sequence over: back to red
            setKeypadStatus(scene, pd.keypad, KeypadStatus::Locked);
            m_flashIdx = -1;
        } else if (m_flashT >= 0.7f) {                // green beat over: amber denial
            setKeypadStatus(scene, pd.keypad, KeypadStatus::Denied);
        }
    }
    // ---- MASTER DOOR auto-close + re-lock: once the rider is > 6 m from the
    // unnumbered door it slides shut and re-arms — 4.5 is never propped open.
    // (7762 works from EITHER side of the slab, so closing behind a rider inside
    // the connector strands nobody: E + the code reopens it.) ----
    if (!doors) return;
    for (const PhantomDoor& pd : m_phantoms) {
        if (!pd.sublevelTell || pd.doorIndex >= doors->count()) continue;
        Door& d = doors->at(pd.doorIndex);
        // The pad tracks the master lock (GREEN while 7762 holds it open) unless
        // the 4545 flash sequencer currently owns the screen.
        const bool flashOwns = (m_flashIdx >= 0 &&
                                &m_phantoms[(size_t)m_flashIdx] == &pd);
        if (d.state == DoorState::Open) {
            if (!flashOwns) setKeypadStatus(scene, pd.keypad, KeypadStatus::Unlocked);
            const float dx = playerPos.x - pd.center.x, dy = playerPos.y - pd.center.y,
                        dz = playerPos.z - pd.center.z;
            if (dx * dx + dy * dy + dz * dz > 36.0f) {
                doors->toggle(d);                     // Open -> Closing
                x3::logInfo("[stairwell] master door auto-closing (rider clear)");
            }
        } else if (d.state == DoorState::Closed && !d.locked) {
            d.locked = true;                          // re-armed: code required again
            if (!flashOwns) setKeypadStatus(scene, pd.keypad, KeypadStatus::Locked);
        }
    }
}

bool FacilityStairwell::stageMasterOpen(Scene& scene, DoorSystem& doors) {
    for (const PhantomDoor& pd : m_phantoms) {
        if (!pd.sublevelTell || pd.doorIndex >= doors.count()) continue;
        doors.unlockAndOpen(doors.at(pd.doorIndex));
        setKeypadStatus(scene, pd.keypad, KeypadStatus::Unlocked);
        return true;
    }
    return false;
}

FacilityStairwell::CodeResponse FacilityStairwell::demoSubmit(bool tell, Scene& scene) {
    if (!m_built) return CodeResponse::NotHandled;
    for (int i = 0; i < (int)m_phantoms.size(); ++i) {
        if (m_phantoms[(size_t)i].sublevelTell != tell) continue;
        // Capture staging: hold the AMBER denial steady (no sequencer — a still
        // needs a stable frame, not a 0.7 s green beat that decays mid-settle).
        setKeypadStatus(scene, m_phantoms[(size_t)i].keypad, KeypadStatus::Denied);
        return tell ? CodeResponse::SublevelTell : CodeResponse::ServiceVoid;
    }
    return CodeResponse::NotHandled;
}

} // namespace x3::game
