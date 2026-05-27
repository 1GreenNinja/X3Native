// EFLZ Act 2 — mid-biomes content (L12 Advanced Cave System, L13 Toxic
// Swamplands Edge, L14 Research Station, L15 Tree Cities). See act2_caves.h.
//
// Clean-room: built ONLY from X3Native's own Scene / monster / trigger / mesh-
// prims systems + the engine interfaces + the EFLZ design docs (Tim's own IP).
// No RBDOOM / id Tech / Doom / Quake — or any other game-engine — source was
// consulted. CONTENT/LEVEL-SCRIPT ONLY: no renderer or core-engine changes.
// Mirrors act2_world.cpp's authoring style exactly (build/tick/onTrigger/draw
// + a headless self-test). The Act-2 host (act2_world.*) and the Act-2 desert
// lane (act2_desert.*, separate machine) are NOT touched here.
#include "act2_caves.h"
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

// ---- Region anchor + per-level layout along +X ---------------------------
// The Act-2 caves region sits well clear of the Act-1 Spire (origin) and of
// the Act-2 open-world host (which anchors at world ~ (1024, 1024)). We pick
// a separate region anchor so build() never collides spatially with either.
constexpr float kRegionX = 2048.0f;
constexpr float kRegionZ = 2048.0f;

// Per-level local origins (each level is its own world neighbourhood along +X
// from the region anchor; deltas keep the levels visually distinct in any HUD
// minimap but a host can rebind them).
constexpr float kL12X = kRegionX +    0.0f;   // L12 cave portal entry
constexpr float kL12HeartX = kL12X + 70.0f;   // Crystal Heart Chamber center
constexpr float kL12ArenaX = kL12X + 120.0f;  // abyss boss arena (Memory Hunter)
constexpr float kL12ExitX  = kL12X + 160.0f;  // L12 -> L13 threshold
constexpr float kL13X = kL12X + 200.0f;       // L13 swamp edge spawn
constexpr float kL13HazardX = kL13X + 60.0f;  // L13 poison hazard zone center
constexpr float kL13ExitX  = kL13X + 140.0f;  // L13 -> L14 threshold
constexpr float kL14X = kL13X + 180.0f;       // L14 research station spawn
constexpr float kL14SirenX = kL14X + 50.0f;   // L14 Siren ambush room
constexpr float kL14ExitX  = kL14X + 110.0f;  // L14 -> L15 threshold
constexpr float kL15X = kL14X + 150.0f;       // L15 Tree-Cities spawn (ground)

// Cave Y-levels (the cave system has authored Y like the Spire floors do —
// not on the streamed terrain). Upper caves at ~ -20 m, archives at ~ -25 m,
// Crystal Heart at ~ -30 m, abyss arena at ~ -40 m. Swamp + station are
// surface-level (Y ~ 0); the Tree Cities rise up to canopy Y ~ +30 m.
constexpr float kL12CaveBaseY    = -20.0f;
constexpr float kL12ArchivesBaseY= -25.0f;
constexpr float kL12HeartBaseY   = -30.0f;
constexpr float kL12AbyssBaseY   = -40.0f;
constexpr float kL13SwampBaseY   =   0.0f;
constexpr float kL14StationBaseY =   0.0f;
constexpr float kL15GroundBaseY  =   0.0f;
constexpr float kL15CanopyBaseY  =  18.0f;    // first platform Y
constexpr float kL15PlatformLift = 12.0f;     // each higher platform lifts this much

// World position at fixed (x, z) and a given base Y plus a body/eye offset.
// Caves + station + tree platforms are authored at hand-picked Ys (no terrain
// sample), so this is a pure addition — mirrors the spire_mid kEnemyAt helper.
x3::phys::Vec3 at(float x, float z, float y, float yOff = 0.0f) {
    return x3::phys::Vec3{ x, y + yOff, z };
}

// ---- L12 hostile pack tuning (cave fauna — repurposed NativeDesertFauna).
// EFLZ canon: the upper caves carry hostile alien wildlife (the bioluminescent
// caves are home territory); we re-tint NativeDesertFauna to cave-blue and use
// it as the L12 hostile pack so the data-driven roster stays single-sourced.
MonsterSystem::Tuning caveFaunaTuning() {
    MonsterSystem::Tuning t = act2EnemyTuning(Act2EnemyType::NativeDesertFauna);
    t.tint[0] = 0.35f; t.tint[1] = 0.65f; t.tint[2] = 1.00f; t.tint[3] = 1.0f; // cave-blue glow
    return t;
}

// L12 Salvari Archives readers: the SalvariAlly row (already startAllied) tinted
// archive-gold. STATIONARY (chase speed forced 0) so they hold their reading-
// chamber positions as friendly markers.
MonsterSystem::Tuning archivesAllyTuning() {
    MonsterSystem::Tuning t = act2EnemyTuning(Act2EnemyType::SalvariAlly);
    t.chaseSpeed = 0.0f;
    t.tint[0] = 0.90f; t.tint[1] = 0.78f; t.tint[2] = 0.45f; t.tint[3] = 1.0f; // archive gold
    return t;
}

// L13 mutated flora (stationary lashing hostile). Pulled directly from the
// roster — the row is already STATIONARY (chaseSpeed=0); we just tint it sicker.
MonsterSystem::Tuning floraTuning() {
    MonsterSystem::Tuning t = act2EnemyTuning(Act2EnemyType::MutatedFlora);
    t.tint[0] = 0.40f; t.tint[1] = 0.72f; t.tint[2] = 0.30f; t.tint[3] = 1.0f; // toxic green
    return t;
}

// L14 mutated scientist (ranged chemical attacker; the row provides ranged AI).
MonsterSystem::Tuning scientistTuning() {
    MonsterSystem::Tuning t = act2EnemyTuning(Act2EnemyType::MutatedScientist);
    t.tint[0] = 0.85f; t.tint[1] = 0.95f; t.tint[2] = 0.60f; t.tint[3] = 1.0f; // sickly yellow
    return t;
}

// L14 Siren ambush boss (Beta — transformed Aria). Built straight from the
// Act-2 boss roster; we tint slightly so the Siren reads as a HOSTILE arrival
// inside the otherwise neutral station palette.
MonsterSystem::Tuning sirenAmbushTuning() {
    MonsterSystem::Tuning t = act2BossTuning(Act2BossType::TheSiren);
    // Roster tint already siren-magenta; nudge to a darker, more menacing hue.
    t.tint[0] = 0.95f; t.tint[1] = 0.35f; t.tint[2] = 0.85f; t.tint[3] = 1.0f;
    return t;
}

// L12 Memory Hunter boss. Pulled straight from the Act-2 boss roster —
// copyFeintPhase is already set on the row (so inCopyFeintPhase() reports the
// data tag at the right phase).
MonsterSystem::Tuning memoryHunterTuning() {
    return act2BossTuning(Act2BossType::MemoryHunter);
}

// ---- Plain Scene prop helpers ---------------------------------------------
// A violet, strongly-emissive crystal "heart" prop the L12 chamber centers on.
// Pure visual (no physics body) — mirrors the L9 crystal-formation pattern in
// act2_world.cpp exactly.
uint32_t addCrystalHeartProp(Scene& scene, x3::rhi::IRenderDevice& device,
                             const x3::phys::Vec3& center) {
    const float halfH = 1.5f;
    x3::prims::PrimMesh m = x3::prims::makeBox(1.4f, halfH, 1.4f,
                                                center.x, center.y + halfH, center.z);
    Entity e;
    e.mesh = device.createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                               m.index.data(), (uint32_t)m.index.size());
    e.baseColor[0] = 0.80f; e.baseColor[1] = 0.45f; e.baseColor[2] = 1.00f; e.baseColor[3] = 1.0f;
    // STRONG violet emission — the Heart reads as a HUD-noticeable glow.
    e.emissive[0] = 0.95f; e.emissive[1] = 0.40f; e.emissive[2] = 1.00f; e.emissive[3] = 3.5f;
    e.tag = (uint32_t)Tag::Prop;
    return scene.add(e);
}

// A Tree-Cities canopy platform (a wide thin emissive slab — readable as a
// floor a host can wire into the navmesh later). Returns the entity id.
uint32_t addTreeCityPlatform(Scene& scene, x3::rhi::IRenderDevice& device,
                             const x3::phys::Vec3& center) {
    const float halfX = 8.0f, halfY = 0.25f, halfZ = 8.0f;
    x3::prims::PrimMesh m = x3::prims::makeBox(halfX, halfY, halfZ,
                                                center.x, center.y - halfY, center.z);
    Entity e;
    e.mesh = device.createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                               m.index.data(), (uint32_t)m.index.size());
    e.baseColor[0] = 0.45f; e.baseColor[1] = 0.55f; e.baseColor[2] = 0.30f; e.baseColor[3] = 1.0f;
    // Soft amber from canopy lanterns underneath each platform.
    e.emissive[0] = 0.50f; e.emissive[1] = 0.35f; e.emissive[2] = 0.10f; e.emissive[3] = 0.80f;
    e.tag = (uint32_t)Tag::Prop;
    return scene.add(e);
}

// L15 Trading-post pillar (a marked Scene prop at a known position the host
// wires an E-interact to). Tall narrow box with warm gold emission so it reads
// as a beckon in the canopy.
uint32_t addTradingPostProp(Scene& scene, x3::rhi::IRenderDevice& device,
                            const x3::phys::Vec3& base) {
    const float halfH = 2.0f;
    x3::prims::PrimMesh m = x3::prims::makeBox(0.6f, halfH, 0.6f,
                                                base.x, base.y + halfH, base.z);
    Entity e;
    e.mesh = device.createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                               m.index.data(), (uint32_t)m.index.size());
    e.baseColor[0] = 0.85f; e.baseColor[1] = 0.65f; e.baseColor[2] = 0.25f; e.baseColor[3] = 1.0f;
    e.emissive[0] = 1.00f; e.emissive[1] = 0.70f; e.emissive[2] = 0.20f; e.emissive[3] = 2.0f;
    e.tag = (uint32_t)Tag::Prop;
    return scene.add(e);
}

} // namespace

void Act2Caves::build(Scene& scene, x3::rhi::IRenderDevice& device,
                      x3::phys::IPhysicsWorld& physics, TriggerSystem& triggers,
                      std::string_view modelDir) {
    m_modelDir = std::string(modelDir);

    // ===================================================================
    // AREA FRAMEWORK — name/biome/objective for every level this module owns.
    // ===================================================================
    auto setBase = [&](Act2CaveLevel lvl, const char* name, const char* biome,
                       const char* obj) {
        Act2CaveAreaPlan& p = m_plan[(uint32_t)lvl - kAct2CavesFirstLevel];
        p.level = lvl; p.name = name; p.biome = biome; p.objective = obj;
    };
    setBase(Act2CaveLevel::L12_AdvancedCaveSystem,
            "Advanced Cave System",
            "Bioluminescent multi-layer caves (Salvari Archives, Crystal Heart)",
            "Reach the Crystal Heart chamber and confront the Memory Hunter");
    setBase(Act2CaveLevel::L13_ToxicSwamplandsEdge,
            "Toxic Swamplands Edge",
            "Failed-terraform mutation swamp (poison hazards)",
            "Cross the toxic swamplands edge");
    setBase(Act2CaveLevel::L14_ResearchStation,
            "Research Station",
            "Abandoned swamp station (mutated scientists, possible Beta ambush)",
            "Search the abandoned research station");
    setBase(Act2CaveLevel::L15_TreeCities,
            "Tree Cities",
            "Canopy city (vertical traversal, trading post)",
            "Climb to the tree cities and reach the trading post");

    // ===================================================================
    // L12 — ADVANCED CAVE SYSTEM. Bioluminescent multi-layer caves: upper
    // caves (cave-fauna pack) -> Salvari Archives reading room (allied markers)
    // -> CRYSTAL HEART CHAMBER (dual-gated interactable, story-branch flag) ->
    // abyss boss arena (Memory Hunter).
    // ===================================================================
    {
        Act2CaveAreaPlan& p = m_plan[(uint32_t)Act2CaveLevel::L12_AdvancedCaveSystem -
                                     kAct2CavesFirstLevel];
        p.implemented = true;
        p.spawn       = at(kL12X, kRegionZ, kL12CaveBaseY, 0.05f);
        p.footprintX  = 200.0f;   // upper cave entry through to the abyss arena
        p.footprintZ  = 40.0f;
        m_l12Spawn    = p.spawn;

        // UPPER CAVE pack: 4 hostile cave-fauna placed off the entry spine so an
        // arriving player isn't ambushed inside the portal.
        const float ux[4] = { kL12X + 20.0f, kL12X + 35.0f, kL12X + 50.0f, kL12X + 60.0f };
        const float uz[4] = { kRegionZ - 4.0f, kRegionZ + 5.0f, kRegionZ - 3.5f, kRegionZ + 4.5f };
        for (int i = 0; i < 4; ++i)
            m_l12Enemies.spawn(scene, device, physics, m_modelDir,
                                at(ux[i], uz[i], kL12CaveBaseY, kAct2CavesEnemyYOff),
                                caveFaunaTuning());
        p.meleeCount = 4;   // cave fauna are melee chargers (Guard archetype)

        // SALVARI ARCHIVES (a reading-room past the cave-fauna pack): 3 allied
        // Salvari markers, stationary. Already start allied from the row's
        // Tuning::startAllied tag (no need to call convertToAllied here).
        const float ax[3] = { kL12X + 80.0f, kL12X + 88.0f, kL12X + 96.0f };
        const float az[3] = { kRegionZ - 2.0f, kRegionZ + 2.0f, kRegionZ + 0.0f };
        for (int i = 0; i < 3; ++i) {
            uint32_t idx = m_l12Allies.spawn(scene, device, physics, m_modelDir,
                                             at(ax[i], az[i], kL12ArchivesBaseY, kAct2CavesEnemyYOff),
                                             archivesAllyTuning());
            // Defensive: re-flip in case the data row's startAllied tag drifts.
            m_l12Allies.at(idx).convertToAllied();
        }
        p.allyCount = m_l12Allies.count();

        // CRYSTAL HEART CHAMBER — the dual-gated interactable. Heart sits at
        // the chamber center; the chamber is INERT at load (both gates false,
        // activated==false). The story-branch flag flips on the first valid
        // activate() (gated by the test/host).
        m_l12HeartRoomPos = at(kL12HeartX, kRegionZ, kL12HeartBaseY, 0.0f);
        m_crystalHeart.worldPos = m_l12HeartRoomPos;
        m_crystalHeart.propEntity = addCrystalHeartProp(scene, device, m_l12HeartRoomPos);
        m_crystalHeart.strengthGate = false;
        m_crystalHeart.hackGate     = false;
        m_crystalHeart.activated    = false;
        m_crystalHeart.storyBranch  = false;
        p.hasInteract = true;
        p.propCount   = 1;

        // ABYSS BOSS ARENA — MEMORY HUNTER (single boss, the L12 capstone).
        const x3::phys::Vec3 bossPos = at(kL12ArenaX, kRegionZ, kL12AbyssBaseY,
                                          kAct2CavesEnemyYOff);
        m_l12Boss.spawn(scene, device, physics, m_modelDir, bossPos, memoryHunterTuning());
        p.bossCount = m_l12Boss.count();

        m_l12CaveExit = at(kL12ExitX, kRegionZ, kL12AbyssBaseY, 0.05f);
    }

    // ===================================================================
    // L13 — TOXIC SWAMPLANDS EDGE. Mutated flora hazards (stationary lash) +
    // a poison/exposure hazard zone. Hazard PRESENT but INERT at load.
    // ===================================================================
    {
        Act2CaveAreaPlan& p = m_plan[(uint32_t)Act2CaveLevel::L13_ToxicSwamplandsEdge -
                                     kAct2CavesFirstLevel];
        p.implemented = true;
        p.spawn       = at(kL13X, kRegionZ, kL13SwampBaseY, 0.05f);
        p.footprintX  = 140.0f;
        p.footprintZ  = 120.0f;
        m_l13Spawn    = p.spawn;

        // MUTATED FLORA — 5 stationary lashers spread across the swamp edge.
        const float fx[5] = { kL13X + 25, kL13X + 50, kL13X + 75, kL13X + 95, kL13X + 115 };
        const float fz[5] = { kRegionZ - 12.0f, kRegionZ + 9.0f, kRegionZ - 6.0f,
                              kRegionZ + 14.0f, kRegionZ - 18.0f };
        for (int i = 0; i < 5; ++i)
            m_l13Enemies.spawn(scene, device, physics, m_modelDir,
                                at(fx[i], fz[i], kL13SwampBaseY, kAct2CavesEnemyYOff),
                                floraTuning());
        p.meleeCount = 5;   // flora is stationary melee-reach (lash)

        // POISON HAZARD ZONE — an AABB centered on the swamp's pooled water;
        // INERT at load (active=false, exposure=0). Arms only on first entry
        // OR on the L13PoisonHazard trigger.
        const float hxz = 35.0f;
        m_poison.min        = x3::phys::Vec3{ kL13HazardX - hxz, kL13SwampBaseY - 5.0f,
                                              kRegionZ - hxz };
        m_poison.max        = x3::phys::Vec3{ kL13HazardX + hxz, kL13SwampBaseY + 20.0f,
                                              kRegionZ + hxz };
        m_poison.active     = false;
        m_poison.exposure   = 0.0f;
        m_poison.ratePerSec = kPoisonExposureRate;
        p.hasHazard         = true;

        m_l13SwampExit = at(kL13ExitX, kRegionZ, kL13SwampBaseY, 0.05f);
    }

    // ===================================================================
    // L14 — RESEARCH STATION. Mutated scientists (always present). The Beta
    // SIREN AMBUSH is TIMELINE-GATED: present only if m_sirenGate==true (F2
    // women unsaved). When the gate is false, no Siren spawn happens (the
    // room is the standard scientists encounter only).
    // ===================================================================
    {
        Act2CaveAreaPlan& p = m_plan[(uint32_t)Act2CaveLevel::L14_ResearchStation -
                                     kAct2CavesFirstLevel];
        p.implemented = true;
        p.spawn       = at(kL14X, kRegionZ, kL14StationBaseY, 0.05f);
        p.footprintX  = 110.0f;
        p.footprintZ  = 50.0f;
        m_l14Spawn    = p.spawn;
        p.sirenGated  = m_sirenGate;

        // MUTATED SCIENTISTS — 4 ranged chemical attackers along the station
        // corridor. Always present regardless of the timeline flag.
        const float sx[4] = { kL14X + 18, kL14X + 36, kL14X + 60, kL14X + 82 };
        const float sz[4] = { kRegionZ - 4.0f, kRegionZ + 5.0f, kRegionZ - 7.0f, kRegionZ + 3.0f };
        for (int i = 0; i < 4; ++i)
            m_l14Enemies.spawn(scene, device, physics, m_modelDir,
                                at(sx[i], sz[i], kL14StationBaseY, kAct2CavesEnemyYOff),
                                scientistTuning());
        p.rangedCount = 4;   // scientists are ranged

        // TIMELINE-GATED SIREN AMBUSH. Only placed if the F2 women were lost
        // (m_sirenGate==true). Spawn position is the ambush room past the
        // scientists.
        if (m_sirenGate) {
            const x3::phys::Vec3 sirenPos = at(kL14SirenX, kRegionZ,
                                               kL14StationBaseY, kAct2CavesEnemyYOff);
            m_l14Siren.spawn(scene, device, physics, m_modelDir, sirenPos,
                              sirenAmbushTuning());
            p.bossCount = 1;
        }

        m_l14StationExit = at(kL14ExitX, kRegionZ, kL14StationBaseY, 0.05f);
    }

    // ===================================================================
    // L15 — TREE CITIES. Vertical canopy graybox: 3 platforms at rising
    // heights (climb route) + a trading-post pillar at the top platform.
    // ===================================================================
    {
        Act2CaveAreaPlan& p = m_plan[(uint32_t)Act2CaveLevel::L15_TreeCities -
                                     kAct2CavesFirstLevel];
        p.implemented = true;
        p.spawn       = at(kL15X, kRegionZ, kL15GroundBaseY, 0.05f);
        p.footprintX  = 80.0f;
        p.footprintZ  = 80.0f;
        m_l15Spawn    = p.spawn;

        // Three canopy platforms at RISING heights along +X. Each is wide
        // enough for the host to wire a navmesh patch onto later.
        for (uint32_t i = 0; i < kTreeCityPlatformCount; ++i) {
            const float platX = kL15X + 10.0f + 22.0f * (float)i;
            const float platY = kL15CanopyBaseY + (float)i * kL15PlatformLift;
            const x3::phys::Vec3 pos = at(platX, kRegionZ + 4.0f, platY, 0.0f);
            m_treePlatformPos.push_back(pos);
            m_treePlatforms.push_back(addTreeCityPlatform(scene, device, pos));
        }

        // TRADING POST — a marked Scene prop on the TOP platform (the host
        // wires an E-interact to this prop's worldPos within range).
        const x3::phys::Vec3 topPlat = m_treePlatformPos.back();
        m_tradingPostPos = x3::phys::Vec3{ topPlat.x, topPlat.y + 0.05f, topPlat.z };
        m_tradingPostEntity = addTradingPostProp(scene, device, m_tradingPostPos);

        p.hasInteract = true;
        p.propCount   = kTreeCityPlatformCount + 1;  // platforms + trading post
    }

    // ===================================================================
    // TRIGGERS — L11 -> L12 -> L13 -> L14 -> L15 reachability + per-level
    // interactable/hazard arming. Fresh id range (100..108), non-colliding
    // with Act-1 (10..) / SpireMid (30/40/50) / Act-2 host (80..82).
    // ===================================================================
    {
        // L11 -> L12 cave portal (at the L12 spawn).
        const x3::phys::Vec3& sp12 = m_l12Spawn;
        triggers.add(x3::phys::Vec3{ sp12.x - 6.0f, sp12.y - 3.0f, sp12.z - 6.0f },
                     x3::phys::Vec3{ sp12.x + 6.0f, sp12.y + 8.0f, sp12.z + 6.0f },
                     (uint32_t)Act2CavesTrigger::L11toL12Portal, true);

        // L12 Crystal Heart Chamber reached (arms HUD prompt, never auto-activates).
        triggers.add(x3::phys::Vec3{ kL12HeartX - 8.0f, kL12HeartBaseY - 3.0f, kRegionZ - 8.0f },
                     x3::phys::Vec3{ kL12HeartX + 8.0f, kL12HeartBaseY + 8.0f, kRegionZ + 8.0f },
                     (uint32_t)Act2CavesTrigger::L12CrystalHeartRoom, true);

        // L12 Memory Hunter abyss arena entry.
        triggers.add(x3::phys::Vec3{ kL12ArenaX - 8.0f, kL12AbyssBaseY - 3.0f, kRegionZ - 10.0f },
                     x3::phys::Vec3{ kL12ArenaX + 8.0f, kL12AbyssBaseY + 8.0f, kRegionZ + 10.0f },
                     (uint32_t)Act2CavesTrigger::L12MemoryHunterArena, true);

        // L12 -> L13 transition (cave exit -> swamp edge).
        triggers.add(x3::phys::Vec3{ kL12ExitX - 6.0f, kL12AbyssBaseY - 3.0f, kRegionZ - 8.0f },
                     x3::phys::Vec3{ kL12ExitX + 6.0f, kL13SwampBaseY + 8.0f, kRegionZ + 8.0f },
                     (uint32_t)Act2CavesTrigger::L12toL13Transition, true);

        // L13 poison hazard zone (matches the hazard AABB footprint exactly).
        triggers.add(m_poison.min, m_poison.max,
                     (uint32_t)Act2CavesTrigger::L13PoisonHazard, true);

        // L13 -> L14 transition.
        triggers.add(x3::phys::Vec3{ kL13ExitX - 6.0f, kL13SwampBaseY - 3.0f, kRegionZ - 8.0f },
                     x3::phys::Vec3{ kL13ExitX + 6.0f, kL14StationBaseY + 8.0f, kRegionZ + 8.0f },
                     (uint32_t)Act2CavesTrigger::L13toL14Transition, true);

        // L14 Siren ambush trigger (always registered; firing it only "matters"
        // if the Siren was actually placed at build — the gate read here is at
        // build time, so a later host can also disable this trigger to mute it).
        triggers.add(x3::phys::Vec3{ kL14SirenX - 6.0f, kL14StationBaseY - 3.0f, kRegionZ - 8.0f },
                     x3::phys::Vec3{ kL14SirenX + 6.0f, kL14StationBaseY + 8.0f, kRegionZ + 8.0f },
                     (uint32_t)Act2CavesTrigger::L14SirenAmbush, true);

        // L14 -> L15 transition.
        triggers.add(x3::phys::Vec3{ kL14ExitX - 6.0f, kL14StationBaseY - 3.0f, kRegionZ - 8.0f },
                     x3::phys::Vec3{ kL14ExitX + 6.0f, kL15GroundBaseY + 8.0f, kRegionZ + 8.0f },
                     (uint32_t)Act2CavesTrigger::L14toL15Transition, true);

        // L15 trading-post interact armer (at the top-platform prop position).
        const x3::phys::Vec3& tp = m_tradingPostPos;
        triggers.add(x3::phys::Vec3{ tp.x - 4.0f, tp.y - 3.0f, tp.z - 4.0f },
                     x3::phys::Vec3{ tp.x + 4.0f, tp.y + 6.0f, tp.z + 4.0f },
                     (uint32_t)Act2CavesTrigger::L15TradingPost, true);
    }

    m_built = true;
    x3::logInfo(
        std::string("Act2Caves::build complete — L12 ADVANCED CAVE SYSTEM (4 cave fauna + "
                    "3 Salvari Archives allies + Crystal Heart Chamber [inert] + Memory "
                    "Hunter abyss boss) + L13 TOXIC SWAMPLANDS EDGE (5 mutated flora + "
                    "poison hazard [inert]) + L14 RESEARCH STATION (4 mutated scientists") +
        (m_sirenGate ? " + Siren ambush [F2 women lost])" : " [no Siren ambush — F2 women saved])") +
        " + L15 TREE CITIES (3 canopy platforms + trading post); triggers 100..108 registered");
}

void Act2Caves::tick(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                     const x3::phys::Vec3& /*eye*/, const x3::phys::Vec3& playerPos,
                     IDamageSink* player, const AttackFxFn& attackFx) {
    if (!m_built) return;

    IDamageSink* atk = (player && player->isAlive()) ? player : nullptr;

    // L12 — hostile cave-fauna pack + Memory Hunter (boss has phase machine).
    m_l12Enemies.update(dt, scene, physics, playerPos, atk, attackFx);
    m_l12Boss.update(dt, scene, physics, playerPos, atk, attackFx);
    // Allied Salvari archivists: movement-only (stationary anyway).
    m_l12Allies.update(dt, scene, physics, playerPos);

    // L13 — flora (stationary lash) hostile updates + the poison hazard tick.
    m_l13Enemies.update(dt, scene, physics, playerPos, atk, attackFx);
    if (m_poison.contains(playerPos)) {
        m_poison.active = true;
        m_poison.exposure += m_poison.ratePerSec * dt;
    }

    // L14 — mutated scientists + (if gated) the Siren ambush.
    m_l14Enemies.update(dt, scene, physics, playerPos, atk, attackFx);
    if (m_l14Siren.count() > 0)
        m_l14Siren.update(dt, scene, physics, playerPos, atk, attackFx);

    // L15 — no hostiles to update; the platforms + trading post are static props.
}

void Act2Caves::onTrigger(uint32_t triggerId) {
    switch ((Act2CavesTrigger)triggerId) {
        case Act2CavesTrigger::L11toL12Portal:
            if (!m_l12Reached) {
                m_l12Reached = true;
                x3::logInfo("Act2Caves: L11 -> L12 ADVANCED CAVE SYSTEM (bioluminescent caves)");
            }
            break;
        case Act2CavesTrigger::L12CrystalHeartRoom:
            x3::logInfo("Act2Caves: L12 Crystal Heart Chamber reached "
                        "(interactable inert until both strength + hack gates)");
            break;
        case Act2CavesTrigger::L12MemoryHunterArena:
            if (!m_memHunterArenaReached) {
                m_memHunterArenaReached = true;
                x3::logInfo("Act2Caves: L12 ABYSS ARENA — Memory Hunter encounter armed");
            }
            break;
        case Act2CavesTrigger::L12toL13Transition:
            if (!m_l13Reached) {
                m_l13Reached = true;
                x3::logInfo("Act2Caves: L12 -> L13 TOXIC SWAMPLANDS EDGE");
            }
            break;
        case Act2CavesTrigger::L13PoisonHazard:
            if (!m_poison.active) {
                m_poison.active = true;
                x3::logInfo("Act2Caves: L13 poison hazard zone entered — exposure tracked");
            }
            break;
        case Act2CavesTrigger::L13toL14Transition:
            if (!m_l14Reached) {
                m_l14Reached = true;
                x3::logInfo("Act2Caves: L13 -> L14 RESEARCH STATION");
            }
            break;
        case Act2CavesTrigger::L14SirenAmbush:
            if (sirenAmbushPresent())
                x3::logInfo("Act2Caves: L14 Siren ambush sprung (F2 women lost — Beta path)");
            else
                x3::logInfo("Act2Caves: L14 ambush trigger fired but Siren is gated off "
                            "(F2 women saved — normal encounter)");
            break;
        case Act2CavesTrigger::L14toL15Transition:
            if (!m_l15Reached) {
                m_l15Reached = true;
                x3::logInfo("Act2Caves: L14 -> L15 TREE CITIES");
            }
            break;
        case Act2CavesTrigger::L15TradingPost:
            x3::logInfo("Act2Caves: L15 trading post interactable in range");
            break;
    }
}

FireResult Act2Caves::onFire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                             Scene& scene, x3::phys::IPhysicsWorld& physics) {
    // Try each hostile group in turn; first hit wins. Allies + Tree-City props
    // are NOT registered as targets here (allies live in m_l12Allies, plain
    // Scene props are not in any monster manager).
    FireResult r = m_l12Enemies.fire(eye, dir, scene, physics);
    if (r.hit) return r;
    r = m_l12Boss.fire(eye, dir, scene, physics);
    if (r.hit) return r;
    r = m_l13Enemies.fire(eye, dir, scene, physics);
    if (r.hit) return r;
    r = m_l14Enemies.fire(eye, dir, scene, physics);
    if (r.hit) return r;
    if (m_l14Siren.count() > 0) {
        r = m_l14Siren.fire(eye, dir, scene, physics);
        if (r.hit) return r;
    }
    return r;  // last (miss) result
}

void Act2Caves::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                     const Scene& scene) const {
    m_l12Enemies.drawAll(device, frame, scene);
    m_l12Allies.drawAll(device, frame, scene);
    m_l12Boss.drawAll(device, frame, scene);
    m_l13Enemies.drawAll(device, frame, scene);
    m_l14Enemies.drawAll(device, frame, scene);
    if (m_l14Siren.count() > 0) m_l14Siren.drawAll(device, frame, scene);
}

bool Act2Caves::transitionReachable(const TriggerSystem& triggers,
                                    Act2CavesTrigger link) const {
    if (!m_built) return false;
    const TriggerVolume* t = triggers.findById((uint32_t)link);
    if (!t || !t->enabled) return false;
    const float tcx = 0.5f * (t->min.x + t->max.x);

    // Each link must sit spatially between its source exit / dest spawn so the
    // player progresses +X across it. The boundaries are inclusive within an
    // 8 m slop — same threshold act2_world.transitionReachable() uses.
    float srcX = 0.0f, dstX = 0.0f;
    switch (link) {
        case Act2CavesTrigger::L11toL12Portal:
            // From the L11 (Salvari Camp) approach to the L12 cave spawn. We
            // don't author L11 here, so the source X is "anything below L12 spawn".
            srcX = m_l12Spawn.x - 50.0f;
            dstX = m_l12Spawn.x + 8.0f;
            break;
        case Act2CavesTrigger::L12toL13Transition:
            srcX = m_l12CaveExit.x - 8.0f; dstX = m_l13Spawn.x + 8.0f; break;
        case Act2CavesTrigger::L13toL14Transition:
            srcX = m_l13SwampExit.x - 8.0f; dstX = m_l14Spawn.x + 8.0f; break;
        case Act2CavesTrigger::L14toL15Transition:
            srcX = m_l14StationExit.x - 8.0f; dstX = m_l15Spawn.x + 8.0f; break;
        default:
            // Non-transition triggers (interactables / hazards / ambush) just
            // need to exist + be enabled.
            return true;
    }
    if (tcx < srcX || tcx > dstX) return false;
    const float gap = dstX - srcX;
    return gap > 0.0f && gap < 200.0f;
}

bool Act2Caves::allTransitionsReachable(const TriggerSystem& triggers) const {
    return transitionReachable(triggers, Act2CavesTrigger::L11toL12Portal)
        && transitionReachable(triggers, Act2CavesTrigger::L12toL13Transition)
        && transitionReachable(triggers, Act2CavesTrigger::L13toL14Transition)
        && transitionReachable(triggers, Act2CavesTrigger::L14toL15Transition);
}

// ===========================================================================
// Headless self-test (--test-act2caves).
// ===========================================================================
namespace {

int gc_pass = 0, gc_fail = 0;
void cvCheck(bool cond, const char* name) {
    if (cond) { ++gc_pass; x3::logInfo(std::string("[act2caves-test] PASS ") + name); }
    else      { ++gc_fail; x3::logError(std::string("[act2caves-test] FAIL ") + name); }
}

constexpr float kFixedDt = 1.0f / 60.0f;
using HeadlessDevice = x3::game::HeadlessRenderDevice;
bool sameName(const char* a, const char* b) { return std::string(a) == std::string(b); }

} // namespace

bool runAct2CavesSelfTest() {
    gc_pass = gc_fail = 0;

    // ---- BUILD #1: timeline flag TRUE (F2 women lost) -> Siren ambush present.
    std::unique_ptr<x3::phys::IPhysicsWorld> physA(x3::phys::createPhysicsWorld());
    physA->init();
    HeadlessDevice deviceA;
    Scene sceneA;
    TriggerSystem triggersA;
    Act2Caves worldA;
    worldA.setSirenAmbushGate(true);
    worldA.build(sceneA, deviceA, *physA, triggersA, riggedGlbRoot());

    cvCheck(worldA.built(), "C0 Act-2 caves module built (siren-gate=true)");

    // ---- Area framework: L12..L15 all named + objective'd + implemented. ----
    {
        bool allNamed = true, allImpl = true;
        for (uint32_t lv = kAct2CavesFirstLevel; lv <= kAct2CavesLastLevel; ++lv) {
            const Act2CaveAreaPlan& p = worldA.plan((Act2CaveLevel)lv);
            if (std::string(p.name).empty() || std::string(p.objective).empty()) allNamed = false;
            if (!p.implemented) allImpl = false;
        }
        cvCheck(allNamed && allImpl,
                "C1 all 4 levels (L12..L15) carry name + objective + are implemented");

        const Act2CaveAreaPlan& l12 = worldA.plan(Act2CaveLevel::L12_AdvancedCaveSystem);
        bool l12ok = sameName(l12.name, "Advanced Cave System") && l12.footprintX > 0.0f &&
                     l12.hasInteract && l12.bossCount == 1 && l12.allyCount == 3 &&
                     l12.meleeCount == 4 && l12.propCount >= 1 &&
                     (l12.spawn.x != 0.0f || l12.spawn.z != 0.0f);
        cvCheck(l12ok, "C2 L12: footprint + spawn + Crystal Heart interact + boss + allies");

        const Act2CaveAreaPlan& l13 = worldA.plan(Act2CaveLevel::L13_ToxicSwamplandsEdge);
        bool l13ok = sameName(l13.name, "Toxic Swamplands Edge") && l13.hasHazard &&
                     l13.meleeCount == 5 && l13.footprintX > 0.0f;
        cvCheck(l13ok, "C3 L13: swamp footprint + 5 mutated flora + poison hazard present");

        const Act2CaveAreaPlan& l14 = worldA.plan(Act2CaveLevel::L14_ResearchStation);
        bool l14ok = sameName(l14.name, "Research Station") && l14.rangedCount == 4 &&
                     l14.sirenGated == true && l14.bossCount == 1;
        cvCheck(l14ok, "C4 L14: station + 4 scientists + Siren ambush placed (flag=true)");

        const Act2CaveAreaPlan& l15 = worldA.plan(Act2CaveLevel::L15_TreeCities);
        bool l15ok = sameName(l15.name, "Tree Cities") && l15.hasInteract &&
                     l15.propCount == (kTreeCityPlatformCount + 1);
        cvCheck(l15ok, "C5 L15: tree cities + trading post + 3+1 props");
    }

    // ---- Memory Hunter present at L12 + carries copy/feint phase tag. ----
    {
        bool ok = worldA.memoryHunterPresent() && worldA.l12Boss().count() == 1 &&
                  worldA.l12Boss().aliveCount() == 1;
        const MonsterSystem& mh = worldA.l12Boss().at(0);
        ok = ok && mh.copyFeintPhase() > 0;   // data tag is set on the row
        cvCheck(ok, "C6 L12 Memory Hunter present in abyss arena (copyFeintPhase>0)");
    }

    // ---- Salvari Archives: 3 allied, attackDamage 0. ----
    {
        bool ok = worldA.l12Allies().count() == 3 && worldA.l12Allies().aliveCount() == 3;
        for (uint32_t i = 0; i < worldA.l12Allies().count(); ++i) {
            const MonsterSystem& a = worldA.l12Allies().at(i);
            if (!a.isAllied() || a.attackDamage() != 0) ok = false;
        }
        cvCheck(ok, "C7 L12 Salvari Archives: 3 allied markers (non-hostile)");
    }

    // ---- Crystal Heart Chamber: inert at load; ARMED only when BOTH gates set;
    //      activate() flips activated + storyBranch (idempotent on a 2nd call). ----
    {
        const CrystalHeartChamber& ch = worldA.crystalHeart();
        cvCheck(!ch.strengthGate && !ch.hackGate && !ch.activated && !ch.storyBranch &&
                !ch.canActivate(),
                "C8 Crystal Heart inert at load (no gates, not activated, canActivate=false)");

        // Flip ONLY the strength gate -> stays inert (one gate is not enough).
        worldA.crystalHeart().strengthGate = true;
        cvCheck(!worldA.crystalHeart().canActivate() &&
                !worldA.crystalHeart().activate() && !worldA.crystalHeart().activated,
                "C9 only-strength gate set: chamber stays INERT (needs hack too)");

        // Flip ONLY the hack gate (clearing strength first) -> still inert.
        worldA.crystalHeart().strengthGate = false;
        worldA.crystalHeart().hackGate     = true;
        cvCheck(!worldA.crystalHeart().canActivate() &&
                !worldA.crystalHeart().activate() && !worldA.crystalHeart().activated,
                "C10 only-hack gate set: chamber stays INERT (needs strength too)");

        // Flip BOTH gates -> arms; activate() succeeds + latches storyBranch.
        worldA.crystalHeart().strengthGate = true;
        cvCheck(worldA.crystalHeart().canActivate(),
                "C11 BOTH gates set: chamber ARMED (canActivate=true)");
        bool first = worldA.crystalHeart().activate();
        cvCheck(first && worldA.crystalHeart().activated && worldA.crystalHeart().storyBranch,
                "C12 activate() succeeds + latches activated + storyBranch flag");
        bool second = worldA.crystalHeart().activate();
        cvCheck(!second && worldA.crystalHeart().activated,
                "C13 activate() is idempotent (2nd call no-op, latches remain)");
    }

    // ---- L13 poison hazard: present + inert at load; stays inert outside;
    //      arms + accumulates exposure once player enters. ----
    {
        const PoisonHazardZone& hz = worldA.poisonHazard();
        cvCheck(!hz.active && hz.exposure == 0.0f,
                "C14 L13 poison hazard present but INERT at load");

        // Tick OUTSIDE the hazard: stays inert.
        const Act2CaveAreaPlan& l13 = worldA.plan(Act2CaveLevel::L13_ToxicSwamplandsEdge);
        x3::phys::Vec3 outside{ l13.spawn.x, l13.spawn.y, l13.spawn.z };
        for (int i = 0; i < 60; ++i)
            worldA.tick(kFixedDt, sceneA, *physA, outside, outside, nullptr, AttackFxFn{});
        cvCheck(!worldA.poisonHazardActive() && worldA.poisonHazardExposure() == 0.0f,
                "C15 hazard stays inert while player outside zone");

        // Tick INSIDE the hazard: arms + exposure climbs.
        x3::phys::Vec3 inside{ 0.5f * (hz.min.x + hz.max.x),
                               0.5f * (hz.min.y + hz.max.y),
                               0.5f * (hz.min.z + hz.max.z) };
        for (int i = 0; i < 60; ++i)
            worldA.tick(kFixedDt, sceneA, *physA, inside, inside, nullptr, AttackFxFn{});
        cvCheck(worldA.poisonHazardActive() && worldA.poisonHazardExposure() > 0.0f,
                "C16 entering L13 hazard arms it + accumulates exposure");
    }

    // ---- L14 Siren ambush IS present in this build (sirenGate=true). ----
    cvCheck(worldA.sirenAmbushPresent() && worldA.l14SirenBoss().count() == 1,
            "C17 L14 Siren ambush PRESENT when timeline flag = TRUE (F2 women lost)");

    // ---- L15 Tree Cities: 3 platforms at RISING Y + trading-post prop present. ----
    {
        bool ok = worldA.treeCityPlatformCount() == kTreeCityPlatformCount &&
                  worldA.tradingPostPresent();
        float prevY = -1e9f;
        for (uint32_t i = 0; i < worldA.treeCityPlatformCount(); ++i) {
            const x3::phys::Vec3 p = worldA.treeCityPlatformPos(i);
            if (!(p.y > prevY)) ok = false;   // strictly rising
            prevY = p.y;
        }
        cvCheck(ok, "C18 L15 Tree Cities: 3 rising platforms + trading-post prop");
    }

    // ---- Story beats + L11 -> L12 -> L13 -> L14 -> L15 reachability. ----
    {
        cvCheck(!worldA.l12Reached() && !worldA.l13Reached() && !worldA.l14Reached() &&
                !worldA.l15Reached(),
                "C19 transition beats not latched at load");
        worldA.onTrigger((uint32_t)Act2CavesTrigger::L11toL12Portal);
        worldA.onTrigger((uint32_t)Act2CavesTrigger::L12toL13Transition);
        worldA.onTrigger((uint32_t)Act2CavesTrigger::L13toL14Transition);
        worldA.onTrigger((uint32_t)Act2CavesTrigger::L14toL15Transition);
        cvCheck(worldA.l12Reached() && worldA.l13Reached() && worldA.l14Reached() &&
                worldA.l15Reached(),
                "C20 all 4 transition triggers latch their beats");
        cvCheck(worldA.allTransitionsReachable(triggersA),
                "C21 L11 -> L12 -> L13 -> L14 -> L15 all reachable via labelled triggers");
    }

    // ---- Trigger ID range non-collision: every act2_caves trigger id sits
    // outside the Act-2 host's L8/L9 range (80..82) AND outside SpireMid's
    // 30/40/50 hubs AND outside the Act-1 L1Trigger 10..29 range. ----
    {
        bool ok = true;
        for (uint32_t id = kAct2CavesTrigBase;
             id < kAct2CavesTrigBase + kAct2CavesTrigCount; ++id)
        {
            if (id >= 80 && id <= 82)   { ok = false; break; }   // Act-2 host
            if (id == 30 || id == 40 || id == 50) { ok = false; break; }   // SpireMid hubs
            if (id < 100) { ok = false; break; }                 // below our base
        }
        cvCheck(ok,
                "C22 trigger ids 100..108 do not collide with Act-1/SpireMid/Act-2 host");
    }

    physA->shutdown();

    // ---- BUILD #2: timeline flag FALSE (F2 women SAVED) -> NO Siren placed. ----
    std::unique_ptr<x3::phys::IPhysicsWorld> physB(x3::phys::createPhysicsWorld());
    physB->init();
    HeadlessDevice deviceB;
    Scene sceneB;
    TriggerSystem triggersB;
    Act2Caves worldB;
    worldB.setSirenAmbushGate(false);
    worldB.build(sceneB, deviceB, *physB, triggersB, riggedGlbRoot());

    {
        const Act2CaveAreaPlan& l14b = worldB.plan(Act2CaveLevel::L14_ResearchStation);
        bool ok = worldB.built() && !worldB.sirenAmbushPresent() &&
                  worldB.l14SirenBoss().count() == 0 &&
                  l14b.sirenGated == false && l14b.bossCount == 0 &&
                  l14b.rangedCount == 4;   // scientists still present
        cvCheck(ok,
                "C23 L14 Siren ambush GATED OFF when flag = FALSE (F2 women saved); "
                "scientists still present");
    }

    physB->shutdown();

    x3::logInfo(std::string("act2caves: ") + std::to_string(gc_pass) + "/" +
                std::to_string(gc_pass + gc_fail) + " passed");
    return gc_fail == 0;
}

} // namespace x3::game
