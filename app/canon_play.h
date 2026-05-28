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

    // ---- Enemy-group accessors (HUD health bars / radar) ------------------
    // Expose every spawned hostile group so the host (main.cpp) can iterate over
    // them for the over-enemy health bar overlay. Read-only enumeration; no state
    // change. Mirrors Level1Game::corridorEnemies()/checkpointEnemies()/etc.
    const MonsterManager& mainHall()   const { return m_mainHall; }
    MonsterManager&       mainHall()         { return m_mainHall; }
    const MonsterManager& cellGuards() const { return m_cellGuards; }
    MonsterManager&       cellGuards()       { return m_cellGuards; }
    const MonsterManager& attackers()  const { return m_attackers; }
    MonsterManager&       attackers()        { return m_attackers; }
    const MonsterSystem&  martinez()   const { return m_martinez; }
    MonsterSystem&        martinez()         { return m_martinez; }

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

} // namespace x3::game
