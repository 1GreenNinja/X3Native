// app/space/ship_damage.h
//
// S10 ship damage model — the reusable shield / hull / destructible-subsystem
// damage component for the Act-3 space combat pillar.
//
// space-pilot already carries a two-pool shield-then-hull model for the PLAYER
// (see app/space_pilot.{h,cpp}: takeDamage()). This component generalizes that
// same shield-first ordering into a standalone, GPU-free value type so that
// ENEMY ships and CAPITAL ships can share one model — and adds the capital-ship
// extra: destructible subsystems (engines / turrets / shield-gen / sensors) that
// only take damage once shields are down and that, when destroyed, leave the
// host to react (engines down => can't move, etc.) via subsystemDown().
//
// Design parity with space-pilot (deliberately mirrored so the player's feel and
// the enemies' feel agree):
//   - shield absorbs first, overflow bleeds to hull;
//   - any hit resets the shield-regen-delay timer (timeSinceHit);
//   - shield regen only resumes after shieldRegenDelaySec without a hit;
//   - hull <= 0 => destroyed; no further damage applies.
// Subsystems do NOT auto-regen — they must be repaired by the host.
//
// Pure value-type logic: fully unit-testable, no GPU, no physics. Exercised by
// --test-ship-damage (headless, deterministic).
//
// CLEAN-ROOM, original work. Built from the space-engine design spec (S10 +
// decision 2.7) + the existing space_pilot two-pool model. No RBDOOM / id Tech /
// Doom / Quake source consulted.
#pragma once
#include <cstdint>

namespace x3::space {

// Destructible capital-ship subsystems. Small ships (fighters) carry NONE of
// these (hasSubsystems == false); capital ships enable all of them. `Count` is
// the array sizer / "no subsystem targeted" sentinel.
enum class Subsystem : uint32_t { Engines, Turrets, ShieldGen, Sensors, Count };

// Plain-old-data state for one ship's damage model. Constructed via the
// ShipDamage factory helpers; mutated by applyDamage()/tick(). Copyable.
struct ShipDamageModel {
    int   shield     = 0;   int maxShield = 0;
    int   hull       = 0;   int maxHull   = 0;
    float shieldRegenPerSec   = 0.0f;
    float shieldRegenDelaySec = 0.0f;  // dead time after a hit before shield ticks
    float timeSinceHit        = 0.0f;  // seconds since the last applyDamage()

    // Capital-ship destructible subsystems. Indexed by (int)Subsystem. Fighters
    // leave these zeroed with hasSubsystems == false.
    int   subHp[(int)Subsystem::Count]    = { 0, 0, 0, 0 };
    int   subMaxHp[(int)Subsystem::Count] = { 0, 0, 0, 0 };
    bool  hasSubsystems = false;
};

// Stateless operations over a ShipDamageModel. Everything is a free function in
// spirit (static methods) so the model stays a pure value type the host owns.
class ShipDamage {
public:
    // ---- Factories -------------------------------------------------------
    // A fighter: shield + hull pools, no subsystems. Regen tuning mirrors the
    // space-pilot defaults (25 hp/s after a 4 s delay) so player + enemies feel
    // consistent.
    static ShipDamageModel makeFighter(int shield, int hull);

    // A capital ship: shield + hull PLUS the four destructible subsystems, each
    // seeded to `subHp`. Slower shield regen (capitals are tankier but recover
    // more sluggishly). Enables hasSubsystems.
    static ShipDamageModel makeCapital(int shield, int hull, int subHp);

    // ---- Damage ----------------------------------------------------------
    // Apply `amount` damage. Order:
    //   1. shield absorbs first (down to 0);
    //   2. overflow bleeds to hull (hull clamps at 0 = destroyed);
    //   3. IF a subsystem is targeted (hitSub != Count) AND the model has
    //      subsystems AND the shield was ALREADY at 0 when the hit landed, the
    //      overflow is instead routed into that subsystem (clamps at 0 = down).
    //      A single hit cannot both break the shield AND hit a subsystem.
    // Any hit (amount > 0) resets timeSinceHit to 0. No-ops on a destroyed ship
    // or non-positive amount.
    static void applyDamage(ShipDamageModel&, int amount, Subsystem hitSub = Subsystem::Count);

    // ---- Per-frame -------------------------------------------------------
    // Advance timers: once timeSinceHit >= shieldRegenDelaySec, regen shield
    // toward maxShield at shieldRegenPerSec. Subsystems do NOT auto-regen.
    static void tick(ShipDamageModel&, float dt);

    // ---- Queries ---------------------------------------------------------
    static bool  isDestroyed(const ShipDamageModel&);             // hull <= 0
    static bool  subsystemDown(const ShipDamageModel&, Subsystem); // subHp <= 0 (and present)
    static float shieldFrac(const ShipDamageModel&);              // shield/maxShield in [0,1]
    static float hullFrac(const ShipDamageModel&);                // hull/maxHull in [0,1]
};

// ---- --test-ship-damage self-test (>=8 sub-checks, no window/Vulkan) --------
// Pure-logic, deterministic. Asserts: fighter shield+hull seeded; shield drains
// before hull; overflow math exact (shield 50, dmg 80 -> shield 0, hull -30);
// isDestroyed flips at hull<=0; tick regens shield ONLY after the delay (not
// before); makeCapital enables subsystems; targeted subsystem damage routes
// only when shield is down; subsystemDown flips at 0; shieldFrac/hullFrac
// correct. Returns true iff all pass.
bool runShipDamageSelfTest();

} // namespace x3::space
