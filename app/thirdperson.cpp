// Third-person view (FIRST MILESTONE). See app/thirdperson.h +
// docs/design/THIRD_PERSON_VIEW.md.
//
// Clean-room: built from the engine interfaces (IRenderDevice / IModelLoader /
// IAssetSource) + the game-layer Skinner (app/anim.*) only. Mirrors the proven
// skinned-character path in monster.cpp + the facing-flip in rescue.cpp; no GPL /
// id Tech / RBDOOM source consulted.
#include "thirdperson.h"
#include "weapon.h"
#include "headless_device.h"
#include "asset_root.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

namespace x3::game {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Live-character scale (the rigged humanoid GLBs read ~1.8-1.9 m at scale 1).
constexpr float kAvatarScale = 1.0f;

// Column-major 4x4 from a 3x3 basis (columns bx,by,bz), uniform scale s, and
// translation t. Identical to monster.cpp / rescue.cpp's composeTRS.
void composeTRS(float m[16],
                const x3::phys::Vec3& bx, const x3::phys::Vec3& by, const x3::phys::Vec3& bz,
                float s, const x3::phys::Vec3& t) {
    m[0]  = bx.x * s; m[1]  = bx.y * s; m[2]  = bx.z * s; m[3]  = 0.0f;
    m[4]  = by.x * s; m[5]  = by.y * s; m[6]  = by.z * s; m[7]  = 0.0f;
    m[8]  = bz.x * s; m[9]  = bz.y * s; m[10] = bz.z * s; m[11] = 0.0f;
    m[12] = t.x;      m[13] = t.y;      m[14] = t.z;      m[15] = 1.0f;
}

// Build the camera look basis from yaw/pitch (CONVENTIONS.md §3).
void lookBasis(float yaw, float pitch, float fwd[3], float right[3], float up[3]) {
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cy = std::cos(yaw),   sy = std::sin(yaw);
    fwd[0] = cp * cy; fwd[1] = sp; fwd[2] = cp * sy;
    // right = normalize(cross(fwd, +Y)). cross((fx,fy,fz),(0,1,0)) = (-fz, 0, fx).
    float rx = -fwd[2], rz = fwd[0];
    float rl = std::sqrt(rx * rx + rz * rz); if (rl < 1e-5f) rl = 1e-5f;
    right[0] = rx / rl; right[1] = 0.0f; right[2] = rz / rl;
    // up = cross(right, fwd)
    up[0] = right[1] * fwd[2] - right[2] * fwd[1];
    up[1] = right[2] * fwd[0] - right[0] * fwd[2];
    up[2] = right[0] * fwd[1] - right[1] * fwd[0];
}

} // namespace

// ===========================================================================
// Pure helpers (testable).
// ===========================================================================
ThirdPersonCamera computeFollowCamera(float feetX, float feetY, float feetZ,
                                      float eyeHeight, float yaw, float pitch,
                                      float distance, float heightAbove) {
    float fwd[3], right[3], up[3];
    lookBasis(yaw, pitch, fwd, right, up);
    // The orbit pivot is the player's HEAD (eye line). The camera sits BEHIND it
    // (along -forward) and a little ABOVE; it looks toward the head along +forward
    // (the SAME look direction the player aims along, so FP and 3P aim agree).
    const float headX = feetX, headY = feetY + eyeHeight, headZ = feetZ;
    ThirdPersonCamera c;
    c.camX = headX - fwd[0] * distance + up[0] * heightAbove;
    c.camY = headY - fwd[1] * distance + up[1] * heightAbove;
    c.camZ = headZ - fwd[2] * distance + up[2] * heightAbove;
    c.yaw   = yaw;
    c.pitch = pitch;
    return c;
}

LocoBand selectLocoBand(float planarSpeed, float walkThreshold, float runThreshold) {
    if (planarSpeed < walkThreshold) return LocoBand::Idle;
    if (planarSpeed < runThreshold)  return LocoBand::Walk;
    return LocoBand::Run;
}

// ===========================================================================
// ThirdPersonView
// ===========================================================================
void ThirdPersonView::build(Scene& scene, x3::rhi::IRenderDevice& device,
                            std::string_view modelDir) {
    m_device = &device;
    for (int i = 0; i < 16; ++i) m_modelFixup[i] = (i % 5 == 0) ? 1.0f : 0.0f;

    const std::string file = kJakeModelFile;
    m_assets.reset(x3::asset::createAssetSource());
    const bool mounted = m_assets->mountDir(std::string(modelDir), 0);
    if (mounted) {
        m_loader.reset(x3::asset::createModelLoader(&device, m_assets.get()));
        m_model = m_loader->load(file);
        if (m_model.ok)
            m_drawables = x3::asset::makeDrawables(m_model);
    } else {
        x3::logWarn("[3p] mountDir failed: " + std::string(modelDir));
    }

    if (m_drawables.empty()) {
        // No avatar: the system stays unbuilt and FP keeps working unchanged.
        x3::logWarn("[3p] Jake (" + file + ") load failed — third-person avatar unavailable "
                    "(FP unaffected)");
        m_built = false;
        return;
    }

    m_modelScale = kAvatarScale;

    // ---- Bind the skeletal-animation runtime (same as monster.cpp). Jake is a
    // genuinely skinned multi-clip rig, so this drives the locomotion blend. ----
    if (m_model.ok && m_skinner.bind(m_model)) {
        const bool gpuSkin = m_skinner.enableGpuSkinning(device, m_model);
        x3::logInfo(std::string("[3p] Jake -> ") +
                    (gpuSkin ? "GPU-SKINNED (compute pre-pass)"
                             : "CPU-SKINNED FALLBACK (per-frame updateMesh)"));
        // Prefer plain "Idle" over "Rifleaimingidle" for default standing — the rifle-aim
        // idle is a low/combat-ready stance that reads as "crouched/angles wrong" when the
        // player isn't aiming (Tim playtest 2026-05-27). Rifleaimingidle is still resolved
        // separately into m_rifleIdleClip below for the fire-held cross-fade.
        m_idleClip      = m_skinner.findClip({ "idle", "stand", "breath", "rifleaimingidle" });
        // Prefer the RIFLE-holding move clips (hands grip the gun): Riflerun for run,
        // Walking for walk, Runbackwards/Walkingbackwards for reverse.
        m_walkClip      = m_skinner.findClip({ "walking", "walk" });
        m_runClip       = m_skinner.findClip({ "riflerun", "run", "sprint", "jog" });
        m_runBackClip   = m_skinner.findClip({ "runbackwards", "walkingbackwards" });
        m_walkBackClip  = m_skinner.findClip({ "walkingbackwards", "walkbackwards" });
        m_rifleIdleClip = m_skinner.findClip({ "rifleaimingidle", "idle" });
        m_fireClip      = m_skinner.findClip({ "firingrifle", "fire" });
        // THE SWIM CLIPS (tools/swim_bake.py). Exact names win in findClip, so
        // "swimidle" resolves before the fuzzy "swim" substring can steal it.
        m_swimIdleClip  = m_skinner.findClip({ "swimidle", "tread" });
        m_swimClip      = m_skinner.findClip({ "swim", "stroke", "breaststroke" });
        if (m_swimClip == m_swimIdleClip) m_swimClip = -1;   // only SwimIdle exists
        if (m_idleClip < 0) m_idleClip = 0;
        m_useLocoBlend = (m_walkClip >= 0 || m_runClip >= 0);
        if (m_useLocoBlend)
            // Walk threshold lowered 1.5 -> 0.2 m/s so any real movement triggers
            // the walk clip (Tim playtest 2026-05-27: characters were sliding/idle
            // instead of walking, the FPS player's nominal speed wasn't reaching
            // the old threshold visibly). Run kicks in at 2 m/s = comfortable jog.
            m_skinner.setLocomotionClips(m_idleClip, m_walkClip, m_runClip, 0.2f, 2.0f);

        // Resolve the weapon-hand socket bone ONCE (per-frame reads are index-based).
        m_handNode = m_skinner.resolveNodeByName(m_model, kJakeHandBone);

        std::string clipList;
        for (uint32_t c = 0; c < m_skinner.clipCount(); ++c)
            clipList += (c ? ", " : "") + std::string(m_skinner.clipName(c));
        x3::logInfo("[3p] Jake bones=" + std::to_string(m_skinner.nodeCount()) +
                    " clips=" + std::to_string(m_skinner.clipCount()) +
                    " hand=" + kJakeHandBone +
                    (m_handNode >= 0 ? ("(node " + std::to_string(m_handNode) + ")") : "(UNRESOLVED)") +
                    " idle=" + std::to_string(m_idleClip) +
                    " walk=" + std::to_string(m_walkClip) +
                    " run=" + std::to_string(m_runClip) +
                    " swim=" + std::to_string(m_swimClip) +
                    " swimIdle=" + std::to_string(m_swimIdleClip));
        x3::logInfo("[3p] Jake clips: " + clipList);
        if (m_swimClip < 0)
            x3::logWarn("[3p] no SWIM clip on this Jake GLB — the swim read degrades to the "
                        "walk-at-" + std::to_string(kTpSwimAnimRate) + " stand-in "
                        "(re-bake: tools/swim_bake.py, then asset_store.py publish)");

        // Pose the bind mesh into idle at t=0 so the first 3P frame already animates.
        if (m_device) {
            if (m_useLocoBlend) {
                m_skinner.setLocomotionSpeed(0.0f);
                m_skinner.applyLocomotion(m_model, *m_device, 0.0f);
            } else {
                m_skinner.apply(m_model, *m_device, (uint32_t)m_idleClip, 0.0f);
            }
        }

        // ---- Skeleton-based FIT (Tim 2026-05-27): Jake_22_actions.glb has chronic
        // XYZ authoring issues across every project that's tried to use it ("they
        // NEVER EVER NEVER got it right"). Don't trust the authored origin/scale;
        // instead READ THE BONES from the freshly-applied bind pose and derive
        // scale (so the avatar renders 1.7m tall regardless of authored size) +
        // Y-shift (so feet sit at the entity origin = the player's feet, regardless
        // of where the GLB origin actually is). This is the same fix recipe the
        // Babylon ttt-model-loader.js used, but driven by the Mixamo SKELETON
        // (deterministic) instead of mesh bounding-box measurement (which Tim
        // notes "never got it right" either). Axes/handedness fixes (if Jake is
        // laying sideways / facing wrong) are a follow-on once the user eyeballs
        // the result. ----
        if (m_device) {
            const int toeNode  = m_skinner.resolveNodeByName(m_model, "mixamorigLeftToeBase");
            const int headNode = m_skinner.resolveNodeByName(m_model, "mixamorigHead");
            float toeMat[16], headMat[16];
            if (toeNode >= 0 && headNode >= 0 &&
                m_skinner.boneGlobal((uint32_t)toeNode, toeMat) &&
                m_skinner.boneGlobal((uint32_t)headNode, headMat)) {
                const float toeY  = toeMat[13];   // column-major: translation Y at index 13
                const float headY = headMat[13];
                const float H     = headY - toeY;
                if (H > 0.1f) {
                    constexpr float kTargetHeight = 1.7f;
                    m_modelScale = kTargetHeight / H;
                    m_modelFixup[13] = -toeY;     // shift feet to origin in model-space; drawXform scales after
                    // Also zero out any XZ offset baked into the GLB root/armature node by
                    // reading the HIP bone's bind-pose world XZ and shifting those out, so
                    // Jake's body center lands at the player's XZ (not offset to one side of
                    // the camera frame as observed in playtest 2026-05-27).
                    const int hipsNode = m_skinner.resolveNodeByName(m_model, "mixamorigHips");
                    float hipMat[16];
                    if (hipsNode >= 0 && m_skinner.boneGlobal((uint32_t)hipsNode, hipMat)) {
                        const float hipX = hipMat[12], hipZ = hipMat[14];
                        m_modelFixup[12] = -hipX;
                        m_modelFixup[14] = -hipZ;
                        x3::logInfo("[3p] Jake XZ center on hips: hipX=" + std::to_string(hipX) +
                                    " hipZ=" + std::to_string(hipZ) +
                                    " -> xShift=" + std::to_string(m_modelFixup[12]) +
                                    " zShift=" + std::to_string(m_modelFixup[14]));
                    }
                    x3::logInfo("[3p] Jake skeleton fit: toeY=" + std::to_string(toeY) +
                                " headY=" + std::to_string(headY) +
                                " H=" + std::to_string(H) +
                                " -> scale=" + std::to_string(m_modelScale) +
                                " yShift=" + std::to_string(m_modelFixup[13]));
                } else {
                    x3::logWarn("[3p] Jake skeleton fit: bad height H=" + std::to_string(H) +
                                " (toe/head bones at same Y?) — keeping default scale/fixup");
                }
            } else {
                x3::logWarn("[3p] Jake skeleton fit: bones not found (toeNode=" +
                            std::to_string(toeNode) + " headNode=" + std::to_string(headNode) +
                            ") — keeping default scale/fixup");
            }
        }
    } else {
        x3::logWarn("[3p] Jake loaded but is not skinnable — avatar will draw statically");
    }

    // ---- Avatar Entity: bookkeeping only (Tag::Prop, invalid render mesh so
    // Scene::render skips it; drawAvatar() owns the multi-primitive skinned draw). ----
    Entity e;
    e.tag     = (uint32_t)Tag::Prop;
    e.visible = false;   // hidden in FP (the default)
    composeTRS(e.transform,
               x3::phys::Vec3{1,0,0}, x3::phys::Vec3{0,1,0}, x3::phys::Vec3{0,0,1},
               m_modelScale, m_pos);
    m_entity = scene.add(e);

    m_built = true;
    x3::logInfo("[3p] third-person avatar (Jake) built — " +
                std::to_string(m_drawables.size()) + " primitive(s); FP is the default "
                "(press the toggle to switch)");
}

void ThirdPersonView::setThirdPerson(bool on) {
    const bool entering = on && !m_thirdPerson;
    m_thirdPerson = on;
    // Show/hide the avatar Entity so Scene-level bookkeeping reflects the mode (the
    // actual draw is gated by avatarVisible() in drawAvatar()).
    // (Entity visibility is toggled in update()/drawAvatar via avatarVisible().)
    //
    // BUG A fix — reset the synthesized-crouch + aim smoothing when ENTERING 3P. The
    // crouch/aim amounts are only driven while update() runs (3P active); in FP they
    // FREEZE at their last value. So crouching in 3P, toggling to FP, then back to 3P
    // would re-enter with a stale m_crouchAmt ~ 1.0 — bakeTransform() leans the basis
    // and the avatar spawns/re-enters visibly TILTED even though the player is
    // standing. Zeroing here guarantees a freshly-shown (standing) avatar is upright;
    // update() then smooths back in if the player actually is crouched.
    if (entering) {
        m_crouchAmt = 0.0f;
        m_aimAmt    = 0.0f;
        m_swimAmt       = 0.0f;   // same staleness rule for the swim pose
        m_swimStrokeAmt = 0.0f;
        m_swimStroking  = false;
    }
}

void ThirdPersonView::bakeTransform(Scene& scene) {
    // FACING FLIP (matches rescue.cpp/monster.cpp): the rigged GLBs are authored
    // facing +Z, but m_yaw assumes local -Z forward (CONVENTIONS) — so flip the
    // VISUAL yaw 180deg here only. m_yaw stays the logical heading.
    // Yaw: the 180-deg facing-flip + the runtime cvar offset (jake_yawoff_deg).
    const float ry = m_yaw + kPi + m_userYawOff;
    const float c = std::cos(ry), s = std::sin(ry);
    // Yaw-only basis columns (the avatar's local right/up/forward in world space).
    x3::phys::Vec3 bx{ c, 0.0f, -s };       // local +X (right) after yaw
    x3::phys::Vec3 by{ 0.0f, 1.0f, 0.0f };  // local +Y (up)
    x3::phys::Vec3 bz{ s, 0.0f, c };        // local +Z (forward) after yaw

    // ---- CROUCH (synthesized; no crouch clip on the rig — TASK#46.2): while
    // crouched, drop the avatar (hip lower) + lean the upper body forward by
    // tilting the whole basis about its LOCAL-RIGHT axis. m_crouchAmt is the
    // smoothed 0..1 amount (driven in update()). At amt=0 this is a no-op so the
    // standing pose is byte-for-byte unchanged. A real retargeted crouch clip is
    // the ideal future fix; this reads acceptably as a squat in the meantime. ----
    x3::phys::Vec3 posAdj = m_pos;
    posAdj.y += m_userYOff;
    if (m_crouchAmt > 1e-3f) {
        posAdj.y -= kTpCrouchDrop * m_crouchAmt;
        // Lean: rotate the up/forward columns about the local-right axis (bx).
        const float lean = kTpCrouchLeanDeg * (kPi / 180.0f) * m_crouchAmt;
        const float cl = std::cos(lean), sl = std::sin(lean);
        const x3::phys::Vec3 fwd = bz, up = by;
        bz = x3::phys::Vec3{ fwd.x * cl + up.x * sl, fwd.y * cl + up.y * sl, fwd.z * cl + up.z * sl };
        by = x3::phys::Vec3{ up.x * cl - fwd.x * sl, up.y * cl - fwd.y * sl, up.z * cl - fwd.z * sl };
    }
    // ---- SWIM (v2): the baked clip does the LIMBS; the basis LAYS HIM IN THE
    // WATER. Pitch the whole basis toward horizontal about the local-right axis
    // (the same lean math as crouch) so the avatar lies belly-down along the look
    // direction, and float the body up so it rides the surface line. The angle +
    // lift ease between the TREADING read (upright, only the head/shoulders out)
    // and the STROKING read (nearly flat, back at the surface) by the smoothed
    // stroke amount. Smoothed by m_swimAmt so the pose eases in on entry and
    // upright restores on exit — a no-op at 0. ----
    if (m_swimAmt > 1e-3f) {
        const float sA = m_swimStrokeAmt;   // 0 = treading, 1 = stroking
        const float proneDeg = kTpSwimTreadDeg + (kTpSwimProneDeg - kTpSwimTreadDeg) * sA;
        const float rise     = kTpSwimTreadRise + (kTpSwimRise - kTpSwimTreadRise) * sA;
        posAdj.y += rise * m_swimAmt;
        // NEGATIVE lean = belly-DOWN (the crouch sign at ~84 deg reads as a
        // back-float: the knees flex up out of the water — eyeballed on the
        // swim_3p_prone proof shot; flipped so the stroke kicks into the water).
        const float lean = -proneDeg * (kPi / 180.0f) * m_swimAmt;
        const float cl = std::cos(lean), sl = std::sin(lean);
        const x3::phys::Vec3 fwd = bz, up = by;
        bz = x3::phys::Vec3{ fwd.x * cl + up.x * sl, fwd.y * cl + up.y * sl, fwd.z * cl + up.z * sl };
        by = x3::phys::Vec3{ up.x * cl - fwd.x * sl, up.y * cl - fwd.y * sl, up.z * cl - fwd.z * sl };
    }
    composeTRS(m_drawXform, bx, by, bz, m_modelScale, posAdj);
    if (m_entity != kNoLink && m_entity < scene.size()) {
        Entity& me = scene.get(m_entity);
        std::memcpy(me.transform, m_drawXform, 16 * sizeof(float));
    }
}

void ThirdPersonView::update(float dt, Scene& scene, const x3::phys::Vec3& feet,
                             float eyeHeight, float yaw, float pitch, uint32_t roomId,
                             bool crouched, bool fireHeld, bool swimming) {
    (void)pitch;
    if (!m_built) return;

    // Place the avatar at the player's feet.
    m_pos = feet;

    // ---- Planar speed from the feet delta (the controller owns movement; we just
    // measure it for the locomotion blend, exactly like the monster AI does). ----
    float planarSpeed = 0.0f;
    float moveX = 0.0f, moveZ = 0.0f;
    if (m_havePrev && dt > 1e-5f) {
        const float dx = m_pos.x - m_prevPos.x, dz = m_pos.z - m_prevPos.z;
        planarSpeed = std::sqrt(dx * dx + dz * dz) / dt;
        moveX = dx; moveZ = dz;
    }
    m_prevPos = m_pos;
    m_havePrev = true;

    // ---- Facing + movement direction RELATIVE to the look (BUG B fix). The avatar
    // ALWAYS faces the player's look yaw — backing up or strafing must NOT spin the
    // body to face the movement vector (that 180-deg whip-around was the bug). We
    // instead decompose the planar movement onto the look's forward/right axes to
    // get a signed forward component; backpedalling then selects the BACKWARDS walk/
    // run clip while the body stays forward-facing (a real backpedal read). The rig
    // has no strafe clip, so lateral-dominant motion keeps the forward clip set
    // (facing forward) — acceptable until a strafe clip is retargeted. ----
    m_yaw = yaw;   // body faces the camera/look direction at all times
    const bool moving = (moveX * moveX + moveZ * moveZ) > 1e-6f && planarSpeed > 0.15f;
    // Look forward on the XZ plane (CONVENTIONS: forward = (cos yaw, *, sin yaw)).
    const float fX = std::cos(yaw), fZ = std::sin(yaw);
    // Signed forward component of the move (m, this frame). >0 = forward, <0 = back.
    const float fwdComp = moving ? (moveX * fX + moveZ * fZ) : 0.0f;
    // Backpedalling: moving and the motion is predominantly opposite the look dir.
    // (Compare the forward component's magnitude against the planar move magnitude so
    //  pure strafing — fwdComp ~ 0 — is NOT treated as "backwards".)
    const float moveMag = std::sqrt(moveX * moveX + moveZ * moveZ);
    const bool movingBack = moving && fwdComp < 0.0f &&
                            (-fwdComp) > 0.5f * moveMag;   // back dominates lateral

    // Avatar belongs to the player's room so the PVS cull keeps it visible.
    if (m_entity != kNoLink && m_entity < scene.size()) {
        Entity& me = scene.get(m_entity);
        me.roomId  = roomId;
        me.visible = avatarVisible();
    }

    // ---- SWIM STATE (v2), latched BEFORE the animation drive reads it. Stroking
    // vs treading is the planar water speed with hysteresis (kTpSwimStrokeOn/Off),
    // so a swimmer drifting to a stop doesn't flicker between the two clips; the
    // smoothed amount then crossfades the PRONE angle + float in bakeTransform()
    // (a treading swimmer is far more upright than a stroking one). ----
    if (!swimming) {
        m_swimStroking = false;
    } else if (m_swimStroking) {
        if (planarSpeed < kTpSwimStrokeOff) m_swimStroking = false;
    } else {
        if (planarSpeed > kTpSwimStrokeOn)  m_swimStroking = true;
    }
    {
        const float target = (swimming && m_swimStroking) ? 1.0f : 0.0f;
        const float k = 1.0f - std::exp(-kTpSwimStrokeBlend * (dt > 0.0f ? dt : 0.0f));
        m_swimStrokeAmt += (target - m_swimStrokeAmt) * k;
        if (m_swimStrokeAmt < 1e-4f)   m_swimStrokeAmt = 0.0f;
        if (m_swimStrokeAmt > 0.9999f) m_swimStrokeAmt = 1.0f;
    }

    // ---- Drive the animation. In 3P drive the locomotion blend from the planar
    // speed (rifle-holding clips keep the hands on the gun); when firing + standing,
    // play the rifle aim/fire clip as a nice touch (cheap one-shot). Crouch uses a
    // lower scale fallback if no crouch clip exists (Jake has none). ----
    if (m_thirdPerson && m_device && m_skinner.valid()) {
        if (swimming && m_useLocoBlend && m_swimClip >= 0) {
            // ---- SWIM v2: play the REAL baked clip. triggerClip() crossfades the
            // locomotion blend out and holds the target clip LOOPING (idempotent —
            // re-requesting the same clip each frame is a true no-op), so the
            // stroke owns the whole pose. Stroking vs treading is chosen by the
            // planar water speed with hysteresis (m_swimStroking, latched below),
            // and the underlying locomotion blend is parked at idle so the
            // crossfade has something neutral to sit on.
            const int clip = (m_swimStroking || m_swimIdleClip < 0) ? m_swimClip
                                                                    : m_swimIdleClip;
            m_skinner.setLocomotionSpeed(0.0f);
            m_skinner.triggerClip(clip, 0.25f, /*loop*/true);
            m_skinner.applyLocomotion(m_model, *m_device, dt * kTpSwimClipRate);
        } else if (swimming && m_useLocoBlend) {
            // ---- SWIM, DEGRADED (an old GLB with no Swim clip): the historical
            // stand-in — the walk cycle at kTpSwimAnimRate. Pin the blend into the
            // WALK band regardless of the actual water speed (buoyant drift would
            // otherwise idle the limbs mid-stroke), cancel any fire pose, and
            // advance the clip clock slow.
            m_skinner.triggerClip(-1, 0.15f);
            m_skinner.setLocomotionSpeed(1.0f);   // solidly in the walk band (0.2..2.0)
            m_skinner.applyLocomotion(m_model, *m_device, dt * kTpSwimAnimRate);
        } else if (m_useLocoBlend) {
            // Swap the locomotion walk/run clip set forward<->backward so backing up
            // plays the backpedal clip with the body STILL FACING FORWARD (BUG B). Only
            // re-register on an actual direction change (cheap; avoids per-frame churn).
            const bool wantBack = movingBack &&
                                  (m_walkBackClip >= 0 || m_runBackClip >= 0);
            if (wantBack != m_locoBackActive) {
                const int wlk = wantBack && m_walkBackClip >= 0 ? m_walkBackClip : m_walkClip;
                const int run = wantBack && m_runBackClip  >= 0 ? m_runBackClip  : m_runClip;
                m_skinner.setLocomotionClips(m_idleClip, wlk, run, 0.2f, 2.0f);
                m_locoBackActive = wantBack;
            }
            m_skinner.setLocomotionSpeed(planarSpeed);
            // Firing while basically stationary: nudge toward the rifle aim/fire pose
            // via a short crossfade (loops while held; idempotent so re-requesting the
            // same clip each frame is a true no-op — see Skinner::triggerClip). Cancel
            // back to locomotion whenever we don't want the fire pose (released OR
            // moving fast enough that locomotion should win), so it never sticks.
            const bool wantFirePose = fireHeld && planarSpeed < 0.4f && m_fireClip >= 0;
            if (wantFirePose) m_skinner.triggerClip(m_fireClip, 0.12f, /*loop*/true);
            else              m_skinner.triggerClip(-1, 0.15f);   // cancel -> locomotion
            m_skinner.applyLocomotion(m_model, *m_device, dt);
        } else {
            const int fwdClip = (planarSpeed > 0.25f && m_walkClip >= 0) ? m_walkClip : m_idleClip;
            const int clip = (movingBack && m_walkBackClip >= 0) ? m_walkBackClip : fwdClip;
            m_animTime += dt;
            m_skinner.apply(m_model, *m_device, (uint32_t)clip, m_animTime);
        }
    }

    // ---- CROUCH (TASK#46.2): Jake has no crouch clip in this 22-clip rig, so we
    // SYNTHESIZE one — smoothly drive a 0..1 crouch amount toward the `crouched`
    // flag; bakeTransform() turns it into a hip drop + forward lean so the avatar
    // visibly squats instead of doing nothing. Exponential smoothing avoids a pop.
    // (A real retargeted crouch clip is the ideal future fix — see thirdperson.h.) --
    {
        const float target = crouched ? 1.0f : 0.0f;
        const float k = 1.0f - std::exp(-kTpCrouchBlend * (dt > 0.0f ? dt : 0.0f));
        m_crouchAmt += (target - m_crouchAmt) * k;
        if (m_crouchAmt < 1e-4f) m_crouchAmt = 0.0f;
        if (m_crouchAmt > 0.9999f) m_crouchAmt = 1.0f;
    }

    // ---- SWIM (v2): smooth a 0..1 in-the-water amount toward the `swimming`
    // flag; bakeTransform() pitches the basis prone + floats the body to the
    // surface line. dt-scaled exponential blend (no pop on enter/exit). ----
    {
        const float target = swimming ? 1.0f : 0.0f;
        const float k = 1.0f - std::exp(-kTpSwimBlend * (dt > 0.0f ? dt : 0.0f));
        m_swimAmt += (target - m_swimAmt) * k;
        if (m_swimAmt < 1e-4f) m_swimAmt = 0.0f;
        if (m_swimAmt > 0.9999f) m_swimAmt = 1.0f;
    }

    // ---- OVER-THE-SHOULDER AIM (TASK#46.3): smooth a 0..1 aim amount toward
    // `fireHeld`; camera() biases the follow cam subtly over the right shoulder by
    // this amount so the avatar body doesn't block the crosshair. Kept gentle. ----
    {
        const float target = fireHeld ? 1.0f : 0.0f;
        const float k = 1.0f - std::exp(-kTpAimBlend * (dt > 0.0f ? dt : 0.0f));
        m_aimAmt += (target - m_aimAmt) * k;
        if (m_aimAmt < 1e-4f) m_aimAmt = 0.0f;
        if (m_aimAmt > 0.9999f) m_aimAmt = 1.0f;
    }

    bakeTransform(scene);
}

ThirdPersonCamera ThirdPersonView::camera(const x3::phys::Vec3& feet, float eyeHeight,
                                          float yaw, float pitch) const {
    // Base follow cam: behind + above the head. When aiming, pull it in a touch and
    // bias it over the RIGHT shoulder so the avatar body clears the crosshair. The
    // shift is along the camera's RIGHT vector + a forward pull, scaled by m_aimAmt
    // (smoothed), so it eases in/out and is a no-op at rest (follow cam unchanged).
    const float dist = m_camDist - kTpAimShoulderIn * m_aimAmt;
    ThirdPersonCamera c = computeFollowCamera(feet.x, feet.y, feet.z, eyeHeight,
                                              yaw, pitch, dist, m_camHeight);
    if (m_aimAmt > 1e-3f) {
        float fwd[3], right[3], up[3];
        lookBasis(yaw, pitch, fwd, right, up);
        const float r = kTpAimShoulderRight * m_aimAmt;
        c.camX += right[0] * r;
        c.camY += right[1] * r;
        c.camZ += right[2] * r;
    }
    return c;
}


void ThirdPersonView::drawAvatar(x3::rhi::IRenderDevice& device,
                                 const x3::rhi::FrameContext& frame,
                                 const Scene& scene) const {
    if (!avatarVisible()) return;
    if (m_entity == kNoLink || m_entity >= scene.size()) return;
    const Entity& e = scene.get(m_entity);
    if (!e.visible) return;
    // final = drawXform * fixup * nodeTransform (matches monster's drawMonsterAt).
    // The skinned vertices already carry the animated pose (the Skinner re-uploaded
    // them / set the GPU palette in update()); here we just place + draw them.
    for (const auto& d : m_drawables) {
        float mf[16], fin[16];
        x3::asset::mulMat4(m_drawXform, m_modelFixup, mf);
        x3::asset::mulMat4(mf, d.nodeTransform, fin);
        device.drawMesh(frame,
                        x3::rhi::MeshHandle{ d.meshId },
                        x3::rhi::TextureHandle{ d.baseColorTexId },
                        d.baseColorFactor, fin);
    }
}

bool ThirdPersonView::handSocketWorld(float out[16], std::string_view weaponName) const {
    if (!m_built || m_handNode < 0 || !out) return false;
    float boneGlobal[16];
    if (!m_skinner.boneGlobal((uint32_t)m_handNode, boneGlobal)) return false;
    // World hand frame = avatarDraw * fixup * boneGlobal. (boneGlobal is the bone's
    // MODEL-space transform from the current pose; fixup is identity for Jake.)
    float mf[16], handWorld[16];
    x3::asset::mulMat4(m_drawXform, m_modelFixup, mf);
    x3::asset::mulMat4(mf, boneGlobal, handWorld);
    // ---- PER-WEAPON GRIP (TASK#46.1): each gun seats differently in the palm.
    // Look up the per-weapon row (pistol / chaingun / shotgun / ...) and build it as
    // a TRS in the hand's LOCAL frame: rotation (small twist/tilt/roll to square the
    // authored barrel onto the grip) then translation (right/down/forward into the
    // palm). hand-local axes: +X right, +Y up, +Z forward (down the barrel). ----
    // LIVE-TUNE override (TASK#53): add the cvar-driven deltas to the table row for
    // the CURRENT weapon so Tim can dial it by eye, then bake. Zero deltas (default)
    // reproduce the baked row byte-for-byte. See effectiveGrip()/the BAKE block in
    // thirdperson.h. Position deltas: x=right, y=down, z=forward (hand-local).
    const TpGrip& g = tpGripFor(weaponName);
    const float gForward = g.forward  + m_gripOvZ;
    const float gRight   = g.right    + m_gripOvX;
    const float gDown    = g.down     + m_gripOvY;
    const float gYawDeg  = g.yawDeg   + m_gripOvYaw;
    const float gPitchD  = g.pitchDeg + m_gripOvPitch;
    const float gRollDeg = g.rollDeg  + m_gripOvRoll;
    const float cy = std::cos(gYawDeg  * (kPi / 180.0f)), sy = std::sin(gYawDeg  * (kPi / 180.0f));
    const float cp = std::cos(gPitchD  * (kPi / 180.0f)), sp = std::sin(gPitchD  * (kPi / 180.0f));
    const float cr = std::cos(gRollDeg * (kPi / 180.0f)), sr = std::sin(gRollDeg * (kPi / 180.0f));
    // R = Ry(yaw) * Rx(pitch) * Rz(roll) (intrinsic), column-major.
    float Rz[16] = { cr, sr, 0,0,  -sr, cr, 0,0,  0,0,1,0,  0,0,0,1 };
    float Rx[16] = { 1,0,0,0,  0, cp, sp, 0,  0,-sp, cp, 0,  0,0,0,1 };
    float Ry[16] = { cy,0,-sy,0,  0,1,0,0,  sy,0,cy,0,  0,0,0,1 };
    float RxRz[16], grip[16];
    x3::asset::mulMat4(Rx, Rz, RxRz);
    x3::asset::mulMat4(Ry, RxRz, grip);
    grip[12] = gRight; grip[13] = -gDown; grip[14] = gForward;
    float out2[16];
    x3::asset::mulMat4(handWorld, grip, out2);
    std::memcpy(out, out2, 16 * sizeof(float));
    return true;
}

void ThirdPersonView::effectiveGrip(std::string_view weaponName, float& fwd,
                                    float& right, float& down, float& yawDeg,
                                    float& pitchDeg, float& rollDeg,
                                    float& scaleMul) const {
    // The ABSOLUTE grip for the HUD readout = the baked table row + the live cvar
    // override. These are the numbers Tim bakes into kTpGripTable for this weapon.
    const TpGrip& g = tpGripFor(weaponName);
    fwd      = g.forward  + m_gripOvZ;
    right    = g.right    + m_gripOvX;
    down     = g.down     + m_gripOvY;
    yawDeg   = g.yawDeg   + m_gripOvYaw;
    pitchDeg = g.pitchDeg + m_gripOvPitch;
    rollDeg  = g.rollDeg  + m_gripOvRoll;
    scaleMul = std::max(0.01f, g.scaleMul + m_gripOvScale);
}

void ThirdPersonView::drawHeldWeapon(x3::rhi::IRenderDevice& device,
                                     const x3::rhi::FrameContext& frame,
                                     const Scene& scene, const Arsenal& arsenal,
                                     bool armed) const {
    if (!avatarVisible() || !armed) return;
    if (m_handNode < 0) return;
    if (!arsenal.viewmodelsLoaded() || !arsenal.currentHasDrawables()) return;
    if (m_entity == kNoLink || m_entity >= scene.size()) return;
    if (!scene.get(m_entity).visible) return;

    const std::string& wname = arsenal.current().name;
    float handWorld[16];
    if (!handSocketWorld(handWorld, wname)) return;
    // Apply the per-weapon viewmodel scale (folded with the global held mul + the
    // per-weapon grip scaleMul) onto the hand frame so the gun reads about right in
    // the palm. Scale the upper-3x3.
    // LIVE-TUNE (TASK#53): add the cvar scale delta onto the row's scaleMul for the
    // current weapon (clamped >0 so a too-negative dial can't invert the model).
    const float scaleMul = std::max(0.01f, tpGripFor(wname).scaleMul + m_gripOvScale);
    const float s = arsenal.currentViewmodelScale() * kTpHeldWeaponScaleMul * scaleMul;
    float scaled[16];
    std::memcpy(scaled, handWorld, 16 * sizeof(float));
    for (int col = 0; col < 3; ++col)
        for (int row = 0; row < 3; ++row)
            scaled[col * 4 + row] *= s;
    arsenal.drawCurrentAt(device, frame, scaled);
}

bool ThirdPersonView::heldMuzzleWorld(const Scene& scene, const Arsenal& arsenal,
                                      x3::phys::Vec3& out) const {
    // Mirror drawHeldWeapon()'s gates EXACTLY: if the gun is not in Jake's hand this frame,
    // the 3P muzzle does not exist and the caller must use the FP barrel tip.
    if (!avatarVisible() || m_handNode < 0) return false;
    if (!arsenal.viewmodelsLoaded() || !arsenal.currentHasDrawables()) return false;
    if (m_entity == kNoLink || m_entity >= scene.size()) return false;
    if (!scene.get(m_entity).visible) return false;

    const std::string& wname = arsenal.current().name;
    float handWorld[16];
    if (!handSocketWorld(handWorld, wname)) return false;
    // Same scale fold as drawHeldWeapon — the barrel tip must ride the SAME matrix the
    // gun's vertices do, or the fire leaves from beside the model again.
    const float scaleMul = std::max(0.01f, tpGripFor(wname).scaleMul + m_gripOvScale);
    const float s = arsenal.currentViewmodelScale() * kTpHeldWeaponScaleMul * scaleMul;
    const x3::phys::Vec3 m = arsenal.currentMuzzleLocal();
    const float lx = m.x * s, ly = m.y * s, lz = m.z * s;
    out = x3::phys::Vec3{
        handWorld[0] * lx + handWorld[4] * ly + handWorld[8]  * lz + handWorld[12],
        handWorld[1] * lx + handWorld[5] * ly + handWorld[9]  * lz + handWorld[13],
        handWorld[2] * lx + handWorld[6] * ly + handWorld[10] * lz + handWorld[14] };
    return true;
}

// ===========================================================================
// Headless self-test (--test-thirdperson). Mechanics only; no window / Vulkan.
// ===========================================================================
namespace {

int g_tpPass = 0, g_tpFail = 0;
void tpcheck(bool cond, const char* name) {
    if (cond) { ++g_tpPass; x3::logInfo(std::string("[3p-test] PASS ") + name); }
    else      { ++g_tpFail; x3::logError(std::string("[3p-test] FAIL ") + name); }
}

float vlen(float x, float y, float z) { return std::sqrt(x * x + y * y + z * z); }

} // namespace

bool runThirdPersonSelfTest() {
    g_tpPass = g_tpFail = 0;

    // ---- TP1..TP3: the PURE follow-camera math (no asset needed). ------------
    {
        // Player at origin, eye 1.6 m, looking along world -Z (yaw = -pi/2 per
        // CONVENTIONS), level pitch. The camera must sit BEHIND (toward +Z, since
        // forward is -Z) and slightly above, and look toward the player.
        const float yaw = -kPi * 0.5f, pitch = 0.0f, eye = 1.6f;
        ThirdPersonCamera c = computeFollowCamera(0, 0, 0, eye, yaw, pitch,
                                                  kTpCamDistance, kTpCamHeightAbove);
        // forward = (cos p cos y, sin p, cos p sin y) = (0,0,-1). Behind = -forward*d
        // = +Z*d, so the camera is at z ~ +kTpCamDistance, above the eye line.
        const bool behind = c.camZ > kTpCamDistance - 0.6f;       // ~ +3.6 in Z
        const bool above  = c.camY > eye + 0.3f;                  // raised above the eye
        tpcheck(behind, "TP1 follow camera sits BEHIND the player (-forward)");
        tpcheck(above,  "TP2 follow camera sits ABOVE the eye line");

        // The camera must LOOK TOWARD the player head: the vector cam->head should be
        // roughly parallel to the look forward (dot > 0 and large).
        const float headX = 0, headY = eye, headZ = 0;
        float toHeadX = headX - c.camX, toHeadY = headY - c.camY, toHeadZ = headZ - c.camZ;
        const float tl = vlen(toHeadX, toHeadY, toHeadZ);
        toHeadX /= tl; toHeadY /= tl; toHeadZ /= tl;
        const float fwdX = std::cos(pitch) * std::cos(yaw);
        const float fwdY = std::sin(pitch);
        const float fwdZ = std::cos(pitch) * std::sin(yaw);
        const float dot = toHeadX * fwdX + toHeadY * fwdY + toHeadZ * fwdZ;
        tpcheck(dot > 0.85f, "TP3 follow camera LOOKS TOWARD the player");
    }

    // ---- TP3b: the PER-WEAPON GRIP TABLE (TASK#46.1) — no asset needed. ------
    {
        // Known weapons resolve to their OWN row (name matches), distinct guns get
        // distinct offsets (the whole point: a pistol seats differently than a
        // chaingun), and an unknown name falls back to the default row.
        const TpGrip& pistol  = tpGripFor("pistol");
        const TpGrip& chain   = tpGripFor("chaingun");
        const TpGrip& shotgun = tpGripFor("shotgun");
        const TpGrip& unknown = tpGripFor("no_such_weapon");
        const TpGrip& deflt   = tpGripFor("");   // empty -> default row too
        const bool resolvesOwn = pistol.name && std::string_view(pistol.name) == "pistol" &&
                                 chain.name  && std::string_view(chain.name)  == "chaingun";
        // Distinct guns -> distinct forward seat (pistol short, chaingun long barrel).
        const bool distinct = std::fabs(pistol.forward - chain.forward) > 1e-3f &&
                              std::fabs(pistol.forward - shotgun.forward) > 1e-3f;
        // Unknown name + empty name both resolve to the SAME default row (no crash).
        const bool fallback = unknown.name == nullptr && deflt.name == nullptr &&
                              unknown.forward == deflt.forward;
        tpcheck(resolvesOwn && distinct && fallback,
                "TP3b per-weapon grip table: known guns resolve distinct rows, unknown -> default");
    }

    // ---- TP4: the locomotion-band selector picks Idle/Walk/Run by speed. -----
    {
        const bool idle = selectLocoBand(0.0f) == LocoBand::Idle;
        const bool walk = selectLocoBand(1.5f) == LocoBand::Walk;
        const bool run  = selectLocoBand(6.0f) == LocoBand::Run;
        // Monotone across the bands (idle < walk < run thresholds).
        const bool order = selectLocoBand(0.1f) == LocoBand::Idle &&
                           selectLocoBand(0.5f) == LocoBand::Walk &&
                           selectLocoBand(4.5f) == LocoBand::Run;
        tpcheck(idle && walk && run && order,
                "TP4 locomotion selector: still->Idle, slow->Walk, fast->Run");
    }

    // ---- TP5: FP/3P toggle flips state + swaps viewmodel<->avatar visibility. -
    HeadlessRenderDevice device;
    Scene scene;
    ThirdPersonView tp;
    tp.build(scene, device, riggedGlbRoot());
    {
        // FP is the default: viewmodel visible, avatar hidden.
        const bool defFp = !tp.thirdPerson() && tp.viewmodelVisible();
        tp.setThirdPerson(true);
        // In 3P: avatar visible IFF Jake built (skinned), viewmodel hidden.
        const bool in3p = tp.thirdPerson() && !tp.viewmodelVisible() &&
                          (tp.avatarVisible() == tp.built());
        const bool flips = tp.viewmodelVisible() != tp.avatarVisible() || !tp.built();
        tp.toggle();   // back to FP
        const bool backFp = !tp.thirdPerson() && tp.viewmodelVisible() && !tp.avatarVisible();
        tpcheck(defFp && in3p && flips && backFp,
                "TP5 FP/3P toggle flips state + swaps viewmodel<->avatar");
    }

    // ---- Asset-present checks (TP6..TP9). Skipped (still PASS) on a clean
    // checkout where Jake's GLB is absent — the camera + selector + toggle above
    // already cover the headless mechanics. ----
    const bool jakePresent = std::filesystem::exists(
        std::filesystem::path(riggedGlbRoot()) / kJakeModelFile);
    if (!tp.built() || !jakePresent) {
        x3::logWarn("[3p-test] Jake GLB absent / avatar unbuilt — skipping the rig-bound "
                    "checks (TP6..TP9) on this checkout (still PASS).");
    } else {
        // TP6: Jake loaded as a skinned rig with the expected ~34-bone Mixamo count.
        const uint32_t bones = tp.boneCount();
        tpcheck(tp.skinned() && bones >= 30 && bones <= 80,
                "TP6 Jake loaded skinned (bone count in the expected Mixamo range)");

        // TP7: the weapon-hand socket bone resolved.
        tpcheck(tp.handBoneResolved() && tp.handNode() >= 0,
                "TP7 weapon-hand bone (mixamorigRightHand) resolves");

        // Enter 3P + drive a couple of frames so a pose exists, then read the socket.
        tp.setThirdPerson(true);
        x3::phys::Vec3 feet{ 5.0f, 0.0f, 7.0f };
        // First frame seeds prev; second frame moves so a planar speed registers.
        tp.update(1.0f / 60.0f, scene, feet, 1.6f, -kPi * 0.5f, 0.0f, kNoRoom, false, false);
        feet.z += 0.05f;   // ~3 m/s -> Run band
        tp.update(1.0f / 60.0f, scene, feet, 1.6f, -kPi * 0.5f, 0.0f, kNoRoom, false, false);

        // TP8: the hand-socket WORLD transform is retrievable and lands NEAR the
        // avatar (within a couple meters of the body, i.e. it's a hand on the rig).
        float sock[16] = {0};
        const bool gotSock = tp.handSocketWorld(sock);
        const float hx = sock[12], hy = sock[13], hz = sock[14];
        const float dToBody = vlen(hx - feet.x, hy - (feet.y + 1.2f), hz - feet.z);
        tpcheck(gotSock && dToBody < 2.5f,
                "TP8 hand-socket world transform is retrievable + near the avatar");

        // TP9: the socket RIDES with the pose — its world position tracks the avatar
        // when the avatar moves (the held weapon follows the hand, not the origin).
        float sock0[16]; tp.handSocketWorld(sock0);
        x3::phys::Vec3 feet2{ feet.x + 4.0f, feet.y, feet.z };
        tp.update(1.0f / 60.0f, scene, feet2, 1.6f, -kPi * 0.5f, 0.0f, kNoRoom, false, false);
        float sock1[16]; tp.handSocketWorld(sock1);
        const float moved = vlen(sock1[12] - sock0[12], sock1[13] - sock0[13],
                                 sock1[14] - sock0[14]);
        tpcheck(moved > 2.5f,
                "TP9 hand socket follows the avatar (held weapon rides the hand)");

        // TP10: the per-weapon grip actually MOVES the socket — a pistol and a
        // chaingun must produce different world placements at the same pose (the
        // grip table is wired through handSocketWorld, not ignored).
        float sockPistol[16], sockChain[16];
        const bool gp = tp.handSocketWorld(sockPistol, "pistol");
        const bool gc = tp.handSocketWorld(sockChain,  "chaingun");
        const float gripMoved = vlen(sockPistol[12] - sockChain[12],
                                     sockPistol[13] - sockChain[13],
                                     sockPistol[14] - sockChain[14]);
        tpcheck(gp && gc && gripMoved > 1e-3f,
                "TP10 per-weapon grip shifts the hand socket (pistol != chaingun)");

        // TP11: CROUCH (TASK#46.2) lowers the avatar — after several crouched frames
        // the baked draw-transform Y sits below the standing Y at the same feet.
        x3::phys::Vec3 cf{ feet2.x, feet2.y, feet2.z };
        // Settle standing first.
        for (int i = 0; i < 20; ++i)
            tp.update(1.0f / 60.0f, scene, cf, 1.6f, -kPi * 0.5f, 0.0f, kNoRoom, false, false);
        float stand[16]; tp.handSocketWorld(stand);
        const float standY = stand[13];
        // Now crouch and settle.
        for (int i = 0; i < 60; ++i)
            tp.update(1.0f / 60.0f, scene, cf, 1.6f, -kPi * 0.5f, 0.0f, kNoRoom, true, false);
        float crouch[16]; tp.handSocketWorld(crouch);
        tpcheck(crouch[13] < standY - 0.1f,
                "TP11 crouch lowers the avatar (hand socket drops vs standing)");

        // TP12: OVER-THE-SHOULDER AIM (TASK#46.3) biases the camera vs the rest cam.
        // Settle un-aimed, capture the cam; then hold fire several frames + capture.
        for (int i = 0; i < 30; ++i)
            tp.update(1.0f / 60.0f, scene, cf, 1.6f, -kPi * 0.5f, 0.0f, kNoRoom, false, false);
        ThirdPersonCamera rest = tp.camera(cf, 1.6f, -kPi * 0.5f, 0.0f);
        for (int i = 0; i < 30; ++i)
            tp.update(1.0f / 60.0f, scene, cf, 1.6f, -kPi * 0.5f, 0.0f, kNoRoom, false, true);
        ThirdPersonCamera aim = tp.camera(cf, 1.6f, -kPi * 0.5f, 0.0f);
        const float camShift = vlen(aim.camX - rest.camX, aim.camY - rest.camY,
                                    aim.camZ - rest.camZ);
        tpcheck(camShift > 0.05f,
                "TP12 over-the-shoulder aim biases the follow camera while firing");

        // ---- TP13 (BUG A): a STANDING avatar's baked basis is UPRIGHT + ORTHONORMAL.
        // After crouching (TP11 left m_crouchAmt high), re-ENTER 3P — setThirdPerson()
        // must zero the synthesized-crouch amount so the freshly-shown standing avatar
        // is perfectly upright (no leftover lean tilt). Then a few standing frames keep
        // it upright. Assert: up column == world +Y (no tilt), and the 3 basis columns
        // are mutually orthogonal + unit-length (after removing the uniform scale).
        tp.setThirdPerson(false);
        tp.setThirdPerson(true);   // re-enter: BUG A fix zeroes the stale crouch amount
        for (int i = 0; i < 5; ++i)
            tp.update(1.0f / 60.0f, scene, cf, 1.6f, -kPi * 0.5f, 0.0f, kNoRoom, false, false);
        float dxf[16]; tp.avatarDrawTransform(dxf);
        // Columns (column-major): right=col0(0,1,2), up=col1(4,5,6), fwd=col2(8,9,10).
        float rgt[3] = { dxf[0], dxf[1], dxf[2] };
        float upc[3] = { dxf[4], dxf[5], dxf[6] };
        float fwc[3] = { dxf[8], dxf[9], dxf[10] };
        const float rl = vlen(rgt[0], rgt[1], rgt[2]);
        const float ul = vlen(upc[0], upc[1], upc[2]);
        const float fl = vlen(fwc[0], fwc[1], fwc[2]);
        // Up must point along world +Y (standing => no pitch/roll lean).
        const bool uprightY = ul > 1e-4f && upc[1] / ul > 0.9999f &&
                              std::fabs(upc[0]) < 1e-3f && std::fabs(upc[2]) < 1e-3f;
        // Orthogonal columns (normalized dot ~ 0) + equal (uniform-scale) lengths.
        auto ndot = [](const float* a, float la, const float* b, float lb) {
            return (a[0]*b[0] + a[1]*b[1] + a[2]*b[2]) / (la * lb + 1e-12f);
        };
        const bool ortho = rl > 1e-4f && ul > 1e-4f && fl > 1e-4f &&
                           std::fabs(ndot(rgt, rl, upc, ul)) < 1e-3f &&
                           std::fabs(ndot(rgt, rl, fwc, fl)) < 1e-3f &&
                           std::fabs(ndot(upc, ul, fwc, fl)) < 1e-3f;
        tpcheck(uprightY && ortho,
                "TP13 standing avatar basis is UPRIGHT + orthonormal at spawn (crouch amt 0)");

        // ---- TP14 (BUG B): backing up keeps the body FACING the look dir + selects
        // the BACKWARDS clip (no 180-deg spin to face the movement vector). Look along
        // world -Z (yaw=-pi/2); step the feet in +Z (BACKWARD relative to the look).
        // Assert avatarYaw stays the look yaw (does NOT flip to atan2(+dz,0)=+pi/2),
        // and the backward clip set is selected (when the rig has a backpedal clip).
        const float lookYaw = -kPi * 0.5f;
        x3::phys::Vec3 bf{ cf.x, cf.y, cf.z };
        tp.update(1.0f / 60.0f, scene, bf, 1.6f, lookYaw, 0.0f, kNoRoom, false, false); // seed prev
        for (int i = 0; i < 8; ++i) {
            bf.z += 0.04f;   // ~2.4 m/s in +Z = backpedal (look faces -Z)
            tp.update(1.0f / 60.0f, scene, bf, 1.6f, lookYaw, 0.0f, kNoRoom, false, false);
        }
        // Body still faces the look dir (within a few degrees), NOT the move heading.
        auto angDiff = [](float a, float b) {
            float d = a - b;
            while (d >  kPi) d -= 2.0f * kPi;
            while (d < -kPi) d += 2.0f * kPi;
            return std::fabs(d);
        };
        const bool facesLook = angDiff(tp.avatarYaw(), lookYaw) < 0.05f;
        // Back clip selected iff the rig actually has one (else gracefully stays fwd).
        const bool backSel = (tp.walkBackClip() < 0) ? true : tp.locoBackActive();
        tpcheck(facesLook && backSel,
                "TP14 backward movement keeps the body FORWARD-facing + selects the back clip");

        // ---- TP15 (BUG C): the avatar keeps ANIMATING over a long sustained run with
        // fireHeld TRUE every frame (the freeze repro). update() requests the looping
        // fire crossfade clip every frame; the bug was that triggerClip() re-seeded the
        // crossfade clock (m_xfadeClipT=0 / m_xfadeTime=0) on EVERY call, pinning the
        // fire pose at t=0 — the avatar froze after the first held-fire. Drive ~3
        // simulated minutes (10800 frames @ 60 Hz) standing + firing, then sample two
        // consecutive frames and assert the pose STILL advances (fire clip looping) +
        // is finite (no precision blow-up). With the idempotent-triggerClip fix the
        // per-frame fire request is a true no-op and the clip advances normally.
        bool stillAnimating = false;
        x3::phys::Vec3 rf{ cf.x, cf.y, cf.z };
        for (int i = 0; i < 10800; ++i)             // ~180 s of held fire, standing
            tp.update(1.0f / 60.0f, scene, rf, 1.6f, lookYaw, 0.0f, kNoRoom, false, true);
        // Two consecutive frames at FIXED feet, still firing — only the animation pose
        // can move the hand socket. A stuck (frozen) crossfade gives dPose ~ 0.
        tp.update(1.0f / 60.0f, scene, rf, 1.6f, lookYaw, 0.0f, kNoRoom, false, true);
        float lateA[16]; tp.handSocketWorld(lateA);
        tp.update(1.0f / 60.0f, scene, rf, 1.6f, lookYaw, 0.0f, kNoRoom, false, true);
        float lateB[16]; tp.handSocketWorld(lateB);
        const float dPose = vlen(lateB[12] - lateA[12],
                                 lateB[13] - lateA[13],
                                 lateB[14] - lateA[14]);
        const bool finite = std::isfinite(lateB[12]) && std::isfinite(lateB[13]) &&
                            std::isfinite(lateB[14]);
        stillAnimating = finite && dPose > 1e-6f;
        tpcheck(stillAnimating,
                "TP15 avatar still animates after a long sustained held-fire run (no crossfade freeze)");

        // ---- TP16 (swim v2): sustained `swimming` AT REST is the TREAD read —
        // the basis leans off vertical (but stays far more upright than the stroke)
        // and the body floats; releasing the flag restores upright through the same
        // dt-scaled blend (no stale lean — mirrors TP13's staleness rule). ----
        {
            x3::phys::Vec3 wf{ rf.x, rf.y, rf.z };
            for (int i = 0; i < 120; ++i)   // ~2 s: the 5/s blend fully settles
                tp.update(1.0f / 60.0f, scene, wf, 1.6f, lookYaw, 0.0f, kNoRoom,
                          false, false, /*swimming*/true);
            float swx[16]; tp.avatarDrawTransform(swx);
            const float upLenS = vlen(swx[4], swx[5], swx[6]);
            // Treading at 38 deg: the up column's world-Y component ~ cos(38) ~ 0.79.
            const float upY = upLenS > 1e-4f ? (swx[5] / upLenS) : 1.0f;
            const bool leaned = upY < 0.90f && upY > 0.35f;   // in the water, but upright-ish
            const float proneY = swx[13];
            const bool treading = !tp.swimStroking();
            for (int i = 0; i < 180; ++i)   // exit: upright restores
                tp.update(1.0f / 60.0f, scene, wf, 1.6f, lookYaw, 0.0f, kNoRoom,
                          false, false, /*swimming*/false);
            float upr[16]; tp.avatarDrawTransform(upr);
            const float upLenU = vlen(upr[4], upr[5], upr[6]);
            const bool uprightBack = upLenU > 1e-4f && (upr[5] / upLenU) > 0.995f;
            // The floating body never sinks below the restored standing Y.
            const bool floated = proneY >= upr[13] - 1e-3f;
            tpcheck(leaned && treading && uprightBack && floated,
                    "TP16 swimming at rest TREADS water (leaned + floating) + exit restores upright");
        }

        // ---- TP17 (swim v2): the rig carries a REAL swim clip and MOVING through
        // the water strokes it — the stroke latch trips on the planar water speed,
        // the basis goes nearly FLAT (much more prone than treading), and the pose
        // actually ADVANCES frame to frame (the clip is playing, not a frozen pose).
        // On an old GLB (no Swim clip) the clip assertions are skipped: the degrade
        // path is legal, it just must not crash or freeze.
        {
            const bool hasSwim = tp.swimClip() >= 0;
            tpcheck(hasSwim && tp.swimIdleClip() >= 0,
                    "TP17a Jake's rig carries the baked Swim + SwimIdle clips");
            // Swim FORWARD at ~1.2 m/s along the look for 2 s.
            x3::phys::Vec3 wf{ rf.x, rf.y, rf.z };
            const float sp = 1.2f, sdt = 1.0f / 60.0f;
            for (int i = 0; i < 120; ++i) {
                wf.x += std::cos(lookYaw) * sp * sdt;
                wf.z += std::sin(lookYaw) * sp * sdt;
                tp.update(sdt, scene, wf, 1.6f, lookYaw, 0.0f, kNoRoom,
                          false, false, /*swimming*/true);
            }
            const bool stroking = tp.swimStroking();
            float sx[16]; tp.avatarDrawTransform(sx);
            const float upLen = vlen(sx[4], sx[5], sx[6]);
            const float upY = upLen > 1e-4f ? (sx[5] / upLen) : 1.0f;
            const bool flat = upY < 0.35f;    // stroking ~84 deg => cos(84) ~ 0.10
            // The pose advances: two more frames at a FIXED position (only the clip
            // can move the hand socket).
            tp.update(sdt, scene, wf, 1.6f, lookYaw, 0.0f, kNoRoom, false, false, true);
            float pa[16]; tp.handSocketWorld(pa);
            tp.update(sdt, scene, wf, 1.6f, lookYaw, 0.0f, kNoRoom, false, false, true);
            float pb[16]; tp.handSocketWorld(pb);
            const float dPose2 = vlen(pb[12] - pa[12], pb[13] - pa[13], pb[14] - pa[14]);
            const bool animating = std::isfinite(dPose2) && dPose2 > 1e-6f;
            tpcheck(stroking && flat && animating,
                    "TP17 swimming FORWARD strokes: flat at the surface + the clip animates");
            for (int i = 0; i < 180; ++i)   // leave the water clean for later tests
                tp.update(sdt, scene, wf, 1.6f, lookYaw, 0.0f, kNoRoom, false, false, false);
        }

        tp.setThirdPerson(false);
    }

    x3::logInfo("[3p-test] " + std::to_string(g_tpPass) + " passed, " +
                std::to_string(g_tpFail) + " failed");
    return g_tpFail == 0;
}

} // namespace x3::game
