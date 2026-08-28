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

    // ---- THE SHIELD GATE (item F: "shields must drop before hull damage
    //      counts, so hosing the hull is the SLOW path") --------------------
    // Fraction of shield-OVERFLOW damage that still reaches the hull on a hit
    // that landed while the shield was UP. 1.0 == legacy behaviour (every
    // fighter and every pre-existing caller): all overflow bleeds through.
    // A capital sets this LOW, so shooting bare hull through a live shield
    // barely scratches it and the player is taught to drop the shield — or
    // kill its generator — first. Clamped to [0,1] on use.
    float hullBleedWhileShielded = 1.0f;
    // SUPPRESSION / hard shutdown of shield regen. tick() never regenerates
    // while this is set — the host raises it when the shield-generator
    // subsystem dies ("NO REGEN"), which is what makes generator-first a real
    // strategy instead of flavour text.
    bool  shieldRegenDisabled = false;
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

// THE TWO AUTHORED ENDINGS (owner, live 2026-08-17: "When it goes down.. it
// should either crash land on a planet OR break up in space"). Picked per
// encounter CONTEXT by the host (a dominant planet in the sky -> she deorbits
// toward it; deep space -> she comes apart where she floats), not rolled.
enum class DeathOutcome : uint32_t {
    BreakupInSpace, // the full in-place cascade -> core detonation (the classic)
    DeorbitCrash,   // rupture + lights-out, then the wreck FALLS toward the
                    // planet under an atmosphere-entry burn and dies as a
                    // distant flash on the disc ("she's going in")
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

    // Which authored ending this death plays (see DeathOutcome above).
    DeathOutcome outcome = DeathOutcome::BreakupInSpace;
    // DeorbitCrash only: unit direction from the kill point toward the planet
    // the wreck falls at (the hero body's sky direction). Zero for Breakup.
    float planetDir[3] = { 0.0f, 0.0f, 0.0f };

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
    // DeorbitCrash only: metres the wreck has plunged along planetDir (the
    // host draws the hull at center + planetDir * plunge). 0 for Breakup.
    float plunge   = 0.0f;
    // DeorbitCrash only: atmosphere-entry burn intensity in [0,1]. The host
    // rides its warm hull glow + the entry trail off this. 0 for Breakup.
    float burn     = 0.0f;
};

class CapitalDeathSequence {
public:
    // Total run time. The host must hold the death window open at least this
    // long or the player never sees the sequence finish.
    static constexpr float kDurationSec      = 5.0f;
    // DeorbitCrash runs longer: the fall + entry burn + impact flash need the
    // extra seconds to READ as a deorbit rather than a sideways drift.
    static constexpr float kDeorbitDurationSec = 9.0f;
    // Per-outcome duration (the hold the host must keep the window open).
    static constexpr float durationFor(DeathOutcome o) {
        return o == DeathOutcome::DeorbitCrash ? kDeorbitDurationSec : kDurationSec;
    }
    // Hard per-step emission cap (frame-cost guard — see the class comment).
    static constexpr int   kMaxEventsPerStep = 3;
    // Where the hull emissive bottoms out (not 0: a black cutout reads as a
    // hole in the starfield, not a wreck — X3_WORLD_RULES rule 5).
    static constexpr float kLightsOutFloor   = 0.10f;
    // How many scheduled beats the whole sequence contains.
    static int scheduleSize();

    // Arm the sequence at `center`, with the hull's long axis `axis` (need not
    // be unit; a degenerate axis falls back to +X) and half-length `halfLen`.
    // Resets every field — safe to re-arm a used state. This overload plays
    // the classic BreakupInSpace ending.
    static void begin(CapitalDeathState&, const float center[3],
                      const float axis[3], float halfLen);

    // Outcome-aware arm. For DeorbitCrash pass the unit sky direction of the
    // planet the wreck falls toward (`planetDir`; a degenerate one demotes the
    // outcome to BreakupInSpace — a crash with nowhere to crash is a breakup).
    static void begin(CapitalDeathState&, const float center[3],
                      const float axis[3], float halfLen,
                      DeathOutcome outcome, const float planetDir[3]);

    // Advance by `dt` and write up to `maxOut` (and never more than
    // kMaxEventsPerStep) newly-due events into `out`. Returns how many were
    // written. Also updates lights/tumble/sag. No-op (returns 0) when the state
    // is not active. Sets `finished` once the schedule AND kDurationSec are done.
    static int step(CapitalDeathState&, float dt, DeathEvent* out, int maxOut);

    static bool isActive(const CapitalDeathState& s)   { return s.active; }
    static bool isFinished(const CapitalDeathState& s) { return s.finished; }
};

// ===========================================================================
// CAPITAL MOTION — the dreadnought MOVES (owner, live 2026-08-17: "The
// overlords ship should move around").
// ===========================================================================
//
// A 450 m battleship does not strafe: she carves a slow, wide ARC. The state
// machine keeps the hull on a fixed-radius circle around an anchor point, so
// she is ALWAYS in the authored arena by construction (the circle IS the
// bound), with a mass-appropriate eased speed that is phase-driven:
//   * Patrol (shields up)   — stately patrol speed;
//   * Combat (shields down) — she answers the helm: speed rises;
//   * Adrift (engines hardpoint dead, or the hull is dead) — the drive is
//     gone: speed EASES to zero and she freezes on the arc (the existing
//     "your choice changed the fight" consequence, now dt-correct instead of
//     a hard snap).
// Pure value type: deterministic, dt-stable (eased exponentially, integrated
// by dt — never per-frame), fully testable headless.
enum class CapitalMovePhase : uint32_t { Patrol, Combat, Adrift };

struct CapitalMotionState {
    float anchor[3] = { 0.0f, 0.0f, 0.0f };  // arc centre (world)
    float radius    = 520.0f;                // arc radius (m)
    float arcPhase  = 0.0f;                  // position angle on the arc (rad)
    float speed     = 0.0f;                  // current tangential speed (m/s)
    float pos[3]    = { 0.0f, 0.0f, 0.0f };  // hull centre (world)
    float fwd[3]    = { -1.0f, 0.0f, 0.0f }; // unit heading (arc tangent)
    float vel[3]    = { 0.0f, 0.0f, 0.0f };  // world velocity (m/s)
    CapitalMovePhase phase = CapitalMovePhase::Patrol;
};

class CapitalMotion {
public:
    // Mass-appropriate speeds. At kPatrolSpeed the full circuit takes ~5
    // minutes and the nose swings ~1.2 deg/s — majestic, never evasive.
    static constexpr float kPatrolSpeed = 11.0f;   // m/s, shields up
    static constexpr float kCombatSpeed = 19.0f;   // m/s, shields down
    // Speed ease rate (1/s). ~e-fold in 4.5 s: the helm answers like mass.
    static constexpr float kSpeedEase   = 0.22f;

    // Seed the state so the hull STARTS exactly at `startPos` heading -X
    // (nose at the player's approach lane — the reveal framing is preserved)
    // and arcs from there. The anchor is placed at startPos - (0,0,radius).
    static void begin(CapitalMotionState&, const float startPos[3]);

    // Advance by dt. `shieldsDown` lifts the target speed to combat;
    // `adrift` (engines dead OR hull dead) eases it to zero.
    static void update(CapitalMotionState&, float dt,
                       bool shieldsDown, bool adrift);
};

// ===========================================================================
// CAPITAL GUN BATTERY — the big mounted weapons (owner, live 2026-08-17:
// "have big mounted weapons that HURT.. but I can shoot off").
// ===========================================================================
//
// Four VISIBLE gun mounts clustered on the ventral battery line. Each gun is
// individually destructible: shoot one off and THAT mount stops firing while
// the rest keep the gauntlet up — fire only ever ceases from destroyed
// mounts. The battery IS the Turrets subsystem made physical: each gun kill
// routes a quarter of the Turrets subsystem HP, so four gun kills == the
// subsystem down == "BATTERY SILENCED" (and a Turrets-subsystem death by any
// other path kills every remaining gun). Pure value type; the host owns the
// meshes, muzzles, telegraphs and damage resolution.
inline constexpr int kCapitalGunCount = 4;

// ===========================================================================
// THE CAPITAL'S POOLS — item F, "it should be HARDer to take down".
// ===========================================================================
// Here rather than in the host so the RELATIONSHIPS are compile-time facts
// (four gun kills must equal exactly one Turrets subsystem) and so a retune
// cannot silently invert the lesson without --test-ship-damage noticing.
//
// The player's sustained output is the thing everything else is sized
// against, and it is COOLDOWN-bound, not energy-bound: the intro tunes
// energyRegen 20/s against a 2.8 cost, so the 0.18 s laser cooldown is what
// actually caps him at 5.56 shots/s x 90 damage ~= 500 DPS. (An earlier pass
// sized these pools against the DEFAULT 8-per-shot economy — 1.5 shots/s —
// and was wrong by 3.7x. Measure the weapon, do not assume it.)
//
// Against 500 DPS:
//   HARDPOINT-FIRST ~47 s — drop the 6000 shield (12 s), walk the four 1300 HP
//     mounts (10 s), then ride the exposed reactor, whose 6x multiplier over
//     an 8-in-13 duty cycle turns 500 DPS into ~2040 against hull (25 s).
//   HULL-ONLY ~112 s — 6000 + 50000 at 1x, never crippled, no reactor.
// The 2.4x gap IS the design, and the 70 s climax window fits the first path
// and not the second. Pinned by T25n/T25o.
inline constexpr int kCapitalShield = 6000;
inline constexpr int kCapitalHull   = 50000;
inline constexpr int kCapitalSubHp  = 1300;   // PER hardpoint (~14 landed hits)

struct CapitalGun {
    int   hp      = 0;     // <= 0 => shot off (a wreck stub remains)
    int   maxHp   = 0;
    float cd      = 0.0f;  // seconds until this gun's next spool
    float spoolT  = -1.0f; // >= 0 while spooling (the telegraph); < 0 idle
};

struct CapitalBatteryState {
    CapitalGun gun[kCapitalGunCount];
};

class CapitalBattery {
public:
    // Per-gun cadence: 4 live guns stagger into one bolt every ~1.2 s — twice
    // the old single-battery pressure, which is the "they HURT" half of the
    // ask; shooting guns off is the counterplay that buys it back.
    static constexpr float kGunPeriod = 4.8f;   // per-gun seconds between spools
    static constexpr float kGunSpool  = 0.6f;   // telegraph: spool before the bolt
    // A mount is armour, not a light bulb: three landed 90-dmg hits shear it.
    // (Was 30 — a single hit — which made the battery free to silence.)
    static constexpr int   kGunHp     = 270;
    // What one landed capital bolt does to the player (vs 500 shield + 1000
    // hull): ~8 landed bolts kill — at full-battery cadence that is ~10 s of
    // flying straight at her. Aspect dodge (transverse velocity) is the out.
    static constexpr int   kGunDamage = 180;
    // Quarter of the Turrets subsystem routed per gun kill, DERIVED so the
    // identity "four sheared mounts == the Turrets subsystem down" can never
    // drift when the pools are retuned.
    static constexpr int   kSubQuarter = kCapitalSubHp / kCapitalGunCount;

    // Seed all guns alive with staggered first-spool clocks (gun i fires its
    // first bolt at ~1.2 * (i+1) s — the gauntlet opens hot but never as one
    // synchronized volley).
    static void init(CapitalBatteryState&);

    // Advance timers. While `inRange` a live idle gun counts down and then
    // SPOOLS; a spooling gun that completes FIRES. Out of range every gun
    // stands down (spool cancelled — no bolt from a target that left).
    // Returns a bitmask of guns that FIRED this tick; `spooled` (optional)
    // gets a bitmask of guns that STARTED their spool this tick (telegraph).
    static uint32_t update(CapitalBatteryState&, float dt, bool inRange,
                           uint32_t* spooled = nullptr);

    static bool gunAlive(const CapitalBatteryState&, int i);
    static int  aliveGuns(const CapitalBatteryState&);

    // Apply damage to gun i. Returns true exactly when THIS call destroyed it
    // (hp crossed to <= 0). No-op on a dead gun / bad index / amount <= 0.
    static bool damageGun(CapitalBatteryState&, int i, int amount);
};

// ===========================================================================
// CAPITAL AIM PICK — ONE ray test shared by the fire path AND the per-frame
// hover HIGHLIGHT (item F, owner 2026-08-18: "The turrets and engine mounts
// and other things can highlight when aiming at them on the capital ship").
// ===========================================================================
//
// The capital is deliberately EXEMPT from lock-on (item E): aiming at it is a
// manual skill. So the game has to TEACH what is aimable. The answer is a
// hover highlight — but a highlight that disagrees with what a shot would
// actually hit is a lie, so the highlight and the bullet MUST run the same
// test. That test lives here, once, as pure logic:
//
//   * the HULL is an OCCLUDER, not a competitor. Its sphere is a crude
//     bounding volume for a long thin ship, so it encloses essentially every
//     part bolted to the outside of it; a naive nearest-entry sweep therefore
//     lets the hull win every ray and NOTHING is ever hoverable or hittable.
//     Instead a part is hidden exactly when it lies on the FAR hemisphere with
//     respect to the ray — behind the ship's own mass from where the player is
//     looking. You still cannot snipe the engines through 1.8 km of ship: you
//     fly around. Among the parts that ARE visible, nearest entry wins;
//   * a DEAD part is not a target: it is skipped entirely, so a destroyed
//     mount never highlights again and shots there fall through to the hull
//     as wreckage damage (exactly the shipped fire-path semantics);
//   * acceptance radius GROWS with range by the same aim-at-what-you-see law
//     the fighters use (shipai::visCompFactor), so a part that is 6 px wide
//     on screen is still hoverable.
//
// Cost: one ray-sphere per candidate — ~12 spheres for the whole dreadnought,
// once per frame. No render pass, no readback, no picking buffer.
enum class CapitalAimKind : uint32_t { None, Hull, Hardpoint, Gun, Bay, Reactor };

struct CapitalAimTarget {
    float pos[3]   = { 0.0f, 0.0f, 0.0f };  // world centre
    float radius   = 0.0f;                  // base acceptance radius (m)
    CapitalAimKind kind = CapitalAimKind::None;
    int   index    = -1;                    // which hardpoint / gun / bay
    bool  alive    = true;                  // dead parts are never picked
};

struct CapitalAimResult {
    CapitalAimKind kind = CapitalAimKind::None;
    int   index = -1;
    float t     = 0.0f;   // ray parameter of the entry point (metres)
};

class CapitalAim {
public:
    // ---- RANGE COMPENSATION on the acceptance radius --------------------
    // "Aim at what you SEE": a part that is only a few pixels wide on screen
    // must still be hoverable. The rule is stated in ANGULAR terms, not as a
    // flat distance multiplier, because a capital's parts and a fighter's
    // parts differ by an order of magnitude in size: a blanket 2.5x at long
    // range turns a 270 m bay mouth into a 670 m sphere that swallows every
    // other hardpoint on the ship, while the same 2.5x is exactly right for a
    // 22 m gun on a corvette. So: grow a part only until it subtends
    // kMinAngularR, and never past kGrowMax times its authored size.
    static constexpr float kMinAngularR = 0.012f;  // ~0.7 deg (~15 px at 720p)
    static constexpr float kGrowMax     = 2.5f;    // hard ceiling on the growth
    static float grownRadius(float radius, float dist);

    // Nearest-entry pick along unit `dir` from `origin`. Returns kind None when
    // the ray misses everything (an honest whiff). `n` may be 0.
    static CapitalAimResult pick(const CapitalAimTarget* targets, uint32_t n,
                                 const float origin[3], const float dir[3]);
};

// ===========================================================================
// CAPITAL LAUNCH BAYS — "More little ships can come out of its bays"
// (owner, 2026-08-18; item G).
// ===========================================================================
//
// A spawn COUNT is a number; a bay is a place. Fighters that appear at a lit
// bay mouth and fly OUT along its launch vector make the capital read as a
// carrier that is PRODUCING the threat, and they give precision a second
// reward: a bay is a destructible hardpoint, and killing it shuts that stream
// off for good. That is item F's "hardpoint-first is the fast path" lesson
// restated in the one currency the player feels — fewer things shooting him.
//
// Escalation is bounded on TWO axes so it can never run away:
//   * a hard LIVE-FIGHTER CAP (kLiveCap): no bay launches while the arena is
//     already at cap, whatever the cadence says;
//   * a per-bay cadence that only tightens to kRateFloor of the base period
//     at zero hull — the fight gets hotter as she dies, it does not avalanche.
// Pure value type: deterministic, dt-integrated (never per-frame), headless.
inline constexpr int kCapitalBayCount = 3;

struct CapitalBay {
    int   hp    = 0;      // <= 0 => bay blown; it never launches again
    int   maxHp = 0;
    float cd    = 0.0f;   // seconds until this bay's next launch
};

struct CapitalBayState {
    CapitalBay bay[kCapitalBayCount];
};

class CapitalBays {
public:
    // A bay is a soft target compared with a gun mount — it is a hole in the
    // hull, not an armoured turret — but it still costs real sustained fire.
    // Six landed 90-dmg hits close a deck — softer than an armoured gun mount
    // (it is a hole in the hull, not a turret) but still real sustained fire.
    static constexpr int   kBayHp        = 540;
    // Base seconds between launches FROM ONE BAY at full hull. Three bays
    // staggered => roughly one fighter every ~3.7 s at the opening cadence.
    static constexpr float kLaunchPeriod = 11.0f;
    // Cadence floor as a fraction of the base period at zero hull (item G:
    // "optionally tie rate to health/phase"). 0.45 == a bit over twice the
    // opening rate when she is dying.
    static constexpr float kRateFloor    = 0.45f;
    // THE CAP. Total live hostile fighters allowed in the arena at once,
    // counting the authored escort screen. No bay launches at or above it.
    static constexpr int   kLiveCap      = 14;

    // Seed all bays alive with STAGGERED first-launch clocks so the three
    // mouths never disgorge in one synchronized lump.
    static void init(CapitalBayState&);

    // Per-bay launch period at `hullFrac` in [0,1]. Monotone: lower hull =>
    // shorter period (she throws fighters harder as she loses).
    static float periodFor(float hullFrac);

    // Advance the bay clocks by dt and return a BITMASK of bays that launched
    // this tick. `liveFighters` is the arena count BEFORE this tick; launches
    // inside this call count against the cap too, so one tick can never blow
    // past kLiveCap. `enabled` gates the whole thing (capital dead / out of
    // the encounter). A bay held at the cap keeps its clock at zero and fires
    // the instant a slot frees — pressure resumes, it does not reset.
    static uint32_t update(CapitalBayState&, float dt, int liveFighters,
                           float hullFrac, bool enabled);

    static bool bayAlive(const CapitalBayState&, int i);
    static int  aliveBays(const CapitalBayState&);

    // Apply damage to bay i. Returns true exactly when THIS call blew it
    // (hp crossed to <= 0). No-op on a dead bay / bad index / amount <= 0.
    static bool damageBay(CapitalBayState&, int i, int amount);
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
