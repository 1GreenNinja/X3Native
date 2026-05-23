// EFLZ Act 2 — alien-planet open-world surface host + L8/L9. See act2_world.h.
//
// Clean-room: built ONLY from X3Native's own Scene / monster / trigger / terrain /
// mesh_prims systems + the engine interfaces + the EFLZ design docs (Tim's own IP).
// No RBDOOM / id Tech / Doom / Quake — or any other game-engine — source consulted.
// CONTENT/LEVEL-SCRIPT ONLY: no renderer or core-engine changes. Mirrors
// spire_mid.cpp's authoring style (build/tick/onTrigger/draw + a headless self-test).
#include "act2_world.h"
#include "asset_root.h"
#include "headless_device.h"
#include "mesh_prims.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace x3::game {

namespace {

// Terrain residency-ring radius the surface comes up with. Small at build (cheap +
// headless-friendly); the interactive host bumps it via terrain().setRadius().
constexpr int kAct2StreamRadius = 2;

// World position sitting ON the canonical alien surface at (x,z), plus a body/eye
// offset. Uses the PURE placement sampler (no tiles needed) so it is identical to
// what the streamer generates underfoot — same approach env_art/Spire anchoring uses.
x3::phys::Vec3 surfaceAt(float x, float z, float yOff) {
    float p[3];
    placeOnTerrain(x, z, p);   // {x, surfaceY, z}
    return x3::phys::Vec3{ p[0], p[1] + yOff, p[2] };
}

// L8 PURSUIT DRONE: a ranged drone hunting the player out of Lab Zero. Built on the
// BlueSynth (Combat Drone) ranged-flier profile, tinted hostile red so the exit
// tunnel reads as a pursuit gauntlet. EXISTING roster type (no new enemy code).
MonsterSystem::Tuning pursuitDroneTuning() {
    MonsterSystem::Tuning t = tuningFor(EnemyType::BlueSynth);
    t.tint[0] = 1.00f; t.tint[1] = 0.25f; t.tint[2] = 0.18f; t.tint[3] = 1.0f; // pursuit red
    return t;
}

// L8 INFECTED SOLDIER: a melee infected on the DominionTrooper (Guard) profile,
// tinted sickly green. EXISTING roster type.
MonsterSystem::Tuning infectedSoldierTuning() {
    MonsterSystem::Tuning t = tuningFor(EnemyType::DominionTrooper);
    t.tint[0] = 0.45f; t.tint[1] = 0.78f; t.tint[2] = 0.40f; t.tint[3] = 1.0f; // infected green
    return t;
}

// A surviving COMPANION marker at the Emergence Point. Humanoid (DominionTrooper
// profile) but STATIONARY (chaseSpeed 0); turned ALLIED after spawn so it never
// fights — a friendly NPC marker for the awe beat. `tint` distinguishes the cast.
MonsterSystem::Tuning companionTuning(float r, float g, float b) {
    MonsterSystem::Tuning t = tuningFor(EnemyType::DominionTrooper);
    t.chaseSpeed = 0.0f;   // stand at the reveal
    t.tint[0] = r; t.tint[1] = g; t.tint[2] = b; t.tint[3] = 1.0f;
    return t;
}

// A neutral L9 FAUNA placeholder: a darting creature (Verthani profile), STATIONARY
// + turned ALLIED after spawn so it is non-hostile (a wildlife marker).
MonsterSystem::Tuning faunaTuning() {
    MonsterSystem::Tuning t = tuningFor(EnemyType::Verthani);
    t.chaseSpeed = 0.0f;
    t.tint[0] = 0.80f; t.tint[1] = 0.72f; t.tint[2] = 0.45f; t.tint[3] = 1.0f; // sand-toned
    return t;
}

} // namespace

void Act2World::build(Scene& scene, x3::rhi::IRenderDevice& device,
                      x3::phys::IPhysicsWorld& physics, TriggerSystem& triggers,
                      x3::jobs::IJobSystem* jobs, std::string_view modelDir) {
    m_modelDir = std::string(modelDir);

    // ---- The Act-2 region anchor (far from the Act-1 Spire) + the L8/L9 layout
    // along +X: lab exit -> 100 m tunnel -> Emergence Point -> desert edge. ----
    const float X0 = 1024.0f, Z0 = 1024.0f; // region anchor
    const float Xe = X0 + 130.0f;           // Emergence Point center (open reveal)
    const float Xt = X0 + 165.0f;           // L8 -> L9 threshold (emergence edge)
    const float X9 = X0 + 200.0f;           // L9 desert spawn
    const float Xh = X0 + 360.0f;           // L9 hazard zone center

    // ===================================================================
    // AREA FRAMEWORK — every Act-2 level L8..L20 gets its canon name/biome/objective
    // so later lanes slot in; only L8/L9 carry content this pass.
    // ===================================================================
    auto setBase = [&](Act2Level lvl, const char* name, const char* biome, const char* obj) {
        Act2AreaPlan& p = m_plan[(uint32_t)lvl - kAct2FirstLevel];
        p.level = lvl; p.name = name; p.biome = biome; p.objective = obj;
    };
    setBase(Act2Level::L8_SurfaceEmergence, "Surface Emergence",
            "Lab-Zero exit tunnel -> alien surface (binary sun, purple sky)",
            "Escape the collapsing facility and reach the Emergence Point");
    setBase(Act2Level::L9_CrystallineDesertEdge, "Crystalline Desert Edge",
            "Crystal desert (singing crystals; heat + sandstorm)",
            "Survive the desert edge and explore the crystal formations");
    setBase(Act2Level::L10_CrystallineDesertDepths, "Crystalline Desert Depths",
            "Crystal caves", "Find the Salvari refugees hiding in the crystal caves");
    setBase(Act2Level::L11_SalvariCamp, "Salvari Camp (Refugee Haven)",
            "Salvari refugee outpost (hub)", "Reach Refugee Haven and meet the Salvari");
    setBase(Act2Level::L12_AdvancedCaveSystem, "Advanced Cave System",
            "Ancient alien ruins (Crystal Heart)", "Reach the Crystal Heart chamber");
    setBase(Act2Level::L13_ToxicSwamplandsEdge, "Toxic Swamplands Edge",
            "Failed-terraform mutation swamp (poison)", "Cross the toxic swamplands");
    setBase(Act2Level::L14_ResearchStation, "Research Station",
            "Swamp research station", "Search the abandoned research station");
    setBase(Act2Level::L15_TreeCities, "Tree Cities",
            "Canopy city (vertical traversal)", "Climb to the tree cities and contact the resistance");
    setBase(Act2Level::L16_RuinedMetropolisOutskirts, "Ruined Metropolis Outskirts",
            "Fallen alien city (scavengers)", "Enter the ruined metropolis");
    setBase(Act2Level::L17_Downtown, "Downtown",
            "Skyscraper core (Central AI)", "Reach downtown and access the Central AI");
    setBase(Act2Level::L18_UndergroundResistance, "Underground Resistance",
            "Multi-species alliance HQ", "Join the underground resistance");
    setBase(Act2Level::L19_SpaceportApproach, "Spaceport Approach",
            "Spaceport exterior (vehicle combat)", "Fight through the spaceport approach");
    setBase(Act2Level::L20_TheSpaceport, "The Spaceport",
            "Spaceport interior (Act-2 finale)", "Capture a ship and escape Keth'zar Prime");

    // ===================================================================
    // L8 — SURFACE EMERGENCE. The 100 m lab-exit tunnel gauntlet (3 Infected melee
    // woven among 5 Pursuit Drones ranged) opening onto The Emergence Point: an
    // enemy-free safe-zone reveal (the awe beat) where 4 surviving companions stand
    // as allied markers.
    // ===================================================================
    {
        Act2AreaPlan& p = m_plan[(uint32_t)Act2Level::L8_SurfaceEmergence - kAct2FirstLevel];
        p.implemented = true;
        p.spawn       = surfaceAt(X0, Z0, 0.05f);  // the lab exit (player entry)
        p.footprintX  = 100.0f;                    // the 100 m exit tunnel
        p.footprintZ  = 12.0f;                     // corridor width
        p.visibility  = 500.0f;                    // Emergence Point open reveal (500 m)

        // Lab-exit gauntlet down the tunnel (between exit x=X0 and mouth x=X0+100),
        // off the center spine so the player isn't point-blank at spawn.
        const float infX[3] = { X0 + 25, X0 + 48, X0 + 72 };
        const float infZ[3] = { Z0 - 2.5f, Z0 + 3.0f, Z0 - 1.5f };
        for (int i = 0; i < 3; ++i)
            m_l8Enemies.spawn(scene, device, physics, m_modelDir,
                              surfaceAt(infX[i], infZ[i], kAct2EnemyYOff), infectedSoldierTuning());
        const float drnX[5] = { X0 + 15, X0 + 35, X0 + 55, X0 + 80, X0 + 95 };
        const float drnZ[5] = { Z0 + 2.5f, Z0 - 3.0f, Z0 + 2.0f, Z0 - 2.0f, Z0 + 1.5f };
        for (int i = 0; i < 5; ++i)
            m_l8Enemies.spawn(scene, device, physics, m_modelDir,
                              surfaceAt(drnX[i], drnZ[i], kAct2EnemyYOff), pursuitDroneTuning());
        p.meleeCount  = 3;   // 3 Infected Soldiers (Guard archetype)
        p.rangedCount = 5;   // 5 Pursuit Drones (Drone archetype)

        // The Emergence Point safe-zone reveal: the surviving companions as allied
        // markers (names/roles per EFLZ canon; placed unconditionally as the awe-beat
        // cast — the full game gates the cast on the Act-1 timeline).
        struct Comp { float r, g, b, dx, dz; };
        const Comp comps[4] = {
            { 0.40f, 0.65f, 1.00f, -3.0f, -3.0f }, // Sarah  (hacker)    — cool blue
            { 0.95f, 0.55f, 0.70f,  3.0f, -1.0f }, // Aria   (medic)     — rose
            { 0.85f, 0.80f, 0.30f,  1.0f,  3.0f }, // Keisha (security)  — amber
            { 0.55f, 0.90f, 0.75f, -2.0f,  1.5f }, // Emily  (researcher)— teal
        };
        for (const Comp& c : comps) {
            uint32_t idx = m_companions.spawn(scene, device, physics, m_modelDir,
                                              surfaceAt(Xe + c.dx, Z0 + c.dz, kAct2EnemyYOff),
                                              companionTuning(c.r, c.g, c.b));
            m_companions.at(idx).convertToAllied();  // friendly NPC marker (never fights)
        }
        p.companionCount = m_companions.count();

        m_l8EmergenceExit = surfaceAt(Xt, Z0, 0.05f);   // the L8 -> L9 threshold
    }

    // ===================================================================
    // L9 — CRYSTALLINE DESERT EDGE. Open desert biome: singing-crystal formation
    // props (emissive Scene entities), a few neutral fauna placeholders (allied),
    // and a heat/sandstorm hazard zone that is INERT until the player enters it.
    // ===================================================================
    {
        Act2AreaPlan& p = m_plan[(uint32_t)Act2Level::L9_CrystallineDesertEdge - kAct2FirstLevel];
        p.implemented = true;
        p.spawn       = surfaceAt(X9, Z0, 0.05f);
        p.footprintX  = 400.0f;   // open desert (footprint chosen — docs leave L9 unsized)
        p.footprintZ  = 400.0f;
        p.visibility  = 0.0f;
        m_l9Spawn     = p.spawn;

        // Singing-crystal formations: tall thin violet shards as emissive Scene props
        // (purely visual, like env_art props over the graybox — no physics body).
        const float cx[kAct2CrystalCount] = { X9 + 40, X9 + 85, X9 + 140, X9 + 185, X9 + 245, X9 + 300 };
        const float cz[kAct2CrystalCount] = { Z0 - 30, Z0 + 50, Z0 - 22,  Z0 + 62,  Z0 - 40,  Z0 + 24  };
        const float ch[kAct2CrystalCount] = { 6.0f,    4.5f,    7.5f,     5.0f,     8.0f,     5.5f     };
        for (uint32_t i = 0; i < kAct2CrystalCount; ++i) {
            const float halfH = ch[i] * 0.5f;
            float base[3];
            placeOnTerrain(cx[i], cz[i], base);
            x3::prims::PrimMesh m = x3::prims::makeBox(0.9f, halfH, 0.9f,
                                                       base[0], base[1] + halfH, base[2]);
            Entity e;
            e.mesh = device.createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                       m.index.data(), (uint32_t)m.index.size());
            e.baseColor[0] = 0.62f; e.baseColor[1] = 0.40f; e.baseColor[2] = 0.95f; e.baseColor[3] = 1.0f;
            // Emissive violet glow (the "singing crystals" read as HDR bloom sources).
            e.emissive[0] = 0.45f; e.emissive[1] = 0.20f; e.emissive[2] = 0.95f; e.emissive[3] = 2.0f;
            e.tag = (uint32_t)Tag::Prop;
            m_crystals.push_back(scene.add(e));
        }
        p.crystalCount = (uint32_t)m_crystals.size();

        // Neutral fauna placeholders (allied wildlife markers) near the desert entry.
        const float fx[3] = { X9 + 60, X9 + 120, X9 + 200 };
        const float fz[3] = { Z0 + 12, Z0 - 25,  Z0 + 34  };
        for (int i = 0; i < 3; ++i) {
            uint32_t idx = m_fauna.spawn(scene, device, physics, m_modelDir,
                                         surfaceAt(fx[i], fz[i], kAct2EnemyYOff), faunaTuning());
            m_fauna.at(idx).convertToAllied();
        }
        p.faunaCount = m_fauna.count();

        // Environmental-hazard zone (heat / sandstorm): a desert AABB, INERT until the
        // player enters. Tall enough to cover the rolling surface within its footprint.
        float c0[3];
        placeOnTerrain(Xh, Z0, c0);
        const float hxz = 90.0f;
        m_hazard.min    = x3::phys::Vec3{ Xh - hxz, c0[1] - 10.0f, Z0 - hxz };
        m_hazard.max    = x3::phys::Vec3{ Xh + hxz, c0[1] + 60.0f, Z0 + hxz };
        m_hazard.active = false; m_hazard.exposure = 0.0f;
        p.hasHazard     = true;
    }

    // ---- Triggers (host's shared TriggerSystem; ids forwarded to onTrigger). ----
    {
        float ey[3]; placeOnTerrain(Xe, Z0, ey);  // Emergence Point surface
        float ty[3]; placeOnTerrain(Xt, Z0, ty);  // threshold surface
        // Emergence-Point reveal (awe beat).
        triggers.add(x3::phys::Vec3{ Xe - 14.0f, ey[1] - 3.0f, Z0 - 14.0f },
                     x3::phys::Vec3{ Xe + 14.0f, ey[1] + 8.0f, Z0 + 14.0f },
                     (uint32_t)Act2Trigger::L8EmergencePoint, true);
        // L8 -> L9 transition at the Emergence-Point edge.
        triggers.add(x3::phys::Vec3{ Xt - 6.0f, ty[1] - 3.0f, Z0 - 14.0f },
                     x3::phys::Vec3{ Xt + 6.0f, ty[1] + 8.0f, Z0 + 14.0f },
                     (uint32_t)Act2Trigger::L8toL9Transition, true);
        // L9 hazard-zone enter trigger (matches the hazard AABB footprint).
        triggers.add(m_hazard.min, m_hazard.max, (uint32_t)Act2Trigger::L9HazardZone, true);
    }

    // ---- Stand up the alien surface: the streamed procedural terrain world (jobs
    // may be null -> synchronous generation, headless self-test) + drain the focus
    // neighbourhood so the player has ground immediately. ----
    m_cfg = worldTerrainConfig();
    const float fX = m_plan[(uint32_t)Act2Level::L8_SurfaceEmergence - kAct2FirstLevel].spawn.x;
    const float fZ = m_plan[(uint32_t)Act2Level::L8_SurfaceEmergence - kAct2FirstLevel].spawn.z;
    m_terrain.init(scene, device, physics, jobs, m_cfg, fX, fZ, kAct2StreamRadius);
    m_terrainUp = true;
    for (int i = 0; i < 12 && !m_terrain.focusTileResident(fX, fZ); ++i)
        m_terrain.update(scene, device, physics, fX, fZ);

    // ---- Alien sky: a binary-sun / purple-atmosphere look approximated through the
    // existing analytic sky (violet sun color + thick haze). No new engine tech. ----
    m_sky = x3::rhi::IRenderDevice::SkyParams{};
    m_sky.enabled      = true;
    m_sky.sunDir[0]    = 0.45f; m_sky.sunDir[1] = 0.82f; m_sky.sunDir[2] = 0.35f;
    m_sky.sunColor[0]  = 0.78f; m_sky.sunColor[1] = 0.62f; m_sky.sunColor[2] = 1.00f; // violet (blue>red)
    m_sky.sunIntensity = 1.15f;
    m_sky.haze         = 0.82f;  // thick purple atmosphere
    m_sky.exposure     = 1.05f;

    m_built = true;
    x3::logInfo("Act2World::build complete — L8 SURFACE EMERGENCE (100 m exit tunnel: "
                "5 Pursuit Drones + 3 Infected -> Emergence Point safe zone + 4 companion "
                "markers) + L9 CRYSTALLINE DESERT EDGE (6 crystal props, 3 fauna, heat/"
                "sandstorm hazard zone [inert]); alien sky + streamed terrain stood up");
}

void Act2World::tick(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                     const x3::phys::Vec3& eye, const x3::phys::Vec3& playerPos,
                     IDamageSink* player, const AttackFxFn& attackFx) {
    if (!m_built) return;

    // L8 gauntlet attacks only while the player is alive (matches Level1Game::tick).
    IDamageSink* atk = (player && player->isAlive()) ? player : nullptr;
    m_l8Enemies.update(dt, scene, physics, eye, atk, attackFx);

    // Allied markers: movement-only (no attacks); stationary (chaseSpeed 0) so they
    // hold their reveal/wildlife positions.
    m_companions.update(dt, scene, physics, playerPos);
    m_fauna.update(dt, scene, physics, playerPos);

    // Hazard exposure: INERT until the player is inside the zone; then it arms + the
    // tracked exposure stat climbs (also armed by the host's L9HazardZone trigger).
    if (m_hazard.contains(playerPos)) {
        m_hazard.active = true;
        m_hazard.exposure += m_hazard.ratePerSec * dt;
    }
}

uint32_t Act2World::update(Scene& scene, x3::rhi::IRenderDevice& device,
                           x3::phys::IPhysicsWorld& physics, float focusX, float focusZ) {
    if (!m_built || !m_terrainUp) return 0;
    return m_terrain.update(scene, device, physics, focusX, focusZ);
}

void Act2World::onTrigger(uint32_t triggerId) {
    switch ((Act2Trigger)triggerId) {
        case Act2Trigger::L8EmergencePoint:
            if (!m_emergenceReached) {
                m_emergenceReached = true;
                x3::logInfo("Act2: L8 EMERGENCE POINT reached — alien surface revealed (the awe beat)");
            }
            break;
        case Act2Trigger::L8toL9Transition:
            if (!m_l9Reached) {
                m_l9Reached = true;
                x3::logInfo("Act2: crossed into L9 CRYSTALLINE DESERT EDGE");
            }
            break;
        case Act2Trigger::L9HazardZone:
            if (!m_hazard.active) {
                m_hazard.active = true;
                x3::logInfo("Act2: L9 hazard zone entered — heat/sandstorm exposure now tracked");
            }
            break;
    }
}

FireResult Act2World::onFire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                             Scene& scene, x3::phys::IPhysicsWorld& physics) {
    // Only the hostile L8 gauntlet is a valid target (allied companions/fauna excluded).
    return m_l8Enemies.fire(eye, dir, scene, physics);
}

void Act2World::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                     const Scene& scene) const {
    m_l8Enemies.drawAll(device, frame, scene);
    m_companions.drawAll(device, frame, scene);
    m_fauna.drawAll(device, frame, scene);
}

void Act2World::shutdown(Scene& scene, x3::rhi::IRenderDevice& device,
                         x3::phys::IPhysicsWorld& physics) {
    if (m_terrainUp) {
        m_terrain.shutdown(scene, device, physics);
        m_terrainUp = false;
    }
}

bool Act2World::transitionReachable(const TriggerSystem& triggers) const {
    if (!m_built) return false;
    const TriggerVolume* t = triggers.findById((uint32_t)Act2Trigger::L8toL9Transition);
    if (!t || !t->enabled) return false;
    // The transition sits between the L8 emergence exit and the L9 spawn (the player
    // crosses +X to progress) and the L9 spawn is adjacent (within reach).
    const float tcx = 0.5f * (t->min.x + t->max.x);
    const bool between = (tcx >= m_l8EmergenceExit.x - 8.0f) && (tcx <= m_l9Spawn.x + 8.0f);
    const float dx = m_l9Spawn.x - m_l8EmergenceExit.x;
    return between && dx > 0.0f && dx < 120.0f;
}

// ===========================================================================
// Headless self-test (--test-act2).
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[act2-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[act2-test] FAIL ") + name); }
}

constexpr float kFixedDt = 1.0f / 60.0f;
using HeadlessDevice = x3::game::HeadlessRenderDevice;

void roleSplit(const MonsterManager& m, uint32_t& melee, uint32_t& ranged) {
    melee = ranged = 0;
    for (uint32_t i = 0; i < m.count(); ++i) { if (m.at(i).ranged()) ++ranged; else ++melee; }
}
bool nameIs(const char* a, const char* b) { return std::string(a) == std::string(b); }

} // namespace

bool runAct2WorldSelfTest() {
    g_pass = g_fail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();
    HeadlessDevice device;
    Scene scene;
    TriggerSystem triggers;
    Act2World world;
    world.build(scene, device, *physics, triggers, /*jobs*/ nullptr, riggedGlbRoot());

    check(world.built(), "A0 Act-2 world built");

    // ---- Area framework: all 13 levels named + objective'd; L8/L9 implemented. ----
    {
        bool allNamed = true;
        for (uint32_t lv = kAct2FirstLevel; lv <= kAct2LastLevel; ++lv) {
            const Act2AreaPlan& p = world.plan((Act2Level)lv);
            if (std::string(p.name).empty() || std::string(p.objective).empty()) allNamed = false;
        }
        check(allNamed, "A1 all 13 Act-2 levels (L8..L20) carry a name + objective");

        const Act2AreaPlan& l8 = world.plan(Act2Level::L8_SurfaceEmergence);
        bool l8ok = nameIs(l8.name, "Surface Emergence") && l8.implemented &&
                    l8.footprintX == 100.0f && l8.visibility == 500.0f &&
                    (l8.spawn.x != 0.0f || l8.spawn.z != 0.0f) &&
                    !std::string(l8.objective).empty();
        check(l8ok, "A2 L8: 100 m tunnel + 500 m emergence reveal + spawn + objective");

        const Act2AreaPlan& l9 = world.plan(Act2Level::L9_CrystallineDesertEdge);
        bool l9ok = nameIs(l9.name, "Crystalline Desert Edge") && l9.implemented &&
                    l9.footprintX > 0.0f && l9.footprintZ > 0.0f &&
                    (l9.spawn.x != 0.0f || l9.spawn.z != 0.0f) &&
                    !std::string(l9.objective).empty();
        check(l9ok, "A3 L9: desert footprint + spawn + objective");
    }

    // ---- L8 lab-exit gauntlet: 5 ranged Pursuit Drones + 3 melee Infected, alive. ----
    {
        const Act2AreaPlan& l8 = world.plan(Act2Level::L8_SurfaceEmergence);
        uint32_t melee = 0, ranged = 0; roleSplit(world.l8Enemies(), melee, ranged);
        check(world.l8Enemies().count() == 8 && melee == 3 && ranged == 5 &&
              l8.meleeCount == 3 && l8.rangedCount == 5,
              "A4 L8 gauntlet = 8 (3 Infected melee + 5 Pursuit Drones ranged)");
        check(world.l8Enemies().aliveCount() == 8, "A5 all L8 gauntlet enemies alive at load");
    }

    // ---- Emergence-Point companions: 4 allied, non-hostile, alive. ----
    {
        const Act2AreaPlan& l8 = world.plan(Act2Level::L8_SurfaceEmergence);
        bool ok = world.companions().count() == 4 && l8.companionCount == 4 &&
                  world.companions().aliveCount() == 4;
        for (uint32_t i = 0; i < world.companions().count(); ++i) {
            const MonsterSystem& c = world.companions().at(i);
            if (!c.isAllied() || c.attackDamage() != 0) ok = false;
        }
        check(ok, "A6 Emergence Point: 4 allied companion markers (non-hostile)");
    }

    // ---- L9 crystals + neutral fauna. ----
    {
        const Act2AreaPlan& l9 = world.plan(Act2Level::L9_CrystallineDesertEdge);
        bool ok = world.crystalCount() == kAct2CrystalCount &&
                  l9.crystalCount == kAct2CrystalCount &&
                  world.fauna().count() == 3 && l9.faunaCount == 3;
        for (uint32_t i = 0; i < world.fauna().count(); ++i)
            if (!world.fauna().at(i).isAllied()) ok = false;
        check(ok, "A7 L9 desert: 6 crystal props + 3 neutral (allied) fauna");
    }

    // ---- Hazard zone PRESENT but INERT at load; stays inert outside; arms inside. ----
    {
        const Act2AreaPlan& l9 = world.plan(Act2Level::L9_CrystallineDesertEdge);
        check(l9.hasHazard && !world.hazardActive() && world.hazardExposure() == 0.0f,
              "A8 L9 hazard zone present but INERT at load (no exposure)");

        // Tick OUTSIDE the hazard (at the L9 spawn): stays inert.
        for (int i = 0; i < 60; ++i)
            world.tick(kFixedDt, scene, *physics, l9.spawn, l9.spawn, nullptr, AttackFxFn{});
        check(!world.hazardActive() && world.hazardExposure() == 0.0f,
              "A9 hazard stays inert while the player is outside the zone");

        // Tick INSIDE the hazard: it arms + the exposure stat climbs.
        const HazardZone& hz = world.hazard();
        x3::phys::Vec3 inside{ 0.5f * (hz.min.x + hz.max.x), 0.5f * (hz.min.y + hz.max.y),
                               0.5f * (hz.min.z + hz.max.z) };
        for (int i = 0; i < 60; ++i)
            world.tick(kFixedDt, scene, *physics, inside, inside, nullptr, AttackFxFn{});
        check(world.hazardActive() && world.hazardExposure() > 0.0f,
              "A10 entering the hazard zone arms it + accumulates exposure");
    }

    // ---- Story beats + L8 -> L9 reachability. ----
    {
        check(!world.emergenceReached() && !world.l9Reached(),
              "A11 emergence + L9 beats not latched at load");
        world.onTrigger((uint32_t)Act2Trigger::L8EmergencePoint);
        world.onTrigger((uint32_t)Act2Trigger::L8toL9Transition);
        check(world.emergenceReached() && world.l9Reached(),
              "A12 emergence-point + L8->L9 triggers latch their beats");
        check(world.transitionReachable(triggers),
              "A13 L8 -> L9 transition trigger registered + enabled + between the areas");
    }

    // ---- Alien sky: enabled + violet (blue>red) + thick haze. ----
    {
        const x3::rhi::IRenderDevice::SkyParams& sky = world.alienSky();
        check(sky.enabled && sky.sunColor[2] > sky.sunColor[0] && sky.haze > 0.6f,
              "A14 alien sky tuned (binary-sun/purple-atmosphere: violet sun + thick haze)");
    }

    // ---- The surface stood up: terrain streamer resident under the L8 spawn. ----
    {
        const Act2AreaPlan& l8 = world.plan(Act2Level::L8_SurfaceEmergence);
        for (int i = 0; i < 12 && !world.focusTileResident(l8.spawn.x, l8.spawn.z); ++i)
            world.update(scene, device, *physics, l8.spawn.x, l8.spawn.z);
        check(world.residentTiles() > 0 && world.focusTileResident(l8.spawn.x, l8.spawn.z),
              "A15 alien terrain surface stood up (focus tile resident under L8 spawn)");
    }

    world.shutdown(scene, device, *physics);
    physics->shutdown();
    x3::logInfo(std::string("act2: ") + std::to_string(g_pass) + "/" +
                std::to_string(g_pass + g_fail) + " passed");
    return g_fail == 0;
}

} // namespace x3::game
