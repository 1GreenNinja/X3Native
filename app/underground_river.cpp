// THE UNDERGROUND RIVER — see underground_river.h. The trench itself is
// terrain.cpp's carve; this file builds the rock vault, the water and the
// light, and carries the --test-underriver gate.

#include "underground_river.h"
#include "terrain.h"
#include "asset_root.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace x3::game {

namespace {

// Deterministic value jitter (the mine_fx bore's trick): rocky displacement
// that is identical every boot and on every thread.
inline float rj(float a, float b) {
    uint32_t h = (uint32_t)((int)(a * 37.31f) * 374761393
                          + (int)(b * 17.77f) * 668265263);
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((float)((h ^ (h >> 16)) & 0xFFFF) / 65535.0f) * 2.0f - 1.0f;
}

struct CpuMesh {
    std::vector<x3::rhi::MeshVertex> v;
    std::vector<uint32_t> i;
};

// Walk the chain: position/value interpolation at along-length s, plus the
// unit direction of the containing segment.
struct ChainWalk {
    const UnderRiverChain& c;
    explicit ChainWalk(const UnderRiverChain& uc) : c(uc) {}
    void at(float s, float& x, float& z, float& w, float& nat,
            float& dx, float& dz) const {
        s = std::clamp(s, 0.0f, c.cum[c.n - 1]);
        int i = 0;
        while (i + 2 < c.n && c.cum[i + 1] < s) ++i;
        const float seg = std::max(c.cum[i + 1] - c.cum[i], 1e-3f);
        const float t = std::clamp((s - c.cum[i]) / seg, 0.0f, 1.0f);
        x = c.x[i] + (c.x[i + 1] - c.x[i]) * t;
        z = c.z[i] + (c.z[i + 1] - c.z[i]) * t;
        w = c.w[i] + (c.w[i + 1] - c.w[i]) * t;
        nat = c.natural[i] + (c.natural[i + 1] - c.natural[i]) * t;
        dx = (c.x[i + 1] - c.x[i]) / seg;
        dz = (c.z[i + 1] - c.z[i]) / seg;
    }
};

} // namespace

UndergroundRiver::Result UndergroundRiver::build(
        Scene& scene, x3::rhi::IRenderDevice& device,
        SurfaceLibrary* surfIn, std::vector<x3::rhi::PointLight>* outLights) {
    Result r{};
    const UnderRiverChain& uc = worldUnderRiverChain();
    if (uc.n < 2) return r;
    const float total = uc.cum[uc.n - 1];
    r.portalX = uc.x[uc.n - 1]; r.portalZ = uc.z[uc.n - 1];

    SurfaceLibrary localSurf;
    SurfaceLibrary& surf = surfIn ? *surfIn : localSurf;
    if (!surf.mounted()) surf.mount(assetRoot() + "/surface_library");
    const SurfaceSet& innerS = surf.get(device, "cv_rock_wet");    // cave rock, wet
    const SurfaceSet& outerS = surf.get(device, "terrain_rock");   // dry country rock

    // ---- THE VAULT: ring-stitched arch strips, head -> gorge mouth. -------
    // Feet planted INTO the trench walls (buried, no daylight seam), crown
    // riding just above the pre-carve natural ground so the hill closes.
    const float vaultEnd = total - kURGorgeLen;
    const ChainWalk walk(uc);
    constexpr int   kAcross = 9;       // ring verts across the arch
    constexpr float kStep = 12.0f;     // ring spacing (m)
    // Feet OUTSIDE the wall band (kURWallOutW = 24): planted into the natural
    // country and buried, so the lid spans the whole trench mouth. The first
    // build put them 15 m out at a floor-derived height — on the deep massif
    // reaches (natural 200+ m) that left a ring of open sky between vault
    // edge and trench rim, and the "vault" floated mid-slot (measured, U3's
    // first run + eyes-on reasoning). Feet now SAMPLE the built terrain.
    constexpr float kFootOut = 26.0f;
    auto buildStrip = [&](float s0, float s1, bool inner) {
        CpuMesh m;
        const int rings = std::max(2, (int)((s1 - s0) / kStep) + 1);
        for (int rg = 0; rg < rings; ++rg) {
            const float s = s0 + (s1 - s0) * ((float)rg / (float)(rings - 1));
            float cx, cz, w, nat, dx, dz;
            walk.at(s, cx, cz, w, nat, dx, dz);
            const float px = -dz, pz = dx;
            const float footYL = terrainHeightAtWorld(cx - px * kFootOut,
                                                      cz - pz * kFootOut) - 1.2f;
            const float footYR = terrainHeightAtWorld(cx + px * kFootOut,
                                                      cz + pz * kFootOut) - 1.2f;
            const float crownY = std::max(nat, std::max(footYL, footYR)) + 1.5f;
            for (int k = 0; k < kAcross; ++k) {
                const float u = (float)k / (float)(kAcross - 1);   // 0..1 across
                const float lat = (u * 2.0f - 1.0f) * kFootOut;    // -26..26
                // Arch from buried foot to buried foot through the crown.
                const float baseY = footYL + (footYR - footYL) * u;
                const float arch = std::sin(u * 3.14159265f);
                float y = baseY + (crownY - baseY) * arch;
                // Rocky displacement (kept off the feet so they stay buried).
                const float j = (k == 0 || k == kAcross - 1) ? 0.0f
                              : rj(s * 0.13f + (inner ? 3.7f : 9.1f), (float)k * 2.1f);
                y += j * 1.1f;
                const float latJ = lat + ((k == 0 || k == kAcross - 1) ? 0.0f
                                          : rj((float)k * 5.3f, s * 0.21f) * 1.4f);
                x3::rhi::MeshVertex v{};
                v.pos[0] = cx + px * latJ;
                v.pos[1] = y + (inner ? 0.0f : 0.55f);   // outer skin rides proud
                v.pos[2] = cz + pz * latJ;
                // Inner face lights from below (normal down-ish), outer from
                // above; exact normals matter less than orientation down here.
                v.normal[0] = 0.0f; v.normal[1] = inner ? -1.0f : 1.0f; v.normal[2] = 0.0f;
                v.uv[0] = s * 0.11f;                    // ~0.11 tiles/m along
                v.uv[1] = u * 3.4f;                     // across the arch
                m.v.push_back(v);
            }
        }
        for (int rg = 0; rg + 1 < rings; ++rg)
            for (int k = 0; k + 1 < kAcross; ++k) {
                const uint32_t a = (uint32_t)(rg * kAcross + k);
                const uint32_t b = a + 1;
                const uint32_t c2 = a + kAcross;
                const uint32_t d2 = c2 + 1;
                if (inner) m.i.insert(m.i.end(), { a, c2, b,  b, c2, d2 });
                else       m.i.insert(m.i.end(), { a, b, c2,  b, d2, c2 });
            }
        return m;
    };
    // Chunked (~500 m per entity) so distance culling has something to cull.
    const float kChunk = 500.0f;
    for (float s0 = 0.0f; s0 < vaultEnd - 1.0f; s0 += kChunk) {
        const float s1 = std::min(s0 + kChunk, vaultEnd);
        for (int inner = 0; inner < 2; ++inner) {
            CpuMesh m = buildStrip(s0, s1, inner == 0);
            if (m.v.empty()) continue;
            Entity e;
            e.mesh = device.createMesh(m.v.data(), (uint32_t)m.v.size(),
                                       m.i.data(), (uint32_t)m.i.size());
            const SurfaceSet& ss = inner == 0 ? innerS : outerS;
            e.tex = ss.albedo; e.normalTex = ss.normal; e.mrTex = ss.mr;
            const float tint = inner == 0 ? 0.62f : 0.80f;   // cave rock darker
            e.baseColor[0] = tint; e.baseColor[1] = tint;
            e.baseColor[2] = tint; e.baseColor[3] = 1.0f;
            e.tag = (uint32_t)Tag::Static;
            scene.add(e);
            ++r.vaultChunks;
        }
    }

    // ---- THE WATER: CaveRiver pointed at the open world. ------------------
    // Denser ribbon nodes than the carve chain (bends + drop steps read as
    // water, not as a low-poly strip); emissive brighter at rush + pools.
    std::vector<CaveRiverNode> wn;
    for (float s = 0.0f; s <= total + 0.1f; s += 24.0f) {
        float cx, cz, w, nat, dx, dz;
        walk.at(std::min(s, total), cx, cz, w, nat, dx, dz);
        // Per-node character from the nearest chain node.
        int ni = 0;
        while (ni + 1 < uc.n && uc.cum[ni + 1] < s) ++ni;
        const float t = std::clamp((s - uc.cum[ni]) /
                        std::max(uc.cum[ni + 1] - uc.cum[ni], 1e-3f), 0.0f, 1.0f);
        CaveRiverNode n;
        n.x = cx; n.y = w + 0.06f; n.z = cz;
        n.halfWidth = uc.hw[ni] + (uc.hw[std::min(ni + 1, uc.n - 1)] - uc.hw[ni]) * t;
        n.rush = uc.rush[ni] + (uc.rush[std::min(ni + 1, uc.n - 1)] - uc.rush[ni]) * t;
        n.pool = (t < 0.5f ? uc.pool[ni] : uc.pool[std::min(ni + 1, uc.n - 1)]);
        n.emissive = 0.30f + 0.25f * n.rush + (n.pool ? 0.10f : 0.0f);
        wn.push_back(n);
    }
    r.waterSegs = m_water.build(scene, device, wn, outLights);

    // ---- THE LIGHT: sparse cool accents down the cavern run. --------------
    if (outLights) {
        const size_t before = outLights->size();
        for (float s = 60.0f; s < vaultEnd; s += 110.0f) {
            float cx, cz, w, nat, dx, dz;
            walk.at(s, cx, cz, w, nat, dx, dz);
            x3::rhi::PointLight l;
            l.pos[0] = cx; l.pos[1] = w + 4.5f; l.pos[2] = cz;
            l.range = 16.0f;
            l.color[0] = 0.08f; l.color[1] = 0.18f; l.color[2] = 0.55f;
            outLights->push_back(l);
        }
        r.lightCount = (int)(outLights->size() - before);
    }

    m_built = r.waterSegs > 0;
    r.built = m_built;
    char b[220];
    std::snprintf(b, sizeof(b),
        "[under-river] built: %.0f m run, %d vault chunks (gorge open last %.0f m), "
        "%d water segs, %d lights; portal at (%.0f, %.0f)",
        uc.cum[uc.n - 1], r.vaultChunks, kURGorgeLen, r.waterSegs, r.lightCount,
        r.portalX, r.portalZ);
    x3::logInfo(b);
    return r;
}

// ---------------------------------------------------------------------------
// --test-underriver — the gate. Headless, no GPU textures needed (4.3 law:
// asserts structure, not pixels).
// ---------------------------------------------------------------------------
bool UndergroundRiver::runSelfTest() {
    int passN = 0, failN = 0;
    char d[300];
    auto check = [&](bool ok, const char* name, const char* detail = nullptr) {
        std::string m = std::string(ok ? "PASS " : "FAIL ") + name;
        if (detail && *detail) m += std::string(" — ") + detail;
        if (ok) { ++passN; x3::logInfo("[underriver] " + m); }
        else    { ++failN; x3::logError("[underriver] " + m); }
    };

    const UnderRiverChain& uc = worldUnderRiverChain();
    // The measured table, printed whole — the numbers this lane is tuned by.
    for (int i = 0; i < uc.n; ++i) {
        std::snprintf(d, sizeof(d),
            "[underriver]   node %2d (%7.1f, %7.1f) natural %7.2f water %7.2f "
            "cover %6.2f hw %.1f rush %.1f%s",
            i, uc.x[i], uc.z[i], uc.natural[i], uc.w[i],
            uc.natural[i] - uc.w[i], uc.hw[i], uc.rush[i],
            uc.pool[i] ? " POOL" : "");
        x3::logInfo(d);
    }

    // U1 — the river FLOWS: strictly descending, and it actually falls.
    {
        bool desc = uc.n >= 2;
        for (int i = 0; i + 1 < uc.n; ++i)
            if (uc.w[i + 1] >= uc.w[i]) desc = false;
        const float fall = uc.w[0] - uc.w[uc.n - 1];
        std::snprintf(d, sizeof(d), "%d nodes, total fall %.1f m over %.0f m",
                      uc.n, fall, uc.cum[uc.n - 1]);
        check(desc && fall > 6.0f, "U1 the underground river descends, head to plunge pool", d);
    }

    // U2 — UNDER the mountain: water below the pre-carve ground everywhere,
    // with real rock cover on the vaulted run (portal pool excepted).
    {
        float minCover = 1e9f; int at = -1;
        for (int i = 0; i + 1 < uc.n; ++i) {   // exclude the portal node
            const float cover = uc.natural[i] - uc.w[i];
            if (cover < minCover) { minCover = cover; at = i; }
        }
        std::snprintf(d, sizeof(d), "min cover %.2f m at node %d; portal cover %.2f m",
                      minCover, at, uc.natural[uc.n - 1] - uc.w[uc.n - 1]);
        check(minCover >= 0.85f, "U2 the run stays under the ground it bores", d);
    }

    // U3 — THE TRENCH IS CARVED: at each station the built terrain sits at
    // the bed under the spine (water floats over it) and at the BEACH SHELF
    // 8 m out — dry, walkable, above the waterline (CONTACT LAW ground).
    {
        const ChainWalk walk(uc);
        int bad = 0, stations = 0; float worstBed = 0, worstShelf = 0;
        float wsx = 0, wsz = 0;
        for (float s = 6.0f; s < uc.cum[uc.n - 1] - 6.0f; s += 18.0f) {
            float cx, cz, w, nat, dx, dz; walk.at(s, cx, cz, w, nat, dx, dz);
            const float px = -dz, pz = dx;
            ++stations;
            const float bedT = terrainHeightAtWorld(cx, cz);
            const float bedErr = bedT - w;              // want < 0 (bed under water)
            if (bedErr > -0.8f) { ++bad; worstBed = std::max(worstBed, bedErr); }
            for (int side = -1; side <= 1; side += 2) {
                const float sx = cx + px * 8.0f * (float)side;
                const float sz = cz + pz * 8.0f * (float)side;
                const float shelfT = terrainHeightAtWorld(sx, sz);
                const float lift = shelfT - w;          // want ~ +kURShelfLift
                if (lift < 0.1f || lift > 1.4f) {
                    ++bad;
                    if (std::fabs(lift) > worstShelf) { worstShelf = std::fabs(lift); wsx = sx; wsz = sz; }
                }
            }
        }
        std::snprintf(d, sizeof(d),
            "%d stations; %d bad probes (worst bed err %.2f, worst shelf dev %.2f at (%.0f, %.0f))",
            stations, bad, worstBed, worstShelf, wsx, wsz);
        check(bad == 0 && stations > 50, "U3 bed under water, rock beaches dry above it, whole run", d);
    }

    // U4 — ONE WATER TRUTH: worldWaterLevelAt on the spine IS the table; a
    // point past the water half-width on the beach shelf is DRY.
    {
        const ChainWalk walk(uc);
        float worst = 0.0f; int wetShelf = 0;
        for (float s = 6.0f; s < uc.cum[uc.n - 1] - 6.0f; s += 15.0f) {
            float cx, cz, w, nat, dx, dz; walk.at(s, cx, cz, w, nat, dx, dz);
            const float q = worldWaterLevelAt(cx, cz);
            if (q > kWorldWaterDry + 1.0f)
                worst = std::max(worst, std::fabs(q - w));
            else
                worst = 1e9f;                            // dry mid-channel = broken
            const float px = -dz, pz = dx;
            if (worldWaterLevelAt(cx + px * 10.5f, cz + pz * 10.5f) >
                kWorldWaterDry + 1.0f) ++wetShelf;       // 10.5 m > every hw
        }
        std::snprintf(d, sizeof(d),
            "largest spine drawn-vs-query gap %.4f m; %d wet probes on the dry shelf",
            worst, wetShelf);
        check(worst < 0.01f && wetShelf == 0, "U4 the query and the table are ONE truth", d);
    }

    // U5 — whitewater + pools exist as authored (the rushing-water read).
    {
        int rushN = 0, poolN = 0;
        for (int i = 0; i < uc.n; ++i) {
            if (uc.rush[i] >= 0.8f) ++rushN;
            if (uc.pool[i]) ++poolN;
        }
        std::snprintf(d, sizeof(d), "%d full-rush drops, %d pools", rushN, poolN);
        check(rushN >= 2 && poolN >= 2, "U5 drops and pools are in the table", d);
    }

    // U6 — determinism: a second read of the chain is bit-identical (the
    // derivation must not depend on corridor registration or call order).
    {
        const UnderRiverChain& uc2 = worldUnderRiverChain();
        bool same = uc2.n == uc.n;
        for (int i = 0; same && i < uc.n; ++i)
            same = uc2.w[i] == uc.w[i] && uc2.natural[i] == uc.natural[i];
        check(same, "U6 the derived table is stable");
    }

    std::snprintf(d, sizeof(d), "--test-underriver: %d/%d passed", passN, passN + failN);
    if (failN) x3::logError(d); else x3::logInfo(d);
    return failN == 0;
}

} // namespace x3::game
