// EFLZ Act-2 open world — the ocean + undersea base + submarine combat. See
// ocean_base.h. Clean-room: X3Native's own Scene / mesh_prims + the engine interfaces
// + the EFLZ design (Tim's own Q3Engine ocean/seafloor/submarine modules as content
// reference). No RBDOOM / id Tech / Doom / Quake source. Graybox; mirrors world_regions.cpp.
#include "ocean_base.h"
#include "headless_device.h"
#include "mesh_prims.h"
#include "surface_library.h"   // W3-4: real PBR sets on the base hull/seafloor
#include "asset_root.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace x3::game {

namespace {
// Offshore undersea zone, absolute deep Y (decoupled from the land terrain).
constexpr float kBaseCx = 1100.0f, kBaseCz = -1350.0f;
constexpr float kSurfaceY  =   4.0f;   // ocean surface
constexpr float kSeafloorY = -80.0f;   // seafloor
constexpr float kBaseRadius = 80.0f;
constexpr uint32_t kLevels = 3;
constexpr float kLevelH = 6.0f;        // disc level height
} // namespace

void OceanBase::build(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics) {
    (void)physics;   // graybox undersea zone is visual-only this pass (no collision body)

    // W3-4 REALISM PASS: the base is no longer flat-tinted graybox — architectural
    // surfaces carry real surface-library PBR sets (ART_BIBLE §4: a wall without an
    // albedo TEXTURE is a red-line offense). Tints stay as MULTIPLIERS so the deep-
    // water palette (dark, desaturated, teal-leaning) rides on top of real material.
    // On a headless device the set loads may no-op (invalid handles) — the entity
    // then renders exactly like the old graybox, so --test-oceanbase is unchanged.
    SurfaceLibrary surf;
    surf.mount(assetRoot() + "/surface_library");
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
    { const float water[4] = { 0.10f, 0.28f, 0.42f, 0.55f };
      addBoxProp(kBaseCx, kSurfaceY, kBaseCz, 620.0f, 0.2f, 620.0f, water, nullptr); }

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

    // ---- Fill the plan + the (inert) submarine-combat model. ----
    m_plan.cx = kBaseCx; m_plan.cz = kBaseCz;
    m_plan.surfaceY = kSurfaceY; m_plan.baseDeckY = baseDeckY; m_plan.seafloorY = kSeafloorY;
    m_plan.radius = kBaseRadius; m_plan.levels = kLevels;
    // m_combat: defaults (hull 200, torpedo 40, depth-charge 30, engaged=false) — INERT.

    m_built = true;
    x3::logInfo("OceanBase::build complete — undersea base @ (1100,-1350): 3-level disc (r=80) + "
                "sub-dock + airlock + reactor; ocean surface + seafloor; 1 player + 3 enemy subs; "
                "sub-combat model inert; " + std::to_string((uint32_t)m_props.size()) + " graybox props");
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

    physics->shutdown();
    x3::logInfo(std::string("oceanbase: ") + std::to_string(g_pass) + "/" +
                std::to_string(g_pass + g_fail) + " passed");
    return g_fail == 0;
}

} // namespace x3::game
