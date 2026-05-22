#pragma once
// Netcode shared POD types + opaque handles — Subsystem N (Phase 0 foundation).
// Spec: specs/NETCODE-architecture.spec.md §4.3 / §8.
//
// Clean-room: built from X3Native's OWN interfaces (mirrors the BodyId / MeshHandle
// opaque-handle style) + public networking references (Fiedler "Gaffer on Games",
// Valve Source Multiplayer Networking, Overwatch GDC 2017, Quake3/Tribes public
// articles). NO third-party library types appear here (same discipline as
// IPhysicsWorld hiding JPH:: and IRenderDevice hiding Vulkan).
//
// Phase 0 scope: these types COMPILE and are unit-testable (runNetworkSelfTest /
// --test-net). They are additive and NOT wired into the game loop yet (the full
// client/server input->snapshot routing is Phase 0b, a coordinated main.cpp split).

#include <cstdint>

namespace x3::net {

// ---------------------------------------------------------------------------
// Generation-tagged entity handle (fixes the scene index-recycling bug, §4.1).
//
// A handle packs {slot index, generation} into one 32-bit id. Recycling a slot
// bumps its generation, so a stale handle (carrying the OLD generation) no longer
// matches the slot's CURRENT generation and is rejected. This gives netcode a
// STABLE network identity AND fixes the real latent defect where TerrainStreamer
// recycles scene slots and an old index could alias a different entity.
//
// Layout: low kSlotBits = slot index, high (32 - kSlotBits) = generation.
//   id == 0  is reserved as the INVALID handle (matches BodyId/ClientId 0==none).
// A freshly allocated slot starts at generation 1 so a valid handle is never 0.
// ---------------------------------------------------------------------------
struct NetEntityId {
    uint32_t id = 0;
    bool valid() const { return id != 0; }
    bool operator==(const NetEntityId& o) const { return id == o.id; }
    bool operator!=(const NetEntityId& o) const { return id != o.id; }
};

// Bit budget for the slot index. 20 bits => up to ~1,048,575 live slots, leaving
// 12 generation bits (wraps after 4096 recycles of the SAME slot — ample for the
// detection window; a wrap is astronomically unlikely to alias a held stale ref).
constexpr uint32_t kSlotBits      = 20;
constexpr uint32_t kSlotMask      = (1u << kSlotBits) - 1u;          // low 20 bits
constexpr uint32_t kGenShift      = kSlotBits;
constexpr uint32_t kGenMask       = (1u << (32u - kSlotBits)) - 1u;  // 12-bit gen
constexpr uint32_t kMaxNetSlots   = kSlotMask;                       // slot 0..mask
constexpr uint32_t kMaxGeneration = kGenMask;

// Compose a handle from a slot index + generation. Generation is masked to its
// field width (callers should already keep it in range; this is belt-and-braces).
// Returns the INVALID handle (id 0) only if both slot and generation are 0, which
// the registry never produces (generations start at 1).
inline NetEntityId makeNetEntityId(uint32_t slot, uint32_t generation) {
    NetEntityId h;
    h.id = ((generation & kGenMask) << kGenShift) | (slot & kSlotMask);
    return h;
}
inline uint32_t netSlot(NetEntityId h)       { return h.id & kSlotMask; }
inline uint32_t netGeneration(NetEntityId h) { return (h.id >> kGenShift) & kGenMask; }

// ---------------------------------------------------------------------------
// Other opaque ids + scalar aliases (§4.3 / §8).
// ---------------------------------------------------------------------------
struct ClientId     { uint32_t id = 0; bool valid() const { return id != 0; } };  // 0 == local/server
struct ConnectionId { uint32_t id = 0; bool valid() const { return id != 0; } };  // transport-level

using NetTick      = uint32_t;   // server simulation tick number (monotone)
using NetArchetype = uint16_t;   // Player / Monster / Door / Projectile / Pickup / ...

// Transport delivery channels (§7.3). Snapshots are latest-wins (unreliable);
// commands + discrete events are reliable-ordered.
enum class NetChannel : uint8_t { UnreliableSequenced = 0, ReliableOrdered = 1 };

// Connection lifecycle events surfaced by INetTransport::pollEvents (§8).
enum class NetEventType : uint8_t { None = 0, Connected = 1, Disconnected = 2 };
struct NetEvent {
    NetEventType type = NetEventType::None;
    ConnectionId conn;
};

// ---------------------------------------------------------------------------
// NetCommand — the timestamped input intent a client sends to drive the sim.
// This is the WIRE FORM of app/player.h PlayerInput (§8 note): the existing
// Player::update(PlayerInput, dt, physics) becomes the server-side application of
// a decoded NetCommand AND the client-side prediction step (one function, two
// callers). Absolute look angles use the CONVENTIONS.md forward basis.
//
// POD with a fixed layout so it round-trips byte-identically through any transport
// (a hard requirement the --test-net round-trip asserts).
// ---------------------------------------------------------------------------
struct NetCommand {
    NetTick  tick      = 0;     // sim tick this command is FOR
    float    moveFwd   = 0.0f;  // -1..1  (W=+1, S=-1) along facing
    float    moveStrafe= 0.0f;  // -1..1  (D=+1, A=-1) along right
    float    yaw       = 0.0f;  // absolute look yaw   (radians; CONVENTIONS.md §3)
    float    pitch     = 0.0f;  // absolute look pitch (radians; (-pi/2,+pi/2))
    uint32_t buttons   = 0;     // bitfield (edges resolved server-side); see NetButton
};

// Button bits packed into NetCommand::buttons. Edges (rising/falling) are resolved
// server-side from the held bitfield, so the wire only carries held state.
enum NetButton : uint32_t {
    NetBtn_Jump   = 1u << 0,
    NetBtn_Sprint = 1u << 1,
    NetBtn_Use    = 1u << 2,
    NetBtn_Fire   = 1u << 3,
    NetBtn_Melee  = 1u << 4,
};

// ---------------------------------------------------------------------------
// Replicated component blocks (POD, fixed layout, the unit of delta-encoding §4.3).
// Archetypes declare which blocks they carry; per-component dirty tracking lets a
// delta send only what changed. Positions are region-relative (§3.4) once rebasing
// lands; for Phase 0 they are absolute (single-server, one machine).
// ---------------------------------------------------------------------------
struct RepTransform { float pos[3] = {0,0,0}; float rotQuat[4] = {0,0,0,1}; }; // (x,y,z,w)
struct RepVelocity  { float lin[3] = {0,0,0}; float ang[3] = {0,0,0}; };
struct RepHealth    { int16_t hp = 0; int16_t maxHp = 0; uint8_t flags = 0; };
struct RepDoor      { uint8_t state = 0; float openFrac = 0.0f; };

// Stable component ids (the dirty-mask bit index AND the setComponent/readComponent
// selector). Kept small + explicit so the wire layout is frozen.
enum NetComponent : uint16_t {
    NetComp_Transform = 0,
    NetComp_Velocity  = 1,
    NetComp_Health    = 2,
    NetComp_Door      = 3,
    NetComp_Count     = 4,
};

} // namespace x3::net
