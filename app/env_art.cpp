// EFLZ environment art overlay (EFLZ art pass). See app/env_art.h.
//
// Clean-room: built from the IModelLoader + IAssetSource + IRenderDevice + Scene
// interfaces only. No purchased C# / id Tech source consulted. Public glTF refs
// + the converted GLB catalog (CATALOG.md) only.
#include "env_art.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <string>

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

inline float cx(const Aabb& a){ return (a.minx+a.maxx)*0.5f; }
inline float cy(const Aabb& a){ return (a.miny+a.maxy)*0.5f; }
inline float cz(const Aabb& a){ return (a.minz+a.maxz)*0.5f; }

// Identity 4x4 (column-major).
void ident(float m[16]) {
    for (int i=0;i<16;++i) m[i]=(i%5==0)?1.0f:0.0f;
}

// Column-major TRS with a yaw about +Y (radians) and uniform scale s, placing the
// transformed asset point that currently sits at (px,py,pz) at world (wx,wy,wz).
// i.e. world = T(w) * Rয়(yaw) * S(s) * T(-p). This recenters an asset (whose
// chosen anchor point is p) onto the world anchor w, optionally yawed + scaled.
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

} // namespace

uint32_t EnvArtSystem::loadAsset(const std::string& relPath) {
    // Cache: one upload per unique kit piece.
    for (uint32_t i=0;i<m_assetPaths.size();++i)
        if (m_assetPaths[i]==relPath) return i;

    EnvAsset a;
    a.model = m_loader->load(relPath);
    if (a.model.ok) {
        a.drawables = x3::asset::makeDrawables(a.model);
        a.ok = !a.drawables.empty();
    }
    if (a.ok)
        x3::logInfo("[env-art] loaded " + relPath + " — " +
                    std::to_string(a.drawables.size()) + " drawable prim(s)");
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

    const bool haveFloor = m_assetTable[floorA].ok;
    const bool haveCeil  = m_assetTable[ceilA].ok;
    const bool haveWall  = m_assetTable[wallA].ok;
    const bool haveFrame = m_assetTable[frameA].ok;
    const bool haveCons  = m_assetTable[consA].ok;
    const bool haveLight = m_assetTable[lightA].ok;

    // ---- Room footprints must match level1.cpp (along +X, z in [-zHalf,+zHalf]). ----
    struct Room { float x0, x1, zHalf; };
    const Room rooms[] = {
        { 0.0f,  6.0f, 3.0f }, // cell
        { 6.0f, 22.0f, 3.0f }, // corridor
        {22.0f, 30.0f, 4.0f }, // armory
        {30.0f, 42.0f, 4.0f }, // checkpoint
        {42.0f, 56.0f, 7.0f }, // arena
        {56.0f, 59.0f, 1.5f }, // elevator
    };
    constexpr float kWallTopY = 3.0f;   // graybox wall height (collision) — art is taller, fine

    float m[16];

    // ---- FLOOR + CEILING: tile the 4.0(X) x 3.0(Z) panels across each room. ----
    // The floor tile's top sits at its AABB max-Y; anchor that to world y=0. The
    // tile is anchored at its XZ center -> world tile center. We tile a grid that
    // covers each room (last row/col may overhang slightly into walls — hidden).
    auto tileSurface = [&](uint32_t asset, const Aabb& ab, bool ceiling) {
        if (asset >= m_assetTable.size() || !m_assetTable[asset].ok) return;
        const float tileX = ab.maxx - ab.minx;   // 4.0
        const float tileZ = ab.maxz - ab.minz;   // 3.0
        const float ax = cx(ab), az = cz(ab);
        const float anchorY = ceiling ? ab.miny : ab.maxy;  // ceiling: bottom; floor: top
        for (const Room& r : rooms) {
            const int nx = (int)std::ceil((r.x1 - r.x0) / tileX);
            const int nz = (int)std::ceil((2.0f * r.zHalf) / tileZ);
            for (int ix=0; ix<nx; ++ix) {
                for (int iz=0; iz<nz; ++iz) {
                    const float wxC = r.x0 + (ix + 0.5f) * tileX;
                    const float wzC = -r.zHalf + (iz + 0.5f) * tileZ;
                    const float wyC = ceiling ? kWallTopY : 0.0f;
                    // anchor: (ax, anchorY, az) -> (wxC, wyC, wzC), no yaw/scale.
                    placeYaw(m, 0.0f, 1.0f, ax, anchorY, az, wxC, wyC, wzC);
                    addInstance(asset, m);
                }
            }
        }
    };
    if (haveFloor) { tileSurface(floorA, kFloorAabb, /*ceiling*/false); mask.floors = true; }
    if (haveCeil)  { tileSurface(ceilA,  kCeilAabb,  /*ceiling*/true ); }

    // ---- WALLS: the panel is 3.0 wide (its local +Z) x 4.45 tall (Y), 1.43 thick
    // (its local X). For the long side walls (run along world X) we yaw +90deg so
    // the 3.0 axis runs along X; the thick X axis then points along Z (into the
    // wall line at z=±zHalf). For the cross end-caps (run along Z) we use yaw 0.
    // Anchor each panel at the floor (min-Y) at its width-center + thickness face. -
    if (haveWall) {
        const float panelW   = kWallAabb.maxz - kWallAabb.minz;  // 3.0 (local Z)
        const float wallMinY = kWallAabb.miny;                   // -0.04 -> floor
        const float wAnchorX = cx(kWallAabb);                    // local X center
        const float wAnchorZ = cz(kWallAabb);                    // local Z center (width)
        // Side walls: tile along X at z = ±zHalf, yaw +90deg (local +Z -> world +X).
        for (const Room& r : rooms) {
            const int n = (int)std::ceil((r.x1 - r.x0) / panelW);
            for (int s=0; s<2; ++s) {
                const float wz = (s==0) ? -r.zHalf : r.zHalf;
                for (int i=0; i<n; ++i) {
                    const float wx = r.x0 + (i + 0.5f) * panelW;
                    placeYaw(m, kPi*0.5f, 1.0f, wAnchorX, wallMinY, wAnchorZ, wx, 0.0f, wz);
                    addInstance(wallA, m);
                }
            }
        }
        mask.walls = true;
    }

    // ---- DOOR FRAMES: drop a frame at each of the five doorways. The frame is
    // 6.25 wide; we scale it to ~1.6 m so it reads as a single doorway header and
    // straddles the 1.2 m opening. The doorway is in a cross-wall (plane x=const),
    // so the frame's wide axis (local X) should run along Z -> yaw +90deg. -------
    if (haveFrame) {
        const x3::phys::Vec3 doors[5] = {
            layout.doorA, layout.doorB, layout.doorC, layout.doorD, layout.doorE };
        // The frame GLB is ~6.25 m wide; scale it so it reads as a ~2.0 m doorway
        // header straddling the 1.2 m opening.
        const float fs = 2.0f / (kFrameAabb.maxx - kFrameAabb.minx);
        for (const auto& d : doors) {
            // anchor the frame width-center + floor onto the doorway center.
            placeYaw(m, kPi*0.5f, fs, cx(kFrameAabb), kFrameAabb.miny, cz(kFrameAabb),
                     d.x, 0.0f, d.z);
            addInstance(frameA, m);
        }
    }

    // ---- CONSOLE: a terminal beside Door A and Door B (the button doors), set
    // against the +Z wall a bit so it reads as a wall terminal. Purely visual. ---
    if (haveCons) {
        const x3::phys::Vec3 consoles[2] = { layout.doorA, layout.doorB };
        for (const auto& d : consoles) {
            // place near the doorway, offset to the +Z side, facing -Z (yaw 180).
            placeYaw(m, kPi, 1.0f, cx(kConsAabb), kConsAabb.miny, cz(kConsAabb),
                     d.x - 0.6f, 0.0f, d.z + 1.4f);
            addInstance(consA, m);
        }
    }

    // ---- CEILING LIGHTS: a strip light down the corridor + arena centers. ------
    if (haveLight && haveCeil) {
        const float lightY = kWallTopY - 0.05f;
        for (const Room& r : rooms) {
            const int n = (int)std::ceil((r.x1 - r.x0) / 4.0f);
            for (int i=0;i<n;++i) {
                const float wx = r.x0 + (i + 0.5f) * 4.0f;
                placeYaw(m, 0.0f, 1.0f, cx(kLightAabb), kLightAabb.maxy, cz(kLightAabb),
                         wx, lightY, 0.0f);
                addInstance(lightA, m);
            }
        }
    }

    x3::logInfo("[env-art] built: " + std::to_string(assetsLoaded()) + " asset(s) loaded, " +
                std::to_string(m_instances.size()) + " instance(s); mask walls=" +
                std::to_string((int)mask.walls) + " floors=" + std::to_string((int)mask.floors));
    return mask;
}

void EnvArtSystem::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const {
    for (const EnvInstance& inst : m_instances) {
        const EnvAsset& a = m_assetTable[inst.asset];
        for (const auto& d : a.drawables) {
            float fin[16];
            x3::asset::mulMat4(inst.transform, d.nodeTransform, fin);
            device.drawMesh(frame,
                            x3::rhi::MeshHandle{ d.meshId },
                            x3::rhi::TextureHandle{ d.baseColorTexId },
                            d.baseColorFactor,
                            fin);
        }
    }
}

uint32_t EnvArtSystem::assetsLoaded() const {
    uint32_t n=0; for (const auto& a : m_assetTable) if (a.ok) ++n; return n;
}

} // namespace x3::game
