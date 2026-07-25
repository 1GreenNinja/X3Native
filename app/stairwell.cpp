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
    return L;
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
        // PHANTOM landing: locked SM_Door_A slab in the opening + a keypad that
        // rejects you. Sealed backing box behind the wall plane — nothing exists
        // there (the 4.5-height door among these is the hidden floor's tell).
        if (doors) {
            DoorSpec spec;
            spec.doorwayCenter = x3::phys::Vec3{ doorPlaneX, nl.y, kDoorZ };
            spec.axis        = DoorAxis::AlongZ;            // slab thin in X
            spec.halfWidth   = kDoorHalfW;
            spec.height      = kDoorH - 0.2f;
            spec.withButton  = false;
            spec.locked      = true;
            spec.code        = 4545;   // UNASSIGNED placeholder — no in-world clue
                                       // exists; flag for the owner (zero-rework hook)
            spec.tint[0] = 0.35f; spec.tint[1] = 0.37f; spec.tint[2] = 0.40f;
            const uint32_t di = buildLevelDoor(scene, *doors, device, physics, spec);
            const uint32_t ent = doors->at(di).entity;
            if (ent != kNoLink && ent < scene.size())
                scene.get(ent).roomId = kNoRoom;            // always-visible
            ++lockedDoors;
        }
        // Sealed backing (dark) just outside the wall plane.
        sbrush(c, c.wall, wallTint, 0.125f, (kDoorH + 0.2f) * 0.5f, kDoorHalfW + 0.15f,
               kSX1 + 0.125f, nl.y + kDoorH * 0.5f, kDoorZ);
        buildKeypad(scene, device, kIX1, nl.y + 1.40f, kDoorZ - kDoorHalfW - 0.28f,
                    KeypadFacing::MinusX, KeypadStatus::Locked, kNoRoom);
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
    x3::logInfo("[stairwell] built: " + std::to_string(lay.floors.size()) +
                " floor landings, " + std::to_string(lay.north.size()) +
                " north landings (" + std::to_string(lockedDoors) +
                " locked keypad doors), " + std::to_string(nFlights) +
                " flights, base y=" + std::to_string(lay.baseY) +
                " top y=" + std::to_string(lay.topY));
    (void)floor;
}

} // namespace x3::game
