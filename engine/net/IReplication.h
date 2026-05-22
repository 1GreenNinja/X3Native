#pragma once
// IReplication — encode/apply replicated entity state (server + client sides).
// Spec: specs/NETCODE-architecture.spec.md §4 / §8.
//
// The thin networked-entity registry (§4.2): NetEntityId -> record with a small
// set of replicated POD component blocks (NetTypes.h Rep*). Stable generation-
// tagged ids (§4.3) solve the index-recycling bug; per-component dirty tracking
// (§4.4) lets a delta send only what changed (Quake3/Tribes/Fiedler state-sync).
//
// Phase 0 scope: the registry + spawn/despawn/setComponent/readComponent + a
// minimal whole-state encode/apply that round-trips through a transport (the
// "loopback discipline" §1.4 — same encode->decode path as the wire). Baseline+
// delta vs per-client ACKed baselines (§4.4) is Phase 1; the interface is shaped
// for it now (encodeSnapshot/onClientAck/setClientInterest present, minimal impl).

#include "engine/net/NetTypes.h"

namespace x3::net {

class IReplication {
public:
    virtual ~IReplication() = default;

    // --- server side ---------------------------------------------------------
    // Create a replicated entity; returns its stable generation-tagged id. owner
    // 0 == server-owned (AI / world). The id is constant until despawn().
    virtual NetEntityId spawn(NetArchetype archetype, ClientId owner) = 0;

    // Retire an entity. Its slot is recycled with a BUMPED generation, so any
    // surviving handle (old generation) is detectably stale (alive() returns false).
    virtual void despawn(NetEntityId id) = 0;

    // True iff `id` refers to a live entity AND its generation matches the slot's
    // current generation (rejects stale handles after a recycle).
    virtual bool alive(NetEntityId id) const = 0;

    // Mark components dirty (componentMask = OR of (1<<NetComponent)). Phase 1 uses
    // this to send only changed blocks; Phase 0 records it for completeness.
    virtual void markDirty(NetEntityId id, uint16_t componentMask) = 0;

    // Write a component block (POD copy of `len` bytes). componentId is a
    // NetComponent. Sets the matching dirty bit. No-op on a stale/dead handle.
    virtual void setComponent(NetEntityId id, uint16_t componentId, const void* pod, uint32_t len) = 0;

    // Encode a snapshot for a client into outBytes (capacity maxLen); returns the
    // byte length written (0 if it would not fit). Phase 0: a full-state snapshot
    // of all live entities. Phase 1 makes it a per-client delta vs the last ACKed
    // baseline (job-parallel, §3.3) — same signature.
    virtual uint32_t encodeSnapshot(ClientId client, NetTick tick, void* outBytes, uint32_t maxLen) = 0;

    // Client ACKed a snapshot tick (advances that client's baseline). Phase 0 stub.
    virtual void onClientAck(ClientId client, NetTick acked) = 0;

    // Area-of-interest: the streaming-grid cells this client subscribes to (§5.1).
    // Phase 0 stub (single client sees everything); shaped for Phase 2 AoI.
    virtual void setClientInterest(ClientId client, const uint64_t* cellKeys, uint32_t count) = 0;

    // --- client side ---------------------------------------------------------
    // Decode a snapshot and patch the registry's component blocks. outServerTick
    // (if non-null) receives the snapshot's tick. Creates/updates/removes entities
    // to match the snapshot's set.
    virtual void applySnapshot(const void* bytes, uint32_t len, NetTick* outServerTick) = 0;

    // Read a component block back (e.g. to drive the render mirror). Returns a
    // pointer to the stored POD + its length, or nullptr if absent / stale handle.
    virtual const void* readComponent(NetEntityId id, uint16_t componentId, uint32_t* outLen) const = 0;

    // Number of currently-live entities (diagnostics / tests).
    virtual uint32_t liveCount() const = 0;
};

// Factory (caller owns; delete to destroy). Mirrors createPhysicsWorld() etc.
IReplication* createReplication();

} // namespace x3::net
