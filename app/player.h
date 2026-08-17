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
#include "cues.h"   // GameCueFn / CueKind (PlayerPain / PlayerLand audio hooks)

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
    // W10 SWIMMING held-key channels (only read while swimming; default false so
    // every existing PlayerInput{} caller is unaffected).
    bool  jumpHeld   = false;  // Space HELD — stroke up toward the surface
    bool  diveHeld   = false;  // Ctrl/C HELD — dive down
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

    // FREE LOOK: unclamp pitch (full up/over). Used by the space dogfight, where the
    // enemy can be anywhere on the sphere and the ±90° head clamp walls it off-view.
    // Off for on-foot play (the ±89.9° head clamp stays). Yaw is always unlimited.
    void setFreeLook(bool f) { m_freeLook = f; }
    bool freeLook() const { return m_freeLook; }

    // ---- Stance (crouch / crawl) ------------------------------------------
    // Stand (full eye height + full move speed), Crouch (C, ducked eye + half
    // speed), Prone (Left-Ctrl, crawling eye + a slow crawl). Lowers the camera
    // (eye height) AND shrinks the physics capsule (Stand 1.8 m -> Crouch 1.2 m ->
    // Prone 0.7 m) so a ducked player actually fits low gaps. The feet stay anchored
    // (the capsule shrinks/grows upward — no pop). UN-crouching is GUARDED: if a ceiling
    // is too low for the taller capsule the request is refused and we stay crouched
    // (no clip/crash). Idempotent: requesting the current stance is a no-op. Needs the
    // physics world to resize the CharacterVirtual.
    enum class Stance : uint32_t { Stand = 0, Crouch = 1, Prone = 2 };
    void   setStance(Stance s, x3::phys::IPhysicsWorld& physics);
    Stance stance() const { return m_stance; }
    // Capsule total height (m) for a stance — exposed for tests / debug.
    static float stanceHeight(Stance s);

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

    // ---- [W9-3 RPG] progression stat layer -------------------------------
    // Multipliers/bonuses LAYERED on the base tuning constants (kPlayerMaxHp /
    // kWalkSpeed are never mutated). The skill tree's Survival branch drives
    // these through the host's applyRpgStats().
    // Max HP becomes kPlayerMaxHp + bonus; raising grants the delta as HP.
    void  setMaxHpBonus(int bonus);
    // Planar move-speed multiplier (clamped [0.5, 2.0]; default 1).
    void  setSpeedMult(float m);
    float speedMult() const { return m_speedMult; }

    // IDDQD god mode (console cheat): while on, takeDamage() absorbs everything.
    void setGod(bool g) { m_god = g; }
    bool god() const { return m_god; }

    // IDCLIP noclip (console cheat): free-fly with no gravity/collision; movement
    // follows the full look direction (look up + forward to ascend). setGod is
    // usually paired so you don't take environmental damage while flying.
    void setNoclip(bool n) { m_noclip = n; }
    bool noclip() const { return m_noclip; }

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

    // ---- SWIMMING (W10) ----------------------------------------------------
    // The water-level FEED: a pure query returning the water SURFACE Y over
    // world (x,z), or a very-negative "dry" sentinel (e.g. terrain.h's
    // kWorldWaterDry / -FLT_MAX) when there is no water there. Host-wired (the
    // canon host passes x3::game::worldWaterLevelAt) instead of Player calling
    // the terrain query directly, so (a) dev worlds/tests that have no world
    // water are bit-identical with no feed set, and (b) the headless self-test
    // injects synthetic water without any terrain dependency. Player never
    // includes terrain.h.
    using WaterQueryFn = std::function<float(float x, float z)>;
    void setWaterQuery(WaterQueryFn fn) { m_waterQuery = std::move(fn); }
    // ---- JETPACK (the `fly` command) ---------------------------------------
    // Diegetic flight for the on-foot character — NOT noclip: collision stays
    // on (moveCharacter), the mesh keeps drawing/animating, and THE CONTACT
    // LAW still owns the boots. Mode on = the pack is WORN; flight itself
    // engages on the first thrust (W / Space) and hands back to plain walking
    // the moment the feet touch down slow — so `fly` once, hop around the
    // world, land, walk, lift off again, `fly` again to take the pack off.
    //
    // FLIGHT MODEL (owner's numbers are spec — NO_SLOP rule 8):
    //   * hold W  = thrust toward the FULL look direction (pitch = altitude),
    //     spooling up over a few seconds to 300 mph (134.1 m/s, kJetTopSpeed);
    //   * hold S  = air-brake (hard velocity bleed);
    //   * A/D     = gentle lateral nudge; Space/Ctrl = rise/sink channels;
    //   * no input = auto-hover: velocity eases to zero and altitude holds
    //     (gravity is off through the same physics switch swimming uses);
    //   * landing = cut thrust, descend: inside the last few metres a FLARE
    //     caps the sink rate, and touchdown returns the walking controller.
    //     The ground probe raycasts DOWN FROM THE FEET — never from above the
    //     head (the tunnel-lid trap, NO_SLOP rule 11 v3).
    void setJetpack(bool on, x3::phys::IPhysicsWorld& physics);
    bool jetpack()   const { return m_jetpack; }    // pack worn (mode on)
    bool jetFlying() const { return m_jetFlying; }  // actually airborne under thrust
    // Live speed through the air (m/s) — HUD readout + FOV ease.
    float jetSpeed() const;

    // True while the swim state is active (deep water). While swimming:
    // gravity is off (physics swim mode), a gentle buoyancy spring settles the
    // eye just above the surface, movement follows the FULL look direction
    // (pitch included) at ~60% walk speed with soft dt-scaled acceleration,
    // Space held strokes up, Ctrl/C held dives. Enter at depth > 1.35 m, exit
    // at depth < 1.05 m (hysteresis — no jitter at the surface boundary).
    bool swimming() const { return m_swimming; }

    // ---- Audio hook (audio-assets pass, W2-B) ------------------------------
    // Wire a cue sink so the host can give the PLAYER a voice: a PlayerPain cue
    // fires when takeDamage() lands a real hit, a PlayerLand cue fires the frame
    // the character transitions airborne -> grounded (louder for a bigger fall).
    // Mirrors MonsterSystem::setCueSink (see app/monster.h / app/cues.h). Default
    // is an empty std::function, which emitCueOrLog() turns into a throttled log
    // line instead of a crash — so this compiles and runs with NO host wiring;
    // the host only needs to call player.setCueSink(...) to hear it (see the
    // one-line subscription noted in app/cues.h's PlayerPain/PlayerLand comment).
    void setCueSink(const GameCueFn& sink) { m_cueSink = sink; }

private:
    x3::phys::BodyId m_body;
    float m_yaw   = 0.0f;   // around +Y; 0 looks toward +X
    float m_pitch = 0.0f;   // up/down; clamped to +/- kPitchClamp unless m_freeLook
    bool  m_freeLook = false; // true = no pitch clamp (space dogfight)

    // Stance state (crouch/crawl). m_eyeHeight tracks the stance (== kEyeHeight when
    // standing); the planar move speed is scaled by a per-stance multiplier in update().
    Stance m_stance    = Stance::Stand;
    float  m_eyeHeight = 1.6f;   // == kEyeHeight (Stand default)

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
    bool  m_noclip    = false;          // IDCLIP: free-fly, no gravity/collision

    // [W9-3 RPG] progression stat layer (see setMaxHpBonus/setSpeedMult).
    float m_speedMult = 1.0f;

    // ---- SWIMMING state (W10) ----------------------------------------------
    void enterSwim(x3::phys::IPhysicsWorld& physics);
    void exitSwim(x3::phys::IPhysicsWorld& physics);
    WaterQueryFn m_waterQuery;            // host-wired; empty => never swims
    bool  m_swimming = false;
    float m_swimVelX = 0.0f, m_swimVelY = 0.0f, m_swimVelZ = 0.0f; // smoothed swim velocity

    // ---- JETPACK state (the `fly` command) ---------------------------------
    bool  m_jetpack   = false;            // pack worn (mode toggled on)
    bool  m_jetFlying = false;            // airborne under the flight model
    float m_jetVelX = 0.0f, m_jetVelY = 0.0f, m_jetVelZ = 0.0f; // smoothed flight velocity
    // Seconds of hands-off flight. Below kJetAssistDelay the pack COASTS on
    // thin-air drag; past it the station-keeping hover brake engages. Borrowed
    // from SpacePilotController::Tuning::assistDelay (app/space_pilot.h) —
    // paired, and the receipt lives on kJetAssistDelay in player.cpp.
    float m_jetIdleFor = 0.0f;

    // ---- Audio hook state (audio-assets pass, W2-B) -----------------------
    GameCueFn m_cueSink;                // host-wired; empty => throttled log (see cues.h)
    float     m_fallStartY = 0.0f;      // feet-Y captured the instant we left the ground
};

// Headless self-test (T1 walk, T2 wall-stop, T3 jump, T4 coyote, T5 crouch +
// W10 SWIMMING S1-S5: float-to-rest buoyancy / swim-along-look / dive / stroke
// up / bank exit with no launch-out pop — synthetic water via setWaterQuery).
// Builds its own physics world (floor + wall / pool + bank ramp) and drives
// synthetic input. Logs PASS/FAIL T#/S#. Returns true iff all pass. Mirrors
// runPhysicsSelfTest() et al.
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
