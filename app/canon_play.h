#pragma once
// CANON FLOOR-1 GAMEPLAY (--world canonlevel). Game/slice code only — engine/ stays pure.
//
// The data-driven canonical Floor 1 (level_loader.*) builds the lit geometry + doors +
// per-room flood-fill cull but spawns ZERO characters ("AREA CLEAR."). This system wires
// the EXISTING gameplay systems (MonsterManager / RescueSystem / WeaponSystem) onto the
// REAL canon room centers so --world canonlevel is PLAYABLE:
//
//   * a SIDEARM pickup in Jake's Cell (the spawn) so you leave the cell armed;
//   * ANIMATED enemies (GPU-skinned marcus_webb / alien_crawler / chief_martinez set via
//     tuningFor(DominionTrooper/Verthani/BlueSynth)) down the Main Hall + a few side cells;
//   * the Martinez BOSS in the Boss Arena;
//   * the 3 rescue girls (Aria/Keisha/Emily) in the Medical Bay + adjacent wards, each
//     guarded by 1-2 attackers — kill the attackers to interrupt the alien-DNA infection
//     and save her before the timer; saved -> grateful companion, expired -> transforms to
//     a boss (the existing RescueSystem victim->boss lifecycle).
//
// EVERY spawned entity is ROOM-TAGGED (its Scene::Entity::roomId set to the canon room it
// occupies) so the portal flood-fill cull + per-room lights include it, and so the model
// draw can be gated to the visible set (perf: only nearby characters are drawn/skinned).
// enemiesRemaining() folds every group + the boss so the HUD "AREA CLEAR" reflects reality.
//
// MP-friendly / headless-testable style (mirrors Level1Game / RescueSystem): build() once
// against a parsed CanonFloor, tick() each frame with the player as the damage sink, an
// interact hook for rescue, room-gated draw, HUD accessors. --test-canonplay drives it on
// a HeadlessDevice (no window / Vulkan) and asserts the spawn anchoring + room tagging +
// the distinct per-girl dialog table.

#include "scene.h"
#include "monster.h"
#include "rescue.h"
#include "weapon.h"
#include "level_loader.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace x3::game {

// ---- Per-girl dialog (staging/girls_dialog.json). 4 lifecycle states, distinct lines
// per girl. Loaded at build (or a tiny baked fallback if the JSON is absent) so each girl
// speaks her OWN lines by state instead of the shared makeRescueDialog() table. ----
enum class GirlDialogState : uint32_t {
    CaptiveFrantic   = 0,   // mid-attack, terror, calling Jake
    RescuedGrateful  = 1,   // saved before the infection took — relief + thanks
    CompanionAmorous = 2,   // now following Jake — warm / flirty / devoted
    InfectedLost     = 3,   // NOT saved in time — transformed to a boss
    Count            = 4
};

// One girl's lines for all four states (each state holds 1..N lines).
struct GirlDialog {
    std::string name;
    std::array<std::vector<std::string>, (size_t)GirlDialogState::Count> states;
    bool empty() const {
        for (const auto& s : states) if (!s.empty()) return false;
        return true;
    }
};

// The per-girl dialog table (Aria/Keisha/Emily + any others the JSON carries). Loaded from
// the staged JSON; on absence a small baked-in table keeps distinct per-girl voices so the
// feature (and --test-canonplay) works on a clean checkout.
class GirlsDialog {
public:
    // Load from `jsonPath` (staging/girls_dialog.json). On any failure, bakes the minimal
    // distinct table. Always leaves the table non-empty. Returns true iff the JSON parsed.
    bool load(std::string_view jsonPath);

    // True iff loaded from the JSON (vs the baked fallback).
    bool fromJson() const { return m_fromJson; }

    uint32_t count() const { return (uint32_t)m_girls.size(); }
    const GirlDialog* find(std::string_view name) const;

    // The first line for a girl in a state (the host shows it as a subtitle). Returns an
    // empty string if the girl / state is unknown. (A subtitle line is enough per spec.)
    std::string line(std::string_view name, GirlDialogState state) const;

    // True iff at least two girls have DISTINCT captive_frantic line sets (the per-girl
    // voice proof for --test-canonplay).
    bool linesAreDistinct() const;

    const std::vector<GirlDialog>& girls() const { return m_girls; }

private:
    void bakeFallback();
    std::vector<GirlDialog> m_girls;
    bool m_fromJson = false;
};

// Resolve the staged girls-dialog JSON path (repo staging/ dir, relative to the exe / cwd).
std::string canonGirlsDialogPath();

// ---------------------------------------------------------------------------
// UPPER-FLOOR CONTENT (floors 2-7 + the hidden F4.5 spire) — the same canon
// building CanonPlay::build already populates on Floor 1, extended UP the tower.
//
// loadCanonBuilding() fuses all 7 canonical floors into ONE CanonFloor (room ids
// stay globally unique, Floor 1's rooms come first). CanonPlay resolves Floor 1 by
// the canonBeats() names; the upper floors are resolved here by their OWN unique
// room names (the JSON already encodes the design intent in the name + desc, e.g.
// "Ward A: Keisha", "F3 Boss: Experiment #7", "Tier 5: Apex Arena"). The upper
// content REUSES the exact same spawn primitives Floor 1 uses — MonsterManager::
// spawn (themed by EnemyType + per-boss Tuning), RescueSystem (captive girls),
// and a lightweight room-tagged item pickup prop — so this is an EXTENSION of the
// Floor-1 mechanism, never a parallel system. Every spawn is room-tagged for the
// portal cull + per-room lights, exactly like the Floor-1 path.
// ---------------------------------------------------------------------------

// Item pickup kinds the upper floors scatter (ammo/health to fight up the tower,
// weapons, keycards for the keypad doors, the nano-booster, lore/terminal reads).
// Rendered as a small tinted box prop (room-tagged) — the same primitive main.cpp
// uses for the Floor-1 Security keycard. Grabbed on proximity in tick().
enum class CanonItemKind : uint32_t {
    Ammo        = 0,   // ammo crate (yellow)
    Health      = 1,   // medkit (red cross-ish)
    Weapon      = 2,   // a heavier weapon pickup (orange)
    Keycard     = 3,   // access keycard for a keypad door (cyan)
    NanoBooster = 4,   // the nano-booster buff (green-cyan)
    LoreTerminal= 5,   // a readable lore/data terminal (blue)
    Count       = 6
};
const char* canonItemKindName(CanonItemKind k);

// One placed pickup: a Tag::Prop entity (tinted box) at a room-tagged spot. Grabbed
// when the player walks within kCanonPickupReach; collected items raise their flag.
struct CanonItem {
    CanonItemKind kind   = CanonItemKind::Ammo;
    uint32_t      room   = kNoRoom;   // canon room it sits in (room-tagged)
    uint32_t      entity = kNoLink;   // its Scene Tag::Prop entity
    x3::phys::Vec3 pos{};             // world position
    bool          taken  = false;     // collected (hidden) once grabbed
};

constexpr float kCanonPickupReach = 1.8f;   // proximity radius to auto-collect an item

// Per-floor authored content summary (read by the host HUD + --test-upperfloors so
// the spawn counts / roles / captives / objective can be asserted without re-deriving).
struct CanonFloorPlan {
    int          floorNum   = 0;
    const char*  name       = "";     // canon floor identity ("Medical Bay", ...)
    uint32_t     enemyCount = 0;      // standard enemies placed on this floor
    uint32_t     bossCount  = 0;      // designed floor bosses placed
    uint32_t     itemCount  = 0;      // pickups placed
    uint32_t     captiveCount = 0;    // rescue captives placed
    const char*  objective  = "";     // one-line floor objective (HUD)
    bool         hasBoss    = false;
};

// ---------------------------------------------------------------------------
// CanonPlay — the canon Floor-1 gameplay controller.
// ---------------------------------------------------------------------------
class CanonPlay {
public:
    // Build the gameplay onto the parsed+resolved `floor`: the sidearm pickup in Jake's
    // Cell, the Main-Hall + side-cell enemy squad, the Martinez boss in the Boss Arena, and
    // the 3 rescue girls + their attackers in the Medical Bay / adjacent wards. Every
    // spawned entity is room-tagged. `modelDir` is the rigged-GLB dir (the animated set).
    // The per-girl dialog is loaded from `girlsDialogPath` (staging JSON; baked fallback on
    // absence). Call once after buildCanonFloor().
    void build(const CanonFloor& floor, Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, std::string_view modelDir,
               std::string_view girlsDialogPath);

    // Populate floors 2-7 + the hidden F4.5 spire with themed content (enemies,
    // bosses, captives, items) onto the SAME fused CanonFloor build() received. A
    // no-op unless the fused floor actually carries the upper-floor rooms (i.e. the
    // whole building was loaded via loadCanonBuilding) — on a single-floor load the
    // upper room names resolve to kNoRoom and nothing is placed (so --world canonfloor1
    // is unchanged). Called from build() automatically; safe to call once. Reuses the
    // Floor-1 spawn primitives (MonsterManager / RescueSystem / item props), all
    // room-tagged for the cull. `floorBase` (optional) is loadCanonBuilding's per-floor
    // first-room map, used only for diagnostics.

    // Advance one frame: enemies attack the player (the damage sink) on cooldown, the boss
    // runs its phase machine, the girls' attackers fight, and the rescue timers/companions
    // tick. `eye`/`playerPos` is the camera world position. `player` may be null (headless
    // geometry tick). `attackFx` spawns the per-attack beam (host wires CombatFx).
    void tick(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
              const x3::phys::Vec3& eye, IDamageSink* player, const AttackFxFn& attackFx);

    // Fire one shot along `dir` from `eye` (only when armed): damages the first live
    // hostile across every group (Main Hall / cells / girl-attackers / Martinez / rescue
    // bosses). Returns the FireResult so the host spawns FX. No-op when not armed.
    FireResult onFire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                      Scene& scene, x3::phys::IPhysicsWorld& physics,
                      int damage = kDamagePerShot);

    // Try to rescue the nearest captive girl in range of `playerPos` (E-interact). Returns
    // true iff a girl was rescued this call (the host logs / surfaces her grateful bark).
    bool tryRescue(const x3::phys::Vec3& playerPos, float reach = kRescueReach);

    // Draw the pickup + all live characters, GATED by the room cull: a monster/girl is
    // drawn only if its room is in the scene's current visible set (perf — only nearby
    // characters are drawn). The boss + rescue bosses draw when their room is visible.
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
              const Scene& scene) const;

    // Draw the held weapon viewmodel (no-op unless armed). Forwards to the WeaponSystem.
    void drawViewmodel(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                       float ex, float ey, float ez, float yaw, float pitch,
                       float yawOff, float pitchOff, float rollOff,
                       float fwd, float right, float down) const;

    // ---- Cue / death-FX fan-out (footsteps, impacts, gib bursts). Wire after build. ----
    void setCueSink(const GameCueFn& sink);
    void setDeathFxSink(const DeathFxFn& sink);

    // Tear down any live death-ragdoll bodies across every enemy group + the boss. Call
    // BEFORE the physics world is shut down (mirrors MonsterManager::shutdown) so no Jolt
    // ragdoll body is destroyed after the world is gone (an access violation otherwise).
    // Idempotent / safe to call any time.
    void shutdown();

    // ---- Queries (HUD + the self-test) ------------------------------------
    bool built() const { return m_built; }
    bool armed() const { return m_weapon.hasWeapon(); }
    void cheatArm(Scene& scene) { m_weapon.forceArm(scene); }   // IDKFA/IDFA

    // Total LIVE hostiles across every group (corridor squad + cell guards + the per-girl
    // attackers + Martinez + any rescue-boss transforms). Drives the HUD "ENEMIES" counter
    // so it never reads "AREA CLEAR" while something is alive.
    int enemiesRemaining() const;

    bool martinezSpawned() const { return m_martinezSpawned; }
    bool martinezAlive()   const { return m_martinezSpawned && m_martinez.alive(); }

    RescueSystem&       rescue()       { return m_rescue; }
    const RescueSystem& rescue() const { return m_rescue; }
    const WeaponSystem& weapon() const { return m_weapon; }
    const GirlsDialog&  dialog() const { return m_dialog; }

    // The canon room id a girl is in (Medical Bay / a ward), by victim index. kNoRoom if
    // unknown. Used by the host to surface her per-girl subtitle from the right state.
    uint32_t girlRoom(uint32_t victimIndex) const {
        return victimIndex < m_girlRooms.size() ? m_girlRooms[victimIndex] : kNoRoom;
    }

    // ---- HUD radar / nameplate feed (read-only enumeration) ----
    struct EnemyMark { x3::phys::Vec3 pos; const char* label; };
    uint32_t liveEnemyMarks(EnemyMark* out, uint32_t cap) const;
    uint32_t liveCompanionPositions(x3::phys::Vec3* out, uint32_t cap) const;

    // ---- Spawn bookkeeping (the self-test asserts these) ------------------
    // The room the sidearm pickup was placed in (Jake's Cell). kNoRoom if unbuilt.
    uint32_t pickupRoom() const { return m_pickupRoom; }
    // The room Martinez spawned in (the Boss Arena). kNoRoom if absent.
    uint32_t bossRoom() const { return m_bossRoom; }
    // Count of room-tagged hostile spawns (every Main-Hall / cell / attacker spawn).
    uint32_t taggedHostileCount() const { return m_taggedHostiles; }
    // The Main-Hall enemy count + the cell-guard count (the corridor squad split).
    uint32_t mainHallCount()  const { return m_mainHall.count(); }
    uint32_t cellGuardCount() const { return m_cellGuards.count(); }
    // Per-girl attacker count (Medical Bay interrupt-rescue enemies).
    uint32_t attackerCount() const { return m_attackers.count(); }

    // ---- UPPER-FLOOR queries (floors 2-7 + the F4.5 spire) — the self-test asserts
    // these; the host HUD folds them into ENEMIES / objective lines. ----
    bool     upperFloorsBuilt() const { return m_upperBuilt; }
    // Standard upper-floor enemies (every floor 2-7 + spire-tier enemy), one manager.
    uint32_t upperEnemyCount() const { return m_upperEnemies.count(); }
    uint32_t upperEnemyAlive() const { return m_upperEnemies.aliveCount(); }
    // Designed upper-floor bosses (F2..F7 floor bosses + the F4.5 Apex Chorus), one
    // manager so each boss's room-tag + tuning can be asserted distinctly.
    uint32_t upperBossCount() const { return m_upperBosses.count(); }
    uint32_t upperBossAlive() const { return m_upperBosses.aliveCount(); }
    // Upper-floor captives (the F2 Keisha/Emily/Aria wards + F7 Sarah), their own
    // RescueSystem (distinct from the Floor-1 Medical-Bay rescue lifecycle).
    uint32_t upperCaptiveCount() const { return m_upperRescue.victimCount(); }
    const RescueSystem& upperRescue() const { return m_upperRescue; }
    RescueSystem&       upperRescue()       { return m_upperRescue; }
    // Items placed across the upper floors (ammo/health/weapons/keycards/nano/lore).
    uint32_t upperItemCount() const { return (uint32_t)m_upperItems.size(); }
    const std::vector<CanonItem>& upperItems() const { return m_upperItems; }
    // The authored per-floor plan (counts/roles/objective), indexed by floor number
    // 2..7 (index 0 == F2). Returns a zeroed plan for out-of-range floors.
    const CanonFloorPlan& floorPlan(int floorNum) const;
    // The Apex (F4.5 spire) boss room — the climactic Chorus encounter. kNoRoom if absent.
    uint32_t apexRoom() const { return m_apexRoom; }
    // True iff the spire's apex reward (the access keycard / data core) was placed.
    bool     apexRewardPlaced() const { return m_apexRewardPlaced; }

private:
    // Tag a freshly-spawned monster's Scene entity with `room` (so the cull + lights include
    // it). Returns the room id (for bookkeeping). Idempotent / bounds-checked.
    uint32_t tagRoom(Scene& scene, const MonsterSystem& m, uint32_t room);

    // Draw a manager's monsters, gated to the scene's visible room set (perf cull).
    void drawManagerCulled(const MonsterManager& mm, x3::rhi::IRenderDevice& device,
                           const x3::rhi::FrameContext& frame, const Scene& scene) const;

    WeaponSystem   m_weapon;       // sidearm pickup in Jake's Cell
    MonsterManager m_mainHall;     // animated squad down the Main Hall
    MonsterManager m_cellGuards;   // a few enemies in side cells
    MonsterManager m_attackers;    // the per-girl Medical-Bay attackers (interrupt rescue)
    MonsterSystem  m_martinez;     // the Boss Arena boss
    RescueSystem   m_rescue;       // the 3 girls (Aria/Keisha/Emily) + their boss transforms
    GirlsDialog    m_dialog;       // per-girl 4-state lines

    std::vector<uint32_t> m_girlRooms;   // canon room per victim index

    // ---- UPPER-FLOOR content (floors 2-7 + the F4.5 spire) — extends the Floor-1
    // mechanism up the tower. Same systems, more groups, all room-tagged. ----
    void buildUpperFloors(const CanonFloor& floor, Scene& scene,
                          x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics);
    // Spawn N standard enemies in a named room (themed mix, depth-scaled). Returns count.
    uint32_t spawnUpperEnemies(const CanonFloor& floor, Scene& scene,
                               x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
                               const char* roomName, const EnemyType* mix, uint32_t mixCount,
                               int hpBonus, float speedBonus);
    // Spawn a designed floor boss (custom Tuning) in a named room. Returns true if placed.
    bool spawnUpperBoss(const CanonFloor& floor, Scene& scene,
                        x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
                        const char* roomName, const MonsterSystem::Tuning& bossTuning);
    // Place one item pickup (tinted box prop) in a named room, room-tagged. Returns true.
    bool placeUpperItem(const CanonFloor& floor, Scene& scene, x3::rhi::IRenderDevice& device,
                        const char* roomName, CanonItemKind kind, float dx = 0.0f, float dz = 0.0f);

    MonsterManager m_upperEnemies; // every standard enemy on floors 2-7 + spire tiers
    MonsterManager m_upperBosses;  // the designed floor bosses (F2..F7) + the Apex Chorus
    RescueSystem   m_upperRescue;  // the upper-floor captives (F2 girls + F7 Sarah)
    std::vector<CanonItem> m_upperItems;   // ammo/health/weapons/keycards/nano/lore props
    CanonFloorPlan m_floorPlans[6];        // F2..F7 authored summaries (index 0 == F2)
    uint32_t       m_apexRoom = kNoRoom;   // the F4.5 Apex Arena (Chorus) boss room
    bool           m_apexRewardPlaced = false;
    bool           m_upperBuilt = false;

    GameCueFn  m_cueSink;
    DeathFxFn  m_deathFx;
    std::string m_modelDir;

    uint32_t m_pickupRoom    = kNoRoom;
    uint32_t m_bossRoom      = kNoRoom;
    uint32_t m_taggedHostiles = 0;
    bool     m_martinezSpawned = false;
    bool     m_built           = false;
};

// Headless self-test (--test-canonplay). Loads Floor 1, builds the canon floor + CanonPlay
// on a HeadlessDevice + Jolt world, and asserts: the sidearm spawns in Jake's Cell; N
// animated enemies + the cell guards spawn in the Main Hall / cells (room-tagged); Martinez
// spawns in the Boss Arena; the 3 girls + their attackers spawn in the Medical Bay (room-
// tagged); enemiesRemaining() counts them all; and the per-girl dialog table has DISTINCT
// lines per girl across the 4 states. Logs PASS/FAIL P#, returns true iff all pass. No
// window / Vulkan. Skips cleanly (PASS) if the canonical JSON is absent on this machine.
bool runCanonPlaySelfTest();

// Headless self-test (--test-upperfloors). Loads the WHOLE canon building
// (loadCanonBuilding, all 7 floors fused), builds the canon geometry + CanonPlay,
// and asserts the UPPER-FLOOR content (floors 2-7 + the F4.5 spire): each populated
// floor spawns non-zero enemies + items (room-tagged, inside their rooms); the
// designed floor bosses (F2 Dr. Chen, F3 Experiment #7, F4 The Collective, F5 Swarm
// Controller, F6 Alien Overseer, F7 Jake's Clone) are placed in their Boss Arenas;
// the F4.5 Apex Chorus mini-boss + its apex reward are placed at the spire top; the
// upper captives (F2 wards + F7 Sarah) are placed in their cells; difficulty scales
// with depth (F2 enemy HP < F7 enemy HP); and every spawn carries a valid room id.
// Skips cleanly (PASS) if the canonical JSON is absent. Logs PASS/FAIL U#, returns
// true iff all pass. No window / Vulkan.
bool runUpperFloorsSelfTest();

} // namespace x3::game
