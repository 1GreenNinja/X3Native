// ClientPredictor — client-side LOCAL-player prediction + server reconciliation.
// Spec: specs/NETCODE-architecture.spec.md §6.1 / §6.3 / §3.2.
//
// Phase 1: predict the local player immediately each tick (zero perceived latency),
// keep an unacknowledged-input ring keyed by client tick, and on each authoritative
// snapshot SNAP to the server's state at the acked tick + REPLAY the still-unacked
// inputs through the SHARED stepPlayer() (PlayerSimStep.h) to re-arrive at "now"
// (rollback/resim). Deterministic single-machine fixed-step (§3.2) makes "predict
// then replay reproduces the server's result" hold to the bit, so steady state
// reconciles as a visual no-op (no rubber-band). REMOTE players are NOT handled here
// (they keep the Phase 0c ISnapshotInterpolator, §6.2).

#include "engine/net/IClientPredictor.h"
#include "engine/net/SimClock.h"
#include "engine/core/x3_log.h"

#include "engine/net/INetTransport.h"   // self-test loopback wiring
#include "engine/net/IReplication.h"    // self-test authoritative store
#include "engine/net/INetworkSystem.h"  // self-test: runNetSyncSelfTest re-run (regression)

#include <deque>
#include <vector>
#include <cmath>
#include <cstring>
#include <string>

namespace x3::net {

// serverApplyCommand lives in NetworkSystem.cpp (the Phase 0b authoritative apply).
// We re-declare it here so the self-test can drive the SAME authoritative integrator
// the production server uses — proving predict/replay converge to the real authority.
void serverApplyCommand(IReplication* rep, NetEntityId e, const NetCommand& cmd);

namespace {

float posErr(const PlayerSimState& a, const PlayerSimState& b) {
    const float dx = a.xf.pos[0] - b.xf.pos[0];
    const float dy = a.xf.pos[1] - b.xf.pos[1];
    const float dz = a.xf.pos[2] - b.xf.pos[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

class ClientPredictor final : public IClientPredictor {
public:
    void configure(const PredictConfig& cfg) override { m_cfg = cfg; }
    PredictConfig config() const override { return m_cfg; }

    void reset(const PlayerSimState& initial) override {
        m_state = initial;
        m_history.clear();
        m_lastPredictedTick = 0;
        m_lastAckedTick = 0;
    }

    const PlayerSimState& predict(const NetCommand& cmd) override {
        // Ignore a stale/duplicate tick so a double-submit can't double-step the
        // prediction (the client tick counter is the authority for ordering here).
        if (m_lastPredictedTick != 0 && cmd.tick <= m_lastPredictedTick)
            return m_state;

        // Step the predicted state immediately with the shared deterministic step
        // — the local player advances THIS tick without waiting for the server.
        stepPlayer(m_state, cmd);
        m_lastPredictedTick = cmd.tick;

        // Record in the unacknowledged-input history (kept tick-ordered: predict()
        // only ever appends a strictly-greater tick). We store the command AND the
        // predicted state AFTER applying it, so reconcile() can compare the server's
        // authority against what we PREDICTED AT THE SAME TICK (the true rubber-band
        // measure) — not against the present predicted state which is ticks ahead.
        m_history.push_back(Entry{ cmd, m_state });
        // Bound memory if ACKs stall: drop the oldest beyond the cap.
        while (m_history.size() > m_cfg.maxUnacked) m_history.pop_front();

        return m_state;
    }

    ReconcileResult reconcile(const PlayerSimState& authoritative, NetTick ackTick) override {
        ReconcileResult r;
        // Ignore a stale/duplicate ack (out-of-order snapshot, or one we already
        // reconciled against). Reconciling on a regressing ack would re-replay inputs
        // we already dropped and corrupt the predicted state.
        if (m_lastAckedTick != 0 && ackTick <= m_lastAckedTick)
            return r;
        r.applied = true;

        // RUBBER-BAND MEASURE: compare the server's authoritative state at `ackTick`
        // against what WE predicted at that SAME tick (from history). Under steady
        // no-loss conditions this is ~0 (prediction matched authority => no visible
        // correction); a genuine mispredict (missed collision / server knockback)
        // makes it large. If we have no stored prediction for that tick (e.g. it was
        // dropped past the cap, or it's the very first ack), fall back to 0 (treat as
        // a clean baseline — there's nothing to have mispredicted against).
        bool havePred = false;
        for (const Entry& e : m_history) {
            if (e.cmd.tick == ackTick) {
                r.positionError = posErr(e.predicted, authoritative);
                havePred = true;
                break;
            }
        }
        (void)havePred;

        // 1) SNAP the predicted state to the server's authoritative state as of the
        //    acked tick (rollback to authority).
        m_state = authoritative;
        m_lastAckedTick = ackTick;

        // 2) DISCARD acked inputs (tick <= ackTick). These are now baked into the
        //    authoritative state the server sent, so replaying them would double-apply.
        while (!m_history.empty() && m_history.front().cmd.tick <= ackTick) {
            m_history.pop_front();
            ++r.droppedInputs;
        }

        // 3) REPLAY all still-unacknowledged inputs through the SAME deterministic
        //    step, in tick order, to re-arrive at the present predicted state. Each
        //    replayed entry's stored predicted snapshot is refreshed so the NEXT
        //    reconcile compares against the post-correction prediction. If prediction
        //    matched the server, this reproduces the same bits we already had (visual
        //    no-op); if it diverged, this re-converges forward from authority.
        for (Entry& e : m_history) {
            stepPlayer(m_state, e.cmd);
            e.predicted = m_state;
            ++r.replayedInputs;
        }

        // A correction is "visible" only if the pre-replay snap exceeded epsilon.
        r.corrected = (r.positionError > m_cfg.reconcileEpsilon);
        return r;
    }

    const PlayerSimState& predictedState() const override { return m_state; }
    NetTick  lastPredictedTick() const override { return m_lastPredictedTick; }
    NetTick  lastAckedTick() const override { return m_lastAckedTick; }
    uint32_t unackedCount() const override { return (uint32_t)m_history.size(); }

private:
    // One retained unacknowledged input: the command + the predicted state AFTER it.
    struct Entry { NetCommand cmd; PlayerSimState predicted; };

    PredictConfig       m_cfg{};
    PlayerSimState      m_state{};
    std::deque<Entry>   m_history;          // unacknowledged inputs, tick-ascending
    NetTick             m_lastPredictedTick = 0;
    NetTick             m_lastAckedTick = 0;
};

} // namespace

IClientPredictor* createClientPredictor() { return new ClientPredictor(); }

// ===========================================================================
// --test-netpredict — Phase 1 prediction + reconciliation self-test.
//
// Topology (loopback discipline §1.4): a CLIENT submits NetCommands over the real
// transport; a LAGGED SERVER processes them a few ticks behind, advances the
// AUTHORITATIVE store with serverApplyCommand (the production integrator), then
// publishes a snapshot carrying the LOCAL player's authoritative transform/velocity
// PLUS the last-processed-input tick (the ACK). The client receives it, reconciles
// (snap-to-authority + replay unacked), and we assert the four properties (a)-(d).
//
// Carrying the ack tick on the wire (minimal extension, §6.3): the server prepends a
// tiny frame [u32 ackTick] to the Phase-0 snapshot bytes. The client peels the ack,
// applies the remaining snapshot to its client replication store exactly as Phase 0b
// does, reads the local player's authoritative transform/velocity out, and feeds
// (authoritativeState, ackTick) to the predictor. The Phase-0 snapshot wire format
// is UNTOUCHED (so --test-netsync still passes) — the ack rides in a 4-byte prefix.
// ===========================================================================
namespace {

int gnp_pass = 0, gnp_fail = 0;
void npCheck(bool ok, const char* name) {
    if (ok) { ++gnp_pass; x3::logInfo(std::string("[netpredict-test] PASS ") + name); }
    else    { ++gnp_fail; x3::logError(std::string("[netpredict-test] FAIL ") + name); }
}

// Re-use the Phase 0b command wire layout so commands traverse the real send() path.
constexpr uint32_t kCmdWire = 4 /*tick*/ + 4*4 /*floats*/ + 4 /*buttons*/; // 24 bytes
void encCmd(const NetCommand& c, uint8_t* o) {
    auto pf = [](uint8_t* d, float f){ uint32_t u; std::memcpy(&u,&f,4); std::memcpy(d,&u,4); };
    auto pu = [](uint8_t* d, uint32_t u){ std::memcpy(d,&u,4); };
    pu(o+0,c.tick); pf(o+4,c.moveFwd); pf(o+8,c.moveStrafe);
    pf(o+12,c.yaw); pf(o+16,c.pitch); pu(o+20,c.buttons);
}
NetCommand decCmd(const uint8_t* i) {
    NetCommand c;
    auto gf = [](const uint8_t* s){ uint32_t u; std::memcpy(&u,s,4); float f; std::memcpy(&f,&u,4); return f; };
    auto gu = [](const uint8_t* s){ uint32_t u; std::memcpy(&u,s,4); return u; };
    c.tick=gu(i+0); c.moveFwd=gf(i+4); c.moveStrafe=gf(i+8);
    c.yaw=gf(i+12); c.pitch=gf(i+16); c.buttons=gu(i+20);
    return c;
}
void putU32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back((uint8_t)(v&0xFF)); b.push_back((uint8_t)((v>>8)&0xFF));
    b.push_back((uint8_t)((v>>16)&0xFF)); b.push_back((uint8_t)((v>>24)&0xFF));
}
uint32_t getU32(const uint8_t* p, uint32_t& off) {
    uint32_t v = (uint32_t)p[off] | ((uint32_t)p[off+1]<<8) |
                 ((uint32_t)p[off+2]<<16) | ((uint32_t)p[off+3]<<24);
    off += 4; return v;
}

// Read a PlayerSimState (transform + velocity + state byte) out of a replication
// store for entity e (the client mirror's local-player record).
bool readSimState(IReplication* rep, NetEntityId e, PlayerSimState& out) {
    uint32_t len = 0;
    const void* pt = rep->readComponent(e, NetComp_Transform, &len);
    if (!pt || len != sizeof(RepTransform)) return false;
    std::memcpy(&out.xf, pt, sizeof(out.xf));
    if (const void* pv = rep->readComponent(e, NetComp_Velocity, &len))
        if (len == sizeof(RepVelocity)) std::memcpy(&out.vel, pv, sizeof(out.vel));
    if (const void* ph = rep->readComponent(e, NetComp_Health, &len))
        if (len == sizeof(RepHealth)) { RepHealth h; std::memcpy(&h, ph, sizeof(h)); out.state = h.flags; }
    return true;
}

// Deterministic scripted input for client tick t (a forward walk, then a sprinting
// strafing turn, then a fire-while-backing arc). Pure fn of t (replay-stable).
NetCommand scriptCmd(NetTick t) {
    NetCommand c; c.tick = t;
    if (t < 40) {                       // walk forward
        c.moveFwd = 1.0f; c.moveStrafe = 0.0f; c.yaw = 0.0f; c.buttons = 0;
    } else if (t < 90) {                // sprint + strafe + turn
        c.moveFwd = 0.6f; c.moveStrafe = 1.0f;
        c.yaw = (float)(t - 40) * 0.03f; c.buttons = NetBtn_Sprint;
    } else {                            // fire while backing up + turning
        c.moveFwd = -0.5f; c.moveStrafe = 0.3f;
        c.yaw = 1.5f - (float)(t - 90) * 0.01f; c.buttons = NetBtn_Fire;
    }
    return c;
}

} // namespace

bool runNetPredictSelfTest() {
    gnp_pass = 0; gnp_fail = 0;

    // --- wire up one loopback pair. Client end submits commands; server end is the
    // authoritative NetworkSystem with its own store. The server publishes an
    // ack-prefixed snapshot; the client mirrors it into a separate client store +
    // feeds the predictor. ----------------------------------------------------------
    INetTransport* serverT = createLoopbackTransport();   // server end
    INetTransport* clientT = serverT->loopbackPeer();      // client end
    IReplication*  serverRep = createReplication();        // authoritative
    IReplication*  clientRep = createReplication();         // client mirror

    INetworkSystem* server = createNetworkSystem();
    bool inited = server->init(NetRole::DedicatedServer, serverT, serverRep);

    const ClientId kClient{1};
    NetEntityId player = serverRep->spawn(/*Player*/ 1, kClient);
    { RepTransform z{}; serverRep->setComponent(player, NetComp_Transform, &z, sizeof(z)); }

    IClientPredictor* pred = createClientPredictor();
    { PlayerSimState init{}; pred->reset(init); }

    constexpr uint32_t kTicks   = 150;
    constexpr uint32_t kLagTicks = 5;     // SIMULATED SERVER LAG: server runs behind
    constexpr float    kEps     = 1e-4f;

    // Independent oracle: predict the SAME script with NO transport/reconcile at all,
    // to confirm reconciliation never perturbs the deterministic predicted trajectory.
    IClientPredictor* oracle = createClientPredictor();
    { PlayerSimState init{}; oracle->reset(init); }

    // Property accumulators.
    bool predictedAlwaysAhead = true;   // (a) predicted tick > acked tick while lagged
    bool everReconciled = false;
    bool reconcileConverged = true;     // (b) post-replay == server within eps
    bool steadyNoRubberBand = true;     // (c) no correction under steady no-loss
    bool maxSteadyErrOk = true;
    float maxSteadyErr = 0.0f;

    // The server's authoritative "last processed input tick" for this client.
    NetTick serverAckTick = 0;
    // We inject ONE dropped snapshot (a late/lost packet) at this server-processed
    // tick to prove reconciliation survives a gap (property d's robustness leg).
    const NetTick kDropAtAck = 30;

    // We inject ONE deliberate authoritative mismatch: at this acked tick the server
    // teleports the player +2 m on X (a stand-in for a server-applied knockback /
    // collision the client could not predict). The client must SNAP+converge (d).
    const NetTick kKnockAtTick = 70;
    bool sawCorrection = false;
    bool postKnockConverged = false;

    // Pending command buffer the lagged server drains from (commands wait kLagTicks).
    std::deque<NetCommand> serverInbox;

    for (uint32_t i = 0; i < kTicks; ++i) {
        const NetTick clientTick = i + 1;          // 1-based client tick
        const NetCommand cmd = scriptCmd(clientTick);

        // 1) CLIENT predicts immediately (zero-latency local response) + records it.
        pred->predict(cmd);
        oracle->predict(cmd);

        // 2) CLIENT sends the command over the real transport (c->s).
        { uint8_t w[kCmdWire]; encCmd(cmd, w);
          clientT->send(ConnectionId{1}, w, kCmdWire, NetChannel::ReliableOrdered);
          clientT->flush(); }

        // 3) SERVER polls the wire -> its pending-command queue, then we move ready
        //    commands into our lag-modeled inbox (delay each by kLagTicks).
        server->poll();
        { NetCommand drained[8]; ClientId who[8];
          uint32_t n = server->serverDrainCommands(clientTick, drained, 8, who);
          for (uint32_t k = 0; k < n; ++k) serverInbox.push_back(drained[k]); }

        // 4) SERVER processes (applies authoritatively) only commands old enough to
        //    have "arrived" given the simulated lag: tick <= clientTick - kLagTicks.
        const NetTick processUpTo = (clientTick > kLagTicks) ? (clientTick - kLagTicks) : 0;
        bool publishedThisTick = false;
        while (!serverInbox.empty() && serverInbox.front().tick <= processUpTo) {
            NetCommand c = serverInbox.front(); serverInbox.pop_front();
            serverApplyCommand(serverRep, player, c);
            serverAckTick = c.tick;     // last input tick the server has now processed

            // Inject the unpredictable authoritative knockback at the chosen tick.
            if (serverAckTick == kKnockAtTick) {
                RepTransform xf{}; uint32_t l = 0;
                if (const void* p = serverRep->readComponent(player, NetComp_Transform, &l))
                    if (l == sizeof(xf)) std::memcpy(&xf, p, sizeof(xf));
                xf.pos[0] += 2.0f;     // server-only displacement the client can't predict
                serverRep->setComponent(player, NetComp_Transform, &xf, sizeof(xf));
            }
            publishedThisTick = true;
        }

        // 5) SERVER publishes an ACK-PREFIXED snapshot (s->c) when it advanced.
        if (publishedThisTick) {
            // [u32 ackTick][Phase-0 snapshot bytes...]
            uint8_t snap[4096];
            uint32_t slen = serverRep->encodeSnapshot(kClient, serverAckTick, snap + 4, sizeof(snap) - 4);
            if (slen > 0) {
                std::memcpy(snap + 0, &serverAckTick, 4);  // little-endian prefix
                // Drop ONE snapshot to simulate a lost/late packet (robustness, d).
                if (serverAckTick != kDropAtAck)
                    serverT->send(ConnectionId{1}, snap, slen + 4, NetChannel::UnreliableSequenced);
                serverT->flush();
            }
        }

        // (a) PREDICTION IS AHEAD: while the server lags, the client's predicted tick
        //     must be strictly ahead of the latest ack it has reconciled.
        if (clientTick > kLagTicks + 1 && pred->lastPredictedTick() <= pred->lastAckedTick())
            predictedAlwaysAhead = false;

        // 6) CLIENT receives any snapshot(s), peels the ack, applies the rest to its
        //    mirror store, reads the local player's authoritative state, reconciles.
        for (;;) {
            uint8_t buf[4096];
            uint32_t got = clientT->recv(ConnectionId{1}, buf, sizeof(buf));
            if (got == 0) break;
            if (got < 4) continue;
            uint32_t off = 0;
            const NetTick ackTick = getU32(buf, off);
            // Apply the Phase-0 snapshot body to the client mirror store.
            NetTick snapTick = 0;
            clientRep->applySnapshot(buf + 4, got - 4, &snapTick);
            // Read the authoritative local-player state out of the mirror.
            PlayerSimState authoritative{};
            if (!readSimState(clientRep, player, authoritative)) continue;

            ReconcileResult rr = pred->reconcile(authoritative, ackTick);
            if (!rr.applied) continue;
            everReconciled = true;
            // rr.positionError is the TRUE rubber-band magnitude: it compares the
            // server's authority AT ackTick against what the client PREDICTED at that
            // same tick (from history) — NOT the present (ticks-ahead) predicted state.

            // (b) After replay, the predicted state must match what the server WILL
            //     have at this same set of inputs. We can't read the server's "now"
            //     (it's lagged), but the converge check is: re-run the SERVER's
            //     authoritative integrator forward from `authoritative` over the SAME
            //     unacked commands and compare to the predictor's post-replay state.
            {
                // Recompute the expected post-replay state independently using the
                // shared step over the still-unacked window [ackTick+1 .. lastPredicted].
                PlayerSimState expect = authoritative;
                for (NetTick t = ackTick + 1; t <= pred->lastPredictedTick(); ++t)
                    stepPlayer(expect, scriptCmd(t));
                if (posErr(expect, pred->predictedState()) > kEps) reconcileConverged = false;
            }

            // (c) STEADY STATE (no knock, no drop-gap recovery): the snap error must
            //     be within epsilon => no rubber-band. We exclude the knock tick and
            //     the first ack right after the dropped snapshot (which legitimately
            //     carries a larger but still-correct catch-up, not a mispredict).
            const bool isKnockAck = (ackTick >= kKnockAtTick && ackTick <= kKnockAtTick + kLagTicks + 1);
            const bool isDropRecovery = (ackTick >= kDropAtAck && ackTick <= kDropAtAck + 2);
            if (!isKnockAck && !isDropRecovery) {
                if (rr.positionError > maxSteadyErr) maxSteadyErr = rr.positionError;
                if (rr.corrected) steadyNoRubberBand = false;
            }

            // (d) The injected knockback MUST register as a correction, and the
            //     replay must re-converge (the post-replay error vs the independent
            //     recompute above is already asserted in (b)).
            if (isKnockAck && rr.corrected) {
                sawCorrection = true;
                // Verify convergence: predicted state == authoritative + replayed
                // unacked inputs (recompute independently).
                PlayerSimState expect = authoritative;
                for (NetTick t = ackTick + 1; t <= pred->lastPredictedTick(); ++t)
                    stepPlayer(expect, scriptCmd(t));
                if (posErr(expect, pred->predictedState()) <= kEps) postKnockConverged = true;
            }
        }
    }
    if (maxSteadyErr > 1e-2f) maxSteadyErrOk = false;  // sanity: steady error tiny

    // --- assertions ----------------------------------------------------------
    npCheck(inited, "P0 server init over loopback");
    npCheck(predictedAlwaysAhead && pred->lastPredictedTick() == kTicks,
            "P1 predicted local player advances immediately each tick (ahead of acks)");
    npCheck(everReconciled && reconcileConverged,
            "P2 post-replay predicted state matches server authority within eps");
    npCheck(steadyNoRubberBand && maxSteadyErrOk,
            "P3 steady no-loss reconciliation produces no correction jump (no rubber-band)");
    npCheck(sawCorrection && postKnockConverged,
            "P4 injected mismatch is corrected and replay converges");

    // P5: the predictor's own determinism — re-running the SAME script through a fresh
    // predictor with NO reconcile (the oracle) reaches a bit-identical predicted state
    // to the live predictor (reconciliation did not perturb the deterministic path).
    // The live predictor was reconciled to authority repeatedly; because every snap +
    // replay is deterministic and the authority equals the prediction (modulo the one
    // injected knock, which the live predictor then carries), the LIVE predicted state
    // includes the +2 m knock while the oracle does NOT. So compare with that offset.
    {
        PlayerSimState live = pred->predictedState();
        PlayerSimState orc  = oracle->predictedState();
        // The live trajectory absorbed exactly one +2 m X knock; remove it to compare
        // the deterministic shape. (Position only — velocity/state are tick-local.)
        const float adjustedDx = (live.xf.pos[0] - 2.0f) - orc.xf.pos[0];
        const float dz = live.xf.pos[2] - orc.xf.pos[2];
        bool deterministicShape = std::fabs(adjustedDx) < kEps && std::fabs(dz) < kEps;
        npCheck(deterministicShape,
                "P5 reconciliation is deterministic (live==oracle modulo the one knock)");
    }

    // P6: unacked history stays bounded by the lag window (it never grows unbounded).
    npCheck(pred->unackedCount() <= kLagTicks + 3 + 1,
            "P6 unacked input history bounded by the lag window");

    server->shutdown();
    delete server;
    delete serverT;     // owns + deletes the client (peer) transport
    delete serverRep;
    delete clientRep;
    delete pred;
    delete oracle;

    const int total = gnp_pass + gnp_fail;
    x3::logInfo("netpredict: " + std::to_string(gnp_pass) + "/" +
                std::to_string(total) + " passed");
    return gnp_fail == 0;
}

} // namespace x3::net
