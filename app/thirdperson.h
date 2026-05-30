#pragma once
// Third-person view — toggleable follow camera + animated Jake player avatar +
// the equipped weapon held in the avatar's hand (FIRST MILESTONE). See
// docs/design/THIRD_PERSON_VIEW.md + app/thirdperson.cpp.
//
// Game/slice code only — engine/ stays pure. This is an ADDITIVE mode, not a
// rewrite: the player capsule / collision / controller are untouched; only the
// CAMERA changes (an orbit camera behind + above the player) and, in 3P, the
// player's own animated avatar (Jake) is drawn while the FP weapon viewmodel is
// hidden. FP remains the DEFAULT and is byte-for-byte unchanged.
//
// What it reuses (does NOT reinvent):
//   * The SKINNED-character GPU-skinning + locomotion blend path (app/anim.* +
//     monster.cpp): Jake loads + binds + animates through the SAME machinery the
//     monsters/girls use.
//   * The 180-deg VISUAL facing-flip (rescue.cpp/monster.cpp: ry = yaw + pi),
//     because the rigged GLBs are authored facing +Z while native is -Z forward
//     (docs/CONVENTIONS.md).
//   * The camera yaw/pitch -> forward/right/up look basis (CONVENTIONS.md §3),
//     shared with the FP eye-camera, audio, FX, and weapon.
//   * The FP weapon viewmodel MESHES (Arsenal::drawCurrentAt) — the held weapon in
//     the avatar's hand is the same drawables, socketed to `mixamorigRightHand`.

#include "scene.h"
#include "anim.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/asset/IModelLoader.h"
#include "engine/asset/IAssetSource.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace x3::game {

class Arsenal;   // app/weapon.h (held-weapon mesh source)

// The Mixamo weapon-hand socket bone on the Jake rig (confirmed via skel_dump,
// see THIRD_PERSON_VIEW.md). The avatar's right-hand bone world transform is read
// each frame to place the carried weapon.
inline constexpr const char* kJakeHandBone = "mixamorigRightHand";
// Jake's loadable skinned GLB (lives alongside chief_martinez_anim.glb in the
// rigged-GLB dir). 34-bone Mixamo rig, 22 clips, ~1.9 m, Y-up, single skinned
// `model` mesh (the stray Icosphere is an orphan mesh not referenced by any glTF
// node, so the loader never builds it — effectively dropped on import).
inline constexpr const char* kJakeModelFile = "Jake_22_actions.glb";

// Pure follow-camera math (testable headlessly). Given the player FEET position
// (capsule reference), the eye height, and the look yaw/pitch (the SAME params the
// FP camera uses), produce the 3P orbit camera: it sits behind the player (along
// -forward) and slightly above, looking toward the player's head. The look angles
// are unchanged (the existing mouse + arrow-turn drive m_yaw/m_pitch); the camera
// just orbits around the player instead of sitting at the eye. Outputs the camera
// position + the yaw/pitch to feed IRenderDevice::setCamera (yaw/pitch are passed
// through so the look direction the player aims along is preserved).
struct ThirdPersonCamera {
    float camX = 0, camY = 0, camZ = 0;   // camera world position
    float yaw = 0, pitch = 0;             // pass-through look angles (== player look)
};
ThirdPersonCamera computeFollowCamera(float feetX, float feetY, float feetZ,
                                      float eyeHeight, float yaw, float pitch,
                                      float distance, float heightAbove);

// Locomotion clip selection by planar speed. A pure helper (testable) that maps a
// planar speed (m/s) + the move sign (forward vs. backward) onto a coarse
// Idle/Walk/Run band. The avatar uses the Skinner's 1D locomotion BLEND for the
// smooth result; this selector reports the dominant band for the test + for
// choosing the run-vs-runbackwards clip set.
enum class LocoBand : uint8_t { Idle = 0, Walk = 1, Run = 2 };
LocoBand selectLocoBand(float planarSpeed, float walkThreshold = 0.4f,
                        float runThreshold = 4.0f);

// ---------------------------------------------------------------------------
// ThirdPersonView — owns the Jake avatar (skinned), the FP/3P toggle state, the
// follow camera, the locomotion drive, and the held-weapon socket.
// ---------------------------------------------------------------------------
class ThirdPersonView {
public:
    // Load Jake from `modelDir` (the rigged-GLB dir) through the SAME asset-source
    // + model-loader + Skinner path the monsters use, bind GPU skinning when
    // supported, locate idle/walk/run/runback/rifle clips by name, and resolve the
    // weapon-hand bone. Adds a Tag::Prop avatar Entity to `scene` (render mesh left
    // invalid so Scene::render skips it; this system owns the multi-primitive +
    // skinned draw). Hidden by default (FP is default). On load failure the avatar
    // stays unbuilt and the whole system gracefully no-ops (FP keeps working).
    void build(Scene& scene, x3::rhi::IRenderDevice& device, std::string_view modelDir);

    bool built() const { return m_built; }
    // True iff Jake loaded as a genuinely SKINNED model (skeleton + clips bound).
    bool skinned() const { return m_skinner.valid(); }
    // Bone count the rig bound over + whether the hand socket resolved (for the test).
    uint32_t boneCount() const { return m_skinner.nodeCount(); }
    bool handBoneResolved() const { return m_handNode >= 0; }
    int  handNode() const { return m_handNode; }

    // ---- FP/3P toggle ------------------------------------------------------
    bool thirdPerson() const { return m_thirdPerson; }
    void setThirdPerson(bool on);
    // Flip FP<->3P; returns the new state. (F5 in the game loop.)
    bool toggle() { setThirdPerson(!m_thirdPerson); return m_thirdPerson; }

    // True iff the FP weapon VIEWMODEL should draw this frame (FP only). The avatar
    // is the inverse: drawn only in 3P. These are the two things the toggle swaps.
    bool viewmodelVisible() const { return !m_thirdPerson; }
    bool avatarVisible() const { return m_thirdPerson && m_built; }

    // ---- Per-frame drive ---------------------------------------------------
    // Advance the avatar: place it at the player's feet, face it along the move
    // direction (or the look yaw when still), drive the locomotion blend from the
    // planar speed (computed from the feet delta + dt), select crouch, set the
    // avatar's roomId so the PVS cull keeps it visible, and bake the facing-flipped
    // draw transform. Re-skins via the cached device (GPU palette upload / CPU LBS).
    // No-op in FP or when unbuilt. `crouched` lowers/uses a crouch pose. `moveYaw`
    // is the planar movement heading (radians, atan2(dz,dx)); when not moving the
    // avatar faces the look `yaw`. `fireHeld` plays the rifle aim/fire clip if easy.
    void update(float dt, Scene& scene, const x3::phys::Vec3& feet, float eyeHeight,
                float yaw, float pitch, uint32_t roomId, bool crouched, bool fireHeld);

    // The follow camera for THIS frame's player state (call after update()). Returns
    // the orbit-camera position + the pass-through look angles. FP callers ignore it.
    // When aiming (built up via update()'s fireHeld) the camera biases subtly
    // over-the-right-shoulder so the avatar body doesn't block the crosshair.
    ThirdPersonCamera camera(const x3::phys::Vec3& feet, float eyeHeight,
                             float yaw, float pitch) const;

    // ---- Draw --------------------------------------------------------------
    // Draw the skinned avatar at its baked transform (gated on avatarVisible()).
    // Call within the frame render block alongside scene.render().
    void drawAvatar(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                    const Scene& scene) const;

    // Draw the equipped weapon in the avatar's hand: read the hand-bone world
    // transform from the Skinner's current pose, compose it with the avatar draw
    // transform + a small per-weapon grip offset, and draw the CURRENT weapon's
    // viewmodel meshes there (reusing the arsenal's loaded drawables). No-op in FP /
    // unbuilt / when the hand bone didn't resolve / the arsenal has no drawables.
    // `armed` gates it (no gun shown before pickup), mirroring the FP viewmodel.
    void drawHeldWeapon(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                        const Scene& scene, const Arsenal& arsenal, bool armed) const;

    // ---- Hand socket readback (also used by --test-thirdperson) ------------
    // Compute the WORLD transform (column-major 4x4) where the held weapon is
    // placed: avatarDraw * handBoneGlobal * perWeaponGrip. Returns false if unbuilt /
    // the hand bone didn't resolve / no pose computed yet. The translation column
    // (out[12..14]) is the weapon's world position (near the right hand). `weaponName`
    // selects the per-weapon grip row (kTpGripTable); empty/unknown -> the default row.
    bool handSocketWorld(float out[16], std::string_view weaponName = {}) const;

    // The avatar's current world position (feet) + facing yaw — for tests / HUD.
    x3::phys::Vec3 avatarPos() const { return m_pos; }
    float avatarYaw() const { return m_yaw; }

private:
    // Build the facing-flipped draw transform (matches rescue.cpp/monster.cpp:
    // ry = m_yaw + pi) into m_drawXform and bake it onto the avatar Entity.
    void bakeTransform(Scene& scene);

    bool m_built       = false;
    bool m_thirdPerson = false;       // FP is the default

    // Loaded Jake model + skinning (mirrors MonsterSystem's members).
    std::unique_ptr<x3::asset::IAssetSource> m_assets;
    std::unique_ptr<x3::asset::IModelLoader> m_loader;
    x3::asset::Model                         m_model;
    std::vector<x3::asset::ModelDrawable>    m_drawables;
    x3::anim::Skinner                        m_skinner;
    x3::rhi::IRenderDevice*                  m_device = nullptr;

    // Locomotion clip indices (resolved by name in build()).
    int  m_idleClip = -1, m_walkClip = -1, m_runClip = -1;
    int  m_runBackClip = -1, m_walkBackClip = -1, m_rifleIdleClip = -1, m_fireClip = -1;
    bool m_useLocoBlend = false;

    // Hand socket bone node index (resolved once in build()).
    int  m_handNode = -1;

    uint32_t m_entity = kNoLink;      // avatar Entity id in the Scene
    x3::phys::Vec3 m_pos{};           // avatar feet world position
    float    m_yaw   = 0.0f;          // avatar facing yaw (planar heading / look)
    float    m_modelScale = 1.0f;
    bool     m_havePrev = false;      // have a previous feet sample for speed calc
    x3::phys::Vec3 m_prevPos{};
    float    m_animTime = 0.0f;       // fallback single-clip cursor

    // The facing-flipped avatar DRAW transform (column-major), rebuilt each
    // update(). Cached so drawHeldWeapon()/handSocketWorld() compose against it.
    float    m_drawXform[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    // The model fixup (identity for the Y-up Jake rig; kept for parity with the
    // monster draw path final = model * fixup * nodeTransform).
    float    m_modelFixup[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

    // Jake-specific authoring fixups baked in 2026-05-27 (dialed via cvars in a
    // live playtest, then locked here as defaults). Jake_22_actions.glb has chronic
    // XYZ authoring issues that no project ever fully untangled — the empirical
    // values below are what the engine needs to plant feet on the floor and face
    // forward. Kept as MEMBER defaults so the same machinery still applies cleanly
    // and a future asset swap can override per-instance if needed.
    float    m_userYOff   = 1.03f;             // extra Y lift (m); empirical floor-plant
    float    m_userYawOff = 1.5707963f;        // +90 deg in rad; GLB faces +X natively
    float    m_camDist    = 2.3f;              // 3P follow camera distance behind player (m)
    float    m_camHeight  = 0.37f;             // 3P follow camera height above eye line (m)

    // CROUCH (synthesized — no crouch clip on this rig). m_crouchAmt is a smoothed
    // 0..1 amount driven by the `crouched` flag in update(); bakeTransform() applies
    // it as a hip drop + forward lean so the avatar visibly squats.
    float    m_crouchAmt  = 0.0f;
    // OVER-THE-SHOULDER AIM. m_aimAmt is a smoothed 0..1 amount driven by `fireHeld`
    // in update(); camera() uses it to bias the follow cam subtly over the right
    // shoulder while aiming so the body doesn't block the crosshair.
    float    m_aimAmt     = 0.0f;
};

// Follow-camera tuning (meters). Behind the player + above; the test asserts the
// camera lands behind + looks toward the player with these.
inline constexpr float kTpCamDistance    = 3.6f;   // m behind the player
inline constexpr float kTpCamHeightAbove = 0.7f;   // m above the eye line

// ---------------------------------------------------------------------------
// PER-WEAPON GRIP TABLE (TASK#46.1)
// ---------------------------------------------------------------------------
// Each gun sits differently in the palm: the hand bone is the WRIST pivot, so a
// single shared zero offset seats every model at the wrist (reads as "floating off
// the hand"). A small per-weapon offset (meters, in the hand-bone LOCAL frame:
// +right toward the thumb side, +down toward the palm, +forward down the barrel)
// plus an optional small rotation (degrees about the hand-local axes) lets each
// model nestle into the grip. Looked up by WeaponDef::name (see makeDefaultRoster):
// pistol / smg / shotgun / plasma / chaingun / plasma_rifle / lightning.
//
// HAND-TUNING: these are starting values dialed by eye against the Mixamo
// right-hand bind pose; nudge them in a headed playtest (the values are small +
// local so a change to one gun never touches another). Forward seats the model
// along the barrel out of the fist; down drops it into the palm; right centers it
// across the fingers. Rotations square the authored barrel axis onto the grip.
struct TpGrip {
    const char* name;        // matches WeaponDef::name (nullptr = default fallback row)
    float forward;           // m along the barrel (out of the fist)
    float right;             // m toward the thumb side
    float down;              // m into the palm
    float yawDeg;            // small twist about hand-up
    float pitchDeg;          // small tilt about hand-right
    float rollDeg;           // small roll about the barrel
    float scaleMul;          // per-weapon scale tweak on top of vmScale (1 = none)
};

// The grip table. The FIRST row whose name is nullptr is the default fallback used
// for any weapon not explicitly listed. Tuned so each model reads as "held", not
// "stuck to the wrist". Values are deliberately small + easy to hand-edit.
inline constexpr TpGrip kTpGripTable[] = {
    // name           fwd     right   down    yaw    pitch  roll   scale
    { "pistol",      0.07f,  0.01f,  0.04f,   0.0f,  -8.0f,  0.0f,  1.00f },
    { "smg",         0.11f,  0.01f,  0.05f,   0.0f,  -6.0f,  0.0f,  1.00f },
    { "shotgun",     0.16f,  0.00f,  0.05f,   0.0f,  -4.0f,  0.0f,  1.05f },
    { "plasma",      0.10f,  0.01f,  0.05f,   0.0f,  -6.0f,  0.0f,  1.00f },
    { "chaingun",    0.18f,  0.00f,  0.06f,   0.0f,  -3.0f,  0.0f,  1.10f },
    { "plasma_rifle",0.15f,  0.00f,  0.05f,   0.0f,  -4.0f,  0.0f,  1.05f },
    { "lightning",   0.13f,  0.01f,  0.05f,   0.0f,  -5.0f,  0.0f,  1.00f },
    { nullptr,       0.09f,  0.01f,  0.05f,   0.0f,  -6.0f,  0.0f,  1.00f }, // default
};

// Look up the grip row for a weapon name (case-sensitive, matches WeaponDef::name).
// Returns the matching row, or the trailing nullptr-named default row if no match.
// constexpr so it's a compile-time table walk + zero-cost in the hot path.
inline constexpr const TpGrip& tpGripFor(std::string_view name) {
    const TpGrip* def = &kTpGripTable[0];
    for (const auto& g : kTpGripTable) {
        if (g.name == nullptr) { def = &g; continue; }
        // constexpr-friendly C-string compare against the string_view.
        std::string_view gn(g.name);
        if (gn == name) return g;
    }
    return *def;
}

// Held-weapon scale relative to the hand bone (the FP vmScale is authored for the
// camera-relative viewmodel; in the hand it reads about the same). Multiplied by
// the per-weapon TpGrip::scaleMul.
inline constexpr float kTpHeldWeaponScaleMul = 1.0f;

// ---------------------------------------------------------------------------
// CROUCH (TASK#46.2) — Jake's 22-clip Mixamo rig has NO crouch clip. We fake a
// graceful crouch by lowering the avatar (hips drop) + a slight forward lean while
// the locomotion clips keep playing, so the avatar visibly squats instead of doing
// nothing. A real RETARGETED crouch clip is the ideal future fix (then this whole
// synthesized-crouch block can be replaced by triggerClip(crouchClip)).
inline constexpr float kTpCrouchDrop    = 0.45f;  // m the avatar lowers when crouched
inline constexpr float kTpCrouchLeanDeg = 10.0f;  // forward lean (pitch) while crouched
inline constexpr float kTpCrouchBlend   = 8.0f;   // 1/s smoothing rate in/out of crouch

// Over-the-shoulder aim (TASK#46.3, subtle): while aiming/firing the follow camera
// biases a touch to the right + in, so the avatar's body doesn't block the
// crosshair. Kept small so the existing follow cam isn't broken.
inline constexpr float kTpAimShoulderRight = 0.35f;  // m camera shift to the right when aiming
inline constexpr float kTpAimShoulderIn    = 0.45f;  // m camera pull-in toward the head when aiming
inline constexpr float kTpAimBlend         = 6.0f;   // 1/s smoothing rate in/out of aim

// Headless self-test (--test-thirdperson). Asserts the MECHANICS only (visual
// correctness is the user's eyeball job): Jake loads with the expected bone count +
// the hand bone resolves; the follow camera lands behind + looks toward the player;
// the FP/3P toggle flips state + swaps viewmodel<->avatar visibility; the loco
// selector picks Idle/Walk/Run by speed; the hand-socket transform is retrievable
// and rides with the pose. Logs PASS/FAIL TP#, returns true iff all pass. Loads the
// real Jake GLB when present; falls back to the synthetic-camera/selector checks
// (still meaningful) on a clean checkout where the asset is absent. No window/Vulkan.
bool runThirdPersonSelfTest();

} // namespace x3::game
