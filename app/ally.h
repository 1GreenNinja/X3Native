#pragma once
// Cooperative ally NPCs (coop-NPC PR).
//
// Three NPCs fighting alongside the player against the existing Faction::Enemy
// roster (Guards / Drones / Boss / bestiary). The system mirrors monster.h's
// shape: a per-instance struct + AI state machine + a Manager that owns N
// instances + a single update() / draw() pair the host calls each frame. The
// design borrows the monster system's combat-balance namespace (combat::) and
// AI-tuning constants directly so allies fight on the same balance grid as the
// enemies — they are NOT bullet sponges and they do NOT one-shot anything.
//
// Layered on top of monster.h:
//   * Faction (see faction.h) labels every combatant. Targeting funnels
//     through factionsHostile() — allies target Faction::Enemy units, the
//     monsters' existing target picker keeps targeting Faction::Player AND
//     now also Faction::Ally (one-line change in MonsterManager::update —
//     "nearest hostile" instead of "the player").
//   * Weapon equip: each ally carries one of the existing player weapon GLBs
//     (pistol / SMG / shotgun / plasma / chaingun / plasma rifle / lightning).
//     The weapon GLB is rendered third-person at the ally's hand-offset; the
//     fire path reuses the player's combat numbers (damage / fire-rate /
//     hitscan vs projectile) so allies feel like player-grade combatants.
//   * Friendly-fire: factionsHostile() returns false for Player<->Ally and
//     Ally<->Ally. A cvar `g_friendlyfire` (default 0) can layer ON TOP of the
//     base rule — when 1, Player projectiles can damage Allies + vice-versa,
//     but NEVER Ally<->Ally infighting (see AllySystem::resolveHit).
//
// AI state machine (subset of monster.h's AiState, plus Follow + Reload):
//   * Follow      — no hostile visible: stay near the player, match speed.
//   * Engage      — hostile in LOS within combat range: face + fire on cooldown.
//   * Reposition  — mid-range: side-step to break the enemy's aim line.
//   * Reload      — local ammo counter hit 0: brief animation, then back.
//   * TakeCover   — HP < kAllyTakeCoverFrac: back off toward player / known
//                   cover anchor; only leaves when HP recovers or timer.
//   * Search      — lost LOS on the last hostile: walk last-known + heading
//                   sweep; times out to Follow.
//
// Bench integration (the WHOLE REASON the coop NPCs were prioritised):
//   * --bench-combat [N] spawns 3 allies + N enemies in an arena, vsync OFF,
//     600 frames, reports averaged FPS / CPU ms / GPU ms. That is the honest
//     "combat-density" number — see the perf-baseline doc for context.
//   * The bench mode is wired in main.cpp + level1_game.cpp; see the comments
//     near AllyManager::makeBenchArena.
//
// Clean-room: built on the existing IRenderDevice / IModelLoader / IAssetSource
// / INavigation / IPhysicsWorld interfaces only. No third-party AI or combat
// source consulted.

#include "scene.h"
#include "faction.h"
#include "monster.h"    // reuse combat:: namespace + AiState + tuning constants

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/asset/IModelLoader.h"
#include "engine/asset/IAssetSource.h"
#include "engine/ai/INavigation.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace x3::game {

// ---------------------------------------------------------------------------
// Ally identity (the WHO). Three fixed slots in the canon; the spec is
// "3 allies, Male and Female". Each slot picks a different GLB so the player
// can tell their squad apart on sight. The default roster is documented in the
// memory file [[coop-npc-feature-design]]; the GLB filenames here are the
// CANDIDATES (resolved at load time via the asset source, with fallback to a
// procedural tinted box if a GLB is missing — same pattern as MonsterSystem).
// ---------------------------------------------------------------------------
enum class AllyKind : uint32_t {
    Sarah         = 0,  // Female slot 1 — canon F7-rescue co-fighter ("Sarah.glb").
    Martinez      = 1,  // Male   slot 2 — repurposed enemy ("chief_martinez_anim.glb").
    AnnaBodySuit  = 2,  // Variety slot 3 — F variant ("AnnaBodySuit.glb").
    Count         = 3,
};

const char* allyKindName(AllyKind k);

// Resolved GLB filename for an AllyKind (no path — caller mounts the dir via
// IAssetSource). Returns nullptr for Count.
const char* allyKindGlb(AllyKind k);

// ---------------------------------------------------------------------------
// Ally weapon slot. The ally equips ONE of the existing player weapons; the
// behaviour numbers (damage / fire-rate / hitscan vs projectile / spread)
// reuse the player's table so allies fight on the same balance grid. A slot
// for "no weapon" (CQC fallback) is included for the case where the ally is
// disarmed mid-fight (future, the picker logic might give them a melee fallback).
// ---------------------------------------------------------------------------
enum class AllyWeapon : uint32_t {
    None         = 0,
    Pistol       = 1,
    SMG          = 2,
    Shotgun      = 3,
    Plasma       = 4,
    Chaingun     = 5,
    PlasmaRifle  = 6,
    Lightning    = 7,
    Count        = 8,
};

const char* allyWeaponName(AllyWeapon w);

// Resolved GLB filename for an AllyWeapon. Returns "" for None.
const char* allyWeaponGlb(AllyWeapon w);

// ---------------------------------------------------------------------------
// AI behaviour state — a focused subset/superset of monster.h::AiState.
//   * Follow      — no hostile visible: trail the player (~3-5 m behind).
//   * Engage      — hostile in LOS within range: face + fire on cooldown.
//   * Reposition  — periodic mid-fight side-step to break enemy aim lines.
//   * Reload      — local mag at 0: brief reload, then back to Engage/Follow.
//   * TakeCover   — HP <= kAllyTakeCoverFrac: back off + face enemy.
//   * Search      — lost LOS on last hostile: walk last-known + sweep.
// ---------------------------------------------------------------------------
enum class AllyState : uint32_t {
    Follow     = 0,
    Engage     = 1,
    Reposition = 2,
    Reload     = 3,
    TakeCover  = 4,
    Search     = 5,
};

const char* allyStateName(AllyState s);

// ---------------------------------------------------------------------------
// Cross-faction query callback. Mirrors monster.h's AllyQueryFn but returns
// HOSTILE units to the caller. The host (LevelGame / arena) fills `out` with
// up to `maxOut` nearby LIVE Faction::Enemy positions within `radius` of
// `self`, and returns the count written. No heap alloc in the hot path.
// ---------------------------------------------------------------------------
using HostileQueryFn = std::function<uint32_t(const x3::phys::Vec3& self,
                                              float radius,
                                              x3::phys::Vec3* out,
                                              uint32_t maxOut)>;

// Death FX sink (reuses the same signature monster.h uses for DeathFxFn). Fired
// ONCE at the moment the ally is killed. The host wires this to a blood/gib
// burst (CombatFx::spawnImpact + optional gpuDebrisSpawnBurst). Cheap to copy.
using AllyDeathFxFn = std::function<void(const float pos[3])>;

// ---------------------------------------------------------------------------
// Tuning constants. Reuse the combat:: namespace for damage / cooldowns /
// reach so allies are balanced against the same numbers their enemies use.
// Ally-specific tunings (HP, follow distance, take-cover threshold) live here.
// ---------------------------------------------------------------------------
constexpr int   kAllyHp                = 120;  // a touch more than the base 100 kMonsterHp — they're trained.
constexpr float kAllyFollowDistMin     = 3.0f; // when no hostile, hold this far behind player (m).
constexpr float kAllyFollowDistMax     = 6.0f; // ... up to this far. Lazily catches up beyond max.
constexpr float kAllyEngageRangeHitscan = 30.0f; // pistol / SMG / chaingun / plasma rifle / lightning.
constexpr float kAllyEngageRangeShotgun = 12.0f; // closer-quarters engage range for the shotgun.
constexpr float kAllyEngageRangePlasma  = 22.0f; // plasma launcher (projectile, mid).
constexpr float kAllyTakeCoverFrac     = 0.30f; // enter TakeCover when HP / kAllyHp <= this.
constexpr float kAllyRepositionPeriod  = 1.8f;  // how often to step laterally during a sustained Engage (s).
constexpr float kAllyReloadTime        = 1.4f;  // seconds spent in Reload state.
constexpr float kAllyMagSize           = 12.0f; // shots per "magazine" before Reload triggers (float for cooldown math).
constexpr float kAllyLosLostMemory     = 2.0f;  // seconds after losing LOS before Search times out.
constexpr float kAllyMoveSpeed         = 4.0f;  // chase / follow speed (m/s) — a touch under the player (5 m/s).
constexpr float kAllyTurnRate          = 8.0f;  // rad/s heading slew (matches monster.h::kAiTurnRate).

// ---------------------------------------------------------------------------
// One loaded ally model (GLB drawables + the optional skinned anim handle).
// Mirrors SpireArtAsset / EnvAsset / the monster.h equivalent — kept loaded
// for the app's lifetime so GPU handles in `drawables` stay valid. `ok` is
// false if the GLB failed to load; the system silently substitutes a tinted
// procedural box (a-la MonsterSystem) so the fight still works.
// ---------------------------------------------------------------------------
struct AllyAsset {
    x3::asset::Model                      model;
    std::vector<x3::asset::ModelDrawable> drawables;
    bool                                  ok    = false;
    float                                 tint[3] = {1.0f, 1.0f, 1.0f}; // fallback box tint, slot-distinct.
};

// ---------------------------------------------------------------------------
// One live ally. Held in AllyManager::m_allies; index = canonical slot.
// ---------------------------------------------------------------------------
struct AllyInstance {
    AllyKind   kind     = AllyKind::Sarah;
    AllyWeapon weapon   = AllyWeapon::Pistol;
    AllyState  state    = AllyState::Follow;

    int        hp       = kAllyHp;
    bool       alive    = true;

    // World pose. `transform` is the model->world 4x4 column-major (same
    // layout the existing draw API uses). yaw is the cached current heading
    // angle around +Y for the slew step (matches the monster.h pattern).
    float      transform[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    float      yaw           = 0.0f;
    float      yawTarget     = 0.0f;

    // Cooldown timers (seconds). Decrement each frame; an action fires when
    // its timer reaches 0 and is re-armed.
    float      fireCooldown  = 0.0f;  // gates the next shot.
    float      magRemaining  = kAllyMagSize;
    float      reloadTimer   = 0.0f;
    float      repositionTimer = kAllyRepositionPeriod;
    float      losMemory     = 0.0f;  // counts down after losing LOS in Search.

    // Last-known hostile, for Search.
    float      lastKnownHostile[3] = {0,0,0};

    // Physics body handle (the ally is a kinematic capsule — moves via
    // setBodyPosition, takes hits via rayCast(Layer::Ally)). 0 = no body.
    uint32_t   bodyId       = 0;

    // Scene entity (Tag::Ally). The Entity's render mesh is left invalid so
    // Scene::render skips it — AllyManager owns the multi-primitive draw,
    // same pattern as MonsterSystem.
    uint32_t   entityId     = 0;
};

// ---------------------------------------------------------------------------
// The system. Owns the 3 loaded ally GLBs + their per-instance state + the
// weapon GLBs the allies equip (one shared instance per weapon type, drawn at
// each ally's hand offset — bindless lets us instance the same GLB N times
// for free). Lifecycle:
//   1. build() once — mounts modelDir + weaponDir, loads each AllyKind GLB
//      and each AllyWeapon GLB the canon roster uses, creates 3 scene
//      Entities + 3 kinematic capsules, sets initial weapon assignments.
//   2. update(dt, ...) each frame — runs the AI state machine for each ally,
//      writes per-instance transforms, fires raycasts on the fire cooldown,
//      applies damage to hit Faction::Enemy bodies.
//   3. draw() each frame — issues drawMesh calls for each ally + its weapon
//      at the authored hand offset.
// ---------------------------------------------------------------------------
class AllyManager {
public:
    // Build the 3-ally squad next to `spawnPos`. `modelDir` mounts the ally
    // character GLBs; `weaponDir` mounts the player-weapon GLBs that the
    // allies equip. Missing GLBs fall back to per-slot tinted procedural
    // boxes. `physics` is used to add each ally's kinematic body so
    // hostiles can rayCast(Layer::Ally) and hit them.
    void build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics,
               std::string_view modelDir,
               std::string_view weaponDir,
               const x3::phys::Vec3& spawnPos);

    // Assign the squad's weapon loadout (Phase 1 default: Sarah=Pistol,
    // Martinez=SMG, AnnaBodySuit=Shotgun). Callable AFTER build(); also wired
    // through the console for live tuning. Pass AllyWeapon::None to disarm.
    void setWeapon(AllyKind k, AllyWeapon w);

    // Advance one frame. `playerPos` anchors Follow + friendly-fire checks.
    // `hostileQuery` finds nearby Faction::Enemy units (mirror of monster.h's
    // AllyQueryFn). `nav` is OPTIONAL — when non-null, the squad path-follows
    // around walls; when null, they take direct moves (no path).
    //
    // The state machine resolves per-ally:
    //   Follow -> Engage when hostileQuery returns at least one in range AND LOS.
    //   Engage -> Reposition every kAllyRepositionPeriod seconds (lateral step).
    //   Engage -> Reload when magRemaining <= 0.
    //   Engage -> Search when LOS to last hostile is broken.
    //   Engage -> TakeCover when hp/kAllyHp <= kAllyTakeCoverFrac.
    //   TakeCover -> Follow when timer hysteresis allows (no insta-flip).
    //
    // Firing is gated by fireCooldown (per-weapon) AND friendly-fire (the
    // ally NEVER fires while the player or another ally is within the fire
    // cone, regardless of g_friendlyfire; the rule is "courteous allies, not
    // surprise-teamkill bots").
    void update(float dt,
                Scene& scene,
                x3::phys::IPhysicsWorld& physics,
                const x3::phys::Vec3& playerPos,
                const HostileQueryFn& hostileQuery,
                x3::ai::INavigation* nav);

    // Resolve a raycast hit against the ally roster. Called by the host's
    // shoot-resolver (player fire / monster fire) to centralise the friendly-
    // fire rule: returns true if the ray's hit body matches a live ally AND
    // damage should land (factoring g_friendlyfire). Sets `outDamage` to the
    // applied amount if it returns true. Centralising here means the rule
    // change site is exactly one function.
    bool resolveHit(uint32_t bodyId,
                    Faction shooterFaction,
                    int shotDamage,
                    bool friendlyFireEnabled,
                    int& outDamage);

    // Draw all live allies + their weapons. Call AFTER scene.render(), same
    // pattern as MonsterSystem::draw. No-op if nothing built.
    void draw(x3::rhi::IRenderDevice& device,
              const x3::rhi::FrameContext& frame) const;

    // Diagnostics (HUD / logs / --test-coop).
    uint32_t aliveCount() const;
    uint32_t totalCount() const { return (uint32_t)m_allies.size(); }
    const AllyInstance& ally(uint32_t i) const { return m_allies[i]; }
    bool any() const { return !m_allies.empty(); }

    // Bench-arena helper (the WHOLE reason this lane exists). Spawns N enemies
    // around `arenaCenter` paired with the 3 already-built allies, returns the
    // entity ids of the spawned enemies for the bench harness to track. The
    // host's --bench-combat flag wires through this. Defined ALONGSIDE this
    // system rather than in MonsterManager because the arena is coop-specific
    // (the arena needs to know about both sides; this system already does).
    // Returns 0 if `build` hasn't been called yet.
    uint32_t makeBenchArena(Scene& scene,
                            x3::rhi::IRenderDevice& device,
                            x3::phys::IPhysicsWorld& physics,
                            uint32_t enemyCount,
                            const x3::phys::Vec3& arenaCenter);

private:
    // Load one AllyKind GLB into m_kindAssets[(uint32_t)k]. Returns true if
    // it loaded for real (false => the tinted procedural-box fallback was
    // installed in its place). Caches by AllyKind so the 3-ally squad does
    // 3 loads even if AllyKinds are reused.
    bool loadKindAsset(x3::rhi::IRenderDevice& device, AllyKind k);

    // Load one AllyWeapon GLB into m_weaponAssets[(uint32_t)w]. Returns true
    // on real load. Multiple allies sharing a weapon share the upload.
    bool loadWeaponAsset(x3::rhi::IRenderDevice& device, AllyWeapon w);

    // The per-state ticks. Each returns the NEXT state (may equal current).
    AllyState tickFollow    (AllyInstance& a, float dt, const x3::phys::Vec3& playerPos, const HostileQueryFn& q);
    AllyState tickEngage    (AllyInstance& a, float dt, const x3::phys::Vec3& playerPos, const HostileQueryFn& q, x3::phys::IPhysicsWorld& physics);
    AllyState tickReposition(AllyInstance& a, float dt, const x3::phys::Vec3& playerPos, const HostileQueryFn& q);
    AllyState tickReload    (AllyInstance& a, float dt);
    AllyState tickTakeCover (AllyInstance& a, float dt, const x3::phys::Vec3& playerPos, const HostileQueryFn& q);
    AllyState tickSearch    (AllyInstance& a, float dt, const HostileQueryFn& q);

    // Issue one shot from `a` toward `targetWorld`. Performs the raycast
    // against Layer::Enemy, applies damage to a resolved Faction::Enemy hit,
    // decrements magRemaining, and re-arms fireCooldown per-weapon. Skipped
    // silently if the line-of-fire intersects an ally / the player (the
    // "no surprise teamkill" rule, even with g_friendlyfire=1).
    void fireOnce(AllyInstance& a, x3::phys::IPhysicsWorld& physics,
                  const x3::phys::Vec3& targetWorld);

    AllyAsset                       m_kindAssets[(uint32_t)AllyKind::Count];
    AllyAsset                       m_weaponAssets[(uint32_t)AllyWeapon::Count];
    std::vector<AllyInstance>       m_allies;
    AllyDeathFxFn                   m_deathFx;
    std::unique_ptr<x3::asset::IAssetSource> m_modelAssets;
    std::unique_ptr<x3::asset::IAssetSource> m_weaponAssetSrc;
    std::unique_ptr<x3::asset::IModelLoader> m_loader;
};

} // namespace x3::game
