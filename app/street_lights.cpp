// STREET LIGHT — real street lamps for the one world. See street_lights.h.
//
// Clean-room: X3Native's own Scene / terrain / prim helpers + the engine
// interfaces only. The cone is a FAKE-VOLUMETRIC surface: an open cone mesh
// (apex at the luminaire, slight trumpet flare, NO end cap) drawn through the
// glass pass's ADDITIVE GLOW mode — axial falloff baked into a gradient
// texture, the soft silhouette from the shader's view-angle rim fade — so it
// reads as light in the air, not a hard glowing traffic cone.
#include "street_lights.h"
#include "mesh_prims.h"
#include "terrain.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace x3::game {

namespace {

constexpr float kPi = 3.14159265358979f;

// Deterministic integer-hash 0..1 (the facility exterior's exact hash).
float h01(uint32_t s) {
    s ^= s >> 16; s *= 0x7feb352du; s ^= s >> 15; s *= 0x846ca68bu; s ^= s >> 16;
    return (float)(s & 0xffffffu) / 16777216.0f;
}

// Per-lamp stable hash keyed on the AUTHORED position + zone (not the build
// index) so dead/flicker/variance survive region stream-out/in and any build
// reordering — the same lamp is always the same lamp.
uint32_t lampHash(float x, float z, StreetLights::Zone zone) {
    const uint32_t hx = (uint32_t)(int32_t)std::lround(x * 4.0f);
    const uint32_t hz = (uint32_t)(int32_t)std::lround(z * 4.0f);
    return hx * 73856093u ^ hz * 19349663u ^ ((uint32_t)zone + 1u) * 83492791u;
}

float smoothstepf(float a, float b, float x) {
    float t = (x - a) / (b - a);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

// Column-major T(pos) * RotY(yaw) * S(sx,sy,sz). RotY maps +Z -> (sin,0,cos).
void composeYawScale(float x, float y, float z, float yaw,
                     float sx, float sy, float sz, float out[16]) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    out[0] = c * sx;  out[1] = 0;      out[2] = -s * sx; out[3] = 0;
    out[4] = 0;       out[5] = sy;     out[6] = 0;       out[7] = 0;
    out[8] = s * sz;  out[9] = 0;      out[10] = c * sz; out[11] = 0;
    out[12] = x;      out[13] = y;     out[14] = z;      out[15] = 1;
}

// ---- Zone material table (the color story) --------------------------------
struct ZoneLook {
    float r, g, b;        // luminaire tint
    float intensity;      // pooled-light intensity
    float range;          // pooled-light falloff radius (m)
    float poolR;          // cone base / pool radius (m)
    float coneStr;        // cone emissive strength (additive HDR)
    float headStr;        // luminaire head emissive strength
    float discStr;        // ground-pool emissive strength
};
const ZoneLook& zoneLook(StreetLights::Zone z) {
    // Calibrated against the dusk shots: the CONE/DISC are whispers (the real
    // pooled light does the lighting); over-bright cones read as solid funnels.
    static const ZoneLook k[(uint32_t)StreetLights::Zone::Count] = {
        { 1.00f, 0.72f, 0.42f,  8.0f, 16.0f, 2.0f, 0.55f, 2.6f, 0.07f },  // Scrapyard sodium
        { 0.80f, 0.90f, 1.00f,  8.5f, 17.0f, 2.2f, 0.52f, 2.8f, 0.07f },  // New District LED
        { 1.00f, 0.70f, 0.38f,  8.0f, 16.0f, 2.0f, 0.55f, 2.5f, 0.07f },  // Industrial sodium
        { 0.84f, 0.92f, 1.00f,  8.0f, 17.0f, 2.1f, 0.50f, 2.7f, 0.06f },  // Approach LED
        { 1.00f, 0.85f, 0.60f,  7.0f, 15.0f, 2.0f, 0.50f, 2.5f, 0.06f },  // Apron warm white
        { 0.88f, 0.95f, 1.00f, 13.0f, 26.0f, 5.0f, 0.85f, 3.4f, 0.14f },  // Dock work light
    };
    return k[(uint32_t)z];
}

// LIGHT CONE baked at WORLD dimensions: apex at the origin, opening DOWN to a
// base ring at y=-drop with base radius `radius`. Profile r(t) = radius *
// (0.10 + 0.90*t^1.35) — a small throat at the luminaire + a slight trumpet
// bow toward the pool, never a dead-straight funnel. OPEN at both ends (no
// caps — a cap is the "solid traffic cone" tell). UV: u = angle, v = t (the
// axial gradient texture rides v).
//
// WHY world-baked (not a scaled unit cone): the mesh path transforms normals
// with the plain model mat3 — a non-uniform (radius, drop, radius) scale
// skews every normal toward +Y (diag6 proof: the whole cone shaded as if
// up-facing), which inverts/flattens the view-angle rim fade and the shot
// read as a HARD solid funnel. Exact world-space normals + a translation-only
// transform sidestep the whole class. `drop` is poleH-0.22 by construction
// (head height is derived from the ground), so one mesh per zone is exact.
void makeConeMesh(float radius, float drop,
                  std::vector<x3::rhi::MeshVertex>& verts, std::vector<uint32_t>& idx) {
    const int kRings = 9, kSegs = 24;
    verts.clear(); idx.clear();
    for (int ri = 0; ri < kRings; ++ri) {
        const float t = (float)ri / (float)(kRings - 1);
        const float r = radius * (0.10f + 0.90f * std::pow(t, 1.35f));
        // Outward normal of the surface of revolution: (radial, dR/d(drop)).
        const float drdy = (t > 0.0f)
            ? radius * 0.90f * 1.35f * std::pow(t, 0.35f) / drop : 0.0f;
        const float nrm = 1.0f / std::sqrt(1.0f + drdy * drdy);
        for (int si = 0; si <= kSegs; ++si) {
            const float a = (float)si / (float)kSegs * 2.0f * kPi;
            const float ca = std::cos(a), sa = std::sin(a);
            x3::rhi::MeshVertex v{};
            v.pos[0] = ca * r; v.pos[1] = -t * drop; v.pos[2] = sa * r;
            v.normal[0] = ca * nrm; v.normal[1] = drdy * nrm; v.normal[2] = sa * nrm;
            v.uv[0] = (float)si / (float)kSegs; v.uv[1] = t;
            verts.push_back(v);
        }
    }
    const int stride = kSegs + 1;
    for (int ri = 0; ri + 1 < kRings; ++ri)
        for (int si = 0; si < kSegs; ++si) {
            const uint32_t a = (uint32_t)(ri * stride + si), b = a + 1;
            const uint32_t c = a + (uint32_t)stride, d = c + 1;
            idx.insert(idx.end(), { a, c, b,  b, c, d });
        }
}

// Unit GROUND-POOL disc: radius 1 at y=0, +Y normal, UV planar so the radial
// gradient texture centers on it.
void makeUnitDisc(std::vector<x3::rhi::MeshVertex>& verts, std::vector<uint32_t>& idx) {
    const int kSegs = 24;
    verts.clear(); idx.clear();
    x3::rhi::MeshVertex c{};
    c.pos[0] = 0; c.pos[1] = 0; c.pos[2] = 0;
    c.normal[1] = 1.0f; c.uv[0] = 0.5f; c.uv[1] = 0.5f;
    verts.push_back(c);
    for (int si = 0; si <= kSegs; ++si) {
        const float a = (float)si / (float)kSegs * 2.0f * kPi;
        x3::rhi::MeshVertex v{};
        v.pos[0] = std::cos(a); v.pos[1] = 0; v.pos[2] = std::sin(a);
        v.normal[1] = 1.0f;
        v.uv[0] = 0.5f + 0.5f * std::cos(a); v.uv[1] = 0.5f + 0.5f * std::sin(a);
        verts.push_back(v);
    }
    for (int si = 0; si < kSegs; ++si)
        idx.insert(idx.end(), { 0u, (uint32_t)(si + 2), (uint32_t)(si + 1) });
}

} // namespace

// ---------------------------------------------------------------------------
// Shared render kit (one per OWNER group — see header on the ledger doctrine).
// ---------------------------------------------------------------------------
StreetLights::Kit StreetLights::makeKit(x3::rhi::IRenderDevice& device) const {
    Kit kit;
    // Unit cube (posts/arms/heads scale it; spans +-0.5 per axis).
    {
        x3::prims::PrimMesh m = x3::prims::makeBox(0.5f, 0.5f, 0.5f, 0, 0, 0, 1.0f);
        kit.unitCube = device.createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                         m.index.data(), (uint32_t)m.index.size());
    }
    {
        std::vector<x3::rhi::MeshVertex> v; std::vector<uint32_t> i;
        makeUnitDisc(v, i);
        kit.disc = device.createMesh(v.data(), (uint32_t)v.size(), i.data(), (uint32_t)i.size());
    }
    // Axial cone gradient (v: 1 at the head -> 0 well BEFORE the base rim, so
    // the cone dissolves in the air — no visible end). LINEAR (srgb=false).
    {
        const int W = 8, H = 64;
        std::vector<uint8_t> px(W * H * 4);
        for (int y = 0; y < H; ++y) {
            // Row-flipped: the upload lands row 0 at v=1 (diag2 proof), so
            // compute the AXIAL coordinate from the far end — v=0 (the apex,
            // sampled at the luminaire) must be the bright end.
            const float v = (float)(H - 1 - y) / (float)(H - 1);
            const float f = std::pow(1.0f - smoothstepf(0.0f, 0.80f, v), 2.2f);
            const uint8_t b = (uint8_t)std::lround(255.0f * f);
            for (int x = 0; x < W; ++x) {
                uint8_t* p = &px[(y * W + x) * 4];
                p[0] = p[1] = p[2] = b; p[3] = 255;
            }
        }
        kit.coneGrad = device.createTexture(px.data(), W, H, false);
    }
    // Radial pool gradient (bright center -> nothing at the rim).
    {
        const int N = 64;
        std::vector<uint8_t> px(N * N * 4);
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x) {
                const float dx = ((float)x + 0.5f) / N - 0.5f;
                const float dy = ((float)y + 0.5f) / N - 0.5f;
                const float r = std::sqrt(dx * dx + dy * dy) * 2.0f;   // 0..1 at rim
                const float f = std::pow(std::max(0.0f, 1.0f - r), 2.2f);
                uint8_t* p = &px[(y * N + x) * 4];
                p[0] = p[1] = p[2] = (uint8_t)std::lround(255.0f * f); p[3] = 255;
            }
        kit.discGrad = device.createTexture(px.data(), N, N, false);
    }
    return kit;
}

// ---------------------------------------------------------------------------
// One lamp: post + arm + head + cone + pool + the lamp record.
// ---------------------------------------------------------------------------
void StreetLights::addLamp(Scene& scene, Kit& kit, x3::rhi::IRenderDevice& device,
                           float x, float z, float groundY,
                           float dirX, float dirZ, Zone zone, bool cityOwned, bool workLight) {
    const ZoneLook& look = zoneLook(zone);
    const uint32_t h = lampHash(x, z, zone);
    const float r0 = h01(h), r1 = h01(h * 2654435761u + 17u), r2 = h01(h * 747796405u + 5u);

    Lamp l;
    l.zone = zone; l.cityOwned = cityOwned; l.workLight = workLight;
    l.state = workLight ? State::Lit
            : (r0 < 0.08f ? State::Dead : (r0 < 0.13f ? State::Flicker : State::Lit));
    // Lived-in variance: +-15% intensity, a slight warmth spread.
    l.intensity = look.intensity * (0.85f + 0.30f * r1);
    l.range = look.range;
    l.color[0] = std::min(1.0f, look.r * (1.0f + 0.06f * (r2 - 0.5f)));
    l.color[1] = look.g;
    l.color[2] = std::min(1.0f, look.b * (1.0f - 0.06f * (r2 - 0.5f)));
    l.rng = h | 1u;
    l.next = 0.9f + 2.6f * h01(h * 2246822519u + 3u);   // first flicker-burst delay

    const float yaw = std::atan2(dirX, dirZ);            // local +Z -> dir
    const float armLen = workLight ? 0.0f : 1.15f;
    const float poleH  = workLight ? 5.3f : 4.9f;
    const float hx = x + dirX * armLen, hz = z + dirZ * armLen;
    const float headY = groundY + poleH - 0.12f;
    l.head[0] = hx; l.head[1] = headY - 0.25f; l.head[2] = hz;

    const bool lit = l.state != State::Dead;
    const float lvl = 1.0f;

    auto addBox = [&](float cx, float cy, float cz, float sx, float sy, float sz,
                      const float col[4], const float* emiss) -> SceneHandle {
        Entity e;
        e.mesh = kit.unitCube;
        e.baseColor[0]=col[0]; e.baseColor[1]=col[1]; e.baseColor[2]=col[2]; e.baseColor[3]=1.0f;
        if (emiss) { e.emissive[0]=emiss[0]; e.emissive[1]=emiss[1];
                     e.emissive[2]=emiss[2]; e.emissive[3]=emiss[3]; }
        composeYawScale(cx, cy, cz, yaw, sx, sy, sz, e.transform);
        e.tag = (uint32_t)Tag::Prop;
        return scene.handle(scene.add(e));
    };

    // Post (galvanized dark steel) + arm reaching over the road.
    const float pole[4] = { 0.34f, 0.35f, 0.38f, 1.0f };
    addBox(x, groundY + poleH * 0.5f, z, workLight ? 0.22f : 0.16f, poleH,
           workLight ? 0.22f : 0.16f, pole, nullptr);
    if (armLen > 0.0f)
        addBox(x + dirX * armLen * 0.5f, groundY + poleH - 0.06f, z + dirZ * armLen * 0.5f,
               0.11f, 0.11f, armLen + 0.12f, pole, nullptr);
    if (workLight)   // crossbar so the rig reads as work gear, not a street pole
        addBox(x, groundY + poleH - 0.55f, z, 1.15f, 0.10f, 0.10f, pole, nullptr);

    // Luminaire head: dark housing, the underside glow is the emissive term.
    {
        const float housing[4] = { 0.10f, 0.10f, 0.11f, 1.0f };
        const float deadHousing[4] = { 0.05f, 0.05f, 0.06f, 1.0f };
        float em[4] = { l.color[0], l.color[1], l.color[2], look.headStr * lvl };
        l.headEnt = addBox(hx, headY, hz,
                           workLight ? 1.05f : 0.62f, workLight ? 0.34f : 0.17f,
                           workLight ? 0.55f : 0.32f,
                           lit ? housing : deadHousing, lit ? em : nullptr);
    }

    if (lit) {
        // THE CONE: additive fake-volumetric shaft, apex at the luminaire.
        {
            // Lazy per-zone world-baked cone (drop = poleH - 0.22 by
            // construction, exact for every lamp of this zone).
            x3::rhi::MeshHandle& coneMesh = kit.cone[(uint32_t)zone];
            const float drop = headY - 0.10f - groundY;
            if (!coneMesh.valid()) {
                std::vector<x3::rhi::MeshVertex> cv; std::vector<uint32_t> ci;
                makeConeMesh(look.poolR, drop, cv, ci);
                coneMesh = device.createMesh(cv.data(), (uint32_t)cv.size(),
                                             ci.data(), (uint32_t)ci.size());
            }
            Entity e;
            e.mesh = coneMesh;
            e.tex  = kit.coneGrad;
            composeYawScale(hx, headY - 0.10f, hz, 0.0f, 1.0f, 1.0f, 1.0f, e.transform);
            e.baseColor[3] = 1.0f;
            e.emissive[0] = l.color[0]; e.emissive[1] = l.color[1]; e.emissive[2] = l.color[2];
            e.emissive[3] = look.coneStr;
            e.transparent = true;
            e.glass.opacity = 0.0f; e.glass.refraction = 0.0f;
            e.glass.roughness = 0.0f; e.glass.specular = 0.0f;
            e.glass.additive = 3.5f;                 // soft silhouette rim fade
            e.tag = (uint32_t)Tag::Prop;
            l.coneEnt = scene.handle(scene.add(e));
        }
        // THE POOL: emissive gradient disc on the asphalt (survives losing the
        // pooled-light budget). additive ~0 => no view-angle fade, so the pool
        // still reads down a grazing street view.
        {
            Entity e;
            e.mesh = kit.disc;
            e.tex  = kit.discGrad;
            composeYawScale(hx, groundY + 0.235f, hz, 0.0f,
                            look.poolR * 0.95f, 1.0f, look.poolR * 0.95f, e.transform);
            e.baseColor[3] = 1.0f;
            e.emissive[0] = l.color[0]; e.emissive[1] = l.color[1]; e.emissive[2] = l.color[2];
            e.emissive[3] = look.discStr;
            e.transparent = true;
            e.glass.opacity = 0.0f; e.glass.refraction = 0.0f;
            e.glass.roughness = 0.0f; e.glass.specular = 0.0f;
            e.glass.additive = 0.05f;
            e.tag = (uint32_t)Tag::Prop;
            l.discEnt = scene.handle(scene.add(e));
        }
    }

    m_lamps.push_back(l);
}

// ---------------------------------------------------------------------------
// Build: the city grid (region-owned) / apron + approach (host-owned).
// ---------------------------------------------------------------------------
void StreetLights::lampRow(Scene& scene, Kit& kit, x3::rhi::IRenderDevice& device,
                           float x0, float z0,
                           float x1, float z1, float spacing, float sideOffset,
                           Zone zone, bool cityOwned) {
    const float dx = x1 - x0, dz = z1 - z0;
    const float len = std::sqrt(dx * dx + dz * dz);
    if (len < 1.0f) return;
    const float ux = dx / len, uz = dz / len;      // along the street
    const float px = -uz, pz = ux;                 // left perpendicular
    (void)ux; (void)uz;
    const int n = std::max(2, (int)(len / spacing) + 1);
    for (int i = 0; i < n; ++i) {
        const float t = ((float)i + 0.5f) / (float)n;
        // Stagger phase chosen so a New District z=500 lamp lands by the
        // authored curb car at (152,494) — a lit pool with a car in it.
        const float side = (i % 2 == 0) ? -1.0f : 1.0f;
        const float x = x0 + dx * t + px * sideOffset * side;
        const float z = z0 + dz * t + pz * sideOffset * side;
        float g[3]; placeOnTerrain(x, z, g);
        addLamp(scene, kit, device, x, z, g[1] + 0.02f,
                -px * side, -pz * side, zone, cityOwned, false);
    }
}

void StreetLights::buildCityLamps(Scene& scene, x3::rhi::IRenderDevice& device) {
    if (m_cityBuilt) return;
    const uint32_t first = (uint32_t)m_lamps.size();
    Kit kit = makeKit(device);

    // SCRAPYARD (warm sodium): the main street + the mid N-S cross street.
    lampRow(scene, kit, device, -758.0f, 500.0f, -442.0f, 500.0f, 32.0f, 9.2f, Zone::Scrapyard, true);
    lampRow(scene, kit, device, -580.0f, 444.0f, -580.0f, 556.0f, 34.0f, 7.2f, Zone::Scrapyard, true);

    // NEW DISTRICT (cool LED): the three E-W mains + Industrial Blvd.
    lampRow(scene, kit, device,  84.0f, 560.0f, 316.0f, 560.0f, 30.0f, 8.2f, Zone::NewDistrict, true);
    lampRow(scene, kit, device,  84.0f, 500.0f, 316.0f, 500.0f, 30.0f, 8.2f, Zone::NewDistrict, true);
    lampRow(scene, kit, device,  84.0f, 440.0f, 316.0f, 440.0f, 30.0f, 8.2f, Zone::NewDistrict, true);
    lampRow(scene, kit, device,  84.0f, 410.0f, 316.0f, 410.0f, 34.0f, 8.2f, Zone::NewDistrict, true);

    // INDUSTRIAL (warm sodium): the cross street + the freeway connector.
    lampRow(scene, kit, device, -256.0f, 350.0f, -144.0f, 350.0f, 32.0f, 6.2f, Zone::Industrial, true);
    lampRow(scene, kit, device, -200.0f, 356.0f, -200.0f, 496.0f, 34.0f, 6.2f, Zone::Industrial, true);

    // THE DOCK WORK LIGHT: a flood rig on the warehouse dock edge, lighting
    // the crate-crew runs (carry lanes x 103..137, z 421..431 — the crew the
    // chatter wave couldn't photograph). Wider, brighter, never dead.
    {
        // In the middle of the work zone (between the two carry lanes, clear
        // of the lane centerlines) so the 5 m flood pool covers both runs.
        float g[3]; placeOnTerrain(122.0f, 426.5f, g);
        addLamp(scene, kit, device, 122.0f, 426.5f, g[1] + 0.02f, 0.0f, -1.0f,
                Zone::Dock, /*cityOwned*/true, /*workLight*/true);
    }

    m_cityBuilt = true;
    logBuild("city (region-owned)", first);
}

void StreetLights::onCityTeardown() {
    if (!m_cityBuilt) return;
    const size_t before = m_lamps.size();
    m_lamps.erase(std::remove_if(m_lamps.begin(), m_lamps.end(),
                                 [](const Lamp& l) { return l.cityOwned; }),
                  m_lamps.end());
    m_cityBuilt = false;
    x3::logInfo("street-lights: " + std::to_string(before - m_lamps.size()) +
                " city lamps abandoned with the region (ledger tears the entities down)");
}

void StreetLights::buildHostLamps(Scene& scene, x3::rhi::IRenderDevice& device,
                                  float apronY, float breachX, float apronZ) {
    if (m_hostBuilt) return;
    const uint32_t first = (uint32_t)m_lamps.size();
    Kit kit = makeKit(device);

    // SPIRE APPROACH road (cool LED, host-owned): the three legs the city
    // grid authors (city.cpp connectors), lamps ~34 m apart.
    lampRow(scene, kit, device, 170.0f, 404.0f, 170.0f, 156.0f, 34.0f, 5.4f, Zone::Approach, false);
    lampRow(scene, kit, device, 164.0f, 150.0f,  28.0f, 150.0f, 34.0f, 5.4f, Zone::Approach, false);
    lampRow(scene, kit, device,  22.0f, 144.0f,  22.0f,  86.0f, 30.0f, 5.4f, Zone::Approach, false);

    // FACILITY APRON (warm white): 3 lamps flanking the breach walk-out,
    // arms facing in over the golden path.
    const float ax[3]  = { breachX - 8.0f, breachX + 8.0f, breachX };
    const float az[3]  = { apronZ + 4.0f,  apronZ + 4.0f,  apronZ + 14.0f };
    const float adx[3] = { 1.0f, -1.0f,  0.0f };
    const float adz[3] = { 0.0f,  0.0f, -1.0f };
    for (int i = 0; i < 3; ++i)
        addLamp(scene, kit, device, ax[i], az[i], apronY, adx[i], adz[i],
                Zone::Apron, false, false);

    m_hostBuilt = true;
    logBuild("host (apron + approach)", first);
}

void StreetLights::logBuild(const char* who, uint32_t first) const {
    uint32_t dead = 0, flick = 0;
    for (size_t i = first; i < m_lamps.size(); ++i) {
        if (m_lamps[i].state == State::Dead) ++dead;
        if (m_lamps[i].state == State::Flicker) ++flick;
    }
    x3::logInfo("street-lights: " + std::string(who) + " built " +
                std::to_string(m_lamps.size() - first) + " lamps (" +
                std::to_string(dead) + " dead, " + std::to_string(flick) +
                " flickering; cones+pools additive via the glass pass)");
}

// ---------------------------------------------------------------------------
// Flicker (irregular 8-13 Hz bursts, deterministic per lamp, dt-scaled).
// ---------------------------------------------------------------------------
void StreetLights::update(float dt, Scene& scene) {
    if (dt <= 0.0f) return;
    for (Lamp& l : m_lamps) {
        if (l.state != State::Flicker) continue;
        auto frand = [&l]() {
            l.rng ^= l.rng << 13; l.rng ^= l.rng >> 17; l.rng ^= l.rng << 5;
            return (float)(l.rng & 0xffffffu) / 16777216.0f;
        };
        l.t += dt;
        float target;
        if (!l.burst) {
            if (l.t >= l.next) {   // enter a burst
                l.burst = true; l.t = 0.0f;
                l.next = 0.22f + 0.5f * frand();              // burst length
                l.period = 1.0f / (8.0f + 5.0f * frand());    // 8-13 Hz
                l.phase = 0.0f; l.on = false;
            }
            target = 1.0f;
        } else {
            l.phase += dt;
            while (l.phase >= l.period) {                     // irregular toggles
                l.phase -= l.period;
                l.on = !l.on;
                l.period = 1.0f / (8.0f + 5.0f * frand());
            }
            if (l.t >= l.next) {   // burst over, settle lit
                l.burst = false; l.t = 0.0f; l.on = true;
                l.next = 0.9f + 2.6f * frand();               // quiet gap
            }
            target = l.on ? 1.0f : 0.10f;
        }
        // Snap fast (a gas-discharge tube dies/strikes in a frame or two).
        const float k = std::min(1.0f, dt * 45.0f);
        l.level += (target - l.level) * k;

        const ZoneLook& look = zoneLook(l.zone);
        if (Entity* e = scene.getChecked(l.coneEnt)) e->emissive[3] = look.coneStr * l.level;
        if (Entity* e = scene.getChecked(l.discEnt)) e->emissive[3] = look.discStr * l.level;
        if (Entity* e = scene.getChecked(l.headEnt)) e->emissive[3] = look.headStr * l.level;
    }
}

// ---------------------------------------------------------------------------
// Nearest-K pooled-light selection.
// ---------------------------------------------------------------------------
uint32_t StreetLights::selectLights(float ex, float ey, float ez,
                                    std::vector<x3::rhi::PointLight>& out, uint32_t k) const {
    if (k == 0 || m_lamps.empty()) return 0;
    std::vector<std::pair<float, uint32_t>> cand;
    cand.reserve(m_lamps.size());
    for (uint32_t i = 0; i < (uint32_t)m_lamps.size(); ++i) {
        const Lamp& l = m_lamps[i];
        if (l.state == State::Dead || l.level < 0.03f) continue;
        const float dx = l.head[0] - ex, dy = l.head[1] - ey, dz = l.head[2] - ez;
        cand.push_back({ dx * dx + dy * dy + dz * dz, i });
    }
    const uint32_t n = std::min<uint32_t>(k, (uint32_t)cand.size());
    std::partial_sort(cand.begin(), cand.begin() + n, cand.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
    for (uint32_t j = 0; j < n; ++j) {
        const Lamp& l = m_lamps[cand[j].second];
        x3::rhi::PointLight pl{};
        pl.pos[0] = l.head[0]; pl.pos[1] = l.head[1]; pl.pos[2] = l.head[2];
        pl.range = l.range;
        const float s = l.intensity * l.level;
        pl.color[0] = l.color[0] * s; pl.color[1] = l.color[1] * s; pl.color[2] = l.color[2] * s;
        out.push_back(pl);
    }
    return n;
}

// ---------------------------------------------------------------------------
// Queries.
// ---------------------------------------------------------------------------
uint32_t StreetLights::lampCount(Zone z) const {
    uint32_t n = 0;
    for (const Lamp& l : m_lamps) if (l.zone == z) ++n;
    return n;
}
uint32_t StreetLights::deadCount() const {
    uint32_t n = 0;
    for (const Lamp& l : m_lamps) if (l.state == State::Dead) ++n;
    return n;
}
uint32_t StreetLights::flickerCount() const {
    uint32_t n = 0;
    for (const Lamp& l : m_lamps) if (l.state == State::Flicker) ++n;
    return n;
}
bool StreetLights::hasDockWorkLight() const {
    for (const Lamp& l : m_lamps)
        if (l.zone == Zone::Dock && l.workLight && l.state == State::Lit) return true;
    return false;
}

} // namespace x3::game
