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
//   * makeDrawables() does NOT bake per-node TRS, so all primitives draw at one
//     model transform (minor offset possible for multi-node models). Same caveat
//     as S1/S5; acceptable for the slice.
//   * Audio (gunshot / monster death sound) is DEFERRED — no audio system until
//     M9. No sound is played on fire or death.

#include "scene.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/asset/IModelLoader.h"
#include "engine/asset/IAssetSource.h"

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace x3::game {

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
    // Build the monster: load alien_crawler.glb from `modelDir` via a fresh
    // IAssetSource + the M2 model loader, upload its drawables through `device`,
    // add an Enemy-layer collision box body via `physics` (sized to roughly the
    // model bounds), and register a Tag::Monster Entity at `pos` in `scene`. On
    // load failure a procedural box monster is used instead. Logs which path
    // (real GLB vs. fallback box) was taken. Call once.
    void buildMonster(Scene& scene, x3::rhi::IRenderDevice& device,
                      x3::phys::IPhysicsWorld& physics,
                      std::string_view modelDir, const x3::phys::Vec3& pos);

    // Fire one shot: raycast from `eye` along `dir` (need not be unit; normalized
    // internally) up to kFireMaxDist against the Enemy layer. If the hit body
    // resolves to THIS live monster, apply kDamagePerShot, start the hit-flash,
    // and — if HP drops to 0 — kill it (hide Entity, remove body, mark dead).
    // Returns what happened (see FireResult). The caller is responsible for the
    // "only if armed" gate (WeaponSystem::hasWeapon()); a no-op here when dead.
    FireResult fire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                    Scene& scene, x3::phys::IPhysicsWorld& physics);

    // Advance one frame: decay the hit-flash timer and, if kChaseSpeed > 0 and the
    // monster is alive, crawl it toward `playerPos` (translation only) via
    // setBodyPosition + sync the Entity transform. No-op once dead.
    void update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                const x3::phys::Vec3& playerPos);

    // Draw all monster primitives at its current transform, tinted toward red by
    // the active hit-flash. No-op once dead / hidden. The monster Entity carries an
    // invalid render mesh so Scene::render skips it; this is the single source of
    // truth for the multi-primitive model (mirrors WeaponSystem::drawPickup). Call
    // alongside scene.render() each frame.
    void drawMonster(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                     const Scene& scene) const;

    // Gameplay state.
    int  hp() const { return m_hp; }
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

    x3::phys::Vec3   m_pos{};                 // current body-center world position
    uint32_t         m_entity = kNoLink;      // index into the Scene
    x3::phys::BodyId m_body;                  // Enemy-layer collision box
    float            m_modelScale = 1.0f;     // uniform scale applied to the model

    int   m_hp        = kMonsterHp;
    bool  m_alive     = true;
    float m_flash     = 0.0f;                 // remaining hit-flash time (s)

    // Death "pop": when killed, m_alive flips false and the body is removed, but
    // m_dying stays true and m_deathPop counts down from kDeathPopTime while the
    // model keeps drawing (shrinking + flashing). Once it reaches 0 the Entity is
    // hidden and m_dying clears.
    bool  m_dying     = false;
    float m_deathPop  = 0.0f;                 // remaining death-pop time (s)

    // Current yaw (radians) the model faces; baked into the render 3x3 each frame.
    float m_yaw       = 0.0f;
};

// Headless self-test (--test-combat). Builds a physics world + an Enemy-layer
// monster + a Scene, then asserts T1 (ray at monster damages it), T2 (enough
// shots kill it -> hidden, body removed, subsequent rays miss), T3 (ray aimed
// away does no damage), T4 (firing when NOT armed does nothing). Logs PASS/FAIL
// T#, returns true iff all pass. No window/Vulkan. Mirrors the S4/S5 self-tests.
bool runCombatSelfTest();

} // namespace x3::game
