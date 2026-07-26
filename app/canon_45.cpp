// LEVEL 4.5 — THE NEXUS CHAMBER build (see canon_45.h). Geometry rides the loader's
// exported brush path (canonAddBrush) so shell/stairs get scene + collision + room-
// tagged vis exactly like level geometry. All coordinates derive from the authored
// platform rooms — nothing here is hardcoded to magic world positions except the
// scaffold layout, which is authored relative to the platforms it serves.
#include "canon_45.h"

#include "stairwell.h"   // MasterAccess plan — the sanctioned 7762 service mouth

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace x3::game {

namespace {

// Doctrine stairs: riser 0.22 m (auto-step clears 0.4), tread 0.30 m, width 1.6 m.
constexpr float kRiser = 0.22f, kTread = 0.30f, kStairW = 1.6f;

struct BrushCtx {
    Scene* scene; x3::rhi::IRenderDevice* device; x3::phys::IPhysicsWorld* physics;
    x3::rhi::TextureHandle rockTex, steelTex;
    x3::rhi::TextureHandle rockNrm;          // normal map (invalid ok)
    uint32_t room = kNoRoom;                 // vis room id for everything we place
};

uint32_t brush(BrushCtx& c, float hx, float hy, float hz, float cx, float cy, float cz,
               x3::rhi::TextureHandle tex, const float col[4]) {
    uint32_t id = canonAddBrush(*c.scene, *c.device, *c.physics,
                                hx, hy, hz, cx, cy, cz, tex, col, c.room);
    return id;
}

// A straight stair flight from (sx,sz) at floor y0 running along `axis` (0=X, 1=Z)
// with direction `dir` (+1/-1), climbing `rise` meters. Returns the (x,z,y) of the
// TOP landing edge (where the next deck/flight continues).
void flight(BrushCtx& c, float sx, float sz, float y0, int axis, float dir,
            float rise, const float col[4], float* outX, float* outZ, float* outY) {
    const int n = std::max(1, (int)std::ceil(rise / kRiser));
    float x = sx, z = sz, y = y0;
    for (int i = 0; i < n; ++i) {
        y += kRiser;
        if (axis == 0) x += dir * kTread; else z += dir * kTread;
        // Each step: a full box from a bit below the tread top (chunky scaffold read).
        const float hy = 0.5f * std::min(0.9f, y - y0 + 0.3f);
        if (axis == 0)
            brush(c, kTread * 0.5f, hy, kStairW * 0.5f, x - dir * kTread * 0.5f, y - hy, z,
                  c.steelTex, col);
        else
            brush(c, kStairW * 0.5f, hy, kTread * 0.5f, x, y - hy, z - dir * kTread * 0.5f,
                  c.steelTex, col);
    }
    *outX = x; *outZ = z; *outY = y;
}

// A flat deck (landing / catwalk segment): full box, top at `yTop`.
void deck(BrushCtx& c, float x0, float x1, float z0, float z1, float yTop,
          const float col[4]) {
    brush(c, (x1 - x0) * 0.5f, 0.15f, (z1 - z0) * 0.5f,
          (x0 + x1) * 0.5f, yTop - 0.15f, (z0 + z1) * 0.5f, c.steelTex, col);
}

// A thin emissive vein strip lying on a platform edge / wall face. Decoration only:
// NO collision (a 5 cm lip that snags feet is worse than none).
void vein(BrushCtx& c, float hx, float hy, float hz, float cx, float cy, float cz,
          float r, float g, float b, float strength) {
    const float col[4] = { r * 0.25f, g * 0.25f, b * 0.25f, 1.0f };
    uint32_t id = canonAddBrush(*c.scene, *c.device, *c.physics, hx, hy, hz, cx, cy, cz,
                                x3::rhi::TextureHandle{}, col, c.room,
                                /*collide*/false, /*visible*/true);
    Entity& e = c.scene->get(id);
    e.emissive[0] = r; e.emissive[1] = g; e.emissive[2] = b; e.emissive[3] = strength;
}

} // namespace

namespace {
// Shared envelope derivation (build() + floorPlaneY() + the lint gate must agree).
// Returns false when the tower has no Nexus (no platforms / no access room).
bool nexusEnvelope(const CanonFloor& floor, uint32_t& accessOut,
                   std::vector<uint32_t>& platsOut,
                   float& x0, float& x1, float& z0, float& z1, float& top) {
    accessOut = kNoRoom;
    platsOut.clear();
    for (uint32_t i = 0; i < floor.rooms.size(); ++i) {
        if (floor.rooms[i].platform) platsOut.push_back(i);
        if (floor.rooms[i].name.find("Nexus Chamber Access") != std::string::npos)
            accessOut = i;
    }
    if (platsOut.empty() || accessOut == kNoRoom) return false;
    const CanonRoom& A = floor.rooms[accessOut];
    x0 = A.x0(); x1 = A.x1(); z0 = A.z0(); z1 = A.z1();
    top = A.y1();
    for (uint32_t p : platsOut) {
        const CanonRoom& r = floor.rooms[p];
        x0 = std::min(x0, r.x0()); x1 = std::max(x1, r.x1());
        z0 = std::min(z0, r.z0()); z1 = std::max(z1, r.z1());
        top = std::max(top, r.y1());
    }
    x0 -= 2.0f; x1 += 2.0f; z0 -= 2.0f; z1 += 2.0f;
    return true;
}
} // namespace

bool Canon45::envelope(const CanonFloor& floor, float out[6]) {
    uint32_t access; std::vector<uint32_t> plats;
    float x0, x1, z0, z1, top;
    if (!nexusEnvelope(floor, access, plats, x0, x1, z0, z1, top)) return false;
    out[0] = x0; out[1] = x1; out[2] = z0; out[3] = z1;
    out[4] = floorPlaneY(floor); out[5] = top + 5.0f;
    return true;
}

float Canon45::floorPlaneY(const CanonFloor& floor) {
    uint32_t access; std::vector<uint32_t> plats;
    float x0, x1, z0, z1, top;
    if (!nexusEnvelope(floor, access, plats, x0, x1, z0, z1, top)) return -1e9f;
    // The cavern floor plane sits just above the tallest NORMAL room roof inside the
    // envelope (the F4 boss arena pokes into the old cavern volume) — slab bottom
    // clears every roof, so the slab seals all vertical sightlines and clips nothing.
    float maxTop = floor.rooms[access].y1();
    for (const CanonRoom& r : floor.rooms) {
        if (r.platform) continue;
        if (r.y1() > 45.0f || r.y1() < 20.0f) continue;      // only the F4-story band
        if (r.x1() <= x0 || r.x0() >= x1 || r.z1() <= z0 || r.z0() >= z1) continue;
        maxTop = std::max(maxTop, r.y1());
    }
    return maxTop + 0.55f;    // 0.5 slab + 5 cm clearance under it
}

void Canon45::build(CanonFloor& floor, Scene& scene, x3::rhi::IRenderDevice& device,
                    x3::phys::IPhysicsWorld& physics, std::string_view riggedModelDir,
                    const std::string& surfaceLibRoot, std::vector<CanonLight>& canonLights) {
    // ---- Find the authored pieces. ----
    uint32_t access = kNoRoom;
    std::vector<uint32_t> plats;
    float ex0, ex1, ez0, ez1, etop;
    if (!nexusEnvelope(floor, access, plats, ex0, ex1, ez0, ez1, etop)) {
        x3::logInfo("[canon45] no Nexus platforms/access in this floor — skipped");
        return;
    }
    auto byName = [&](const char* s) -> const CanonRoom* {
        for (uint32_t p : plats)
            if (floor.rooms[p].name.find(s) != std::string::npos) return &floor.rooms[p];
        return nullptr;
    };
    (void)access;   // the Access room is SEALED (W5-1b) — geometry no longer touches it
    const CanonRoom* t1 = byName("Tier 1"); const CanonRoom* t2 = byName("Tier 2");
    const CanonRoom* t3 = byName("Tier 3"); const CanonRoom* t4 = byName("Tier 4");
    const CanonRoom* t5 = byName("Tier 5"); const CanonRoom* ep = byName("Entry Platform");
    if (!t1 || !t2 || !t3 || !t4 || !t5) {
        x3::logWarn("[canon45] tier set incomplete — cavern skipped");
        return;
    }

    // ---- Cavern envelope: platforms + access + margin (nexusEnvelope). ----
    const float x0 = ex0, x1 = ex1, z0 = ez0, z1 = ez1, top = etop;
    // W5-1b (fix/spire-hollow-core): the cavern floor is a FULL slab whose plane sits
    // just ABOVE the tallest F4 roof inside the envelope (floorPlaneY) — the old
    // annulus at the Access room's floor level left the F4 boss arena poking into the
    // cavern with an invisible (PVS-culled) roof: from the 4.5 catwalks the tower's
    // center read as a VOID dropping into a fog-washed pit. The slab is the hidden
    // level's REAL BOTTOM: it seals every sightline between 4.5 and the normal
    // floors, and a player who falls from a tier lands on rock with a way back (the
    // scaffold climb starts on this floor).
    const float fy = floorPlaneY(floor);           // top of the cavern floor slab
    const float cy = top + 5.0f;                   // rock ceiling: 5 m of dark above the apex
    m_x0 = x0; m_x1 = x1; m_y0 = fy; m_y1 = cy; m_z0 = z0; m_z1 = z1;
    m_whisperX = t1->cx; m_whisperY = t1->y1(); m_whisperZ = t1->cz;
    m_apexX = t5->cx; m_apexY = t5->y1(); m_apexZ = t5->cz;

    // ---- Materials. ----
    m_lib.mount(surfaceLibRoot);
    const SurfaceSet& rock  = m_lib.get(device, "sr_concrete_01");
    const SurfaceSet& steel = m_lib.get(device, "mw_metal_grate");
    // Vis tag: ALWAYS-DRAWN (kNoRoom). The cavern hangs outside the doorway graph,
    // so the portal flood can never resolve it — room-tagged shell/scaffold brushes
    // culled themselves whenever the eye stood anywhere else in 4.5 (the tiers'
    // mutual-void defect). Sealed rock; frustum + HZB own the draw cost.
    BrushCtx c{ &scene, &device, &physics,
                rock.ok ? rock.albedo : x3::rhi::TextureHandle{},
                steel.ok ? steel.albedo : x3::rhi::TextureHandle{},
                rock.ok ? rock.normal : x3::rhi::TextureHandle{},
                kNoRoom };
    const float rockCol[4]  = { 0.34f, 0.31f, 0.27f, 1.0f };   // black-brown organic base
    const float darkCol[4]  = { 0.22f, 0.21f, 0.19f, 1.0f };
    const float steelCol[4] = { 0.38f, 0.40f, 0.42f, 1.0f };   // scaffold

    // ---- SHELL: FULL rock floor slab (the hidden level's real bottom), walls,
    // ceiling. The slab is laid as a grid of ~6 m panels so the rock texture tiles at
    // a sane density instead of stretching one UV square over 30 m. ----
    {
        const int nx = std::max(1, (int)std::ceil((x1 - x0) / 6.0f));
        const int nz = std::max(1, (int)std::ceil((z1 - z0) / 6.0f));
        const float pw = (x1 - x0) / (float)nx, pd = (z1 - z0) / (float)nz;
        for (int ix = 0; ix < nx; ++ix)
            for (int iz = 0; iz < nz; ++iz)
                brush(c, pw * 0.5f, 0.25f, pd * 0.5f,
                      x0 + (ix + 0.5f) * pw, fy - 0.25f, z0 + (iz + 0.5f) * pd,
                      c.rockTex, rockCol);
    }
    // Walls (slab bottom -> cy) + ceiling. Dark rock — the void should swallow light.
    const float wb = fy - 0.5f;            // wall base = slab bottom (no gap under walls)
    const float wh = cy - wb;
    brush(c, 0.4f, wh * 0.5f, (z1 - z0) * 0.5f, x0 - 0.4f, wb + wh * 0.5f, (z0 + z1) * 0.5f, c.rockTex, darkCol);
    brush(c, 0.4f, wh * 0.5f, (z1 - z0) * 0.5f, x1 + 0.4f, wb + wh * 0.5f, (z0 + z1) * 0.5f, c.rockTex, darkCol);
    // -Z wall carries the ARRIVAL MOUTH: the elevator tunnel seals onto a doorway cut
    // (four pieces around the opening — LAW 1, an opening in a shared plane).
    {
        uint32_t lobby5 = kNoRoom;
        for (uint32_t i = 0; i < floor.rooms.size(); ++i)
            if (floor.rooms[i].type == "Elevator Lobby" &&
                i < floor.roomFloorNum.size() && floor.roomFloorNum[i] == 5) {
                lobby5 = i; break;
            }
        // Tunnel/mouth X center: the spine shaft's X (fall back to Tier 1's X so the
        // mouth still cuts sanely on a dataset with no F5 lobby).
        const float tx = (lobby5 != kNoRoom) ? floor.rooms[lobby5].cx : t1->cx;
        const float mx0 = tx - kMouthHalf, mx1 = tx + kMouthHalf;
        const float my0 = fy, my1 = fy + kMouthH;
        const float wallCx = (x0 + x1) * 0.5f, wallHx = (x1 - x0) * 0.5f + 0.8f;
        auto zPiece = [&](float px0, float px1, float py0, float py1) {
            if (px1 - px0 < 0.02f || py1 - py0 < 0.02f) return;
            brush(c, (px1 - px0) * 0.5f, (py1 - py0) * 0.5f, 0.4f,
                  (px0 + px1) * 0.5f, (py0 + py1) * 0.5f, z0 - 0.4f, c.rockTex, darkCol);
        };
        // ---- THE SANCTIONED SERVICE MOUTH (feat/secret-code-clues, owner order:
        // backup code 7762). The stairwell's master L-connector seals onto this
        // wall's outer face; its mouth is cut here from the SAME MasterAccess plan
        // (stairwellLayout) the builder and the lint gate read — one plan, one
        // opening. Absent (sx1 <= sx0) when the tower has no master access.
        float sx0 = 0.0f, sx1 = -1.0f, sy1 = my0;
        {
            const StairwellLayout sl = stairwellLayout(floor);
            if (sl.valid && sl.master.present) {
                sx0 = sl.master.mouthX0; sx1 = sl.master.mouthX1;
                sy1 = fy + StairwellLayout::kMasterH;
            }
        }
        zPiece(wallCx - wallHx, wallCx + wallHx, wb, my0);     // below the mouths (slab band)
        zPiece(wallCx - wallHx, wallCx + wallHx, my1, cy);     // above both mouths
        if (sx1 > sx0) {
            // Two openings in the band [my0, my1]: strips outside/between them +
            // a header over the (shorter) service mouth. LAW 1 — openings live in
            // a shared plane, cut as pieces, never overlapped.
            zPiece(wallCx - wallHx, sx0, my0, my1);            // west of the service mouth
            zPiece(sx1, mx0, my0, my1);                        // between the mouths
            zPiece(mx1, wallCx + wallHx, my0, my1);            // east of the arrival mouth
            zPiece(sx0, sx1, sy1, my1);                        // service-mouth header
            x3::logInfo("[canon45] service mouth cut at x[" + std::to_string(sx0) +
                        ".." + std::to_string(sx1) + "] (master access, code-locked)");
        } else {
            zPiece(wallCx - wallHx, mx0, my0, my1);            // -X side
            zPiece(mx1, wallCx + wallHx, my0, my1);            // +X side
        }

        // ---- THE ARRIVAL TUNNEL: elevator spine shaft -> cavern. Floor flush with
        // the cavern slab (and with the cab platform at the 4.5 stop), steel walls +
        // lid, sealing the spine tube's +Z mouth (cut by the loader, CanonBuildOpts::
        // spineMouth*) to the cavern's -Z mouth (cut above). ----
        const float tubeZ = (lobby5 != kNoRoom) ? floor.rooms[lobby5].cz + 1.5f : z0;
        const float tz0 = tubeZ, tz1 = z0;                     // tunnel span in Z
        if (lobby5 != kNoRoom && tz1 > tz0 + 1.0f) {
            const float ix0 = tx - kMouthHalf - 0.2f, ix1 = tx + kMouthHalf + 0.2f; // interior
            // Floor (top at fy) + lid (bottom at fy + kMouthH).
            brush(c, (ix1 - ix0) * 0.5f + 0.25f, 0.25f, (tz1 - tz0) * 0.5f,
                  tx, fy - 0.25f, (tz0 + tz1) * 0.5f, c.rockTex, rockCol);
            brush(c, (ix1 - ix0) * 0.5f + 0.25f, 0.25f, (tz1 - tz0) * 0.5f,
                  tx, fy + kMouthH + 0.25f, (tz0 + tz1) * 0.5f, c.rockTex, darkCol);
            // Side walls (full tunnel height).
            brush(c, 0.25f, kMouthH * 0.5f, (tz1 - tz0) * 0.5f,
                  ix0 - 0.25f, fy + kMouthH * 0.5f, (tz0 + tz1) * 0.5f, c.rockTex, darkCol);
            brush(c, 0.25f, kMouthH * 0.5f, (tz1 - tz0) * 0.5f,
                  ix1 + 0.25f, fy + kMouthH * 0.5f, (tz0 + tz1) * 0.5f, c.rockTex, darkCol);
            x3::logInfo("[canon45] arrival tunnel: shaft z=" + std::to_string(tubeZ) +
                        " -> cavern z=" + std::to_string(z0) + " at floor y=" +
                        std::to_string(fy));
        }
    }
    brush(c, (x1 - x0) * 0.5f + 0.8f, wh * 0.5f, 0.4f, (x0 + x1) * 0.5f, wb + wh * 0.5f, z1 + 0.4f, c.rockTex, darkCol);
    brush(c, (x1 - x0) * 0.5f + 0.8f, 0.4f, (z1 - z0) * 0.5f + 0.8f, (x0 + x1) * 0.5f, cy + 0.4f, (z0 + z1) * 0.5f, c.rockTex, darkCol);

    // ---- THE CLIMB. Scaffold rising from the cavern floor near the arrival mouth to
    // Tier 1, then L-stairs tier to tier, catwalk to the Entry Platform. (The old
    // climb rose out of the Access room's open ceiling — that room is SEALED now; the
    // hidden level starts at the elevator, owner canon 2026-07-25.) ----
    float px, pz, py;
    const float towerRise = (t1->y1() - fy);
    const float seg2 = towerRise * 0.5f;
    // Flight 1: from just inside the mouth, east along +X. Flight 2: north along +Z to
    // Tier 1's -Z edge; arrival catwalk bridges onto the platform (2 cm lip).
    flight(c, t1->cx + 0.6f, z0 + 1.6f, fy, 0, +1.0f, seg2, steelCol, &px, &pz, &py);
    deck(c, px, px + 1.7f, pz - 0.9f, pz + 0.9f, py, steelCol);
    flight(c, px + 0.85f, pz + 0.8f, py, 1, +1.0f, towerRise - seg2, steelCol, &px, &pz, &py);
    deck(c, px - 0.9f, px + 0.9f, pz - 0.9f, std::max(pz + 0.9f, t1->z0() + 1.0f),
         t1->y1() + 0.02f, steelCol);
    // Connector deck from the flight-2 arrival across to the platform edge.
    deck(c, std::min(px - 0.9f, t1->x1() - 1.8f), px + 0.9f,
         t1->z0() - 0.6f, t1->z0() + 1.0f, t1->y1() + 0.02f, steelCol);

    // Tier links: L-stairs (out along Z, then along X) + arrival deck. Helper.
    auto link = [&](const CanonRoom& lo, const CanonRoom& hi) {
        const float rise = hi.y1() - lo.y1();
        const float half = rise * 0.5f;
        const float zSide = (lo.cz <= hi.cz ? 1.0f : 1.0f);   // run out the +Z side (open cavern)
        float lx = (hi.cx > lo.cx) ? lo.x1() - 0.9f : lo.x0() + 0.9f;   // start near the facing edge
        float lz = lo.z1() - 0.2f;
        float ox, oz, oy;
        flight(c, lx, lz, lo.y1(), 1, +zSide, half, steelCol, &ox, &oz, &oy);       // out +Z, half rise
        deck(c, ox - 0.9f, ox + 0.9f, oz, oz + 1.7f, oy, steelCol);
        const float xDir = (hi.cx > lo.cx) ? +1.0f : -1.0f;
        flight(c, ox + xDir * 0.9f, oz + 0.85f, oy, 0, xDir, rise - half, steelCol, &ox, &oz, &oy);
        // Arrival catwalk over the high platform's +Z edge. Top rides 2 cm ABOVE the
        // platform top: the overlap region would otherwise be two coplanar top faces
        // (z-fight); a 2 cm lip is auto-stepped and reads as a bolted deck plate.
        deck(c, std::min(ox - 0.9f, hi.cx), std::max(ox + 0.9f, hi.cx),
             std::min(oz - 0.9f, hi.z1() - 0.6f), oz + 0.9f, hi.y1() + 0.02f, steelCol);
    };
    link(*t1, *t2);
    link(*t2, *t3);
    link(*t3, *t4);
    link(*t4, *t5);
    // Entry Platform catwalk from Tier 2 (near-flat: flat walk + a 5-step rise at the end).
    if (ep) {
        const float wy = t2->y1();
        deck(c, t2->cx + 1.0f, t2->cx + 3.0f, t2->z1() - 0.6f, ep->z0() + 1.0f, wy + 0.02f, steelCol);
        float ox, oz, oy;
        flight(c, t2->cx + 2.0f, ep->z0() + 1.0f, wy, 1, +1.0f, ep->y1() - wy, steelCol, &ox, &oz, &oy);
        deck(c, ox - 0.9f, ox + 0.9f, oz - 0.3f, oz + 1.2f, ep->y1() + 0.02f, steelCol);
    }

    // ---- DRESSING: biolume veins (green) on tier edges; blood-red only at the apex;
    // three abandoned work lights (warm practicals). Two accents, never elsewhere. ----
    for (const CanonRoom* t : { t1, t2, t3, t4 }) {
        vein(c, t->w * 0.42f, 0.05f, 0.09f, t->cx, t->y1() + 0.05f, t->z0() + 0.15f,
             0.18f, 1.05f, 0.40f, 1.35f);
        vein(c, 0.09f, 0.05f, t->d * 0.35f, t->x0() + 0.15f, t->y1() + 0.05f, t->cz,
             0.18f, 1.05f, 0.40f, 1.05f);
    }
    // Apex: red organic accents ringing the arena + a green counter-vein (the exception).
    vein(c, t5->w * 0.46f, 0.06f, 0.10f, t5->cx, t5->y1() + 0.06f, t5->z0() + 0.2f,
         1.05f, 0.10f, 0.07f, 1.6f);
    vein(c, t5->w * 0.46f, 0.06f, 0.10f, t5->cx, t5->y1() + 0.06f, t5->z1() - 0.2f,
         1.05f, 0.10f, 0.07f, 1.6f);
    vein(c, 0.10f, 0.06f, t5->d * 0.40f, t5->x0() + 0.2f, t5->y1() + 0.06f, t5->cz,
         0.18f, 1.05f, 0.40f, 1.2f);
    // Work lights: small warm practical boxes at access mouth + T3 + apex approach.
    auto workLight = [&](float wx, float wy, float wz) {
        const float col[4] = { 0.5f, 0.45f, 0.35f, 1.0f };
        uint32_t id = brush(c, 0.18f, 0.14f, 0.18f, wx, wy, wz, c.steelTex, col);
        Entity& e = scene.get(id);
        e.emissive[0] = 1.0f; e.emissive[1] = 0.82f; e.emissive[2] = 0.55f; e.emissive[3] = 1.4f;
    };
    workLight(t1->cx, fy + 1.1f, z0 + 1.0f);            // arrival-mouth work light
    workLight(t3->cx, t3->y1() + 0.6f, t3->cz);
    workLight(t5->x0() + 1.2f, t5->y1() + 0.6f, t5->z0() + 1.2f);

    // ---- LIGHTS (canonLights feed, room-gated like everything else). Sparse: the dark
    // is the point. One warm pool per work light, a dim green wash at the whisper tier,
    // a red-leaning dim at the apex. ----
    auto addL = [&](float lx, float ly, float lz, float range, float r, float g, float b) {
        CanonLight cl; cl.room = kNoRoom;   // un-roomed: range-gated by the light feed
        cl.light.pos[0] = lx; cl.light.pos[1] = ly; cl.light.pos[2] = lz;
        cl.light.range = range;
        cl.light.color[0] = r; cl.light.color[1] = g; cl.light.color[2] = b;
        canonLights.push_back(cl);
    };
    addL(t1->cx, fy + 1.8f, z0 + 1.4f, 6.0f, 1.5f, 1.2f, 0.8f);            // arrival-mouth pool
    addL(t1->cx, fy + kMouthH - 0.4f, z0 - 6.0f, 5.0f, 1.1f, 0.95f, 0.7f); // tunnel practical
    addL(t1->cx, t1->y1() + 1.2f, t1->cz, 6.0f, 0.22f, 0.75f, 0.35f);      // whisper green
    addL(t3->cx, t3->y1() + 1.3f, t3->cz, 5.0f, 1.2f, 1.0f, 0.7f);         // mid work light
    addL(t5->cx, t5->y1() + 1.6f, t5->cz, 7.0f, 1.1f, 0.22f, 0.16f);       // apex blood-red
    addL(t5->x0() + 1.2f, t5->y1() + 1.0f, t5->z0() + 1.2f, 4.0f, 1.2f, 1.0f, 0.7f);

    // ---- CREATURES: sparse. Two prowlers (T2 / T4), one dormant APEX STAND-IN.
    // TIM'S RULING (2026-07-08): the apex IS **THE CHORUS AMALGAM** — the source
    // of 4.5's whispers, a fused 70-80 ft mass of the facility's failed
    // experiments. The Whisper Gallery, the spare-a-voice mechanic, and this
    // creature are ONE story: every voice the player spares is a voice inside
    // it. This body is still a placeholder rig (scaled hard, boss-tuned, asleep
    // until approach); the true amalgam model is the future asset. ----
    {
        MonsterSystem::Tuning prowler = tuningFor(EnemyType::Verthani);
        prowler.patrolRadius = 2.5f;   // slow prowl on the platform
        m_creatures.spawn(scene, device, physics, riggedModelDir,
                          x3::phys::Vec3{ t2->cx, t2->y1() + 0.1f, t2->cz }, prowler);
        m_creatures.spawn(scene, device, physics, riggedModelDir,
                          x3::phys::Vec3{ t4->cx, t4->y1() + 0.1f, t4->cz }, prowler);
        MonsterSystem::Tuning apex = tuningFor(EnemyType::Verthani);
        apex.type = MonsterType::Boss;
        apex.hp = 900; apex.damage = 22; apex.chaseSpeed = 3.0f;
        apex.attackRange = 3.2f; apex.attackCooldown = 1.3f; apex.attackWindup = 0.5f;
        apex.modelScale = 3.0f;
        apex.tint[0] = 0.85f; apex.tint[1] = 0.75f; apex.tint[2] = 0.70f; apex.tint[3] = 1.0f;
        m_apexIdx = (int)m_creatures.spawn(scene, device, physics, riggedModelDir,
                        x3::phys::Vec3{ t5->cx, t5->y1() + 0.1f, t5->cz - 2.0f }, apex);
    }

    m_built = true;
    x3::logInfo("[canon45] NEXUS CHAMBER built: cavern " +
                std::to_string((int)(x1 - x0)) + "x" + std::to_string((int)(cy - fy)) +
                "x" + std::to_string((int)(z1 - z0)) + " m, " +
                std::to_string(plats.size()) + " tiers, scaffold climb, " +
                std::to_string(m_creatures.count()) + " creatures (apex dormant)");
}

void Canon45::update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                     const x3::phys::Vec3& playerPos, IDamageSink* playerSink,
                     const AttackFxFn& fx,
                     x3::audio::IAudioSystem* audio,
                     x3::audio::SoundHandle whisperQuiet, x3::audio::SoundHandle whisperCall) {
    if (!m_built) return;
    const bool inCavern = playerPos.x > m_x0 && playerPos.x < m_x1 &&
                          playerPos.y > m_y0 - 1.0f && playerPos.y < m_y1 &&
                          playerPos.z > m_z0 && playerPos.z < m_z1;

    // Creatures: prowlers always tick (cheap, room-gated draw); the APEX sleeps until
    // the player closes on the arena — then it wakes once, permanently.
    if (!m_apexAwake && m_apexIdx >= 0) {
        const float dx = playerPos.x - m_apexX, dy = playerPos.y - m_apexY,
                    dz = playerPos.z - m_apexZ;
        if (dx * dx + dy * dy + dz * dz < 14.0f * 14.0f) {
            m_apexAwake = true;
            x3::logInfo("[canon45] THE APEX WAKES");
        }
    }
    // While dormant, hold the apex out of the AI update by zeroing dt for it: the
    // manager has no per-monster gate, so tick everything only once awake; before
    // that, tick with a null target so nothing attacks (prowlers still roam).
    if (m_apexAwake)
        m_creatures.update(dt, scene, physics, playerPos, playerSink, fx);
    else
        m_creatures.update(dt, scene, physics, playerPos, nullptr, AttackFxFn{});

    // Whisper dread — only while the player is inside the cavern. Quiet murmurs on a
    // 9-16 s jitter anywhere below the player; the NAME-CALL (VIGIL's warning made
    // real) on a rare 40-75 s jitter, always from the dark ABOVE. Stand-in take: the
    // creature-bucket vocal at low gain/pitch (no VO exists in the packs — documented).
    if (!inCavern || !audio) return;
    auto lcg = [&]() { m_rng = m_rng * 1664525u + 1013904223u; return (m_rng >> 8) & 0xFFFF; };
    auto frand = [&](float lo, float hi) { return lo + (hi - lo) * (lcg() / 65535.0f); };
    m_whisperT -= dt; m_callT -= dt;
    if (m_whisperT <= 0.0f) {
        m_whisperT = frand(9.0f, 16.0f);
        audio->playSound3D(whisperQuiet,
                           frand(m_x0 + 2, m_x1 - 2), playerPos.y - frand(1, 4),
                           frand(m_z0 + 2, m_z1 - 2), 0.30f, 0.62f);
    }
    if (m_callT <= 0.0f) {
        m_callT = frand(40.0f, 75.0f);
        audio->playSound3D(whisperCall,
                           m_whisperX + frand(-3, 3), playerPos.y + frand(3, 7),
                           m_whisperZ + frand(-3, 3), 0.42f, 0.50f);
    }
}

} // namespace x3::game
