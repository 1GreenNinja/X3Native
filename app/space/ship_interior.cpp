// app/space/ship_interior.cpp — S5 walkable, data-driven ship interior.
#include "ship_interior.h"

#include "../mesh_prims.h"        // x3::prims box builders + procedural sci-fi textures
#include "../headless_device.h"   // HeadlessRenderDevice for the self-test

#include "engine/core/x3_log.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace x3::space {

using x3::rhi::MeshHandle;
using x3::rhi::TextureHandle;
using x3::game::Entity;
using x3::game::Scene;

namespace {

// Identity column-major model — every shell prim already bakes its world position
// into its vertices (makeBox takes a world center), so entities use identity.
const float kIdentity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

// Wall thickness + ceiling height defaults for the shells. Rooms author their
// INTERIOR floor AABB; we wrap it in plates of this thickness.
constexpr float kWallT = 0.18f;   // wall/floor/ceiling plate HALF-thickness

} // namespace

// Append one box shell: render mesh + entity (Tag::Static) + static collision body.
// `hx/hy/hz` are HALF extents, `(cx,cy,cz)` world center. Records the mesh + body so
// shutdown() can release them. Returns the scene entity id.
static uint32_t addShell(x3::rhi::IRenderDevice& device, Scene& scene,
                         x3::phys::IPhysicsWorld& physics,
                         std::vector<MeshHandle>& meshes,
                         std::vector<x3::phys::BodyId>& bodies,
                         TextureHandle tex, const float color[4],
                         float hx, float hy, float hz,
                         float cx, float cy, float cz) {
    x3::prims::PrimMesh m = x3::prims::makeBox(hx, hy, hz, cx, cy, cz, 0.5f);
    MeshHandle mesh = device.createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                        m.index.data(), (uint32_t)m.index.size());
    meshes.push_back(mesh);

    x3::phys::BodyId body = physics.addStaticMesh(
        m.cverts.data(), (uint32_t)(m.cverts.size() / 3),
        m.cindex.data(), (uint32_t)m.cindex.size());
    bodies.push_back(body);

    Entity e;
    e.mesh = mesh;
    e.tex  = tex;
    e.baseColor[0] = color[0]; e.baseColor[1] = color[1];
    e.baseColor[2] = color[2]; e.baseColor[3] = color[3];
    std::memcpy(e.transform, kIdentity, sizeof(kIdentity));
    e.tag = (uint32_t)x3::game::Tag::Static;
    return scene.add(e);
}

void ShipInterior::build(x3::rhi::IRenderDevice& device, Scene& scene,
                         x3::phys::IPhysicsWorld& physics,
                         const ShipManifest& manifest) {
    m_manifest = manifest;
    m_built = true;
    m_entityCount = 0;
    const uint32_t startEntities = scene.size();

    // ---- Shared procedural sci-fi surfaces (industrial deck look) -----------
    auto floorPx = x3::prims::makeFloorGrateRGBA(128, 2, x3::prims::detail::kNoTint, true);
    TextureHandle floorTex = device.createTexture(floorPx.data(), 128, 128, true);
    auto wallPx = x3::prims::makeSciFiPanelRGBA(128, 2);
    TextureHandle wallTex = device.createTexture(wallPx.data(), 128, 128, true);
    auto ceilPx = x3::prims::makeCeilingPanelRGBA(128, 2, x3::prims::detail::kNoTint, true);
    TextureHandle ceilTex = device.createTexture(ceilPx.data(), 128, 128, true);
    m_textures = { floorTex, wallTex, ceilTex };

    const float floorC[4] = { 0.85f, 0.88f, 0.95f, 1.0f };
    const float wallC[4]  = { 0.72f, 0.76f, 0.84f, 1.0f };
    const float ceilC[4]  = { 0.55f, 0.58f, 0.66f, 1.0f };

    // Helper: does a doorway gap intersect the wall plate we are about to build on
    // this side of a room? If so we split the wall around the opening so the player
    // can pass. A doorway matches a wall side when its center lies on that wall plane.
    auto doorOnWall = [&](float planeAxisVal, int axis, float lo, float hi,
                          float& outA, float& outB, float& outTop) -> bool {
        // axis: 0 = wall runs along X (a +/-Z wall), test door.pos.z == plane;
        //       2 = wall runs along Z (a +/-X wall), test door.pos.x == plane.
        for (const auto& d : m_manifest.doors) {
            const float planePos = (axis == 0) ? d.pos[2] : d.pos[0];
            if (std::fabs(planePos - planeAxisVal) > 0.4f) continue;
            const float along = (axis == 0) ? d.pos[0] : d.pos[2];
            const float hw = d.size[0] * 0.5f;
            if (along - hw < lo || along + hw > hi) continue; // opening not on this span
            outA = along - hw; outB = along + hw; outTop = d.size[1];
            return true;
        }
        return false;
    };

    // ---- Build each room: floor + ceiling + 4 walls (gapped for doorways) ----
    for (uint32_t ri = 0; ri < (uint32_t)m_manifest.rooms.size(); ++ri) {
        const Room& r = m_manifest.rooms[ri];
        const float x0 = r.boundsMin[0], x1 = r.boundsMax[0];
        const float y0 = r.boundsMin[1], y1 = r.boundsMax[1];
        const float z0 = r.boundsMin[2], z1 = r.boundsMax[2];
        const float cx = 0.5f * (x0 + x1), cz = 0.5f * (z0 + z1);
        const float hx = 0.5f * (x1 - x0), hz = 0.5f * (z1 - z0);
        const float roomH = (y1 - y0);
        const float cyWall = y0 + roomH * 0.5f;

        // Floor (just below the interior floor y0) + ceiling (just above y1).
        addShell(device, scene, physics, m_meshes, m_bodies, floorTex, floorC,
                 hx + kWallT, kWallT, hz + kWallT, cx, y0 - kWallT, cz);
        ++m_entityCount;
        addShell(device, scene, physics, m_meshes, m_bodies, ceilTex, ceilC,
                 hx + kWallT, kWallT, hz + kWallT, cx, y1 + kWallT, cz);
        ++m_entityCount;

        // Four walls. For each, if a doorway lies on it, split into two segments
        // around the opening (and a lintel above) so the player can walk through.
        auto buildWallX = [&](float zPlane) {  // wall in the XY plane at z = zPlane
            float a, b, topH;
            if (doorOnWall(zPlane, 0, x0, x1, a, b, topH)) {
                // left segment x0..a, right segment b..x1, lintel above topH.
                if (a - x0 > 0.05f) {
                    addShell(device, scene, physics, m_meshes, m_bodies, wallTex, wallC,
                             0.5f * (a - x0), roomH * 0.5f, kWallT,
                             0.5f * (x0 + a), cyWall, zPlane);
                    ++m_entityCount;
                }
                if (x1 - b > 0.05f) {
                    addShell(device, scene, physics, m_meshes, m_bodies, wallTex, wallC,
                             0.5f * (x1 - b), roomH * 0.5f, kWallT,
                             0.5f * (b + x1), cyWall, zPlane);
                    ++m_entityCount;
                }
                if (roomH - topH > 0.05f) {   // lintel above the opening
                    addShell(device, scene, physics, m_meshes, m_bodies, wallTex, wallC,
                             0.5f * (b - a), 0.5f * (roomH - topH), kWallT,
                             0.5f * (a + b), y0 + topH + 0.5f * (roomH - topH), zPlane);
                    ++m_entityCount;
                }
            } else {
                addShell(device, scene, physics, m_meshes, m_bodies, wallTex, wallC,
                         hx + kWallT, roomH * 0.5f, kWallT, cx, cyWall, zPlane);
                ++m_entityCount;
            }
        };
        auto buildWallZ = [&](float xPlane) {  // wall in the ZY plane at x = xPlane
            float a, b, topH;
            if (doorOnWall(xPlane, 2, z0, z1, a, b, topH)) {
                if (a - z0 > 0.05f) {
                    addShell(device, scene, physics, m_meshes, m_bodies, wallTex, wallC,
                             kWallT, roomH * 0.5f, 0.5f * (a - z0),
                             xPlane, cyWall, 0.5f * (z0 + a));
                    ++m_entityCount;
                }
                if (z1 - b > 0.05f) {
                    addShell(device, scene, physics, m_meshes, m_bodies, wallTex, wallC,
                             kWallT, roomH * 0.5f, 0.5f * (z1 - b),
                             xPlane, cyWall, 0.5f * (b + z1));
                    ++m_entityCount;
                }
                if (roomH - topH > 0.05f) {
                    addShell(device, scene, physics, m_meshes, m_bodies, wallTex, wallC,
                             kWallT, 0.5f * (roomH - topH), 0.5f * (b - a),
                             xPlane, y0 + topH + 0.5f * (roomH - topH), 0.5f * (a + b));
                    ++m_entityCount;
                }
            } else {
                addShell(device, scene, physics, m_meshes, m_bodies, wallTex, wallC,
                         kWallT, roomH * 0.5f, hz + kWallT, xPlane, cyWall, cz);
                ++m_entityCount;
            }
        };
        buildWallX(z0); buildWallX(z1);
        buildWallZ(x0); buildWallZ(x1);

        // Spawn = center of the FIRST room, feet on its floor.
        if (ri == 0) m_spawn = x3::phys::Vec3{ cx, y0 + 0.05f, cz };
    }

    // ---- Station marker props (helm/nav/repair/weapons consoles) ------------
    // A small emissive console box so each station reads as an interactive fixture.
    // Color-coded by kind. Purely visual (no collision needed to walk up to it).
    auto stationTexPx = x3::prims::makeSolidRGBA(8, 40, 48, 60);
    TextureHandle stTex = device.createTexture(stationTexPx.data(), 8, 8, true);
    m_textures.push_back(stTex);
    for (const auto& s : m_manifest.stations) {
        float er = 0.2f, eg = 0.8f, eb = 1.0f;   // default helm cyan
        if (s.kind == "nav")     { er = 0.3f; eg = 1.0f; eb = 0.4f; }
        else if (s.kind == "repair")  { er = 1.0f; eg = 0.7f; eb = 0.2f; }
        else if (s.kind == "weapons") { er = 1.0f; eg = 0.3f; eb = 0.3f; }

        x3::prims::PrimMesh m = x3::prims::makeBox(0.45f, 0.55f, 0.35f,
                                                   s.pos[0], s.pos[1] + 0.55f, s.pos[2], 1.0f);
        MeshHandle mesh = device.createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                            m.index.data(), (uint32_t)m.index.size());
        m_meshes.push_back(mesh);
        Entity e;
        e.mesh = mesh;
        e.tex  = stTex;
        e.baseColor[0] = 0.3f; e.baseColor[1] = 0.34f; e.baseColor[2] = 0.4f; e.baseColor[3] = 1.0f;
        e.emissive[0] = er; e.emissive[1] = eg; e.emissive[2] = eb; e.emissive[3] = 2.0f;
        std::memcpy(e.transform, kIdentity, sizeof(kIdentity));
        e.tag = (uint32_t)x3::game::Tag::Prop;
        scene.add(e);
        ++m_entityCount;
    }

    (void)startEntities;
}

void ShipInterior::render(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                          Scene& scene) {
    scene.render(device, frame);
}

void ShipInterior::shutdown(x3::phys::IPhysicsWorld& physics) {
    for (auto b : m_bodies) if (b.valid()) physics.removeBody(b);
    m_bodies.clear();
    // Meshes/textures are released implicitly at device shutdown; leave them to the
    // device's own teardown (matches the other showcases, which don't churn handles).
    m_built = false;
}

// ---------------------------------------------------------------------------
// Built-in manifests
// ---------------------------------------------------------------------------
ShipManifest ShipInterior::makeSmallCockpit() {
    ShipManifest m;
    m.shipClass = ShipClass::Small;

    // Room 0: the COCKPIT — a 6 x 3 x 6 m capsule the player spawns in.
    Room cockpit;
    cockpit.name = "Cockpit";
    cockpit.boundsMin[0] = -3.0f; cockpit.boundsMin[1] = 0.0f; cockpit.boundsMin[2] = -3.0f;
    cockpit.boundsMax[0] =  3.0f; cockpit.boundsMax[1] = 3.0f; cockpit.boundsMax[2] =  3.0f;
    m.rooms.push_back(cockpit);

    // Room 1: a short CORRIDOR aft of the cockpit (3 x 3 x 5 m), joined at z = +3.
    Room corridor;
    corridor.name = "Corridor";
    corridor.boundsMin[0] = -1.5f; corridor.boundsMin[1] = 0.0f; corridor.boundsMin[2] = 3.0f;
    corridor.boundsMax[0] =  1.5f; corridor.boundsMax[1] = 3.0f; corridor.boundsMax[2] = 8.0f;
    m.rooms.push_back(corridor);

    // Door between cockpit (room 0) and corridor (room 1), centered on the z = +3
    // shared wall: a 1.4 m wide, 2.2 m tall opening.
    Door d;
    d.pos[0] = 0.0f; d.pos[1] = 1.1f; d.pos[2] = 3.0f;
    d.size[0] = 1.4f; d.size[1] = 2.2f;
    d.roomA = 0; d.roomB = 1;
    m.doors.push_back(d);

    // Stations: a HELM at the front of the cockpit + a NAV console beside it.
    m.stations.push_back(Station{ "helm", { 0.0f, 0.0f, -2.4f }, 0.0f });
    m.stations.push_back(Station{ "nav",  { 1.8f, 0.0f, -1.6f }, -0.6f });

    // Windows (S6 consumes later): the cockpit's forward viewport + a side port.
    m.windows.push_back({ 0.0f, 1.6f, -3.0f, 3.0f, 1.4f, 0.0f });          // forward
    m.windows.push_back({ -3.0f, 1.6f, 0.0f, 2.0f, 1.0f, 1.5708f });       // port side

    return m;
}

// ===========================================================================
// FireflyCockpit — warm "used future" GLB dressing for the --world showcase.
// ===========================================================================
namespace {

constexpr float kPiF = 3.14159265358979f;

// Probed world-space AABBs of the SM_* kit pieces (meters), AFTER the GLB node
// transforms — copied from env_art.cpp's measured table so placement matches.
struct CkAabb { float minx,miny,minz, maxx,maxy,maxz; };
constexpr CkAabb kFloorAabb { -5.12f, -0.04f, 0.00f, -1.12f, 0.19f, 3.00f };  // 4.0(X)x3.0(Z)
constexpr CkAabb kCeilAabb  { -5.12f,  4.04f, 0.00f, -1.12f, 4.40f, 3.00f };
constexpr CkAabb kWallAabb  { -1.43f, -0.04f, 0.00f,  0.00f, 4.40f, 3.00f };  // 3.0 wide(Z) x 4.45 tall
constexpr CkAabb kConsAabb  { -0.47f,  0.00f,-0.31f,  0.31f, 1.55f, 0.29f };
constexpr CkAabb kLightAabb { -0.22f,  0.00f, 0.00f,  0.00f, 0.03f, 2.26f };
constexpr CkAabb kPipesAabb { -0.123f,-0.011f,-0.000f, 0.539f, 0.164f, 3.000f };
constexpr CkAabb kFrameAabb { -6.25f, -0.04f,-0.28f,  0.00f, 4.40f, 0.28f };

inline float ckcx(const CkAabb& a){ return (a.minx+a.maxx)*0.5f; }
inline float ckcz(const CkAabb& a){ return (a.minz+a.maxz)*0.5f; }

// Tileable hash + value noise for the deep-space window texture.
inline float sHash(uint32_t x, uint32_t y, uint32_t n, uint32_t salt) {
    uint32_t h = (x % n) * 374761393u + (y % n) * 668265263u + salt * 2654435761u;
    h = (h ^ (h >> 13)) * 1274126177u; h ^= h >> 16;
    return (float)(h & 0xFFFFFFu) / (float)0x1000000u;
}
inline float sNoise(float u, float v, uint32_t cell, uint32_t salt) {
    const float fx = u*(float)cell, fy = v*(float)cell;
    const uint32_t x0=(uint32_t)std::floor(fx), y0=(uint32_t)std::floor(fy);
    const float tx=fx-(float)x0, ty=fy-(float)y0;
    auto sm=[](float t){return t*t*(3.0f-2.0f*t);};
    const float sx=sm(tx), sy=sm(ty);
    const float a=sHash(x0,y0,cell,salt), b=sHash(x0+1,y0,cell,salt);
    const float c=sHash(x0,y0+1,cell,salt), d=sHash(x0+1,y0+1,cell,salt);
    const float ab=a+(b-a)*sx, cd=c+(d-c)*sx;
    return ab+(cd-ab)*sy;
}
// A DEEP, mostly-BLACK star field with a faint warm-violet nebula band + sparse
// stars. Kept dim on purpose so an emissive pane reads as real space (points of
// light on black), not a glowing white sheet. Tileable so a UV pan never seams.
inline std::vector<uint8_t> bakeSpaceRGBA(uint32_t n) {
    std::vector<uint8_t> px((size_t)n*n*4, 0);
    for (uint32_t y=0;y<n;++y){
        const float v=(y+0.5f)/(float)n;
        for (uint32_t x=0;x<n;++x){
            const float u=(x+0.5f)/(float)n;
            // Faint nebula: low-freq noise -> a soft warm-violet cloud, VERY dim, on
            // near-black space (so stars read as points, not a gray sheet).
            float neb = 0.6f*sNoise(u,v,5,7u) + 0.4f*sNoise(u,v,11,23u);
            neb = std::clamp((neb-0.62f)/0.38f, 0.0f, 1.0f); neb*=neb;
            float r = 0.010f + 0.30f*neb;      // warm-violet wisp
            float g = 0.006f + 0.10f*neb;
            float b = 0.020f + 0.36f*neb;
            // Stars: sparse hash threshold, driven to FULL brightness so they survive
            // the low pane emissive as crisp points on the near-black field.
            float hs = sHash(x,y,n,131u);
            if (hs > 0.9975f){ float br=3.2f; r+=br; g+=br; b+=br; }          // bright star
            else if (hs > 0.992f){ float br=1.5f; r+=br*1.05f; g+=br*0.97f; b+=br; } // mid star
            else if (hs > 0.982f){ float br=0.7f; r+=br; g+=br*0.95f; b+=br; }       // dim star
            auto u8=[](float c){c=std::clamp(c,0.0f,1.0f);return (uint8_t)std::lround(c*255.0f);};
            uint8_t* p=&px[((size_t)y*n+x)*4];
            p[0]=u8(r); p[1]=u8(g); p[2]=u8(b); p[3]=255;
        }
    }
    return px;
}

// Build a unit window quad (local XY plane, +Z normal, -0.5..0.5) with the given
// UV span so the camera samples a sub-window of the larger field (overscan).
inline void ckQuad(x3::rhi::MeshVertex out[4], float u0, float v0, float u1, float v1) {
    auto mk=[](float x,float y,float u,float v){ x3::rhi::MeshVertex q{};
        q.pos[0]=x; q.pos[1]=y; q.pos[2]=0; q.normal[2]=1.0f; q.uv[0]=u; q.uv[1]=v; return q; };
    out[0]=mk(-0.5f,-0.5f,u0,v0); out[1]=mk(0.5f,-0.5f,u1,v0);
    out[2]=mk(0.5f,0.5f,u1,v1);   out[3]=mk(-0.5f,0.5f,u0,v1);
}

// Column-major TRS: yaw about +Y + uniform scale s, mapping local anchor (px,py,pz)
// to world (wx,wy,wz). Mirrors env_art::placeYaw (glTF facing: yaw 0 -> faces -Z).
void ckPlaceYaw(float m[16], float yaw, float s,
                float px, float py, float pz,
                float wx, float wy, float wz) {
    const float c = std::cos(yaw), sn = std::sin(yaw);
    m[0]=c*s;  m[1]=0;   m[2]=-sn*s; m[3]=0;
    m[4]=0;    m[5]=s;   m[6]=0;     m[7]=0;
    m[8]=sn*s; m[9]=0;   m[10]=c*s;  m[11]=0;
    const float rpx = (c*px + sn*pz) * s;
    const float rpy = (py) * s;
    const float rpz = (-sn*px + c*pz) * s;
    m[12]=wx - rpx; m[13]=wy - rpy; m[14]=wz - rpz; m[15]=1.0f;
}

} // namespace

uint32_t FireflyCockpit::loadAsset(const std::string& relPath) {
    for (uint32_t i=0;i<m_paths.size();++i) if (m_paths[i]==relPath) return i;
    Asset a;
    a.model = m_loader->load(relPath);
    if (a.model.ok) { a.drawables = x3::asset::makeDrawables(a.model); a.ok = !a.drawables.empty(); }
    if (a.ok) x3::logInfo("[firefly] loaded " + relPath + " (" +
                          std::to_string(a.drawables.size()) + " prim)");
    else      x3::logWarn("[firefly] FAILED " + relPath);
    uint32_t idx = (uint32_t)m_table.size();
    m_table.push_back(std::move(a));
    m_paths.push_back(relPath);
    return idx;
}

void FireflyCockpit::addInstance(uint32_t a, const float m[16], const float emissive[4]) {
    if (a >= m_table.size() || !m_table[a].ok) return;
    Inst e; e.asset = a;
    for (int i=0;i<16;++i) e.transform[i]=m[i];
    if (emissive) for (int i=0;i<4;++i) e.emissive[i]=emissive[i];
    m_instances.push_back(e);
}

bool FireflyCockpit::build(x3::rhi::IRenderDevice& device, const std::string& convertedGlbDir) {
    m_assets.reset(x3::asset::createAssetSource());
    if (!m_assets->mountDir(convertedGlbDir, 0)) {
        x3::logWarn("[firefly] mountDir failed: " + convertedGlbDir + " — graybox kept");
        return false;
    }
    m_loader.reset(x3::asset::createModelLoader(&device, m_assets.get()));

    const std::string kit = "ModularSciFi_Interior/";
    const uint32_t floorA = loadAsset(kit + "SM_Floor_A.glb");
    const uint32_t ceilA  = loadAsset(kit + "SM_Ceiling_A.glb");
    const uint32_t wallA  = loadAsset(kit + "SM_Wall_A.glb");
    const uint32_t consA  = loadAsset(kit + "SM_Console.glb");
    const uint32_t lightA = loadAsset(kit + "SM_Light_A.glb");
    const uint32_t pipesA = loadAsset(kit + "SM_Pipes_A.glb");
    const uint32_t frameA = loadAsset(kit + "SM_DoorFrame_A.glb");

    const bool haveFloor = m_table[floorA].ok;
    const bool haveWall  = m_table[wallA].ok;
    float m[16];

    // ---- COCKPIT FOOTPRINT ---------------------------------------------------
    // A 6 m (X, -3..3) x 6 m (Z, -3..8) deck that matches makeSmallCockpit()'s
    // collision shell (cockpit X[-3,3] Z[-3,3] + corridor aft to z=8). The FORWARD
    // wall (z = -3) is LEFT OPEN for the big window to space (ShipWindows portal).
    const float fTileX = kFloorAabb.maxx - kFloorAabb.minx; // 4.0
    const float fTileZ = kFloorAabb.maxz - kFloorAabb.minz; // 3.0
    const float fax = ckcx(kFloorAabb), faz = ckcz(kFloorAabb);

    // FLOOR + CEILING tiling over X[-3,3] Z[-3,8].
    auto tile = [&](uint32_t asset, const CkAabb& ab, float anchorY, float wyC) {
        if (asset >= m_table.size() || !m_table[asset].ok) return;
        const float tx = ab.maxx - ab.minx, tz = ab.maxz - ab.minz;
        const float ax = ckcx(ab), az = ckcz(ab);
        for (float wx = -3.0f + tx*0.5f; wx < 3.0f + 0.1f; wx += tx)
            for (float wz = -3.0f + tz*0.5f; wz < 8.0f + 0.1f; wz += tz) {
                ckPlaceYaw(m, 0.0f, 1.0f, ax, anchorY, az, wx, wyC, wz);
                addInstance(asset, m);
            }
    };
    (void)fTileX; (void)fTileZ; (void)fax; (void)faz;
    if (haveFloor) tile(floorA, kFloorAabb, kFloorAabb.maxy, 0.0f);      // floor top -> y=0
    if (m_table[ceilA].ok) tile(ceilA, kCeilAabb, kCeilAabb.miny, 3.0f); // ceiling bottom -> y=3

    // ---- WALLS: left (x=-3) + right (x=+3) side walls run along Z; aft (z=+8) cap.
    // FORWARD wall (z=-3) intentionally omitted (the window). Wall panel is 3.0 wide
    // (local Z), 1.43 thick (local X). Side walls -> yaw +/-90 so width runs along Z.
    if (haveWall) {
        const float panelW = kWallAabb.maxz - kWallAabb.minz; // 3.0
        const float wMinY  = kWallAabb.miny;
        const float wax = ckcx(kWallAabb), waz = ckcz(kWallAabb);
        // Side walls along Z (x = -3 and x = +3), tiling z = -3..8.
        for (int side = 0; side < 2; ++side) {
            const float wx = (side==0) ? -3.0f : 3.0f;
            const float yaw = (side==0) ? kPiF*0.5f : -kPiF*0.5f; // face into the room
            for (float wz = -3.0f + panelW*0.5f; wz < 8.0f + 0.1f; wz += panelW) {
                ckPlaceYaw(m, yaw, 1.0f, wax, wMinY, waz, wx, 0.0f, wz);
                addInstance(wallA, m);
            }
        }
        // Aft cap (z = +8), runs along X (yaw 0).
        for (float wx = -3.0f + panelW*0.5f; wx < 3.0f + 0.1f; wx += panelW) {
            ckPlaceYaw(m, 0.0f, 1.0f, wax, wMinY, waz, wx, 0.0f, 8.0f);
            addInstance(wallA, m);
        }
        // The FORWARD wall (z=-3) is deliberately left fully open — the big Serenity
        // window (S6 portal) fills it, so the pilot's whole forward view is space.
    }

    // ---- DOOR FRAME at the aft corridor mouth (z ~ +3) for mechanical character.
    if (m_table[frameA].ok) {
        const float fs = 2.4f / (kFrameAabb.maxx - kFrameAabb.minx);
        ckPlaceYaw(m, kPiF*0.5f, fs, ckcx(kFrameAabb), kFrameAabb.miny, ckcz(kFrameAabb),
                   0.0f, 0.0f, 3.0f);
        addInstance(frameA, m);
    }

    // ---- PILOT'S CONSOLE: the centerpiece. A wide bank across the front of the
    // cockpit, just inside the window, facing aft (+Z) toward the pilot's seat. We
    // place THREE consoles side-by-side so it reads as a real helm dash, not one box.
    if (m_table[consA].ok) {
        const float consBaseZ = -2.0f;          // just inside the forward window
        for (int i = -1; i <= 1; ++i) {
            const float wx = (float)i * 0.85f;
            // Console faces the pilot (toward +Z) -> yaw pi. Slight fan-out yaw so the
            // side panels angle inward (a curved Serenity dash).
            const float yaw = kPiF + (float)i * 0.18f;
            ckPlaceYaw(m, yaw, 1.0f, ckcx(kConsAabb), kConsAabb.miny, ckcz(kConsAabb),
                       wx, 0.0f, consBaseZ);
            // Warm amber screen glow on the console itself (feeds bloom -> lit dash).
            // Kept modest so the dash reads as warm panels, not a white blowout.
            const float consEmis[4] = { 1.0f, 0.52f, 0.20f, 1.1f };
            addInstance(consA, m, consEmis);
        }
    }

    // ---- PIPES: ceiling + wall runs (the "lived-in" mechanical clutter). --------
    if (m_table[pipesA].ok) {
        // Two ceiling pipe runs down the length of the cockpit (along Z), tucked to
        // the corners. Pipes_A is 3.0 long (local Z); tile 2 along z=-3..3.
        for (int s=0;s<2;++s) {
            const float px = (s==0) ? -2.5f : 2.5f;
            for (float pz = -3.0f; pz < 3.0f; pz += 3.0f) {
                ckPlaceYaw(m, 0.0f, 1.0f, ckcx(kPipesAabb), kPipesAabb.miny, ckcz(kPipesAabb),
                           px, 2.78f, pz + 1.5f);
                addInstance(pipesA, m);
            }
        }
    }

    // ---- WARM CEILING LIGHTS — the Firefly signature. Amber/orange, low + warm. --
    // Each Light_A fixture also registers a warm point light (this is what does most
    // of the "Serenity" feel). Premultiplied by intensity (linear; mesh.frag adds +
    // tonemaps). Emissive on the fixture mesh so it glows + feeds the bloom chain.
    if (m_table[lightA].ok) {
        // DEEPLY saturated amber so the kit's light-gray PBR surfaces actually pick
        // up a warm tint instead of blooming to neutral white. Lower intensity than
        // a clinical white fill — Firefly is DIM + warm, not bright. Strong R, much
        // lower G, near-zero B (incandescent tungsten, ~2700K and warmer).
        const float kInt = 1.9f;
        const float kR = 1.00f * kInt, kG = 0.46f * kInt, kB = 0.16f * kInt; // amber
        const float kEmis[4] = { 1.00f, 0.50f, 0.18f, 4.0f };
        // Two fixtures over the cockpit + two over the corridor aft.
        const float fz[4] = { -1.6f, 0.6f, 3.6f, 6.4f };
        for (float z : fz) {
            ckPlaceYaw(m, 0.0f, 1.0f, ckcx(kLightAabb), kLightAabb.maxy, ckcz(kLightAabb),
                       0.0f, 2.95f, z);
            addInstance(lightA, m, kEmis);
            x3::rhi::PointLight pl;
            pl.pos[0]=0.0f; pl.pos[1]=2.55f; pl.pos[2]=z; pl.range=7.0f;
            pl.color[0]=kR; pl.color[1]=kG; pl.color[2]=kB;
            m_warm.push_back(pl);
        }
        // A low warm "console uplight" right at the dash so the pilot's seat area
        // glows amber from below (the iconic warm pool under the cockpit window).
        x3::rhi::PointLight dash;
        dash.pos[0]=0.0f; dash.pos[1]=1.0f; dash.pos[2]=-1.2f; dash.range=4.0f;
        dash.color[0]=1.00f*2.0f; dash.color[1]=0.40f*2.0f; dash.color[2]=0.12f*2.0f;
        m_warm.push_back(dash);
    }

    // ---- FORWARD SPACE WINDOW: self-owned deep-space pane (controlled brightness).
    // A 4.4 m x 2.3 m glass at the forward hull opening (z = -3), centered a bit above
    // the dash. Sampling a sub-window of the baked field (overscan) so renderWindow()
    // can drift it. Two-sided so it reads from inside regardless.
    {
        auto spacePx = bakeSpaceRGBA(512);
        m_winTex = device.createTexture(spacePx.data(), 512, 512, /*srgb=*/false);
        x3::rhi::MeshVertex qv[4]; ckQuad(qv, 0.0f, 0.0f, 0.6f, 0.6f);
        const uint32_t qi[12] = { 0,1,2, 0,2,3,  0,2,1, 0,3,2 }; // two-sided
        m_winMesh = device.createMesh(qv, 4, qi, 12);
        // Sit the pane a hair INSIDE the forward opening so it clears the floor/ceiling
        // edges; faces +Z (into the room) so the pilot sees it. yaw = pi -> normal +Z.
        m_winPlace = { 0.0f, 1.85f, -2.78f, 4.4f, 2.3f, kPiF };
        m_winOk = m_winMesh.valid() && m_winTex.valid();
    }

    m_ok = haveFloor && haveWall;
    x3::logInfo("[firefly] built: " + std::to_string(assetsLoaded()) + " asset(s), " +
                std::to_string(m_instances.size()) + " instance(s), " +
                std::to_string(m_warm.size()) + " warm light(s), window=" +
                std::to_string((int)m_winOk));
    return m_ok;
}

void FireflyCockpit::renderWindow(x3::rhi::IRenderDevice& device,
                                  const x3::rhi::FrameContext& frame, float panSec) const {
    if (!m_winOk) return;
    // Gently pan the UV sub-window so the starfield drifts (the ship is "moving").
    const float uPan = std::fmod(panSec * 0.01f, 0.4f);
    const float vPan = 0.12f + 0.004f * std::sin(panSec * 0.2f);
    x3::rhi::MeshVertex qv[4]; ckQuad(qv, uPan, vPan, uPan + 0.6f, vPan + 0.6f);
    device.updateMesh(m_winMesh, qv, 4);
    // Column-major model: scale to (w,h), yaw about +Y, translate to placement.
    const float c = std::cos(m_winPlace[5]), s = std::sin(m_winPlace[5]);
    const float w = m_winPlace[3], h = m_winPlace[4];
    float model[16] = {
        c*w, 0,  -s*w, 0,
        0,   h,  0,    0,
        s,   0,  c,    0,
        m_winPlace[0], m_winPlace[1], m_winPlace[2], 1,
    };
    // MODEST emissive so the dim stars on black read as space, NOT a white sheet.
    // The base field is already mostly black; a low strength keeps it from blooming
    // out while the brightest stars still glint. White multiplier (texture carries
    // the color).
    // mesh.frag adds emissive FLAT (not texture-modulated), so a high emissive would
    // wash the pane to a uniform sheet. Instead keep emissive TINY (a faint deep-space
    // ambient glow) and let the STAR TEXTURE show through ALBEDO — the brightest texels
    // (the baked stars, driven >1) survive even the dim interior lighting as crisp
    // points on near-black space. baseColorFactor 1 so the texture passes through.
    const float col[4]  = { 1.4f, 1.4f, 1.6f, 1.0f };   // lift albedo so stars pop
    const float emis[4] = { 0.05f, 0.05f, 0.08f, 0.6f }; // faint deep-space floor only
    device.drawMeshEmissive(frame, m_winMesh, m_winTex, col, emis, model);
}

void FireflyCockpit::render(x3::rhi::IRenderDevice& device,
                            const x3::rhi::FrameContext& frame) const {
    // WARM BASE TINT — the single biggest Firefly lever. The SM_* kit ships with a
    // cold light-gray/white PBR albedo; multiplying a warm amber tint into every
    // surface's baseColorFactor gives the whole hull the aged-brass / tungsten-lit
    // "used future" cast (so even unlit surfaces read warm, not clinical gray-blue).
    const float kWarmTint[3] = { 1.00f, 0.74f, 0.50f };
    for (const Inst& inst : m_instances) {
        const Asset& a = m_table[inst.asset];
        for (const auto& d : a.drawables) {
            float fin[16];
            x3::asset::mulMat4(inst.transform, d.nodeTransform, fin);
            const float tinted[4] = {
                d.baseColorFactor[0] * kWarmTint[0],
                d.baseColorFactor[1] * kWarmTint[1],
                d.baseColorFactor[2] * kWarmTint[2],
                d.baseColorFactor[3],
            };
            device.drawMeshPBR(frame,
                               x3::rhi::MeshHandle{ d.meshId },
                               x3::rhi::TextureHandle{ d.baseColorTexId },
                               x3::rhi::TextureHandle{ d.normalTexId },
                               x3::rhi::TextureHandle{ d.mrTexId },
                               tinted,
                               inst.emissive,
                               fin);
        }
    }
}

uint32_t FireflyCockpit::assetsLoaded() const {
    uint32_t n=0; for (const auto& a : m_table) if (a.ok) ++n; return n;
}

// ---------------------------------------------------------------------------
// Headless self-test (--test-ship-interior)
// ---------------------------------------------------------------------------
bool runShipInteriorSelfTest() {
    int pass = 0, total = 0;
    auto check = [&](bool c, const char* name) {
        ++total;
        if (c) { ++pass; std::printf("  PASS %s\n", name); }
        else   {          std::printf("  FAIL %s\n", name); }
    };

    // T1: makeSmallCockpit returns >=1 room AND >=1 station.
    ShipManifest sm = ShipInterior::makeSmallCockpit();
    check(sm.rooms.size() >= 1 && sm.stations.size() >= 1,
          "T1 makeSmallCockpit >=1 room + >=1 station");

    // Build it on a headless device + a fresh physics world.
    x3::game::HeadlessRenderDevice device;
    device.init({});
    x3::phys::IPhysicsWorld* phys = x3::phys::createPhysicsWorld();
    phys->init();

    x3::game::Scene scene;
    ShipInterior interior;
    interior.build(device, scene, *phys, sm);

    // T2: build() populated the scene with drawable entities (drawnCount > 0).
    check(interior.entityCount() > 0 && scene.drawnCount() > 0,
          "T2 build populates scene (drawnCount > 0)");

    // T3: manifest() round-trips the counts that were fed in.
    const ShipManifest& got = interior.manifest();
    check(got.rooms.size() == sm.rooms.size() &&
          got.stations.size() == sm.stations.size() &&
          got.doors.size() == sm.doors.size() &&
          got.windows.size() == sm.windows.size(),
          "T3 manifest() round-trips room/station/door/window counts");

    // T4: a LARGE multi-room manifest builds MORE rooms on the SAME system (scope
    // scales with ship class).
    ShipManifest big;
    big.shipClass = ShipClass::Large;
    for (int i = 0; i < 4; ++i) {
        Room r;
        r.name = "R" + std::to_string(i);
        r.boundsMin[0] = (float)(i * 7) - 2.0f; r.boundsMin[1] = 0.0f; r.boundsMin[2] = -2.0f;
        r.boundsMax[0] = (float)(i * 7) + 2.0f; r.boundsMax[1] = 3.0f; r.boundsMax[2] =  2.0f;
        big.rooms.push_back(r);
    }
    big.stations.push_back(Station{ "weapons", { 0.0f, 0.0f, 0.0f }, 0.0f });
    x3::game::Scene bscene;
    ShipInterior bigInterior;
    bigInterior.build(device, bscene, *phys, big);
    check(bigInterior.roomCount() > interior.roomCount(),
          "T4 Large manifest builds more rooms than Small (scope scales)");

    // T5: the spawn point lies INSIDE room 0's bounds.
    {
        const Room& r0 = sm.rooms[0];
        x3::phys::Vec3 sp = interior.spawnPoint();
        bool inside = sp.x >= r0.boundsMin[0] && sp.x <= r0.boundsMax[0] &&
                      sp.z >= r0.boundsMin[2] && sp.z <= r0.boundsMax[2] &&
                      sp.y >= r0.boundsMin[1] - 0.5f && sp.y <= r0.boundsMax[1];
        check(inside, "T5 spawn point lies inside room 0");
    }

    // T6: shutdown() is clean + idempotent (second call must not crash).
    interior.shutdown(*phys);
    bigInterior.shutdown(*phys);
    interior.shutdown(*phys);   // idempotent
    check(!interior.built(), "T6 shutdown clean + idempotent");

    phys->shutdown();
    delete phys;
    device.shutdown();

    std::printf("ship-interior: %d/%d passed\n", pass, total);
    std::fflush(stdout);
    return pass == total;
}

} // namespace x3::space
