#pragma once
// IClientPredictor — client-side LOCAL-player prediction + server reconciliation.
// Spec: specs/NETCODE-architecture.spec.md §6.1 (local-player prediction) /
// §6.3 (server reconciliation + rollback/resim) / §3.2 (single-machine determinism).
//
// Clean-room: built from X3Native's OWN net types (NetTypes.h NetCommand / Rep*,
// PlayerSimStep.h shared integrator) + PUBLIC references (Valve "Source Multiplayer
// Networking" prediction/reconciliation, Fiedler "Networked Physics", Overwatch GDC
// 2017 command-frame rollback). NO third-party types appear here (same discipline as
// IPhysicsWorld hiding JPH:: / ISnapshotInterpolator hiding its buffer).
//
// What this is (Phase 1, LOCAL player only — REMOTE players keep using the Phase 0c
// ISnapshotInterpolator and are NOT fed here, §6.2):
//   1. PREDICT — each client tick, apply the local input IMMEDIATELY via the SHARED
//      deterministic stepPlayer() (PlayerSimStep.h, the SAME step the server runs),
//      so the local player responds with ZERO perceived latency; it does not wait
//      for the server.
//   2. INPUT HISTORY — retain every still-unacknowledged NetCommand, keyed by client
//      tick, so they can be re-applied after a correction.
//   3. RECONCILE (rollback + resim, §6.3) — when an authoritative snapshot arrives
//      carrying the last-processed-input tick (the ACK), SNAP the predicted local
//      player to the server's authoritative state as of that acked tick, DISCARD the
//      acked inputs, then REPLAY all still-unacked inputs through stepPlayer() to
//      re-arrive at the present predicted state. If prediction already matched the
//      server (within epsilon), the replay reproduces the same state — a no-op
//      visually (NO rubber-band). Deterministic by construction (§3.2).
//
// Pure net/game layer — NO renderer dependency. The renderer reads predictedState()
// each render frame and writes it to its mirror Scene for the local player.

#include "engine/net/NetTypes.h"
#include "engine/net/PlayerSimStep.h"   // PlayerSimState + the shared stepPlayer()

namespace x3::net {

// Tuning for the predictor. Defaults are conservative for a 60 Hz sim.
struct PredictConfig {
    // Position error (meters) below which a reconciliation is treated as a perfect
    // match: no correction is reported and (since the replay reproduces the same
    // state to the bit) nothing visibly moves. This is the "no rubber-banding under
    // steady state" threshold (§6.3). Generous vs float round-trip noise, tight vs a
    // real mispredict (a missed collision / server knockback moves you >> this).
    float reconcileEpsilon = 1e-3f;

    // Hard cap on retained unacknowledged commands. Under steady ACKs the history is
    // a handful deep (RTT/kSimDt); this bounds memory if ACKs stall (a stuck client
    // is corrected on the next ack, it never grows unbounded). Oldest are dropped.
    uint32_t maxUnacked = 256;
};

// The outcome of one reconcile() call — lets tests/diagnostics see whether a visible
// correction happened and how big it was (the rubber-band magnitude).
struct ReconcileResult {
    bool     applied        = false;  // an authoritative state was actually consumed
    bool     corrected      = false;  // post-snap pre-replay error exceeded epsilon
    float    positionError  = 0.0f;   // |predicted - authoritative| at the acked tick (m)
    uint32_t replayedInputs = 0;      // # still-unacked commands re-simulated after the snap
    uint32_t droppedInputs  = 0;      // # acked commands discarded from history
};

class IClientPredictor {
public:
    virtual ~IClientPredictor() = default;

    // (Re)configure. Safe to call before any predict()/reconcile().
    virtual void configure(const PredictConfig& cfg) = 0;
    virtual PredictConfig config() const = 0;

    // Seed the predicted state (e.g. the spawn transform) without recording a
    // command. Call once at startup so prediction starts from the right place.
    virtual void reset(const PlayerSimState& initial) = 0;

    // --- predict (once per client tick) --------------------------------------
    // Record `cmd` in the unacknowledged-input history (keyed by cmd.tick) AND step
    // the predicted local-player state immediately via the shared stepPlayer(). The
    // local player thus advances THIS tick without waiting for the server. Returns a
    // const ref to the freshly-advanced predicted state. cmd.tick MUST be monotone
    // increasing (the client's own tick counter); a stale/duplicate tick is ignored
    // (returns the unchanged state) so a double-submit can't double-step.
    virtual const PlayerSimState& predict(const NetCommand& cmd) = 0;

    // --- reconcile (on each authoritative snapshot for the local player) ------
    // Roll back + re-simulate (§6.3): given the server's AUTHORITATIVE state for the
    // local player as of input tick `ackTick` (the last command tick the server
    // processed for this client, carried in the snapshot), this:
    //   1. snaps the predicted state to `authoritative`,
    //   2. discards every history command with tick <= ackTick (acked),
    //   3. replays every remaining (still-unacked) command via stepPlayer(), in tick
    //      order, to re-arrive at the present predicted state.
    // Returns what happened (correction magnitude, counts). A stale/duplicate ack
    // (<= the last one already reconciled) is ignored (result.applied == false).
    virtual ReconcileResult reconcile(const PlayerSimState& authoritative, NetTick ackTick) = 0;

    // --- read-back ------------------------------------------------------------
    virtual const PlayerSimState& predictedState() const = 0;   // current predicted local player
    virtual NetTick  lastPredictedTick() const = 0;             // highest cmd.tick predicted (0 if none)
    virtual NetTick  lastAckedTick() const = 0;                 // highest ackTick reconciled (0 if none)
    virtual uint32_t unackedCount() const = 0;                  // # commands awaiting an ack
};

// Factory (caller owns; delete to destroy). Mirrors createSnapshotInterpolator() etc.
IClientPredictor* createClientPredictor();

// Headless acceptance test (--test-netpredict): client-side prediction +
// reconciliation (Phase 1, LOCAL player). Drives a deterministic input sequence
// through a loopback client+server with SIMULATED SERVER LAG (the server processes
// commands a few ticks behind the client) plus a late/dropped snapshot, and asserts:
//   (a) the predicted local player advances IMMEDIATELY each tick (ahead of acks);
//   (b) after each reconciliation the post-replay predicted state matches the
//       server's authoritative state within epsilon;
//   (c) under steady no-loss conditions reconciliation produces NO correction jump
//       (no rubber-band);
//   (d) after an injected mismatch/correction the replay converges correctly.
// Logs each property as "[netpredict-test] PASS ..." / "[netpredict-test] FAIL ..."
// and prints "netpredict: X/Y passed". Returns true iff all pass. Mirrors
// runNetSyncSelfTest() / runNetInterpSelfTest().
bool runNetPredictSelfTest();

} // namespace x3::net
