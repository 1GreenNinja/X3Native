#pragma once
// INetworkSystem — the facade that wires transport + replication + the sim tick.
// Spec: specs/NETCODE-architecture.spec.md §8 / §0.
//
// This is the seam main.cpp will eventually drive: the server drains commands for
// a tick, hands them to the sim, then publishes snapshots; the client submits its
// per-frame command and pulls the latest applied server tick. In single-player the
// SAME object plays both roles over a loopback transport (§0 — always-on c/s).
//
// Phase 0 scope: the interface + a minimal in-process impl exercised by
// runNetworkSelfTest() / --test-net. NOT wired into the game loop yet (Phase 0b).
// No render/physics/lib types appear here (house style).

#include "engine/net/NetTypes.h"

namespace x3::net {

class INetTransport;
class IReplication;

// Deployment role (§1.3). Phase 0 uses LocalLoopback (SP = local server + local
// client in one process over loopback).
enum class NetRole : uint8_t { LocalLoopback, ListenServer, DedicatedServer, RemoteClient };

class INetworkSystem {
public:
    virtual ~INetworkSystem() = default;

    // Wire up the role + the transport + the replication store. SP passes a
    // loopback transport. Returns false if already initialised or on bad args.
    virtual bool init(NetRole role, INetTransport* transport, IReplication* replication) = 0;
    virtual void shutdown() = 0;

    // --- server side ---------------------------------------------------------
    // Drain the commands queued for `tick` into `out` (capacity maxOut); the
    // matching client id for each is written to outWho. Returns the count drained.
    // The caller applies each command through the existing Player::update path.
    virtual uint32_t serverDrainCommands(NetTick tick, NetCommand* out, uint32_t maxOut, ClientId* outWho) = 0;

    // Publish snapshots for `tick` to all connected clients (encodes via the
    // IReplication store, sends via the transport, flushes).
    virtual void serverPublish(NetTick tick) = 0;

    // --- client side ---------------------------------------------------------
    // Submit this frame's input command (sent to the server over the transport).
    virtual void clientSubmitCommand(const NetCommand& cmd) = 0;

    // The latest server tick whose snapshot the client has applied (0 if none yet).
    virtual NetTick clientLastServerTick() const = 0;

    // Pump the transport: deliver queued buffers, surface connect/disconnect,
    // apply any received snapshot (client) / enqueue received commands (server).
    // Called once per server tick AND once per client frame.
    virtual void poll() = 0;

    virtual bool isServer() const = 0;
    virtual bool isClient() const = 0;
};

// Factory (caller owns; delete to destroy). Mirrors createPhysicsWorld() etc.
INetworkSystem* createNetworkSystem();

// Headless acceptance test (--test-net): loopback transport round-trips a
// NetCommand byte-identically; NetEntityId generation rejects a stale handle after
// a slot recycle; the fixed-step accumulator advances deterministically. Logs each
// as "[net-test] PASS T# ..." / "[net-test] FAIL T# ...". Returns true iff all
// pass. Mirrors runPhysicsSelfTest() / runJobSystemSelfTest().
bool runNetworkSelfTest();

// Headless acceptance test (--test-netsync): Phase 0b client/server input->snapshot
// routing. Spins up a loopback client+server (the server owns the authoritative
// replication store; the client owns a separate render-mirror store), feeds a
// deterministic sequence of NetCommands, and runs the FULL loop each tick:
//   client samples input -> NetCommand -> send (client->server over transport)
//   server poll/drain -> apply command authoritatively -> advance fixed-step sim
//   server publishes a Snapshot of all net entities -> client receives + mirrors
// Asserts the client mirror converges to the server's authoritative state
// (positions within epsilon, ids correct, no leaked ids). Logs each property as
// "[netsync-test] PASS ..." / "[netsync-test] FAIL ..." and prints
// "netsync: X/Y passed". Returns true iff all pass.
bool runNetSyncSelfTest();

} // namespace x3::net
