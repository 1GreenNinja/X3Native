#pragma once
// THE WATER ZAP — "Lightning gun will electrify the water.. one Zap, and the
// player takes half health damage, and all the fish around die" (Tim, 2026-07-11).
//
// The LIGHTNING GUN (WeaponFxKind::Lightning / DamageType::Energy) is the one
// weapon that DOES something when its shot meets water: the surface goes live.
//
// TRIGGER (either is enough):
//   * the shot's ray CROSSES THE WATER SURFACE within the weapon's range
//     (findWaterEntry marches the ray against the world water query), or
//   * the shooter is HIMSELF in the water (a swimming/wading player firing the
//     lightning gun electrifies the pool he is floating in — this is the joke,
//     and it hurts). The host therefore lets the lightning gun fire while
//     swimming; every OTHER weapon keeps the dry-click refusal.
//
// ONE ZAP: a WaterZapper LATCH + cooldown (kWaterZapCooldown). Holding the
// trigger on an automatic beam weapon does NOT re-zap every frame: the latch
// must be released (trigger up) AND the cooldown must expire.
//
// DAMAGE (Tim's numbers, exactly):
//   * PLAYER: if he is IN the water (feet below the surface — swimming OR
//     wading) and within kWaterZapRadius of the entry point (measured on the
//     water plane — electrification is a SURFACE phenomenon), he takes
//     HALF OF MAX HEALTH, once per zap. 100 max HP -> 50 damage.
//   * FISH: every LIVE fish within the radius DIES — belly-up, floats,
//     drifts, despawns (FishSystem::killWithin).
//   * MONSTERS/NPCs standing in the water within the radius take
//     kWaterZapEnemyDamage of DamageType::Energy — a wading monster fries.
//   * CROWDS: the host feeds onViolence(center) — they scatter.
//
// RADIUS: kWaterZapRadius = 12 m on the water plane (tuned: it lights up a
// whole river reach without electrifying the next county).

#include "fish.h"
#include "monster.h"
#include "player.h"
#include "scene.h"

#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>
#include <functional>

namespace x3::game {

// The water-surface query (same feed the Player's swim state and the fish use:
// x3::game::worldWaterLevelAt). Returns the surface Y, or < kFishDryTest when dry.
using WaterZapQueryFn = FishWaterFn;

// ---- Tuning (documented; the self-test asserts the damage numbers) ----------
constexpr float kWaterZapRadius   = 12.0f;   // damage/arc radius on the water plane (m)
constexpr float kWaterZapCooldown = 1.75f;   // seconds between zaps ("ONE Zap")
constexpr int   kWaterZapEnemyDamage = 150;  // Energy damage to anything wading in it
constexpr int   kWaterZapArcs     = 18;      // radial surface arcs drawn per zap
constexpr float kWaterZapMarch    = 0.35f;   // ray-march step for the surface crossing (m)

// Where a shot MEETS the water.
struct WaterZapEntry {
    bool  hit = false;         // the ray met water (or the shooter was in it)
    float x = 0, y = 0, z = 0; // the ENTRY POINT, ON the surface (y == surfaceY)
    float surfaceY = 0.0f;     // the water surface there
    bool  fromInWater = false; // the shooter was already submerged
};

// PURE. March `origin + dir * t` for t in [0, range] and return the first point
// where the ray is at/below the water surface. If `origin` itself is under a
// water surface, returns that surface point directly (fromInWater). `dir` need
// not be normalized (it is normalized internally). No hit => .hit == false, and
// the host fires NO zap (a shot that never meets water is just a shot).
WaterZapEntry findWaterEntry(const x3::phys::Vec3& origin, const x3::phys::Vec3& dir,
                             float range, const WaterZapQueryFn& water,
                             float step = kWaterZapMarch);

// PURE. Is a body standing/floating IN the water at `feet` (feet below the
// surface = wading or swimming), and within `radius` of (cx,cz) on the plane?
bool inZappedWater(const x3::phys::Vec3& feet, float cx, float cz, float radius,
                   const WaterZapQueryFn& water);

// The LATCH. "One Zap" per trigger pull: a held trigger cannot re-zap until it
// is released AND the cooldown has expired.
class WaterZapper {
public:
    void  tick(float dt) { if (m_cool > 0.0f) m_cool -= dt; }
    // The trigger came UP — the next pull is a fresh one.
    void  triggerReleased() { m_latched = false; }
    // May a trigger pull zap right now?
    bool  canZap() const { return !m_latched && m_cool <= 0.0f; }
    // A zap just fired: latch it and start the cooldown.
    void  noteZap() { m_latched = true; m_cool = kWaterZapCooldown; m_zaps++; }
    float cooldownLeft() const { return m_cool > 0.0f ? m_cool : 0.0f; }
    bool  latched() const { return m_latched; }
    uint32_t zapCount() const { return m_zaps; }
    void  reset() { m_cool = 0.0f; m_latched = false; m_zaps = 0; }
private:
    float    m_cool = 0.0f;
    bool     m_latched = false;
    uint32_t m_zaps = 0;
};

// Apply the zap to the PLAYER: HALF OF MAX HEALTH if he is in the water inside
// the radius, else nothing. Returns the damage applied (0 = untouched). Called
// ONCE per zap by the host (the latch is what makes "once" true).
int zapPlayer(Player& player, const x3::phys::Vec3& feet, float cx, float cz,
              const WaterZapQueryFn& water, float radius = kWaterZapRadius);

// Apply the zap to every LIVE monster of `mm` that is standing in the water
// inside the radius: kWaterZapEnemyDamage as DamageType::Energy. Returns the
// number of monsters hit.
uint32_t zapMonsters(MonsterManager& mm, Scene& scene, x3::phys::IPhysicsWorld& physics,
                     float cx, float cz, const WaterZapQueryFn& water,
                     float radius = kWaterZapRadius,
                     int damage = kWaterZapEnemyDamage);

// Headless self-test (--test-waterzap). Asserts:
//   Z1 a shot from the bank into the river MEETS the water (entry on the surface);
//   Z2 a shot fired by a SUBMERGED shooter zaps the pool he floats in;
//   Z3 a shot that never meets water produces NO entry (=> no zap);
//   Z4 fish INSIDE the radius die, fish OUTSIDE survive;
//   Z5 a player IN the water loses EXACTLY half his max health, ONCE — a held
//      trigger (60 frames) does not drain him (the latch);
//   Z6 after release + cooldown a SECOND zap lands (the latch is not a one-shot);
//   Z7 a player on the BANK takes ZERO;
//   Z8 dead fish float belly-up to the surface and despawn; the fish sim is
//      DETERMINISTIC (two systems, identical ticks -> identical positions) and
//      leaks no meshes.
// Prints "waterzap: X/Y passed"; returns true iff all pass. No window/Vulkan.
bool runWaterZapSelfTest();

} // namespace x3::game
