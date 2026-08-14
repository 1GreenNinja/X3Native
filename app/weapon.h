#pragma once
// Weapon pickup + first-person viewmodel (S5).
//
// Game/slice code only — engine/ stays pure. Builds on S2 (Scene/Entity/Tag),
// S3 (Player camera yaw/pitch + eye position), the M2 model loader (load the
// purchased WeaponEnergyPistol.glb -> per-primitive drawables) and the M2/D5
// asset source (mount the loose model dir). This is the first real purchased GLB
// used in gameplay.
//
// Flow:
//   * buildWeaponPickup() loads the weapon GLB (fallback: a procedural box if the
//     file/load fails), uploads its drawables via the device, and places a
//     bobbing/spinning pickup Entity (Tag::Weapon) in the room.
//   * Each frame, update() bobs+spins the pickup, then runs ARMING: if the player
//     is within kPickupRadius of the pickup AND not yet armed, set hasWeapon=true
//     and hide the pickup. Once armed it stays armed (no un-pickup).
//   * Once armed, drawViewmodel() draws the same drawables at a camera-relative
//     transform (lower-right of the view) so the weapon reads as held.
//
// Design choices (documented for the slice):
//   * Distance check, not a physics trigger. A radius test on the player's feet
//     vs. the pickup center is simpler + robust for the slice. The M3 trigger
//     system (setTriggerCallback) is the "proper" future path.
//   * Viewmodel is depth-tested in world space (no dedicated viewmodel depth
//     pass), so it can clip into nearby walls. Acceptable for the slice.
//   * makeDrawables() bakes each glTF node's world TRS into the drawable
//     (nodeTransform); drawWeaponAt multiplies it in (model * nodeTransform), so
//     multi-node / Y-up-corrected GLBs place correctly (M2 node-TRS fix).
//   * Audio (pickup chime / gunshot) is DEFERRED — no audio system until M9.

#include "scene.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/asset/IModelLoader.h"
#include "engine/asset/IAssetSource.h"
#include "engine/core/x3_damage.h"   // DamageType (per-weapon canon-aliens Adaptive-Hide tag)

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace x3::game {

// Radius (meters) within which walking into the pickup arms the player. Measured
// player-feet (camera/eye XZ) to pickup center.
constexpr float kPickupRadius = 1.2f;

// Default first-person viewmodel pose. These seed the live-tunable console cvars
// (vm_yaw/vm_pitch/vm_roll in DEGREES, vm_fwd/vm_right/vm_down in METERS) and are
// the values baked from play-testing. Orientation defaults are in DEGREES here so
// main.cpp can register the cvars without converting. The pistol GLB's barrel
// reads "to the right" with a plain camera-basis orientation, so a -90 deg yaw
// swings it FULLY to forward = traditional point-to-crosshair. Dial vm_* live to converge.
//
// ===========================================================================
// WHAT fwd/right/down MEASURE — read this before re-tuning (Tim 2026-08, live
// play on ceb3c48: "the gun is in your face way over your head").
// ===========================================================================
// They position the weapon's BARREL LINE — the point (0, vmMuzzle.y, 0) in the
// GLB's scene space — relative to the eye. They used to position the GLB's own
// ORIGIN, and EVERY purchased weapon GLB in this roster is authored STANDING ON
// THE FLOOR: its origin is the ground plane under the gun and its barrel is the
// TOP of the model (scene-space y 0.37 .. 0.68, i.e. 0.15 .. 0.25 m once the
// viewmodel scale is applied). Pushing that origin 0.35 m "down" therefore left
// the BARREL only ~0.08 m below eye level — the sights landed ON the crosshair
// and the gun's mass filled the middle of the frame. That is the bug Tim saw,
// and it also made every gun sit at a DIFFERENT height (each model's barrel is a
// different distance above its floor plane). Anchoring on the barrel line makes
// `down` mean exactly what its name says, for every weapon, and normalizes the
// roster. See Arsenal::currentViewmodelFrame.
constexpr float kVmDefYawDeg   = 193.0f; // yaw about camera up (degrees) — barrel -> forward (Tim-tuned)
constexpr float kVmDefPitchDeg = 7.0f;   // pitch about camera right (degrees) (Tim-tuned)
constexpr float kVmDefRollDeg  = 0.0f;   // roll about camera forward (degrees)
constexpr float kVmDefFwd      = 0.86f;  // barrel line ahead of the eye (meters)
constexpr float kVmDefRight    = 0.30f;  // barrel line right of the eye (meters)
constexpr float kVmDefDown     = 0.30f;  // barrel line BELOW the eye line (meters)

// ---- FP viewmodel LENS (vm_fov) -------------------------------------------
// The engine draws the viewmodel with the WORLD's projection (one frame-UBO
// viewProj; shaders/mesh.vert has no per-draw projection), so there is no second
// camera to give the gun its own FOV. This reproduces one the honest way: at a
// fixed forward distance, rendering an object under fovV instead of fovW is
// equivalent to magnifying its SIZE and its OFF-AXIS offsets by
//     k = tan(fovW/2) / tan(fovV/2)
// which is exactly what a narrower viewmodel FOV does to the on-screen read.
// (What it does NOT reproduce is the perspective INSIDE the gun — a real narrow
// viewmodel FOV also flattens the barrel's own foreshortening. Stated plainly so
// nobody mistakes this for a second render pass.)
//   kVmDefFovDeg <= 0  -> k = 1, the gun shares the world lens (default).
//   kVmWorldFovDeg     -> the world FOV to measure against; the live game camera
//                         renders at 60 (app_run: device->setCamera(..., 60.0f)),
//                         the still-capture hosts at 70.
constexpr float kVmDefFovDeg   = 0.0f;   // 0/<=0 = share the world lens
constexpr float kVmWorldFovDeg = 60.0f;  // the live loop's vertical FOV

// Global "hold it up bigger" multiplier folded onto every weapon's vmScale when the
// FP viewmodel is composed (Tim: 2x is CORRECT, keep it). Public because the MUZZLE
// solve must use the SAME scale the gun is DRAWN at — see WeaponDef::vmMuzzle.
constexpr float kVmScaleBoost  = 2.0f;

// Pure arming rule, factored out so it is testable headlessly (see
// runPickupSelfTest). Given the player position, the pickup position and the
// current armed flag, returns true iff the player should become armed THIS call
// (i.e. in-range AND not already armed). Latching ("once armed, stays armed") is
// the caller's job: only flip false->true, never true->false.
//
// `playerPos`/`pickupPos` are world positions; only the horizontal (x,z) plane
// is used so standing under/over the pickup at a different height still arms.
bool shouldArm(const x3::phys::Vec3& playerPos, const x3::phys::Vec3& pickupPos,
               float radius, bool alreadyArmed);

// Weapon pickup + viewmodel system. Owns the loaded model (keeps the loader +
// Model alive so the GPU handles in the drawables stay valid for the app's
// lifetime) and the gameplay state (hasWeapon).
class WeaponSystem {
public:
    // Build the pickup: load WeaponEnergyPistol.glb from `modelDir` via a fresh
    // IAssetSource + the M2 model loader, upload its drawables through `device`,
    // and add a bobbing/spinning Tag::Weapon Entity at `pickupPos` to `scene`.
    // On load failure, a procedural box "weapon" is used instead. Logs which
    // path (real GLB vs. fallback box) was taken. Call once.
    void buildWeaponPickup(Scene& scene, x3::rhi::IRenderDevice& device,
                           std::string_view modelDir, const x3::phys::Vec3& pickupPos);

    // Advance one frame: bob + spin the pickup (if not yet picked up), then run
    // arming against `playerPos`. Flips hasWeapon false->true + hides the pickup
    // the frame the player first enters kPickupRadius. `playerPos` is the
    // player's eye/feet world position (horizontal distance is what matters).
    void update(float dt, Scene& scene, const x3::phys::Vec3& playerPos);

    // Draw the bobbing/spinning pickup for this frame (all model primitives at
    // the pickup Entity's current transform). No-op once picked up / hidden. The
    // pickup Entity itself carries an invalid mesh so Scene::render skips it and
    // this system is the single source of truth for the multi-primitive model.
    // Call alongside scene.render() each frame.
    void drawPickup(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                    const Scene& scene) const;

    // Draw the weapon as a first-person viewmodel for this frame. No-op unless
    // hasWeapon. Places the drawables at a camera-relative transform built from
    // the eye position + look angles (lower-right of the view). Call AFTER
    // scene.render() each frame so it composites on top of the world.
    //
    // The viewmodel pose is fully caller-supplied so it can be live-tuned from
    // the console (cvars vm_yaw/vm_pitch/vm_roll in RADIANS about the camera
    // basis, and vm_fwd/vm_right/vm_down in METERS along the camera basis). The
    // purchased pistol GLB's authored axes are mismapped, so these let the player
    // dial the barrel onto the look direction without recompiling. These have NO
    // gameplay effect — the fire ray uses the camera look dir, not the viewmodel.
    void drawViewmodel(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                       float eyeX, float eyeY, float eyeZ, float yaw, float pitch,
                       float yawOff, float pitchOff, float rollOff,
                       float fwd, float right, float down) const;

    // Gameplay state. S6 reads this: only shoot if armed.
    bool hasWeapon() const { return m_hasWeapon; }

    // Force the armed state (used by the smoketest so the viewmodel draws without
    // walking the player into the pickup). Hides the pickup when arming.
    void forceArm(Scene& scene);

    // The pickup entity id (kNoLink until built).
    uint32_t pickupEntity() const { return m_pickupEntity; }

    // The legacy pistol GLB's BARREL LINE height in its own scene space (the same
    // anchor Arsenal uses; see the kVmDef* note above). 0 for the fallback box,
    // which is already authored around its own origin.
    float viewmodelPivotY() const { return m_vmPivotY; }

    // True if the real GLB loaded; false if the procedural fallback box is in
    // use. Valid after buildWeaponPickup().
    bool usingRealModel() const { return m_usingReal; }

private:
    // Issue the per-primitive drawMesh calls for the weapon at `model` (16-float
    // column-major). Shared by the pickup (via the Entity) and the viewmodel.
    void drawWeaponAt(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                      const float model[16]) const;

    // Loaded model + its draw records (one per primitive). The loader is kept so
    // unload() could free the handles later; for the slice the app owns it until
    // shutdown.
    std::unique_ptr<x3::asset::IAssetSource>  m_assets;
    std::unique_ptr<x3::asset::IModelLoader>  m_loader;
    x3::asset::Model                          m_model;
    std::vector<x3::asset::ModelDrawable>     m_drawables;
    bool                                      m_usingReal = false;

    // Pickup placement + animation.
    x3::phys::Vec3 m_pickupPos{};            // authored base position (bob center)
    uint32_t       m_pickupEntity = kNoLink; // index into the Scene
    float          m_animT = 0.0f;           // animation cursor (seconds), bob+spin
    float          m_modelScale = 1.0f;      // uniform scale applied to the model
    // Barrel-line anchor for the FP viewmodel (scene-space Y of the barrel in
    // WeaponEnergyPistol.glb, MEASURED with tools/weapon_muzzle_probe.py: the
    // model is 1.8 units tall standing on y=0 and its barrel rides at y=1.603).
    // 0 while the procedural fallback box is in use.
    float          m_vmPivotY = 0.0f;

    bool m_hasWeapon = false;
};

// Headless self-test (--test-pickup). Drives shouldArm()/update() with synthetic
// player positions (T1 far, T2 enter radius, T3 stays armed on leave). Logs
// PASS/FAIL T#, returns true iff all pass. No window/Vulkan. Mirrors
// runPlayerSelfTest()/runInteractSelfTest().
bool runPickupSelfTest();

// ===========================================================================
// DATA-DRIVEN WEAPON ARSENAL (general FPS weapon system; EFLZ first consumer)
// ===========================================================================
// A WeaponDef is pure DATA describing one weapon. The Arsenal owns the roster +
// the per-weapon runtime state (selection, per-weapon ammo/mag, cooldown, reload
// timer) and the PURE logic for switch / fire-gating / spread / recoil / reload
// and hitscan-vs-projectile shot resolution. It is fully headless-testable
// (--test-weapons): no Vulkan, no physics, no Scene needed for the logic.
//
// Rendering (the per-weapon first-person viewmodel GLBs) is a SEPARATE optional
// layer (loadViewmodels()/drawCurrentViewmodel()) the host wires in; the existing
// single-pistol WeaponSystem above is untouched and still drives pickup+arming.
//
// Values source: docs/EFLZ_DESIGN.md §5 + docs/ASSET_INVENTORY.md S5 weapon table
// (Pistol 15 dmg / 3 per s / mag 12 / 50 m / hitscan; Shotgun 20 per pellet x8 /
// 1 per s / 15 m; Chaingun = the SMG/auto archetype; Plasma = projectile). See
// makeDefaultRoster() in weapon.cpp for the exact per-weapon numbers + provenance.

// How a weapon delivers its shot.
enum class FireKind : uint8_t {
    Hitscan,     // instant raycast(s) — pistol / SMG / shotgun
    Projectile,  // spawn a travelling projectile — plasma / energy
};

// One weapon's data. Plain values, no behaviour. Copyable.
struct WeaponDef {
    std::string name        = "weapon"; // display / log / switch name
    FireKind    kind        = FireKind::Hitscan;
    bool        automatic   = false;    // true: holding fire keeps firing at fireRate
    int         damage      = 15;       // damage PER hitscan ray / per projectile / per pellet
    // canon-aliens Adaptive Hide: the type-tag this weapon stamps onto each shot
    // (resolved into HitscanRay::type / ProjectileSpawn::type by Arsenal::fire and
    // ultimately passed to MonsterManager::fire(..., damage, type) by the host).
    // Default Kinetic — every weapon that doesn't opt-in reads as Kinetic on the
    // canon-aliens Warlord's resist machine. (See docs/canon-aliens-adaptive-hide.md §4.1.)
    x3::DamageType type    = x3::DamageType::Kinetic;
    float       fireRate    = 3.0f;     // shots per second (cooldown = 1/fireRate)
    int         pellets     = 1;        // rays per shot (shotgun > 1); 1 for single-ray
    float       spreadDeg   = 0.0f;     // half-angle cone of random spread (deg) per ray
    float       recoilDeg   = 1.0f;     // upward pitch kick per shot (deg) applied to camera
    float       range       = 50.0f;    // effective hitscan range (m)
    int         magSize     = 12;       // rounds per magazine (one shot = one round)
    int         reserveAmmo = 60;       // spare rounds carried (refills the mag on reload)
    float       reloadTime  = 1.5f;     // seconds to reload a full magazine
    float       projSpeed   = 0.0f;     // projectile travel speed (m/s); 0 for hitscan
    // ---- Act-1 weapon-ladder mechanics (additive; default 0 = old behaviour) ----
    // ChainGun spin-up: seconds of continuous firing before the weapon reaches its
    // full fireRate. While spinning up the EFFECTIVE rate ramps linearly from
    // (fireRate * spinUpStartFrac) to fireRate; the spin decays when not firing.
    // spinUpTime <= 0 -> no spin-up (instant full rate), so existing weapons are
    // unchanged. Pure timing data; the host needs no new wiring.
    float       spinUpTime     = 0.0f;  // s of sustained fire to reach full RoF (0=off)
    float       spinUpStartFrac= 0.4f;  // fraction of fireRate at a cold start
    // Plasma Rifle splash: a small radius (m) of area damage on projectile impact.
    // The host reads splashRadius/splashDamage on a ProjectileSpawn to optionally
    // apply AoE; 0 radius -> plain single-target (no behaviour change for plasma).
    float       splashRadius   = 0.0f;  // AoE radius on impact (m); 0 = none
    int         splashDamage   = 0;     // AoE damage at the impact center
    // Lightning Gun beam: a continuous instant-hit beam that can chain to nearby
    // enemies and falls off past a range threshold. chainTargets>0 emits up to that
    // many extra rays flagged as chain links; falloffStart begins linear damage
    // falloff (to 0 at `range`). All default to "no chain / no falloff".
    bool        beam           = false; // true: continuous instant beam (lightning)
    int         chainTargets   = 0;     // extra chain rays beyond the primary (0=single)
    float       falloffStart   = 0.0f;  // m at which damage begins falling off (0=none)
    // ---- CHARGE weapon (Lightning Gun redesign — Tim spec) ------------------
    // A charge weapon has NO magazine and NO reload. Holding fire drains a
    // continuous CHARGE pool at chargeDrainPerSec (units/second) while the beam is
    // held; it can fire as long as charge > 0 (IDKFA bypasses the drain entirely).
    // Battery pickups add charge, STACKING past chargeMax up to chargeCap. When
    // usesCharge is true the magSize/reserveAmmo/reloadTime fields are ignored for
    // this weapon (canFire/fire/reload/tick special-case it). Default off, so every
    // existing ammo weapon is byte-identical.
    bool        usesCharge        = false; // true: charge pool instead of mag/reload
    float       chargeMax         = 100.0f; // base full charge (Tim: "100 base charge")
    float       chargeCap         = 300.0f; // hard ceiling for battery stacking
    float       chargeDrainPerSec = 10.0f;  // continuous drain while the beam is held
    // ---- PASSIVE REGEN (Tim: "lets let the lightning recharge when not in use",
    //                         "regen all the way, but half speed over 150") ---------
    // After chargeRegenDelay seconds of NOT firing, the pool refills passively —
    // ALL THE WAY TO THE CAP (chargeRegenTo defaults to chargeCap = 300), but on a
    // TWO-SPEED curve:
    //   * below chargeRegenSlowAbove (150): the FULL rate  (1.667/s -> base 100
    //     refills from empty in ~60 s), so you are fighting again quickly.
    //   * at/above chargeRegenSlowAbove: HALF rate (chargeRegenSlowMult = 0.5), a
    //     long slow crawl to top off the reserve.
    // 0 -> 300 therefore costs ~90 s (fast half) + ~180 s (slow half) = ~270 s.
    //
    // WHY THE CRYSTAL BATTERY CELLS STILL MATTER: they let you SKIP the slow crawl.
    // Cells are the FAST way to a stocked gun, not the only way. (A flat regen to
    // the cap at full speed WOULD have made the cells pointless — hence the halving.)
    //
    // Regen HARD-STOPS at chargeRegenTo — it never overshoots, and it never DRAINS a
    // pool that is already above it. chargeRegenPerSec <= 0 disables regen entirely
    // (the default), so every non-charge weapon is byte-identical.
    float       chargeRegenPerSec  = 0.0f;   // charge/second (fast band) once the delay elapses
    float       chargeRegenDelay   = 0.0f;   // s after firing STOPS before regen begins
    float       chargeRegenTo      = 0.0f;   // regen ceiling; <= 0 -> chargeCap (the 300 cap)
    float       chargeRegenSlowAbove = 0.0f; // charge at/above which regen halves (0 = no slow band)
    float       chargeRegenSlowMult  = 0.5f; // rate multiplier in the slow band
    // Viewmodel: the GLB filename (in the rigged-GLB dir) + the convention-correct
    // viewmodel pose offsets (degrees / meters about the camera basis — same basis
    // the existing pistol viewmodel uses; see WeaponSystem::drawViewmodel + §3 of
    // docs/CONVENTIONS.md). Empty viewmodelGlb -> fall back to the pistol viewmodel.
    std::string viewmodelGlb = "WeaponEnergyPistol.glb";
    float       vmYawDeg     = kVmDefYawDeg;
    float       vmPitchDeg   = kVmDefPitchDeg;
    float       vmRollDeg    = kVmDefRollDeg;
    float       vmFwd        = kVmDefFwd;
    float       vmRight      = kVmDefRight;
    float       vmDown       = kVmDefDown;
    float       vmScale      = 0.18f;   // model scale for the held viewmodel
    // ---- THE MUZZLE (barrel tip) — Tim 2026-07-11: "the fire doesn't come from the barrel"
    // The FX origin USED TO be a camera-relative guess (muzzleFromCamera: eye + fwd 1.3 /
    // right 0.26 / down 0.30), which has NO IDEA where this weapon's barrel actually is —
    // so flashes/tracers/bolts spawned in MID-AIR beside the gun, and every weapon (they are
    // all different lengths!) was wrong by a different amount.
    //
    // vmMuzzle is the barrel tip expressed in the viewmodel GLB's SCENE space (i.e. AFTER the
    // glTF node transform, BEFORE vmScale) — the exact space the FP viewmodel world matrix
    // maps from (drawCurrentViewmodel: model = composeTRS(bx,by,bz, vmScale * kVmScaleBoost,
    // pos), each drawable drawn as model * nodeTransform). So:
    //     muzzleWorld = pos + (bx*mx + by*my + bz*mz) * (vmScale * kVmScaleBoost)
    // which tracks the gun EXACTLY — its pose, its per-weapon vm* offsets and its scale.
    // (No weapon GLB ships a muzzle/barrel socket node — they are single "model" /
    // "model_LOD*" nodes — so these are MEASURED per GLB by tools/weapon_muzzle_probe.py:
    // the centroid of the +Z front slice of the geometry, +Z being the down-barrel axis the
    // viewmodel basis maps onto camera-forward via the 193 deg vmYaw flip.)
    x3::phys::Vec3 vmMuzzle{ 0.0f, 0.65f, 0.86f };   // GLB scene-space barrel tip
    // NOTE: the fold's `vmLitPBR` flag is deliberately NOT carried over. It selected a
    // flat dark-gunmetal factor INSTEAD of the weapon's baked texture — a workaround for
    // the over-unity kVmBright, which the 1/PI engine fix (5c35d65) made unnecessary. The
    // fold's own weapon.cpp had already deleted the branch that read it, leaving a dead
    // field. The textured path at kVmBright = 1.0 is the single source of truth.
    // FX preset hints (string keys the host maps onto CombatFx muzzle/impact). Kept
    // as data so designers can retune which preset a weapon uses; the host reads them.
    // The host maps these onto a WeaponFxKind (see app/fx.h fxKindFromId) so each gun
    // reads with its own tint/scale (plasma blue, chaingun hot/sparky, etc.).
    std::string muzzleFx     = "muzzle_default";
    std::string impactFx     = "impact_default";
    // Per-weapon FIRE sound (sci-fi weapon SFX from the Sci-Fi Guns pack). Pack-relative
    // path resolved by resolveAudio() (the repo-local assets/audio mirror or the
    // per-machine D:/G: roots). Empty -> the host falls back to the shared gunshot SFX.
    // The host loads each distinct fireSfx once into a name->SoundHandle cache at init
    // and plays the CURRENT weapon's handle when it fires (headless: silent, no crash).
    std::string fireSfx      = "";
    // Loopable fire SFX? (Task #21 FIX B). When true the host plays fireSfx as ONE
    // sustained LOOP voice (IAudioSystem::startLoop) begun on the rising edge of held
    // fire and stopped the instant fire stops (release / weapon switch / empty mag /
    // death / menu) — so a held auto = one continuous whine that cuts on release,
    // instead of a per-round one-shot whose reverb tails stack into a long roar.
    // When false the host plays fireSfx as a per-shot one-shot (unchanged).
    bool        fireSfxLoop  = false;
    // Per-weapon IMPACT sound (played 3D at the hit point when a shot strikes a hard
    // surface — energy weapons get an energy splat, ballistics a bullet impact). Same
    // resolveAudio() resolution + graceful-miss semantics as fireSfx. Empty -> the host
    // plays no dedicated impact sound (the visual impact FX still spawns). Cached once
    // per distinct WAV at init alongside the fire sounds.
    std::string impactSfx    = "";
    // Per-weapon RELOAD sound (played once, non-positional, on the rising edge of a
    // reload). Empty -> no reload sound. W2-C: every roster weapon now ships the
    // shared repo-local WAV (weapons/reload_generic.wav) — same graceful-miss
    // semantics if the file is absent.
    std::string reloadSfx    = "";
    // W2-C: empty-mag trigger click (same resolveAudio + graceful-miss semantics).
    // Played by the host when ResolvedFire.dryFire is set.
    std::string dryfireSfx   = "";
};

// One travelling projectile spawned by a Projectile-kind weapon. Pure data the
// host advances (pos += vel*dt) and resolves against physics each frame. The
// Arsenal returns these from resolveFire(); it does NOT own/integrate them (the
// host holds the live list so it can spawn FX + apply damage on impact).
struct ProjectileSpawn {
    x3::phys::Vec3 pos{};      // muzzle origin
    x3::phys::Vec3 vel{};      // unit dir * projSpeed (m/s)
    int            damage = 0; // damage on impact
    float          range  = 0; // max travel distance before despawn (m)
    // canon-aliens Adaptive Hide: type stamped from the firing WeaponDef::type so the
    // host can pass it to MonsterManager::fire(..., damage, type).
    x3::DamageType type   = x3::DamageType::Kinetic;
    // Plasma Rifle splash: if splashRadius > 0 the host may apply splashDamage to
    // every enemy within splashRadius of the impact point (in addition to the
    // direct-hit `damage`). 0 radius -> plain single-target bolt.
    float          splashRadius = 0; // AoE radius on impact (m)
    int            splashDamage = 0; // AoE damage at the impact center
};

// One resolved hitscan ray (after spread). The host raycasts each one and applies
// `damage` to the first enemy hit (reusing the existing combat damage + FX path).
struct HitscanRay {
    x3::phys::Vec3 dir{};      // unit fire direction (spread already applied)
    int            damage = 0; // damage this ray deals on a hit
    float          range  = 0; // max ray distance (m)
    // canon-aliens Adaptive Hide: stamped from the firing WeaponDef::type (same on
    // every ray of a multi-pellet shot; chain rays inherit too).
    x3::DamageType type   = x3::DamageType::Kinetic;
    // Lightning Gun beam metadata (default = a plain ray, so existing weapons are
    // unaffected). `beam` marks an instant-hit beam (continuous-feel) the host may
    // render as a solid line rather than a tracer; `chain` is true for the extra
    // chain-link rays past the primary; `falloffStart` is the distance past which
    // `damage` falls off linearly to 0 at `range` (0 = no falloff, full damage).
    bool           beam  = false;
    bool           chain = false;
    float          falloffStart = 0; // m where damage starts falling off (0 = none)
};

// Result of a successful fire (gating already passed). Exactly one of the two
// vectors is populated depending on the weapon's FireKind. `recoilPitchDeg` is the
// camera pitch kick the host applies. `fired` is false when the shot was gated
// (cooldown not elapsed / empty mag / reloading) — nothing was consumed.
struct ResolvedFire {
    bool                         fired = false;
    // W2-C: true when the trigger pull was gated by an EMPTY MAG specifically (not
    // mid-reload, not the fire-rate cooldown) — the host plays the weapon's
    // dryfireSfx click on this. Cadence-respecting: the cooldown gate is checked
    // first, so holding fire on an auto weapon clicks at the weapon's fire rate,
    // not once per frame.
    bool                         dryFire = false;
    std::vector<HitscanRay>      rays;          // FireKind::Hitscan (pellets entries)
    std::vector<ProjectileSpawn> projectiles;   // FireKind::Projectile (1 entry)
    float                        recoilPitchDeg = 0.0f;
};

// Build the default roster: pistol, SMG (auto), shotgun (pellets), plasma
// (projectile), then the Act-1 ladder — ChainGun (spin-up auto hitscan),
// Plasma Rifle (splash projectile) and Lightning Gun (chaining beam).
// Values pulled from the design docs (see weapon.cpp provenance).
std::vector<WeaponDef> makeDefaultRoster();

// Apply a uniform random cone of half-angle `spreadDeg` around unit `dir`, using
// the deterministic stream `rngState` (xorshift; advanced in place). Returns a new
// unit direction. spreadDeg <= 0 returns `dir` unchanged (perfect accuracy).
x3::phys::Vec3 applySpread(const x3::phys::Vec3& dir, float spreadDeg, uint32_t& rngState);

// The data-driven multi-weapon arsenal. Owns the roster + per-weapon runtime state
// (ammo in mag, reserve, the shared fire cooldown, the reload timer, the selected
// index). All gameplay logic (switch / canFire / fire / reload / tick) is here and
// is headless-pure. The optional viewmodel-render layer is loaded separately.
class Arsenal {
public:
    // Per-weapon mutable runtime state (parallel to the roster vector).
    struct WeaponState {
        int   ammoInMag   = 0;     // rounds currently chambered
        int   reserve     = 0;     // spare rounds left
        float cooldown    = 0.0f;  // seconds until this weapon can fire again
        float reloadTimer = 0.0f;  // > 0 while reloading; refills the mag at 0
        // ChainGun spin-up charge in [0,1]: 0 = cold (fires at spinUpStartFrac of
        // fireRate), 1 = fully spun up (full fireRate). Climbs while firing, decays
        // when idle. Stays 0/unused for weapons with spinUpTime <= 0.
        float spinUp      = 0.0f;
        // CHARGE weapons only (usesCharge): current charge in [0, chargeCap]. Seeded
        // to chargeMax at construction, drained continuously while the beam is held,
        // refilled (stacking past chargeMax to chargeCap) by battery pickups. Unused
        // (stays 0) for ammo weapons.
        float charge      = 0.0f;
        // CHARGE weapons only: seconds still to wait before PASSIVE REGEN may begin.
        // Reset to chargeRegenDelay on every firing frame (held beam or fire()), so a
        // burst never free-refills mid-fight — you must let it cool. Counts down while
        // idle; regen runs only once it reaches 0.
        float regenDelay  = 0.0f;
    };

    // Construct with a roster (defaults to makeDefaultRoster()). Each weapon starts
    // with a full magazine + its full reserve. Selection starts at index 0.
    explicit Arsenal(std::vector<WeaponDef> roster = makeDefaultRoster());

    // ---- Roster / selection -------------------------------------------------
    int  count() const { return (int)m_defs.size(); }
    int  selected() const { return m_sel; }
    const WeaponDef&   def(int i) const { return m_defs[(size_t)i]; }
    const WeaponDef&   current() const { return m_defs[(size_t)m_sel]; }
    const WeaponState& state(int i) const { return m_state[(size_t)i]; }
    const WeaponState& currentState() const { return m_state[(size_t)m_sel]; }

    // Select weapon by 0-based index (number keys 1..N map to 0..N-1). Out-of-range
    // is ignored. Switching cancels an in-progress reload on the OLD weapon (its
    // reload timer is cleared) and resets the shared fire cooldown to the NEW
    // weapon's so you can't switch-fire faster than its rate. Returns the selection.
    int  select(int index);
    // Find a weapon by (case-sensitive) name; -1 if none. selectByName selects it.
    int  indexOf(const std::string& name) const;
    bool selectByName(const std::string& name);

    // ---- Fire / reload ------------------------------------------------------
    // True iff the current weapon CAN fire right now: not reloading, cooldown
    // elapsed, and at least one round in the mag.
    bool canFire() const;

    // Attempt to fire the current weapon along unit-ish `dir` from `eye`. If gated
    // (see canFire) returns {fired=false} and consumes nothing. On success it
    // consumes one round, starts the fire cooldown (1/fireRate), applies recoil, and
    // resolves the shot: `pellets` spread rays (Hitscan) OR one projectile
    // (Projectile). `rngState` drives spread + is advanced. The host runs each ray /
    // spawns each projectile + the FX. (Reload is automatic when the mag empties only
    // if you call reload(); firing an empty mag is simply gated.)
    ResolvedFire fire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir, uint32_t& rngState);

    // Begin reloading the current weapon: starts the reload timer if there is
    // reserve ammo and the mag isn't already full and we aren't already reloading.
    // Returns true iff a reload actually began. The mag is refilled when the timer
    // reaches 0 in tick().
    bool reload();
    bool isReloading() const { return currentState().reloadTimer > 0.0f; }

    // IDKFA infinite ammo: when set, fire() never depletes the mag and canFire()
    // ignores an empty mag (the fire-rate cooldown still applies). Console cheat.
    void setInfiniteAmmo(bool b) { m_infiniteAmmo = b; }
    bool infiniteAmmo() const { return m_infiniteAmmo; }

    // ---- [W9-3 RPG] progression multiplier layer ---------------------------
    // Multipliers LAYERED on the WeaponDef table (the defs are never mutated).
    // The skill tree / weapon mods drive these via the host's applyRpgStats().
    // Reload time multiplier (< 1 = faster; clamped [0.35, 1]).
    void  setReloadMult(float m) { m_reloadMult = m < 0.35f ? 0.35f : (m > 1.0f ? 1.0f : m); }
    float getReloadMult() const { return m_reloadMult; }
    // Reserve-ammo CAP multiplier (>= 1) — raises where addReserve tops out.
    void  setAmmoCapMult(float m) { m_ammoCapMult = m < 1.0f ? 1.0f : m; }
    float getAmmoCapMult() const { return m_ammoCapMult; }
    // Add reserve rounds to weapon `index` (the ammo-pack item's use verb),
    // capped at reserveAmmo * ammoCapMult. Returns rounds actually added.
    int addReserve(int index, int rounds);
    int addReserveCurrent(int rounds) { return addReserve(m_sel, rounds); }

    // ---- CHARGE weapon (Lightning Gun) --------------------------------------
    // Host sets this each frame: true while the fire button is HELD for the current
    // (charge) weapon. tick() bleeds the current charge weapon's charge at its
    // chargeDrainPerSec while held (IDKFA bypasses). No-op for ammo weapons.
    void setBeamHeld(bool b) { m_beamHeld = b; }
    // Battery pickup grant: add `amount` charge to the (first) charge weapon,
    // stacking past chargeMax up to chargeCap. Returns the amount actually added
    // (0 if there is no charge weapon or it's already capped).
    float grantCharge(float amount);
    // Index of the first usesCharge weapon (-1 if none) — for battery-pickup wiring
    // + the HUD charge readout.
    int   chargeWeaponIndex() const;

    // ---- PASSIVE REGEN readout (HUD; must not lie) ---------------------------
    // Is the CURRENT weapon's charge pool passively regenerating RIGHT NOW? True iff
    // it is a charge weapon with regen configured, the post-fire delay has fully
    // elapsed, and the pool is still below its regen ceiling. False while the beam is
    // held, during the cool-down delay, and once the ceiling is reached.
    bool  chargeRegenerating() const;
    // Seconds still to wait before regen begins (0 once it is running / N/A).
    float chargeRegenWait() const;
    // True iff the current charge weapon is in the SLOW (half-rate) regen band —
    // i.e. its charge is at/above chargeRegenSlowAbove. Lets the HUD show the crawl
    // honestly instead of implying one uniform refill speed.
    bool  chargeRegenSlow() const;

    // Advance the per-weapon timers by dt: decay the current weapon's fire cooldown
    // and, if reloading, the reload timer (completing the reload — moving rounds
    // from reserve into the mag — when it hits 0). Other weapons' cooldowns are also
    // decayed so a quick switch-back isn't unfairly fast.
    void tick(float dt);

    // ---- Optional first-person viewmodel render layer -----------------------
    // Load each roster weapon's viewmodel GLB from `modelDir` (the rigged-GLB dir),
    // uploading drawables via `device`. A weapon whose GLB is missing/empty falls
    // back to the FIRST successfully-loaded viewmodel (the pistol), per the task.
    // Safe to skip entirely (the logic layer works without it). Call once.
    void loadViewmodels(x3::rhi::IRenderDevice& device, std::string_view modelDir);

    // Draw the CURRENT weapon's viewmodel at the camera-relative pose, using the
    // weapon's own convention-correct vm* offsets (the live cvar offsets, if the
    // host passes them, are added on top). No-op if viewmodels weren't loaded. The
    // camera basis + math match WeaponSystem::drawViewmodel exactly.
    void drawCurrentViewmodel(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                              float eyeX, float eyeY, float eyeZ, float yaw, float pitch,
                              float extraYawOff = 0.0f, float extraPitchOff = 0.0f,
                              float extraRollOff = 0.0f, float extraFwd = 0.0f,
                              float extraRight = 0.0f, float extraDown = 0.0f) const;

    bool viewmodelsLoaded() const { return !m_views.empty(); }

    // ---- FP viewmodel LENS + SIZE levers (see kVmDefFovDeg in this header) ----
    // Set the viewmodel FOV emulation. `vmFovDeg` <= 0 shares the world lens (the
    // default, magnification 1). `worldFovDeg` is the FOV the frame is ACTUALLY
    // rendered at (60 in the live loop, 70 in the still-capture hosts) — pass it so
    // the same vm_fov reads the same in a still as it does in play. `scaleMul` is a
    // pure size multiplier on top (1 = the shipped kVmScaleBoost read).
    // Seeded at construction from the environment so the levers are reachable from
    // a command line with NO rebuild and NO app_run.cpp plumbing:
    //     X3_VM_FOV, X3_VM_WORLDFOV, X3_VM_SCALE
    void  setViewmodelLens(float vmFovDeg, float worldFovDeg, float scaleMul = 1.0f);
    float viewmodelFovDeg()      const { return m_vmFovDeg; }
    float viewmodelWorldFovDeg() const { return m_vmWorldFovDeg; }
    float viewmodelScaleMul()    const { return m_vmScaleMul; }
    // The magnification the lens levers currently apply (1.0 = untouched).
    float viewmodelMagnification() const;

    // ---- THE MUZZLE -----------------------------------------------------------
    // The composed FP viewmodel frame: the world basis (bx,by,bz), the origin `pos` and
    // the total scale the gun is DRAWN at. Identical math to (and shared with)
    // drawCurrentViewmodel, so anything solved in this frame lands ON the gun.
    struct VmFrame {
        x3::phys::Vec3 bx{1,0,0}, by{0,1,0}, bz{0,0,1};
        // The GLB ORIGIN in the world = the model matrix's translation column, i.e.
        // a GLB scene-space point p draws at pos + scale*(bx*p.x + by*p.y + bz*p.z).
        // NOTE this is NOT where vm_fwd/vm_right/vm_down point: those position the
        // gun's BARREL LINE (0, vmMuzzle.y, 0) and the anchor is folded in here.
        x3::phys::Vec3 pos{};
        float          scale = 1.0f;   // vmScale * kVmScaleBoost * lens magnification
    };
    VmFrame currentViewmodelFrame(float eyeX, float eyeY, float eyeZ, float yaw, float pitch,
                                  float extraYawOff = 0.0f, float extraPitchOff = 0.0f,
                                  float extraRollOff = 0.0f, float extraFwd = 0.0f,
                                  float extraRight = 0.0f, float extraDown = 0.0f) const;

    // The CURRENT weapon's barrel tip IN THE WORLD — the ONE true origin for the muzzle
    // flash, the tracer, the projectile spawn and the 3D fire audio. Same args as
    // drawCurrentViewmodel (pass the host's live vm_* cvar DELTAS so the muzzle follows a
    // console-nudged gun too).
    x3::phys::Vec3 currentMuzzle(float eyeX, float eyeY, float eyeZ, float yaw, float pitch,
                                 float extraYawOff = 0.0f, float extraPitchOff = 0.0f,
                                 float extraRollOff = 0.0f, float extraFwd = 0.0f,
                                 float extraRight = 0.0f, float extraDown = 0.0f) const;

    // The current weapon's barrel tip in the viewmodel GLB's SCENE space (pre-scale). The
    // THIRD-PERSON path needs this: it already builds its own hand-socket world matrix
    // (ThirdPersonView::drawHeldWeapon) and just has to transform this point by it, so the
    // fire leaves the barrel in 3P too.
    x3::phys::Vec3 currentMuzzleLocal() const {
        return (m_sel >= 0 && m_sel < (int)m_defs.size()) ? m_defs[(size_t)m_sel].vmMuzzle
                                                          : x3::phys::Vec3{ 0.0f, 0.65f, 0.86f };
    }

    // ---- Third-person HELD-weapon render (socket to the avatar's hand bone) --
    // Draw the CURRENT weapon's loaded viewmodel meshes at an arbitrary world
    // `model` matrix (column-major 4x4) — REUSING the same drawables loaded for the
    // FP viewmodel (no asset duplication). The 3P avatar reads its right-hand bone
    // world transform from the Skinner and composes it with a per-weapon grip
    // offset to produce `model`, so the equipped gun rides in the avatar's hand and
    // swaps automatically when the player switches weapons (m_sel). No-op if
    // viewmodels weren't loaded / the current slot has no drawables. The per-weapon
    // viewmodel scale (vmScale) is folded into `scale` so the host doesn't need it.
    void drawCurrentAt(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                       const float model[16]) const;

    // The current weapon's viewmodel scale (vmScale) — exposed so the host can fold
    // it into the hand-socket placement matrix it builds.
    float currentViewmodelScale() const {
        return (m_sel >= 0 && m_sel < (int)m_defs.size()) ? m_defs[(size_t)m_sel].vmScale : 0.18f;
    }
    // True iff the CURRENT weapon slot resolves to a non-empty set of drawables
    // (its own load, or the pistol fallback). Lets the host gate the held-weapon draw.
    bool currentHasDrawables() const;

    // ---- Checkpoint restore (save/load) -----------------------------------
    // Restore the selection + a per-weapon (ammoInMag,reserve) pair. `ammo` is
    // applied in roster order, up to min(ammo.size(), count()); each entry is clamped
    // to [0, magSize]/[0,reserveAmmo]. Cooldowns + reload timers are cleared (a clean
    // checkpoint restore, not mid-reload). `sel` is clamped to a valid index. This is
    // the load-time apply of an Arsenal checkpoint captured via state()/selected().
    void restore(int sel, const std::vector<std::pair<int,int>>& ammo);

private:
    std::vector<WeaponDef>   m_defs;
    std::vector<WeaponState> m_state;
    int                      m_sel = 0;
    bool                     m_infiniteAmmo = false;   // IDKFA infinite-ammo flag
    bool                     m_beamHeld     = false;   // CHARGE weapon: fire held this frame

    // One loaded viewmodel per roster slot (drawables + scale). `fallbackIndex` is
    // the slot it actually draws (its own load, or the pistol fallback).
    struct ViewModel {
        std::unique_ptr<x3::asset::IAssetSource> assets;
        std::unique_ptr<x3::asset::IModelLoader> loader;
        x3::asset::Model                         model;
        std::vector<x3::asset::ModelDrawable>    drawables;
        int                                      fallbackIndex = -1; // -1 = own draws
    };
    std::vector<ViewModel> m_views;
    // W2-C FIRST-PERSON ARMS: Jake's own arms baked into a static aim pose
    // (assets/rigged_glb/FPArms_Jake.glb — tools/bake_fp_arms.ps1 bakes them from
    // Jake_22_actions.glb: Rifleaimingidle pose frozen, arm/hand-weighted vertices
    // only, origin re-centered on the NECK so rotating about the eye swings them
    // like a body, not a prop). Drawn eye-anchored under every weapon by
    // drawCurrentViewmodel. Missing GLB = graceful no-op (arms simply don't draw).
    // Arms deliberately do NOT get kVmScaleBoost — the guns are 2x by design
    // (Tim-approved oversized reads); human arms at 2x read as a giant.
    ViewModel m_arms;

    // [W9-3 RPG] progression multiplier layer state (see setReloadMult/setAmmoCapMult).
    float m_reloadMult  = 1.0f;
    float m_ammoCapMult = 1.0f;

    // FP viewmodel lens levers (setViewmodelLens; env-seeded in the ctor).
    float m_vmFovDeg      = kVmDefFovDeg;
    float m_vmWorldFovDeg = kVmWorldFovDeg;
    float m_vmScaleMul    = 1.0f;
};

// Headless self-test (--test-weapons). Exercises the data-driven arsenal with NO
// window / Vulkan / physics: roster correctness, weapon switching selects the right
// def, fire respects fireRate (can't fire faster than the cooldown) + ammo (can't
// fire on an empty mag), reload refills the mag from reserve, the shotgun emits N
// pellets, and hitscan vs projectile shots both resolve into the right payload.
// Also covers the Act-1 ladder (W8..W11): ChainGun spin-up ramp, Plasma Rifle
// splash bolt, Lightning Gun chaining beam, and the ladder power ordering.
// Logs PASS/FAIL W#, returns true iff all pass.
bool runWeaponsSelfTest();

// Headless self-test (--test-lightning-charge). Exercises the Lightning Gun CHARGE
// model with NO window/Vulkan: base charge seeded to chargeMax, continuous drain
// math (~chargeDrainPerSec while beam held), IDKFA never depletes, battery grant
// stacks past chargeMax to chargeCap, and canFire gates on charge (no mag/reload).
// Logs PASS/FAIL LC#, returns true iff all pass.
bool runLightningChargeSelfTest();

} // namespace x3::game
