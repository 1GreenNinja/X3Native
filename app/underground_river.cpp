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
#include <cstdlib>
#include <string>
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

    // ---- THE VAULT: the LID that puts the hillside back. ------------------
    // Cut-and-cover's cover. The trench carve pulled the country down to the
    // water; this restores the surface it removed, so from outside the hill is
    // shut and from inside the void between carved floor and restored surface
    // IS the cavern — a lens, tallest over the channel, closing to nothing at
    // the band edge. That is why every lid vertex SAMPLES
    // worldPreUnderRiverHeight (the ground before this trench existed) rather
    // than arching between two feet: the first build arched from foot to foot
    // through a single crown, which on the west valley's sloped flanks bulged
    // the crown up to 15 m above the hillside it was supposed to hide.
    const float vaultEnd = total - kURGorgeLen;
    const ChainWalk walk(uc);
    constexpr int   kAcross = 13;      // ring verts across the lid
    constexpr float kStep = 10.0f;     // ring spacing (m)
    const float kFootOut = kURWallOutW + 2.0f;   // feet just outside the carve
    auto buildStrip = [&](float s0, float s1, bool inner) {
        CpuMesh m;
        const int rings = std::max(2, (int)((s1 - s0) / kStep) + 1);
        for (int rg = 0; rg < rings; ++rg) {
            const float s = s0 + (s1 - s0) * ((float)rg / (float)(rings - 1));
            float cx, cz, w, nat, dx, dz;
            walk.at(s, cx, cz, w, nat, dx, dz);
            const float px = -dz, pz = dx;
            for (int k = 0; k < kAcross; ++k) {
                const float u = (float)k / (float)(kAcross - 1);   // 0..1 across
                const float lat = (u * 2.0f - 1.0f) * kFootOut;
                const bool  foot = (k == 0 || k == kAcross - 1);
                // Lateral jitter first: the lid is sampled AT the jittered
                // point, so a rough edge still lands on the real ground.
                const float latJ = lat + (foot ? 0.0f
                                        : rj((float)k * 5.3f, s * 0.21f) * 1.6f);
                const float vx = cx + px * latJ, vz = cz + pz * latJ;
                const float ground = worldPreUnderRiverHeight(vx, vz);
                // EVERY offset tapers to nothing at the rim (sin over the
                // span). The carve already eases back to the natural country
                // by kURWallOutW, so a lid that stayed a constant 0.35 m proud
                // would draw a 1.7 km rock lip along both edges of the
                // corridor; converging both skins onto the ground there closes
                // the lens instead, and the feet tuck the seam under.
                const float archT = std::sin(u * 3.14159265f);
                float y;
                if (foot) {
                    y = ground - 1.4f;                 // tucked under the country
                } else if (inner) {
                    // A ROUGH ROCK CEILING: displacement only ever hangs DOWN
                    // into the void (never up through the hill), and only where
                    // the void is tall enough to take it.
                    const float room = std::max(ground - (w + kURShelfLift), 0.0f);
                    const float amp  = std::min(2.6f, room * 0.12f) * archT;
                    y = ground - 0.30f * archT
                      - (rj(s * 0.13f + 3.7f, (float)k * 2.1f) * 0.5f + 0.5f) * amp;
                } else {
                    y = ground + (0.35f + rj(s * 0.13f + 9.1f, (float)k * 2.1f)
                                          * 0.28f) * archT;
                }
                x3::rhi::MeshVertex v{};
                v.pos[0] = vx;
                v.pos[1] = y;
                v.pos[2] = vz;
                // Inner face lights from below (normal down-ish), outer from
                // above; exact normals matter less than orientation down here.
                v.normal[0] = 0.0f; v.normal[1] = inner ? -1.0f : 1.0f; v.normal[2] = 0.0f;
                v.uv[0] = s * 0.11f;                    // ~0.11 tiles/m along
                v.uv[1] = u * 4.6f;                     // across the lid
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

    // ---- THE ROCK BEACHES -------------------------------------------------
    // The shelves themselves are HEIGHT FIELD (that is the whole point of
    // cut-and-cover: the trench IS the cavern floor, so collision, streaming
    // and the CONTACT LAW need no special case down here). But the terrain
    // splat picks its material from world height + slope, and the shelf is
    // flat lowland — it would paint the owner's rock beaches GRASS, indoors.
    // So the beaches get an APRON: an authored water-worn rock skin laid 7 cm
    // over the carved shelf, the same "rides proud" idiom as the arrival decks
    // (ENGINE_GOTCHAS 3.5). Collision stays the terrain underneath, so the
    // apron cannot lift a boot off the ground it is standing on.
    const SurfaceSet& beachS = surf.get(device, "cv_rock_flume");  // water-worn rock
    {
        constexpr int   kAcross = 13;
        constexpr float kStep   = 8.0f;
        const float outW = kURShelfHalfW + 9.0f;   // bed + beach + the wall's toe
        const float kChunkB = 320.0f;
        for (float s0 = 0.0f; s0 < total - 1.0f; s0 += kChunkB) {
            const float s1 = std::min(s0 + kChunkB, total);
            CpuMesh m;
            const int rings = std::max(2, (int)((s1 - s0) / kStep) + 1);
            for (int rg = 0; rg < rings; ++rg) {
                const float s = s0 + (s1 - s0) * ((float)rg / (float)(rings - 1));
                float cx, cz, w, nat, dx, dz;
                walk.at(s, cx, cz, w, nat, dx, dz);
                const float px = -dz, pz = dx;
                for (int k = 0; k < kAcross; ++k) {
                    const float u = (float)k / (float)(kAcross - 1);
                    const float lat = (u * 2.0f - 1.0f) * outW;
                    const float vx = cx + px * lat, vz = cz + pz * lat;
                    x3::rhi::MeshVertex v{};
                    v.pos[0] = vx;
                    v.pos[1] = terrainHeightAtWorld(vx, vz) + 0.07f;
                    v.pos[2] = vz;
                    // The apron climbs the trench wall's toe, which is a ~56
                    // deg face — a flat +Y normal there would shade the wall
                    // like a floor. Take the field's own normal.
                    terrainNormalAtWorld(vx, vz, v.normal);
                    v.uv[0] = s * 0.14f; v.uv[1] = lat * 0.14f;   // world-scaled
                    m.v.push_back(v);
                }
            }
            for (int rg = 0; rg + 1 < rings; ++rg)
                for (int k = 0; k + 1 < kAcross; ++k) {
                    const uint32_t a = (uint32_t)(rg * kAcross + k);
                    const uint32_t b = a + 1, c2 = a + kAcross, d2 = c2 + 1;
                    m.i.insert(m.i.end(), { a, c2, b,  b, c2, d2 });
                }
            if (m.v.empty()) continue;
            Entity e;
            e.mesh = device.createMesh(m.v.data(), (uint32_t)m.v.size(),
                                       m.i.data(), (uint32_t)m.i.size());
            e.tex = beachS.albedo; e.normalTex = beachS.normal; e.mrTex = beachS.mr;
            e.baseColor[0] = e.baseColor[1] = e.baseColor[2] = 0.74f;
            e.baseColor[3] = 1.0f;
            e.tag = (uint32_t)Tag::Static;
            scene.add(e);
            ++r.beachChunks;
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
        // The ribbon is SELF-LUMINESCENT because there is no sun down here —
        // but the last reach is the open gorge, where there is. Fade the glow
        // out as the lid ends or the plunge pool reads as a neon strip lying
        // in daylight.
        const float openT = std::clamp((s - (vaultEnd - 40.0f)) /
                                       std::max(total - (vaultEnd - 40.0f), 1.0f),
                                       0.0f, 1.0f);
        const float daylight = openT * openT * (3.0f - 2.0f * openT);
        n.emissive = (0.30f + 0.25f * n.rush + (n.pool ? 0.10f : 0.0f))
                   * (1.0f - 0.92f * daylight);
        wn.push_back(n);
    }
    r.waterSegs = m_water.build(scene, device, wn, outLights);

    // ---- THE MIST: emitters on the steps and the pools. -------------------
    // Density follows the water's own character: a rushing reach throws spray,
    // a pool just breathes. Both come from the derived rush/pool table, so a
    // reroute moves the mist with the river and nothing goes stale.
    m_mist.clear();
    for (float s = 0.0f; s <= total; s += 26.0f) {
        float cx, cz, w, nat, dx, dz;
        walk.at(s, cx, cz, w, nat, dx, dz);
        int ni = 0;
        while (ni + 1 < uc.n && uc.cum[ni + 1] < s) ++ni;
        const float t = std::clamp((s - uc.cum[ni]) /
                        std::max(uc.cum[ni + 1] - uc.cum[ni], 1e-3f), 0.0f, 1.0f);
        const float rush = uc.rush[ni] +
                           (uc.rush[std::min(ni + 1, uc.n - 1)] - uc.rush[ni]) * t;
        const bool pool = (t < 0.5f ? uc.pool[ni] : uc.pool[std::min(ni + 1, uc.n - 1)]);
        if (rush < 0.35f && !pool) continue;      // still water does not steam
        MistSource ms;
        ms.x = cx; ms.y = w + 0.25f; ms.z = cz;
        ms.dx = dx; ms.dz = dz; ms.rush = rush;
        m_mist.push_back(ms);
    }
    m_puffs.assign(m_mist.empty() ? 0u : 420u, Puff{});
    r.mistSources = (int)m_mist.size();

    // ---- THE LIGHT: cool accents down the whole cavern run. ---------------
    // Kept in OUR OWN array, not pushed into the host's boot-time block: the
    // host re-uploads one merged light array every frame and a boot push does
    // not survive the first of them (see nearestLights()). Dense enough that
    // wherever you stand down here something is lit, because only the nearest
    // handful are ever uploaded.
    m_lights.clear();
    for (float s = 30.0f; s < total; s += 42.0f) {
        float cx, cz, w, nat, dx, dz;
        walk.at(s, cx, cz, w, nat, dx, dz);
        int ni = 0;
        while (ni + 1 < uc.n && uc.cum[ni + 1] < s) ++ni;
        const bool nearPool = uc.pool[ni] ||
                              uc.pool[std::min(ni + 1, uc.n - 1)];
        x3::rhi::PointLight l;
        // Off the spine and up: a wash across the beach and the wall, not a
        // lamp hanging over the middle of the channel.
        const float px = -dz, pz = dx;
        const float side = (std::fmod(s / 42.0f, 2.0f) < 1.0f) ? 1.0f : -1.0f;
        l.pos[0] = cx + px * 9.0f * side;
        l.pos[1] = w + 3.2f;
        l.pos[2] = cz + pz * 9.0f * side;
        l.range = nearPool ? 34.0f : 26.0f;
        // Cold cave glow — the same blue family CaveRiver's pool banks use, so
        // the accents and the water read as one light source, not two.
        const float k = nearPool ? 1.35f : 1.0f;
        l.color[0] = 0.10f * k; l.color[1] = 0.22f * k; l.color[2] = 0.60f * k;
        m_lights.push_back(l);
    }
    r.lightCount = (int)m_lights.size();
    (void)outLights;   // CaveRiver's pool bank lights already went in above

    m_built = r.waterSegs > 0;
    r.built = m_built;
    char b[300];
    std::snprintf(b, sizeof(b),
        "[under-river] built: %.0f m run, %d vault chunks (gorge open last %.0f m), "
        "%d beach chunks, %d water segs, %d mist sources, %d lights; "
        "portal at (%.0f, %.0f)",
        uc.cum[uc.n - 1], r.vaultChunks, kURGorgeLen, r.beachChunks, r.waterSegs,
        r.mistSources, r.lightCount, r.portalX, r.portalZ);
    x3::logInfo(b);
    return r;
}

bool UndergroundRiver::insideCorridor(const float p[3]) {
    const UnderRiverChain& uc = worldUnderRiverChain();
    return uc.n >= 2 && p[0] > uc.bx0 && p[0] < uc.bx1
                     && p[2] > uc.bz0 && p[2] < uc.bz1;
}

uint32_t UndergroundRiver::nearestLights(const float cam[3],
                                         x3::rhi::PointLight* out,
                                         uint32_t maxN) const {
    if (!m_built || !out || maxN == 0 || m_lights.empty()) return 0;
    struct Scored { float d2; uint32_t i; };
    std::vector<Scored> s;
    s.reserve(m_lights.size());
    for (uint32_t i = 0; i < (uint32_t)m_lights.size(); ++i) {
        const float dx = m_lights[i].pos[0] - cam[0];
        const float dy = m_lights[i].pos[1] - cam[1];
        const float dz = m_lights[i].pos[2] - cam[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < 340.0f * 340.0f) s.push_back({ d2, i });
    }
    const uint32_t k = std::min<uint32_t>((uint32_t)s.size(), maxN);
    if (k == 0) return 0;
    std::partial_sort(s.begin(), s.begin() + k, s.end(),
                      [](const Scored& a, const Scored& b) { return a.d2 < b.d2; });
    for (uint32_t i = 0; i < k; ++i) out[i] = m_lights[s[i].i];
    return k;
}

// ---------------------------------------------------------------------------
// THE MIST — RiverLife's wake-puff system aimed underground (rule 1). Spray
// off the whitewater steps is ADDITIVE (it catches the bank lights and feeds
// bloom); the cold breath lying on the pools is ALPHA haze. Both are cheap
// billboards through the one particle pass the rain and the wake already use.
// ---------------------------------------------------------------------------
void UndergroundRiver::update(float dt, Scene& scene) {
    if (!m_built) return;
    m_water.update(dt, scene);
    if (m_puffs.empty()) return;
    auto rnd = [&]() {
        m_seed = m_seed * 1664525u + 1013904223u;
        return (float)((m_seed >> 8) & 0xFFFF) / 65535.0f;
    };
    for (MistSource& ms : m_mist) {
        // A rushing step throws several puffs a second; a pool exhales one.
        const float rate = 0.7f + 5.5f * ms.rush;
        ms.acc += dt * rate;
        while (ms.acc >= 1.0f) {
            ms.acc -= 1.0f;
            Puff& p = m_puffs[m_puffNext];
            m_puffNext = (m_puffNext + 1) % (uint32_t)m_puffs.size();
            const float px = -ms.dz, pz = ms.dx;
            const float lat = (rnd() * 2.0f - 1.0f) * (2.5f + 3.0f * ms.rush);
            p.x = ms.x + px * lat + ms.dx * (rnd() * 8.0f - 4.0f);
            p.y = ms.y + rnd() * 0.6f;
            p.z = ms.z + pz * lat + ms.dz * (rnd() * 8.0f - 4.0f);
            p.spray = ms.rush > 0.5f && rnd() < ms.rush;
            // Spray is thrown downstream and up; pool haze barely moves.
            const float thr = p.spray ? (1.4f + 2.2f * ms.rush) : 0.15f;
            p.vx = ms.dx * thr + (rnd() - 0.5f) * 0.5f;
            p.vz = ms.dz * thr + (rnd() - 0.5f) * 0.5f;
            p.vy = p.spray ? (0.9f + 1.4f * rnd()) : (0.10f + 0.14f * rnd());
            p.life = p.spray ? (1.3f + 1.2f * rnd()) : (5.0f + 4.0f * rnd());
            p.size0 = p.spray ? (0.5f + 0.7f * rnd()) : (2.4f + 2.4f * rnd());
            p.age = 0.0f;
        }
    }
    for (Puff& p : m_puffs) {
        if (p.age >= p.life) continue;
        p.age += dt;
        p.x += p.vx * dt; p.y += p.vy * dt; p.z += p.vz * dt;
        // Spray arcs and falls back; haze keeps drifting up into the dark.
        if (p.spray) p.vy -= 2.6f * dt;
        p.vx *= (1.0f - 1.1f * dt); p.vz *= (1.0f - 1.1f * dt);
    }
}

void UndergroundRiver::render(x3::rhi::IRenderDevice& device,
                              const x3::rhi::FrameContext&) {
    if (!m_built || m_puffs.empty()) return;
    m_hazeOut.clear(); m_sprayOut.clear();
    for (const Puff& p : m_puffs) {
        if (p.age >= p.life) continue;
        const float t = p.age / p.life;
        x3::rhi::IRenderDevice::ParticleInstance pi;
        pi.pos[0] = p.x; pi.pos[1] = p.y; pi.pos[2] = p.z;
        if (p.spray) {
            pi.size = p.size0 * (1.0f + 1.5f * t);
            // Fade IN over the first fifth, then out: a droplet burst that
            // pops into existence at full brightness reads as a sprite bug.
            const float a = std::min(t * 5.0f, 1.0f) * (1.0f - t) * 0.55f;
            pi.color[0] = 0.52f * a; pi.color[1] = 0.60f * a; pi.color[2] = 0.70f * a;
            pi.color[3] = 1.0f;
            m_sprayOut.push_back(pi);
        } else {
            pi.size = p.size0 * (1.0f + 1.8f * t);
            pi.color[0] = 0.62f; pi.color[1] = 0.68f; pi.color[2] = 0.76f;
            pi.color[3] = 0.16f * std::min(t * 6.0f, 1.0f) * (1.0f - t) * (1.0f - t);
            m_hazeOut.push_back(pi);
        }
    }
    if (!m_hazeOut.empty())
        device.submitParticles(m_hazeOut.data(), (uint32_t)m_hazeOut.size(),
                               x3::rhi::IRenderDevice::ParticleBlend::Alpha);
    if (!m_sprayOut.empty())
        device.submitParticles(m_sprayOut.data(), (uint32_t)m_sprayOut.size(),
                               x3::rhi::IRenderDevice::ParticleBlend::Additive);
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

    // ROUTE SCAN (X3_UR_SCAN=1) — the pre-corridor country the route is picked
    // ON (NO_SLOP rule 9: the first authored route was drawn from the map and
    // measured 228 m of massif over nodes 7-8, which cut-and-cover cannot
    // express; every node below is now chosen off THIS grid). Diagnostic only.
    if (const char* sc = std::getenv("X3_UR_SCAN"); sc && sc[0]) {
        float x0 = -1200, x1 = -100, z0 = -600, z1 = 1150, st = 50;
        std::sscanf(sc, "%f,%f,%f,%f,%f", &x0, &x1, &z0, &z1, &st);
        std::snprintf(d, sizeof(d),
                      "[underriver] SCAN pre-UR ground: x %.0f..%.0f (columns) "
                      "z %.0f..%.0f, step %.0f", x0, x1, z0, z1, st);
        x3::logInfo(d);
        { std::string hdr = "        ";
          char cell[16];
          for (float x = x0; x <= x1; x += st) {
              std::snprintf(cell, sizeof(cell), "%5.0f", x); hdr += cell; }
          x3::logInfo("[underriver] x=  " + hdr); }
        for (float z = z1; z >= z0; z -= st) {
            std::string row;
            char cell[16];
            std::snprintf(cell, sizeof(cell), "z%+6.0f:", z);
            row = cell;
            for (float x = x0; x <= x1; x += st) {
                std::snprintf(cell, sizeof(cell), "%5.0f",
                              worldPreUnderRiverHeight(x, z));
                row += cell;
            }
            x3::logInfo("[underriver] " + row);
        }
    }

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

    // U3 — THE TRENCH IS CARVED, AND ITS BEACHES ARE WALKABLE ROCK. Two
    // invariants, not a shape: (1) the bed under the spine is below the water,
    // so the ribbon floats on a floor rather than over the countryside, and
    // (2) the beach is DRY and STANDABLE — the owner asked for rock beaches
    // and NO_SLOP rule 11 says a surface a character can be on is a surface
    // that has to hold them up.
    //
    // What this gate deliberately does NOT assert is a beach of constant
    // height. An earlier version wanted the shelf within 1.4 m of the water
    // everywhere and failed on the outside of bends — where the polyline's
    // nearest point is the VERTEX, the effective distance exceeds the lateral
    // offset, and the beach correctly narrows into the wall. That is a real
    // river bank, not a defect; the tolerance was the bug.
    {
        const ChainWalk walk(uc);
        int wetBeach = 0, steepBeach = 0, badBed = 0, stations = 0;
        float worstBed = 0, worstLift = 1e9f, worstNy = 1.0f;
        float wsx = 0, wsz = 0;
        for (float s = 6.0f; s < uc.cum[uc.n - 1] - 6.0f; s += 18.0f) {
            float cx, cz, w, nat, dx, dz; walk.at(s, cx, cz, w, nat, dx, dz);
            const float px = -dz, pz = dx;
            ++stations;
            const float bedT = terrainHeightAtWorld(cx, cz);
            const float bedErr = bedT - w;              // want < 0 (bed under water)
            if (bedErr > -0.8f) { ++badBed; worstBed = std::max(worstBed, bedErr); }
            // The beach: outside even a POOL's widened channel floor (the carve
            // starts the beach at max(kURShelfHalfW, hw+0.8+3.5) = 13.8 m at
            // the 9.5 m pools), so this lands on shelf everywhere on the run.
            const float bd = kURShelfHalfW + 3.0f;
            for (int side = -1; side <= 1; side += 2) {
                const float sx = cx + px * bd * (float)side;
                const float sz = cz + pz * bd * (float)side;
                const float lift = terrainHeightAtWorld(sx, sz) - w;
                if (lift < worstLift) { worstLift = lift; wsx = sx; wsz = sz; }
                if (lift < 0.1f) ++wetBeach;            // beach under the water
                float n[3]; terrainNormalAtWorld(sx, sz, n);
                worstNy = std::min(worstNy, n[1]);
                if (n[1] < 0.72f) ++steepBeach;         // > ~44 deg: not standable
            }
        }
        std::snprintf(d, sizeof(d),
            "%d stations; bed %d bad (worst err %.2f); beach %d wet / %d too steep "
            "(lowest beach %.2f m over the water at (%.0f, %.0f), worst normal.y %.2f)",
            stations, badBed, worstBed, wetBeach, steepBeach, worstLift, wsx, wsz, worstNy);
        check(badBed == 0 && wetBeach == 0 && steepBeach == 0 && stations > 50,
              "U3 bed under the water, beaches dry and walkable, whole run", d);
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
            const float dry = kURShelfHalfW + 3.0f;   // > every node half-width
            if (worldWaterLevelAt(cx + px * dry, cz + pz * dry) >
                kWorldWaterDry + 1.0f) ++wetShelf;
        }
        std::snprintf(d, sizeof(d),
            "largest spine drawn-vs-query gap %.4f m; %d wet probes on the dry shelf",
            worst, wetShelf);
        check(worst < 0.01f && wetShelf == 0, "U4 the query and the table are ONE truth", d);
    }

    // U5 — the owner's RUSHING WATER is really in the table. rush is derived
    // from the gradient the country forces (terrain.h), so this gate is a
    // statement about the ROUTE: it must actually fall somewhere, not glide.
    {
        int rushN = 0, poolN = 0; float maxRush = 0.0f;
        for (int i = 0; i < uc.n; ++i) {
            if (uc.rush[i] >= 0.55f) ++rushN;
            maxRush = std::max(maxRush, uc.rush[i]);
            if (uc.pool[i]) ++poolN;
        }
        std::snprintf(d, sizeof(d), "%d rushing reaches (max rush %.2f), %d pools",
                      rushN, maxRush, poolN);
        check(rushN >= 2 && maxRush >= 0.9f && poolN >= 2,
              "U5 the run rushes at the steps and stills at the pools", d);
    }

    // U7 — THE COVER BUDGET. Cut-and-cover's walls have to climb out of the
    // trench inside the wall band; cover IS the cavern's height and is bounded
    // by what that band can carry (terrain.h's mechanism note). A route that
    // busts this gets MOVED — forcing it is how you get a 200 m slot.
    {
        float wall = 0.0f, minCover = 1e9f; int atWall = -1, atMin = -1;
        for (int i = 0; i < uc.n; ++i) {
            const float cov = uc.floorMin[i] - uc.w[i];       // roof over the water
            const float wal = uc.floorMax[i] - uc.w[i];       // the climb out
            if (wal > wall) { wall = wal; atWall = i; }
            if (cov < minCover) { minCover = cov; atMin = i; }
        }
        const float deg = std::atan(wall / (kURWallOutW - kURShelfHalfW))
                        * 57.2957795f;
        std::snprintf(d, sizeof(d),
            "thinnest roof %.1f m (node %d); tallest trench wall %.1f m (node %d) "
            "over the %.0f m band = %.1f deg (limit %.0f)",
            minCover, atMin, wall, atWall, kURWallOutW - kURShelfHalfW,
            deg, kURWallMaxDeg);
        check(deg <= kURWallMaxDeg && minCover >= kURCoverMin - 0.01f,
              "U7 the route stays inside what cut-and-cover can build", d);
    }

    // U8 — THE CARVE GUARD. Every authored cut is multiplied by the facility /
    // city-pad / outpost guard, so a spine that strays into one is NOT DUG
    // while worldWaterLevelAt still reports wet — water hanging in mid-air
    // over solid ground, which is JOB 1's defect reintroduced underground.
    // Measured across the WHOLE band, not just the spine.
    {
        const ChainWalk walk(uc);
        float worst = 1.0f, wx = 0, wz = 0;
        for (float s = 0.0f; s <= uc.cum[uc.n - 1]; s += 12.0f) {
            float cx, cz, w, nat, dx, dz; walk.at(s, cx, cz, w, nat, dx, dz);
            const float px = -dz, pz = dx;
            for (int k = -3; k <= 3; ++k) {
                const float lat = (float)k * (kURWallOutW / 3.0f);
                const float g = worldCarveGuardAt(cx + px * lat, cz + pz * lat);
                if (g < worst) { worst = g; wx = cx + px * lat; wz = cz + pz * lat; }
            }
        }
        std::snprintf(d, sizeof(d),
                      "weakest carve guard across the corridor %.3f at (%.0f, %.0f)",
                      worst, wx, wz);
        check(worst >= 0.999f, "U8 the whole corridor is allowed to be dug", d);
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
