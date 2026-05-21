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
//   * Optional chase is translation-only: IPhysicsWorld exposes no rotation
//     getter/setter, so the monster does NOT turn to face the player (its authored
//     orientation is preserved, same caveat as Scene::update + S4/S5). Chase is
//     OFF by default for the slice (kChaseSpeed gates it) to keep the target a
//     stationary, predictable thing to shoot.
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

// Optional gentle chase speed (m/s) toward the player. 0 disables chasing (the
// slice default: a stationary target). Translation only — no turning.
constexpr float kChaseSpeed    = 0.0f;

// Result of a fire() call, returned for HUD/logging and used by the self-test.
struct FireResult {
    bool hitMonster = false;  // the ray hit a live monster
    bool killed     = false;  // this shot dropped it to HP <= 0
    int  hpAfter    = 0;      // monster HP after the shot (0 if it died / no hit)
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
};

// Headless self-test (--test-combat). Builds a physics world + an Enemy-layer
// monster + a Scene, then asserts T1 (ray at monster damages it), T2 (enough
// shots kill it -> hidden, body removed, subsequent rays miss), T3 (ray aimed
// away does no damage), T4 (firing when NOT armed does nothing). Logs PASS/FAIL
// T#, returns true iff all pass. No window/Vulkan. Mirrors the S4/S5 self-tests.
bool runCombatSelfTest();

} // namespace x3::game
