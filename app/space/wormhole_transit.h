// app/space/wormhole_transit.h
//
// S3 wormhole transit — the Salvari crystal-matrix interstellar jump, AS A RIDE.
//
// WHAT THIS WAS, AND WHAT IT IS NOW
// ---------------------------------
// This started as a 6-second timer. It registered a wormhole RUNNER (a
// SpaceLayer::TransitionFn) with the S0 spine, ramped `progress` 0..1, and drew
// the crystal-matrix WormholeVfx tube AT THE WORLD ORIGIN while the player went
// on flying their ship normally somewhere else entirely. `requestWormhole()`
// discarded its destination argument. Nothing about it was a journey.
//
// It is now the JOURNEY. The same runner still drives the same S0 spine (that
// contract is frozen and untouched), but the class now also owns:
//
//   * A STAGED TIMELINE. Entry (the punch through the membrane) -> Tunnel (you
//     are INSIDE the throat) -> Exit (the convergence blooms and lets you out).
//     Every stage boundary is a function of ACCUMULATED TIME, never of a frame
//     count, so a 60 Hz run and a 165 Hz run take the same wall-clock trip and
//     arrive at the same place (--test-wormhole-transit proves this).
//
//   * THE RIDE FRAME. rollRad()/bankX()/bankY()/axialDistance() are the tunnel's
//     motion: the throat spins about its own axis, swings laterally around the
//     ship, and streams past at a distance that is INTEGRATED from dt. The host
//     feeds these straight into WormholeVfx::setOrigin/setRoll, so the walls of
//     twisting light are the SAME membrane machinery the exterior lanes built —
//     seen from the inside, not reinvented.
//
//   * THE OFF-SCALE INSTRUMENTATION. readSpeedMs()/readPos()/distanceRemainLy()
//     are the numbers the HUD spins past legibility. This is the detail the
//     owner named: the readouts are what tell the player the game knows this is
//     not normal flight. They live HERE, not in the host, so they are unit
//     testable and so both the windowed loop and the headless capture path read
//     one source of truth.
//
//   * THE AEGIS SCRIPT. A small queue of comms lines the transit emits at its
//     own beats (departure / mid-transit / arrival), with a DISTINCT and
//     worrying set for an unstable corridor. The host pops them and posts them
//     to the comms bus; it does not decide what AEGIS says.
//
//   * THE DESTINATION. TransitPlan carries where you are going. `arrival()`
//     hands the host the star system to land in. A transit that returns you
//     where you started is not a transit.
//
// Owns the WormholeVfx (GPU). The Context state machine still lives in
// SpaceLayer; this class contributes the runner, the ride model, and the draw.
#pragma once

#include "engine/rhi/IRenderDevice.h"

#include <cstdint>

namespace x3::space {

class SpaceLayer;       // S0 spine (frozen interface)
class WormholeVfx;      // crystal-matrix tunnel VFX (feat/wormhole-vfx)

// ---------------------------------------------------------------------------
// The three beats of a jump. Idle when nothing is in flight.
//
//   Entry  : still outside. The membrane rushes at you, FOV punches wide, the
//            frame washes toward the throat's own colour. Short and violent.
//   Tunnel : INSIDE. Walls of twisting light streaming past; the tunnel banks
//            and rolls around the ship. This is the bulk of the ride.
//   Exit   : the convergence blooms out and hands you back to real space.
// ---------------------------------------------------------------------------
enum class TransitStage : uint8_t { Idle = 0, Entry = 1, Tunnel = 2, Exit = 3 };

const char* transitStageName(TransitStage s);

// ---------------------------------------------------------------------------
// WHERE THIS CORRIDOR GOES. Strings are COPIED into fixed buffers on begin() —
// a caller may hand us a pointer into a Wormhole that is about to be re-seeded.
// ---------------------------------------------------------------------------
struct TransitPlan {
    const char* corridorName = "UNCHARTED CORRIDOR"; // the wormhole's own name
    const char* fromSystem   = "Kethzar Prime";      // display name of departure
    const char* toSystem     = "Tau Ceti";           // display name of arrival
    const char* toSystemId   = "tau_ceti";           // x3::starsys stable id
    float       distanceLy   = 11.9f;                // corridor length, light-years
    bool        stable       = true;                 // Wormhole::stable()
};

// One queued AEGIS line. `alarm` marks the lines that should read as trouble
// (the host can colour/prioritise them); the text is already written for the
// stability of the corridor in flight.
struct TransitCommsLine {
    char text[192] = {0};
    bool alarm     = false;
};

class WormholeTransit {
public:
    // ---- Legacy S0 wiring (signature FROZEN — other lanes call this) -------
    // Wire a wormhole runner into the SpaceLayer: each update(dt) the runner
    // advances an internal timer, progress = clamp(elapsed/durationSec). The
    // runner returns true when progress reaches 1.0 (transit complete). Brings
    // up the owned WormholeVfx (GPU). durationSec must be > 0.
    void init(rhi::IRenderDevice&, SpaceLayer&, float durationSec = 6.0f);

    // ---- The plan -----------------------------------------------------------
    // Set the corridor BEFORE SpaceLayer::requestWormhole(). The plan is latched
    // on the transit's first tick (so a late begin() cannot retarget a jump that
    // is already in flight) and copied, so the caller's strings need not outlive
    // the call. Without a begin(), a transit runs against the default plan.
    void begin(const TransitPlan&);
    const TransitPlan& plan() const { return plan_; }

    // ---- Draw ---------------------------------------------------------------
    // LEGACY showcase draw: one tube at the VFX origin, at the current progress.
    // A no-op while no transit is active and the VFX is down.
    void render(rhi::IRenderDevice&, const rhi::FrameContext&,
                const float* viewProj16, float timeSec);

    // THE RIDE DRAW. Wraps the camera in the crystal throat: several copies of
    // the SAME tube mesh at scrolling origins along +Z so the tunnel is endless,
    // rolled about its axis by rollRad() and swung laterally by bankX/bankY, so
    // the walls twist past and the throat banks around the ship. `camPos` is the
    // world position of the transit camera (the tunnel is built around it, which
    // keeps the coordinates small and the ride independent of where in the world
    // the player happened to enter). No-op unless a transit is active.
    void renderTunnel(rhi::IRenderDevice&, const rhi::FrameContext&,
                      const float camPos[3], float timeSec);

    // ---- Progress + stage ---------------------------------------------------
    float        progress() const;   // 0..1, current transit completion
    bool         active() const;     // true while a transit is running
    float        elapsed() const  { return elapsed_; }
    float        duration() const { return duration_; }
    TransitStage stage() const;
    // 0..1 within the CURRENT stage (0 when Idle).
    float stageT01() const;

    // ---- The ride frame (all dt-integrated or pure functions of elapsed) -----
    // 0..~1.1 "how hard are we moving right now": ramps over Entry, plateaus
    // through Tunnel with a slow surge, decays over Exit. Drives the streaks,
    // the FOV punch and the instrument curve alike, so they cannot disagree.
    float ride01() const;
    // Metres travelled down the tunnel axis, INTEGRATED from dt. Scrolls the
    // tube copies; wraps in the host against the tube length.
    float axialDistance() const { return axial_; }
    // Accumulated roll of the throat about its own axis, radians. Integrated.
    float rollRad() const { return roll_; }
    // Lateral swing of the tunnel around the ship, metres. A pure function of
    // elapsed time (two incommensurate sines), so it is identical at any
    // framerate with no integration drift at all.
    float bankX() const;
    float bankY() const;
    // Extra FOV, degrees, on top of whatever the host's base FOV is. Peaks on
    // the entry punch and again mid-tunnel; back to 0 by arrival.
    float fovPunchDeg() const;
    // 0..1 wash over the frame at the two membrane crossings (entry punch and
    // exit bloom). The host paints this as a HUD quad in the corridor's colour.
    float membraneWash01() const;
    // 0..~1.3 drive for the near-field streak layer. Reuses the host's existing
    // sense-of-speed FX rather than a second streak system.
    float streakDrive() const;

    // ---- THE OFF-SCALE INSTRUMENTATION --------------------------------------
    // Velocity in multiples of c, and the same thing in m/s. Peaks in the
    // thousands of c mid-tunnel: numbers that do not fit their field, which is
    // the point.
    double readSpeedC()  const;
    double readSpeedMs() const;
    // Position coordinates spinning past legibility. INTEGRATED from the
    // velocity above, so they are consistent with it rather than decorative.
    void   readPos(double out3[3]) const;
    // Light-years still to run. Collapses to 0 at arrival.
    float  distanceRemainLy() const;
    // Seconds of transit remaining (real seconds, not the fiction).
    float  etaSec() const;

    // ---- The AEGIS script ---------------------------------------------------
    // Pop the next queued comms line. Returns false when the queue is empty.
    // The host posts these to the comms bus; it does not author them.
    bool popComms(TransitCommsLine& out);
    int  pendingComms() const { return commsCount_ - commsHead_; }

    // ---- Arrival ------------------------------------------------------------
    // True for exactly one arrival, until consumeArrival() clears it. The host
    // uses this to relocate the ship and re-dress the sky for the new system.
    bool arrivalPending() const { return arrivalPending_; }
    void consumeArrival()       { arrivalPending_ = false; }
    // Was the last completed transit ABORTED rather than flown to the far side?
    // An aborted transit must leave the player in a valid world — the host puts
    // them back on the departure side rather than at the destination.
    bool lastArrivalAborted() const { return arrivalAborted_; }

    // Bail out of a transit in flight. The runner completes on its next tick,
    // the spine lands back in DeepSpace (never in limbo), and AEGIS says so.
    // A no-op when nothing is in flight.
    void abort();

    // ---- Teardown -----------------------------------------------------------
    // Release the owned WormholeVfx. Idempotent.
    void  shutdown(rhi::IRenderDevice&);

private:
    // Stage boundaries as fractions of duration_, clamped so a very short
    // transit still has all three beats.
    float entryEndSec() const;
    float exitStartSec() const;

    void  queueComms(const char* text, bool alarm);
    void  resetRide();

    WormholeVfx* vfx_ = nullptr;     // owned (heap; fwd-decl keeps header light)
    float duration_   = 6.0f;
    float elapsed_    = 0.0f;
    float progress_   = 0.0f;
    bool  active_     = false;

    // The plan for the jump in flight, and the one begin() staged for the next.
    TransitPlan plan_{};
    TransitPlan staged_{};
    bool        stagedValid_ = false;
    // Stable storage for the plan's strings (TransitPlan holds const char*).
    char nameBuf_[64]  = "UNCHARTED CORRIDOR";
    char fromBuf_[48]  = "Kethzar Prime";
    char toBuf_[48]    = "Tau Ceti";
    char toIdBuf_[48]  = "tau_ceti";

    // Integrated ride state.
    float  axial_   = 0.0f;   // metres down the tunnel axis
    float  roll_    = 0.0f;   // radians of throat roll
    double posAcc_  = 0.0;    // metres of FICTIONAL interstellar travel

    // Beat latches so each AEGIS line fires exactly once per transit.
    bool beatDepart_ = false, beatMid_ = false, beatWarn_ = false;

    bool abortReq_       = false;
    bool arrivalPending_ = false;
    bool arrivalAborted_ = false;

    static constexpr int kMaxComms = 8;
    TransitCommsLine comms_[kMaxComms]{};
    int commsCount_ = 0;   // written
    int commsHead_  = 0;   // popped
};

// --test-wormhole-transit: headless self-test of the S3 transit driving the S0
// SpaceLayer spine (integration-feast fold; body lives in wormhole_transit.cpp).
bool runWormholeTransitSelfTest();

} // namespace x3::space
