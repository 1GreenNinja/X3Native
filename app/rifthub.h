#pragma once
// EFLZ Portal Hub — a tiny graybox HUB area with visible COSMETIC rift
// portals (one per known --world target) the player can walk between to
// signpost which slice they want to relaunch into. Game/slice code only;
// engine/ stays pure.
//
// CLEAN-ROOM, original work. Built ONLY from X3Native's OWN systems (Scene,
// trigger, mesh_prims) + the engine interfaces. NO RBDOOM / id Tech / Doom /
// Quake — or ANY other game-engine — source was forked, copied, or consulted.
//
// SCOPE (DRAFT): the portal is a DISCOVERY + SIGNPOSTING tool. Entering a
// portal's trigger volume LATCHES "<name> rift activated" + emits one HUD
// line ("Rift activated: <name>") + logs "[rifthub] entered <name> rift —
// relaunch with --world <name> to traverse". Runtime world-switching is a
// FUTURE task — this draft does NOT respawn the player into the linked
// world. The portals are arranged in a clockwise circle around the spawn
// point so the player can walk past all 8 by orbiting once.
//
// Authoring style mirrors act2_caves' Scene-prop helpers: every portal is a
// vertical round "tech-gate" ring built from N tangent box segments (two
// concentric layers — outer + inner — so the gate reads as a framed
// doorway), a small octagonal emissive floor-plate, and a thin emissive
// "shimmer disk" at the center of the ring as the portal's energy core. A
// wider AABB trigger sits underneath. No GLB asset needed.
//
// SHIMMER ANIMATION: Rifthub::tick(dt) runs each frame and pulses the ring's
// + the shimmer disk's emissive intensity via sin(time * freq + phase), with
// a per-portal phase offset so the gates ripple around the hub instead of
// pulsing in unison. See rifthub.cpp's tick() for the constants.

#include "scene.h"
#include "trigger.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace x3::game {

// Portal trigger ids — a fresh id range starting at 200 so the rifthub never
// collides with the Act-1 L1Trigger / SpireMid (10/30/40/50) / Act-2 host
// (80..82) / Act-2 caves (100..108) ranges. One id per known --world target.
enum class RifthubTrigger : uint32_t {
    Act2Caves = 200,
    Act2      = 201,
    Valley    = 202,
    Cliffs    = 203,
    Club      = 204,
    Destruct  = 205,
    Ragdoll   = 206,
    Terrain   = 207,
};
constexpr uint32_t kRifthubTrigBase  = 200;
constexpr uint32_t kRifthubTrigCount = 8;

// One placed portal in the hub. The portal is a vertical round tech-gate ring
// (two concentric layers of N tangent box segments) + an octagonal emissive
// floor-plate + a thin "shimmer disk" energy core, anchored at `worldPos`.
// `triggerId` is the matching RifthubTrigger; `worldName` is the --world flag
// the host should relaunch with to traverse this rift (NO runtime switch in
// this draft).
//
// The host calls Rifthub::tick(dt) each frame to drive the shimmer pulse —
// the per-portal entity-id ranges below let tick() poke emissive[3] on the
// ring segments + the shimmer disk in-place without re-issuing render calls.
struct RiftPortal {
    const char*    worldName  = "";       // --world flag (e.g. "act2caves")
    uint32_t       triggerId  = 0;        // matching RifthubTrigger id
    x3::phys::Vec3 worldPos{};            // portal center (XZ); Y = floor
    float          tint[3]    = {1,1,1};  // emissive color (portal-specific)
    bool           activated  = false;    // latched when the trigger fires
    // Entity-id ranges for the shimmer animation. Each portal owns a contiguous
    // span of ring-segment entities (outer ring then inner ring) and a single
    // shimmer-disk entity; tick() walks these to pulse emissive strength.
    uint32_t       ringEntFirst = 0;      // first scene entity id in the ring span
    uint32_t       ringEntCount = 0;      // number of ring-segment entities
    uint32_t       coreEnt      = 0;      // shimmer disk entity id (energy core, blue)
    uint32_t       coreInnerEnt = 0;      // brighter inner blue disk (core depth)
    // Rim emitter nodes — bright blue cubes on the outer ring that chase-pulse
    // in sequence (the wormhole generator "charging" the field). Contiguous span.
    uint32_t       nodeEntFirst = 0;      // first rim emitter-node entity id
    uint32_t       nodeEntCount = 0;      // number of rim emitter nodes
};

// The Portal-Hub area. Build once after the device + physics + a TriggerSystem
// are up; tick() each frame with the player's eye position so the host can
// see which portal (if any) is in the 5 m HUD-prompt range. No per-frame
// heap activity. The portals + their triggers + a flat ground plane are
// authored ONCE at build().
class Rifthub {
public:
    // Stand up the hub: a 40x40 m flat physics ground centered at origin, a
    // ring of 8 portals (one per --world target) at radius 14 m, and one
    // AABB trigger per portal (3 m square footprint, 4 m tall). `triggers`
    // is the host's shared TriggerSystem; the host forwards fired ids to
    // onTrigger(). Call once.
    void build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, TriggerSystem& triggers);

    // Per-frame shimmer animation. Pulses each portal's ring-segment emissive
    // strength + its inner shimmer-disk strength using sin(m_time*freq+phase),
    // with a small per-portal phase offset so the gates ripple around the hub
    // instead of pulsing in unison. The pulse is RANGE-mapped to {ring 1.5..4,
    // core 3..6.5} so the energy core always outshines the frame.
    // `scene` is the Scene the portals were authored into (build()'s scene).
    void tick(float dt, Scene& scene);

    // Free the Scene meshes/textures owned by the hub (the portal ring meshes,
    // the floor plates, the ground checker). Leaves physics ownership to the
    // caller (the host shuts down its own world).
    void shutdown(x3::rhi::IRenderDevice& device);

    // Dispatch a fired RifthubTrigger id the host forwards from its
    // TriggerSystem. Latches the matching portal's `activated` flag + logs
    // "[rifthub] entered <name> rift — relaunch with --world <name> to
    // traverse". Idempotent.
    void onTrigger(uint32_t triggerId);

    // Per-frame: emit a HUD prompt for any portal within `hudRadiusM` of the
    // eye, into `outPrompt` ("Rift: <name> — walk in to activate" / "Rift
    // activated: <name>"). Returns true iff a portal is in HUD range.
    // `hudRadiusM` defaults to 5 m (spec: "label visible in HUD as the player
    // approaches").
    bool hudPromptForEye(const x3::phys::Vec3& eye, std::string& outPrompt,
                         float hudRadiusM = 5.0f) const;

    // Where the player spawns into the hub: the center of the ring at a
    // player-feet Y so the capsule lands on the ground plane.
    x3::phys::Vec3 spawn() const { return m_spawn; }

    // Queries.
    bool built() const { return m_built; }
    uint32_t portalCount() const { return (uint32_t)m_portals.size(); }
    const RiftPortal& portal(uint32_t i) const { return m_portals[i]; }
    const std::vector<RiftPortal>& portals() const { return m_portals; }
    // True iff every portal's trigger fired at least once.
    bool allActivated() const;

private:
    bool                       m_built = false;
    float                      m_time = 0.0f;          // shimmer accumulator (sec)
    x3::phys::Vec3             m_spawn{};
    std::vector<RiftPortal>    m_portals;

    // Owned render resources (freed in shutdown()). The portal mesh vector
    // collects EVERY device-allocated mesh authored by build() — ring segments
    // (outer + inner), floor-plate wedges, and shimmer-disk core — so shutdown
    // can free them uniformly. The per-portal entity-id ranges in RiftPortal
    // index into the Scene, not into this vector.
    x3::rhi::MeshHandle        m_groundMesh;
    x3::rhi::TextureHandle     m_groundTex;
    std::vector<x3::rhi::MeshHandle> m_portalMeshes;
};

// Headless self-test (--test-rifthub). Builds the hub on a HeadlessDevice + Jolt
// world and asserts:
//   * the hub builds with exactly 8 portals (one per --world target);
//   * each portal owns a trigger volume + a contiguous span of ring/core/node
//     scene entities;
//   * entering a portal's trigger (via TriggerSystem::update with a point inside
//     the volume) latches that portal's `activated` flag + the HUD prompt flips
//     to "Rift activated: <name>" — and only AFTER every portal is entered does
//     allActivated() become true;
//   * tick(dt) advances the shimmer: a ring-segment + core emissive intensity
//     changes across two ticks at different times (the pulse is live);
//   * all 8 portal worldNames map to REAL --world targets the host accepts.
// Prints "rifthub: X/Y passed"; returns true iff all pass. No window/Vulkan.
bool runRifthubSelfTest();

} // namespace x3::game
