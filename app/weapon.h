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
constexpr float kVmDefYawDeg   = -90.0f; // yaw about camera up (degrees) — barrel -> forward
constexpr float kVmDefPitchDeg = 0.0f;   // pitch about camera right (degrees)
constexpr float kVmDefRollDeg  = 0.0f;   // roll about camera forward (degrees)
constexpr float kVmDefFwd      = 0.5f;   // forward along look dir (meters)
constexpr float kVmDefRight    = 0.25f;  // to the right (meters)
constexpr float kVmDefDown     = 0.2f;   // below the eye line (meters)

// ---------------------------------------------------------------------------
// Weapon ViewKick (game-feel): recoil + screen-shake on fire.
// ---------------------------------------------------------------------------
// A small self-contained, headless-testable feel layer the host drives on each
// weapon FIRE. It accumulates a transient (1) viewmodel/camera RECOIL kick — an
// upward pitch + a backward push along the look dir — and (2) a brief CAMERA
// SHAKE (a decaying random jitter). Both DECAY to ~0 over a short window so the
// view recovers. The host reads the offsets each frame and folds them into the
// camera + viewmodel transform (the recoil pitch matches the existing
// weaponRecoilPitch path in main.cpp). NO gameplay effect — feel only (the fire
// ray uses the un-kicked look dir). Magnitudes are named tunables below.
// ---------------------------------------------------------------------------
// Per-shot recoil kick magnitudes (added on fire). The pitch kick is scaled by
// the weapon's own recoilDeg at the call site; these are the viewmodel pushes.
constexpr float kRecoilPitchScale = 1.0f;    // multiplier on the weapon's recoilDeg (-> rad)
constexpr float kRecoilBackPush   = 0.05f;   // meters the viewmodel kicks back per shot
constexpr float kRecoilBackMax    = 0.12f;   // clamp on accumulated back-push (m)
constexpr float kRecoilPitchMax   = 0.20f;   // clamp on accumulated recoil pitch (rad ~11.5 deg)
// Decay: recoil + back-push relax toward 0 at this exponential-ish rate (per s).
// ~6/s gives a ~0.15-0.2 s recovery (matches main.cpp's kRecoilRecover feel).
constexpr float kRecoilRecover    = 6.0f;    // pitch + back-push recovery rate (1/s)
// Screen-shake: a per-shot amplitude (radians of camera yaw/pitch jitter) that
// decays over kShakeTime. Brief + subtle so it punches without nausea.
constexpr float kShakeAmp         = 0.012f;  // rad peak jitter added per shot (clamped)
constexpr float kShakeAmpMax      = 0.03f;   // clamp on accumulated shake amplitude (rad)
constexpr float kShakeTime        = 0.18f;   // seconds for one shot's shake to fully decay
constexpr float kShakeFreq        = 38.0f;   // jitter oscillation rate (rad/s) — fast = punchy

// Transient view-kick state. Construct once per held-weapon context; call fire()
// on each shot, tick(dt) each frame, then read pitchOffset()/backOffset() and
// shakeYaw()/shakePitch() to fold into the camera + viewmodel. Fully headless:
// no device / physics / Scene. Deterministic shake (seeded xorshift) so captures
// + the self-test repeat.
class ViewKick {
public:
    // Apply one shot's kick. `weaponRecoilDeg` is the firing weapon's recoilDeg
    // (degrees) — converted to radians + scaled by kRecoilPitchScale. Adds the
    // back-push + arms a fresh screen-shake burst. Clamped to the *Max ceilings so
    // rapid auto-fire can't run the offsets away.
    void fire(float weaponRecoilDeg);

    // Advance the decay by dt: relax the recoil pitch + back-push toward 0 and age
    // the shake window. No-op at dt <= 0. Call once per frame.
    void tick(float dt);

    // Current accumulated transient offsets (read each frame; fold into the view).
    float pitchOffset() const { return m_pitch; }   // upward camera/viewmodel pitch (rad)
    float backOffset()  const { return m_back; }    // viewmodel back-push along look (m)
    // Current screen-shake yaw/pitch jitter (rad), 0 once the burst decays.
    float shakeYaw()   const;
    float shakePitch() const;

    // True while any kick is still active (offsets / shake non-negligible). For the
    // self-test (assert it returns to rest) + to skip the fold when fully recovered.
    bool active() const;

private:
    float    m_pitch   = 0.0f;   // accumulated recoil pitch (rad), decays
    float    m_back    = 0.0f;   // accumulated back-push (m), decays
    float    m_shake   = 0.0f;   // remaining shake time (s); shake amp scales with this
    float    m_shakeAmp= 0.0f;   // current shake amplitude (rad), set on fire, clamped
    float    m_shakeT  = 0.0f;   // phase accumulator for the jitter oscillator
    uint32_t m_rng     = 0x5EED1234u; // deterministic jitter stream
};

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
    std::string muzzleFx     = "muzzle_default";
    std::string impactFx     = "impact_default";
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
};

// One resolved hitscan ray (after spread). The host raycasts each one and applies
// `damage` to the first enemy hit (reusing the existing combat damage + FX path).
struct HitscanRay {
    x3::phys::Vec3 dir{};      // unit fire direction (spread already applied)
    int            damage = 0; // damage this ray deals on a hit
    float          range  = 0; // max ray distance (m)
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
// (projectile). Values pulled from the design docs (see weapon.cpp provenance).
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

private:
    std::vector<WeaponDef>   m_defs;
    std::vector<WeaponState> m_state;
    int                      m_sel = 0;

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
// Logs PASS/FAIL W#, returns true iff all pass.
bool runWeaponsSelfTest();

} // namespace x3::game
