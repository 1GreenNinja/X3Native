// app/space/wormhole_transit.cpp — S3 wormhole transit: the RIDE.
//
// See wormhole_transit.h for what changed and why. The short version: the S0
// runner contract is untouched, and everything else about this file is new —
// a staged timeline, an integrated ride frame, off-scale instrumentation, an
// AEGIS script and a real destination.
//
// DT LAW (165 Hz). Every quantity here is either
//   (a) a pure function of ACCUMULATED TIME (`elapsed_`), which is trivially
//       framerate-independent, or
//   (b) integrated as `x += rate * dt`, which is framerate-independent to the
//       accuracy of the integrator.
// Nothing is a function of a frame count. --test-wormhole-transit drives the
// same transit at 60 Hz and at 165 Hz and asserts the same duration, the same
// arrival and matching telemetry.
#include "wormhole_transit.h"

#include "space_layer.h"
#include "wormhole_vfx.h"
#include "../headless_device.h"     // HeadlessRenderDevice for the self-test
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace x3::space {

namespace {

constexpr double kC = 299792458.0;          // m/s
constexpr float  kTau = 6.28318530717959f;

// Visual metres-per-second the tunnel walls stream past at full ride. This is a
// LOOK number, not the fiction on the HUD — the walls have to move at a rate the
// eye can track or the tunnel strobes.
constexpr float kTunnelVisualSpeed = 168.0f;

// Radians/sec the throat rolls about its axis at full ride.
constexpr float kRollRate = 0.62f;

static inline float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

// Smoothstep between two edges.
static float smooth01(float e0, float e1, float x) {
    if (e1 <= e0) return x >= e1 ? 1.0f : 0.0f;
    const float t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}

// Smootherstep — C2 continuous. Used wherever something ACCELERATES rather than
// translating linearly (the movie-grade motion note: nothing moves at a constant
// rate, and nothing pops on or off).
static float smoother01(float e0, float e1, float x) {
    if (e1 <= e0) return x >= e1 ? 1.0f : 0.0f;
    const float t = clamp01((x - e0) / (e1 - e0));
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static void copyStr(char* dst, size_t n, const char* src, const char* fallback) {
    const char* s = (src && src[0]) ? src : fallback;
    std::snprintf(dst, n, "%s", s);
}

} // namespace

const char* transitStageName(TransitStage s) {
    switch (s) {
        case TransitStage::Entry:  return "ENTRY";
        case TransitStage::Tunnel: return "TUNNEL";
        case TransitStage::Exit:   return "EXIT";
        case TransitStage::Idle:   break;
    }
    return "IDLE";
}

// ---------------------------------------------------------------------------
// Stage boundaries. Entry is a hard, short punch; Exit is a slightly longer
// bloom out. Both are capped at a fraction of the duration so a very short
// transit (the tests use 2 s) still plays all three beats in order.
// ---------------------------------------------------------------------------
float WormholeTransit::entryEndSec() const {
    return std::min(1.15f, duration_ * 0.18f);
}
float WormholeTransit::exitStartSec() const {
    const float exitLen = std::min(1.45f, duration_ * 0.22f);
    return std::max(entryEndSec() + 0.01f, duration_ - exitLen);
}

TransitStage WormholeTransit::stage() const {
    if (!active_) return TransitStage::Idle;
    if (elapsed_ < entryEndSec())  return TransitStage::Entry;
    if (elapsed_ < exitStartSec()) return TransitStage::Tunnel;
    return TransitStage::Exit;
}

float WormholeTransit::stageT01() const {
    if (!active_) return 0.0f;
    const float e0 = entryEndSec(), e1 = exitStartSec();
    if (elapsed_ < e0) return clamp01(elapsed_ / std::max(1e-4f, e0));
    if (elapsed_ < e1) return clamp01((elapsed_ - e0) / std::max(1e-4f, e1 - e0));
    return clamp01((elapsed_ - e1) / std::max(1e-4f, duration_ - e1));
}

// ---------------------------------------------------------------------------
// THE RIDE CURVE. Accelerate into the throat, hold with a slow surge, decelerate
// out. Deliberately NOT linear: linear translation is the game-grade tell.
// ---------------------------------------------------------------------------
float WormholeTransit::ride01() const {
    // Nothing in flight => nothing moving. This is also what makes every derived
    // readout (speed, streaks, bank, FOV) RECOVER the moment the jump lands,
    // rather than leaving the instruments stuck off-scale in normal flight.
    if (!active_) return 0.0f;
    const float e0 = entryEndSec(), e1 = exitStartSec();
    // Acceleration in: smootherstep so there is no velocity discontinuity at the
    // membrane. Deceleration out: ease down to a slow drift, never to zero — the
    // walls are still moving when the tunnel opens, which is what stops the exit
    // feeling like a freeze-frame.
    const float up   = smoother01(0.0f, e0, elapsed_);
    const float down = 1.0f - 0.80f * smoother01(e1, duration_, elapsed_);
    // A slow surge through the tunnel body so the ride BREATHES rather than
    // holding a flat plateau. Two incommensurate periods so it never repeats
    // within a transit.
    const float surge = 1.0f + 0.11f * std::sin(elapsed_ * 1.9f)
                             + 0.06f * std::sin(elapsed_ * 4.3f + 1.1f);
    return std::max(0.0f, up * down * surge);
}

float WormholeTransit::bankX() const {
    // Lateral swing of the tunnel around the ship. PURE function of elapsed time
    // (no integration), so 60 Hz and 165 Hz agree exactly rather than to within
    // integrator error. Two incommensurate periods => a drifting figure that
    // never repeats, which is what secondary motion is supposed to look like.
    const float amp = 3.4f * ride01();
    return amp * (std::sin(elapsed_ * 0.63f) * 0.72f + std::sin(elapsed_ * 1.41f + 2.1f) * 0.28f);
}

float WormholeTransit::bankY() const {
    const float amp = 2.6f * ride01();
    return amp * (std::sin(elapsed_ * 0.51f + 1.7f) * 0.70f + std::sin(elapsed_ * 1.13f) * 0.30f);
}

float WormholeTransit::fovPunchDeg() const {
    if (!active_) return 0.0f;
    const float e0 = entryEndSec(), e1 = exitStartSec();
    // The ENTRY PUNCH: a sharp widening as the ship crosses the membrane, which
    // decays back once you are inside (the eye adapts). Plus a shallow standing
    // widening through the tunnel body that tracks the ride curve, and a second
    // small punch on the exit crossing.
    const float punchIn  = 26.0f * std::pow(smooth01(0.0f, e0 * 0.72f, elapsed_), 1.4f)
                                 * (1.0f - 0.62f * smooth01(e0 * 0.72f, e0 + 0.85f, elapsed_));
    const float body     = 9.0f * ride01();
    const float punchOut = 11.0f * smooth01(e1, e1 + 0.45f, elapsed_)
                                 * (1.0f - smooth01(e1 + 0.45f, duration_, elapsed_));
    return punchIn + body + punchOut;
}

float WormholeTransit::membraneWash01() const {
    if (!active_) return 0.0f;
    const float e0 = entryEndSec(), e1 = exitStartSec();
    // Two crossings, each a rise-and-fall. Nothing pops: both edges are
    // smoothstepped, and the wash never fully saturates the frame (0.86 cap) so
    // the tunnel behind it is always still readable through it.
    const float in  = smooth01(0.0f, e0 * 0.80f, elapsed_)
                    * (1.0f - smooth01(e0 * 0.80f, e0 + 0.30f, elapsed_));
    const float out = smooth01(e1 + 0.25f, duration_ - 0.08f, elapsed_);
    // Capped well below opaque. A wash that saturates is a cut, not a crossing —
    // the tunnel has to stay visible THROUGH the flash or the transition reads as
    // a loading screen. (First pass ran to 0.86 and erased the exit entirely.)
    return std::min(0.52f, in * 0.42f + out * 0.52f);
}

float WormholeTransit::streakDrive() const {
    return std::min(1.30f, ride01() * 1.18f);
}

// ---------------------------------------------------------------------------
// THE OFF-SCALE INSTRUMENTATION. This is the detail that tells the player the
// game knows this is not normal flight: velocity in thousands of c, position
// coordinates spinning past legibility, distance-to-destination collapsing.
// ---------------------------------------------------------------------------
double WormholeTransit::readSpeedC() const {
    // Cubed ride curve: the number climbs far harder than the visuals do, so it
    // reads as "the instruments have given up" rather than as a speedometer.
    const double r = (double)ride01();
    return 12.0 + 5240.0 * (r * r * r);
}

double WormholeTransit::readSpeedMs() const { return readSpeedC() * kC; }

void WormholeTransit::readPos(double out3[3]) const {
    if (!out3) return;
    // INTEGRATED from the velocity above (posAcc_), not decorated noise — the
    // coordinates and the speed readout agree with each other. Seeds are large
    // and irrational-looking so the digits are already unreadable at t=0. Out of
    // transit the readout RETURNS TO REST: an instrument that never comes back is
    // a bug, not an effect.
    const double acc = active_ ? posAcc_ : 0.0;
    out3[0] =  4.417298e7 + acc *  0.77371;
    out3[1] = -1.902884e7 - acc *  0.44127;
    out3[2] =  8.336151e7 + acc *  1.22317;
}

float WormholeTransit::distanceRemainLy() const {
    if (!active_ && elapsed_ <= 0.0f) return plan_.distanceLy;
    // Collapses on a smootherstep of progress, so the last light-years fall off a
    // cliff rather than ticking down linearly. Exactly 0 once the jump completes.
    const float p = clamp01(elapsed_ / std::max(1e-4f, duration_));
    return plan_.distanceLy * (1.0f - smoother01(0.0f, 1.0f, p));
}

float WormholeTransit::etaSec() const {
    return std::max(0.0f, duration_ - elapsed_);
}

// ---------------------------------------------------------------------------
// The AEGIS script.
// ---------------------------------------------------------------------------
void WormholeTransit::queueComms(const char* text, bool alarm) {
    if (commsCount_ >= kMaxComms) return;
    std::snprintf(comms_[commsCount_].text, sizeof(comms_[commsCount_].text), "%s", text);
    comms_[commsCount_].alarm = alarm;
    ++commsCount_;
}

bool WormholeTransit::popComms(TransitCommsLine& out) {
    if (commsHead_ >= commsCount_) return false;
    out = comms_[commsHead_++];
    return true;
}

// ---------------------------------------------------------------------------
void WormholeTransit::resetRide() {
    elapsed_  = 0.0f;
    progress_ = 0.0f;
    axial_    = 0.0f;
    roll_     = 0.0f;
    posAcc_   = 0.0;
    beatDepart_ = beatMid_ = beatWarn_ = false;
    abortReq_       = false;
    arrivalAborted_ = false;
    commsCount_ = 0;
    commsHead_  = 0;
}

void WormholeTransit::begin(const TransitPlan& p) {
    staged_      = p;
    stagedValid_ = true;
}

void WormholeTransit::abort() {
    if (active_) abortReq_ = true;
}

// ---------------------------------------------------------------------------
void WormholeTransit::init(rhi::IRenderDevice& dev, SpaceLayer& layer,
                           float durationSec) {
    duration_ = (durationSec > 0.0f) ? durationSec : 6.0f;
    active_   = false;
    arrivalPending_ = false;
    resetRide();
    // Default plan so a transit armed without begin() is still a journey.
    copyStr(nameBuf_, sizeof(nameBuf_), plan_.corridorName, "UNCHARTED CORRIDOR");
    copyStr(fromBuf_, sizeof(fromBuf_), plan_.fromSystem,   "Kethzar Prime");
    copyStr(toBuf_,   sizeof(toBuf_),   plan_.toSystem,     "Tau Ceti");
    copyStr(toIdBuf_, sizeof(toIdBuf_), plan_.toSystemId,   "tau_ceti");
    plan_.corridorName = nameBuf_;
    plan_.fromSystem   = fromBuf_;
    plan_.toSystem     = toBuf_;
    plan_.toSystemId   = toIdBuf_;

    // Bring up the crystal-matrix tunnel VFX (GPU). Owned for our lifetime.
    if (!vfx_) vfx_ = new WormholeVfx();
    vfx_->init(dev);

    // Register the runner with the S0 spine. requestWormhole() arms the pending
    // transition; each SpaceLayer.update(dt) ticks THIS lambda until it returns
    // true. The lambda owns the transit timer + the whole ride model; SpaceLayer
    // owns the Context state machine (WormholeTransit -> DeepSpace on completion).
    layer.registerWormholeRunner([this](float dt) -> bool {
        // First tick of a fresh transit: the runner was just (re)armed, so reset
        // the ride. `active_` flips false on the completing tick below, so a
        // false->arm here marks the start of a new jump.
        if (!active_) {
            resetRide();
            active_ = true;
            // LATCH THE PLAN. A begin() that arrives after the jump is under way
            // cannot retarget it — you do not get to change destination halfway
            // down the corridor.
            if (stagedValid_) {
                copyStr(nameBuf_, sizeof(nameBuf_), staged_.corridorName, "UNCHARTED CORRIDOR");
                copyStr(fromBuf_, sizeof(fromBuf_), staged_.fromSystem,   "Kethzar Prime");
                copyStr(toBuf_,   sizeof(toBuf_),   staged_.toSystem,     "Tau Ceti");
                copyStr(toIdBuf_, sizeof(toIdBuf_), staged_.toSystemId,   "tau_ceti");
                plan_.distanceLy   = (staged_.distanceLy > 0.0f) ? staged_.distanceLy : 1.0f;
                plan_.stable       = staged_.stable;
                plan_.corridorName = nameBuf_;
                plan_.fromSystem   = fromBuf_;
                plan_.toSystem     = toBuf_;
                plan_.toSystemId   = toIdBuf_;
                stagedValid_ = false;
            }
        }

        // ---- ABORT. Complete on this tick and land in a VALID world. --------
        if (abortReq_) {
            queueComms("Corridor rejected the hull. Aborting translation - "
                       "dumping us back the way we came.", /*alarm=*/true);
            elapsed_        = duration_;
            progress_       = 1.0f;
            active_         = false;
            abortReq_       = false;
            arrivalAborted_ = true;
            arrivalPending_ = true;
            return true;
        }

        // ---- DEPARTURE BEAT (fires on the first tick) -----------------------
        if (!beatDepart_) {
            beatDepart_ = true;
            char line[192];
            if (plan_.stable) {
                std::snprintf(line, sizeof(line),
                              "Threshold crossed - %s. Autopilot has the helm. "
                              "Plotting %s, %.1f light-years. Hold for translation.",
                              plan_.corridorName, plan_.toSystem, (double)plan_.distanceLy);
                queueComms(line, false);
            } else {
                std::snprintf(line, sizeof(line),
                              "Threshold crossed - %s. Commander, this aperture is NOT "
                              "stable. I cannot guarantee where %s puts us down.",
                              plan_.corridorName, plan_.corridorName);
                queueComms(line, true);
            }
        }

        // ---- INTEGRATE the ride. dt-correct, never per-frame. ---------------
        elapsed_ += dt;
        progress_ = clamp01(elapsed_ / duration_);
        const float r = ride01();
        axial_ += kTunnelVisualSpeed * r * dt;
        // The throat's roll ACCELERATES with the ride and drifts with a slow
        // secondary term, so the spin is never a constant rate.
        roll_  += (kRollRate * r + 0.14f * std::sin(elapsed_ * 0.77f)) * dt;
        posAcc_ += readSpeedMs() * (double)dt;

        // ---- MID-TRANSIT BEAT ----------------------------------------------
        if (!beatMid_ && progress_ >= 0.42f) {
            beatMid_ = true;
            char line[192];
            if (plan_.stable) {
                std::snprintf(line, sizeof(line),
                              "Mid-corridor. Velocity reads %.0f c - the instruments have "
                              "no scale for this. %.1f light-years to %s.",
                              readSpeedC(), (double)distanceRemainLy(), plan_.toSystem);
                queueComms(line, false);
            } else {
                std::snprintf(line, sizeof(line),
                              "Mid-corridor and the walls are MOVING, Commander. Aperture "
                              "geometry is degrading. Nav solution for %s is gone.",
                              plan_.toSystem);
                queueComms(line, true);
            }
        }

        // ---- UNSTABLE-ONLY WARNING BEAT ------------------------------------
        if (!beatWarn_ && !plan_.stable && progress_ >= 0.72f) {
            beatWarn_ = true;
            queueComms("Corridor is collapsing behind us. Whatever is on the far side, "
                       "we are committed to it.", true);
        }

        // ---- COMPLETION -----------------------------------------------------
        if (progress_ >= 1.0f) {
            active_         = false;
            arrivalAborted_ = false;
            arrivalPending_ = true;
            char line[192];
            if (plan_.stable) {
                std::snprintf(line, sizeof(line),
                              "Translation complete. We are through, Commander - %s. "
                              "Look at that star: wrong colour for home. %.1f light-years "
                              "from %s.",
                              plan_.toSystem, (double)plan_.distanceLy, plan_.fromSystem);
            } else {
                std::snprintf(line, sizeof(line),
                              "Translation complete - the corridor collapsed mid-transit. "
                              "That is not %s out there. I do not know what star we are under.",
                              plan_.toSystem);
            }
            queueComms(line, !plan_.stable);
            return true;
        }
        return false;
    });
}

void WormholeTransit::render(rhi::IRenderDevice& dev, const rhi::FrameContext& fr,
                             const float* viewProj16, float timeSec) {
    if (!vfx_ || !vfx_->initialized()) return;
    // Draw the tunnel at the current progress so the core blooms to white-hot
    // convergence as the jump finishes. The host owns the camera transform.
    vfx_->render(dev, fr, viewProj16, timeSec, progress_);
}

// ---------------------------------------------------------------------------
// THE RIDE DRAW. The camera sits ON the tunnel axis; the tunnel is assembled
// AROUND it out of scrolling copies of each shell, so the walls are endless and
// the world coordinates stay small no matter where the player entered.
//
// Each shell scrolls at its OWN rate (the outer wall slowest, the inner grain
// fastest) and rolls in its OWN direction — the multi-frequency motion that
// separates this from one sheet sliding past. The far-end copy FADES IN over
// its last stretch, so no ring of geometry ever pops into existence.
// ---------------------------------------------------------------------------
void WormholeTransit::renderTunnel(rhi::IRenderDevice& dev, const rhi::FrameContext& fr,
                                   const float camPos[3], float timeSec) {
    if (!vfx_ || !vfx_->initialized() || !camPos) return;
    if (!active_) return;

    const float len = std::max(1.0f, vfx_->shellLength());
    const float bx  = bankX();
    const float by  = bankY();

    // Per-shell scroll multipliers and roll directions. The wall is the slow,
    // broad backdrop; the grain shell runs 2.3x faster and counter-rolls, which
    // is what produces the twisting read at the centre of frame.
    const float scrollMul[3] = { 1.00f, 1.45f, 2.30f };
    const float rollMul[3]   = { 1.00f, -1.70f, 2.85f };
    // Inner shells bank HARDER than the wall: secondary motion lags and
    // overshoots primary rather than moving in lockstep with it.
    const float bankMul[3]   = { 1.00f, 1.45f, 2.10f };

    for (int s = 0; s < WormholeVfx::shellCount(); ++s) {
        const float dist = axial_ * scrollMul[s];
        // Where the tiling starts: pull the pattern back by the fractional part
        // of the travelled distance so copies stream toward and past the camera.
        const float frac = dist - std::floor(dist / len) * len;
        const float z0   = -frac;
        const float roll = roll_ * rollMul[s] + timeSec * 0.04f * (float)(s + 1);
        // HOW FAR THE CORRIDOR REACHES. The first pass tiled two tube lengths
        // ahead and left a HOLE at the centre of frame: looking down the axis, the
        // rays nearest the centre travel furthest before they hit a wall, so a
        // tunnel that stops at 400 m has no geometry where the vanishing point is
        // supposed to be — and a hole is the exact opposite of a readable centre.
        // kAhead tube lengths is what actually closes the corridor to a point.
        const int kAhead = 9;
        for (int k = -1; k < kAhead; ++k) {
            const float oz = z0 + (float)k * len;
            // THE DEPTH RAMP is the composition. The near walls fill most of the
            // frame at grazing angles; drawn at full gain they bloom over
            // everything and the eye has nothing to hold (measured: mean frame
            // luminance 0.85, standard deviation 0.08 — a white sheet with a ship
            // on it). So gain RISES with distance: dark structured lanes close to
            // the hull, the hot convergence far down the axis. The ramp is a power
            // curve rather than linear, so the brightness builds the way depth in a
            // real tunnel does instead of stepping ring by ring.
            float gain;
            if (k < 0) {
                gain = 0.045f;                       // behind the eye: peripheral only
            } else {
                const float u = (float)k / (float)(kAhead - 1);
                gain = 0.085f + 1.05f * std::pow(u, 0.85f);
            }
            // FADE the farthest copy in over its own length so no ring of geometry
            // ever pops into existence at the end of the corridor.
            if (k == kAhead - 1) gain *= 1.0f - smooth01(0.0f, len, frac) * 0.5f;
            const float origin[3] = {
                camPos[0] + bx * bankMul[s],
                camPos[1] + by * bankMul[s],
                camPos[2] + oz
            };
            vfx_->renderShell(dev, fr, s, origin, roll, gain, timeSec, progress_);
        }
    }
}

float WormholeTransit::progress() const { return progress_; }
bool  WormholeTransit::active() const   { return active_; }

void WormholeTransit::shutdown(rhi::IRenderDevice& dev) {
    if (vfx_) {
        vfx_->shutdown(dev);
        delete vfx_;
        vfx_ = nullptr;
    }
    active_ = false;
    arrivalPending_ = false;
    resetRide();
}

// ---------------------------------------------------------------------------
// --test-wormhole-transit: S3 wormhole transit self-test. Headless -- wires a
// WormholeTransit runner into a SpaceLayer and drives the S0 spine:
// requestWormhole(dest) -> Context::WormholeTransit + active(); stepping ramps
// progress 0->1 monotonically; at 1.0 the layer lands back in DeepSpace and the
// transit completes; a second jump re-arms cleanly.
//
// T6..T12 are the RIDE gates added by feat/wormhole-transit-ride: the staged
// timeline, the 60 Hz vs 165 Hz equivalence proof, the off-scale telemetry and
// its recovery, the AEGIS script (including the distinct unstable lines), the
// destination plan, and that an ABORTED transit lands in a valid world.
// ---------------------------------------------------------------------------
bool runWormholeTransitSelfTest() {
    int pass = 0, total = 0;
    auto check = [&](bool c, const char* name) {
        ++total;
        if (c) { ++pass; x3::logInfo(std::string("  [ok] ") + name); }
        else   {          x3::logError(std::string("  [FAIL] ") + name); }
    };

    x3::game::HeadlessRenderDevice hdev;

    // T1: init brings up the owned VFX and the transit starts idle.
    {
        SpaceLayer L; L.init();
        WormholeTransit wt;
        wt.init(hdev, L, /*durationSec=*/6.0f);
        check(!wt.active(), "T1 init() -> not active (no transit armed yet)");
        check(wt.progress() == 0.0f, "T1b init() -> progress 0");
        wt.shutdown(hdev);
    }

    // T2: requestWormhole -> WormholeTransit + active(); stepping ramps
    // progress 0->1; at 1.0 the context lands in DeepSpace and active==false.
    {
        SpaceLayer L; L.init();
        WormholeTransit wt;
        const float dur = 6.0f;
        wt.init(hdev, L, dur);

        L.requestWormhole(/*destSystemId=*/42u);
        check(L.context() == Context::WormholeTransit,
              "T2 requestWormhole -> Context::WormholeTransit");

        L.update(1.0f);
        check(wt.active(), "T2b after first update -> active()");
        check(wt.progress() > 0.0f && wt.progress() < 1.0f,
              "T2c progress ramps into (0,1)");
        check(L.context() == Context::WormholeTransit,
              "T2d still in WormholeTransit mid-jump");

        float prev = wt.progress();
        bool monotonic = true;
        for (int i = 0; i < 5; ++i) {
            L.update(1.0f);
            if (wt.progress() < prev) monotonic = false;
            prev = wt.progress();
        }
        check(monotonic, "T2e progress is monotonic non-decreasing");
        check(wt.progress() >= 1.0f, "T2f progress reaches 1.0 at duration");
        check(L.context() == Context::DeepSpace,
              "T2g transit complete -> back in DeepSpace (arrived at dest)");
        check(!wt.active(), "T2h transit complete -> active()==false");

        wt.shutdown(hdev);
    }

    // T3: progress() never exceeds 1.0 even if over-stepped past duration.
    {
        SpaceLayer L; L.init();
        WormholeTransit wt;
        wt.init(hdev, L, /*durationSec=*/2.0f);
        L.requestWormhole(7u);
        L.update(100.0f); // wildly over-step
        check(wt.progress() == 1.0f, "T3 progress clamps to 1.0 on over-step");
        check(L.context() == Context::DeepSpace, "T3b over-step still lands DeepSpace");
        check(!wt.active(), "T3c over-step completes the transit");
        wt.shutdown(hdev);
    }

    // T4: a second jump after the first re-arms cleanly (timer resets).
    {
        SpaceLayer L; L.init();
        WormholeTransit wt;
        wt.init(hdev, L, /*durationSec=*/4.0f);
        L.requestWormhole(1u);
        for (int i = 0; i < 4; ++i) L.update(1.0f);
        check(L.context() == Context::DeepSpace && !wt.active(),
              "T4 first jump completes");
        L.requestWormhole(2u);
        L.update(1.0f);
        check(wt.active() && wt.progress() > 0.0f && wt.progress() < 1.0f,
              "T4b second jump re-arms with a fresh progress ramp");
        wt.shutdown(hdev);
    }

    // T5: render() before/after init is VUID-safe (no crash; no-op pre-init).
    {
        SpaceLayer L; L.init();
        WormholeTransit wt;
        wt.init(hdev, L, 6.0f);
        rhi::FrameContext fr = hdev.beginFrame();
        const float idM[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        wt.render(hdev, fr, idM, /*timeSec=*/0.5f); // active()==false here -> still safe
        L.requestWormhole(3u);
        L.update(1.0f);
        wt.render(hdev, fr, idM, 1.0f);             // mid-transit draw
        const float cam[3] = { 0.0f, 0.0f, 0.0f };
        wt.renderTunnel(hdev, fr, cam, 1.0f);       // the RIDE draw
        wt.renderTunnel(hdev, fr, nullptr, 1.0f);   // null camera must be a no-op
        hdev.endFrame(fr);
        check(true, "T5 render()/renderTunnel() are crash-free pre- and mid-transit");
        wt.shutdown(hdev);
        rhi::FrameContext fr2 = hdev.beginFrame();
        wt.render(hdev, fr2, idM, 2.0f);
        wt.renderTunnel(hdev, fr2, cam, 2.0f);
        hdev.endFrame(fr2);
        check(!wt.active() && wt.progress() == 0.0f,
              "T5b shutdown() resets active()/progress() and render() stays safe");
    }

    // -----------------------------------------------------------------------
    // T6: THE STAGED TIMELINE. Entry -> Tunnel -> Exit, in order, all three
    // reached, and Idle before and after.
    // -----------------------------------------------------------------------
    {
        SpaceLayer L; L.init();
        WormholeTransit wt;
        const float dur = 8.0f;
        wt.init(hdev, L, dur);
        check(wt.stage() == TransitStage::Idle, "T6 idle before a transit is armed");
        L.requestWormhole(900u);
        const float h = 1.0f / 165.0f;
        bool sawEntry = false, sawTunnel = false, sawExit = false;
        bool orderOk = true;
        int lastRank = 0;
        for (int i = 0; i < (int)(dur / h) + 4; ++i) {
            L.update(h);
            const TransitStage st = wt.stage();
            const int rank = (st == TransitStage::Entry)  ? 1 :
                             (st == TransitStage::Tunnel) ? 2 :
                             (st == TransitStage::Exit)   ? 3 : 0;
            if (rank > 0) {
                if (rank < lastRank) orderOk = false;
                lastRank = rank;
            }
            if (st == TransitStage::Entry)  sawEntry  = true;
            if (st == TransitStage::Tunnel) sawTunnel = true;
            if (st == TransitStage::Exit)   sawExit   = true;
        }
        check(sawEntry && sawTunnel && sawExit,
              "T6b all three stages (Entry/Tunnel/Exit) are reached");
        check(orderOk, "T6c stages advance in order and never go backwards");
        check(wt.stage() == TransitStage::Idle, "T6d idle again after arrival");
        wt.shutdown(hdev);
    }

    // -----------------------------------------------------------------------
    // T7: DT STABILITY. The SAME transit at 60 Hz and at 165 Hz must take the
    // same wall-clock duration and arrive at the same place. This is the 165 Hz
    // rule stated as an assertion.
    // -----------------------------------------------------------------------
    {
        auto run = [&](float hz, float& outWall, float& outAxial, double& outPosX,
                       int& outSteps) {
            SpaceLayer L; L.init();
            WormholeTransit wt;
            const float dur = 8.0f;
            wt.init(hdev, L, dur);
            TransitPlan p; p.corridorName = "THE GAMMA CORRIDOR";
            p.toSystem = "Tau Ceti"; p.toSystemId = "tau_ceti";
            p.distanceLy = 11.9f; p.stable = true;
            wt.begin(p);
            L.requestWormhole(900u);
            const float h = 1.0f / hz;
            float wall = 0.0f; int steps = 0;
            double lastPosX = 0.0;
            while (L.context() == Context::WormholeTransit && steps < (int)(hz * 30.0f)) {
                L.update(h); wall += h; ++steps;
                // Sample only while the transit is IN FLIGHT — the readouts
                // deliberately return to rest the instant it lands.
                if (wt.active()) { double pos[3]; wt.readPos(pos); lastPosX = pos[0]; }
            }
            outWall = wall; outAxial = wt.axialDistance(); outPosX = lastPosX;
            outSteps = steps;
            wt.shutdown(hdev);
        };
        float w60 = 0, w165 = 0, a60 = 0, a165 = 0;
        double p60 = 0, p165 = 0;
        int s60 = 0, s165 = 0;
        run(60.0f,  w60,  a60,  p60,  s60);
        run(165.0f, w165, a165, p165, s165);
        check(std::fabs(w60 - w165) < 0.05f,
              "T7 60 Hz and 165 Hz transits take the same wall-clock duration");
        check(s165 > s60 * 2,
              "T7b the 165 Hz run really did take ~2.75x as many steps");
        // The integrated ride distance is an integrator, so agreement is to
        // within integrator error, not exact. 1% is a tight bar for a 8 s ride.
        const float axRel = std::fabs(a60 - a165) / std::max(1.0f, a165);
        check(axRel < 0.01f,
              "T7c integrated tunnel distance agrees to <1% across framerates");
        const double posRel = std::fabs(p60 - p165) / std::max(1.0, std::fabs(p165));
        check(posRel < 0.01,
              "T7d the off-scale position readout agrees to <1% across framerates");
    }

    // -----------------------------------------------------------------------
    // T8: THE OFF-SCALE INSTRUMENTATION reaches its off-scale state and
    // RECOVERS afterwards. A readout that never comes back is a bug.
    // -----------------------------------------------------------------------
    {
        SpaceLayer L; L.init();
        WormholeTransit wt;
        const float dur = 8.0f;
        wt.init(hdev, L, dur);
        TransitPlan p; p.distanceLy = 11.9f; p.stable = true;
        wt.begin(p);
        check(wt.readSpeedC() < 100.0, "T8 speed readout is small before a transit");
        check(wt.distanceRemainLy() > 11.0f, "T8b full distance to run before departure");
        L.requestWormhole(900u);
        const float h = 1.0f / 165.0f;
        double peakC = 0.0; double peakPosMag = 0.0; float minDist = 1e9f;
        double prevPos = 0.0; bool posGrows = true; bool first = true;
        while (L.context() == Context::WormholeTransit) {
            L.update(h);
            minDist = std::min(minDist, wt.distanceRemainLy());
            if (!wt.active()) continue;   // landed on this tick; instruments are back at rest
            peakC = std::max(peakC, wt.readSpeedC());
            double pos[3]; wt.readPos(pos);
            peakPosMag = std::max(peakPosMag, std::fabs(pos[2]));
            if (!first && pos[2] < prevPos) posGrows = false;
            prevPos = pos[2]; first = false;
        }
        // "Off the charts": thousands of c, and a Z coordinate wider than any
        // sane HUD field (>1e12 m — 13+ digits).
        check(peakC > 3000.0, "T8c velocity readout goes off-scale (>3000 c)");
        check(peakPosMag > 1.0e12, "T8d position coordinates overflow any HUD field");
        check(posGrows, "T8e position readout advances monotonically down the corridor");
        check(minDist <= 0.001f, "T8f distance-to-destination collapses to zero");
        // RECOVERY: after arrival the instruments must be sane again.
        check(wt.readSpeedC() < 100.0, "T8g speed readout recovers after arrival");
        double after[3]; wt.readPos(after);
        check(std::fabs(after[2] - 8.336151e7) < 1.0,
              "T8h position readout recovers to its rest value after arrival");
        wt.shutdown(hdev);
    }

    // -----------------------------------------------------------------------
    // T9: THE AEGIS SCRIPT — departure, mid-transit and arrival lines, with a
    // DISTINCT set for an unstable corridor.
    // -----------------------------------------------------------------------
    {
        auto runLines = [&](bool stable, std::string out[8], int& n) {
            SpaceLayer L; L.init();
            WormholeTransit wt;
            wt.init(hdev, L, 8.0f);
            TransitPlan p;
            p.corridorName = stable ? "THE GAMMA CORRIDOR" : "THE DERELICT APERTURE";
            p.toSystem = "Tau Ceti"; p.toSystemId = "tau_ceti";
            p.distanceLy = 11.9f; p.stable = stable;
            wt.begin(p);
            L.requestWormhole(900u);
            n = 0;
            const float h = 1.0f / 165.0f;
            TransitCommsLine line;
            while (L.context() == Context::WormholeTransit) {
                L.update(h);
                while (wt.popComms(line) && n < 8) out[n++] = line.text;
            }
            while (wt.popComms(line) && n < 8) out[n++] = line.text;
            wt.shutdown(hdev);
        };
        std::string sLines[8], uLines[8];
        int sn = 0, un = 0;
        runLines(true,  sLines, sn);
        runLines(false, uLines, un);
        check(sn >= 3, "T9 a stable transit posts at least departure/mid/arrival");
        check(sLines[0].find("THE GAMMA CORRIDOR") != std::string::npos,
              "T9b the departure line names the corridor");
        check(sn >= 3 && sLines[sn - 1].find("Translation complete") != std::string::npos,
              "T9c the last line is the arrival");
        check(sn >= 3 && sLines[sn - 1].find("Tau Ceti") != std::string::npos,
              "T9d the arrival line names the destination system");
        check(un > sn, "T9e an unstable corridor gets an EXTRA warning beat");
        check(uLines[0] != sLines[0],
              "T9f the unstable departure line is distinct from the stable one");
        check(un >= 1 && uLines[0].find("NOT") != std::string::npos,
              "T9g the unstable departure line reads as trouble");
        check(un >= 1 && uLines[un - 1] != sLines[sn - 1],
              "T9h the unstable arrival line is distinct from the stable one");
    }

    // -----------------------------------------------------------------------
    // T10: THE DESTINATION. begin() is latched on the first tick; a plan set
    // mid-flight cannot retarget the jump in progress but DOES take for the
    // next one.
    // -----------------------------------------------------------------------
    {
        SpaceLayer L; L.init();
        WormholeTransit wt;
        wt.init(hdev, L, 4.0f);
        TransitPlan a; a.toSystem = "Tau Ceti"; a.toSystemId = "tau_ceti"; a.distanceLy = 11.9f;
        wt.begin(a);
        L.requestWormhole(900u);
        L.update(0.5f);
        check(std::string(wt.plan().toSystem) == "Tau Ceti",
              "T10 the armed transit carries the plan it was begun with");
        TransitPlan b; b.toSystem = "Wolf 359"; b.toSystemId = "wolf_359"; b.distanceLy = 7.86f;
        wt.begin(b);
        L.update(0.5f);
        check(std::string(wt.plan().toSystem) == "Tau Ceti",
              "T10b a mid-flight begin() cannot retarget the jump in progress");
        while (L.context() == Context::WormholeTransit) L.update(0.25f);
        L.requestWormhole(901u);
        L.update(0.25f);
        check(std::string(wt.plan().toSystem) == "Wolf 359",
              "T10c the staged plan takes effect on the NEXT jump");
        check(std::string(wt.plan().toSystemId) == "wolf_359",
              "T10d the star-system id travels with the plan");
        wt.shutdown(hdev);
    }

    // -----------------------------------------------------------------------
    // T11: ARRIVAL SIGNALLING. arrivalPending() latches exactly once per
    // transit and clears on consumeArrival().
    // -----------------------------------------------------------------------
    {
        SpaceLayer L; L.init();
        WormholeTransit wt;
        wt.init(hdev, L, 3.0f);
        L.requestWormhole(900u);
        check(!wt.arrivalPending(), "T11 no arrival pending mid-transit");
        while (L.context() == Context::WormholeTransit) L.update(0.25f);
        check(wt.arrivalPending(), "T11b arrival latches when the transit completes");
        check(!wt.lastArrivalAborted(), "T11c a flown transit is not an abort");
        wt.consumeArrival();
        check(!wt.arrivalPending(), "T11d consumeArrival() clears the latch");
        wt.shutdown(hdev);
    }

    // -----------------------------------------------------------------------
    // T12: AN ABORTED TRANSIT LEAVES THE PLAYER IN A VALID WORLD, never in
    // limbo: the spine lands back in DeepSpace, the transit is not active, and
    // the arrival is flagged as an abort so the host puts the ship back on the
    // departure side rather than at a destination it never reached.
    // -----------------------------------------------------------------------
    {
        SpaceLayer L; L.init();
        WormholeTransit wt;
        wt.init(hdev, L, 8.0f);
        TransitPlan p; p.corridorName = "THE DERELICT APERTURE"; p.stable = false;
        wt.begin(p);
        L.requestWormhole(901u);
        for (int i = 0; i < 60; ++i) L.update(1.0f / 165.0f);
        check(L.context() == Context::WormholeTransit && wt.active(),
              "T12 transit is genuinely in flight before the abort");
        wt.abort();
        L.update(1.0f / 165.0f);
        check(L.context() == Context::DeepSpace,
              "T12b an aborted transit lands the spine back in DeepSpace (no limbo)");
        check(!wt.active(), "T12c an aborted transit is no longer active");
        check(wt.arrivalPending() && wt.lastArrivalAborted(),
              "T12d the arrival is flagged as an ABORT for the host to handle");
        TransitCommsLine line; bool sawAbort = false;
        while (wt.popComms(line))
            if (std::string(line.text).find("Aborting") != std::string::npos) sawAbort = true;
        check(sawAbort, "T12e AEGIS says the transit was aborted");
        // And the world must still be usable: a fresh jump re-arms cleanly.
        wt.consumeArrival();
        L.requestWormhole(900u);
        L.update(1.0f / 165.0f);
        check(wt.active() && wt.progress() > 0.0f,
              "T12f a new transit re-arms cleanly after an abort");
        wt.shutdown(hdev);
    }

    // -----------------------------------------------------------------------
    // T13: THE RIDE FRAME. The tunnel actually moves, banks and rolls, and the
    // motion EASES rather than translating linearly (the movie-grade rule).
    // -----------------------------------------------------------------------
    {
        SpaceLayer L; L.init();
        WormholeTransit wt;
        const float dur = 8.0f;
        wt.init(hdev, L, dur);
        L.requestWormhole(900u);
        const float h = 1.0f / 165.0f;
        float prevAxial = 0.0f, prevRoll = 0.0f;
        bool axialGrows = true, rollGrows = true;
        float maxBank = 0.0f, maxFov = 0.0f, maxRide = 0.0f, maxWash = 0.0f;
        float rideEarly = -1.0f, rideMid = -1.0f;
        while (L.context() == Context::WormholeTransit) {
            L.update(h);
            if (wt.axialDistance() < prevAxial) axialGrows = false;
            if (wt.rollRad() < prevRoll) rollGrows = false;
            prevAxial = wt.axialDistance(); prevRoll = wt.rollRad();
            maxBank = std::max(maxBank, std::fabs(wt.bankX()));
            maxFov  = std::max(maxFov,  wt.fovPunchDeg());
            maxRide = std::max(maxRide, wt.ride01());
            maxWash = std::max(maxWash, wt.membraneWash01());
            if (rideEarly < 0.0f && wt.progress() >= 0.04f) rideEarly = wt.ride01();
            if (rideMid   < 0.0f && wt.progress() >= 0.50f) rideMid   = wt.ride01();
        }
        check(axialGrows && prevAxial > 500.0f,
              "T13 the tunnel streams a real distance past the ship");
        check(rollGrows && prevRoll > 1.0f, "T13b the throat rolls about its own axis");
        check(maxBank > 1.0f, "T13c the tunnel banks laterally around the ship");
        check(maxFov > 20.0f, "T13d FOV punches wide on the membrane crossing");
        check(maxRide > 0.9f, "T13e the ride curve reaches full");
        check(maxWash > 0.5f && maxWash <= 0.86f,
              "T13f the membrane wash peaks but never saturates the frame");
        // EASING: the ride at 4% progress must be far below the ride at 50%. A
        // linear ramp would put it near 8% of the mid value; smootherstep puts it
        // far lower. This is the assertion that the motion accelerates.
        check(rideEarly >= 0.0f && rideMid > 0.0f && rideEarly < rideMid * 0.35f,
              "T13g the ride ACCELERATES in (eased), it does not translate linearly");
        wt.shutdown(hdev);
    }

    x3::logInfo("wormhole-transit: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
    std::printf("wormhole-transit: %d/%d passed\n", pass, total);
    std::fflush(stdout);
    return pass == total;
}

} // namespace x3::space
