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

// ===========================================================================
// CAPITAL DEATH SEQUENCE — the staged break-up a 450 m dreadnought earns.
// ===========================================================================
//
// WHY THIS EXISTS (owner playtest, 2026-08: "Antimatter critical, igniting ion
// drive? Does that mean I destroyed the big enemy ship? We should SEE that").
// A capital kill USED to be one frame of nine simultaneous fireballs followed
// by a 2.6 s hold on an untouched, fully-lit hull — the ship never visibly
// died, so the kill read as nothing at all. A capital does not pop: it ruptures,
// walks a cascade of secondaries down its spine, vents, goes DARK, sheds hull
// sections, and only THEN does the core let go.
//
// This is a pure value-type state machine — no GPU, no host types, no RNG. It
// owns the SCHEDULE (what happens when) and the PRESENTATION SCALARS the host
// reads each frame (how lit the hull is, how far it has rolled over). The host
// owns the primitives: it pumps step()'s events into CombatFx / the GPU debris
// pool and multiplies the draw by lights()/tumble(). That split is what makes
// the whole sequence testable headless with zero rendering.
//
// DETERMINISM: the schedule is a compile-time table walked by a cursor, so the
// same dt stream always yields the same events in the same order. BUDGET: step()
// never emits more than kMaxEventsPerStep events in one call — a long frame
// hitch cannot dump the whole cascade into a single frame's particle pool.

// What the host should spawn for one scheduled beat of the sequence.
enum class DeathFx : uint32_t {
    Rupture,        // first hull rupture: white-hot flash + a shockwave shell
    Cascade,        // one secondary detonation walked along the hull spine
    Vent,           // an atmosphere / plasma vent plume streaming off the hull
    HullFragment,   // a hull section tears free (a heavy, chunky debris burst)
    CoreDetonation, // the reactor lets go: the biggest blast of the sequence
};

// One scheduled beat, resolved to world space by step().
struct DeathEvent {
    DeathFx kind   = DeathFx::Cascade;
    float   pos[3] = { 0.0f, 0.0f, 0.0f };  // world position of the beat
    float   radius = 0.0f;                  // blast/plume radius in metres
    float   drift[3] = { 0.0f, 0.0f, 0.0f };// outward direction for plume/debris
};

// Live state of one capital's death. Host-owned; begin() seeds it, step()
// advances it. Copyable POD.
struct CapitalDeathState {
    bool  active   = false;   // begin() called and the sequence has not finished
    bool  finished = false;   // the full schedule has played out
    float t        = 0.0f;    // seconds since begin()
    int   cursor   = 0;       // next un-emitted entry in the schedule table
    float center[3] = { 0.0f, 0.0f, 0.0f };  // hull centre at the moment of death
    float axis[3]   = { 1.0f, 0.0f, 0.0f };  // hull long axis (unit; bow -> stern)
    float halfLen   = 210.0f;                // half the hull length in metres

    // ---- Presentation scalars the host reads EVERY frame -------------------
    // Hull emissive / self-light multiplier. Starts at 1 (window rows, running
    // lights and the drive plume all lit), stutters, then falls to kLightsOutFloor
    // and stays there: THE LIGHTS GO OUT. A dark hull against the starfield is
    // the single clearest "that ship is dead" signal in the genre.
    float lights   = 1.0f;
    // Dead-stick roll, radians. A ship with no attitude control tumbles.
    float tumble   = 0.0f;
    // Metres the dead hull has fallen off its patrol line (it stops holding
    // station and starts going DOWN — the read that sells "she's going in").
    float sag      = 0.0f;
};

class CapitalDeathSequence {
public:
    // Total run time. The host must hold the death window open at least this
    // long or the player never sees the sequence finish.
    static constexpr float kDurationSec      = 5.0f;
    // Hard per-step emission cap (frame-cost guard — see the class comment).
    static constexpr int   kMaxEventsPerStep = 3;
    // Where the hull emissive bottoms out (not 0: a black cutout reads as a
    // hole in the starfield, not a wreck — X3_WORLD_RULES rule 5).
    static constexpr float kLightsOutFloor   = 0.10f;
    // How many scheduled beats the whole sequence contains.
    static int scheduleSize();

    // Arm the sequence at `center`, with the hull's long axis `axis` (need not
    // be unit; a degenerate axis falls back to +X) and half-length `halfLen`.
    // Resets every field — safe to re-arm a used state.
    static void begin(CapitalDeathState&, const float center[3],
                      const float axis[3], float halfLen);

    // Advance by `dt` and write up to `maxOut` (and never more than
    // kMaxEventsPerStep) newly-due events into `out`. Returns how many were
    // written. Also updates lights/tumble/sag. No-op (returns 0) when the state
    // is not active. Sets `finished` once the schedule AND kDurationSec are done.
    static int step(CapitalDeathState&, float dt, DeathEvent* out, int maxOut);

    static bool isActive(const CapitalDeathState& s)   { return s.active; }
    static bool isFinished(const CapitalDeathState& s) { return s.finished; }
};

// ---- --test-ship-damage self-test (>=8 sub-checks, no window/Vulkan) --------
// Pure-logic, deterministic. Asserts: fighter shield+hull seeded; shield drains
// before hull; overflow math exact (shield 50, dmg 80 -> shield 0, hull -30);
// isDestroyed flips at hull<=0; tick regens shield ONLY after the delay (not
// before); makeCapital enables subsystems; targeted subsystem damage routes
// only when shield is down; subsystemDown flips at 0; shieldFrac/hullFrac
// correct. Returns true iff all pass. PLUS the capital-death-sequence block:
// begin() arms from a live model whose hull reached 0; the schedule fires a
// Rupture first and a CoreDetonation last; the cascade walks the hull axis
// bow->stern; lights fall to the floor; the per-step emission budget is never
// exceeded; the sequence finishes; and the whole thing is bit-deterministic
// across two identical runs.
bool runShipDamageSelfTest();

} // namespace x3::space
