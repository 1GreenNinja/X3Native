// EFLZ environment art overlay (EFLZ art pass). See app/env_art.h.
//
// Clean-room: built from the IModelLoader + IAssetSource + IRenderDevice + Scene
// interfaces only. No purchased C# / id Tech source consulted. Public glTF refs
// + the converted GLB catalog (CATALOG.md) only.
#include "env_art.h"

#include "engine/core/x3_log.h"

#include <algorithm>
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

    // ---- Room footprints + RAISED ceiling heights come from level1.cpp's shared
    // canonical table (level1Rooms()), so the GLB floor/wall/ceiling/light tiling
    // matches the collision geometry EXACTLY (bounds AND per-room height). ----
    struct Room { float x0, x1, zHalf, ceil; };
    Room rooms[(uint32_t)L1Room::Count];
    {
        const L1RoomDef* tbl = level1Rooms();
        for (uint32_t i = 0; i < (uint32_t)L1Room::Count; ++i)
            rooms[i] = Room{ tbl[i].x0, tbl[i].x1, tbl[i].zHalf, tbl[i].ceil };
    }

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
                    // Floor at y=0; ceiling at THIS room's raised height (so the
                    // panel sits at the new ceiling, not floating at the old 3 m).
                    const float wyC = ceiling ? r.ceil : 0.0f;
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
                const float rowBaseY = (float)row * panelH;     // this row's floor
                for (int s=0; s<2; ++s) {
                    const float wz = (s==0) ? -r.zHalf : r.zHalf;
                    for (int i=0; i<n; ++i) {
                        const float wx = r.x0 + (i + 0.5f) * panelW;
                        placeYaw(m, kPi*0.5f, 1.0f, wAnchorX, wallMinY, wAnchorZ, wx, rowBaseY, wz);
                        addInstance(wallA, m);
                    }
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
            // Place near the doorway, offset to the +Z side wall (z = d.z + 1.4),
            // and face the console screen back into the room toward -Z so the player
            // (who approaches from -Z / room center) sees its face. Per the facing
            // convention above, facing world -Z is placeYaw yaw = 0 (NOT pi — the
            // old "yaw 180" comment was wrong: pi faces +Z, into the wall).
            placeYaw(m, 0.0f, 1.0f, cx(kConsAabb), kConsAabb.miny, cz(kConsAabb),
                     d.x - 0.6f, 0.0f, d.z + 1.4f);
            addInstance(consA, m);
        }
    }

    // ---- DETAIL PROPS (D-content): dress each room with crates / barrels /
    // pallets / a wall fusebox / pipe runs so the cell, armory, checkpoint and
    // arena read as distinct spaces instead of empty boxes. PURELY VISUAL (no
    // collision) — placed clear of the z=0 walk spine + the doorways so they never
    // block the player. Each prop is anchored at its floor (min-Y) onto the target
    // world XZ, optionally yawed. A failed asset is silently skipped. ----
    auto placeProp = [&](uint32_t asset, const Aabb& ab, float yaw,
                         float wx, float wz, float scale = 1.0f) {
        if (asset >= m_assetTable.size() || !m_assetTable[asset].ok) return;
        placeYaw(m, yaw, scale, cx(ab), ab.miny, cz(ab), wx, 0.0f, wz);
        addInstance(asset, m);
    };
    // Wall-panel prop (fusebox): anchored on a side wall, raised so it reads as a
    // mounted panel (its AABB dips below 0, so lift it ~1 m onto the wall).
    auto placeWallPanel = [&](uint32_t asset, const Aabb& ab, float yaw,
                              float wx, float wy, float wz) {
        if (asset >= m_assetTable.size() || !m_assetTable[asset].ok) return;
        placeYaw(m, yaw, 1.0f, cx(ab), cy(ab), cz(ab), wx, wy, wz);
        addInstance(asset, m);
    };
    {
        const float cellZ = rooms[(uint32_t)L1Room::Cell].zHalf;
        const float armZ  = rooms[(uint32_t)L1Room::Armory].zHalf;
        const float chkZ  = rooms[(uint32_t)L1Room::Checkpoint].zHalf;
        const float arZ   = rooms[(uint32_t)L1Room::Arena].zHalf;
        // CELL: a barrel + a short crate tucked in the corner behind the spawn.
        placeProp(barrel, kBarrelAabb, 0.0f,        4.6f, -cellZ + 0.7f);
        placeProp(crateS, kCrateSAabb, 0.4f,        4.9f,  cellZ - 0.7f);
        // CORRIDOR: pipe runs along both side walls (reads as a service corridor)
        // + a wall fusebox between the guards' patrol.
        placeProp(pipes,  kPipesAabb,  0.0f,        10.0f, -3.0f + 0.2f);
        placeProp(pipes,  kPipesAabb,  0.0f,        17.0f,  3.0f - 0.2f);
        placeWallPanel(fuse, kFuseAabb, kPi*0.5f,   14.0f, 1.6f, -3.0f + 0.15f);
        // ARMORY: weapon-locker feel — a row of crates + a pallet flanking the
        // pistol pedestal (armoryCenter), kept off the z=0 line to the pickup.
        placeProp(pallet, kPalletAabb, 0.0f,        24.0f, -armZ + 1.1f);
        placeProp(crateL, kCrateLAabb, kPi*0.5f,    24.0f, -armZ + 1.0f);
        placeProp(crateS, kCrateSAabb, 0.0f,        24.7f, -armZ + 1.9f);
        placeProp(crateS, kCrateSAabb, 0.7f,        28.0f,  armZ - 1.0f);
        placeProp(barrel, kBarrelAabb, 0.0f,        28.6f,  armZ - 1.6f);
        // CHECKPOINT: barricade crates the guards use as cover (off the spine).
        placeProp(crateL, kCrateLAabb, 0.0f,        34.0f, -chkZ + 1.2f);
        placeProp(crateS, kCrateSAabb, 0.0f,        34.0f, -chkZ + 2.0f);
        placeProp(crateL, kCrateLAabb, 0.0f,        38.0f,  chkZ - 1.2f);
        placeWallPanel(fuse, kFuseAabb, kPi*0.5f,   36.0f, 1.6f, -chkZ + 0.15f);
        // ARENA: heavy crates + barrels around the perimeter so the boss room has
        // cover/scenery; kept well back from the center where Martinez spawns.
        placeProp(crateL, kCrateLAabb, kPi*0.5f,    45.0f, -arZ + 1.4f);
        placeProp(crateS, kCrateSAabb, 0.0f,        45.8f, -arZ + 1.2f);
        placeProp(barrel, kBarrelAabb, 0.0f,        53.0f, -arZ + 1.5f);
        placeProp(crateL, kCrateLAabb, kPi*0.5f,    45.0f,  arZ - 1.4f);
        placeProp(barrel, kBarrelAabb, 0.0f,        53.0f,  arZ - 1.5f);
        placeProp(crateS, kCrateSAabb, 0.5f,        53.6f,  arZ - 2.2f);
        placeProp(pallet, kPalletAabb, kPi*0.5f,    49.0f, -arZ + 1.1f);
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
        for (const Room& r : rooms) {
            const float lightY  = r.ceil - 0.05f;                 // fixture just below ceiling
            // Range covers the 4 m spacing AND reaches the floor in tall rooms.
            const float range   = std::max(7.5f, r.ceil + 3.5f);
            const int   n       = (int)std::ceil((r.x1 - r.x0) / 4.0f);
            // Wide rooms (arena) get two z-rows so the floor is evenly lit.
            const bool  twoRows = (r.zHalf >= 6.0f);
            const int   zr      = twoRows ? 2 : 1;
            const float zoff    = twoRows ? r.zHalf * 0.5f : 0.0f;
            for (int j=0;j<zr;++j) {
                const float wz = twoRows ? ((j==0) ? -zoff : zoff) : 0.0f;
                for (int i=0;i<n;++i) {
                    const float wx = r.x0 + (i + 0.5f) * 4.0f;
                    placeYaw(m, 0.0f, 1.0f, cx(kLightAabb), kLightAabb.maxy, cz(kLightAabb),
                             wx, lightY, wz);
                    addInstance(lightA, m);
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
