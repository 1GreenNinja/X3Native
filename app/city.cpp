// EFLZ Act-2 open world — city / industrial metropolis + roads + freeway tunnels.
// See city.h. Clean-room: X3Native's own Scene / terrain / mesh_prims + the engine
// interfaces + the EFLZ design (Tim's own Q3Engine city/freeway modules as content
// reference). No RBDOOM / id Tech / Doom / Quake source. Graybox; mirrors world_regions.cpp.
#include "city.h"
#include "headless_device.h"
#include "mesh_prims.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace x3::game {

namespace {

// One graybox district (center, footprint, tint) + a cluster of building offsets.
struct DistrictDef { const char* name; float cx, cz, radius; float r, g, b; };
const DistrictDef kDistricts[kCityZoneCount] = {
    { "Scrapyard City", -600.0f, 500.0f, 60.0f, 0.45f, 0.40f, 0.32f },   // salvage / ramshackle
    { "New District",    200.0f, 500.0f, 55.0f, 0.55f, 0.56f, 0.60f },   // urban concrete
    { "Industrial Zone",-200.0f, 350.0f, 50.0f, 0.50f, 0.45f, 0.35f },   // factories / refinery
};

// The 4 freeway tunnels (mouth XZ, axis-aligned heading toward a range, bore length).
struct TunnelDef { const char* name; float mx, mz, dx, dz, len; };
const TunnelDef kTunnels[kFreewayTunnelCount] = {
    { "North Freeway Tunnel",   0.0f,  560.0f,  0.0f,  1.0f, 200.0f },  // -> Northern Range
    { "East Freeway Tunnel",  260.0f,  500.0f,  1.0f,  0.0f, 200.0f },  // -> Eastern Range
    { "South Freeway Tunnel", -200.0f, 290.0f,  0.0f, -1.0f, 200.0f },  // -> Southern Range
    { "West Freeway Tunnel",  -660.0f, 500.0f, -1.0f,  0.0f, 200.0f },  // -> Western Highlands
};

} // namespace

void City::build(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics) {
    (void)physics;   // graybox city is visual-only this pass (no collision body)

    auto addBoxProp = [&](float cx, float cy, float cz, float hx, float hy, float hz, const float col[4]) {
        x3::prims::PrimMesh m = x3::prims::makeBox(hx, hy, hz, cx, cy, cz, 0.5f);
        Entity e;
        e.mesh = device.createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                   m.index.data(), (uint32_t)m.index.size());
        e.baseColor[0] = col[0]; e.baseColor[1] = col[1]; e.baseColor[2] = col[2]; e.baseColor[3] = 1.0f;
        e.tag = (uint32_t)Tag::Prop;
        m_props.push_back(scene.add(e));
    };

    // ---- 3 DISTRICTS: each a cluster of graybox buildings on the surface. ----
    for (uint32_t i = 0; i < kCityZoneCount; ++i) {
        const DistrictDef& d = kDistricts[i];
        CityZonePlan& p = m_zones[i];
        p.zone = (CityZone)i; p.name = d.name; p.cx = d.cx; p.cz = d.cz; p.radius = d.radius;
        const float col[4] = { d.r, d.g, d.b, 1.0f };
        const uint32_t before = (uint32_t)m_props.size();
        // A 3x2 grid of buildings with varied heights, centered on the district.
        const float bw = 7.0f, gap = d.radius * 0.5f;
        const float bh[6] = { 14.0f, 9.0f, 20.0f, 11.0f, 16.0f, 8.0f };  // building heights (m)
        int k = 0;
        for (int gx = -1; gx <= 1; ++gx) for (int gz = 0; gz <= 1; ++gz) {
            float s[3]; placeOnTerrain(d.cx + gx * gap, d.cz + (gz - 0.5f) * gap, s);
            addBoxProp(s[0], s[1] + bh[k] * 0.5f, s[2], bw * 0.5f, bh[k] * 0.5f, bw * 0.5f, col);
            ++k;
        }
        p.buildingCount = (uint32_t)m_props.size() - before;
    }

    // ---- ROAD GRID: the E-W freeway (Z=500) + connector spurs to each district.
    // Roads are thin flat strips laid on the surface. ----
    const float roadCol[4] = { 0.10f, 0.10f, 0.11f, 1.0f };   // asphalt
    auto addRoad = [&](float x0, float z0, float x1, float z1, float halfW) {
        const float cx = (x0 + x1) * 0.5f, cz = (z0 + z1) * 0.5f;
        const float hx = std::fabs(x1 - x0) * 0.5f + ((x1 == x0) ? halfW : 0.0f);
        const float hz = std::fabs(z1 - z0) * 0.5f + ((z1 == z0) ? halfW : 0.0f);
        float s[3]; placeOnTerrain(cx, cz, s);
        addBoxProp(cx, s[1] + 0.1f, cz, hx, 0.1f, hz, roadCol);
        ++m_roadSegments;
    };
    addRoad(-680.0f, 500.0f, 280.0f, 500.0f, 6.0f);    // main E-W freeway @ Z=500
    addRoad(-600.0f, 470.0f, -600.0f, 530.0f, 5.0f);   // Scrapyard spur
    addRoad( 200.0f, 470.0f,  200.0f, 530.0f, 5.0f);   // New District spur
    addRoad(-200.0f, 350.0f, -200.0f, 500.0f, 5.0f);   // Industrial -> freeway connector
    addRoad(-200.0f, 350.0f,  -60.0f, 350.0f, 5.0f);   // Industrial cross street

    // ---- 4 FREEWAY TUNNELS: a bore + a mouth portal heading toward each range. ----
    const float tunCol[4]   = { 0.30f, 0.30f, 0.33f, 1.0f };   // concrete
    const float mouthCol[4] = { 0.22f, 0.22f, 0.24f, 1.0f };
    const float tunHalfW = 7.0f, tunHalfH = 5.0f;
    for (uint32_t i = 0; i < kFreewayTunnelCount; ++i) {
        const TunnelDef& t = kTunnels[i];
        FreewayTunnelPlan& fp = m_tunnels[i];
        fp.name = t.name; fp.mouthX = t.mx; fp.mouthZ = t.mz; fp.dirX = t.dx; fp.dirZ = t.dz; fp.length = t.len;
        float ms[3]; placeOnTerrain(t.mx, t.mz, ms);
        // Mouth portal (a frame block at the mouth).
        addBoxProp(t.mx, ms[1] + tunHalfH, t.mz,
                   (t.dx != 0.0f ? 2.0f : tunHalfW + 1.5f), tunHalfH + 1.0f,
                   (t.dz != 0.0f ? 2.0f : tunHalfW + 1.5f), mouthCol);
        // Bore: a long box from the mouth heading dir*length.
        const float bx = t.mx + t.dx * t.len * 0.5f;
        const float bz = t.mz + t.dz * t.len * 0.5f;
        float bs[3]; placeOnTerrain(bx, bz, bs);
        addBoxProp(bx, bs[1] + tunHalfH, bz,
                   (t.dx != 0.0f ? t.len * 0.5f : tunHalfW),
                   tunHalfH,
                   (t.dz != 0.0f ? t.len * 0.5f : tunHalfW), tunCol);
    }

    m_built = true;
    x3::logInfo("City::build complete — 3 districts (Scrapyard / New District / Industrial) + "
                + std::to_string(m_roadSegments) + " road segments + 4 freeway tunnels; "
                + std::to_string((uint32_t)m_props.size()) + " graybox props");
}

// ===========================================================================
// Headless self-test (--test-city).
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const std::string& name) {
    if (cond) { ++g_pass; x3::logInfo("[city-test] PASS " + name); }
    else      { ++g_fail; x3::logError("[city-test] FAIL " + name); }
}
using HeadlessDevice = x3::game::HeadlessRenderDevice;
bool nonEmpty(const char* s) { return s && s[0] != '\0'; }

} // namespace

bool runCitySelfTest() {
    g_pass = g_fail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();
    HeadlessDevice device;
    Scene scene;

    City c;
    c.build(scene, device, *physics);

    check(c.built(), "C0 city built");

    // ---- 3 districts present, named, at authored centers, each with buildings. ----
    {
        bool ok = true;
        for (uint32_t i = 0; i < kCityZoneCount; ++i) {
            const CityZonePlan& z = c.zone((CityZone)i);
            if (!nonEmpty(z.name) || z.buildingCount == 0) ok = false;
        }
        bool centers = std::fabs(c.zone(CityZone::ScrapyardCity).cx - (-600.0f)) < 1.0f &&
                       std::fabs(c.zone(CityZone::NewDistrict).cx   - ( 200.0f)) < 1.0f;
        check(ok && centers, "C1 3 districts named + populated, at authored centers");
    }

    // ---- A road grid exists. ----
    check(c.roadSegmentCount() > 0, "C2 road grid built (" + std::to_string(c.roadSegmentCount()) + " segments)");

    // ---- Exactly 4 freeway tunnels, each with a unit heading + nonzero length. ----
    {
        bool ok = c.tunnelCount() == kFreewayTunnelCount;
        for (uint32_t i = 0; i < c.tunnelCount(); ++i) {
            const FreewayTunnelPlan& t = c.tunnel(i);
            const float mag = std::sqrt(t.dirX * t.dirX + t.dirZ * t.dirZ);
            if (t.length <= 0.0f || std::fabs(mag - 1.0f) > 1e-3f || !nonEmpty(t.name)) ok = false;
        }
        check(ok, "C3 four freeway tunnels (unit heading + nonzero bore length)");
    }

    // ---- Props placed. ----
    check(c.propCount() > 0, "C4 graybox props placed (total " + std::to_string(c.propCount()) + ")");

    physics->shutdown();
    x3::logInfo(std::string("city: ") + std::to_string(g_pass) + "/" +
                std::to_string(g_pass + g_fail) + " passed");
    return g_fail == 0;
}

} // namespace x3::game
