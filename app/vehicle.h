#pragma once
// Vehicle demo worlds (drive / boat / fly) — game/slice layer over the engine's
// GENERAL vehicle framework (engine/physics/IVehicle.h).
//
// Game/slice code only; engine/ stays pure. Three self-contained demo scenes,
// each = a dynamic rigid body + one IVehicleController + render meshes:
//   * DriveDemo : a wheeled car (Jolt VehicleConstraint) on the STREAMED terrain.
//   * BoatDemo  : a buoyant hull floating on a flat ocean (sea level), + a sub
//                 mode that can dive.
//   * FlyDemo   : an aircraft (thrust + lift + drag + attitude control).
//
// Built ONLY through the public IRenderDevice + IPhysicsWorld + IVehicleController
// interfaces. No id Tech / RBDOOM source. Low-conflict with the rest of the app
// (own world, its own --world flags).

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/physics/IVehicle.h"
#include "engine/asset/IModelLoader.h"   // hero-car GLB skin (ModelDrawable)
#include "engine/asset/IAssetSource.h"
#include "mesh_prims.h"

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace x3::game {

// A simple Y-aligned cylinder mesh (radius 1, height 1: spans y in [-0.5,0.5]).
// Used for the car wheels; the vehicle controller's wheel transform already bakes
// the radius/width scale, so the renderer just draws this unit cylinder.
void makeUnitCylinderY(uint32_t segments,
                       std::vector<x3::rhi::MeshVertex>& verts,
                       std::vector<uint32_t>& idx);

// ---------------------------------------------------------------------------
// DriveDemo — a wheeled car on flat ground OR on streamed terrain. The car is a
// chassis box (dynamic, layer Dynamic) + a WheeledVehicleController with 4 raycast
// wheels. The host steps physics; this owns the controller + render meshes and
// draws the chassis (full rotation) + the 4 wheels at their live transforms.
// ---------------------------------------------------------------------------
class DriveDemo {
public:
    // Spawn the car at (x,y,z). The chassis is a ~1300 kg box sized to the HERO
    // CAR GLB (CTR: 1.81 x 1.3 x 4.3 m, wheel stations from the model); 4 wheels
    // (front steered, rear powered + handbrake). Returns false if the controller
    // failed. build() = buildPhysics() + the graybox render meshes.
    bool build(x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
               float x, float y, float z);

    // Physics-only build (no render device) — the headless self-test path.
    bool buildPhysics(x3::phys::IPhysicsWorld& physics, float x, float y, float z);

    // HERO-CAR SKIN: load the converted vehicle GLB (clearcoat paint + emissive
    // lights) and render IT instead of the graybox box+cylinders. The GLB's
    // Wheel_FL/FR/RL/RR node drawables follow the live physics wheel poses (spin +
    // steer + suspension); everything else rides the sprung chassis. The GLB is
    // authored nose = +Z / origin on the ground; the skin bakes the 180-degree yaw
    // to the engine's -Z forward + the ride-height drop. Returns false (graybox
    // render kept) if the GLB is missing — drive still works.
    bool skin(x3::rhi::IRenderDevice& device, std::string_view glbDir,
              std::string_view relPath);
    bool skinned() const { return m_skinned; }

    // All four wheels touching ground? (drive self-test assert)
    bool allWheelsInContact() const;

    // Feed driver input + advance one step. Call setInput()+preStep() BEFORE the
    // host's physics->step(), then postStep() AFTER. drive() is a convenience that
    // does setInput()+preStep() (host steps next).
    void setInput(const x3::phys::VehicleInput& in);
    void preStep(float dt);
    void postStep(float dt);

    // Draw the chassis + wheels. Call between beginFrame/endFrame.
    void render(const x3::rhi::FrameContext& frame) const;

    void shutdown();

    x3::phys::IVehicleController* controller() const { return m_ctl.get(); }
    x3::phys::BodyId chassis() const { return m_chassis; }
    // World position of the chassis (for chasing the camera).
    void chassisPos(float out[3]) const;
    float forwardSpeed() const { return m_ctl ? m_ctl->forwardSpeed() : 0.0f; }
    float engineRPM()    const { return m_ctl ? m_ctl->engineRPM() : 0.0f; }
    int   gear()         const { return m_ctl ? m_ctl->gear() : 0; }
    // Peak longitudinal slip across all wheels (0 = rolling, 1 = full spin) —
    // drives the tire-squeal audio and the wheel-spin FX.
    float maxSlip()      const {
        if (!m_ctl) return 0.0f;
        float s = 0.0f;
        for (uint32_t i = 0; i < m_ctl->wheelCount(); ++i)
            s = std::max(s, m_ctl->longitudinalSlip(i));
        return s;
    }
    // Raw driver throttle, pre-traction-control. For HUD + engine AUDIO: tone
    // must follow LOAD, and load starts with what the driver is asking for.
    float throttleInput() const { return m_lastIn.throttle; }
    // Throttle AFTER traction control — what the engine is actually given, and
    // therefore what the engine NOTE should follow. When TC trims for slip the
    // sound must trim with it, or the audio lies about what the car is doing.
    float effectiveThrottle() const { return m_effThrottle; }
    // The audio engine model's RPM (idle + flywheel lag, decoupled from Jolt's
    // road-speed-locked RPM). The engine NOTE follows THIS, not engineRPM().
    float audioRPM() const { return m_engineRpm; }

    // ---- Performance-shop live tuning passthrough (engine WheeledTuning). ----
    // Re-tunes the RUNNING Jolt vehicle in place (engine curve/torque, mass, grip,
    // suspension, ride height, brakes) — drive out of the shop and FEEL the parts.
    bool applyTuning(const x3::phys::WheeledTuning& t) {
        return m_ctl ? m_ctl->applyWheeledTuning(t) : false;
    }
    // Nitrous: temporary torque multiplier (1 = off). Host owns the tank timer.
    // NOTE this is now COMBINED with the turbo model's multiplier rather than
    // written straight through, so a nitrous bottle and a spooling turbo can
    // both be live without one clobbering the other every step.
    void  setTorqueBoost(float mult) { m_userTorqueMult = mult; }
    float torqueBoost() const { return m_userTorqueMult; }
    // Traction control (default ON — see setInput). Off = full burnout mode.
    void  setTractionControl(bool on) { m_tcEnabled = on; }
    bool  tractionControl() const { return m_tcEnabled; }

    // THE CONTACT LAW toggle (NO_SLOP rule 11 — default ON, every WORLD keeps
    // it on). Exists ONLY for the headless self-tests, which drive on a flat
    // slab world that is NOT the streamed terrain: postStep's lifter samples
    // terrainHeightAtWorld() — the procedural WORLD field — and after the
    // W-MOUNTAIN merge that field rises ~37 m a few hundred meters from the
    // test origin, so the slab-test car was hoisted up a PHANTOM mountainside
    // at 140 mph (chassis.y = 37 m on a flat slab; broke the wheels-contact /
    // ride-height / skidpad sections, 2026-08-16). Tests on non-terrain worlds
    // call setTerrainContactLaw(false); every real host leaves it on.
    void  setTerrainContactLaw(bool on) { m_contactLaw = on; }
    bool  terrainContactLaw() const { return m_contactLaw; }

    // ---- CLIMB MODE: crawl traction for steep terrain -----------------------
    // The car is already AWD; what stops it on a mountainside is wheelspin —
    // all four spun deep into the friction plateau where grip FALLS. Climb
    // mode runs the traction controller with crawl numbers (slip held at the
    // friction peak, trim floor nearly zero) and kills the turbo so torque is
    // the full curve INSTANTLY at crawl rpm instead of arriving half a second
    // after you asked, spinning the wheels you just hooked. It overrides a
    // TC-off setting while active; leaving it restores your TC choice.
    void setClimbMode(bool on) {
        m_climbMode = on;
        setTurboEnabled(!on);
    }
    bool climbMode() const { return m_climbMode; }

    // ---- TURBO: a manifold-pressure model (see updateTurbo in vehicle.cpp) --
    // The torque CURVE is the full-boost delivery; this supplies the transient
    // that a curve cannot express — the lag before it arrives and the shove
    // when it does. Peak power is unchanged, only its timing.
    struct TurboParams {
        // 35 psi — Tim's call, overruling my 16: "Boost should hit 35 PSI!!!
        // This is a TURBO not a Supercharger." Big boost arriving late IS the
        // character of the build. The rule that survives the reversal: the
        // GAUGE ART and this number change TOGETHER (the art is now drawn
        // -10..+40 with red from 30) — the earlier defect was never the 35,
        // it was 35 psi of model under 20 psi of dial.
        float maxPsi        = 35.0f;   // peak manifold pressure over atmosphere
        // SHORTER SPOOL (2026-08-16, "MORE acceleration"): 1800/0.45 was nearly
        // half a second of nothing at WOT — the single cheapest place the car
        // was throwing away launch. 1500/0.30 keeps the turbo CHARACTER (you
        // still feel it arrive) but the shove lands while the launch is still
        // happening instead of after it. Peak power unchanged — this is timing
        // only. Live: `turbo_start` / `turbo_spool` (help text carries these
        // numbers — NO_SLOP rule 4, change together).
        float spoolStartRpm = 1500.0f; // nothing below this
        float spoolFullRpm  = 4200.0f; // full compressor above this
        float spoolTau      = 0.30f;   // seconds to build (compressor inertia)
        float dumpTau       = 0.11f;   // seconds to bleed off (wastegate/BOV)
        float vacuumPsi     = 8.5f;    // depth of vacuum at a closed throttle
        // floorTorque REMOVED 2026-08-16: the pressure-ratio model (updateTurbo)
        // derives off-boost torque from absolute manifold pressure — the old
        // ad-hoc floor had been dead for a while and its `turbo_floor` cvar was
        // a knob wired to nothing (NO_SLOP rule 6: a dead flag is a lie).
    };
    TurboParams&       turbo()       { return m_turbo; }
    const TurboParams& turbo() const { return m_turbo; }
    void  setTurboEnabled(bool on)   { m_turboOn = on; }
    bool  turboEnabled() const       { return m_turboOn; }
    // Manifold pressure, psi. NEGATIVE is vacuum, which is where a real boost
    // gauge spends most of its life.
    float boostPsi() const { return m_boostPsi; }
    // Multiplier the turbo is currently applying to engine torque.
    float turboMult() const { return m_turboMult; }

    // ---- NFS STEERING: speed-sensitive map + fast slew (2026-08-16) ---------
    // The raw input is digital (A/D = instant -1/0/+1) and used to hit the
    // wheels UNFILTERED at the full 30-deg lock: fine at parking speed, a
    // spin/twitch machine at 100 mph — one reason the car "doesn't like to
    // turn" (you couldn't hold a big steer input at speed, so you stopped
    // asking). The shaping runs in preStep (dt-scaled — the HARD rule):
    //   1) SPEED MAP: available lock is 100% at/below fullLockMph, tapering
    //      linearly to hiFrac at/above hiSpeedMph. Full angle for hairpins,
    //      tight precise angles at speed — instant turn-in without twitch.
    //   2) SLEW: the shaped target is rate-limited at slewPerSec full-lock
    //      units/s. 7/s = lock-to-lock in ~0.3 s: FAST (arcade), but the two
    //      or three frames of ramp keep a tap from being a step function, and
    //      countersteer still lands near-instantly.
    // All four knobs live: car_steer_lo / car_steer_hi / car_steer_min /
    // car_steer_rate (host_tunnel.cpp — help text carries these defaults,
    // NO_SLOP rule 4: change together).
    struct SteerParams {
        float fullLockMph = 25.0f;  // at/below this speed: 100% of max lock
        float hiSpeedMph  = 95.0f;  // at/above this speed: hiFrac of max lock
        float hiFrac      = 0.34f;  // lock fraction remaining at high speed
        float slewPerSec  = 7.0f;   // steer slew, full-lock units per second
    };
    SteerParams&       steerParams()       { return m_steerP; }
    const SteerParams& steerParams() const { return m_steerP; }
    // The shaped steering actually being sent to the wheels this step [-1,1]
    // (post speed-map, post slew). HUD / self-test telemetry.
    float steerNow() const { return m_steerNow; }

    // =========================================================================
    // NITROUS + THE THREE-STAGE SECRET (owner-blessed, 2026-08-17).
    // MOVED DOWN from host_tunnel.cpp (the standing law that produced the shared
    // AnimatedCharacter module: features every world's car should have live in
    // the SHARED VEHICLE LAYER, not in one host). host_tunnel keeps only the
    // SHIFT read + HUD/audio hooks; the tank, the kick, the overdrive and the
    // wings all live here so every world that drives this car gets them.
    //
    // THE STORY. The old host-local block had no hysteresis and recharged the
    // tank even while SHIFT was held, so at empty it oscillated around the
    // 0.02 threshold and re-fired the 2600 N·s ignition kick every ~3rd frame
    // (~20 Hz) — ~48 m/s² of raw impulse that took the car to ~379 mph. The
    // owner fell in love with the bug ("that is a crazy COOL thing"), so it is
    // now SPEC, rebuilt as a deliberate three-stage secret:
    //   STAGE 1 — DEPLETION: the instant the tank first empties while spraying,
    //     nitroJustDepleted() fires once. The host flashes "NITROUS DEPLETED" +
    //     plays the blow-off PSSSHT. The warning is the secret's camouflage:
    //     normal players release SHIFT (and the tank recharges as always).
    //   STAGE 2 — OVERDRIVE (SHIFT held past empty): the accidental threshold
    //     oscillation becomes an explicit state that fires the SAME 2600 N·s
    //     kick at a fixed 20 Hz (the accident's measured cadence), tapering in
    //     over ~1.2 s. overdrive01()/overdriveKickedThisStep() feed the host's
    //     escalating feedback (FOV punch, gauge flare, rhythmic sputter).
    //   STAGE 3 — WINGS (held 5 s past empty): wings deploy (wingDeploy01()
    //     animates, wingsJustDeployed() is the THUNK) and the car becomes a
    //     winged beast on the engine's FlightController — atmosphere-style:
    //     bank to turn, full aerobatics (rolls/loops/inverted are never fought),
    //     700 mph flat out, hands-off drag-equilibrium cruise at 277 mph, no
    //     stall. Landing at < ~60 mph with wheels down retracts the wings and
    //     gives the car back (CONTACT LAW resumes — see postStep).
    // =========================================================================
    struct NitroTuning {
        // Stage 0 — the NORMAL bottle. UNCHANGED behavior (regression-gated in
        // runWingedFlightSelfTest N1): 15 s bottle, ~20 s recharge, one kick
        // per engagement, +1.1 g shove, x1.19 torque while spraying.
        float bottleSecs      = 15.0f;   // full tank of continuous spray (Tim)
        float rechargeSecs    = 20.0f;   // empty -> full off the button (Tim)
        float kickImpulse     = 2600.0f; // N·s ignition seat-slam (fires ONCE per spray)
        float shoveForce      = 12000.0f;// N sustained while spraying (~+1.1 g)
        float sprayTorqueMult = 1.19f;   // 200-shot: +200 hp = x1.19 on this tune
        // Stage 2 — OVERDRIVE (the accident, made deliberate).
        float odKickHz        = 20.0f;   // the oscillation re-fired every ~3rd frame at 60
        float odKickImpulse   = 2600.0f; // N·s, same slam — the feel IS the kick train
        float odShoveForce    = 4000.0f; // N — the accident's 1/3-duty 12 kN shove, time-averaged
        float odTaperInSecs   = 1.2f;    // 0 -> full over this long past depletion
        float odWingsSecs     = 5.0f;    // SHIFT held this long past empty -> WINGS
    };
    struct WingTuning {
        // OWNER NUMBERS ARE SPEC (NO_SLOP rule 8): "speed up to 700mph...
        // coast at 277mph". Like the turbo's pressure-ratio model, the numbers
        // must FALL OUT of the physics, not be clamps: terminal velocity is
        // where thrust equals total drag (quadratic aero + Jolt body damping),
        // so maxThrust/drag are calibrated so full throttle equilibrates at
        // 313 m/s (700 mph) and idle thrust at 124 m/s (277 mph). The N4
        // self-test MEASURES both terminals and gates them — if these numbers
        // drift from the sim (Jolt damping change etc.), the test names it.
        float maxThrust  = 36500.0f; // N at full throttle (SHIFT — the overdrive lineage)
        float idleThrust = 9800.0f;  // N hands-off: drag-equilibrium cruise, restful
        float drag       = 0.20f;    // quadratic aero N/(m/s)^2 (PAIRED with the two above)
        // Lift = liftCoeff * |fwdSpeed| * mass, capped at 1.25 g (engine model,
        // JoltVehicle.cpp FlightController). g/124 => lift equals gravity at
        // exactly the coast speed: altitude HOLDS at cruise, gentle sink below
        // it (no stall, ever), gentle climb available above it.
        float liftCoeff  = 0.0791f;  // = 9.81 / 124 m/s (PAIRED with idleThrust)
        // Attitude authority (N·m at full input) + damping. Arcade-plane rates:
        // fast roll, honest pitch (a full-stick loop takes a few seconds),
        // mild rudder. NEVER auto-levels — a committed roll or loop is the
        // player's to keep (owner: "bank and roll and all that").
        float pitchTorque = 9000.0f;
        float yawTorque   = 6000.0f;
        float rollTorque  = 16000.0f;
        float angDamping  = 2.0f;
        // BANK-TO-TURN: yaw command injected per sin(bank), so rolling into a
        // bank CARVES the heading (the casual path), while full-roll commits
        // pass through 90° where sin ~ 1 but pitch input owns the turn. Plus a
        // mild direct rudder from steer for flat corrections.
        float carveGain   = 0.9f;
        float rudderGain  = 0.25f;
        // ARCADE NOSE-FOLLOW: velocity direction eases toward the nose at this
        // rate (1/s) so the beast flies where it points (Crimson Skies, not a
        // momentum brick) — this is what makes loops/rolls track true.
        float noseFollow  = 1.6f;
        float deploySecs  = 0.45f;   // wing pop-out animation time (the THUNK)
        float retractMph  = 60.0f;   // grounded below this -> wings away, car again
        // CRASH ("Crashing hurts, a lot"): a single-step speed loss above this
        // while winged+fast = a real crash — wings torn, tumble handed to Jolt,
        // NOS/overdrive locked out for crashLockSecs.
        float crashDeltaV    = 18.0f; // m/s lost in one 60 Hz step (~1100 m/s²)
        float crashMinSpeed  = 40.0f; // only counts as a crash above this (m/s)
        float crashLockSecs  = 6.0f;  // NOS/overdrive/wings lockout after a crash
    };
    NitroTuning&       nitroTuning()       { return m_nitroT; }
    WingTuning&        wingTuning()        { return m_wingT; }

    // Host input, once per frame BEFORE preStep: SHIFT && throttle > 0.1 (the
    // exact predicate the old host block used). In WING flight this is the
    // throttle (the overdrive lineage); on the ground it is the spray button.
    void setNitroInput(bool wantSpray) { m_wantNos = wantSpray; }
    // Flight attitude intent, once per frame BEFORE preStep (host: A/D = roll,
    // S/W (+mouse Y) = pitch up/down). Ignored while wings are retracted.
    void setFlightInput(float pitchUp, float rollRight) {
        m_flyPitchIn = pitchUp; m_flyRollIn = rollRight;
    }

    // ---- State reads (HUD / audio / camera / FX) ----------------------------
    float nosTank()      const { return m_nosTank; }     // 0..1 (the gauge)
    bool  nosSpraying()  const { return m_nosSpraying; } // stage-0 spray live
    float overdrive01()  const { return m_od01; }        // stage-2 taper 0..1
    float overdriveHeldSecs() const { return m_odHeld; }
    bool  wingsDeployed() const { return m_wings; }
    float wingDeploy01()  const { return m_wingPose; }   // 0 stowed .. 1 out
    bool  grounded() const;                              // any wheel in contact
    float crashLockout()  const { return m_crashLock; }  // >0 = post-crash lockout

    // ---- Edge events (cleared on read — call once per frame) ----------------
    bool  nitroJustDepleted()  { const bool e = m_evDepleted;  m_evDepleted  = false; return e; }
    bool  overdriveKickedThisStep() { const bool e = m_evOdKick; m_evOdKick = false; return e; }
    bool  wingsJustDeployed()  { const bool e = m_evWingsOut;  m_evWingsOut  = false; return e; }
    bool  wingsJustRetracted() { const bool e = m_evWingsIn;   m_evWingsIn   = false; return e; }
    bool  justCrashed()        { const bool e = m_evCrashed;   m_evCrashed   = false; return e; }

    // WING SKIN: textured wing GLB (armory Sci-Fi Kit Vol 3 Wing_02 — dark
    // paneled fin, 1 embedded texture), drawn mirrored L/R, scaled to a ~2.6 m
    // swept half-span, animated by wingDeploy01(). Falls back to NO wings
    // rendered if the GLB is missing (rule 3: no untextured stand-ins — the
    // flight still works, the mesh just isn't faked).
    bool skinWings(x3::rhi::IRenderDevice& device, std::string_view glbDir,
                   std::string_view relPath);

    // ---- Per-instance paint tint (WORLD CARS variants) ----------------------
    // Replaces the CLEARCOAT drawables' baseColor RGB (the car-paint panels;
    // glass/tires/trim keep their authored look) so the one live rig matches
    // whichever parked variant was entered. Also tints the graybox fallback
    // body. Off by default — the authored GLB paint, byte-identical.
    void setPaintTint(const float rgb[3]) {
        m_tint[0] = rgb[0]; m_tint[1] = rgb[1]; m_tint[2] = rgb[2]; m_tintOn = true;
    }
    void clearPaintTint() { m_tintOn = false; }

    // ---- TIRE SQUASH (render-only, hard-landing visual) ---------------------
    // Owner: "when Landing hard on pavement, the RUBBER TIRES should deflect
    // visually, a tiny bit." Detected in postStep() from each wheel's live
    // WheelState.suspensionLength (a fast compression toward the suspension's
    // min length while the wheel has contact = a hard hit), consumed in
    // render() to nudge that wheel's drawn transform — see squashFactors() in
    // vehicle.cpp for the exact math. NEVER touches physics (WheeledTuning /
    // the Jolt suspension settings are untouched) — this is cosmetic only, so
    // it cannot fight the DS-Vehicle session's tuning work.
    // `tire_squash` console cvar, 0 = off, 1 = full (default). Independent of
    // WheeledTuning on purpose (a shop part should never gate a visual).
    void  setTireSquash(float amount01) { m_tireSquash = std::clamp(amount01, 0.0f, 1.0f); }
    float tireSquash() const { return m_tireSquash; }

private:
    x3::rhi::IRenderDevice*  m_device  = nullptr;
    x3::phys::IPhysicsWorld* m_physics = nullptr;
    std::unique_ptr<x3::phys::IVehicleController> m_ctl;
    x3::phys::BodyId m_chassis;
    // Chassis half extents — sized to the hero-car GLB footprint (CTR).
    // m_hx widened 0.84 -> 0.95 so the collision box spans the 1.808 m body
    // (it was 12 cm NARROWER than the bodywork) and the wider stance.
    // 1.07 = half the widened 2.13 m body (1.808 * 1.18), so the collision box
    // spans the bodywork rather than sitting inside it.
    float m_hx = 1.07f, m_hy = 0.5f, m_hz = 1.95f;

    x3::phys::VehicleInput m_lastIn;     // raw driver input (pre-TC; HUD)
    x3::phys::VehicleInput m_effIn;      // post-TC input (preStep shapes steer on this)
    float m_effThrottle = 0.0f;          // post-TC throttle (engine audio load)
    SteerParams m_steerP;                // speed-sensitive steering map (see steerParams)
    float m_steerNow = 0.0f;             // slewed steering actually at the wheels
    bool m_tcEnabled = false;            // TC off — grip 10 hooks up and the box shifts; T toggles
    bool m_contactLaw = true;            // terrain contact-law lifter (see setTerrainContactLaw)
    bool m_tcCutting = false;            // TC hysteresis latch — stays cut until slip falls (see setInput)
    float m_tcTrim = 1.0f;               // smoothed TC trim (one-pole, see setInput)
    bool m_climbMode = false;            // crawl traction (see setClimbMode)

    void  updateTurbo(float dt);         // called from preStep
    void  updateEngineModel(float dt);   // called from preStep
    void  shapeSteering(float dt);       // called from preStep (speed map + slew)

    // ---- NITROUS / OVERDRIVE / WINGS internals (see the public block) -------
    void  updateNitro(float dt);         // called from preStep (stages 0-2 + deploy)
    void  updateWingFlight(float dt);    // called from preStep while wings are out
    void  deployWings();
    void  retractWings(bool torn);       // torn = crash (instant, no fold animation)
    NitroTuning m_nitroT;
    WingTuning  m_wingT;
    bool  m_wantNos     = false;         // host SHIFT read (setNitroInput)
    float m_nosTank     = 1.0f;          // 0..1
    bool  m_nosSpraying = false;
    float m_odHeld      = 0.0f;          // seconds SHIFT held past empty
    float m_od01        = 0.0f;          // overdrive taper 0..1
    float m_odKickClock = 0.0f;          // 20 Hz kick metronome
    bool  m_wings       = false;
    float m_wingPose    = 0.0f;          // deploy animation 0..1
    float m_crashLock   = 0.0f;          // seconds of post-crash lockout left
    float m_flyPitchIn  = 0.0f;          // host flight intent (setFlightInput)
    float m_flyRollIn   = 0.0f;
    float m_prevFlySpeed = 0.0f;         // crash detector: last step's |v|
    bool  m_evDepleted = false, m_evOdKick = false, m_evWingsOut = false,
          m_evWingsIn = false, m_evCrashed = false;
    std::unique_ptr<x3::phys::IVehicleController> m_flyCtl;  // live only while winged
    // Wing render skin (Wing_02.glb, mirrored). Loaded once via skinWings().
    bool m_wingSkinned = false;
    std::unique_ptr<x3::asset::IAssetSource>  m_wingSrc;
    std::unique_ptr<x3::asset::IModelLoader>  m_wingLoader;
    x3::asset::Model m_wingModel;
    std::vector<x3::asset::ModelDrawable> m_wingDrawL;   // node drawables (drawn twice, L/R)
    void drawWings(const x3::rhi::FrameContext& frame, const float chassisM[16]) const;

    TurboParams m_turbo;
    bool  m_turboOn         = true;
    float m_boostPsi        = 0.0f;      // manifold pressure (negative = vacuum)
    float m_turboMult       = 1.0f;      // torque multiplier the turbo is applying
    float m_userTorqueMult  = 1.0f;      // host/console override (vampire etc.), multiplied in
    float m_nosTorqueMult   = 1.0f;      // NITROUS 200-shot mult, owned by updateNitro —
                                         // the host no longer folds x1.19 into
                                         // setTorqueBoost (PAIRED with host_tunnel's
                                         // setTorqueBoost call — NO_SLOP rule 4)
    float m_engineRpm       = 800.0f;   // audio engine-model RPM (idle + flywheel lag)

    bool  m_tintOn = false;              // paint tint (see setPaintTint)
    float m_tint[3] = { 1, 1, 1 };

    x3::rhi::MeshHandle    m_chassisMesh;
    x3::rhi::MeshHandle    m_wheelMesh;
    x3::rhi::TextureHandle m_chassisTex;
    x3::rhi::TextureHandle m_wheelTex;
    std::vector<x3::phys::WheelDesc> m_wheels;

    // ---- Tire squash runtime state (see setTireSquash / squashFactors in
    // vehicle.cpp). One slot per physics wheel (4-wheel car; unused slots for
    // anything with fewer wheels are simply never touched). Updated once per
    // FIXED physics step in postStep() (deterministic dt, not render-rate),
    // consumed in render(). Pure cosmetic state — nothing here reaches Jolt.
    struct WheelSquash {
        float prevSuspLen = 0.0f;  // last step's WheelState.suspensionLength (m)
        bool  havePrev    = false; // false until the first postStep after build
        float squash      = 0.0f;  // current squash amount [0,1], 1 = strongest
        float squashVel   = 0.0f;  // critically-damped spring "velocity" term
    };
    WheelSquash m_squash[4];
    float m_tireSquash = 1.0f;   // `tire_squash` cvar multiplier, 0..1 (default 1)
    void  updateTireSquash(float dt);  // called from postStep
    // Per-wheel squash factors this frame: outSquashY = radial shrink [0,1)
    // (~4-8% at full squash), outBulge = width growth (~2-3% at full squash).
    // Both 0 when the wheel is not squashing (fast, byte-identical path).
    void  squashFactors(int slot, float& outSquashY, float& outBulge) const;

    // ---- Hero-car GLB skin (optional; graybox fallback when absent) ----
    bool m_skinned = false;
    std::unique_ptr<x3::asset::IAssetSource> m_skinSrc;
    std::unique_ptr<x3::asset::IModelLoader> m_skinLoader;
    x3::asset::Model m_skinModel;
    std::vector<x3::asset::ModelDrawable> m_bodyDraw;     // everything but the wheels
    std::vector<x3::asset::ModelDrawable> m_wheelDraw[4]; // per physics wheel slot
    void drawDrawable(const x3::rhi::FrameContext& f,
                      const x3::asset::ModelDrawable& d, const float world[16]) const;
};

// ---------------------------------------------------------------------------
// ParachuteBailout — "Crashing hurts, a lot.. and you need a parachute."
// P while airborne-with-wings ejects Jake under a canopy: a kinematic drift-
// down (steerable, CONTACT LAW landing on the terrain field) that any host
// with a flying car can run. The host draws Jake (shared AnimatedCharacter,
// fall clip) at pos() while descending and hands control back to its on-foot
// player at landing. SHARED LAYER on purpose (same law as the wings): no
// per-host descent math.
// ---------------------------------------------------------------------------
class ParachuteBailout {
public:
    void deploy(const float pos[3], const float vel[3]);
    // steerX/steerZ in [-1,1] (world-relative drift intent, e.g. from A/D +
    // W/S while descending). Returns true while still airborne.
    bool update(float dt, float steerX, float steerZ);
    bool active() const { return m_active; }
    bool landed() const { return m_landed; }
    void pos(float out[3]) const { out[0]=m_pos[0]; out[1]=m_pos[1]; out[2]=m_pos[2]; }
    void reset() { m_active = m_landed = false; }
    // Descent profile: the canopy snaps open over ~0.8 s (initial velocity
    // bleeds toward the drift), then falls at kSinkRate with kDriftRate of
    // lateral steer authority.
    static constexpr float kSinkRate  = 6.5f;   // m/s steady descent
    static constexpr float kDriftRate = 8.0f;   // m/s max lateral drift
private:
    bool  m_active = false, m_landed = false;
    float m_pos[3] = {0,0,0};
    float m_vel[3] = {0,0,0};
};

// Headless game-layer DRIVE self-test (--test-vehicle): spawn the car on a flat
// static slab, ENTER it (player control handed to the car), throttle N fixed
// ticks, assert FORWARD DISPLACEMENT + all-wheel ground contact, then EXIT and
// assert player control is restored (on-foot position placed beside the car).
// Physics-only (no render device); returns true when every check passes.
bool runDriveEnterExitSelfTest();

// Headless THREE-STAGE SECRET self-test (--test-vehicle): N1 the NORMAL bottle
// is regression-gated byte-for-feel (15 s bottle / 20 s recharge / ONE kick per
// engagement); N2 depletion fires exactly once and the tank stays empty while
// held; N3 overdrive thrust A/B-measured against a replica of the original
// threshold-oscillation accident (the feel must survive — mean accel within
// tolerance, honest numbers logged); N4 wings deploy at 5 s, full throttle
// terminals at 700 mph, hands-off settles at 277 mph holding altitude, steer
// banks-and-carves, a committed full-stick loop completes; N5 landing below
// 60 mph retracts the wings and restores the car + CONTACT LAW; N6 the
// parachute descends, steers, and lands ON the field.
bool runWingedFlightSelfTest();

// ---------------------------------------------------------------------------
// BoatDemo — a buoyant hull floating on a flat ocean at `seaLevel`. `isSub` makes
// it a submarine (can dive with the dive input). The hull is a box; buoyancy +
// drag keep it at the waterline. Renders the hull at its full transform.
// ---------------------------------------------------------------------------
class BoatDemo {
public:
    bool build(x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
               float x, float y, float z, float seaLevel, bool isSub = false);

    void setInput(const x3::phys::VehicleInput& in);
    // Move the water surface under the hull (a RIVER's surface descends
    // downstream and swells in rain — feed worldWaterLevelAt at the hull's XZ
    // each step). No-op on a flat ocean; see IVehicleController::setSeaLevel.
    void setSeaLevel(float y);
    void preStep(float dt);
    void postStep(float dt);

    void render(const x3::rhi::FrameContext& frame) const;
    void shutdown();

    x3::phys::IVehicleController* controller() const { return m_ctl.get(); }
    x3::phys::BodyId hull() const { return m_hull; }
    void hullPos(float out[3]) const;
    float submergedFraction() const { return m_ctl ? m_ctl->submergedFraction() : 0.0f; }

private:
    x3::rhi::IRenderDevice*  m_device  = nullptr;
    x3::phys::IPhysicsWorld* m_physics = nullptr;
    std::unique_ptr<x3::phys::IVehicleController> m_ctl;
    x3::phys::BodyId m_hull;
    float m_hx = 1.5f, m_hy = 0.6f, m_hz = 3.0f;

    x3::rhi::MeshHandle    m_hullMesh;
    x3::rhi::TextureHandle m_hullTex;
};

// ---------------------------------------------------------------------------
// FlyDemo — an aircraft body driven by the flight controller (thrust + lift +
// drag + pitch/yaw/roll). Renders the airframe at its full transform.
// ---------------------------------------------------------------------------
class FlyDemo {
public:
    bool build(x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
               float x, float y, float z);

    void setInput(const x3::phys::VehicleInput& in);
    void preStep(float dt);
    void postStep(float dt);

    void render(const x3::rhi::FrameContext& frame) const;
    void shutdown();

    x3::phys::IVehicleController* controller() const { return m_ctl.get(); }
    x3::phys::BodyId airframe() const { return m_body; }
    void airframePos(float out[3]) const;
    float forwardSpeed() const { return m_ctl ? m_ctl->forwardSpeed() : 0.0f; }

private:
    x3::rhi::IRenderDevice*  m_device  = nullptr;
    x3::phys::IPhysicsWorld* m_physics = nullptr;
    std::unique_ptr<x3::phys::IVehicleController> m_ctl;
    x3::phys::BodyId m_body;
    float m_hx = 2.0f, m_hy = 0.4f, m_hz = 2.6f;

    x3::rhi::MeshHandle    m_bodyMesh;
    x3::rhi::MeshHandle    m_wingMesh;
    x3::rhi::TextureHandle m_bodyTex;
};

// ---------------------------------------------------------------------------
// vehcam — hull-attitude chase-camera math for the REAL rigid-body vehicles
// (fly / boat). Unlike the car (which SYNTHESIZES bank from steer input), the
// flyer and boat are Jolt bodies whose orientation the physics already owns:
// these helpers read the body quaternion and derive a roll-capable camera
// basis (IRenderDevice::setCameraBasis) from the REAL attitude. Pure math +
// tiny persistent smoothing state, factored here so the host stays surgical
// and the math is unit-testable headlessly (--test-vehicle).
// Conventions (CONVENTIONS.md): body quat is (x,y,z,w); hull local forward =
// (0,0,-1), up = (0,1,0); setCamera fwd = (cos p cos y, sin p, cos p sin y).
// ---------------------------------------------------------------------------
namespace vehcam {

// World-space hull forward/up from the body quaternion (unit output).
void hullAxes(const float q[4], float outFwd[3], float outUp[3]);

// Signed hull roll (+ = starboard/right lean) and pitch (+ = nose up), radians,
// extracted from the world axes. Meaningful in the near-level band (the boat);
// a looping flyer should use the full basis (flyChase), not these Eulers.
void hullRollPitch(const float fwdW[3], const float upW[3],
                   float& outRoll, float& outPitch);

// FLY chase basis: the smoothed camera fwd/up trail the hull's genuine
// attitude. Target up is the hull up BLENDED toward world-up by upLevelBlend
// (a hard-mounted cam feels rigid; a chase plane lags slightly level); the
// whole basis eases at lerpRate (1/s, dt-scaled — never per-frame). Vector
// lerp on the basis handles past-vertical natively: a loop rolls the horizon
// over the top instead of pinwheeling through an Euler wall.
struct FlyCamState { float fwd[3] = {0,0,-1}; float up[3] = {0,1,0}; bool valid = false; };
void flyChase(FlyCamState& s, const float q[4], float dt,
              float upLevelBlend, float lerpRate);

// BOAT attitude-follow: smoothed FRACTIONS of the hull's wave roll and pitch
// (heavily damped — the camera is ON the water, not bolted to the hull).
struct BoatCamState { float roll = 0.0f; float pitch = 0.0f; };
void boatFollow(BoatCamState& s, const float q[4], float dt,
                float rollFrac, float pitchFrac, float lerpRate);

// Roll-capable basis from FPS yaw/pitch (the setCamera convention) + a roll
// about the view axis: up = cos(roll)*levelUp + sin(roll)*right (the car's
// banking construction, shared).
void basisYawPitchRoll(float yaw, float pitch, float roll,
                       float outFwd[3], float outUp[3]);

} // namespace vehcam

// Headless vehicle-camera self-test (--test-vehicle): asserts (1) the fly
// camera's up-vector tracks a rolled hull within tolerance (and stays level on
// a level hull — negative control), (2) the boat camera's roll is a FRACTION
// (< 0.5) of the hull roll with matching sign (level control likewise), and
// (3) the buoyancy SWELL genuinely rocks a floating hull (bounded amplitude;
// calm without it). Physics-only, no window/Vulkan.
bool runVehicleCamSelfTest();

} // namespace x3::game
