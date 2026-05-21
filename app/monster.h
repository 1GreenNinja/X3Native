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

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/asset/IModelLoader.h"
#include "engine/asset/IAssetSource.h"

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

// Monster starting health, and damage per shot. 100 / 34 => 3 shots to kill.
constexpr int   kMonsterHp     = 100;
constexpr int   kDamagePerShot = 34;

// Max shoot range (meters) for the fire raycast.
constexpr float kFireMaxDist   = 100.0f;

// Hit-flash duration (seconds): the monster tints toward red for this long after
// a hit, then decays back to its base color.
constexpr float kHitFlashTime  = 0.1f;

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
    FireResult fire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                    Scene& scene, x3::phys::IPhysicsWorld& physics);

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
    // True during the brief death "pop" after a kill: not alive, body already
    // removed, but the model is still being drawn (shrinking/flashing).
    bool dying() const { return m_dying; }

    // The monster's entity id (kNoLink until built) and physics body.
    uint32_t entity() const { return m_entity; }
    x3::phys::BodyId body() const { return m_body; }

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
    // Model-local fixup applied between the gameplay transform and each drawable's
    // node transform (final = model * fixup * nodeTransform). Identity for Y-up
    // models; a -90deg-X stand-up for the Z-up converted character GLBs. Also
    // grounds the feet to y=0 after the rotation.
    float                                     m_modelFixup[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

    x3::phys::Vec3   m_pos{};                 // current body-center world position
    uint32_t         m_entity = kNoLink;      // index into the Scene
    x3::phys::BodyId m_body;                  // Enemy-layer collision box
    float            m_modelScale = 1.0f;     // uniform scale applied to the model

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
    float m_atkTimer            = 0.0f;       // cooldown countdown until next attack
    float m_windupTimer         = 0.0f;       // >0 while winding up an attack
    bool  m_winding             = false;      // currently in an attack wind-up

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
    // (or a default miss). At most one monster is damaged per call.
    FireResult fire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                    Scene& scene, x3::phys::IPhysicsWorld& physics);

private:
    std::vector<std::unique_ptr<MonsterSystem>> m_monsters;
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

} // namespace x3::game
