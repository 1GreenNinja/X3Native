// Club 1127 BEDROCK ENCASEMENT — see club_bedrock.h for the full rationale.
#include "club_bedrock.h"
#include "mesh_prims.h"

#include <algorithm>
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

// ============================================================================
// THE EARTH TUNNEL NETWORK — see club_bedrock.h for the model + the author-next
// recipe. A vertical switchback DESCENT bore (surface -> club) + a bottom CONNECTOR
// into the club doorway + 3 OFFSHOOT passages (one holds a Salvari crystal hollow).
// ============================================================================
int buildEarthTunnels(Scene& scene, x3::rhi::IRenderDevice& device,
                      x3::phys::IPhysicsWorld& physics, const TunnelConfig& tc,
                      std::vector<x3::rhi::PointLight>* outCrystalLights) {
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
    // WALKABLE colliding RAMP wedge (run along +Z, `dir` picks the climb direction).
    auto addRamp = [&](float cx, float cy, float cz, float halfW, float run,
                       float rise, float dir) {
        x3::prims::PrimMesh geo =
            x3::prims::makeRamp(cx, cy, cz, halfW, run, rise, /*axis*/1u, dir, tc.uvScale);
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

    // ---- descent geometry parameters -----------------------------------------
    const float SX = tc.shaftX, SZ = tc.shaftZ;
    const float halfWX = 4.0f;                       // walkway half-width (8 m clear)
    const float wxMin = SX - halfWX, wxMax = SX + halfWX;   // [34,42]
    const float southZ = SZ - 7.0f, northZ = SZ + 7.0f;     // turn-landing centers [-3,11]
    const float landHz = 2.0f;                       // landing half-depth
    const float run = northZ - southZ;               // ramp Z run (14 m)
    const float span = tc.topY - tc.bottomY;         // 197 m
    const int   nFlights = std::max(2, (int)std::ceil(span / 11.0f));   // ~18 flights
    const float flightRise = span / (float)nFlights; // ~10.9 m/flight (~38 deg)
    auto flightY = [&](int k) { return tc.bottomY + (float)k * flightRise; };
    auto landZ   = [&](int k) { return (k % 2 == 0) ? southZ : northZ; };

    // Bore liner extents (a touch beyond the walkway; leaves 2-3 m rock to the walls).
    const float linZ0 = southZ - landHz - 2.0f, linZ1 = northZ + landHz + 2.0f;   // [-7,15]
    const float linY0 = tc.bottomY - 1.0f, linY1 = tc.topY;                       // [-201,-3]
    const float linXin = wxMin - 1.0f, linXout = wxMax + 1.0f;                     // wall inner faces

    // ---- OFFSHOOTS: choose 3 branch landings (crystal in the middle one) -------
    const int offK[3]      = { std::max(2, nFlights / 4),
                               nFlights / 2,
                               std::min(nFlights - 2, (3 * nFlights) / 4) };
    const bool offCrystal[3] = { false, tc.crystalOffshoot, false };
    // Each offshoot exits +X at its landing (Z,Y). Precompute the +X liner door holes.
    std::vector<Hole> plusXHoles;
    for (int o = 0; o < 3; ++o) {
        const float lz = landZ(offK[o]), ly = flightY(offK[o]);
        plusXHoles.push_back({ lz - 3.0f, lz + 3.0f, ly - 0.5f, ly + 4.2f });
    }

    // ============================ BUILD =======================================
    // BOTTOM FLOOR — a full-width colliding floor at the club level (the connector
    // and flight 0 both meet it). Top surface at bottomY.
    addCol(wxMin, wxMax, tc.bottomY - 0.6f, tc.bottomY, southZ - landHz, northZ + landHz);

    // SWITCHBACK RAMPS + TURN LANDINGS (walkable spine, bottom -> surface).
    for (int k = 0; k < nFlights; ++k) {
        const float y = flightY(k);
        if (k % 2 == 0) addRamp(SX, y, southZ, halfWX, run, flightRise, +1.0f);  // climb +Z
        else            addRamp(SX, y, northZ, halfWX, run, flightRise, -1.0f);  // climb -Z
        // Turn landing at the TOP of this flight (Y_{k+1}, opposite Z end).
        const float ty = flightY(k + 1), tz = landZ(k + 1);
        addCol(wxMin, wxMax, ty - 0.6f, ty, tz - landHz, tz + landHz);
        // A warm WORKLIGHT over each landing — strung down the shaft. Bright enough
        // to pool on the rock (falloff = depth), so the bore reads as a lit descent
        // and looking down the shaft shows a receding chain of pools.
        addLight(SX, ty + 2.0f, tz, 1.45f, 1.15f, 0.82f, 17.0f);
    }

    // BORE LINER — 4 rock walls (framed for the openings) + top cap + it is closed.
    //  -X wall: solid, MINUS the connector doorway at the bottom (z around the club
    //           doorway, y just above the floor).
    {
        std::vector<Hole> minusXHoles = {
            { SZ - 2.2f, SZ + 2.2f, tc.bottomY - 0.5f, tc.bottomY + 5.0f }
        };
        wallXHoles(linXin - 1.5f, linXin, linZ0, linZ1, linY0, linY1, minusXHoles);
    }
    //  +X wall: solid, MINUS the 3 offshoot doorways.
    wallXHoles(linXout, linXout + 1.5f, linZ0, linZ1, linY0, linY1, plusXHoles);
    //  -Z / +Z end walls (full).
    addVis(linXin - 1.5f, linXout + 1.5f, linY0, linY1, linZ0 - 1.5f, linZ0);
    addVis(linXin - 1.5f, linXout + 1.5f, linY0, linY1, linZ1, linZ1 + 1.5f);
    //  Top CAP (fills the bore roof up to the surface ground plane, Y=0).
    addVis(linXin - 1.5f, linXout + 1.5f, tc.topY, 0.0f, linZ0 - 1.5f, linZ1 + 1.5f);

    // CONNECTOR — a short rock corridor from the shaft bottom WEST into the club
    // east doorway (walkable; the club shell is open below 2.8 m at the doorway).
    {
        const float y  = tc.bottomY;                 // walk at the club floor level
        const float cx0 = tc.clubDoorX - 2.0f;       // overlap into the club a touch
        const float cx1 = wxMin + 1.0f;              // overlap into the bore floor
        const float z0 = tc.clubDoorZ - 2.2f, z1 = tc.clubDoorZ + 2.2f;
        addCol(cx0, cx1, y - 0.6f, y, z0, z1);                    // floor
        addVis(cx0, cx1, y, y + 4.6f, z0 - 0.5f, z0);            // -Z wall
        addVis(cx0, cx1, y, y + 4.6f, z1, z1 + 0.5f);            // +Z wall
        addVis(cx0, cx1, y + 4.1f, y + 4.6f, z0 - 0.5f, z1 + 0.5f);   // ceiling
        addLight(tc.clubDoorX + 5.0f, y + 2.4f, tc.clubDoorZ, 1.40f, 1.10f, 0.80f, 16.0f);
    }

    // OFFSHOOTS — passages branching off the +X liner at the chosen landings.
    for (int o = 0; o < 3; ++o) {
        const float lz = landZ(offK[o]), ly = flightY(offK[o]);
        const float y  = ly;                          // passage floor at the landing Y
        const float mouthX = linXout + 1.5f;          // just outside the +X wall
        const float cz0 = lz - 3.0f, cz1 = lz + 3.0f; // corridor Z (matches the door)
        if (offCrystal[o]) {
            // CRYSTAL CHAMBER: a short corridor opening into a wider room with a
            // Salvari crystal hollow (the reward). Corridor:
            const float corrX = mouthX + 12.0f;
            addCol(wxMax, corrX, y - 0.6f, y, cz0, cz1);          // floor (from the wall out)
            addVis(mouthX, corrX, y, y + 4.6f, cz0 - 0.5f, cz0);  // -Z wall
            addVis(mouthX, corrX, y, y + 4.6f, cz1, cz1 + 0.5f);  // +Z wall
            addVis(mouthX, corrX, y + 4.1f, y + 4.6f, cz0 - 0.5f, cz1 + 0.5f);  // ceiling
            // Chamber (wider in Z):
            const float chX = corrX + 16.0f;
            const float chz0 = lz - 8.0f, chz1 = lz + 8.0f;
            const float chTop = y + 8.0f;
            addCol(corrX, chX, y - 0.6f, y, chz0, chz1);          // floor
            addVis(corrX, chX, y, chTop, chz0 - 0.5f, chz0);      // -Z wall
            addVis(corrX, chX, y, chTop, chz1, chz1 + 0.5f);      // +Z wall
            addVis(chX, chX + 0.5f, y, chTop, chz0 - 0.5f, chz1 + 0.5f);  // far endcap
            addVis(corrX, chX, chTop - 0.5f, chTop, chz0 - 0.5f, chz1 + 0.5f);  // ceiling
            addVis(corrX, corrX + 0.5f, y, chTop, chz0, cz0);     // front flank (-Z)
            addVis(corrX, corrX + 0.5f, y, chTop, cz1, chz1);     // front flank (+Z)
            // The Salvari crystal hollow — reuse the deepened blue cluster + light.
            BedrockConfig bc;                          // default look for the hollow liner
            for (int i = 0; i < 4; ++i) bc.tint[i] = tc.tint[i];
            for (int i = 0; i < 4; ++i) bc.emissive[i] = tc.emissive[i];
            bc.uvScale = tc.uvScale;
            const float hcx = (corrX + chX) * 0.5f, hcz = lz;
            x3::rhi::PointLight bl =
                addSalvariHollow(scene, device, rockTex, bc, hcx, y + 4.0f, hcz, 1.05f);
            added += 7;
            if (outCrystalLights) outCrystalLights->push_back(bl);
        } else {
            // DEAD-END passage: a corridor of connected rock walls ending in a cap.
            const float endX = mouthX + (o == 0 ? 16.0f : 22.0f);
            addCol(wxMax, endX, y - 0.6f, y, cz0, cz1);           // floor
            addVis(mouthX, endX, y, y + 4.6f, cz0 - 0.5f, cz0);   // -Z wall
            addVis(mouthX, endX, y, y + 4.6f, cz1, cz1 + 0.5f);   // +Z wall
            addVis(mouthX, endX, y + 4.1f, y + 4.6f, cz0 - 0.5f, cz1 + 0.5f);  // ceiling
            addVis(endX, endX + 0.5f, y, y + 4.6f, cz0 - 0.5f, cz1 + 0.5f);    // dead-end cap
            addLight(endX - 3.0f, y + 2.4f, lz, 1.40f, 1.10f, 0.80f, 15.0f);
        }
    }

    return added;
}

} // namespace x3::game
