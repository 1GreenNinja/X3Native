// Club 1127 BEDROCK ENCASEMENT — see club_bedrock.h for the full rationale.
#include "club_bedrock.h"
#include "mesh_prims.h"
#include "elevator.h"     // ElevatorSystem::strata() — the 9 geology bands the fall shaft walls are colored by

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace x3::game {

namespace {

// ---- Procedural mottled rock/dirt albedo -------------------------------------
// A brown/grey value-noise texture so the earth reads as raw rock, not a flat wall.
// Self-contained (no pack asset needed) so the headless self-test / fresh clones
// never break. Two octaves of hash noise -> a rough dirt-and-stone mottle.
inline float hash2(int x, int y) {
    uint32_t h = (uint32_t)(x * 374761393 + y * 668265263);
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xFFFFFF) / (float)0xFFFFFF;
}
inline float smoothNoise(float u, float v, int period) {
    float fx = u * period, fy = v * period;
    int x0 = (int)fx, y0 = (int)fy;
    float tx = fx - x0, ty = fy - y0;
    // wrap for a tileable texture
    auto H = [&](int a, int b) { return hash2((a % period + period) % period,
                                              (b % period + period) % period); };
    float a = H(x0, y0),     b = H(x0 + 1, y0);
    float c = H(x0, y0 + 1), d = H(x0 + 1, y0 + 1);
    float sx = tx * tx * (3 - 2 * tx), sy = ty * ty * (3 - 2 * ty);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
}
std::vector<uint8_t> makeRockRGBA(uint32_t n) {
    std::vector<uint8_t> px(n * n * 4);
    for (uint32_t y = 0; y < n; ++y)
        for (uint32_t x = 0; x < n; ++x) {
            float u = (float)x / n, v = (float)y / n;
            float coarse = smoothNoise(u, v, 8);
            float fine   = smoothNoise(u, v, 32);
            float g = 0.55f * coarse + 0.45f * fine;     // 0..1 rock value
            // dark earthy palette: brown base, grey grit streaks, dark crevices
            float r = 0.34f + 0.40f * g;
            float gr = 0.26f + 0.34f * g;
            float b = 0.20f + 0.26f * g;
            // deepen crevices (low-noise pockets) toward near-black dirt
            float crev = smoothNoise(u * 1.7f + 3.1f, v * 1.7f + 7.7f, 16);
            float dark = 0.45f + 0.55f * crev;
            r *= dark; gr *= dark; b *= dark;
            auto clamp8 = [](float f) { return (uint8_t)(f < 0 ? 0 : f > 1 ? 255 : f * 255.0f + 0.5f); };
            uint32_t i = (y * n + x) * 4;
            px[i + 0] = clamp8(r); px[i + 1] = clamp8(gr); px[i + 2] = clamp8(b); px[i + 3] = 255;
        }
    return px;
}

// ---- Double-sided solid rock box ---------------------------------------------
// prims::makeBox authors OUTWARD (CCW) faces only; the main mesh pipeline culls
// back faces, so from inside the box you'd see nothing. Emit the SAME faces a
// second time with reversed winding + flipped normals so the box is visible from
// BOTH sides — noclip INTO the solid and rock is all around you.
x3::prims::PrimMesh makeSolidRockBox(float hx, float hy, float hz,
                                     float cx, float cy, float cz, float uvScale) {
    x3::prims::PrimMesh m = x3::prims::makeBox(hx, hy, hz, cx, cy, cz, uvScale);
    const uint32_t vbase = (uint32_t)m.verts.size();
    const uint32_t icnt  = (uint32_t)m.index.size();
    // duplicate verts with negated normals (inner-facing copy)
    m.verts.reserve(vbase * 2);
    for (uint32_t i = 0; i < vbase; ++i) {
        x3::rhi::MeshVertex vv = m.verts[i];
        vv.normal[0] = -vv.normal[0]; vv.normal[1] = -vv.normal[1]; vv.normal[2] = -vv.normal[2];
        m.verts.push_back(vv);
    }
    // reversed-winding triangles referencing the duplicated verts
    m.index.reserve(icnt * 2);
    for (uint32_t t = 0; t < icnt; t += 3) {
        m.index.push_back(vbase + m.index[t + 0]);
        m.index.push_back(vbase + m.index[t + 2]);
        m.index.push_back(vbase + m.index[t + 1]);
    }
    // Collision geometry is left untouched (outward faces only) — but the caller
    // adds the rock as NON-colliding anyway (visual shell), so it never matters.
    return m;
}

// ---- Lumpy natural cave TUBE (REAL TUBE GEOMETRY, feat/cave-atmosphere #3) -----
// A double-sided (seen from INSIDE), NON-colliding rock tube running along the X
// axis from x0..x1, centered on (cy,cz). The bore RADIUS varies along the length —
// PINCHED at the two ends (rMin, a tight crawl) and BELLYING OUT to a cathedral
// cavern (rMax) in the middle — plus per-ring hash lumps so the walls are knobbly
// rock, not a smooth pipe. That is the tight->vast->tight rhythm the box corridors
// lacked. Inner-facing faces (the crystal light catches the wall you're looking at);
// a second outward copy makes it solid from the far side too (noclip-proof, like
// makeSolidRockBox). Render-only visual shell — a flat colliding floor is laid
// separately so navigation never fights the round bore.
inline float tubeHash(int a, int b) {
    uint32_t h = (uint32_t)(a * 374761393 + b * 668265263);
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}
x3::prims::PrimMesh makeCaveTubeX(float x0, float x1, float cy, float cz,
                                  float rMin, float rMax, float lump,
                                  int rings, int seg, float uvScale, uint32_t sd) {
    x3::prims::PrimMesh m;
    const float kTwoPi = 6.28318531f, kPiL = 3.14159265f;
    auto rad = [&](int i, int s) -> float {
        const float t = (float)i / (float)rings;             // 0..1 along length
        const float belly = std::sin(t * kPiL);              // 0 ends, 1 middle: pinch->cathedral->pinch
        const float base = rMin + (rMax - rMin) * belly;
        const float n = 0.5f * tubeHash(i + (int)sd, s) + 0.5f * tubeHash(i * 3 + 1, s * 2 + (int)sd);
        return base * (1.0f + lump * (n - 0.5f));
    };
    std::vector<std::vector<std::array<float,3>>> rp((size_t)rings + 1);
    for (int i = 0; i <= rings; ++i) {
        const float x = x0 + (x1 - x0) * (float)i / (float)rings;
        rp[(size_t)i].resize((size_t)seg);
        for (int s = 0; s < seg; ++s) {
            const float a = kTwoPi * (float)s / (float)seg;
            const float r = rad(i, s);
            rp[(size_t)i][(size_t)s] = { x, cy + std::sin(a) * r, cz + std::cos(a) * r };
        }
    }
    auto emit = [&](const std::array<float,3>& A, const std::array<float,3>& B,
                    const std::array<float,3>& C, float u0, float v0, float u1, float v1, float u2, float v2) {
        // inward normal (toward the tube axis line at (·,cy,cz)) so the crystal light
        // shades the wall the player faces from inside; nx=0 (no axial component).
        float ny = cy - (A[1]+B[1]+C[1])/3.0f, nz = cz - (A[2]+B[2]+C[2])/3.0f;
        float l = std::sqrt(ny*ny + nz*nz); if (l < 1e-5f) l = 1.0f; ny /= l; nz /= l;
        const uint32_t base = (uint32_t)m.verts.size();
        m.verts.push_back({{A[0],A[1],A[2]},{0,ny,nz},{u0,v0}});
        m.verts.push_back({{B[0],B[1],B[2]},{0,ny,nz},{u1,v1}});
        m.verts.push_back({{C[0],C[1],C[2]},{0,ny,nz},{u2,v2}});
        m.index.insert(m.index.end(), { base, base+1, base+2 });
        // outward copy (reversed winding, flipped normal) — solid from both sides.
        const uint32_t b2 = (uint32_t)m.verts.size();
        m.verts.push_back({{A[0],A[1],A[2]},{0,-ny,-nz},{u0,v0}});
        m.verts.push_back({{C[0],C[1],C[2]},{0,-ny,-nz},{u2,v2}});
        m.verts.push_back({{B[0],B[1],B[2]},{0,-ny,-nz},{u1,v1}});
        m.index.insert(m.index.end(), { b2, b2+1, b2+2 });
    };
    for (int i = 0; i < rings; ++i)
        for (int s = 0; s < seg; ++s) {
            const int s1 = (s + 1) % seg;
            const auto& A = rp[(size_t)i][(size_t)s];
            const auto& B = rp[(size_t)i][(size_t)s1];
            const auto& C = rp[(size_t)i+1][(size_t)s1];
            const auto& D = rp[(size_t)i+1][(size_t)s];
            const float u = (x1 - x0) * uvScale * (float)i / (float)rings;
            const float u2 = (x1 - x0) * uvScale * (float)(i + 1) / (float)rings;
            const float v = (float)s * uvScale, v1 = (float)s1 * uvScale;
            emit(A, B, C, u, v, u, v1, u2, v1);
            emit(A, C, D, u, v, u2, v1, u2, v);
        }
    return m;
}

// ---- A bare CRYSTAL CLUSTER (no rock pocket) — for embedding glowing shards in an
// EXISTING wall / cavern (the shaft walls, tube bellies, landmarks). `n` shards,
// `scale` size, emissive Salvari-blue; appends a blue point light to outLights.
void addCrystalCluster(Scene& scene, x3::rhi::IRenderDevice& device,
                       float cx, float cy, float cz, float scale, int n,
                       float lightRange, std::vector<x3::rhi::PointLight>* outLights,
                       float upDir = 1.0f) {
    const float blueTint[4] = { 0.02f, 0.07f, 0.52f, 1.0f };
    const float blueEmit[4] = { 0.015f, 0.07f, 0.62f, 1.0f };
    for (int k = 0; k < n; ++k) {
        const float ph = (float)k * 2.39996f;               // golden-angle fan
        const float rr = 0.20f + 0.55f * ((k * 37) % 100) / 100.0f;
        const float dx = std::cos(ph) * rr * scale, dz = std::sin(ph) * rr * scale;
        const float sc = (0.5f + 0.5f * (((k * 53) % 100) / 100.0f)) * scale;
        x3::prims::PrimMesh geo = x3::prims::makeCrystal(0.32f * sc, 0.9f * sc, 0.9f * sc);
        Entity e;
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
        for (int i = 0; i < 4; ++i) e.baseColor[i] = blueTint[i];
        for (int i = 0; i < 4; ++i) e.emissive[i]  = blueEmit[i];
        e.tag = (uint32_t)Tag::Static; e.body = x3::phys::BodyId{};
        const float tilt = (((k * 29) % 100) / 100.0f - 0.5f) * 0.7f;
        const float ct = std::cos(tilt), st = std::sin(tilt) * upDir;
        e.transform[0]=ct; e.transform[1]=st; e.transform[2]=0; e.transform[3]=0;
        e.transform[4]=-st;e.transform[5]=ct*upDir; e.transform[6]=0; e.transform[7]=0;
        e.transform[8]=0;  e.transform[9]=0;  e.transform[10]=1;e.transform[11]=0;
        e.transform[12]=cx+dx; e.transform[13]=cy+0.9f*sc*upDir; e.transform[14]=cz+dz;
        e.transform[15]=1.0f;
        scene.add(e);
    }
    if (outLights) {
        x3::rhi::PointLight l;
        l.pos[0]=cx; l.pos[1]=cy+0.6f*scale; l.pos[2]=cz;
        l.range=lightRange; l.color[0]=0.12f; l.color[1]=0.34f; l.color[2]=1.90f;
        outLights->push_back(l);
    }
}

// ---- Ancient Salvari crystal hollow ------------------------------------------
// A small rock pocket carved in the strata holding a cluster of glowing-blue
// crystal shards (the Salvari crystal-people's deposits). Adds the pocket liner +
// the crystal cluster geometry to the scene and returns the pocket's blue point
// light (the host merges it into the per-frame light set).
x3::rhi::PointLight addSalvariHollow(Scene& scene, x3::rhi::IRenderDevice& device,
                                     x3::rhi::TextureHandle rockTex,
                                     const BedrockConfig& cfg,
                                     float cx, float cy, float cz, float scale) {
    // 1) POCKET: a small double-sided rock room liner so the hollow has near rock
    //    walls the blue light catches (the surrounding earth is a distant shell).
    {
        const float h = 5.5f * scale;
        x3::prims::PrimMesh geo = makeSolidRockBox(h, h * 0.8f, h, cx, cy, cz, cfg.uvScale * 1.6f);
        Entity e;
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
        e.tex = rockTex;
        for (int i = 0; i < 4; ++i) e.baseColor[i] = cfg.tint[i];
        // pocket walls a touch darker than bulk rock so the blue crystal read pops
        for (int i = 0; i < 3; ++i) e.emissive[i] = cfg.emissive[i] * 0.6f;
        e.emissive[3] = 1.0f;
        e.tag  = (uint32_t)Tag::Static;
        e.body = x3::phys::BodyId{};
        scene.add(e);
    }
    // 2) CRYSTAL CLUSTER: a few angular shards (hex bipyramids) fanned around the
    //    pocket centre, emissive SALVARI-BLUE — a deep saturated sapphire/electric
    //    blue that reads BLUE at the core, not a blown-out white.
    //    WHY THE OLD VALUES WENT WHITE: emissive contributes `rgb * strength` to the
    //    HDR color (mesh.frag: color += emis * vEmissive.a). The old {0.10,0.40,1.15}
    //    x 1.5 = final {0.15,0.60,1.725} — the blue channel at 1.7 sat FAR past 1.0,
    //    so the ACES tonemap + bloom stack desaturated the bright core toward WHITE
    //    (and the 0.60 green tugged it cyan). Fix: keep the peak channel UNDER 1.0 so
    //    ACES holds the hue, push the ratio hard to blue (blue >> green >> ~0 red) for
    //    a rich sapphire, and let the point light below carry the blue GLOW halo.
    const float blueTint[4] = { 0.02f, 0.07f, 0.52f, 1.0f };   // deep sapphire base (lit)
    const float blueEmit[4] = { 0.015f, 0.07f, 0.62f, 1.0f };  // DEEP saturated blue core: low G/R kills the
                                                               // cyan/pastel, blue held ~0.6 (bright but not blown)
    struct Shard { float dx, dz, s, r, mid, tip, tilt; };
    const Shard shards[6] = {
        {  0.0f,  0.0f, 1.00f, 0.55f, 1.4f, 1.1f,  0.00f },
        {  1.3f,  0.6f, 0.70f, 0.38f, 0.9f, 0.9f,  0.28f },
        { -1.1f,  0.9f, 0.62f, 0.34f, 0.8f, 0.8f, -0.24f },
        {  0.7f, -1.2f, 0.78f, 0.42f, 1.1f, 1.0f,  0.16f },
        { -1.4f, -0.7f, 0.55f, 0.30f, 0.7f, 0.7f, -0.34f },
        {  0.2f,  1.5f, 0.66f, 0.36f, 1.0f, 0.9f,  0.10f },
    };
    for (const Shard& sh : shards) {
        x3::prims::PrimMesh geo = x3::prims::makeCrystal(sh.r * scale, sh.mid * scale, sh.tip * scale);
        Entity e;
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
        for (int i = 0; i < 4; ++i) e.baseColor[i] = blueTint[i];
        for (int i = 0; i < 4; ++i) e.emissive[i]  = blueEmit[i];
        e.tag  = (uint32_t)Tag::Static;
        e.body = x3::phys::BodyId{};
        // place + tilt via the model transform (crystal authored at local origin).
        const float ct = std::cos(sh.tilt), st = std::sin(sh.tilt);
        e.transform[0] = ct;  e.transform[1] = st;  e.transform[2]  = 0; e.transform[3]  = 0;
        e.transform[4] = -st; e.transform[5] = ct;  e.transform[6]  = 0; e.transform[7]  = 0;
        e.transform[8] = 0;   e.transform[9] = 0;   e.transform[10] = 1; e.transform[11] = 0;
        e.transform[12] = cx + sh.dx * scale;
        e.transform[13] = cy - 3.0f * scale + sh.mid * scale;   // sprout up from the pocket floor
        e.transform[14] = cz + sh.dz * scale;
        e.transform[15] = 1.0f;
        scene.add(e);
    }
    // 3) BLUE POINT LIGHT pooling in the hollow — the blue GLOW HALO on the rock.
    //    Kept (Salvari signature) but shifted BLUER (less green) so the pool reads as
    //    a deep-blue halo around the sapphire cores, not a cyan wash.
    x3::rhi::PointLight l;
    l.pos[0] = cx; l.pos[1] = cy; l.pos[2] = cz;
    l.range  = 16.0f * scale;
    l.color[0] = 0.12f; l.color[1] = 0.34f; l.color[2] = 1.90f;   // bluer halo (less cyan)
    return l;
}

} // namespace

int buildClubBedrock(Scene& scene, x3::rhi::IRenderDevice& device,
                     x3::phys::IPhysicsWorld& physics, const BedrockConfig& cfg,
                     std::vector<x3::rhi::PointLight>* outCrystalLights) {
    (void)physics;   // rock is purely visual (non-colliding) — see header.

    // Shared procedural rock texture (one upload, reused by all six slabs).
    const uint32_t kN = 256;
    auto rockPx = makeRockRGBA(kN);
    x3::rhi::TextureHandle rockTex = device.createTexture(rockPx.data(), kN, kN, true);

    const float ov  = cfg.weld;
    const float uvs = cfg.uvScale;

    // Outer extent of the whole earth block (host-sized from the city footprint +
    // surface; spans from below the club up to the surface ground plane).
    const float oMinX = cfg.earthMinX, oMaxX = cfg.earthMaxX;
    const float oMinY = cfg.earthMinY, oMaxY = cfg.earthMaxY;
    const float oMinZ = cfg.earthMinZ, oMaxZ = cfg.earthMaxZ;

    int added = 0;
    // Add one double-sided, non-colliding rock slab spanning [x0,x1]x[y0,y1]x[z0,z1].
    auto slab = [&](float x0, float x1, float y0, float y1, float z0, float z1) {
        const float hx = (x1 - x0) * 0.5f, hy = (y1 - y0) * 0.5f, hz = (z1 - z0) * 0.5f;
        const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, cz = (z0 + z1) * 0.5f;
        x3::prims::PrimMesh geo = makeSolidRockBox(hx, hy, hz, cx, cy, cz, uvs);
        Entity e;
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
        e.tex = rockTex;                                  // procedural rock albedo
        for (int i = 0; i < 4; ++i) e.baseColor[i] = cfg.tint[i];
        for (int i = 0; i < 4; ++i) e.emissive[i]  = cfg.emissive[i];
        e.tag  = (uint32_t)Tag::Static;
        e.body = x3::phys::BodyId{};                      // invalid => non-colliding
        scene.add(e);
        ++added;
    };

    // BASE (below the floor) and CAP (above the ceiling) — full padded footprint.
    slab(oMinX, oMaxX, oMinY,             cfg.cavMinY + ov, oMinZ, oMaxZ);   // base
    slab(oMinX, oMaxX, cfg.cavMaxY - ov,  oMaxY,            oMinZ, oMaxZ);   // cap
    // FOUR WALLS between base and cap (they weld into cap/base + each other at the
    // corners; overlaps are buried inside rock so nothing z-fights or shows a gap).
    slab(oMinX,            cfg.cavMinX + ov, cfg.cavMinY - ov, cfg.cavMaxY + ov, oMinZ, oMaxZ); // -X
    slab(cfg.cavMaxX - ov, oMaxX,            cfg.cavMinY - ov, cfg.cavMaxY + ov, oMinZ, oMaxZ); // +X
    slab(oMinX, oMaxX, cfg.cavMinY - ov, cfg.cavMaxY + ov, oMinZ,            cfg.cavMinZ + ov); // -Z
    slab(oMinX, oMaxX, cfg.cavMinY - ov, cfg.cavMaxY + ov, cfg.cavMaxZ - ov, oMaxZ);            // +Z

    // ---- ANCIENT SALVARI CRYSTAL HOLLOWS -------------------------------------
    // A handful of glowing-blue crystal pockets scattered through the dead earth
    // at varied depths/positions — discovered by digging/noclipping. Two sit just
    // outside the club walls (where noclipping out lands you), two up in the strata
    // toward the surface (dig UP), two down in the base rock. Positions are world-
    // space, chosen to sit in solid earth (outside the club cavity, inside the
    // block).
    if (cfg.salvariCrystals) {
        // Ys are expressed RELATIVE to the cavity (club) so the hollows stay near the
        // club at whatever depth it sits (relocated to Y=-800, 2026-07) instead of the
        // old hardcoded ~-200 band. cy0 = cavity mid; dig UP toward the strata / city
        // and DOWN into the base rock from there.
        const float cy0 = (cfg.cavMinY + cfg.cavMaxY) * 0.5f;
        struct Hollow { float x, y, z, s; };
        const Hollow hollows[6] = {
            {  cfg.cavMaxX + 22.0f, cy0 + 1.0f,        6.0f,               1.10f },  // just E of the club
            { -12.0f,               cy0,               cfg.cavMinZ - 24.0f, 0.95f }, // just N of the club
            {  38.0f,               cy0 + 75.0f,       46.0f,              1.30f },  // up in the strata (dig up)
            { -64.0f,               cy0 + 130.0f,      150.0f,             1.15f },  // higher, under the city
            {  46.0f,               cfg.cavMinY - 30.0f, 34.0f,            1.00f },  // down in the base rock
            { -44.0f,               cfg.cavMinY - 15.0f, -34.0f,           0.85f },  // down, other side
        };
        for (const Hollow& h : hollows) {
            x3::rhi::PointLight bl =
                addSalvariHollow(scene, device, rockTex, cfg, h.x, h.y, h.z, h.s);
            added += 7;   // pocket + 6 shards per hollow (bookkeeping)
            if (outCrystalLights) outCrystalLights->push_back(bl);
        }
    }
    return added;
}

// ============================================================================
// THE DESCENT FALL (feat/descent-fall) — see club_bedrock.h. A VERTICAL FALL SHAFT
// (surface mouth -> dark landing room just above the club), strata-colored walls, a
// keypad DOOR -> HALL -> ELEVATOR alcove into the club, + a few SIDE-SHOOT rooms off
// the shaft. REPLACES the old walkable switchback ramp. Positions are published into
// `outLayout` for the interactive descent_fall layer.
// ============================================================================
int buildEarthTunnels(Scene& scene, x3::rhi::IRenderDevice& device,
                      x3::phys::IPhysicsWorld& physics, const TunnelConfig& tc,
                      std::vector<x3::rhi::PointLight>* outCrystalLights,
                      DescentFallLayout* outLayout) {
    // Shared procedural rock albedo (same generator the earth uses, so the bore rock
    // matches its surround). One upload, reused by every tunnel surface.
    const uint32_t kN = 256;
    auto rockPx = makeRockRGBA(kN);
    x3::rhi::TextureHandle rockTex = device.createTexture(rockPx.data(), kN, kN, true);

    int added = 0;

    // ---- authoring primitives ------------------------------------------------
    // VISUAL rock slab spanning the axis-aligned box [x0,x1]x[y0,y1]x[z0,z1] — the
    // double-sided, NON-colliding earth shell (walls / ceilings / caps).
    auto addVis = [&](float x0, float x1, float y0, float y1, float z0, float z1) {
        const float hx = (x1 - x0) * 0.5f, hy = (y1 - y0) * 0.5f, hz = (z1 - z0) * 0.5f;
        const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, cz = (z0 + z1) * 0.5f;
        if (hx <= 0.0f || hy <= 0.0f || hz <= 0.0f) return;      // skip degenerate frames
        x3::prims::PrimMesh geo = makeSolidRockBox(hx, hy, hz, cx, cy, cz, tc.uvScale);
        Entity e;
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
        e.tex = rockTex;
        for (int i = 0; i < 4; ++i) e.baseColor[i] = tc.tint[i];
        for (int i = 0; i < 4; ++i) e.emissive[i]  = tc.emissive[i];
        e.tag  = (uint32_t)Tag::Static;
        e.body = x3::phys::BodyId{};                             // non-colliding shell
        scene.add(e); ++added;
    };
    // COLLIDING rock box (Jolt static mesh) — the WALKABLE floors / landings.
    auto addCol = [&](float x0, float x1, float y0, float y1, float z0, float z1) {
        const float hx = (x1 - x0) * 0.5f, hy = (y1 - y0) * 0.5f, hz = (z1 - z0) * 0.5f;
        const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, cz = (z0 + z1) * 0.5f;
        if (hx <= 0.0f || hy <= 0.0f || hz <= 0.0f) return;
        x3::prims::PrimMesh geo = x3::prims::makeBox(hx, hy, hz, cx, cy, cz, tc.uvScale);
        Entity e;
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
        e.tex = rockTex;
        for (int i = 0; i < 4; ++i) e.baseColor[i] = tc.tint[i];
        for (int i = 0; i < 4; ++i) e.emissive[i]  = tc.emissive[i];
        e.tag  = (uint32_t)Tag::Static;
        e.body = physics.addStaticMesh(geo.cverts.data(), (uint32_t)(geo.cverts.size() / 3),
                                       geo.cindex.data(), (uint32_t)geo.cindex.size());
        scene.add(e); ++added;
    };
    // COLORED colliding rock box (variant of addCol with an explicit tint/emissive —
    // the fall-shaft walls are colored per STRATA band so the geology rushes past as
    // you drop; a glowing band carries its emissive so it lights the bore).
    auto addColC = [&](float x0, float x1, float y0, float y1, float z0, float z1,
                       const float col[3], const float em[3]) {
        const float hx = (x1 - x0) * 0.5f, hy = (y1 - y0) * 0.5f, hz = (z1 - z0) * 0.5f;
        const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, cz = (z0 + z1) * 0.5f;
        if (hx <= 0.0f || hy <= 0.0f || hz <= 0.0f) return;
        x3::prims::PrimMesh geo = makeSolidRockBox(hx, hy, hz, cx, cy, cz, tc.uvScale);
        Entity e;
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
        e.tex = rockTex;
        e.baseColor[0]=col[0]; e.baseColor[1]=col[1]; e.baseColor[2]=col[2]; e.baseColor[3]=1.0f;
        e.emissive[0]=em[0]; e.emissive[1]=em[1]; e.emissive[2]=em[2]; e.emissive[3]=1.0f;
        e.tag  = (uint32_t)Tag::Static;
        e.body = physics.addStaticMesh(geo.cverts.data(), (uint32_t)(geo.cverts.size() / 3),
                                       geo.cindex.data(), (uint32_t)geo.cindex.size());
        scene.add(e); ++added;
    };
    auto addLight = [&](float x, float y, float z, float r, float g, float b, float range) {
        if (!outCrystalLights) return;
        x3::rhi::PointLight l;
        l.pos[0] = x; l.pos[1] = y; l.pos[2] = z;
        l.range = range; l.color[0] = r; l.color[1] = g; l.color[2] = b;
        outCrystalLights->push_back(l);
    };
    // A framed rock wall in an X=const plane ([xa,xb] thick), filling [z0,z1]x[y0,y1]
    // MINUS a list of rectangular door holes (each z/y). Built as horizontal Y-bands:
    // a band inside a hole's Y range fills only the Z to the LEFT/RIGHT of the hole
    // (holes sit at distinct Y, so at most one is active per band).
    struct Hole { float z0, z1, y0, y1; };
    auto wallXHoles = [&](float xa, float xb, float z0, float z1, float y0, float y1,
                          const std::vector<Hole>& holes) {
        std::vector<float> ys = { y0, y1 };
        for (const Hole& h : holes) {
            if (h.y0 > y0 && h.y0 < y1) ys.push_back(h.y0);
            if (h.y1 > y0 && h.y1 < y1) ys.push_back(h.y1);
        }
        std::sort(ys.begin(), ys.end());
        ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
        for (size_t i = 0; i + 1 < ys.size(); ++i) {
            const float ya = ys[i], yb = ys[i + 1], ym = (ya + yb) * 0.5f;
            const Hole* act = nullptr;
            for (const Hole& h : holes) if (ym > h.y0 && ym < h.y1) { act = &h; break; }
            if (!act) { addVis(xa, xb, ya, yb, z0, z1); continue; }
            if (act->z0 > z0) addVis(xa, xb, ya, yb, z0, act->z0);   // left of the door
            if (act->z1 < z1) addVis(xa, xb, ya, yb, act->z1, z1);   // right of the door
        }
    };

    // ================= THE DESCENT FALL — vertical fall shaft ==================
    // REPLACES the old walkable switchback ramp with a straight VERTICAL BORE: you
    // DROP the whole way from the surface mouth (topY) down to a DARK ROOM just above
    // the club, then a code-locked keypad door -> HALL -> ELEVATOR takes the last leg
    // into Club 1127. The shaft walls are colored per STRATA band so the geology rushes
    // UP past the falling camera. The interactive layer (fall-catch, terminal, keypad
    // door, elevator ride) is descent_fall.{h,cpp}; it reads the world positions this
    // build writes into `outLayout`.
    const float SX = tc.shaftX, SZ = tc.shaftZ;
    const float HW = 4.0f;                              // bore half-width (8 m clear chute)
    const float wxMin = SX - HW, wxMax = SX + HW;
    const float wzMin = SZ - HW, wzMax = SZ + HW;
    const float wallT = 1.5f;                           // rock wall thickness
    const float mouthY = tc.topY;                       // trapdoor mouth Y (top of the fall)

    // DARK LANDING ROOM: a sealed dark box just above the club (its floor ~12 m over
    // the club floor, i.e. a couple of m above the club ceiling). The fall drops
    // through a hole in the room's ceiling and lands on the room floor.
    const float roomFloorY = tc.bottomY + 12.0f;
    const float roomCeilY  = roomFloorY + 6.0f;         // 6 m tall dark room
    const float HRX = 6.0f, HRZ = 6.0f;                 // room half extents (12x12 m)
    const float rMinX = SX - HRX, rMaxX = SX + HRX;
    const float rMinZ = SZ - HRZ, rMaxZ = SZ + HRZ;
    const float shaftBotY = roomCeilY;                  // shaft opens into the room ceiling hole
    const float catchTopY = roomFloorY + 10.0f;         // last ~10 m: the slowdown/catch volume

    // Dark-vault look for the room / hall / alcove shells (the terminal + elevator
    // supply the light; the shell itself reads as near-black rock, barely lifted so
    // it isn't pure void).
    const float rmCol[3] = { 0.11f, 0.12f, 0.15f };
    const float rmEm[3]  = { 0.010f, 0.010f, 0.014f };

    // ---- STRATA-COLORED FALL SHAFT WALLS -------------------------------------
    // Four colliding rock walls contain the bore (the player can't drift out of the
    // chute), stacked as ~6 m vertical BANDS colored by the geology band covering each
    // Y — glowing bands (Crystal Veins / Magma / Alien Substrate) carry their emissive
    // so the deep shaft glows. As you plummet the bands rush up past the camera.
    const auto& strata = ElevatorSystem::strata();
    auto bandAt = [&](float y, float outCol[3], float outEm[3]) {
        for (const auto& s : strata) {
            if (y >= s.yMin && y <= s.yMax) {
                outCol[0]=s.rgb[0]; outCol[1]=s.rgb[1]; outCol[2]=s.rgb[2];
                if (s.glow) { outEm[0]=s.glowRgb[0]*0.16f; outEm[1]=s.glowRgb[1]*0.16f; outEm[2]=s.glowRgb[2]*0.16f; }
                else        { outEm[0]=tc.emissive[0]; outEm[1]=tc.emissive[1]; outEm[2]=tc.emissive[2]; }
                return;
            }
        }
        outCol[0]=tc.tint[0]; outCol[1]=tc.tint[1]; outCol[2]=tc.tint[2];
        outEm[0]=tc.emissive[0]; outEm[1]=tc.emissive[1]; outEm[2]=tc.emissive[2];
    };

    // SIDE SHOOTS (the "room for activities"): a few offshoot rooms branching off the
    // +X shaft wall at various depths down the 800 m fall — content to discover. Their
    // mouths are punched out of the +X wall bands (below); one holds a Salvari crystal
    // hollow. NOTE: the mouths open OUTWARD (east of the bore) so they never block the
    // fall; reach them by air-steering into a mouth or noclip. Add more via this list.
    // Cave KINDS (BRANCHING NETWORK + LANDMARKS, #7 + SALVARI SHRINE, #6): each side-
    // shoot has its own character so the player navigates by SIGHT, not a corridor.
    //   Loop     — a branch that forks (a short dead-end spur off the main tube).
    //   Cache    — a dead-end pocket with a dense crystal CACHE (reward-to-find).
    //   Shrine   — the ancient SALVARI SHRINE cavern (altar + carved glyph slabs).
    //   Landmark — a HOUSE-SIZED crystal in a cathedral cavern (a wayfinding beacon).
    //   Collapse — a collapsed section (fallen rock blocks) with a crystal cache.
    enum class OffKind { Loop, Cache, Shrine, Landmark, Collapse };
    struct Off { float y; OffKind kind; float len; };
    const float fallSpan = mouthY - shaftBotY;          // total fall height (>0)
    const Off offs[5] = {
        { shaftBotY + fallSpan * 0.82f, OffKind::Loop,     18.0f },  // high (limestone) — a forking branch
        { shaftBotY + fallSpan * 0.64f, OffKind::Cache,    14.0f },  // granite — dead-end crystal cache
        { shaftBotY + fallSpan * 0.46f, OffKind::Shrine,   22.0f },  // obsidian — the SALVARI SHRINE
        { shaftBotY + fallSpan * 0.28f, OffKind::Landmark, 24.0f },  // crystal veins — HOUSE-SIZED crystal
        { shaftBotY + fallSpan * 0.14f, OffKind::Collapse, 18.0f },  // deep (near club) — collapsed cache
    };
    const float offMouthHalfZ = 2.0f, offMouthH = 3.2f;

    // Build the four bands' walls. The +X wall is split in Z around any side-shoot
    // mouth whose Y-window overlaps the band (a colliding wallXHoles).
    {
        const float bandH = 6.0f;
        for (float y0 = shaftBotY; y0 < mouthY; y0 += bandH) {
            const float y1 = std::min(y0 + bandH, mouthY);
            float col[3], em[3]; bandAt((y0 + y1) * 0.5f, col, em);
            // -X (west), -Z (south), +Z (north) walls: solid.
            addColC(wxMin - wallT, wxMin, y0, y1, wzMin - wallT, wzMax + wallT, col, em);
            addColC(wxMin, wxMax, y0, y1, wzMin - wallT, wzMin, col, em);
            addColC(wxMin, wxMax, y0, y1, wzMax, wzMax + wallT, col, em);
            // +X (east) wall: subtract any offshoot mouth active in this band.
            const Off* act = nullptr;
            for (const Off& o : offs)
                if ((y0+y1)*0.5f > o.y && (y0+y1)*0.5f < o.y + offMouthH) { act = &o; break; }
            if (!act) {
                addColC(wxMax, wxMax + wallT, y0, y1, wzMin - wallT, wzMax + wallT, col, em);
            } else {
                addColC(wxMax, wxMax + wallT, y0, y1, wzMin - wallT, SZ - offMouthHalfZ, col, em); // south of mouth
                addColC(wxMax, wxMax + wallT, y0, y1, SZ + offMouthHalfZ, wzMax + wallT, col, em); // north of mouth
            }
        }
    }

    // MOUTH RIM at the top: a colliding rock lip around the chute opening at mouthY so
    // you can walk to the edge and step off into the fall (the Floor-1 descent point).
    addColC(wxMin - 2.5f, wxMin,        mouthY - 0.5f, mouthY, wzMin - 2.5f, wzMax + 2.5f, rmCol, rmEm);
    addColC(wxMax,        wxMax + 2.5f, mouthY - 0.5f, mouthY, wzMin - 2.5f, wzMax + 2.5f, rmCol, rmEm);
    addColC(wxMin, wxMax, mouthY - 0.5f, mouthY, wzMin - 2.5f, wzMin, rmCol, rmEm);
    addColC(wxMin, wxMax, mouthY - 0.5f, mouthY, wzMax, wzMax + 2.5f, rmCol, rmEm);
    // A FAINT cold sky-leak at the mouth (near the surface) — barely there; the shaft
    // is otherwise CRYSTAL-LIT ONLY (#1), the eerie glow of the descent.
    addLight(SX, mouthY - 2.0f, SZ, 0.35f, 0.40f, 0.52f, 10.0f);

    // ---- CRYSTAL-LIT SHAFT (crystal-only lighting, #1) -----------------------
    // Salvari crystal clusters embedded just inside the bore walls at intervals down
    // the fall — the ONLY light in the deep shaft (no worklights): they rush up past
    // the falling camera as glowing blue veins, doubling as wayfinding (you fall THROUGH
    // a singing crystal shaft, not a lit mine). Alternate the wall so both sides glow;
    // DENSER (bigger + brighter range) toward the bottom, luring you down to the club.
    {
        const int nCz = 9;
        for (int k = 1; k < nCz; ++k) {
            const float fy = shaftBotY + fallSpan * ((float)k / (float)nCz);
            const bool east = (k & 1);                       // alternate +X / -X wall
            const float wx = east ? (wxMax - 0.6f) : (wxMin + 0.6f);
            const float wz = SZ + ((k % 3) - 1) * 1.6f;      // stagger in Z
            const float depth01 = 1.0f - (fy - shaftBotY) / fallSpan;   // 0 top .. 1 bottom
            const float sc = 0.9f + 1.3f * depth01;          // bigger toward the club
            addCrystalCluster(scene, device, wx, fy, wz, sc, 4,
                              10.0f + 6.0f * depth01, outCrystalLights,
                              (k & 1) ? 1.0f : -1.0f);
            added += 4;
        }
    }

    // ---- DARK LANDING ROOM ---------------------------------------------------
    // Floor (colliding) = the safe landing surface + the catch backstop.
    addColC(rMinX, rMaxX, roomFloorY - 0.4f, roomFloorY, rMinZ, rMaxZ, rmCol, rmEm);
    // Ceiling WITH a hole under the bore (4 segments around the shaft opening).
    addColC(rMinX, wxMin, roomCeilY, roomCeilY + 0.4f, rMinZ, rMaxZ, rmCol, rmEm);  // -X of bore
    addColC(wxMax, rMaxX, roomCeilY, roomCeilY + 0.4f, rMinZ, rMaxZ, rmCol, rmEm);  // +X of bore
    addColC(wxMin, wxMax, roomCeilY, roomCeilY + 0.4f, rMinZ, wzMin, rmCol, rmEm);  // -Z strip
    addColC(wxMin, wxMax, roomCeilY, roomCeilY + 0.4f, wzMax, rMaxZ, rmCol, rmEm);  // +Z strip
    // Solid walls: +X (east), -Z (south), +Z (north). The -X (west) wall carries the
    // keypad DOOR opening -> the hall.
    const float doorHalfW = 1.1f, doorH = 2.6f;
    const float doorZ = SZ, doorY = roomFloorY;
    addColC(rMaxX, rMaxX + 0.4f, roomFloorY, roomCeilY, rMinZ, rMaxZ, rmCol, rmEm);            // +X
    addColC(rMinX, rMaxX, roomFloorY, roomCeilY, rMinZ - 0.4f, rMinZ, rmCol, rmEm);            // -Z
    addColC(rMinX, rMaxX, roomFloorY, roomCeilY, rMaxZ, rMaxZ + 0.4f, rmCol, rmEm);            // +Z
    // -X wall in 3 segments around the door opening (south of door / north of door / lintel).
    addColC(rMinX - 0.4f, rMinX, roomFloorY, roomCeilY, rMinZ, doorZ - doorHalfW, rmCol, rmEm);   // south
    addColC(rMinX - 0.4f, rMinX, roomFloorY, roomCeilY, doorZ + doorHalfW, rMaxZ, rmCol, rmEm);   // north
    addColC(rMinX - 0.4f, rMinX, roomFloorY + doorH, roomCeilY, doorZ - doorHalfW, doorZ + doorHalfW, rmCol, rmEm); // lintel

    // ---- HALL: from the room's -X door WEST to the elevator alcove -----------
    const float elevX = tc.clubDoorX + 8.0f;            // elevator shaft east of the club doorway
    const float elevZ = SZ;
    const float hallHalfZ = 2.0f, hallH = 3.4f;
    const float hallX0 = elevX + 2.0f;                  // hall west end (at the alcove)
    const float hallX1 = rMinX;                         // hall east end (at the room door)
    addColC(hallX0, hallX1, roomFloorY - 0.4f, roomFloorY, elevZ - hallHalfZ, elevZ + hallHalfZ, rmCol, rmEm);        // floor
    addColC(hallX0, hallX1, roomFloorY, roomFloorY + hallH, elevZ - hallHalfZ - 0.4f, elevZ - hallHalfZ, rmCol, rmEm); // -Z wall
    addColC(hallX0, hallX1, roomFloorY, roomFloorY + hallH, elevZ + hallHalfZ, elevZ + hallHalfZ + 0.4f, rmCol, rmEm); // +Z wall
    addColC(hallX0, hallX1, roomFloorY + hallH, roomFloorY + hallH + 0.4f, elevZ - hallHalfZ - 0.4f, elevZ + hallHalfZ + 0.4f, rmCol, rmEm); // ceiling
    addLight(rMinX - 4.0f, roomFloorY + 2.4f, elevZ, 0.85f, 0.80f, 0.72f, 14.0f);
    addLight(elevX + 3.0f, roomFloorY + 2.4f, elevZ, 0.85f, 0.80f, 0.72f, 12.0f);

    // ---- ELEVATOR ALCOVE + SHAFT (THE HUB): club + complex ------------------
    // The elevator car (built by descent_fall.cpp) rides a shaft from the hall level
    // (elevTopY) down through the club floor (elevBotY) to the survival-complex L7
    // floor (complexBottomY) — the Route-B hub. The shaft extends to the DEEPEST stop.
    const float elevTopY = roomFloorY, elevBotY = tc.bottomY;
    const bool haveHall = tc.underClubHall;
    const float shaftBottom = haveHall ? std::min(tc.bottomY, tc.complexBottomY) : tc.bottomY;
    const float ahz = 2.6f;                             // alcove half-Z
    const float ax0 = elevX - 2.6f, ax1 = elevX + 2.6f;
    // Shaft walls (colliding), from the deepest stop up to just over the hall ceiling.
    addColC(ax0 - 0.4f, ax0, shaftBottom, elevTopY + hallH, elevZ - ahz, elevZ + ahz, rmCol, rmEm);   // -X shaft wall
    addColC(ax1, ax1 + 0.4f, shaftBottom, elevTopY + hallH, elevZ - ahz, elevZ + ahz, rmCol, rmEm);   // +X shaft wall
    addColC(ax0, ax1, shaftBottom, elevTopY + hallH, elevZ - ahz - 0.4f, elevZ - ahz, rmCol, rmEm);   // -Z shaft wall
    addColC(ax0, ax1, shaftBottom, elevTopY + hallH, elevZ + ahz, elevZ + ahz + 0.4f, rmCol, rmEm);   // +Z shaft wall
    addColC(ax0, ax1, elevTopY + hallH, elevTopY + hallH + 0.4f, elevZ - ahz, elevZ + ahz, rmCol, rmEm); // shaft cap
    // CLUB CONNECTOR — from the elevator's CLUB stop WEST into the club's east doorway
    // at the club floor (the club shell is open below 2.8 m at the doorway).
    {
        const float y  = tc.bottomY;
        const float cx0 = tc.clubDoorX - 2.0f, cx1 = ax1;
        const float z0 = tc.clubDoorZ - 2.2f, z1 = tc.clubDoorZ + 2.2f;
        addColC(cx0, cx1, y - 0.6f, y, z0, z1, rmCol, rmEm);                                  // floor
        addColC(cx0, cx1, y, y + 3.0f, z0 - 0.4f, z0, rmCol, rmEm);                           // -Z wall
        addColC(cx0, cx1, y, y + 3.0f, z1, z1 + 0.4f, rmCol, rmEm);                           // +Z wall
        addColC(cx0, cx1, y + 3.0f, y + 3.4f, z0 - 0.4f, z1 + 0.4f, rmCol, rmEm);             // ceiling
        addLight(tc.clubDoorX + 3.0f, y + 2.2f, tc.clubDoorZ, 1.30f, 1.05f, 0.78f, 15.0f);
    }

    // ---- UNDER-CLUB HALL (Route-B): from the elevator's BOTTOM stop (complex L7
    // level) WEST, beneath the club, to the survival complex's east edge. This is the
    // canon link between the elevator hub and Danny's 7-level complex (L7 bottom). On
    // --world club the complex itself isn't present, so the hall ends in a capped STUB
    // landing at the complex east-wall attach point (a marker light) — @13700k punches
    // a door in the complex east shell at (complexAttachX, complexBottomY, complexAttachZ)
    // to receive it.
    if (haveHall) {
        const float y  = tc.complexBottomY;
        const float uhz = 2.0f, uh = 3.4f;
        const float hx0 = tc.complexAttachX, hx1 = ax0;      // from complex east edge to the elevator
        // Run the hall down the elevator Z; near the west (complex) end, splay Z to
        // reach the L7 stair-bay landing Z. Keep it a single straight bore at elevZ +
        // a short jog is future polish; here a straight hall at elevZ suffices.
        const float zc = elevZ;
        addColC(hx0, hx1, y - 0.6f, y, zc - uhz, zc + uhz, rmCol, rmEm);                      // floor
        addColC(hx0, hx1, y, y + uh, zc - uhz - 0.4f, zc - uhz, rmCol, rmEm);                 // -Z wall
        addColC(hx0, hx1, y, y + uh, zc + uhz, zc + uhz + 0.4f, rmCol, rmEm);                 // +Z wall
        addColC(hx0, hx1, y + uh, y + uh + 0.4f, zc - uhz - 0.4f, zc + uhz + 0.4f, rmCol, rmEm); // ceiling
        // STUB landing at the complex attach (a small pad + a marker light so the link
        // reads on camera and the attach point is obvious).
        addColC(hx0 - 2.0f, hx0, y - 0.6f, y, tc.complexAttachZ - 2.0f, tc.complexAttachZ + 2.0f, rmCol, rmEm);
        addColC(hx0 - 2.4f, hx0 - 2.0f, y, y + uh + 1.0f, tc.complexAttachZ - 2.4f, tc.complexAttachZ + 2.4f, rmCol, rmEm); // west cap wall (the door goes HERE)
        addLight(hx0 - 1.0f, y + 2.2f, tc.complexAttachZ, 0.55f, 0.85f, 1.25f, 14.0f);        // cool marker at the complex link
        addLight((hx0 + hx1) * 0.5f, y + 2.2f, zc, 1.10f, 0.95f, 0.75f, 16.0f);               // hall worklight
    }

    // ---- SIDE SHOOTS: NATURAL CAVE tubes off the fall shaft (#3 tubes, #4 biomes,
    // #1 crystal-only, #6 shrine, #7 landmarks/branching) ---------------------
    // Each offshoot is a LUMPY ROCK TUBE (pinch mouth -> cathedral belly -> pinch),
    // not a box corridor: a flat colliding+visual floor is walked, an invisible
    // containment shell keeps the player over it, and a double-sided lumpy tube mesh
    // (makeCaveTubeX) is the cave rock you see, tinted to the STRATA BIOME at its
    // depth. Lit ONLY by Salvari crystals (crystal-only, no worklights). Content
    // varies by kind (shrine altar / house crystal / collapse / cache / fork).
    // Collision-only box (invisible) — containment so the player stays inside the tube.
    auto addColl = [&](float x0, float x1, float y0, float y1, float z0, float z1) {
        const x3::phys::Vec3 he{ (x1-x0)*0.5f, (y1-y0)*0.5f, (z1-z0)*0.5f };
        const x3::phys::Vec3 c { (x0+x1)*0.5f, (y0+y1)*0.5f, (z0+z1)*0.5f };
        if (he.x <= 0.0f || he.y <= 0.0f || he.z <= 0.0f) return;
        physics.addBox(he, c, 0.0f, x3::phys::Layer::Static);
    };
    // Lumpy rock tube (VISUAL, non-colliding), tinted to the biome band.
    auto addTube = [&](float x0, float x1, float cyc, float czc, float rMin, float rMax,
                       uint32_t sd, const float col[3], const float em[3]) {
        const int rings = std::max(6, (int)((x1 - x0) / 2.2f));
        x3::prims::PrimMesh geo = makeCaveTubeX(x0, x1, cyc, czc, rMin, rMax, 0.34f,
                                                rings, 12, tc.uvScale, sd);
        Entity e;
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
        e.tex = rockTex;
        e.baseColor[0]=col[0]; e.baseColor[1]=col[1]; e.baseColor[2]=col[2]; e.baseColor[3]=1.0f;
        e.emissive[0]=em[0]; e.emissive[1]=em[1]; e.emissive[2]=em[2]; e.emissive[3]=1.0f;
        e.tag = (uint32_t)Tag::Static; e.body = x3::phys::BodyId{};
        scene.add(e); ++added;
    };
    // A single dark rock STALACTITE (down) / STALAGMITE (up) spike — an elongated,
    // rock-tinted crystal prim (reuses makeCrystal); render-only cave dressing.
    auto addSpike = [&](float x, float yTop, float z, float r, float len, float dir) {
        x3::prims::PrimMesh geo = x3::prims::makeCrystal(r, len * 0.5f, len * 0.5f);
        Entity e;
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
        e.tex = rockTex;
        e.baseColor[0]=tc.tint[0]*0.8f; e.baseColor[1]=tc.tint[1]*0.8f; e.baseColor[2]=tc.tint[2]*0.8f; e.baseColor[3]=1.0f;
        e.emissive[0]=tc.emissive[0]*0.5f; e.emissive[1]=tc.emissive[1]*0.5f; e.emissive[2]=tc.emissive[2]*0.5f; e.emissive[3]=1.0f;
        e.tag = (uint32_t)Tag::Static; e.body = x3::phys::BodyId{};
        e.transform[0]=1;e.transform[1]=0;e.transform[2]=0;e.transform[3]=0;
        e.transform[4]=0;e.transform[5]=dir;e.transform[6]=0;e.transform[7]=0;   // dir=-1 flips to point down
        e.transform[8]=0;e.transform[9]=0;e.transform[10]=1;e.transform[11]=0;
        e.transform[12]=x; e.transform[13]=yTop - dir*len*0.5f; e.transform[14]=z; e.transform[15]=1.0f;
        scene.add(e); ++added;
    };

    for (const Off& o : offs) {
        const float y  = o.y;                            // walkable floor at the mouth
        const float mouthX = wxMax + wallT;              // just outside the +X wall
        const float rx1 = mouthX + o.len;                // far end of the cavern
        float col[3], em[3]; bandAt(y + 3.0f, col, em);  // BIOME: rock tinted to the strata band
        const uint32_t sd = (uint32_t)std::lround((y + 4000.0f) * 0.37f);

        // Bore sizing by kind (Landmark = a cathedral; Cache = a tight pocket).
        const float floorHalf = (o.kind == OffKind::Landmark) ? 5.0f : 3.4f;
        const float rMin = floorHalf + 0.6f;
        const float rMax = (o.kind == OffKind::Landmark) ? 8.5f
                          : (o.kind == OffKind::Cache)    ? 4.4f : 6.2f;
        const float cyc  = y + 2.0f;                     // tube axis: floor tucks into the lower wall

        // Flat colliding+visual floor strip the length of the shoot (walkable rock).
        addColC(wxMax, rx1, y - 0.6f, y, SZ - floorHalf, SZ + floorHalf, col, em);
        // The lumpy rock TUBE you see (pinch -> cathedral -> pinch).
        addTube(wxMax, rx1 + 0.6f, cyc, SZ, rMin, rMax, sd, col, em);
        // Invisible containment: side walls + ceiling + endcap keep the player on the floor.
        const float topY = cyc + rMax;
        addColl(wxMax, rx1, y, topY, SZ - floorHalf - 0.3f, SZ - floorHalf);   // -Z wall
        addColl(wxMax, rx1, y, topY, SZ + floorHalf, SZ + floorHalf + 0.3f);   // +Z wall
        addColl(mouthX, rx1, topY, topY + 0.4f, SZ - floorHalf, SZ + floorHalf); // ceiling
        addColl(rx1, rx1 + 0.4f, y, topY, SZ - floorHalf, SZ + floorHalf);     // endcap

        // Stalactites from the dome + a few stalagmites — the tight->vast->tight tube dressing.
        for (int s = 0; s < 5; ++s) {
            const float fx = mouthX + o.len * (0.2f + 0.15f * s);
            const float fz = SZ + ((s % 3) - 1) * (floorHalf * 0.5f);
            addSpike(fx, cyc + rMax * 0.72f, fz, 0.28f, 1.4f + 0.5f * (s % 2), -1.0f);   // hang down
            if (s % 2 == 0) addSpike(fx + 1.0f, y + 0.05f, fz + 0.8f, 0.30f, 0.9f, 1.0f); // rise up
        }

        // Crystal light budget helper — a Salvari hollow (full pocket) reward.
        auto hollow = [&](float hx, float hy, float hz, float scale) {
            BedrockConfig bc;
            for (int i = 0; i < 4; ++i) bc.tint[i] = tc.tint[i];
            for (int i = 0; i < 4; ++i) bc.emissive[i] = tc.emissive[i];
            bc.uvScale = tc.uvScale;
            x3::rhi::PointLight bl = addSalvariHollow(scene, device, rockTex, bc, hx, hy, hz, scale);
            added += 7;
            if (outCrystalLights) outCrystalLights->push_back(bl);
        };

        const float cavX = (mouthX + rx1) * 0.5f;
        switch (o.kind) {
        case OffKind::Cache: {
            // DEAD-END crystal CACHE — a dense cluster at the tight far end (reward).
            addCrystalCluster(scene, device, rx1 - 1.6f, y + 0.1f, SZ, 1.6f, 8, 14.0f, outCrystalLights);
            addCrystalCluster(scene, device, rx1 - 2.6f, y + 0.1f, SZ - 1.4f, 1.0f, 5, 10.0f, outCrystalLights);
            added += 13;
        } break;
        case OffKind::Loop: {
            // A FORKING BRANCH: a short spur tube off the +Z side of the cavern, with a
            // crystal at its end — the network forks here (navigate by sight).
            const float spurZ0 = SZ + floorHalf, spurZ1 = SZ + floorHalf + 10.0f;
            addColC(cavX - 2.5f, cavX + 2.5f, y - 0.6f, y, spurZ0, spurZ1, col, em);   // spur floor
            addColl(cavX - 2.8f, cavX - 2.5f, y, y + 4.0f, spurZ0, spurZ1);
            addColl(cavX + 2.5f, cavX + 2.8f, y, y + 4.0f, spurZ0, spurZ1);
            addColl(cavX - 2.8f, cavX + 2.8f, y + 4.0f, y + 4.4f, spurZ0, spurZ1);
            addColl(cavX - 2.8f, cavX + 2.8f, y, y + 4.4f, spurZ1, spurZ1 + 0.3f); // spur endcap
            addCrystalCluster(scene, device, cavX, y + 0.1f, spurZ1 - 1.4f, 1.2f, 6, 12.0f, outCrystalLights);
            addCrystalCluster(scene, device, cavX + 1.0f, y + 0.1f, SZ, 0.9f, 4, 9.0f, outCrystalLights);
            added += 10;
        } break;
        case OffKind::Landmark: {
            // A HOUSE-SIZED CRYSTAL — a single towering shard the player sees from the
            // shaft and navigates toward (the cavern beacon). Bright blue point light.
            x3::prims::PrimMesh geo = x3::prims::makeCrystal(1.9f, 2.6f, 2.0f);   // ~9 m tall (fits the cavern)
            Entity e;
            e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                       geo.index.data(), (uint32_t)geo.index.size());
            const float bT[4]={0.02f,0.07f,0.52f,1.0f}, bE[4]={0.015f,0.08f,0.60f,1.0f};
            for (int i=0;i<4;++i){ e.baseColor[i]=bT[i]; e.emissive[i]=bE[i]; }
            e.tag=(uint32_t)Tag::Static; e.body=x3::phys::BodyId{};
            e.transform[0]=1;e.transform[1]=0;e.transform[2]=0;e.transform[3]=0;
            e.transform[4]=0;e.transform[5]=1;e.transform[6]=0;e.transform[7]=0;
            e.transform[8]=0;e.transform[9]=0;e.transform[10]=1;e.transform[11]=0;
            e.transform[12]=cavX; e.transform[13]=y + 4.6f; e.transform[14]=SZ; e.transform[15]=1.0f;
            scene.add(e); ++added;
            addColl(cavX-1.4f, cavX+1.4f, y, y + 4.0f, SZ-1.4f, SZ+1.4f);   // can't walk through it
            if (outCrystalLights) {
                x3::rhi::PointLight l; l.pos[0]=cavX; l.pos[1]=y+4.0f; l.pos[2]=SZ;
                l.range=17.0f; l.color[0]=0.13f; l.color[1]=0.36f; l.color[2]=1.30f;   // beacon, not a floodlight
                outCrystalLights->push_back(l);
            }
            // A ring of smaller shards around its base.
            addCrystalCluster(scene, device, cavX + 3.0f, y + 0.1f, SZ + 2.0f, 1.2f, 5, 11.0f, outCrystalLights);
            addCrystalCluster(scene, device, cavX - 3.0f, y + 0.1f, SZ - 2.2f, 1.0f, 4, 10.0f, outCrystalLights);
        } break;
        case OffKind::Collapse: {
            // COLLAPSED SECTION — tumbled rock blocks strewn on the floor (a cave-in),
            // with a crystal cache glinting between them. A LANDMARK you remember.
            for (int b = 0; b < 6; ++b) {
                const float bx = mouthX + o.len * (0.35f + 0.09f * b);
                const float bz = SZ + ((b * 3) % 5 - 2) * 0.9f;
                const float bs = 0.8f + 0.5f * ((b * 7) % 10) / 10.0f;
                x3::prims::PrimMesh geo = makeSolidRockBox(bs, bs*0.7f, bs, bx, y + bs*0.7f, bz, tc.uvScale*1.4f);
                Entity e;
                e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                           geo.index.data(), (uint32_t)geo.index.size());
                e.tex = rockTex;
                e.baseColor[0]=col[0]; e.baseColor[1]=col[1]; e.baseColor[2]=col[2]; e.baseColor[3]=1.0f;
                e.emissive[0]=em[0]; e.emissive[1]=em[1]; e.emissive[2]=em[2]; e.emissive[3]=1.0f;
                e.tag=(uint32_t)Tag::Static;
                e.body = physics.addBox(x3::phys::Vec3{bs,bs*0.7f,bs}, x3::phys::Vec3{bx,y+bs*0.7f,bz}, 0.0f, x3::phys::Layer::Static);
                scene.add(e); ++added;
            }
            addCrystalCluster(scene, device, rx1 - 2.0f, y + 0.1f, SZ, 1.3f, 6, 12.0f, outCrystalLights);
            added += 6;
        } break;
        case OffKind::Shrine: {
            // ANCIENT SALVARI SHRINE (#6) — carved into the cavern: a raised crystal
            // ALTAR, a ring of shards, and dark carved GLYPH SLABS on the back wall
            // (faint self-glow so the etched Salvari script reads). Environmental-
            // storytelling reward tied to the rescue-the-Salvari arc.
            // Altar PLINTH — a broad stepped stone dais the crystal sits ON (so the
            // altar reads as built, not just a crystal on the floor).
            const float steps[3][2] = { {1.6f, 0.35f}, {1.15f, 0.30f}, {0.8f, 0.28f} };
            float pt = y;
            for (int st = 0; st < 3; ++st) {
                const float ph = steps[st][0], hh = steps[st][1];
                x3::prims::PrimMesh geo = makeSolidRockBox(ph, hh, ph, cavX, pt + hh, SZ, tc.uvScale*1.5f);
                Entity e;
                e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                           geo.index.data(), (uint32_t)geo.index.size());
                e.tex=rockTex;
                e.baseColor[0]=0.22f;e.baseColor[1]=0.20f;e.baseColor[2]=0.26f;e.baseColor[3]=1.0f;
                e.emissive[0]=0.03f;e.emissive[1]=0.06f;e.emissive[2]=0.12f;e.emissive[3]=1.0f;  // faint votive glow
                e.tag=(uint32_t)Tag::Static;
                e.body = physics.addBox(x3::phys::Vec3{ph,hh,ph}, x3::phys::Vec3{cavX,pt+hh,SZ}, 0.0f, x3::phys::Layer::Static);
                scene.add(e); ++added;
                pt += hh * 2.0f;
            }
            // The altar CRYSTAL — a modest bright shard cluster ON the dais (smaller so
            // the plinth stays visible beneath it).
            addCrystalCluster(scene, device, cavX, pt, SZ, 0.8f, 5, 15.0f, outCrystalLights);
            // GLYPH STELAE — four tall carved Salvari steles FLANKING the approach (two
            // each side), etched-blue self-glow so the script reads as you walk up. Dark
            // obsidian panels catching the altar's crystal light.
            for (int g = 0; g < 4; ++g) {
                const float side = (g < 2) ? -1.0f : 1.0f;
                const float gx = cavX - 3.0f + (g % 2) * 4.5f;
                const float gz = SZ + side * (floorHalf - 0.5f);
                x3::prims::PrimMesh geo = makeSolidRockBox(0.5f, 1.35f, 0.16f, gx, y + 1.35f, gz, tc.uvScale*2.2f);
                Entity e;
                e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                           geo.index.data(), (uint32_t)geo.index.size());
                e.tex=rockTex;
                e.baseColor[0]=0.05f;e.baseColor[1]=0.06f;e.baseColor[2]=0.11f;e.baseColor[3]=1.0f;
                e.emissive[0]=0.03f;e.emissive[1]=0.16f;e.emissive[2]=0.52f;e.emissive[3]=1.0f;  // etched-blue glyph glow
                e.tag=(uint32_t)Tag::Static;
                e.body = physics.addBox(x3::phys::Vec3{0.5f,1.35f,0.16f}, x3::phys::Vec3{gx,y+1.35f,gz}, 0.0f, x3::phys::Layer::Static);
                scene.add(e); ++added;
            }
            hollow(cavX - 3.0f, y + 3.2f, SZ - 2.0f, 0.9f);   // a wall pocket for depth
        } break;
        }

        // ---- UNDERGROUND RIVER centerline (feat/cave-river) -------------------
        // Thread a mildly-luminescent blue STREAM down the low floor of the WIDER
        // caverns (Cache / Landmark cathedral / Collapse), pooling in the belly + at
        // the dead-end. Emitted as a centerline polyline into the layout; CaveRiver
        // (cave_river.h) builds the SELF-EMISSIVE blue water ribbon + pool bank-lights
        // from it (no sky/sun — the water IS the light, like the Salvari crystals).
        if (outLayout && (o.kind == OffKind::Cache ||
                          o.kind == OffKind::Landmark ||
                          o.kind == OffKind::Collapse)) {
            const float xA = mouthX + 0.6f;              // just inside the tube mouth (shaft side)
            const float xB = rx1 - 0.4f;                 // near the dead-end wall (downstream)
            const float span = std::max(1.0f, xB - xA);
            const int   nSeg = std::max(4, (int)(span / 2.2f));   // ~2.2 m nodes
            const int   bellyI = (int)std::lround((float)nSeg * 0.5f);
            const int   endI   = nSeg - 1;
            const float streamHW = 0.9f;
            const float poolHW   = std::min(floorHalf * 0.72f, floorHalf - 0.4f);
            for (int i = 0; i <= nSeg; ++i) {
                const float t = (float)i / (float)nSeg;  // 0 mouth .. 1 dead-end (downstream +X)
                CaveRiverNode nd;
                nd.x = xA + span * t;
                // gentle meander so it reads as a natural stream, not a straight strip
                // (kept inside the floor: |zOff| + halfWidth < floorHalf).
                const float zOff = (floorHalf - streamHW - 0.5f) * 0.55f * std::sin(t * 4.2f + 0.6f);
                nd.z = SZ + zOff;
                // Smoothly WIDEN toward the belly (cathedral middle) + the dead-end pool.
                const float belly = std::exp(-((t - 0.5f) * (t - 0.5f)) / (2.0f * 0.11f * 0.11f));
                const float endp  = std::max(0.0f, (t - 0.72f) / 0.28f);
                const float pool01 = std::min(1.0f, std::max(belly, endp));
                nd.halfWidth = streamHW + (poolHW - streamHW) * pool01;
                nd.emissive  = 0.30f + 0.10f * pool01;   // pools a touch brighter
                nd.pool      = (i == bellyI) || (i == endI);   // exactly two bank lights / tube
                // sits just above the flat floor (top = o.y); a hair higher at the mouth
                // so the surface tilts DOWN toward the dead-end (the flow direction).
                nd.y = o.y + 0.05f + 0.05f * (1.0f - t);
                nd.breakSeg = (i == nSeg);               // don't bridge to the next tube
                outLayout->river.push_back(nd);
            }
        }
    }

    // ---- publish the layout for the interactive descent_fall layer -----------
    if (outLayout) {
        outLayout->shaftX = SX; outLayout->shaftZ = SZ; outLayout->shaftHalfW = HW;
        outLayout->mouthY = mouthY; outLayout->catchTopY = catchTopY;
        outLayout->roomCx = SX; outLayout->roomCz = SZ;
        outLayout->roomFloorY = roomFloorY; outLayout->roomCeilY = roomCeilY;
        outLayout->roomHalfX = HRX; outLayout->roomHalfZ = HRZ;
        outLayout->doorX = rMinX; outLayout->doorZ = doorZ; outLayout->doorY = doorY;
        outLayout->doorHalfW = doorHalfW;
        outLayout->elevX = elevX; outLayout->elevZ = elevZ;
        outLayout->elevTopY = elevTopY; outLayout->elevBotY = elevBotY;
        outLayout->complexBottomY = haveHall ? tc.complexBottomY : elevBotY;
        outLayout->clubDoorX = tc.clubDoorX; outLayout->clubDoorZ = tc.clubDoorZ;
        outLayout->complexAttachX = tc.complexAttachX; outLayout->complexAttachZ = tc.complexAttachZ;
        outLayout->hasUnderHall = haveHall;
    }

    (void)wallXHoles;   // (kept as the visual-liner helper for future authored tunnels)
    return added;
}

} // namespace x3::game
