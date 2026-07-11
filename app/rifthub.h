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
// Stargate-INSPIRED gateway (original procedural design, not a copy) —
//   * a SUBSTANTIAL, thick grey-stone RING you walk through, built from a
//     single circle of N deep tangent box segments with a beefy squarish
//     cross-section (real radial thickness + depth). The ring is stone, it
//     does NOT glow;
//   * CHEVRON-like AMBER locking clamps ringing the gate's outer face (one at
//     12 o'clock, the rest evenly spaced) — chunky triangular prisms that sit
//     proud of the ring surface and glow amber, the "powered gate" cue;
//   * a small octagonal emissive floor-plate (carries the per-destination
//     accent tint, since the ring itself is neutral stone);
//   * an EVENT-HORIZON MEMBRANE filling the ring opening as the portal's
//     energy surface: a vertical pool of blue-white energy built from
//     concentric bands of thin tangent box segments, brightest at the center
//     and fading to deep blue at the rim (the outermost band bleeds a fraction
//     of the destination tint as a second subtle signposting cue).
// A wider AABB trigger sits underneath. No GLB asset needed.
//
// ANIMATION: Rifthub::tick(dt) runs each frame and (a) flickers the amber
// chevrons with a slow sin(time*freq + phase) per-chevron pulse (a powered
// gate breathing), (b) pulses the blue core hot-spot, and (c) drives the
// membrane's LIQUID RIPPLE: each concentric band's emissive is phased by
// sin(time*w - radius*k) so bright crests travel outward from the center like
// rings on a pond, plus a slow angular swirl term per segment. The grey-stone
// ring is static (stone doesn't pulse). See rifthub.cpp's tick() for constants.

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

// One placed portal in the hub. The portal is a thick grey-stone Stargate-style
// RING (a single circle of N deep tangent box segments) + amber CHEVRON locking
// clamps on its outer face + an octagonal emissive floor-plate + a blue energy
// core hot-spot + the event-horizon membrane pool, anchored at `worldPos`.
// `triggerId` is the matching RifthubTrigger; `worldName` is the --world flag
// the host should relaunch with to traverse this rift (NO runtime switch in
// this draft).
//
// The host calls Rifthub::tick(dt) each frame to drive the animation — the
// per-portal entity-id ranges below let tick() poke emissive[3] on the amber
// chevrons, the blue core, and the membrane bands in-place without re-issuing
// render calls. The stone ring is static (authored once, never animated).
struct RiftPortal {
    const char*    worldName  = "";       // --world flag (e.g. "act2caves")
    uint32_t       triggerId  = 0;        // matching RifthubTrigger id
    x3::phys::Vec3 worldPos{};            // portal center (XZ); Y = floor
    float          tint[3]    = {1,1,1};  // per-destination accent color
    bool           activated  = false;    // latched when the trigger fires
    // Entity-id ranges. The stone ring is a contiguous span (static, but tracked
    // so shutdown/self-test can reason about it); the amber chevrons, blue core
    // disks, and membrane bands are the animated spans tick() pokes.
    uint32_t       ringEntFirst = 0;      // first scene entity id in the ring span
    uint32_t       ringEntCount = 0;      // number of stone ring-segment entities
    uint32_t       coreEnt      = 0;      // core hot-spot disk entity id (blue)
    uint32_t       coreInnerEnt = 0;      // brighter inner blue disk (core depth)
    // Amber CHEVRON locking clamps — chunky triangular prisms proud of the ring's
    // outer face that flicker amber (the "powered gate" cue). Contiguous span,
    // chevron 0 at 12 o'clock; tick() pulses each with a per-chevron phase.
    uint32_t       chevronEntFirst = 0;   // first chevron entity id
    uint32_t       chevronEntCount = 0;   // number of amber chevrons
    // Event-horizon membrane — the visible portal SURFACE: concentric bands of
    // thin emissive segments filling the ring opening (a vertical blue-white
    // energy pool). Contiguous span, authored band 0 (innermost) outward,
    // kMembraneSegs entities per band; tick() phases each band's emissive with
    // sin(time*w - bandRadius*k) so ripples propagate center -> rim.
    uint32_t       membraneEntFirst = 0;  // first membrane-band entity id
    uint32_t       membraneEntCount = 0;  // bands * segments entities
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

    // Per-frame animation. Flickers each portal's amber chevrons with a slow
    // sin(m_time*freq + per-chevron phase) pulse (a powered gate breathing),
    // pulses the blue core hot-spot disks, and drives the event-horizon
    // membrane's liquid ripple: each concentric band's emissive follows
    // sin(m_time*w - bandRadius*k) (crests travel center -> rim) plus a slow
    // per-segment swirl. The grey-stone ring is static. All emissive pokes are
    // in-place on the authored entities — no per-frame heap.
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

    // Per-portal blue CORE light set — one cool-blue point light at each ring
    // center that CASTS onto the grey stone gate + floor, pulsing with the core
    // hum (so the stone reads as lit by its own event horizon; step-4 blue-light
    // pass). Rebuilt each tick() with the pulsed intensity; the host forwards
    // this to device.setPointLights each frame. Empty until build().
    const std::vector<x3::rhi::PointLight>& pointLights() const { return m_lights; }

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
    // collects EVERY device-allocated mesh authored by build() — stone ring
    // segments, amber chevron prisms, floor-plate wedges, core disks, and
    // membrane bands — so shutdown can free them uniformly. The per-portal
    // entity-id ranges in RiftPortal index into the Scene, not into this vector.
    x3::rhi::MeshHandle        m_groundMesh;
    x3::rhi::TextureHandle     m_groundTex;
    std::vector<x3::rhi::MeshHandle> m_portalMeshes;
    // Per-portal blue core lights (1:1 with m_portals); intensity pulsed in tick().
    std::vector<x3::rhi::PointLight> m_lights;
};

// Headless self-test (--test-rifthub). Builds the hub on a HeadlessDevice + Jolt
// world and asserts:
//   * the hub builds with exactly 8 portals (one per --world target);
//   * each portal owns a trigger volume + a contiguous span of ring/core/
//     chevron/membrane scene entities;
//   * entering a portal's trigger (via TriggerSystem::update with a point inside
//     the volume) latches that portal's `activated` flag + the HUD prompt flips
//     to "Rift activated: <name>" — and only AFTER every portal is entered does
//     allActivated() become true;
//   * tick(dt) advances the animation: a chevron + core + membrane emissive
//     intensity changes across two ticks at different times (the pulse is
//     live), and two membrane bands at different radii sit at DIFFERENT
//     emissive levels at the same instant (the ripple really is radial);
//   * all 8 portal worldNames map to REAL --world targets the host accepts.
// Prints "rifthub: X/Y passed"; returns true iff all pass. No window/Vulkan.
bool runRifthubSelfTest();

} // namespace x3::game
