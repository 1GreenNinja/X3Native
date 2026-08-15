#pragma once
// Souped-up strata / disco elevator (core). A cab platform that travels between
// floor stops and carries the player, PLUS the full "premium glass elevator"
// experience ported from Tim's OWN Babylon game module
// (Q3Engine/src/features/x3-elevator.js — Tim's IP, NOT id Tech / RBDOOM /
// Quake; clean-room intact). Design summary: docs/design/X3_WORLD_BLUEPRINT.md
// §2.2; motion contract + the player-carry approach: specs/ELEVATOR.spec.md.
//
// TWO LAYERS, both in this one class:
//   * CORE LIFT (unchanged): the cab is a Static-layer body (mass 0) repositioned
//     each frame via setBodyPosition() (so while still it blocks like ground),
//     animated between an ordered list of stop heights. update() returns the cab's
//     per-frame vertical delta so the host carries riders (see playerRiding()).
//     With the FSM disabled (the default) the motion is byte-identical to the old
//     linear-ramp lift, so --test-elevator (E1-E6) stays green.
//   * SOUPED-UP FSM (additive, opt-in via enableFsm()): a 10-state machine
//     (IDLE..FREEFALL) with MAX_SPEED/accel/decel ramps + doors, an earth-strata
//     scroll display behind a glass wall (9 layers), twin OLED viewscreens, a
//     back-wall mirror, a blue access terminal + keypad, a ceiling light, and
//     DISCO MODE on keypad code 1127 (mirror-ball / spots / strobe cue), which
//     then drives the car DOWN to Club 1127 at Y = clubStopY (the club lane builds
//     the room there; we just drive the cab + open the doors into it).
//
// Built ONLY from X3Native's own IRenderDevice + IPhysicsWorld + Scene +
// IAudioSystem interfaces, mirroring DoorSystem's moved-static-body technique.

#include "scene.h"
#include "surface_library.h"   // real PBR cab surfaces (albedo/normal/mr)

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/audio/IAudioSystem.h"   // SoundHandle / LoopHandle (small POD)

namespace x3::audio { class IAudioSystem; }

namespace x3::game {

// One-shot + loop sound handles the elevator plays. All optional: an invalid
// (id==0) handle simply makes that cue silent (the audio backend no-ops on it),
// so a host that has no WAV for, say, the ding, still works. The motor hum is a
// SUSTAINED loop voice (startLoop/stopLoop) whose pitch tracks the cab speed.
struct ElevatorSounds {
    x3::audio::SoundHandle doorOpen;   // doors retracting
    x3::audio::SoundHandle doorClose;  // doors sealing (often the same WAV)
    x3::audio::SoundHandle ding;       // arrival / floor-pass chime
    x3::audio::SoundHandle motor;      // looped motor/cable hum (pitched by speed)
    x3::audio::SoundHandle keyClick;   // keypad digit press
    x3::audio::SoundHandle buzz;       // wrong-code / emergency buzzer
    // Cabin-experience cues (the Babylon x3-elevator.js parity pass):
    x3::audio::SoundHandle muzak;      // looped cabin muzak (rides doors-close -> arrival)
    x3::audio::SoundHandle creak;      // random cable groan while the cab travels
    x3::audio::SoundHandle doorThunk;  // panels SEAT at end of travel (fires on the exact frame)
    x3::audio::SoundHandle clubTrack;  // looped 128 BPM disco track (rides the 1127 toggle)
};

// ---------------------------------------------------------------------------
// The 10-state machine (ported 1:1 from x3-elevator.js STATE{}). The CORE lift
// only ever uses Idle/Moving (mapped from these), so the legacy moving()/state
// accessors keep working.
// ---------------------------------------------------------------------------
enum class ElevState : uint32_t {
    Idle          = 0,
    Accelerating  = 1,
    Cruising      = 2,
    Decelerating  = 3,
    Arriving      = 4,
    DoorsOpening  = 5,
    DoorsOpen     = 6,
    DoorsClosing  = 7,
    EmergencyStop = 8,
    Freefall      = 9,
};

// One earth-strata layer for the scroll display (ported from CFG.STRATA). Y
// values are RELATIVE to the facility (atmospheric geology — see blueprint
// §2.2 note: the display runs to -400 m even though the shaft bottoms higher).
struct StrataLayer {
    float       yMin, yMax;       // facility-relative Y band
    const char* name;             // layer label
    float       rgb[3];           // base color (linear)
    bool        glow;             // emissive vein layer?
    float       glowRgb[3];       // glow color when glow==true
};

// FSM tuning (ported from CFG). Public so a test / the host can tweak the ride.
struct ElevTuning {
    float maxSpeed  = 14.0f;   // m/s cruise clamp
    float accel     = 6.0f;    // m/s^2
    float decel     = 8.0f;    // m/s^2
    float decelDist = 8.0f;    // start decel within this distance of target
    float doorSpeed = 1.8f;    // seconds for a full open/close
    float doorHold  = 3.0f;    // seconds doors stay open with no rider call
    float motorIdleHz = 40.0f; // motor hum at rest
    float motorMoveHz = 120.0f;// motor hum at full speed
    float dingHz      = 880.0f;// floor-passing ding pitch reference
};

class ElevatorSystem {
public:
    // -------- CORE LIFT (unchanged API; --test-elevator depends on this) -----
    // Build one cab at shaft XZ, sitting at stopsCenterY[startStop]. cabHalf* are
    // the platform half-extents (m); stopsCenterY = ordered cab-CENTER world Y per
    // stop (low -> high). Returns true on success. Call once.
    bool build(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
               float shaftX, float shaftZ, float cabHalfX, float cabHalfY, float cabHalfZ,
               const std::vector<float>& stopsCenterY, int startStop = 0);

    // -------- ANYWHERE ELEVATOR (3D waypoint graph; additive, T1) ------------
    // One stop of the 3D graph. `center` is the cab-CENTER world position at the
    // stop; `hidden` stops stay off callNext()/floor-button cycling until
    // unlockHidden() (keypad code kAnnexCode) reveals them.
    struct Stop {
        x3::phys::Vec3 center;         // cab-CENTER world position at this stop
        const char*    label = "";     // panel/button label
        bool           hidden = false; // not in callNext()/button cycling until unlocked
    };
    // 3D build: stops + rails (undirected adjacency pairs). Legacy build()
    // forwards here with x=shaftX,z=shaftZ on every stop and a full vertical
    // rail chain, so vertical cabs behave byte-identically (m_stopsY is kept as
    // a derived mirror — m_stopsY[i] = m_stops[i].center.y — so every legacy
    // read is untouched).
    bool buildEx(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
                 float cabHalfX, float cabHalfY, float cabHalfZ,
                 const std::vector<Stop>& stops,
                 const std::vector<std::pair<int, int>>& rails, int startStop = 0);
    const x3::phys::Vec3& stopCenter(int i) const;   // full 3D stop accessor
    void unlockHidden();                             // reveals hidden stops (annex rail)
    bool hiddenUnlocked() const { return m_unlocked; }

    // Request travel to a stop index (clamped). No-op while already moving.
    void callTo(int stopIndex);
    // Cycle to the next stop, wrapping high->low. Simple "call" verb for the core.
    void callNext();

    // True if `feet` (player capsule reference point) is on the cab: XZ within the
    // footprint (+margin) and Y near the cab top. Generous window for ride detection.
    bool playerRiding(const x3::phys::Vec3& feet) const;
    // Landing lookup for "call the cab TO the caller" (real elevator manners):
    // the stop whose cab-center Y is closest to the given feet height, and a
    // stop's Y for the is-it-already-here test. Empty stops -> 0 / 0.0f.
    int nearestStopTo(float feetY) const {
        int best = 0; float bd = 1e9f;
        for (int i = 0; i < (int)m_stopsY.size(); ++i) {
            const float d = std::fabs(m_stopsY[i] - feetY);
            if (d < bd) { bd = d; best = i; }
        }
        return best;
    }
    float stopY(int i) const {
        return (i >= 0 && i < (int)m_stopsY.size()) ? m_stopsY[i] : 0.0f;
    }
    // JS checkRider parity (rider craft): walking up to an IDLE car whose doors
    // are SEALED opens them — no button press. Host feeds the player feet each
    // frame; no-op unless the FSM is on, the cab is parked, the doors are closed,
    // and the feet stand near the cab at its floor level.
    void autoOpenFor(const x3::phys::Vec3& feet);

    // Advance the cab toward its target; returns the cab's vertical delta this frame
    // (0 when idle). The host adds this to every rider's Y to carry them.
    float update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics);

    // T2 — the FULL per-frame cab delta (Vec3). Legacy float update() keeps its
    // exact old semantics (it returns this vector's Y); hosts that ride lateral
    // rails add carryDelta() to every rider position instead.
    const x3::phys::Vec3& carryDelta() const { return m_carry; }

    bool  built() const { return m_built; }
    // "moving" == any travel/door/emergency/freefall state (NOT Idle / DoorsOpen).
    bool  moving() const;
    int   targetStop() const { return m_target; }
    int   stopCount() const { return (int)m_stopsY.size(); }
    x3::phys::Vec3 cabCenter() const { return m_pos; }
    float cabTopY() const { return m_pos.y + m_halfY; }

    // Tuning (m/s). With the FSM OFF this is the constant travel speed (legacy).
    void  setSpeed(float s) { m_speed = s; }

    // -------- SOUPED-UP FSM + visuals (additive; opt-in) ---------------------
    // Turn on the 10-state FSM. While enabled, update() runs the full ramp/door
    // machine instead of the legacy linear move (still returns the per-frame
    // vertical delta + keeps the body synced). Off by default so the core test is
    // unaffected. clubStopY defaults so the cab descends to Club 1127 at Y=-200.
    void enableFsm(bool on = true);
    bool fsmEnabled() const { return m_fsm; }
    ElevState state() const { return m_state; }
    const char* stateName() const { return stateName(m_state); }
    static const char* stateName(ElevState s);

    ElevTuning& tuning() { return m_tune; }
    const ElevTuning& tuning() const { return m_tune; }

    // Build the in-car visuals (glass + strata plane + twin OLEDs + mirror + blue
    // terminal/keypad + ceiling light + disco ball) as child Scene entities offset
    // around the cab center every update(), and register the car's interior point
    // light. Safe to call after build(); a second call is a no-op.
    void buildVisuals(Scene& scene, x3::rhi::IRenderDevice& device);

    // R11 (art): the cab's own atmosphere. A SEALED LIFT is not lit by the building's
    // ambient — it is lit by the fixture in its ceiling. While the rider is aboard this
    // pulls ambient + IBL down to an interior floor (the rifthub lesson: ambient is
    // omnidirectional, so raising it lights a room by DESTROYING its contrast); when they
    // step out it restores the values the world was using. Safe to call every frame; a
    // no-op until the state actually changes. `feet` = the player body position.
    // Returns TRUE iff the cab-air state CHANGED on this call. A host that runs its OWN
    // per-zone atmosphere (the canon facility) must re-assert it when this returns true
    // and the rider has just stepped OFF — see RoomDressing::resetZoneAtmosphere().
    bool applyCabAtmosphere(x3::rhi::IRenderDevice& device, const x3::phys::Vec3& feet);

    // THE ELEVATOR WAS STAMPING THE ENGINE DEFAULTS OVER THE WHOLE BUILDING.
    // applyCabAtmosphere's "outside" branch used to RESTORE hardcoded {0.42,0.44,0.50}
    // + IBL 1.0 — the very crutch KNOWN_BUGS R2 is about — and m_cabAir starts at -1, so
    // it fired on FRAME ONE and overwrote any atmosphere the world had just set, before
    // the player had ever seen the cab. (This silently ate level1's interior air until
    // 2026-07-12.) The elevator must not INVENT a world ambient; the WORLD tells it what
    // to hand back. Defaults are the old hardcodes, so a host that never calls this is
    // byte-identical.
    //
    // LAND-LIGHTING — ONE OWNER. `fix/honest-lighting-rooms` and `fix/prim-point-light`
    // BOTH fixed this, independently, with identical semantics and identical members,
    // under two names (`setWorldAir` / `setWorldAtmosphere`). They are now ONE method
    // under prim-point-light's name, because that branch owns the ambient MODEL (R4:
    // there are two ambients, and `setAmbient` alone was a no-op wherever an env cube
    // was baked). A world declares its air HERE and via setIblProbe/setIblIntensity —
    // never by hoping setAmbient does something on its own.
    void setWorldAtmosphere(float ambR, float ambG, float ambB, float iblIntensity) {
        m_worldAmb[0] = ambR; m_worldAmb[1] = ambG; m_worldAmb[2] = ambB;
        m_worldIbl = iblIntensity;
    }
    // True while the cab owns the frame's ambient/IBL (the rider is aboard). Hosts that
    // run their own per-zone atmosphere (the canon facility) must yield to this, or the
    // two writers fight over the same device state every frame.
    bool riderAboard() const { return m_cabAir == 1; }

    // The car interior + disco point lights the host should push via
    // IRenderDevice::setPointLights (re-applied each frame by the host; the disco
    // cue animates their intensity/color in update()). Empty until buildVisuals().
    const std::vector<x3::rhi::PointLight>& pointLights() const { return m_lights; }

    // Attach an audio system for the procedural-audio hooks (motor hum sweep,
    // floor-passing dings, door SFX, disco kit). The miniaudio backend is sample-
    // based (not an oscillator graph like the JS Web-Audio source), so the hooks
    // map onto loaded one-shots where a WAV resolves and are otherwise silent
    // no-ops — exactly the "stub the calls" path the task allows. May be null.
    void setAudio(x3::audio::IAudioSystem* audio) { m_audio = audio; }

    // Wire the concrete elevator SFX (host resolves real WAVs from the asset packs
    // and passes them here). Any invalid handle leaves that cue silent. The motor
    // hum is started as a looping voice the first time the cab moves and pitched by
    // speed; it is stopped on arrival. Safe to call before or after build().
    void setSounds(const ElevatorSounds& s) { m_snd = s; }

    // The cab's current world center — where the host should treat the elevator as
    // the 3D sound emitter (door/ding/motor are spatialized to this point).
    // (cabCenter() already returns it; this name documents the audio intent.)

    // Floor labels for the directory OLED (optional; defaults to "S0..Sn").
    void setFloorLabels(const std::vector<std::string>& labels) { m_floorLabels = labels; }
    // Human label for a stop index ("F3", "B1", ...); falls back to "S<idx>" when no
    // labels were set or the index is out of range. Used for the in-world HUD prompt.
    std::string floorLabel(int stopIndex) const;
    // The cab's CURRENT nearest/arrived stop (vs targetStop(), where it's headed).
    int currentStop() const { return m_curStop; }

    // Set the Club-1127 descent target (cab-center world Y). The 1127 keypad code
    // makes the cab travel here. Default = kDefaultClubFloorY + cab half-height.
    void setClubStopY(float centerY);
    float clubStopY() const { return m_clubStopY; }

    // ===== THE RIFT STOP (W-RIFT) =============================================
    // The facility BURIED a rift-tech chamber under the detention level, and it took
    // the stop off the panel. It is a REAL floor on this cab — one more entry in the
    // stop list, labelled "RIFT" — but it is DARK on the directory and the cab will
    // not travel there until the access code (kRiftCode, 4790) is entered on the
    // cabin keypad. Exactly the 1127 pattern, which is the point: the elevator
    // already knows how to hide a floor behind a code.
    //
    // The host tells the cab which stop index is the rift level (setRiftStop) after
    // it builds the stops; callTo()/callNext() then SKIP that stop while it is
    // locked, so the panel simply never goes there. unlockRift() is the story hook
    // (a beat could open it without the code); riftUnlocked() is the query.
    static constexpr const char* kRiftAccessCode = "4790";
    void setRiftStop(int stopIndex) { m_riftStop = stopIndex; }
    int  riftStop() const { return m_riftStop; }
    void unlockRift();
    bool riftUnlocked() const { return m_riftUnlocked; }

    // ===== THE HIDDEN 4.5 STOP (fix/spire-hollow-core, owner canon 2026-07-25:
    // "Spire is on level 4.5 — HIDDEN from the other levels; only accessible via
    // Elevator. No stairways get to it.") ======================================
    // Same machinery as the RIFT stop: one more entry in the stop list, dark on the
    // directory, skipped by callTo()/callNext() until its access code is entered on
    // the cabin keypad. TAUGHT IN-WORLD (feat/secret-code-clues): the chief
    // engineer's log on the F4 holo-terminal — "double the four, double the five"
    // (Ch. Eng. Vasquez). The stairwell's unnumbered service-void door points the
    // player there ("SUBLEVEL ACCESS VIA PRIMARY LIFT ONLY - SEE CHIEF ENGINEER").
    static constexpr const char* kNexusAccessCode = "4455";
    // THE OWNER'S MASTER BACKUP (owner order 2026-07-25: "backup code 7762 opens
    // the door for me"): also unlocks the 4.5 stop — a master key masters both
    // locks (this cab stop AND the stairwell's unnumbered master door, which
    // carries the same code). Deliberately UNDOCUMENTED IN-WORLD: no terminal,
    // no chatter, no graffiti — do not teach it anywhere.
    static constexpr const char* kMasterBackupCode = "7762";
    void setSecretStop(int stopIndex) { m_secretStop = stopIndex; }
    int  secretStop() const { return m_secretStop; }
    void unlockSecret();
    bool secretUnlocked() const { return m_secretUnlocked; }

    // True iff `stopIndex` is a code-locked stop (rift / 4.5) still locked (the OLED
    // directory shows it as a dead row; the HUD refuses to offer it).
    bool stopLocked(int stopIndex) const {
        return (m_riftStop >= 0 && stopIndex == m_riftStop && !m_riftUnlocked) ||
               (m_secretStop >= 0 && stopIndex == m_secretStop && !m_secretUnlocked);
    }

    // ----- Keypad (terminal code entry; 1127 = DISCO + descend to the club,
    //       4790 = the RIFT stop unlocks + the cab descends to it) -------------
    // Push one digit (0-9) into the 4-slot code buffer. On the 4th digit the buffer
    // is checked: "1127" toggles disco mode + (when turning ON) issues a descent to
    // the club stop; any other 4-digit code clears with a buzz. Returns true iff
    // this digit completed the disco code. No-op unless the FSM is enabled.
    bool keypadDigit(int digit);
    void keypadClear() { m_codeBuf.clear(); }
    const std::string& keypadBuffer() const { return m_codeBuf; }
    bool  disco() const { return m_disco; }
    float doorPct() const { return m_doorPct; }   // 1=open, 0=closed (arrival handoff reads this)

    // Force the EMERGENCY_STOP / FREEFALL states (horror events; reachable for the
    // test). emergencyStop() halts the cab + shakes; freefall() drops it. Both no-op
    // unless the FSM is enabled.
    void emergencyStop();
    // Arm the one-shot CABLE-SLIP freefall scare (see m_slipArmed). Hosts opt in.
    void armCableSlip() { m_slipArmed = true; }
    void freefall();

    // The current facility-relative stratum name for the cab Y (geo-survey OLED).
    const char* currentStratum() const;
    // Total distance travelled (odometer; m). Read by the geo-survey OLED.
    float odometer() const { return m_totalDist; }
    // The 9 earth-strata layers (atmospheric geology; ported from CFG.STRATA).
    static const std::array<StrataLayer, 9>& strata();

    // Default Club 1127 cab-center Y reference: room floor at world Y=-200
    // (blueprint §2.2). setClubStopY()/enableFsm() add the cab half-height so the
    // cab top sits flush with the club floor.
    static constexpr float kDefaultClubFloorY = -200.0f;

private:
    void  startTravelTo(int stopIndex);   // FSM: begin a ride to a stop
    void  syncBodyAndTransform(Scene& scene, x3::phys::IPhysicsWorld& physics);
    float fsmUpdate(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics);
    float legacyUpdate(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics);
    void  updateMotorAudio(float dt);     // motor-hum loop pitch/start/stop
    void  playOneShot(x3::audio::SoundHandle s, float vol, float pitch); // 3D @ cab
    void  layoutVisuals(Scene& scene);    // reposition child entities at the cab
    void  applyDiscoCue(float dt, float t); // disco light/strata/glass cue

    // ---- Core lift state ----
    bool     m_built = false;
    uint32_t m_entity = kNoLink;
    x3::phys::BodyId m_body;
    x3::phys::Vec3   m_pos{};                 // cab CENTER world pos
    float    m_halfX = 1.5f, m_halfY = 0.15f, m_halfZ = 1.5f;
    std::vector<float> m_stopsY;              // DERIVED MIRROR: m_stops[i].center.y
                                              // (low -> high on legacy vertical cabs)

    // ---- The Anywhere Elevator: 3D waypoint graph (T1) ----
    std::vector<Stop>             m_stops;    // primary stop store (mirrored into m_stopsY)
    std::vector<std::vector<int>> m_adj;      // undirected rail adjacency per stop
    bool m_chainGraph = false;                // built via legacy build() (sorted vertical chain)
    bool m_unlocked   = false;                // hidden stops revealed (annex code)
    int  insertStopY(float centerY);          // mirror-preserving insert (disco club stop)

    // ---- Straight-segment 3D motion (T2). One leg = one straight segment; the
    // EXISTING trapezoid speed profile advances arclength m_s in [0, m_segLen]
    // instead of m_pos.y. Vertical graphs degenerate to the old math exactly.
    std::vector<int> m_route;                 // remaining waypoint stops (last == m_target)
    x3::phys::Vec3   m_segFrom{}, m_segTo{};  // active leg endpoints (world)
    x3::phys::Vec3   m_segDir{};              // normalized leg direction
    float            m_segLen = 0.0f;         // leg arclength
    float            m_s      = 0.0f;         // arclength progressed on the leg
    x3::phys::Vec3   m_carry{};               // last update()'s full cab delta
    void  buildRouteTo(int stopIndex);        // BFS over m_adj + collinear collapse
    void  beginLeg(int stopIndex);            // set the segment from m_pos to a stop
    int   nearestStopTo3D(const x3::phys::Vec3& p) const;
    int      m_target = 0;
    int      m_curStop = 0;                   // last-arrived/nearest stop (FSM tracking)
    ElevState m_state = ElevState::Idle;
    float    m_speed = 3.0f;                  // m/s travel speed (legacy constant)

    // ---- Souped-up FSM state ----
    bool     m_fsm = false;
    ElevTuning m_tune{};
    float    m_fsmSpeed = 0.0f;               // FSM ramped speed (m/s, always >=0)
    float    m_stateTime = 0.0f;
    float    m_doorPct = 1.0f;                // 1=open, 0=closed
    float    m_totalDist = 0.0f;
    float    m_shakeX = 0.0f;                 // emergency-stop shake offset
    bool     m_descendToClub = false;         // a club descent is queued/active
    float    m_clubStopY = kUninit;           // cab-center Y of the Club 1127 stop

    // ---- The rift stop (W-RIFT) ----
    int         m_riftStop     = -1;      // stop index of the RIFT level (-1 = none on this cab)
    bool        m_riftUnlocked = false;   // code 4790 accepted (or a story beat opened it)

    // ---- The hidden 4.5 stop (fix/spire-hollow-core) ----
    int         m_secretStop     = -1;    // stop index of level 4.5 (-1 = none on this cab)
    bool        m_secretUnlocked = false; // code 4455 accepted (or a story beat opened it)

    // ---- Disco / keypad ----
    bool        m_disco = false;
    float       m_discoTime = 0.0f;
    bool        m_discoSlow = false;          // 1/4-speed glide while descending in disco
    std::string m_codeBuf;

    // ---- Visuals / audio ----
    bool   m_visualsBuilt = false;
    x3::audio::IAudioSystem* m_audio = nullptr;
    std::vector<std::string> m_floorLabels;
    std::vector<x3::rhi::PointLight> m_lights;   // [0]=ceiling, [1..4]=disco spots
    // Visual child entity ids (offset around the cab center each frame). kNoLink
    // until buildVisuals(). The strata plane + disco ball glow via their emissive
    // term; the OLED / terminal / mirror are tinted boxes (graybox) the car carries.
    uint32_t m_eGlass = kNoLink, m_eStrata = kNoLink, m_eMirror = kNoLink;
    uint32_t m_eOledL = kNoLink, m_eOledR = kNoLink, m_eTerm = kNoLink;
    // OLED TELEMETRY (the Babylon twin-viewscreen parity): the two panels carry
    // LIVE text textures — LEFT = geological survey (depth/stratum/speed/temp/
    // odometer), RIGHT = floor directory with cur/target markers — rebaked ~3x/s
    // by update() via stb_truetype (the HoloTerminal on-glass move). m_device is
    // captured by buildVisuals() for the rebakes.
    x3::rhi::IRenderDevice* m_oledDevice = nullptr;
    x3::rhi::TextureHandle  m_oledTexL{}, m_oledTexR{}, m_oledMr{};
    // B7 (second order): the matte MR texel that puts the strata rock face on the PBR
    // (1/pi-normalized) path instead of the unnormalized-Lambert prim path — see buildVisuals.
    x3::rhi::TextureHandle  m_strataMr{};
    float                   m_oledTimer = 999.0f;   // first update bakes immediately
    uint32_t m_eDiscoBall = kNoLink, m_eCeil = kNoLink;
    uint32_t m_eCable[4] = { kNoLink, kNoLink, kNoLink, kNoLink };  // steel shaft cables above the car
    // Twin sliding door panels (front +X wall) that part along Z with m_doorPct, an
    // indicator strip above the doors that tints by state, and a floor numeral plate.
    uint32_t m_eDoorL = kNoLink, m_eDoorR = kNoLink, m_eIndicator = kNoLink;
    // ---- R11 "RIFT HUB GLOW-UP" cab (see the block comment in buildVisuals) --------
    // The cab had NO WALLS: it was a physics platform plus a handful of flat-tinted
    // floating boxes. You could stand in it and see the shaft's graybox around you.
    // These are the real cab shell + its practical fixture, all textured from the
    // SurfaceLibrary (albedo + normal + mr) rather than tinted flat colours.
    SurfaceLibrary m_surf;                       // brushed panels / worn deck / trim
    uint32_t m_eWallBack = kNoLink;              // -Z wall (behind the mirror)
    uint32_t m_eWallSide = kNoLink;              // +Z wall (carries the left OLED)
    uint32_t m_eWallFrontL = kNoLink, m_eWallFrontR = kNoLink;  // +X door jambs
    uint32_t m_eHeader   = kNoLink;              // +X header above the doors
    uint32_t m_eCabCeil  = kNoLink;              // the real ceiling slab
    uint32_t m_eFixture  = kNoLink;              // practical: dark housing...
    uint32_t m_eLensCore = kNoLink;              // ...with a small hot lit core inside it
    uint32_t m_eKick     = kNoLink;              // deck kick-plate / skirt
    // The -X OBSERVATION WINDOW's frame. The glass must be an APERTURE IN A WALL, not a
    // slab hanging in a hole: the cab's -X face had no wall at all, so the new practical
    // sprayed straight out of it, lit the shaft's graybox, and that lit graybox then
    // metered as the brightest thing in the car (auto-exposure duly stopped the cab down).
    uint32_t m_eWinTop = kNoLink, m_eWinBot = kNoLink;
    uint32_t m_eWinL   = kNoLink, m_eWinR   = kNoLink;
    // The floor INDICATOR is now a per-texel emissive display (a real floor readout,
    // baked with stb_truetype like the OLEDs) instead of a flat glowing bar. FOOTGUN:
    // Scene::submit only forwards emissiveTex on the mrTex.valid() PBR branch, so it
    // MUST carry an MR texel or the map is silently dropped (learned the hard way in
    // the club; see f2d86bc).
    x3::rhi::TextureHandle m_indTex{};
    int                    m_indStop = -999;     // last stop baked into m_indTex
    bool                   m_indDisco = false;   // last disco state baked
    bool                   m_indMoving = false;  // last motion state baked
    int                    m_cabAir = -1;        // -1 unset, 0 = world air, 1 = cab air
    float                  m_worldAmb[3] = { 0.42f, 0.44f, 0.50f };  // air OUTSIDE the cab; see setWorldAtmosphere
    float                  m_worldIbl    = 1.0f;
    // Per-frame audio bookkeeping.
    float  m_motorHz = 40.0f;
    int    m_lastDingStop = -1;
    ElevatorSounds        m_snd{};            // host-wired SFX (any may be invalid)
    x3::audio::LoopHandle m_motorLoop{};      // live motor-hum voice (0 == none)
    // Cabin experience (JS parity): muzak rides each trip; cable creaks fire on a
    // jittered timer while travelling; HORROR events roll on arrival (8 %, or
    // always on the SUB stop) — a light-flicker+creak or a brief emergency stop.
    x3::audio::LoopHandle m_muzakLoop{};      // live muzak voice (0 == none)
    x3::audio::LoopHandle m_clubLoop{};       // live 128 BPM disco voice (0 == none)
    float    m_creakTimer  = 4.0f;            // seconds to the next cable groan
    float    m_flickerT    = 0.0f;            // >0: interior light is dipped (horror)
    float    m_lightSaveR  = 0.0f, m_lightSaveG = 0.0f, m_lightSaveB = 0.0f;
    uint32_t m_rng         = 0x1127u;         // tiny deterministic LCG (creak jitter + horror roll)
    // THE CABLE SLIPS — a once-per-ride-line scripted freefall scare. OPT-IN via
    // armCableSlip() (hosts arm it; tests/canon stay deterministic): on the first
    // LONG descent, past the shaft's halfway line, the cable lets go — lights die,
    // the cab drops (Freefall), the emergency brakes CATCH it (EmergencyStop),
    // and the ride quietly resumes to the original stop. Never fires in disco.
    bool  m_slipArmed   = false;
    bool  m_slipDone    = false;
    bool  m_slipAlarmed = false;              // the mid-fall alarm fired
    bool  m_pendingResume = false;            // emergency recovery re-calls m_resumeStop
    int   m_resumeStop  = -1;
    float m_slipStartY  = 0.0f;
    bool   m_doorWasOpening = false;          // edge-detect for the door SFX
    bool   m_doorWasClosing = false;

    // Sentinel for "club stop not set yet".
    static constexpr float kUninit = -1.0e30f;
};

// Headless self-test (--test-elevatorfsm). Drives the FSM through a normal ride
// (IDLE->ACCEL->CRUISE->DECEL->ARRIVING->DOORS->IDLE), asserts the state sequence
// + that the cab reaches the target floor Y + the speed ramps/clamps, asserts the
// 1127 keypad triggers DISCO + a descent target of Y=-200, and that FREEFALL /
// EMERGENCY are reachable. Prints "elevatorfsm: X/Y passed". Leak-clean; returns
// true iff all assertions pass.
bool runElevatorFsmSelfTest();

} // namespace x3::game
