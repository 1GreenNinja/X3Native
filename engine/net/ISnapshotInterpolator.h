#pragma once
// ISnapshotInterpolator — client-side snapshot jitter buffer + interpolation.
// Spec: specs/NETCODE-architecture.spec.md §6.2 (remote-entity interpolation) /
// §3.1 (render rate != snapshot rate) / §4.4 (client apply -> interp buffer).
//
// Clean-room: built from X3Native's OWN net types (NetTypes.h Rep* / NetEntityId /
// NetTick) + PUBLIC references (Fiedler "Snapshot Interpolation", Valve "Source
// Multiplayer Networking" entity interpolation, Overwatch GDC 2017). NO third-party
// types appear here (same discipline as IPhysicsWorld hiding JPH::).
//
// What this is (Phase 0c): remote net entities arrive as discrete per-tick
// snapshots that the client mirrors into its IReplication store (Phase 0b). Those
// snapshots are spaced one server tick apart but arrive jittery (reorder, variable
// delay, occasional drops). Rendering them raw looks stuttery and snaps. This
// component is the smoothing layer §6.2 calls for:
//   1. a small JITTER BUFFER of the last few snapshots, keyed by server tick;
//   2. an INTERPOLATION DELAY — render remote entities renderTick = latest - delay
//      ticks in the PAST, so two snapshots normally bracket the render time;
//   3. INTERPOLATE — lerp position + slerp rotation between the bracketing pair by
//      the fractional time, exposing smoothed transforms for the renderer/gameplay.
//
// It is server-authoritative-friendly: it never invents authority, it only smooths
// what the server already sent. Remote entities are interpolated, NEVER predicted
// (§6.2). The LOCAL player is predicted elsewhere (§6.1) and is not fed here.
//
// Pure net/game layer — NO renderer dependency. The renderer reads the exposed
// interpolated RepTransform each render frame and writes it to its mirror Scene.

#include "engine/net/NetTypes.h"

namespace x3::net {

// Tuning for the jitter buffer + interpolation clock. Defaults match §6.2's
// "~100 ms / a few snapshots" guidance at kSimHz=60 (interpDelayTicks=6 ≈ 100 ms).
struct InterpConfig {
    // How far behind the latest received tick we render, in server ticks. Larger =
    // smoother / more drop-tolerant but more visual latency. 6 ticks @ 60 Hz ≈ 100 ms.
    uint32_t interpDelayTicks = 6;

    // Jitter-buffer depth (how many recent snapshot ticks to retain). Must comfortably
    // exceed interpDelayTicks so the bracketing pair is still resident under reorder.
    // Snapshots older than (newest - bufferTicks) are evicted.
    uint32_t bufferTicks = 24;

    // Bounded extrapolation budget when the buffer starves (no future sample past the
    // render time): extrapolate forward from the newest pair's velocity for at most
    // this many ticks, then CLAMP (hold the newest sample). 0 disables extrapolation
    // (pure clamp-to-newest). Kept small so a dropped tail never overshoots wildly.
    uint32_t maxExtrapTicks = 4;
};

// The smoothed output for one remote entity at the current render time.
struct InterpTransform {
    NetEntityId  id;
    RepTransform xf;          // interpolated position + rotation (slerp), ready to render
    bool         extrapolated = false;  // true if produced by bounded extrapolation
};

class ISnapshotInterpolator {
public:
    virtual ~ISnapshotInterpolator() = default;

    // (Re)configure the buffer + clock. Safe to call before any snapshot is ingested.
    virtual void configure(const InterpConfig& cfg) = 0;
    virtual InterpConfig config() const = 0;

    // --- ingest (client side) ------------------------------------------------
    // Begin recording the snapshot for server tick `tick`. Out-of-order arrival is
    // handled (inserted by tick); a stale tick (older than the buffer's tail) or an
    // exact DUPLICATE of a tick already held is ignored — returns false in that case
    // (the caller may skip the addEntity calls, but calling them is harmless: they
    // no-op while no snapshot frame is open). Returns true if a frame was opened.
    virtual bool beginSnapshot(NetTick tick) = 0;

    // Add one remote entity's authoritative transform to the currently-open snapshot
    // frame (between beginSnapshot/endSnapshot). No-op if no frame is open. The
    // caller obtains `xf` by applying the wire snapshot to its client IReplication
    // store and reading NetComp_Transform (§4.4) — this keeps the interpolator off
    // the wire-decode path (no duplicated serialization). `vel` (optional) feeds the
    // bounded extrapolation on starvation; pass the entity's RepVelocity or nullptr.
    virtual void addEntity(NetEntityId id, const RepTransform& xf, const RepVelocity* vel) = 0;

    // Close the open snapshot frame (commits it into the jitter buffer + evicts the
    // tail beyond bufferTicks). No-op if no frame is open.
    virtual void endSnapshot() = 0;

    // --- render-time query (client side) -------------------------------------
    // Advance the interpolation clock by a real (render) frame dt and recompute the
    // interpolated output for every remote entity at renderTick = latest - delay.
    // Render rate is independent of snapshot rate (§3.1) — call once per render frame.
    // Deterministic given the same ingest sequence + the same dt sequence.
    virtual void advance(float renderDtSeconds) = 0;

    // Read back the interpolated transforms computed by the last advance(). Writes up
    // to maxOut entries into `out`; returns the count written (the number of remote
    // entities currently being interpolated). The renderer copies these to its mirror.
    virtual uint32_t sample(InterpTransform* out, uint32_t maxOut) const = 0;

    // Convenience: read one entity's last interpolated transform. Returns false if the
    // entity is not currently in the buffer / not yet bracketed.
    virtual bool sampleOne(NetEntityId id, InterpTransform& out) const = 0;

    // --- diagnostics ---------------------------------------------------------
    virtual NetTick newestTick() const = 0;   // highest server tick in the buffer (0 if empty)
    virtual NetTick oldestTick() const = 0;   // lowest server tick in the buffer (0 if empty)
    virtual uint32_t bufferedCount() const = 0; // number of snapshot frames held
    // The fractional render time (in ticks) the last advance() resolved to, for tests:
    // the integer part is the lower bracketing tick; the fraction is the lerp alpha.
    virtual double renderTimeTicks() const = 0;
};

// Factory (caller owns; delete to destroy). Mirrors createReplication() etc.
ISnapshotInterpolator* createSnapshotInterpolator();

// Headless acceptance test (--test-netinterp): client-side interpolation + jitter
// buffer (Phase 0c). Synthesizes a known ground-truth trajectory, produces per-tick
// snapshots from it, feeds them to the buffer WITH simulated jitter (reorder +
// variable delay + occasional drops), advances render time, and asserts the
// interpolated output is smooth + bounded (no NaNs, no overshoot beyond the
// bracketing samples within tolerance), uses the correct bracketing pair, tolerates
// reorder/drops without breaking, and converges to the authoritative trajectory
// within the interpolation delay. Logs each property as "[netinterp-test] PASS ..."
// / "[netinterp-test] FAIL ..." and prints "netinterp: X/Y passed". Returns true iff
// all pass. Mirrors runNetSyncSelfTest().
bool runNetInterpSelfTest();

} // namespace x3::net
