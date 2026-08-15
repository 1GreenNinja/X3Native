// First-person walking character controller (S3). See app/player.h.
//
// Clean-room implementation from the IPhysicsWorld interface + the parameter
// VALUES extracted in docs/ASSET_INVENTORY.md. No purchased C# was copied.
//
// Physics seam (verified in engine/physics/JoltPhysicsWorld.cpp):
//   * createCharacter(radius,height,pos): pos is the capsule FEET; the capsule
//     bottom sits at the body origin. getBodyPosition() returns feet position.
//   * moveCharacter(id, desiredVelocity, dt): records the HORIZONTAL (.x/.z)
//     desired velocity; the .y is a one-shot JUMP impulse when > 0 (it sets the
//     character's vertical velocity), and is IGNORED when <= 0. Gravity is
//     integrated internally during step(). The world also does its own
//     stick-to-floor while grounded, so a negative ground-stick velocity cannot
//     (and need not) be pushed through the .y channel — see kGroundStick note.
//   * characterGrounded(id): true only while OnGround.

#include "player.h"
#include "combat_log.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace x3::game {

// ---------------------------------------------------------------------------
// Extracted controller parameters (docs/ASSET_INVENTORY.md §5).
// ---------------------------------------------------------------------------
namespace {
constexpr float kWalkSpeed    = 5.0f;    // m/s
constexpr float kSprintSpeed  = 8.0f;    // m/s
constexpr float kJumpHeight   = 1.4f;    // m  (target apex above feet)
// The Jolt world integrates gravity at -9.81 m/s^2 internally (see
// JoltPhysicsWorld::init -> SetGravity). The extracted doc value is -20..-22,
// but to land the jump APEX at kJumpHeight we MUST compute jumpVelocity from the
// gravity the world actually applies, otherwise the apex would overshoot. So we
// derive from the world gravity here. (Caveat documented in the slice summary.)
constexpr float kGravity      = -9.81f;  // matches JoltPhysicsWorld
constexpr float kAirControl   = 0.5f;    // 50% horizontal control while airborne
constexpr float kCoyoteTime   = 0.15f;   // s — jump still allowed after leaving ground
constexpr float kJumpBuffer   = 0.10f;   // s — jump remembered if pressed before landing
constexpr float kStandHeight  = 1.8f;    // m — capsule full standing height
constexpr float kCrouchHeight = 1.2f;    // m — ducked capsule height (Crouch)
constexpr float kProneHeight  = 0.7f;    // m — prone/crawl capsule height (Prone)
constexpr float kEyeHeight    = 1.6f;    // m — camera above feet (Stand)
constexpr float kCrouchEye    = 0.95f;   // m — ducked eye height (Crouch)
constexpr float kProneEye     = 0.45f;   // m — crawling/prone eye height (Prone)
// Planar move-speed multipliers per stance (Stand=full, Crouch=half, Prone=slow crawl).
constexpr float kCrouchSpeedMul = 0.5f;
constexpr float kProneSpeedMul  = 0.28f;
constexpr float kCapsuleRadius = 0.35f;  // m
constexpr float kMouseSens    = 1.9f;    // look multiplier
// Mouse delta is in pixels; scale to radians. The extracted "sensitivity ~1.9"
// is the Unity Input.GetAxis multiplier (input already small per-frame). Our raw
// pixel delta needs a px->rad factor; 1.9 * this gives a comfortable feel that
// matches the prior fly-cam (which used 0.0025 rad/px directly).
constexpr float kPxToRad      = 0.00132f; // so kMouseSens*kPxToRad ~= 0.0025 rad/px
constexpr float kPitchClamp   = 89.9f * 3.14159265358979f / 180.0f; // +/- 89.9 deg (full up/down; 0.1 off the pole to avoid yaw gimbal spin)
// Ground-stick: doc value is -2.0 m/s. The world ignores a negative .y in
// moveCharacter and already sticks the character to the floor internally during
// step(), so we keep .y = 0 when grounded/not jumping and rely on that. The
// constant is retained for documentation / future use.
constexpr float kGroundStick  = -2.0f;

// ---- SWIMMING tuning (W10). All velocities dt-scaled through the physics
// step; the accel blend below is the standard 1-exp(-k*dt) frame-rate-
// independent smoothing (delta-time HARD RULE — never per-frame). ----
constexpr float kSwimEnterDepth = 1.35f;  // m of water over the feet -> start swimming
constexpr float kSwimExitDepth  = 1.05f;  // m -> stop swimming (hysteresis vs enter)
constexpr float kSwimSpeedMul   = 0.60f;  // swim speed = walk/sprint * this (~3.0 m/s)
constexpr float kSwimVertSpeed  = 2.2f;   // m/s stroke-up (Space) / dive (Ctrl/C)
constexpr float kSwimEyeAbove   = 0.20f;  // rest: eye settles this far ABOVE the surface
constexpr float kSwimBuoyGain   = 2.5f;   // buoyancy spring: m/s per m of depth error
constexpr float kSwimBuoyMax    = 1.6f;   // buoyancy speed cap (gentle bob, no pop)
constexpr float kSwimAccel      = 6.0f;   // 1/s soft-acceleration rate (tau ~0.17 s)
} // namespace

void Player::spawn(x3::phys::IPhysicsWorld& physics, float x, float y, float z) {
    m_body = physics.createCharacter(kCapsuleRadius, kStandHeight,
                                     x3::phys::Vec3{ x, y, z });
    m_grounded   = false;
    m_coyote     = 0.0f;
    m_jumpBuffer = 0.0f;
    m_spawned    = true;
    // Fresh capsule is full standing height: reset the stance + eye to match (a respawn
    // after dying while crouched must not leave us logically crouched on a stand capsule).
    m_stance     = Stance::Stand;
    m_eyeHeight  = kEyeHeight;
    // Fresh capsule is in normal (non-swim) mode — reset the swim state to match.
    m_swimming   = false;
    m_swimVelX = m_swimVelY = m_swimVelZ = 0.0f;
    // Seed the cached feet so damageTargetPos()/camera() are valid before the
    // first update() (e.g. a ranged enemy aiming on frame 0 of a test).
    m_feetX = x; m_feetY = y; m_feetZ = z;
    resetHealth();
}

// ---------------------------------------------------------------------------
// Health / damage / death (Phase 2a, spec §6.6).
// ---------------------------------------------------------------------------
bool Player::takeDamage(int amount) {
    // Gate: no damage while dead, during the iframe window, or for non-positive
    // amounts. Returning false here is what makes iframes "absorb" extra hits
    // that arrive in the same window (fair DPS).
    if (m_god) return false;   // IDDQD: degreelessness mode — absorb all damage
    if (!m_alive || m_iframe > 0.0f || amount <= 0) return false;

    m_hp -= amount;
    if (m_hp < 0) m_hp = 0;
    m_iframe = kPlayerIFrame;            // open the invuln window
    m_flash  = kDamageFlashTime;         // raise the red damage flash

    if (m_hp == 0) {
        m_alive   = false;
        m_respawn = kRespawnDelay;       // start the death->respawn countdown
        x3::logInfo("[player] HP 0 — YOU DIED (respawning in " +
                    std::to_string(kRespawnDelay) + "s)");
    } else {
        if (combatLogEnabled())                             // [P3-5] combat_log
            x3::logInfo("[player] took " + std::to_string(amount) +
                        " damage — HP now " + std::to_string(m_hp));
    }
    // A real hit landed (not absorbed by god/iframe/dead) — give the player a
    // pain vocal. Intensity scales with the hit size so a chip tap and a heavy
    // blow don't sound identical; the host picks/alternates the actual clip
    // (e.g. pain_1.wav / pain_2.wav) off this one cue kind.
    const float painIntensity = std::min(1.0f, 0.35f + (float)amount / 40.0f);
    emitCueOrLog(m_cueSink, GameCue{ CueKind::PlayerPain, damageTargetPos(), painIntensity });
    return true;
}

x3::phys::Vec3 Player::damageTargetPos() const {
    return x3::phys::Vec3{ m_feetX, m_feetY + m_eyeHeight, m_feetZ };
}

float Player::stanceHeight(Stance s) {
    return (s == Stance::Prone)  ? kProneHeight :
           (s == Stance::Crouch) ? kCrouchHeight : kStandHeight;
}

void Player::setStance(Stance s, x3::phys::IPhysicsWorld& physics) {
    if (s == m_stance) return;                       // idempotent: already in this stance
    // Resize the physics capsule. SHRINKING (duck) always succeeds; GROWING (stand up /
    // half-stand) is REFUSED by the physics world if a ceiling is too low — in that case
    // keep the current (lower) stance so we never clip a low overhead. Feet stay anchored.
    if (m_spawned && m_body.valid()) {
        const bool growing = stanceHeight(s) > stanceHeight(m_stance);
        if (!physics.setCharacterHeight(m_body, stanceHeight(s)) && growing)
            return;                                  // blocked by a ceiling: stay crouched
    }
    m_stance    = s;
    m_eyeHeight = (s == Stance::Prone)  ? kProneEye :
                  (s == Stance::Crouch) ? kCrouchEye : kEyeHeight;
}

void Player::heal(int amount) {
    if (amount <= 0) return;
    m_hp += amount;
    if (m_hp > m_maxHp) m_hp = m_maxHp;
}

// [W9-3 RPG] progression stat layer: max HP = kPlayerMaxHp + bonus (base constant
// never mutated). Raising the cap also grants the delta as current HP (a level-up
// feels good); lowering (mods removed on load) clamps current HP to the new cap.
void Player::setMaxHpBonus(int bonus) {
    if (bonus < 0) bonus = 0;
    const int newMax = kPlayerMaxHp + bonus;
    const int delta  = newMax - m_maxHp;
    m_maxHp = newMax;
    if (delta > 0 && m_alive) m_hp += delta;
    if (m_hp > m_maxHp) m_hp = m_maxHp;
}

void Player::setSpeedMult(float m) {
    if (m < 0.5f) m = 0.5f;
    if (m > 2.0f) m = 2.0f;
    m_speedMult = m;
}

void Player::resetHealth() {
    m_hp      = m_maxHp;
    m_alive   = true;
    m_iframe  = 0.0f;
    m_flash   = 0.0f;
    m_respawn = 0.0f;
}

float Player::damageFlash() const {
    if (kDamageFlashTime <= 0.0f) return 0.0f;
    float f = m_flash / kDamageFlashTime;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

void Player::updateHealth(float dt) {
    if (dt <= 0.0f) return;
    if (m_iframe > 0.0f) { m_iframe -= dt; if (m_iframe < 0.0f) m_iframe = 0.0f; }
    if (m_flash  > 0.0f) { m_flash  -= dt; if (m_flash  < 0.0f) m_flash  = 0.0f; }
    if (!m_alive && m_respawn > 0.0f) {
        m_respawn -= dt;
        if (m_respawn < 0.0f) m_respawn = 0.0f;
    }
}

void Player::update(const PlayerInput& in, float dt, x3::phys::IPhysicsWorld& physics) {
    if (!m_spawned || !m_body.valid() || dt <= 0.0f) return;

    // Advance health timers every frame (iframe / flash decay, respawn countdown).
    updateHealth(dt);
    // While dead, freeze movement: the host suppresses input + shows the death
    // overlay until readyToRespawn(), then respawns the body. Still cache feet so
    // camera()/damageTargetPos() stay valid at the death spot.
    if (!m_alive) {
        const x3::phys::Vec3 feet = physics.getBodyPosition(m_body);
        m_feetX = feet.x; m_feetY = feet.y; m_feetZ = feet.z;
        return;
    }

    // ---- Mouse look: yaw from X delta, pitch from Y delta (inverted screen-Y).
    m_yaw   += in.lookDX * kMouseSens * kPxToRad;
    m_pitch -= in.lookDY * kMouseSens * kPxToRad;
    if (!m_freeLook) {                       // free look (dogfight) = no pitch wall
        if (m_pitch >  kPitchClamp) m_pitch =  kPitchClamp;
        if (m_pitch < -kPitchClamp) m_pitch = -kPitchClamp;
    }

    // ---- IDCLIP: free-fly. Move along the FULL look direction (pitch included, so
    // looking up + forward ascends), no gravity, no collision — teleport the body
    // each frame (the same setBodyPosition trick doors/monsters use). 2x speed.
    if (m_noclip) {
        const float cp = std::cos(m_pitch), sp = std::sin(m_pitch);
        const float fX = std::cos(m_yaw) * cp, fY = sp, fZ = std::sin(m_yaw) * cp;
        const float rX = -std::sin(m_yaw),     rZ = std::cos(m_yaw);
        const float spd = (in.sprint ? kSprintSpeed : kWalkSpeed) * 2.0f;
        x3::phys::Vec3 feet = physics.getBodyPosition(m_body);
        feet.x += (fX * in.moveFwd + rX * in.moveStrafe) * spd * dt;
        feet.y += (fY * in.moveFwd) * spd * dt;          // look up/down to climb/descend
        feet.z += (fZ * in.moveFwd + rZ * in.moveStrafe) * spd * dt;
        physics.setBodyPosition(m_body, feet);
        m_feetX = feet.x; m_feetY = feet.y; m_feetZ = feet.z;
        return;
    }

    // ---- SWIMMING (W10): water depth over the feet from the host-wired feed.
    // Enter deep (>1.35 m), exit shallow (<1.05 m) — hysteresis so the surface
    // boundary never jitters. No feed wired (dev worlds/tests) => depth is
    // -inf-ish and this whole block is inert.
    {
        const float waterY = m_waterQuery ? m_waterQuery(m_feetX, m_feetZ) : -3.0e38f;
        const float depth  = waterY - m_feetY;
        if (!m_swimming) {
            if (depth > kSwimEnterDepth) enterSwim(physics);
        } else if (depth < kSwimExitDepth) {
            exitSwim(physics);   // feet found ground / shallows — clean walking handoff
        }
        if (m_swimming) {
            // Move along the FULL look direction (pitch included) at ~60% speed.
            const float cp = std::cos(m_pitch), sp = std::sin(m_pitch);
            const float f3x = std::cos(m_yaw) * cp, f3y = sp, f3z = std::sin(m_yaw) * cp;
            const float r3x = -std::sin(m_yaw),     r3z = std::cos(m_yaw);
            float wx = f3x * in.moveFwd + r3x * in.moveStrafe;
            float wy = f3y * in.moveFwd;
            float wz = f3z * in.moveFwd + r3z * in.moveStrafe;
            const float wl = std::sqrt(wx * wx + wy * wy + wz * wz);
            const float spd = (in.sprint ? kSprintSpeed : kWalkSpeed) * kSwimSpeedMul * m_speedMult;
            if (wl > 1e-4f) { wx = wx / wl * spd; wy = wy / wl * spd; wz = wz / wl * spd; }
            else            { wx = wy = wz = 0.0f; }
            // Stroke up (Space held) / dive (Ctrl/C held) ride on the look-move.
            wy += ((in.jumpHeld ? 1.0f : 0.0f) - (in.diveHeld ? 1.0f : 0.0f)) * kSwimVertSpeed;
            // Buoyancy: a gentle spring that settles the EYE just above the
            // surface (rest feet = waterY - (eyeHeight - kSwimEyeAbove)). It is
            // the vertical command when the player has no vertical intent, and
            // the UPWARD CAP whenever the feet are already at/above rest — so a
            // stroke-up at the surface bobs, it never launches out of the water.
            const float restDepth = std::max(kSwimEnterDepth + 0.05f, m_eyeHeight - kSwimEyeAbove);
            const float restFeetY = waterY - restDepth;
            float spring = (restFeetY - m_feetY) * kSwimBuoyGain;
            if (spring >  kSwimBuoyMax) spring =  kSwimBuoyMax;
            if (spring < -kSwimBuoyMax) spring = -kSwimBuoyMax;
            if (std::fabs(wy) < 0.05f) wy = spring;
            // Anticipate the smoothing lag (v*tau ~0.35 m) so a fast stroke eases
            // into the rest height instead of overshooting the waterline.
            else if (m_feetY >= restFeetY - 0.35f && wy > spring) wy = spring;
            // Soft acceleration, frame-rate independent (1-exp(-k*dt)).
            const float blend = 1.0f - std::exp(-kSwimAccel * dt);
            m_swimVelX += (wx - m_swimVelX) * blend;
            m_swimVelY += (wy - m_swimVelY) * blend;
            m_swimVelZ += (wz - m_swimVelZ) * blend;
            physics.moveCharacter(m_body,
                x3::phys::Vec3{ m_swimVelX, m_swimVelY, m_swimVelZ }, dt);
            m_grounded = physics.characterGrounded(m_body);
            const x3::phys::Vec3 sfeet = physics.getBodyPosition(m_body);
            m_feetX = sfeet.x; m_feetY = sfeet.y; m_feetZ = sfeet.z;
            // Keep the walking controller's carried velocity in sync so the
            // bank exit hands off without a lurch.
            m_lastHorizX = m_swimVelX;
            m_lastHorizZ = m_swimVelZ;
            return;
        }
    }

    // ---- Horizontal basis from yaw (device forward convention, pitch ignored
    // for movement so looking up/down doesn't slow you on the ground).
    const float fx = std::cos(m_yaw), fz = std::sin(m_yaw);  // forward (XZ)
    const float rx = -fz,             rz = fx;               // right   (XZ)

    // ---- Desired horizontal target velocity from input. Crouch/crawl scale it down
    // (you move slower the lower your stance — crouch = half, prone/crawl = a slow crawl).
    const float stanceMul = (m_stance == Stance::Prone)  ? kProneSpeedMul :
                            (m_stance == Stance::Crouch) ? kCrouchSpeedMul : 1.0f;
    // [W9-3 RPG] the skill-layer speed multiplier rides ON the base constants.
    const float speed = (in.sprint ? kSprintSpeed : kWalkSpeed) * stanceMul * m_speedMult;
    float wishX = (fx * in.moveFwd + rx * in.moveStrafe);
    float wishZ = (fz * in.moveFwd + rz * in.moveStrafe);
    // Normalize so diagonal isn't faster, then scale to speed.
    const float wishLen = std::sqrt(wishX * wishX + wishZ * wishZ);
    if (wishLen > 1e-4f) { wishX = wishX / wishLen * speed; wishZ = wishZ / wishLen * speed; }
    else                 { wishX = 0.0f; wishZ = 0.0f; }

    // ---- Grounded state + coyote/jump-buffer timers.
    const bool grounded = physics.characterGrounded(m_body);
    const bool wasGrounded = m_grounded;   // last frame's state, before we overwrite it
    m_grounded = grounded;
    if (grounded) m_coyote = kCoyoteTime;
    else          m_coyote = std::max(0.0f, m_coyote - dt);
    // Track the height we left the ground at so a landing cue can be scaled by
    // how far we fell (a short hop should be quieter than a big drop).
    if (wasGrounded && !grounded) m_fallStartY = m_feetY;

    if (in.jumpPressed) m_jumpBuffer = kJumpBuffer;
    else                m_jumpBuffer = std::max(0.0f, m_jumpBuffer - dt);

    // ---- Air control: while airborne, blend the horizontal command toward the
    // wish at 50% so you keep some momentum but can still steer in the air.
    if (!grounded) {
        wishX = m_lastHorizX + (wishX - m_lastHorizX) * kAirControl;
        wishZ = m_lastHorizZ + (wishZ - m_lastHorizZ) * kAirControl;
    }
    m_lastHorizX = wishX;
    m_lastHorizZ = wishZ;

    // ---- Jump: a buffered press fires when we're within the coyote window.
    // jumpVelocity = sqrt(2 * |g| * H) lands the apex at H above the feet.
    float velY = 0.0f;
    if (m_jumpBuffer > 0.0f && m_coyote > 0.0f) {
        // m_jumpScale = 1.0 everywhere except inside the factory annex's
        // low-grav zone (setJumpScale; x1.8 there) — the default is byte-
        // identical to the old jump.
        velY = std::sqrt(2.0f * std::fabs(kGravity) * kJumpHeight) * m_jumpScale;
        m_jumpBuffer = 0.0f;   // consume
        m_coyote     = 0.0f;   // no double-jump from one ground contact
    }
    // When not jumping, leave .y = 0: the world ignores a non-positive .y and
    // sticks us to the floor internally (kGroundStick documented above).

    physics.moveCharacter(m_body, x3::phys::Vec3{ wishX, velY, wishZ }, dt);

    // Cache feet position so camera() can build the eye-height view without a
    // physics-world argument.
    const x3::phys::Vec3 feet = physics.getBodyPosition(m_body);
    m_feetX = feet.x; m_feetY = feet.y; m_feetZ = feet.z;

    // Landed this frame (airborne -> grounded transition): fire a PlayerLand cue,
    // louder for a bigger drop. A tiny step-off-a-curb hop still gets a soft cue
    // (floor at 0.15 intensity) rather than total silence.
    if (!wasGrounded && grounded) {
        const float drop = std::max(0.0f, m_fallStartY - feet.y);
        const float landIntensity = std::min(1.0f, 0.15f + drop / 3.0f);
        emitCueOrLog(m_cueSink, GameCue{ CueKind::PlayerLand, damageTargetPos(), landIntensity });
    }
}

// ---------------------------------------------------------------------------
// SWIMMING transitions (W10).
// ---------------------------------------------------------------------------
void Player::enterSwim(x3::phys::IPhysicsWorld& physics) {
    m_swimming = true;
    physics.setCharacterSwim(m_body, true);
    // Swim with the full standing capsule/eye (the rest-depth math assumes it).
    // If a low overhang refuses the grow we stay ducked — the rest-depth clamp
    // (>= enter depth) keeps the state stable either way.
    setStance(Stance::Stand, physics);
    // Carry the walking momentum into the stroke; vertical starts at rest (the
    // water absorbs the fall — that IS the splash).
    m_swimVelX = m_lastHorizX; m_swimVelY = 0.0f; m_swimVelZ = m_lastHorizZ;
    m_jumpBuffer = 0.0f;   // a buffered jump must not fire underwater / on exit
    m_coyote     = 0.0f;
    const float drop = m_grounded ? 0.0f : std::max(0.0f, m_fallStartY - m_feetY);
    const float splashIntensity = std::min(1.0f, 0.45f + drop / 4.0f);
    emitCueOrLog(m_cueSink, GameCue{ CueKind::PlayerSplash, damageTargetPos(), splashIntensity });
}

void Player::exitSwim(x3::phys::IPhysicsWorld& physics) {
    m_swimming = false;
    physics.setCharacterSwim(m_body, false);   // resets the vertical channel to 0
    // Hand the planar momentum to the walking controller (no exit lurch).
    m_lastHorizX = m_swimVelX;
    m_lastHorizZ = m_swimVelZ;
}

// ---------------------------------------------------------------------------
// Checkpoint restore (save/load). Direct, unclamped-except-pitch setters so the
// save layer can put the player state back exactly as it was captured.
// ---------------------------------------------------------------------------
void Player::setLook(float yaw, float pitch) {
    m_yaw = yaw;
    if (pitch >  kPitchClamp) pitch =  kPitchClamp;
    if (pitch < -kPitchClamp) pitch = -kPitchClamp;
    m_pitch = pitch;
}

void Player::setHp(int hp) {
    if (hp < 0) hp = 0;
    if (hp > m_maxHp) hp = m_maxHp;
    m_hp      = hp;
    m_alive   = hp > 0;
    m_iframe  = 0.0f;
    m_flash   = 0.0f;
    m_respawn = 0.0f;
}

void Player::setFeetPosition(x3::phys::IPhysicsWorld& physics, const x3::phys::Vec3& feet) {
    if (!m_spawned || !m_body.valid()) return;
    physics.setBodyPosition(m_body, feet);
    m_feetX = feet.x; m_feetY = feet.y; m_feetZ = feet.z;
}

void Player::camera(float& x, float& y, float& z, float& yaw, float& pitch) const {
    x = m_feetX;
    y = m_feetY + m_eyeHeight;
    z = m_feetZ;
    yaw = m_yaw;
    pitch = m_pitch;
}

// ===========================================================================
// Self-test (T1 walk, T2 wall-stop, T3 jump, T4 coyote)
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[player-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[player-test] FAIL ") + name); }
}

constexpr float kFixedDt = 1.0f / 60.0f;

// Flat ground at y=0 (CCW so the +Y face is solid), `half` units to a side.
x3::phys::BodyId makeGround(x3::phys::IPhysicsWorld& w, float cx, float cz, float half) {
    float v[] = {
        cx-half, 0.0f, cz-half,
        cx+half, 0.0f, cz-half,
        cx+half, 0.0f, cz+half,
        cx-half, 0.0f, cz+half,
    };
    uint32_t idx[] = { 0,2,1, 0,3,2 };
    return w.addStaticMesh(v, 4, idx, 6);
}

// Drive one frame: build input, update the player, step the world.
void frame(Player& p, x3::phys::IPhysicsWorld& w, const PlayerInput& in) {
    p.update(in, kFixedDt, w);
    w.step(kFixedDt);
}

} // namespace

bool runPlayerSelfTest() {
    g_pass = g_fail = 0;

    // ---- T1: walk advances along facing -----------------------------------
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        makeGround(*w, 0, 0, 50.0f);
        Player p;
        p.spawn(*w, 0.0f, 0.05f, 0.0f);   // yaw=0 -> faces +X
        // settle on the ground
        for (int i = 0; i < 30; ++i) frame(p, *w, PlayerInput{});
        float cx0, cy0, cz0, yaw, pitch;
        p.camera(cx0, cy0, cz0, yaw, pitch);
        // hold forward ~2s
        PlayerInput fwd; fwd.moveFwd = 1.0f;
        for (int i = 0; i < 120; ++i) frame(p, *w, fwd);
        float cx1, cy1, cz1;
        p.camera(cx1, cy1, cz1, yaw, pitch);
        // Faces +X, so x should advance meaningfully and z stay ~constant.
        bool advancedX = (cx1 - cx0) > 4.0f;
        bool straight  = std::fabs(cz1 - cz0) < 0.5f;
        check(advancedX && straight, "T1 walk advances along facing");
        w->shutdown();
    }

    // ---- T2: walking into a wall clamps forward progress (no tunneling) ----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        makeGround(*w, 0, 0, 50.0f);
        // Wall slab at x=3 (face at x=2.75), tall + wide enough to block.
        w->addBox(x3::phys::Vec3{0.25f, 2.0f, 5.0f}, x3::phys::Vec3{3.0f, 2.0f, 0.0f},
                  0.0f, x3::phys::Layer::Static);
        Player p;
        p.spawn(*w, 0.0f, 0.05f, 0.0f);   // faces +X, straight at the wall
        for (int i = 0; i < 30; ++i) frame(p, *w, PlayerInput{});
        PlayerInput fwd; fwd.moveFwd = 1.0f; fwd.sprint = true; // push hard
        for (int i = 0; i < 240; ++i) frame(p, *w, fwd);        // 4s of shoving
        float x, y, z, yaw, pitch; p.camera(x, y, z, yaw, pitch);
        // Wall face x=2.75, capsule radius 0.35 -> should stop near x<=2.5 and
        // certainly never tunnel past the wall center (x=3).
        bool stopped = x < 2.6f;
        bool noTunnel = x < 3.0f;
        check(stopped && noTunnel, "T2 wall stops forward progress (no tunnel)");
        w->shutdown();
    }

    // ---- T3: jump leaves the ground, then lands again ----------------------
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        makeGround(*w, 0, 0, 50.0f);
        Player p;
        p.spawn(*w, 0.0f, 0.05f, 0.0f);
        for (int i = 0; i < 30; ++i) frame(p, *w, PlayerInput{});
        bool groundedBefore = p.grounded();
        // one frame with the rising-edge jump
        PlayerInput jump; jump.jumpPressed = true;
        frame(p, *w, jump);
        // a few frames later we should be airborne
        bool leftGround = false;
        for (int i = 0; i < 20; ++i) {
            frame(p, *w, PlayerInput{});
            if (!p.grounded()) { leftGround = true; break; }
        }
        // then come back down and land
        bool landed = false;
        for (int i = 0; i < 120; ++i) {
            frame(p, *w, PlayerInput{});
            if (p.grounded()) { landed = true; break; }
        }
        check(groundedBefore && leftGround && landed, "T3 jump leaves ground then lands");
        w->shutdown();
    }

    // ---- T4: coyote time (jump within 0.15s of leaving a ledge still works,
    //          jumping later does not) --------------------------------------
    {
        // Helper: spawn on a platform top at y=H, walk off the +X edge into open
        // space (ground is far below), then after `waitFrames` of falling press
        // jump. Returns the gain in feet-y from the moment of the jump press to
        // the highest point reached afterwards. A successful coyote jump gives a
        // clear positive gain; a missed window gives ~none (only falls).
        auto runCoyote = [](int waitFrames) -> float {
            std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
            w->init();
            // Far-below catch floor so the character keeps falling for a while.
            makeGround(*w, 0, 0, 200.0f);
            // Platform top at y=4: a static box centered so its top face is y=4,
            // spanning x=[-2,2] (near edge for walk-off at x=2).
            w->addBox(x3::phys::Vec3{2.0f, 2.0f, 4.0f}, x3::phys::Vec3{0.0f, 2.0f, 0.0f},
                      0.0f, x3::phys::Layer::Static);
            Player p;
            p.spawn(*w, 0.0f, 4.05f, 0.0f); // feet on the platform top, faces +X
            for (int i = 0; i < 40; ++i) frame(p, *w, PlayerInput{});
            // walk forward off the +X edge until we leave the ground
            PlayerInput fwd; fwd.moveFwd = 1.0f;
            int guard = 0;
            while (p.grounded() && guard++ < 240) frame(p, *w, fwd);
            // fall for waitFrames (keep walking forward, no jump)
            for (int i = 0; i < waitFrames; ++i) frame(p, *w, fwd);
            float bx, byJump, bz, yaw, pitch; p.camera(bx, byJump, bz, yaw, pitch);
            // press jump this frame
            PlayerInput jump = fwd; jump.jumpPressed = true;
            frame(p, *w, jump);
            // track the highest feet-y reached over the next short window
            float peak = byJump;
            for (int i = 0; i < 30; ++i) {
                frame(p, *w, fwd);
                float x, yy, z; p.camera(x, yy, z, yaw, pitch);
                if (yy > peak) peak = yy;
            }
            w->shutdown();
            return peak - byJump; // upward gain after the jump press
        };

        // Within window: ~0.08s after leaving ground (5 frames @ 1/60).
        float gainEarly = runCoyote(5);
        // Outside window: ~0.30s after leaving ground (18 frames) > 0.15s coyote.
        float gainLate  = runCoyote(18);
        bool earlyJumps = gainEarly > 0.2f;   // clear upward boost
        bool lateNoJump = gainLate  < 0.05f;  // essentially just falling
        check(earlyJumps && lateNoJump, "T4 coyote (early jumps, late does not)");
    }

    // ---- T5: crouch SHRINKS the capsule so a ducked player fits a low gap that blocks a
    //          standing one; the feet stay anchored; un-crouching under a low ceiling is
    //          REFUSED (you stay crouched, no clip). ------------------------------------
    {
        // (a) Standing player blocked by a low overhead beam; crouched fits under it.
        auto runGap = [](Player::Stance st) -> float {
            std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
            w->init();
            makeGround(*w, 0, 0, 50.0f);
            // Low beam crossing x=3, underside at y=1.3 (a 1.3 m gap): a stand capsule (1.8 m)
            // can't pass; a crouch capsule (1.2 m) can.
            w->addBox(x3::phys::Vec3{0.3f, 1.0f, 5.0f}, x3::phys::Vec3{3.0f, 2.3f, 0.0f},
                      0.0f, x3::phys::Layer::Static);
            Player p;
            p.spawn(*w, 0.0f, 0.05f, 0.0f);   // faces +X, toward the beam
            for (int i = 0; i < 30; ++i) frame(p, *w, PlayerInput{});
            p.setStance(st, *w);              // duck (or stay standing)
            PlayerInput fwd; fwd.moveFwd = 1.0f;
            for (int i = 0; i < 300; ++i) frame(p, *w, fwd);
            float x, y, z, yaw, pitch; p.camera(x, y, z, yaw, pitch);
            w->shutdown();
            return x;   // how far +X we got (past the beam at x=3 means we fit through)
        };
        float xStand  = runGap(Player::Stance::Stand);
        float xCrouch = runGap(Player::Stance::Crouch);
        bool standBlocked = xStand  < 3.0f;   // stopped at/under the beam
        bool crouchFits   = xCrouch > 4.0f;   // ducked through to the far side

        // (b) Feet stay anchored when crouching: feet-y barely moves on the duck.
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        makeGround(*w, 0, 0, 50.0f);
        Player p; p.spawn(*w, 0.0f, 0.05f, 0.0f);
        for (int i = 0; i < 30; ++i) frame(p, *w, PlayerInput{});
        float fy0 = p.feet().y;
        p.setStance(Player::Stance::Crouch, *w);
        for (int i = 0; i < 5; ++i) frame(p, *w, PlayerInput{});
        bool feetAnchored = std::fabs(p.feet().y - fy0) < 0.2f;

        // (c) Un-crouch GUARD: crouched under a low ceiling, requesting Stand is refused.
        std::unique_ptr<x3::phys::IPhysicsWorld> w2(x3::phys::createPhysicsWorld());
        w2->init();
        makeGround(*w2, 0, 0, 50.0f);
        // Ceiling slab right overhead, underside at y=1.3 (only a crouch fits).
        w2->addBox(x3::phys::Vec3{5.0f, 0.5f, 5.0f}, x3::phys::Vec3{0.0f, 1.8f, 0.0f},
                   0.0f, x3::phys::Layer::Static);
        Player p2; p2.spawn(*w2, 0.0f, 0.05f, 0.0f);
        for (int i = 0; i < 30; ++i) frame(p2, *w2, PlayerInput{});
        p2.setStance(Player::Stance::Crouch, *w2);    // duck under the ceiling (succeeds)
        bool ducked = p2.stance() == Player::Stance::Crouch;
        p2.setStance(Player::Stance::Stand, *w2);     // try to stand: ceiling blocks it
        bool stayedCrouched = p2.stance() == Player::Stance::Crouch;
        w->shutdown(); w2->shutdown();

        check(standBlocked && crouchFits && feetAnchored && ducked && stayedCrouched,
              "T5 crouch shrinks capsule (fits low gap, feet anchored, un-crouch ceiling-guarded)");
    }

    // ==== SWIMMING (W10): S1 float to rest, S2 swim along look, S3 dive,
    // S4 stroke back up, S5 bank exit onto ground (no launch-out pop). One
    // synthetic pool: deep floor at y=-8, a gently-sloped bank ramp rising to
    // y=-0.5 toward +X, water surface at y=0 injected via setWaterQuery (no
    // terrain dependency — the same seam the canon host wires). ====
    {
        auto makePool = [](x3::phys::IPhysicsWorld& w) {
            // Deep floor at y=-8 (CCW, +Y solid), generous extent.
            float fv[] = { -60.0f, -8.0f, -60.0f,   60.0f, -8.0f, -60.0f,
                            60.0f, -8.0f,  60.0f,  -60.0f, -8.0f,  60.0f };
            uint32_t fi[] = { 0,2,1, 0,3,2 };
            w.addStaticMesh(fv, 4, fi, 6);
            // Bank ramp: rises -8 -> +2 across x=[6,60] (~10.5 deg — a real bank:
            // deep water, then wadable shallows, then dry ground above the line).
            float rv[] = {  6.0f, -8.0f, -8.0f,   60.0f,  2.0f, -8.0f,
                           60.0f,  2.0f,  8.0f,    6.0f, -8.0f,  8.0f };
            uint32_t ri[] = { 0,2,1, 0,3,2 };
            w.addStaticMesh(rv, 4, ri, 6);
        };
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        makePool(*w);
        Player p;
        p.spawn(*w, 0.0f, -6.0f, 0.0f);     // dropped DEEP in the pool, faces +X
        p.setWaterQuery([](float, float) { return 0.0f; });   // surface at y=0

        // ---- S1: buoyancy floats the player until the eye rests just above the
        // surface (eye ~= +0.20), swimming, and holds there without jitter. ----
        for (int i = 0; i < 360; ++i) frame(p, *w, PlayerInput{});   // 6 s idle
        float minY = 1e9f, maxY = -1e9f;
        for (int i = 0; i < 60; ++i) {                                // +1 s settled
            frame(p, *w, PlayerInput{});
            minY = std::min(minY, p.feet().y); maxY = std::max(maxY, p.feet().y);
        }
        float cx, cy, cz, cyaw, cpit; p.camera(cx, cy, cz, cyaw, cpit);
        const bool s1Swim   = p.swimming();
        const bool s1EyeOk  = std::fabs(cy - 0.20f) < 0.25f;   // eye just above the surface
        const bool s1Stable = (maxY - minY) < 0.10f;           // no surface jitter
        check(s1Swim && s1EyeOk && s1Stable,
              "S1 swim: floats to rest, eye just above surface, no jitter");

        // ---- S2: swims FORWARD along the look direction (~60% walk speed). ----
        const float s2x0 = p.feet().x;
        PlayerInput fwd; fwd.moveFwd = 1.0f;
        for (int i = 0; i < 120; ++i) frame(p, *w, fwd);   // 2 s at ~3 m/s
        const float s2dx = p.feet().x - s2x0;
        check(p.swimming() && s2dx > 3.0f && s2dx < 8.0f,
              "S2 swim: advances along look at swim speed");

        // ---- S3: dive (Ctrl held) descends well below the rest depth. ----
        PlayerInput dive; dive.diveHeld = true;
        for (int i = 0; i < 105; ++i) frame(p, *w, dive);  // 1.75 s down
        const float s3y = p.feet().y;
        check(p.swimming() && s3y < -2.6f, "S3 swim: dive descends under the surface");

        // ---- S4: stroke up (Space held) returns to the surface rest, and the
        // surface cap keeps the eye from launching out of the water. ----
        float s4Peak = -1e9f;
        PlayerInput up; up.jumpHeld = true;
        for (int i = 0; i < 240; ++i) { frame(p, *w, up); s4Peak = std::max(s4Peak, p.feet().y); }
        check(p.swimming() && p.feet().y > -1.9f && s4Peak < -1.0f,
              "S4 swim: stroke up returns to rest, capped at the surface (no launch-out)");

        // ---- S5: swimming toward the bank exits into WALKING when the feet find
        // ground in the shallows — no pop above the surface during the handoff. ----
        bool exited = false, popped = false;
        for (int i = 0; i < 1800 && !exited; ++i) {         // up to 30 s toward +X
            frame(p, *w, fwd);
            if (p.feet().y > 0.1f) popped = true;           // never above the waterline
            if (!p.swimming() && p.grounded()) exited = true;
        }
        // Handoff window: the first half second of walking is still in the
        // shallows — the exit must not launch the body over the waterline.
        for (int i = 0; i < 30; ++i) { frame(p, *w, fwd); if (p.feet().y > 0.1f) popped = true; }
        const float s5ExitY = p.feet().y;
        // Then keep walking a moment: up the bank (still ON the ramp — its top
        // is at x=60; don't walk off the far edge).
        for (int i = 0; i < 60; ++i) frame(p, *w, fwd);
        if (!(exited && !popped)) {
            x3::logError("[player-test] S5 detail: exited=" + std::to_string(exited) +
                         " popped=" + std::to_string(popped) +
                         " swim=" + std::to_string(p.swimming()) +
                         " grounded=" + std::to_string(p.grounded()) +
                         " feet=(" + std::to_string(p.feet().x) + "," +
                         std::to_string(p.feet().y) + ")");
        }
        check(exited && !popped && s5ExitY > -1.2f &&
              !p.swimming() && p.grounded() && p.feet().y > s5ExitY + 0.2f,
              "S5 swim: bank exit onto ground (walking resumes, no launch-out pop)");
        w->shutdown();
    }

    x3::logInfo(std::string("[player-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
