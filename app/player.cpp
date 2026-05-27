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
constexpr float kPitchClamp   = 80.0f * 3.14159265358979f / 180.0f; // +/- 80 deg
// Ground-stick: doc value is -2.0 m/s. The world ignores a negative .y in
// moveCharacter and already sticks the character to the floor internally during
// step(), so we keep .y = 0 when grounded/not jumping and rely on that. The
// constant is retained for documentation / future use.
constexpr float kGroundStick  = -2.0f;
} // namespace

void Player::spawn(x3::phys::IPhysicsWorld& physics, float x, float y, float z) {
    m_body = physics.createCharacter(kCapsuleRadius, kStandHeight,
                                     x3::phys::Vec3{ x, y, z });
    m_grounded   = false;
    m_coyote     = 0.0f;
    m_jumpBuffer = 0.0f;
    m_spawned    = true;
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
        x3::logInfo("[player] took " + std::to_string(amount) +
                    " damage — HP now " + std::to_string(m_hp));
    }
    return true;
}

x3::phys::Vec3 Player::damageTargetPos() const {
    return x3::phys::Vec3{ m_feetX, m_feetY + m_eyeHeight, m_feetZ };
}

void Player::setStance(Stance s) {
    m_stance    = s;
    m_eyeHeight = (s == Stance::Prone)  ? kProneEye :
                  (s == Stance::Crouch) ? kCrouchEye : kEyeHeight;
}

void Player::heal(int amount) {
    if (amount <= 0) return;
    m_hp += amount;
    if (m_hp > m_maxHp) m_hp = m_maxHp;
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
    if (m_pitch >  kPitchClamp) m_pitch =  kPitchClamp;
    if (m_pitch < -kPitchClamp) m_pitch = -kPitchClamp;

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

    // ---- Horizontal basis from yaw (device forward convention, pitch ignored
    // for movement so looking up/down doesn't slow you on the ground).
    const float fx = std::cos(m_yaw), fz = std::sin(m_yaw);  // forward (XZ)
    const float rx = -fz,             rz = fx;               // right   (XZ)

    // ---- Desired horizontal target velocity from input. Crouch/crawl scale it down
    // (you move slower the lower your stance — crouch = half, prone/crawl = a slow crawl).
    const float stanceMul = (m_stance == Stance::Prone)  ? kProneSpeedMul :
                            (m_stance == Stance::Crouch) ? kCrouchSpeedMul : 1.0f;
    const float speed = (in.sprint ? kSprintSpeed : kWalkSpeed) * stanceMul;
    float wishX = (fx * in.moveFwd + rx * in.moveStrafe);
    float wishZ = (fz * in.moveFwd + rz * in.moveStrafe);
    // Normalize so diagonal isn't faster, then scale to speed.
    const float wishLen = std::sqrt(wishX * wishX + wishZ * wishZ);
    if (wishLen > 1e-4f) { wishX = wishX / wishLen * speed; wishZ = wishZ / wishLen * speed; }
    else                 { wishX = 0.0f; wishZ = 0.0f; }

    // ---- Grounded state + coyote/jump-buffer timers.
    const bool grounded = physics.characterGrounded(m_body);
    m_grounded = grounded;
    if (grounded) m_coyote = kCoyoteTime;
    else          m_coyote = std::max(0.0f, m_coyote - dt);

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
        velY = std::sqrt(2.0f * std::fabs(kGravity) * kJumpHeight);
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

    x3::logInfo(std::string("[player-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
