#pragma once
// EFLZ Act 2 — alien-planet open-world surface HOST + the opening two levels
// (L8 Surface Emergence + L9 Crystalline Desert Edge). Game/slice content code
// only — engine/ stays pure.
//
// CLEAN-ROOM, original work. Built ONLY from X3Native's OWN systems (Scene,
// monster.*, trigger.*, terrain.*, mesh_prims) + the engine interfaces + the
// EFLZ design docs in docs/design/ (Tim's own IP). NO RBDOOM / id Tech / Doom /
// Quake — or ANY other game-engine — source was forked, copied, or consulted.
//
// SCOPE / OWNERSHIP: this lane owns the Act-2 open-world surface host and the
// first two levels. It does NOT touch the Act-1 floor files (level1.*, spire_*),
// monster.* (the data-driven roster + the Act-2-specific roster land separately),
// or the renderer/engine. It COMPOSES the existing roster + the procedural
// terrain world + the analytic sky + the trigger system into the alien surface:
//
//   * The surface "stands up" via the engine's own TerrainStreamer (an unbounded,
//     procedurally-generated tiled world) under an ALIEN-tuned analytic sky
//     (binary-sun / purple-atmosphere approximated through the existing SkyParams
//     sun-color + haze — NO new engine tech). Deserts have no sea level, so no
//     water is built for L8/L9 ("water where needed").
//
//   * A biome/area FRAMEWORK (Act2Level enum L8..L20 + a per-level Act2AreaPlan
//     descriptor: footprint / biome / spawn / objective + content counts) so the
//     later Act-2 levels slot in. Only L8 + L9 carry content this pass.
//
//   * L8 SURFACE EMERGENCE — a lab-exit tunnel gauntlet (5 Pursuit Drones [ranged,
//     on the BlueSynth profile] + 3 Infected Soldiers [melee, on the
//     DominionTrooper profile] — EXISTING roster types) opening onto "The
//     Emergence Point": an enemy-free safe-zone reveal (the awe beat) where the
//     surviving companions stand as allied markers (Sarah / Aria / Keisha / Emily,
//     spawned from the roster + convertToAllied()).
//
//   * L9 CRYSTALLINE DESERT EDGE — an open desert biome with singing-crystal
//     formation props (emissive Scene entities) + a couple of neutral fauna
//     placeholders (allied roster markers) + an environmental-hazard zone
//     (heat / sandstorm) modelled as an AABB + a tracked exposure stat that is
//     INERT until the player enters it (gated, never at load).
//
//   * REACHABILITY: L8 -> L9 progresses through a labelled transition trigger at
//     the edge of the Emergence Point (mirrors the Act-1 elevator/hub pattern).
//
// Authoring style mirrors spire_mid.* exactly: build() once, tick()/update() each
// frame, onTrigger() dispatch, draw() helpers, and plan()/query accessors read by
// the host HUD + the headless self-test (--test-act2) so nothing is re-derived.

#include "scene.h"
#include "monster.h"
#include "trigger.h"
#include "terrain.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/core/IJobSystem.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace x3::game {

// The Act-2 open-world levels (the alien-planet surface campaign). The enum value
// IS the canonical level number (L8..L20) so a plan lookup reads naturally. Only
// L8 + L9 are implemented this pass; the rest are declared (with their canon
// names/biomes) so later lanes drop in without renumbering.
enum class Act2Level : uint32_t {
    L8_SurfaceEmergence          = 8,
    L9_CrystallineDesertEdge     = 9,
    L10_CrystallineDesertDepths  = 10,
    L11_SalvariCamp              = 11,
    L12_AdvancedCaveSystem       = 12,
    L13_ToxicSwamplandsEdge      = 13,
    L14_ResearchStation          = 14,
    L15_TreeCities               = 15,
    L16_RuinedMetropolisOutskirts= 16,
    L17_Downtown                 = 17,
    L18_UndergroundResistance    = 18,
    L19_SpaceportApproach        = 19,
    L20_TheSpaceport             = 20,
};
constexpr uint32_t kAct2FirstLevel = 8;
constexpr uint32_t kAct2LastLevel  = 20;
constexpr uint32_t kAct2LevelCount = kAct2LastLevel - kAct2FirstLevel + 1; // 13

// Act-2 trigger event ids — a distinct range (80+) so this system can share one
// TriggerSystem with the Act-1 systems (L1Trigger / SpireMidTrigger 30/40/50)
// without colliding.
enum class Act2Trigger : uint32_t {
    L8EmergencePoint = 80,   // reached the Emergence Point safe zone (the awe beat)
    L8toL9Transition = 81,   // crossed the Emergence-Point edge into the L9 desert
    L9HazardZone     = 82,   // entered the L9 heat / sandstorm hazard zone
};

// Body-center Y above the surface for a placed humanoid/drone (the rigged GLBs sit
// ~0.4 m up — same as Level1Game::kEnemyY / SpireMid kEnemyYOff).
constexpr float kAct2EnemyYOff = 0.4f;

// Number of crystal-formation props placed across the L9 desert edge.
constexpr uint32_t kAct2CrystalCount = 6;

// One Act-2 area's authored descriptor (read by the host HUD + the self-test so
// they never re-derive footprint/spawn/objective/counts).
struct Act2AreaPlan {
    Act2Level   level       = Act2Level::L8_SurfaceEmergence;
    const char* name        = "";     // canon level name ("Surface Emergence", ...)
    const char* biome       = "";     // biome / setting one-liner
    const char* objective   = "";     // primary objective text
    bool        implemented = false;  // true for L8/L9 this pass

    x3::phys::Vec3 spawn{};            // where the player enters this area
    float       footprintX  = 0.0f;   // area extent in meters (X)
    float       footprintZ  = 0.0f;   // area extent in meters (Z)
    float       visibility  = 0.0f;   // open-reveal visibility (m); 0 = N/A

    uint32_t    meleeCount     = 0;   // melee enemies placed (Infected Soldiers)
    uint32_t    rangedCount    = 0;   // ranged enemies placed (Pursuit Drones)
    uint32_t    companionCount = 0;   // allied companion markers (L8 awe beat)
    uint32_t    crystalCount   = 0;   // crystal-formation props (L9)
    uint32_t    faunaCount     = 0;   // neutral fauna placeholders (L9)
    bool        hasHazard      = false; // an environmental-hazard zone is present
};

// An environmental-hazard zone (L9 heat / sandstorm): a world AABB + a tracked
// exposure stat. INERT until the player first enters (active=false, exposure=0 at
// load); once active, exposure accumulates while the player stands inside.
struct HazardZone {
    x3::phys::Vec3 min{};
    x3::phys::Vec3 max{};
    bool   active     = false;   // false until entered (gated, never at load)
    float  exposure   = 0.0f;    // tracked stat: accumulates while inside
    float  ratePerSec = 8.0f;    // exposure units / second while inside
    bool contains(const x3::phys::Vec3& p) const { return pointInBox(p, min, max); }
};

// The Act-2 open-world surface host. Build once after the player/render context
// exists; tick() each frame for the enemy/companion/fauna groups + the hazard
// stat, update() each frame to advance the terrain streamer around the focus.
class Act2World {
public:
    // Stand up the alien surface + author L8/L9. `triggers` is the host's shared
    // TriggerSystem (the host forwards fired Act2Trigger ids to onTrigger()).
    // `jobs` may be null (then terrain generation runs synchronously on the calling
    // thread — used by the headless self-test). `modelDir` is the loose rigged-GLB
    // dir (same one Level1Game/SpireMid receive). Call once.
    void build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, TriggerSystem& triggers,
               x3::jobs::IJobSystem* jobs, std::string_view modelDir);

    // Advance one frame: the L8 enemy gauntlet (attacks `player`), the allied
    // companion/fauna markers (movement only), and — once the hazard zone is
    // active OR the player is standing inside it — the tracked exposure stat.
    // `player` may be null (geometry/headless movement only).
    void tick(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
              const x3::phys::Vec3& eye, const x3::phys::Vec3& playerPos,
              IDamageSink* player, const AttackFxFn& attackFx);

    // Advance the terrain streamer's residency ring around the focus (player/cam)
    // world position. Returns tiles uploaded this frame. No-op before build().
    uint32_t update(Scene& scene, x3::rhi::IRenderDevice& device,
                    x3::phys::IPhysicsWorld& physics, float focusX, float focusZ);

    // Dispatch a fired Act2Trigger id the host forwards from its TriggerSystem.
    // Latches the emergence/transition beats and arms the L9 hazard. Idempotent.
    void onTrigger(uint32_t triggerId);

    // Fire one shot against the L8 enemy gauntlet (allied companions/fauna are NOT
    // valid targets, so they are excluded). Returns the result for FX/HUD.
    FireResult onFire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                      Scene& scene, x3::phys::IPhysicsWorld& physics);

    // Draw the L8 enemies + the allied companion/fauna markers (the crystal props
    // are plain Scene entities drawn by the host's scene.render()).
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
              const Scene& scene) const;

    // Tear down the terrain streamer's GPU + physics resources. Safe to call once.
    void shutdown(Scene& scene, x3::rhi::IRenderDevice& device,
                  x3::phys::IPhysicsWorld& physics);

    // ---- Queries (host HUD + the self-test) -------------------------------
    bool built() const { return m_built; }
    // The authored descriptor for an Act-2 level (footprint/spawn/objective/counts).
    const Act2AreaPlan& plan(Act2Level l) const { return m_plan[(uint32_t)l - kAct2FirstLevel]; }

    // L8 lab-exit gauntlet enemies (5 Pursuit Drones [ranged] + 3 Infected [melee]).
    const MonsterManager& l8Enemies() const { return m_l8Enemies; }
    MonsterManager&       l8Enemies()       { return m_l8Enemies; }
    // The Emergence-Point companion markers (allied; the awe beat).
    const MonsterManager& companions() const { return m_companions; }
    // The L9 neutral fauna placeholders (allied).
    const MonsterManager& fauna() const { return m_fauna; }
    // Count of crystal-formation props placed in the L9 desert.
    uint32_t crystalCount() const { return (uint32_t)m_crystals.size(); }

    // The L9 environmental-hazard zone (present at load, inert until entered).
    const HazardZone& hazard() const { return m_hazard; }
    bool  hazardActive()  const { return m_hazard.active; }
    float hazardExposure() const { return m_hazard.exposure; }

    // Story beats latched by onTrigger().
    bool emergenceReached() const { return m_emergenceReached; }
    bool l9Reached() const { return m_l9Reached; }
    // L8 -> L9 is reachable iff the labelled transition trigger is registered +
    // enabled in `triggers` and it sits between the L8 emergence exit and the L9
    // spawn (the player can cross it to progress). A content-side assertion.
    bool transitionReachable(const TriggerSystem& triggers) const;

    // The ALIEN sky tuning (the host applies it via device.setSkyParams). A
    // binary-sun / purple-atmosphere approximation through the existing SkyParams.
    const x3::rhi::IRenderDevice::SkyParams& alienSky() const { return m_sky; }

    // Terrain surface (the streamed alien world).
    const TerrainConfig& worldConfig() const { return m_cfg; }
    uint32_t residentTiles() const { return m_terrain.residentCount(); }
    bool focusTileResident(float x, float z) const { return m_terrain.focusTileResident(x, z); }
    TerrainStreamer&       terrain()       { return m_terrain; }
    const TerrainStreamer& terrain() const { return m_terrain; }

private:
    bool        m_built = false;
    std::string m_modelDir;

    Act2AreaPlan m_plan[kAct2LevelCount];

    MonsterManager m_l8Enemies;   // L8 lab-exit gauntlet (hostile)
    MonsterManager m_companions;  // Emergence-Point companion markers (allied)
    MonsterManager m_fauna;       // L9 neutral fauna placeholders (allied)
    std::vector<uint32_t> m_crystals;   // Scene entity ids of the L9 crystal props

    HazardZone  m_hazard;         // L9 heat / sandstorm zone

    TerrainStreamer m_terrain;    // the alien surface (streamed procedural world)
    TerrainConfig   m_cfg;        // the world config (== worldTerrainConfig())
    bool        m_terrainUp = false;

    x3::rhi::IRenderDevice::SkyParams m_sky;   // the alien sky tuning

    bool m_emergenceReached = false;  // L8 Emergence Point reached (awe beat)
    bool m_l9Reached        = false;  // crossed into the L9 desert

    x3::phys::Vec3 m_l8EmergenceExit{}; // far edge of L8 (the L8->L9 threshold)
    x3::phys::Vec3 m_l9Spawn{};         // L9 desert entry
};

// Headless self-test (--test-act2). Builds the Act-2 surface + L8/L9 on a
// HeadlessDevice + Jolt world (terrain streamer synchronous, jobs==null) and
// asserts: the world builds + the surface stands up; the L8/L9 area plans carry
// the expected footprints + spawn + objective; the L8 gauntlet has 5 ranged
// Pursuit Drones + 3 melee Infected (all alive) and 4 allied companion markers;
// the L9 desert has the crystal props + neutral fauna + a hazard zone that is
// PRESENT but INERT at load and only accumulates exposure once entered; the
// L8 -> L9 transition is reachable; and the alien sky is tuned violet. Prints
// "act2: X/Y passed"; returns true iff all pass. No window/Vulkan.
bool runAct2WorldSelfTest();

} // namespace x3::game
