// EFLZ Act-2 open world — city / industrial metropolis + roads + freeway tunnels.
// See city.h. Clean-room: X3Native's own Scene / terrain / mesh_prims + the engine
// interfaces + the EFLZ design (Tim's own Q3Engine city/freeway modules as content
// reference). No RBDOOM / id Tech / Doom / Quake source.
//
// W8-3 (feat/babylon-world): BLOCKOUT-PLUS pass, layout ported from the Babylon
// world map (x3-world-city.js Scrapyard / x3-world-structures.js New District /
// x3-city-roads.js street grid — content reference only):
//   * Scrapyard City (-600,500): the Babylon named-building roster (Robot Shop /
//     Arena Bar / Hack Den / Fuel Depot / Armor Works / Garage / Water Tower /
//     4 towers / the 30 m Helipad Tower) + plaza & fountain + street lights,
//     3 E-W x 3 N-S street grid.
//   * New District (200,500): 6 skyscrapers + 8 apartments + 4 warehouses +
//     10 neon shops on the Babylon grid (mains Z=560/500/440 + Industrial Blvd
//     Z=410; side streets X=120..310) with signalized intersections.
//   * Industrial Zone (-200,350): factories + smokestacks + tank farm + lattice.
//   * Connector roads: Scrapyard<->District freeway, the District->Spire
//     approach, the coast spur — terrain-conformed in segments.
// Massing = textured boxes (surface-library PBR sets) with VARIED heights and
// footprints; window bands = dark glass strips (some warm-lit). The districts
// sit on FLAT PADS carved by the canonical terrain field (terrain.cpp kPads).
// This is massing + materials + streets + hero blocks — density/props/interiors
// are future waves. Visual-only (no collision bodies yet); mirrors world_regions.cpp.
#include "city.h"
#include "headless_device.h"
#include "mesh_prims.h"
#include "street_lights.h"
#include "engine/rhi/ClusterLights.h"   // kMaxSceneLights (--test-city D3)
#include "surface_library.h"
#include "tunnel_corridor.h"   // TunnelSpec / registerTunnelCorridorFor (freeway bores)
#include "asset_root.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace x3::game {

namespace {

// One district (center, footprint, tint). Centers = blueprint gazetteer §1 /
// the Babylon map; footprints match the terrain flat pads (terrain.cpp kPads).
struct DistrictDef { const char* name; float cx, cz, radius; float r, g, b; };
const DistrictDef kDistricts[kCityZoneCount] = {
    { "Scrapyard City", -600.0f, 500.0f, 250.0f, 0.62f, 0.52f, 0.42f },   // salvage / ramshackle
    { "New District",    200.0f, 500.0f, 190.0f, 0.80f, 0.82f, 0.86f },   // urban concrete
    { "Industrial Zone",-200.0f, 350.0f, 150.0f, 0.66f, 0.58f, 0.46f },   // factories / refinery
};

// The 4 freeway tunnels (mouth XZ, axis-aligned heading toward a range, bore length).
struct TunnelDef { const char* name; float mx, mz, dx, dz, len; };
const TunnelDef kTunnels[kFreewayTunnelCount] = {
    { "North Freeway Tunnel",   0.0f,  700.0f,  0.0f,  1.0f, 200.0f },  // -> Northern Range
    { "East Freeway Tunnel",  430.0f,  500.0f,  1.0f,  0.0f, 200.0f },  // -> Eastern Range (on the coast spur)
    { "South Freeway Tunnel", -200.0f, 240.0f,  0.0f, -1.0f, 200.0f },  // -> Southern Range
    { "West Freeway Tunnel",  -830.0f, 500.0f, -1.0f,  0.0f, 200.0f },  // -> Western Highlands
};

// Neon sign palette (the Babylon shop-neon colors).
const float kNeon[5][3] = {
    { 1.00f, 0.30f, 0.50f },   // pink
    { 0.20f, 1.00f, 0.30f },   // green
    { 0.20f, 0.40f, 1.20f },   // blue
    { 1.00f, 0.55f, 0.10f },   // orange
    { 0.70f, 0.25f, 1.00f },   // purple
};

} // namespace

// ---- BOOT: register one terrain corridor per freeway tunnel ---------------
// Each plan gives a MOUTH, a heading and a bore length. The corridor's spine has
// to be longer than the bore: a tunnel needs an approach CUTTING on each side to
// bring the road down to portal level at a drivable grade, or the portal sits in
// a cliff face. kApproach is that run, per side.
uint32_t registerCityFreewayTunnels() {
    static uint32_t bored = 0;
    static bool done = false;
    if (done) return bored;
    done = true;

    constexpr float kApproach = 170.0f;   // graded run outside each portal (m)
    for (uint32_t i = 0; i < kFreewayTunnelCount; ++i) {
        const TunnelDef& t = kTunnels[i];
        TunnelSpec spec;
        spec.name = t.name;
        // Centre the spine on the MIDDLE of the intended bore, not the mouth,
        // so the approach cuttings fall symmetrically outside it.
        spec.cx   = t.mx + t.dx * (t.len * 0.5f);
        spec.cz   = t.mz + t.dz * (t.len * 0.5f);
        spec.dirX = t.dx; spec.dirZ = t.dz;
        spec.halfLen = t.len * 0.5f + kApproach;
        const TunnelRoute* r = registerTunnelCorridorFor(spec);
        if (!r) continue;
        if (r->boreValid) {
            ++bored;
            x3::logInfo(std::string("[city] ") + t.name + ": BORED — shell " +
                        std::to_string((int)(r->boreS1 - r->boreS0)) + " m");
        } else {
            x3::logWarn(std::string("[city] ") + t.name +
                        ": no hill on this heading — registered as an open cutting, "
                        "no tunnel dressed");
        }
    }
    x3::logInfo("[city] freeway tunnels: " + std::to_string(bored) + "/" +
                std::to_string(kFreewayTunnelCount) + " produced a genuine bore");
    return bored;
}


void City::build(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
                 SurfaceLibrary* sharedSurf, std::vector<StreetLights::Glow>* outGlows) {
    (void)physics;   // blockout-plus city is visual-only this pass (no collision body)

    // CONTENT WIRING: emit a glow-only pooled light for every warm-lit window
    // band and every neon sign, so the night city's own surfaces light the
    // street. No-op when the caller does not ask (outGlows == nullptr), which
    // is what keeps the legacy render byte-identical.
    auto emitGlow = [&](float x, float y, float z, const float col[3],
                        float range, float intensity, bool sign) {
        if (!outGlows) return;
        StreetLights::Glow g;
        g.pos[0] = x; g.pos[1] = y; g.pos[2] = z;
        g.color[0] = col[0]; g.color[1] = col[1]; g.color[2] = col[2];
        g.range = range; g.intensity = intensity; g.sign = sign;
        outGlows->push_back(g);
    };

    // Real PBR surface sets (ART_BIBLE §4). On a headless device the loads no-op
    // and everything renders as tinted graybox — tests are unaffected. A shared
    // (streamer-lifetime) library skips the per-realize PNG decode.
    SurfaceLibrary localSurf;
    SurfaceLibrary& surf = sharedSurf ? *sharedSurf : localSurf;
    if (!surf.mounted()) surf.mount(assetRoot() + "/surface_library");
    auto set = [&](const char* name) -> const SurfaceSet& { return surf.get(device, name); };

    auto addBoxProp = [&](float cx, float cy, float cz, float hx, float hy, float hz,
                          const float col[4], const float emiss[4],
                          const SurfaceSet* s = nullptr, float uvScale = 0.5f) {
        x3::prims::PrimMesh m = x3::prims::makeBox(hx, hy, hz, cx, cy, cz, uvScale);
        Entity e;
        e.mesh = device.createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                   m.index.data(), (uint32_t)m.index.size());
        if (s && s->ok) { e.tex = s->albedo; e.normalTex = s->normal; e.mrTex = s->mr; }
        e.baseColor[0]=col[0]; e.baseColor[1]=col[1]; e.baseColor[2]=col[2]; e.baseColor[3]=1.0f;
        if (emiss) { e.emissive[0]=emiss[0]; e.emissive[1]=emiss[1]; e.emissive[2]=emiss[2]; e.emissive[3]=emiss[3]; }
        e.tag = (uint32_t)Tag::Prop;
        m_props.push_back(scene.add(e));
    };

    const float kDarkGlass[4] = { 0.05f, 0.06f, 0.09f, 1.0f };  // window-band glass
    const float kLitBand[4]   = { 0.85f, 0.70f, 0.45f, 0.9f };  // warm interior light

    // ---- BUILDING: a textured massing box seated on the terrain pad, with
    // dark-glass WINDOW BANDS wrapped at every storey line (h >= 6). Buildings
    // taller than 20 m get every 3rd band warm-lit + the tallest get a rooftop
    // block; `accent` (optional) adds a neon signage strip on the +Z face. ----
    auto building = [&](float cx, float cz, float w, float d, float h,
                        const SurfaceSet* s, const float col[4],
                        const float* accentNeon = nullptr) {
        float g[3]; placeOnTerrain(cx, cz, g);
        addBoxProp(cx, g[1] + h * 0.5f, cz, w * 0.5f, h * 0.5f, d * 0.5f, col, nullptr, s, 0.35f);
        ++m_buildings;
        if (h >= 6.0f) {
            int floorIdx = 0;
            for (float y = 3.0f; y < h - 1.4f; y += 3.2f, ++floorIdx) {
                const bool lit = (h > 20.0f) && (floorIdx % 3 == 1);
                addBoxProp(cx, g[1] + y, cz, w * 0.5f + 0.06f, 0.55f, d * 0.5f + 0.06f,
                           kDarkGlass, lit ? kLitBand : nullptr);
                ++m_windowBands;
                // A lit band is a whole floor of windows; give it a warm spill
                // light just outside each long face so the wash lands on the
                // facade and the street, not inside the massing box.
                if (lit) {
                    const float warm[3] = { 1.00f, 0.80f, 0.52f };
                    emitGlow(cx, g[1] + y, cz + d * 0.5f + 1.2f, warm, 11.0f, 2.4f, false);
                    emitGlow(cx, g[1] + y, cz - d * 0.5f - 1.2f, warm, 11.0f, 2.4f, false);
                }
            }
        }
        if (h >= 24.0f) {   // rooftop AC block + antenna + red beacon
            addBoxProp(cx + w * 0.15f, g[1] + h + 0.8f, cz, 1.4f, 0.8f, 1.4f, col, nullptr, s, 1.0f);
            addBoxProp(cx - w * 0.2f, g[1] + h + 2.2f, cz - d * 0.2f, 0.12f, 2.2f, 0.12f, kDarkGlass, nullptr);
            const float beacon[4] = { 1.0f, 0.12f, 0.08f, 2.0f };
            const float bc[4]     = { 0.20f, 0.05f, 0.05f, 1.0f };
            addBoxProp(cx - w * 0.2f, g[1] + h + 4.6f, cz - d * 0.2f, 0.22f, 0.22f, 0.22f, bc, beacon);
        }
        if (accentNeon) {   // neon signage strip over the entrance (+Z face)
            const float em[4] = { accentNeon[0], accentNeon[1], accentNeon[2], 2.2f };
            const float nc[4] = { accentNeon[0] * 0.2f, accentNeon[1] * 0.2f, accentNeon[2] * 0.2f, 1.0f };
            addBoxProp(cx, g[1] + std::min(h - 0.6f, 3.4f), cz + d * 0.5f + 0.10f,
                       w * 0.32f, 0.28f, 0.08f, nc, em);
            ++m_neonSigns;
            // The signage wash: the strip's own colour thrown onto the awning,
            // the walk and the shopfront a couple of metres in front of it.
            emitGlow(cx, g[1] + std::min(h - 0.6f, 3.4f) - 0.4f, cz + d * 0.5f + 1.6f,
                     accentNeon, 9.0f, 3.0f, true);
        }
    };

    // ---- ROADS: dark asphalt strips seated on the surface. Long connectors are
    // SEGMENTED so they conform to terrain between the flat pads. ----
    const float roadCol[4] = { 0.10f, 0.10f, 0.11f, 1.0f };
    auto addRoad = [&](float x0, float z0, float x1, float z1, float halfW) {
        const float cx = (x0 + x1) * 0.5f, cz = (z0 + z1) * 0.5f;
        const float hx = std::fabs(x1 - x0) * 0.5f + ((x1 == x0) ? halfW : 0.0f);
        const float hz = std::fabs(z1 - z0) * 0.5f + ((z1 == z0) ? halfW : 0.0f);
        float s[3]; placeOnTerrain(cx, cz, s);
        addBoxProp(cx, s[1] + 0.08f, cz, hx, 0.09f, hz, roadCol, nullptr);
        ++m_roadSegments;
    };
    auto addRoadSegmented = [&](float x0, float z0, float x1, float z1, float halfW) {
        const float dx = x1 - x0, dz = z1 - z0;
        const float len = std::sqrt(dx * dx + dz * dz);
        const int   n   = std::max(1, (int)(len / 36.0f));
        for (int i = 0; i < n; ++i) {
            const float t0 = (float)i / (float)n, t1 = (float)(i + 1) / (float)n;
            addRoad(x0 + dx * t0, z0 + dz * t0, x0 + dx * t1, z0 + dz * t1, halfW);
        }
    };

    // ---- Street furniture ----
    // STREET LIGHTS moved to app/street_lights.* (real lamps: pooled
    // PointLights + additive light cones + ground pools + variance). The canon
    // host builds them inside this region's realize via the region-build hook,
    // so the old emissive-prop posts here are retired (no double posts).
    auto trafficLight = [&](float cx, float cz) {
        float g[3]; placeOnTerrain(cx, cz, g);
        const float pole[4]  = { 0.18f, 0.19f, 0.21f, 1.0f };
        const float green[4] = { 0.15f, 1.0f, 0.25f, 2.0f };
        const float box[4]   = { 0.10f, 0.10f, 0.10f, 1.0f };
        addBoxProp(cx, g[1] + 2.2f, cz, 0.09f, 2.2f, 0.09f, pole, nullptr);
        addBoxProp(cx, g[1] + 4.6f, cz, 0.22f, 0.55f, 0.22f, box, green);
        ++m_trafficLights;
    };

    const SurfaceSet* sConcrete   = &set("sr_concrete_01");
    const SurfaceSet* sConcreteA  = &set("sr_concrete_a");
    const SurfaceSet* sCement     = &set("cc_cement_white");
    const SurfaceSet* sPanelsA    = &set("mw_concrete_panels_a");
    const SurfaceSet* sPanelsB    = &set("mw_concrete_panels_b");
    const SurfaceSet* sMetalPan   = &set("mw_metal_panels_a");
    const SurfaceSet* sMetalB     = &set("sr_metal_b");
    const SurfaceSet* sMetalTrimA = &set("mw_metal_trim_a");
    const SurfaceSet* sMetalTrimB = &set("mw_metal_trim_b");
    const SurfaceSet* sGrate      = &set("mw_metal_grate");
    const SurfaceSet* sLattice    = &set("sr_metal_lattice");
    const SurfaceSet* sPlaster    = &set("mw_plaster_painted");

    // Tints ride on MID-GREY albedos through the 1/pi PBR diffuse — keep them
    // HIGH or the massing reads black under a sun-only outdoor rig.
    const float rust[4]   = { 0.92f, 0.68f, 0.50f, 1.0f };
    const float steel[4]  = { 0.85f, 0.88f, 0.92f, 1.0f };
    const float darkMt[4] = { 0.62f, 0.60f, 0.58f, 1.0f };
    const float conc[4]   = { 0.95f, 0.95f, 0.97f, 1.0f };
    const float white[4]  = { 1.0f, 1.0f, 1.0f, 1.0f };
    const float warmPl[4] = { 0.98f, 0.86f, 0.68f, 1.0f };

    // =====================================================================
    // SCRAPYARD CITY (-600, 500) — the Babylon named-building roster.
    // =====================================================================
    {
        const DistrictDef& d = kDistricts[0];
        CityZonePlan& p = m_zones[0];
        p.zone = CityZone::ScrapyardCity; p.name = d.name; p.cx = d.cx; p.cz = d.cz; p.radius = d.radius;
        const uint32_t before = m_buildings;

        // Street grid: 3 E-W (mains) + 3 N-S.
        addRoad(-775.0f, 500.0f, -425.0f, 500.0f, 8.0f);   // main street
        addRoad(-750.0f, 450.0f, -450.0f, 450.0f, 7.0f);
        addRoad(-725.0f, 400.0f, -475.0f, 400.0f, 6.0f);
        addRoad(-680.0f, 440.0f, -680.0f, 560.0f, 6.0f);
        addRoad(-580.0f, 440.0f, -580.0f, 560.0f, 6.0f);
        addRoad(-500.0f, 440.0f, -500.0f, 560.0f, 6.0f);
        // Sidewalks along the main street.
        { const float walk[4] = { 0.55f, 0.54f, 0.50f, 1.0f };
          float g[3]; placeOnTerrain(-600.0f, 500.0f, g);
          addBoxProp(-600.0f, g[1] + 0.14f, 490.0f, 170.0f, 0.06f, 1.6f, walk, nullptr, sConcreteA, 0.25f);
          addBoxProp(-600.0f, g[1] + 0.14f, 510.0f, 170.0f, 0.06f, 1.6f, walk, nullptr, sConcreteA, 0.25f); }

        // Named heroes (Babylon x3-world-city.js roster; positions relative to CX/CZ).
        building(-650.0f, 525.0f, 15.0f, 10.0f,  8.0f, sConcreteA, rust);                // Robot Shop
        building(-580.0f, 525.0f, 10.0f,  8.0f,  6.0f, sPlaster,  warmPl, kNeon[0]);     // Arena Bar (pink neon)
        building(-580.0f, 475.0f,  8.0f,  7.0f,  5.0f, sConcreteA, darkMt, kNeon[1]);    // Hack Den (green neon)
        building(-500.0f, 525.0f, 10.0f,  8.0f,  5.0f, sPanelsB, steel);                 // Fuel Depot
        { float g[3]; placeOnTerrain(-492.0f, 532.0f, g);                                // fuel tanks
          addBoxProp(-492.0f, g[1] + 3.0f, 532.0f, 2.0f, 3.0f, 2.0f, steel, nullptr, sMetalTrimB, 0.8f);
          addBoxProp(-487.0f, g[1] + 2.4f, 528.0f, 1.7f, 2.4f, 1.7f, steel, nullptr, sMetalTrimB, 0.8f); }
        building(-500.0f, 475.0f, 12.0f,  9.0f,  7.0f, sConcreteA, rust);                // Armor Works
        building(-680.0f, 420.0f,  9.0f,  7.0f,  4.0f, sConcreteA, darkMt);              // Garage
        // Water Tower: 4 legs + elevated tank.
        { float g[3]; placeOnTerrain(-700.0f, 400.0f, g);
          const float leg[4] = { 0.30f, 0.28f, 0.26f, 1.0f };
          for (int lx = -1; lx <= 1; lx += 2) for (int lz = -1; lz <= 1; lz += 2)
              addBoxProp(-700.0f + lx * 2.2f, g[1] + 5.5f, 400.0f + lz * 2.2f, 0.18f, 5.5f, 0.18f, leg, nullptr);
          addBoxProp(-700.0f, g[1] + 13.5f, 400.0f, 3.2f, 2.6f, 3.2f, rust, nullptr, sMetalTrimB, 0.6f);
          ++m_buildings; }
        // Scrapyard lot: junk piles + a low rim wall.
        { float g[3]; placeOnTerrain(-650.0f, 450.0f, g);
          const float junk[4] = { 0.30f, 0.26f, 0.22f, 1.0f };
          addBoxProp(-650.0f, g[1] + 0.6f, 450.0f, 12.0f, 0.6f, 9.0f, junk, nullptr, sGrate, 0.3f);
          addBoxProp(-654.0f, g[1] + 2.0f, 452.0f, 2.6f, 1.6f, 2.2f, junk, nullptr, sMetalB, 0.8f);
          addBoxProp(-646.0f, g[1] + 2.6f, 447.0f, 2.0f, 2.0f, 1.8f, junk, nullptr, sMetalB, 0.8f);
          addBoxProp(-649.0f, g[1] + 1.6f, 455.0f, 1.6f, 1.2f, 1.4f, junk, nullptr, sMetalPan, 0.8f); }

        // The 4 tall towers + the Helipad Tower (tallest, per the Babylon map).
        building(-730.0f, 525.0f, 12.0f, 12.0f, 26.0f, sPanelsA, conc);                  // Office Tower
        building(-450.0f, 475.0f,  8.0f,  8.0f, 24.0f, sPanelsA, steel);                 // Comm Tower
        building(-450.0f, 550.0f, 10.0f, 10.0f, 22.0f, sPanelsB, steel);                 // Water-Processing
        building(-730.0f, 425.0f, 14.0f, 10.0f, 18.0f, sPanelsB, conc);                  // Apartment Block
        { // Helipad Tower: 30 m + rooftop pad + corner pad lights.
          building(-600.0f, 560.0f, 10.0f, 10.0f, 30.0f, sConcrete, conc);
          float g[3]; placeOnTerrain(-600.0f, 560.0f, g);
          const float padc[4] = { 0.20f, 0.21f, 0.23f, 1.0f };
          addBoxProp(-600.0f, g[1] + 30.4f, 560.0f, 5.6f, 0.35f, 5.6f, padc, nullptr, sConcreteA, 0.4f);
          const float padLight[4] = { 0.2f, 1.0f, 0.4f, 1.8f };
          const float plc[4] = { 0.05f, 0.15f, 0.08f, 1.0f };
          for (int lx = -1; lx <= 1; lx += 2) for (int lz = -1; lz <= 1; lz += 2)
              addBoxProp(-600.0f + lx * 5.2f, g[1] + 30.95f, 560.0f + lz * 5.2f, 0.25f, 0.22f, 0.25f, plc, padLight); }

        // Generic salvage shacks filling the blocks (varied ramshackle massing).
        building(-630.0f, 478.0f,  7.0f, 6.0f, 3.6f, sConcreteA, rust);
        building(-615.0f, 430.0f,  6.0f, 5.0f, 3.2f, sConcreteA, darkMt);
        building(-555.0f, 435.0f,  8.0f, 6.0f, 4.2f, sPlaster,   rust);
        building(-530.0f, 545.0f,  6.0f, 6.0f, 3.4f, sConcreteA, rust, kNeon[3]);
        building(-670.0f, 545.0f,  7.0f, 5.0f, 4.6f, sPlaster,   darkMt);
        building(-540.0f, 478.0f,  6.0f, 5.0f, 3.0f, sConcreteA, rust);

        // Town-square plaza + fountain.
        { float g[3]; placeOnTerrain(-600.0f, 490.0f, g);
          const float plazaCol[4] = { 0.60f, 0.58f, 0.54f, 1.0f };
          addBoxProp(-600.0f, g[1] + 0.12f, 478.0f, 11.0f, 0.06f, 9.0f, plazaCol, nullptr, sConcreteA, 0.2f);
          addBoxProp(-600.0f, g[1] + 0.55f, 478.0f, 2.0f, 0.4f, 2.0f, conc, nullptr, sCement, 0.8f);
          const float water[4] = { 0.25f, 0.55f, 0.75f, 0.9f };
          addBoxProp(-600.0f, g[1] + 1.5f, 478.0f, 0.55f, 0.8f, 0.55f, conc, water, sCement, 1.0f); }

        p.buildingCount = m_buildings - before;
    }

    // =====================================================================
    // NEW DISTRICT (200, 500) — Babylon buildNewDistrict + buildCityRoads grid.
    // =====================================================================
    {
        const DistrictDef& d = kDistricts[1];
        CityZonePlan& p = m_zones[1];
        p.zone = CityZone::NewDistrict; p.name = d.name; p.cx = d.cx; p.cz = d.cz; p.radius = d.radius;
        const uint32_t before = m_buildings;

        // Main streets (E-W) + Industrial Blvd + side streets (N-S) — the
        // Babylon x3-city-roads.js grid verbatim.
        const float mainZ[4] = { 560.0f, 500.0f, 440.0f, 410.0f };
        for (int i = 0; i < 4; ++i) addRoad(80.0f, mainZ[i], 320.0f, mainZ[i], 7.0f);
        const float sideX[5] = { 120.0f, 170.0f, 220.0f, 270.0f, 310.0f };
        for (int i = 0; i < 5; ++i) addRoad(sideX[i], 410.0f, sideX[i], 580.0f, 4.0f);
        // Traffic lights: top-2 mains x first-4 sides (the Babylon intersection rule).
        for (int zi = 0; zi < 2; ++zi)
            for (int xi = 0; xi < 4; ++xi)
                trafficLight(sideX[xi] + 5.5f, mainZ[zi] + 8.5f);

        // 6 skyscrapers clustered on the core (varied footprint + height).
        building(185.0f, 485.0f, 13.0f, 13.0f, 42.0f, sCement,  white);
        building(215.0f, 515.0f, 12.0f, 11.0f, 36.0f, sPanelsA, conc);
        building(170.0f, 530.0f, 11.0f, 12.0f, 30.0f, sPanelsB, conc);
        building(235.0f, 470.0f, 10.0f, 10.0f, 28.0f, sCement,  white);
        building(200.0f, 455.0f, 12.0f, 10.0f, 33.0f, sPanelsA, conc);
        building(160.0f, 465.0f, 10.0f, 11.0f, 24.0f, sPanelsB, conc);

        // 8 apartment blocks ringing the core (+ south-face balcony slabs).
        const float apX[8] = { 135, 265, 135, 265, 150, 250, 300, 300 };
        const float apZ[8] = { 540, 540, 460, 460, 575, 575, 530, 470 };
        const float apH[8] = { 12, 14, 10, 15, 9, 11, 13, 10 };
        for (int i = 0; i < 8; ++i) {
            building(apX[i], apZ[i], 10.0f, 8.0f, apH[i], (i % 2) ? sPanelsB : sPlaster,
                     (i % 2) ? conc : warmPl);
            float g[3]; placeOnTerrain(apX[i], apZ[i], g);
            for (int b = 0; b < 2; ++b)   // balconies on the south (+Z) face
                addBoxProp(apX[i] - 2.0f + b * 4.0f, g[1] + 4.5f + b * 3.2f, apZ[i] + 4.3f,
                           1.1f, 0.10f, 0.7f, conc, nullptr, sConcreteA, 1.0f);
        }

        // 4 warehouses on the industrial edge (Blvd side) + loading docks.
        const float whX[4] = { 100, 140, 280, 310 };
        for (int i = 0; i < 4; ++i) {
            building(whX[i], 420.0f, 16.0f, 12.0f, 5.5f, sPanelsB, darkMt);
            float g[3]; placeOnTerrain(whX[i], 420.0f, g);
            addBoxProp(whX[i], g[1] + 0.6f, 427.0f, 3.5f, 0.6f, 1.2f, darkMt, nullptr, sGrate, 0.8f);
        }

        // 10 neon shops along the main street (alternating sides + sign colors).
        for (int i = 0; i < 10; ++i) {
            const float sx = 95.0f + (float)i * 23.0f;
            const float sz = (i % 2) ? 514.0f : 486.0f;
            building(sx, sz, 6.0f, 5.0f, 3.6f + (float)(i % 3) * 0.5f,
                     (i % 3) ? sPlaster : sConcreteA, (i % 3) ? warmPl : conc, kNeon[i % 5]);
            // Awning slab over the walk.
            float g[3]; placeOnTerrain(sx, sz, g);
            const float awn[4] = { 0.45f, 0.30f, 0.25f, 1.0f };
            addBoxProp(sx, g[1] + 2.6f, sz + ((i % 2) ? -3.1f : 3.1f), 2.6f, 0.07f, 0.9f, awn, nullptr);
        }

        p.buildingCount = m_buildings - before;
    }

    // =====================================================================
    // INDUSTRIAL ZONE (-200, 350) — factories / refinery / tank farm.
    // =====================================================================
    {
        const DistrictDef& d = kDistricts[2];
        CityZonePlan& p = m_zones[2];
        p.zone = CityZone::IndustrialZone; p.name = d.name; p.cx = d.cx; p.cz = d.cz; p.radius = d.radius;
        const uint32_t before = m_buildings;

        addRoad(-260.0f, 350.0f, -140.0f, 350.0f, 5.0f);   // cross street
        addRoad(-200.0f, 350.0f, -200.0f, 500.0f, 5.0f);   // connector -> freeway

        building(-230.0f, 340.0f, 18.0f, 12.0f, 8.0f, sPanelsB, darkMt);    // factory A
        { // twin smokestacks
          float g[3]; placeOnTerrain(-236.0f, 334.0f, g);
          addBoxProp(-236.0f, g[1] + 8.0f, 334.0f, 0.9f, 8.0f, 0.9f, darkMt, nullptr, sMetalTrimB, 0.8f);
          addBoxProp(-232.0f, g[1] + 7.0f, 334.0f, 0.8f, 7.0f, 0.8f, darkMt, nullptr, sMetalTrimB, 0.8f); }
        building(-180.0f, 330.0f, 14.0f, 10.0f, 6.0f, sPanelsA, steel);     // factory B
        building(-215.0f, 375.0f, 12.0f, 10.0f, 7.0f, sConcreteA, darkMt);  // processing
        building(-195.0f, 315.0f, 10.0f,  8.0f, 9.0f, sLattice,  steel);    // refinery lattice
        building(-165.0f, 395.0f, 14.0f, 10.0f, 5.0f, sConcreteA, rust);    // warehouse
        { // tank farm
          const float tx[3] = { -160.0f, -152.0f, -156.0f };
          const float tz[3] = {  358.0f,  362.0f,  350.0f };
          for (int i = 0; i < 3; ++i) {
              float g[3]; placeOnTerrain(tx[i], tz[i], g);
              addBoxProp(tx[i], g[1] + 3.4f, tz[i], 2.4f, 3.4f, 2.4f, steel, nullptr, sMetalTrimB, 0.6f);
              ++m_buildings;
          } }
        building(-240.0f, 370.0f,  9.0f,  7.0f, 4.5f, sConcreteA, rust);    // maintenance shed

        p.buildingCount = m_buildings - before;
    }

    // =====================================================================
    // CONNECTORS (terrain-conformed, segmented between the flat pads).
    // =====================================================================
    addRoadSegmented(-425.0f, 500.0f,   80.0f, 500.0f, 6.0f);   // Scrapyard <-> District freeway
    addRoadSegmented( 320.0f, 500.0f,  650.0f, 500.0f, 4.0f);   // coast spur (east)
    // The District -> Spire approach (the facility pad at the origin).
    addRoadSegmented( 170.0f, 410.0f,  170.0f, 150.0f, 4.0f);
    addRoadSegmented( 170.0f, 150.0f,   22.0f, 150.0f, 4.0f);
    // SEAM 3: the last leg ends AT the canon facility's apron edge (facade z1
    // ~55.5 + 24 m apron ring => ~79.5), not z=40 — the old end point is INSIDE
    // the canonical tower footprint (z -34.5..55.5), which put asphalt through
    // the Entrance-edge rooms. The approach road now meets the concrete apron.
    addRoadSegmented(  22.0f, 150.0f,   22.0f,  80.0f, 4.0f);

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
                   (t.dz != 0.0f ? 2.0f : tunHalfW + 1.5f), mouthCol, nullptr, sConcrete, 0.4f);
        // Throat: a SHORT sunken box behind the portal reading as the bore
        // diving underground (the old full-length 200 m surface bore rendered as
        // a giant wall across the map; the logical bore length stays in the
        // plan for gameplay/tests).
        const float throatLen = 18.0f;
        const float bx = t.mx + t.dx * throatLen * 0.5f;
        const float bz = t.mz + t.dz * throatLen * 0.5f;
        float bs[3]; placeOnTerrain(bx, bz, bs);
        addBoxProp(bx, bs[1] + tunHalfH - 3.0f, bz,
                   (t.dx != 0.0f ? throatLen * 0.5f : tunHalfW),
                   tunHalfH,
                   (t.dz != 0.0f ? throatLen * 0.5f : tunHalfW), tunCol, nullptr, sConcrete, 0.15f);
    }

    m_built = true;
    x3::logInfo("City::build complete — 3 districts (Scrapyard / New District / Industrial): "
                + std::to_string(m_buildings) + " buildings, "
                + std::to_string(m_windowBands) + " window bands, "
                + std::to_string(m_neonSigns) + " neon signs, "
                + std::to_string(m_trafficLights) + " traffic lights, "
                + std::to_string(m_roadSegments) + " road segments, 4 freeway tunnels; "
                + std::to_string((uint32_t)m_props.size()) + " props"
                + (outGlows ? ("; " + std::to_string(outGlows->size()) +
                               " glow lights emitted (window spill + sign wash)")
                            : std::string()));
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

    // ---- A real street grid exists (mains + sides + connectors). ----
    check(c.roadSegmentCount() >= 15,
          "C2 street grid built (" + std::to_string(c.roadSegmentCount()) + " segments, >=15)");

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

    // ---- C3b THE TUNNELS ARE ACTUALLY BORED, not two boxes each. -----------
    // Before this lane each "tunnel" was a portal box plus an 18 m sunken
    // throat box: nothing was cut, nothing was bored, and nothing could be
    // driven through. Now each plan registers a real TerrainCorridor.
    {
        clearTerrainCorridors();
        clearTerrainPortalHoles();
        const uint32_t before = terrainCorridorCount();
        const uint32_t bored  = registerCityFreewayTunnels();
        const uint32_t after  = terrainCorridorCount();
        // Each route registers ONE main corridor plus up to TWO portal plugs
        // per mouth (the portal cut — see app/tunnel_corridor.cpp), so the
        // count is now a range, not an equality: at least one per tunnel,
        // at most three.
        check(before == 0 && after >= kFreewayTunnelCount &&
              after <= kFreewayTunnelCount * 3u,
              "C3b every freeway tunnel registered a terrain corridor (" +
              std::to_string(after) + " corridors incl. portal plugs for " +
              std::to_string(kFreewayTunnelCount) + " tunnels)");
        // The corridors must be DISTINCT — the singleton builder this replaced
        // could only ever hold one route, so four calls yielded one tunnel.
        bool distinct = tunnelRouteCount() >= kFreewayTunnelCount;
        for (uint32_t a = 0; a < tunnelRouteCount() && distinct; ++a)
            for (uint32_t b = a + 1; b < tunnelRouteCount(); ++b) {
                const TunnelRoute* ra = tunnelRouteAt(a);
                const TunnelRoute* rb = tunnelRouteAt(b);
                if (ra && rb && std::fabs(ra->cx - rb->cx) < 1.0f
                             && std::fabs(ra->cz - rb->cz) < 1.0f) { distinct = false; break; }
            }
        check(distinct, "C3b the four routes are DISTINCT (the singleton could hold only one)");
        // Report bore validity rather than assert it: whether a heading meets a
        // hill is a fact about the terrain, and a silent pass here would hide a
        // freeway that tunnels through flat ground.
        x3::logInfo("[city-test] freeway bores that found a hill: " +
                    std::to_string(bored) + "/" + std::to_string(kFreewayTunnelCount));
        for (uint32_t i = 0; i < tunnelRouteCount(); ++i) {
            const TunnelRoute* r = tunnelRouteAt(i);
            if (!r) continue;
            x3::logInfo(std::string("[city-test]   ") + r->name + ": " +
                        (r->boreValid ? ("bore " + std::to_string((int)(r->boreS1 - r->boreS0)) + " m")
                                      : std::string("open cutting (no hill on this heading)")));
        }
        clearTerrainCorridors();
        clearTerrainPortalHoles();   // registered alongside the plugs — same hygiene
    }

    // ---- Props placed. ----
    check(c.propCount() > 0, "C4 props placed (total " + std::to_string(c.propCount()) + ")");

    // ---- W8-3 blockout-plus content: Babylon-scale massing counts. ----
    {
        const bool counts = c.buildingCount() >= 50 &&
                            c.zone(CityZone::ScrapyardCity).buildingCount >= 15 &&
                            c.zone(CityZone::NewDistrict).buildingCount   >= 25 &&
                            c.zone(CityZone::IndustrialZone).buildingCount >= 6;
        check(counts, "C5 Babylon-scale massing (" + std::to_string(c.buildingCount()) +
                      " buildings total; scrapyard/district/industrial populated)");
    }
    check(c.windowBandCount() >= 60,
          "C6 window bands on the massing (" + std::to_string(c.windowBandCount()) + ", >=60)");
    check(c.trafficLightCount() >= 6 && c.neonSignCount() >= 6,
          "C7 street life props (traffic lights " + std::to_string(c.trafficLightCount()) +
          ", neon signs " + std::to_string(c.neonSignCount()) + ")");

    // ==== STREET LIGHT (street_lights.*): real lamps over the same grid. ====
    {
        StreetLights sl;
        sl.buildCityLamps(scene, device);                       // region-hook path
        sl.buildHostLamps(scene, device, 0.2f, 25.0f, 58.0f);   // apron + approach

        // ---- L1: lamps in every district + the approach/apron rows. ----
        const uint32_t nScrap = sl.lampCount(StreetLights::Zone::Scrapyard);
        const uint32_t nNew   = sl.lampCount(StreetLights::Zone::NewDistrict);
        const uint32_t nInd   = sl.lampCount(StreetLights::Zone::Industrial);
        const uint32_t nAppr  = sl.lampCount(StreetLights::Zone::Approach);
        const uint32_t nApron = sl.lampCount(StreetLights::Zone::Apron);
        check(nScrap > 0 && nNew > 0 && nInd > 0 && nAppr > 0 && nApron > 0,
              "L1 lamps per district (scrapyard " + std::to_string(nScrap) +
              ", new " + std::to_string(nNew) + ", industrial " + std::to_string(nInd) +
              ", approach " + std::to_string(nAppr) + ", apron " + std::to_string(nApron) + ")");

        // ---- L2: nearest-K selection returns <= K, all lit, nearest included. ----
        {
            std::vector<x3::rhi::PointLight> out;
            const uint32_t n = sl.selectLights(200.0f, 2.0f, 500.0f, out, 14);
            bool nearestIn = false;
            float bestD = 1e30f; const StreetLights::Lamp* best = nullptr;
            for (const auto& l : sl.lamps()) {
                if (l.state == StreetLights::State::Dead) continue;
                const float dx = l.head[0] - 200.0f, dz = l.head[2] - 500.0f;
                const float d = dx * dx + dz * dz;
                if (d < bestD) { bestD = d; best = &l; }
            }
            for (const auto& pl : out)
                if (best && std::fabs(pl.pos[0] - best->head[0]) < 0.01f &&
                    std::fabs(pl.pos[2] - best->head[2]) < 0.01f) nearestIn = true;
            check(n <= 14 && n == (uint32_t)out.size() && n > 0 && nearestIn,
                  "L2 nearest-K lamp selection (returned " + std::to_string(n) +
                  " <= 14, nearest lit lamp included)");
        }

        // ---- L3: lived-in variance fractions in range (~8% dead / ~5% flicker). ----
        {
            const float total = (float)sl.lampCount();
            const float fDead  = (float)sl.deadCount() / total;
            const float fFlick = (float)sl.flickerCount() / total;
            check(fDead >= 0.02f && fDead <= 0.16f && fFlick >= 0.01f && fFlick <= 0.12f,
                  "L3 variance fractions (dead " + std::to_string(sl.deadCount()) + "/" +
                  std::to_string(sl.lampCount()) + ", flicker " +
                  std::to_string(sl.flickerCount()) + ")");
        }

        // ---- L4: the dock work light exists (lit, never dead). ----
        check(sl.hasDockWorkLight(), "L4 dock work light rig at the crate zone");

        // ---- L5: the flicker machine animates (dt-scaled) and stays in [0,1]. ----
        {
            bool inRange = true, sawDip = false;
            for (int i = 0; i < 300; ++i) {   // 5 s at 60 Hz
                sl.update(1.0f / 60.0f, scene);
                for (const auto& l : sl.lamps()) {
                    if (l.level < -0.001f || l.level > 1.001f) inRange = false;
                    if (l.state == StreetLights::State::Flicker && l.level < 0.5f)
                        sawDip = true;
                }
            }
            check(inRange && (sl.flickerCount() == 0 || sawDip),
                  "L5 flicker bursts animate (levels in range, dips observed)");
        }

        // ==== CONTENT WIRING (lane inspx/content-wiring) =================
        // The legacy build above is the NEGATIVE CONTROL: it must stay at the
        // nine authored rows. If a future edit accidentally makes the dense
        // grid the default, D1 fails and says so.
        check(sl.lampCount() >= 50 && sl.lampCount() <= 80,
              "D1 NEGATIVE CONTROL: r_citylights 0 keeps the legacy lamp count ("
              + std::to_string(sl.lampCount()) + ", expected 50..80)");
    }

    // ==== D2..D4: the DENSE city (r_citylights 1) ========================
    {
        Scene dscene;
        HeadlessDevice ddev;
        std::unique_ptr<x3::phys::IPhysicsWorld> dphys(x3::phys::createPhysicsWorld());
        dphys->init();

        // D2: City emits a glow light per warm-lit window band + per neon sign.
        std::vector<StreetLights::Glow> glows;
        City dc;
        dc.build(dscene, ddev, *dphys, nullptr, &glows);
        uint32_t signGlows = 0, windowGlows = 0;
        for (const auto& g : glows) (g.sign ? signGlows : windowGlows)++;
        check(!glows.empty() && signGlows == dc.neonSignCount() && windowGlows > 0,
              "D2 city glow lights emitted (" + std::to_string(windowGlows) +
              " window spill + " + std::to_string(signGlows) + " sign wash; signs match "
              + std::to_string(dc.neonSignCount()) + ")");

        StreetLights dsl;
        dsl.buildCityLamps(dscene, ddev, /*dense*/true);
        const uint32_t denseLamps = dsl.lampCount();
        dsl.adoptCityGlows(glows);

        // D3: THE POINT OF THE LANE. Clustered lighting raised the cap to 1024
        // and the city fed 14. A dense night city must clear 200 LIVE sources
        // (dead lamps excluded -- those emit nothing), or the froxel path is
        // still carrying a scene the legacy 64-light loop could have handled.
        std::vector<x3::rhi::PointLight> pool;
        const uint32_t live = dsl.selectLights(200.0f, 2.0f, 500.0f, pool,
                                               x3::rhi::kMaxSceneLights);
        check(live >= 200,
              "D3 dense city yields 200+ LIVE point lights (" + std::to_string(live) +
              " of " + std::to_string(dsl.lampCount()) + " sources; " +
              std::to_string(denseLamps) + " lamps + " + std::to_string(glows.size()) +
              " glows, " + std::to_string(dsl.deadCount()) + " dead)");

        // D4: and it must be a real multiple of the legacy grid, not a nudge.
        check(denseLamps >= 200,
              "D4 every authored street lamped (" + std::to_string(denseLamps) +
              " lamps vs ~56 legacy, >=200)");

        // D5: the glow lights carry NO geometry -- they are pure PointLights.
        // If a future edit gives them posts/cones, the city entity count
        // explodes silently. Compare the scene size before/after adoption.
        {
            StreetLights probe;
            Scene pscene;
            HeadlessDevice pdev;
            probe.buildCityLamps(pscene, pdev, true);
            const size_t before = pscene.size();
            probe.adoptCityGlows(glows);
            check(pscene.size() == before,
                  "D5 glow lights add ZERO scene entities (scene stayed at " +
                  std::to_string(before) + ")");
        }

        dphys->shutdown();
    }

    physics->shutdown();
    x3::logInfo(std::string("city: ") + std::to_string(g_pass) + "/" +
                std::to_string(g_pass + g_fail) + " passed");
    return g_fail == 0;
}

} // namespace x3::game
