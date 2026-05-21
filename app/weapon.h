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
#include <vector>

namespace x3::game {

// Radius (meters) within which walking into the pickup arms the player. Measured
// player-feet (camera/eye XZ) to pickup center.
constexpr float kPickupRadius = 1.2f;

// Default first-person viewmodel pose. These seed the live-tunable console cvars
// (vm_yaw/vm_pitch/vm_roll in DEGREES, vm_fwd/vm_right/vm_down in METERS) and are
// the values baked from play-testing. Orientation defaults are in DEGREES here so
// main.cpp can register the cvars without converting. The pistol GLB's barrel
// reads "to the right" with a plain camera-basis orientation, so the default yaw
// offset (-45 deg) swings it toward forward; dial vm_* in-game to converge.
constexpr float kVmDefYawDeg   = -45.0f; // yaw about camera up   (degrees)
constexpr float kVmDefPitchDeg = 0.0f;   // pitch about camera right (degrees)
constexpr float kVmDefRollDeg  = 0.0f;   // roll about camera forward (degrees)
constexpr float kVmDefFwd      = 0.5f;   // forward along look dir (meters)
constexpr float kVmDefRight    = 0.25f;  // to the right (meters)
constexpr float kVmDefDown     = 0.2f;   // below the eye line (meters)

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

} // namespace x3::game
