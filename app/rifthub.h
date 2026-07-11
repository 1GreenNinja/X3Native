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
//   * a SUBSTANTIAL, thick RING you walk through (a smooth procedural torus),
//     industrial weathered metal — the ring itself does NOT glow;
//   * CHEVRON locking clamps ringing the gate's outer face (one at 12
//     o'clock, the rest evenly spaced) — machined dark-metal clamp housings
//     with only a thin amber-lit SLIT strip emitting, the "powered gate" cue;
//   * a small octagonal emissive floor-plate (carries the per-destination
//     accent tint, since the ring itself is neutral metal);
//   * an EVENT-HORIZON MEMBRANE filling the ring opening: the fable-rock
//     ART TARGET (docs/RIFTHUB_ART_TARGET.md, palette LOCKED) — a DEEP BLUE
//     plasma storm, NOT a white-clipped disk. Three layers per portal:
//       [0] a dim parallax VISTA disk behind (another world glimpsed through),
//       [1] the PLASMA disk itself (procedural emissive texture on the PBR
//           route, deep-blue emissive with a HARD INTENSITY CAP so the blue
//           always reads — never tonemap-clips to white), slowly rotating,
//       [2] a bright-blue FRESNEL RIM ring hugging the ring's inner edge.
//     The membrane is a 3-STATE MACHINE (the MEMBRANE ANIMATION ARC from
//     docs/reference/PortalAnimated.mp4, mapped onto the existing gameplay
//     states — no new gameplay flags):
//       IDLE  (!activated)          — calm nebula: wispy filament texture,
//             sparse cross-disk tendrils, slow drift, vista faintly visible;
//       SURGE (kawoosh > 0)         — the activation flash: lightning arcs
//             re-target into a VORTEX RING whipping the rim circumference,
//             the whole membrane brightens toward its caps (bright BLUE-white,
//             never flat white), spark burst;
//       OPEN  (activated, settled)  — the throat: the plasma disk swaps to a
//             RADIAL-STREAMING texture (looking down the wormhole), runs a
//             brighter steady base, the vista dissolves into the energy, and
//             the tendrils become center->rim streamers.
//     Plus per-frame FX drawn by drawFx(): short-lived white-blue forked
//     LIGHTNING ARCS on the disk (idle chords / surge rim-orbits / open
//     radials) and drifting spark MOTES (additive particles).
// A wider AABB trigger sits underneath. No GLB asset needed.
//
// ANIMATION: Rifthub::tick(dt) runs each frame and (a) flickers the amber
// chevron cores with a slow per-chevron pulse, (b) pulses the blue core
// hot-spot, (c) breathes + slowly ROTATES the plasma membrane disk (the
// procedural filament texture sweeping around reads as the storm churning),
// shimmer on the fresnel rim, all emissive writes CLAMPED to the per-layer
// caps (the blown-white v1 fix), and (d) advances the lightning-arc spawner
// + integrates the mote particle pool. The metal ring is static. The host
// calls drawFx() between beginFrame/endFrame (after Scene::render) to draw
// the arcs + submit the motes. See rifthub.cpp's tick()/drawFx() constants.

#include "scene.h"
#include "surface_library.h"
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
    // CHEVRON locking clamps — machined dark-metal housings (body + jaw
    // flanges + face cap, all PBR-textured, never emissive) seated into the
    // ring; the animated span is the thin amber-lit SLIT strip per clamp that
    // flickers (the "powered gate" cue, capped ~2.0). Contiguous span,
    // chevron 0 at 12 o'clock; tick() pulses each with a per-chevron phase.
    uint32_t       chevronEntFirst = 0;   // first amber SLIT entity id
    uint32_t       chevronEntCount = 0;   // number of slit cores (one per clamp)
    // Segmented amber RATCHET TRACK on the ring's inner-facing front edge
    // (confirmed by PortalAnimated.mp4): dim amber when dormant, a bright
    // chase sweeps the circumference during the ACTIVATION SURGE, and the
    // track holds a steady powered glow once OPEN. Contiguous span.
    uint32_t       trackEntFirst = 0;
    uint32_t       trackEntCount = 0;
    // ORANGE conduit run (gate -> floor -> skirt; the locked palette's warm
    // accent): contiguous span of pipe segments whose emissive tick() phases
    // along the run so the power visibly FLOWS toward the gate.
    uint32_t       conduitEntFirst = 0;
    uint32_t       conduitEntCount = 0;
    // Event-horizon membrane — the visible portal SURFACE (membrane v2, the
    // fable-rock art pass): a contiguous 3-entity span in authoring order
    //   [0] VISTA disk (dim parallax backdrop — the glimpsed other world),
    //   [1] PLASMA disk (procedural filament emissive texture, deep blue,
    //       capped intensity, slow rotation driven by tick()),
    //   [2] FRESNEL RIM ring (bright blue inner-edge ring, shimmer).
    uint32_t       membraneEntFirst = 0;  // first membrane entity id (vista)
    uint32_t       membraneEntCount = 0;  // == 3 (vista + plasma + rim)
    // Portal-local basis (unit, XZ plane): outward = radial from hub center
    // through the gate (the ring's hole axis), right = outward x up. Cached at
    // build() so tick()/drawFx() can rebuild the rotating membrane transform +
    // place lightning arcs without re-deriving from worldPos.
    float          rightX = 1, rightZ = 0;
    float          outX   = 0, outZ   = 1;
    // ---- Membrane lightning arcs (white-blue tendrils crawling the disk) ----
    // A tiny per-portal pool. Each live arc is a chord across the membrane disk
    // (polar endpoints in the ring plane) drawn per-frame as a jagged forked
    // polyline of thin emissive beams (re-jittered every frame -> crackle).
    struct MembraneArc {
        float    life    = 0.0f;   // remaining seconds (<= 0 == free slot)
        float    maxLife = 0.3f;
        float    a0 = 0, r0 = 0;   // endpoint A (angle rad, radius m) in ring plane
        float    a1 = 0, r1 = 0;   // endpoint B
        uint32_t seed = 1;         // per-arc jitter seed (re-mixed per frame)
        bool     fork = false;     // draw a short branch off an interior vertex
        uint8_t  mode = 0;         // 0 idle chord / 1 surge rim-orbit / 2 open radial
    };
    static constexpr uint32_t kMaxArcs = 3;
    MembraneArc    arcs[kMaxArcs];
    float          arcCooldown = 0.0f;    // seconds until the next arc may spawn
    float          moteAccum   = 0.0f;    // fractional mote-spawn accumulator
    float          vistaEm     = 0.0f;    // current vista emissive (fades on OPEN)
    bool           throatOn    = false;   // plasma disk swapped to the throat texture
    float          spinAngle   = 0.0f;    // plasma disk rotation (integrated — the
                                          // OPEN state spins faster without snapping)
    // KAWOOSH one-shot: seconds remaining in the activation "unstable vortex"
    // surge (0 = idle). onTrigger() sets it to the kawoosh duration; tick()
    // decays it and rides a bright bulge-out emissive envelope on the membrane
    // (the puddle-splash), then settles back to the steady event-horizon ripple.
    float          kawoosh = 0.0f;
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

    // Per-frame animation. Flickers each portal's amber chevrons, pulses the
    // blue core hot-spots, breathes + slowly rotates the plasma membrane disk,
    // shimmers the fresnel rim (EVERY membrane emissive write clamped to its
    // cap — deep blue must always read, never clip to white), advances the
    // lightning-arc spawner and integrates the spark-mote pool. The metal ring
    // is static. All emissive pokes are in-place on the authored entities — no
    // per-frame heap. `scene` is the Scene the portals were authored into.
    void tick(float dt, Scene& scene);

    // Per-frame membrane FX draw: the live lightning arcs (jagged forked
    // white-blue tendrils re-jittered each frame so they crackle) + the spark
    // motes (one additive submitParticles batch). Call between beginFrame /
    // endFrame, AFTER Scene::render. No-op before build() / when nothing is
    // live. Not const: the per-frame jitter advances the FX rng.
    void drawFx(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame);

    // Free the Scene meshes/textures owned by the hub (the portal ring meshes,
    // the floor plates, the hall shell, the surface-library sets). Leaves
    // physics ownership to the caller (the host shuts down its own world).
    void shutdown(x3::rhi::IRenderDevice& device);

    // Apply the RIFTHUB HALL atmosphere to the device (host opt-in, called
    // once after build()): industrial fog/haze, teal-shadow grade, cool
    // ambient, interior IBL probe (wet-floor reflections), and an exposure
    // bias that keeps auto-exposure from washing the dark hall pale. Values
    // live with the art (rifthub.cpp) so the light balance is one knob.
    void applyAtmosphere(x3::rhi::IRenderDevice& device) const;

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
    // FX observability (self-test + diagnostics): live lightning arcs on one
    // portal / live spark motes across the whole hub.
    uint32_t liveArcCount(uint32_t portalIdx) const;
    uint32_t liveMoteCount() const;

private:
    bool                       m_built = false;
    float                      m_time = 0.0f;          // shimmer accumulator (sec)
    x3::phys::Vec3             m_spawn{};
    std::vector<RiftPortal>    m_portals;

    // Owned render resources (freed in shutdown()). The portal mesh vector
    // collects every PER-ENTITY device mesh authored by build() (ring torus,
    // chevron prisms, floor-plate wedges, core disks) so shutdown can free
    // them uniformly. SHARED meshes/textures (one handle referenced by many
    // entities — the membrane disks, the rim ring, the FX beam box, the
    // plasma/vista/mr textures) are tracked separately and freed ONCE.
    x3::rhi::MeshHandle        m_groundMesh;
    x3::rhi::TextureHandle     m_groundTex;
    std::vector<x3::rhi::MeshHandle> m_portalMeshes;
    // Shared membrane v2 resources (created once, used by all 8 portals).
    x3::rhi::MeshHandle        m_diskMesh;    // two-sided membrane disk fan
    x3::rhi::MeshHandle        m_rimMesh;     // thin fresnel rim torus
    x3::rhi::MeshHandle        m_fxBeamMesh;  // unit box the arc beams stretch
    x3::rhi::TextureHandle     m_plasmaTex;   // IDLE nebula/filament emissive map
    x3::rhi::TextureHandle     m_throatTex;   // OPEN radial-streaming throat map
    x3::rhi::TextureHandle     m_vistaTex;    // parallax backdrop (other world)
    x3::rhi::TextureHandle     m_mrFlat;      // 1x1 rough/dielectric MR (PBR route)
    x3::rhi::TextureHandle     m_mrWet;       // 1x1 glossy MR (wet concrete floor)
    x3::rhi::TextureHandle     m_holoTexA;    // teal holo data-screen texture (variant A)
    x3::rhi::TextureHandle     m_holoTexB;    // teal holo data-screen texture (variant B)
    // Per-portal blue core lights (1:1 with m_portals); intensity pulsed in tick().
    std::vector<x3::rhi::PointLight> m_lights;
    // Curated PBR surface sets (ring plates / housings / cradle / hall). The
    // library owns the loaded textures; destroyAll() in shutdown(). On a box
    // with no assets present a set loads !ok and authoring falls back to the
    // flat-tinted look (the self-test path never breaks).
    SurfaceLibrary m_surf;

    // ---- Spark-mote pool (membrane embers). CPU-integrated fixed ring, no
    // per-frame heap; drawFx() streams the live ones as one additive batch. ----
    struct Mote {
        float px = 0, py = 0, pz = 0;
        float vx = 0, vy = 0, vz = 0;
        float life = 0.0f;      // remaining seconds (<= 0 == free)
        float maxLife = 1.0f;
        float size = 0.02f;     // billboard half-extent (m)
        float r = 0.6f, g = 0.8f, b = 1.0f;
    };
    static constexpr int kMaxMotes = 512;
    Mote     m_motes[kMaxMotes];
    int      m_nextMote = 0;    // round-robin recycle cursor
    uint32_t m_rng = 0x9E3779B9u;   // xorshift state (arc + mote jitter)
    // Per-frame submit scratch (member so drawFx does no heap alloc).
    mutable x3::rhi::IRenderDevice::ParticleInstance m_moteScratch[kMaxMotes];

    // Spawn helpers (rifthub.cpp file-local logic uses these members).
    float frand();     // [0,1)
    float frandSym();  // [-1,1)
    void  spawnMote(const Mote& m);
    // mode: 0 idle chord / 1 surge rim-orbit (the vortex ring) / 2 open radial.
    void  spawnArc(RiftPortal& p, int mode);
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
//   * tick(dt) advances the animation: a chevron + core + plasma-membrane
//     emissive intensity changes across two ticks at different times (the
//     pulse is live) AND the plasma disk's transform ROTATES (the storm churns);
//   * the EMISSIVE CAP LAW (the blown-white v1 fix): across a kawoosh surge +
//     many ticks, every membrane-layer emissive stays <= its cap (deep blue
//     always reads — the surge peaks bright blue, never flat white);
//   * the membrane FX are alive: lightning arcs spawn on a ticking portal and
//     spark motes exist after a kawoosh;
//   * all 8 portal worldNames map to REAL --world targets the host accepts.
// Prints "rifthub: X/Y passed"; returns true iff all pass. No window/Vulkan.
bool runRifthubSelfTest();

} // namespace x3::game
