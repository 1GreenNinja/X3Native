// EFLZ opening-space polish — set-dressing + motivated lighting for the canon cell.
// See app/cell_dressing.h. Clean-room: built from the IModelLoader / IAssetSource /
// IRenderDevice interfaces + the converted GLB catalog only (mirrors env_art.cpp).
#include "cell_dressing.h"

#include "mesh_prims.h"          // x3::prims::makeBox / makeUVSphere (atmosphere geometry)
#include "asset_root.h"          // assetRoot() — the surface_library mount point
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

    // ---- THE HONEST-LIGHT RE-TUNE (2026-07-12) — see RIFTHUB_ART_TARGET.md ---------
    // Everything below this line that used to be tuned by "add more lumens" was tuned
    // against a BROKEN renderer: mesh.frag shaded every GLB at 1/PI vs the prim path
    // (fixed in 5c35d65), and NOBODY in the canon world ever called setAmbient — so the
    // cell ran at the engine default {0.42,0.44,0.50}. Result: a white cardboard box
    // with SEVENTEEN point lights inside it and not one shadow. The crutches removed in
    // this pass, in order of how much damage they did:
    //   1. THE AMBIENT FLOOD (0.42) — now owned per-zone by RoomDressing::applyZone-
    //      Atmosphere (ZWard -> 0.030). Ambient is omnidirectional: it lights a room by
    //      DESTROYING its contrast. This alone was the cardboard.
    //   2. SEVENTEEN LIGHTS in a 7x6 m cell (wall wash + corner key + floor fill + hatch
    //      spot + basin fill + two sconce washes + bunk pool + alarm + threshold + ...).
    //      Fill from every direction IS ambient by another name. Cut to SIX, of which
    //      exactly one — the failing fluorescent — is a key. Every survivor is motivated
    //      by a visible fixture (tube / sconce / exit sign / cam LED / terminal screen).
    //   3. An OVER-UNITY albedo on the cot (tint B = 1.08 — "the crate tex is dark"), a
    //      classic 1/PI compensation. Renormalized.
    //   4. FOUR full-wall GLASS panes ("armored glazing") whose only job was to plug the
    //      kit panels' window cutouts. Glass writes depth in the pre-pass, and at any
    //      opacity the pass pre-blends to MILK. Replaced with opaque dark plating.
    // ---- PAINTERLY LEVERS OPT-IN (ART_BIBLE §5) — the detention-zone atmosphere.
    // CellDressing::build only runs for the canon worlds, so this is the canonical
    // "host opt-in" site: worlds that never build the dressing keep fog + grade
    // fully OFF (byte-identical frames). Values = the bible's detention block:
    // subtle amber-grey air (~3%/10 m), teal-shadow/warm-highlight split-tone,
    // gentle vignette. Live-tunable later via the director's cvar block.
    // NOTE (AD-1): deliberately placed in a file no other active agent owns;
    // relocate to the app_run canon block when W2-A releases it if preferred.
    {
        x3::rhi::IRenderDevice::FogParams fog;
        fog.enabled  = true;
        fog.color[0] = 0.045f; fog.color[1] = 0.040f; fog.color[2] = 0.034f;
        fog.density  = 0.0035f;
        fog.start    = 1.2f;      // clean air around the viewmodel/arms
        fog.maxOpacity = 0.60f;   // far walls never wash out
        device.setFog(fog);
        x3::rhi::IRenderDevice::GradeParams gr;
        gr.strength = 0.85f;
        gr.shadowTint[0] = 0.94f; gr.shadowTint[1] = 1.00f; gr.shadowTint[2] = 1.03f;  // teal shadows
        gr.highlightTint[0] = 1.04f; gr.highlightTint[1] = 1.00f; gr.highlightTint[2] = 0.95f; // warm pools
        gr.saturation = 0.96f;
        gr.vignette   = 0.10f;
        device.setGrade(gr);
    }

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

    // ---- REAL OBSERVATION WINDOW (feat/cell-real-glass). The +Z wall faces the Main Hall.
    // buildCanonFloor punches a full-height, collision-SEALED opening in that graybox wall
    // (cellObsWindow — the SAME span resolved here), so through it the hall is actually
    // visible instead of flat-blue graybox. Here we (a) skip the opaque SM_Wall panel that
    // would occlude it, (b) leave the whole-wall privacy glaze OUT of this span, and (c)
    // glaze it with a genuinely CLEAR armored pane + a reinforced mullion frame below.
    const CellWindow obsWin = cellObsWindow(floor);
    const bool haveWin = obsWin.valid() && obsWin.wall == 3;   // +Z (hall-facing) only

    // ---- REAL DOORWAY OPENINGS of this cell (WAVE — cell-door fix). The 7x6 cell
    // OVERLAPS its neighbours, so the resolver opens Overlap *junctions* (NO slab) on the
    // +Z (Main Hall — the primary egress) and +X (West Cell Hall) walls, plus an Adjacent
    // door on the -Z wall (WL-2). Previously this module HARDCODED a single decorative
    // frame on the +X wall at ccz — which (a) missed the real openings (they sit off-centre
    // at the resolved cut coords) so the frame floated in a solid wall while the traversed
    // gap stayed bare graybox, and (b) let the armoured-glass panes below seal the actual
    // openings behind a flat grey sheet ("the oddly-coloured panel the player walks
    // through"). Fix: drive BOTH the frames and the pane-clipping off floor.doorways so the
    // decor lands ON the real openings and never glazes a threshold. wall: 0=-X 1=+X 2=-Z
    // 3=+Z; c = opening centre along that wall's run (Z for X-walls, X for Z-walls); half =
    // the resolved cut half-width. (GapBridge/CrossLevel doorways own a separate
    // corridor/tube and are NOT openings on this cell's own walls, so they are skipped.)
    struct CellOpening { int wall; float c; float half; };
    std::vector<CellOpening> openings;
    for (const CanonDoorway& dw : floor.doorways) {
        if (dw.a != bt.jakeCell && dw.b != bt.jakeCell) continue;
        if (dw.kind == DoorwayKind::GapBridge || dw.kind == DoorwayKind::CrossLevel ||
            dw.kind == DoorwayKind::None) continue;
        const float oh = (dw.cutHalf > 0.05f) ? dw.cutHalf : 0.8f;
        if (dw.axis == 0) {   // wall plane X=const -> -X or +X wall; run along Z
            const int wall = (std::fabs(dw.cx - x0) < std::fabs(dw.cx - x1)) ? 0 : 1;
            openings.push_back({ wall, dw.cz, oh });
        } else {              // wall plane Z=const -> -Z or +Z wall; run along X
            const int wall = (std::fabs(dw.cz - z0) < std::fabs(dw.cz - z1)) ? 2 : 3;
            openings.push_back({ wall, dw.cx, oh });
        }
    }
    // True if [a0,a1] on `wall` overlaps a doorway opening (with a small jamb margin) — used
    // to keep the armoured-glass panes OUT of the thresholds so the openings read clear.
    auto spansOpening = [&](int wall, float a0, float a1) {
        for (const CellOpening& o : openings) {
            if (o.wall != wall) continue;
            const float m = 0.15f;   // jamb margin
            if (a1 > o.c - o.half - m && a0 < o.c + o.half + m) return true;
        }
        return false;
    };

    // ======================================================================
    // THE MOUNTED-PROP LAW (fix/spawn-anomalies, 2026-08-17)
    // ======================================================================
    // A prop that is BOLTED TO A WALL needs a wall to be bolted to. Obvious;
    // nothing enforced it.
    //
    // The shipped defect: a small white low-poly wedge floating in a cell-block
    // DOORWAY at head height, filed by the VFX lane (d8bf8224) as a stray
    // "delta-wing ship" and reported again mid-hall. It is neither ship nor
    // marker mesh — it is a Main Hall WALL SCONCE (SciFi_Warehouse_Kit/Wall
    // Light.glb, 0.25 x 0.26 x 0.06 m, lit lens emissive). The opening-route
    // sconce run marches down the hall's long walls on a blind fixed pitch
    // (`sx += 6.5`), and the hall's -Z wall is PIERCED by the cell-block
    // doorways: the run's first stop lands squarely on the cell's door opening.
    // With no wall behind it the fixture hangs in the doorway, and seen
    // point-blank from inside the cell that untextured, self-lit lens reads as a
    // white paper-plane wedge embedded in the door header.
    //
    // This is the SAME defect family as the pistol buried in the jamb
    // (canonPickupSpotClear, d8bf8224) and the drone floating inside the header
    // (THE HOVER RULE, monster.cpp): AN AUTHORED OFFSET APPLIED WITHOUT ASKING
    // WHETHER THE SURFACE IT ASSUMES IS ACTUALLY THERE. The answer is the same
    // one all three times — probe first, and let the data move the prop.
    //
    // wallSolidAt(): is `room`'s wall `wall` (0=-X 1=+X 2=-Z 3=+Z) SOLID over
    // [coord-halfW, coord+halfW] along that wall's run? False when the span
    // crosses a resolved doorway cut (plus a jamb margin). Driven off the same
    // floor.doorways data the frames and pane-clipping already use, so a future
    // layout change moves the sconces with it instead of stranding them in air.
    auto wallSolidAt = [&](uint32_t room, int wall, float coord, float halfW) -> bool {
        if (room == kNoRoom || room >= floor.rooms.size()) return true;
        const CanonRoom& R = floor.rooms[room];
        const bool xWall = (wall == 0 || wall == 1);
        const float plane = (wall == 0) ? R.x0() : (wall == 1) ? R.x1()
                          : (wall == 2) ? R.z0() : R.z1();
        for (const CanonDoorway& dw : floor.doorways) {
            if (dw.a != room && dw.b != room) continue;
            if (dw.kind == DoorwayKind::GapBridge || dw.kind == DoorwayKind::CrossLevel ||
                dw.kind == DoorwayKind::None) continue;
            // axis 0 = wall plane X=const (run along Z); axis 1 = plane Z=const (run along X).
            if ((dw.axis == 0) != xWall) continue;
            const float dwPlane = xWall ? dw.cx : dw.cz;
            if (std::fabs(dwPlane - plane) > 0.75f) continue;   // a cut in a DIFFERENT wall
            const float c  = xWall ? dw.cz : dw.cx;             // cut centre along the run
            const float oh = (dw.cutHalf > 0.05f) ? dw.cutHalf : 0.8f;
            const float m  = 0.20f;                             // jamb margin
            if (coord + halfW > c - oh - m && coord - halfW < c + oh + m) return false;
        }
        return true;
    };
    // Slide a wall-mounted prop along its wall to the first SOLID spot, searching
    // outward from the authored coordinate in `step` increments up to `reach`.
    // Returns false when the whole neighbourhood is opening — the caller then
    // SKIPS the prop rather than hanging it in a doorway.
    auto slideToSolidWall = [&](uint32_t room, int wall, float& coord, float halfW,
                                float reach, float step) -> bool {
        if (wallSolidAt(room, wall, coord, halfW)) return true;
        for (float d = step; d <= reach + 1e-4f; d += step) {
            if (wallSolidAt(room, wall, coord - d, halfW)) { coord -= d; return true; }
            if (wallSolidAt(room, wall, coord + d, halfW)) { coord += d; return true; }
        }
        return false;
    };

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

    // ================= THE DECK — a REAL floor (recipe ingredient 4) ================
    // The cell floor was the graybox: a flat-tinted box. Flat colors do not catch light,
    // so the one thing the player stares at while lying on the cot had no material read
    // at all. Tile the ward's authored PBR set (hh_floor_01a — albedo + NORMAL + mr, with
    // real grime/wear in the normal map) over it at the 0.14 m inset law, so the
    // practical's pool rakes across actual surface relief and the wet spec breaks up.
    {
        m_surf.mount(assetRoot() + "/surface_library");
        const SurfaceSet& sDeck = m_surf.get(device, "hh_floor_01a");
        if (sDeck.ok) {
            const float w = x1 - x0, d = z1 - z0;
            SurfPanel p;
            p.set  = &sDeck;
            p.tint[0] = 0.40f; p.tint[1] = 0.41f; p.tint[2] = 0.40f;   // worn, wet-dark deck (renormalized with the walls)
            p.mesh = m_surf.makePanel(device, /*axis 1 = floor, faces +Y*/1, w, d, 2.0f);
            // The library's floor quad is centred on X but runs 0..d along +Z, so the Z
            // origin is z0 (not the room centre). Lift 1.2 cm off the graybox floor plane
            // so it never z-fights the collision truth underneath it.
            p.transform[12] = (x0 + x1) * 0.5f;
            p.transform[13] = fY + 0.012f;
            p.transform[14] = z0;
            m_surfPanels.push_back(p);
        }
    }

    // Consistent industrial palette tints so the warehouse-kit props read as one cohesive
    // dressed space (the raw GLBs vary from near-white to grey, which looks scattered).
    const float tCrate[4]  = { 0.66f, 0.60f, 0.52f, 1.0f };  // weathered crate (warm grey, lifted R3)
    // R11: was { 0.92, 0.95, 1.08 } — an OVER-UNITY albedo multiplier (a surface cannot
    // reflect 108% of the blue that lands on it). It was cranked because the cot read as
    // "black-gold trash bags"; the real cause was the 1/PI GLB shading bug, now fixed.
    const float tBunk[4]   = { 0.62f, 0.63f, 0.68f, 1.0f };  // cot: honest institutional grey
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

    // ================= WALLS — R11: REAL PBR SURFACE SETS, not kit slabs ============
    // WHAT WAS HERE: the four walls were tiled with the ModularSciFi kit's SM_Wall_A/B/C
    // GLB slabs. Five rounds of comments in this block are the author fighting them —
    // "A's UVs map a near-FLAT plain-panel region", "weight the sequence toward B/C so no
    // large wall lands a flat A slab", tint them dark "so the relief reads instead of
    // blowing to flat white". It never worked: the honest-light screenshot proves the
    // cell walls are FEATURELESS PAINTED DRYWALL with a gradient on them. And B/C carry
    // authored WINDOW CUTOUTS — holes in a detention cell — which is why F3 had to hang a
    // full-wall glass pane behind every one of them (see the plating block, now deleted).
    //
    // The recipe (RIFTHUB_ART_TARGET ingredient 4) says: use the surface_library sets,
    // with real rivets/wear/grime in the NORMAL map. So do that. hh_wall_01a is the SAME
    // ward set the other 110 canon rooms are dressed with — authored albedo + normal + mr,
    // no cutouts, no glass, no tint hacks. Tiled at 2.6 m so the panel run reads at eye
    // height, opening-aware around the +X doorway (the level-authoring law: no panel ever
    // covers a door).
    {
        const float wallH = ceilY - fY;
        const float inset = 0.12f;   // graybox slab is 0.2 m thick, centred -> face at 0.10
        const SurfaceSet& sWall = m_surf.get(device, "hh_wall_01a");
        // A wall panel: quad + a yaw about +Y so its normal faces INTO the room. The
        // surface-library quad is authored with its base at local y=0 and centred on its
        // long axis, so (wx, fY, wz) seats it on the floor at the wall plane.
        auto addWall = [&](int axis, float yaw, float w, float wx, float wz) {
            if (!sWall.ok || w < 0.2f) return;
            SurfPanel p;
            p.set  = &sWall;
            // Grimed down from clinical white to a dirty institutional grey-green. The
            // relief, the cracks and the tile grout all still read — they read BETTER,
            // because they are no longer sitting at the top of the exposure curve.
            // VALUE, NOT LUMENS. hh_wall_01a's albedo texture is near-white plaster
            // (measured mean ~0.496 linear); a 0.50 tint left the wall at ~0.248 linear =
            // 0.53 sRGB - ABOVE the 0.35-0.50 band a painted institutional wall actually
            // sits in, so anything that lit it clipped. 0.42 -> ~0.208 linear / 0.49 sRGB:
            // still plaster, no longer parked at the top of the exposure curve.
            p.tint[0] = 0.42f; p.tint[1] = 0.43f; p.tint[2] = 0.41f;
            p.mesh = m_surf.makePanel(device, axis, w, wallH, 2.6f);
            const float c = std::cos(yaw), s = std::sin(yaw);
            p.transform[0]=c;  p.transform[1]=0; p.transform[2]=-s; p.transform[3]=0;
            p.transform[4]=0;  p.transform[5]=1; p.transform[6]=0;  p.transform[7]=0;
            p.transform[8]=s;  p.transform[9]=0; p.transform[10]=c; p.transform[11]=0;
            p.transform[12]=wx; p.transform[13]=fY; p.transform[14]=wz; p.transform[15]=1;
            m_surfPanels.push_back(p);
        };
        // WALLS: playable-build (R11) RETIRED the kit wall slabs here — they carried authored
        // cutouts that had to be glazed over, and their sub-1.0 tint hacks were 1/PI-era. The
        // cell is authored quads now. Kept verbatim, with ONE addition: the +Z wall is SPLIT
        // around the observation window (0ffff60) exactly the way the +X wall is split around
        // its doorway, so nothing opaque is ever drawn across the see-through opening.
        // -X wall: axis 2 authors a ZY quad whose normal is +X — already facing the room.
        addWall(2, 0.0f, z1 - z0, x0 + inset, (z0 + z1) * 0.5f);
        // +Z wall: axis 0 authors an XY quad facing -Z — already facing the room. When the
        // observation window exists, emit the two flanking segments instead of one full quad.
        if (haveWin) {
            const float segL = obsWin.lo - x0, segR = x1 - obsWin.hi;
            if (segL > 0.05f) addWall(0, 0.0f, segL, (x0 + obsWin.lo) * 0.5f, z1 - inset);
            if (segR > 0.05f) addWall(0, 0.0f, segR, (obsWin.hi + x1) * 0.5f, z1 - inset);
        } else {
            addWall(0, 0.0f, x1 - x0, (x0 + x1) * 0.5f, z1 - inset);
        }
        // -Z wall: same quad, yawed 180 so its normal points +Z (into the room). Normals
        // matter even though the quad is double-wound — a back-facing normal would shade
        // the wall as if the light were behind it.
        addWall(0, kPi, x1 - x0, (x0 + x1) * 0.5f, z0 + inset);
        // +X wall: the DOORWAY wall. Two segments flanking the ~2.4 m opening at ccz, so
        // nothing is ever drawn across the threshold. Yawed 180 -> normal points -X.
        {
            const float halfOpen = 1.30f;
            const float segA0 = z0, segA1 = ccz - halfOpen;
            const float segB0 = ccz + halfOpen, segB1 = z1;
            addWall(2, kPi, segA1 - segA0, x1 - inset, (segA0 + segA1) * 0.5f);
            addWall(2, kPi, segB1 - segB0, x1 - inset, (segB0 + segB1) * 0.5f);
        }

        // THE OBSERVATION WINDOW (0ffff60) — kept. The kit slabs are gone, so the F3
        // armored-glass GLAZING that sealed their cutouts went with them; this pane is a
        // different thing: a real, collision-sealed see-through viewport into the Main Hall.
        // (paneMesh was declared by that retired glazing block; the window is its only
        // consumer now, so it moves here.)
        {
            const uint32_t paneMesh = addProcMesh(device, makeMoteQuad());
            if (haveWin) {
                const float wz    = z1 - 0.12f;               // pane plane (roomward of z1, faces the hall)
                const float wy0   = fY + 0.04f, wy1 = ceilY - 0.04f;   // fill the full-height opening
                const float wlo   = obsWin.lo,  whi = obsWin.hi;
                // Emit an axis-'z' (XY-plane) glass quad rect with an explicit material.
                auto glassRect = [&](float xlo, float xhi, float ylo, float yhi, float zpl,
                                     float r, float g, float b, float op, float rough, float spec) {
                    if (xhi - xlo < 0.01f || yhi - ylo < 0.01f) return;
                    ProcDraw d; d.meshIdx = paneMesh; d.glass = true;
                    d.color[0]=r; d.color[1]=g; d.color[2]=b; d.color[3]=op;
                    d.glassRough = rough; d.glassSpec = spec;
                    const float w = xhi - xlo, h = yhi - ylo;
                    d.transform[0]=w;  d.transform[1]=0; d.transform[2]=0;  d.transform[3]=0;
                    d.transform[4]=0;  d.transform[5]=h; d.transform[6]=0;  d.transform[7]=0;
                    d.transform[8]=0;  d.transform[9]=0; d.transform[10]=1; d.transform[11]=0;
                    d.transform[12]=(xlo+xhi)*0.5f; d.transform[13]=(ylo+yhi)*0.5f; d.transform[14]=zpl; d.transform[15]=1;
                    m_proc.push_back(d);
                };
                // (1) The CLEAR armored viewport: near-clear (alpha 0.04) so the hall reads
                // straight through, faint cool tint, polished (low roughness). Specular kept
                // RESTRAINED (0.18) — a high spec drives the glass pass's environment-
                // reflection sheen, which milks a big flat pane over; a low spec keeps the
                // depth crisp with just an edge glint so it still reads as a glass surface.
                glassRect(wlo, whi, wy0, wy1, wz, 0.76f, 0.85f, 0.92f, 0.04f, 0.04f, 0.18f);
                // (2) The reinforced MULLION FRAME: thin dark-steel bars (near-opaque glass,
                // so they stay LIT by the room like real metal) — a full perimeter, one
                // vertical centre bar, and two horizontal transoms => a gridded armored
                // viewport. A hair roomward of the glass so it always reads in front.
                const float fr[3] = { 0.14f, 0.15f, 0.18f };   // dark gunmetal
                const float fz = wz - 0.02f, t = 0.05f;         // bar half-thickness in world units
                auto bar = [&](float xlo, float xhi, float ylo, float yhi) {
                    glassRect(xlo, xhi, ylo, yhi, fz, fr[0], fr[1], fr[2], 0.94f, 0.5f, 0.25f);
                };
                bar(wlo - t, whi + t, wy1 - t, wy1 + t);        // head bar
                bar(wlo - t, whi + t, wy0 - t, wy0 + t);        // sill bar
                bar(wlo - t, wlo + t, wy0 - t, wy1 + t);        // left jamb
                bar(whi - t, whi + t, wy0 - t, wy1 + t);        // right jamb
                const float wmid = (wlo + whi) * 0.5f;
                bar(wmid - t, wmid + t, wy0, wy1);              // vertical centre mullion
                const float t1 = fY + (ceilY - fY) * 0.38f, t2 = fY + (ceilY - fY) * 0.70f;
                bar(wlo, whi, t1 - t, t1 + t);                  // lower transom
                bar(wlo, whi, t2 - t, t2 + t);                  // upper transom
                // A soft cool wash on the sill so the viewport glass + frame catch a key
                // (guard-station light spilling in), and the window doesn't sit in shadow.
                addLight(bt.jakeCell, wmid, fY + 1.5f, z1 - 0.5f, 3.0f, 0.55f, 0.72f, 0.95f);
            }
        }
        (void)aWall; (void)aWallB; (void)aWallC;   // kit slabs retired (still loaded for other users)
        (void)tWall; (void)tWallWarm;              // their tint hacks retired with them

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
            // QA MAINLEVEL SWEEP (propclip lint): the old 2x2 grid of RAW 4x3 m panels
            // (x0+2 + 4*ix) spanned 8 m over a 7 m cell — the east tile overhung Jake's
            // Cell by 1 m and hung VISIBLY inside the West Cell Hall's airspace (0.5 m
            // below the hall ceiling, over the throat). Scale 0.875 makes the X span
            // exactly 7 m (2 x 3.5); Z becomes 2 x 2.625 = 5.25 over the 6 m cell, the
            // 0.375 m end borders reading as honest shadow gaps at the wall junctions.
            const float cs = 0.875f;
            for (int ix = 0; ix < 2; ++ix)
                for (int iz = 0; iz < 2; ++iz)
                    place(aCeil, 0.0f, cs, cx(kCeilAabb), kCeilAabb.miny, cz(kCeilAabb),
                          x0 + 1.75f + 3.5f * ix, ceilY - cInset,
                          z0 + 1.6875f + 2.625f * iz,
                          nullptr, tCeil);
        }
    }

    // ================= WALL DETAIL — vents / conduit / cam / signage / sconces =====
    // Break the panels with recessed/mounted industrial detail so the surface reads as
    // a built detention wall (not a tiled kit). Each is anchored flush to a wall plane.
    {
        // A recessed VENT GRATE high on the -Z wall. WAVE-2B (LD review #4a): these two
        // grates were the "floating ceiling crates" — the 1 m-deep Duct Vent box is
        // authored with its depth along LOCAL +Z, and at yaw 0 that +Z points INTO the
        // room (the -Z wall is at z0, so "into the wall" is -Z, not +Z). Anchored at the
        // wall plane, the whole rusty-brown box protruded a metre into the upper room and
        // read as a crate hanging by the pipes. Fix: yaw 180deg so the body recesses INTO
        // the wall (-Z) with only the grate mouth flush + proud (z0+0.02), facing the room.
        place(aVent, kPi, 1.0f, cx(kVentAabb), cy(kVentAabb), kVentAabb.minz,
              ccx - 1.4f, fY + 2.7f, z0 - 0.06f, nullptr, tRust);
        place(aVent, kPi, 0.85f, cx(kVentAabb), cy(kVentAabb), kVentAabb.minz,
              ccx + 1.5f, fY + 2.9f, z0 - 0.06f, nullptr, tRust);
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
        // Tiny red cam LED — motivated by the housing, and the only thing watching you.
        addLight(bt.jakeCell, x1 - 0.4f, ceilY - 0.45f, z1 - 0.4f, 1.4f, 0.35f, 0.02f, 0.02f);
        // FIRE EXTINGUISHER — R5 final: BY THE DOOR on the +X wall (where extinguishers
        // live — beside the exit). Every paneled wall is window-dominated (B/C panels
        // carry cutouts at each panel center) and the -X wall also holds the trapdoor
        // ladder; the +X door wall is the one guaranteed-solid surface. yaw 0 keeps the
        // local +X mount-back facing the +X wall; back plane on the graybox face.
        // DECLUTTER 2026-07-12 (Tim: "why is it so cluttered?") -- CUT: the FIRE
        // EXTINGUISHER. You do not hang a steel club on the wall of a locked cell with the
        // prisoner. It was pure dressing; it earned nothing; under honest light it read as
        // red junk. (void) below keeps the asset load harmless.
        (void)aExt; (void)tRed;
        // More -X back-wall detail so the HERO diagonal (which faces this corner) reads as
        // a built surface, not a bare panel: a wall-mounted fusebox/panel + a recessed vent.
        // R4: the fusebox is SLIMMED (0.55) and tinted mid-gunmetal — at tDark + 0.85 scale
        // it was a featureless 2.2 m BLACK PLANK sticking 0.5 m off the wall.
        // DECLUTTER: CUT the -X wall FUSEBOX too. One already sits on the -Z wall; two
        // fuseboxes in a 4 m cell is a plant room, not a cell. The recessed vent below
        // STAYS -- it is the one thing on that wall a real cell would have.
        place(aVent, -kPi * 0.5f, 1.0f, cx(kVentAabb), cy(kVentAabb), kVentAabb.minz,
              x0 + 0.16f, fY + 2.6f, ccz - 0.6f, nullptr, tRust);
        // R11 — CUT: the "grazing wall-wash" on the -X wall. A wash from the far side of
        // the room is not a light source, it is fill; three of them from three directions
        // are ambient with extra steps. The -X wall now gets its relief from the single
        // overhead practical raking DOWN it (which is what a ceiling tube actually does).
        // WALL SCONCE — ONE, by the door (the second, over the bunk, is CUT: two sconces
        // + a tube + a wash meant no surface in the cell could ever fall into shadow).
        // R4: the sconce's screen is its local -Z face, and local -Z maps to world
        // (sin yaw, 0, -cos yaw). On the +X wall the screen must face -X -> yaw = -pi/2.
        const float emSconce[4] = { 1.0f, 0.86f, 0.62f, 1.0f };
        // THE MOUNTED-PROP LAW (see wallSolidAt above): the +X wall carries this
        // cell's West Cell Hall opening, so the authored ccz+1.4 seat is not
        // guaranteed to be wall. Slide along the wall to solid, or drop the
        // fixture and its pool rather than float them in the doorway.
        float sconceZ = ccz + 1.4f;
        const float cellSconceHalf = (kWLightAabb.maxx - kWLightAabb.minx) * 0.5f;
        const bool sconceSeated =
            slideToSolidWall(bt.jakeCell, 1, sconceZ, cellSconceHalf, 1.6f, 0.3f);
        if (sconceSeated)
        place(aWLight, -kPi * 0.5f, 1.0f, cx(kWLightAabb), cy(kWLightAabb), kWLightAabb.maxz,
              x1 - 0.10f, fY + 2.2f, sconceZ, emSconce, tSteel);   // back ON the graybox face
        // Its pool: SMALL and warm. It is a bulb on a wall, not a floodlight — it lights
        // the plate it is bolted to and dies within a couple of metres.
        if (sconceSeated)
            addLight(bt.jakeCell, x1 - 0.45f, fY + 2.2f, sconceZ, 2.2f, 0.55f, 0.44f, 0.29f);
    }

    // ================= CELL DOORWAYS — a reinforced frame at EACH real opening =======
    // WAVE (cell-door fix). The old code dropped ONE SM_DoorFrame_A on the +X wall at ccz
    // from a hardcoded "+X = exit" guess — but the resolver actually cuts this cell's
    // traversed openings elsewhere (Main Hall on +Z, West Cell Hall on +X, WL-2 on -Z; see
    // `openings`). So the frame floated in a solid wall while the real gaps stayed bare
    // graybox. Fix: frame EACH resolved opening, seated on the cell floor (Rule 4 — the
    // frame base is the contact surface) and centred on the resolved cut (Rule 6 — contents
    // relative to bounds). SM_DoorFrame_A is a 6.25 m showroom frame; frScale reduces it to
    // a single-door jamb (~2.6 m wide × ~1.85 m) that straddles the ~1.6 m opening. The
    // frame's probed base (kDoorFrAabb.miny) is anchored at fY so the jamb rests ON the deck
    // (the old ring read as floating — its bright octagon head sat mid-wall over a bare
    // sill); a taller frScale lifts the header to a full standing threshold.
    {
        const float frScale = 0.58f;   // taller jamb: header ~2.55 m so the opening clears standing
        auto placeDoorFrame = [&](int wall, float runC) {
            float yaw = 0.0f, wx = 0.0f, wz = 0.0f;
            // lx/lz = the THRESHOLD LAMP seat: the same opening, pushed 0.25 m in from the
            // wall plane along that wall's INWARD normal, so the lamp hugs the lintel it is
            // bolted to instead of hanging out in the room in front of the slab. (See B5.)
            float lx = 0.0f, lz = 0.0f;
            switch (wall) {
                case 0: yaw =  kPi * 0.5f; wx = x0 + 0.06f; wz = runC;       // -X wall, faces +X
                        lx = x0 + 0.25f;   lz = runC;       break;
                case 1: yaw = -kPi * 0.5f; wx = x1 - 0.06f; wz = runC;       // +X wall, faces -X
                        lx = x1 - 0.25f;   lz = runC;       break;
                case 2: yaw =  0.0f;       wx = runC;       wz = z0 + 0.06f; // -Z wall, faces +Z
                        lx = runC;         lz = z0 + 0.25f; break;
                case 3: yaw =  kPi;        wx = runC;       wz = z1 - 0.06f; // +Z wall, faces -Z
                        lx = runC;         lz = z1 - 0.25f; break;
            }
            // Seat the frame's VISIBLE jamb on the floor. SM_DoorFrame_A's bright jamb sits
            // ~0.4 m above its probed AABB base (the base is a thin sill), so anchoring miny
            // at fY left the ring reading as "floating". Drop the anchor by kFrameSeat so the
            // visible jamb rests on the deck (the buried sill below the floor is unseen).
            constexpr float kFrameSeat = 0.40f;
            place(aDoorFr, yaw, frScale, cx(kDoorFrAabb), kDoorFrAabb.miny, cz(kDoorFrAabb),
                  wx, fY - kFrameSeat, wz, nullptr, tSteel);
            // THE THRESHOLD: one low, saturated red per opening — the locked-in tell.
            // R11 (playable-build): 1.5 -> 0.62. It was tuned when GLBs shaded at 1/PI, and
            // there was a SECOND 3.5 m "alarm wash" stacked on it. One red, at honest strength.
            // The per-opening PLACEMENT is kept (e70b8c9): the cell has three thresholds and
            // the old code hardcoded ONE frame on +X from a wrong "+X = exit" guess, so frames
            // floated in solid walls while the real doorways stayed bare graybox.
            //
            // B5 / THE PATTERN — THIS is why the door rendered PINK, and it was NOT the albedo.
            // MEASURED: hanging ~0.5 m off the slab, HEAD-ON (N.L ~ 1) at slab-centre height
            // with range 3.0, this lamp delivered ~0.43 red to the door face while the room's
            // white key — a CEILING tube, GRAZING the vertical slab — delivered only ~0.35.
            // The red WON: the door was a red-LIT surface, so a neutral grey slab read salmon
            // and its trim glowed magenta. Renormalizing the albedo only scaled the value
            // (R-G held at +57); the HUE never moved, which is the proof.
            // A door-status lamp does not floodlight a door — it HUGS the frame. Mount it over
            // the lintel (a real "LOCKED" indicator, fY+2.18) and tighten the range to a local
            // pool (3.0 -> 1.5): grazing incidence on the slab gives a red gradient at the head
            // of the door + a pool on the jamb and threshold floor, while the white key defines
            // the door as institutional grey. The tell survives; the wash does not.
            addLight(bt.jakeCell, lx, fY + 2.18f, lz, 1.5f, 0.55f, 0.030f, 0.02f);
        };
        for (const CellOpening& o : openings) placeDoorFrame(o.wall, o.c);
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
        // R11 — THE WHOLE BUNK LIGHT PILE IS CUT. Four lights (warm cot pool 2.6 + corner
        // key 1.7 + floor fill 0.5 + hatch spot 1.15) existed to make a GLB cot that was
        // shading at 1/PI look like an object. The renderer is honest now (5c35d65) and
        // the cot's own albedo does that job. The bed is lit by the tube overhead — from
        // ABOVE, like a bed in a cell — and the parts of it the tube cannot reach are
        // SUPPOSED to be dark. That is the whole point of the room.
        // (The hatch spot went with them: the trapdoor's own amber rim lens is its read;
        // a hidden hatch that is helpfully spotlit is not hidden.)
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
        // The screen's own spill — the ONE accent hue in the room (bible: one accent).
        // Small range: a monitor lights the wall it sits on, not the far corner.
        addLight(bt.jakeCell, tx, fY + 1.25f, tz + 0.5f, 2.4f, 0.09f, 0.42f, 0.58f);
    }

    // SINK / TOILET FIXTURE — a stainless basin in the -X/+Z corner (the warehouse bin
    // reads as a cell's combined steel toilet/sink unit). Lit by its own small fill so it
    // never goes black. A detention staple that grounds the space as a lived-in cell.
    {
        const float fx = x0 + 0.55f, fz = z1 - 0.7f;
        place(aBin, 0.0f, 0.95f, cx(kBinAabb), kBinAabb.miny, cz(kBinAabb),
              fx, fY + 0.02f, fz, nullptr, tSteel);
        // R11 — CUT: the basin's private fill light ("so it never goes black"). A steel
        // basin in the far corner of a cell SHOULD be nearly black; that is what a corner
        // is. Its rim catches the tube and that is all it gets.
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
        // DECLUTTER: THREE overhead pipe runs in a 4 m cell was spaghetti. ONE stays --
        // a single service pipe crossing the ceiling is the industrial note; three is a
        // boiler room. (The hall keeps its own runs.)
        // A cross pipe run along the back -Z wall near the ceiling (yaw +pi/2 -> runs in X).
        (void)tDark;
    }

    // FUSEBOX / wall panels (security + monitoring look) mounted on the -Z and +Z walls.
    // R4: tinted mid-gunmetal — untinted they rendered raw WHITE plastic.
    {
        const float tPanelBox2[4] = { 0.30f, 0.32f, 0.36f, 1.0f };
        place(aFuse, kPi, 0.8f, cx(kFuseAabb), 0.0f, kFuseAabb.minz,
              x0 + 0.6f, fY + 1.1f, z0 + 0.18f, nullptr, tPanelBox2);
        // DECLUTTER: the +Z fusebox twin is CUT (see above).
    }

    // DEBRIS / CLUTTER cluster in the +X-near corner (lived-in, ransacked feel): a barrel,
    // a toppled short crate, and a stacked pair.
    {
        // ============ DECLUTTER 2026-07-12 -- THE EMPTINESS *IS* THE FEELING ==========
        // Tim, live: "the Cell is TINY, and CLUTTERED. Why is it so cluttered?"
        // CUT IN FULL: the DEBRIS CLUSTER -- a fuel BARREL, THREE crates (including a
        // stacked pair) and their shadow blobs, packed into the corner of a 4 m LOCKED
        // CELL. A detention cell is not a store room. Nobody leaves a prisoner a barrel
        // and a crate stack to climb on; the corner they filled is the corner the player
        // has to stand in; and under honest light they read as junk, not as story.
        // Every prop that SURVIVES this room now earns its place: it is STORY (the bunk he
        // wakes on, the terminal that opens the door, the basin), it is ARCHITECTURE (the
        // door, one vent, one pipe run, one fusebox), or it MOTIVATES A LIGHT (the failing
        // tube, the cam, the exit sign). Nothing here is decoration for its own sake.
        // The barrels and crates still exist where they BELONG -- the Main Hall mouth,
        // just outside the door, which is also where they can be climbed on.
        // MERGE 2026-07-12 (fold/intro-cockpit): the fold made this corner's drum a REAL
        // explodable BarrelSystem barrel (e7a2986 "barrels explode game-wide") and kept the
        // two stacked crates. The FEATURE is kept -- but NOT in here. The declutter above is
        // the newer call and it is the owner's: a 4 m locked cell does not get a barrel and a
        // crate stack. The explodable drum lives at the HALL clutter pile (see the main-hall
        // section below: m_barrelSink at hx0+0.9), which is the Main Hall mouth just outside
        // this door -- the "red tank by the cell door" the owner asked to be able to shoot,
        // and the place it can actually be climbed on. Explodable barrels are unaffected.
        (void)aBarrel; (void)aCrateS; (void)tBarrel; (void)tCrate;
        // R7: the angled crate moved OFF the trapdoor — at (dx-0.4, dz-1.3) it lay
        // across the hatch rim (hid the rim + status lens, and would FLOAT over the
        // hole when the panels part). Now against the +Z wall beside the stack; the
        // hatch keeps a clear 360 read + clear drop path.
        (void)aCrateL;
    }

    // EXIT SIGN over the doorway (the warehouse exit sign has a green-emissive face).
    {
        const float emGreen[4] = { 0.10f, 1.2f, 0.30f, 1.0f };
        place(aExit, -kPi * 0.5f, 1.0f, cx(kExitAabb), cy(kExitAabb), kExitAabb.maxz,
              x1 - 0.10f, ceilY - 0.55f, ccz, emGreen, nullptr);   // flush on the graybox face
        addLight(bt.jakeCell, x1 - 0.4f, ceilY - 0.6f, ccz, 1.5f, 0.05f, 0.34f, 0.09f);
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
        // R11 — CUT: the "warm pool over the bed corner" (a lightbulb with no lightbulb).
        // THE KEY — and now the ONLY key in the room: the failing fluorescent overhead.
        // A single hard cool-white source at the ceiling, straight down the middle of the
        // cell. It rakes the wall panels, throws the cot's frame across the deck, and
        // leaves the corners in the dark. Everything the player can see about the shape
        // of this room, this light is telling them.
        // Value: 2.30 @ range 7 in a 7x6x4 m cell put ~1.5 units of light on EVERY wall —
        // with auto-exposure then metering a now-dark scene and adding up to 2.2x on top,
        // the panels blew straight back to the white cardboard we were trying to kill.
        // The lesson from the rifthub tube, in reverse: this is not a lumens problem, it
        // is a REACH problem. A 4-foot fluorescent does not reach the far corner of a
        // room — so shorten the reach (7.0 -> 4.6) and take the intensity down with the
        // /PI cut the rest of the rig got (2.30 -> 1.25). The cot and the deck under the
        // fixture are hot; the corners fall off; the room finally has a gradient.
        // The flicker DEPTH goes UP (0.35 -> 0.55): with honest ambient underneath, a
        // dip actually reads as the room going dark, which is the horror beat the audio
        // has been selling on its own for months.
        // 2026-07-12 - Tim, live: "when Jake turns OFF his FLASHLIGHT the room goes
        // absolutely BLACK." The honest-light re-tune cut this tube to 1.25 @ reach 4.6
        // while a STRAY generic room light (3.2 @ range 8, dropped in by the level loader
        // because Jake's Cell has no room recipe) was quietly doing the room's actual
        // lighting - and scorching the ceiling it hung 0.25 m under. That stray light is
        // now erased (app_run.cpp, at the recipe-light swap), so THIS TUBE IS THE ROOM'S
        // LIGHT and it has to carry the room: 1.25 -> 3.30, reach 4.6 -> 6.2.
        //   * 3.30 puts a wall 2 m away at radiance ~0.64; against the honest 0.030
        //     ambient and the renormalized ~0.21-linear wall albedo that lands near the
        //     0.18 middle grey the auto-exposure meters for - a DIM, READABLE, moody room
        //     with no AE panic-lift to 2.2x (which is what was bleaching the walls).
        //   * reach 6.2 (not 4.6) because the cell is about to get BIGGER (the 13700K's
        //     cell-scale fix lands separately): a reach dialled to the walls of a 4 m box
        //     would leave a 7 m box black in the corners. 6.2 still falls off well before
        //     the far corners of either, so the gradient survives the rescale.
        // Flicker depth stays 0.55 - now that this tube really is the room's light, a dip
        // actually takes the room down, which is the horror beat.
        // 2026-07-12 MERGE (fold/intro-cockpit): VERIFIED against the landed cell-scale fix.
        // The reach above was set generous ON PURPOSE for exactly this rescale, and it holds:
        // shot at four player-eye cameras in the corrected 7x4x6 cell, flashlight OFF, the
        // room reads dim, moody and NAVIGABLE with a real gradient and ZERO pixels above 0.90
        // (it does not clip, and it does not go black). Left byte-identical at 3.30 / 6.2 —
        // do not "fix" this by eyeballing a darker screenshot: a camera derived for the OLD
        // 4 m cell lands INSIDE A WALL in the new one and renders a near-black frame that
        // looks exactly like a lighting regression and is not one. Derive cameras from room
        // data (docs/ENGINE_GOTCHAS.md 4.1).
        // 2026-07-12 — THE CELL WAS BLACK. Re-tuned FOR THE ROOM IT ACTUALLY LIVES IN.
        // The note above ("left byte-identical at 3.30 / 6.2 — verified against the landed
        // cell-scale fix") WAS WRONG, and it is worth saying why, because the reasoning is
        // seductive: the reach WAS set generous on purpose, and the window term really does
        // survive the rescale. But reach is not what was killing this room.
        // MEASURED on main @ f295caf, flashlight OFF, 4 data-derived eye cameras in the
        // 7x4x6 cell: mean luma 6.7-10.1, and 65-80% OF EVERY FRAME AT OR BELOW LUMA 6.
        // That is not "dim and moody". That is the void the player wakes up in.
        //
        // THE TWO INSTRUMENTS SETTLED IT (r_debugview, from fix/prim-point-light):
        //   view 2 (point-light term ALONE): mean 57.7, 2.6% void — THE LAMP REACHES.
        //     "The key dies in mid-air" was the wrong diagnosis. It arrives.
        //   view 5 (real lighting, albedo forced FLAT 0.5): mean 15.6 — a room made of
        //     50% reflectors STILL READS DARK under this rig.
        // A surface cannot be the fault when a WHITE room is dark. And the value check
        // (do it FIRST, always) exonerates the surfaces outright: hh_floor_01a is 0.462
        // linear x tint 0.40 = 0.185, hh_wall_01a is 0.505 x 0.34 = 0.172 — both sit in
        // the honest 0.18-0.20 band prim-point-light renormalized the graybox INTO. There
        // is no asphalt in this room. DO NOT "fix" this cell by lifting its albedos; they
        // are already right, and raising them just makes a grey room out of a dark one.
        //
        // So it is FLUX, and it is GEOMETRY — the two things the rescale actually changed:
        //   * The room went 4x3.5x4 -> 7x4x6. THREE TIMES THE VOLUME, 42 m^2 of floor, and
        //     still exactly ONE fixture. The rest of this building lights a corridor with a
        //     practical every few metres; the cell was asked to do it with one lamp tuned
        //     for a closet. Level1's fixtures run 3.2-3.3 EACH — and there are 337 of them.
        //   * The lamp hung 0.40 m under the ceiling. With pointAtten = w^2/(d^2+1), the
        //     CEILING 0.40 m away caught 0.86 of it and the FLOOR 3.6 m below caught 0.056.
        //     A 15:1 waste ratio — the fixture was lighting the slab above it. THAT is why
        //     the tube reads p95 233 while the deck under it reads p95 11.9: not a lamp that
        //     cannot reach, a lamp pointed at the wrong half of the room.
        // Raising intensity ALONE would have scorched the ceiling before the floor lit.
        // Raising RANGE alone could never have worked: the window term was already 0.79 at
        // the floor, so 6.2 -> infinity buys 1.26x. Neither dial fixes this on its own.
        //
        // HANG IT LOWER (0.40 -> 1.10 m below the ceiling: floor atten 0.056 -> 0.104, and
        // the ceiling's share drops 0.86 -> 0.45), LENGTHEN THE REACH to a 7 m room's far
        // corner (6.2 -> 9.0), and THEN give the one lamp the flux to carry 42 m^2 (x2.73).
        // The cool-white ratio (3.30:3.42:3.70) is preserved EXACTLY — this is the same
        // lamp, hung right and fed properly, not a new one. No ambient was raised. No
        // albedo went over unity. The flicker beat is untouched (depth 0.55) and now has a
        // real room to take down with it.
        // GAMMA-RECAL (fix/gamma-recal, 2026-07-25): x0.80 trim from 9.01/9.34/10.10.
        // The flux above was tuned against the BENT output curve (5951890b); under the
        // honest curve the cell measured mean 58-63 / p50 53-61 at four eye cameras,
        // flashlight OFF — above the room's own locked criteria band (dim, moody,
        // mean ~35-54, zero clipping; docs/screenshots/cell/). Cool-white ratio
        // preserved EXACTLY; hang height, reach and flicker untouched.
        const uint32_t li = (uint32_t)m_lights.size();
        addLight(bt.jakeCell, lx, ceilY - 1.10f, lz, 9.0f, 7.21f, 7.47f, 8.08f);
        m_flickers.push_back({ li, 7.21f, 7.47f, 8.08f, 0.0f, 9.0f, 0.55f });
        (void)aLight; (void)aHLight;   // loaded (hall may reuse); no cell placement
    }

    // R11 — CUT: the second "RED ALARM WASH" at the door. There is already a red at the
    // threshold three blocks up; two reds 20 cm apart is not a mood, it is a leak.

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
            addLight(bt.mainHall, tx + 0.6f, hfY + 1.3f, tz, 3.2f, 0.10f, 0.55f, 0.72f);
        }
        // Crates + a barrel stacked against the hall's -X wall (cover / clutter).
        place(aCrateL, 0.0f, 1.0f, cx(kCrateLAabb), kCrateLAabb.miny, cz(kCrateLAabb),
              hx0 + 0.8f, hfY + 0.02f, hz + 2.2f, nullptr, tCrate);
        place(aCrateS, 0.6f, 1.0f, cx(kCrateSAabb), kCrateSAabb.miny, cz(kCrateSAabb),
              hx0 + 0.8f, hfY + 0.62f, hz + 2.2f, nullptr, tCrate);
        // WAVE (barrels-universal): the hall clutter drum by the -X wall is now a REAL
        // explodable barrel when the host wires the sink (canon loop -> BarrelSystem). The
        // BarrelSystem owns the intact Barrel.glb + fracture + blast, so we DON'T also draw
        // a static barrel over it. No sink (tests) -> the static rusted drum, as before.
        // QA MAINLEVEL SWEEP (propclip lint): hz + 3.2 put the drum at z=47.7 — THROUGH
        // the hall's +Z wall (z1 = 47, the hall is only 5 m deep) and 0.6 m INTO the
        // Entrance room's corner. hz + 1.1 keeps it against the -X end wall beside the
        // crate stack, fully inside the hall (barrel radius 0.44 -> z 45.16..46.04).
        if (m_barrelSink) {
            m_barrelSink(hx0 + 0.9f, hfY, hz + 1.1f);
        } else {
            place(aBarrel, 0.0f, 1.0f, cx(kBarrelAabb), kBarrelAabb.miny, cz(kBarrelAabb),
                  hx0 + 0.9f, hfY + 0.02f, hz + 1.1f, nullptr, tBarrel);
        }
        // A red running light at the hall mouth (guard-corridor mood). R11 (playable-build):
        // 2.4 -> 1.10, the /PI cut — it was tuned against GLB props that shaded at 1/PI.
        // The BARREL above stays the explodable one (e7a2986); only the LIGHT takes the cut.
        addLight(bt.mainHall, hx0 + 1.2f, hCeil - 0.4f, hz, 5.0f, 1.10f, 0.055f, 0.03f);

        // ================= OPENING-ROUTE SCONCE RUN (corridor readability) =========
        // OBSERVED (live playtest + docs/screenshots/opening_flow baseline): the Main
        // Hall read as a BLACK TUBE — the recipe's ceiling row shows as a line of
        // bright dots down the lid, but their pools die before the walls/floor, and
        // the only other cue was the red mouth light ("red ceiling dots" corridor).
        // Doctrine fix (motivated lighting, NOT an ambient boost): a run of REAL wall
        // sconces down BOTH long walls of the hall — the same SciFi_Warehouse_Kit
        // fixture + warm pool the cell's door sconce proved, alternating sides every
        // ~6.5 m at head height, each carrying a local pool that overlaps the next.
        // Visible sources, pools of light, navigable without the flashlight; the
        // in-between stays moody. OPENING ROUTE ONLY: this dresses bt.mainHall (the
        // first corridor past the cell) — no other room's statement is touched.
        {
            const float hx1 = H.x1();
            const float emSconce[4] = { 1.0f, 0.86f, 0.62f, 1.0f };   // lit lens
            const float sconceY = hfY + 2.2f;                          // head height
            // Half-width of the fixture on the wall's run — what has to land on solid wall.
            const float sconceHalf = (kWLightAabb.maxx - kWLightAabb.minx) * 0.5f;
            m_mountedFirst = (uint32_t)m_instances.size();   // audit window start
            int side = 0, skipped = 0, slid = 0;
            for (float sx = hx0 + 3.0f; sx <= hx1 - 3.0f; sx += 6.5f, ++side) {
                const bool onMinZ = (side % 2) == 0;                  // alternate walls
                // THE MOUNTED-PROP LAW: this wall is pierced by the cell-block
                // doorways, and the blind 6.5 m pitch used to plant a fixture in
                // mid-air inside one of them (the "stray white delta-wing"). Slide
                // to the first solid span; skip the stop if the whole neighbourhood
                // is opening. Its POOL goes with it — a light with no visible source
                // is the other half of the same lie.
                const int   wallId = onMinZ ? 2 : 3;
                float       mx     = sx;
                if (!slideToSolidWall(bt.mainHall, wallId, mx, sconceHalf, 2.6f, 0.4f)) {
                    ++skipped;
                    continue;
                }
                if (std::fabs(mx - sx) > 1e-3f) ++slid;
                // Screen = local -Z -> world (sin yaw, 0, -cos yaw): face INTO the hall.
                const float yaw = onMinZ ? kPi : 0.0f;
                const float wz  = onMinZ ? (H.z0() + 0.10f) : (H.z1() - 0.10f);
                place(aWLight, yaw, 1.0f, cx(kWLightAabb), cy(kWLightAabb), kWLightAabb.maxz,
                      mx, sconceY, wz, emSconce, tSteel);
                // Its pool: warm tungsten, ranged so neighbouring pools OVERLAP on the
                // walls (6.5 m pitch / 6.5 m range) — a rhythm of pools, not a wash.
                const float lz = onMinZ ? (H.z0() + 0.55f) : (H.z1() - 0.55f);
                addLight(bt.mainHall, mx, sconceY, lz, 6.5f, 2.60f, 2.17f, 1.51f);
            }
            if (skipped || slid)
                x3::logInfo("[cell-dress] sconce run: " + std::to_string(slid) +
                            " slid off a doorway, " + std::to_string(skipped) +
                            " skipped (no solid wall within reach)");
            // ---- THE AUDIT. The law above is only worth what it can PROVE, so
            // re-test every sconce actually placed and report the count that ended
            // up spanning a doorway. It must be zero; a nonzero count is the exact
            // defect that shipped (a fixture hanging in an opening reading as a
            // stray white object) and it now says so, loudly, at every level build
            // instead of waiting for someone to photograph it.
            for (uint32_t si = m_mountedFirst; si < (uint32_t)m_instances.size(); ++si) {
                const Instance& in = m_instances[si];
                const bool minZ = std::fabs(in.transform[14] - (H.z0() + 0.10f)) < 0.02f;
                if (!minZ && std::fabs(in.transform[14] - (H.z1() - 0.10f)) >= 0.02f) continue;
                if (!wallSolidAt(bt.mainHall, minZ ? 2 : 3, in.transform[12], sconceHalf))
                    ++m_mountedInOpening;
                ++m_mountedAudited;
            }
            if (m_mountedInOpening > 0)
                x3::logError("[cell-dress] MOUNTED-PROP AUDIT FAILED: " +
                             std::to_string(m_mountedInOpening) + " of " +
                             std::to_string(m_mountedAudited) +
                             " wall sconces span a DOORWAY — a fixture bolted to no wall "
                             "renders as a stray object floating in the opening");
            else
                x3::logInfo("[cell-dress] mounted-prop audit: " +
                            std::to_string(m_mountedAudited) +
                            " wall sconces, 0 in a doorway");
        }
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
    // The REAL ARCHITECTURE first (tiled PBR sets — albedo/normal/mr), under everything
    // else. Drawn here rather than via SurfaceLibrary::drawPanel because the cell needs a
    // per-panel albedo tint (see SurfPanel::tint).
    {
        const float noEm[4] = { 0, 0, 0, 0 };
        for (const SurfPanel& p : m_surfPanels) {
            if (!p.set || !p.set->ok || !p.mesh.valid()) continue;
            device.drawMeshPBR(frame, p.mesh, p.set->albedo, p.set->normal, p.set->mr,
                               p.tint, noEm, p.transform, false, false,
                               x3::rhi::TextureHandle{}, x3::rhi::TextureHandle{},
                               1.0f, 0.0f, 0.0f);
        }
    }
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
            //
            // ...AND THAT GATE WAS A CRUTCH OVER A BROKEN CONVERT (THE PATTERN, again).
            // The reason kit emissive "bloomed as glowing blobs" is that the kit HAD NO
            // EMISSIVE MAP: the ModularSciFi conversion threw the pack's glow channel
            // (MRAG.a) away, so a piece could only ever carry a FLAT emissiveFactor —
            // and a flat factor with no map floods the WHOLE SURFACE. Gating that off
            // was right. But the gate keys on the wrong thing: it also zeroes the
            // emissive of a piece whose glow is a real PER-TEXEL MAP, which is
            // self-limiting by construction (it glows only where the artist painted a
            // lens, and is black everywhere else). With the kit's maps restored, this
            // gate was silently deleting every ceiling panel, door lens and wall strip
            // in the cell. PROVEN, not guessed: at emissive strength 12 the door was
            // still DARK.
            //
            // So gate on what actually separates the two cases:
            //   * a MAP localizes the glow  -> honour it at full strength.
            //   * a bare FACTOR floods it   -> still opt-in via inst.emissive[3].
            const bool matMap  = d.emissiveTexId != 0;
            const bool matEmis = matMap ||
                d.emissiveFactor[0] > 0.001f || d.emissiveFactor[1] > 0.001f || d.emissiveFactor[2] > 0.001f;
            float emis[4];
            if (matEmis) { emis[0]=d.emissiveFactor[0]; emis[1]=d.emissiveFactor[1]; emis[2]=d.emissiveFactor[2];
                           emis[3]= matMap ? 1.0f : inst.emissive[3]; }
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

void CellDressing::forEachPropInstance(
    const std::function<void(uint32_t, const std::string&,
                             const std::vector<x3::asset::ModelDrawable>&,
                             const float*)>& fn) const {
    for (const Instance& in : m_instances) {
        if (in.asset >= m_assetTable.size()) continue;
        const Asset& a = m_assetTable[in.asset];
        if (!a.ok) continue;
        fn(kNoRoom, m_assetPaths[in.asset], a.drawables, in.transform);
    }
}

} // namespace x3::game
