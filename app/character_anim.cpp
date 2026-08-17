// AnimatedCharacter — the shared on-foot character rig runtime. See the
// receipt + contract in character_anim.h (owner: "THIS ENGINE NEEDS
// CONSISTENT APPLICATION OF MODEL ANIMATION").

#include "character_anim.h"

#include "terrain.h"        // terrainHeightAtWorld — the CONTACT LAW's floor
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace x3::game {

// ---------------------------------------------------------------------------
// AXES LAW facing helpers. Yaw 0 = engine -Z (the asset convention this
// module enforces); facing direction (dx,dz) => yaw = atan2(-dx,-dz).
// Positive yaw turns LEFT (facing swings -Z -> -X).
// ---------------------------------------------------------------------------
static float yawFromDir(float dx, float dz) { return std::atan2(-dx, -dz); }

static float wrapAngle(float a) {
    while (a >  3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

// ---------------------------------------------------------------------------
// The MEASURED Jake_44_actions.glb table. Every direction below was measured
// from the file (tools/jake_clip_motion.py: hips root motion + stance-foot
// drift; tools/jake_hips_stats.py: hips net yaw), because the labels are
// untrusted — the rig ships TWO clips named "Walking" and six strafe-family
// clips. Post-bake convention: rig faces -Z at identity, clips are in-place.
//
//   clip              idx  measured (rig space, pre-bake fwd=+Z)
//   Walking            22  fwd walk, was +1.52 m net Z (de-drifted), 1.14 m/s
//                          (the SECOND "Walking", idx 43, is the batch-2
//                          in-place walk; exact lookup takes the first)
//   Running            39  fwd run, in-place (batch 2, repaired cm->m)
//   Walkingbackwards   23  BACK, was -1.736 m Z / 1.38 s  = 1.26 m/s
//   Runbackwards       12  BACK, was -1.579 m Z / 0.50 s  = 3.16 m/s
//   Strafeleft         15  LEFT  (+X rig = character LEFT), 1.59 m/s
//   Straferight        16  RIGHT (-X rig), 0.87 m/s
//   Leftturn90          5  in-place turn, net hips yaw +89.3 deg (LEFT)
//   Rightturn90        11  in-place turn, net hips yaw -101.8 deg (RIGHT)
//   Jump                3  jump one-shot — MEASURED dead in-place (net AND
//                          stance). Preferred over Regular_Jump (idx 38),
//                          which keeps a -0.28 m BACK stance drift even
//                          post-bake; its pre-bake root motion lunged the
//                          mesh through the chase camera (owner: "jumping
//                          switches camera to INSIDE JAKE" — f15ce5f1 made
//                          the same call and deferred the final word to
//                          this table).
//   Fall_Down          35  losing-balance fall (held on last frame airborne)
//   Idle_11            37  idle variation, 1.9 s
//   Swim / SwimIdle 17/18  in-place; selection ready for the river lane
//
// NOT wired (measured but rejected): Walk_Turn_Left/Right are TRAVELLING
// turns from the cm-scale batch with a ~1.4 m baked Z offset — not in-place
// turns.
//
// COMBAT LAYER (the weapons task) — durations read from the GLB itself:
//   Rifleaimingidle     7   3.04 s loop — rifle at the shoulder, ready
//   Firingrifle         0   0.25 s one-shot — retriggered at the fire rate
//   Reloading           6   3.29 s one-shot — PAIRED VALUE: host_tunnel's
//                           tunnelRifleRoster() reloadTime carries the same
//                           number so the mag refills when the hands finish
//   Tossgrenade        19   2.96 s one-shot — release at ~1.15 s (host reads
//                           oneShotTime(); verified against the toss captures)
//   Riflerun            9   0.71 s loop — armed run (swapped into the blend)
//   Riflejump           8   0.58 s one-shot — armed jump
// ---------------------------------------------------------------------------
CharacterClipTable jakeClipTable() {
    CharacterClipTable t;
    t.idle        = "Idle";
    t.walk        = "Walking";          t.walkSpeed  = 0.2f;  // blend-map m/s
    t.run         = "Running";          t.runSpeed   = 2.0f;  // (host-proven)
    t.jump        = "Jump";            // in-place (see measurement above)
    t.walkBack    = "Walkingbackwards"; t.walkBackSpeed  = 1.26f;
    t.runBack     = "Runbackwards";     t.runBackSpeed   = 3.16f;
    t.strafeLeft  = "Strafeleft";       t.strafeLeftSpeed  = 1.59f;
    t.strafeRight = "Straferight";      t.strafeRightSpeed = 0.87f;
    t.turnLeft    = "Leftturn90";       t.turnLeftRad  = +1.5586f;  // +89.3 deg
    t.turnRight   = "Rightturn90";      t.turnRightRad = -1.7767f;  // -101.8 deg
    t.fall        = "Fall_Down";
    t.idleVariant = "Idle_11";          t.idleVariantEvery = 20.0f;
    t.swim        = "Swim";
    t.swimIdle    = "SwimIdle";
    t.rifleIdle   = "Rifleaimingidle";
    t.rifleFire   = "Firingrifle";
    t.rifleReload = "Reloading";
    t.rifleGrenade= "Tossgrenade";
    t.rifleRun    = "Riflerun";
    t.rifleJump   = "Riflejump";
    return t;
}

// ---------------------------------------------------------------------------
// Camera modes (F1 cycle). All three ride the SAME Player look angles, so
// mouse-look works — and orbits rather than fights — in every mode.
// ---------------------------------------------------------------------------
const char* characterCamModeName(int mode) {
    switch (mode) {
        case (int)CharacterCamMode::FirstPerson: return "FIRST PERSON";
        case (int)CharacterCamMode::ThirdNear:   return "THIRD PERSON - NEAR";
        default:                                 return "THIRD PERSON - FAR";
    }
}

void characterCameraEye(const Player& player, int mode,
                        float& cx, float& cy, float& cz,
                        float& yaw, float& pitch) {
    float ex, ey, ez;
    player.camera(ex, ey, ez, yaw, pitch);
    if (mode == (int)CharacterCamMode::FirstPerson) {
        cx = ex; cy = ey; cz = ez;                 // the eye IS the camera
        return;
    }
    // Over the shoulder, pulled back along the look vector — dead-centre
    // behind the head means the body hides what you are walking toward.
    const float back     = (mode == (int)CharacterCamMode::ThirdNear) ? 2.5f : 5.5f;
    const float shoulder = (mode == (int)CharacterCamMode::ThirdNear) ? 0.55f : 0.70f;
    const float lift     = (mode == (int)CharacterCamMode::ThirdNear) ? 0.25f : 0.45f;
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float fx = cp * std::cos(yaw), fy = sp, fz = cp * std::sin(yaw);
    const float rx = -std::sin(yaw), rz = std::cos(yaw);
    cx = ex - fx * back + rx * shoulder;
    cy = ey - fy * back + lift;
    cz = ez - fz * back + rz * shoulder;
}

// ---------------------------------------------------------------------------
// Load + bind (the sarah.cpp recipe).
// ---------------------------------------------------------------------------
int AnimatedCharacter::resolve(const char* name) const {
    if (!name || !m_skin.valid()) return -1;
    for (uint32_t c = 0; c < m_skin.clipCount(); ++c)
        if (m_skin.clipName(c) == std::string_view(name)) return (int)c;
    return -1;
}

int AnimatedCharacter::clipIndex(const char* exactName) const {
    return resolve(exactName);
}

bool AnimatedCharacter::load(x3::rhi::IRenderDevice& device,
                             const std::string& glbDir, const std::string& file,
                             const CharacterClipTable& table) {
    m_table = table;
    m_src.reset(x3::asset::createAssetSource());
    if (!m_src || !m_src->mountDir(glbDir, 0)) {
        x3::logWarn("[char] mountDir failed: " + glbDir);
        return false;
    }
    m_loader.reset(x3::asset::createModelLoader(&device, m_src.get()));
    m_model = m_loader->load(file);
    if (!m_model.ok) {
        x3::logWarn("[char] " + file + " failed to load");
        return false;
    }
    m_draw = x3::asset::makeDrawables(m_model);

    if (m_skin.bind(m_model)) {
        // ROOT-Y LOCK: the capsule owns world Y; a clip's baked root Y must
        // never sink or bob the mesh (anim.h documents the retarget family).
        m_skin.setRootYLock(true);
        m_skin.enableGpuSkinning(device, m_model);

        int idle = resolve(table.idle);
        int walk = resolve(table.walk);
        int run  = resolve(table.run);
        if (idle < 0) idle = m_skin.findClip({ "idle" });
        if (idle < 0) idle = 0;
        m_idle = idle; m_walk = walk; m_run = run;   // kept for setArmed()
        m_jump     = resolve(table.jump);
        m_walkBack = resolve(table.walkBack);
        m_runBack  = resolve(table.runBack);
        m_strafeL  = resolve(table.strafeLeft);
        m_strafeR  = resolve(table.strafeRight);
        m_turnL    = resolve(table.turnLeft);
        m_turnR    = resolve(table.turnRight);
        m_fall     = resolve(table.fall);
        m_idleVar  = resolve(table.idleVariant);
        m_swim     = resolve(table.swim);
        m_swimIdle = resolve(table.swimIdle);
        m_rifleIdle    = resolve(table.rifleIdle);
        m_rifleFire    = resolve(table.rifleFire);
        m_rifleReload  = resolve(table.rifleReload);
        m_rifleGrenade = resolve(table.rifleGrenade);
        m_rifleRun     = resolve(table.rifleRun);
        m_rifleJump    = resolve(table.rifleJump);
        m_skin.setLocomotionClips(idle, walk, run, table.walkSpeed, table.runSpeed);
        m_skin.setLocomotionSpeed(0.0f);
        m_skin.applyLocomotion(m_model, device, 0.0f);
        m_animated = true;
        char b[256];
        std::snprintf(b, sizeof(b),
            "[char] %s animated: idle=%d walk=%d run=%d jump=%d back=%d/%d "
            "strafe=%d/%d turn=%d/%d fall=%d idleVar=%d swim=%d/%d",
            file.c_str(), idle, walk, run, m_jump, m_walkBack, m_runBack,
            m_strafeL, m_strafeR, m_turnL, m_turnR, m_fall, m_idleVar,
            m_swim, m_swimIdle);
        x3::logInfo(b);
        std::snprintf(b, sizeof(b),
            "[char] %s combat: rifleIdle=%d fire=%d reload=%d grenade=%d "
            "rifleRun=%d rifleJump=%d", file.c_str(), m_rifleIdle, m_rifleFire,
            m_rifleReload, m_rifleGrenade, m_rifleRun, m_rifleJump);
        x3::logInfo(b);
    } else {
        x3::logInfo("[char] " + file + " not skinnable — static draw");
    }
    return true;
}

// ---------------------------------------------------------------------------
// One-shot layer (punch/kick/fire/reload/grenade). `restart` lets rapid fire
// rewind its own clip at the weapon's fire rate instead of being refused.
// ---------------------------------------------------------------------------
bool AnimatedCharacter::playOneShot(const char* exactName, bool restart) {
    if (!m_animated) return false;
    if (m_userT >= 0.0f && !restart) return false;
    const int c = resolve(exactName);
    if (c < 0) return false;
    m_userClip = c;
    m_userT = 0.0f;
    return true;
}

// ---------------------------------------------------------------------------
// WEAPON layer: armed swaps the locomotion RUN clip for the rifle run (the
// same registration path load() used, so the blend machinery is untouched)
// and update() selects the rifle-ready idle + the rifle jump.
// ---------------------------------------------------------------------------
void AnimatedCharacter::setArmed(bool armed) {
    if (armed == m_armed) return;
    m_armed = armed;
    if (!armed) m_aiming = false;
    if (!m_animated) return;
    const int run = (armed && m_rifleRun >= 0) ? m_rifleRun : m_run;
    m_skin.setLocomotionClips(m_idle, m_walk, run,
                              m_table.walkSpeed, m_table.runSpeed);
}

// ---------------------------------------------------------------------------
// Named-bone world transform: the SAME draw matrix draw() composes (feet at
// the capsule, yaw about +Y) times the Skinner's model-space bone global —
// the weapon hand socket (mirrors ThirdPersonView::handSocketWorld).
// ---------------------------------------------------------------------------
bool AnimatedCharacter::boneWorld(const char* boneName, const Player& player,
                                  float yawTrimRad, float yTrim, float out[16]) {
    if (!m_animated || !boneName || !out) return false;
    if (m_boneNode < 0 || m_boneName != boneName) {
        m_boneName = boneName;
        m_boneNode = m_skin.resolveNodeByName(m_model, boneName);
        if (m_boneNode < 0) return false;
    }
    float bone[16];
    if (!m_skin.boneGlobal((uint32_t)m_boneNode, bone)) return false;
    const x3::phys::Vec3 ft = player.feet();
    const float a  = m_yaw + yawTrimRad;
    const float ca = std::cos(a), sa = std::sin(a);
    const float world[16] = {
         ca, 0.0f, -sa, 0.0f,
       0.0f, 1.0f, 0.0f, 0.0f,
         sa, 0.0f,  ca, 0.0f,
       ft.x, ft.y + yTrim, ft.z, 1.0f
    };
    x3::asset::mulMat4(world, bone, out);
    return true;
}

void AnimatedCharacter::applyExclusive(x3::rhi::IRenderDevice& device,
                                       int clip, float t) {
    m_skin.apply(m_model, device, (uint32_t)clip, t);
}

// ---------------------------------------------------------------------------
// The frame update: contact law, facing, ONE selection mapping, skinning.
// ---------------------------------------------------------------------------
void AnimatedCharacter::update(Player& player, const Intent& in, float camYaw,
                               float dt, x3::phys::IPhysicsWorld& phys,
                               x3::rhi::IRenderDevice& device) {
    dt = std::max(dt, 1e-4f);

    // ---- 1) THE CONTACT LAW (NO_SLOP rule 11 v3). Clamp the capsule to the
    // TOPMOST WALKABLE SURFACE: max of the terrain height field and a SHORT
    // downward static raycast — roads/decks ride ABOVE the field, so the field
    // alone once entombed a man under his own carriageway.
    //
    // v2 -> v3, THE TUNNEL EJECTION (owner, 2026-08-17: "I popped ABOVE the
    // tunnel" and then "I could swear there was just a tunnel here"). v2
    // started the ray 40 m above the feet, which INSIDE A BORE starts above
    // the 458 m x 58 m backfill lid: the ray came down, hit the TOP of the
    // lid, and the law dutifully "lifted the character onto the surface" —
    // through the tunnel roof, onto the hillside. He then looked back at a
    // road ending in a cut face and reasonably concluded the tunnel was gone.
    // It was under his feet.
    //
    // The ray must never see anything ABOVE the character's head. 1.0 m is
    // enough: standing on a deck, the feet are already on it, so a short
    // downward ray finds it immediately. The only case +40 bought was
    // "character is metres BELOW a deck", which is precisely the tunnel case
    // it broke. The header's promise that "the law never fights tunnels" is
    // true of the height field and became false the moment the raycast was
    // added; this restores it.
    {
        const x3::phys::Vec3 jf = player.feet();
        float gy = terrainHeightAtWorld(jf.x, jf.z);
        const x3::phys::RayHit rh = phys.rayCast(
            x3::phys::Vec3{ jf.x, jf.y + 1.0f, jf.z },
            x3::phys::Vec3{ 0.0f, -1.0f, 0.0f }, 4.0f, x3::phys::Layer::Static);
        if (rh.hit) gy = std::max(gy, rh.point.y);
        // ... and inside a bore even the FIELD is the hilltop, because the
        // corridor carve only cuts the open approach — so a lid overhead means
        // the field is not our floor. If a static surface is close above the
        // head, trust the raycast/current stance and never the field.
        const x3::phys::RayHit up = phys.rayCast(
            x3::phys::Vec3{ jf.x, jf.y + 0.2f, jf.z },
            x3::phys::Vec3{ 0.0f, 1.0f, 0.0f }, 12.0f, x3::phys::Layer::Static);
        if (up.hit && gy > jf.y) gy = rh.hit ? rh.point.y : jf.y;
        if (jf.y < gy - 0.25f) {
            player.setFeetPosition(phys, x3::phys::Vec3{ jf.x, gy + 0.15f, jf.z });
            x3::logInfo("[char] CONTACT LAW: character lifted onto the surface "
                        "(was below by more than 0.25 m)");
        }
    }

    // ---- 2) What the capsule actually DID this frame.
    const x3::phys::Vec3 ft = player.feet();
    if (!m_havePrev) {
        m_prevFeet[0] = ft.x; m_prevFeet[1] = ft.y; m_prevFeet[2] = ft.z;
        m_havePrev = true;
    }
    const float vx = (ft.x - m_prevFeet[0]) / dt;
    const float vy = (ft.y - m_prevFeet[1]) / dt;
    const float vz = (ft.z - m_prevFeet[2]) / dt;
    m_prevFeet[0] = ft.x; m_prevFeet[1] = ft.y; m_prevFeet[2] = ft.z;
    const float planar = std::sqrt(vx * vx + vz * vz);
    if (player.grounded() || player.swimming()) m_airT = 0.0f;
    else                                        m_airT += dt;

    if (!m_animated) {
        // Static mesh: still face the camera's planar forward so a rig with no
        // skin at least points the right way.
        m_yaw = yawFromDir(std::cos(camYaw), std::sin(camYaw));
        return;
    }

    // ---- 3) ONE mapping: (intent, grounded, airborne-time, swimming) -> clip.
    // sel <  0  : locomotion blend (idle/walk/run by planar speed) — fallback.
    // sel >= 0  : exclusive directional/one-shot clip.
    const float camFace = yawFromDir(std::cos(camYaw), std::sin(camYaw));
    const bool  moving  = planar > 0.4f;
    const bool  backing = in.moveFwd < -0.1f &&
                          std::fabs(in.moveFwd) >= std::fabs(in.moveStrafe);
    const bool  strafing = !backing && std::fabs(in.moveStrafe) > 0.1f &&
                           std::fabs(in.moveStrafe) > std::fabs(in.moveFwd);
    int   sel  = -1;
    float rate = 1.0f;            // playback-rate scale (feet vs capsule speed)
    float faceTarget = m_yaw;
    bool  freezeFace = false;

    if (player.swimming()) {
        sel = moving ? m_swim : m_swimIdle;      // river lane: swimClipset()
        faceTarget = camFace;
    } else if (!player.grounded() && m_airT > 0.4f && vy < -1.0f && m_fall >= 0) {
        sel = m_fall;                            // held on its last frame below
    } else if (moving && backing && m_walkBack >= 0) {
        const bool fast = in.sprint && m_runBack >= 0;
        sel  = fast ? m_runBack : m_walkBack;
        rate = planar / (fast ? m_table.runBackSpeed : m_table.walkBackSpeed);
        faceTarget = camFace;                    // backpedal faces the camera
    } else if (moving && strafing && m_strafeL >= 0 && m_strafeR >= 0) {
        const bool right = in.moveStrafe > 0.0f;
        sel  = right ? m_strafeR : m_strafeL;    // MEASURED directions, see table
        rate = planar / (right ? m_table.strafeRightSpeed : m_table.strafeLeftSpeed);
        faceTarget = camFace;                    // strafe faces the camera
    } else if (moving) {
        // Face the travel, not the camera — EXCEPT while aiming: the gun is
        // slaved to the crosshair, so the body follows the camera (fine-aim).
        faceTarget = (m_armed && m_aiming) ? camFace : yawFromDir(vx, vz);
    } else if (m_armed && m_rifleIdle >= 0) {
        // Armed + stationary: rifle at the ready (the aim loop doubles as the
        // armed idle). Aiming keeps the body slaved to the camera.
        sel = m_rifleIdle;
        if (m_aiming) faceTarget = camFace;
    } else {
        // Stationary. Turn-in-place when the camera has swung away, else the
        // occasional idle variation on top of the locomotion idle.
        const float err = wrapAngle(camFace - m_yaw);
        if (m_turnT < 0.0f && player.grounded() &&
            std::fabs(err) > 1.2f &&
            (err > 0.0f ? m_turnL : m_turnR) >= 0) {
            m_turnClip  = err > 0.0f ? m_turnL : m_turnR;
            m_turnDelta = err > 0.0f ? m_table.turnLeftRad : m_table.turnRightRad;
            m_turnT     = 0.0f;
        }
    }

    // Turn one-shot: facing is FROZEN while it plays (the clip itself rotates
    // the hips); the measured yaw lands in m_yaw when it finishes — or a
    // proportional share if movement cancels it early (no snap either way).
    if (m_turnT >= 0.0f) {
        const float dur = m_skin.clipDuration((uint32_t)m_turnClip);
        if (moving || m_turnT >= dur) {
            m_yaw = wrapAngle(m_yaw + m_turnDelta *
                              std::min(1.0f, dur > 0.0f ? m_turnT / dur : 1.0f));
            m_turnT = -1.0f;
        } else {
            m_turnT += dt;
            sel = m_turnClip;
            freezeFace = true;
        }
    }

    // Idle variation: after idleVariantEvery seconds of uninterrupted true
    // idle, play the variant once, then go back to the idle blend.
    if (sel < 0 && !moving && player.grounded() && m_turnT < 0.0f &&
        m_idleVar >= 0) {
        m_idleAccum += dt;
        if (m_idleVarT >= 0.0f) {
            m_idleVarT += dt;
            if (m_idleVarT >= m_skin.clipDuration((uint32_t)m_idleVar)) {
                m_idleVarT = -1.0f;
                m_idleAccum = 0.0f;
            } else {
                sel = m_idleVar;
            }
        } else if (m_idleAccum >= m_table.idleVariantEvery) {
            m_idleVarT = 0.0f;
            sel = m_idleVar;
        }
    } else {
        m_idleAccum = 0.0f;
        m_idleVarT = -1.0f;
    }

    // ---- Facing slew (10 rad/s ease — a man turns, he doesn't snap).
    if (!freezeFace) {
        const float d = wrapAngle(faceTarget - m_yaw);
        m_yaw = wrapAngle(m_yaw + d * std::min(1.0f, dt * 10.0f));
    }

    // ---- 4) Apply. One-shots override, locomotion blend is the fallback.
    // Armed jump takes the rifle-jump clip (hands stay on the gun).
    if (in.jumpPressed && m_jump >= 0 && m_jumpT < 0.0f) {
        m_jumpClip = (m_armed && m_rifleJump >= 0) ? m_rifleJump : m_jump;
        m_jumpT = 0.0f;
    }

    if (m_userT >= 0.0f) {                                   // playOneShot layer
        m_userT += dt;
        if (m_userT >= m_skin.clipDuration((uint32_t)m_userClip)) m_userT = -1.0f;
        else { applyExclusive(device, m_userClip, m_userT); return; }
    }
    if (m_jumpT >= 0.0f) {                                   // jump one-shot
        m_jumpT += dt;
        if (m_jumpT >= m_skin.clipDuration((uint32_t)m_jumpClip)) m_jumpT = -1.0f;
        else { applyExclusive(device, m_jumpClip, m_jumpT); return; }
    }
    if (sel >= 0) {                                          // directional loop
        if (sel != m_moveClip) { m_moveClip = sel; m_moveT = 0.0f; }
        // Turn/idle-variant one-shots track their own timers; loops accumulate
        // at a rate matched to the capsule so the feet don't skate.
        if (sel == m_turnClip && m_turnT >= 0.0f)      m_moveT = m_turnT;
        else if (sel == m_idleVar && m_idleVarT >= 0.0f) m_moveT = m_idleVarT;
        else m_moveT += dt * std::clamp(rate, 0.6f, 1.8f);
        if (sel == m_fall)                                    // hold the fall
            m_moveT = std::min(m_moveT,
                m_skin.clipDuration((uint32_t)m_fall) - 0.02f);
        applyExclusive(device, sel, m_moveT);
        return;
    }
    m_moveClip = -1;                                         // locomotion blend
    m_skin.setLocomotionSpeed(planar);
    m_skin.applyLocomotion(m_model, device, dt);
}

// ---------------------------------------------------------------------------
// Draw at the capsule's feet. The asset owns origin + facing (feet at origin,
// -Z forward at identity) — yTrim/yawTrim are live console trims, default 0.
// ---------------------------------------------------------------------------
void AnimatedCharacter::draw(const x3::rhi::FrameContext& frame,
                             x3::rhi::IRenderDevice& device,
                             const Player& player, float yawTrimRad,
                             float yTrim, bool visible) {
    if (!visible || m_draw.empty()) return;
    const x3::phys::Vec3 ft = player.feet();
    const float a  = m_yaw + yawTrimRad;
    const float ca = std::cos(a), sa = std::sin(a);
    // Column-major: rotation about +Y, translation at the feet. yaw 0 = -Z.
    const float world[16] = {
         ca, 0.0f, -sa, 0.0f,
       0.0f, 1.0f, 0.0f, 0.0f,
         sa, 0.0f,  ca, 0.0f,
       ft.x, ft.y + yTrim, ft.z, 1.0f
    };
    for (const x3::asset::ModelDrawable& d : m_draw) {
        const float bc[4]   = { d.baseColorFactor[0], d.baseColorFactor[1],
                                d.baseColorFactor[2], d.baseColorFactor[3] };
        const float emis[3] = { d.emissiveFactor[0], d.emissiveFactor[1],
                                d.emissiveFactor[2] };
        device.drawMeshPBR(frame,
            x3::rhi::MeshHandle{ d.meshId },
            x3::rhi::TextureHandle{ d.baseColorTexId },
            x3::rhi::TextureHandle{ d.normalTexId },
            x3::rhi::TextureHandle{ d.mrTexId },
            bc, emis, world, d.alphaMask, d.alphaBlend,
            x3::rhi::TextureHandle{ d.emissiveTexId },
            x3::rhi::TextureHandle{ d.detailTexId }, d.detailUvScale,
            d.clearcoat, d.clearcoatRough);
    }
}

} // namespace x3::game
