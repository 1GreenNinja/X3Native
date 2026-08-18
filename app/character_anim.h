#pragma once
// AnimatedCharacter — the ONE on-foot character rig runtime, shared by every
// world host.
//
// RECEIPT (owner directive, 2026-08-16): "We ABSOLUTELY can NOT have to Do
// this fix for EVERY SINGLE WORLD. THIS ENGINE NEEDS CONSISTENT APPLICATION
// OF MODEL ANIMATION." Every Jake fix of the buried-body weekend — the exact
// clip lookup, the facing flip, the root-Y offset, the feet clamp, the jump
// one-shot, the camera — had been hand-wired into host_tunnel.cpp, which
// means the NEXT world starts the same fight from zero. This module is the
// cure: hosts construct an AnimatedCharacter with an asset + a MEASURED clip
// table, feed it input + dt, and draw. host_tunnel is merely the first
// consumer.
//
// THE CONTRACT the module enforces so no host ever re-fights it:
//   1. ASSET CONVENTION: the rig faces engine -Z at identity, feet at the
//      origin plane. Jake_44_actions.glb was SURGERY'D to this convention
//      (tools/jake_bake.py: armature -0.9488 Y zeroed, Y-180 facing baked,
//      cm-scale clips repaired, root motion de-drifted). No runtime yFix, no
//      Babylon flip — one value, living in the asset.
//   2. LABELS ARE UNTRUSTED: clips are resolved by EXACT name from a
//      CharacterClipTable whose every entry was MEASURED (the rig ships two
//      clips both named "Walking" and six strafe-family clips; the strafes'
//      directions were verified from the hips root motion, the turns' yaw
//      from the hips rotation — tools/jake_clip_motion.py, jake_hips_stats.py).
//   3. THE CONTACT LAW (NO_SLOP rule 11 v2): feet clamp to the TOPMOST
//      WALKABLE SURFACE — max(terrain height, downward static raycast) —
//      every frame, inside the module. No host can ship a buried character.
//   4. FACING: yaw 0 faces -Z; facing (dx,dz) => yaw = atan2(-dx,-dz)
//      (AXES LAW). Forward movement faces the travel direction; backpedal,
//      strafe and turn-in-place face the camera and play the measured
//      directional clip.
//
// Clean-room: engine-local code only (Skinner/IModelLoader/IPhysicsWorld).

#include "engine/asset/IAssetSource.h"
#include "engine/asset/IModelLoader.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/rhi/IRenderDevice.h"

#include "anim.h"
#include "player.h"

#include <memory>
#include <string>

namespace x3::game {

// ---------------------------------------------------------------------------
// A MEASURED clip table. Names are exact-match (labels untrusted — resolve
// deliberately, never fuzzily); authored speeds (m/s) let playback rate track
// the capsule so feet don't skate; turn deltas are the clips' measured net
// hips yaw (radians, +yaw = turn LEFT in engine space). Null entries degrade
// gracefully (the state falls back to the locomotion blend).
// ---------------------------------------------------------------------------
struct CharacterClipTable {
    const char* idle        = nullptr;
    const char* walk        = nullptr;  float walkSpeed  = 1.5f;
    const char* run         = nullptr;  float runSpeed   = 4.0f;
    const char* jump        = nullptr;
    const char* walkBack    = nullptr;  float walkBackSpeed  = 1.3f;
    const char* runBack     = nullptr;  float runBackSpeed   = 3.2f;
    const char* strafeLeft  = nullptr;  float strafeLeftSpeed  = 1.6f;
    const char* strafeRight = nullptr;  float strafeRightSpeed = 0.9f;
    const char* turnLeft    = nullptr;  float turnLeftRad  = 0.0f;
    const char* turnRight   = nullptr;  float turnRightRad = 0.0f;
    const char* fall        = nullptr;  // airborne >0.4 s and falling
    // JETPACK flight pose (the `fly` command): an exact-named clip HELD at
    // jetFlyHold seconds while the character is airborne in jetpack mode —
    // rigs ship no "flying" loop, so the readable frame of an airborne clip
    // is the pose (Jake: Riflejump's mid-air frame; picked by eye against
    // Fall_Down, which reads as losing balance, not flying).
    const char* jetFly      = nullptr;  float jetFlyHold = 0.0f;
    const char* idleVariant = nullptr;  float idleVariantEvery = 20.0f;
    const char* swim        = nullptr;  // selection ready; swim STATE is the
    const char* swimIdle    = nullptr;  // river lane's (Player::swimming()).

    // ---- COMBAT / RIFLE layer (the weapons task). Exact names, module-owned:
    // hosts flip armed/aiming and call the one-shot helpers; the module owns
    // every clip decision so no host ever re-wires a rig state by hand.
    const char* rifleIdle   = nullptr;  // armed idle / RMB aim loop (upper body on the gun)
    const char* rifleFire   = nullptr;  // firing one-shot (retriggerable at the fire rate)
    const char* rifleReload = nullptr;  // reload one-shot — PAIRED VALUE: the host's
                                        // WeaponDef::reloadTime must equal this clip's
                                        // duration (host_tunnel tunnelRifleRoster()).
    const char* rifleGrenade= nullptr;  // grenade-toss one-shot
    const char* rifleRun    = nullptr;  // armed run loop (swapped into the blend while armed)
    const char* rifleJump   = nullptr;  // armed jump one-shot (replaces `jump` while armed)
};

// The measured Jake_44_actions.glb table (post tools/jake_bake.py bake).
// Direction/yaw measurements live in the .cpp next to the numbers.
CharacterClipTable jakeClipTable();

// ---------------------------------------------------------------------------
// F1 camera cycle: 0 = first person (eye at the Player camera, character mesh
// hidden by the host via draw(visible=false)), 1 = third person near (~2.5 m),
// 2 = third person far (~5.5 m). Mouse-look works in every mode because all
// three orbit the SAME Player look angles. Hosts persist the mode in a cvar
// (e.g. jake_cam) and cycle it on F1.
// ---------------------------------------------------------------------------
enum class CharacterCamMode : int { FirstPerson = 0, ThirdNear = 1, ThirdFar = 2 };
constexpr int kCharacterCamModes = 3;
const char* characterCamModeName(int mode);
// Compute the eye for a mode from the player's own camera (eye/yaw/pitch out).
void characterCameraEye(const Player& player, int mode,
                        float& cx, float& cy, float& cz,
                        float& yaw, float& pitch);

// ---------------------------------------------------------------------------
// The character itself.
// ---------------------------------------------------------------------------
class AnimatedCharacter {
public:
    // Load + bind (the sarah.cpp recipe): mount dir, load GLB, makeDrawables,
    // Skinner bind, root-Y lock, GPU skinning, resolve the clip table by exact
    // name. Returns true if the mesh loaded (animation may still be absent —
    // animated() reports that; a static mesh still draws + clamps).
    bool load(x3::rhi::IRenderDevice& device, const std::string& glbDir,
              const std::string& file, const CharacterClipTable& table);

    bool loaded()   const { return !m_draw.empty(); }
    bool animated() const { return m_animated; }

    // One frame of intent, camera-relative (the Player moves the capsule by
    // camera-relative velocity, so lateral-vs-forward intent comes from HERE,
    // not from the velocity).
    struct Intent {
        float moveFwd     = 0.0f;   // -1..1 (W..S)
        float moveStrafe  = 0.0f;   // -1..1 (A..D; +1 = right)
        bool  sprint      = false;
        bool  jumpPressed = false;
    };

    // Advance one frame: (1) THE CONTACT LAW feet clamp on the capsule,
    // (2) facing update, (3) clip selection — one readable mapping of
    // (intent, grounded, airborne-time, swimming) -> clip, one-shots override,
    // locomotion blend as fallback — and (4) skinning. camYaw is the camera's
    // planar look yaw (device convention: fwd = (cos, ., sin)).
    void update(Player& player, const Intent& in, float camYaw, float dt,
                x3::phys::IPhysicsWorld& phys, x3::rhi::IRenderDevice& device);

    // Draw at the capsule's feet. yawTrimRad / yTrim are the host's live
    // console trims (both default 0 — the asset owns the truth); visible=false
    // skips the mesh (first-person mode).
    void draw(const x3::rhi::FrameContext& frame, x3::rhi::IRenderDevice& device,
              const Player& player, float yawTrimRad, float yTrim, bool visible);

    // One-shot layer (punch/kick/reload/…): play an exact-named clip
    // exclusively once, then return to locomotion. Returns false if the clip
    // is absent or one is already playing — unless `restart` is set, which
    // rewinds an already-playing SAME clip to 0 (rapid fire retrigger) and
    // replaces a DIFFERENT one (fire interrupts reload is the caller's call).
    bool playOneShot(const char* exactName, bool restart = false);
    bool oneShotActive() const { return m_userT >= 0.0f; }
    // Seconds into the active one-shot (-1 when none) — grenade-release timing.
    float oneShotTime() const { return m_userT; }

    // ---- WEAPON layer (the weapons task) --------------------------------
    // Armed: locomotion run swaps to the rifle-run clip, the stationary idle
    // becomes the rifle-ready loop, jump becomes the rifle jump. Aiming
    // additionally faces the CAMERA (fine-aim) even while moving forward.
    void setArmed(bool armed);
    bool armed()  const { return m_armed; }
    // JETPACK mode (the `fly` command): while on and the capsule is airborne,
    // the module holds the table's jetFly pose and faces the camera (you fly
    // where you look). Grounded with the pack on, everything is normal
    // locomotion — the mode only owns the AIR.
    void setJetpack(bool on) { m_jetpackMode = on; }
    bool jetpackMode() const { return m_jetpackMode; }
    void setAiming(bool aiming) { m_aiming = aiming && m_armed; }
    bool aiming() const { return m_aiming; }
    // The rifle one-shots, resolved from the clip table (false if absent).
    bool fireOneShot()    { return playOneShot(m_table.rifleFire, true); }
    bool reloadOneShot()  { return playOneShot(m_table.rifleReload); }
    bool grenadeOneShot() { return playOneShot(m_table.rifleGrenade); }
    // True while the active one-shot is the grenade toss (release timing).
    bool grenadeOneShotActive() const {
        return m_userT >= 0.0f && m_userClip >= 0 && m_userClip == m_rifleGrenade;
    }

    // Named-bone WORLD transform (column-major 4x4) under the SAME draw
    // matrix draw() uses (feet at the capsule, facing m_yaw) — the weapon
    // hand socket. Resolves + caches the node on first call; false when the
    // rig isn't animated / the bone doesn't resolve / no pose computed yet.
    bool boneWorld(const char* boneName, const Player& player,
                   float yawTrimRad, float yTrim, float out[16]);

    // Facing yaw (radians; 0 = engine -Z). Exposed for debug/host cameras.
    float yaw() const { return m_yaw; }

    // The swim clip selection, resolved and ready for the river lane.
    struct SwimClipset { int swim = -1; int swimIdle = -1; };
    SwimClipset swimClipset() const { return { m_swim, m_swimIdle }; }

    // Exact-name clip lookup + the bound skinner (weapon-socket readback,
    // debug). -1 / invalid when not animated.
    int clipIndex(const char* exactName) const;
    const x3::anim::Skinner& skinner() const { return m_skin; }

private:
    int  resolve(const char* name) const;   // exact-name, -1 on null/miss
    void applyExclusive(x3::rhi::IRenderDevice& device, int clip, float t);

    // ---- asset ----
    std::unique_ptr<x3::asset::IAssetSource>  m_src;
    std::unique_ptr<x3::asset::IModelLoader>  m_loader;
    x3::asset::Model                          m_model;
    std::vector<x3::asset::ModelDrawable>     m_draw;
    x3::anim::Skinner                         m_skin;
    bool                                      m_animated = false;
    CharacterClipTable                        m_table;

    // ---- resolved clips (exact-name; -1 = absent) ----
    int m_walkBack = -1, m_runBack = -1, m_strafeL = -1, m_strafeR = -1;
    int m_turnL = -1, m_turnR = -1, m_jump = -1, m_fall = -1;
    int m_idleVar = -1, m_swim = -1, m_swimIdle = -1, m_jetFly = -1;
    // Combat layer (the weapons task). m_idle/m_walk/m_run are ALSO kept so
    // setArmed() can re-register the locomotion blend both ways.
    int m_idle = -1, m_walk = -1, m_run = -1;
    int m_rifleIdle = -1, m_rifleFire = -1, m_rifleReload = -1;
    int m_rifleGrenade = -1, m_rifleRun = -1, m_rifleJump = -1;

    // ---- state ----
    float m_yaw = 0.0f;                       // 0 = engine -Z (asset identity)
    float m_prevFeet[3] = { 0, 0, 0 };
    bool  m_havePrev = false;
    float m_airT = 0.0f;                      // seconds since last grounded
    int   m_moveClip = -1;                    // exclusive directional loop
    float m_moveT = 0.0f;
    float m_jumpT = -1.0f;                    // >=0 while the jump plays
    float m_turnT = -1.0f;                    // >=0 while a turn plays
    int   m_turnClip = -1;
    float m_turnDelta = 0.0f;                 // measured net yaw of that clip
    float m_idleAccum = 0.0f;                 // continuous-idle stopwatch
    float m_idleVarT = -1.0f;                 // >=0 while the variant plays
    int   m_userClip = -1;                    // playOneShot layer
    float m_userT = -1.0f;
    int   m_jumpClip = -1;                    // which jump one-shot is playing
    // Weapon layer state.
    bool  m_armed  = false;
    bool  m_aiming = false;
    // Jetpack layer state (the `fly` command).
    bool  m_jetpackMode = false;
    // boneWorld() cache: TWO resolved nodes (hand socket + spine mount can be
    // queried in the same frame; a one-slot cache re-resolved both per frame).
    struct BoneSlot { std::string name; int node = -1; };
    BoneSlot m_boneCache[2];
};

} // namespace x3::game
