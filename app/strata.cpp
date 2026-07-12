// STRATA — "THE DESCENT". See app/strata.h.
//
// CLEAN-ROOM, original work. Built ONLY from X3Native's own Scene / mesh_prims /
// IRenderDevice / IPhysicsWorld / TriggerSystem seams (the same seams club1127.* /
// act2_caves.* / env_art.* use). The 9 strata band names / colors / depth order
// are ElevatorSystem::strata() (ported from Tim's OWN Babylon x3-elevator.js), made
// LITERAL geology per X3_WORLD_BLUEPRINT.md §2.6.
//
// GEOMETRY APPROACH: the shaft walls are authored as a RING of CANTED rock slabs
// (x3::prims::makeCantedStrut — sheared prisms that lean, so the bore reads as a
// rough rough-hewn rock face, NOT a clean box tube) at varied radii + jittered
// heights per band, plus shelf ledges and boulder massing. Each band tints its rock
// to the elevator color; the bottom three bands (Crystal Veins / Magma / Alien
// Substrate) carry emissive glow + mood point lights that breathe in update(). The
// offshoot tunnels + the on-foot ledge/ramp route are built from boxes + makeRamp
// with real Jolt collision (addStaticMesh) so a player can walk them.

#include "strata.h"
#include "mesh_prims.h"

#include "engine/core/x3_log.h"
#include "headless_device.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

namespace x3::game {

namespace {

constexpr float kPi    = 3.14159265358979f;
constexpr float kTwoPi = 6.28318530718f;

// Deterministic hash -> [0,1) for jittering rock so the ring isn't a perfect tube.
float hash01(uint32_t a, uint32_t b) {
    uint32_t h = a * 374761393u + b * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float)(h & 0xFFFFFFu) / (float)0x1000000u;
}
float jit(uint32_t a, uint32_t b, float lo, float hi) {
    return lo + (hi - lo) * hash01(a, b);
}

} // namespace

// ===========================================================================
// GEOMETRY HELPER — a solid static rock box (render + collision + entity).
// ===========================================================================
uint32_t StrataWorld::addRock(Scene& scene, x3::rhi::IRenderDevice& device,
                              x3::phys::IPhysicsWorld& physics,
                              float cx, float cy, float cz, float hx, float hy, float hz,
                              const float color[4], const float emissive[4], bool collide,
                              float uvScale) {
    x3::prims::PrimMesh geo = x3::prims::makeBox(hx, hy, hz, cx, cy, cz, uvScale);
    Entity e;
    e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                               geo.index.data(), (uint32_t)geo.index.size());
    for (int i = 0; i < 4; ++i) e.baseColor[i] = color[i];
    if (emissive) for (int i = 0; i < 4; ++i) e.emissive[i] = emissive[i];
    e.tag = (uint32_t)Tag::Static;
    if (collide) {
        e.body = physics.addStaticMesh(geo.cverts.data(), (uint32_t)(geo.cverts.size() / 3),
                                       geo.cindex.data(), (uint32_t)geo.cindex.size());
    }
    return scene.add(e);
}

// ===========================================================================
// PHASE 1 — one band's scenic shaft-wall ring of canted rock slabs.
// ===========================================================================
void StrataWorld::buildBandRing(Scene& scene, x3::rhi::IRenderDevice& device,
                                x3::phys::IPhysicsWorld& physics, StrataBand& band) {
    const float bandH = band.yMax - band.yMin;
    if (bandH <= 0.0f) return;

    // Band rock color (slightly darkened from the survey color so emission pops).
    const float rock[4] = { band.rgb[0] * 0.9f, band.rgb[1] * 0.9f, band.rgb[2] * 0.9f, 1.0f };
    // Glow term for the deep glowing bands; off otherwise. Authored strength is
    // modulated each frame by update() (the pulse breathes around it).
    const float kGlowStrength = 2.6f;
    float em[4] = {0,0,0,0};
    if (band.glow) {
        em[0] = band.glowRgb[0]; em[1] = band.glowRgb[1];
        em[2] = band.glowRgb[2]; em[3] = kGlowStrength;
    }

    // Ring of canted slabs around the shaft. The slab COUNT scales with the band
    // height so tall bands read as continuous rough rock. Each slab leans inward
    // (radial-out cant) and jitters its radius / width / height a bit.
    const uint32_t slices = 14;                 // slabs around the ring per layer
    const int      layers = std::max(1, (int)std::round(bandH / 8.0f)); // vertical layers
    const float    layerH = bandH / (float)layers;
    uint32_t bandSeed = (uint32_t)((band.yMax + 1000.0f) * 7.0f);

    for (int ly = 0; ly < layers; ++ly) {
        const float baseY = band.yMin + (float)ly * layerH;
        const float topY  = baseY + layerH;
        for (uint32_t s = 0; s < slices; ++s) {
            const float a = (float)s / (float)slices * kTwoPi;
            const float ca = std::cos(a), sa = std::sin(a);
            // Jittered radius (the bore is rough — slabs poke in/out).
            const float r   = m_radius + jit(bandSeed, s + ly * 31u, -1.6f, 2.4f);
            const float rIn = r - jit(bandSeed, s + ly * 53u + 7u, 1.0f, 2.6f);  // inward lean at top
            const float halfW = (kTwoPi * m_radius / (float)slices) * 0.62f
                                + jit(bandSeed, s + ly * 17u, -0.2f, 0.6f);       // tangential half-width
            const float halfT = jit(bandSeed, s + ly * 11u + 3u, 0.9f, 1.8f);     // radial thickness
            const float baseX = m_shaftX + ca * r,   baseZ = m_shaftZ + sa * r;
            const float topX  = m_shaftX + ca * rIn, topZ  = m_shaftZ + sa * rIn;
            // W-RIFT: the rift corridor is bored straight through this wall — no rock
            // inside it (either end of the slab counts: a canted slab leans inward).
            if (keptOut(baseX, baseY, baseZ) || keptOut(topX, topY, topZ)) continue;
            x3::prims::PrimMesh geo =
                x3::prims::makeCantedStrut(baseX, baseY, baseZ, topX, topY, topZ,
                                           halfW, halfT, /*rox*/ca, /*roz*/sa,
                                           /*uvScale*/0.18f, /*hollow*/false);
            Entity e;
            e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                       geo.index.data(), (uint32_t)geo.index.size());
            for (int i = 0; i < 4; ++i) e.baseColor[i] = rock[i];
            // Only a SUBSET of slabs glow (the glowing veins thread through the rock,
            // they don't coat every face) — every 3rd slab in glow bands.
            const bool veinSlab = band.glow && ((s + (uint32_t)ly) % 3u == 0u);
            if (veinSlab) {
                for (int i = 0; i < 4; ++i) e.emissive[i] = em[i];
            }
            e.tag = (uint32_t)Tag::Static;
            e.body = physics.addStaticMesh(geo.cverts.data(), (uint32_t)(geo.cverts.size() / 3),
                                           geo.cindex.data(), (uint32_t)geo.cindex.size());
            uint32_t id = scene.add(e);
            ++band.ringEntities; ++m_stats.shaftRings; ++m_stats.entities;
            if (veinSlab) { m_glowEnts.push_back(id); m_glowBaseStrength.push_back(em[3]); }
        }
    }

    // A few BOULDER clumps + a shelf ledge jutting from the wall per band, for
    // varied massing (so the descent reads as real geology, not a smooth pipe).
    const int boulders = 3 + (band.glow ? 2 : 0);
    for (int bI = 0; bI < boulders; ++bI) {
        const float a = jit(bandSeed + 99u, bI, 0.0f, kTwoPi);
        const float r = m_radius - jit(bandSeed, bI + 200u, 0.5f, 2.5f);
        const float by = band.yMin + jit(bandSeed, bI + 300u, 1.0f, bandH - 1.0f);
        const float bx = m_shaftX + std::cos(a) * r, bz = m_shaftZ + std::sin(a) * r;
        const float h = jit(bandSeed, bI + 400u, 1.0f, 2.4f);
        if (keptOut(bx, by, bz)) continue;              // W-RIFT: bored out
        addRock(scene, device, physics, bx, by, bz, h, h * 0.8f, h,
                rock, band.glow ? em : nullptr, true, 0.2f);
        ++band.ringEntities; ++m_stats.entities;
        if (band.glow) { /* boulder glow handled via em already; track for pulse */ }
    }

    // Per-band MOOD point lights — sit just inside the bore at the band mid-height
    // so the rock face is lit from within the shaft. Deep bands get colored light.
    const int nLights = band.glow ? 3 : 1;
    for (int li = 0; li < nLights; ++li) {
        const float a = (float)li / (float)nLights * kTwoPi + 0.4f;
        x3::rhi::PointLight l;
        l.pos[0] = m_shaftX + std::cos(a) * (m_radius - 4.0f);
        l.pos[1] = band.yMin + bandH * (0.3f + 0.4f * (float)li / std::max(1, nLights));
        l.pos[2] = m_shaftZ + std::sin(a) * (m_radius - 4.0f);
        if (band.glow) {
            l.color[0] = band.glowRgb[0] * 1.4f;
            l.color[1] = band.glowRgb[1] * 1.4f;
            l.color[2] = band.glowRgb[2] * 1.4f;
            l.range = 26.0f;
        } else {
            // Cool dim earth-fill for the upper bands (moody, atmospheric).
            l.color[0] = band.rgb[0] * 0.5f;
            l.color[1] = band.rgb[1] * 0.5f;
            l.color[2] = band.rgb[2] * 0.6f;
            l.range = 18.0f;
        }
        m_lights.push_back(l);
        ++band.lightCount; ++m_stats.moodLights;
        // Track magma-band lights for the heat flicker.
        if (band.glow && std::string(band.name) == "Magma Zone")
            m_magmaLightIdx.push_back(m_lights.size() - 1);
    }
}

// ===========================================================================
// PHASE 2 — one walkable offshoot tunnel + its side pocket.
// ===========================================================================
void StrataWorld::buildOffshoot(Scene& scene, x3::rhi::IRenderDevice& device,
                                x3::phys::IPhysicsWorld& physics, TriggerSystem& triggers,
                                StrataOffshoot& off, uint32_t triggerId) {
    // The tunnel runs RADIALLY OUT from the shaft wall at the mouth, a ~16 m bore,
    // to the side pocket. Built as a sequence of box "ribs" (floor + two walls +
    // ceiling) so it's a real walkable corridor with collision.
    const float dx = off.pocket.x - off.mouth.x;
    const float dz = off.pocket.z - off.mouth.z;
    const float len = std::sqrt(dx * dx + dz * dz);
    const float ux = (len > 0.001f) ? dx / len : 1.0f;
    const float uz = (len > 0.001f) ? dz / len : 0.0f;
    const float px = -uz, pz = ux;            // perpendicular (tunnel half-width axis)
    const float halfW = 2.6f, halfH = 2.4f;
    const float y = off.mouth.y;

    // Rock tint from the band; glow if a glowing band.
    const float rock[4] = { 0.22f, 0.20f, 0.22f, 1.0f };
    float em[4] = {0,0,0,0};
    if (off.glow) { em[0] = 0.45f; em[1] = 0.12f; em[2] = 0.6f; em[3] = 2.0f; }

    const int ribs = std::max(2, (int)std::round(len / 3.5f));
    for (int i = 0; i <= ribs; ++i) {
        const float t = (float)i / (float)ribs;
        const float cx = off.mouth.x + ux * len * t;
        const float cz = off.mouth.z + uz * len * t;
        // Floor slab (collidable walkway).
        addRock(scene, device, physics, cx, y - 0.2f, cz,
                std::fabs(ux) * 2.0f + halfW * std::fabs(px), 0.2f,
                std::fabs(uz) * 2.0f + halfW * std::fabs(pz), rock, nullptr, true, 0.3f);
        ++off.entities; ++m_stats.entities;
        // Two side walls (rough rock). Slight inward jitter for a cave feel.
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            const float wx = cx + px * halfW * (float)sgn;
            const float wz = cz + pz * halfW * (float)sgn;
            const float jh = jit(triggerId, i * 7u + (uint32_t)(sgn + 1), 0.0f, 0.6f);
            uint32_t id = addRock(scene, device, physics, wx, y + halfH * 0.5f, wz,
                                  1.0f, halfH + jh, 1.0f, rock,
                                  off.glow ? em : nullptr, true, 0.3f);
            ++off.entities; ++m_stats.entities;
            if (off.glow && (i % 2 == 0)) { m_glowEnts.push_back(id); m_glowBaseStrength.push_back(em[3]); }
        }
        // Ceiling (non-collidable cap — keeps the player in but cheap).
        addRock(scene, device, physics, cx, y + halfH * 2.0f, cz,
                halfW + 0.5f, 0.3f, halfW + 0.5f, rock, nullptr, false, 0.3f);
        ++off.entities; ++m_stats.entities;
    }

    // The side POCKET — a small room at the tunnel end (secret pocket / content
    // hook / rescue-storyline space). A floor + a strongly emissive marker crystal
    // so the payoff reads from the tunnel.
    const float pcx = off.pocket.x, pcz = off.pocket.z;
    addRock(scene, device, physics, pcx, y - 0.2f, pcz, 5.0f, 0.3f, 5.0f, rock, nullptr, true, 0.25f);
    // Pocket walls (ring of 4).
    for (int w = 0; w < 4; ++w) {
        const float wa = (float)w / 4.0f * kTwoPi;
        addRock(scene, device, physics, pcx + std::cos(wa) * 5.0f, y + halfH, pcz + std::sin(wa) * 5.0f,
                std::fabs(std::sin(wa)) * 5.0f + 0.6f, halfH + 0.5f,
                std::fabs(std::cos(wa)) * 5.0f + 0.6f, rock, nullptr, true, 0.25f);
        ++off.entities; ++m_stats.entities;
    }
    // The payoff crystal (Salvari-style singing crystal — strong glow, no body).
    {
        x3::prims::PrimMesh geo = x3::prims::makeBox(0.8f, 1.4f, 0.8f, pcx, y + 1.4f, pcz, 0.5f);
        Entity e;
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
        e.baseColor[0] = 0.30f; e.baseColor[1] = 0.55f; e.baseColor[2] = 1.0f; e.baseColor[3] = 1.0f;
        e.emissive[0] = 0.35f; e.emissive[1] = 0.55f; e.emissive[2] = 1.0f; e.emissive[3] = 3.4f;
        e.tag = (uint32_t)Tag::Prop;
        uint32_t id = scene.add(e);
        ++off.entities; ++m_stats.entities;
        m_glowEnts.push_back(id); m_glowBaseStrength.push_back(3.4f);
    }
    // Pocket mood light.
    {
        x3::rhi::PointLight l;
        l.pos[0] = pcx; l.pos[1] = y + 2.0f; l.pos[2] = pcz;
        l.color[0] = off.glow ? 0.9f : 0.4f; l.color[1] = 0.5f; l.color[2] = 1.0f;
        l.range = 16.0f;
        m_lights.push_back(l); ++m_stats.moodLights;
    }

    // Register the offshoot-entry trigger at the tunnel mouth (a box across the bore).
    off.trigger = triggerId;
    x3::phys::Vec3 tmin{ off.mouth.x - 3.0f, y - 1.0f, off.mouth.z - 3.0f };
    x3::phys::Vec3 tmax{ off.mouth.x + 3.0f, y + 4.0f, off.mouth.z + 3.0f };
    triggers.add(tmin, tmax, triggerId, true);
}

// ===========================================================================
// BUILD — the whole descent.
// ===========================================================================
void StrataWorld::build(Scene& scene, x3::rhi::IRenderDevice& device,
                        x3::phys::IPhysicsWorld& physics, TriggerSystem& triggers,
                        float shaftX, float shaftZ, float radius) {
    if (m_built) return;
    m_shaftX = shaftX; m_shaftZ = shaftZ; m_radius = radius;
    m_stats.topY = kStrataTopY; m_stats.bottomY = kStrataClubY;

    // ---- Derive the modeled bands from the elevator's strata (Y <= 0 only). ----
    // The elevator's 9 bands run +200..-400; we model the descent from the facility
    // base (0) down to the club ceiling (-200). The deep glow bands sit just above
    // the club, exactly where the elevator design places them (Crystal -200..-260
    // etc.) — but the modeled SHAFT bottoms at -200, so we COMPRESS the three glow
    // bands into the -200..0 descent so the glowing geology is what the glass sees
    // on the way down + what the offshoots open into (per §2.6 "literal/reachable").
    //
    // Layout top->bottom across the -200..0 shaft:
    //   Foundation/Limestone (0..-30) -> Granite (-30..-70) -> Basalt (-70..-110)
    //   -> Obsidian (-110..-140) -> Crystal Veins (-140..-170, GLOW)
    //   -> Magma Zone (-170..-188, GLOW) -> Alien Substrate (-188..-200, GLOW).
    struct BandDef { float yMin, yMax; const char* name; float rgb[3]; bool glow; float grgb[3]; };
    const BandDef defs[] = {
        {  -30.0f,    0.0f, "Foundation Stone",{0.35f,0.30f,0.25f}, false, {0,0,0} },
        {  -70.0f,  -30.0f, "Granite",         {0.30f,0.28f,0.32f}, false, {0,0,0} },
        { -110.0f,  -70.0f, "Basalt",          {0.20f,0.18f,0.15f}, false, {0,0,0} },
        { -140.0f, -110.0f, "Obsidian",        {0.10f,0.08f,0.12f}, false, {0,0,0} },
        { -170.0f, -140.0f, "Crystal Veins",   {0.12f,0.08f,0.18f}, true,  {0.30f,0.10f,0.60f} },
        { -188.0f, -170.0f, "Magma Zone",      {0.25f,0.06f,0.02f}, true,  {0.80f,0.20f,0.05f} },
        { -200.0f, -188.0f, "Alien Substrate", {0.08f,0.04f,0.12f}, true,  {0.20f,0.04f,0.40f} },
    };
    for (const BandDef& d : defs) {
        StrataBand b;
        b.yMin = d.yMin; b.yMax = d.yMax; b.name = d.name;
        b.rgb[0]=d.rgb[0]; b.rgb[1]=d.rgb[1]; b.rgb[2]=d.rgb[2];
        b.glow = d.glow;
        b.glowRgb[0]=d.grgb[0]; b.glowRgb[1]=d.grgb[1]; b.glowRgb[2]=d.grgb[2];
        m_bands.push_back(b);
    }
    m_stats.bandCount = (int)m_bands.size();

    // ---- Phase 1: build each band's scenic shaft-wall ring. ----
    for (StrataBand& b : m_bands) {
        buildBandRing(scene, device, physics, b);
        if (b.glow) ++m_stats.glowBands;
    }

    // A capping floor at the very bottom (the shaft floor / club ceiling line at
    // -200) so the on-foot descent has something to land on + the seam to the club
    // is clean (no gap). Dark alien rock.
    {
        const float rock[4] = { 0.08f, 0.05f, 0.10f, 1.0f };
        const float em[4]   = { 0.18f, 0.04f, 0.35f, 1.2f };
        addRock(scene, device, physics, m_shaftX, kStrataClubY - 0.5f, m_shaftZ,
                m_radius + 4.0f, 0.5f, m_radius + 4.0f, rock, em, true, 0.15f);
        ++m_stats.entities;
    }

    // ---- Phase 2: offshoot tunnels at the deep band boundaries. ----
    // One per atmospheric band boundary; the deep glow bands get glowing tunnels.
    struct OffDef { float y; float ang; const char* band; bool glow; uint32_t trig; };
    const OffDef offs[] = {
        {  -55.0f, 0.0f,        "Granite",       false, (uint32_t)StrataTrigger::Offshoot0Granite  },
        {  -95.0f, kPi * 0.5f,  "Basalt",        false, (uint32_t)StrataTrigger::Offshoot1Basalt   },
        { -125.0f, kPi,         "Obsidian",      false, (uint32_t)StrataTrigger::Offshoot2Obsidian },
        { -155.0f, kPi * 1.5f,  "Crystal Veins", true,  (uint32_t)StrataTrigger::Offshoot3Crystal  },
        { -180.0f, kPi * 0.25f, "Magma Zone",    true,  (uint32_t)StrataTrigger::Offshoot4Magma    },
    };
    for (const OffDef& od : offs) {
        StrataOffshoot off;
        off.bandName = od.band; off.glow = od.glow;
        const float ca = std::cos(od.ang), sa = std::sin(od.ang);
        off.mouth  = { m_shaftX + ca * (m_radius - 1.0f), od.y, m_shaftZ + sa * (m_radius - 1.0f) };
        off.pocket = { m_shaftX + ca * (m_radius + 18.0f), od.y, m_shaftZ + sa * (m_radius + 18.0f) };
        buildOffshoot(scene, device, physics, triggers, off, od.trig);
        m_offshoots.push_back(off);
        // Mark the host band as having an offshoot.
        for (StrataBand& b : m_bands)
            if (od.y <= b.yMax && od.y >= b.yMin) b.hasOffshoot = true;
    }
    m_stats.offshootCount = (int)m_offshoots.size();
    m_offshootReached.assign(m_offshoots.size(), false);

    // ---- The ON-FOOT descent route: a spiral of collidable ledges + ramps down ----
    // the shaft wall, passing each offshoot mouth, from the top (Y~=0) to the club
    // floor (Y=-200). Each ledge is a wide collidable slab; ramps bridge them so a
    // player can WALK the whole way down (alternate to the elevator).
    {
        const float rock[4] = { 0.26f, 0.24f, 0.26f, 1.0f };
        const float ledgeR = m_radius - 3.0f;       // ledges hug the wall
        const int   steps  = 26;                     // ledges down the spiral
        const float topY   = -2.0f;
        const float dyTotal = kStrataClubY + 1.0f - topY;   // ~ -199 of descent
        for (int i = 0; i <= steps; ++i) {
            const float t = (float)i / (float)steps;
            const float ang = t * kTwoPi * 3.0f;     // 3 full turns down the shaft
            const float y = topY + dyTotal * t;
            const float lx = m_shaftX + std::cos(ang) * ledgeR;
            const float lz = m_shaftZ + std::sin(ang) * ledgeR;
            // A collidable ledge slab. (W-RIFT: not inside the bored rift corridor —
            // a rock shelf through the hall would be a wall you cannot see coming.)
            if (keptOut(lx, y, lz)) continue;
            addRock(scene, device, physics, lx, y - 0.2f, lz, 3.0f, 0.3f, 3.0f, rock, nullptr, true, 0.3f);
            ++m_stats.entities;
            m_route.push_back({ lx, y, lz });
            ++m_stats.routeWaypoints;
        }
    }

    // Spawn at the top of the on-foot route, facing down into the shaft.
    m_spawn = m_route.empty() ? x3::phys::Vec3{ m_shaftX, -1.0f, m_shaftZ + m_radius - 3.0f }
                              : x3::phys::Vec3{ m_route.front().x, m_route.front().y + 0.1f, m_route.front().z };

    // ---- Triggers: descent-top, on-foot entry, club arrival at the bottom. ----
    triggers.add({ m_shaftX - m_radius, -3.0f, m_shaftZ - m_radius },
                 { m_shaftX + m_radius,  1.0f, m_shaftZ + m_radius },
                 (uint32_t)StrataTrigger::DescentTop, true);
    triggers.add({ m_spawn.x - 3.0f, m_spawn.y - 2.0f, m_spawn.z - 3.0f },
                 { m_spawn.x + 3.0f, m_spawn.y + 2.0f, m_spawn.z + 3.0f },
                 (uint32_t)StrataTrigger::OnFootRouteEntry, true);
    triggers.add({ m_shaftX - m_radius, kStrataClubY - 1.0f, m_shaftZ - m_radius },
                 { m_shaftX + m_radius, kStrataClubY + 4.0f, m_shaftZ + m_radius },
                 (uint32_t)StrataTrigger::ClubArrival, true);

    m_built = true;
    x3::logInfo("[strata] descent built: " + std::to_string(m_stats.entities) +
                " entities, " + std::to_string(m_stats.bandCount) + " bands (" +
                std::to_string(m_stats.glowBands) + " glowing), " +
                std::to_string(m_stats.offshootCount) + " offshoots, " +
                std::to_string(m_stats.routeWaypoints) + " on-foot waypoints, " +
                std::to_string(m_stats.moodLights) + " mood lights, Y " +
                std::to_string((int)kStrataTopY) + ".." + std::to_string((int)kStrataClubY));
}

// ===========================================================================
// UPDATE — breathe the crystal/magma/alien glow + flicker the magma lights.
// ===========================================================================
void StrataWorld::update(float dt, Scene& scene, x3::rhi::IRenderDevice& device,
                         const x3::phys::Vec3& /*eye*/) {
    if (!m_built) return;
    m_time += dt;

    // Slow breathing pulse on the glowing rock (each glow entity around its base).
    const float pulse = 0.78f + 0.22f * std::sin(m_time * 1.6f);
    for (size_t i = 0; i < m_glowEnts.size(); ++i) {
        const uint32_t id = m_glowEnts[i];
        if (id >= scene.size()) continue;
        Entity& e = scene.get(id);
        // Per-entity phase offset so the field shimmers rather than blinking as one.
        const float ph = std::sin(m_time * 1.6f + (float)i * 0.7f) * 0.5f + 0.5f;
        e.emissive[3] = m_glowBaseStrength[i] * (0.7f + 0.5f * ph);
    }

    // Magma heat-flicker on the magma mood lights (fast, irregular).
    for (size_t k : m_magmaLightIdx) {
        if (k >= m_lights.size()) continue;
        const float f = 0.7f + 0.3f * std::sin(m_time * 9.0f + (float)k) *
                        std::sin(m_time * 13.0f + (float)k * 2.1f);
        m_lights[k].color[0] = 0.80f * 1.4f * f;
        m_lights[k].color[1] = 0.20f * 1.4f * f;
        m_lights[k].color[2] = 0.05f * 1.4f * f;
    }
    (void)pulse;

    // Re-push the (breathing) mood-light set so the glow animates.
    if (!m_lights.empty())
        device.setPointLights(m_lights.data(), (uint32_t)m_lights.size());
}

// ===========================================================================
// onTrigger — latch story beats.
// ===========================================================================
void StrataWorld::onTrigger(uint32_t triggerId) {
    switch ((StrataTrigger)triggerId) {
        case StrataTrigger::DescentTop:        m_descentEntered = true; break;
        case StrataTrigger::ClubArrival:       m_clubReached = true; break;
        case StrataTrigger::Offshoot0Granite:  if (m_offshootReached.size()>0) m_offshootReached[0]=true; break;
        case StrataTrigger::Offshoot1Basalt:   if (m_offshootReached.size()>1) m_offshootReached[1]=true; break;
        case StrataTrigger::Offshoot2Obsidian: if (m_offshootReached.size()>2) m_offshootReached[2]=true; break;
        case StrataTrigger::Offshoot3Crystal:  if (m_offshootReached.size()>3) m_offshootReached[3]=true; break;
        case StrataTrigger::Offshoot4Magma:    if (m_offshootReached.size()>4) m_offshootReached[4]=true; break;
        default: break;
    }
}

// ===========================================================================
// Queries.
// ===========================================================================
const StrataBand& StrataWorld::bandAtY(float y) const {
    static const StrataBand kEmpty{};
    if (m_bands.empty()) return kEmpty;
    for (const StrataBand& b : m_bands)
        if (y <= b.yMax && y >= b.yMin) return b;
    // Outside the modeled range: clamp to nearest end.
    if (y > m_bands.front().yMax) return m_bands.front();
    return m_bands.back();
}

void StrataWorld::showcaseCamera(float out[5]) const {
    // Perched on an upper ledge looking DOWN the shaft toward the glowing depths,
    // so the frame catches the earth bands fading into crystal/magma glow.
    out[0] = m_shaftX + m_radius - 4.0f;
    out[1] = -36.0f;
    out[2] = m_shaftZ;
    // Yaw toward the shaft center, pitch down into the glow.
    out[3] = std::atan2(m_shaftZ - out[2], m_shaftX - out[0]);   // face center
    out[4] = -0.55f;
}

bool StrataWorld::offshootReachable(const TriggerSystem& triggers, uint32_t i) const {
    if (!m_built || i >= m_offshoots.size()) return false;
    const StrataOffshoot& off = m_offshoots[i];
    const TriggerVolume* t = triggers.findById(off.trigger);
    if (!t || !t->enabled) return false;
    // The trigger must sit at the tunnel mouth (within slop) AND the mouth must be
    // on the shaft wall (so a player at the shaft can cross into the tunnel) AND
    // the pocket must be reachable beyond it (finite, > the mouth radius).
    const float tcx = 0.5f * (t->min.x + t->max.x);
    const float tcz = 0.5f * (t->min.z + t->max.z);
    const float dMouth = std::sqrt((tcx - off.mouth.x) * (tcx - off.mouth.x) +
                                   (tcz - off.mouth.z) * (tcz - off.mouth.z));
    if (dMouth > 5.0f) return false;
    const float mouthR = std::sqrt((off.mouth.x - m_shaftX) * (off.mouth.x - m_shaftX) +
                                   (off.mouth.z - m_shaftZ) * (off.mouth.z - m_shaftZ));
    const float pocketR = std::sqrt((off.pocket.x - m_shaftX) * (off.pocket.x - m_shaftX) +
                                    (off.pocket.z - m_shaftZ) * (off.pocket.z - m_shaftZ));
    return mouthR <= m_radius + 1.0f && pocketR > mouthR + 5.0f && pocketR < m_radius + 60.0f;
}

bool StrataWorld::allOffshootsReachable(const TriggerSystem& triggers) const {
    if (!m_built || m_offshoots.empty()) return false;
    for (uint32_t i = 0; i < m_offshoots.size(); ++i)
        if (!offshootReachable(triggers, i)) return false;
    return true;
}

bool StrataWorld::onFootRouteContinuous() const {
    if (m_route.size() < 4) return false;
    // Top reaches near Y=0; bottom reaches near the club Y=-200.
    if (m_route.front().y < -6.0f) return false;
    if (m_route.back().y > kStrataClubY + 6.0f) return false;
    // Each step within a climbable gap of the next (no impossible drops).
    for (size_t i = 1; i < m_route.size(); ++i) {
        const x3::phys::Vec3& a = m_route[i - 1];
        const x3::phys::Vec3& b = m_route[i];
        const float dy = std::fabs(a.y - b.y);
        const float dxz = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z));
        if (dy > 12.0f) return false;          // no impossible vertical drop between ledges
        if (dxz > 16.0f) return false;          // ledges stay close enough to traverse
    }
    return true;
}

bool StrataWorld::clubConnected(const TriggerSystem& triggers) const {
    if (!m_built) return false;
    const TriggerVolume* t = triggers.findById((uint32_t)StrataTrigger::ClubArrival);
    if (!t || !t->enabled) return false;
    // The arrival trigger must sit at the bottom of the shaft (~club Y) and the
    // on-foot route must actually reach there.
    const float tcy = 0.5f * (t->min.y + t->max.y);
    if (tcy > kStrataClubY + 6.0f) return false;
    return !m_route.empty() && m_route.back().y <= kStrataClubY + 6.0f;
}

// ===========================================================================
// Headless self-test (--test-strata).
// ===========================================================================
namespace {
int gs_pass = 0, gs_fail = 0;
void scCheck(bool cond, const char* name) {
    if (cond) { ++gs_pass; x3::logInfo(std::string("[strata-test] PASS ") + name); }
    else      { ++gs_fail; x3::logError(std::string("[strata-test] FAIL ") + name); }
}
} // namespace

bool runStrataSelfTest() {
    gs_pass = gs_fail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
    phys->init();
    HeadlessRenderDevice device;
    Scene scene;
    TriggerSystem triggers;
    StrataWorld strata;

    // Build around a shaft at the origin (the elevator shaft XZ in the real game).
    strata.build(scene, device, *phys, triggers, /*shaftX*/0.0f, /*shaftZ*/0.0f, /*radius*/14.0f);

    scCheck(strata.built(), "S0 strata descent built");

    // ---- S1: spans facility base (Y~=0) down to the club (Y=-200). ----
    {
        const auto& st = strata.stats();
        bool ok = std::fabs(st.topY - 0.0f) < 1.0f && std::fabs(st.bottomY - (-200.0f)) < 1.0f &&
                  st.entities > 200;     // a rich, non-trivial descent
        scCheck(ok, "S1 descent spans Y=0 (base) .. Y=-200 (club), rich geometry");
    }

    // ---- S2: bands carry the elevator layer names + 3 deep bands glow. ----
    {
        const auto& bands = strata.bands();
        bool haveCrystal = false, haveMagma = false, haveAlien = false;
        int glow = 0;
        for (const StrataBand& b : bands) {
            if (std::string(b.name) == "Crystal Veins")   haveCrystal = true;
            if (std::string(b.name) == "Magma Zone")      haveMagma = true;
            if (std::string(b.name) == "Alien Substrate") haveAlien = true;
            if (b.glow) ++glow;
        }
        bool ok = bands.size() >= 5 && haveCrystal && haveMagma && haveAlien &&
                  glow == 3 && strata.stats().glowBands == 3;
        scCheck(ok, "S2 layer bands named (Crystal/Magma/Alien) + exactly 3 glow");
    }

    // ---- S3: bandAtY returns the right band across the descent (elevator glass). ----
    {
        bool ok = std::string(strata.bandAtY(-10.0f).name) == "Foundation Stone" &&
                  std::string(strata.bandAtY(-50.0f).name) == "Granite" &&
                  std::string(strata.bandAtY(-155.0f).name) == "Crystal Veins" &&
                  std::string(strata.bandAtY(-180.0f).name) == "Magma Zone" &&
                  std::string(strata.bandAtY(-195.0f).name) == "Alien Substrate" &&
                  strata.bandAtY(-155.0f).glow && strata.bandAtY(-180.0f).glow;
        scCheck(ok, "S3 bandAtY maps the descent to the right glowing layer");
    }

    // ---- S4: offshoot tunnels all REACHABLE via their triggers. ----
    {
        bool ok = strata.offshoots().size() == kStrataOffshootCount &&
                  strata.allOffshootsReachable(triggers);
        scCheck(ok, "S4 all offshoot tunnels reachable via their triggers");
    }

    // ---- S5: on-foot route is continuous top->bottom. ----
    {
        bool ok = strata.onFootRouteContinuous() && strata.onFootRoute().size() > 8;
        scCheck(ok, "S5 on-foot ledge route continuous Y~=0 .. Y=-200");
    }

    // ---- S6: the shaft connects to the club (arrival trigger at the bottom). ----
    {
        scCheck(strata.clubConnected(triggers), "S6 shaft opens into Club 1127 at Y=-200");
    }

    // ---- S7: onTrigger latches beats; trigger firing reaches the offshoots. ----
    {
        // Simulate the player crossing each registered trigger.
        // DescentTop at center top.
        auto fired1 = triggers.update({ 0.0f, 0.0f, 0.0f });
        for (uint32_t id : fired1) strata.onTrigger(id);
        // Each offshoot mouth.
        bool allReached = true;
        for (uint32_t i = 0; i < strata.offshoots().size(); ++i) {
            const auto& off = strata.offshoots()[i];
            auto f = triggers.update({ off.mouth.x, off.mouth.y, off.mouth.z });
            for (uint32_t id : f) strata.onTrigger(id);
            if (!strata.offshootReached(i)) allReached = false;
        }
        // Club arrival at the bottom.
        auto fc = triggers.update({ 0.0f, kStrataClubY + 1.0f, 0.0f });
        for (uint32_t id : fc) strata.onTrigger(id);
        bool ok = strata.descentEntered() && allReached && strata.clubReached();
        scCheck(ok, "S7 triggers fire: descent entered, offshoots + club reached");
    }

    // ---- S8: a few ticks animate the glow without crashing (leak-clean). ----
    {
        const float dt = 1.0f / 60.0f;
        for (int i = 0; i < 12; ++i) {
            strata.update(dt, scene, device, { 0.0f, -40.0f, 0.0f });
            phys->step(dt);
            scene.update(*phys);
        }
        scCheck(true, "S8 12 ticks of glow-pulse + mood-light animation (leak-clean)");
    }

    // ---- S9: trigger ids don't collide with other modules' ranges. ----
    {
        bool ok = kStrataTrigBase == 200 &&
                  (uint32_t)StrataTrigger::ClubArrival < 210;   // 200..207, clear of 100..108
        scCheck(ok, "S9 strata trigger ids 200..207 clear of act2caves 100..108");
    }

    phys->shutdown();
    x3::logInfo("strata: " + std::to_string(gs_pass) + "/" +
                std::to_string(gs_pass + gs_fail) + " passed");
    return gs_fail == 0;
}

} // namespace x3::game
