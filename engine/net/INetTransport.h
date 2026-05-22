#pragma once
// INetTransport — the single swappable byte-pipe seam (loopback OR udp).
// Spec: specs/NETCODE-architecture.spec.md §1.2 / §7 / §8.
//
// Clean interface: opaque ids + raw byte buffers ONLY. NO library types leak here
// (no GameNetworkingSockets / ENet / sockets) — the same discipline IPhysicsWorld
// uses to hide JPH::. Loopback is just ANOTHER INetTransport impl, not a fast-path
// or a #ifdef: that is what keeps single-player and multiplayer on ONE code path
// (§0 — always-on client/server).
//
// Phase 0 ships ONLY createLoopbackTransport(): two in-process queues moving
// command/snapshot byte buffers reliably, in-order, with ~0 latency. The UDP impl
// (createUdpTransport) is declared for symmetry but built in Phase 2.

#include "engine/net/NetTypes.h"

namespace x3::net {

class INetTransport {
public:
    virtual ~INetTransport() = default;

    // Bring the transport up. `bindOrConnect` is the address (loopback ignores it).
    // asServer: a server accepts connections; a client makes one. Returns false if
    // already started or on failure.
    virtual bool start(const char* bindOrConnect, bool asServer) = 0;
    virtual void shutdown() = 0;

    // Drain pending connection-lifecycle events (connect / disconnect) into `out`
    // (up to maxOut). Returns the number written. For loopback, a connect event is
    // surfaced once both ends have start()ed.
    virtual uint32_t pollEvents(NetEvent* out, uint32_t maxOut) = 0;

    // Send one byte buffer to `conn` on `channel`. The net system owns all
    // (de)serialization; the transport is byte-agnostic. Returns false if the
    // connection is unknown / closed. The buffer is copied; the caller keeps
    // ownership of `bytes`.
    virtual bool send(ConnectionId conn, const void* bytes, uint32_t len, NetChannel channel) = 0;

    // Receive ONE queued buffer from `conn` into `outBytes` (capacity maxLen).
    // Returns the byte length written, or 0 if nothing is queued. A buffer larger
    // than maxLen is dropped and 0 is returned (caller must size adequately).
    virtual uint32_t recv(ConnectionId conn, void* outBytes, uint32_t maxLen) = 0;

    // Push queued sends to the peer (called once per server tick / client frame).
    // Loopback delivers immediately; this is where a real transport flushes sockets.
    virtual void flush() = 0;

    // Dev knobs (loopback + udp): inject artificial latency/loss so prediction,
    // interpolation, and reconciliation can be developed/tested in single-player
    // before any real network exists (§1.4 net_loopback_simlag). Phase 0 loopback
    // accepts these but applies zero conditions (reliable, in-order, ~0 ms).
    virtual void setSimulatedConditions(float latencyMs, float lossPct) = 0;

    // Loopback ONLY: the matched peer endpoint. The server gets the client end and
    // vice-versa so a single in-process pair shares two SPSC queues. Returns
    // nullptr for transports without an in-process peer (udp). Convenience for the
    // self-test + the in-process SP wiring; not part of the wire contract.
    virtual INetTransport* loopbackPeer() { return nullptr; }
};

// Factory: in-process loopback transport (the default + the Phase-0 bring-up
// vehicle). The returned object owns its peer; delete it to tear both down.
INetTransport* createLoopbackTransport();

// Factory: real UDP transport (wraps GNS/ENet internally; NO lib types here).
// Declared for the §8 symmetry; Phase 2 implements it. Phase 0 returns nullptr.
INetTransport* createUdpTransport();

} // namespace x3::net
