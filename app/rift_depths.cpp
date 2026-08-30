// RIFT DEPTHS — the landing + the approach corridor into the Rift Hub.
// See rift_depths.h for the design + the seam law this file is written to.
#include "rift_depths.h"

#include "asset_root.h"
#include "mesh_prims.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace x3::game {

namespace {
constexpr float kWallT   = 0.30f;   // shell thickness (walls / ceilings / floors)
constexpr float kFloorT  = 0.40f;
constexpr float kCeilT   = 0.30f;

constexpr float kConcrete[3] = { 0.44f, 0.46f, 0.49f };   // dark wet concrete (hub palette)
constexpr float kDeckTint[3] = { 0.40f, 0.42f, 0.45f };
constexpr float kSteel[3]    = { 0.30f, 0.32f, 0.35f };
constexpr float kDark[3]     = { 0.13f, 0.14f, 0.16f };
constexpr float kStrip[3]    = { 0.72f, 0.82f, 0.95f };   // cool white-blue fixture
constexpr float kBlue[3]     = { 0.30f, 0.62f, 1.00f };   // the hub's spill

// Fixture rig (kept honest with rifthub.cpp's R9 re-tune: this is a DARK place; the
// fixtures read, the rig only keeps the corners out of the void).
constexpr float kStripEm    = 2.30f;
constexpr float kHallLightI = 3.60f;   // shot pass: a 33 m corridor on 2.6 read as BLACK
constexpr float kSpillI     = 6.00f;   // the blue tease — it CLIMBS toward the hub (see below)
} // namespace

void RiftDepths::box(Scene& scene, x3::rhi::IRenderDevice& device,
                     x3::phys::IPhysicsWorld& physics,
                     float x0, float x1, float y0, float y1, float z0, float z1,
                     const SurfaceSet* surf, const float tint[3], float uvScale,
                     bool collide, float emissive) {
    if (x1 <= x0 || y1 <= y0 || z1 <= z0) return;   // degenerate span: author nothing
    const float hx = (x1 - x0) * 0.5f, hy = (y1 - y0) * 0.5f, hz = (z1 - z0) * 0.5f;
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, cz = (z0 + z1) * 0.5f;
    x3::prims::PrimMesh b = x3::prims::makeBox(hx, hy, hz, cx, cy, cz, uvScale);
    Entity e;
    e.mesh = device.createMesh(b.verts.data(), (uint32_t)b.verts.size(),
                               b.index.data(), (uint32_t)b.index.size());
    m_meshes.push_back(e.mesh);
    if (surf && surf->ok) { e.tex = surf->albedo; e.normalTex = surf->normal; e.mrTex = surf->mr; }
    e.baseColor[0] = tint[0]; e.baseColor[1] = tint[1]; e.baseColor[2] = tint[2];
    e.baseColor[3] = 1.0f;
    e.emissive[0] = tint[0]; e.emissive[1] = tint[1]; e.emissive[2] = tint[2];
    e.emissive[3] = emissive;
    e.tag = (uint32_t)Tag::Static;
    m_ents.push_back(scene.add(e));
    if (collide)
        physics.addStaticMesh(b.cverts.data(), (uint32_t)(b.cverts.size() / 3),
                              b.cindex.data(), (uint32_t)b.cindex.size());
}

void RiftDepths::build(Scene& scene, x3::rhi::IRenderDevice& device,
                       x3::phys::IPhysicsWorld& physics, const Desc& desc) {
    if (m_built) return;
    m_desc = desc;

    m_surf.mount(assetRoot() + "/surface_library");
    const SurfaceSet& sFloor = m_surf.get(device, "sr_concrete_01");
    const SurfaceSet& sWall  = m_surf.get(device, "mw_concrete_panels_b");
    const SurfaceSet& sPlate = m_surf.get(device, "mw_metal_panels_a");
    const SurfaceSet& sGrate = m_surf.get(device, "mw_metal_grate");
    const SurfaceSet& sTrim  = m_surf.get(device, "mw_metal_trim_a");

    const float sx = desc.shaft.x, sz = desc.shaft.z, fy = desc.shaft.y;
    const float LH = desc.landingHalf;
    const float W  = desc.shaftWellHalf;
    const float hx = desc.hallHalfW;
    const float LHt = desc.landingH;
    const float HH  = desc.hallH;
    const float dx  = desc.hubDoor.x;         // corridor B's centreline (== the hub door X)
    const float dz  = desc.hubDoor.z;         // the hub's -Z wall inner face
    const float xA1 = sx - LH;                // corridor A's east end = the landing wall inner face
    const float xA0 = dx - hx;                // corridor A's west end  = the bend's west edge
    const float zB1 = dz - 0.30f;             // corridor B ends on the hub wall's OUTER face

    auto floorSlab = [&](float x0, float x1, float z0, float z1, const SurfaceSet* sf,
                         const float tint[3], float uv) {
        box(scene, device, physics, x0, x1, fy - kFloorT, fy, z0, z1, sf, tint, uv, true);
        m_floors.push_back(Slab{ x0, x1, z0, z1, fy - kFloorT, fy, true });
    };

    // ===================== THE LANDING (the cab well is OPEN) ==================
    // Deck: four slabs around the well, so the cab drops straight through on the
    // 1127 descent to Club 1127 and the player steps off onto solid deck at the
    // RIFT stop. The well gap (0.22 m each side of the cab) is far too narrow to
    // fall into.
    floorSlab(sx - LH, sx - W,  sz - LH, sz + LH, &sFloor, kDeckTint, 0.5f);
    floorSlab(sx + W,  sx + LH, sz - LH, sz + LH, &sFloor, kDeckTint, 0.5f);
    floorSlab(sx - W,  sx + W,  sz - LH, sz - W,  &sGrate, kSteel,    1.0f);
    floorSlab(sx - W,  sx + W,  sz + W,  sz + LH, &sGrate, kSteel,    1.0f);
    // Ceiling: the same four spans (the well stays open UP — the cab comes from
    // the facility above).
    box(scene, device, physics, sx - LH, sx - W, fy + LHt, fy + LHt + kCeilT, sz - LH, sz + LH,
        &sWall, kDark, 0.4f, true);
    box(scene, device, physics, sx + W, sx + LH, fy + LHt, fy + LHt + kCeilT, sz - LH, sz + LH,
        &sWall, kDark, 0.4f, true);
    box(scene, device, physics, sx - W, sx + W, fy + LHt, fy + LHt + kCeilT, sz - LH, sz - W,
        &sWall, kDark, 0.4f, true);
    box(scene, device, physics, sx - W, sx + W, fy + LHt, fy + LHt + kCeilT, sz + W, sz + LH,
        &sWall, kDark, 0.4f, true);
    // Walls. +/-X walls span the full Z (corners included); +/-Z walls butt between
    // them. The -X wall carries the corridor DOORWAY (two jambs + a lintel).
    box(scene, device, physics, sx + LH, sx + LH + kWallT, fy, fy + LHt,
        sz - LH - kWallT, sz + LH + kWallT, &sWall, kConcrete, 0.35f, true);
    {   // -X wall with the opening at z in [sz-hx, sz+hx], height HH
        const float wx0 = sx - LH - kWallT, wx1 = sx - LH;
        box(scene, device, physics, wx0, wx1, fy, fy + LHt, sz - LH - kWallT, sz - hx,
            &sWall, kConcrete, 0.35f, true);
        box(scene, device, physics, wx0, wx1, fy, fy + LHt, sz + hx, sz + LH + kWallT,
            &sWall, kConcrete, 0.35f, true);
        box(scene, device, physics, wx0, wx1, fy + HH, fy + LHt, sz - hx, sz + hx,
            &sWall, kConcrete, 0.35f, true);                     // lintel
        // Threshold floor UNDER the opening (the wall's own thickness) — without it
        // the walk out of the landing steps over a 0.30 m hole.
        floorSlab(wx0, wx1, sz - hx, sz + hx, &sFloor, kDeckTint, 0.5f);
        // Lit door frame: the way OUT is the only thing in this room that glows.
        box(scene, device, physics, wx0 - 0.02f, wx1 + 0.02f, fy + HH, fy + HH + 0.07f,
            sz - hx - 0.10f, sz + hx + 0.10f, &sTrim, kStrip, 0.5f, false, 1.25f);
    }
    box(scene, device, physics, sx - LH, sx + LH, fy, fy + LHt, sz + LH, sz + LH + kWallT,
        &sWall, kConcrete, 0.35f, true);
    box(scene, device, physics, sx - LH, sx + LH, fy, fy + LHt, sz - LH - kWallT, sz - LH,
        &sWall, kConcrete, 0.35f, true);
    // Well kerb: a low steel lip around the open cab well (you can see the shaft
    // yawning under the cab, and you cannot walk into it by accident).
    box(scene, device, physics, sx - W - 0.14f, sx + W + 0.14f, fy, fy + 0.16f,
        sz - W - 0.14f, sz - W, &sTrim, kSteel, 1.0f, true);
    box(scene, device, physics, sx - W - 0.14f, sx + W + 0.14f, fy, fy + 0.16f,
        sz + W, sz + W + 0.14f, &sTrim, kSteel, 1.0f, true);
    box(scene, device, physics, sx - W - 0.14f, sx - W, fy, fy + 0.16f,
        sz - W, sz + W, &sTrim, kSteel, 1.0f, true);
    box(scene, device, physics, sx + W, sx + W + 0.14f, fy, fy + 0.16f,
        sz - W, sz + W, &sTrim, kSteel, 1.0f, true);

    // ===================== THE APPROACH — leg A (west) =========================
    floorSlab(xA0, xA1, sz - hx, sz + hx, &sFloor, kConcrete, 0.5f);
    box(scene, device, physics, xA0, xA1, fy + HH, fy + HH + kCeilT, sz - hx, sz + hx,
        &sWall, kDark, 0.4f, true);
    // Side-room ceiling lights, DEFERRED: m_lights is rebuilt in the fixtures
    // section below (m_lights.clear()), so rooms queue theirs here and the
    // fixture section flushes them.
    std::vector<std::array<float, 3>> addLightDeferred;

    // ---- South wall + THE SIDE ROOMS (owner 2026-08-30) ----------------------
    // Two rooms off leg A's south wall — the OPS ANNEX (west, by the bend) and
    // the STORES BAY (east, by the landing) — each behind a keycard-locked
    // "slider" registry door with a CARD READER beside it. LAW 1: each room
    // connects through an OPENING cut in the SHARED south wall (jamb segments +
    // lintel), never by proximity. LAW 2: the rooms reuse that wall as their
    // north wall (one wall, one hole); their own walls butt on its south face.
    // LAW 3: room floors sit at fy exactly — zero step through the doorway
    // (threshold slabs span the wall thickness). No DoorSystem in Desc =
    // corridor only, byte-identical to the pre-room build.
    m_readers.clear();
    m_roomsBuilt = desc.doors != nullptr && (xA1 - xA0) > 16.0f;
    {
        const float wz0 = sz - hx - kWallT;   // wall south face (rooms' north face)
        const float wz1 = sz - hx;            // wall north face (corridor side)
        const float oHW = 0.80f;              // side-door opening half width
        const float oH  = 2.20f;              // opening height (standard door)
        const float aCx = xA0 + 5.0f;         // OPS ANNEX opening center
        const float bCx = xA1 - 5.0f;         // STORES BAY opening center
        if (!m_roomsBuilt) {
            box(scene, device, physics, xA0 - kWallT, xA1, fy, fy + HH + kCeilT,
                wz0, wz1, &sWall, kConcrete, 0.35f, true);
        } else {
            // Wall segments AROUND the two openings + a lintel OVER each.
            const float a0 = aCx - oHW, a1 = aCx + oHW;
            const float b0 = bCx - oHW, b1 = bCx + oHW;
            box(scene, device, physics, xA0 - kWallT, a0, fy, fy + HH + kCeilT,
                wz0, wz1, &sWall, kConcrete, 0.35f, true);
            box(scene, device, physics, a1, b0, fy, fy + HH + kCeilT,
                wz0, wz1, &sWall, kConcrete, 0.35f, true);
            box(scene, device, physics, b1, xA1, fy, fy + HH + kCeilT,
                wz0, wz1, &sWall, kConcrete, 0.35f, true);
            box(scene, device, physics, a0, a1, fy + oH, fy + HH + kCeilT,
                wz0, wz1, &sWall, kConcrete, 0.35f, true);         // annex lintel
            box(scene, device, physics, b0, b1, fy + oH, fy + HH + kCeilT,
                wz0, wz1, &sWall, kConcrete, 0.35f, true);         // stores lintel

            // One room = shell + threshold + trim + door + reader + dressing.
            const float RD = 4.6f;            // room depth (south of the wall)
            const float RHW = 2.6f;           // room half width
            auto sideRoom = [&](float cx, const char* what, bool annex) {
                const float rx0 = cx - RHW, rx1 = cx + RHW;
                const float rz0 = wz0 - RD;
                // Floor (interior) + threshold (the wall's own thickness).
                floorSlab(rx0, rx1, rz0, wz0, &sFloor, kDeckTint, 0.5f);
                floorSlab(cx - oHW, cx + oHW, wz0, wz1, &sFloor, kDeckTint, 0.5f);
                // Ceiling.
                box(scene, device, physics, rx0, rx1, fy + HH, fy + HH + kCeilT,
                    rz0, wz0, &sWall, kDark, 0.4f, true);
                // South wall spans the full width incl. corners; east/west butt
                // between it and the corridor wall (the corner law).
                box(scene, device, physics, rx0 - kWallT, rx1 + kWallT, fy, fy + HH + kCeilT,
                    rz0 - kWallT, rz0, &sWall, kConcrete, 0.35f, true);
                box(scene, device, physics, rx0 - kWallT, rx0, fy, fy + HH + kCeilT,
                    rz0, wz0, &sWall, kConcrete, 0.35f, true);
                box(scene, device, physics, rx1, rx1 + kWallT, fy, fy + HH + kCeilT,
                    rz0, wz0, &sWall, kConcrete, 0.35f, true);
                // Lit trim ring over the corridor-side opening (the landing's
                // "the way out glows" pattern, here saying "a room is HERE").
                box(scene, device, physics, cx - oHW - 0.10f, cx + oHW + 0.10f,
                    fy + oH, fy + oH + 0.07f, wz1 - 0.02f, wz1 + 0.02f,
                    &sTrim, kStrip, 0.5f, false, 1.10f);
                // The DOOR: keycard-locked slider seated ON the shared wall.
                x3::game::DoorSpec ds;
                ds.doorwayCenter = x3::phys::Vec3{ cx, fy, (wz0 + wz1) * 0.5f };
                ds.axis       = x3::game::DoorAxis::AlongX;   // wall runs along X
                ds.halfWidth  = oHW;
                ds.height     = oH;
                ds.locked     = true;
                ds.keycard    = x3::game::kKeycardSecurity;   // Security badge opens
                ds.withButton = false;
                ds.withFrame  = false;                        // slider carries its own
                ds.model      = "slider";
                const uint32_t di = x3::game::buildLevelDoor(
                    scene, *desc.doors, device, physics, ds);
                // The CARD READER, corridor side, right of the opening: a steel
                // wall unit + an LED strip whose entity we keep — syncReaders()
                // drives it red (locked) / green (unlocked) off the door's LIVE
                // state, so the light can never lie about the lock.
                box(scene, device, physics, cx + oHW + 0.10f, cx + oHW + 0.34f,
                    fy + 1.02f, fy + 1.38f, wz1, wz1 + 0.05f,
                    &sPlate, kSteel, 1.0f, true);
                const float kLedRed[3] = { 1.0f, 0.12f, 0.08f };
                box(scene, device, physics, cx + oHW + 0.13f, cx + oHW + 0.31f,
                    fy + 1.40f, fy + 1.46f, wz1, wz1 + 0.055f,
                    nullptr, kLedRed, 1.0f, false, 0.90f);
                CardReader rd;
                rd.doorIdx = di;
                rd.ledEnt  = m_ents.back();
                rd.pos = x3::phys::Vec3{ cx + oHW + 0.22f, fy + 1.24f, wz1 };
                m_readers.push_back(rd);
                // Room light + dressing (in the corridor's own box language).
                addLightDeferred.push_back(std::array<float, 3>{ cx, fy + HH - 0.35f, (rz0 + wz0) * 0.5f });
                if (annex) {
                    // OPS ANNEX: a work counter + two teal wall screens.
                    box(scene, device, physics, rx0 + 0.35f, rx1 - 0.35f, fy + 0.82f,
                        fy + 0.92f, rz0 + 0.35f, rz0 + 1.15f, &sPlate, kSteel, 1.0f, true);
                    const float kTeal[3] = { 0.10f, 0.55f, 0.60f };
                    box(scene, device, physics, rx0 + 0.6f, rx0 + 2.0f, fy + 1.35f,
                        fy + 2.15f, rz0, rz0 + 0.05f, nullptr, kTeal, 1.0f, false, 0.55f);
                    box(scene, device, physics, rx1 - 2.0f, rx1 - 0.6f, fy + 1.35f,
                        fy + 2.15f, rz0, rz0 + 0.05f, nullptr, kTeal, 1.0f, false, 0.55f);
                } else {
                    // STORES BAY: three crate stacks off the armory palette.
                    for (int c = 0; c < 3; ++c) {
                        const float ccx = rx0 + 0.9f + (float)c * 1.7f;
                        const float h = 0.7f + 0.35f * (float)((c * 7) % 3);
                        box(scene, device, physics, ccx - 0.55f, ccx + 0.55f, fy,
                            fy + h, rz0 + 0.5f, rz0 + 1.6f, &sPlate, kSteel, 0.8f, true);
                    }
                }
                x3::logInfo(std::string("[riftdepths] side room ") + what +
                            " built (door idx " + std::to_string(di) + ", card reader armed)");
            };
            sideRoom(aCx, "OPS ANNEX", true);
            sideRoom(bCx, "STORES BAY", false);
        }
    }
    box(scene, device, physics, dx + hx, xA1, fy, fy + HH + kCeilT,
        sz + hx, sz + hx + kWallT, &sWall, kConcrete, 0.35f, true);

    // ===================== THE APPROACH — leg B (the turn, north) ==============
    floorSlab(dx - hx, dx + hx, sz + hx, dz, &sFloor, kConcrete, 0.5f);
    box(scene, device, physics, dx - hx, dx + hx, fy + HH, fy + HH + kCeilT,
        sz + hx, zB1, &sWall, kDark, 0.4f, true);
    box(scene, device, physics, dx - hx - kWallT, dx - hx, fy, fy + HH + kCeilT,
        sz - hx - kWallT, zB1, &sWall, kConcrete, 0.35f, true);   // outer corner wall
    box(scene, device, physics, dx + hx, dx + hx + kWallT, fy, fy + HH + kCeilT,
        sz + hx, zB1, &sWall, kConcrete, 0.35f, true);

    // ---- Dressing: rib frames + a rusted pipe run down the corridor ceiling ----
    {
        const float legLen = xA1 - xA0;
        const int   ribs   = std::max(2, (int)(legLen / 4.0f));
        for (int r = 0; r <= ribs; ++r) {
            const float x = xA0 + legLen * ((float)r / (float)ribs);
            box(scene, device, physics, x - 0.10f, x + 0.10f, fy, fy + HH,
                sz - hx, sz - hx + 0.10f, &sPlate, kDark, 0.6f, false);
            box(scene, device, physics, x - 0.10f, x + 0.10f, fy, fy + HH,
                sz + hx - 0.10f, sz + hx, &sPlate, kDark, 0.6f, false);
        }
        const float legB = dz - (sz + hx);
        const int   ribsB = std::max(2, (int)(legB / 4.0f));
        for (int r = 0; r <= ribsB; ++r) {
            const float z = sz + hx + legB * ((float)r / (float)ribsB);
            box(scene, device, physics, dx - hx, dx - hx + 0.10f, fy, fy + HH,
                z - 0.10f, z + 0.10f, &sPlate, kDark, 0.6f, false);
            box(scene, device, physics, dx + hx - 0.10f, dx + hx, fy, fy + HH,
                z - 0.10f, z + 0.10f, &sPlate, kDark, 0.6f, false);
        }
        // Conduit run along the ceiling of leg A (it feeds the thing at the end).
        box(scene, device, physics, xA0, xA1, fy + HH - 0.28f, fy + HH - 0.16f,
            sz + hx - 0.45f, sz + hx - 0.33f, &sTrim, kSteel, 1.0f, false);
        box(scene, device, physics, dx - hx + 0.33f, dx - hx + 0.45f,
            fy + HH - 0.28f, fy + HH - 0.16f, sz + hx, dz, &sTrim, kSteel, 1.0f, false);
    }

    // ---- Fixtures + the rig ---------------------------------------------------
    m_lights.clear();
    auto addLight = [&](float x, float y, float z, const float col[3], float I, float range) {
        x3::rhi::PointLight L;
        L.pos[0] = x; L.pos[1] = y; L.pos[2] = z;
        L.range  = range;
        L.color[0] = col[0] * I; L.color[1] = col[1] * I; L.color[2] = col[2] * I;
        m_lights.push_back(L);
    };
    // Landing: two overheads (one of them is the one that dies — see tick()).
    box(scene, device, physics, sx - LH + 1.0f, sx - LH + 2.6f, fy + LHt - 0.12f, fy + LHt - 0.06f,
        sz - 0.18f, sz + 0.18f, nullptr, kStrip, 1.0f, false, kStripEm);
    box(scene, device, physics, sx + 1.2f, sx + 2.8f, fy + LHt - 0.12f, fy + LHt - 0.06f,
        sz - 0.18f, sz + 0.18f, nullptr, kStrip, 1.0f, false, kStripEm);
    addLight(sx - LH + 1.8f, fy + LHt - 0.4f, sz, kStrip, kHallLightI, 12.0f);
    addLight(sx + 2.0f,      fy + LHt - 0.4f, sz, kStrip, kHallLightI, 12.0f);
    // Corridor A: strip fixtures every ~7 m (the last one before the bend flickers).
    {
        const float legLen = xA1 - xA0;
        const int   n = std::max(2, (int)(legLen / 5.5f));
        for (int i = 0; i <= n; ++i) {
            const float x = xA0 + 1.5f + (legLen - 3.0f) * ((float)i / (float)n);
            box(scene, device, physics, x - 0.7f, x + 0.7f, fy + HH - 0.10f, fy + HH - 0.05f,
                sz - 0.16f, sz + 0.16f, nullptr, kStrip, 1.0f, false, kStripEm);
            addLight(x, fy + HH - 0.35f, sz, kStrip, kHallLightI, 12.0f);
        }
    }
    // Side rooms: one warm-white overhead each (queued during the room build).
    for (const auto& p : addLightDeferred)
        addLight(p[0], p[1], p[2], kStrip, kHallLightI * 0.85f, 9.0f);
    // Corridor B: the fixtures FAIL toward the hub — the last stretch is lit by the
    // rifts themselves. (Whoever maintained this hall stopped, the closer they got.)
    {
        const float legB = dz - (sz + hx);
        const int   n = std::max(2, (int)(legB / 6.0f));
        for (int i = 0; i < n; ++i) {
            const float t = (float)i / (float)n;
            const float z = sz + hx + 2.0f + (legB - 6.0f) * t;
            box(scene, device, physics, dx - 0.16f, dx + 0.16f, fy + HH - 0.10f, fy + HH - 0.05f,
                z - 0.7f, z + 0.7f, nullptr, kStrip, 1.0f, false, kStripEm);
            addLight(dx, fy + HH - 0.35f, z, kStrip, kHallLightI * (1.0f - 0.45f * t), 11.0f);
        }
    }
    // THE TEASE — and it is a GRADIENT, not a lamp. The blue CLIMBS the corridor: a
    // wash at the corner (so leg A's end wall is already the wrong colour before you
    // turn), then brighter with every step up leg B, then the doorway itself burning
    // blue. tick() breathes all of them with the hub's core hum, so it reads as a
    // room breathing at you from around the bend, not as a light fixture.
    {
        const float legB = dz - sz;
        const int   n = 5;
        for (int i = 0; i <= n; ++i) {
            const float t = (float)i / (float)n;            // 0 = the corner, 1 = the door
            // The corner light is pulled a little INTO leg A so it washes the outer
            // corner wall — the surface you are staring at from 30 m back down the
            // straight. That wash is the whole tease: the hall ends in the wrong colour.
            const float z = (i == 0) ? sz - 0.4f
                                     : sz + legB * t - (i == n ? 2.0f : 0.0f);
            const float I = kSpillI * (0.95f + 1.30f * t * t);
            addLight(dx, fy + 2.4f - 0.3f * t, z, kBlue, I, 16.0f + 9.0f * t);
        }
    }

    m_lightBase.clear();
    for (const auto& L : m_lights) {
        m_lightBase.push_back(L.color[0]);
        m_lightBase.push_back(L.color[1]);
        m_lightBase.push_back(L.color[2]);
    }

    // ---- Route + the zone AABB ------------------------------------------------
    m_landingSpawn = x3::phys::Vec3{ sx - W - 1.4f, fy + 0.05f, sz };
    m_route = {
        m_landingSpawn,
        x3::phys::Vec3{ sx - LH - kWallT * 0.5f, fy + 0.05f, sz },   // the corridor mouth
        x3::phys::Vec3{ (xA0 + xA1) * 0.5f,      fy + 0.05f, sz },   // mid-leg A
        x3::phys::Vec3{ dx,                      fy + 0.05f, sz },   // THE BEND (blue on the wall)
        x3::phys::Vec3{ dx, fy + 0.05f, (sz + dz) * 0.5f },          // mid-leg B
        x3::phys::Vec3{ dx, fy + 0.05f, dz - 0.4f },                 // the hub threshold
    };
    m_zoneMin = x3::phys::Vec3{ std::min(xA0, sx - LH) - 3.0f, fy - 3.0f,
                                std::min(sz - LH, sz - hx) - 3.0f };
    m_zoneMax = x3::phys::Vec3{ sx + LH + 3.0f, fy + LHt + 3.0f, dz + 1.0f };

    m_built = true;
    const std::string chk = selfCheck();
    // The spawn + annex-door coords are CAPTURE ANCHORS (paste into --shot-cam).
    x3::logInfo("[riftdepths] landing + approach built at Y=" + std::to_string(fy) +
                " spawn=(" + std::to_string(m_landingSpawn.x) + "," +
                std::to_string(m_landingSpawn.z) + ") annexDoor=(" +
                std::to_string(xA0 + 5.0f) + "," + std::to_string(sz - hx) + ")" +
                " — leg A " + std::to_string(xA1 - xA0) + " m west, leg B " +
                std::to_string(dz - sz) + " m to the hub door (" +
                std::to_string(m_ents.size()) + " entities); seams: " +
                (chk.empty() ? std::string("CLEAN") : ("VIOLATION: " + chk)));
    if (!chk.empty()) x3::logWarn("[riftdepths] SEAM VIOLATION: " + chk);
}

void RiftDepths::tick(float dt) {
    if (!m_built) return;
    m_t += dt;
    if (m_lights.size() * 3 != m_lightBase.size()) return;
    const size_t n = m_lights.size();
    // The two BLUE spills are the last two lights: they breathe with the hub's core
    // hum (same 0.9 Hz slow pulse the gate cores run on) — the tease.
    for (size_t i = 0; i < n; ++i) {
        float k = 1.0f;
        if (i + 6 >= n) {   // the six BLUE spills: the hub's core hum, crawling the hall
            k = 0.78f + 0.30f * (0.5f + 0.5f * std::sin(m_t * 5.65f + (float)i * 1.7f));
        } else if (i == 1 || (i + 7 == n)) {
            // One dying landing overhead + the last corridor fixture before the bend:
            // a mains stutter (a place the facility stopped maintaining).
            const float s = std::sin(m_t * 21.0f + (float)i) * std::sin(m_t * 3.3f);
            k = (s > 0.72f) ? 0.15f : 1.0f;
        }
        for (int c = 0; c < 3; ++c) m_lights[i].color[c] = m_lightBase[i * 3 + c] * k;
    }
}

std::string RiftDepths::selfCheck() const {
    if (!m_built) return "not built";
    const Desc& d = m_desc;
    // 1) The corridor must run WEST out of the landing and turn along +Z to the hub.
    if (!(d.hubDoor.x < d.shaft.x - d.landingHalf))
        return "hub door is not west of the landing (the approach would run backwards)";
    if (!(d.hubDoor.z > d.shaft.z + d.hallHalfW + 4.0f))
        return "hub door is not far enough along +Z for the turn";
    // 2) The corridor mouth must be at least as wide as the hub's opening, or the
    //    seam would leave a slot beside the door.
    if (d.hallHalfW + 1e-3f < d.doorHalfW) return "corridor is narrower than the hub door";
    if (d.hallH    + 1e-3f < d.doorH)      return "corridor is lower than the hub door";
    // 3) The rift level's floors must agree: the hub floor, the corridor floor and
    //    the landing deck are ONE plane (a step here would be a trip hazard the
    //    player cannot see in the dark).
    if (std::fabs(d.hubDoor.y - d.shaft.y) > 1e-3f) return "hub floor Y != landing floor Y";
    // 4) FLOOR CONTINUITY: every route waypoint must stand over an authored slab, and
    //    every step BETWEEN waypoints must too (sampled — a gap in the shell shows up
    //    as a sample with no floor under it).
    auto overFloor = [&](float x, float z) {
        for (const Slab& s : m_floors)
            if (x >= s.x0 - 1e-3f && x <= s.x1 + 1e-3f &&
                z >= s.z0 - 1e-3f && z <= s.z1 + 1e-3f) return true;
        return false;
    };
    for (size_t i = 0; i + 1 < m_route.size(); ++i) {
        const x3::phys::Vec3& a = m_route[i];
        const x3::phys::Vec3& b = m_route[i + 1];
        const float len = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z));
        const int steps = std::max(2, (int)(len / 0.5f));
        for (int s = 0; s <= steps; ++s) {
            const float t = (float)s / (float)steps;
            const float x = a.x + (b.x - a.x) * t;
            const float z = a.z + (b.z - a.z) * t;
            // The last waypoint sits ON the hub threshold: past the corridor's own
            // floor the HUB's slab takes over, so stop checking at the wall face.
            if (z > d.hubDoor.z - 1e-3f) continue;
            if (!overFloor(x, z))
                return "floor gap on the approach at (" + std::to_string(x) + ", " +
                       std::to_string(z) + ")";
        }
    }
    // 5) SIDE ROOMS (when built): each reader watches a real doorway — walk the
    //    line from the corridor centreline THROUGH the opening to the room's
    //    heart and demand authored floor under every step (LAW 3's zero-step
    //    doorway: threshold + interior slabs must be continuous at fy).
    if (m_roomsBuilt) {
        if (m_readers.size() != 2) return "side rooms built but reader count != 2";
        const float sz = m_desc.shaft.z;
        for (const CardReader& r : m_readers) {
            const float cx = r.pos.x - 0.80f - 0.22f;   // opening centre (reader is +x of it)
            const float zA = sz;                        // corridor centreline
            const float zB = sz - m_desc.hallHalfW - 0.30f - 2.3f;   // the room's heart
            for (int s = 0; s <= 24; ++s) {
                const float t = (float)s / 24.0f;
                const float z = zA + (zB - zA) * t;
                if (!overFloor(cx, z))
                    return "side-room floor gap through the doorway at x=" +
                           std::to_string(cx) + " z=" + std::to_string(z);
            }
        }
    }
    return "";
}

void RiftDepths::syncReaders(Scene& scene, const DoorSystem& doors) {
    // The LED is a STATE readout, not a decoration: red while its door is
    // locked, green from the frame the card clears it. Cheap (two entities),
    // called per-frame while the eye is in the rift zone.
    static const float kRed[3]   = { 1.0f, 0.12f, 0.08f };
    static const float kGreen[3] = { 0.15f, 1.0f, 0.25f };
    for (const CardReader& r : m_readers) {
        if (r.doorIdx >= doors.count() || r.ledEnt >= scene.size()) continue;
        const float* c = doors.at(r.doorIdx).isLocked() ? kRed : kGreen;
        Entity& e = scene.get(r.ledEnt);
        e.emissive[0] = c[0]; e.emissive[1] = c[1]; e.emissive[2] = c[2];
        e.baseColor[0] = c[0] * 0.05f; e.baseColor[1] = c[1] * 0.05f;
        e.baseColor[2] = c[2] * 0.05f;   // near-black body; the glow carries (band law)
    }
}

void RiftDepths::shutdown(x3::rhi::IRenderDevice& device) {
    for (auto m : m_meshes) if (m.valid()) device.destroyMesh(m);
    m_meshes.clear();
    m_surf.destroyAll(device);
    m_ents.clear();
    m_lights.clear();
    m_floors.clear();
    m_route.clear();
    m_built = false;
}

} // namespace x3::game
