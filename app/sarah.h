#pragma once
// SARAH COMPANION COMBAT (LANE B — docs/design/THE_CLONE_BOSS_PLAN.md).
//
// Game/slice code only — engine/ stays pure. Sarah is the Act-1 finale ally: she
// spawns on F7 RESTRAINED (collared, non-combat, idling on the AnnaTactical rig),
// and when Lane A's "Sarah freed" event fires she WAKES and FIGHTS BESIDE JAKE —
// she follows him, acquires the nearest hostile, and fires a hitscan on a cooldown
// with an aim/fire pose, until she is downed or the fight ends.
//
// THIS IS A SELF-CONTAINED MODULE with a CLEAN INTERFACE Lane A / the integrator
// wires to (nothing in here reaches into spire_top / the Clone boss):
//   * build()        — spawn her restrained/idle on F7 (call once).
//   * onFreed()      — THE trigger. Lane A calls this on its Phase-2 collar-destroy
//                      success; Sarah WAKES into a combat companion. Idempotent.
//   * freed()        — the flag the integration reads back.
//   * onCloneDown()  — Lane A calls this when the Clone falls (her victory bark).
//   * takeDamage()   — drop her HP; at 0 she is INCAPACITATED (downed pose), NOT
//                      deleted — the emotional beat.
//
// REUSE, not reinvention (per the brief):
//   * The load-GLB + Skinner idle/walk locomotion + facing-flip + follow pattern is
//     lifted from RescueVictim (app/rescue.cpp, the #48 companion path).
//   * Combat firing mirrors the MonsterSystem drone ranged path: a muzzle->target
//     Static rayCast for line-of-sight, then damage the acquired hostile — here via
//     MonsterSystem::takeMeleeDamage (the ally version: target ENEMIES, not Jake).
//   * Personal-space separation (#25 / ecology) keeps her from stacking on / blocking
//     Jake: she holds a standoff ring and is pushed off the player + other allies.
//
// MP-friendly style: this OWNS Sarah's state (restrained/awake/incapacitated + HP +
// cooldowns) and is driven purely by data fed in each frame (dt, the player position,
// the live hostile list). The host reads it for the HUD and pokes onFreed()/takeDamage().

#include "scene.h"
#include "monster.h"   // MonsterSystem (the hostiles she shoots) + AttackFxFn
#include "anim.h"      // Skinner (idle/walk/aim so she isn't a frozen mannequin)

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

// Sarah's lifecycle.
//   * Restrained   — spawned collared; idles on her rig; NO combat, NO follow.
//   * Awake        — freed (onFreed): follows Jake + fights (acquire + fire).
//   * Incapacitated— HP hit 0: downed pose, out of the fight, NOT deleted.
enum class SarahState : uint32_t { Restrained = 0, Awake = 1, Incapacitated = 2 };

// ---- Combat / follow tuning (tuned so she HELPS but Jake still carries) -------
constexpr int   kSarahHp            = 90;    // a touch under Jake's 100
constexpr int   kSarahShotDamage    = 8;     // per hitscan (Jake's rifle ~34) — support DPS
constexpr float kSarahFireCooldown  = 0.9f;  // seconds between shots (~9 DPS)
constexpr float kSarahEngageRange    = 28.0f;// acquire a hostile within this (m)
constexpr float kSarahFireRange      = 24.0f;// only fire when the target is within this (m)
// NOTE: there is no separate "aim-pose duration" — she HOLDS the weapon-up pose for as
// long as a hostile is inside kSarahFireRange (a per-shot pose would snap back to idle
// during the 0.9 s cooldown and read as a twitch). See SarahCompanion::tick.
// Follow (mirrors the rescue companion, but a beat wider so she flanks, not crowds).
constexpr float kSarahFollowSpeed   = 4.2f;  // m/s toward the player
constexpr float kSarahFollowStop    = 2.6f;  // hold this far from Jake (standoff ring)
constexpr float kSarahTeleport      = 30.0f; // snap up if left this far behind
// Personal-space separation (#25): pushed off anything closer than this.
constexpr float kSarahSepRadius     = 1.6f;  // crowd radius (m)
constexpr float kSarahSepStrength   = 2.5f;  // push speed out of the crowd (m/s)

// A one-line spoken bark. The host wires setBarkSink to route it to the dialog /
// subtitle / TTS layer (dialog.* VoiceId::Sarah); unset => a log line. Fired ONCE
// per event (freed / first-kill / clone-down), never per frame.
using SarahBarkFn = std::function<void(const std::string& line)>;

// Sarah — a self-contained companion. build() once, tick() each frame, onFreed()
// to wake her, takeDamage() to down her. Mirrors RescueVictim's shape.
class SarahCompanion {
public:
    // Spawn Sarah RESTRAINED at `pos` on the AnnaTactical rig (loaded from `modelDir`;
    // a procedural box stands in on load failure so the level never breaks). She idles
    // but does not follow or fight until onFreed(). Call once.
    void build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics,
               std::string_view modelDir, const x3::phys::Vec3& pos);

    // ---- THE CLEAN INTERFACE (Lane A wires these) -------------------------
    // WAKE Sarah: Restrained -> Awake. She now follows Jake + fights. Idempotent
    // (a second call is a no-op); no-op if she is already Incapacitated. Fires the
    // "freed" bark once.
    void onFreed();
    bool freed() const { return m_state == SarahState::Awake ||
                                (m_state == SarahState::Incapacitated && m_wasFreed); }
    // Lane A calls this when the Clone boss falls — her victory bark (once).
    void onCloneDown();

    // Drop `dmg` off Sarah's HP. At <= 0 she is INCAPACITATED: downed pose, out of
    // the fight, but NOT deleted (her entity/model persist). No-op once incapacitated
    // or while Restrained (she can't be hurt before she's in the fight). Returns true
    // the frame she goes down.
    bool takeDamage(int dmg, Scene& scene, x3::phys::IPhysicsWorld& physics);

    // ---- Per-frame ---------------------------------------------------------
    // Advance one frame. While Restrained: idle in place. While Awake: FOLLOW `playerPos`
    // (holding the standoff ring, pushed off the player + `allies` for separation),
    // ACQUIRE the nearest live hostile in `hostiles` (alive, not allied) within engage
    // range with clear line-of-sight, and FIRE a hitscan on cooldown (damaging it via
    // MonsterSystem::takeMeleeDamage), playing the aim/fire pose. While Incapacitated:
    // hold the downed pose. `hostiles` is the host's live-enemy list (it may gather
    // from several MonsterManagers on F7); null/empty => she just follows.
    void tick(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
              const x3::phys::Vec3& playerPos,
              const std::vector<MonsterSystem*>& hostiles,
              const std::vector<x3::phys::Vec3>& allies = {});

    // Draw her model at its current transform (the entity render mesh is invalid; this
    // owns the multi-primitive draw, like RescueVictim::draw / MonsterSystem::drawMonster).
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
              const Scene& scene) const;

    // ---- Host hooks (inert by default) ------------------------------------
    void setBarkSink(const SarahBarkFn& sink) { m_bark = sink; }
    // Visible tracer FX per shot (muzzle->impact). Wire to CombatFx::addTracer. Optional.
    void setAttackFx(const AttackFxFn& fx) { m_fx = fx; }

    // ---- Queries (HUD + tests) --------------------------------------------
    SarahState state() const { return m_state; }
    bool restrained()    const { return m_state == SarahState::Restrained; }
    bool awake()         const { return m_state == SarahState::Awake; }
    bool incapacitated() const { return m_state == SarahState::Incapacitated; }
    bool built()         const { return m_built; }
    int  hp()    const { return m_hp; }
    int  maxHp() const { return kSarahHp; }
    x3::phys::Vec3 pos() const { return m_pos; }
    // The hostile she is currently aiming at (nullptr if none acquired / not awake).
    const MonsterSystem* target() const { return m_target; }
    bool hasTarget() const { return m_target != nullptr; }
    // True on any frame she is in an aim/fire pose (read by the HUD / test).
    bool firing() const { return m_fireAnimT >= 0.0f; }
    uint32_t kills() const { return m_kills; }
    // Animation introspection (grounded-anim QA / the self-test).
    bool animActive() const { return m_animActive; }
    const x3::anim::Skinner& skinner() const { return m_skinner; }
    const x3::asset::Model&  model()   const { return m_model; }

    // Override the draw tint (debug / grounded-anim framing). Multiplies base color.
    void setTint(float r, float g, float b, float a) { m_tint[0]=r; m_tint[1]=g; m_tint[2]=b; m_tint[3]=a; }

private:
    // Bake yaw + scale + pos into the entity transform (with the +180deg VISUAL flip the
    // rigged +Z GLBs need — identical to RescueVictim::bakeTransform).
    void bakeTransform(Scene& scene);
    // Drive the Skinner one frame: aim/fire pose while firing, else walk/idle by speed.
    void driveAnim(float dt, float planarSpeed);
    void drawAt(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                const float model[16]) const;
    // Acquire the nearest live, non-allied hostile within engage range (LOS not required
    // for acquisition — LOS is re-checked at the shot). Returns nullptr if none.
    MonsterSystem* acquireNearest(const std::vector<MonsterSystem*>& hostiles) const;
    // Clear line-of-sight from Sarah's muzzle to `p` — i.e. no WALL between.
    // GOTCHA (engine physics): rayCast's `mask` is not an exclusive layer filter —
    // queryHitsLayer() falls through to objectLayersCollide(), and Static collides
    // with Dynamic/Player/Enemy, so a Layer::Static ray ALSO hits enemy bodies. A
    // naive "no Static hit" test therefore always reports blocked: the first thing
    // the ray hits is the very hostile she is aiming at. So the hit is reconciled
    // against `hostiles` — a hit on any hostile's own body is NOT a wall.
    bool losClear(x3::phys::IPhysicsWorld& physics, const x3::phys::Vec3& p,
                  const std::vector<MonsterSystem*>& hostiles) const;
    void bark(const std::string& line);

    // ---- Model + skin (mirrors RescueVictim) ------------------------------
    std::unique_ptr<x3::asset::IAssetSource> m_assets;
    std::unique_ptr<x3::asset::IModelLoader> m_loader;
    x3::asset::Model                         m_model;
    std::vector<x3::asset::ModelDrawable>    m_drawables;
    bool                                     m_usingReal = false;

    x3::anim::Skinner       m_skinner;
    x3::rhi::IRenderDevice* m_device   = nullptr;
    int   m_idleClip = -1, m_walkClip = -1, m_runClip = -1;
    int   m_aimClip  = -1;   // aim/fire pose (fuzzy: aim/fire/shoot/rifle/attack)
    int   m_deathClip = -1;  // downed pose (fuzzy: death/die/collapse)
    bool  m_useLocoBlend = false;
    bool  m_animActive   = false;
    float m_animTime     = 0.0f;

    // ---- Gameplay state ---------------------------------------------------
    SarahState m_state = SarahState::Restrained;
    bool  m_built    = false;
    bool  m_wasFreed = false;   // she reached Awake at least once (survives incap)
    int   m_hp       = kSarahHp;
    float m_fireCooldown = 0.0f; // counts DOWN to the next shot
    float m_fireAnimT    = -1.0f;// >=0 while in the aim/fire pose (counts up)
    MonsterSystem* m_target = nullptr;   // borrowed (host owns the hostiles)
    uint32_t m_kills = 0;
    bool  m_firstKillBarked = false;
    bool  m_cloneDownBarked = false;

    // ---- Transform / body -------------------------------------------------
    x3::phys::Vec3   m_pos{};
    uint32_t         m_entity = kNoLink;
    x3::phys::BodyId m_body;
    float            m_modelScale = 1.0f;
    float            m_yaw = 0.0f;
    float            m_tint[4] = { 1, 1, 1, 1 };

    SarahBarkFn m_bark;
    AttackFxFn  m_fx;
};

// Headless self-test (--test-companion-combat). Builds Sarah + a hostile on a
// HeadlessDevice + Jolt world and asserts:
//   (C1) she spawns RESTRAINED (no follow / no fire) and ignores a hostile;
//   (C2) onFreed() WAKES her (Awake, freed()) — idempotent;
//   (C3) freed, she ACQUIRES the nearest hostile and FIRES (its HP drops);
//   (C4) freed, she FOLLOWS the player (closes the distance to the standoff ring);
//   (C5) SEPARATION: spawned on top of the player she is pushed OUT (doesn't stack);
//   (C6) takeDamage to 0 => INCAPACITATED, NOT deleted (entity still present),
//        and she stops firing/following.
// Logs PASS/FAIL C#, returns true iff all pass. No window / Vulkan.
bool runCompanionCombatSelfTest();

} // namespace x3::game
