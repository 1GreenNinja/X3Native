#pragma once
// Combat FX: shot tracers + muzzle flash (gameplay-feel pass).
//
// Game/slice code only — engine/ stays pure. Built from the IRenderDevice +
// Vec3 interfaces only. No id Tech / RBDOOM source consulted.
//
// WHY world-space geometry: tracers + muzzle flash are real world-space boxes
// (the only thing the mesh path can draw is a mesh at a column-major transform):
//   * Tracer: a thin bright box stretched from the muzzle to the hit point (or to
//     max range on a miss), shown for a fraction of a second.
//   * Muzzle flash: a brief bright box at the muzzle.
//
// The crosshair USED to live here as a world-space "+"; as of S7 it moved to the
// new screen-space HUD layer (app/hud.*), which draws a crisp, depth-free 2D
// reticle at the framebuffer center via IRenderDevice::drawHudQuad. CombatFx no
// longer draws a crosshair.

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>
#include <string_view>

namespace x3::con { class IConsole; }   // cvar sync (applyWeaponFxCVars) — fx.cpp includes the real header

namespace x3::game {

// Per-weapon FX look. The WeaponDef carries string FX-id hints (muzzleFx/impactFx);
// the host maps those onto one of these via fxKindFromId() so each gun's muzzle
// flash + impact read distinctly (color/size) instead of one shared generic burst.
//   Default  : the original hot orange-white ballistic look (pistol-grade).
//   Pistol   : the same ballistic look (an explicit alias so the roster is readable).
//   Smg      : leaner/cooler ballistic — fast small sparks (auto bloom).
//   Shotgun  : a WIDE, fat muzzle flash + a broad spark spray (heavy boom).
//   Chaingun : hot + extra-sparky (a busy, glowing auto roar).
//   Plasma   : BLUE energy — cool tint, soft round flash, no metal sparks.
//   Lightning: electric CRACKLE — LIGHT ELECTRIC BLUE, twitchy fast sparks (beam zap).
enum class WeaponFxKind : uint8_t {
    Default = 0,
    Pistol,
    Smg,
    Shotgun,
    Chaingun,
    Plasma,
    Lightning,
    Rocket,      // heavy explosive: big orange launch flash, fireball impact
    // ---- weapon-vfx lane (2026-08): the canon-12 elemental reads. These are the
    // DamageType-keyed impact/bolt consumer rows — the flamethrower/freezeray/napalm
    // used to borrow Rocket/Plasma and read as generic bolts.
    Flame,       // FIRE: orange->red gradient puffs that grow + rise, ignition cone
    Frost,       // ICE: cyan-white crystals that shrink in flight, crystalline burst
    Napalm,      // rocket-class fireball + the burning ground pool on impact
};
// Number of WeaponFxKind rows (keep in sync with the enum above — the per-kind
// live-tuning cvar array in FxTuning is indexed by (int)kind).
constexpr int kWeaponFxKindCount = (int)WeaponFxKind::Napalm + 1;

// Stable lower-case name per kind — used to build the per-kind muzzle-flash cvar
// names ("w_flash_" + name) in ONE place (registration + per-frame sync share it,
// so the two can never drift). nullptr for an out-of-range index.
inline const char* weaponFxKindName(int k) {
    static const char* kNames[kWeaponFxKindCount] = {
        "default", "pistol", "smg", "shotgun", "chaingun",
        "plasma", "lightning", "rocket", "flame", "frost", "napalm" };
    return (k >= 0 && k < kWeaponFxKindCount) ? kNames[k] : nullptr;
}

// Map a WeaponDef FX-id string (e.g. "muzzle_plasma", "impact_bullet") onto a
// WeaponFxKind. Recognizes the substrings the roster uses; anything unknown ->
// Default. Pure + header-inline so both the host and tests can call it.
inline WeaponFxKind fxKindFromId(std::string_view id) {
    auto has = [&](std::string_view s) { return id.find(s) != std::string_view::npos; };
    if (has("plasma"))    return WeaponFxKind::Plasma;
    if (has("lightning")) return WeaponFxKind::Lightning;
    if (has("chaingun"))  return WeaponFxKind::Chaingun;
    if (has("shotgun"))   return WeaponFxKind::Shotgun;
    if (has("smg"))       return WeaponFxKind::Smg;
    if (has("pistol"))    return WeaponFxKind::Pistol;
    // Elemental rows BEFORE rocket: "napalm" ids must not fall through to a
    // generic kind, and "flame"/"freeze"/"frost"/"cryo" are the DamageType-keyed
    // reads (Bio burn -> Flame scorch, Cryo -> Frost crystals).
    if (has("napalm"))    return WeaponFxKind::Napalm;
    if (has("flame"))                                 return WeaponFxKind::Flame;
    if (has("freeze") || has("frost") || has("cryo")) return WeaponFxKind::Frost;
    if (has("rocket") || has("explosion")) return WeaponFxKind::Rocket;
    return WeaponFxKind::Default;
}

// ---- Tracer tuning ----
// How long (seconds) a shot tracer beam stays visible. Long enough to actually
// READ as a streak leaving the barrel (0.06 was ~3 frames -> invisible).
constexpr float kTracerTime      = 0.12f;
// Half-thickness (m) of the tracer beam box (cross-section is 2*thickness). A
// hair-thin beam viewed nearly end-on (it leaves the gun toward the crosshair)
// is invisible; fatten it so the shot clearly comes out of the weapon.
constexpr float kTracerThickness = 0.035f;
// Max concurrent tracers (pool). Excess shots overwrite the oldest slot.
// Concurrent tracer slots. Was 8 — exactly one shotgun volley, and far too few
// for an AREA discharge: THE WATER ZAP (app/waterzap.h) lays down a whole web of
// arcs across the surface at once, and an 8-slot ring recycled them at age 0 so
// every bolt rendered as a 5 m stub (a bolt EXTENDS at kLightningBoltSpeed over
// its life). 64 slots lets a web of arcs coexist and finish propagating; normal
// weapons (1-10 tracers a volley) are unaffected except that rapid fire now keeps
// its older tracers instead of stomping them.
constexpr int   kMaxTracers      = 64;
// Lightning bolt ARC PROPAGATION speed (m/s): the jagged bolt visibly extends from
// the muzzle toward the hit point at this rate rather than snapping full-length the
// instant the (hitscan) beam fires. Director note: the old instant/over-fast read
// was reduced ~39% — this is the tuned, slower travel (0.61x of the prior feel).
// Tuned so a ~28 m max-range beam still completes well within the tracer lifetime
// (kTracerTime): at 300 m/s a 28 m beam fully connects in ~0.093 s (< 0.12 s life),
// so the tip is still visibly travelling yet always reaches the hit point.
constexpr float kLightningBoltSpeed = 300.0f;
// Lightning re-roll period (seconds), the MEAN of a jittered interval. The bolt holds
// a shape for one bucket then JUMPS to a new one. The interval is deliberately NOT
// constant: a fixed 65 ms bucket is a metronome, and a metronome reads as a machine.
// Each re-roll lasts ~kLightningRerollPeriod * [0.6, 1.4] (i.e. ~40-90 ms), so the
// bolt crackles irregularly. Brightness is also re-rolled per bucket (real arcs pulse
// in INTENSITY, not just in shape).
// SIZZLE PASS (Tim, 2026-07-26: "add more sizzle .. a touch faster reroll flicker"):
// nudged 0.065 -> 0.052 so the bolt re-shapes/re-pulses a hair more often — tighter
// electric crackle. Still jittered [0.6,1.4] so it never reads as a metronome.
constexpr float kLightningRerollPeriod = 0.052f;

// ---- FRACTAL BOLT (recursive midpoint displacement) ----
// Real lightning is SELF-SIMILAR: big lazy bends with smaller kinks riding on them,
// and smaller kinks on those. The old algorithm WALKED muzzle->hit in fixed-length
// runs and kicked each joint by a random angle — a uniform step with a uniform kick,
// which is a regular zigzag no matter how you tune it. Tim: "we need natural
// lightning". So the shape is now built the opposite way: start from the single
// straight muzzle->hit segment and SUBDIVIDE, displacing each midpoint perpendicular
// to its own parent segment and halving the displacement at every level.
//
// kLightningFractalDepth: subdivision levels (2^depth segments per strand).
// kLightningDisplaceFrac: initial midpoint displacement as a FRACTION OF BOLT LENGTH —
//   scale-relative, so a 2 m bolt and a 25 m bolt look equally natural (the old
//   fixed-metre step is exactly why short bolts read as coat hangers).
// kLightningDecay: displacement multiplier per level. THIS IS THE NATURALNESS KNOB
//   (the fractal dimension): 0.5 = each halving of length halves the wobble.
constexpr int   kLightningFractalDepth = 6;      // 64 segments on the trunk
constexpr float kLightningDisplaceFrac = 0.06f;  // first midpoint kick ~6% of length (was 0.16, then 0.11 — owner "STILL goes quite wide": ~62% cut so the bolt HUGS the aim line, tight jitter not a wide splay)
constexpr float kLightningDecay        = 0.43f;  // per-level displacement falloff (was 0.55 — lower so finer subdivisions swing LESS, killing the wild secondary loops while the trunk stays jagged)

// ---- BRANCHING ----
// Forks are CHILD BOLTS: they inherit the parent's direction rotated off-axis, take a
// fraction of the remaining parent length, recurse (so branches branch), and inherit
// REDUCED brightness + thickness. Probability decays with depth so the tree thins out.
constexpr int   kLightningMaxBranchDepth = 3;     // a fork can fork, up to this depth
constexpr float kLightningBranchChance   = 0.20f; // per-midpoint chance at depth 0 (was 0.28 — trimmed a notch so forks don't fan the beam out wide; some forking kept)
constexpr float kLightningBranchDecay    = 0.55f; // chance/brightness falloff per level
// Most branches DIE partway instead of reaching anything — a dead-end tendril that
// fades is one of the strongest naturalness cues. Only the TRUNK terminates exactly
// on the hit point.
constexpr float kLightningBranchLenFrac  = 0.38f; // branch length vs remaining parent (was 0.5 — shorter fork stubs so forks hug the trunk instead of reaching off to the walls/ceiling; forking kept, just less lateral reach)
// Core / glow thickness (m): a THIN white-hot core ribbon inside a wider, DIMMER blue
// glow ribbon, both emissive (HDR) so BLOOM builds the halo.
//
// POLISH PASS (landed under honest lighting, 5c35d65): the original 0.035 / 0.10 m
// pair was authored against the OLD washed 0.42-ambient look. Under honest lighting
// the bolt read as a white ASTERISK of fat rectangular planks — you could see the
// quad edges, the core clipped to flat white, and the fork/arc strands were just as
// thick as the trunk. Per docs/DECISIONS.md ("VALUE, NOT LUMENS — don't crank
// emissive until it's a white blob"), the halo now comes from bloom on a SHARP thread
// rather than from wide bright geometry. ~3x thinner; brightness comes down with it.
// PLAYTEST TRIM #2 (Tim, 2026-07-19): still "quite wide.. needs to be constrained a bit."
// Core + glow narrowed a further ~33% (0.007->0.0047, 0.015->0.010) for a tighter, more
// focused bolt. Both scale by the SAME factor so the glow/core RATIO (~2.1x) is preserved
// (glow width = kLightningGlowThick * coreThick/kLightningCoreThick, see drawBoltSegment):
// length, arcing, brightness and the branch/tendril character are untouched — only the
// core WIDTH tightens. Bloom still smears the sharp thread into a halo, so it stays visible.
// POLISH #3 (Tim, 2026-07-26): "slightly thicker beam maybe .. the earlier trim may
// have gone a touch far." Core widened 0.0047 -> 0.0060 (~+28%) — still a tight thread,
// just no longer hairline. Glow tracks the SAME ~2.13x ratio (0.0128) so the corona
// stays proportional (glow width = kLightningGlowThick * coreThick/kLightningCoreThick).
constexpr float kLightningCoreThick = 0.0060f;  // a hair thicker (Tim: trim went a touch far)
constexpr float kLightningGlowThick = 0.0128f;  // halo tracks the core (ratio ~2.13x preserved)

// ---- Electric-arc tendril ring (lightning impact violence) ----
// Short-lived mini zigzag arcs crawling on the surface at a lightning hit point.
constexpr int   kMaxArcs   = 16;      // ring capacity (SIZZLE pass: holds the 9-13 tendril burst)
constexpr float kArcLife   = 0.14f;   // seconds one tendril lives

// ---- Muzzle flash tuning ----
// How long (seconds) the muzzle flash quad stays visible.
constexpr float kMuzzleFlashTime = 0.04f;
// Half-extent (m) of the muzzle flash box.
constexpr float kMuzzleFlashSize = 0.05f;

// ---- In-flight BOLT style (weapon-vfx lane) --------------------------------
// The per-kind look of a TRAVELLING projectile core (what boltFx drops each frame).
// Factored out of boltFx into a queryable table row so the flame/frost reads are
// testable headlessly (--test-weapons asserts the params are in range) instead of
// living as literals inside a render-path switch. Colors are linear HDR (feed the
// additive bloom chain); sizes are billboard half-extents in meters.
struct BoltStyle {
    float r, g, b;          // core tint at BIRTH
    float r1, g1, b1;       // core tint at DEATH (gradient over the core's life;
                            // == birth for every legacy kind, so nothing re-reads)
    float coreSize;         // core half-extent at birth (m)
    float endScale;         // core half-extent multiplier at death (legacy 0.7;
                            // FIRE grows >1, ICE shrinks <1)
    float rise;             // upward drift (m/s) given to the core (fire rises)
    float life;             // core lifetime (s)
};
inline BoltStyle boltStyleFor(WeaponFxKind k) {
    switch (k) {
        case WeaponFxKind::Plasma:    return { 0.5f, 1.9f, 6.0f,  0.5f, 1.9f, 6.0f,  0.16f, 0.7f, 0.0f, 0.06f }; // blue-cyan
        case WeaponFxKind::Rocket:
        case WeaponFxKind::Napalm:    return { 6.0f, 2.2f, 0.5f,  6.0f, 2.2f, 0.5f,  0.20f, 0.7f, 0.0f, 0.06f }; // orange fire shell
        case WeaponFxKind::Lightning: return { 0.9f, 2.4f, 5.0f,  0.9f, 2.4f, 5.0f,  0.12f, 0.7f, 0.0f, 0.06f }; // light electric blue
        // FIRE: hot orange core that COOLS to deep red as it dies, GROWS (a flame
        // front expands) and DRIFTS UPWARD (heat rises — matches the weapon's own
        // -8 m/s^2 projectileGravity, which is data for the same truth).
        case WeaponFxKind::Flame:     return { 5.0f, 1.9f, 0.35f, 1.9f, 0.30f, 0.06f, 0.15f, 1.9f, 0.9f, 0.22f };
        // ICE: cyan-white crystal that pales toward white and SHRINKS slightly
        // over its life (a crystal sublimating, not a flame front).
        case WeaponFxKind::Frost:     return { 1.3f, 3.4f, 6.2f,  2.6f, 4.6f, 7.0f,  0.11f, 0.55f, 0.0f, 0.10f };
        default:                      return { 5.0f, 3.4f, 1.0f,  5.0f, 3.4f, 1.0f,  0.13f, 0.7f, 0.0f, 0.06f }; // hot yellow
    }
}

// ---- Per-kind muzzle-flash tuning (weapons-tuning lane) --------------------
// Formerly a file-local table in fx.cpp; hoisted here (like boltStyleFor) so the
// headless --test-weapons gate can assert the rows directly. Returns a tint
// (linear HDR), per-spark count + size + speed multipliers, and a soft-flash
// tint/scale, so each weapon's flash reads distinctly.
//
// flashScale (NEW, Tim live-play 2026-08-16) is the SIZE scale of the whole
// muzzle read (spark billboards + the soft flash sprite). Because the sparks are
// ADDITIVE and accumulate at the barrel tip, perceived flare energy goes with
// AREA (~flashScale^2):
//   * Lightning 0.05 — "big muzzle flare ... could be 95% reduced": the arc is
//     the read; the accumulated blue spark cone was a headlight in front of it.
//   * Pistol 0.5 — "muzzle flashes ... can be 50% of what they are now in size".
// Live per-kind multipliers stack on top via the w_flash_<kind> cvars (FxTuning).
struct MuzzleStyle {
    float sparkR, sparkG, sparkB;     // spark tint (linear HDR -> bloom)
    float flashR, flashG, flashB;     // soft-flash sprite tint
    int   sparkCount;                 // number of cone sparks
    float sizeMul;                    // spark + flash size multiplier
    float speedMul;                   // spark cone speed multiplier
    float coneJitter;                 // lateral spark spread (m/s)
    float flashSize;                  // soft-flash half-extent (m) at birth
    float flashScale;                 // overall muzzle-flash SIZE scale (1 = legacy)
};
inline MuzzleStyle muzzleStyleFor(WeaponFxKind k) {
    switch (k) {
        case WeaponFxKind::Smg:       // leaner/cooler, many small fast sparks
            return { 4.5f, 3.0f, 1.2f,  5.5f, 3.8f, 1.8f, 12, 0.8f, 1.15f, 2.0f, 0.22f, 1.0f };
        case WeaponFxKind::Shotgun:   // WIDE fat boom: big flash, broad spray
            return { 6.0f, 3.6f, 1.0f,  7.5f, 4.6f, 1.6f, 16, 1.6f, 1.0f,  3.6f, 0.52f, 1.0f };
        case WeaponFxKind::Chaingun:  // hot + extra-sparky (busy auto roar)
            return { 6.0f, 3.0f, 0.7f,  7.0f, 4.0f, 1.2f, 18, 0.95f,1.25f, 2.6f, 0.30f, 1.0f };
        case WeaponFxKind::Plasma:    // BLUE energy: soft round flash, no metal sparks
            return { 0.8f, 2.4f, 6.0f,  1.2f, 3.0f, 7.0f,  8, 1.2f, 0.85f, 1.4f, 0.40f, 1.0f };
        case WeaponFxKind::Lightning: // electric crackle: LIGHT BLUE, twitchy fast.
                                      // flashScale 0.05 = Tim's 95% flare cut — the
                                      // accumulated additive spark cone was a headlight
                                      // swamping the arc (the arc is untouched).
            return { 1.8f, 4.0f, 7.2f,  2.0f, 4.5f, 7.5f, 9, 0.55f, 1.5f,  3.2f, 0.26f, 0.05f };
        case WeaponFxKind::Flame:     // IGNITION CONE: a fat orange flash + slower, wide,
                                      // larger tongues leaving the nozzle (fuel catching,
                                      // not a gunshot crack)
            return { 5.0f, 2.0f, 0.45f, 6.0f, 2.6f, 0.7f, 14, 1.5f, 0.75f, 2.8f, 0.34f, 1.0f };
        case WeaponFxKind::Frost:     // icy discharge: cyan-white, small tight cone
            return { 1.5f, 3.5f, 6.5f,  2.0f, 4.2f, 7.0f,  8, 0.9f, 0.9f,  1.6f, 0.30f, 1.0f };
        case WeaponFxKind::Napalm:    // heavy launcher pop (rocket-class, warmer)
            return { 5.5f, 2.4f, 0.6f,  6.5f, 3.2f, 1.0f, 12, 1.3f, 1.0f,  2.4f, 0.40f, 1.0f };
        case WeaponFxKind::Pistol:    // hot orange-white ballistic look at HALF size
                                      // (Tim live-play: "muzzle flashes ... 50% of what
                                      // they are now in size" — the stream-of-bullets read)
            return { 5.0f, 3.2f, 1.0f,  6.0f, 4.0f, 1.6f, 10, 1.0f, 1.0f,  2.0f, 0.28f, 0.5f };
        case WeaponFxKind::Default:
        default:                      // original hot orange-white ballistic look
            return { 5.0f, 3.2f, 1.0f,  6.0f, 4.0f, 1.6f, 10, 1.0f, 1.0f,  2.0f, 0.28f, 1.0f };
    }
}

// ---- LIVE WEAPON-FX TUNING (Tim: tune the read by eye, in play) ------------
// One mutable singleton of the live-tunable weapon-FX dials, written per frame
// from the w_* cvars (applyWeaponFxCVars) and read by CombatFx. Defaults are the
// shipped look — a fresh process with no cvar edits renders bit-identically to
// the constants above.
struct FxTuning {
    // w_lightning_thickness: scale on the lightning arc core (glow tracks the
    // core at its fixed ~2.13x ratio, and the impact arc tendrils track too).
    float lightningThickness = 1.0f;
    // w_tracer_len / w_tracer_speed: the travelling BULLET STREAK (a short bright
    // window that runs muzzle->hit at tracerSpeed, replacing the old full-length
    // "phaser beam" line for hitscan bullet weapons).
    float tracerLen   = 2.5f;    // streak length (m)
    // 160 m/s: fast enough to read as a BULLET (not a floating paintball), slow
    // enough that the streak is on screen for ~0.35 s across a 55 m pistol shot —
    // so held fire shows SEVERAL rounds in flight at once, which is the "stream of
    // bullets" read Tim asked for. --test-weapons WT4 bounds the concurrency.
    float tracerSpeed = 160.0f;  // streak travel speed (m/s)
    // w_flash_<kind>: per-kind LIVE multiplier stacked on the MuzzleStyle
    // flashScale row (1 = the shipped row value).
    float flashKind[kWeaponFxKindCount] = { 1,1,1,1,1,1,1,1,1,1,1 };
};
FxTuning& fxTuning();   // the live tuning state (mutable singleton; fx.cpp)

// Per-frame cvar sync: read w_lightning_thickness / w_tracer_len / w_tracer_speed /
// w_flash_<kind> from the console into fxTuning(), clamped to sane ranges. A cvar
// that is NOT registered on this console (getString empty) leaves the current
// value untouched — bare hosts without the engine cvar catalog stay at defaults.
void applyWeaponFxCVars(const x3::con::IConsole& console);

// ---- GPU-instanced particle pool (combat juice) ----
// Bounded CPU-simulated, GPU-instanced billboard pool. The CPU integrates each
// live particle (pos/vel/gravity/drag/life/fade) into a FIXED ring (no per-frame
// heap alloc) and submits the live ones to the device as camera-facing billboards
// each frame (additive for sparks/fire/muzzle, alpha for smoke/dust/blood). The
// device draws them in the HDR pass before bloom (bright sparks glow) with a soft
// depth fade against the scene depth. Spawned ONLY from combat events, so the pool
// is empty (zero GPU cost) until a weapon fires / something is hit / dies.
constexpr int kMaxParticles = 4096;   // pool capacity (bounded; oldest recycled)

// ---- Impact decal ring ----
// Bounded ring of impact decals (bullet holes / scorch marks). Each is an oriented
// quad laid on the hit surface at the raycast hit point + normal; the oldest is
// recycled when full. Persistent until recycled or faded out over its lifetime.
constexpr int   kMaxDecals    = 64;     // ring capacity (oldest recycled)
constexpr float kDecalLife    = 12.0f;  // seconds before a decal fully fades

// Combat FX system. Owns a couple of shared unit box meshes (created in init,
// destroyed in shutdown) and a small pool of active tracers / a muzzle flash
// timer. Self-contained: no Scene/physics state.
class CombatFx {
public:
    // Create the shared meshes (a unit box). Call once after the device is up.
    void init(x3::rhi::IRenderDevice& device);

    // Register a shot beam from `from` (the muzzle) to `to` (the hit point, or
    // eye + dir*range on a miss). Also lights the muzzle flash at `from` AND spawns
    // the muzzle-flash particle burst (a few hot additive sparks at the muzzle).
    // `kind` tints/shapes the beam: Lightning renders as a jagged white-cyan bolt
    // (re-randomized each frame); everything else is the straight hot-yellow tracer.
    // `widthOverride` (m, full ribbon width; 0 = the on-foot kTracerThickness
    // default): SPACE bolts pass ~0.5 m — the 0.035 m rifle tracer is sub-pixel
    // at dogfight range (a 10 m hull judged from 60-300 m).
    // `carrierVel` (m/s, optional): the SHOOTER's velocity at the instant of the
    // shot. The beam then RIDES it — both endpoints advance by carrierVel*dt
    // every frame of the tracer's life, so a bolt fired from a moving ship stays
    // attached to the muzzle instead of hanging in world space while the hull
    // slides out from under it.
    //   OWNER BUG (2026-08, space): "the laser blaster plasma whatever weapon
    //   beams do not come from the ship when its strafing, they come beside it".
    //   The muzzle origin was already correct and current-frame; the beam is
    //   simply a 0.12 s world-anchored segment, so at 110 m/s of lateral thrust
    //   the ship travels 13 m — nearly two hull lengths — before it fades. It was
    //   always wrong; it only became visible once the ship could genuinely strafe.
    //   (This also rules out the one-frame-lag hypothesis, which would predict a
    //   fixed velocity*dt ~= 0.7 m gap rather than one that GROWS as the bolt ages.)
    // nullptr / omitted == a stationary shooter == byte-identical to the previous
    // behaviour, which is what every on-foot caller wants.
    void addTracer(const x3::phys::Vec3& from, const x3::phys::Vec3& to,
                   WeaponFxKind kind = WeaponFxKind::Default,
                   float widthOverride = 0.0f,
                   const x3::phys::Vec3* carrierVel = nullptr);

    // ---- Combat-event particle/decal presets (the juice) -------------------
    // Each spawns a tuned burst into the bounded pool / decal ring. Called from the
    // existing combat hooks (weapon fire, melee, monster hit/death). `dir` is the
    // shot/impact direction (need not be unit); `normal` is the surface normal.

    // Muzzle flash burst at the muzzle, biased along the fire `dir` (additive). The
    // default overload keeps the original hot-orange ballistic look; the `kind`
    // overload tints + scales the burst per weapon (plasma blue, shotgun wide,
    // lightning crackle, ...) so each gun reads distinctly. The default forwards to
    // kind=Default, so existing callers (melee/monster) are unchanged.
    void spawnMuzzleFlash(const x3::phys::Vec3& pos, const x3::phys::Vec3& dir);
    void spawnMuzzleFlash(const x3::phys::Vec3& pos, const x3::phys::Vec3& dir, WeaponFxKind kind);
    // Bullet hit on a hard surface: a cone of additive sparks + an alpha dust puff,
    // sprayed back along the surface `normal`. Also drops a scorch DECAL at the hit.
    // The `kind` overload tints the sparks per weapon (plasma = blue energy splash,
    // lightning = white-cyan, etc.); the default keeps the original metal-spark look.
    void spawnImpact(const x3::phys::Vec3& pos, const x3::phys::Vec3& normal);
    void spawnImpact(const x3::phys::Vec3& pos, const x3::phys::Vec3& normal, WeaponFxKind kind);
    // PROJECTILE BOLT visual (playtest: plasma read hitscan-looking — bolts were
    // INVISIBLE in flight). Call once per frame per live projectile: drops a hot
    // additive core billboard at the bolt position + a dimmer trail speck behind it
    // (60 fps of overlapping cores reads as a continuous glowing bolt with a fading
    // tail). Rocket additionally puffs alpha smoke so the exhaust trail lingers.
    void boltFx(const x3::phys::Vec3& pos, const x3::phys::Vec3& vel, WeaponFxKind kind);
    // Hit on an enemy: a short spray of dark-red alpha blood along the shot `dir`.
    void spawnBlood(const x3::phys::Vec3& pos, const x3::phys::Vec3& dir);
    // Enemy death: a burst of debris chunks (alpha, gravity) + a lingering smoke
    // puff so the kill reads on screen.
    void spawnDeath(const x3::phys::Vec3& pos);
    // EXPLOSION fireball (playtest "barrels look like red boxes" fix): a bright
    // ADDITIVE orange/yellow fireball burst + dark smoke at `center`, sized by
    // `radius`. Hot additive cores feed bloom so a shot barrel reads as a violent
    // fireball, not just scattered red chunks. Used by the barrel FX sink.
    void spawnExplosion(const x3::phys::Vec3& center, float radius);
    // Lingering smoke puff (alpha, slow rise) — used by death + as a generic cue.
    void spawnSmoke(const x3::phys::Vec3& pos);
    // ---- NAPALM FIRE-POOL visual (weapon-vfx lane) -------------------------
    // Renders one live burning ground pool for THIS frame: dt-SCALED (house rule)
    // probabilistic emission of licking flame billboards (orange->red gradient,
    // grow + rise), popping embers and low black smoke across the pool disc at
    // `center`/`radius`. The flicker is intrinsic (jittered counts/sizes/lifetimes).
    // Call once per live pool per frame while the pool burns; the pool's LOGIC
    // (damage ticks, expiry, the water rule) lives in FirePoolSystem (weapon.h) —
    // this is only the look. Steam variant: `extinguishFx` is the one-shot white
    // puff for a napalm shell that lands in WATER and never ignites.
    void firePoolFx(const x3::phys::Vec3& center, float radius, float dt);
    void extinguishFx(const x3::phys::Vec3& pos);

    // ---- SHIP-SCALE damage-state FX (space combat readability) -------------
    // The on-foot presets above are sized for a 2 m humanoid at 5-20 m; a
    // wounded FIGHTER is a ~10 m hull judged from 60-150 m, so these are the
    // same primitives scaled up ~5x, zero-gravity (space), and velocity-aware
    // (the puff inherits a fraction of the ship's velocity so the trail STREAMS
    // behind the flight path instead of hanging in a bead chain). Staging —
    // which of these fires, how often — is the pure shipai::damageFxProfile.
    // Spark burst at the hull (additive, no decal — nothing to scorch in vacuum).
    void spawnShipSparks(const x3::phys::Vec3& pos);
    // One grey smoke puff of the trail. heavy01: 0 = thin wisp (<50% hull),
    // 1 = churning black-grey (<25%). `vel` = the ship's velocity.
    void spawnShipSmoke(const x3::phys::Vec3& pos, const x3::phys::Vec3& vel, float heavy01);
    // Hot ember/fire glow licking the hull (<25% — the "burning" read).
    void spawnShipEmber(const x3::phys::Vec3& pos, const x3::phys::Vec3& vel);
    // Ship-scale muzzle flash at a wing hardpoint (the on-foot 0.05 m flash is
    // invisible from a chase camera): one bright core + a short spray along
    // the fire direction, so the bolt visibly LEAVES the ship.
    // `carrierVel` (optional): the shooter's velocity, ADDED to every spawned
    // particle so the flash rides the hull instead of being left behind — the
    // particle half of the strafing-beam bug (see addTracer). nullptr == the
    // previous stationary behaviour.
    void spawnShipMuzzle(const x3::phys::Vec3& pos, const x3::phys::Vec3& dir,
                         const x3::phys::Vec3* carrierVel = nullptr);
    // SHIP DISINTEGRATION blast (space-combat power fantasy): a MASSIVE one-shot
    // kill burst — a dense hot fireball (scaled up ~5x vs spawnExplosion, more
    // cores), a huge white-hot central FLASH that blooms hard for a couple frames,
    // and an expanding SHOCKWAVE shell of fast bright specks flung radially outward.
    // Zero-gravity (space). Pair with spawnDeath/spawnSmoke (near-field chunks +
    // plume) + a GPU debris burst (the flung hull fragments) for the full kill.
    // `radius` sizes the whole event (a ~10 m fighter uses ~9 m).
    void spawnShipDeathBlast(const x3::phys::Vec3& center, float radius);
    // Drop a scorch decal directly (bullet-hole / impact mark) at a hit point+normal.
    void addDecal(const x3::phys::Vec3& pos, const x3::phys::Vec3& normal);

    // Advance the FX timers (tracer lifetimes, muzzle flash) AND simulate the
    // particle pool (integrate pos/vel/gravity/drag/life) + age the decals. No-op
    // at dt <= 0 for the timer decay; the sim is integrated each call.
    void update(float dt);

    // Draw the crosshair (always, at screen center in front of the camera) and
    // any active tracers + muzzle flash. `eye` + camera `yaw`/`pitch` build the
    // camera basis for the crosshair. Call AFTER scene/viewmodel/monster draws.
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
              float eyeX, float eyeY, float eyeZ, float yaw, float pitch) const;

    // Submit the live particles (camera-facing billboards) + decals to the device
    // for THIS frame. Call between beginFrame/endFrame, after draw(). No-op when
    // the pool + decal ring are empty (zero GPU cost when idle).
    void submit(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const;

    // Live particle count (for --bench reporting / debug).
    int liveParticleCount() const;

    // Destroy the shared meshes. Call once on exit (no VMA leaks).
    void shutdown(x3::rhi::IRenderDevice& device);

private:
    struct Tracer {
        x3::phys::Vec3 from{};
        x3::phys::Vec3 to{};
        float          life = 0.0f;  // remaining seconds; <= 0 means free slot
        float          age  = 0.0f;  // seconds since spawn (Lightning bolt propagation)
        float          width = 0.0f; // full ribbon width override (0 = default)
        x3::phys::Vec3 vel{};        // shooter velocity the beam rides (0 = world-anchored)
        WeaponFxKind   kind = WeaponFxKind::Default;  // Lightning -> jagged bolt
    };

    // Draw one bright box stretched/oriented along the segment a->b with the
    // given half-thickness (cross-section), tinted by `color`.
    void drawBeam(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                  const x3::phys::Vec3& a, const x3::phys::Vec3& b,
                  float thickness, const float color[4]) const;

    // Draw a tracer as a thin CAMERA-FACING ribbon (billboard quad) from a->b
    // (playtest "chaingun fires a square rod" fix). The quad's WIDTH axis is
    // perpendicular to BOTH the segment direction and the eye->segment view
    // direction, so it always faces the camera and reads as a flat bright streak
    // rather than drawBeam's world-fixed square cross-section box. `width` is the
    // full ribbon width; the depth axis is collapsed flat. Falls back to a thin
    // beam when the segment points straight at the eye (degenerate width axis).
    void drawTracerBillboard(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                             const x3::phys::Vec3& a, const x3::phys::Vec3& b,
                             const x3::phys::Vec3& eye, float width,
                             const float color[4]) const;

    // Deterministic xorshift32 PRNG, threaded by REFERENCE through the whole recursive
    // bolt build so one `seed` reproduces one exact bolt (const-safe: it never touches
    // m_rng, which the headless captures depend on being repeatable).
    struct BoltRng {
        uint32_t s;
        float operator()() {                 // [0,1)
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            return (float)(s & 0x00FFFFFFu) / (float)0x01000000u;
        }
        float sym() { return (*this)() * 2.0f - 1.0f; }   // [-1,1)
    };

    // Draw a NATURAL FRACTAL lightning bolt a->b (Tim: "we need natural lightning").
    // Recursive midpoint displacement: the single straight a->b segment is subdivided
    // kLightningFractalDepth times, each midpoint kicked perpendicular to its own
    // parent segment, with the displacement decaying by kLightningDecay per level. The
    // result is SELF-SIMILAR — big lazy bends carrying smaller kinks carrying smaller
    // kinks — instead of the old uniform-step/uniform-kick walk, which was a regular
    // zigzag at every zoom level. Branches are recursive CHILD bolts (they fork too),
    // inheriting reduced brightness + thickness, and mostly dying partway. `a` and `b`
    // are EXACT (muzzle and hit point); only branches are allowed to end nowhere.
    // The whole shape is deterministic from `seed` (bucketed by the caller so the bolt
    // holds a shape then jumps, rather than strobing a new one every frame).
    void drawLightningBolt(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                           const x3::phys::Vec3& a, const x3::phys::Vec3& b,
                           const x3::phys::Vec3& eye,
                           float coreThick, uint32_t seed, float brightness) const;

    // One recursive strand: subdivide a->b, emitting camera-facing ribbon pairs at the
    // leaves and spawning branch children off the midpoints. `displace` is the current
    // midpoint kick magnitude (metres), `depth` the remaining subdivision levels,
    // `branchDepth` how many forks deep this strand already is. `t0`/`t1` are the
    // strand-relative positions of a/b, used to TAPER thickness+brightness toward the
    // tip (a branch that ends as thick as it started reads fake — nature tapers).
    void boltSubdivide(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                       const x3::phys::Vec3& a, const x3::phys::Vec3& b,
                       const x3::phys::Vec3& eye, BoltRng& rng,
                       float displace, int depth, int branchDepth,
                       float coreThick, float brightness, float t0, float t1) const;
    // Draw one straight zigzag SEGMENT as a camera-facing GLOW ribbon + a thinner
    // white-hot CORE ribbon, both via drawMeshEmissive (HDR emissive -> bloom halo).
    void drawBoltSegment(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                         const x3::phys::Vec3& a, const x3::phys::Vec3& b,
                         const x3::phys::Vec3& eye,
                         float coreThick, float brightness) const;

    // A centered unit box mesh (half-extent 0.5 each axis) reused for all FX,
    // scaled per draw via the model matrix.
    x3::rhi::MeshHandle m_box;

    Tracer m_tracers[kMaxTracers];
    int    m_nextTracer = 0;        // round-robin write cursor into the pool

    // ---- Electric arc-tendril pool (lightning IMPACT violence) -------------
    // Short-lived mini zigzag arcs that crawl/whip off a lightning hit point (Tim:
    // impacts must be sharp electric streaks + arc tendrils, NOT white puffballs).
    // Each is drawn as a tiny re-rolled zigzag via drawLightningBolt so it crackles.
    struct Arc {
        x3::phys::Vec3 base{};   // hit point
        x3::phys::Vec3 dir{};    // tendril direction (unit) * length baked into tip
        float          len  = 0.6f;
        float          life = 0.0f;   // remaining seconds (<=0 == free)
        float          maxLife = kArcLife;
        uint32_t       seed = 0;
    };
    Arc  m_arcs[kMaxArcs];
    int  m_nextArc = 0;
    // Spawn a ring of arc tendrils whipping off a lightning hit (called by
    // spawnImpact for the Lightning kind).
    void spawnArcs(const x3::phys::Vec3& pos, const x3::phys::Vec3& normal);

    x3::phys::Vec3 m_muzzlePos{};
    float          m_muzzleFlash = 0.0f;  // remaining seconds

    // ---- CPU particle pool (bounded; GPU-instanced billboards) -------------
    // One slot per particle. `life <= 0` means a free slot. Each frame update()
    // integrates the live ones and submit() streams them to the device. The spawn
    // helpers find a free slot (or recycle the oldest) — no per-frame heap alloc.
    struct Particle {
        x3::phys::Vec3 pos{};
        x3::phys::Vec3 vel{};
        float life     = 0.0f;     // remaining seconds (<=0 == free)
        float maxLife  = 1.0f;     // for the fade curve
        float size0    = 0.1f;     // half-extent at birth (m)
        float size1    = 0.1f;     // half-extent at death (m)
        float r = 1, g = 1, b = 1; // linear RGB (scaled by intensity at birth)
        // Optional END color (weapon-vfx lane): when r1 >= 0 the submitted color
        // LERPS birth->death over the particle's life — the fire read's orange->red
        // cooling gradient. Default -1 = no gradient (legacy particles unchanged;
        // submit() reads r/g/b exactly as before).
        float r1 = -1.0f, g1 = -1.0f, b1 = -1.0f;
        float a0       = 1.0f;     // opacity at birth
        float gravity  = 0.0f;     // * world gravity (m/s^2 along -Y)
        float drag     = 0.0f;     // per-second velocity damping (0 = none)
        bool  additive = true;     // additive (sparks) vs alpha (smoke/blood)
    };
    Particle m_particles[kMaxParticles];
    int      m_nextParticle = 0;   // round-robin recycle cursor

    // Spawn one particle into a free/recycled slot (returns the slot index).
    int  spawnParticle(const Particle& p);
    // Small fast deterministic PRNG (xorshift) for spawn jitter — no <random>
    // churn, repeatable in headless captures.
    uint32_t m_rng = 0x1234567u;
    float frand();                 // [0,1)
    float frandSym();              // [-1,1)

    // ---- Impact decal ring (oriented quads on hit surfaces) ----------------
    struct Decal {
        x3::phys::Vec3 center{};
        x3::phys::Vec3 normal{ 0, 1, 0 };
        float halfSize = 0.1f;
        float angle    = 0.0f;     // spin about the normal (rad)
        float life     = 0.0f;     // remaining seconds (<=0 == free)
        float maxLife  = kDecalLife;
        // Decal tint (linear rgb). Default == dark scorch (bullet hole); blood pools
        // override it to dark red. Submit multiplies the lifetime fade into alpha.
        float color[3] = { 0.02f, 0.015f, 0.01f };
    };
    Decal m_decalsRing[kMaxDecals];
    int   m_nextDecal = 0;         // round-robin recycle cursor

    // Per-frame submit scratch (member-owned so submit() does NO heap/stack alloc
    // of the big instance arrays; mutable so the const submit() can fill them).
    mutable x3::rhi::IRenderDevice::ParticleInstance m_scratchAdd[kMaxParticles];
    mutable x3::rhi::IRenderDevice::ParticleInstance m_scratchAlpha[kMaxParticles];
    mutable x3::rhi::IRenderDevice::DecalInstance     m_scratchDecal[kMaxDecals];
};

} // namespace x3::game
