// EFLZ Act 2 — desert depths + the Salvari camp (L10/L11). See act2_desert.h.
//
// Clean-room: built ONLY from X3Native's own Scene / monster (incl. the Act-2
// roster) / trigger / terrain / mesh_prims systems + the engine interfaces + the
// EFLZ design docs (Tim's own IP). No RBDOOM / id Tech / Doom / Quake — or any
// other game-engine — source consulted. CONTENT/LEVEL-SCRIPT ONLY: no renderer or
// core-engine changes. Mirrors act2_world.cpp's authoring style (build / tick /
// onTrigger / onInteract / draw + a headless self-test).
#include "act2_desert.h"
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

// Terrain residency-ring radius the surface comes up with (matches act2_world):
// small at build (cheap + headless-friendly); the interactive host bumps it via
// terrain().setRadius().
constexpr int kDesertStreamRadius = 2;

// World position sitting ON the canonical alien surface at (x,z), plus a body/eye
// offset. Uses the PURE placement sampler (no tiles needed) so it is identical to
// what the streamer generates underfoot — same approach act2_world uses.
x3::phys::Vec3 surfaceAt(float x, float z, float yOff) {
    float p[3];
    placeOnTerrain(x, z, p);   // {x, surfaceY, z}
    return x3::phys::Vec3{ p[0], p[1] + yOff, p[2] };
}

// L10 OVERLORD PATROL — melee. A light patrol of the Overlord invasion force
// hunting the approach to the Salvari camp. Built on the DominionTrooper (Guard)
// melee profile, tinted Overlord crimson, with damage forced into the combat::
// melee band so the patrol is unambiguously HOSTILE. EXISTING roster type.
MonsterSystem::Tuning overlordMeleeTuning() {
    MonsterSystem::Tuning t = tuningFor(EnemyType::DominionTrooper);
    t.type           = MonsterType::Guard;
    t.ranged         = false;
    t.damage         = combat::kMeleeDamageDefault;     // ensure > 0 (hostile)
    t.attackRange    = combat::kMeleeRange;
    t.attackCooldown = combat::kMeleeCooldownDefault;
    t.tint[0] = 0.85f; t.tint[1] = 0.20f; t.tint[2] = 0.28f; t.tint[3] = 1.0f; // Overlord crimson
    return t;
}

// L10 OVERLORD PATROL — ranged elite. Built on the Illuminated (ranged elite)
// profile, tinted Overlord magenta, damage forced into the combat:: ranged band.
// EXISTING roster type.
MonsterSystem::Tuning overlordRangedTuning() {
    MonsterSystem::Tuning t = tuningFor(EnemyType::Illuminated);
    t.type           = MonsterType::Drone;
    t.ranged         = true;
    t.damage         = combat::kRangedDamageDefault;    // ensure > 0 (hostile)
    t.attackCooldown = combat::kRangedCooldownDefault;
    t.standoff       = combat::kRangedStandoff;
    t.tint[0] = 0.80f; t.tint[1] = 0.25f; t.tint[2] = 0.55f; t.tint[3] = 1.0f; // Overlord magenta
    return t;
}

// A Salvari refugee marker. Uses the REAL Act-2 roster `SalvariAlly` type (the
// roster lane already shipped it): startAllied => m_allied=true + m_dmg=0 at build,
// bioluminescent-teal tint, humanoid stand-in (box fallback on a clean checkout).
// `r,g,b` overrides the tint to distinguish the cast (K'thara / the injured one).
MonsterSystem::Tuning salvariTuning(float r, float g, float b) {
    MonsterSystem::Tuning t = act2EnemyTuning(Act2EnemyType::SalvariAlly);
    t.chaseSpeed = 0.0f;   // a stationary NPC marker (a camp/contact, not a fighter)
    t.tint[0] = r; t.tint[1] = g; t.tint[2] = b; t.tint[3] = 1.0f;
    return t;
}

} // namespace

void Act2Desert::build(Scene& scene, x3::rhi::IRenderDevice& device,
                       x3::phys::IPhysicsWorld& physics, TriggerSystem& triggers,
                       x3::jobs::IJobSystem* jobs, std::string_view modelDir) {
    m_modelDir = std::string(modelDir);

    // ---- Layout: the SAME Act-2 region anchor act2_world uses (X0/Z0), continuing
    // the +X desert progression past L9. L9's content runs to ~X0+500, so this lane
    // begins at its far edge and walks deeper: desert depths -> hidden cave mouth ->
    // Salvari camp inside the cave. ----
    const float X0 = 1024.0f, Z0 = 1024.0f;  // region anchor (matches act2_world)
    const float Xedge = X0 + 500.0f;         // L9 far edge (the L9->L10 approach)
    const float Xt910 = X0 + 520.0f;         // L9 -> L10 transition gate
    const float X10   = X0 + 560.0f;         // L10 desert-depths spawn
    const float Xcave = X0 + 740.0f;         // hidden Salvari camp entrance (L10 far end)
    const float Xt1011= X0 + 760.0f;         // L10 -> L11 transition gate (into the cave)
    const float X11   = X0 + 800.0f;         // L11 Salvari camp ("Refugee Haven") center

    m_l9Edge       = surfaceAt(Xedge, Z0, 0.05f);
    m_l10Spawn     = surfaceAt(X10,   Z0, 0.05f);
    m_caveMouthPos = surfaceAt(Xcave, Z0, 0.05f);
    m_l11Spawn     = surfaceAt(X11,   Z0, 0.05f);

    // A reusable box-prop builder (crystals / cave graybox / cave mouth / upgrade
    // station). `b*` is the base color; `e*`/`es` the optional HDR emissive term.
    auto addBoxProp = [&](float hx, float hy, float hz, float cx, float cy, float cz,
                          float br, float bg, float bb,
                          float er, float eg, float eb, float es) -> uint32_t {
        x3::prims::PrimMesh m = x3::prims::makeBox(hx, hy, hz, cx, cy, cz);
        Entity e;
        e.mesh = device.createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                   m.index.data(), (uint32_t)m.index.size());
        e.baseColor[0] = br; e.baseColor[1] = bg; e.baseColor[2] = bb; e.baseColor[3] = 1.0f;
        e.emissive[0] = er; e.emissive[1] = eg; e.emissive[2] = eb; e.emissive[3] = es;
        e.tag = (uint32_t)Tag::Prop;
        return scene.add(e);
    };
    // A "singing crystal" shard standing on the surface at (x,z): a tall emissive
    // box (violet body + HDR bloom glow), exactly like act2_world's L9 crystals.
    auto addCrystal = [&](float x, float z, float height,
                          float er, float eg, float eb) {
        const float halfH = height * 0.5f;
        float base[3]; placeOnTerrain(x, z, base);
        return addBoxProp(0.9f, halfH, 0.9f, base[0], base[1] + halfH, base[2],
                          0.62f, 0.40f, 0.95f, er, eg, eb, 2.0f);
    };

    // ===================================================================
    // L10 — CRYSTALLINE DESERT DEPTHS. A deeper continuation of the L9 crystal
    // desert: emissive crystal formations, FIRST CONTACT with allied Salvari
    // refugees near a hidden crystal-cave entrance (one of them injured -> a side-
    // quest hook), and a light Overlord patrol hunting the approach.
    // ===================================================================
    {
        DesertAreaPlan& p = m_plan[(uint32_t)Act2Level::L10_CrystallineDesertDepths - kDesertFirstLevel];
        p.level       = Act2Level::L10_CrystallineDesertDepths;
        p.name        = "Crystalline Desert Depths";
        p.biome       = "Deeper crystal desert -> hidden Salvari crystal-cave camp";
        p.objective   = "Find the Salvari refugees in the crystal caves; learn of the Overlord invasion";
        p.implemented = true;
        p.spawn       = m_l10Spawn;
        p.footprintX  = Xcave - X10 + 40.0f;   // depths run from the spawn past the cave mouth
        p.footprintZ  = 120.0f;

        // Emissive singing-crystal formations scattered through the depths (violet
        // body, violet/teal glow), guiding the player toward the cave mouth.
        const float cx[kDesertCrystalCount] = { X10+20, X10+55, X10+95, X10+130, X10+165, Xcave-60, Xcave-25 };
        const float cz[kDesertCrystalCount] = { Z0-28,  Z0+34,  Z0-18,  Z0+44,   Z0-40,   Z0+22,    Z0-14   };
        const float chh[kDesertCrystalCount]= { 6.0f,   4.5f,   7.5f,   5.0f,    8.0f,    5.5f,     6.5f    };
        for (uint32_t i = 0; i < kDesertCrystalCount; ++i)
            m_desertCrystals.push_back(addCrystal(cx[i], cz[i], chh[i], 0.45f, 0.20f, 0.95f));
        p.crystalCount = (uint32_t)m_desertCrystals.size();

        // The hidden camp entrance: a tall glowing crystal cluster marking the cave
        // mouth (a brighter teal-violet beacon so the way down reads on screen).
        {
            float base[3]; placeOnTerrain(Xcave, Z0, base);
            m_caveMouth = addBoxProp(2.0f, 4.5f, 1.2f, base[0], base[1] + 4.5f, base[2],
                                     0.40f, 0.55f, 0.90f, 0.35f, 0.85f, 1.00f, 2.5f);
        }

        // Light Overlord patrol (existing hostile roster, tinted Overlord) spread
        // along the approach: 2 melee troopers woven with 1 ranged elite.
        m_patrol.spawn(scene, device, physics, m_modelDir,
                       surfaceAt(X10 + 45, Z0 + 5, kDesertEnemyYOff), overlordMeleeTuning());
        m_patrol.spawn(scene, device, physics, m_modelDir,
                       surfaceAt(X10 + 85, Z0 - 6, kDesertEnemyYOff), overlordMeleeTuning());
        m_patrol.spawn(scene, device, physics, m_modelDir,
                       surfaceAt(X10 + 125, Z0 + 3, kDesertEnemyYOff), overlordRangedTuning());
        p.patrolCount = m_patrol.count();

        // FIRST CONTACT: allied Salvari refugees near the cave mouth. Index 0 is the
        // INJURED Salvari that anchors the "help the injured Salvari" side-quest
        // (tinted wounded-amber, set apart); the rest are bioluminescent teal. All
        // are flipped allied (the roster's startAllied already does this; the
        // convertToAllied() call makes it explicit + robust).
        struct Contact { float x, z, r, g, b; };
        const Contact contacts[kDesertContactCount] = {
            { Xcave - 55, Z0 - 7, 0.90f, 0.62f, 0.30f }, // injured Salvari (wounded amber)
            { Xcave - 34, Z0 + 9, 0.55f, 0.95f, 0.85f }, // refugee scout (teal)
            { Xcave - 18, Z0 - 3, 0.55f, 0.95f, 0.85f }, // refugee scout (teal)
        };
        for (uint32_t i = 0; i < kDesertContactCount; ++i) {
            const Contact& c = contacts[i];
            uint32_t idx = m_contacts.spawn(scene, device, physics, m_modelDir,
                                            surfaceAt(c.x, c.z, kDesertEnemyYOff),
                                            salvariTuning(c.r, c.g, c.b));
            m_contacts.at(idx).convertToAllied();
        }
        m_injuredIdx       = 0;                 // the wounded contact -> the side-quest target
        p.salvariCount     = m_contacts.count();
        p.hasSideQuest     = true;              // hook PRESENT (inert until interacted)
    }

    // ===================================================================
    // L11 — SALVARI CAMP ("Refugee Haven"). A hidden cave settlement: a graybox
    // rock enclosure lit by bioluminescent crystals, sheltering the refugee
    // population (canon: hundreds; the slice places survivor markers incl. K'thara).
    // An alien-equipment UPGRADE STATION interact + a cultural-exchange beat.
    // ===================================================================
    {
        DesertAreaPlan& p = m_plan[(uint32_t)Act2Level::L11_SalvariCamp - kDesertFirstLevel];
        p.level       = Act2Level::L11_SalvariCamp;
        p.name        = "Salvari Camp (Refugee Haven)";
        p.biome       = "Hidden crystal-cave refugee settlement (hub)";
        p.objective   = "Reach Refugee Haven, meet the Salvari, and recruit K'thara";
        p.implemented = true;
        p.spawn       = m_l11Spawn;
        p.footprintX  = 90.0f;                  // the cave-settlement footprint
        p.footprintZ  = 90.0f;
        p.survivorPopulation = kSalvariCampPopulation;

        // ---- Cave graybox: a floor pad + a ceiling + a back wall + two side walls
        // (the entrance side, toward the cave mouth at -X, is left open). Dark rock,
        // no emissive — the crystals below provide the light. ----
        const float half = 45.0f;               // half-footprint
        float fb[3]; placeOnTerrain(X11, Z0, fb);
        const float floorY = fb[1];
        // Floor pad (thin).
        m_campStructures.push_back(addBoxProp(half, 0.4f, half, X11, floorY + 0.4f, Z0,
                                              0.18f, 0.17f, 0.20f, 0,0,0,0));
        // Ceiling.
        m_campStructures.push_back(addBoxProp(half, 0.4f, half, X11, floorY + 12.0f, Z0,
                                              0.16f, 0.15f, 0.18f, 0,0,0,0));
        // Back wall (+X far side).
        m_campStructures.push_back(addBoxProp(0.6f, 6.0f, half, X11 + half, floorY + 6.0f, Z0,
                                              0.20f, 0.19f, 0.23f, 0,0,0,0));
        // Side walls (+/-Z).
        m_campStructures.push_back(addBoxProp(half, 6.0f, 0.6f, X11, floorY + 6.0f, Z0 + half,
                                              0.20f, 0.19f, 0.23f, 0,0,0,0));
        m_campStructures.push_back(addBoxProp(half, 6.0f, 0.6f, X11, floorY + 6.0f, Z0 - half,
                                              0.20f, 0.19f, 0.23f, 0,0,0,0));

        // Bioluminescent cave crystals lining the settlement (teal/violet glow).
        const float kx[kCampCrystalCount] = { X11-30, X11-10, X11+12, X11+30, X11-22, X11+22, X11-2,  X11+38 };
        const float kz[kCampCrystalCount] = { Z0-30,  Z0+28,  Z0-24,  Z0+18,  Z0+10,  Z0-12,  Z0+34,  Z0-30  };
        const float kh[kCampCrystalCount] = { 3.5f,   4.5f,   3.0f,   5.0f,   2.5f,   4.0f,   3.5f,   4.5f   };
        for (uint32_t i = 0; i < kCampCrystalCount; ++i)
            m_campCrystals.push_back(addCrystal(kx[i], kz[i], kh[i], 0.30f, 0.80f, 0.85f));

        // The alien-equipment UPGRADE STATION: a glowing cyan tech console (a stub
        // prop). Inert until interacted via onInteract(L11UpgradeStation).
        {
            float ub[3]; placeOnTerrain(X11, Z0 + 10.0f, ub);
            m_upgradeStation = addBoxProp(1.2f, 1.0f, 1.2f, ub[0], ub[1] + 1.0f, ub[2],
                                          0.35f, 0.40f, 0.50f, 0.20f, 0.90f, 1.00f, 2.5f);
            p.hasUpgradeStation = true;         // PRESENT (inert until interacted)
        }

        // Salvari survivor markers (allied). Index 0 is K'thara, the Salvari
        // commander (brighter cyan-white) the player recruits; the rest are
        // refugees (bioluminescent teal). All allied (startAllied + explicit flip).
        struct Survivor { float x, z, r, g, b; };
        const Survivor survivors[kCampSurvivorCount] = {
            { X11 - 8,  Z0 + 0,  0.70f, 0.98f, 1.00f }, // K'thara (commander) — cyan-white
            { X11 - 18, Z0 - 10, 0.55f, 0.95f, 0.85f },
            { X11 - 14, Z0 + 14, 0.55f, 0.95f, 0.85f },
            { X11 + 4,  Z0 - 16, 0.55f, 0.95f, 0.85f },
            { X11 + 10, Z0 + 12, 0.55f, 0.95f, 0.85f },
            { X11 + 20, Z0 - 4,  0.55f, 0.95f, 0.85f },
            { X11 + 16, Z0 + 22, 0.55f, 0.95f, 0.85f },
        };
        for (uint32_t i = 0; i < kCampSurvivorCount; ++i) {
            const Survivor& s = survivors[i];
            uint32_t idx = m_survivors.spawn(scene, device, physics, m_modelDir,
                                             surfaceAt(s.x, s.z, kDesertEnemyYOff),
                                             salvariTuning(s.r, s.g, s.b));
            m_survivors.at(idx).convertToAllied();
        }
        p.salvariCount = m_survivors.count();
    }

    // ===================================================================
    // L10 — SAURIAN WARLORD boss (canon-aliens Reptilian Overlord enforcer). Spawned
    // in a dedicated arena center deep in L10 (between the Overlord patrol and the
    // cave mouth) using the canon-alien SaurianWarlord Tuning (HP 540, Boss type,
    // 3 phases + memory-flash window aligned to the Adaptive-Hide rhythm). PRESENT
    // at build but INERT (m_warlordSpawned=false) — arming the L10WarlordArena
    // trigger latches it active. Once the Adaptive-Hide engine extension lands,
    // setting adaptiveHideResist=0.6 on the Tuning row in canon_aliens.cpp lights up
    // the "rotate damage type" rhythm.
    // ===================================================================
    {
        const float Xwarlord = X0 + 680.0f;
        m_warlord.spawn(scene, device, physics, m_modelDir,
                        surfaceAt(Xwarlord, Z0, kDesertEnemyYOff),
                        canonAlienTuning(CanonAlien::SaurianWarlord));
    }

    // ---- Triggers (host's shared TriggerSystem; ids forwarded to onTrigger).
    // FRESH 90+ id range so this shares one TriggerSystem with act2_world (80..82)
    // and the Act-1 systems without colliding. ----
    {
        auto addGate = [&](float x, float z, float halfX, float halfZ, Act2DesertTrigger id) {
            float y[3]; placeOnTerrain(x, z, y);
            triggers.add(x3::phys::Vec3{ x - halfX, y[1] - 3.0f, z - halfZ },
                         x3::phys::Vec3{ x + halfX, y[1] + 8.0f, z + halfZ },
                         (uint32_t)id, true);
        };
        addGate(Xt910,  Z0, 6.0f,  14.0f, Act2DesertTrigger::L9toL10Transition);
        addGate(Xcave,  Z0, 10.0f, 14.0f, Act2DesertTrigger::L10CaveEntrance);
        addGate(X0 + 680.0f, Z0, 12.0f, 14.0f, Act2DesertTrigger::L10WarlordArena);
        addGate(Xt1011, Z0, 6.0f,  14.0f, Act2DesertTrigger::L10toL11Transition);
        addGate(X11,    Z0, 16.0f, 16.0f, Act2DesertTrigger::L11CulturalExchange);
    }

    // ---- Stand up the alien surface (same streamed world + config as act2_world,
    // keeping L9->L10 visually continuous). jobs may be null -> synchronous
    // generation (headless self-test); drain the focus neighbourhood so the player
    // has ground immediately under the L10 spawn. ----
    m_cfg = worldTerrainConfig();
    const float fX = m_l10Spawn.x, fZ = m_l10Spawn.z;
    m_terrain.init(scene, device, physics, jobs, m_cfg, fX, fZ, kDesertStreamRadius);
    m_terrainUp = true;
    for (int i = 0; i < 12 && !m_terrain.focusTileResident(fX, fZ); ++i)
        m_terrain.update(scene, device, physics, fX, fZ);

    // ---- Alien sky: the SAME binary-sun / purple-atmosphere tuning act2_world uses
    // (this lane is the continuation of the same surface). No new engine tech. ----
    m_sky = x3::rhi::IRenderDevice::SkyParams{};
    m_sky.enabled      = true;
    m_sky.sunDir[0]    = 0.45f; m_sky.sunDir[1] = 0.82f; m_sky.sunDir[2] = 0.35f;
    m_sky.sunColor[0]  = 0.78f; m_sky.sunColor[1] = 0.62f; m_sky.sunColor[2] = 1.00f; // violet (blue>red)
    m_sky.sunIntensity = 1.15f;
    m_sky.haze         = 0.82f;
    m_sky.exposure     = 1.05f;

    m_built = true;
    x3::logInfo("Act2Desert::build complete — L10 CRYSTALLINE DESERT DEPTHS (7 crystals, "
                "hidden cave mouth, 3 first-contact Salvari [1 injured -> side-quest], "
                "3-strong Overlord patrol) + L11 SALVARI CAMP (cave graybox + 8 crystals, "
                "7 survivor markers incl. K'thara, an upgrade station + cultural-exchange "
                "beat); alien sky + streamed terrain stood up");
}

void Act2Desert::tick(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                      const x3::phys::Vec3& eye, const x3::phys::Vec3& playerPos,
                      IDamageSink* player, const AttackFxFn& attackFx) {
    if (!m_built) return;

    // The Overlord patrol attacks only while the player is alive (matches act2_world).
    IDamageSink* atk = (player && player->isAlive()) ? player : nullptr;
    m_patrol.update(dt, scene, physics, eye, atk, attackFx);
    if (m_warlordSpawned) m_warlord.update(dt, scene, physics, eye, atk, attackFx);

    // Allied Salvari (contacts + camp survivors): movement-only, stationary markers
    // (chaseSpeed 0) — they never fight the player.
    m_contacts.update(dt, scene, physics, playerPos);
    m_survivors.update(dt, scene, physics, playerPos);
}

uint32_t Act2Desert::update(Scene& scene, x3::rhi::IRenderDevice& device,
                            x3::phys::IPhysicsWorld& physics, float focusX, float focusZ) {
    if (!m_built || !m_terrainUp) return 0;
    return m_terrain.update(scene, device, physics, focusX, focusZ);
}

void Act2Desert::onTrigger(uint32_t triggerId) {
    switch ((Act2DesertTrigger)triggerId) {
        case Act2DesertTrigger::L9toL10Transition:
            if (!m_l10Reached) {
                m_l10Reached = true;
                x3::logInfo("Act2: crossed into L10 CRYSTALLINE DESERT DEPTHS");
            }
            break;
        case Act2DesertTrigger::L10CaveEntrance:
            if (!m_caveFound) {
                m_caveFound = true;
                x3::logInfo("Act2: found the hidden Salvari camp entrance (crystal cave)");
            }
            break;
        case Act2DesertTrigger::L10toL11Transition:
            if (!m_l11Reached) {
                m_l11Reached = true;
                x3::logInfo("Act2: entered L11 SALVARI CAMP — Refugee Haven");
            }
            break;
        case Act2DesertTrigger::L11CulturalExchange:
            if (!m_culturalExchange) {
                m_culturalExchange = true;
                x3::logInfo("Act2: Salvari cultural-exchange beat reached");
            }
            break;
        case Act2DesertTrigger::L10WarlordArena:
            if (!m_warlordSpawned) {
                m_warlordSpawned = true;
                x3::logInfo("Act2: L10 WARLORD ARENA armed — Saurian Warlord active");
            }
            break;
    }
}

bool Act2Desert::onInteract(uint32_t interactId) {
    switch ((Act2DesertInteract)interactId) {
        case Act2DesertInteract::L10InjuredSalvari:
            if (!m_injuredSalvariRescued) {
                m_injuredSalvariRescued = true;
                x3::logInfo("Act2: side-quest — helped the injured Salvari (refugee saved)");
                return true;
            }
            return false;
        case Act2DesertInteract::L11UpgradeStation:
            if (!m_upgradeGranted) {
                m_upgradeGranted = true;
                // TODO(roster/loadout lane): apply a real alien-equipment upgrade to the
                // player loadout. The gameplay-state half (a flagged grant) is here.
                x3::logInfo("Act2: used the Salvari upgrade station — alien-equipment upgrade granted");
                return true;
            }
            return false;
    }
    return false;
}

FireResult Act2Desert::onFire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                              Scene& scene, x3::phys::IPhysicsWorld& physics) {
    // L10 Warlord (when armed) takes priority — return its hit if any.
    if (m_warlordSpawned) {
        FireResult rw = m_warlord.fire(eye, dir, scene, physics);
        if (rw.hitMonster) return rw;
    }
    // Otherwise fire against the L10 Overlord patrol (allied Salvari excluded).
    return m_patrol.fire(eye, dir, scene, physics);
}

void Act2Desert::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                      const Scene& scene) const {
    m_patrol.drawAll(device, frame, scene);
    if (m_warlordSpawned) m_warlord.drawAll(device, frame, scene);
    m_contacts.drawAll(device, frame, scene);
    m_survivors.drawAll(device, frame, scene);
}

void Act2Desert::shutdown(Scene& scene, x3::rhi::IRenderDevice& device,
                          x3::phys::IPhysicsWorld& physics) {
    if (m_terrainUp) {
        m_terrain.shutdown(scene, device, physics);
        m_terrainUp = false;
    }
}

bool Act2Desert::chainReachable(const TriggerSystem& triggers) const {
    if (!m_built) return false;
    const TriggerVolume* a = triggers.findById((uint32_t)Act2DesertTrigger::L9toL10Transition);
    const TriggerVolume* b = triggers.findById((uint32_t)Act2DesertTrigger::L10toL11Transition);
    if (!a || !a->enabled || !b || !b->enabled) return false;
    // A transition is valid iff its center sits between its two areas (monotone +X)
    // and the areas are adjacent (within reach) — the player can cross it to progress.
    auto between = [](const TriggerVolume* t, const x3::phys::Vec3& lo, const x3::phys::Vec3& hi) {
        const float cx = 0.5f * (t->min.x + t->max.x);
        const float dx = hi.x - lo.x;
        return cx >= lo.x - 8.0f && cx <= hi.x + 8.0f && dx > 0.0f && dx < 120.0f;
    };
    return between(a, m_l9Edge, m_l10Spawn) && between(b, m_caveMouthPos, m_l11Spawn);
}

// ===========================================================================
// Headless self-test (--test-act2desert).
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[act2desert-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[act2desert-test] FAIL ") + name); }
}

constexpr float kFixedDt = 1.0f / 60.0f;
using HeadlessDevice = x3::game::HeadlessRenderDevice;

bool nameIs(const char* a, const char* b) { return std::string(a) == std::string(b); }

// All members of `m` are allied + deal 0 damage to the player + are alive.
bool allAlliedAndHarmless(const MonsterManager& m) {
    if (m.count() == 0) return false;
    for (uint32_t i = 0; i < m.count(); ++i) {
        const MonsterSystem& s = m.at(i);
        if (!s.isAllied() || s.attackDamage() != 0 || !s.alive()) return false;
    }
    return true;
}

} // namespace

bool runAct2DesertSelfTest() {
    g_pass = g_fail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();
    HeadlessDevice device;
    Scene scene;
    TriggerSystem triggers;
    Act2Desert world;
    world.build(scene, device, *physics, triggers, /*jobs*/ nullptr, riggedGlbRoot());

    check(world.built(), "D0 Act-2 desert world built");

    // ---- Area plans: L10 + L11 named + objective'd + implemented, with counts. ----
    {
        const DesertAreaPlan& l10 = world.plan(Act2Level::L10_CrystallineDesertDepths);
        bool l10ok = nameIs(l10.name, "Crystalline Desert Depths") && l10.implemented &&
                     !std::string(l10.objective).empty() &&
                     l10.footprintX > 0.0f && l10.footprintZ > 0.0f &&
                     (l10.spawn.x != 0.0f || l10.spawn.z != 0.0f) &&
                     l10.crystalCount == kDesertCrystalCount &&
                     l10.salvariCount == kDesertContactCount &&
                     l10.patrolCount  == kDesertPatrolCount &&
                     l10.hasSideQuest;
        check(l10ok, "D1 L10 plan: footprint + spawn + objective + crystal/Salvari/patrol counts + side-quest");

        const DesertAreaPlan& l11 = world.plan(Act2Level::L11_SalvariCamp);
        bool l11ok = nameIs(l11.name, "Salvari Camp (Refugee Haven)") && l11.implemented &&
                     !std::string(l11.objective).empty() &&
                     l11.footprintX > 0.0f && l11.footprintZ > 0.0f &&
                     (l11.spawn.x != 0.0f || l11.spawn.z != 0.0f) &&
                     l11.salvariCount == kCampSurvivorCount &&
                     l11.hasUpgradeStation &&
                     l11.survivorPopulation > 0;
        check(l11ok, "D2 L11 plan: footprint + spawn + objective + survivor count + upgrade station + population");
    }

    // ---- L10 first-contact Salvari: allied, 0 damage, alive. ----
    {
        check(world.contacts().count() == kDesertContactCount &&
              allAlliedAndHarmless(world.contacts()),
              "D3 L10 first-contact Salvari are ALLIED + 0 damage to player + alive");
    }

    // ---- L11 camp survivors (incl. K'thara): allied, 0 damage, alive. ----
    {
        check(world.survivors().count() == kCampSurvivorCount &&
              allAlliedAndHarmless(world.survivors()),
              "D4 L11 camp survivors (incl. K'thara) are ALLIED + 0 damage + alive");
    }

    // ---- L10 Overlord patrol: hostile (nonzero damage) + alive. ----
    {
        bool ok = world.patrol().count() == kDesertPatrolCount &&
                  world.patrol().aliveCount() == kDesertPatrolCount;
        for (uint32_t i = 0; i < world.patrol().count(); ++i) {
            const MonsterSystem& s = world.patrol().at(i);
            if (s.isAllied() || s.attackDamage() <= 0) ok = false;
        }
        check(ok, "D5 L10 Overlord patrol is HOSTILE (nonzero damage) + alive");
    }

    // ---- Emissive props placed (desert crystals + cave mouth + camp crystals +
    // upgrade station). ----
    {
        check(world.desertCrystalCount() == kDesertCrystalCount &&
              world.campCrystalCount()   == kCampCrystalCount &&
              world.caveMouthPlaced() &&
              world.upgradeStationEntity() != kNoLink,
              "D6 emissive props placed (desert/camp crystals + cave mouth + upgrade station)");
    }

    // ---- Side-quest + upgrade hooks: PRESENT but INERT at load. ----
    {
        const DesertAreaPlan& l10 = world.plan(Act2Level::L10_CrystallineDesertDepths);
        const DesertAreaPlan& l11 = world.plan(Act2Level::L11_SalvariCamp);
        check(l10.hasSideQuest && l11.hasUpgradeStation &&
              !world.injuredSalvariRescued() && !world.upgradeGranted(),
              "D7 side-quest + upgrade station present but INERT at load");
    }

    // ---- Interacting flips them; idempotent (second interact returns false). ----
    {
        bool firstSide = world.onInteract((uint32_t)Act2DesertInteract::L10InjuredSalvari);
        bool secondSide = world.onInteract((uint32_t)Act2DesertInteract::L10InjuredSalvari);
        bool firstUp = world.onInteract((uint32_t)Act2DesertInteract::L11UpgradeStation);
        bool secondUp = world.onInteract((uint32_t)Act2DesertInteract::L11UpgradeStation);
        check(firstSide && !secondSide && world.injuredSalvariRescued() &&
              firstUp && !secondUp && world.upgradeGranted(),
              "D8 interacting completes the side-quest + grants the upgrade (idempotent)");
    }

    // ---- Allied Salvari never harm the player even when ticked at point-blank. ----
    {
        // Place the "player" right on top of the camp survivors and tick: a hostile
        // group would damage a sink here; the allied group must not (0 damage).
        const DesertAreaPlan& l11 = world.plan(Act2Level::L11_SalvariCamp);
        for (int i = 0; i < 120; ++i)
            world.tick(kFixedDt, scene, *physics, l11.spawn, l11.spawn, nullptr, AttackFxFn{});
        check(allAlliedAndHarmless(world.survivors()) && allAlliedAndHarmless(world.contacts()),
              "D9 allied Salvari stay harmless (0 damage) after ticking at the camp");
    }

    // ---- Story beats: not latched at load; latch on their triggers. ----
    {
        check(!world.l10Reached() && !world.caveFound() && !world.l11Reached() &&
              !world.culturalExchangeDone(),
              "D10 L10/cave/L11/cultural beats not latched at load");
        world.onTrigger((uint32_t)Act2DesertTrigger::L9toL10Transition);
        world.onTrigger((uint32_t)Act2DesertTrigger::L10CaveEntrance);
        world.onTrigger((uint32_t)Act2DesertTrigger::L10toL11Transition);
        world.onTrigger((uint32_t)Act2DesertTrigger::L11CulturalExchange);
        check(world.l10Reached() && world.caveFound() && world.l11Reached() &&
              world.culturalExchangeDone(),
              "D11 L9->L10 / cave / L10->L11 / cultural-exchange triggers latch their beats");
    }

    // ---- L10 Saurian Warlord boss (canon-aliens) — PRESENT at load but INERT;
    // arming the L10WarlordArena trigger latches it active. ----
    {
        // Warlord built (1 monster in manager) but INERT at load (m_warlordSpawned=false).
        bool buildOk = world.warlord().count() == 1 && !world.warlordSpawned();
        bool tuningOk = false;
        if (world.warlord().count() == 1) {
            const MonsterSystem& w = world.warlord().at(0);
            tuningOk = w.maxHp() == 540 && w.type() == MonsterType::Boss && w.alive();
        }
        check(buildOk && tuningOk,
              "D15 Saurian Warlord PRESENT at load (HP 540, Boss-typed, alive) but INERT (warlordSpawned=false)");

        // Arming the L10WarlordArena trigger latches the boss live.
        world.onTrigger((uint32_t)Act2DesertTrigger::L10WarlordArena);
        check(world.warlordSpawned(),
              "D16 L10WarlordArena trigger latches warlordSpawned=true (boss goes live)");
    }

    // ---- L9 -> L10 -> L11 chain reachable (both transitions registered + enabled +
    // between their areas). ----
    {
        check(world.chainReachable(triggers),
              "D12 L9 -> L10 -> L11 transition chain reachable");
    }

    // ---- Alien sky: enabled + violet (blue>red) + thick haze (continuous w/ L9). ----
    {
        const x3::rhi::IRenderDevice::SkyParams& sky = world.alienSky();
        check(sky.enabled && sky.sunColor[2] > sky.sunColor[0] && sky.haze > 0.6f,
              "D13 alien sky tuned (binary-sun/purple-atmosphere: violet sun + thick haze)");
    }

    // ---- The surface stood up: terrain streamer resident under the L10 spawn. ----
    {
        const DesertAreaPlan& l10 = world.plan(Act2Level::L10_CrystallineDesertDepths);
        for (int i = 0; i < 12 && !world.focusTileResident(l10.spawn.x, l10.spawn.z); ++i)
            world.update(scene, device, *physics, l10.spawn.x, l10.spawn.z);
        check(world.residentTiles() > 0 && world.focusTileResident(l10.spawn.x, l10.spawn.z),
              "D14 alien terrain surface stood up (focus tile resident under L10 spawn)");
    }

    world.shutdown(scene, device, *physics);
    physics->shutdown();
    x3::logInfo(std::string("act2desert: ") + std::to_string(g_pass) + "/" +
                std::to_string(g_pass + g_fail) + " passed");
    return g_fail == 0;
}

} // namespace x3::game
