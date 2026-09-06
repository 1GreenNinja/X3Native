// EFLZ Act-2 open world — the ocean + undersea base + submarine combat. See
// ocean_base.h. Clean-room: X3Native's own Scene / mesh_prims + the engine interfaces
// + the EFLZ design (Tim's own Q3Engine ocean/seafloor/submarine modules as content
// reference). No RBDOOM / id Tech / Doom / Quake source. Graybox; mirrors world_regions.cpp.
#include "ocean_base.h"
#include "headless_device.h"
#include "mesh_prims.h"
#include "surface_library.h"   // W3-4: real PBR sets on the base hull/seafloor
#include "asset_root.h"
#include "terrain.h"           // W10: kWorldSeaLevel (single source for the sea surface)

#include "engine/core/x3_log.h"
#include "engine/asset/IModelLoader.h"  // station: load abyssal_station.glb -> Scene entities
#include "engine/asset/IAssetSource.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace x3::game {

namespace {
// Offshore undersea zone, absolute deep Y (decoupled from the land terrain).
constexpr float kBaseCx = 1100.0f, kBaseCz = -1350.0f;
// W9 (terrain drama): the sea surface moved 4.0 -> -10.0. The facility plain is
// graded at Y=-2 and THE RIVER must flow DOWNHILL from the plain's east face
// into this sea (its last water level is -9.9); at +4 the ocean sat ABOVE the
// plain 1.7 km inland. -10 also puts the waterline ON the terrain basin's shore
// falloff (shore ring -6 stays a dry beach, the water starts where the bowl
// drops past -10, ~r700), which is what the basin was shaped for. The disc
// base itself (r80, seafloor -80) is untouched; depth ordering surface > deck
// > seafloor still holds (-10 > -62 > -80) and the patrol subs at -30 stay
// submerged.
// W10 (swimming): builds from terrain.h's kWorldSeaLevel so the rendered plane
// and the worldWaterLevelAt() query share ONE constant and can never drift.
constexpr float kSurfaceY  = kWorldSeaLevel;   // -10 (was 4.0 pre-river)
constexpr float kSeafloorY = -80.0f;   // seafloor
constexpr float kBaseRadius = 80.0f;
constexpr uint32_t kLevels = 3;
constexpr float kLevelH = 6.0f;        // disc level height

// ---- Abyssal Station — a HERO deep-sea landmark, a discoverable destination a
// good distance offshore from the sub-dock (the dock is at the base's +X edge; the
// station sits ~370 m along -Z, out in the dark deep). It rests ON the seabed
// (X3_WORLD_RULES Rule 4: the GLB's origin is already at its base — native minY≈0 —
// so its origin sits at kSeafloorY). The GLB is ~42x21x24 m native; kStationScale
// blows it up to a ~63 m wide, ~31 m tall structure that reads as a large station.
constexpr float kStationCx    = 1100.0f;    // straight offshore (dock is on +X)
constexpr float kStationCz    = -1720.0f;   // ~370 m past the base center, deeper in
constexpr float kStationScale = 1.5f;       // 42.25 m -> ~63 m wide; 20.93 m -> ~31 m tall
} // namespace

void OceanBase::build(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
                      SurfaceLibrary* sharedSurf, bool surfaceSlab) {
    (void)physics;   // graybox undersea zone is visual-only this pass (no collision body)

    // W3-4 REALISM PASS: the base is no longer flat-tinted graybox — architectural
    // surfaces carry real surface-library PBR sets (ART_BIBLE §4: a wall without an
    // albedo TEXTURE is a red-line offense). Tints stay as MULTIPLIERS so the deep-
    // water palette (dark, desaturated, teal-leaning) rides on top of real material.
    // On a headless device the set loads may no-op (invalid handles) — the entity
    // then renders exactly like the old graybox, so --test-oceanbase is unchanged.
    SurfaceLibrary localSurf;                         // W8-3: prefer the streamer's
    SurfaceLibrary& surf = sharedSurf ? *sharedSurf : localSurf;   // shared PBR cache
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
        e.baseColor[0]=col[0]; e.baseColor[1]=col[1]; e.baseColor[2]=col[2]; e.baseColor[3]=col[3];
        if (emiss) { e.emissive[0]=emiss[0]; e.emissive[1]=emiss[1]; e.emissive[2]=emiss[2]; e.emissive[3]=emiss[3]; }
        e.tag = (uint32_t)Tag::Prop;
        m_props.push_back(scene.add(e));
    };

    const float baseDeckY = kSeafloorY + (float)kLevels * kLevelH;   // top deck Y

    // ---- Ocean surface plane (large translucent blue slab at the surface). ----
    // W3-4: widened 220 -> 620 half-extent — from sub depth the old plane's hard
    // box EDGE hung in frame as a floating monolith; the surface must read endless.
    // W9: widened again 620 -> 760 so the plane covers the whole -10 waterline
    // ring of the terrain basin (~r700) and the river mouth's final ribbon
    // segment (ending (900,-1120) at Y=-9.9, 0.1 m proud — no coplanar fight).
    // ONE WATER: skipped when the host's engine water pass owns the sea (see
    // the header) — the slab was only ever visible within this region's load
    // radius anyway, so nothing distant is lost.
    if (surfaceSlab) {
      const float water[4] = { 0.10f, 0.28f, 0.42f, 0.55f };
      addBoxProp(kBaseCx, kSurfaceY, kBaseCz, 760.0f, 0.2f, 760.0f, water, nullptr); }

    // ---- Seafloor pad — rough dark stone, tiled coarse (8 m repeats read as rock
    // shelf at sub scale, not bathroom tile). Deep-sediment tint over the albedo. ----
    { const float floorCol[4] = { 0.42f, 0.42f, 0.38f, 1.0f };
      addBoxProp(kBaseCx, kSeafloorY - 0.5f, kBaseCz, 150.0f, 0.5f, 150.0f, floorCol, nullptr,
                 &set("sr_concrete_01"), 0.125f); }

    // ---- The 3-level undersea disc base — riveted steel hull plates (the AD-3
    // star set), tinted to deep-steel so light falloff owns the read. 2 m repeats. ----
    { const float hull[4] = { 0.52f, 0.56f, 0.62f, 1.0f };
      for (uint32_t l = 0; l < kLevels; ++l) {
          const float half = kBaseRadius * (1.0f - (float)l * 0.12f);
          const float cy   = kSeafloorY + ((float)l + 0.5f) * kLevelH;
          addBoxProp(kBaseCx, cy, kBaseCz, half, kLevelH * 0.5f, half, hull, nullptr,
                     &set("mw_metal_trim_b"), 0.5f);
          // Viewport band: a thin warm-lit window strip just under each level's top
          // edge on all four faces (one thin emissive frame box per level) — the
          // bible's "instruments glow": inhabited light leaking out, not floodlights.
          const float bandEm[4] = { 0.95f, 0.72f, 0.38f, 1.25f };   // warm, not white-blown
          const float bandCol[4] = { 0.10f, 0.10f, 0.10f, 1.0f };
          addBoxProp(kBaseCx, cy + kLevelH * 0.28f, kBaseCz,
                     half + 0.15f, 0.35f, half + 0.15f, bandCol, bandEm);
      } }

    // ---- Central reactor (glowing) through the core — its crown rises 4 m ABOVE
    // the top deck so the glow reads as the base's landmark from approach range
    // (fully inside the hull it was an invisible practical). ----
    { const float reactorCol[4] = { 0.30f, 0.20f, 0.10f, 1.0f };
      const float reactorEm[4]  = { 0.95f, 0.45f, 0.10f, 2.2f };
      const float coreH = ((float)kLevels * kLevelH + 4.0f) * 0.5f;
      addBoxProp(kBaseCx, kSeafloorY + coreH, kBaseCz,
                 6.0f, coreH, 6.0f, reactorCol, reactorEm,
                 &set("sr_metal_lattice"), 1.0f); }

    // ---- Sub-docking bay (a portal box at the base edge) + airlock chamber —
    // industrial panel steel; the dock mouth gets a hazard-amber edge strip so a
    // sub pilot reads the entry point from range (the zone's ONE accent). ----
    { const float dockCol[4] = { 0.48f, 0.50f, 0.54f, 1.0f };
      addBoxProp(kBaseCx + kBaseRadius, baseDeckY - kLevelH, kBaseCz, 12.0f, 5.0f, 16.0f, dockCol, nullptr,
                 &set("mw_metal_panels_a"), 0.5f);   // dock bay
      addBoxProp(kBaseCx + kBaseRadius - 14.0f, baseDeckY - kLevelH, kBaseCz, 4.0f, 4.0f, 4.0f, dockCol, nullptr,
                 &set("mw_metal_grate"), 0.5f);      // airlock
      // Dock-mouth entry marker: THIN amber frame strips (top bar + two posts)
      // outlining the mouth on the +X face — a readable entry cue, not a billboard.
      const float amberEm[4] = { 1.0f, 0.62f, 0.10f, 1.8f };
      const float amberCol[4] = { 0.12f, 0.10f, 0.06f, 1.0f };
      const float mx = kBaseCx + kBaseRadius + 12.05f;   // just proud of the dock +X face
      const float my = baseDeckY - kLevelH;
      addBoxProp(mx, my + 4.6f, kBaseCz, 0.25f, 0.30f, 15.5f, amberCol, amberEm);  // top bar
      addBoxProp(mx, my, kBaseCz - 15.6f, 0.25f, 4.8f, 0.30f, amberCol, amberEm);  // post -Z
      addBoxProp(mx, my, kBaseCz + 15.6f, 0.25f, 4.8f, 0.30f, amberCol, amberEm); } // post +Z
    m_plan.hasSubDock = true; m_plan.hasAirlock = true; m_plan.hasReactor = true;

    // ---- A player submarine at the dock + 3 enemy subs patrolling mid-water. ----
    { const float subCol[4]  = { 0.35f, 0.48f, 0.44f, 1.0f };   // player sub (teal-steel)
      addBoxProp(kBaseCx + kBaseRadius + 16.0f, baseDeckY - kLevelH, kBaseCz, 6.0f, 2.0f, 2.0f, subCol, nullptr,
                 &set("sr_metal_b"), 1.0f);
      m_playerSub = true; }
    { const float enemyCol[4] = { 0.40f, 0.22f, 0.20f, 1.0f };  // enemy subs (dark red-steel)
      const float ez[3] = { kBaseCz - 60.0f, kBaseCz + 40.0f, kBaseCz - 20.0f };
      const float ex[3] = { kBaseCx - 50.0f, kBaseCx + 30.0f, kBaseCx + 70.0f };
      for (int i = 0; i < 3; ++i)
          addBoxProp(ex[i], -30.0f, ez[i], 7.0f, 2.5f, 2.5f, enemyCol, nullptr,
                     &set("sr_metal_b"), 1.0f);
      m_combat.enemySubs = 3; }

    // ========================================================================
    // ABYSSAL STATION — the hero deep-sea landmark (a destination out in the deep).
    // ========================================================================
    // A dedicated seabed pad UNDER the station so it rests on real ground far from
    // the base's own floor pad (mirrors the base seafloor-pad pattern above — same
    // rough dark stone, coarse tiling). Without it the station would hang over the
    // fog void; with it the landmark reads as a genuine seafloor structure.
    { const float floorCol[4] = { 0.42f, 0.42f, 0.38f, 1.0f };
      addBoxProp(kStationCx, kSeafloorY - 0.5f, kStationCz, 120.0f, 0.5f, 120.0f, floorCol, nullptr,
                 &set("sr_concrete_01"), 0.125f); }

    m_stationX = kStationCx; m_stationY = kSeafloorY; m_stationZ = kStationCz;

    // Load the station GLB and add each primitive as a Scene entity (rides
    // scene.render() in every host — no per-host draw plumbing). Its blue-emissive
    // windows/panels GLOW because each entity carries mrTex + emissiveTex, which
    // routes Scene::render through drawMeshPBR with the emissive map honored
    // (X3_WORLD_RULES Rule 5: emissiveTex is only honored on the PBR route). The
    // loader/model are LOCAL — once makeDrawables() has minted the device mesh/
    // texture handles they are owned by the device and outlive this scope (the
    // loader only frees on an explicit unload(), which we never call).
    {
        std::unique_ptr<x3::asset::IAssetSource> assets(x3::asset::createAssetSource());
        if (assets->mountDir(convertedGlbRoot(), 0)) {
            std::unique_ptr<x3::asset::IModelLoader> loader(
                x3::asset::createModelLoader(&device, assets.get()));
            x3::asset::Model model = loader->load("Undersea/abyssal_station.glb");
            if (model.ok) {
                std::vector<x3::asset::ModelDrawable> draws = x3::asset::makeDrawables(model);
                // object transform = T(station) * S(scale) (column-major). The GLB's
                // baked node transforms (Y-up correction + part placement) ride on top.
                const float S = kStationScale;
                const float obj[16] = { S,0,0,0,  0,S,0,0,  0,0,S,0,
                                        kStationCx, kSeafloorY, kStationCz, 1.0f };
                uint32_t emisPrims = 0;
                for (const auto& d : draws) {
                    if (!d.meshId) continue;   // headless / un-uploaded primitive
                    float fin[16];
                    x3::asset::mulMat4(obj, d.nodeTransform, fin);
                    Entity e;
                    e.mesh        = x3::rhi::MeshHandle{ d.meshId };
                    e.tex         = x3::rhi::TextureHandle{ d.baseColorTexId };
                    e.normalTex   = x3::rhi::TextureHandle{ d.normalTexId };
                    e.mrTex       = x3::rhi::TextureHandle{ d.mrTexId };
                    e.emissiveTex = x3::rhi::TextureHandle{ d.emissiveTexId };
                    for (int i = 0; i < 4; ++i) e.baseColor[i] = d.baseColorFactor[i];
                    // Material emissive, gated per-texel by the emissive map in the
                    // shader: the blue window/panel texels glow, the black hull stays
                    // dark. Strength 1.3 = a touch of HDR headroom (the bible's window
                    // bands use 1.25) so it reads as POWERED in the murk — NOT an
                    // over-unity crutch (honest emissive over a real light).
                    const bool matEmis = d.emissiveTexId != 0 ||
                        d.emissiveFactor[0] > 0.001f || d.emissiveFactor[1] > 0.001f ||
                        d.emissiveFactor[2] > 0.001f;
                    if (matEmis) {
                        e.emissive[0] = d.emissiveFactor[0];
                        e.emissive[1] = d.emissiveFactor[1];
                        e.emissive[2] = d.emissiveFactor[2];
                        e.emissive[3] = 1.3f;
                        ++emisPrims;
                    }
                    e.alphaBlend = d.alphaBlend;
                    for (int i = 0; i < 16; ++i) e.transform[i] = fin[i];
                    e.tag = (uint32_t)Tag::Prop;
                    m_props.push_back(scene.add(e));
                }
                m_stationPlaced = !draws.empty();
                x3::logInfo("OceanBase: abyssal_station.glb placed @ (" +
                            std::to_string((int)kStationCx) + "," + std::to_string((int)kSeafloorY) +
                            "," + std::to_string((int)kStationCz) + ") scale " +
                            std::to_string(kStationScale) + " — " + std::to_string(draws.size()) +
                            " prims (" + std::to_string(emisPrims) + " emissive)");
            }
        }
    }
    if (!m_stationPlaced) {
        // Per-piece FALLBACK: a station-sized graybox block with a cool blue glow so
        // the landmark (and its "powered" read) survive even if the GLB fails to load.
        const float boxCol[4] = { 0.30f, 0.34f, 0.40f, 1.0f };
        const float boxEm[4]  = { 0.22f, 0.48f, 0.90f, 0.9f };   // cool blue "powered"
        const float hw = 21.0f * kStationScale, hh = 10.5f * kStationScale, hd = 12.0f * kStationScale;
        addBoxProp(kStationCx, kSeafloorY + hh, kStationCz, hw, hh, hd, boxCol, boxEm);
        x3::logWarn("OceanBase: abyssal_station.glb failed to load — graybox landmark placed");
    }

    // Cool key + rim lights so the hull CATCHES light (not a flat silhouette),
    // sized to the station, moderate range — kept inside the ocean's deep-water
    // fog/diver mood (pre-multiplied color = linear RGB * intensity; blue-dominant
    // cool key, dim cool rim from below-front for a 3D read). The host feeds these
    // to setPointLights (EnvArtSystem::lightFixtures convention).
    {
        const float top = kSeafloorY + 20.93f * kStationScale;   // ~ -48.6 (station crown)
        x3::rhi::PointLight key{};       // cool blue-white key, above-front
        key.pos[0] = kStationCx + 3.0f; key.pos[1] = top + 14.0f; key.pos[2] = kStationCz + 20.0f;
        key.range  = 155.0f;
        key.color[0] = 3.2f; key.color[1] = 4.4f; key.color[2] = 6.2f;
        m_stationLights.push_back(key);

        x3::rhi::PointLight rim{};        // dim cool rim/up-light, below-front
        rim.pos[0] = kStationCx + 2.0f; rim.pos[1] = kSeafloorY + 6.0f; rim.pos[2] = kStationCz - 14.0f;
        rim.range  = 95.0f;
        rim.color[0] = 0.9f; rim.color[1] = 1.7f; rim.color[2] = 2.6f;
        m_stationLights.push_back(rim);
    }

    // ---- Fill the plan + the (inert) submarine-combat model. ----
    m_plan.cx = kBaseCx; m_plan.cz = kBaseCz;
    m_plan.surfaceY = kSurfaceY; m_plan.baseDeckY = baseDeckY; m_plan.seafloorY = kSeafloorY;
    m_plan.radius = kBaseRadius; m_plan.levels = kLevels;
    // m_combat: defaults (hull 200, torpedo 40, depth-charge 30, engaged=false) — INERT.

    m_built = true;
    x3::logInfo("OceanBase::build complete — undersea base @ (1100,-1350): 3-level disc (r=80) + "
                "sub-dock + airlock + reactor; ocean surface + seafloor; 1 player + 3 enemy subs; "
                "sub-combat model inert; abyssal station landmark " +
                std::string(m_stationPlaced ? "(real GLB)" : "(graybox fallback)") + " @ (1100,-80,-1720) + " +
                std::to_string((uint32_t)m_stationLights.size()) + " station lights; " +
                std::to_string((uint32_t)m_props.size()) + " props");
}

// ===========================================================================
// Headless self-test (--test-oceanbase).
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const std::string& name) {
    if (cond) { ++g_pass; x3::logInfo("[oceanbase-test] PASS " + name); }
    else      { ++g_fail; x3::logError("[oceanbase-test] FAIL " + name); }
}
using HeadlessDevice = x3::game::HeadlessRenderDevice;

} // namespace

bool runOceanBaseSelfTest() {
    g_pass = g_fail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();
    HeadlessDevice device;
    Scene scene;

    OceanBase ob;
    ob.build(scene, device, *physics);

    check(ob.built(), "O0 ocean base built");

    // ---- Base offshore, 3 levels, depth profile ordered (surface > deck > seafloor). ----
    {
        const OceanBasePlan& p = ob.plan();
        bool ok = std::abs(p.cx - 1100.0f) < 1.0f && std::abs(p.cz - (-1350.0f)) < 1.0f &&
                  p.levels == 3 && p.radius == 80.0f &&
                  p.surfaceY > p.baseDeckY && p.baseDeckY > p.seafloorY;
        check(ok, "O1 base offshore (1100,-1350), 3 levels, r=80, depth profile ordered");
    }

    // ---- Sub-dock + airlock + reactor present. ----
    {
        const OceanBasePlan& p = ob.plan();
        check(p.hasSubDock && p.hasAirlock && p.hasReactor,
              "O2 sub-dock + airlock + reactor present");
    }

    // ---- A player sub + 3 enemy subs placed. ----
    check(ob.playerSubPresent() && ob.enemySubCount() == 3,
          "O3 player submarine + 3 enemy subs placed");

    // ---- Submarine-combat model INERT at load; arms via engage(). ----
    {
        const SubCombat& c = ob.combat();
        bool inert = !c.engaged && c.playerHull == c.playerHullMax && c.playerHull == 200 &&
                     c.torpedoDamage == 40 && c.depthChargeDamage == 30 && c.enemySubs == 3;
        ob.engage();
        check(inert && ob.combat().engaged,
              "O4 sub-combat inert at load (hull 200, torpedo 40, dc 30) -> engage() arms it");
    }

    // ---- Graybox props placed. ----
    check(ob.propCount() > 0, "O5 graybox props placed (total " + std::to_string(ob.propCount()) + ")");

    // ---- Abyssal Station landmark placed + lit. On the HEADLESS device the GLB
    // primitives get non-zero fake mesh ids (so stationPlaced() is true even without
    // Vulkan), and the key/rim lights are populated regardless. Assert both a distinct
    // offshore position and that the station carries its cool lights. ----
    {
        float sx, sy, sz; ob.stationPos(sx, sy, sz);
        const OceanBasePlan& p = ob.plan();
        const float dx = sx - p.cx, dz = sz - p.cz;
        const bool farOff = std::sqrt(dx * dx + dz * dz) > 200.0f;   // a real destination, not on top of the base
        const bool onFloor = std::abs(sy - p.seafloorY) < 0.01f;     // resting on the seabed
        check(farOff && onFloor && ob.stationLights().size() >= 2,
              "O6 abyssal station landmark offshore on the seabed (" +
              std::to_string((int)sx) + "," + std::to_string((int)sy) + "," + std::to_string((int)sz) +
              ") + " + std::to_string(ob.stationLights().size()) + " cool lights");
    }

    physics->shutdown();
    x3::logInfo(std::string("oceanbase: ") + std::to_string(g_pass) + "/" +
                std::to_string(g_pass + g_fail) + " passed");
    return g_fail == 0;
}

} // namespace x3::game
