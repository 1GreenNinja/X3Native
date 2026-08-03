// EFLZ environment art overlay (EFLZ art pass). See app/env_art.h.
//
// Clean-room: built from the IModelLoader + IAssetSource + IRenderDevice + Scene
// interfaces only. No purchased C# / id Tech source consulted. Public glTF refs
// + the converted GLB catalog (CATALOG.md) only.
#include "env_art.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <unordered_set>

namespace x3::game {

namespace {

constexpr float kPi = 3.14159265358979f;

// ---- Converted-GLB relative paths (under the mounted converted_glb dir). ------
const char* kRelFloor     = "ModularSciFi_Interior/SM_Floor_A.glb";
const char* kRelCeiling   = "ModularSciFi_Interior/SM_Ceiling_A.glb";
const char* kRelWall      = "ModularSciFi_Interior/SM_Wall_A.glb";
const char* kRelDoorFrame = "ModularSciFi_Interior/SM_DoorFrame_A.glb";
const char* kRelConsole   = "ModularSciFi_Interior/SM_Console.glb";
const char* kRelLight     = "ModularSciFi_Interior/SM_Light_A.glb";
// ---- Detail props (D-content) so rooms read as distinct spaces, not empty boxes.
// All purely visual (no collision) — the graybox boxes remain the truth. Reused
// from the converted SciFi_Warehouse_Kit catalog. Per-asset fallback: a missing
// GLB simply isn't drawn (the room still works), exactly like the kit pieces. ----
const char* kRelCrateShort = "SciFi_Warehouse_Kit/Crate Short.glb";
const char* kRelCrateLong  = "SciFi_Warehouse_Kit/Crate Long.glb";
const char* kRelBarrel     = "SciFi_Warehouse_Kit/Barrel.glb";
const char* kRelPallet     = "SciFi_Warehouse_Kit/Pallet.glb";
const char* kRelFusebox    = "SciFi_Warehouse_Kit/Fusebox 01.glb";
const char* kRelPipes      = "ModularSciFi_Interior/SM_Pipes_A.glb";

// ---- Probed world-space AABBs of each kit piece (meters), AFTER the GLB node
// transforms are applied (matches the M2 node-TRS loader). Used to recenter each
// asset onto a target world placement. (Measured via trimesh; see CATALOG.md.)
//   Floor_A : X[-5.12,-1.12] Z[0,3]  Ytop~0.19   -> 4.0(X) x 3.0(Z) tile
//   Ceiling : X[-5.12,-1.12] Z[0,3]  Y[4.04,4.40]-> 4.0(X) x 3.0(Z) panel
//   Wall_A  : X[-1.43,0] Y[-0.04,4.40] Z[0,3]    -> 3.0 wide(Z) x 4.45 tall(Y), 1.43 thick(X)
//   DoorFrm : X[-6.25,0] Y[-0.04,4.40] Z[-0.28,0.28]
//   Console : X[-0.47,0.31] Y[0,1.55] Z[-0.31,0.29]
//   Light_A : X[-0.22,0] Y[0,0.03] Z[0,2.26]
// We store the center we want to map to the target and the floor (min-Y) value.
struct Aabb { float minx,miny,minz, maxx,maxy,maxz; };
constexpr Aabb kFloorAabb { -5.12f, -0.04f, 0.00f, -1.12f, 0.19f, 3.00f };
constexpr Aabb kCeilAabb  { -5.12f,  4.04f, 0.00f, -1.12f, 4.40f, 3.00f };
constexpr Aabb kWallAabb  { -1.43f, -0.04f, 0.00f,  0.00f, 4.40f, 3.00f };
constexpr Aabb kFrameAabb { -6.25f, -0.04f,-0.28f,  0.00f, 4.40f, 0.28f };
constexpr Aabb kConsAabb  { -0.47f,  0.00f,-0.31f,  0.31f, 1.55f, 0.29f };
constexpr Aabb kLightAabb { -0.22f,  0.00f, 0.00f,  0.00f, 0.03f, 2.26f };
// Detail props (probed via trimesh; Y-up, floor at min-Y ~ 0). See CATALOG.md.
//   Crate Short : 0.67(X) x 0.60(Y) x 0.67(Z)
//   Crate Long  : 0.64(X) x 0.60(Y) x 1.27(Z)
//   Barrel      : 0.88(X) x 1.23(Y) x 0.88(Z)
//   Pallet      : 1.56(X) x 0.20(Y) x 1.52(Z)
//   Fusebox 01  : 0.18(X) x 2.24(Y) x 0.64(Z) (a wall panel; sits low on the wall)
//   Pipes_A     : 0.66(X) x 0.18(Y) x 3.00(Z) (a ceiling/wall pipe run)
constexpr Aabb kCrateSAabb { -0.668f, 0.000f,-0.000f, 0.000f, 0.600f, 0.671f };
constexpr Aabb kCrateLAabb { -0.640f, 0.000f, 0.000f, 0.000f, 0.600f, 1.274f };
constexpr Aabb kBarrelAabb { -0.441f, 0.000f,-0.456f, 0.440f, 1.225f, 0.425f };
constexpr Aabb kPalletAabb { -1.559f, 0.000f,-0.005f, 0.003f, 0.198f, 1.519f };
constexpr Aabb kFuseAabb   { -0.329f,-0.936f,-0.245f,-0.144f, 1.308f, 0.396f };
constexpr Aabb kPipesAabb  { -0.123f,-0.011f,-0.000f, 0.539f, 0.164f, 3.000f };

inline float cx(const Aabb& a){ return (a.minx+a.maxx)*0.5f; }
inline float cy(const Aabb& a){ return (a.miny+a.maxy)*0.5f; }
inline float cz(const Aabb& a){ return (a.minz+a.maxz)*0.5f; }

// Identity 4x4 (column-major).
void ident(float m[16]) {
    for (int i=0;i<16;++i) m[i]=(i%5==0)?1.0f:0.0f;
}

// Column-major TRS with a yaw about +Y (radians) and uniform scale s, placing the
// transformed asset point that currently sits at (px,py,pz) at world (wx,wy,wz).
// i.e. world = T(w) * R_y(yaw) * S(s) * T(-p). This recenters an asset (whose
// chosen anchor point is p) onto the world anchor w, optionally yawed + scaled.
//
// FACING CONVENTION (see docs/CONVENTIONS.md §1 + §3):
//   A model's default facing is its local -Z (glTF convention). Under this
//   rotation, local -Z maps to world ( -sin(yaw), 0, -cos(yaw) ). Therefore:
//     yaw = 0      -> faces world -Z  (forward / into the scene)   <-- default
//     yaw = +pi/2  -> faces world -X  (left)
//     yaw = +pi    -> faces world +Z  (toward viewer)              [NOT -Z!]
//     yaw = -pi/2  -> faces world +X  (right)
//   This local "yaw" is offset by +pi/2 from the §3 camera/AI yaw (whose 0 = +X):
//     placeYaw_yaw = yaw_AI + pi/2,  where yaw_AI = atan2(dz, dx).
//   So to face the §3 world-forward (-Z): yaw_AI = -pi/2 -> placeYaw_yaw = 0.
void placeYaw(float m[16], float yaw, float s,
              float px, float py, float pz,
              float wx, float wy, float wz) {
    const float c = std::cos(yaw), sn = std::sin(yaw);
    // R*S basis columns (R about Y): col0=(c,0,-sn), col1=(0,1,0), col2=(sn,0,c), all *s.
    m[0]=c*s;  m[1]=0;   m[2]=-sn*s; m[3]=0;
    m[4]=0;    m[5]=s;   m[6]=0;     m[7]=0;
    m[8]=sn*s; m[9]=0;   m[10]=c*s;  m[11]=0;
    // translation = w - (R*S)*p
    const float rpx = (c*px + sn*pz) * s;
    const float rpy = (py) * s;
    const float rpz = (-sn*px + c*pz) * s;
    m[12]=wx - rpx; m[13]=wy - rpy; m[14]=wz - rpz; m[15]=1.0f;
}

// Lowercase copy (ASCII only — matches namedBounds()'s ad-hoc loop).
inline std::string toLowerCopy(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return s;
}

} // namespace

void EnvArtSystem::setNodeSkip(std::vector<std::string> subs) {
    m_nodeSkip.clear();
    for (std::string& s : subs) {
        std::string ls = toLowerCopy(std::move(s));
        if (!ls.empty()) m_nodeSkip.push_back(std::move(ls));
    }
}

uint32_t EnvArtSystem::loadAsset(const std::string& relPath) {
    // Cache: one upload per unique kit piece.
    for (uint32_t i=0;i<m_assetPaths.size();++i)
        if (m_assetPaths[i]==relPath) return i;

    EnvAsset a;
    a.model = m_loader->load(relPath);
    if (a.model.ok && !m_nodeSkip.empty()) {
        // Drop primitives belonging to a skipped NODE (by name) or a skipped
        // MATERIAL (by name) BEFORE makeDrawables() ever sees them — a dropped
        // primitive never becomes a drawable, so it never draws. load() always
        // hands back a private deep-copied Model (the process-wide model cache
        // stores its own template), so mutating a.model.primitives here is safe
        // and never corrupts a later loadAsset() of the same path in another
        // EnvArtSystem instance.
        std::unordered_set<int> skipMesh;
        for (const auto& nd : a.model.nodes) {
            if (nd.meshIndex < 0 || nd.name.empty()) continue;
            const std::string ln = toLowerCopy(nd.name);
            for (const std::string& s : m_nodeSkip)
                if (ln.find(s) != std::string::npos) { skipMesh.insert(nd.meshIndex); break; }
        }
        const size_t before = a.model.primitives.size();
        a.model.primitives.erase(
            std::remove_if(a.model.primitives.begin(), a.model.primitives.end(),
                [&](const x3::asset::MeshPrimitive& p) {
                    if (skipMesh.count((int)p.meshIndex)) return true;
                    if (p.materialIndex < a.model.materials.size()) {
                        const std::string& mn = a.model.materials[p.materialIndex].name;
                        if (!mn.empty()) {
                            const std::string ln = toLowerCopy(mn);
                            for (const std::string& s : m_nodeSkip)
                                if (ln.find(s) != std::string::npos) return true;
                        }
                    }
                    return false;
                }),
            a.model.primitives.end());
        if (a.model.primitives.size() != before)
            x3::logInfo("[env-art] node/material skip on " + relPath + ": " +
                        std::to_string(before) + " -> " +
                        std::to_string(a.model.primitives.size()) + " primitives");
    }
    if (a.model.ok) {
        // makeDrawablesNamed == makeDrawables + the source node NAME per drawable
        // (same order/length). The names cost nothing and let densifyFoliage() find
        // the foliage drawables without re-walking the node hierarchy.
        a.drawables = x3::asset::makeDrawablesNamed(a.model, a.drawableNames);
        a.ok = !a.drawables.empty();
    }
    if (a.ok) {
        uint32_t nTex = 0, nMr = 0, nNrm = 0;
        for (const auto& d : a.drawables) { if (d.baseColorTexId) ++nTex; if (d.mrTexId) ++nMr; if (d.normalTexId) ++nNrm; }
        float f0 = a.model.materials.empty() ? -1.0f : a.model.materials[0].baseColor[0];
        x3::logInfo("[env-art] loaded " + relPath + " — " + std::to_string(a.drawables.size()) +
                    " drawables; baseColorTex=" + std::to_string(nTex) + " mr=" + std::to_string(nMr) +
                    " normal=" + std::to_string(nNrm) + "; materials=" + std::to_string(a.model.materials.size()) +
                    " mat0.baseColor.r=" + std::to_string(f0));
    }
    else
        x3::logWarn("[env-art] FAILED to load " + relPath + " (graybox fallback kept)");

    uint32_t idx = (uint32_t)m_assetTable.size();
    m_assetTable.push_back(std::move(a));
    m_assetPaths.push_back(relPath);
    return idx;
}

void EnvArtSystem::addInstance(uint32_t a, const float transform[16]) {
    if (a >= m_assetTable.size() || !m_assetTable[a].ok) return; // skip failed assets
    EnvInstance e; e.asset = a;
    for (int i=0;i<16;++i) e.transform[i]=transform[i];
    m_instances.push_back(e);
}

void EnvArtSystem::addInstanceEmissive(uint32_t a, const float transform[16], const float emissive[4]) {
    if (a >= m_assetTable.size() || !m_assetTable[a].ok) return; // skip failed assets
    EnvInstance e; e.asset = a;
    for (int i=0;i<16;++i) e.transform[i]=transform[i];
    if (emissive) for (int i=0;i<4;++i) e.emissive[i]=emissive[i];
    m_instances.push_back(e);
}

bool EnvArtSystem::buildFromGlb(x3::rhi::IRenderDevice& device,
                                std::string_view convertedGlbDir, std::string_view relPath) {
    return buildFromGlbAt(device, convertedGlbDir, relPath, nullptr);
}

bool EnvArtSystem::buildFromGlbAt(x3::rhi::IRenderDevice& device,
                                  std::string_view convertedGlbDir, std::string_view relPath,
                                  const float transform[16]) {
    m_assets.reset(x3::asset::createAssetSource());
    if (!m_assets->mountDir(convertedGlbDir, 0)) {
        x3::logWarn("[env-art] buildFromGlb mountDir failed: " + std::string(convertedGlbDir));
        return false;
    }
    m_loader.reset(x3::asset::createModelLoader(&device, m_assets.get()));
    const uint32_t a = loadAsset(std::string(relPath));   // cgltf loads geometry + materials + embedded textures
    if (a >= m_assetTable.size() || !m_assetTable[a].ok) return false;
    const float I[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    addInstance(a, transform ? transform : I);   // identity: the GLB's baked node transforms ARE the world placement
    x3::logInfo("[env-art] buildFromGlb: " + std::string(relPath) + " — " +
                std::to_string(m_assetTable[a].drawables.size()) + " drawables, 1 instance");
    return true;
}

uint32_t EnvArtSystem::densifyFoliage(const std::vector<std::string>& nameSubs, uint32_t addCount,
                                      uint32_t seed, float minR, float maxR,
                                      float scaleMin, float scaleMax, float sink,
                                      const float* keepOutXZR) {
    if (addCount == 0 || m_instances.empty()) return 0;
    const uint32_t assetIdx = m_instances[0].asset;
    if (assetIdx >= m_assetTable.size() || !m_assetTable[assetIdx].ok) return 0;
    const EnvAsset& a = m_assetTable[assetIdx];
    if (a.drawableNames.size() != a.drawables.size()) return 0;

    // Source pool: every drawable whose node name matches (lowercased substring).
    std::vector<uint32_t> pool;
    for (uint32_t i = 0; i < a.drawables.size(); ++i) {
        std::string ln = a.drawableNames[i];
        for (char& c : ln) c = (char)std::tolower((unsigned char)c);
        for (const std::string& s : nameSubs) {
            std::string ls = s;
            for (char& c : ls) c = (char)std::tolower((unsigned char)c);
            if (!ls.empty() && ln.find(ls) != std::string::npos) { pool.push_back(i); break; }
        }
    }
    if (pool.empty()) {
        x3::logWarn("[env-art] densifyFoliage: no drawable matched — forest unchanged");
        return 0;
    }

    // Deterministic xorshift (no <random> ordering surprises across builds).
    uint32_t rng = seed ? seed : 1u;
    auto nextf = [&rng]() {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return (float)(rng & 0xFFFFFFu) / (float)0x1000000u;   // [0,1)
    };

    uint32_t added = 0;
    for (uint32_t k = 0; k < addCount; ++k) {
        const uint32_t src = pool[(uint32_t)(nextf() * (float)pool.size()) % (uint32_t)pool.size()];
        const float* N = a.drawables[src].nodeTransform;   // column-major

        // Source scale = column lengths (the pack's trees are uniformly scaled).
        const float sx = std::sqrt(N[0]*N[0] + N[1]*N[1] + N[2]*N[2]);
        const float sy = std::sqrt(N[4]*N[4] + N[5]*N[5] + N[6]*N[6]);
        const float sz = std::sqrt(N[8]*N[8] + N[9]*N[9] + N[10]*N[10]);
        if (sx < 1e-5f || sy < 1e-5f || sz < 1e-5f) continue;

        const float ang  = nextf() * 6.2831853f;                 // scatter direction
        const float rad  = minR + nextf() * std::max(0.0f, maxR - minR);
        const float yaw  = nextf() * 6.2831853f;                 // fresh facing (breaks the copy read)
        const float k2   = scaleMin + nextf() * std::max(0.0f, scaleMax - scaleMin);
        const float c = std::cos(yaw), s = std::sin(yaw);

        const float px = N[12] + std::cos(ang) * rad;
        const float pz = N[14] + std::sin(ang) * rad;
        if (keepOutXZR) {   // never grow the forest onto the hero building's apron
            const float dx = px - keepOutXZR[0], dz = pz - keepOutXZR[1];
            if (dx * dx + dz * dz < keepOutXZR[2] * keepOutXZR[2]) continue;
        }

        ScatterDraw sd{};
        sd.asset = assetIdx; sd.drawable = src;
        // World = T(sourceOrigin + offset) * Ry(yaw) * S(sourceScale * k2). The source's
        // own rotation is dropped on purpose: these are radially symmetric conifer
        // billboards, and a fresh yaw is exactly the variation we want.
        sd.transform[0]  =  c * sx * k2; sd.transform[1] = 0.0f; sd.transform[2]  = -s * sx * k2; sd.transform[3]  = 0.0f;
        sd.transform[4]  =  0.0f;        sd.transform[5] = sy * k2; sd.transform[6] = 0.0f;       sd.transform[7]  = 0.0f;
        sd.transform[8]  =  s * sz * k2; sd.transform[9] = 0.0f; sd.transform[10] =  c * sz * k2; sd.transform[11] = 0.0f;
        sd.transform[12] = px;
        sd.transform[13] = N[13] - sink;
        sd.transform[14] = pz;
        sd.transform[15] = 1.0f;
        m_scatter.push_back(sd);
        ++added;
    }
    x3::logInfo("[env-art] densifyFoliage: +" + std::to_string(added) + " clones from " +
                std::to_string(pool.size()) + " source drawables");
    return added;
}

void EnvArtSystem::setInstanceTransform(uint32_t idx, const float transform[16]) {
    if (idx >= m_instances.size() || !transform) return;
    for (int i = 0; i < 16; ++i) m_instances[idx].transform[i] = transform[i];
}

bool EnvArtSystem::beginFromDir(x3::rhi::IRenderDevice& device, std::string_view glbDir) {
    m_assets.reset(x3::asset::createAssetSource());
    if (!m_assets->mountDir(glbDir, 0)) {
        x3::logWarn("[env-art] beginFromDir mountDir failed: " + std::string(glbDir));
        return false;
    }
    m_loader.reset(x3::asset::createModelLoader(&device, m_assets.get()));
    return true;
}

bool EnvArtSystem::addGlbInstance(std::string_view relPath, const float transform[16]) {
    if (!m_loader) return false;
    const uint32_t a = loadAsset(std::string(relPath));   // cached by path
    if (a >= m_assetTable.size() || !m_assetTable[a].ok) return false;
    addInstance(a, transform);
    return true;
}

Level1ArtMask EnvArtSystem::build(x3::rhi::IRenderDevice& device,
                                  std::string_view convertedGlbDir,
                                  const Level1Layout& layout) {
    Level1ArtMask mask; // all false until art actually loads

    m_assets.reset(x3::asset::createAssetSource());
    if (!m_assets->mountDir(convertedGlbDir, 0)) {
        x3::logWarn("[env-art] mountDir failed: " + std::string(convertedGlbDir) +
                    " — keeping full graybox");
        return mask;
    }
    m_loader.reset(x3::asset::createModelLoader(&device, m_assets.get()));

    const uint32_t floorA = loadAsset(kRelFloor);
    const uint32_t ceilA  = loadAsset(kRelCeiling);
    const uint32_t wallA  = loadAsset(kRelWall);
    const uint32_t frameA = loadAsset(kRelDoorFrame);
    const uint32_t consA  = loadAsset(kRelConsole);
    const uint32_t lightA = loadAsset(kRelLight);
    // Detail props (D-content). Each may fail to load independently.
    const uint32_t crateS = loadAsset(kRelCrateShort);
    const uint32_t crateL = loadAsset(kRelCrateLong);
    const uint32_t barrel = loadAsset(kRelBarrel);
    const uint32_t pallet = loadAsset(kRelPallet);
    const uint32_t fuse   = loadAsset(kRelFusebox);
    const uint32_t pipes  = loadAsset(kRelPipes);

    const bool haveFloor = m_assetTable[floorA].ok;
    const bool haveCeil  = m_assetTable[ceilA].ok;
    const bool haveWall  = m_assetTable[wallA].ok;
    const bool haveFrame = m_assetTable[frameA].ok;
    const bool haveCons  = m_assetTable[consA].ok;
    const bool haveLight = m_assetTable[lightA].ok;

    // ---- Floor footprints + base Y + RAISED ceiling heights come from level1.cpp's
    // shared canonical table (level1Rooms()), so the GLB floor/wall/ceiling/light
    // tiling matches the collision geometry EXACTLY (bounds, base height AND per-
    // floor ceiling). The Spire is a vertical stack: each floor plate sits at y0 and
    // its ceiling at y0+ceil, so the art tiles up the whole tower. ----
    struct Room { float x0, x1, zHalf, ceil, y0; };
    Room rooms[(uint32_t)L1Floor::Count];
    {
        const L1RoomDef* tbl = level1Rooms();
        for (uint32_t i = 0; i < (uint32_t)L1Floor::Count; ++i)
            rooms[i] = Room{ tbl[i].x0, tbl[i].x1, tbl[i].zHalf, tbl[i].ceil, tbl[i].y0 };
    }

    float m[16];

    // ---- FLOOR + CEILING: tile the 4.0(X) x 3.0(Z) panels across each room. ----
    // The floor tile's top sits at its AABB max-Y; anchor that to world y=0. The
    // tile is anchored at its XZ center -> world tile center. We tile a grid that
    // covers each room (last row/col may overhang slightly into walls — hidden).
    // THE STAIR-WELL SKIP (QA upper-floors sweep, D19). The emergency stairwell is a
    // shaft cut clean through every floor slab and ceiling lid it passes. Tiling GLB
    // panels over that opening would paint a solid-looking floor across a hole with no
    // collision under it — art you walk onto and fall through. Skip every tile the well
    // touches; buildLevel1 lays a graybox apron over exactly the same tiles (minus the
    // well) so the skip leaves no void ring. ONE shared rect: spireWellTileSpan().
    const x3::game::SpireStair& stairWell = x3::game::spireStair();
    auto tileSurface = [&](uint32_t asset, const Aabb& ab, bool ceiling) {
        if (asset >= m_assetTable.size() || !m_assetTable[asset].ok) return;
        const float tileX = ab.maxx - ab.minx;   // 4.0
        const float tileZ = ab.maxz - ab.minz;   // 3.0
        const float ax = cx(ab), az = cz(ab);
        const float anchorY = ceiling ? ab.miny : ab.maxy;  // ceiling: bottom; floor: top
        for (uint32_t fi = 0; fi < (uint32_t)x3::game::L1Floor::Count; ++fi) {
            const Room& r = rooms[fi];
            // The B1 slab is the BOTTOM of the well (never cut); F7 has no lid.
            const bool wellCut = ceiling ? (fi != (uint32_t)x3::game::L1Floor::F7)
                                         : (fi != (uint32_t)x3::game::L1Floor::B1);
            const int nx = (int)std::ceil((r.x1 - r.x0) / tileX);
            const int nz = (int)std::ceil((2.0f * r.zHalf) / tileZ);
            for (int ix=0; ix<nx; ++ix) {
                for (int iz=0; iz<nz; ++iz) {
                    const float wxC = r.x0 + (ix + 0.5f) * tileX;
                    const float wzC = -r.zHalf + (iz + 0.5f) * tileZ;
                    if (wellCut &&
                        wxC + tileX * 0.5f > stairWell.wellX0 + 0.01f &&
                        wxC - tileX * 0.5f < stairWell.wellX1 - 0.01f &&
                        wzC + tileZ * 0.5f > stairWell.wellZ0 + 0.01f &&
                        wzC - tileZ * 0.5f < stairWell.wellZ1 - 0.01f)
                        continue;                       // this tile lands on the shaft
                    // Floor at this floor's base y0; ceiling at y0+ceil (the Spire
                    // is a vertical stack, so the panels follow each plate's height).
                    const float wyC = ceiling ? (r.y0 + r.ceil) : r.y0;
                    // anchor: (ax, anchorY, az) -> (wxC, wyC, wzC), no yaw/scale.
                    placeYaw(m, 0.0f, 1.0f, ax, anchorY, az, wxC, wyC, wzC);
                    addInstance(asset, m);
                }
            }
        }
    };
    // The overlay tile footprint must match the shared art/graybox contract, or the
    // stair-well skip and the graybox apron would disagree and leave a seam.
    static_assert(x3::game::kSpireArtTileX == 4.0f && x3::game::kSpireArtTileZ == 3.0f,
                  "env_art surface tile is 4 x 3 m — keep kSpireArtTile* in level1.h in step");
    if (haveFloor) { tileSurface(floorA, kFloorAabb, /*ceiling*/false); mask.floors = true; }
    if (haveCeil)  { tileSurface(ceilA,  kCeilAabb,  /*ceiling*/true ); mask.ceilings = true; }

    // ---- WALLS: the panel is 3.0 wide (its local +Z) x 4.45 tall (Y), 1.43 thick
    // (its local X). For the long side walls (run along world X) we yaw +90deg so
    // the 3.0 axis runs along X; the thick X axis then points along Z (into the
    // wall line at z=±zHalf). For the cross end-caps (run along Z) we use yaw 0.
    // Anchor each panel at the floor (min-Y) at its width-center + thickness face. -
    if (haveWall) {
        const float panelW   = kWallAabb.maxz - kWallAabb.minz;  // 3.0 (local Z)
        const float wallMinY = kWallAabb.miny;                   // -0.04 -> floor
        const float panelH   = kWallAabb.maxy - kWallAabb.miny;  // ~4.45 (local Y)
        const float wAnchorX = cx(kWallAabb);                    // local X center
        const float wAnchorZ = cz(kWallAabb);                    // local Z center (width)
        // Side walls: tile along X at z = ±zHalf, yaw +90deg (local +Z -> world +X).
        // STACK rows of the 4.45 m panel up to each room's (raised) ceiling so the
        // taller rooms (arena 8 m, elevator 9 m) have no gap above a single panel.
        // Row r's panel bottom sits at y = r*panelH; we add a row while its bottom
        // is still below the ceiling (the topmost row overhangs into the ceiling
        // panel above — hidden, and the collision lid seals it).
        for (const Room& r : rooms) {
            const int n = (int)std::ceil((r.x1 - r.x0) / panelW);
            const int rows = (int)std::ceil(r.ceil / panelH);   // 1 row for <=4.45 m
            for (int row=0; row<rows; ++row) {
                const float rowBaseY = r.y0 + (float)row * panelH; // this row's floor (plate base)
                for (int s=0; s<2; ++s) {
                    const float wz = (s==0) ? -r.zHalf : r.zHalf;
                    // Opposite facing per side so the visible (front) face points INTO the
                    // room on BOTH walls. Single-sided panels were back-culled on the +Z
                    // side (same yaw both sides) -> see-through. +90 for -Z wall, -90 for +Z.
                    const float wYaw = (s==0) ? kPi*0.5f : -kPi*0.5f;
                    for (int i=0; i<n; ++i) {
                        const float wx = r.x0 + (i + 0.5f) * panelW;
                        placeYaw(m, wYaw, 1.0f, wAnchorX, wallMinY, wAnchorZ, wx, rowBaseY, wz);
                        addInstance(wallA, m);
                    }
                }
            }
        }
        mask.walls = true;
    }

    // ---- DOOR FRAMES: drop a frame at the elevator-shaft doorway of every floor
    // (the meaningful per-floor door up the tower) PLUS the B1 spine doors A-E (the
    // Awakening beats). The frame is 6.25 wide; we scale it to ~2.0 m so it reads as
    // a single doorway header straddling the 1.2 m opening. The doorway is in a
    // cross-wall (plane x=const), so the frame's wide axis (local X) runs along Z ->
    // yaw +90deg. Anchored at the floor's base Y (d.y carries y0). -------
    if (haveFrame) {
        const float fs = 2.0f / (kFrameAabb.maxx - kFrameAabb.minx);
        auto frameAt = [&](const x3::phys::Vec3& d) {
            placeYaw(m, kPi*0.5f, fs, cx(kFrameAabb), kFrameAabb.miny, cz(kFrameAabb),
                     d.x, d.y, d.z);
            addInstance(frameA, m);
        };
        for (uint32_t fi = 0; fi < (uint32_t)L1Floor::Count; ++fi)
            frameAt(layout.elevatorDoor[fi]);            // shaft doorway per floor
        const x3::phys::Vec3 b1doors[5] = {
            layout.doorA, layout.doorB, layout.doorC, layout.doorD, layout.doorE };
        for (const auto& d : b1doors) frameAt(d);        // B1 spine doors A-E
    }

    // ---- CONSOLE: terminals beside the B1 Door A / Door B (the Awakening button
    // doors) + the strength terminal beat. Set against the +Z wall so they read as
    // wall terminals. Purely visual. ---
    if (haveCons) {
        const x3::phys::Vec3 consoles[2] = { layout.doorA, layout.doorB };
        for (const auto& d : consoles) {
            // Place near the doorway, offset to the +Z side wall (z = d.z + 1.4),
            // facing the console screen back into the room toward -Z (placeYaw yaw=0).
            placeYaw(m, 0.0f, 1.0f, cx(kConsAabb), kConsAabb.miny, cz(kConsAabb),
                     d.x - 0.6f, d.y, d.z + 1.4f);
            addInstance(consA, m);
        }
    }

    // ---- DETAIL PROPS (D-content): dress each FLOOR with crates / barrels /
    // pallets / a wall fusebox / pipe runs so the wings read as distinct spaces
    // instead of empty boxes. PURELY VISUAL (no collision) — placed clear of the
    // z=0 walk spine + the elevator doorway so they never block the player. Each
    // prop is anchored at its floor's base Y onto the target world XZ, optionally
    // yawed. A failed asset is silently skipped. The Spire is a vertical stack, so
    // every prop carries the floor's y0. ----
    auto placeProp = [&](uint32_t asset, const Aabb& ab, float yaw,
                         float wx, float baseY, float wz, float scale = 1.0f) {
        if (asset >= m_assetTable.size() || !m_assetTable[asset].ok) return;
        placeYaw(m, yaw, scale, cx(ab), ab.miny, cz(ab), wx, baseY, wz);
        addInstance(asset, m);
    };
    // Wall-panel prop (fusebox): anchored on a side wall, raised so it reads as a
    // mounted panel (its AABB dips below 0, so lift it ~1 m onto the wall + base Y).
    auto placeWallPanel = [&](uint32_t asset, const Aabb& ab, float yaw,
                              float wx, float wy, float wz) {
        if (asset >= m_assetTable.size() || !m_assetTable[asset].ok) return;
        placeYaw(m, yaw, 1.0f, cx(ab), cy(ab), cz(ab), wx, wy, wz);
        addInstance(asset, m);
    };
    // Per-floor dressing. A light, repeating kit so each plate reads as a real
    // interior; the exact placements differ a touch by floor so they don't feel
    // copy-pasted, but all stay off the z=0 spine and the +X elevator doorway.
    for (uint32_t fi = 0; fi < (uint32_t)L1Floor::Count; ++fi) {
        const Room& r = rooms[fi];
        const float y0 = r.y0;
        const float zH = r.zHalf;
        // Crates + a pallet clustered along the -Z wall, mid-plate.
        placeProp(pallet, kPalletAabb, 0.0f,     6.0f + (float)(fi % 3) * 2.0f, y0, -zH + 1.1f);
        placeProp(crateL, kCrateLAabb, kPi*0.5f, 6.0f, y0, -zH + 1.0f);
        placeProp(crateS, kCrateSAabb, 0.4f,     7.0f, y0, -zH + 1.9f);
        // Barrels + a crate along the +Z wall.
        placeProp(barrel, kBarrelAabb, 0.0f,     12.0f, y0,  zH - 1.0f);
        placeProp(barrel, kBarrelAabb, 0.0f,     13.0f, y0,  zH - 1.6f);
        placeProp(crateS, kCrateSAabb, 0.7f,     17.0f, y0,  zH - 1.1f);
        // Pipe runs along both side walls + a wall fusebox (service look).
        placeProp(pipes,  kPipesAabb,  0.0f,     9.0f,  y0, -zH + 0.3f);
        placeProp(pipes,  kPipesAabb,  0.0f,     15.0f, y0,  zH - 0.3f);
        placeWallPanel(fuse, kFuseAabb, kPi*0.5f, 11.0f, y0 + 1.6f, -zH + 0.15f);
    }

    // ---- WAVE-2B: SPIRE FLOOR IDENTITY (LD review #1). The F3..F7 spire floors were
    // pixel-identical dark corridors — a player could not tell F3 from F7 and the Act-1
    // climax had no escalation. Give each floor ONE distinct LIT LANDMARK on the
    // elevator-arrival sightline (the capture cam stands at x~=x1-6 and looks -X across
    // the plate; the landmark sits at x~5, centre of frame), each in its zone-accent hue
    // from the colour ladder (docs/design/TEXTURE_DESIGN_STRATEGY §1.2):
    //   F3 Genetics  GREEN vat gallery · F4 Cybernetics CYAN server canyon ·
    //   F5 Drone     AMBER hero pad    · F6 Alien TEAL portal ring ·
    //   F7 Executive BRASS boardroom band.
    // The landmark is EMISSIVE geometry (dark albedo + a texture-of-one instance-emissive
    // accent, ACES-safe: one channel dominates so it blooms its HUE, never white-clips)
    // so it reads in-game AND in --capture-spire (whose dev light-rig would wash out any
    // point light we add, but cannot touch per-instance emissive). A matching per-floor
    // accent POINT LIGHT is also registered for the live game (the capture re-issues its
    // own rig, so these only shape the runtime floor — exactly the wayfinding the LD asked
    // for). Distinct silhouette + distinct hue = a one-second "where am I" read. ----
    {
        // placeProp's emissive twin: same recentre-onto-world transform, glows the accent.
        auto placeGlow = [&](uint32_t asset, const Aabb& ab, float yaw,
                             float wx, float baseY, float wz, float scale, const float em[4]) {
            if (asset >= m_assetTable.size() || !m_assetTable[asset].ok) return;
            placeYaw(m, yaw, scale, cx(ab), ab.miny, cz(ab), wx, baseY, wz);
            addInstanceEmissive(asset, m, em);
        };
        // Register a runtime accent point light near a landmark (colour = linear rgb *
        // intensity, matching env_art's fixture convention). Capture overrides these.
        auto accentLight = [&](float wx, float wy, float wz, float range,
                               float r, float g, float b, float intensity) {
            x3::rhi::PointLight pl;
            pl.pos[0]=wx; pl.pos[1]=wy; pl.pos[2]=wz; pl.range=range;
            pl.color[0]=r*intensity; pl.color[1]=g*intensity; pl.color[2]=b*intensity;
            m_lightFixtures.push_back(pl);
        };
        // Accent hues (rgb) per the ladder; emissive strength 2.6 blooms the hue.
        struct Accent { float r, g, b; };
        auto emiss = [](const Accent& a, float s, float out[4]) {
            out[0]=a.r; out[1]=a.g; out[2]=a.b; out[3]=s;
        };
        for (uint32_t fi = (uint32_t)L1Floor::F3; fi <= (uint32_t)L1Floor::F7; ++fi) {
            const Room& r  = rooms[fi];
            const float y0 = r.y0;
            const float lz = y0 + 1.6f;   // accent light hangs ~waist-high over the landmark
            float em[4];
            switch ((L1Floor)fi) {
                case L1Floor::F3: {   // GENETICS — green vat gallery: a row of 3 tall drums
                    const Accent a{ 0.22f, 1.05f, 0.45f };
                    emiss(a, 2.6f, em);
                    placeGlow(barrel, kBarrelAabb, 0.0f, 5.0f, y0, -3.2f, 1.8f, em);
                    placeGlow(barrel, kBarrelAabb, 0.0f, 5.0f, y0,  0.0f, 2.0f, em);
                    placeGlow(barrel, kBarrelAabb, 0.0f, 5.0f, y0,  3.2f, 1.8f, em);
                    accentLight(5.0f, lz, 0.0f, 10.0f, a.r, a.g, a.b, 2.6f);
                    break;
                }
                case L1Floor::F4: {   // CYBERNETICS — cyan server canyon: two flanking stacks
                    const Accent a{ 0.22f, 0.85f, 1.05f };
                    emiss(a, 2.6f, em);
                    placeGlow(crateL, kCrateLAabb, kPi*0.5f, 5.0f, y0,        -3.6f, 1.5f, em);
                    placeGlow(crateL, kCrateLAabb, kPi*0.5f, 5.0f, y0 + 0.90f,-3.6f, 1.5f, em);
                    placeGlow(crateL, kCrateLAabb, kPi*0.5f, 5.0f, y0,         3.6f, 1.5f, em);
                    placeGlow(crateL, kCrateLAabb, kPi*0.5f, 5.0f, y0 + 0.90f, 3.6f, 1.5f, em);
                    accentLight(5.0f, lz, 0.0f, 11.0f, a.r, a.g, a.b, 2.6f);
                    break;
                }
                case L1Floor::F5: {   // DRONE — amber hero pad: a wide pallet + a beacon drum
                    const Accent a{ 1.05f, 0.60f, 0.16f };
                    emiss(a, 2.6f, em);
                    placeGlow(pallet, kPalletAabb, 0.0f, 5.0f, y0,        0.0f, 3.0f, em);
                    placeGlow(barrel, kBarrelAabb, 0.0f, 5.0f, y0 + 0.55f, 0.0f, 2.0f, em);
                    accentLight(5.0f, y0 + 2.4f, 0.0f, 12.0f, a.r, a.g, a.b, 2.8f);
                    break;
                }
                case L1Floor::F6: {   // ALIEN — teal portal: a shallow arc of 5 drums
                    const Accent a{ 0.15f, 1.00f, 0.90f };
                    emiss(a, 2.6f, em);
                    for (int k = 0; k < 5; ++k) {
                        const float zz = -4.0f + (float)k * 2.0f;
                        const float xx = 4.0f + std::fabs(zz) * 0.35f;   // arc bows toward +X at the ends
                        placeGlow(barrel, kBarrelAabb, 0.0f, xx, y0, zz, 1.3f, em);
                    }
                    accentLight(4.0f, lz, 0.0f, 12.0f, a.r, a.g, a.b, 2.6f);
                    break;
                }
                case L1Floor::F7: {   // EXECUTIVE — brass boardroom band: a low row of 4 crates
                    const Accent a{ 1.05f, 0.80f, 0.40f };
                    emiss(a, 2.6f, em);
                    for (int k = 0; k < 4; ++k)
                        placeGlow(crateL, kCrateLAabb, kPi*0.5f, 4.0f, y0, -4.5f + (float)k * 3.0f, 1.6f, em);
                    accentLight(4.0f, lz, 0.0f, 13.0f, a.r, a.g, a.b, 2.4f);
                    break;
                }
                default: break;
            }
        }
    }

    // ---- CEILING LIGHTS: fixtures hung from EACH room's (now raised) ceiling. ----
    // Each placed Light_A fixture also registers a forward POINT LIGHT (captured in
    // m_lightFixtures) so the rooms are actually lit, not just decorated with dark
    // fixture meshes. The light emits from just below the fixture (so the ceiling
    // doesn't occlude it). Warm-white, color premultiplied by an intensity so a
    // fixture reads as a bright pool of light with sensible overlap.
    //   - Lights now follow the room ceiling height (no longer floating at 3 m).
    //   - The point-light RANGE scales with the room ceiling so the floor of the
    //     tall arena/elevator (8-9 m down) still receives light.
    //   - The big arena gets a 2-row grid across its width (z = ±zHalf*0.5) so the
    //     boss room is evenly lit instead of a single lonely strip overhead.
    if (haveLight && haveCeil) {
        // Warm-white emitter, premultiplied by intensity (kept in linear light;
        // mesh.frag accumulates additively then tonemaps).
        const float kIntensity = 3.2f;
        const float kColR = 1.00f * kIntensity;
        const float kColG = 0.86f * kIntensity;
        const float kColB = 0.62f * kIntensity;       // warm tungsten-ish white
        // HDR pipeline: the Light_A fixture MESH itself glows. An emissive radiance
        // (linear, > 1 so it is a bright HDR source) is set on each fixture instance
        // so it reads as a lit fixture + feeds the bloom chain. Matches the warm
        // point-light tint; strength chosen so the panel blooms tastefully without
        // blowing out the tonemap. (rgb = warm white, w = strength.)
        const float kEmis[4] = { 1.00f, 0.86f, 0.62f, 6.0f };
        for (const Room& r : rooms) {
            const float lightY  = r.y0 + r.ceil - 0.05f;          // just below THIS plate's ceiling
            // Range covers the 4 m spacing AND reaches the floor in tall rooms.
            const float range   = std::max(9.0f, r.ceil + 4.5f);
            const int   n       = (int)std::ceil((r.x1 - r.x0) / 4.0f);
            // B4 — THE ROWS WERE PLACED WHERE NOBODY STANDS. This used to be two rows at
            // z = +/- zHalf * 0.5. B1's plate is 76 m deep (zHalf 38), so the rows sat at
            // z = +/-19 m — while the ENTIRE play corridor (spawn, doors A-E, the elevator
            // shaft) runs down z ~ 0. With a 7.5 m range, the nearest fixture to the player
            // was ~19 m away and contributed EXACTLY ZERO. Level 1's 332 ceiling lights were
            // decorating a ceiling nobody could see by, and the 0.42 ambient wash was the
            // only thing lighting the level. (THE PATTERN: kill the crutch, the real bug
            // walks out.)
            //
            // Anchor a row ON the corridor (z = 0) and pull the flanking rows in to within
            // a light-range of it, so the pools actually OVERLAP instead of straddling a
            // dark 23 m gutter. Narrow rooms keep their single centre row exactly as before.
            //
            // ---- 2026-07-12, FACILITY LIGHTING AUDIT — THE FIX ABOVE ONLY LIT THE CORRIDOR.
            // The previous pass anchored a row at z=0 and clamped the two flanking rows to
            // z = +/-min(zHalf*0.5, 7). That rescued the z~0 play corridor — and left the
            // REST OF THE PLATE in the dark, because the clamp does not scale with depth:
            //     B1 / F1 : zHalf 38, rows at z = -7/0/+7, range 9.0  -> lit to z = +/-16.
            //               A **22 m VOID on EACH SIDE** — 58% of the plate depth. The
            //               detention cells run down -Z to z = -22 (Cell 4 "Skeleton"), so
            //               whole authored ROOMS sat outside every light in the building.
            //     F5      : zHalf 32, range 15.5 -> lit to +/-22. A 10 m void each side of
            //               the drone high-bay.
            //     F6      : 4 m void each side.
            // MEASURED, flashlight OFF (docs/screenshots/lighting_audit/facility):
            //     corridor z=0  : mean 20.5, 43% of pixels <= 6/255
            //     z = -22 (Cell4): mean  5.9, **83% of pixels <= 6/255** — a VOID, not a room.
            //     z = -25        : mean  6.8, **82% void**
            // This is the SAME bug as before, one step out: the lights were still placed
            // where the player is not. The rows must TILE THE PLATE, not hug its spine.
            //
            // So: tile the z rows across the FULL depth at a fixed 8 m pitch, phase-locked to
            // z = 0 so the corridor row (the whole point of the last fix) is still exactly on
            // the spine. 8 m pitch under a >=9 m range means adjacent pools always overlap —
            // no gutters at any depth, on any plate. Narrow rooms (zHalf < 6) keep their
            // single centre row exactly as before. Fixture count 498 -> ~1.4k: these are
            // INSTANCED ceiling panels (draw cost is nil) and the point lights are selected
            // NEAREST-TO-EYE (K=44) every frame, so the GPU-side cost is unchanged.
            constexpr float kZPitch = 8.0f;                       // < the 9.0 m minimum range => pools overlap
            const bool  wide    = (r.zHalf >= 6.0f);
            const int   zk      = wide ? (int)std::floor(r.zHalf / kZPitch) : 0;
            const int   zr      = 2 * zk + 1;                     // rows at z = -zk*8 .. 0 .. +zk*8
            for (int j=0;j<zr;++j) {
                const float wz = wide ? ((float)(j - zk) * kZPitch) : 0.0f;
                for (int i=0;i<n;++i) {
                    const float wx = r.x0 + (i + 0.5f) * 4.0f;
                    placeYaw(m, 0.0f, 1.0f, cx(kLightAabb), kLightAabb.maxy, cz(kLightAabb),
                             wx, lightY, wz);
                    addInstanceEmissive(lightA, m, kEmis);  // HDR: fixture glows + feeds bloom
                    // Point light a touch below the fixture so the ceiling panel above
                    // doesn't occlude the pool of light it casts on the floor/walls.
                    x3::rhi::PointLight pl;
                    pl.pos[0] = wx; pl.pos[1] = lightY - 0.25f; pl.pos[2] = wz;
                    pl.range  = range;
                    pl.color[0] = kColR; pl.color[1] = kColG; pl.color[2] = kColB;
                    m_lightFixtures.push_back(pl);
                }
            }
        }
        x3::logInfo("[env-art] registered " + std::to_string(m_lightFixtures.size()) +
                    " point light(s) at Light_A fixtures");
    }

    x3::logInfo("[env-art] built: " + std::to_string(assetsLoaded()) + " asset(s) loaded, " +
                std::to_string(m_instances.size()) + " instance(s); mask walls=" +
                std::to_string((int)mask.walls) + " floors=" + std::to_string((int)mask.floors));
    return mask;
}

void EnvArtSystem::worldBounds(float outMin[3], float outMax[3]) const {
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    for (const EnvInstance& inst : m_instances) {
        if (inst.asset >= m_assetTable.size()) continue;
        for (const auto& d : m_assetTable[inst.asset].drawables) {
            float fin[16]; x3::asset::mulMat4(inst.transform, d.nodeTransform, fin);
            const float w[3] = { fin[12], fin[13], fin[14] };
            for (int i = 0; i < 3; ++i) { mn[i] = std::min(mn[i], w[i]); mx[i] = std::max(mx[i], w[i]); }
        }
    }
    for (int i = 0; i < 3; ++i) { outMin[i] = mn[i]; outMax[i] = mx[i]; }
}

uint32_t EnvArtSystem::namedBounds(const std::vector<std::string>& subs,
                                   float outMin[3], float outMax[3]) const {
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    uint32_t hits = 0;
    for (const EnvAsset& a : m_assetTable) {
        if (!a.ok || a.model.nodes.empty()) continue;
        const std::vector<x3::asset::Node>& nodes = a.model.nodes;
        const int N = (int)nodes.size();
        // World matrix per node (mirror makeDrawables: world = parentWorld * local).
        // Iterative ancestor-chain resolve (no recursion lib); the depth cap guards cycles.
        std::vector<float> world((size_t)N * 16, 0.0f);
        std::vector<char>  done(N, 0);
        for (int i = 0; i < N; ++i) {
            if (done[i]) continue;
            int chain[512]; int depth = 0; int cur = i;
            while (cur >= 0 && cur < N && !done[cur] && depth < 512) {
                chain[depth++] = cur; cur = nodes[cur].parent;
            }
            for (int j = depth - 1; j >= 0; --j) {
                const int ni = chain[j];
                const int p  = nodes[ni].parent;
                if (p >= 0 && p < N && done[p])
                    x3::asset::mulMat4(&world[(size_t)p * 16], nodes[ni].localTransform, &world[(size_t)ni * 16]);
                else
                    for (int k = 0; k < 16; ++k) world[(size_t)ni * 16 + k] = nodes[ni].localTransform[k];
                done[ni] = 1;
            }
        }
        // Filter mesh nodes whose (lowercased) name contains any requested substring.
        for (int i = 0; i < N; ++i) {
            if (nodes[i].meshIndex < 0) continue;
            std::string ln = nodes[i].name;
            for (char& c : ln) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            bool hit = false;
            for (const std::string& s : subs) if (ln.find(s) != std::string::npos) { hit = true; break; }
            if (!hit) continue;
            ++hits;
            const float* w = &world[(size_t)i * 16];
            for (int k = 0; k < 3; ++k) { const float v = w[12 + k]; if (v < mn[k]) mn[k] = v; if (v > mx[k]) mx[k] = v; }
        }
        // Nothing matched? Dump a sample of mesh-node names so the caller can see the
        // real naming convention and correct the filter substrings.
        if (hits == 0) {
            int shown = 0;
            for (int i = 0; i < N && shown < 18; ++i)
                if (nodes[i].meshIndex >= 0 && !nodes[i].name.empty()) {
                    x3::logInfo("[env-art] node name sample: " + nodes[i].name); ++shown;
                }
        }
    }
    for (int k = 0; k < 3; ++k) { outMin[k] = mn[k]; outMax[k] = mx[k]; }
    return hits;
}

uint32_t EnvArtSystem::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                            uint32_t maxDrawables, const float* cullMin, const float* cullMax) const {
    // TIER-2 STREAMING (WP-4): a destroyed system has released its GPU handles —
    // drawing it is a caller bug (some fan still calling ->draw() on a torn-down
    // container). Never crash: log it once, then no-op forever after.
    if (m_destroyed) {
        if (!m_drawAfterDestroyLogged) {
            x3::logError("[env-art] draw() called AFTER destroy() — no-op (caller bug: "
                         "a destroyed EnvArtSystem is still in a draw fan)");
            m_drawAfterDestroyLogged = true;
        }
        return 0;
    }
    uint32_t drawn = 0;
    for (const EnvInstance& inst : m_instances) {
        const EnvAsset& a = m_assetTable[inst.asset];
        for (const auto& d : a.drawables) {
            float fin[16];
            x3::asset::mulMat4(inst.transform, d.nodeTransform, fin);
            // AABB cull by world origin: frames a region of a large baked scene (the
            // building) and bounds the per-frame draw count under the renderer budget.
            if (cullMin && cullMax &&
                (fin[12] < cullMin[0] || fin[13] < cullMin[1] || fin[14] < cullMin[2] ||
                 fin[12] > cullMax[0] || fin[13] > cullMax[1] || fin[14] > cullMax[2])) continue;
            if (drawn >= maxDrawables) return drawn;
            ++drawn;
            // Emissive: the GLB MATERIAL emissive (HDR-scaled factor, gated by the emissive
            // map in the shader -> glowing edge strips) takes priority; otherwise the per-
            // INSTANCE emissive (Level1 Light_A fixtures). Both feed the HDR bloom chain.
            const bool matEmis = d.emissiveTexId != 0 ||
                d.emissiveFactor[0] > 0.001f || d.emissiveFactor[1] > 0.001f || d.emissiveFactor[2] > 0.001f;
            float emis[4];
            if (matEmis) { emis[0]=d.emissiveFactor[0]; emis[1]=d.emissiveFactor[1]; emis[2]=d.emissiveFactor[2]; emis[3]=1.0f; }
            else         { emis[0]=inst.emissive[0]; emis[1]=inst.emissive[1]; emis[2]=inst.emissive[2]; emis[3]=inst.emissive[3]; }
            device.drawMeshPBR(frame,
                               x3::rhi::MeshHandle{ d.meshId },
                               x3::rhi::TextureHandle{ d.baseColorTexId },
                               x3::rhi::TextureHandle{ d.normalTexId },
                               x3::rhi::TextureHandle{ d.mrTexId },
                               d.baseColorFactor,
                               emis,
                               fin,
                               d.alphaMask,
                               d.alphaBlend,
                               x3::rhi::TextureHandle{ d.emissiveTexId },
                               x3::rhi::TextureHandle{ d.detailTexId },   // HDRP micro-detail
                               d.detailUvScale,
                               d.clearcoat, d.clearcoatRough,             // car-paint clearcoat lobe
                               /*selfLight=*/0.0f, m_metalClamp,          // BLACK-PROP metallic clamp
                               m_foliage);                                // vegetation wrap/translucency
        }
    }
    // FOLIAGE DENSIFY clones (densifyFoliage): one drawable each, at their own world
    // transform. Same draw path/material as the source tree — they ARE the source tree,
    // re-posed. Empty unless a host asked for them, so every other world is untouched.
    for (const ScatterDraw& sd : m_scatter) {
        const EnvAsset& a = m_assetTable[sd.asset];
        if (!a.ok || sd.drawable >= a.drawables.size()) continue;
        const auto& d = a.drawables[sd.drawable];
        if (cullMin && cullMax &&
            (sd.transform[12] < cullMin[0] || sd.transform[13] < cullMin[1] || sd.transform[14] < cullMin[2] ||
             sd.transform[12] > cullMax[0] || sd.transform[13] > cullMax[1] || sd.transform[14] > cullMax[2])) continue;
        if (drawn >= maxDrawables) return drawn;
        ++drawn;
        const bool matEmis = d.emissiveTexId != 0 ||
            d.emissiveFactor[0] > 0.001f || d.emissiveFactor[1] > 0.001f || d.emissiveFactor[2] > 0.001f;
        const float emis[4] = { matEmis ? d.emissiveFactor[0] : 0.0f,
                                matEmis ? d.emissiveFactor[1] : 0.0f,
                                matEmis ? d.emissiveFactor[2] : 0.0f,
                                matEmis ? 1.0f : 0.0f };
        device.drawMeshPBR(frame,
                           x3::rhi::MeshHandle{ d.meshId },
                           x3::rhi::TextureHandle{ d.baseColorTexId },
                           x3::rhi::TextureHandle{ d.normalTexId },
                           x3::rhi::TextureHandle{ d.mrTexId },
                           d.baseColorFactor,
                           emis,
                           sd.transform,
                           d.alphaMask,
                           d.alphaBlend,
                           x3::rhi::TextureHandle{ d.emissiveTexId },
                           x3::rhi::TextureHandle{ d.detailTexId },
                           d.detailUvScale,
                           d.clearcoat, d.clearcoatRough);
    }
    return drawn;
}

uint32_t EnvArtSystem::assetsLoaded() const {
    uint32_t n=0; for (const auto& a : m_assetTable) if (a.ok) ++n; return n;
}

// ===========================================================================
// TIER-2 STREAMING (WP-4): EnvArtSystem::destroy() — see env_art.h for the
// contract. Design notes (why this is NOT "loop m_instances/drawables and call
// device.destroyMesh/destroyTexture on their ids"):
//
//   HANDLE TRACKING: every GPU handle this system ever minted already lives in
//   m_assetTable[i].model (Model::primitives[].vertexBuffer/indexBuffer,
//   Model::materials[].baseColorTex/normalTex/mrTex/emissiveTex/occlusionTex —
//   see engine/asset/IModelLoader.h). loadAsset() never discards a Model after
//   loading it, so no NEW tracking vectors are needed at the creation sites —
//   the existing m_assetTable IS the handle table. What was missing was simply
//   a call to release it.
//
//   WHY unload(), NOT raw device.destroy*(): engine/asset/ModelLoader.cpp shows
//   two very different lifetimes for the two handle kinds:
//     * MESHES are never shared — GpuUploader mints a fresh createMesh() per
//       primitive on every load() call (ModelLoader.cpp ~L189), even for a
//       process-wide MODEL CACHE hit (the cached CPU template's prim data is
//       RE-uploaded, not its old handle reused; see "BOOT-TIME model cache"
//       ~L137-160). A direct destroyMesh() per drawable would be safe in
//       isolation for meshes alone.
//     * TEXTURES are process-wide REFCOUNTED (g_texCacheByKey / g_texKeyByHandle,
//       ModelLoader.cpp ~L93-135, decremented in GpuUploader::free() ~L266-290,
//       only physically destroyed when the refcount hits 0). The SAME converted
//       kit piece (e.g. SM_Wall_A.glb, or a district's shared prefab) is loaded
//       by MANY separate EnvArtSystem instances across Echotropolis (towers,
//       houses, condos, mine props, each district — loadAsset()'s path-cache is
//       only PER-INSTANCE, see env_art.cpp's loadAsset()), and the BOOT-TIME
//       MODEL CACHE itself holds a PERMANENT extra ref on every texture it has
//       ever cached (preloadModels()'s comment: "meshes freed; the caches keep
//       their own refs"). Calling device.destroyTexture() straight off a
//       ModelDrawable's baseColorTexId/normalTexId/mrTexId/emissiveTexId would
//       physically free a texture some OTHER live EnvArtSystem (or the process
//       cache) still points at — exactly the double-free this WP must not
//       introduce. ModelDrawable ids are also untagged uint32_t copies (see
//       IModelLoader.h's meshIdOf()/ModelDrawable comment) — they carry no
//       information the refcount code could use even if we wanted to reuse it.
//
//   IModelLoader::unload(Model&) (ModelLoader.cpp ~L516-534) already does this
//   exactly right: unconditionally releases every primitive's mesh, and routes
//   every material texture through the SAME acquire/release refcount the loader
//   used when it loaded them — physically destroying only on the last release.
//   So: destroy() simply hands every OK'd asset's Model to the loader that
//   loaded it. That IS the "handle tracking" this WP needs; a second, parallel
//   vector of raw ids would be redundant and (per the above) actively unsafe if
//   ever used to free textures directly instead of asking the loader.
//
//   PARKED-CARS DOCTRINE — what destroy() intentionally does NOT free, and why:
//     * The process-wide MODEL TEMPLATE cache (g_modelCache) and its own
//       permanent texture refs. That cache is the engine's boot-time warm
//       cache, shared by every loader (EnvArtSystem, MonsterSystem, CrowdSkin,
//       NpcSkin...) in the process, and nothing in the engine ever clears it.
//       Leaking that shared cache entry is CORRECT: it isn't this instance's to
//       free, and freeing it would corrupt every other live loader of the same
//       GLB. (This is the one deliberate leak in this design — acceptable per
//       the WP-4 brief: "leaking a shared texture is acceptable; double-freeing
//       is not.")
//     * Any texture still referenced by a SIBLING EnvArtSystem's Model — the
//       refcount above simply won't reach 0 yet for those; unload() already
//       handles this correctly per-model, so destroy() does not second-guess it
//       with its own bookkeeping.
//     * m_lightFixtures hold no GPU handles (plain PointLight pos/color/range
//       structs) — cleared below purely because a destroyed system's fixtures
//       are meaningless (its geometry is gone), not because there's anything to
//       release.
//
//   IDEMPOTENCY: m_destroyed guards a second call (logged once, no-op) so an
//   integrator's teardown hook can be called defensively without risk.
// ===========================================================================
void EnvArtSystem::destroy(x3::rhi::IRenderDevice& device) {
    (void)device; // must be the SAME device m_loader was created against (build()/
                  // buildFromGlb()/buildFromGlbAt()/beginFromDir() all bind m_loader
                  // to a device pointer at construction time); the parameter exists
                  // so the call site reads the same way as every other device-owning
                  // teardown (EchoRegion::destroy) and so a future loader-less path
                  // (e.g. a headless variant) still has a device to hand assets to.
    if (m_destroyed) {
        x3::logWarn("[env-art] destroy() called again — ignoring (already destroyed)");
        return;
    }
    if (m_loader) {
        for (EnvAsset& a : m_assetTable) {
            if (a.ok) m_loader->unload(a.model);   // frees meshes; refcount-releases textures
            a.ok = false;
        }
    }
    m_assetTable.clear();
    m_assetPaths.clear();
    m_instances.clear();     // draw() is instance-driven -> now a hard no-op even without the guard
    m_lightFixtures.clear();
    m_loader.reset();
    m_assets.reset();
    m_destroyed = true;
    x3::logInfo("[env-art] destroy(): GPU resources released");
}

} // namespace x3::game
