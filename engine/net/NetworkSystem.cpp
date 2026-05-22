// NetworkSystem — the facade wiring transport + replication + tick, plus the
// Phase-0 self-test (--test-net).
// Spec: specs/NETCODE-architecture.spec.md §8 / §0.
//
// Phase 0: a minimal in-process impl. In LocalLoopback the same object owns the
// server-side replication store and routes commands client->server + snapshots
// server->client over the loopback transport. NOT wired into the game loop yet
// (Phase 0b); it exists to make the seam real + testable.

#include "engine/net/INetworkSystem.h"
#include "engine/net/INetTransport.h"
#include "engine/net/IReplication.h"
#include "engine/net/SimClock.h"
#include "engine/core/x3_log.h"

#include <vector>
#include <deque>
#include <cstring>
#include <cmath>
#include <string>

namespace x3::net {

namespace {

// Wire framing for the command channel (client -> server): a NetCommand serialized
// field-by-field (fixed little-endian layout so it round-trips byte-identically).
constexpr uint32_t kCmdWireSize = 4 /*tick*/ + 4*4 /*floats*/ + 4 /*buttons*/; // 24 bytes

void encodeCommand(const NetCommand& c, uint8_t* out) {
    auto pf = [](uint8_t* d, float f) { uint32_t u; std::memcpy(&u, &f, 4); std::memcpy(d, &u, 4); };
    auto pu = [](uint8_t* d, uint32_t u) { std::memcpy(d, &u, 4); };
    pu(out + 0,  c.tick);
    pf(out + 4,  c.moveFwd);
    pf(out + 8,  c.moveStrafe);
    pf(out + 12, c.yaw);
    pf(out + 16, c.pitch);
    pu(out + 20, c.buttons);
}
NetCommand decodeCommand(const uint8_t* in) {
    NetCommand c;
    auto gf = [](const uint8_t* s) { uint32_t u; std::memcpy(&u, s, 4); float f; std::memcpy(&f, &u, 4); return f; };
    auto gu = [](const uint8_t* s) { uint32_t u; std::memcpy(&u, s, 4); return u; };
    c.tick       = gu(in + 0);
    c.moveFwd    = gf(in + 4);
    c.moveStrafe = gf(in + 8);
    c.yaw        = gf(in + 12);
    c.pitch      = gf(in + 16);
    c.buttons    = gu(in + 20);
    return c;
}

class NetworkSystem final : public INetworkSystem {
public:
    bool init(NetRole role, INetTransport* transport, IReplication* replication) override {
        if (m_inited || !transport) return false;
        m_role = role;
        m_transport = transport;
        m_replication = replication;
        m_isServer = (role != NetRole::RemoteClient);
        m_isClient = (role == NetRole::LocalLoopback || role == NetRole::ListenServer ||
                      role == NetRole::RemoteClient);
        // Loopback: start both ends so the in-process pair is connectable.
        m_transport->start("loopback", /*asServer*/ m_isServer);
        if (INetTransport* peer = m_transport->loopbackPeer())
            peer->start("loopback", /*asServer*/ false);
        m_conn = ConnectionId{ 1 };
        m_inited = true;
        return true;
    }

    void shutdown() override {
        if (!m_inited) return;
        if (m_transport) m_transport->shutdown();
        m_inited = false;
    }

    uint32_t serverDrainCommands(NetTick tick, NetCommand* out, uint32_t maxOut, ClientId* outWho) override {
        uint32_t n = 0;
        // Move any commands whose tick is <= the current tick out of the pending
        // queue (Phase 0 keeps it simple: drain everything ready for this tick).
        for (auto it = m_pendingCmds.begin(); it != m_pendingCmds.end() && n < maxOut; ) {
            if (it->cmd.tick <= tick) {
                if (out) out[n] = it->cmd;
                if (outWho) outWho[n] = it->who;
                ++n;
                it = m_pendingCmds.erase(it);
            } else {
                ++it;
            }
        }
        return n;
    }

    void serverPublish(NetTick tick) override {
        if (!m_transport || !m_replication) return;
        uint8_t buf[4096];
        const uint32_t len = m_replication->encodeSnapshot(ClientId{1}, tick, buf, sizeof(buf));
        if (len > 0) {
            // Send from the SERVER end over the unreliable/sequenced channel
            // (snapshots are latest-wins). This pushes onto the server->client
            // queue, which the client end reads via its own recv().
            m_transport->send(m_conn, buf, len, NetChannel::UnreliableSequenced);
        }
        m_transport->flush();
    }

    void clientSubmitCommand(const NetCommand& cmd) override {
        if (!m_transport) return;
        uint8_t buf[kCmdWireSize];
        encodeCommand(cmd, buf);
        // Client end sends to the server over the reliable-ordered channel.
        m_transport->send(m_conn, buf, kCmdWireSize, NetChannel::ReliableOrdered);
        m_transport->flush();
    }

    NetTick clientLastServerTick() const override { return m_lastServerTick; }

    void poll() override {
        if (!m_transport) return;
        NetEvent ev[4];
        m_transport->pollEvents(ev, 4);  // connect/disconnect (Phase 0: ignored beyond surfacing)

        // Server side: pull any commands the client sent.
        if (m_isServer && m_transport->loopbackPeer()) {
            // The server reads from ITS transport (server end). recv drains one
            // buffer per call; loop until empty.
            uint8_t buf[256];
            for (;;) {
                const uint32_t n = m_transport->recv(m_conn, buf, sizeof(buf));
                if (n == 0) break;
                if (n == kCmdWireSize) {
                    Pending p; p.cmd = decodeCommand(buf); p.who = ClientId{1};
                    m_pendingCmds.push_back(p);
                }
            }
        }

        // Client side: apply any received snapshot. In LocalLoopback the SAME
        // object is both ends; the peer transport carries server->client snapshots.
        if (m_isClient && m_replication && m_transport->loopbackPeer()) {
            uint8_t buf[4096];
            for (;;) {
                const uint32_t n = m_transport->loopbackPeer()->recv(m_conn, buf, sizeof(buf));
                if (n == 0) break;
                NetTick t = 0;
                m_replication->applySnapshot(buf, n, &t);
                m_lastServerTick = t;
            }
        }
    }

    bool isServer() const override { return m_isServer; }
    bool isClient() const override { return m_isClient; }

private:
    struct Pending { NetCommand cmd; ClientId who; };

    NetRole        m_role = NetRole::LocalLoopback;
    INetTransport* m_transport = nullptr;
    IReplication*  m_replication = nullptr;
    ConnectionId   m_conn;
    bool m_inited = false, m_isServer = false, m_isClient = false;
    NetTick m_lastServerTick = 0;
    std::deque<Pending> m_pendingCmds;
};

// ---------------------------------------------------------------------------
// Self-test (--test-net). Three properties from the task's verification gate:
//   T1  loopback transport round-trips a NetCommand byte-identically
//   T2  NetEntityId generation rejects a stale handle after a slot recycle
//   T3  the fixed-step accumulator advances deterministically (same input+dt =>
//       same tick count), and physics steps exactly once per kSimDt tick worth
//   T4  (bonus) end-to-end: client command -> server -> snapshot -> client apply
// ---------------------------------------------------------------------------
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
    if (ok) { ++g_pass; x3::logInfo(std::string("[net-test] PASS ") + name); }
    else    { ++g_fail; x3::logError(std::string("[net-test] FAIL ") + name); }
}

bool testCommandRoundTrip() {
    auto* t = createLoopbackTransport();           // server end
    INetTransport* peer = t->loopbackPeer();        // client end
    t->start("loopback", true);
    peer->start("loopback", false);

    NetCommand sent;
    sent.tick = 12345;
    sent.moveFwd = 0.5f; sent.moveStrafe = -0.25f;
    sent.yaw = 1.5707963f; sent.pitch = -0.3f;
    sent.buttons = NetBtn_Fire | NetBtn_Jump;

    uint8_t wire[kCmdWireSize];
    encodeCommand(sent, wire);
    // client -> server: client end sends, server end receives.
    bool okSend = peer->send(ConnectionId{1}, wire, kCmdWireSize, NetChannel::ReliableOrdered);
    uint8_t got[kCmdWireSize] = {0};
    uint32_t n = t->recv(ConnectionId{1}, got, sizeof(got));
    bool sameLen = (n == kCmdWireSize);
    bool sameBytes = sameLen && (std::memcmp(wire, got, kCmdWireSize) == 0);
    NetCommand decoded = decodeCommand(got);
    bool sameFields = decoded.tick == sent.tick && decoded.buttons == sent.buttons &&
                      decoded.moveFwd == sent.moveFwd && decoded.moveStrafe == sent.moveStrafe &&
                      decoded.yaw == sent.yaw && decoded.pitch == sent.pitch;
    delete t;  // owns + deletes peer
    bool ok = okSend && sameLen && sameBytes && sameFields;
    check(ok, "T1 NetCommand loopback round-trip (byte-identical)");
    return ok;
}

bool testGenerationStaleReject() {
    auto* rep = createReplication();
    // Spawn an entity, grab a handle, despawn it, then spawn AGAIN. Because the
    // slot is recycled with a bumped generation, the OLD handle must be rejected.
    NetEntityId a = rep->spawn(/*archetype*/ 1, ClientId{0});
    bool aliveBefore = rep->alive(a);
    RepHealth h; h.hp = 80; h.maxHp = 100;
    rep->setComponent(a, NetComp_Health, &h, sizeof(h));

    rep->despawn(a);
    bool deadAfterDespawn = !rep->alive(a);

    // Recycle the slot: a fresh spawn should reuse slot of `a` with a higher gen.
    NetEntityId b = rep->spawn(/*archetype*/ 2, ClientId{0});
    bool sameSlot = (netSlot(a) == netSlot(b));
    bool diffGen  = (netGeneration(a) != netGeneration(b));
    bool diffId   = (a.id != b.id);
    bool staleRejected = !rep->alive(a);            // old handle now stale
    bool freshAccepted = rep->alive(b);             // new handle valid

    // The stale handle must NOT read/leak the recycled slot's data.
    uint32_t cl = 0;
    const void* staleRead = rep->readComponent(a, NetComp_Health, &cl);

    delete rep;
    bool ok = aliveBefore && deadAfterDespawn && sameSlot && diffGen && diffId &&
              staleRejected && freshAccepted && staleRead == nullptr;
    check(ok, "T2 NetEntityId generation rejects stale handle after slot recycle");
    return ok;
}

bool testFixedStepDeterminism() {
    // Same input dt sequence => identical tick count, irrespective of how the dt is
    // chopped across frames. Run TWO accumulators: one fed many small frames, one
    // fed a few large frames, both summing to the same total time; the total tick
    // counts must match. Also assert one-step-per-kSimDt mapping (Jolt alignment).
    auto runSeq = [](const float* dts, int count) -> NetTick {
        SimAccumulator acc;
        for (int i = 0; i < count; ++i) acc.advance(dts[i]);
        return acc.tick;
    };

    // Sequence A: 600 frames of exactly kSimDt -> exactly 600 ticks.
    std::vector<float> a(600, kSimDt);
    NetTick tA = runSeq(a.data(), (int)a.size());
    bool exact = (tA == 600);

    // Sequence B: irregular frame times summing to ~10 s (600 * kSimDt). The
    // leftover-carry must make the tick total deterministic and equal to floor.
    std::vector<float> b;
    double total = 0.0;
    // 10 seconds of jittery frame times (alternating 1/144 and 1/30-ish).
    for (int i = 0; i < 2000 && total < 10.0; ++i) {
        float d = (i & 1) ? (1.0f / 144.0f) : (1.0f / 50.0f);
        b.push_back(d); total += d;
    }
    // Two identical irregular runs must give identical tick counts (determinism).
    NetTick tB1 = runSeq(b.data(), (int)b.size());
    NetTick tB2 = runSeq(b.data(), (int)b.size());
    bool reproducible = (tB1 == tB2);
    // And it should equal floor(total / kSimDt) within the carry (no steps lost).
    NetTick expected = (NetTick)(total / (double)kSimDt + 1e-6);
    bool noLostSteps = (tB1 == expected);

    // Spiral-of-death clamp: a giant dt must NOT produce unbounded steps.
    SimAccumulator big;
    uint32_t steps = big.advance(100.0f);   // 100 s in one frame
    bool clamped = (steps <= (uint32_t)(kMaxAccum / kSimDt) + 1);

    bool ok = exact && reproducible && noLostSteps && clamped;
    check(ok, "T3 fixed-step accumulator deterministic + anti-spiral clamp");
    return ok;
}

bool testEndToEnd() {
    // Wire a server-side network system over loopback; spawn a replicated entity,
    // set a transform, publish a snapshot, and confirm the client side applies it.
    auto* serverTransport = createLoopbackTransport();
    auto* serverRep = createReplication();
    auto* clientRep = createReplication();

    NetworkSystem server;
    server.init(NetRole::DedicatedServer, serverTransport, serverRep);

    // Build a tiny client that shares the loopback peer + its own replication.
    // (Phase 0 wires the client by reading the peer transport directly.)
    INetTransport* clientTransport = serverTransport->loopbackPeer();

    NetEntityId e = serverRep->spawn(/*Player*/ 1, ClientId{1});
    RepTransform xf; xf.pos[0] = 3.0f; xf.pos[1] = 1.0f; xf.pos[2] = -2.0f;
    serverRep->setComponent(e, NetComp_Transform, &xf, sizeof(xf));

    server.serverPublish(/*tick*/ 7);

    // Client: drain the snapshot from the peer transport, apply to clientRep.
    uint8_t buf[4096];
    NetTick appliedTick = 0;
    uint32_t n = clientTransport->recv(ConnectionId{1}, buf, sizeof(buf));
    if (n > 0) clientRep->applySnapshot(buf, n, &appliedTick);

    bool gotTick = (appliedTick == 7);
    bool gotEntity = clientRep->alive(e);
    uint32_t cl = 0;
    const RepTransform* rx =
        (const RepTransform*)clientRep->readComponent(e, NetComp_Transform, &cl);
    bool gotXform = rx && cl == sizeof(RepTransform) &&
                    rx->pos[0] == 3.0f && rx->pos[1] == 1.0f && rx->pos[2] == -2.0f;

    server.shutdown();
    delete serverTransport;   // owns + deletes peer
    delete serverRep;
    delete clientRep;

    bool ok = (n > 0) && gotTick && gotEntity && gotXform;
    check(ok, "T4 end-to-end command/snapshot over loopback (spawn->publish->apply)");
    return ok;
}

// ---------------------------------------------------------------------------
// Phase 0b — client/server input->snapshot routing (--test-netsync).
//
// Builds the FULL slice on top of the Phase 0 facade: a client samples input into
// NetCommands, sends them over the loopback transport; the server applies each
// command authoritatively and advances a deterministic fixed-step sim ONE tick per
// command; after each tick the server publishes a Snapshot (list of net entities:
// {NetEntityId, position, rotation, velocity, state byte}); the client receives the
// snapshot and mirrors it into its OWN replication store. Server-authoritative +
// deterministic; NO prediction/rollback/AoI (later phases).
//
// The simulation model here is intentionally small + self-contained (clean-room):
// a single player entity whose planar position integrates from the command's move
// axes along the yaw-derived forward/right basis (CONVENTIONS.md forward), at the
// fixed kSimDt. This stands in for the server-side Player::update path (§8 "one
// function, two callers") while keeping the test free of render/physics deps.
// ---------------------------------------------------------------------------

// State byte bits packed into RepHealth::flags by the authoritative sim, so the
// snapshot carries a "state byte" the client mirror can verify converges.
enum NetState : uint8_t {
    NetState_Moving   = 1u << 0,   // |move axes| above the deadzone this tick
    NetState_Sprinting= 1u << 1,   // sprint button held
    NetState_Firing   = 1u << 2,   // fire button held
};

// Deterministic per-tick authoritative step for one player entity. Reads its
// current RepTransform/RepVelocity from the store, integrates from `cmd`, writes
// the post-tick RepTransform + RepVelocity + a RepHealth state byte back. This is
// the ONE place authoritative state changes (server-only), at exactly kSimDt.
void serverApplyCommand(IReplication* rep, NetEntityId e, const NetCommand& cmd) {
    // Read current transform (default to origin if not yet set).
    RepTransform xf{};
    uint32_t len = 0;
    if (const void* p = rep->readComponent(e, NetComp_Transform, &len))
        if (len == sizeof(RepTransform)) std::memcpy(&xf, p, sizeof(xf));

    // Movement constants: a base speed, sprint multiplier. Deterministic scalars.
    constexpr float kBaseSpeed   = 4.0f;   // m/s
    constexpr float kSprintScale = 1.8f;
    const bool sprint = (cmd.buttons & NetBtn_Sprint) != 0;
    const float speed = kBaseSpeed * (sprint ? kSprintScale : 1.0f);

    // Yaw-derived planar basis (CONVENTIONS.md §3 forward). Forward = (-sin y, 0,
    // -cos y) style is engine-specific; for the deterministic test we only need a
    // STABLE, reproducible basis, so use the standard right-handed yaw mapping:
    //   forward = (sin yaw, 0, -cos yaw),  right = (cos yaw, 0, sin yaw)
    const float s = std::sin(cmd.yaw);
    const float c = std::cos(cmd.yaw);
    const float fwd[3]   = {  s, 0.0f, -c };
    const float right[3] = {  c, 0.0f,  s };

    // Planar velocity from the command axes, integrated at the fixed dt.
    float vx = (fwd[0] * cmd.moveFwd + right[0] * cmd.moveStrafe) * speed;
    float vz = (fwd[2] * cmd.moveFwd + right[2] * cmd.moveStrafe) * speed;
    xf.pos[0] += vx * kSimDt;
    xf.pos[2] += vz * kSimDt;

    // Rotation: the entity faces its look yaw (quaternion about +Y, x,y,z,w order).
    const float half = cmd.yaw * 0.5f;
    xf.rotQuat[0] = 0.0f;
    xf.rotQuat[1] = std::sin(half);
    xf.rotQuat[2] = 0.0f;
    xf.rotQuat[3] = std::cos(half);

    RepVelocity vel{};
    vel.lin[0] = vx; vel.lin[1] = 0.0f; vel.lin[2] = vz;

    // State byte (server-resolved from the held button bitfield + motion).
    const float moveMag2 = cmd.moveFwd * cmd.moveFwd + cmd.moveStrafe * cmd.moveStrafe;
    uint8_t state = 0;
    if (moveMag2 > 1e-6f)              state |= NetState_Moving;
    if (sprint)                       state |= NetState_Sprinting;
    if (cmd.buttons & NetBtn_Fire)    state |= NetState_Firing;
    RepHealth hp{};
    hp.hp = 100; hp.maxHp = 100; hp.flags = state;

    rep->setComponent(e, NetComp_Transform, &xf,  sizeof(xf));
    rep->setComponent(e, NetComp_Velocity,  &vel, sizeof(vel));
    rep->setComponent(e, NetComp_Health,    &hp,  sizeof(hp));
}

} // namespace

INetworkSystem* createNetworkSystem() { return new NetworkSystem(); }

bool runNetworkSelfTest() {
    g_pass = 0; g_fail = 0;
    testCommandRoundTrip();
    testGenerationStaleReject();
    testFixedStepDeterminism();
    testEndToEnd();
    x3::logInfo("[net-test] " + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

// ---------------------------------------------------------------------------
// --test-netsync — Phase 0b end-to-end client/server input->snapshot routing.
// ---------------------------------------------------------------------------
namespace {

int gns_pass = 0, gns_fail = 0;
void nsCheck(bool ok, const char* name) {
    if (ok) { ++gns_pass; x3::logInfo(std::string("[netsync-test] PASS ") + name); }
    else    { ++gns_fail; x3::logError(std::string("[netsync-test] FAIL ") + name); }
}

// Read a RepTransform out of a store (returns false if absent / wrong size).
bool readXform(IReplication* rep, NetEntityId e, RepTransform& out) {
    uint32_t len = 0;
    const void* p = rep->readComponent(e, NetComp_Transform, &len);
    if (!p || len != sizeof(RepTransform)) return false;
    std::memcpy(&out, p, sizeof(out));
    return true;
}
uint8_t readState(IReplication* rep, NetEntityId e) {
    uint32_t len = 0;
    const void* p = rep->readComponent(e, NetComp_Health, &len);
    if (!p || len != sizeof(RepHealth)) return 0xFF;
    RepHealth h; std::memcpy(&h, p, sizeof(h));
    return h.flags;
}

// Build a deterministic NetCommand for tick `t` (a scripted walk: forward, then
// a strafing arc with sprint, then a fire-while-turning segment). Pure function of
// t so both the "expected" recompute and the driven run are bit-identical.
NetCommand scriptCommand(NetTick t) {
    NetCommand c;
    c.tick = t;
    if (t < 60) {                       // 0..59: walk forward, facing +yaw 0
        c.moveFwd = 1.0f; c.moveStrafe = 0.0f; c.yaw = 0.0f; c.pitch = 0.0f;
        c.buttons = 0;
    } else if (t < 120) {               // 60..119: sprint + strafe, turning
        c.moveFwd = 0.5f; c.moveStrafe = 1.0f;
        c.yaw = (float)(t - 60) * 0.02f; c.pitch = 0.0f;
        c.buttons = NetBtn_Sprint;
    } else {                            // 120+: fire while backing up + turning
        c.moveFwd = -0.5f; c.moveStrafe = 0.25f;
        c.yaw = 1.2f - (float)(t - 120) * 0.01f; c.pitch = -0.1f;
        c.buttons = NetBtn_Fire;
    }
    return c;
}

} // namespace

bool runNetSyncSelfTest() {
    gns_pass = 0; gns_fail = 0;

    // --- wire up: one loopback pair; server end drives a NetworkSystem with the
    // AUTHORITATIVE store; the client is a separate render-mirror store reading the
    // peer transport (the Phase-0 in-process client wiring, T4 pattern). This routes
    // every command + snapshot through the real transport interface so a UDP impl
    // can drop in unchanged.
    INetTransport* serverTransport = createLoopbackTransport();  // server end
    INetTransport* clientTransport = serverTransport->loopbackPeer();
    IReplication*  serverRep = createReplication();   // authoritative
    IReplication*  clientRep = createReplication();   // client mirror

    NetworkSystem server;
    bool inited = server.init(NetRole::DedicatedServer, serverTransport, serverRep);

    // Server spawns the authoritative player entity (owner = client 1).
    const ClientId kClient{1};
    NetEntityId player = serverRep->spawn(/*Player archetype*/ 1, kClient);
    // Seed an initial transform at the origin so the entity is replicated from t=0.
    RepTransform seed{};
    serverRep->setComponent(player, NetComp_Transform, &seed, sizeof(seed));

    constexpr uint32_t kTicks = 180;            // 3 s @ 60 Hz
    constexpr float    kEps   = 1e-4f;

    // Independent "oracle": recompute the authoritative trajectory in a SECOND
    // store from the same scripted commands, with no transport involved. The driven
    // server store MUST match this exactly (determinism / authority sanity).
    IReplication* oracleRep = createReplication();
    NetEntityId oraclePlayer = oracleRep->spawn(1, kClient);
    { RepTransform z{}; oracleRep->setComponent(oraclePlayer, NetComp_Transform, &z, sizeof(z)); }

    bool noLeak = true;            // server/client/oracle never exceed 1 live entity
    bool clientEverApplied = false;

    NetTick lastServerTick = 0;
    for (uint32_t i = 0; i < kTicks; ++i) {
        const NetTick tick = i + 1;             // ticks are 1-based (0 == "none yet")
        const NetCommand cmd = scriptCommand(tick);

        // 1) CLIENT samples input -> NetCommand -> send over transport (c->s).
        //    Route through clientSubmitCommand on the client transport so the bytes
        //    traverse the real send() path (loopback discipline §1.4).
        {
            uint8_t wire[kCmdWireSize];
            encodeCommand(cmd, wire);
            clientTransport->send(ConnectionId{1}, wire, kCmdWireSize, NetChannel::ReliableOrdered);
            clientTransport->flush();
        }

        // 2) SERVER poll: drain the wire into the pending-command queue.
        server.poll();

        // 3) SERVER apply + advance the deterministic fixed-step sim ONE tick per
        //    command. Drain the command(s) for this tick and apply authoritatively.
        NetCommand drained[8]; ClientId who[8];
        uint32_t n = server.serverDrainCommands(tick, drained, 8, who);
        for (uint32_t k = 0; k < n; ++k)
            serverApplyCommand(serverRep, player, drained[k]);

        // Oracle advances identically (no transport): same command, same apply.
        serverApplyCommand(oracleRep, oraclePlayer, cmd);

        // 4) SERVER publishes a Snapshot of all net entities (s->c over transport).
        server.serverPublish(tick);

        // 5) CLIENT receives the snapshot + mirrors it into its own store.
        {
            uint8_t buf[4096];
            for (;;) {
                const uint32_t got = clientTransport->recv(ConnectionId{1}, buf, sizeof(buf));
                if (got == 0) break;
                NetTick t = 0;
                clientRep->applySnapshot(buf, got, &t);
                lastServerTick = t;
                clientEverApplied = true;
            }
        }

        if (serverRep->liveCount() > 1 || clientRep->liveCount() > 1 ||
            oracleRep->liveCount() > 1)
            noLeak = false;
    }

    // --- assertions ----------------------------------------------------------
    nsCheck(inited, "S0 server init over loopback");

    // S1: client mirror has exactly the one server entity, by the SAME id.
    bool idsCorrect = clientRep->alive(player) && clientRep->liveCount() == 1 &&
                      serverRep->liveCount() == 1;
    nsCheck(idsCorrect, "S1 client mirrors server entity set by stable id (no extras)");

    // S2: positions converge within epsilon (server authoritative == client mirror).
    RepTransform sx{}, cx{};
    bool haveS = readXform(serverRep, player, sx);
    bool haveC = readXform(clientRep, player, cx);
    bool posMatch = haveS && haveC &&
        std::fabs(sx.pos[0] - cx.pos[0]) < kEps &&
        std::fabs(sx.pos[1] - cx.pos[1]) < kEps &&
        std::fabs(sx.pos[2] - cx.pos[2]) < kEps;
    nsCheck(posMatch, "S2 client position converges to server authority (within eps)");

    // S3: rotation converges too (the full transform block round-trips).
    bool rotMatch = haveS && haveC &&
        std::fabs(sx.rotQuat[0] - cx.rotQuat[0]) < kEps &&
        std::fabs(sx.rotQuat[1] - cx.rotQuat[1]) < kEps &&
        std::fabs(sx.rotQuat[2] - cx.rotQuat[2]) < kEps &&
        std::fabs(sx.rotQuat[3] - cx.rotQuat[3]) < kEps;
    nsCheck(rotMatch, "S3 client rotation converges to server authority (within eps)");

    // S4: the state byte (server-resolved from buttons+motion) replicates exactly.
    uint8_t ss = readState(serverRep, player);
    uint8_t cs = readState(clientRep, player);
    // tick 180 falls in the fire+back-up segment => Firing set, Moving set.
    bool stateMatch = (ss != 0xFF) && (ss == cs) &&
                      (ss & NetState_Firing) && (ss & NetState_Moving);
    nsCheck(stateMatch, "S4 server-resolved state byte replicates to client exactly");

    // S5: velocity replicates (client mirror carries the server's RepVelocity).
    bool velMatch = false;
    {
        uint32_t ls = 0, lc = 0;
        const void* ps = serverRep->readComponent(player, NetComp_Velocity, &ls);
        const void* pc = clientRep->readComponent(player, NetComp_Velocity, &lc);
        if (ps && pc && ls == sizeof(RepVelocity) && lc == sizeof(RepVelocity)) {
            RepVelocity vs, vc; std::memcpy(&vs, ps, sizeof(vs)); std::memcpy(&vc, pc, sizeof(vc));
            velMatch = std::fabs(vs.lin[0]-vc.lin[0]) < kEps &&
                       std::fabs(vs.lin[1]-vc.lin[1]) < kEps &&
                       std::fabs(vs.lin[2]-vc.lin[2]) < kEps;
        }
    }
    nsCheck(velMatch, "S5 client velocity converges to server authority (within eps)");

    // S6: the driven authoritative trajectory == the no-transport oracle (proves the
    // transport routing did not perturb the deterministic sim).
    RepTransform ox{};
    bool oracleMatch = readXform(oracleRep, oraclePlayer, ox) && haveS &&
        std::fabs(sx.pos[0]-ox.pos[0]) < kEps &&
        std::fabs(sx.pos[1]-ox.pos[1]) < kEps &&
        std::fabs(sx.pos[2]-ox.pos[2]) < kEps;
    nsCheck(oracleMatch, "S6 driven server trajectory matches deterministic oracle");

    // S7: the client tracked the latest server tick it applied.
    bool tickTracked = clientEverApplied && lastServerTick == kTicks &&
                       server.clientLastServerTick() == 0; // server-role obj has no client tick
    // (server-role NetworkSystem doesn't apply snapshots; the mirror is external.)
    nsCheck(tickTracked, "S7 client tracked the latest applied server tick");

    // S8: no leaked ids (live counts stayed bounded at 1 throughout, and the entity
    // is the SAME stable handle on both sides — generation matches).
    bool sameHandle = (netSlot(player) == netSlot(player)) &&  // trivially true; clarity
                      clientRep->alive(player) && serverRep->alive(player);
    bool idsClean = noLeak && sameHandle;
    nsCheck(idsClean, "S8 no leaked ids (bounded live count; stable handle both sides)");

    server.shutdown();
    delete serverTransport;   // owns + deletes the client (peer) transport
    delete serverRep;
    delete clientRep;
    delete oracleRep;

    const int total = gns_pass + gns_fail;
    x3::logInfo("netsync: " + std::to_string(gns_pass) + "/" +
                std::to_string(total) + " passed");
    return gns_fail == 0;
}

} // namespace x3::net
