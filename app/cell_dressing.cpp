// EFLZ opening-space polish — set-dressing + motivated lighting for the canon cell.
// See app/cell_dressing.h. Clean-room: built from the IModelLoader / IAssetSource /
// IRenderDevice interfaces + the converted GLB catalog only (mirrors env_art.cpp).
#include "cell_dressing.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace x3::game {

namespace {

constexpr float kPi = 3.14159265358979f;

// ---- Converted-GLB relative paths (under the mounted converted_glb dir). ------------
const char* kRelConsole    = "ModularSciFi_Interior/SM_Console.glb";
const char* kRelPipes      = "ModularSciFi_Interior/SM_Pipes_A.glb";
const char* kRelLightFix    = "ModularSciFi_Interior/SM_Light_A.glb";
const char* kRelCrateShort = "SciFi_Warehouse_Kit/Crate Short.glb";
const char* kRelCrateLong  = "SciFi_Warehouse_Kit/Crate Long.glb";
const char* kRelBarrel     = "SciFi_Warehouse_Kit/Barrel.glb";
const char* kRelPallet     = "SciFi_Warehouse_Kit/Pallet.glb";
const char* kRelFusebox    = "SciFi_Warehouse_Kit/Fusebox 01.glb";

// ---- Probed world-space AABBs of each kit piece (meters), AFTER the GLB node TRS is
// applied (matches the M2 node-TRS loader; same numbers env_art.cpp uses). We store the
// anchor we map to a world target: (min-Y) seats a prop on the floor, center-XZ centers it.
struct Aabb { float minx, miny, minz, maxx, maxy, maxz; };
constexpr Aabb kConsAabb   { -0.47f,  0.00f, -0.31f,  0.31f, 1.55f, 0.29f };
constexpr Aabb kPipesAabb  { -0.123f,-0.011f,  0.00f,  0.539f,0.164f,3.000f };
constexpr Aabb kLightAabb  { -0.22f,  0.00f,  0.00f,  0.00f, 0.03f, 2.26f };
constexpr Aabb kCrateSAabb { -0.668f, 0.000f,-0.000f,  0.000f,0.600f,0.671f };
constexpr Aabb kCrateLAabb { -0.640f, 0.000f, 0.000f,  0.000f,0.600f,1.274f };
constexpr Aabb kBarrelAabb { -0.441f, 0.000f,-0.456f,  0.440f,1.225f,0.425f };
constexpr Aabb kPalletAabb { -1.559f, 0.000f,-0.005f,  0.003f,0.198f,1.519f };
constexpr Aabb kFuseAabb   { -0.329f,-0.936f,-0.245f, -0.144f,1.308f,0.396f };

inline float cx(const Aabb& a) { return (a.minx + a.maxx) * 0.5f; }
inline float cz(const Aabb& a) { return (a.minz + a.maxz) * 0.5f; }

} // namespace

uint32_t CellDressing::load(const std::string& relPath) {
    for (uint32_t i = 0; i < m_assetPaths.size(); ++i)
        if (m_assetPaths[i] == relPath) return i;
    Asset a;
    a.model = m_loader->load(relPath);
    if (a.model.ok) {
        a.drawables = x3::asset::makeDrawables(a.model);
        a.ok = !a.drawables.empty();
    }
    if (!a.ok)
        x3::logWarn("[cell-dress] FAILED to load " + relPath + " (prop skipped)");
    uint32_t idx = (uint32_t)m_assetTable.size();
    m_assetTable.push_back(std::move(a));
    m_assetPaths.push_back(relPath);
    return idx;
}

void CellDressing::place(uint32_t asset, float yaw, float s,
                         float ax, float ay, float az,
                         float wx, float wy, float wz,
                         const float emissive[4], const float tint[4]) {
    if (asset >= m_assetTable.size() || !m_assetTable[asset].ok) return;
    // Column-major TRS: world = T(w) * R_y(yaw) * S(s) * T(-anchor). (Same math as
    // env_art.cpp::placeYaw — local -Z faces world -Z at yaw 0.)
    const float c = std::cos(yaw), sn = std::sin(yaw);
    Instance e; e.asset = asset;
    e.transform[0]=c*s;  e.transform[1]=0;  e.transform[2]=-sn*s; e.transform[3]=0;
    e.transform[4]=0;    e.transform[5]=s;  e.transform[6]=0;     e.transform[7]=0;
    e.transform[8]=sn*s; e.transform[9]=0;  e.transform[10]=c*s;  e.transform[11]=0;
    const float rpx = (c*ax + sn*az) * s;
    const float rpy = (ay) * s;
    const float rpz = (-sn*ax + c*az) * s;
    e.transform[12]=wx - rpx; e.transform[13]=wy - rpy; e.transform[14]=wz - rpz; e.transform[15]=1.0f;
    if (emissive) for (int i = 0; i < 4; ++i) e.emissive[i] = emissive[i];
    if (tint)     for (int i = 0; i < 4; ++i) e.tint[i]     = tint[i];
    m_instances.push_back(e);
}

void CellDressing::addLight(uint32_t room, float x, float y, float z, float range,
                            float r, float g, float b) {
    DressLight dl; dl.room = room;
    dl.light.pos[0]=x; dl.light.pos[1]=y; dl.light.pos[2]=z;
    dl.light.range=range;
    dl.light.color[0]=r; dl.light.color[1]=g; dl.light.color[2]=b;
    m_lights.push_back(dl);
}

bool CellDressing::build(x3::rhi::IRenderDevice& device, std::string_view convertedGlbDir,
                         const CanonFloor& floor) {
    if (!floor.valid()) return false;
    m_assets.reset(x3::asset::createAssetSource());
    if (!m_assets->mountDir(convertedGlbDir, 0)) {
        x3::logWarn("[cell-dress] mountDir failed: " + std::string(convertedGlbDir));
        return false;
    }
    m_loader.reset(x3::asset::createModelLoader(&device, m_assets.get()));

    CanonBeats bt = canonBeats(floor);
    if (bt.jakeCell == kNoRoom) {
        x3::logWarn("[cell-dress] no Jake's Cell in this floor — dressing skipped");
        return false;
    }
    const CanonRoom& cell = floor.rooms[bt.jakeCell];
    const float fY = cell.y0();          // cell floor (≈ -2.0)
    const float ceilY = cell.y1();       // cell ceiling (≈ +2.0)
    const float x0 = cell.x0(), x1 = cell.x1();   // X span (≈ -1.5 .. 5.5)
    const float z0 = cell.z0(), z1 = cell.z1();   // Z span (≈ 37 .. 43)
    const float ccx = cell.cx, ccz = cell.cz;
    // The cell opens toward the Main Hall on its +X wall (cell (2,40) -> hall (22,44)),
    // so we keep the +X-center wall clear for the doorway and dress the -X / back walls.

    // ---- Load all kit pieces up front (cached). ----
    const uint32_t aConsole = load(kRelConsole);
    const uint32_t aPipes   = load(kRelPipes);
    const uint32_t aLight   = load(kRelLightFix);
    const uint32_t aCrateS  = load(kRelCrateShort);
    const uint32_t aCrateL  = load(kRelCrateLong);
    const uint32_t aBarrel  = load(kRelBarrel);
    const uint32_t aPallet  = load(kRelPallet);
    const uint32_t aFuse    = load(kRelFusebox);

    // Consistent industrial palette tints so the warehouse-kit props read as one cohesive
    // dressed space (the raw GLBs vary from near-white to grey, which looks scattered).
    const float tCrate[4]  = { 0.45f, 0.42f, 0.38f, 1.0f };  // weathered crate (warm grey)
    const float tBunk[4]   = { 0.30f, 0.32f, 0.40f, 1.0f };  // cot mattress (cool blue-grey)
    const float tBarrel[4] = { 0.40f, 0.30f, 0.22f, 1.0f };  // rusted drum (warm brown)
    const float tPallet[4] = { 0.38f, 0.30f, 0.20f, 1.0f };  // wood pallet

    // ================= JAKE'S CELL — the hero opening space =================
    // BUNK against the -X (back-left) wall: a pallet base + two long crates stacked as the
    // cot platform, with a short crate as a footlocker. This reads as a prison bunk.
    {
        const float bunkX = x0 + 1.05f;             // hugging the -X wall
        const float bunkZ = z0 + 1.7f;              // toward the -Z corner
        // Pallet base (flat on the floor), long axis along Z.
        place(aPallet, 0.0f, 1.0f, cx(kPalletAabb), kPalletAabb.miny, cz(kPalletAabb),
              bunkX, fY + 0.02f, bunkZ, nullptr, tPallet);
        // The cot: a long crate laid on the pallet (raised slightly), long axis along Z.
        place(aCrateL, 0.0f, 1.0f, cx(kCrateLAabb), kCrateLAabb.miny, cz(kCrateLAabb),
              bunkX, fY + 0.20f, bunkZ - 0.30f, nullptr, tBunk);
        place(aCrateL, 0.0f, 1.0f, cx(kCrateLAabb), kCrateLAabb.miny, cz(kCrateLAabb),
              bunkX, fY + 0.20f, bunkZ + 0.95f, nullptr, tBunk);
        // A short crate at the bunk foot = a footlocker.
        place(aCrateS, 0.4f, 1.0f, cx(kCrateSAabb), kCrateSAabb.miny, cz(kCrateSAabb),
              bunkX + 0.15f, fY + 0.02f, bunkZ + 2.1f, nullptr, tCrate);
        // Warm fill light OVER the bunk so the cot/footlocker read (they sit against the
        // -X wall, away from the center tube — without this they fall into shadow). Two
        // fills span the bunk length so neither end goes black from the door-facing angles.
        addLight(bt.jakeCell, bunkX + 0.5f, fY + 1.8f, bunkZ - 0.2f, 4.5f, 3.0f, 2.4f, 1.6f);
        addLight(bt.jakeCell, bunkX + 0.5f, fY + 1.8f, bunkZ + 1.5f, 4.0f, 2.6f, 2.1f, 1.4f);
    }

    // WALL TERMINAL (the cell's control panel) on the -Z wall, with a cyan glow + a cyan
    // accent light. Console faces +Z (into the room) -> yaw = +pi.
    {
        const float tx = ccx + 0.7f;
        const float tz = z0 + 0.18f;                // flush to the -Z wall
        // Modest screen-glow emissive (not a wash) so the panel reads as a lit terminal,
        // not a blown-out white slab. Console faces -Z by default; on the -Z wall we want
        // the SCREEN facing +Z (into the room) -> yaw = +pi, anchored at its +Z (back) face.
        const float emCyan[4]   = { 0.10f, 0.70f, 1.0f, 1.6f };
        const float darkMetal[4] = { 0.22f, 0.26f, 0.34f, 1.0f };  // painted blue-grey panel
        place(aConsole, kPi, 1.0f, cx(kConsAabb), kConsAabb.miny, kConsAabb.maxz,
              tx, fY + 0.0f, tz, emCyan, darkMetal);
        addLight(bt.jakeCell, tx, fY + 1.2f, tz + 0.6f, 3.6f, 0.16f, 0.9f, 1.2f); // cyan accent
    }

    // PIPES running along the ceiling (two parallel runs along Z, near the -X wall) — the
    // industrial overhead that breaks up the flat ceiling.
    {
        const float py = ceilY - 0.30f;
        place(aPipes, 0.0f, 1.0f, cx(kPipesAabb), kPipesAabb.maxy, cz(kPipesAabb),
              x0 + 0.45f, py, ccz);
        place(aPipes, 0.0f, 1.0f, cx(kPipesAabb), kPipesAabb.maxy, cz(kPipesAabb),
              x0 + 1.05f, py - 0.12f, ccz);
        // A cross pipe run along the back -Z wall near the ceiling (yaw +pi/2 -> runs in X).
        place(aPipes, kPi * 0.5f, 0.8f, cx(kPipesAabb), kPipesAabb.maxy, cz(kPipesAabb),
              ccx, py, z0 + 0.5f);
        // A vertical-ish wall cable drop on the +Z wall (rotated so the pipe run descends
        // the wall) — breaks the flat panel + reads as conduit feeding the cell.
        place(aPipes, kPi * 0.5f, 0.7f, cx(kPipesAabb), kPipesAabb.maxy, cz(kPipesAabb),
              ccx + 1.4f, py, z1 - 0.45f);
        place(aPipes, kPi * 0.5f, 0.7f, cx(kPipesAabb), kPipesAabb.maxy, cz(kPipesAabb),
              ccx - 1.6f, py - 0.10f, z1 - 0.45f);
    }

    // FUSEBOX / wall panels (security + monitoring look) mounted on the -Z and +Z walls.
    {
        // Fusebox sits low-ish on the -Z wall; its AABB min-Y is below 0, so anchor by maxy
        // to hang it. Faces +Z (yaw +pi).
        place(aFuse, kPi, 1.0f, cx(kFuseAabb), 0.0f, kFuseAabb.minz,
              x0 + 0.6f, fY + 1.1f, z0 + 0.12f);
        place(aFuse, 0.0f, 0.9f, cx(kFuseAabb), 0.0f, kFuseAabb.minz,
              x1 - 0.6f, fY + 1.1f, z1 - 0.12f);
    }

    // DEBRIS / CLUTTER cluster in the +X-near corner (lived-in, ransacked feel): a barrel,
    // a toppled short crate, and a stacked pair.
    {
        const float dx = x1 - 1.2f, dz = z1 - 1.4f;
        place(aBarrel, 0.0f, 1.0f, cx(kBarrelAabb), kBarrelAabb.miny, cz(kBarrelAabb),
              dx, fY + 0.02f, dz, nullptr, tBarrel);
        place(aCrateS, 0.9f, 1.0f, cx(kCrateSAabb), kCrateSAabb.miny, cz(kCrateSAabb),
              dx - 0.9f, fY + 0.02f, dz - 0.2f, nullptr, tCrate);
        place(aCrateS, 0.3f, 0.9f, cx(kCrateSAabb), kCrateSAabb.miny, cz(kCrateSAabb),
              dx - 0.9f, fY + 0.56f, dz - 0.2f, nullptr, tCrate);   // stacked on the one below
        place(aCrateL, 1.3f, 1.0f, cx(kCrateLAabb), kCrateLAabb.miny, cz(kCrateLAabb),
              dx - 0.4f, fY + 0.02f, dz - 1.3f, nullptr, tCrate);   // a third crate, angled
    }

    // (Door frame intentionally omitted: SM_DoorFrame_A is a wide showroom frame whose
    // 6.25 m X-extent slabs the whole cell; the cut doorway already gets an SM_Door_A
    // slab from canonDoors. Keeping the opening clean reads better than a giant frame.)

    // CEILING LIGHT FIXTURE (the physical tube) at the cell center, with the FLICKERING
    // overhead light driven by tick(). It also gets a self-emissive so the tube glows.
    {
        const float lx = ccx, lz = ccz;
        const float emWarm[4] = { 1.0f, 0.92f, 0.7f, 3.0f };
        place(aLight, 0.0f, 1.4f, cx(kLightAabb), kLightAabb.miny, cz(kLightAabb),
              lx, ceilY - 0.06f, lz, emWarm);
        // The motivated flickering tube (cool-white, stutters). Recorded as a flicker light.
        // Bright base + a SHALLOW flicker depth so the cell stays readable but the tube
        // visibly stutters (a failing fluorescent), not a strobe that blacks the room out.
        const uint32_t li = (uint32_t)m_lights.size();
        addLight(bt.jakeCell, lx, ceilY - 0.35f, lz, 8.0f, 3.4f, 3.5f, 3.7f);
        m_flickers.push_back({ li, 3.4f, 3.5f, 3.7f, 0.0f, 9.0f, 0.35f });
    }

    // RED ALARM WASH near the door (a low, saturated red accent so the exit reads as a
    // guarded threshold — moody contrast against the warm interior).
    addLight(bt.jakeCell, x1 - 0.6f, fY + 1.9f, ccz, 4.5f, 2.6f, 0.10f, 0.05f);

    // ================= MAIN HALL MOUTH — the first room past the cell =================
    // Dress the hall end nearest the cell so stepping out reads as a built corridor: a
    // line of pipes overhead, a console terminal, crates against the wall, and a red
    // running light. Kept light (the hall is large; the canon room light already lights it).
    if (bt.mainHall != kNoRoom) {
        const CanonRoom& H = floor.rooms[bt.mainHall];
        const float hfY = H.y0();
        const float hCeil = H.y1();
        const float hx0 = H.x0();
        const float hz = H.cz;
        // Pipes overhead near the hall's cell-facing (-X) end.
        place(aPipes, kPi * 0.5f, 1.0f, cx(kPipesAabb), kPipesAabb.maxy, cz(kPipesAabb),
              hx0 + 2.0f, hCeil - 0.35f, hz - 1.2f);
        place(aPipes, kPi * 0.5f, 1.0f, cx(kPipesAabb), kPipesAabb.maxy, cz(kPipesAabb),
              hx0 + 2.0f, hCeil - 0.50f, hz + 1.2f);
        // A wall terminal + cyan accent at the hall mouth.
        {
            const float tx = hx0 + 0.6f, tz = hz - 2.0f;
            const float emCyan[4] = { 0.10f, 0.70f, 1.0f, 1.6f };
            const float darkMetal[4] = { 0.22f, 0.26f, 0.34f, 1.0f };
            place(aConsole, -kPi * 0.5f, 1.0f, cx(kConsAabb), kConsAabb.miny, kConsAabb.minz,
                  tx, hfY, tz, emCyan, darkMetal);
            addLight(bt.mainHall, tx + 0.6f, hfY + 1.3f, tz, 4.0f, 0.18f, 1.0f, 1.3f);
        }
        // Crates + a barrel stacked against the hall's -X wall (cover / clutter).
        place(aCrateL, 0.0f, 1.0f, cx(kCrateLAabb), kCrateLAabb.miny, cz(kCrateLAabb),
              hx0 + 0.8f, hfY + 0.02f, hz + 2.2f, nullptr, tCrate);
        place(aCrateS, 0.6f, 1.0f, cx(kCrateSAabb), kCrateSAabb.miny, cz(kCrateSAabb),
              hx0 + 0.8f, hfY + 0.62f, hz + 2.2f, nullptr, tCrate);
        place(aBarrel, 0.0f, 1.0f, cx(kBarrelAabb), kBarrelAabb.miny, cz(kBarrelAabb),
              hx0 + 0.9f, hfY + 0.02f, hz + 3.2f, nullptr, tBarrel);
        // A red running light at the hall mouth (guard-corridor mood).
        addLight(bt.mainHall, hx0 + 1.2f, hCeil - 0.4f, hz, 5.0f, 2.4f, 0.12f, 0.06f);
    }

    m_built = propsLoaded() > 0;
    x3::logInfo("[cell-dress] opening-space dressed: " + std::to_string(propsLoaded()) +
                " kit pieces loaded, " + std::to_string(m_instances.size()) +
                " prop instances, " + std::to_string(m_lights.size()) + " motivated lights" +
                (m_built ? "" : " (NONE loaded — graybox fallback kept)"));
    return m_built;
}

void CellDressing::tick(float dt) {
    // Fluorescent stutter: each flicker light dips/blinks on a noisy sine so the cell tube
    // reads as failing. Keep it subtle (depth < 1) so the room never goes fully dark.
    for (Flicker& f : m_flickers) {
        if (f.idx >= m_lights.size()) continue;
        f.phase += dt * f.rate;
        // A sharp, occasional dip: mostly near 1.0, with brief drops.
        const float s = std::sin(f.phase) * 0.5f + 0.5f;          // 0..1
        const float dip = std::sin(f.phase * 2.37f) * 0.5f + 0.5f; // a second, faster term
        float k = 1.0f - f.depth * (1.0f - s * dip);              // 1 .. 1-depth
        k = std::clamp(k, 1.0f - f.depth, 1.0f);
        m_lights[f.idx].light.color[0] = f.baseR * k;
        m_lights[f.idx].light.color[1] = f.baseG * k;
        m_lights[f.idx].light.color[2] = f.baseB * k;
    }
}

void CellDressing::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const {
    for (const Instance& inst : m_instances) {
        const Asset& a = m_assetTable[inst.asset];
        for (const auto& d : a.drawables) {
            float fin[16];
            x3::asset::mulMat4(inst.transform, d.nodeTransform, fin);
            // Material emissive (from the GLB) wins; otherwise the per-instance glow.
            const bool matEmis = d.emissiveTexId != 0 ||
                d.emissiveFactor[0] > 0.001f || d.emissiveFactor[1] > 0.001f || d.emissiveFactor[2] > 0.001f;
            float emis[4];
            if (matEmis) { emis[0]=d.emissiveFactor[0]; emis[1]=d.emissiveFactor[1]; emis[2]=d.emissiveFactor[2]; emis[3]=1.0f; }
            else         { emis[0]=inst.emissive[0]; emis[1]=inst.emissive[1]; emis[2]=inst.emissive[2]; emis[3]=inst.emissive[3]; }
            // Per-instance baseColor tint (darken plain/white kit pieces to believable
            // dark metal/painted surfaces so they don't blow out under accent lights).
            const float bc[4] = { d.baseColorFactor[0]*inst.tint[0], d.baseColorFactor[1]*inst.tint[1],
                                  d.baseColorFactor[2]*inst.tint[2], d.baseColorFactor[3]*inst.tint[3] };
            device.drawMeshPBR(frame,
                               x3::rhi::MeshHandle{ d.meshId },
                               x3::rhi::TextureHandle{ d.baseColorTexId },
                               x3::rhi::TextureHandle{ d.normalTexId },
                               x3::rhi::TextureHandle{ d.mrTexId },
                               bc,
                               emis,
                               fin,
                               d.alphaMask,
                               d.alphaBlend,
                               x3::rhi::TextureHandle{ d.emissiveTexId },
                               x3::rhi::TextureHandle{ d.detailTexId },
                               d.detailUvScale,
                               d.clearcoat, d.clearcoatRough);
        }
    }
}

uint32_t CellDressing::propsLoaded() const {
    uint32_t n = 0; for (const auto& a : m_assetTable) if (a.ok) ++n; return n;
}

} // namespace x3::game
