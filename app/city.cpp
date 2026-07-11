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
#include "hackables.h"          // feat/city-aaa: the WD2 hacking layer scattered by the neon district
#include "crowd.h"              // feat/city-aaa: civilian crowd on the neon-district drag
#include "env_art.h"            // feat/city-aaa: real GLB building facades for the neon district
#include "headless_device.h"
#include "mesh_prims.h"
#include "surface_library.h"
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

void City::build(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
                 SurfaceLibrary* sharedSurf) {
    (void)physics;   // blockout-plus city is visual-only this pass (no collision body)

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
    auto streetLight = [&](float cx, float cz) {
        float g[3]; placeOnTerrain(cx, cz, g);
        const float pole[4] = { 0.22f, 0.23f, 0.25f, 1.0f };
        const float warm[4] = { 1.0f, 0.82f, 0.55f, 1.6f };
        const float head[4] = { 0.12f, 0.11f, 0.09f, 1.0f };
        addBoxProp(cx, g[1] + 2.4f, cz, 0.10f, 2.4f, 0.10f, pole, nullptr);
        addBoxProp(cx, g[1] + 4.9f, cz, 0.42f, 0.16f, 0.42f, head, warm);
    };
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

        // Street lights every ~40 m along the main street.
        for (float lx = -760.0f; lx <= -440.0f; lx += 40.0f) streetLight(lx, 511.5f);

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
    addRoadSegmented(  22.0f, 150.0f,   22.0f,  40.0f, 4.0f);

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
                + std::to_string((uint32_t)m_props.size()) + " props");
}

namespace {

// Metallic-roughness map (glTF packing: B=metallic, G=roughness). Low roughness = a
// wet mirror street; high metallic + low rough = chrome trim.
x3::rhi::TextureHandle makeMRTex(x3::rhi::IRenderDevice& device, float metallic, float roughness) {
    auto px = x3::prims::makeSolidRGBA(4, 0,
                (uint8_t)std::clamp(roughness * 255.0f, 0.0f, 255.0f),
                (uint8_t)std::clamp(metallic  * 255.0f, 0.0f, 255.0f));
    return device.createTexture(px.data(), 4, 4, /*srgb=*/false);
}

// A small emissive HOLO-MARKER diamond hung above a hackable — the WD2 "NetHack" tag.
// Returns its entity id (the host brightens it while the highlight is held). Dim by
// default so the street still reads clean until you scan.
uint32_t addHackMarker(Scene& scene, x3::rhi::IRenderDevice& device,
                       float x, float y, float z, const float col[3]) {
    auto m = x3::prims::makeCrystal(0.28f, 0.05f, 0.45f);   // a little floating lozenge
    Entity e;
    e.mesh = device.createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                               m.index.data(), (uint32_t)m.index.size());
    e.baseColor[0] = col[0]; e.baseColor[1] = col[1]; e.baseColor[2] = col[2]; e.baseColor[3] = 1.0f;
    // Dim resting glow; the highlight toggle multiplies this up (host loop).
    e.emissive[0] = col[0]; e.emissive[1] = col[1]; e.emissive[2] = col[2]; e.emissive[3] = 0.5f;
    e.transform[12] = x; e.transform[13] = y; e.transform[14] = z;
    e.tag = (uint32_t)Tag::Prop;
    return scene.add(e);
}

} // namespace

// Converted cyberpunk building facades (HIVEMIND Cyberpunk City, LOD0, base-centered,
// solid-PBR re-skinned by tools/build_city_facades.py). nativeH = the GLB's native Y
// extent (m) so the placer can non-uniform-scale each to a lot's target height. Placed
// round-robin so the skyline is varied but deterministic.
namespace {
// nativeW/H/D = the GLB's native extent (m). lbx/lby/lbz = the local base-center
// (footprint center X/Z + min Y) so the placer maps that point to the lot (bx,g,bz)
// — the GLBs are NOT origin-centered, so this offset seats them on the ground.
// The loader bakes the GLB's node transform (already base-centered on X/Z, base at
// Y=0) into each drawable, so lb = (0,0,0) here; nativeW/H/D are the WORLD extents.
struct FacadeDef { const char* rel; float nativeW, nativeH, nativeD; float lbx, lby, lbz; };
const FacadeDef kFacades[] = {
    { "CyberpunkCity/SM_MERGED_BP_Shop_B10.glb",      15.30f, 16.90f, 16.88f, 0,0,0 },
    { "CyberpunkCity/SM_MERGED_BP_Shop_A20.glb",      14.01f,  8.59f, 15.24f, 0,0,0 },
    { "CyberpunkCity/SM_MERGED_BP_Building9.glb",      8.60f, 17.91f, 16.40f, 0,0,0 },
    { "CyberpunkCity/SM_MERGED_BP_House_Shop_E10.glb",12.48f, 22.70f, 12.48f, 0,0,0 },
    { "CyberpunkCity/SM_MERGED_BP_Shop_B15.glb",      13.78f, 16.90f, 13.78f, 0,0,0 },
    { "CyberpunkCity/SM_MERGED_BP_Building10_2.glb",   8.60f, 17.90f, 16.50f, 0,0,0 },
};
constexpr int kFacadeCount = (int)(sizeof(kFacades) / sizeof(kFacades[0]));
// Compose T * RotY(theta) * Scale(sx,sy,sz) into a column-major 4x4, choosing T so
// the local base-center point `lb` lands exactly at world (tx,ty,tz).
inline void makeFacadeXform(float tx, float ty, float tz, float theta,
                            float sx, float sy, float sz,
                            float lbx, float lby, float lbz, float out[16]) {
    const float c = std::cos(theta), s = std::sin(theta);
    // R*S*lb (the base-center after rotate+scale) — subtract it from the target T.
    const float px = c * (sx * lbx) + s * (sz * lbz);
    const float py = sy * lby;
    const float pz = -s * (sx * lbx) + c * (sz * lbz);
    out[0]=c*sx;  out[1]=0;    out[2]=-s*sx; out[3]=0;
    out[4]=0;     out[5]=sy;   out[6]=0;     out[7]=0;
    out[8]=s*sz;  out[9]=0;    out[10]=c*sz; out[11]=0;
    out[12]=tx-px; out[13]=ty-py; out[14]=tz-pz; out[15]=1;
}
} // namespace

NeonDistrictStats buildNeonDistrict(Scene& scene, x3::rhi::IRenderDevice& device,
                                    x3::phys::IPhysicsWorld& physics,
                                    HackableRegistry* hax, CrowdSystem* crowd,
                                    float cx, float cz, EnvArtSystem* facades) {
    (void)physics;
    using x3::prims::makeBox;
    NeonDistrictStats st; st.centerX = cx; st.centerZ = cz;

    // Preload the real facade GLBs once (cached upload; instanced across the block).
    // facadeIdx[i] == UINT32_MAX if that GLB is missing -> that lot keeps its box body.
    uint32_t facadeIdx[kFacadeCount];
    for (int i = 0; i < kFacadeCount; ++i) facadeIdx[i] = 0xFFFFFFFFu;
    int facadesReady = 0;
    if (facades) {
        for (int i = 0; i < kFacadeCount; ++i) {
            facadeIdx[i] = facades->loadFacade(kFacades[i].rel);
            if (facadeIdx[i] != 0xFFFFFFFFu) ++facadesReady;
        }
        x3::logInfo("buildNeonDistrict: " + std::to_string(facadesReady) + "/" +
                    std::to_string(kFacadeCount) + " real GLB facades loaded");
    }

    // ---- Shared materials (procedural — no disk dependency) ----
    auto concreteTex = device.createTexture(x3::prims::makeCleanPanelRGBA(256, 5).data(), 256, 256, true);
    auto panelTex    = device.createTexture(x3::prims::makeSciFiPanelRGBA(256, 4).data(), 256, 256, true);
    auto asphaltTex  = device.createTexture(x3::prims::makeSolidRGBA(4, 14, 15, 20).data(), 4, 4, true);
    auto sidewalkTex = device.createTexture(x3::prims::makeSolidRGBA(4, 70, 72, 80).data(), 4, 4, true);
    auto darkTex     = device.createTexture(x3::prims::makeSolidRGBA(4, 10, 12, 16).data(), 4, 4, true);
    auto steelTex    = device.createTexture(x3::prims::makeSolidRGBA(4, 150, 154, 162).data(), 4, 4, true);
    auto wetMR       = makeMRTex(device, /*metal*/0.30f, /*rough*/0.05f);  // wet MIRROR asphalt (crisp neon SSR)
    auto chromeMR    = makeMRTex(device, /*metal*/0.9f,  /*rough*/0.18f);

    const float kNoEmis[4] = { 0, 0, 0, 0 };
    const float white[4]   = { 0.92f, 0.94f, 1.0f, 1.0f };   // cool bone concrete
    const float darkCol[4] = { 0.10f, 0.12f, 0.16f, 1.0f };  // void-dark housing/panel color

    // Terrain-anchored ground for the district center.
    float cs[3]; placeOnTerrain(cx, cz, cs);
    const float g = cs[1];
    st.groundY = g;

    auto addBox = [&](float x, float y, float z, float hx, float hy, float hz,
                      const float col[4], const float emis[4],
                      x3::rhi::TextureHandle tex, x3::rhi::TextureHandle mr = {}) -> uint32_t {
        auto m = makeBox(hx, hy, hz, x, y, z, 0.4f);
        Entity e;
        e.mesh = device.createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                   m.index.data(), (uint32_t)m.index.size());
        e.tex = tex; e.mrTex = mr;
        for (int i = 0; i < 4; ++i) e.baseColor[i] = col[i];
        for (int i = 0; i < 4; ++i) e.emissive[i]  = emis[i];
        e.tag = (uint32_t)Tag::Prop;
        return scene.add(e);
    };

    // ================= 1) STREETS + SIDEWALKS (the wet grid) =================
    // A dark asphalt PAD under the whole district (so terrain grass never shows through
    // the block), then a main drag (E-W along X) + two cross streets (along Z), each a
    // low-roughness wet-reflective ribbon. Raised bone sidewalks flank the drag.
    const float halfBlock = 90.0f;   // district half-extent
    const float roadY = g + 0.06f;
    addBox(cx, roadY - 0.05f, cz, halfBlock, 0.05f, halfBlock, white /*unused*/, kNoEmis, darkTex); // base pad
    // Main drag: 200 m long, 14 m wide, wet mirror.
    addBox(cx, roadY, cz, halfBlock, 0.07f, 7.0f, white, kNoEmis, asphaltTex, wetMR);
    // Two cross streets.
    addBox(cx - 45.0f, roadY, cz, 6.0f, 0.07f, halfBlock, white, kNoEmis, asphaltTex, wetMR);
    addBox(cx + 45.0f, roadY, cz, 6.0f, 0.07f, halfBlock, white, kNoEmis, asphaltTex, wetMR);
    // Sidewalks flanking the drag (raised curbs). DARK wet pavement so the hot sodium
    // lamps read as pools ON pavement, not as continuous glowing cream bands (the old
    // 0.55 albedo blew out under the bright point lights). curbCol (lighter) stays for
    // the posts/poles/junction boxes below where a brighter concrete reads correctly.
    const float curbCol[4]  = { 0.50f, 0.52f, 0.57f, 1.0f };
    const float walkCol[4]  = { 0.24f, 0.25f, 0.29f, 1.0f };   // dark sidewalk pavement
    addBox(cx, roadY + 0.12f, cz + 9.5f, halfBlock, 0.18f, 2.5f, walkCol, kNoEmis, sidewalkTex);
    addBox(cx, roadY + 0.12f, cz - 9.5f, halfBlock, 0.18f, 2.5f, walkCol, kNoEmis, sidewalkTex);
    // Painted lane center-line (faint emissive so the wet street reads as a road).
    const float lane[4] = { 1.2f, 1.0f, 0.3f, 0.8f };
    for (float lx = cx - 80.0f; lx < cx + 80.0f; lx += 8.0f)
        addBox(lx, roadY + 0.09f, cz, 1.6f, 0.02f, 0.18f, lane, lane, steelTex);

    // ================= 2) BUILDINGS (varied massing + neon) =================
    // Two rows of lots facing the drag (north row at +Z, south row at -Z). Deterministic
    // LCG so the skyline is stable. Each tower: bone concrete body, an emissive WINDOW
    // grid tint, a lit ground-floor shopfront, and ONE neon accent sign (signal cyan or a
    // single warm/magenta accent — never three colors on one building).
    uint32_t rng = 0x5EED11u;
    auto rnd = [&]() { rng = rng * 1664525u + 1013904223u; return (float)((rng >> 8) & 0xFFFF) / 65535.0f; };
    struct Accent { float r, g, b; };
    const Accent kCyan{ 0.15f, 1.5f, 2.1f };
    const Accent kAmber{ 2.2f, 1.1f, 0.25f };
    const Accent kMagenta{ 2.0f, 0.3f, 1.7f };
    const Accent accents[3] = { kCyan, kAmber, kMagenta };

    // Record each building's street front so the PROP-CLUTTER pass (below) can hang
    // AC condensers / vents / pipes on the facades without perturbing the building LCG.
    struct Front { float bx, bz, faceZ, useH, w, d; };
    std::vector<Front> fronts;

    for (int row = 0; row < 2; ++row) {
        const float rz = cz + (row == 0 ? 1.0f : -1.0f) * 15.0f;    // building front line
        const float faceZ = (row == 0 ? 1.0f : -1.0f);
        for (int i = 0; i < 8; ++i) {
            const float bx = cx - 80.0f + i * 22.0f + (rnd() - 0.5f) * 3.0f;
            const float w  = 8.0f + rnd() * 3.5f;
            const float d  = 9.0f + rnd() * 5.0f;
            const float h  = 16.0f + rnd() * 46.0f;             // varied 16..62 m
            const float bz = rz + faceZ * (d + 3.0f);           // set back from the sidewalk
            const float by = g + h * 0.5f;
            // ---- BODY: a REAL cyberpunk GLB facade when available, else the box. ----
            // Round-robin a facade GLB; non-uniform-scale it to the lot (full width 2w,
            // full depth 2d, height clamped to keep window proportions sane) and rotate
            // to face the street. The GLB is base-centered so ty = ground g. The neon
            // sign / shopfront / canopy / camera / crown below still key off `h`, so
            // scaling the facade's height to `fh` keeps them registered.
            bool placedFacade = false;
            const float fh = std::clamp(h, 14.0f, 36.0f);   // facade target height (proportion cap)
            if (facadesReady > 0) {
                const int fi = (row * 8 + i) % kFacadeCount;
                if (facadeIdx[fi] != 0xFFFFFFFFu) {
                    const FacadeDef& F = kFacades[fi];
                    const float sx = (2.0f * w) / F.nativeW;
                    const float sy = fh / F.nativeH;
                    const float sz = (2.0f * d) / F.nativeD;
                    // Front faces the street: row 0 (+Z side) looks toward -Z, row 1 toward +Z.
                    const float theta = (row == 0 ? 3.14159265f : 0.0f);
                    float xf[16];
                    makeFacadeXform(bx, g, bz, theta, sx, sy, sz, F.lbx, F.lby, F.lbz, xf);
                    placedFacade = facades->bakeInto(scene, facadeIdx[fi], xf,
                                                     /*emisScale=*/4.5f, /*baseGlow=*/0.09f) > 0;
                }
            }
            const float useH = placedFacade ? fh : h;   // signage/crown key off the actual body height
            if (!placedFacade) {
                // Body: bone concrete with a cool emissive window-grid glow (inhabited dusk).
                const float lit = 0.10f + rnd() * 0.16f;
                const float winEmis[4] = { lit * 0.55f, lit * 0.8f, lit * 1.15f, lit };
                addBox(bx, by, bz, w, h * 0.5f, d, white, winEmis, panelTex);
            } else {
                (void)by; rnd();   // keep the LCG stream aligned with the box path (deterministic layout)
            }
            st.buildings++;
            // Ground-floor shopfront: a WARM lit interior band behind a dark mullion
            // grid facing the street. Kept SLIM + dim (was a 2.2 m tall 0.55-strength band
            // that bloomed into a continuous glowing-cream WALL down the block); now a
            // ~1.2 m window strip at a subtle glow so it reads as lit shop interiors.
            const float shopWarm[4] = { 0.85f, 0.5f, 0.22f, 0.22f };
            const float shopCol[4]  = { 0.5f, 0.4f, 0.3f, 1.0f };
            addBox(bx, g + 1.2f, bz - faceZ * (d - 0.1f), w * 0.82f, 0.6f, 0.22f,
                   shopCol, shopWarm, panelTex);
            // A slim dark canopy over the shopfront (trim — sells the ground floor).
            addBox(bx, g + 2.8f, bz - faceZ * (d + 0.35f), w * 0.85f, 0.12f, 0.5f,
                   darkCol, kNoEmis, steelTex);
            // ONE neon sign: an emissive strip up the street-facing corner (upper third).
            const Accent a = accents[(int)(rnd() * 3.0f) % 3];
            const float neon[4] = { a.r, a.g, a.b, 2.1f };
            addBox(bx + w * 0.8f, g + useH * 0.72f, bz - faceZ * (d + 0.2f),
                   0.28f, useH * 0.22f, 0.28f, white, neon, steelTex);
            st.signs++;
            // Rooftop parapet crest (thin cyan strip — the tower crown rhythm).
            const float crown[4] = { kCyan.r, kCyan.g, kCyan.b, 2.2f };
            addBox(bx, g + useH + 0.4f, bz, w + 0.3f, 0.25f, d + 0.3f, white, crown, steelTex);
            fronts.push_back({ bx, bz, faceZ, useH, w, d });

            // ---- HACKABLE: a security CAMERA on the street-facing upper corner ----
            if (hax) {
                const float camX = bx - w * 0.8f, camY = g + useH - 2.0f, camZ = bz - faceZ * (d + 0.3f);
                const float camLens[4] = { 2.0f, 0.2f, 0.2f, 1.4f };            // red lens dot
                addBox(camX, camY, camZ, 0.35f, 0.22f, 0.5f, curbCol, camLens, steelTex);
                HackableObject c; c.type = HackableType::Camera;
                c.pos = { camX, camY, camZ };
                const float cyanMk[3] = { kCyan.r, kCyan.g, kCyan.b };
                c.entity = addHackMarker(scene, device, camX, camY + 1.0f, camZ, cyanMk);
                hax->add(c); st.hackables++;
            }
        }
    }

    // ================= 2.5) PROP CLUTTER (fill the empty street with life) =======
    // The biggest non-AAA tell after the facades: AAA streets are DENSE. Scatter real
    // HIVEMIND props (converted GLBs w/ their own PBR sets) — dumpsters/trash/pallets on
    // the sidewalks, AC condensers + vents + pipes hung on the facades, real street lamps,
    // and procedural overhead cables — so the eye never finds bare asphalt. Instanced via
    // the EnvArtSystem (cached GLB upload, baked static entities). Deterministic prng so
    // the layout is stable and INDEPENDENT of the building LCG.
    if (facades) {
        // Load each clutter GLB once (cached). UINT32_MAX => skip that prop (fallback).
        auto LP = [&](const char* rel) { return facades->loadFacade(rel); };
        const uint32_t pDumpster = LP("CityProps/Dumpster.glb");
        const uint32_t pCondA    = LP("CityProps/Cond_A.glb");
        const uint32_t pCondB    = LP("CityProps/Cond_B.glb");
        const uint32_t pTrash1   = LP("CityProps/Trash_01.glb");
        const uint32_t pTrash2   = LP("CityProps/Trash_02.glb");
        const uint32_t pBag1     = LP("CityProps/Trashbag_01.glb");
        const uint32_t pBag2     = LP("CityProps/Trashbag_02.glb");
        const uint32_t pPallet   = LP("CityProps/Pallet.glb");
        const uint32_t pCan      = LP("CityProps/TrashCan.glb");
        const uint32_t pVentSq   = LP("CityProps/Vent_Sq.glb");
        const uint32_t pVentR    = LP("CityProps/Vent_Round.glb");
        const uint32_t pPipe     = LP("CityProps/Pipe_Wall.glb");
        const uint32_t pLamp     = LP("CityProps/LampStreet.glb");
        const uint32_t pBillboard = LP("CityProps/Billboard.glb");

        uint32_t prng = 0xC1A77Eu;
        auto pr = [&]() { prng = prng * 1664525u + 1013904223u; return (float)((prng >> 8) & 0xFFFF) / 65535.0f; };
        // Place one prop instance: real-world scale (uniform s), Y-rotated, seated on g.
        auto place = [&](uint32_t pi, float x, float y, float z, float theta,
                         float s, float emisScale, float baseGlow) {
            if (pi == 0xFFFFFFFFu) return;
            float xf[16];
            makeFacadeXform(x, y, z, theta, s, s, s, 0, 0, 0, xf);
            if (facades->bakeInto(scene, pi, xf, emisScale, baseGlow) > 0) st.propClutter++;
        };
        const float TWO_PI = 6.2831853f;

        // (a) GROUND CLUTTER down BOTH sidewalks (z ~ ±11..±13), receding down the drag.
        // Dense: a cluster every ~7 m, alternating sides, jittered + randomly rotated.
        for (float x = cx - 84.0f; x <= cx + 84.0f; x += 7.0f) {
            for (int side = 0; side < 2; ++side) {
                const float sz = (side == 0 ? 1.0f : -1.0f);
                const float baseZ = cz + sz * (11.0f + pr() * 2.5f);
                const float jx = x + (pr() - 0.5f) * 3.0f;
                const float th = pr() * TWO_PI;
                const float roll = pr();
                if (roll < 0.18f)      place(pDumpster, jx, g, baseZ, th, 1.0f, 0, 0.05f);
                else if (roll < 0.34f) place(pTrash1, jx, g, baseZ, th, 0.8f + pr() * 0.4f, 0, 0.05f);
                else if (roll < 0.48f) place(pTrash2, jx, g, baseZ, th, 0.8f + pr() * 0.4f, 0, 0.05f);
                else if (roll < 0.64f) place(pCan, jx, g, baseZ, th, 1.0f, 0, 0.06f);
                else if (roll < 0.80f) place(pPallet, jx, g, baseZ, th, 1.0f, 0, 0.05f);
                else if (roll < 0.90f) place(pBag1, jx, g, baseZ, th, 1.0f, 0, 0.06f);
                else                   place(pBag2, jx, g, baseZ, th, 1.0f, 0, 0.06f);
                // A second small item next to ~half of them (piles read as piles).
                if (pr() < 0.5f)
                    place(pr() < 0.5f ? pBag1 : pCan, jx + (pr() - 0.5f) * 1.8f, g,
                          baseZ + (pr() - 0.5f) * 1.8f, pr() * TWO_PI, 1.0f, 0, 0.06f);
            }
        }

        // (b) WALL CLUTTER on the building fronts: AC condensers + vents + a riser pipe
        // on the street-facing wall of each building, at varied heights (the busy-facade
        // silhouette that sells a lived-in block).
        for (const Front& f : fronts) {
            const float wallZ = f.bz - f.faceZ * (f.d + 0.15f);   // just proud of the facade
            const float faceTheta = (f.faceZ > 0.0f ? 0.0f : 3.14159265f);
            // 2-3 AC condensers stacked/scattered up the wall.
            const int nAC = 2 + (int)(pr() * 2.0f);
            for (int k = 0; k < nAC; ++k) {
                const float ax = f.bx + (pr() - 0.5f) * (f.w * 1.4f);
                const float ay = g + 3.0f + pr() * (f.useH - 5.0f);
                place(pr() < 0.5f ? pCondA : pCondB, ax, ay, wallZ, faceTheta, 1.0f, 0, 0.05f);
            }
            // A tall riser pipe up one side + a vent.
            place(pPipe, f.bx + f.w * (pr() < 0.5f ? 0.9f : -0.9f), g + 0.2f, wallZ,
                  faceTheta, 1.0f + pr() * 0.6f, 0, 0.04f);
            place(pr() < 0.5f ? pVentSq : pVentR, f.bx + (pr() - 0.5f) * f.w,
                  g + 2.5f + pr() * 3.0f, wallZ, faceTheta, 1.0f, 0, 0.05f);
            // ---- HOLO-AD BILLBOARD on the upper facade (item 4): a real cyberpunk ad
            // panel (T_Billboard emissive), mounted flat on the street-facing wall so
            // the block reads as advertised, not blank massing. ~65% of buildings get one.
            if (pBillboard != 0xFFFFFFFFu && pr() < 0.65f) {
                const float bs = 1.6f + pr() * 1.3f;                 // 8..15 m wide ad
                const float by = g + f.useH * (0.42f + pr() * 0.32f);
                const float btheta = (f.faceZ > 0.0f ? 1.5707963f : -1.5707963f);
                const float bwz = f.bz - f.faceZ * (f.d + 0.30f);    // proud of the wall
                place(pBillboard, f.bx + (pr() - 0.5f) * f.w * 0.5f, by, bwz, btheta, bs, 0.55f, 0.0f);
            }
        }

        // (c) REAL street-lamp meshes down the drag (richer than the box posts; the box
        // lamp HEADS below still provide the emissive light-source pools).
        if (pLamp != 0xFFFFFFFFu) {
            int li = 0;
            for (float lx = cx - 80.0f; lx <= cx + 80.0f; lx += 20.0f) {
                const float lz = cz + (li++ % 2 == 0 ? 1.0f : -1.0f) * 10.5f;
                place(pLamp, lx, g, lz, (lz > cz ? 0.0f : 3.14159265f), 1.0f, 1.6f, 0.0f);
            }
        }

        // (d) OVERHEAD CABLES: thin near-taut strands strung high across the street. Kept
        // SHALLOW-sag (small dy per segment) so the axis-aligned thin boxes read as wires,
        // not blocks; strung high (~8 m) so they frame the sky, not the eyeline.
        {
            const float cableCol[4] = { 0.04f, 0.04f, 0.05f, 1.0f };
            for (float lx = cx - 78.0f; lx <= cx + 78.0f; lx += 10.0f) {
                const int segs = 10;
                const float z0 = cz - 11.0f, z1 = cz + 11.0f;
                const float sag = 0.35f + pr() * 0.35f;   // shallow -> reads as a wire
                const float topY = g + 8.0f + pr() * 1.6f;
                const float px = lx + (pr() - 0.5f) * 4.0f;
                const float thick = 0.035f;
                for (int s = 0; s < segs; ++s) {
                    const float t0 = (float)s / segs, t1 = (float)(s + 1) / segs;
                    const float za = z0 + (z1 - z0) * t0, zb = z0 + (z1 - z0) * t1;
                    const float ya = topY - sag * 4.0f * t0 * (1.0f - t0);
                    const float yb = topY - sag * 4.0f * t1 * (1.0f - t1);
                    const float mz = (za + zb) * 0.5f, my = (ya + yb) * 0.5f;
                    const float halfZ = std::fabs(zb - za) * 0.5f;
                    const float halfY = std::fabs(yb - ya) * 0.5f + thick;   // span the small dy
                    addBox(px, my, mz, thick, halfY, halfZ, cableCol, kNoEmis, darkTex);
                    st.propClutter++;
                }
            }
        }
    }

    // ================= 3) STREET LAMPS (light pools, dark between) =================
    const float lampWarm[4] = { 1.7f, 1.35f, 0.85f, 3.0f };
    for (float lx = cx - 80.0f; lx <= cx + 80.0f; lx += 20.0f) {
        for (int s = 0; s < 2; ++s) {
            const float lz = cz + (s == 0 ? 1.0f : -1.0f) * 11.0f;
            addBox(lx, g + 3.0f, lz, 0.18f, 3.0f, 0.18f, curbCol, kNoEmis, steelTex);   // post
            addBox(lx, g + 6.1f, lz, 0.5f, 0.2f, 0.5f, white, lampWarm, steelTex);      // lamp head
            st.streetlights++;
        }
    }

    // ================= 4) TRAFFIC SIGNALS + JUNCTION BOXES + ATMs (hackables) =====
    if (hax) {
        const float amberMk[3] = { kAmber.r, kAmber.g, kAmber.b };
        const float magMk[3]   = { kMagenta.r, kMagenta.g, kMagenta.b };
        const float greenMk[3] = { 0.2f, 1.8f, 0.6f };
        // Traffic signals at the two intersections.
        for (float ix : { cx - 45.0f, cx + 45.0f }) {
            addBox(ix, g + 3.2f, cz + 9.0f, 0.16f, 3.2f, 0.16f, curbCol, kNoEmis, steelTex);
            const float sig[4] = { 2.0f, 0.5f, 0.2f, 2.0f };
            addBox(ix, g + 5.6f, cz + 9.0f, 0.3f, 0.6f, 0.25f, darkCol, sig, steelTex);
            HackableObject s; s.type = HackableType::TrafficSignal; s.pos = { ix, g + 5.6f, cz + 9.0f };
            s.entity = addHackMarker(scene, device, ix, g + 6.8f, cz + 9.0f, amberMk);
            hax->add(s); st.hackables++;
        }
        // Junction boxes on the sidewalk (waist height).
        for (float jx : { cx - 60.0f, cx - 20.0f, cx + 20.0f, cx + 60.0f }) {
            const float jz = cz + 9.0f;
            const float jEmis[4] = { 0.3f, 1.4f, 0.6f, 0.8f };
            addBox(jx, g + 0.9f, jz, 0.6f, 0.9f, 0.4f, curbCol, jEmis, panelTex);
            HackableObject j; j.type = HackableType::JunctionBox; j.pos = { jx, g + 1.4f, jz };
            j.entity = addHackMarker(scene, device, jx, g + 2.2f, jz, greenMk);
            hax->add(j); st.hackables++;
        }
        // ATMs set into the south-row shopfronts.
        int idx = 0;
        for (float ax : { cx - 50.0f, cx + 10.0f, cx + 50.0f }) {
            const float az = cz - 12.5f;
            const float atmEmis[4] = { kMagenta.r, kMagenta.g, kMagenta.b, 1.6f };
            addBox(ax, g + 1.5f, az, 0.5f, 0.9f, 0.25f, darkCol, atmEmis, steelTex);
            HackableObject a; a.type = HackableType::ATM; a.pos = { ax, g + 1.5f, az };
            a.credits = 150 + ((idx * 137 + 91) % 6) * 60;   // deterministic 150..450
            a.entity = addHackMarker(scene, device, ax, g + 2.6f, az, magMk);
            hax->add(a); st.hackables++; ++idx;
        }
    }

    // ================= 5) PARKED VEHICLES (hackable) =================
    {
        const float carBody[4] = { 0.12f, 0.13f, 0.18f, 1.0f };
        const float greenMk[3] = { 0.2f, 1.8f, 0.6f };
        int vi = 0;
        for (float vx = cx - 70.0f; vx <= cx + 70.0f; vx += 28.0f) {
            const float vz = cz + (vi % 2 == 0 ? -6.5f : 6.5f);
            const float vy = g + 0.8f;
            addBox(vx, vy, vz, 2.2f, 0.7f, 1.1f, carBody, kNoEmis, steelTex, chromeMR);   // body
            addBox(vx, vy + 0.7f, vz, 1.3f, 0.45f, 1.0f, carBody, kNoEmis, darkTex, chromeMR); // cabin
            // Head/tail light strips (emissive).
            const float head[4] = { 1.4f, 1.5f, 1.8f, 1.6f };
            const float tail[4] = { 1.8f, 0.2f, 0.2f, 1.6f };
            addBox(vx + 2.1f, vy + 0.1f, vz, 0.12f, 0.14f, 0.9f, carBody, head, steelTex);
            addBox(vx - 2.1f, vy + 0.1f, vz, 0.12f, 0.14f, 0.9f, carBody, tail, steelTex);
            st.vehicles++;
            if (hax) {
                HackableObject v; v.type = HackableType::Vehicle; v.pos = { vx, vy + 0.4f, vz };
                v.entity = addHackMarker(scene, device, vx, vy + 1.6f, vz, greenMk);
                hax->add(v); st.hackables++;
            }
            ++vi;
        }
    }

    // ================= 6) LNG TANK landmark @ (-500, 525) =================
    {
        const float tx = cx + 100.0f, tz = cz + 25.0f;   // == (-500, 525) for the canon center
        float ts[3]; placeOnTerrain(tx, tz, ts);
        const float tgy = ts[1];
        const float R = 18.0f, legH = 12.0f, tcy = tgy + legH + R;
        const float tankCol[4] = { 0.88f, 0.91f, 0.97f, 1.0f };
        auto s = x3::prims::makeUVSphere(32, 48);
        Entity e;
        e.mesh = device.createMesh(s.verts.data(), (uint32_t)s.verts.size(),
                                   s.index.data(), (uint32_t)s.index.size());
        e.tex = steelTex;
        for (int i = 0; i < 4; ++i) e.baseColor[i] = tankCol[i];
        // scale + translate
        e.transform[0] = R; e.transform[5] = R; e.transform[10] = R;
        e.transform[12] = tx; e.transform[13] = tcy; e.transform[14] = tz;
        e.tag = (uint32_t)Tag::Static;
        scene.add(e);
        // Support legs + a red aircraft-warning beacon on top.
        for (int i = 0; i < 4; ++i) {
            const float a = (float)i / 4.0f * 6.2831853f + 0.78f;
            addBox(tx + std::cos(a) * R * 0.7f, tgy + legH * 0.5f, tz + std::sin(a) * R * 0.7f,
                   0.6f, legH * 0.5f, 0.6f, white, kNoEmis, steelTex);
        }
        const float beacon[4] = { 2.4f, 0.15f, 0.12f, 3.0f };
        addBox(tx, tcy + R + 1.0f, tz, 0.5f, 0.5f, 0.5f, darkCol, beacon, steelTex);
    }

    // ================= 7) CROWD (civilians on the drag) + NPC hackables =========
    if (crowd) {
        CrowdConfig cc;
        cc.count = 14; cc.centerX = cx; cc.centerZ = cz; cc.groundY = g + 0.2f;
        cc.radius = 70.0f; cc.walkSpeed = 1.1f; cc.scatterRadius = 20.0f;
        cc.emissive = 0.15f;   // faint neon rim so they read at dusk
        // Hangout points down both sidewalks.
        cc.points = { cx - 50.0f, cz + 9.0f, cx + 30.0f, cz + 9.0f,
                      cx - 20.0f, cz - 9.0f, cx + 55.0f, cz - 9.0f };
        crowd->build(cc, scene, device);
        // Register a few civilians as NPC scan targets (WD2 profiler).
        if (hax) {
            struct Prof { const char* name; const char* job; const char* detail; int credits; };
            const Prof profs[4] = {
                { "MARA VOSS",   "GRID TECH",     "OWES THE SYNDICATE",   40 },
                { "DEX OKORO",   "COURIER",       "OFF-GRID SINCE TUE",   25 },
                { "LIN CHEZ",    "STREET MEDIC",  "FLAGGED: SYMPATHIZER", 30 },
                { "RAV KOLE",    "DOCK FOREMAN",  "GAMBLING DEBT",        55 },
            };
            const float npcMk[3] = { 0.2f, 1.8f, 0.6f };
            for (int i = 0; i < 4 && (uint32_t)i < crowd->agentCount(); ++i) {
                const x3::phys::Vec3 ap = crowd->agent(i).pos;
                HackableObject n; n.type = HackableType::Npc; n.pos = { ap.x, g + 1.6f, ap.z };
                n.label = profs[i].name; n.occupation = profs[i].job;
                n.detail = profs[i].detail; n.credits = profs[i].credits;
                n.entity = addHackMarker(scene, device, ap.x, g + 2.4f, ap.z, npcMk);
                hax->add(n); st.hackables++;
            }
        }
    }

    x3::logInfo("buildNeonDistrict @ (" + std::to_string((int)cx) + "," + std::to_string((int)cz) +
                "): " + std::to_string(st.buildings) + " buildings, " +
                std::to_string(st.streetlights) + " lamps, " + std::to_string(st.vehicles) +
                " vehicles, " + std::to_string(st.signs) + " signs, " +
                std::to_string(st.propClutter) + " clutter props, " +
                std::to_string(st.hackables) + " hackables");
    return st;
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

    // ---- The NEON DISTRICT (feat/city-aaa Milestone 1): richer art-directed build +
    // hackables + crowd, on the same headless device. Additive to the W8-3 metropolis. ----
    {
        HackableRegistry hax;
        CrowdSystem crowd;
        NeonDistrictStats st = buildNeonDistrict(scene, device, *physics, &hax, &crowd);
        check(st.buildings >= 12 && st.streetlights > 0 && st.vehicles > 0 && st.signs > 0,
              "C8 neon district built (buildings/lamps/vehicles/signs)");
        // Every hackable type present (WD2 layer scattered through the district).
        bool allTypes = true;
        for (uint32_t t = 0; t < kHackableTypeCount; ++t)
            if (hax.countType((HackableType)t) == 0) allTypes = false;
        check(hax.count() == st.hackables && st.hackables > 0 && allTypes,
              "C9 hackable objects scattered (" + std::to_string(hax.count()) +
              ", every type present)");
        check(crowd.built() && crowd.agentCount() > 0, "C10 civilian crowd built on the drag");
    }

    physics->shutdown();
    x3::logInfo(std::string("city: ") + std::to_string(g_pass) + "/" +
                std::to_string(g_pass + g_fail) + " passed");
    return g_fail == 0;
}

} // namespace x3::game
