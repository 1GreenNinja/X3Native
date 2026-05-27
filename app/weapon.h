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
constexpr float kVmDefYawDeg   = 193.0f; // yaw about camera up (degrees) — barrel -> forward (Tim-tuned)
constexpr float kVmDefPitchDeg = 7.0f;   // pitch about camera right (degrees) (Tim-tuned)
constexpr float kVmDefRollDeg  = 0.0f;   // roll about camera forward (degrees)
constexpr float kVmDefFwd      = 1.0f;   // forward along look dir (meters) (Tim-tuned)
constexpr float kVmDefRight    = 0.25f;  // to the right (meters) (Tim-tuned; more-right pass)
constexpr float kVmDefDown     = 0.35f;  // below the eye line (meters) (Tim-tuned)

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

} // namespace x3::game
