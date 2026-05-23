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

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/asset/IModelLoader.h"
#include "engine/asset/IAssetSource.h"
#include "engine/ai/INavigation.h"   // GENERAL navigation: route around walls (optional)

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
enum class AiState : uint32_t {
    Idle = 0, Search = 1, Advance = 2, Attack = 3, Strafe = 4, Retreat = 5, Regroup = 6
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
// Per-instance decision cadence + jitter so enemies don't all switch in lockstep.
constexpr float kAiDecisionPeriod  = 0.30f;  // re-evaluate state every ~0.3 s
constexpr float kAiDecisionJitter  = 0.15f;  // +/- randomization on the cadence (s)
constexpr float kAiStateMinTime    = 0.45f;  // min dwell before a non-forced switch

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

        // ---- Data-driven AI weighting (bestiary pass) ---------------------
        // Per-instance strafe/flank bias in [0,1]: the probability, at mid-range,
        // that the enemy STRAFES (orbits/flanks) instead of straight-Advancing. A
        // negative value (the default) means "use the MonsterType default" in the
        // state machine (Guard 0.20, Drone 0.75, Boss 0.45) so existing enemies are
        // unchanged. The MonsterDef roster sets this per archetype (e.g. Verthani
        // strafe-heavy ~0.80, Illuminated standoff-low ~0.10) so AI weighting is
        // DATA, not new code. Clamped to [0,1] when >= 0.
        float aiStrafeBias        = -1.0f;

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
                    int damage = kDamagePerShot);

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
    bool takeMeleeDamage(int damage, Scene& scene, x3::phys::IPhysicsWorld& physics);

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

    // The monster's entity id (kNoLink until built) and physics body.
    uint32_t entity() const { return m_entity; }
    x3::phys::BodyId body() const { return m_body; }

    // Current body-center world position (D-ai: read by the ally query / regroup).
    x3::phys::Vec3 pos() const { return m_pos; }

    // True if the real GLB loaded; false if the procedural fallback box is in use.
    // Valid after buildMonster().
    bool usingRealModel() const { return m_usingReal; }

private:
    // Issue the per-primitive drawMesh calls for the monster at `model`, with
    // `tint` multiplied into each primitive's base color (for the hit-flash).
    void drawMonsterAt(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                       const float model[16], const float tint[4]) const;

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
    // Game-feel cue sink (footstep / impact). Empty => throttled-log stub.
    GameCueFn                m_cueSink;
    int                      m_idleClip = -1;
    int                      m_walkClip = -1;
    // T1: separate Run / Jump clips for the locomotion blend (multi-clip *_anim.glb).
    // When a distinct Walk AND Run exist the 1D blend is driven by planar speed; on
    // single-locomotion-clip models (only Idle, or Idle+one move clip) it degrades
    // to the legacy idle/move switch via the Skinner's graceful blend collapse.
    int                      m_runClip  = -1;
    int                      m_jumpClip = -1;
    bool                     m_useLocoBlend = false;  // a real idle(+walk/+run) set drives the blend
    float                    m_animTime = 0.0f;
    bool                     m_animActive = false;   // a usable clip was found
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
    float            m_hitCenterOff = 0.0f;   // box center offset above m_pos (feet-origin ground enemies raise it)
    bool             m_flyer       = false;   // hovering, center-origin enemy (vs ground feet/center-origin)

    int   m_hp        = kMonsterHp;
    int   m_maxHp     = kMonsterHp;           // per-instance starting HP (Tuning)
    float m_chaseSpeed = kChaseSpeed;         // per-instance chase speed (Tuning)
    float m_baseTint[4] = { 1, 1, 1, 1 };     // per-instance color multiplier (Tuning)
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

    // Death "pop": when killed, m_alive flips false and the body is removed, but
    // m_dying stays true and m_deathPop counts down from kDeathPopTime while the
    // model keeps drawing (shrinking + flashing). Once it reaches 0 the Entity is
    // hidden and m_dying clears.
    bool  m_dying     = false;
    float m_deathPop  = 0.0f;                 // remaining death-pop time (s)

    // Current yaw (radians) the model faces; baked into the render 3x3 each frame.
    float m_yaw       = 0.0f;

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

    // ---- GENERAL navigation (optional, off by default) --------------------
    // Borrowed shared nav grid (nullptr => straight-line, original behaviour).
    const x3::ai::INavGrid* m_navGrid = nullptr;
    x3::ai::PathFollower    m_follower;       // walks the current A* path's waypoints
    float    m_repathTimer  = 0.0f;          // countdown to rebuild the path (cadence)
    x3::phys::Vec3 m_pathGoal{};             // goal the current path was built toward
    bool     m_hasPath      = false;         // a valid path is being followed
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

    // Number of monsters still alive (not dead). Used for objective/door gating.
    uint32_t aliveCount() const;

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
                    int damage = kDamagePerShot);

private:
    std::vector<std::unique_ptr<MonsterSystem>> m_monsters;
    GameCueFn m_cueSink;   // applied to every spawn (current + future)
};

// Headless self-test (--test-combat). Builds a physics world + an Enemy-layer
// monster + a Scene, then asserts T1 (ray at monster damages it), T2 (enough
// shots kill it -> hidden, body removed, subsequent rays miss), T3 (ray aimed
// away does no damage), T4 (firing when NOT armed does nothing). Logs PASS/FAIL
// T#, returns true iff all pass. No window/Vulkan. Mirrors the S4/S5 self-tests.
bool runCombatSelfTest();

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
    FireResult fire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                    Scene& scene, x3::phys::IPhysicsWorld& physics,
                    int damage = kDamagePerShot);

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

} // namespace x3::game
