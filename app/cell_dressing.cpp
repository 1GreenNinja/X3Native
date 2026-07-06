// EFLZ opening-space polish — set-dressing + motivated lighting for the canon cell.
// See app/cell_dressing.h. Clean-room: built from the IModelLoader / IAssetSource /
// IRenderDevice interfaces + the converted GLB catalog only (mirrors env_art.cpp).
#include "cell_dressing.h"

#include "mesh_prims.h"          // x3::prims::makeBox / makeUVSphere (atmosphere geometry)
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace x3::game {

// A tiny procedural-geometry carrier (MeshVertex verts + indices) so addProcMesh can
// take any built shape. Declared here (matches the forward-decl in the header).
struct ProcGeo {
    std::vector<x3::rhi::MeshVertex> verts;
    std::vector<uint32_t>           idx;
};

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
// ROUND 2 wall + detail kit (real PBR wall panels + industrial wall detail). SM_Wall_A
// is a normal-mapped paneled metal wall section; the rest are the warehouse detail set.
// ROUND 3 — wall VARIETY: SM_Wall_A's base-color UVs map a near-FLAT plain-panel region,
// so a wall tiled only with A reads as a uniform light-grey slab in the hero corner.
// SM_Wall_B / SM_Wall_C share A's exact slab AABB (face at local X=0, 3 m wide along Z,
// 4.4 m tall) but carry richer paneled relief + greebles, so we mix them across faces so
// every wall has real detail instead of one flat tone. Drop-in (same kWallAabb).
const char* kRelWall       = "ModularSciFi_Interior/SM_Wall_A.glb";
const char* kRelWallB      = "ModularSciFi_Interior/SM_Wall_B.glb";   // heavier paneling/greebles
const char* kRelWallC      = "ModularSciFi_Interior/SM_Wall_C.glb";   // ribbed/detailed variant
// ROUND 4 — a real CEILING: R3 left the graybox ceiling exposed (unlit near-black kit ->
// the room read as OPEN VOID above the pipes). SM_Ceiling_A is the kit's 4x3 m horizontal
// ceiling panel (authored in-situ at Y 4.0..4.4, underside at local Y=4.0); we tile it
// under the graybox plane exactly like the wall panels so looking up reads as a built,
// paneled service ceiling instead of night sky.
const char* kRelCeiling    = "ModularSciFi_Interior/SM_Ceiling_A.glb";
// F3 — the REAL COT: the HorrorHospital bed (converted + ALB/NRM injected by
// tools/f3_inject_bed_tex.py; worn institutional frame + plastic-covered mattress).
// Replaces the crate-stack bunk that read as black-gold cargo, never a bed.
const char* kRelCot        = "Detention/SM_Hospital_Bed.glb";
const char* kRelDoor       = "ModularSciFi_Interior/SM_Door_A.glb";
const char* kRelDoorFrame  = "ModularSciFi_Interior/SM_DoorFrame_A.glb";
const char* kRelDuctVent   = "SciFi_Warehouse_Kit/Duct Vent.glb";
const char* kRelDuctRun    = "SciFi_Warehouse_Kit/Duct Straight.glb";
const char* kRelCam        = "SciFi_Warehouse_Kit/Security Cam.glb";
const char* kRelExtinguish = "SciFi_Warehouse_Kit/Fire Extinguisher.glb";
const char* kRelExitSign   = "SciFi_Warehouse_Kit/Exit Sign.glb";
const char* kRelWallLight  = "SciFi_Warehouse_Kit/Wall Light.glb";
const char* kRelHangLight  = "SciFi_Warehouse_Kit/Hanging Light.glb";
const char* kRelBin        = "SciFi_Warehouse_Kit/Garbage Bin.glb";

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
// ROUND 2: probed via tools/glb_node_bounds (M2 node-TRS convention).
constexpr Aabb kWallAabb   { -1.431f,-0.043f, -0.000f,  0.000f,4.403f,3.000f };   // panel: face at X=0, 3m wide(Z), 4.4 tall
constexpr Aabb kCeilAabb   { -5.100f, 4.000f,  0.000f, -1.100f,4.400f,3.000f };   // 4x3 m flat panel, underside at Y=4.0
constexpr Aabb kCotAabb    { -1.200f, 0.100f, -0.600f,  1.100f,1.200f,0.600f };   // hospital bed: 2.3 long(X); wheel-bottoms at Y=0.1
constexpr Aabb kDoorAabb   { -4.875f, 0.054f, -0.112f, -2.525f,3.554f,0.112f };   // 2.35 wide x 3.5 tall slab
constexpr Aabb kDoorFrAabb { -6.250f,-0.043f, -0.277f, -0.000f,4.403f,0.277f };   // wide frame (we scale down)
constexpr Aabb kVentAabb   { -0.327f,-0.327f,  0.000f,  0.327f,0.327f,0.999f };   // grate cube, depth in +Z
constexpr Aabb kDuctAabb   { -0.335f,-0.337f, -0.000f,  0.335f,1.200f,6.000f };   // long duct, runs in Z
constexpr Aabb kCamAabb    { -0.540f,-0.051f, -0.158f, -0.150f,0.209f,0.158f };
constexpr Aabb kExtAabb    { -0.394f, 0.000f, -0.184f, -0.151f,1.137f,0.184f };
constexpr Aabb kExitAabb   { -0.306f,-0.625f, -0.302f, -0.150f,-0.375f,0.302f };
constexpr Aabb kWLightAabb { -0.144f,-0.128f, -0.059f,  0.105f,0.129f,0.000f };   // wall-flush sconce
constexpr Aabb kHLightAabb { -0.273f,-0.952f, -0.299f,  0.273f,0.000f,0.299f };   // hangs DOWN from maxy=0
constexpr Aabb kBinAabb    { -0.277f, 0.001f, -0.277f,  0.277f,1.061f,0.277f };

inline float cx(const Aabb& a) { return (a.minx + a.maxx) * 0.5f; }
inline float cy(const Aabb& a) { return (a.miny + a.maxy) * 0.5f; }
inline float cz(const Aabb& a) { return (a.minz + a.maxz) * 0.5f; }

// A tiny dust-mote billboard-ish quad (a flat XY card, ~unit-sized; scaled tiny per
// instance). Drawn emissive so it reads as a lit speck regardless of light.
ProcGeo makeMoteQuad() {
    ProcGeo g;
    auto push=[&](float x,float y,float u,float v){ x3::rhi::MeshVertex mv{}; mv.pos[0]=x; mv.pos[1]=y; mv.pos[2]=0; mv.normal[2]=1; mv.uv[0]=u; mv.uv[1]=v; g.verts.push_back(mv); };
    push(-0.5f,-0.5f,0,0); push(0.5f,-0.5f,1,0); push(0.5f,0.5f,1,1); push(-0.5f,0.5f,0,1);
    g.idx = {0,1,2, 0,2,3, 0,2,1, 0,3,2};   // double-sided
    return g;
}

// ROUND 3 — a flat ground-plane DISC (in the XZ plane, +Y up) used as a soft contact /
// ambient-occlusion shadow blob UNDER props so they sit in the space instead of floating.
// A center vertex (dark, opaque-ish) fans out to a ring (fully transparent) so the disc
// fades to nothing at its edge — i.e. a radial soft shadow. Drawn through the glass pass
// with a near-black tint + low center opacity (vertex alpha rides the baseColor.a). Unit
// radius; scaled per blob. Normal points +Y so it lies on the floor.
ProcGeo makeShadowDisc(int seg = 24) {
    ProcGeo g;
    auto push=[&](float x,float z,float a){
        x3::rhi::MeshVertex mv{}; mv.pos[0]=x; mv.pos[1]=0; mv.pos[2]=z;
        mv.normal[1]=1.0f; mv.uv[0]=0.5f; mv.uv[1]=a;   // stash alpha hint in uv.y (unused by glass)
        g.verts.push_back(mv);
    };
    push(0.0f, 0.0f, 1.0f);                       // center (vertex 0)
    for (int i = 0; i <= seg; ++i) {
        float t = (float)i / (float)seg * 2.0f * kPi;
        push(std::cos(t), std::sin(t), 0.0f);     // ring (transparent edge)
    }
    for (int i = 1; i <= seg; ++i) {
        g.idx.push_back(0); g.idx.push_back(i); g.idx.push_back(i + 1);
        // double-sided so it reads from below too (harmless, cheap)
        g.idx.push_back(0); g.idx.push_back(i + 1); g.idx.push_back(i);
    }
    return g;
}

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

uint32_t CellDressing::addProcMesh(x3::rhi::IRenderDevice& device, const ProcGeo& g) {
    x3::rhi::MeshHandle h = device.createMesh(g.verts.data(), (uint32_t)g.verts.size(),
                                              g.idx.data(), (uint32_t)g.idx.size());
    m_procMeshes.push_back(h);
    return (uint32_t)m_procMeshes.size() - 1;
}

void CellDressing::addDustMotes(uint32_t moteMesh, int n, float x, float y, float z,
                                float rx, float rz, float riseY, float span,
                                float r, float g, float b, float glow) {
    for (int i = 0; i < n; ++i) {
        ProcDraw d; d.meshIdx = moteMesh; d.glass = false;
        d.color[0]=r; d.color[1]=g; d.color[2]=b; d.color[3]=1.0f;
        d.emissive[0]=r; d.emissive[1]=g; d.emissive[2]=b; d.emissive[3]=glow;
        uint32_t di = (uint32_t)m_proc.size();
        m_proc.push_back(d);
        Mote m;
        m.draw = di; m.ox = x; m.oy = y; m.oz = z; m.rx = rx; m.rz = rz;
        // Deterministic spread (no rng dependency): hash the index into a phase + radius.
        float h = (float)((i * 1327 + 71) % 997) / 997.0f;
        float h2 = (float)((i * 5333 + 17) % 991) / 991.0f;
        m.phase = h * 2.0f * kPi;
        m.rate = 0.20f + 0.30f * h2;
        m.riseY = riseY; m.span = span;
        // R4: 0.012+0.020h -> 0.005+0.008h — the big bright quads froze into white DASHES
        // in stills (Tim read them as floating tracer garbage). Real dust is sub-cm.
        m.size = 0.005f + 0.008f * h;
        m_motes.push_back(m);
    }
}

void CellDressing::addShadowBlob(uint32_t discMesh, float x, float y, float z,
                                 float radX, float radZ, float darkness) {
    ProcDraw d; d.meshIdx = discMesh; d.glass = true;
    // Near-black tint, center opacity = darkness (the disc fans to a transparent rim).
    d.color[0] = 0.02f; d.color[1] = 0.02f; d.color[2] = 0.03f; d.color[3] = darkness;
    d.emissive[0] = d.emissive[1] = d.emissive[2] = 0.0f; d.emissive[3] = 0.0f;
    // Lie the unit disc flat on the floor (its verts are already in XZ), scale to the blob
    // radii, lift a hair off the floor to avoid z-fighting with the floor plane.
    d.transform[0]=radX; d.transform[1]=0;    d.transform[2]=0;    d.transform[3]=0;
    d.transform[4]=0;    d.transform[5]=1.0f; d.transform[6]=0;    d.transform[7]=0;
    d.transform[8]=0;    d.transform[9]=0;    d.transform[10]=radZ;d.transform[11]=0;
    d.transform[12]=x;   d.transform[13]=y + 0.012f; d.transform[14]=z; d.transform[15]=1;
    m_proc.push_back(d);
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
    // ROUND 2 — real PBR wall panels + industrial wall detail + the cell door.
    const uint32_t aWall    = load(kRelWall);
    // ROUND 3 — richer paneled wall variants (drop-in, same slab AABB as SM_Wall_A).
    const uint32_t aWallB   = load(kRelWallB);
    const uint32_t aWallC   = load(kRelWallC);
    const uint32_t aCeil    = load(kRelCeiling);   // ROUND 4 — the real ceiling panels
    const uint32_t aCot     = load(kRelCot);       // F3 — the real hospital cot
    (void)aPallet;   // F3: pallet retired with the crate bunk (kept loaded for variants)
    const uint32_t aDoorFr  = load(kRelDoorFrame);
    (void)kRelDoor;   // SM_Door_A's sliding slab is owned by canonDoors; frame only here.
    const uint32_t aVent    = load(kRelDuctVent);
    const uint32_t aDuct    = load(kRelDuctRun);
    const uint32_t aCam     = load(kRelCam);
    const uint32_t aExt     = load(kRelExtinguish);
    const uint32_t aExit    = load(kRelExitSign);
    const uint32_t aWLight  = load(kRelWallLight);
    const uint32_t aHLight  = load(kRelHangLight);
    const uint32_t aBin     = load(kRelBin);

    // ROUND 3 — the soft contact-shadow disc, built once + reused for every grounding blob
    // (created up front so the prop blocks below can ground themselves as they're placed).
    m_shadowDisc = addProcMesh(device, makeShadowDisc());

    // Consistent industrial palette tints so the warehouse-kit props read as one cohesive
    // dressed space (the raw GLBs vary from near-white to grey, which looks scattered).
    const float tCrate[4]  = { 0.66f, 0.60f, 0.52f, 1.0f };  // weathered crate (warm grey, lifted R3)
    const float tBunk[4]   = { 0.92f, 0.95f, 1.08f, 1.0f };  // cot mattress (R5: near-full albedo —
                                                             // the crate tex is dark; 0.6x read as
                                                             // black-gold trash bags under the pools)
    const float tBarrel[4] = { 0.46f, 0.34f, 0.25f, 1.0f };  // rusted drum (warm brown)
    const float tPallet[4] = { 0.48f, 0.38f, 0.26f, 1.0f };  // wood pallet
    // Wall/detail palette: keep the PBR wall near its real albedo but desaturate +
    // dim slightly so the normal-mapped panels read as worn painted metal, not glossy
    // showroom plastic. Untextured detail pieces (cam/extinguisher) get dark tints so
    // they never blow out white under the accent lights.
    const float tWall[4]   = { 0.34f, 0.35f, 0.40f, 1.0f };  // worn painted metal (kept dark so
                                                             // the normal-map relief + tone read
                                                             // instead of blowing to flat white)
    const float tWallWarm[4]= { 0.36f, 0.35f, 0.34f, 1.0f }; // subtle warm grime (R3: less flat-tan)
    const float tDark[4]   = { 0.20f, 0.21f, 0.24f, 1.0f };  // dark gunmetal detail
    const float tRust[4]   = { 0.34f, 0.22f, 0.16f, 1.0f };  // rusted duct/grate
    const float tRed[4]    = { 0.55f, 0.07f, 0.06f, 1.0f };  // painted red (extinguisher)
    const float tSteel[4]  = { 0.42f, 0.45f, 0.50f, 1.0f };  // brushed steel fixture

    // ================= WALLS (offender #1) — real PBR panels over the cell graybox ===
    // The graybox walls are flat repetitive blue kit; SM_Wall_A is a normal-mapped
    // paneled metal section (3 m wide along Z, 4.4 m tall, face at local X=0). We tile
    // it across the -X / -Z / +Z walls (the +X wall is the doorway, kept clear). The
    // panels sit ~3 cm IN FROM the graybox plane so the real surface is what the player
    // reads; the graybox stays behind it as the collision truth.
    {
        const float wallH = ceilY - fY;                 // ~4 m
        const float wScaleY = wallH / kWallAabb.maxy;    // scale the 4.4 m panel to room height
        const float wScale  = std::max(wScaleY, 1.0f);
        const float panelW  = (kWallAabb.maxz - kWallAabb.minz) * wScale; // tiled span along the wall
        // The graybox walls are kWallT(0.2 m)-thick boxes CENTERED on the plane, so the
        // inner face sits ~0.10 m inside the room. Pull the PBR panel face IN PAST that
        // (~0.14 m) so it occludes the flat-blue graybox instead of z-fighting behind it.
        const float inset   = 0.14f;
        // ROUND 3: alternate the three paneled variants per segment so no wall reads as a
        // single uniform tone. picWall(i) returns B / C / A on a rotating index so every
        // run shows distinct relief; the hero -X wall LEADS with the richest B/C pieces.
        // B/C carry real relief; A's UVs map a flatter plain-panel region. Weight the
        // sequence toward B/C so no large visible wall lands a flat A slab; A appears once
        // per cycle only (a calmer pilaster between the detailed pieces).
        const uint32_t wallSeq[4] = { aWallB, aWallC, aWallB, aWallC };
        auto picWall = [&](int i) { return wallSeq[((i % 4) + 4) % 4]; };
        (void)aWall;   // A still loaded (the +X stub uses C); kept for variety if needed
        // Wall on the -X plane (x=x0): face points +X. SM_Wall face is the +X side of
        // its slab (slab spans X -1.43..0). At yaw 0 the slab's face plane is at local X=0
        // pointing +X; anchor that face (X=0) at x0+inset and seat the base at the floor.
        auto tileWallX = [&](float xPlane, float yaw, const float tint[4], int seed) {
            int i = seed;
            for (float z = z0; z < z1 - 0.05f; z += panelW) {
                place(picWall(i++), yaw, wScale, 0.0f, kWallAabb.miny, kWallAabb.minz,
                      xPlane, fY, z, nullptr, tint);
            }
        };
        // Wall on a Z plane (z=zPlane): rotate the panel 90° so its face points ±X->±Z.
        auto tileWallZ = [&](float zPlane, float yaw, float xStart, float xEnd,
                             const float tint[4], int seed) {
            int i = seed;
            for (float x = xStart; x < xEnd - 0.05f; x += panelW) {
                place(picWall(i++), yaw, wScale, 0.0f, kWallAabb.miny, kWallAabb.minz,
                      x, fY, zPlane, nullptr, tint);
            }
        };
        // -X HERO wall: panel face +X (yaw 0 keeps the slab's +X face toward the room).
        // seed=0 -> starts on SM_Wall_B (the heavy-panel piece) so the hero corner reads
        // as built relief, not a flat plain panel.
        tileWallX(x0 + inset, 0.0f, tWall, 0);
        // +Z wall (z=z1): face -Z (into room). Rotate yaw=+pi/2 so the slab face points -Z.
        tileWallZ(z1 - inset, kPi * 0.5f, x0, x1, tWallWarm, 1);
        // -Z wall (z=z0): face +Z (into room). yaw=-pi/2. Seed offset so the back wall does
        // not line up its seams/relief with the side walls.
        tileWallZ(z0 + inset, -kPi * 0.5f, x0, x1, tWall, 2);
        // +X wall: only dress the segments NOT covered by the doorway (door at z≈ccz).
        // Two short stubs flanking the ~2.4 m opening keep the door wall built without
        // slabbing the threshold.
        place(aWallC, kPi, wScale, 0.0f, kWallAabb.miny, kWallAabb.minz,
              x1 - inset, fY, z0, nullptr, tWall);

        // F3 — ARMORED GLASS GLAZING: the B/C wall panels carry authored cutouts (B = a
        // big rounded window, C = arch slits) that today are OPEN HOLES into the
        // neighbor rooms — a detention cell you could climb out of. One thin glass pane
        // per dressed wall, sitting in the 4 cm gap BETWEEN the graybox inner face
        // (plane+0.10) and the panel face (plane+0.14), spans the whole wall: the panel
        // occludes it everywhere except through a cutout, so every window/arch reads as
        // sealed armored glass regardless of the cutout's exact shape. Visual-only (the
        // graybox stays the collision truth, matching this module's contract).
        {
            const uint32_t paneMesh = addProcMesh(device, makeMoteQuad());
            const float gInset = 0.115f;                  // between graybox face and panel face
            const float gy0 = fY + 0.45f, gy1 = ceilY - 0.9f;   // cutouts live in this band
            auto addPane = [&](char axis, float plane, float a0, float a1) {
                ProcDraw d; d.meshIdx = paneMesh; d.glass = true;
                // Cool armored-glass tint, faint opacity, smooth + specular so the panes
                // catch the room lights as a sheen (NOT the matte shaft defaults).
                // R2: 0.16 opacity read as MILK (the pane mesh is double-sided, so the
                // blend applies twice) — the neighbor-room depth vanished behind frosted
                // teal. Near-clear + a restrained sheen keeps the see-through-but-sealed
                // armored read.
                // R3 (final): the glass pass pre-blends strongly (the reason the old
                // light-shaft cones were scrapped), so even small alphas read frosted.
                // 0.03 keeps a visible sheen while letting the most depth through the
                // pass allows; the residual frost reads as detention privacy glass.
                d.color[0] = 0.70f; d.color[1] = 0.80f; d.color[2] = 0.82f; d.color[3] = 0.03f;
                d.glassRough = 0.10f; d.glassSpec = 0.45f;
                const float w = a1 - a0, h = gy1 - gy0;
                const float cyw = (gy0 + gy1) * 0.5f, caw = (a0 + a1) * 0.5f;
                if (axis == 'x') {   // pane in the YZ plane at x=plane (quad local X -> world Z)
                    d.transform[0]=0;  d.transform[1]=0; d.transform[2]=w;  d.transform[3]=0;
                    d.transform[4]=0;  d.transform[5]=h; d.transform[6]=0;  d.transform[7]=0;
                    d.transform[8]=1;  d.transform[9]=0; d.transform[10]=0; d.transform[11]=0;
                    d.transform[12]=plane; d.transform[13]=cyw; d.transform[14]=caw; d.transform[15]=1;
                } else {             // pane in the XY plane at z=plane (quad local X -> world X)
                    d.transform[0]=w;  d.transform[1]=0; d.transform[2]=0;  d.transform[3]=0;
                    d.transform[4]=0;  d.transform[5]=h; d.transform[6]=0;  d.transform[7]=0;
                    d.transform[8]=0;  d.transform[9]=0; d.transform[10]=1; d.transform[11]=0;
                    d.transform[12]=caw; d.transform[13]=cyw; d.transform[14]=plane; d.transform[15]=1;
                }
                m_proc.push_back(d);
            };
            addPane('x', x0 + gInset, z0, z1);            // -X wall (big B window + C cutouts)
            addPane('z', z0 + gInset, x0, x1);            // -Z wall (arch slits into the south room)
            addPane('z', z1 - gInset, x0, x1);            // +Z wall (window w/ the neighbor trapdoor view)
            addPane('x', x1 - gInset, z0, z0 + 2.2f);     // +X door-wall stub — ends BEFORE the
                                                          // door jamb (~z0+2.3) so no glass sliver
                                                          // crosses the doorway
        }

        // ROUND 4 — CEILING: tile SM_Ceiling_A flat under the graybox ceiling plane so
        // looking up reads as a BUILT paneled service ceiling, not open void (the graybox
        // ceiling is unlit near-black kit). 2 panels along X (4 m each) x 2 along Z (3 m
        // each) covers the 7x6 cell; the 0.10 m inset keeps them clear of the graybox
        // plane (no coplanar z-fight) and the walls occlude the slight X overshoot.
        // Kept DARK (cool gunmetal) so the overhead stays moody — the pipes and fixtures
        // read against panel lines instead of a black hole.
        {
            const float tCeil[4] = { 0.26f, 0.27f, 0.31f, 1.0f };
            // 0.14 m in from the plane: the graybox slab is 0.2 m thick CENTERED on it, so
            // its inner face is at -0.10 — 0.14 keeps the panel underside past it (no
            // coplanar z-fight), same inset law the wall panels use.
            const float cInset = 0.14f;
            for (int ix = 0; ix < 2; ++ix)
                for (int iz = 0; iz < 2; ++iz)
                    place(aCeil, 0.0f, 1.0f, cx(kCeilAabb), kCeilAabb.miny, cz(kCeilAabb),
                          x0 + 2.0f + 4.0f * ix, ceilY - cInset, z0 + 1.5f + 3.0f * iz,
                          nullptr, tCeil);
        }
    }

    // ================= WALL DETAIL — vents / conduit / cam / signage / sconces =====
    // Break the panels with recessed/mounted industrial detail so the surface reads as
    // a built detention wall (not a tiled kit). Each is anchored flush to a wall plane.
    {
        // A recessed VENT GRATE high on the -Z wall (the grate depth runs +Z into the
        // wall; anchor its mouth at the wall plane, facing the room).
        place(aVent, 0.0f, 1.0f, cx(kVentAabb), cy(kVentAabb), kVentAabb.minz,
              ccx - 1.4f, fY + 2.7f, z0 + 0.16f, nullptr, tRust);
        place(aVent, 0.0f, 0.85f, cx(kVentAabb), cy(kVentAabb), kVentAabb.minz,
              ccx + 1.5f, fY + 2.9f, z0 + 0.16f, nullptr, tRust);
        // A horizontal CONDUIT/DUCT run hugging the +Z wall, raised FLUSH under the new
        // ceiling panels (R3 hung it 0.55 m down with no hangers -> a floating black box).
        // Runs in X -> yaw +pi/2.
        const float ductS = 0.42f;
        place(aDuct, kPi * 0.5f, ductS, cx(kDuctAabb), kDuctAabb.maxy, cz(kDuctAabb),
              x1 - 0.35f, ceilY - 0.16f, z1 - 0.30f, nullptr, tSteel);
        // R4: the R3 "vertical conduit DROP" was REMOVED — place() can only yaw (no
        // pitch/roll), so the duct stayed HORIZONTAL and floated mid-air off the -X wall
        // as a black plank (Tim's "black wires"). No legal vertical run in this kit.
        // SECURITY CAMERA in the +X/+Z upper corner watching the cell (it has no GLB
        // material -> dark-tinted so it reads as a black gunmetal housing, not white).
        place(aCam, kPi * 0.75f, 1.0f, cx(kCamAabb), cy(kCamAabb), cz(kCamAabb),
              x1 - 0.25f, ceilY - 0.35f, z1 - 0.25f, nullptr, tDark);
        addLight(bt.jakeCell, x1 - 0.4f, ceilY - 0.45f, z1 - 0.4f, 1.6f, 0.9f, 0.05f, 0.04f); // tiny red cam LED
        // FIRE EXTINGUISHER — R5 final: BY THE DOOR on the +X wall (where extinguishers
        // live — beside the exit). Every paneled wall is window-dominated (B/C panels
        // carry cutouts at each panel center) and the -X wall also holds the trapdoor
        // ladder; the +X door wall is the one guaranteed-solid surface. yaw 0 keeps the
        // local +X mount-back facing the +X wall; back plane on the graybox face.
        place(aExt, 0.0f, 1.0f, kExtAabb.maxx, kExtAabb.miny, cz(kExtAabb),
              x1 - 0.10f, fY + 0.55f, ccz - 1.35f, nullptr, tRed);
        // More -X back-wall detail so the HERO diagonal (which faces this corner) reads as
        // a built surface, not a bare panel: a wall-mounted fusebox/panel + a recessed vent.
        // R4: the fusebox is SLIMMED (0.55) and tinted mid-gunmetal — at tDark + 0.85 scale
        // it was a featureless 2.2 m BLACK PLANK sticking 0.5 m off the wall.
        const float tPanelBox[4] = { 0.30f, 0.32f, 0.36f, 1.0f };
        place(aFuse, kPi * 0.5f, 0.55f, cx(kFuseAabb), 0.0f, kFuseAabb.minz,
              x0 + 0.10f, fY + 1.45f, ccz + 0.6f, nullptr, tPanelBox);
        place(aVent, -kPi * 0.5f, 1.0f, cx(kVentAabb), cy(kVentAabb), kVentAabb.minz,
              x0 + 0.16f, fY + 2.6f, ccz - 0.6f, nullptr, tRust);
        // A grazing wall-wash on the -X wall so its normal-map relief CATCHES the light
        // (a low, side-skimming key — pools-and-shadow contrast, not flat front fill).
        addLight(bt.jakeCell, x0 + 0.9f, fY + 1.6f, z1 - 1.0f, 3.0f, 1.1f, 1.0f, 0.8f);
        // WALL SCONCES (the warehouse Wall Light) flanking the door + over the bunk so the
        // walls themselves emit pools of light (motivated). They self-glow + drive a fill.
        // R4: the two YAWS WERE SWAPPED — the sconce's screen is its local -Z face, and
        // local -Z maps to world (sin yaw, 0, -cos yaw). On the +X wall the screen must
        // face -X -> yaw = -pi/2; on the -Z wall it must face +Z -> yaw = pi. R3 had them
        // crossed, so each sconce pointed ALONG its wall and read as a floating white box.
        const float emSconce[4] = { 1.0f, 0.86f, 0.62f, 1.3f };
        place(aWLight, -kPi * 0.5f, 1.0f, cx(kWLightAabb), cy(kWLightAabb), kWLightAabb.maxz,
              x1 - 0.10f, fY + 2.2f, ccz + 1.4f, emSconce, tSteel);   // back ON the graybox face
        place(aWLight, kPi, 1.0f, cx(kWLightAabb), cy(kWLightAabb), kWLightAabb.maxz,
              x0 + 1.6f, fY + 2.2f, z0 + 0.145f, emSconce, tSteel);   // back ON the panel face
        // Sconce washes kept MODEST (they sit high; too bright flattens the wall to white).
        addLight(bt.jakeCell, x1 - 0.4f, fY + 2.2f, ccz + 1.4f, 2.4f, 0.9f, 0.72f, 0.48f);
        addLight(bt.jakeCell, x0 + 1.6f, fY + 2.2f, z0 + 0.4f, 2.4f, 0.85f, 0.70f, 0.46f);
    }

    // ================= CELL DOOR — a real reinforced slab in the +X opening =========
    // The doorway is on the +X wall at z≈ccz (toward the Main Hall). canonDoors already
    // places a sliding SM_Door_A there; we add a DOOR FRAME so the threshold reads as a
    // built, reinforced cell door (recessed jamb + header), plus a red threshold wash.
    {
        // SM_DoorFrame_A is a wide showroom frame (6.25 m). Scale it down hard to a
        // single-door jamb (~1.4 m opening) and seat it at the +X wall, facing the room.
        const float frScale = 0.42f;
        place(aDoorFr, -kPi * 0.5f, frScale, cx(kDoorFrAabb), kDoorFrAabb.miny, cz(kDoorFrAabb),
              x1 - 0.06f, fY, ccz, nullptr, tSteel);
        // A static reinforced slab just inside the jamb (the cell door, slightly ajar look
        // is handled by canonDoors' animated slab; this is the heavy frame around it).
        addLight(bt.jakeCell, x1 - 0.5f, fY + 1.0f, ccz, 3.0f, 1.5f, 0.08f, 0.04f); // red threshold wash
    }

    // ================= JAKE'S CELL — the hero opening space =================
    // F3 — A REAL COT: the textured hospital bed against the -X wall replaces the
    // pallet+crates stack (which read as black-gold cargo, never a bed, through five
    // polish rounds). Long axis along Z (yaw pi/2 maps the bed's 2.3 m local X to Z);
    // wheel-bottoms live at local Y=0.1, so anchoring that plane at fY seats the
    // wheels ON the floor. One short crate stays as a footlocker at the bed's foot.
    {
        const float bedX = x0 + 0.76f;              // half-width 0.55 + 6 cm off the panel face
        const float bedZ = z0 + 1.95f;              // spans z ~37.8..40.1, clear of the trapdoor ladder
        place(aCot, kPi * 0.5f, 1.0f, cx(kCotAabb), kCotAabb.miny, cz(kCotAabb),
              bedX, fY, bedZ, nullptr, nullptr);    // authored albedo — no tint needed
        // Footlocker hugging the bed FOOT (bed half-length 1.15 + crate half 0.33): F3's
        // +2.30 left it stranded mid-floor a metre past the frame.
        place(aCrateS, 0.0f, 1.0f, cx(kCrateSAabb), kCrateSAabb.miny, cz(kCrateSAabb),
              bedX + 0.05f, fY + 0.02f, bedZ + 1.55f, nullptr, tCrate);   // footlocker
        // Lighting kept to the R5 trio (pool + corner key + dim floor fill), pool
        // recentred low over the mattress so the fabric + plastic cover catch a warm
        // grazing key and the frame rails rim-light.
        addLight(bt.jakeCell, bedX + 0.75f, fY + 1.15f, bedZ + 0.1f, 3.0f, 2.6f, 2.05f, 1.3f);  // warm cot pool
        addLight(bt.jakeCell, bedX + 1.6f, fY + 1.3f, bedZ + 1.6f, 4.5f, 1.7f, 1.45f, 1.15f);   // corner key (tamed)
        addLight(bt.jakeCell, ccx + 0.5f, fY + 0.5f, ccz + 0.5f, 4.5f, 0.5f, 0.48f, 0.44f);     // dim floor fill
        // R7 — HATCH SPOT: a cool security downlight over the floor hatch (the same
        // spot app_run picks: cell center +1.4x / -1.1z) so the code-locked trapdoor +
        // its amber rim read evenly instead of sitting in the pooled shadow between
        // the bunk and door lights. Cool white = "security fixture" (distinct from the
        // warm bunk pools) and it quietly draws the eye to the way out.
        addLight(bt.jakeCell, ccx + 1.4f, fY + 1.9f, ccz - 1.1f, 2.8f, 1.15f, 1.2f, 1.35f);
        // Ground the bed + footlocker (contact-shadow blobs; m_shadowDisc built above).
        addShadowBlob(m_shadowDisc, bedX, fY, bedZ, 0.80f, 1.40f, 0.55f);               // bed
        addShadowBlob(m_shadowDisc, bedX + 0.05f, fY, bedZ + 1.55f, 0.55f, 0.55f, 0.5f); // footlocker
    }

    // WALL TERMINAL (the cell's control panel) on the -Z wall, with a cyan glow + a cyan
    // accent light. Console faces +Z (into the room) -> yaw = +pi.
    {
        const float tx = ccx + 0.7f;
        const float tz = z0 + 0.18f;                // flush to the -Z wall
        // Modest screen-glow emissive (not a wash) so the panel reads as a lit terminal,
        // not a blown-out white slab. Console faces -Z by default; on the -Z wall we want
        // the SCREEN facing +Z (into the room) -> yaw = +pi, anchored at its +Z (back) face.
        // Brighter screen content so the terminal reads as an ACTIVE panel with display
        // glow (HDR -> bloom), not a dim slab. The console body stays dark gunmetal.
        // R5: NO instance emissive — the per-instance glow is the fallback for EVERY
        // drawable, so emCyan lit the console's whole BODY into a solid cyan toy (R4
        // doorwall shot). The console reads as dark powered-down gunmetal; the cyan
        // accent light alone sells the screen spill.
        const float darkMetal[4] = { 0.18f, 0.21f, 0.28f, 1.0f };  // painted gunmetal panel
        place(aConsole, kPi, 1.0f, cx(kConsAabb), kConsAabb.miny, kConsAabb.maxz,
              tx, fY + 0.0f, tz, nullptr, darkMetal);
        addLight(bt.jakeCell, tx, fY + 1.2f, tz + 0.6f, 3.2f, 0.16f, 0.75f, 1.0f); // cyan accent
    }

    // SINK / TOILET FIXTURE — a stainless basin in the -X/+Z corner (the warehouse bin
    // reads as a cell's combined steel toilet/sink unit). Lit by its own small fill so it
    // never goes black. A detention staple that grounds the space as a lived-in cell.
    {
        const float fx = x0 + 0.55f, fz = z1 - 0.7f;
        place(aBin, 0.0f, 0.95f, cx(kBinAabb), kBinAabb.miny, cz(kBinAabb),
              fx, fY + 0.02f, fz, nullptr, tSteel);
        addLight(bt.jakeCell, fx + 0.3f, fY + 1.4f, fz - 0.2f, 2.4f, 0.9f, 0.85f, 0.75f);
        addShadowBlob(m_shadowDisc, fx, fY, fz, 0.5f, 0.5f, 0.5f);   // ground the basin
    }

    // PIPES running along the ceiling (two parallel runs along Z, near the -X wall) — the
    // industrial overhead that breaks up the flat ceiling.
    // R4: (1) TINTED — raw SM_Pipes_A is glossy WHITE with bright RED accents; untinted
    // under 17 lights it read as cartoon spaghetti. Dark steel/rust multiplies the texture
    // down to worn service pipes. (2) FLUSH — R3 hung them 0.30 m below the ceiling with
    // no hangers (floating); now they hug the new ceiling panels. (3) the two "vertical
    // wall drop" placements were CUT — yaw-only place() kept them horizontal, so they
    // floated mid-air off the +Z wall as black planks.
    {
        const float py = ceilY - 0.18f;   // just under the R4 ceiling panels
        place(aPipes, 0.0f, 1.0f, cx(kPipesAabb), kPipesAabb.maxy, cz(kPipesAabb),
              x0 + 0.45f, py, ccz, nullptr, tSteel);
        place(aPipes, 0.0f, 1.0f, cx(kPipesAabb), kPipesAabb.maxy, cz(kPipesAabb),
              x0 + 1.05f, py, ccz, nullptr, tRust);
        // A cross pipe run along the back -Z wall near the ceiling (yaw +pi/2 -> runs in X).
        place(aPipes, kPi * 0.5f, 0.8f, cx(kPipesAabb), kPipesAabb.maxy, cz(kPipesAabb),
              ccx, py, z0 + 0.5f, nullptr, tDark);
    }

    // FUSEBOX / wall panels (security + monitoring look) mounted on the -Z and +Z walls.
    // R4: tinted mid-gunmetal — untinted they rendered raw WHITE plastic.
    {
        const float tPanelBox2[4] = { 0.30f, 0.32f, 0.36f, 1.0f };
        place(aFuse, kPi, 0.8f, cx(kFuseAabb), 0.0f, kFuseAabb.minz,
              x0 + 0.6f, fY + 1.1f, z0 + 0.18f, nullptr, tPanelBox2);
        place(aFuse, 0.0f, 0.7f, cx(kFuseAabb), 0.0f, kFuseAabb.minz,
              x1 - 0.6f, fY + 1.1f, z1 - 0.18f, nullptr, tPanelBox2);
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
        // R7: the angled crate moved OFF the trapdoor — at (dx-0.4, dz-1.3) it lay
        // across the hatch rim (hid the rim + status lens, and would FLOAT over the
        // hole when the panels part). Now against the +Z wall beside the stack; the
        // hatch keeps a clear 360 read + clear drop path.
        place(aCrateL, 1.3f, 1.0f, cx(kCrateLAabb), kCrateLAabb.miny, cz(kCrateLAabb),
              dx - 2.0f, fY + 0.02f, dz + 0.6f, nullptr, tCrate);   // a third crate, angled
        // Ground the debris cluster so it sits in the corner instead of floating.
        addShadowBlob(m_shadowDisc, dx, fY, dz, 0.55f, 0.55f, 0.5f);            // barrel
        addShadowBlob(m_shadowDisc, dx - 0.9f, fY, dz - 0.2f, 0.75f, 0.75f, 0.5f); // crate stack
        addShadowBlob(m_shadowDisc, dx - 2.0f, fY, dz + 0.6f, 0.8f, 0.7f, 0.45f);  // angled crate
    }

    // EXIT SIGN over the doorway (the warehouse exit sign has a green-emissive face).
    {
        const float emGreen[4] = { 0.10f, 1.2f, 0.30f, 1.2f };
        place(aExit, -kPi * 0.5f, 1.0f, cx(kExitAabb), cy(kExitAabb), kExitAabb.maxz,
              x1 - 0.10f, ceilY - 0.55f, ccz, emGreen, nullptr);   // flush on the graybox face
        addLight(bt.jakeCell, x1 - 0.4f, ceilY - 0.6f, ccz, 1.6f, 0.07f, 0.5f, 0.13f);
    }

    // CEILING LIGHTING — WAVE-1 DIRECTOR'S CUT: both physical fixtures REMOVED after the
    // close-up survey identified them as the hero shot's worst cluster. SM_Light_A is not
    // a tube — it's a bare L-shaped WIRE (0.03 m rod) that read as dangling cabling below
    // the panels; the warehouse Hanging Light's bulb bloomed into the infamous white "bag"
    // (its shade + the panel seam trim behind it were the black "wings"). The coffered
    // R4 ceiling panels + light POOLS carry the read on their own — fixtures return only
    // when a real industrial caged-light GLB exists.
    {
        const float lx = ccx, lz = ccz;
        // Warm pool over the bed corner (was the hanging light's pool; kept, dimmed).
        addLight(bt.jakeCell, x0 + 1.3f, ceilY - 0.9f, z0 + 1.6f, 4.0f, 1.3f, 1.0f, 0.62f);
        // The motivated flickering overhead (cool-white, stutters via tick()) — the failing
        // fluorescent the player hears about; shallow depth so the room never goes black.
        const uint32_t li = (uint32_t)m_lights.size();
        addLight(bt.jakeCell, lx, ceilY - 0.40f, lz, 7.0f, 2.3f, 2.4f, 2.6f);
        m_flickers.push_back({ li, 2.3f, 2.4f, 2.6f, 0.0f, 9.0f, 0.35f });
        (void)aLight; (void)aHLight;   // loaded (hall may reuse); no cell placement
    }

    // RED ALARM WASH near the door (a low, saturated red accent so the exit reads as a
    // guarded threshold — moody contrast against the warm interior).
    addLight(bt.jakeCell, x1 - 0.6f, fY + 1.9f, ccz, 3.5f, 1.4f, 0.07f, 0.04f);

    // ================= ATMOSPHERE — drifting dust motes in the light pools ==========
    // The engine has NO fog API. Atmosphere is sold by a scatter of tiny EMISSIVE DUST
    // MOTES drifting up through the lit pools (caught in the light), which ride the same
    // room-gated draw as the props. They read as floating specks in the beams without the
    // risk of a full volumetric pass.
    //
    // SKIPPED (documented): (1) translucent CONE light-shafts via the glass pass — they
    // rendered as near-solid bright cones (the glass pipeline adds emissive pre-blend and
    // does not soft-fade a large near-camera hull), dominating the frame; removed as too
    // risky for a visual-only pass. (2) a depth/height-fog term in mesh.frag — that would
    // touch the tonemap path for EVERY world; out of scope + high-risk here. The motivated
    // HDR fixtures (tube / sconces / hanging light) already drive real bloom haloes, which
    // carry most of the "air in the room" read.
    {
        const uint32_t moteMesh  = addProcMesh(device, makeMoteQuad());
        // Dust motes drifting through the central + bunk pools (caught in the light).
        // R4: 48 -> 20 motes, glow 2.2/2.0 -> 1.1, neutral color — at the old count/glow
        // they read as a field of frozen white tracers against the dark ceiling.
        addDustMotes(moteMesh, 12, ccx, fY + 1.2f, ccz, 1.5f, 1.5f, fY + 0.3f, ceilY - fY - 0.6f,
                     1.0f, 1.0f, 1.1f, 1.1f);
        addDustMotes(moteMesh, 8, x0 + 1.3f, fY + 1.0f, z0 + 1.6f, 0.9f, 0.9f, fY + 0.3f, ceilY - fY - 0.8f,
                     1.1f, 1.0f, 0.8f, 1.1f);
    }

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
        // Pipes overhead near the hall's cell-facing (-X) end. R4: tinted (raw = white/red
        // plastic spaghetti — same offender as the cell ceiling).
        place(aPipes, kPi * 0.5f, 1.0f, cx(kPipesAabb), kPipesAabb.maxy, cz(kPipesAabb),
              hx0 + 2.0f, hCeil - 0.35f, hz - 1.2f, nullptr, tSteel);
        place(aPipes, kPi * 0.5f, 1.0f, cx(kPipesAabb), kPipesAabb.maxy, cz(kPipesAabb),
              hx0 + 2.0f, hCeil - 0.50f, hz + 1.2f, nullptr, tRust);
        // A wall terminal + cyan accent at the hall mouth.
        {
            const float tx = hx0 + 0.6f, tz = hz - 2.0f;
            // R5: no instance emissive (same whole-body-glow bug as the cell console).
            const float darkMetal[4] = { 0.22f, 0.26f, 0.34f, 1.0f };
            place(aConsole, -kPi * 0.5f, 1.0f, cx(kConsAabb), kConsAabb.miny, kConsAabb.minz,
                  tx, hfY, tz, nullptr, darkMetal);
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
    // DUST MOTES: each speck drifts slowly upward + circles laterally inside its pool,
    // looping over `span`. A slow tumble keeps the flat quad from disappearing edge-on.
    for (Mote& m : m_motes) {
        if (m.draw >= m_proc.size()) continue;
        m.phase += dt * m.rate;
        // Vertical loop (wrap 0..span), lateral lissajous so motes don't move in lockstep.
        float t = m.phase;
        float rise = std::fmod(t * 0.18f, 1.0f);                 // 0..1 up the column
        float y = m.riseY + rise * m.span;
        float ang = t * 0.9f;
        float lx = m.ox + std::cos(ang) * m.rx * (0.3f + 0.7f * std::sin(t * 0.4f));
        float lz = m.oz + std::sin(ang * 1.13f) * m.rz * (0.3f + 0.7f * std::cos(t * 0.37f));
        // A gentle tumble (yaw + a little pitch) so the speck card catches light.
        float c = std::cos(ang * 1.7f), s = std::sin(ang * 1.7f);
        ProcDraw& d = m_proc[m.draw];
        const float sz = m.size;
        d.transform[0]=c*sz; d.transform[1]=0;     d.transform[2]=-s*sz; d.transform[3]=0;
        d.transform[4]=0;    d.transform[5]=sz;    d.transform[6]=0;     d.transform[7]=0;
        d.transform[8]=s*sz; d.transform[9]=0;     d.transform[10]=c*sz; d.transform[11]=0;
        d.transform[12]=lx;  d.transform[13]=y;    d.transform[14]=lz;   d.transform[15]=1;
    }
}

void CellDressing::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const {
    for (const Instance& inst : m_instances) {
        const Asset& a = m_assetTable[inst.asset];
        for (const auto& d : a.drawables) {
            float fin[16];
            x3::asset::mulMat4(inst.transform, d.nodeTransform, fin);
            // Material emissive (from the GLB), SCALED by the instance glow strength
            // (emissive[3]). R4 let material emissive win unconditionally — random kit
            // pieces carry authored emissive (a yellow hazard panel in SM_Pipes_A, vent
            // fan faces) that bloomed through every tint as glowing blobs. Now a prop
            // placed with emissive=nullptr (alpha 0) shows NO authored glow; a prop that
            // opts in (alpha > 0) shows the authored glow at that strength. Props with no
            // material emissive keep the per-instance glow exactly as before.
            const bool matEmis = d.emissiveTexId != 0 ||
                d.emissiveFactor[0] > 0.001f || d.emissiveFactor[1] > 0.001f || d.emissiveFactor[2] > 0.001f;
            float emis[4];
            if (matEmis) { emis[0]=d.emissiveFactor[0]; emis[1]=d.emissiveFactor[1]; emis[2]=d.emissiveFactor[2]; emis[3]=inst.emissive[3]; }
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
    // ---- ATMOSPHERE: light shafts (soft translucent glass) + dust motes (emissive) ---
    const x3::rhi::TextureHandle white{ 0 };   // invalid -> built-in 1x1 white
    for (const ProcDraw& p : m_proc) {
        if (p.meshIdx >= m_procMeshes.size()) continue;
        const x3::rhi::MeshHandle mh = m_procMeshes[p.meshIdx];
        if (!mh.valid()) continue;
        if (p.glass) {
            x3::rhi::IRenderDevice::GlassMaterial gm;
            gm.opacity = p.color[3];
            gm.refraction = 0.0f;        // a light shaft must NOT distort the scene behind it
            gm.roughness = p.glassRough; // 1.0 for shafts/blobs; low for the window panes
            gm.specular = p.glassSpec;   // 0.0 for shafts/blobs; high for the window panes
            gm.tint[0] = p.color[0]; gm.tint[1] = p.color[1]; gm.tint[2] = p.color[2];
            device.drawMeshGlass(frame, mh, white, p.color, p.emissive, gm, p.transform);
        } else {
            device.drawMeshEmissive(frame, mh, white, p.color, p.emissive, p.transform);
        }
    }
}

uint32_t CellDressing::propsLoaded() const {
    uint32_t n = 0; for (const auto& a : m_assetTable) if (a.ok) ++n; return n;
}

} // namespace x3::game
