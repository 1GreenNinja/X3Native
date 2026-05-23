#pragma once
// First-person walking character controller (S3).
//
// Game/slice code only — engine/ stays pure. The Player wraps a Jolt
// CharacterVirtual (via IPhysicsWorld::createCharacter) and turns abstract
// PlayerInput into a desired horizontal velocity + jump, manages mouse-look
// (yaw/pitch), and exposes an eye-height camera. Movement parameters are the
// values extracted in docs/ASSET_INVENTORY.md "Character Controller Parameters".
//
// Input is abstracted (PlayerInput) so the controller is testable headlessly
// with synthetic input and no GLFW/Vulkan — see --test-player.
//
// S4-S6 build on this: use camera() forward for the weapon raycast, body() for
// physics queries, and grounded() for gameplay state.

#include "engine/physics/IPhysicsWorld.h"

namespace x3::game {

// ---------------------------------------------------------------------------
// Player health tuning (EFLZ Phase 2a, spec §6.6). Placeholders per
// docs/EFLZ_DESIGN.md; treat as tuning targets.
// ---------------------------------------------------------------------------
// Starting + max health.
constexpr int   kPlayerMaxHp   = 100;
// Invulnerability ("iframe") window after taking a hit (seconds). DPS is gated
// by this so a single attack can only land once per window, keeping melee fair.
constexpr float kPlayerIFrame  = 0.5f;
// Damage-flash duration (seconds): how long the red full-screen flash lingers
// after a hit (drives the HUD damage feedback). Decayed by Player::update().
constexpr float kDamageFlashTime = 0.4f;
// Respawn delay (seconds) after death before the player is restored to the
// level-start checkpoint at full HP.
constexpr float kRespawnDelay  = 2.0f;

// One frame of abstracted player input. Decoupled from GLFW so the controller
// can be driven by synthetic input in tests.
struct PlayerInput {
    float moveFwd    = 0;      // -1..1  (W = +1, S = -1) along facing
    float moveStrafe = 0;      // -1..1  (D = +1, A = -1) along right
    bool  sprint     = false;  // hold to move at sprint speed
    bool  jumpPressed = false; // rising edge only (true the frame Space goes down)
    float lookDX = 0;          // mouse delta X this frame (pixels)
    float lookDY = 0;          // mouse delta Y this frame (pixels)
};

// Abstract damage receiver so enemy attacks can hurt "the player" without the
// monster system depending on the concrete Player (keeps engine/game seams clean
// and lets the headless test substitute a trivial sink). Player implements it.
class IDamageSink {
public:
    virtual ~IDamageSink() = default;
    // Apply `amount` damage. Returns true iff the hit actually landed (i.e. it was
    // not absorbed by an invulnerability window). Implementations own any iframe /
    // alive gating; a no-op (returns false) when dead or invulnerable.
    virtual bool takeDamage(int amount) = 0;
    // World position the attacker should aim at (eye/center). Used by ranged
    // enemies to point a hitscan/tracer at the target.
    virtual x3::phys::Vec3 damageTargetPos() const = 0;
    // True while the sink can still be hurt / is a valid target.
    virtual bool isAlive() const = 0;
};

class Player : public IDamageSink {
public:
    // Create the character capsule at feet position (x,y,z). Call once.
    void spawn(x3::phys::IPhysicsWorld& physics, float x, float y, float z);

    // Advance one frame: integrate look, build desired velocity, manage coyote
    // time + jump buffer, issue moveCharacter(). Does NOT call physics.step()
    // (the caller steps the world afterwards so all bodies advance together).
    void update(const PlayerInput& in, float dt, x3::phys::IPhysicsWorld& physics);

    // Eye-height camera state for IRenderDevice::setCamera. Position is the
    // capsule feet + eye height; yaw/pitch are the look angles (radians) in the
    // device's forward convention: fwd = (cos p cos y, sin p, cos p sin y).
    void camera(float& x, float& y, float& z, float& yaw, float& pitch) const;

    // Grounded this frame (cached from the last update()).
    bool grounded() const { return m_grounded; }

    // The underlying physics character body.
    x3::phys::BodyId body() const { return m_body; }

    // Look angles (radians). Exposed for tests / debug HUD.
    float yaw() const { return m_yaw; }
    float pitch() const { return m_pitch; }

    // ---- Health / damage / death (Phase 2a, spec §6.6) --------------------
    // Current + max HP, and alive state.
    int  hp() const { return m_hp; }
    int  maxHp() const { return m_maxHp; }
    bool isAlive() const override { return m_alive; }
    bool dead() const { return !m_alive; }

    // Apply damage (IDamageSink). Ignored (returns false) while the iframe window
    // is active or already dead. On a real hit: subtract HP (clamped at 0), open
    // the iframe window, raise the damage flash, and — if HP hits 0 — enter the
    // death state and start the respawn countdown.
    bool takeDamage(int amount) override;

    // The position enemies should aim at: the eye (feet + eye height).
    x3::phys::Vec3 damageTargetPos() const override;

    // Heal to a specific value (clamped to [0,maxHp]); default fully heals.
    void heal(int amount = kPlayerMaxHp);

    // Reset health/iframe/flash/death state to a fresh, full-HP, alive player.
    // Does NOT move the body — the caller respawns position via setBodyPosition.
    void resetHealth();

    // True for kPlayerIFrame seconds after a hit (no new damage lands).
    bool invulnerable() const { return m_iframe > 0.0f; }

    // IDDQD god mode (console cheat): while on, takeDamage() absorbs everything.
    void setGod(bool g) { m_god = g; }
    bool god() const { return m_god; }

    // Damage-flash strength in [0,1] for the HUD red flash (1 right after a hit,
    // decays to 0 over kDamageFlashTime). Drives drawDamageFlash().
    float damageFlash() const;

    // Remaining respawn countdown (seconds) while dead; 0 once it elapses (the
    // host then respawns). Only meaningful while dead().
    float respawnTimer() const { return m_respawn; }
    // True the moment the respawn delay has elapsed and the host should respawn.
    bool readyToRespawn() const { return !m_alive && m_respawn <= 0.0f; }

    // Advance health timers (iframe decay, flash decay, respawn countdown). The
    // movement update() also calls this; exposed so the host/test can tick health
    // independently (e.g. while dead, when movement input is suppressed).
    void updateHealth(float dt);

    // ---- Checkpoint restore (save/load) -----------------------------------
    // Restore the look angles (radians) directly. Used by the save layer to put the
    // camera back exactly where it was at the saved checkpoint. No clamping beyond
    // the normal pitch clamp; gameplay is otherwise unaffected.
    void setLook(float yaw, float pitch);
    // Restore current HP to an exact value (clamped to [0,maxHp]) and mark the
    // player alive iff hp>0, clearing the iframe/flash/respawn timers. This is the
    // load-time restore of a checkpoint's health (a clean restore, not "heal").
    void setHp(int hp);
    // Re-seat the capsule at `feet` world position (teleport). Wraps
    // physics.setBodyPosition + refreshes the cached feet so camera() is valid
    // immediately. Requires spawn() to have run.
    void setFeetPosition(x3::phys::IPhysicsWorld& physics, const x3::phys::Vec3& feet);
    // Cached feet (capsule reference) world position from the last update()/spawn().
    // The save layer reads this to capture the player transform.
    x3::phys::Vec3 feet() const { return x3::phys::Vec3{ m_feetX, m_feetY, m_feetZ }; }

private:
    x3::phys::BodyId m_body;
    float m_yaw   = 0.0f;   // around +Y; 0 looks toward +X
    float m_pitch = 0.0f;   // up/down; clamped to +/- kPitchClamp

    bool  m_grounded   = false;
    float m_coyote     = 0.0f;   // time-since-grounded countdown (s)
    float m_jumpBuffer = 0.0f;   // remaining jump-buffer window (s)
    bool  m_spawned    = false;

    // Cached feet position from the last update() (camera() has no world arg).
    float m_feetX = 0.0f, m_feetY = 0.0f, m_feetZ = 0.0f;
    // Horizontal velocity carried between frames for air control blending.
    float m_lastHorizX = 0.0f, m_lastHorizZ = 0.0f;

    // ---- Health / damage / death state (Phase 2a) -------------------------
    int   m_hp        = kPlayerMaxHp;   // current health
    int   m_maxHp     = kPlayerMaxHp;   // max health (start value)
    bool  m_alive     = true;
    float m_iframe    = 0.0f;           // remaining invuln window (s)
    float m_flash     = 0.0f;           // remaining damage-flash time (s)
    float m_respawn   = 0.0f;           // remaining respawn countdown while dead (s)
    bool  m_god       = false;          // IDDQD: ignore all incoming damage
};

// Headless self-test (T1 walk, T2 wall-stop, T3 jump, T4 coyote). Builds its own
// physics world (floor + wall) and drives synthetic input. Logs PASS/FAIL T#.
// Returns true iff all pass. Mirrors runPhysicsSelfTest() et al.
bool runPlayerSelfTest();

// Headless self-test (--test-phase2a, EFLZ Phase 2a). Asserts: a Guard within
// range reduces player HP over time; a Drone at standoff range reduces player HP
// (ranged); HP->0 triggers death then a respawn restores full HP; iframes prevent
// multiple hits landing in one frame/window; Guard vs Drone tuning params differ.
// Logs PASS/FAIL T#, returns true iff all pass. No window/Vulkan. Lives in
// monster.cpp (where the MonsterManager + HeadlessDevice are). Mirrors the other
// self-tests.
bool runPhase2aSelfTest();

} // namespace x3::game
