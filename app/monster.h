#pragma once
// Monster + combat: shoot a monster, it takes damage and dies (S6).
//
// Game/slice code only — engine/ stays pure. This is the FINAL step of the
// vertical slice and closes the loop: walk (S3) -> button/door (S4) -> weapon
// pickup (S5) -> SHOOT a monster (S6). Builds on S2 (Scene/Entity/Tag::Monster),
// S5's model-loading pattern (load the purchased alien_crawler.glb via a mounted
// loose-dir IAssetSource + the M2 loader, fall back to a procedural box), and the
// M3 physics interface (an Enemy-layer collision body for the shoot raycast).
//
// Flow:
//   * buildMonster() loads alien_crawler.glb (fallback: a procedural box), uploads
//     its drawables via the device, adds an Enemy-layer collision box body so a
//     rayCast(Layer::Enemy) can hit it, and registers a Tag::Monster Entity. The
//     monster starts with kMonsterHp HP.
//   * fire(eye, dir, scene, physics) is called by the host on a left-mouse rising
//     edge ONLY when the player is armed (caller checks WeaponSystem::hasWeapon()).
//     It raycasts along the look direction; if the hit body resolves (via
//     Scene::entityForBody) to a live Monster it applies kDamagePerShot and
//     triggers a brief red hit-flash. On HP <= 0 the monster dies: Entity hidden,
//     physics body removed (so subsequent rays miss), and marked dead.
//   * update(dt, scene, physics, playerPos) decays the hit-flash and (optionally,
//     gently) crawls a live monster toward the player via setBodyPosition.
//   * drawMonster() draws all model primitives at the monster's transform with the
//     current hit-flash tint (this system owns the multi-primitive draw, exactly
//     like WeaponSystem; the Entity's render mesh is left invalid so Scene::render
//     skips it).
//
// Design choices (documented for the slice):
//   * The monster body is a STATIC-by-mass Enemy box (addBox mass 0, Layer::Enemy):
//     it stays put under gravity but is hittable by an Enemy-mask ray (see the M3
//     JoltPhysicsWorld layer-mask rules) and movable by setBodyPosition (same
//     teleport trick the S4 door uses). It does NOT physically collide-and-push;
//     for the slice that's fine.
//   * Damage / death are factored into a pure applyDamage() helper so combat is
//     fully testable headlessly (see --test-combat) with no rendering.
//   * Chase moves the body toward the player (translation via setBodyPosition,
//     since IPhysicsWorld has no rotation setter) AND bakes a yaw-toward-player
//     rotation into the render transform's upper-left 3x3 each frame. This is safe
//     because Scene::update only overwrites the translation column and preserves
//     the 3x3, so the baked facing survives the per-frame physics sync (the host
//     must call update() AFTER scene.update() — see main loop order). kChaseSpeed
//     gates movement; kFaceSign flips the model's forward axis if needed.
//   * makeDrawables() bakes each glTF node's world TRS into the drawable
//     (nodeTransform); drawMonsterAt multiplies it in (model * nodeTransform), so
//     multi-node / Y-up-corrected GLBs place correctly (M2 node-TRS fix).
//   * Audio (gunshot / monster death sound) is DEFERRED — no audio system until
//     M9. No sound is played on fire or death.

#include "scene.h"
#include "player.h"   // IDamageSink (enemies attack the player)
#include "anim.h"     // J1: skeletal animation + CPU skinning runtime
#include "cues.h"     // game-feel footstep / impact cue hooks
#include "ragdoll.h"  // TASK#12: RagdollSkin (drive the skin from physics parts)

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/physics/Ragdoll.h"  // TASK#12: IRagdoll (skinned death ragdoll)
#include "engine/asset/IModelLoader.h"
#include "engine/asset/IAssetSource.h"
#include "engine/ai/INavigation.h"   // GENERAL navigation: route around walls (optional)
#include "engine/core/x3_damage.h"   // DamageType (Adaptive Hide: rotate-damage-type rhythm)

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace x3::game {

// Enemy archetype (Phase 2a, spec §6.5). Drives the attack behaviour in
// MonsterSystem::update:
//   * Guard — melee: chase, then deal `damage` every `attackCooldown` once within
//     `attackRange` (after a short wind-up).
//   * Drone — ranged: keep a standoff distance, periodically fire a hitscan toward
//     the player (with a brief visible tracer/telegraph) for `damage` on cooldown.
//   * Boss  — Chief Martinez: melee like a Guard but more HP/damage, PLUS a
//     multi-phase state machine keyed off HP thresholds (Phase 2b — see BossPhase).
enum class MonsterType : uint32_t { Guard = 0, Drone = 1, Boss = 2 };

// ---------------------------------------------------------------------------
// EnemyType — the DATA-DRIVEN bestiary roster (bestiary pass). Each value names a
// distinct enemy *species* from the EFLZ bestiary; the per-species stats / model /
// AI weighting live in a static MonsterDef table (see monsterDefs() / monsterDef()
// + tuningFor()), NOT in code. Adding a new enemy is then DATA: append a row.
//
// The roster reuses the existing combat-AI state machine (advance/attack/strafe/
// retreat/regroup/search), the combat:: balance bands, the locomotion blend, and
// the nav path-follow — only the per-row stats/weights differ. Models resolve from
// assets/rigged_glb (animated) or assets/converted_glb/Characters; a row whose GLB
// is absent falls back to the MonsterSystem's per-enemy tinted procedural box, so
// the bestiary never breaks a clean checkout. Values are grounded in the bible's
// TASK_6 enemy bestiary (HP/damage/speed/ranged) — see tuningFor() comments.
//
//   * DominionTrooper — baseline humanoid soldier (bible "Security Guard (Basic)":
//     HP 100, melee). The neutral reference enemy.
//   * Verthani        — insectoid: faster, flanks more (strafe-heavy), melee
//     (bible "Infected Human (Stage 1)" profile: fast, claw, erratic). Lower HP.
//   * Illuminated     — elite: ranged + "shielded" (modeled as higher HP), holds a
//     long standoff (bible "Security Guard (Elite)": HP 200, rifle 25). Standoff AI.
//   * BlueSynth       — synthetic: ranged drone-like flier (bible "Combat Drone":
//     HP 150, plasma, flanks). Uses blue_synth_seed*.glb if present (absent today
//     -> falls back to Drone.glb tinted blue, then to the procedural box).
enum class EnemyType : uint32_t {
    DominionTrooper = 0, Verthani = 1, Illuminated = 2, BlueSynth = 3, Count = 4
};

// Human-readable EnemyType name (logs / --test-bestiary trace / HUD).
const char* enemyTypeName(EnemyType t);

// ---------------------------------------------------------------------------
// Combat-AI behaviour state (D-ai). Each live enemy runs a per-instance state
// machine; FACING IS A CONSEQUENCE OF THE STATE (the enemy does NOT always face
// the player — that was the old swivel-turret behaviour we are replacing). The
// machine decides movement + heading; the heading yaw is then baked into the
// rendered transform (and, for rigid bodies, IPhysicsWorld::setBodyRotation).
//
//   * Idle    — no LOS, given up tracking: sit, occasional idle heading sweep.
//   * Search  — just lost LOS: move to the player's last-known position while
//               sweeping the heading left/right ("looking around"); times out to
//               Idle. Does NOT track the live player.
//   * Advance — has LOS, out of strike range: move toward + FACE the player.
//   * Attack  — in strike range: FACE the player and run the existing attack
//               (melee reach / drone hitscan) on its cooldown.
//   * Strafe  — mid-range circling: orbit the player laterally while FACING the
//               player (the only "face you while moving sideways" state).
//   * Retreat — low HP / heavy recent damage / cornered: back off to a safer
//               distance, FACING AWAY from the player (toward the retreat point).
//   * Regroup — fall back toward nearby allies to re-form; FACE the rally
//               direction while moving, then re-engage (-> Advance/Attack).
//   * Patrol  — (guard-life pass) the calm-state replacement for Idle on rows
//               with Tuning.patrolRadius > 0: walk a small waypoint loop around
//               the SPAWN anchor at walk speed, pause-look at each corner.
//               Detection/aggro outranks it exactly like Idle; a given-up Search
//               returns here instead of Idle.
enum class AiState : uint32_t {
    Idle = 0, Search = 1, Advance = 2, Attack = 3, Strafe = 4, Retreat = 5, Regroup = 6,
    Patrol = 7   // APPENDED (stable values per the log/test contract)
};

// Human-readable AI state name (for logs / the --test-ai transition trace).
const char* aiStateName(AiState s);

// Boss phase (Phase 2b, spec §8 + EFLZ_DESIGN.md Chief Martinez). A Boss-type
// monster advances through phases as its HP drops past tuned thresholds. Each
// phase scales speed/damage and re-tints/re-scales the model, plus fires a
// one-shot host callback (HUD flash / audio / summon adds). The machine is a
// pure HP-threshold latch (monotone: it only advances, never reverts).
//   * Phase1 (>66% HP)        — baseline aggression (cover/posturing in the bible).
//   * Phase2 (66%..33% HP)    — ENRAGE: faster chase + harder melee + reddened/
//                               scaled tint.
//   * Phase3 (<33% HP)        — DESPERATE: faster still + a one-time summon of
//                               Guard adds (the bible's "summons" beat).
// Non-Boss monsters always report Phase1 and never transition.
enum class BossPhase : uint32_t { Phase1 = 0, Phase2 = 1, Phase3 = 2 };

// Callback the MonsterSystem invokes ONCE when a Boss crosses into a new phase.
// `newPhase` is the phase just entered. The host wires this to: a "PHASE 2!/
// PHASE 3!" HUD flash + log + audio cue, and (for Phase3) spawning summoned adds
// via the MonsterManager. Optional (may be empty). Fired from update().
using BossPhaseFn = std::function<void(BossPhase newPhase)>;

// Callback the MonsterSystem invokes to spawn a visible attack effect (a ranged
// drone's hitscan tracer / a melee wind-up telegraph). `from`->`to` is the beam;
// the host wires this to CombatFx::addTracer. Optional (may be empty).
using AttackFxFn = std::function<void(const x3::phys::Vec3& from,
                                      const x3::phys::Vec3& to)>;

// Death FX sink (gib burst). The MonsterSystem fires this ONCE at the moment it is
// KILLED (HP hits 0 via fire()/melee) — NOT on a cure/spare. `pos` is the body-center
// world position (m_pos + the box-center offset); `flying` is true for a flyer/drone
// (the host can spark a flyer death differently). The host wires this to a violent gib
// explosion: a GPU debris burst (gpuDebrisSpawnBurst) + a cluster of blood impacts
// (CombatFx::spawnImpact). Optional (may be empty => no extra FX; the existing death-
// pop/topple is unaffected). Cheap to copy (a std::function).
using DeathFxFn = std::function<void(const float pos[3], bool flying)>;

// Ally query (D-ai, Regroup). The host fills `out` with up to `maxOut` nearby
// LIVE ally positions within `radius` of `self`, EXCLUDING the querying enemy
// (matched by `selfEntity`), and returns the count written. The MonsterManager
// provides a default implementation over its own instances; Level 1 composes the
// groups. Empty => no allies known => the enemy never regroups (still fights).
// No heap alloc in the hot path: `out` is a caller-owned fixed buffer.
using AllyQueryFn = std::function<uint32_t(const x3::phys::Vec3& self,
                                           uint32_t selfEntity, float radius,
                                           x3::phys::Vec3* out, uint32_t maxOut)>;

// Monster starting health, and damage per shot. 100 / 34 => 3 shots to kill.
constexpr int   kMonsterHp     = 100;
constexpr int   kDamagePerShot = 34;

// Max shoot range (meters) for the fire raycast.
constexpr float kFireMaxDist   = 100.0f;

// Hit-flash duration (seconds): the monster tints toward red for this long after
// a hit, then decays back to its base color.
constexpr float kHitFlashTime  = 0.1f;

// Footstep cadence: a footstep cue fires each time the locomotion phase crosses
// one of this-many evenly-spaced marks per cycle (2 = a step on each half-cycle,
// i.e. a left/right foot plant). VISUAL/AUDIO TUNING.
constexpr int   kFootstepsPerCycle = 2;
// Minimum planar speed (m/s) to emit footsteps (don't tick steps while idling).
constexpr float kFootstepMinSpeed  = 0.4f;

// Gentle chase speed (m/s) toward the player (player walks 5 m/s). Set > 0 to
// enable chasing. Translation toward the player + facing rotation baked into the
// render 3x3 each frame (see update()).
constexpr float kChaseSpeed    = 2.5f;

// Stop distance (meters, horizontal): the monster crawls toward the player only
// while horizontal distance exceeds this, so it doesn't grind into the player.
constexpr float kChaseStopDist = 1.5f;

// Wander/strafe so the crawler weaves while approaching and orbits when close,
// instead of beelining then freezing ("glued to player"). VISUAL TUNING.
constexpr float kStrafeFreq    = 1.6f;   // weave oscillation rate (rad/s)
constexpr float kStrafeAmt     = 0.7f;   // perpendicular weave strength while approaching
constexpr float kOrbitRetarget = 1.8f;   // seconds before flipping orbit direction when close

// Facing sign for the model's authored forward axis. glTF convention is local -Z
// forward, so the default -1 makes local -Z point at the player. Flip to +1 if
// the model turns its back to the player (authored +Z forward). VISUAL TUNING.
constexpr float kFaceSign      = -1.0f;

// Max VERTICAL separation (m) between an attacker's body center and the player at
// which ANY attack (melee or ranged) may land. The combat attack gate previously
// keyed only on the PLANAR (x,z) distance `horiz`, so an enemy a full floor ABOVE
// the player (planar distance ~0, but several metres up, with a Static floor slab
// between them) would "attack through the floor". A melee/ranged hit must also be
// within this vertical band AND have clear line-of-sight (rayCast vs Layer::Static,
// which the floor/ceiling slabs are) — see the attack block in update(). 2.5 m
// comfortably covers a tall boss + a crouch/jump but rejects a one-floor (~3 m+)
// vertical stack. Floors are LOS-blocked anyway; this is the cheap belt-and-braces
// bound so even a doorway sightline up a ramp can't be exploited as a free hit.
constexpr float kAttackMaxVertical  = 2.5f;

// ===========================================================================
// GENERAL COMBAT BALANCE PARAMS (playtest-fix). Named, reusable across games —
// these are the single source of truth for "how hard does a squad hit, and how
// many can pile on at once", so a level never has to bury magic numbers in its
// per-enemy Tuning. The corridor squad (3 guards + 2 drones) is balanced against
// these: an enemy can only damage the player ONCE per (its) cooldown (enforced
// in MonsterSystem::update via m_atkTimer + the wind-up), and only a CAPPED
// number of melee attackers may swing at once (enforced in MonsterManager::update
// via per-frame melee-attack permits) so the player isn't melted by a dogpile.
//
// Level tunings should pull their per-enemy damage/cooldown from these bands
// rather than hand-rolling values (guardTuning/droneTuning in level1_game.cpp do).
// All are gameplay tuning — safe to retune for playtest; the self-tests assert the
// BEHAVIOUR (cooldown gating + attacker cap), not the exact numbers.
// ---------------------------------------------------------------------------
namespace combat {

// ---- Melee (Guard / Boss) ----
// Per-swing damage band. A focused player (100 HP, 0.5 s iframes) survives a
// small squad: with the attacker cap below, at most kMaxMeleeAttackers swing, each
// on its own ~1 s cooldown, and the player's iframe window further gates DPS.
constexpr int   kMeleeDamageMin     = 6;     // weakest melee swing (HP)
constexpr int   kMeleeDamageMax     = 10;    // strongest basic melee swing (HP)
constexpr int   kMeleeDamageDefault = 8;     // the value Level 1 guards use
// Seconds between melee swings (the per-enemy attack cooldown). >= ~1 s so an
// enemy CANNOT deal damage every frame (the playtest "8 dmg every tick" bug).
constexpr float kMeleeCooldownMin     = 1.0f;
constexpr float kMeleeCooldownMax     = 1.3f;
constexpr float kMeleeCooldownDefault = 1.1f;
// Melee reach (m): how close a melee enemy must be to land a hit.
constexpr float kMeleeRange         = 1.9f;
// Telegraph (s) before a melee hit lands, so the swing reads + is dodgeable.
constexpr float kMeleeWindup        = 0.25f;

// ---- Ranged (Drone) ----
constexpr int   kRangedDamageMin     = 4;    // weakest ranged bolt (HP)
constexpr int   kRangedDamageMax     = 6;    // strongest ranged bolt (HP)
constexpr int   kRangedDamageDefault = 5;    // the value Level 1 drones use
constexpr float kRangedCooldownMin     = 0.8f;
constexpr float kRangedCooldownMax     = 1.2f;
constexpr float kRangedCooldownDefault = 1.4f; // Level 1 drones fire a touch slower
constexpr float kRangedStandoff      = 7.0f; // preferred distance the drone keeps

// ---- Dogpile limiting ----
// The hard cap on how many MELEE enemies may be actively swinging at the player at
// the same time. Others hold at a standoff ring (kStandoffRing) and wait their turn
// — they still advance/flank/face the player, they just don't get an attack permit.
// Ranged enemies are NOT counted by this cap (they keep their distance anyway).
constexpr uint32_t kMaxMeleeAttackers = 2;
// Standoff ring (m): a melee enemy that did NOT get an attack permit holds roughly
// this far out (just beyond melee reach) instead of stacking onto the player's tile.
constexpr float kStandoffRing       = 2.6f;

// Clamp a value to [lo,hi] (used to keep level tunings inside the sane bands).
constexpr int   clampDamage(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
constexpr float clampCooldown(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace combat

// ---------------------------------------------------------------------------
// Combat-AI tuning (D-ai). Defaults; per-MonsterType weighting is applied in the
// state machine (e.g. Drone strafes/flanks more, Guard advances harder). All are
// VISUAL/BEHAVIOUR TUNING — safe to retune for playtest.
// ---------------------------------------------------------------------------
// Health fraction at/below which the enemy prefers to Retreat. Hysteresis: it
// only LEAVES retreat once HP recovers above kAiRetreatExitFrac (it can't heal
// here, so retreat is mainly broken by reaching a safe distance / a timer).
constexpr float kAiRetreatFrac     = 0.30f;  // enter Retreat at <= 30% HP
constexpr float kAiRetreatExitFrac = 0.45f;  // (would) leave Retreat above 45% HP
// A burst of recent damage also triggers a brief flinch-retreat even at high HP.
constexpr int   kAiHeavyDmgWindowHp = 50;    // HP lost within the window => flinch
constexpr float kAiDamageMemory     = 1.2f;  // seconds the "recent damage" lingers
constexpr float kAiRetreatMinTime   = 1.0f;  // min seconds to stay retreating (hysteresis)
constexpr float kAiRetreatDist      = 9.0f;  // back off until at least this far (m)
// Distance bands (m) for Advance vs Strafe. Closer than the strafe band -> Attack/
// Strafe; farther -> Advance. The drone uses its own m_standoff instead.
constexpr float kAiStrafeBandLo    = 2.5f;   // start orbiting when this close
constexpr float kAiStrafeBandHi    = 6.0f;   // beyond this, just advance
// LOS / Search. Lost LOS -> Search for kAiSearchTime, walking to the last-known
// player position + sweeping the heading; then -> Idle.
constexpr float kAiSearchTime      = 4.0f;   // seconds to search before giving up
constexpr float kAiSearchSweepFreq = 2.2f;   // heading sweep rate while searching (rad/s)
constexpr float kAiSearchSweepAmp  = 1.0f;   // heading sweep amplitude (rad)
constexpr float kAiLastKnownReach  = 1.5f;   // "arrived" at last-known within this (m)
// Regroup: if an ally is within this radius and the enemy is pressured, fall back
// toward the ally to re-form, then re-engage. 0 allies in range -> no regroup.
constexpr float kAiRegroupRadius   = 12.0f;  // look for allies within this (m)
constexpr float kAiRegroupHold     = 1.2f;   // min seconds to stay regrouping
// Turn rate (rad/s): the heading slews toward its target instead of snapping, so
// turns read as a body rotation (and states don't visually jitter).
constexpr float kAiTurnRate        = 7.0f;
// ---- Inter-enemy SEPARATION (anti-crowding, playtest-fix) --------------------
// Boids-style separation so a squad SPREADS OUT instead of piling onto the player's
// tile (the playtest "enemies crowd the player / pile up" bug — the dogpile cap
// limited who ATTACKS but not who STACKS). Each moving enemy is pushed away from
// nearby allies within kAiSeparationRadius; the push is blended into its desired
// move direction at kAiSeparationWeight strength (capped so it nudges, never
// overrides, the state's intent). Behavioural tuning — the self-tests assert state
// + facing, not exact spacing, so this is safe.
// Playtest polish 2026-07 (Tim: "characters must NEVER want to crowd"): the earlier
// values (2.2m / 0.9) were too weak — a chasing squad still collapsed onto one tile.
// Widened the feel-radius (they start spreading sooner) and roughly doubled the push
// so a group forms a loose RING/arc around the target at ~1.2-1.6m spacing instead of
// stacking. Backed by the HARD de-overlap pass (deOverlapMembers) so overlap is
// impossible even if the steering equilibrium is briefly overrun.
constexpr float kAiSeparationRadius = 3.2f;  // allies closer than this push us apart (m)
constexpr float kAiSeparationWeight = 1.6f;  // how hard separation steers vs the state dir
// Per-instance decision cadence + jitter so enemies don't all switch in lockstep.
constexpr float kAiDecisionPeriod  = 0.30f;  // re-evaluate state every ~0.3 s
constexpr float kAiDecisionJitter  = 0.15f;  // +/- randomization on the cadence (s)
constexpr float kAiStateMinTime    = 0.45f;  // min dwell before a non-forced switch

// Enemy-SFX taunt cadence: an engaged (has-LOS) live enemy emits an idle/harass
// TAUNT vocalization roughly every kAiTauntPeriod seconds (+/- kAiTauntJitter), so
// the enemies HARASS audibly at range instead of being silent. Tuning — safe to
// retune; spaced wide so a squad isn't a wall of noise.
constexpr float kAiTauntPeriod     = 3.5f;   // mean seconds between taunts
constexpr float kAiTauntJitter     = 1.5f;   // +/- randomization on the cadence (s)

// GENERAL navigation cadence: when a nav grid is attached, rebuild the A* path to
// the move target every this-many seconds (NOT every frame — pathfinding is cheap
// but not free, and waypoints stay valid between rebuilds). The agent steers toward
// the current next waypoint each frame in between. VISUAL/PERF TUNING.
constexpr float kNavRepathPeriod = 0.5f;     // re-run A* twice a second
constexpr float kNavWaypointArrive = 0.6f;   // "reached" a waypoint within this (m)

// Death "pop" duration (seconds): on death the physics body is removed
// IMMEDIATELY (rays miss right away) but the model keeps DRAWING for this long,
// shrinking toward zero and flashing bright, then hides. Gives shot feedback.
constexpr float kDeathPopTime  = 0.25f;

// Death TOPPLE duration (seconds): replaces the old shrink-poof. On death the
// model falls over (rotates ~90deg about its feet) across this window with a brief
// white flash, then settles as a corpse on the ground.
constexpr float kDeathToppleTime = 0.7f;

// Corpse DESPAWN delay (seconds, BUG#30): after the topple/gib finishes and the
// monster settles into a corpse, it lingers for this long and then DISAPPEARS —
// the Entity is hidden, its physics body is already gone, and its GPU skinned-mesh
// registration is freed. A killed monster no longer stands/lies around forever.
// ~2.5 s reads the kill, then clears the screen. A gib-class kill despawns at once
// (see kGibImmediateDespawn): the body burst into chunks, so there is no corpse.
constexpr float kCorpseDespawnTime = 2.5f;

// Result of a fire() call, returned for HUD/FX/logging and used by the self-test.
struct FireResult {
    bool hitMonster = false;  // the ray hit a live monster
    bool killed     = false;  // this shot dropped it to HP <= 0
    int  hpAfter    = 0;      // monster HP after the shot (0 if it died / no hit)
    bool hit        = false;  // the ray hit ANY body (monster, wall, etc.)
    x3::phys::Vec3 hitPoint{};// world-space hit point (valid iff hit)
    x3::phys::Vec3 endPoint{};// FX tracer end: hitPoint on a hit, else eye+dir*range
};

// Pure damage rule, factored out so it is testable headlessly. Subtracts `damage`
// from `*hp` (clamped at 0) and returns true iff the monster is now dead (hp<=0).
// Caller owns the live/dead latch + body removal; this only mutates the number.
bool applyDamage(int* hp, int damage);

// Monster + combat system. Owns the loaded model (keeps the loader + Model alive
// so the GPU handles in the drawables stay valid for the app's lifetime) and the
// gameplay state (HP, alive/dead, hit-flash timer). Single-monster for the slice,
// but the entity/body are looked up through the Scene so multi-monster is a small
// extension.
class MonsterSystem {
public:
    // Per-instance tuning so the same MonsterSystem can be a basic enemy or a
    // boss-tier "Martinez" (more HP, a bit faster) WITHOUT new AI/phases this
    // pass (§8). Defaults reproduce the original single-monster behaviour.
    struct Tuning {
        int   hp         = kMonsterHp;     // starting health
        float chaseSpeed = kChaseSpeed;    // m/s toward the player (0 = stationary)
        float modelScale = -1.0f;          // <0 => use the per-model default scale
        float tint[4]    = { 1, 1, 1, 1 }; // multiplied into the model base color
        // PBR draw path: scales the GLB's AUTHORED material emissive (visors, eyes,
        // engine glow). 1 = as authored, 0 = suppressed, >1 = boosted. Kept modest
        // per-archetype so no enemy blooms into a shapeless white blob (R5 lesson:
        // uncontrolled material emissive + HDR bloom = glowing bags).
        float emissiveScale = 1.0f;

        // ---- Model override (EFLZ art pass) -------------------------------
        // GLB filename to load (relative to the mounted modelDir). Empty => the
        // legacy "alien_crawler.glb" (keeps --test-combat / --test-level1 green).
        std::string modelFile;
        // Loose-dir to load `modelFile` from. Empty => the modelDir passed to
        // buildMonster (the legacy rigged_glb). Lets Level 1 point characters at
        // G:/GameModels/converted_glb while the tests stay on rigged_glb.
        std::string modelDirOverride;
        // Stand-up fixup: the converted character GLBs are authored Z-up (lying
        // flat — height runs along +Z, feet at z=0). Set true to rotate -90deg
        // about X so they stand upright in our Y-up world (feet land at y=0). The
        // ModularSciFi / crawler models are already Y-up, so leave false for them.
        bool  standUpZtoY = false;

        // ---- Attack behaviour (Phase 2a, spec §6.5) -----------------------
        MonsterType type          = MonsterType::Guard;
        int   damage              = 0;     // HP dealt to the player per attack (0 = inert)
        float attackRange         = 1.8f;  // melee reach (m) / drone fire range (m)
        float attackCooldown      = 1.0f;  // seconds between attacks
        float attackWindup        = 0.25f; // telegraph delay (s) before the hit lands
        bool  ranged              = false; // true => hitscan toward the player (drone)
        // Drone-only: preferred standoff distance (m). The drone strafes to keep
        // roughly this far from the player instead of closing to melee.
        float standoff            = 6.0f;
        // FLYER: hovers off the floor with a model origin at its CENTER (not the
        // feet). Distinct from `type==Drone`, which is just the ranged AI lane and
        // is ALSO used by ground elites (e.g. Illuminated). Drives hover + hitbox.
        bool  flyer               = false;

        // ROOT-Y LOCK (Skinner::setRootYLock, see anim.h): clamp the clip-animated
        // root (pelvis) local Y to its rest-pose Y so a clip with a BROKEN baked
        // root Y (the retarget "buried half-way + bouncing" family) can never
        // sink/bob the character through the floor. For characters whose world Y
        // is OWNED by the feeder (crowd citizens fed via setPropPose). Off by
        // default — combat enemies keep their authored root bob. Authored
        // triggerClip crossfades (Sit/Jump) still move their root.
        bool  lockRootY           = false;

        // ---- Data-driven AI weighting (bestiary pass) ---------------------
        // Per-instance strafe/flank bias in [0,1]: the probability, at mid-range,
        // that the enemy STRAFES (orbits/flanks) instead of straight-Advancing. A
        // negative value (the default) means "use the MonsterType default" in the
        // state machine (Guard 0.20, Drone 0.75, Boss 0.45) so existing enemies are
        // unchanged. The MonsterDef roster sets this per archetype (e.g. Verthani
        // strafe-heavy ~0.80, Illuminated standoff-low ~0.10) so AI weighting is
        // DATA, not new code. Clamped to [0,1] when >= 0.
        float aiStrafeBias        = -1.0f;

        // ---- Guard-life (W4-3): species identity + patrol ------------------
        // Which bestiary species this row is. Carried into every GameCue the
        // instance emits (cue.species) so the host can pick per-species vocal /
        // footstep samples. tuningFor() stamps it per roster row; hand-built
        // tunings keep the default and read as the baseline humanoid bucket.
        EnemyType species         = EnemyType::DominionTrooper;
        // PATROL: > 0 enables the Patrol calm state — a diamond waypoint loop of
        // this radius (m) around the spawn anchor, walked at chaseSpeed *
        // patrolSpeedMul with patrolPauseSec look-around beats at each corner.
        // 0 (the default) keeps the original stand-still Idle for every existing
        // enemy, test, and hand-built tuning — INERT-BY-DEFAULT per the Tuning
        // hook house pattern.
        float patrolRadius        = 0.0f;
        float patrolPauseSec      = 1.6f;
        float patrolSpeedMul      = 0.45f;   // walk fraction of chaseSpeed

        // ---- Boss phases (Phase 2b, spec §8) ------------------------------
        // Only consulted for type == Boss. HP-fraction thresholds (of maxHp)
        // at which the boss enters Phase2 / Phase3. Defaults: 2/3 and 1/3.
        float phase2Frac          = 0.66f; // enter Phase2 when hp/maxHp <= this
        float phase3Frac          = 0.33f; // enter Phase3 when hp/maxHp <= this
        // Per-phase multipliers applied OVER the base chaseSpeed / damage when the
        // phase is active (Phase1 = 1x; later phases ramp up the pressure).
        float phase2SpeedMul      = 1.35f;
        float phase2DamageMul     = 1.4f;
        float phase3SpeedMul      = 1.7f;
        float phase3DamageMul     = 1.8f;
        // Phase2 "enrage" cosmetic: extra red tint push + model up-scale so the
        // phase shift reads on screen (graybox feedback, no new animations).
        float phase2ScaleMul      = 1.15f;
        float phase3ScaleMul      = 1.30f;
        // Phase3 "desperate" summon: how many Guard adds the boss summons once on
        // entering Phase3 (the bible's "summons" beat). The host owns the actual
        // spawn (it has the MonsterManager); this is just the count it should use.
        int   phase3SummonCount   = 2;

        // ---- Act-1 boss gimmicks (Wave 1). DATA-DRIVEN per-boss hooks, all inert
        // by default so Chief Martinez and every existing monster are unchanged. ----
        // KILL-vs-CURE outcome (Dr. Chen, F2). When true the boss exposes an
        // incapacitate/cure path: instead of dying on HP<=0, it can be "cured" once
        // it reaches its Phase3 ("Monster") window — the host calls cure() to spare
        // it (Chen survives -> 100% cure ally) rather than fire()-killing it (50%
        // cure formula). The machine just tracks the outcome; the floor module wires
        // the narrative. False (default) => the normal lethal death path only.
        bool  hasCureOption       = false;
        // MEMORY FLASH vulnerability (Failed Experiment #7, F3). When > 0, on EACH
        // phase transition the boss enters a brief "memory flash": for this many
        // seconds it is STAGGERED (cannot attack) and takes amplified damage
        // (memoryFlashDamageMul). Models the bible's clarity/Memory-Flash windows as
        // a simple, data-driven vulnerability beat. 0 (default) => no flash.
        float memoryFlashTime     = 0.0f;
        float memoryFlashDamageMul = 1.0f;  // damage multiplier while flashing (>1 = vulnerable)

        // ---- Act-2 boss/enemy gimmicks (Wave 2). DATA-DRIVEN per-row hooks, all
        // inert by default so Act-1 bosses and existing enemies are unchanged. ----
        // START ALLIED (Salvari ally, L8+). When true, the spawned MonsterSystem is
        // FLIPPED TO ALLIED at build time: m_allied = true, m_dmg forced to 0 so it
        // cannot harm the player (matches the post-build convertToAllied() result).
        // The host owns any re-tinting / re-targeting; the gameplay-state half of
        // "this monster fights beside the player" is right here. False (default) =>
        // normal hostile spawn (the Act-1 + existing roster behaviour, unchanged).
        bool  startAllied         = false;

        // COPY / FEINT phase descriptor (Memory Hunter, Act-2 L12). 0 (default) => no
        // copy-phase. 1 / 2 / 3 nominate WHICH boss phase the copy/feint mechanic is
        // active in (typically Phase2: the boss copies a previously-defeated enemy's
        // moveset and feints — the bible's "memory warfare"). DATA-DRIVEN: this is a
        // phase TAG the floor module reads (HUD + spawn the illusion adds); the boss
        // machine itself does not implement the copy logic, only labels the phase.
        uint32_t copyFeintPhase    = 0;

        // ESCAPE TIMER (Planetary Garrison Commander, Act-2 L20 finale). When > 0,
        // the boss broadcasts an "orbital strike" countdown the moment it enters
        // Phase3: this many seconds before the strike lands. The floor module reads
        // it to drive a HUD timer + the level-exit trigger ("escape or die"); the
        // boss machine just carries the value. 0 (default) => no escape timer.
        float escapeTimerSeconds   = 0.0f;

        // ---- SKINNED CITIZENS (crowd skin layer). True => skip the Enemy-layer
        // collision body entirely: the character is a PURE VISUAL (crowd agents
        // are kinematic by design — no hitbox, no ray interception, nothing for
        // a stray Enemy-mask raycast to eat). m_body stays invalid; update()'s
        // body-rotation sync already guards on m_body.valid(). False (default)
        // keeps every existing monster/prop identical. ----
        bool  noBody              = false;

        // ---- GROUNDING OPT-OUT (app/grounding.h). Every non-flyer character is
        // seated on the surface under it at build time, so authored Y constants
        // can no longer put feet inside a floor. Set this ONLY for a character
        // that is deliberately not standing on the ground and whose intended
        // height is within 2 m of a surface (beyond that groundCharacter already
        // refuses to move it): suspended pods, a body on a slab, a rider. Default
        // false — every existing row is grounded, which is the point. ----
        bool  skipGrounding       = false;

        // ---- Adaptive Hide (canon-aliens SaurianWarlord). Boss-style "rotate
        // damage type" rhythm: after taking damage of type T, gain `adaptiveHideResist`
        // reduction on ALL further damage of type T for `adaptiveHideDurationSec`
        // seconds, then re-evaluate. INERT by default (resist == 0 leaves every
        // existing row unchanged); only rows that opt in get the behaviour.
        // Stacks multiplicatively with memoryFlashDamageMul (a matched type during a
        // flash window: 1.5 * 0.4 = 0.6, still reduced but less harshly — exactly
        // the design intent that rotating during a flash multiplies the bonus).
        float adaptiveHideResist      = 0.0f;   // 0..1; e.g. 0.6 == 60% reduction
        float adaptiveHideDurationSec = 8.0f;   // window length; unused when resist == 0
    };

    // Build the monster: load alien_crawler.glb from `modelDir` via a fresh
    // IAssetSource + the M2 model loader, upload its drawables through `device`,
    // add an Enemy-layer collision box body via `physics` (sized to roughly the
    // model bounds), and register a Tag::Monster Entity at `pos` in `scene`. On
    // load failure a procedural box monster is used instead. Logs which path
    // (real GLB vs. fallback box) was taken. Call once.
    void buildMonster(Scene& scene, x3::rhi::IRenderDevice& device,
                      x3::phys::IPhysicsWorld& physics,
                      std::string_view modelDir, const x3::phys::Vec3& pos);

    // As buildMonster(), but apply per-instance Tuning (HP / chase speed / scale /
    // tint). Used for the corridor guards, the drone, and boss-tier Martinez.
    void buildMonsterTuned(Scene& scene, x3::rhi::IRenderDevice& device,
                           x3::phys::IPhysicsWorld& physics,
                           std::string_view modelDir, const x3::phys::Vec3& pos,
                           const Tuning& tuning);

    // Fire one shot: raycast from `eye` along `dir` (need not be unit; normalized
    // internally) up to kFireMaxDist against the Enemy layer. If the hit body
    // resolves to THIS live monster, apply kDamagePerShot, start the hit-flash,
    // and — if HP drops to 0 — kill it (hide Entity, remove body, mark dead).
    // Returns what happened (see FireResult). The caller is responsible for the
    // "only if armed" gate (WeaponSystem::hasWeapon()); a no-op here when dead.
    // `damage` is the firing weapon's per-shot damage (defaults to kDamagePerShot
    // for legacy/test paths); a HEADSHOT (hit in the upper hitbox) deals 3x.
    FireResult fire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                    Scene& scene, x3::phys::IPhysicsWorld& physics,
                    int damage = kDamagePerShot,
                    x3::DamageType type = x3::DamageType::Kinetic);

    // [P2-5] Single-raycast fire path (specs/MONSTER_FIRE_SINGLE_RAY.spec.md).
    // Resolve a PRECOMPUTED Enemy-layer ray hit against THIS monster: if the hit
    // body is this live monster's, apply the full fire() damage path (headshot /
    // adaptive-hide / memory-flash / death) and return the same FireResult fire()
    // would. If the hit is some other body (or a miss), report the geometry
    // hit/miss for the tracer and no-op on this instance. This lets a container
    // (MonsterManager / MultiPodBoss) cast ONE ray per shot and fan the SAME hit
    // across all instances instead of one rayCast per monster. fire() itself is
    // now cast-one-ray + applyFireHit(), so single-monster behaviour is unchanged.
    // `eye`/`dir` are the original shot ray (used only for the miss-tracer end
    // point and the death-ragdoll shove direction).
    FireResult applyFireHit(const x3::phys::RayHit& hit,
                            const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                            Scene& scene, x3::phys::IPhysicsWorld& physics,
                            int damage = kDamagePerShot,
                            x3::DamageType type = x3::DamageType::Kinetic);

    // Advance one frame: decay the hit-flash timer and, if chaseSpeed > 0 and the
    // monster is alive, move it relative to `playerPos` (chase for melee, hold a
    // standoff for the drone) via setBodyPosition + sync the Entity transform, and
    // run its attack (melee reach hit / ranged hitscan) against `target` on
    // cooldown. For a Boss, also advance the HP-keyed phase machine (firing
    // `onPhase` once per transition). `target` may be null (no attacks — pure
    // movement, e.g. the legacy single-monster path). `fx`, if set, is invoked to
    // spawn a visible attack beam (drone tracer / melee telegraph). `onPhase`, if
    // set, fires when a Boss enters a new phase. No-op once dead.
    void update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                const x3::phys::Vec3& playerPos,
                IDamageSink* target, const AttackFxFn& fx, const BossPhaseFn& onPhase);

    // D-ai full overload: same as above PLUS an ally query (for Regroup) and the
    // player's eye position used for the LOS rayCast. `playerPos` is the planar
    // tracking target (foot/center) and `playerEye` is the LOS endpoint (may be
    // the same). `allies` may be empty (no regroup). This is the canonical entry
    // point; the shorter overloads forward to it with empty extras.
    void update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                const x3::phys::Vec3& playerPos, const x3::phys::Vec3& playerEye,
                IDamageSink* target, const AttackFxFn& fx, const BossPhaseFn& onPhase,
                const AllyQueryFn& allies);

    // As above but with no phase callback (still advances the phase machine).
    void update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                const x3::phys::Vec3& playerPos,
                IDamageSink* target, const AttackFxFn& fx);

    // Movement-only overload (no attacks): the legacy single-monster behaviour.
    // Forwards to the full update() with a null target / empty fx.
    void update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                const x3::phys::Vec3& playerPos);

    // Take melee damage (Phase 2b super-strength punch). Like fire()'s damage path
    // but the caller has already resolved that THIS monster is in the arc — it just
    // applies `damage` (heavy), starts the hit-flash, and kills on HP<=0 (hide +
    // remove body + death-pop), exactly as a lethal shot does. Returns the killed
    // flag. No-op (returns false) when already dead. The knockback impulse is the
    // caller's job (it owns the physics body + direction).
    bool takeMeleeDamage(int damage, Scene& scene, x3::phys::IPhysicsWorld& physics,
                         x3::DamageType type = x3::DamageType::Melee);

    // Enemy archetype + attack params (read for the HUD / self-test / tuning).
    MonsterType type() const { return m_type; }
    int   attackDamage() const { return m_dmg; }
    float attackRange() const { return m_attackRange; }
    float attackCooldown() const { return m_attackCooldown; }
    bool  ranged() const { return m_ranged; }

    // ---- Dogpile limiting (playtest-fix) ----------------------------------
    // Per-frame MELEE attack permit. MonsterManager grants a permit to only the
    // nearest combat::kMaxMeleeAttackers melee enemies each frame; the rest are
    // DENIED a permit and hold at the standoff ring instead of swinging. A denied
    // melee enemy never lands a melee hit that frame (it still advances/faces the
    // player). Ranged enemies ignore this (always permitted — they keep distance).
    // Default true reproduces the original "everyone may attack" behaviour when no
    // manager arbitrates (e.g. the single-monster --test-combat path).
    void setMeleeAttackPermit(bool allow) { m_meleePermit = allow; }
    bool meleeAttackPermit() const { return m_meleePermit; }
    // True iff this is a melee enemy currently inside its attack reach of the player
    // (read by MonsterManager to decide who competes for an attack permit). Uses the
    // last-known planar distance computed in update().
    bool inMeleeRange() const { return !m_ranged && m_dmg > 0 && m_lastHoriz <= m_attackRange; }
    // Horizontal distance to the player measured at the last update() (diagnostics +
    // the manager's nearest-attacker selection). Large until first update().
    float distToPlayer() const { return m_lastHoriz; }

    // ---- Boss phase state (Phase 2b) --------------------------------------
    // Current boss phase (always Phase1 for non-Boss monsters).
    BossPhase phase() const { return m_phase; }
    // Effective (phase-scaled) chase speed + attack damage in the current phase.
    // Useful for the HUD / self-test to observe that enrage actually changed stats.
    float effectiveChaseSpeed() const { return m_chaseSpeed * m_phaseSpeedMul; }
    int   effectiveDamage() const;
    // How many Guard adds this boss wants summoned on entering Phase3 (read by the
    // host's phase callback). 0 for non-Boss.
    int   summonCount() const { return m_phase3SummonCount; }

    // ---- Act-1 boss gimmicks (Wave 1) -------------------------------------
    // KILL-vs-CURE (Dr. Chen, F2). True iff this boss was built with the cure
    // option (Tuning::hasCureOption). The floor module reads this to offer the
    // "incapacitate + cure" prompt instead of (only) killing.
    bool  hasCureOption() const { return m_hasCureOption; }
    // True iff the boss is currently in a state where it MAY be cured: it has the
    // cure option AND has reached its Phase3 ("Monster") window AND is still alive.
    bool  canCure() const { return m_hasCureOption && m_alive && m_phase == BossPhase::Phase3; }
    // CURE / spare path (vs. killing). Incapacitates the boss WITHOUT a kill: it is
    // removed from the fight (body gone, model hidden via the death-pop) but flagged
    // as CURED, not killed. No-op + returns false unless canCure(). The floor wires
    // the narrative outcome (Chen survives -> 100% cure ally). Returns true if cured.
    bool  cure(Scene& scene, x3::phys::IPhysicsWorld& physics);
    bool  wasCured() const { return m_cured; }
    // SPARE / free a live monster non-lethally (multi-pod "save" path). Like cure()
    // but with NO phase precondition: removes it from the fight (body gone, model
    // hidden via the death-pop) and flags it spared (m_cured) so it is NOT counted as
    // killed. No-op (returns false) if already down. Used by MultiPodBoss::sparePod.
    bool  spare(Scene& scene, x3::phys::IPhysicsWorld& physics);

    // MEMORY FLASH (Failed Experiment #7, F3). True while the boss is in its brief
    // post-phase-transition vulnerability window: it cannot attack and takes
    // amplified damage. Read by the HUD (clarity/stagger tell) + the self-test.
    bool  inMemoryFlash() const { return m_flashTimer > 0.0f; }
    float memoryFlashRemaining() const { return m_flashTimer > 0.0f ? m_flashTimer : 0.0f; }
    // Damage multiplier an incoming hit should use right now (>1 during a memory
    // flash, else 1). fire()/takeMeleeDamage() apply this automatically; exposed so
    // the host/self-test can observe the amplified-vulnerability beat.
    float incomingDamageMul() const { return inMemoryFlash() ? m_memoryFlashDamageMul : 1.0f; }

    // Adaptive-Hide queries (HUD colour-tint, self-test introspection). m_adaptiveHideType
    // is the LAST damage type seen; m_adaptiveHideTimer is the seconds left in the resist
    // window (0 == window expired / never opened / row not opted-in).
    x3::DamageType adaptiveHideType()   const { return m_adaptiveHideType; }
    float          adaptiveHideTimer()  const { return m_adaptiveHideTimer; }
    float          adaptiveHideResist() const { return m_adaptiveHideResist; }

    // ---- Act-2 boss tags (Wave 2) -----------------------------------------
    // Memory Hunter (Act-2 L12): which phase runs the copy/feint gimmick (0 = none).
    uint32_t copyFeintPhase() const { return m_copyFeintPhase; }
    // True iff THIS boss is currently in its copy/feint phase (data tag only — the
    // floor module owns the actual illusion spawns + HUD).
    bool  inCopyFeintPhase() const {
        return m_copyFeintPhase != 0 && (uint32_t)m_phase + 1u == m_copyFeintPhase;
    }
    // Garrison Commander (Act-2 L20): the configured orbital-strike timer (sec).
    float escapeTimerSeconds() const { return m_escapeTimer; }
    // True iff this boss carries an escape timer at all (data tag).
    bool  hasEscapeTimer() const { return m_escapeTimer > 0.0f; }

    // ---- W5-2: scripted CALM LOOP (the assault tableau) ---------------------
    // Resolve a clip by fuzzy name (e.g. "struggle", baked by tools/struggle_bake.py)
    // and play it as the CALM-state animation instead of idle, while the monster is
    // unaggroed and stationary. Aggro/attack/death preempt it exactly like idle —
    // the loop is purely the "what you burst in on" pose. No-op when the clip is
    // absent (unbaked rigs keep plain idle; the tableau still stands).
    void setCalmLoop(const char* fuzzyName) {
        if (m_animActive) m_calmLoopClip = m_skinner.findClip({ fuzzyName });
    }
    // Gallery/dev: pin the calm loop to an EXACT clip index (setCalmLoop is a
    // fuzzy substring find, which can't distinguish "Idle" from "IdleAlt").
    // Restarts the loop at t=0 so a cycle always shows the clip from its top.
    void setCalmLoopClip(int clip) {
        if (m_animActive && clip >= 0 && (uint32_t)clip < m_skinner.clipCount()) {
            m_calmLoopClip = clip; m_calmLoopT = 0.0f;
        }
    }
    // The pinned calm-loop clip index, or -1 (gallery clip-cycle HUD readback).
    int  calmLoopClip() const { return m_calmLoopClip; }
    bool calmLoopActive() const { return m_calmLoopClip >= 0; }

    // ---- CLIP-SLOT OVERRIDE (Clone boss). The locomotion/combat clip slots are
    // resolved at build time from a fixed set of fuzzy keys ("attack"/"strike"/
    // "swing"/..., "death"/"die", ...). A rig whose clips are named outside that
    // vocabulary — e.g. the JakeClone_player rig's "Backflip_Sweep_Kick",
    // "Gunshot_Reaction", "Dead" — leaves those slots EMPTY and the character has no
    // attack/death animation. This lets the spawning content module name the clip
    // explicitly, by the SAME fuzzy-resolve rule setCalmLoop uses.
    // ADDITIVE + INERT: nothing calls it unless a content module opts in, and an
    // unresolved name leaves the existing slot untouched — so every existing enemy,
    // roster row and self-test is bit-identical. Call AFTER buildMonsterTuned().
    enum class ClipSlot : uint32_t {
        Idle = 0, Walk = 1, Run = 2, Attack = 3, Attack2 = 4, HitReact = 5, Death = 6
    };
    void overrideClip(ClipSlot slot, const char* fuzzyName);
    // Read a slot's resolved clip index (-1 = unresolved). Diagnostics/self-test.
    int clipIndex(ClipSlot slot) const;

    // ---- W9-1: desc-mechanics hooks (docs/DESC_MECHANICS_TODO.md Tier A) ----
    // STUN (EMP): freeze the AI in place for `secs` — no movement, no attack,
    // any wind-up cancelled; death/corpse flow and fire() damage are untouched
    // (a stunned enemy is still damageable/killable). Extends, never shortens.
    void  stun(float secs) { if (m_alive && secs > m_stunTimer) m_stunTimer = secs; }
    bool  stunned() const { return m_stunTimer > 0.0f; }
    // DOCILE (master hack): permanently powered down — never targets, moves or
    // attacks again; killing it afterwards still counts (fire() path untouched).
    void  setDocile(bool d) { m_docile = d; }
    bool  docile() const { return m_docile; }
    // DORMANT (opening-flow spawn gating): the monster idles/patrols its beat but
    // neither perceives nor engages the player — no LOS, no chase, no attack, no
    // alert feed — until the host wakes it (region/progression gating, CanonPlay).
    // Implemented by substituting a null target in update(), i.e. the exact
    // "no target" AI path, so animation/calm loops/presence stay fully live.
    // Unlike stun it is not a combat state (no timer); unlike docile it is
    // reversible and is the NORMAL pre-activation state of far-away spawns.
    void  setDormant(bool d) { m_dormant = d; }
    bool  dormant() const { return m_dormant; }
    // Damage-taken multiplier (coolant sabotage: The Collective x1.5). Applied
    // at damage application in fire()/takeMeleeDamage(), stacking with the
    // memory-flash incomingDamageMul (both are >1 vulnerability windows).
    void  setDamageTakenMul(float m) { m_damageTakenMul = (m > 0.0f) ? m : 1.0f; }
    float damageTakenMul() const { return m_damageTakenMul; }
    // Bestiary species this instance was spawned as (stamped from Tuning).
    EnemyType species() const { return m_species; }

    // ---- Combat-AI state (D-ai) -------------------------------------------
    // Current behaviour state + the heading (yaw, radians) the body is turning to.
    // Read by the HUD / self-test to observe that facing follows state.
    AiState aiState() const { return m_ai; }
    float   heading() const { return m_yaw; }
    // World-space planar forward the model is currently facing (local -Z under the
    // heading), i.e. (-sin yaw, 0, -cos yaw). Used by the facing self-test.
    x3::phys::Vec3 facingDir() const;
    // Player position last seen with LOS (the Search target). Valid once LOS held.
    x3::phys::Vec3 lastKnownPlayerPos() const { return m_lastKnown; }
    bool    hasLineOfSight() const { return m_hasLos; }
    // [P2-6] True while an attack wind-up is in flight (the telegraph window
    // between attack start and the hit landing). Read by the fresh-LOS self-test.
    bool    winding() const { return m_winding; }

    // PACK ALERT: a squadmate that CAN see the player calls this on nearby allies so
    // the pack reacts together — an enemy with no LOS of its own still turns and
    // moves to investigate (a Search toward the shared last-known spot) instead of
    // idling until it personally rounds the corner. Data-light: seeds the same
    // last-known / search fields a real sighting would. Does NOT wake a DORMANT
    // (opening-flow-gated) spawn — those stay gated until their own trigger/damage.
    void notifyPackAlert(const x3::phys::Vec3& playerPos) {
        if (!m_alive || m_dormant || m_dmg <= 0) return;   // combat, ungated only
        if (m_hasLos) return;                              // already sees for itself
        m_everSawPlayer = true;
        m_lastKnown     = playerPos;
        if (m_searchTimer <= 0.0f) m_searchTimer = kAiSearchTime;
    }

    // Draw all monster primitives at its current transform, tinted toward red by
    // the active hit-flash. No-op once dead / hidden. The monster Entity carries an
    // invalid render mesh so Scene::render skips it; this is the single source of
    // truth for the multi-primitive model (mirrors WeaponSystem::drawPickup). Call
    // alongside scene.render() each frame.
    void drawMonster(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                     const Scene& scene) const;

    // Gameplay state.
    int  hp() const { return m_hp; }
    int  maxHp() const { return m_maxHp; }
    bool alive() const { return m_alive; }

    // Hit-react flash strength in [0,1] (mirrors Player::damageFlash): 1 the instant
    // the monster takes damage, decaying to 0 over kHitFlashTime. The draw path tints
    // by this; the HUD health-bar layer flares white on a fresh hit by reading it.
    float hitFlash() const;

    // ---- Game-feel cues (footstep / impact). The host wires a single sink that
    // maps cues onto audio/FX; unset => a throttled-log stub (see app/cues.h). The
    // MonsterSystem emits Footstep cues at locomotion phase crossings (foot plants)
    // while moving, and BulletImpact/MeleeImpact when its attack lands. ----
    void setCueSink(const GameCueFn& sink) { m_cueSink = sink; }

    // ---- Death FX sink (gib burst) ----------------------------------------
    // Wire a callback the monster fires ONCE the instant it is KILLED (HP->0 via a
    // shot or melee), passing its body-center world position + a flying flag. The
    // host turns this into the gib explosion (GPU debris + blood). Empty => no extra
    // FX. Does NOT fire on a cure()/spare() (those are non-kills). See app/monster.h.
    void setDeathFxSink(const DeathFxFn& sink) { m_deathFx = sink; }

    // ---- Scripted-hook support (Wave 1) -----------------------------------
    // Set HP directly (clamped to [0, maxHp]) WITHOUT running the death path — used
    // by ScriptedFightHook::stripBossHp to debuff a boss pre-fight (the master hack).
    // Does NOT kill: if you set 0 the monster stays "alive" until a real damage call
    // resolves death. Callers strip to >=1 so the boss still fights.
    void setHp(int hp) { m_hp = hp < 0 ? 0 : (hp > m_maxHp ? m_maxHp : hp); }
    // Flip from hostile to ALLIED: zero this monster's attack damage so it no longer
    // harms the player (the gameplay half of the drone-army conversion), and flag it
    // allied. Idempotent. The host owns any re-tinting / re-targeting.
    void convertToAllied() { m_dmg = 0; m_allied = true; }
    bool isAllied() const { return m_allied; }
    // True during the brief death "pop" after a kill: not alive, body already
    // removed, but the model is still being drawn (shrinking/flashing).
    bool dying() const { return m_dying; }

    // True once the corpse has DESPAWNED (BUG#30): the topple/gib finished, the
    // despawn delay elapsed, the Entity is hidden, and the GPU skinned-mesh
    // registration was freed. A despawned monster is fully removed (not drawn,
    // no body, skin freed). Distinct from alive()/dying(): a corpse is !alive() &&
    // !dying() && !despawned() until its timer expires.
    bool despawned() const { return m_despawned; }

    // ---- SKINNED DEATH RAGDOLL accessors (TASK#12, for the self-test) ------
    // True while a physics ragdoll is driving the (rigged) corpse's skin instead of
    // the legacy rigid topple. False for unrigged monsters (they topple) and after
    // the ragdoll is torn down on corpse-expire.
    bool ragdollActive() const { return m_ragdollActive; }
    // The death ragdoll (null unless a skinned ragdoll was spawned). Lets the test
    // read bone world transforms / inWorld() to assert the bones fall + tear down.
    const x3::phys::IRagdoll* deathRagdoll() const { return m_deathRagdoll.get(); }
    // The skinning runtime (for the death-ragdoll self-test to read lastPalette()).
    const x3::anim::Skinner&  skinner() const { return m_skinner; }
    // True if this monster bound a usable skeleton (drives the skinned-ragdoll path).
    bool skinnable() const { return m_skinner.valid() && m_skinner.nodeCount() > 0; }
    // Remove any live death-ragdoll bodies NOW (idempotent). The host should call this
    // (via MonsterManager::shutdown) BEFORE tearing down the physics world if a monster
    // could still be mid-flop, so the Jolt bodies are removed while the world is alive
    // (mirrors RagdollDemo::shutdown). Harmless if no ragdoll is active.
    void shutdownRagdoll() { clearDeathRagdoll(); }

    // ---- EXTERNAL death-flop trigger (living-city citizens) ----------------
    // Force the SKINNED DEATH RAGDOLL flop NOW, driven by an external kill (a shot
    // citizen in the crowd-skin layer, not the combat AI). Runs the SAME death
    // teardown the fire()/melee kill path does — marks the character dead, drops its
    // physics body + the entity's body handle, and spawns the death ragdoll kicked by
    // `shove` (the shot direction). From then on update() reads the bones back and
    // flops the skin (the corpse settles + despawns on the usual timers). Idempotent
    // (no-op if already dead/ragdolled). Returns true iff a skinned ragdoll spawned
    // (false on an unrigged/non-skinnable model — the caller keeps the standing prop).
    bool triggerRagdoll(Scene& scene, x3::phys::IPhysicsWorld& physics,
                        const x3::phys::Vec3& shove);

    // ---- GENERAL navigation (optional) ------------------------------------
    // Give this monster a shared nav grid so it ROUTES AROUND walls/obstacles
    // instead of beelining into them. When set, the Advance/Search states steer
    // toward the next A* path WAYPOINT (rebuilt on a cadence, not every frame)
    // rather than straight at the target. nullptr (default) keeps the original
    // straight-line behaviour exactly (so --test-ai / --test-combat are unchanged).
    // The grid is borrowed (not owned); the host owns its lifetime.
    void setNavGrid(const x3::ai::INavGrid* grid) { m_navGrid = grid; }
    bool usingNav() const { return m_navGrid != nullptr; }
    // The number of waypoints in the current path (0 = none / direct). Diagnostics.
    uint32_t pathWaypointCount() const { return m_follower.waypointCount(); }

    // ---- STAIR ROUTE (feat/stair-nav): inter-floor commute over an authored
    // waypoint chain (the FacilityStairwell's nav chain — see stairwell.h). While
    // a route is active it OVERRIDES the state movement each frame: the agent
    // steers planar toward the next waypoint at chase speed and its Y is LERPED
    // along the current segment, so feet ride the stair nosing line (+- half a
    // riser) instead of staying floor-locked. The wall probe is skipped while on
    // the route (the chain is authored down the clear lane centers). On reaching
    // the final waypoint the route clears, the arrival floor latches for the host
    // (takeStairArrivalFloor) and normal per-floor AI resumes. ----
    void setStairRoute(const std::vector<x3::phys::Vec3>& wps, int targetFloor) {
        m_stairWps = wps; m_stairIdx = 0;
        m_stairActive = m_stairWps.size() >= 2;
        m_stairTargetFloor = m_stairActive ? targetFloor : -1;
    }
    void clearStairRoute() {
        m_stairWps.clear(); m_stairIdx = 0; m_stairActive = false;
        m_stairTargetFloor = -1;
    }
    bool stairRouteActive() const { return m_stairActive; }
    int  stairTargetFloor() const { return m_stairTargetFloor; }
    // The floor a finished route delivered this agent to (-1 = none). Latched at
    // route completion, cleared by this read — the host re-tags the entity's room.
    int takeStairArrivalFloor() {
        const int f = m_stairArrivedFloor; m_stairArrivedFloor = -1; return f;
    }
    // The stair segment currently walked (a = departed waypoint, b = steered-to).
    // False before the first waypoint / with no route. Diagnostics + the
    // --test-stairnav follow assertion (agent Y vs the chain's segment lerp).
    bool stairSegment(x3::phys::Vec3& a, x3::phys::Vec3& b) const {
        if (!m_stairActive || m_stairIdx == 0 || m_stairIdx >= m_stairWps.size())
            return false;
        a = m_stairWps[m_stairIdx - 1]; b = m_stairWps[m_stairIdx];
        return true;
    }
    // Hovering flyers are excluded from stair routing by the host (they hold their
    // spawn-floor hover today — a flight lane is a follow-up).
    bool flyer() const { return m_flyer; }

    // The monster's entity id (kNoLink until built) and physics body.
    uint32_t entity() const { return m_entity; }
    x3::phys::BodyId body() const { return m_body; }

    // Current body-center world position (D-ai: read by the ally query / regroup).
    x3::phys::Vec3 pos() const { return m_pos; }

    // Enemy hitbox half-width (scaled) — the mutual-exclusion collision radius used
    // by the character-separation pass (no two characters may be closer than the sum
    // of their radii).
    float hitRadius() const { return m_hitHalfXZ; }

    // MUTUAL EXCLUSION nudge: set the planar (x,z) position (Y unchanged) and sync the
    // physics body (respecting the box center offset). Used by the host's
    // character-separation pass to push a monster OUT of a captive / another monster
    // it has walked into. No-op once dead or bodyless. Defined in monster.cpp.
    void setPlanarPos(float x, float z, x3::phys::IPhysicsWorld& physics);
    // ---- Anti-overlap (hard de-overlap pass) ------------------------------
    // Planar collision radius (the Enemy hitbox half-width). Two agents whose
    // centers are closer than the sum of their radii are considered overlapping.
    float bodyRadiusXZ() const { return m_hitHalfXZ; }
    // Apply a planar positional correction and sync the physics body. This is the
    // HARD "two characters never share a cell" floor that sits on top of the
    // separation STEERING (which only makes them not WANT to crowd). Small,
    // per-frame-clamped shoves — no-op on a dead / bodyless agent. Y is untouched.
    void nudgePlanar(float dx, float dz, x3::phys::IPhysicsWorld& physics) {
        if (!m_alive || !m_body.valid()) return;
        m_pos.x += dx; m_pos.z += dz;
        physics.setBodyPosition(m_body,
            x3::phys::Vec3{ m_pos.x, m_pos.y + m_hitCenterOff, m_pos.z });
    }

    // Club max-out: externally drive an INERT prop's pose (position + heading).
    // Intended ONLY for chaseSpeed-0 / damage-0 character props (the Club 1127
    // dancers) — the caller owns the choreography and calls this each frame
    // BEFORE update(); the AI never moves an inert prop, so nothing fights it.
    void setPropPose(const x3::phys::Vec3& p, float yaw) {
        m_pos = p; m_yaw = yaw; m_yawTarget = yaw;
    }

    // SKINNED CITIZENS (crowd skin layer): externally drive an inert prop's
    // LOCOMOTION + GESTURE, alongside setPropPose. The caller owns the brain
    // (CrowdSystem) and calls this each frame BEFORE update():
    //   * speed — the prop's own planar speed (m/s). update() measures speed
    //     from position deltas, but a prop posed via setPropPose has already
    //     moved by the time update() runs (delta == 0), so the caller feeds the
    //     real speed and the locomotion blend picks Idle/Walk/Run from it
    //     (>= the 0.2 m/s walk threshold => Walk). < 0 (the default) restores
    //     the measured-delta behaviour exactly (every existing monster).
    //   * lean — a small torso pitch (radians) toward the facing, layered onto
    //     the render transform (the crowd's converse nod / carry lean / console
    //     lean-in reads). 0 (default) leaves the transform bake byte-identical.
    void setPropMotion(float speed, float lean) {
        m_propSpeed = speed; m_propLean = lean;
    }
    // Drop an active calm loop (back to plain idle/locomotion). Used by the
    // crowd skin layer to toggle the Talk clip on conversation start/end.
    void clearCalmLoop() { m_calmLoopClip = -1; m_calmLoopT = 0.0f; }

    // True if the real GLB loaded; false if the procedural fallback box is in use.
    // Valid after buildMonster().
    bool usingRealModel() const { return m_usingReal; }

private:
    // Issue the per-primitive drawMesh calls for the monster at `model`, with
    // `tint` multiplied into each primitive's base color (for the hit-flash).
    void drawMonsterAt(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                       const float model[16], const float tint[4]) const;

    // [P2-6] One line-of-sight probe: ray from our body center (+0.3 m, stepped
    // past our own collision box) toward `playerEye` against Layer::Static; clear
    // iff no wall blocks it before the player. Extracted from the decision-cadence
    // block so the ATTACK path can re-run it FRESH (every frame while in attack
    // range / winding) instead of trusting the ~0.3 s-stale decision snapshot.
    // See docs/design/SUBSYSTEM_HARDENING_PLAN.md AI-2.
    bool probeLos(x3::phys::IPhysicsWorld& physics,
                  const x3::phys::Vec3& playerEye) const;

    // ---- SKINNED DEATH RAGDOLL helpers (TASK#12) --------------------------
    // Try to spawn the physics ragdoll at the kill moment (rigged models only). On
    // success m_deathRagdoll is built+added to `physics` with a death impulse and
    // m_ragdollActive becomes true. On a non-skinnable model it is a no-op (the
    // legacy rigid topple draws instead). `shove` is the existing topple's directional
    // hit (world dir; may be zero).
    void spawnDeathRagdoll(x3::phys::IPhysicsWorld& physics, const x3::phys::Vec3& shove);
    // Per-frame: read the ragdoll bone world transforms, map them onto the skin nodes
    // and feed the Skinner's external-pose path so the model flops with the bones.
    void driveSkinFromRagdoll();
    // Remove the ragdoll bodies (corpse cleanup). Idempotent. Leaves the corpse drawable.
    void clearDeathRagdoll();

    // BUG#30: DESPAWN the corpse — hide its Entity, free the GPU skinned-mesh
    // registration (Skinner::disableGpuSkinning -> unregisterSkinnedMesh), tear down
    // any lingering ragdoll bodies, and latch m_despawned. The physics body is already
    // gone (removed at the kill) and the alive count already dropped, so this does NOT
    // change the count. Idempotent; safe even if never killed (no-op while alive).
    void despawn(Scene& scene);

    // Loaded model + its draw records (one per primitive). Loader kept alive so the
    // GPU handles in m_drawables stay valid for the app's lifetime.
    std::unique_ptr<x3::asset::IAssetSource>  m_assets;
    std::unique_ptr<x3::asset::IModelLoader>  m_loader;
    x3::asset::Model                          m_model;
    std::vector<x3::asset::ModelDrawable>     m_drawables;
    bool                                      m_usingReal = false;

    // ---- Skeletal animation (J1). If the loaded model is skinnable, m_skinner is
    // valid and drives CPU skinning each update() via the device. m_idleClip /
    // m_walkClip index the model's clips (-1 if none found); m_animTime accumulates
    // the active clip's playback time (looped in the runtime). The device pointer is
    // captured at build time so update() (which has no device param) can re-upload
    // the skinned vertices. Unskinned models leave m_skinner invalid -> static draw. ----
    x3::anim::Skinner        m_skinner;
    x3::rhi::IRenderDevice*  m_device = nullptr;

    // Kick the flinch/hit-react on a surviving hit: plays the BAKED Hit clip one-shot
    // if the rig has one; ALWAYS raises the procedural stagger floor so a shot ALWAYS
    // produces a visible recoil (never a silent no-react), regardless of asset state.
    // Suppressed while a death or attack one-shot owns the pose (don't stomp them).
    inline void kickHitReact() {
        if (m_dying) return;
        m_procStagger = 1.0f;                       // guaranteed visible recoil floor
        if (m_animActive && m_hitClip >= 0 && m_attackAnimT < 0.0f)
            m_hitAnimT = 0.0f;                       // preferred: real baked clip
    }
    // Game-feel cue sink (footstep / impact). Empty => throttled-log stub.
    GameCueFn                m_cueSink;
    // Death FX sink (gib burst). Empty => no extra FX. Fired once at the kill moment.
    DeathFxFn                m_deathFx;
    int                      m_idleClip = -1;
    int                      m_walkClip = -1;
    int                      m_calmLoopClip = -1;   // W5-2 scripted calm loop (see setCalmLoop)
    float                    m_calmLoopT    = 0.0f; // its looping playback clock
    // T1: separate Run / Jump clips for the locomotion blend (multi-clip *_anim.glb).
    // When a distinct Walk AND Run exist the 1D blend is driven by planar speed; on
    // single-locomotion-clip models (only Idle, or Idle+one move clip) it degrades
    // to the legacy idle/move switch via the Skinner's graceful blend collapse.
    int                      m_runClip  = -1;
    int                      m_jumpClip = -1;
    // W2-D: one-shot combat clips (fuzzy-found at bind; -1 = rig has none — the
    // procedural lunge tell below carries the attack read instead).
    int                      m_attackClip = -1;
    int                      m_attackClip2 = -1;     // 2nd attack variant (alternated for variety)
    int                      m_deathClip  = -1;
    // ANIM-ENRICH: one-shot HIT-REACTION flinch — fuzzy-found ("hit/react/flinch/
    // stagger"); played once when the enemy SURVIVES a shot/melee (not while
    // attacking or dying) so it visibly reacts. -1 on rigs that ship no flinch.
    int                      m_hitReactClip = -1;
    float                    m_hitReactAnimT = -1.0f; // >=0 while the flinch plays
    int                      m_attackActiveClip = -1; // the variant chosen for the CURRENT swing
    bool                     m_attackAlt = false;     // toggles Attack <-> Attack2
    // opening-enemies-rigged: a second baked flinch handle used by the drone/procedural
    // fallback path (union-kept alongside m_hitReactClip; both are referenced in .cpp).
    int                      m_hitClip    = -1;      // one-shot flinch/hit-react (baked)
    float                    m_attackAnimT = -1.0f;  // >=0 while the attack one-shot plays
    float                    m_deathAnimT  = -1.0f;  // >=0 while the death clip plays
    float                    m_hitAnimT    = -1.0f;  // >=0 while the hit-react one-shot plays
    bool                     m_deathClipDone = false; // clip finished -> freeze final pose
    // Procedural STAGGER TELL (visual-only floor): a short recoil offset applied to the
    // DRAW transform when the monster is shot, guaranteeing a visible flinch even on a
    // rig with no baked Hit clip. Decays back to 0. Never touches the physics body.
    float                    m_procStagger  = 0.0f;  // 0..1 recoil intensity (decays)
    // Procedural attack TELL (visual-only): a rear-back + lunge offset applied to the
    // DRAW transform during the melee wind-up so attacks read at gameplay distance
    // even on rigs with no authored Attack clip. Never touches the physics body.
    float                    m_lunge    = 0.0f;      // metres along facing (- = rear back)
    float                    m_lungeDip = 0.0f;      // metres of crouch dip (-Y)
    bool                     m_useLocoBlend = false;  // a real idle(+walk/+run) set drives the blend
    float                    m_animTime = 0.0f;
    bool                     m_animActive = false;   // a usable clip was found
    // GUARANTEED PROCEDURAL MOTION FLOOR (Tim's "anim keeps getting lost" fix): when
    // NO skeletal clip drives an enemy (the rig-less Drone, a legacy static mesh, or
    // the box fallback for a missing/stub GLB) this clock advances so update() can
    // add a hover/idle bob + yaw sway + tilt + attack dive purely in the DRAW
    // transform — so a rig-less enemy is NEVER a frozen prop. Unused when m_animActive.
    float                    m_procTime = 0.0f;
    // Footstep cue tracking: the last sampled locomotion phase, so update() can
    // detect a phase crossing (foot plant) between frames and emit a Footstep cue.
    float                    m_lastFootPhase = 0.0f;
    // Model-local fixup applied between the gameplay transform and each drawable's
    // node transform (final = model * fixup * nodeTransform). Identity for Y-up
    // models; a -90deg-X stand-up for the Z-up converted character GLBs. Also
    // grounds the feet to y=0 after the rotation.
    float                                     m_modelFixup[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

    x3::phys::Vec3   m_pos{};                 // current body-center world position
    uint32_t         m_entity = kNoLink;      // index into the Scene
    x3::phys::BodyId m_body;                  // Enemy-layer collision box
    float            m_modelScale = 1.0f;     // uniform scale applied to the model
    float            m_hitHalfY   = 0.95f;    // Enemy hitbox half-height (scaled); top half = head zone
    float            m_hitHalfXZ  = 0.5f;     // Enemy hitbox half-width (scaled); chase probe clears this
    float            m_hitCenterOff = 0.0f;   // box center offset above m_pos (feet-origin ground enemies raise it)
    bool             m_flyer       = false;   // hovering, center-origin enemy (vs ground feet/center-origin)

    int   m_hp        = kMonsterHp;
    int   m_maxHp     = kMonsterHp;           // per-instance starting HP (Tuning)
    float m_chaseSpeed = kChaseSpeed;         // per-instance chase speed (Tuning)
    float m_baseTint[4] = { 1, 1, 1, 1 };     // per-instance color multiplier (Tuning)
    float m_emissiveScale = 1.0f;             // authored-emissive scale (Tuning, PBR path)
    bool  m_alive     = true;
    float m_flash     = 0.0f;                 // remaining hit-flash time (s)

    // ---- Attack behaviour (Phase 2a) --------------------------------------
    MonsterType m_type          = MonsterType::Guard;
    int   m_dmg                 = 0;          // damage per attack (0 = inert)
    float m_attackRange         = 1.8f;       // melee reach / drone fire range (m)
    float m_attackCooldown      = 1.0f;       // seconds between attacks
    float m_attackWindup        = 0.25f;      // telegraph delay before the hit lands
    bool  m_ranged              = false;      // hitscan toward the player (drone)
    float m_standoff            = 6.0f;       // drone preferred distance (m)
    float m_aiStrafeBias        = -1.0f;      // per-instance strafe/flank bias (<0 = type default)
    float m_atkTimer            = 0.0f;       // cooldown countdown until next attack
    float m_windupTimer         = 0.0f;       // >0 while winding up an attack
    bool  m_winding             = false;      // currently in an attack wind-up

    // ---- Dogpile limiting (playtest-fix) ----------------------------------
    // Per-frame melee attack permit (set by MonsterManager each frame). When false,
    // a melee enemy does NOT swing this frame and holds at the standoff ring. Always
    // effectively true for ranged enemies. Default true = unmanaged single-monster.
    bool  m_meleePermit         = true;
    // Planar distance to the player from the last update() (drives inMeleeRange()).
    float m_lastHoriz           = 1e30f;

    // ---- Boss phases (Phase 2b) -------------------------------------------
    BossPhase m_phase           = BossPhase::Phase1; // current phase (HP-keyed)
    float m_phase2Frac          = 0.66f;      // hp/maxHp threshold to enter Phase2
    float m_phase3Frac          = 0.33f;      // hp/maxHp threshold to enter Phase3
    float m_phase2SpeedMul      = 1.35f;
    float m_phase2DamageMul     = 1.4f;
    float m_phase3SpeedMul      = 1.7f;
    float m_phase3DamageMul     = 1.8f;
    float m_phase2ScaleMul      = 1.15f;
    float m_phase3ScaleMul      = 1.30f;
    int   m_phase3SummonCount   = 2;
    // Active per-phase multipliers (Phase1 = 1x). Folded into movement/attacks +
    // the render scale/tint so the phase shift is felt + seen.
    float m_phaseSpeedMul       = 1.0f;
    float m_phaseDamageMul      = 1.0f;
    float m_phaseScaleMul       = 1.0f;
    float m_phaseTintMul[3]     = { 1.0f, 1.0f, 1.0f }; // extra red push per phase

    // ---- Act-1 boss gimmicks (Wave 1) -------------------------------------
    bool  m_allied              = false;  // flipped hostile->allied (drone conversion)
    bool  m_hasCureOption       = false;  // Dr. Chen: KILL-vs-CURE outcome available
    bool  m_cured               = false;  // spared via cure() (not killed)
    float m_memoryFlashTime     = 0.0f;   // FE#7: flash duration per phase transition (s)
    float m_memoryFlashDamageMul = 1.0f;  // FE#7: incoming-damage mul while flashing
    float m_flashTimer          = 0.0f;   // >0 while in a memory-flash window (s)

    // ---- Adaptive Hide (canon-aliens SaurianWarlord; opt-in via Tuning). ------
    float          m_adaptiveHideResist      = 0.0f;                  // copied from Tuning
    float          m_adaptiveHideDurationSec = 8.0f;                  // copied from Tuning
    x3::DamageType m_adaptiveHideType        = x3::DamageType::None;  // last incoming type
    float          m_adaptiveHideTimer       = 0.0f;                  // s remaining in current resist window
    // Act-2 boss tags carried as DATA (read by the floor module / HUD / self-test).
    uint32_t m_copyFeintPhase   = 0;      // Memory Hunter: which boss phase runs copy/feint
    float    m_escapeTimer      = 0.0f;   // Garrison Commander: P3 orbital-strike countdown (s)

    // Death "pop": when killed, m_alive flips false and the body is removed, but
    // m_dying stays true and m_deathPop counts down from kDeathPopTime while the
    // model keeps drawing (shrinking + flashing). Once it reaches 0 the Entity is
    // hidden and m_dying clears.
    bool  m_dying     = false;
    float m_deathPop  = 0.0f;                 // remaining topple time (s) while m_dying
    bool  m_corpse    = false;                // topple finished -> lingering body on the floor

    // ---- Corpse despawn (BUG#30) -----------------------------------------
    // Once the corpse settles, m_corpseTimer counts down kCorpseDespawnTime; when it
    // hits 0 the monster DESPAWNS: the Entity is hidden, the GPU skinned-mesh
    // registration is freed (unregisterSkinnedMesh), and m_despawned latches so the
    // draw/cleanup happen exactly once. A gib-class kill skips the corpse and despawns
    // immediately (the body burst). m_despawned implies fully removed (not drawn).
    float m_corpseTimer = 0.0f;               // countdown to despawn while m_corpse
    bool  m_despawned   = false;              // corpse removed: hidden + skin freed (once)

    // ---- SKINNED DEATH RAGDOLL (TASK#12) ----------------------------------
    // On the KILL of a RIGGED monster (m_skinner.valid()) we build a physics
    // ragdoll from the canonical humanoid rig, positioned/yawed/scaled to match the
    // monster, and add it to the shared world with a death impulse. Each frame while
    // m_dying we step is driven by the host, read the bone WORLD transforms out, map
    // them onto the model's skin nodes (RagdollSkin, rigid-attach) and feed the
    // result to the Skinner's external-pose path (applyExternalGlobals) so the GPU-
    // skinned MODEL flops physically. The ragdoll is torn down (bodies removed) when
    // the corpse settles / times out — no leaked Jolt bodies. If the model has no
    // usable skeleton, m_deathRagdoll stays null and the legacy rigid TOPPLE draws.
    std::unique_ptr<x3::phys::IRagdoll>      m_deathRagdoll;   // null => no skinned ragdoll
    RagdollSkin                              m_ragdollSkin;    // rigid bone->skin driver
    std::vector<x3::phys::RagdollBoneDesc>   m_ragdollBones;   // the rig the ragdoll was built from
    std::vector<float>                       m_ragPartInit;    // per-bone INIT skin-local 4x4 (boneCount*16)
    std::vector<float>                       m_ragWorldScratch;// per-bone CURRENT world 4x4 scratch
    std::vector<float>                       m_ragPartCur;     // per-bone CURRENT skin-local 4x4 scratch
    std::vector<float>                       m_ragNodeGlobals; // RagdollSkin output (nodeCount*16)
    float                                    m_deathModelInv[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; // inverse of the frozen death model matrix
    bool                                     m_ragdollActive = false; // a skinned ragdoll is driving the skin
    bool                                     m_ragdolled     = false; // this corpse FLOPPED via ragdoll (skip the rigid topple draw)

    // Current yaw (radians) the model faces; baked into the render 3x3 each frame.
    float m_yaw       = 0.0f;

    // ---- SKINNED CITIZENS: externally-fed prop locomotion + lean (see
    // setPropMotion). speed < 0 == "not driven" (measure deltas, the default);
    // lean 0 == no pitch (the default) — every existing monster is unchanged.
    float m_propSpeed = -1.0f;
    float m_propLean  = 0.0f;

    // Wander/strafe state so the chase weaves + orbits instead of beelining.
    float m_wander    = 0.0f;   // weave oscillator phase
    float m_strafeDir = 1.0f;   // orbit direction (+1/-1) when close
    float m_retarget  = 1.8f;   // countdown to flip orbit direction

    // ---- Combat-AI state machine (D-ai) -----------------------------------
    AiState m_ai            = AiState::Idle; // current behaviour state
    float   m_yawTarget     = 0.0f;          // desired heading (m_yaw slews toward it)
    float   m_stateTime     = 0.0f;          // seconds spent in the current state
    float   m_decisionTimer = 0.0f;          // countdown to the next state re-eval
    float   m_dmgMemory     = 0.0f;          // seconds-left of "recently took damage"
    int     m_dmgWindowHp   = 0;             // HP lost while m_dmgMemory active (flinch)
    bool    m_hasLos        = false;         // had LOS to the player this decision
    bool    m_everSawPlayer = false;         // seen the player at least once
    x3::phys::Vec3 m_lastKnown{};            // last position the player was seen at
    float   m_searchTimer   = 0.0f;          // seconds-left searching before Idle
    float   m_searchSweep   = 0.0f;          // search heading-sweep oscillator phase
    float   m_regroupTimer  = 0.0f;          // min-dwell timer while regrouping
    x3::phys::Vec3 m_rallyPoint{};           // ally position to fall back toward
    bool    m_hasRally      = false;         // a valid rally point was found
    // Per-instance pseudo-random seed so decision cadence + jitter desync between
    // enemies (no per-frame heap alloc; a tiny LCG advanced in the hot path).
    uint32_t m_rng          = 0x9E3779B9u;
    bool     m_aiInit       = false;         // seeded the RNG / decision timer yet
    // Enemy-SFX (vocalization): countdown to the next idle/harass TAUNT cue while the
    // enemy is alive + engaged (has LOS). Reseeded to a jittered interval each taunt so
    // a squad doesn't vocalize in lockstep. <=0 fires a taunt (see update()).
    float    m_tauntTimer   = 0.0f;

    // ---- W9-1 desc-mechanics state ------------------------------------------
    float    m_stunTimer      = 0.0f;        // EMP stun: frozen while > 0
    bool     m_docile         = false;       // master-hack power-down (permanent)
    bool     m_dormant        = false;       // opening-flow spawn gating (see setDormant)
    float    m_damageTakenMul = 1.0f;        // coolant-sabotage vulnerability

    // ---- Guard-life (W4-3): species + patrol state -------------------------
    EnemyType m_species       = EnemyType::DominionTrooper;  // stamped from Tuning
    float    m_patrolRadius   = 0.0f;        // 0 = patrol disabled (original Idle)
    float    m_patrolPauseSec = 1.6f;
    float    m_patrolSpeedMul = 0.45f;
    x3::phys::Vec3 m_patrolAnchor{};         // spawn position (loop center)
    int      m_patrolIdx      = 0;           // current waypoint (0..3 diamond)
    float    m_patrolPause    = 0.0f;        // >0: paused at a waypoint, sweeping
    int      m_patrolStall    = 0;           // consecutive blocked frames -> skip wp
    float    m_patrolGrunt    = 6.0f;        // countdown to a quiet on-patrol grunt

    // ---- GENERAL navigation (optional, off by default) --------------------
    // Borrowed shared nav grid (nullptr => straight-line, original behaviour).
    const x3::ai::INavGrid* m_navGrid = nullptr;
    x3::ai::PathFollower    m_follower;       // walks the current A* path's waypoints
    float    m_repathTimer  = 0.0f;          // countdown to rebuild the path (cadence)
    x3::phys::Vec3 m_pathGoal{};             // goal the current path was built toward
    bool     m_hasPath      = false;         // a valid path is being followed

    // ---- STAIR ROUTE state (feat/stair-nav, see setStairRoute) --------------
    std::vector<x3::phys::Vec3> m_stairWps;  // authored waypoints (with Y)
    uint32_t m_stairIdx          = 0;        // waypoint currently steered toward
    bool     m_stairActive       = false;
    int      m_stairTargetFloor  = -1;       // floor the route ends on
    int      m_stairArrivedFloor = -1;       // completion latch (host consumes)
    // Advance along the stair route (planar steer + segment-lerped Y + body sync).
    // Returns true iff the route drove this frame's movement (state movement skips).
    bool updateStairRoute(float dt, x3::phys::IPhysicsWorld& physics, float speed);
};

// ---------------------------------------------------------------------------
// Multi-monster manager (Level 1 / §6.1). Holds N MonsterSystem instances and
// fans the per-frame calls (update / draw) and a single fire() across them. Each
// MonsterSystem stays a self-contained unit (its own model + body + state), so
// the existing single-monster self-test (--test-combat) is unaffected. The
// manager is intentionally thin: a list + iteration, not new combat logic.
// ---------------------------------------------------------------------------
class MonsterManager {
public:
    // Spawn a monster into the world with the given tuning. Returns its index.
    // Each instance owns its own model load (so guards/drone/Martinez can differ).
    uint32_t spawn(Scene& scene, x3::rhi::IRenderDevice& device,
                   x3::phys::IPhysicsWorld& physics,
                   std::string_view modelDir, const x3::phys::Vec3& pos,
                   const MonsterSystem::Tuning& tuning);

    uint32_t count() const { return (uint32_t)m_monsters.size(); }
    MonsterSystem&       at(uint32_t i)       { return *m_monsters[i]; }
    const MonsterSystem& at(uint32_t i) const { return *m_monsters[i]; }

    // Set the game-feel cue sink (footstep/impact) on every monster — current AND
    // future spawns (stored so spawn() applies it to new instances). Empty => the
    // per-monster throttled-log stub. See app/cues.h.
    void setCueSink(const GameCueFn& sink);

    // Set the death FX sink (gib burst) on every monster — current AND future spawns
    // (stored so spawn() applies it to new instances). Empty => no extra death FX.
    // Mirrors setCueSink. See app/monster.h DeathFxFn.
    void setDeathFxSink(const DeathFxFn& sink);

    // Number of monsters still alive (not dead). Used for objective/door gating.
    uint32_t aliveCount() const;

    // Remove any live death-ragdoll bodies across ALL monsters (idempotent). Call
    // BEFORE the physics world is shut down if a monster could be mid-flop on quit,
    // so no Jolt ragdoll bodies are touched after the world is gone. Safe to call any
    // time; a no-op when nothing is ragdolling.
    void shutdown();

    // Advance every monster one frame (hit-flash decay, death-pop, chase, boss
    // phase machine, and — if `target` is non-null — attacks against the player on
    // cooldown). `fx`, if set, spawns a visible attack beam per attack (drone
    // tracer / melee tell). `onPhase`, if set, fires per Boss phase transition.
    void update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                const x3::phys::Vec3& playerPos,
                IDamageSink* target, const AttackFxFn& fx, const BossPhaseFn& onPhase);

    // As above but with no phase callback.
    void update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                const x3::phys::Vec3& playerPos,
                IDamageSink* target, const AttackFxFn& fx);

    // Movement-only overload (no attacks): forwards with a null target/empty fx.
    void update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                const x3::phys::Vec3& playerPos);

    // Hard de-overlap: NO two live members share a cell. Pairwise planar push so
    // every pair of centers ends up >= (rA + rB) apart. A cheap positional
    // correction (split between the pair, clamped per-frame so it eases apart and
    // never explodes) that GUARANTEES no clipping on top of the separation STEERING
    // (which only shapes where they WANT to be). Called at the end of update(); also
    // exposed so a host can run a combined pass across several managers + companions.
    void deOverlapMembers(x3::phys::IPhysicsWorld& physics);

    // Draw every monster (each owns its multi-primitive model).
    void drawAll(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                 const Scene& scene) const;

    // Fire one shot across all monsters: the first LIVE monster the ray hits takes
    // the damage (the Enemy-layer rayCast inside each fire() returns the nearest
    // body, but since fire() ignores hits that aren't THIS monster, we test each
    // and keep the result that actually hit a monster). Returns that FireResult
    // (or a default miss). At most one monster is damaged per call. `damage` is the
    // firing weapon's per-shot damage (defaults to kDamagePerShot).
    FireResult fire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                    Scene& scene, x3::phys::IPhysicsWorld& physics,
                    int damage = kDamagePerShot,
                    x3::DamageType type = x3::DamageType::Kinetic);

private:
    std::vector<std::unique_ptr<MonsterSystem>> m_monsters;
    GameCueFn m_cueSink;   // applied to every spawn (current + future)
    DeathFxFn m_deathFx;   // gib-burst sink applied to every spawn (current + future)
};

// Headless self-test (--test-combat). Builds a physics world + an Enemy-layer
// monster + a Scene, then asserts T1 (ray at monster damages it), T2 (enough
// shots kill it -> hidden, body removed, subsequent rays miss), T3 (ray aimed
// away does no damage), T4 (firing when NOT armed does nothing). Logs PASS/FAIL
// T#, returns true iff all pass. No window/Vulkan. Mirrors the S4/S5 self-tests.
bool runCombatSelfTest();

// Headless self-test (--test-deathragdoll, TASK#12). Builds a physics world + a
// RIGGED monster + a Scene, kills it, and asserts: (D1) the kill spawns a skinned
// death ragdoll (ragdollActive()); (D2) its bones FALL under gravity over the
// death window (top bone Y drops); (D3) the monster's Skinner receives the external
// ragdoll pose (the joint palette diverges from the static animated palette); and
// (D4) the ragdoll is TORN DOWN on corpse-expire (ragdollActive() false, ragdoll
// removed from the world — no leaked bodies). Logs PASS/FAIL D#, returns true iff
// all pass. No window/Vulkan (headless skin path). Lives in monster.cpp.
bool runDeathRagdollSelfTest();

// Headless self-test (--test-phase2b, EFLZ Phase 2b). Asserts:
//   (a) a super-strength melee damages an enemy in FRONT but not one BEHIND / out
//       of range (the forward arc), and applies a knockback impulse;
//   (b) melee on a locked door forces it open (unlockAndOpen);
//   (c) a Boss transitions phase at the HP threshold — enrage stats change (faster
//       chase + harder melee) and the Phase3 summon callback fires exactly once;
//   (d) a Boss still only opens Door E on death (the existing gate is intact).
// Logs PASS/FAIL T#, returns true iff all pass. No window/Vulkan. Lives in
// melee.cpp (where the MeleeSystem + a HeadlessDevice are). Mirrors the others.
bool runPhase2bSelfTest();

// Headless self-test (--test-ai, D-ai monster combat behaviour state machine).
// Asserts:
//   (a) a healthy enemy WITH LOS enters Advance/Attack and FACES the player
//       (heading within epsilon of the atan2-derived facing);
//   (b) a heavily-damaged / low-HP enemy enters Retreat and faces/moves AWAY
//       from the player (not at it);
//   (c) losing LOS -> Search: stops tracking the live player and moves toward the
//       last-known position (then times out toward Idle);
//   (d) the facing math is correct for a known target -> known heading (the
//       off-by-pi/2 trap is caught);
//   (e) states don't jitter: hysteresis holds a state for >= a few ticks.
// Prints the state transitions. Logs PASS/FAIL T#, returns true iff all pass.
// No window / Vulkan. Lives in monster.cpp. Mirrors the other self-tests.
bool runAiSelfTest();

// ---------------------------------------------------------------------------
// MonsterDef — one row of the data-driven bestiary roster (bestiary pass). Pure
// data: a species' display name + a fully-populated MonsterSystem::Tuning (stats /
// model / AI weights). The static table monsterDefs() holds one row per EnemyType;
// monsterDef(t) fetches a row and tuningFor(t) returns a spawn-ready Tuning copy.
// New enemies are added by appending a row to the table in monster.cpp — no new
// code paths. The Tuning still flows through buildMonsterTuned()/spawn() unchanged,
// so the roster reuses the entire existing combat lane.
struct MonsterDef {
    EnemyType              type;     // which species (table is keyed by this)
    const char*           name;      // display name (DominionTrooper, Verthani, ...)
    MonsterSystem::Tuning tuning;    // stats / model / AI weighting for this species
};

// The full roster table (one row per EnemyType, in enum order). Built once and
// returned by reference. Source of truth for the bestiary; mine the bible's TASK_6
// enemy bestiary for the values (see tuningFor() comments in monster.cpp).
const std::vector<MonsterDef>& monsterDefs();

// Fetch a single roster row by species (asserts the table is in enum order).
const MonsterDef& monsterDef(EnemyType t);

// Convenience: a spawn-ready Tuning copy for a species. Equivalent to
// monsterDef(t).tuning. Pass this straight to MonsterManager::spawn() /
// buildMonsterTuned() to place that enemy.
MonsterSystem::Tuning tuningFor(EnemyType t);

// Headless self-test (--test-bestiary, the data-driven enemy roster). Asserts each
// EnemyType in the roster:
//   (a) BUILDS with its table stats (HP / type / ranged / damage all match the row,
//       proving the data-driven path wires through buildMonsterTuned end-to-end);
//   (b) its AI BEHAVES per its weighting under a steady LOS engagement — a melee
//       species (Trooper/Verthani) closes/strafes and FACES the player; a ranged
//       species (Illuminated/BlueSynth) holds a standoff (does NOT close to melee);
//       a strafe-heavy species (Verthani) reaches the Strafe state, a standoff
//       species (Illuminated) reaches Strafe/Advance at range — i.e. weights differ;
//   (c) the roster rows are DISTINCT (no two identical stat blocks) — proving the
//       table actually carries variety.
// Logs PASS/FAIL T#, returns true iff all pass. No window / Vulkan. Lives in
// monster.cpp. Mirrors the other self-tests. Reuses the existing combat AI verbatim.
bool runBestiarySelfTest();

// ===========================================================================
// ACT-1 MID-BOSS ROSTER (Wave 1). The five canon Act-1 mid-bosses, as DATA on top
// of the existing single-body multi-phase Boss machine + two general machine
// EXTENSIONS (multi-pod + scripted pre-fight hook) the Wave-2 floor modules stage.
//
// Floor map (EFLZ_MASTER_PLAN §Act 1):
//   F1  Chief Martinez       — already implemented (the Boss archetype template).
//   F2  Dr. Chen             — single body, 3 phases, KILL-vs-CURE outcome.
//   F3  Failed Experiment #7 — single body, 3 phases, MEMORY-FLASH vuln window.
//   F4.5 The Collective/Chorus — MULTI-POD (5 fused minds), save up to 4.
//   F5  Swarm Controller AI  — SCRIPTED PRE-FIGHT HOOK (Sarah's master hack).
//   F6  Alien Overseer       — single body, 3 phases, ranged psychic commander.
//   F7  Jake's Clone         — design-noted (out of this wave's scope).
//
// All HP/damage are tuned RELATIVE TO MARTINEZ (HP 340, dmg 15) and to the
// combat:: bands, NOT the bible's raw values, so each fight stays winnable under
// the engine's time/iframe budget.
// ===========================================================================

// The Act-1 mid-bosses that ride the SINGLE-BODY phase machine (Boss type). The
// multi-pod Chorus + scripted Swarm are their own machines (below), not rows here.
enum class BossType : uint32_t {
    DrChen = 0,            // F2 — transforming oncologist; KILL-vs-CURE
    FailedExperiment7 = 1, // F3 — Marcus Webb; tragic predecessor; Memory-Flash
    AlienOverseer = 2,     // F6 — psychic alien commander (ranged)
    Count = 3
};

// Human-readable boss name (logs / --test-bosses trace / HUD).
const char* bossTypeName(BossType t);

// One row of the single-body Act-1 boss roster. Pure data: a display name + a
// fully-populated Boss-type MonsterSystem::Tuning (HP / phases / gimmicks). Built
// by buildBossDefs(); a row spawns through buildMonsterTuned() exactly like any
// other Tuning, so these bosses reuse the whole existing combat + phase lane.
struct BossDef {
    BossType              type;    // which boss (table is keyed by this, enum order)
    const char*           name;    // display name (Dr. Chen, Failed Experiment #7, ...)
    MonsterSystem::Tuning tuning;  // Boss-type stats / phases / gimmick config
};

// The single-body boss table (one row per BossType, enum order). Built once.
const std::vector<BossDef>& bossDefs();
// Fetch one row by boss id (asserts enum order; defensive linear fallback).
const BossDef& bossDef(BossType t);
// Convenience: a spawn-ready Boss Tuning copy. Equivalent to bossDef(t).tuning.
MonsterSystem::Tuning bossTuning(BossType t);

// ---------------------------------------------------------------------------
// MULTI-POD BOSS (machine extension #2). A boss that consists of N independently-
// damageable PODS (The Collective / The Chorus = 5 fused minds in 5 pods). Each pod
// is a self-contained MonsterSystem (its own model + body + HP), so pods reuse the
// whole combat lane. The boss "falls" when a THRESHOLD of pods are DOWNED, and a
// pod may be SPARED (freed) instead of killed — the "save up to N" morality count.
//
// GENERAL + DATA-DRIVEN (not Chorus-specific): the Wave-2 Nexus module configures
// pod count / threshold / per-pod tuning via PodConfig and instantiates it. The
// single-body path is untouched — this is an ADD, not a change to MonsterSystem.
// ---------------------------------------------------------------------------
class MultiPodBoss {
public:
    // Config for one pod. A small offset from the boss origin + a full Tuning so
    // every pod can differ (the Chorus voices have different HP/role).
    struct PodConfig {
        const char*           name   = "Pod";  // voice/pod display name
        x3::phys::Vec3        offset{};         // spawn offset from the boss origin
        MonsterSystem::Tuning tuning;           // this pod's stats (its own MonsterSystem)
    };
    // Whole-boss config. Pods + the down-threshold at which the boss falls + the
    // max number of pods that may be SAVED (spared). General: the floor fills it.
    struct Config {
        std::vector<PodConfig> pods;            // the N pods (>=1)
        // The boss FALLS once this many pods are DOWNED (killed OR spared). Default
        // 0 => all pods (the Chorus: down/save all 5). The Wave-2 module may set a
        // lower threshold (e.g. "fall when the core + 2 others are out").
        uint32_t               fallThreshold = 0;
        // Cap on how many pods may be SPARED via sparePod() (the "save up to N"
        // morality budget). The Chorus saves up to 4 (Subject Zero/Maya remains).
        uint32_t               maxSaved      = 0;  // 0 => no cap (any pod sparable)
    };

    // Build all pods into the world at `origin` (+ each pod's offset). Each pod is
    // its own MonsterSystem load. Call once.
    void build(const Config& cfg, Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, std::string_view modelDir,
               const x3::phys::Vec3& origin);

    // TASK#12: tear down any in-flight death ragdolls across ALL pods (idempotent).
    // A killed RIGGED pod spawns a physics ragdoll (Jolt bodies in the shared world);
    // its bodies are only auto-cleared when its corpse-pop times out in update(). If
    // the pods are destroyed (or the physics world is shut down) while a pod is still
    // mid-flop, the IRagdoll dtor would call removeFromWorld() on a DEAD Jolt system
    // (an access violation in teardown). Call this BEFORE the physics world is shut
    // down (mirrors MonsterManager::shutdown). Safe any time; a no-op when no pod is
    // ragdolling.
    void shutdown();

    uint32_t podCount() const { return (uint32_t)m_pods.size(); }
    MonsterSystem&       pod(uint32_t i)       { return *m_pods[i]; }
    const MonsterSystem& pod(uint32_t i) const { return *m_pods[i]; }
    const char*          podName(uint32_t i) const { return m_podNames[i].c_str(); }

    // SPARE (free) pod `i` instead of killing it — counts toward the save total, NOT
    // the kill total. No-op (returns false) if the pod is already down, if `i` is out
    // of range, or if the maxSaved cap is already reached. Removes the pod from the
    // fight (body gone, model hidden) like a non-lethal incapacitation. Returns true
    // if the pod was spared. The Wave-2 module calls this for the morality choice.
    bool sparePod(uint32_t i, Scene& scene, x3::phys::IPhysicsWorld& physics);

    // Per-frame: advance every LIVE pod (chase / attack / phase). Forwards to each
    // pod's MonsterSystem::update. `onPhase` fires per pod phase transition.
    void update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                const x3::phys::Vec3& playerPos, IDamageSink* target,
                const AttackFxFn& fx, const BossPhaseFn& onPhase);

    // Draw every pod's model.
    void drawAll(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                 const Scene& scene) const;

    // Fire one shot across all pods (the first live pod the ray hits takes damage).
    // A pod that DIES this way counts as KILLED (not spared). Returns the result.
    // `damage` is the firing weapon's per-shot damage (defaults to kDamagePerShot).
    // `type` is the canon-aliens DamageType — forwarded into each pod's fire so a
    // future Chorus-tier boss that opts into adaptiveHideResist reacts to the
    // player's loadout (current Chorus row has resist=0 so this is a no-op for them).
    FireResult fire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                    Scene& scene, x3::phys::IPhysicsWorld& physics,
                    int damage = kDamagePerShot,
                    x3::DamageType type = x3::DamageType::Kinetic);

    // ---- Outcome counts (the morality quest) ------------------------------
    uint32_t killedCount() const;            // pods downed by lethal damage
    uint32_t savedCount()  const { return m_saved; }   // pods spared via sparePod()
    uint32_t downedCount() const;            // killed + saved (out of the fight)
    uint32_t aliveCount()  const;            // pods still fighting
    uint32_t fallThreshold() const { return m_fallThreshold; }
    // The boss has FALLEN once downed pods >= the fall threshold.
    bool      hasFallen()  const { return downedCount() >= m_fallThreshold; }

private:
    std::vector<std::unique_ptr<MonsterSystem>> m_pods;
    std::vector<std::string>                    m_podNames;
    uint32_t m_fallThreshold = 0;   // resolved threshold (cfg.fallThreshold or podCount)
    uint32_t m_maxSaved      = 0;   // 0 => uncapped
    uint32_t m_saved         = 0;   // pods spared so far
};

// Convenience: the canonical Chorus config (5 fused minds, save up to 4). DATA only
// — the Wave-2 Nexus module may use this verbatim or build its own Config. Models
// fall back to the tinted box when the GLBs are absent (clean checkout safe).
MultiPodBoss::Config chorusConfig();

// ---------------------------------------------------------------------------
// SCRIPTED PRE-FIGHT HOOK (machine extension #3). A general "scripted debuff +
// faction-flip" action a floor module triggers on a boss + a set of enemies — used
// by the F5 Swarm Controller AI for Sarah's 90-second master hack: it (a) strips a
// fraction of the boss's max HP and (b) flips a designated set of enemies from
// hostile to ALLIED (the drone-army conversion). GENERAL + DATA-DRIVEN: no F5
// specifics live here; the Wave-2 module supplies the boss, the HP fraction, and
// the enemy set. Pure gameplay-state mutation — fully testable headless.
// ---------------------------------------------------------------------------
struct ScriptedFightHook {
    // Strip `fraction` (0..1) of `boss`'s MAX HP off its CURRENT HP, clamped to
    // [1, current] (never kills outright — the boss still spawns/fights). Returns
    // the HP actually removed. Used for the master-hack "boss spawns at -75% HP".
    // No phase side-effects here; the boss's own update() advances phases from the
    // new (lower) HP fraction on the next tick.
    static int stripBossHp(MonsterSystem& boss, float fraction);

    // Flip a set of enemies from hostile to ALLIED: each MonsterSystem in `enemies`
    // is neutralized as a threat (its attack damage zeroed so it no longer harms the
    // player) and reported as converted. Returns how many were flipped. The host
    // owns any re-tinting / re-targeting; this is the gameplay-state half of the
    // drone-army conversion. A null entry is skipped.
    static uint32_t flipToAllied(const std::vector<MonsterSystem*>& enemies);

    // The whole master-hack action in one call: strip `bossHpFraction` of the boss
    // AND flip `drones` to allied. Returns {hpStripped, dronesFlipped}. The Wave-2
    // F5 module calls this when Sarah's hack completes.
    struct Result { int hpStripped = 0; uint32_t dronesFlipped = 0; };
    static Result masterHack(MonsterSystem& boss, float bossHpFraction,
                             const std::vector<MonsterSystem*>& drones);
};

// Headless self-test (--test-bosses, Act-1 mid-boss roster + machine extensions).
// Asserts:
//   (a) each of the 5 mid-bosses exists with sane phase/HP/damage and BUILDS;
//       the 3 single-body bosses (Chen/FE#7/Overseer) transition phases on the
//       existing HP-keyed machine; Chen exposes the cure path in Phase3; FE#7's
//       Memory-Flash window opens on a phase transition (staggered + vulnerable);
//   (b) the multi-pod Chorus DOWNS only when the pod fall-threshold is met, and the
//       SAVE path increments savedCount (not killedCount) and respects the cap;
//   (c) the scripted hook strips the right HP fraction AND flips the right enemies
//       to allied (their damage zeroed);
//   (d) Chief Martinez still constructs + behaves as before (regression guard).
// Logs PASS/FAIL T#, prints "bosses: X/Y passed", returns true iff all pass. No
// window / Vulkan. Lives in monster.cpp. Mirrors the other self-tests.
bool runBossesSelfTest();

// Adaptive Hide self-test (--test-adaptive-hide). Builds one MonsterSystem with a
// Tuning that opts into Adaptive Hide (resist 0.6, window 8 s) and walks through
// the rhythm specified in docs/canon-aliens-adaptive-hide.md §3: full damage on
// first hit, reduced on a same-type repeat, full on a type-rotation, timer expiry
// re-opens the window, opt-out (resist == 0) is dead-code (regression). Headless
// (no Vulkan / window), logs PASS/FAIL, prints "adaptivehide: X/Y passed".
bool runAdaptiveHideSelfTest();

// ===========================================================================
// ACT-2 ENEMY + BOSS ROSTER (Wave 2). Alien-planet surface (Keth'zar Prime,
// Levels 8-20). Pure DATA on top of the existing single-body multi-phase Boss
// machine + the Tuning::startAllied / copyFeintPhase / escapeTimerSeconds tags
// added for this wave. Read by act2_world.{h,cpp} (owned by another machine).
//
// Level map (EFLZ_MASTER_PLAN §Act 2 + EFLZ_WORLD_STRUCTURE §4):
//   L8     Surface Emergence    — SURFACE PURSUIT DRONE (fast ranged flyer; escape).
//   L9-10  Crystalline Desert   — NATIVE DESERT FAUNA   (crystal-shard predator).
//   L11    Salvari Camp         — SALVARI ALLY          (refugee/companion, ALLIED).
//   L12    Advanced Cave System — MEMORY HUNTER         (psychological/identity boss).
//   L13-14 Toxic Swamplands     — MUTATED SCIENTIST     + MUTATED FLORA (stationary).
//   L14    Research Station     — THE SIREN (Beta)      — transformed Aria.
//   L16    Ruined Metropolis    — BREEDER QUEEN (Beta)  — transformed Keisha (summons).
//   L20    The Spaceport        — PLANETARY GARRISON COMMANDER (3-phase finale).
//
// All HP/damage map to the combat:: bands relative to Martinez (Act-1 final ref
// HP 340 / dmg 15) — NOT the bible's raw values — so each fight stays winnable
// under the engine's iframe/cooldown budget.
// ===========================================================================

// Act-2 ENEMY rows (data-driven bestiary, mirrors EnemyType but in a separate
// enum so the Act-1 roster + --test-bestiary stay unchanged).
enum class Act2EnemyType : uint32_t {
    SalvariAlly         = 0,  // L11 — refugee/companion, ALLIED (0 dmg to player)
    NativeDesertFauna   = 1,  // L9-10 — crystalline-desert predator (neutral-or-hostile)
    MutatedScientist    = 2,  // L13-14 — toxic-swamp hostile (chemical attacks)
    MutatedFlora        = 3,  // L13-14 — toxic-swamp hostile, STATIONARY (lash reach)
    SurfacePursuitDrone = 4,  // L8 — fast ranged flyer (escape encounter)
    Count               = 5
};

// Human-readable Act-2 enemy name (logs / --test-act2bosses trace / HUD).
const char* act2EnemyTypeName(Act2EnemyType t);

// One row of the Act-2 enemy roster. Same shape as MonsterDef; kept separate so
// the Act-1 EnemyType-keyed table stays single-sourced and ordered cleanly.
struct Act2EnemyDef {
    Act2EnemyType         type;
    const char*           name;
    MonsterSystem::Tuning tuning;
};

// The full Act-2 enemy table (one row per Act2EnemyType, in enum order). Built once.
const std::vector<Act2EnemyDef>& act2EnemyDefs();
// Fetch one row by enum id (asserts enum order; defensive linear fallback).
const Act2EnemyDef& act2EnemyDef(Act2EnemyType t);
// Convenience: a spawn-ready Tuning copy. Equivalent to act2EnemyDef(t).tuning.
MonsterSystem::Tuning act2EnemyTuning(Act2EnemyType t);

// Act-2 BOSS rows (single-body, ride the existing phase machine). Separate from
// BossType so the Act-1 boss roster + --test-bosses stay unchanged.
enum class Act2BossType : uint32_t {
    MemoryHunter        = 0,  // L12 — psychological/identity gimmick (copy/feint phase)
    TheSiren            = 1,  // L14 Beta — transformed Aria (sonic/psychic lure)
    BreederQueen        = 2,  // L16 Beta — transformed Keisha (summons; tactical mind)
    GarrisonCommander   = 3,  // L20 finale — troops -> mech-suit -> orbital-strike timer
    Count               = 4
};

// Human-readable Act-2 boss name (logs / --test-act2bosses trace / HUD).
const char* act2BossTypeName(Act2BossType t);

// One row of the Act-2 boss roster (single-body, Boss-type Tuning). Same shape as
// BossDef; kept in a separate enum/table so the Act-1 boss roster is unchanged.
struct Act2BossDef {
    Act2BossType          type;
    const char*           name;
    MonsterSystem::Tuning tuning;
};

// The full Act-2 boss table (one row per Act2BossType, in enum order). Built once.
const std::vector<Act2BossDef>& act2BossDefs();
const Act2BossDef& act2BossDef(Act2BossType t);
MonsterSystem::Tuning act2BossTuning(Act2BossType t);

// Headless self-test (--test-act2bosses, Act-2 roster + Wave-2 data hooks).
// Asserts:
//   (a) each of the 5 Act-2 enemy defs exists and BUILDS with sane stats; the
//       Salvari ally reports allied==true AND attack damage == 0 (cannot harm
//       the player); mutated flora is stationary (chaseSpeed == 0); the surface
//       pursuit drone is a fast ranged flyer (ranged && flyer && fast chase).
//   (b) each of the 4 Act-2 bosses exists with Boss-type stats + valid phase
//       thresholds AND advances Phase1 -> Phase2 -> Phase3 on the HP machine.
//   (c) Memory Hunter carries a copy/feint phase descriptor (copyFeintPhase>0)
//       AND reports inCopyFeintPhase() once driven into that phase.
//   (d) Garrison Commander has its 3 phases AND carries an escape-timer tag
//       (escapeTimerSeconds > 0) that the floor module reads in P3.
//   (e) Breeder Queen summons in P3 (phase3SummonCount > 0).
//   (f) REGRESSION: Chief Martinez + all 3 single-body Act-1 bosses still build
//       (Boss-type, table HP/damage, Phase1 at spawn).
// Logs PASS/FAIL T#, prints "act2bosses: X/Y passed", returns true iff all pass.
// No window / Vulkan. Lives in monster.cpp. Mirrors the other self-tests.
bool runAct2BossesSelfTest();

// ---------------------------------------------------------------------------
// CHARACTER MUTUAL EXCLUSION (Tim 2026-07-26): no two characters (monster OR
// captive) may occupy the same volume. A live character is a planar disc (center
// x,z + collision radius r) that is either MOVABLE (a monster — may be pushed) or
// an ANCHOR (a captive/companion — immovable; monsters get pushed off it).
// ---------------------------------------------------------------------------
struct SepBody {
    float x = 0.0f, z = 0.0f, r = 0.4f;
    bool  movable = true;   // false = captive anchor (never moved off a monster)
};

// Resolve character-vs-character overlap IN PLACE so that, after the pass, no two
// centers are closer than r_i + r_j. Iterative relaxation over `maxIters` passes:
// each overlapping pair is separated along its center axis — movable/movable splits
// the correction 50/50, movable/anchor moves only the movable one the full overlap,
// anchor/anchor splits (never left merged). Exactly co-located centers get a
// deterministic seeded jitter axis so the push is stable + reproducible. Returns the
// number of pair corrections applied on the LAST pass (0 => fully resolved). Pure /
// allocation-free / headless-testable (--test-canonplay P11). Defined in monster.cpp.
uint32_t resolveCharacterOverlaps(SepBody* bodies, uint32_t n,
                                  uint32_t maxIters = 8, uint32_t seed = 0x9E3779B9u);

} // namespace x3::game
