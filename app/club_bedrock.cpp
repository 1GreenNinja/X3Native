// Club 1127 BEDROCK ENCASEMENT — see club_bedrock.h for the full rationale.
#include "club_bedrock.h"
#include "mesh_prims.h"

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
    //    pocket centre, emissive electric-blue so they bloom and light the rock.
    const float blueTint[4] = { 0.08f, 0.24f, 0.62f, 1.0f };
    const float blueEmit[4] = { 0.10f, 0.40f, 1.15f, 1.5f };   // saturated electric-blue -> bloom
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
    // 3) BLUE POINT LIGHT pooling in the hollow (premultiplied electric blue).
    x3::rhi::PointLight l;
    l.pos[0] = cx; l.pos[1] = cy; l.pos[2] = cz;
    l.range  = 16.0f * scale;
    l.color[0] = 0.30f; l.color[1] = 0.80f; l.color[2] = 1.90f;
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
        struct Hollow { float x, y, z, s; };
        const Hollow hollows[6] = {
            {  cfg.cavMaxX + 22.0f, -195.0f,   6.0f, 1.10f },  // just E of the club
            { -12.0f,               -196.0f, cfg.cavMinZ - 24.0f, 0.95f }, // just N of the club
            {  38.0f,               -120.0f,  46.0f, 1.30f },  // up in the strata (dig up)
            { -64.0f,                -64.0f, 150.0f, 1.15f },  // higher, under the city
            {  46.0f,               -232.0f,  34.0f, 1.00f },  // down in the base rock
            { -44.0f,               -216.0f, -34.0f, 0.85f },  // down, other side
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

} // namespace x3::game
