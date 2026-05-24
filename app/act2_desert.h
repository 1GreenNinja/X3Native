#pragma once
// EFLZ Act 2 — the desert arc continues: L10 Crystalline Desert Depths +
// L11 Salvari Camp ("Refugee Haven"). Game/slice content code only — engine/
// stays pure. This module picks up where act2_world.* (L8 Surface Emergence +
// L9 Crystalline Desert Edge) leaves off and is reachable from L9's edge.
//
// CLEAN-ROOM, original work. Built ONLY from X3Native's OWN systems (Scene,
// monster.* incl. the Act-2 roster, trigger.*, terrain.*, mesh_prims) + the
// engine interfaces + the EFLZ design docs in docs/design/ (Tim's own IP). NO
// RBDOOM / id Tech / Doom / Quake — or ANY other game-engine — source was forked,
// copied, or consulted.
//
// SCOPE / OWNERSHIP: this lane owns the Act-2 desert depths + the Salvari camp.
// It does NOT edit act2_world.* (it READS it for the Act2Level enum + the
// authoring framework), the Act-1 floor files, or monster.* (it USES the existing
// roster, including the Act-2 SalvariAlly / NativeDesertFauna rows the roster lane
// already shipped). It COMPOSES the existing roster + the procedural terrain world
// + the analytic alien sky + the trigger system into the desert-depths content:
//
//   * L10 CRYSTALLINE DESERT DEPTHS — a deeper continuation of the L9 crystal
//     desert (same streamed alien surface + violet sky). FIRST CONTACT: a small
//     group of allied Salvari refugees (the REAL Act-2 roster `SalvariAlly` type —
//     allied + 0 damage) near a hidden crystal-cave camp entrance, including one
//     INJURED Salvari that anchors a "help the injured Salvari" SIDE-QUEST hook
//     (present but inert until interacted). A LIGHT Overlord patrol (existing
//     hostile roster, tinted Overlord) hunts the approach. Emissive singing-crystal
//     formations + the glowing cave mouth mark the way down.
//
//   * L11 SALVARI CAMP ("Refugee Haven") — a hidden cave settlement (graybox rock
//     enclosure + emissive bioluminescent crystals) sheltering the refugee
//     population (canon: hundreds; the slice places several survivor markers incl.
//     K'thara, the Salvari commander). An alien-equipment UPGRADE STATION interact
//     (a stub that grants a FLAGGED upgrade, inert until interacted) + a
//     cultural-exchange trigger beat seal the first-contact arc.
//
//   * REACHABILITY: L9 -> L10 -> L11 progresses through labelled transition
//     triggers, mirroring act2_world's trigger pattern but on a FRESH id range
//     (90+) so this system can share one TriggerSystem with act2_world (80..82)
//     and the Act-1 systems (30/40/50) without colliding.
//
//   * Existing-systems only — NO renderer/engine changes.
//
// Authoring style mirrors act2_world.* (and spire_mid.* before it): build() once,
// tick()/update() each frame, onTrigger()/onInteract() dispatch, draw() helpers,
// and plan()/query accessors read by the host HUD + the headless self-test
// (--test-act2desert) so nothing is re-derived.

#include "scene.h"
#include "monster.h"
#include "trigger.h"
#include "terrain.h"
#include "act2_world.h"   // READ-ONLY: the Act2Level enum (canonical L8..L20 numbering)

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/core/IJobSystem.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace x3::game {

// This module implements the next two Act-2 levels after act2_world's L8/L9. The
// canonical level numbers come from act2_world's Act2Level (L10 / L11).
constexpr uint32_t kDesertFirstLevel = 10;            // L10
constexpr uint32_t kDesertLastLevel  = 11;            // L11
constexpr uint32_t kDesertLevelCount = kDesertLastLevel - kDesertFirstLevel + 1; // 2

// Act-2 DESERT-DEPTHS trigger event ids — a FRESH range (90+) so this system can
// share one TriggerSystem with act2_world (Act2Trigger 80..82) and the Act-1
// systems (30/40/50) without colliding.
enum class Act2DesertTrigger : uint32_t {
    L9toL10Transition  = 90,  // crossed the L9 desert edge into the L10 depths
    L10CaveEntrance    = 91,  // found the hidden Salvari camp entrance (crystal cave)
    L10toL11Transition = 92,  // entered the Salvari camp (the cave settlement)
    L11CulturalExchange= 93,  // reached the cultural-exchange beat inside the camp
};

// Act-2 DESERT-DEPTHS interact ids (the host forwards a use/interact to onInteract).
// A separate id space from the triggers (different dispatch method).
enum class Act2DesertInteract : uint32_t {
    L10InjuredSalvari = 1,  // "help the injured Salvari" side-quest hook (inert at load)
    L11UpgradeStation = 2,  // alien-equipment upgrade station (grants a flagged upgrade)
};

// Body-center Y above the surface for a placed humanoid/drone (matches act2_world's
// kAct2EnemyYOff — the rigged GLBs sit ~0.4 m up).
constexpr float kDesertEnemyYOff = 0.4f;

// Content counts (read by the host HUD + the self-test so nothing is re-derived).
constexpr uint32_t kDesertCrystalCount   = 7;  // L10 deeper-desert singing-crystal props
constexpr uint32_t kDesertContactCount   = 3;  // L10 first-contact Salvari (1 is injured)
constexpr uint32_t kDesertPatrolCount    = 3;  // L10 light Overlord patrol (hostile)
constexpr uint32_t kCampSurvivorCount    = 7;  // L11 camp Salvari survivor markers (incl. K'thara)
constexpr uint32_t kCampCrystalCount     = 8;  // L11 cave bioluminescent crystals
// Narrative refugee population sheltering in the camp (canon: hundreds — the
// graybox places markers, this records the headcount for the HUD/objective text).
constexpr uint32_t kSalvariCampPopulation = 200;

// One desert-depths area descriptor (read by the host HUD + the self-test so they
// never re-derive footprint/spawn/objective/counts). Mirrors Act2AreaPlan but
// carries the L10/L11-specific content fields (Salvari / patrol / side-quest /
// upgrade station) instead of L8/L9's companions / fauna / hazard.
struct DesertAreaPlan {
    Act2Level   level       = Act2Level::L10_CrystallineDesertDepths;
    const char* name        = "";     // canon level name
    const char* biome       = "";     // biome / setting one-liner
    const char* objective   = "";     // primary objective text
    bool        implemented = false;  // true for L10/L11 this pass

    x3::phys::Vec3 spawn{};            // where the player enters this area
    float       footprintX  = 0.0f;   // area extent in meters (X)
    float       footprintZ  = 0.0f;   // area extent in meters (Z)

    uint32_t    salvariCount   = 0;   // allied Salvari NPCs placed (contacts / survivors)
    uint32_t    patrolCount    = 0;   // hostile Overlord patrol placed (L10)
    uint32_t    crystalCount   = 0;   // emissive crystal props placed
    bool        hasSideQuest   = false; // L10 "help the injured Salvari" hook present
    bool        hasUpgradeStation = false; // L11 alien-equipment upgrade station present
    uint32_t    survivorPopulation = 0; // narrative refugee headcount (L11)
};

// The Act-2 desert-depths system. Build once after the player/render context
// exists; tick() each frame for the patrol/Salvari groups, update() each frame to
// advance the terrain streamer around the focus.
class Act2Desert {
public:
    // Stand up the desert-depths surface + author L10/L11. `triggers` is the host's
    // shared TriggerSystem (the host forwards fired Act2DesertTrigger ids to
    // onTrigger()). `jobs` may be null (then terrain generation runs synchronously
    // on the calling thread — used by the headless self-test). `modelDir` is the
    // loose rigged-GLB dir (same one Level1Game/Act2World receive). Call once.
    void build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, TriggerSystem& triggers,
               x3::jobs::IJobSystem* jobs, std::string_view modelDir);

    // Advance one frame: the L10 Overlord patrol (attacks `player`), and the allied
    // Salvari contacts/survivors (movement only — they never fight the player).
    // `player` may be null (geometry/headless movement only).
    void tick(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
              const x3::phys::Vec3& eye, const x3::phys::Vec3& playerPos,
              IDamageSink* player, const AttackFxFn& attackFx);

    // Advance the terrain streamer's residency ring around the focus (player/cam)
    // world position. Returns tiles uploaded this frame. No-op before build().
    uint32_t update(Scene& scene, x3::rhi::IRenderDevice& device,
                    x3::phys::IPhysicsWorld& physics, float focusX, float focusZ);

    // Dispatch a fired Act2DesertTrigger id the host forwards from its TriggerSystem.
    // Latches the L9->L10 / cave-found / L10->L11 / cultural-exchange beats. Idempotent.
    void onTrigger(uint32_t triggerId);

    // Dispatch a player interact (Act2DesertInteract id). Returns true the first time
    // the hook fires (and latches it): L10InjuredSalvari completes the side-quest;
    // L11UpgradeStation grants the flagged upgrade. Idempotent (returns false after
    // the first success). Both are INERT until this is called.
    bool onInteract(uint32_t interactId);

    // Fire one shot against the L10 Overlord patrol (the allied Salvari are NOT valid
    // targets, so they are excluded). Returns the result for FX/HUD.
    FireResult onFire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                      Scene& scene, x3::phys::IPhysicsWorld& physics);

    // Draw the L10 patrol + the allied Salvari contacts/survivors (the crystal /
    // cave / upgrade-station props are plain Scene entities drawn by scene.render()).
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
              const Scene& scene) const;

    // Tear down the terrain streamer's GPU + physics resources. Safe to call once.
    void shutdown(Scene& scene, x3::rhi::IRenderDevice& device,
                  x3::phys::IPhysicsWorld& physics);

    // ---- Queries (host HUD + the self-test) -------------------------------
    bool built() const { return m_built; }
    // The authored descriptor for L10 / L11 (footprint/spawn/objective/counts).
    const DesertAreaPlan& plan(Act2Level l) const { return m_plan[(uint32_t)l - kDesertFirstLevel]; }

    // L10 first-contact Salvari (allied) + the L11 camp survivors (allied).
    const MonsterManager& contacts()  const { return m_contacts; }
    const MonsterManager& survivors() const { return m_survivors; }
    // The L10 Overlord patrol (hostile).
    const MonsterManager& patrol()    const { return m_patrol; }
    MonsterManager&       patrol()          { return m_patrol; }

    // Emissive prop counts.
    uint32_t desertCrystalCount() const { return (uint32_t)m_desertCrystals.size(); }
    uint32_t campCrystalCount()   const { return (uint32_t)m_campCrystals.size(); }
    // L10 hidden cave-entrance marker present (visual + the L10CaveEntrance trigger).
    bool caveMouthPlaced() const { return m_caveMouth != kNoLink; }
    // L11 alien-equipment upgrade station entity (kNoLink until built).
    uint32_t upgradeStationEntity() const { return m_upgradeStation; }

    // ---- Story beats latched by onTrigger(). ------------------------------
    bool l10Reached() const { return m_l10Reached; }
    bool caveFound()  const { return m_caveFound; }
    bool l11Reached() const { return m_l11Reached; }
    bool culturalExchangeDone() const { return m_culturalExchange; }

    // ---- Side-quest / upgrade hooks (present at build, INERT until interacted). ---
    bool injuredSalvariRescued() const { return m_injuredSalvariRescued; }
    bool upgradeGranted()        const { return m_upgradeGranted; }

    // The desert arc is reachable iff BOTH the L9->L10 and L10->L11 transition
    // triggers are registered + enabled and sit (monotonically in +X) between their
    // areas — i.e. the player can cross L9 edge -> L10 -> L11 to progress. A
    // content-side assertion (mirrors Act2World::transitionReachable).
    bool chainReachable(const TriggerSystem& triggers) const;

    // The ALIEN sky tuning (the host applies it via device.setSkyParams). Same
    // binary-sun / purple-atmosphere approximation act2_world uses (this lane keeps
    // the L9->L10 surface visually continuous).
    const x3::rhi::IRenderDevice::SkyParams& alienSky() const { return m_sky; }

    // Terrain surface (the streamed alien world, shared config with act2_world).
    const TerrainConfig& worldConfig() const { return m_cfg; }
    uint32_t residentTiles() const { return m_terrain.residentCount(); }
    bool focusTileResident(float x, float z) const { return m_terrain.focusTileResident(x, z); }
    TerrainStreamer&       terrain()       { return m_terrain; }
    const TerrainStreamer& terrain() const { return m_terrain; }

private:
    bool        m_built = false;
    std::string m_modelDir;

    DesertAreaPlan m_plan[kDesertLevelCount];

    MonsterManager m_patrol;     // L10 Overlord patrol (hostile)
    MonsterManager m_contacts;   // L10 first-contact Salvari (allied; one injured)
    MonsterManager m_survivors;  // L11 camp Salvari survivors (allied; incl. K'thara)

    std::vector<uint32_t> m_desertCrystals;  // L10 crystal-formation Scene props
    std::vector<uint32_t> m_campCrystals;    // L11 cave bioluminescent crystals
    std::vector<uint32_t> m_campStructures;  // L11 cave graybox (floor/walls/ceiling)
    uint32_t    m_caveMouth      = kNoLink;  // L10 hidden cave-entrance marker
    uint32_t    m_upgradeStation = kNoLink;  // L11 alien-equipment upgrade station
    uint32_t    m_injuredIdx     = 0;        // index into m_contacts of the injured Salvari

    TerrainStreamer m_terrain;   // the alien surface (streamed procedural world)
    TerrainConfig   m_cfg;       // the world config (== worldTerrainConfig())
    bool        m_terrainUp = false;

    x3::rhi::IRenderDevice::SkyParams m_sky;  // the alien sky tuning (matches act2_world)

    // Story beats (latched by onTrigger / onInteract).
    bool m_l10Reached        = false;  // crossed into the L10 desert depths
    bool m_caveFound         = false;  // discovered the hidden camp entrance
    bool m_l11Reached        = false;  // entered the Salvari camp
    bool m_culturalExchange  = false;  // cultural-exchange beat reached
    bool m_injuredSalvariRescued = false;  // side-quest completed (interact)
    bool m_upgradeGranted    = false;  // upgrade station used (interact)

    // Reachability anchors (the monotonic +X progression L9 edge -> L10 -> cave -> L11).
    x3::phys::Vec3 m_l9Edge{};       // L9 far edge (the L9->L10 threshold approach)
    x3::phys::Vec3 m_l10Spawn{};     // L10 depths entry
    x3::phys::Vec3 m_caveMouthPos{}; // the hidden cave entrance (L10 far end)
    x3::phys::Vec3 m_l11Spawn{};     // L11 camp entry (inside the cave)
};

// Headless self-test (--test-act2desert). Builds the desert depths + L10/L11 on a
// HeadlessDevice + Jolt world (terrain streamer synchronous, jobs==null) and
// asserts: the world builds + the surface stands up; the L10/L11 area plans carry
// the expected footprints + spawn + objective + counts; the Salvari contacts +
// camp survivors are ALLIED (0 damage to the player) and alive; the L10 Overlord
// patrol is HOSTILE (nonzero damage) and alive; the "help the injured Salvari"
// side-quest + the L11 upgrade station are PRESENT but INERT until interacted (and
// then flip on interact, idempotently); the cultural-exchange beat latches on its
// trigger; and the L9 -> L10 -> L11 chain is reachable. Prints "act2desert: X/Y
// passed"; returns true iff all pass. No window / Vulkan.
bool runAct2DesertSelfTest();

} // namespace x3::game
