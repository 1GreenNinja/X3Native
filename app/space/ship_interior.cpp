// app/space/ship_interior.cpp — S5 walkable, data-driven ship interior.
#include "ship_interior.h"

#include "../mesh_prims.h"        // x3::prims box builders + procedural sci-fi textures
#include "../headless_device.h"   // HeadlessRenderDevice for the self-test

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
        // Retuned for the LINEAR-HDR + ACES + auto-exposure pipeline (pre-HDR 2.0
        // blew the consoles to flat white/green slabs — integration-feast fold).
        e.emissive[0] = er; e.emissive[1] = eg; e.emissive[2] = eb; e.emissive[3] = 0.28f;
        std::memcpy(e.transform, kIdentity, sizeof(kIdentity));
        e.tag = (uint32_t)x3::game::Tag::Prop;
        m_markerIds.push_back(scene.add(e));
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
