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
//     plasma storm, NOT a white-clipped disk. MEMBRANE v3 — TWO layers:
//       [0] the PLASMA disk (the reference-video FLIPBOOK by default; the
//           procedural filament map is the no-atlas fallback), deep-blue
//           emissive with a HARD INTENSITY CAP so the blue always reads —
//           never tonemap-clips to white — slowly rotating. The disk mesh is
//           DOUBLE-WOUND: one entity, the portal reads from BOTH sides.
//       [1] a bright-blue FRESNEL RIM ring hugging the ring's inner edge.
//     (v2's parallax VISTA disk is DELETED — an opaque disk parked outward of
//     an opaque plasma disk of equal radius is invisible from the hub side and
//     BLACKS OUT the portal from the far side. A real see-through vista needs
//     a render-to-texture portal view.)
//     The membrane is a 3-STATE MACHINE (the MEMBRANE ANIMATION ARC from
//     docs/reference/PortalAnimated.mp4, mapped onto the existing gameplay
//     states — no new gameplay flags):
//     ROUND 6 — EVERY STATE IS THE OWNER'S REAL FOOTAGE. The states no longer
//     mix baked video with hand-coded math textures (the owner: "the swirling
//     one looks fake"); each plays its OWN flipbook baked from its own span of
//     PortalAnimated.mp4, through ONE shared playback path:
//       IDLE  (!activated)          — video t 0.0-3.2: calm nebula, looped (its
//             OWN lightning filaments carry the detail — no procedural bolts);
//       SURGE (kawoosh > 0)         — video t 6.4-8.3: the vortex ring collapsing
//             into the throat, played ONCE across the 1.6 s kawoosh (a real
//             escalation), the membrane riding its capped brightness envelope,
//             the ratchet track chasing, spark burst;
//       OPEN  (activated, settled)  — video t 8.4-9.95: the settled throat,
//             radial plasma streaming down the wormhole, looped; picks up exactly
//             where the surge span ended. Runs a brighter, faster-spinning base.
//     The procedural nebula/throat maps survive ONLY as the missing-atlas fallback.
//     Plus per-frame FX drawn by drawFx(): drifting spark MOTES (additive
//     particles) + the hall's light shafts.
// A wider AABB trigger sits underneath. No GLB asset needed.
//
// ANIMATION: Rifthub::tick(dt) runs each frame and (a) flickers the amber
// chevron cores with a slow per-chevron pulse, (b) drives the per-gate blue
// point light, (c) breathes + slowly ROTATES the plasma membrane disk (the
// procedural filament texture sweeping around reads as the storm churning),
// shimmer on the fresnel rim, all emissive writes CLAMPED to the per-layer
// caps (the blown-white v1 fix), and (d) advances the lightning-arc spawner
// + integrates the mote particle pool. The metal ring is static. The host
// calls drawFx() between beginFrame/endFrame (after Scene::render) to draw
// the arcs + submit the motes. See rifthub.cpp's tick()/drawFx() constants.

#include "holo_terminal.h"
#include "rift_console.h"
#include "scene.h"
#include "surface_library.h"
#include "trigger.h"
#include "ui.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/asset/IAssetSource.h"
#include "engine/asset/IModelLoader.h"

#include <cstdint>
#include <memory>
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
// clamps on its outer face + an octagonal emissive floor-plate + the
// event-horizon membrane, anchored at `worldPos`.
// `triggerId` is the matching RifthubTrigger; `worldName` is the --world flag
// the host should relaunch with to traverse this rift (NO runtime switch in
// this draft).
//
// The host calls Rifthub::tick(dt) each frame to drive the animation — the
// per-portal entity-id ranges below let tick() poke emissive[3] on the amber
// chevrons and the membrane layers in-place without re-issuing
// render calls. The stone ring is static (authored once, never animated).
struct RiftPortal {
    const char*    worldName  = "";       // --world flag (e.g. "act2caves")
    uint32_t       triggerId  = 0;        // matching RifthubTrigger id
    x3::phys::Vec3 worldPos{};            // portal center (XZ); Y = floor
    float          tint[3]    = {1,1,1};  // per-destination accent color
    bool           activated  = false;    // latched when the trigger fires
    // Entity-id ranges. The stone ring is a contiguous span (static, but tracked
    // so shutdown/self-test can reason about it); the amber chevrons and the
    // membrane layers are the animated spans tick() pokes.
    uint32_t       ringEntFirst = 0;      // first scene entity id in the ring span
    uint32_t       ringEntCount = 0;      // number of stone ring-segment entities
    // (ROUND 6: the procedural CORE DISKS are DELETED. Two bright blue-white
    //  disks were drawn ON TOP of the membrane's center — the owner's "why the
    //  dot in the middle?". The footage has whatever center it needs. The blue
    //  POINT LIGHT the gate casts into the bay stays: that is lighting, not a
    //  fake sprite.)
    // (CHEVRONS ARE GONE — round 7 addendum 2, "No chevrons needed". The nine
    //  amber clamp slits and their entity span are deleted; the tube carries its
    //  detail as CUT FEATURES + forged normal maps instead. What survives is the
    //  recessed indicator TRACK below, which is in the owner's reference footage.)
    // Segmented amber RATCHET TRACK on the ring's inner-facing front edge
    // (confirmed by PortalAnimated.mp4): dim amber when dormant, a bright
    // chase sweeps the circumference during the ACTIVATION SURGE, and the
    // track holds a steady powered glow once OPEN. Contiguous span.
    uint32_t       trackEntFirst = 0;
    uint32_t       trackEntCount = 0;
    // Conduit run (gate -> floor -> skirt; the locked palette's warm accent).
    // The pipe BODIES are static gunmetal PBR geometry; this span is the thin
    // amber CORE LINES riding them, whose emissive tick() phases along the
    // run so the power visibly FLOWS toward the gate.
    uint32_t       conduitEntFirst = 0;
    uint32_t       conduitEntCount = 0;
    // Event-horizon membrane — the visible portal SURFACE (membrane v3): a
    // contiguous 2-entity span in authoring order
    //   [0] PLASMA disk (the flipbook / throat emissive texture, deep blue,
    //       capped intensity, slow rotation driven by tick()) — DOUBLE-WOUND,
    //       so it is the portal from BOTH sides,
    //   [1] FRESNEL RIM ring (bright blue inner-edge ring, shimmer).
    // The v2 VISTA disk is GONE (round 5): an opaque disk parked OUTWARD of an
    // opaque plasma disk of the same radius contributed nothing from the hub
    // side and BLACKED OUT the portal from the far side (the owner's "activated
    // portal goes black" — he had walked through the gate). See rifthub.cpp.
    uint32_t       membraneEntFirst = 0;  // first membrane entity id (plasma)
    uint32_t       membraneEntCount = 0;  // == 2 (plasma + rim)
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
    bool           throatOn    = false;   // OPEN state live: the plasma disk plays the
                                          // OPEN flipbook (procedural throat = fallback)
    float          spinAngle   = 0.0f;    // plasma disk rotation (integrated — the
                                          // OPEN state spins faster without snapping)
    // KAWOOSH one-shot: seconds remaining in the activation "unstable vortex"
    // surge (0 = idle). onTrigger() sets it to the kawoosh duration; tick()
    // decays it and rides a bright bulge-out emissive envelope on the membrane
    // (the puddle-splash), then settles back to the steady event-horizon ripple.
    float          kawoosh = 0.0f;

    // ===== ROUND 8 — THE OPERATOR PANEL + THE CONSOLE =========================
    // The gate GLB now ships an LCD + button cluster + LED readout strips set INTO
    // a recessed bay in the tube's face (tools/build_rifthub_gate.py). Those arrive
    // as two extra material-group entities, and tick() drives their emissive off
    // the console's live INSTABILITY — so the tube's own panel is part of the
    // telegraph: it goes green -> amber -> red under the player's hand.
    uint32_t       panelScreenEnt = 0;    // gate_screen group (the LCD) — 0 = absent
    uint32_t       panelLedEnt    = 0;    // gate_led group (LEDs + lit button caps)
    bool           hasPanel       = false;

    // The console's tunable parameters + its last engaged outcome (rift_console.h).
    RiftConsole    console;
    // The destination this rift currently points at. Starts as worldName; a NOMINAL
    // engage with a valid typed TARGET RE-POINTS it (a real, visible consequence:
    // the holoterminal and the identity trim both change).
    std::string    destination;

    // ---- Consequences (persistent where it makes sense) ----------------------
    // IMPLOSION: the membrane inverts and sucks INWARD, then the gate is DEAD.
    // `implode` counts down the collapse; `dead` is FOREVER — a collapsed gate
    // stays collapsed (the brief: "that gate goes DARK/dead afterwards").
    float          implode = 0.0f;
    bool           dead    = false;
    // NOMINAL aperture boost: a stable rift dialled wide really is wider/brighter
    // (the membrane disk is scaled by this).
    float          aperture = 1.0f;
    // Live emissive scale for the membrane while a console is being dialled — the
    // rift snarls CONTINUOUSLY as instability climbs, before anything blows.
    float          snarl = 0.0f;
};

// The Portal-Hub area. Build once after the device + physics + a TriggerSystem
// are up; tick() each frame with the player's eye position so the host can
// see which portal (if any) is in the 5 m HUD-prompt range. No per-frame
// heap activity. The portals + their triggers + a flat ground plane are
// authored ONCE at build().
class Rifthub {
public:
    // ===== W-RIFT (ONE WORLD): the hub is a REGION, not only a --world ==========
    // The hub used to author itself at the world origin with a SEALED shell — fine
    // for `--world rifthub`, useless as a room in the canon world (it would sit on
    // top of the facility and have no way in). Two knobs fix that, and nothing else
    // about the authoring changes:
    //   * origin  — the hub's CENTER (floor Y). Every piece of hub geometry,
    //               collision, trigger, light, particle and spawn is authored
    //               relative to it. Default {0,0,0} == the old behaviour, so the
    //               dev world is byte-identical.
    //   * doorway — cut a real DOOR OPENING in the -Z (south) wall so the approach
    //               corridor (app/rift_depths.*) can seal into it. The wall is
    //               authored as left/right jamb segments + a lintel over the gap
    //               (seam law: flush faces, no gaps, still collidable).
    struct Desc {
        x3::phys::Vec3 origin{ 0.0f, 0.0f, 0.0f };
        bool  doorway     = false;   // cut the -Z wall opening
        float doorCenterX = 7.0f;    // opening center, hub-LOCAL X (between two gates)
        float doorHalfW   = 1.7f;    // half width of the opening
        float doorH       = 3.4f;    // opening height above the hub floor
    };

    // Stand up the hub: a 40x40 m flat physics ground centered at the origin, a
    // ring of 8 portals (one per --world target) at radius 14 m, and one
    // AABB trigger per portal (3 m square footprint, 4 m tall). `triggers`
    // is the host's shared TriggerSystem; the host forwards fired ids to
    // onTrigger(). Call once.
    void build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, TriggerSystem& triggers) {
        build(scene, device, physics, triggers, Desc{});
    }
    void build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, TriggerSystem& triggers,
               const Desc& desc);

    // The hub's authored center (floor Y) and the door opening in its -Z wall (world
    // space) — the approach corridor seals against these, and the self-test asserts
    // the corridor's mouth and the hub's doorway agree.
    const Desc&    desc()   const { return m_desc; }
    x3::phys::Vec3 origin() const { return m_desc.origin; }
    // World-space center of the -Z wall opening (x, floorY, z of the wall plane).
    x3::phys::Vec3 doorCenter() const {
        return x3::phys::Vec3{ m_desc.origin.x + m_desc.doorCenterX,
                               m_desc.origin.y,
                               m_desc.origin.z - kHubHalfM };
    }
    // The hub's floor half-extent (the shell is 2*this on a side).
    static constexpr float kHubHalfM = 20.0f;

    // Re-point a rift at a destination the HOST can actually deliver (the one-world
    // fast-travel wiring: in canonlevel the 8 gates are re-aimed at REAL places in
    // this world — the club, the caves, the crash site... — instead of the 8 dev
    // `--world` names they carry by default). Rebakes that rift's holo readout so the
    // glass can never disagree with where the gate goes.
    void setDestination(uint32_t portalIdx, const std::string& dest);

    // Per-frame animation. Flickers each portal's amber chevrons, pulses the
    // pulses the per-gate blue light, breathes + slowly rotates the membrane disk,
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

    // ===== TRAVERSAL (the payoff): stepping THROUGH an open rift ================
    // Index of the rift whose OPEN throat the eye is standing in (within `radiusM`
    // of the gate axis, at ring height), or -1. A rift only traverses when it is
    // ACTIVATED (its membrane is live) and not DEAD (a collapsed gate takes you
    // nowhere). The HOST owns the actual move — it is the only thing that knows the
    // world's anchors — so this is a pure query; see app_run.cpp's rift resolver.
    int  traversalPortal(const x3::phys::Vec3& eye, float radiusM = 1.7f) const;
    // Where rift `idx` currently points (the console can re-aim it).
    const std::string& destination(uint32_t idx) const { return m_portals[idx].destination; }

    // Per-portal blue CORE light set — one cool-blue point light at each ring
    // center that CASTS onto the grey stone gate + floor, pulsing with the core
    // hum (so the stone reads as lit by its own event horizon; step-4 blue-light
    // pass). Rebuilt each tick() with the pulsed intensity; the host forwards
    // this to device.setPointLights each frame. Empty until build().
    const std::vector<x3::rhi::PointLight>& pointLights() const { return m_lights; }

    // =======================================================================
    // ROUND 8 — THE PORTAL CONSOLES (owner: "let the user interact with each
    // portal ... they can do wonderful or disasterous things").
    //
    // In front of every rift hangs a HOLOTERMINAL in the project's canonical holo
    // language (black glass slab, blue/green text, a shiny round-pipe frame, a
    // single support pipe up to the ceiling so it HANGS). It reads out where the
    // portal goes. Walk up, press [E], and it becomes a live control surface:
    // glowing sliders + rotary knobs + a typed TARGET field (ui.h's R8 widgets).
    //
    // HOST CONTRACT (see world_hosts/host_rifthub.cpp):
    //   1. each frame:  consoleInRange(eye)      -> the [E] prompt
    //   2. on [E]:      openConsole(i) / closeConsole()
    //   3. while open:  ALL input goes to the console (the cell-terminal
    //                   discipline — typing must never fire the weapon or move
    //                   the player) and the host calls updateConsole() between
    //                   beginFrame/endFrame;
    //   4. every frame: multiply the sim dt by timeScale(), add fovOffset() to the
    //                   camera FOV, and apply shake()/damageFlash().
    // =======================================================================

    // Portal index whose console the eye is standing at (within `radiusM`), or -1.
    int  consoleInRange(const x3::phys::Vec3& eye, float radiusM = 3.4f) const;
    bool consoleOpen() const { return m_activeConsole >= 0; }
    int  activeConsole() const { return m_activeConsole; }
    void openConsole(int portalIdx);
    void closeConsole();

    // Draw + drive the OPEN console for one frame. Returns true on the frame the
    // player commits ENGAGE (the outcome has already been APPLIED to the world:
    // membrane, lights, geometry, alarms). No-op / false when no console is open.
    bool updateConsole(x3::ui::UiContext& ui, float dt);

    // The readout baked onto rift `idx`'s hanging glass: destination, status, and the
    // live parameter values in real units. ONE builder, called at build() and after
    // every ENGAGE, so the world's glass and the control surface can never disagree.
    std::vector<std::string> consoleReadout(uint32_t idx) const;

    // The hanging holoterminal for rift `idx` (the self-test asserts its screen has
    // real content bound — see HoloTerminal::screenHasContent()).
    const HoloTerminal& holo(uint32_t idx) const { return m_holos[idx]; }
    uint32_t holoCount() const { return (uint32_t)m_holos.size(); }

    // Apply an outcome to a portal directly (the console path calls this; the
    // self-test calls it too, which is how the consequences are gated).
    void applyOutcome(uint32_t portalIdx, RiftOutcome outcome);

    // ---- Global consequences the HOST must apply ---------------------------
    // TEMPORAL RIFT: a sim-dt multiplier. Slow-motion with a stutter (time stops
    // agreeing with itself). 1.0 when no rift is torn.
    float timeScale() const;
    // ROOM WARP: degrees to ADD to the camera FOV — a lens that breathes while
    // space bends. 0 when the hub is not warped. (The hub's props/columns/beams
    // physically bow and drift in tick(); this is the lens half of it.)
    float fovOffset() const;
    // IMPLOSION shockwave: camera-shake amplitude (m) and a red damage flash [0,1].
    float shake() const { return m_shake; }
    float damageFlash() const { return m_flash; }
    // The alarm banner ("" = none). The hall lighting reacts on its own in tick().
    const std::string& alarm() const { return m_alarm; }
    // True while ANY hub-wide catastrophe is running (host: kill the [E] prompt).
    bool catastrophe() const { return m_warp > 0.0f || m_temporal > 0.0f; }

    // Queries.
    bool built() const { return m_built; }
    // ROUND 3: true iff the Blender-authored gate GLB (tools/build_rifthub_gate.py
    // -> assets/converted_glb/rifthub/gate_ring.glb) loaded and produced drawables,
    // i.e. the dense authored gate replaced the procedural torus/plate/clamp
    // assembly. False = the procedural fallback ring was authored (GLB missing /
    // failed / produced no drawables) — the world NEVER breaks either way.
    bool gateGlbActive() const { return m_gateGlbActive; }
    // ROUND 4: number of membrane-flipbook frames loaded (0 = atlas absent /
    // undecodable -> the procedural nebula fallback is live). Self-test hook.
    uint32_t flipbookFrames() const { return (uint32_t)m_flipTex.size(); }
    // ROUND 6: frames loaded for the SURGE / OPEN state atlases (0 = that atlas is
    // absent -> that state falls back to its procedural map). Self-test hooks.
    uint32_t surgeFlipbookFrames() const { return (uint32_t)m_flipSurgeTex.size(); }
    uint32_t openFlipbookFrames() const { return (uint32_t)m_flipOpenTex.size(); }
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
    Desc                       m_desc{};               // origin + the -Z wall doorway
    x3::phys::Vec3             m_spawn{};
    std::vector<RiftPortal>    m_portals;
    // The ring-center height in WORLD space (hub-local kRingY lifted by the region
    // origin). tick()/drawFx() place membranes, motes and arcs with this.
    float ringWorldY() const;

    // Owned render resources (freed in shutdown()). The portal mesh vector
    // collects every PER-ENTITY device mesh authored by build() (ring torus,
    // chevron prisms, floor-plate wedges) so shutdown can free
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
    x3::rhi::TextureHandle     m_mrFlat;      // 1x1 rough/dielectric MR (PBR route)
    x3::rhi::TextureHandle     m_mrWet;       // 1x1 glossy MR (wet concrete floor)
    // 1x1 WEATHERED MR per gate-GLB material group (ROUND 4 ghost-glass fix):
    // the curated metal sets' MR maps are POLISHED (rough 0.25-0.45, metal up
    // to 0.98) — inside mesh.frag's mirror gate — so the SSR/RT reflection
    // radiance (wrong on the GLB's dense thin-plate geometry; the half-res
    // march tunnels behind it) replaced the gate's specular with the bright
    // content BEHIND it and the whole gate read as translucent glass.
    // Each texel keeps its set's METALLIC character but lifts roughness past
    // the 0.6 reflection cutoff; the gate stays opaque lit PBR.
    // [0] = patina plates, [1] = steel, [2] = dark hardware.
    x3::rhi::TextureHandle     m_mrGate[3];
    // MEMBRANE FLIPBOOKS (ROUND 4 J2 "steal Grok's pixels", completed in ROUND 6):
    // ALL THREE membrane states are now the owner's REAL FOOTAGE. Each is a 48-tile
    // 8x6 atlas baked by tools/make_membrane_flipbook.py from a span of
    // docs/reference/PortalAnimated.mp4 (radially masked; same disc crop for every
    // state so the membrane never jumps scale on a swap), sliced into per-frame
    // textures at build():
    //   m_flipTex      IDLE  (video t 0.00-3.20) — looped at kFlipFps, per-portal phase
    //   m_flipSurgeTex SURGE (video t 6.40-8.30) — played ONCE across the kawoosh
    //                  (progress-mapped, one-shot bake: no loop blend), the vortex
    //                  ring collapsing into the throat = a real escalation
    //   m_flipOpenTex  OPEN  (video t 8.40-9.95) — looped: the settled radial
    //                  streaming throat, frame-continuous with the surge's last frame
    // Any missing/undecodable atlas (fresh clone with LFS stubs) leaves its vector
    // empty and that state degrades to its procedural map (nebula / throat) — the
    // world never breaks. NOTHING hand-drawn is composited over the footage.
    std::vector<x3::rhi::TextureHandle> m_flipTex;
    std::vector<x3::rhi::TextureHandle> m_flipSurgeTex;
    std::vector<x3::rhi::TextureHandle> m_flipOpenTex;
    // Per-portal blue core lights (1:1 with m_portals); intensity pulsed in tick().
    std::vector<x3::rhi::PointLight> m_lights;
    // Curated PBR surface sets (ring plates / housings / cradle / hall). The
    // library owns the loaded textures; destroyAll() in shutdown(). On a box
    // with no assets present a set loads !ok and authoring falls back to the
    // flat-tinted look (the self-test path never breaks).
    SurfaceLibrary m_surf;
    // ROUND 3 gate GLB (the Blender-authored dense industrial gate; one model,
    // instanced by all 8 portals as Scene entities at portalXform*nodeTransform).
    // The loader owns the GPU handles — unload() in shutdown(), and these meshes
    // are deliberately NOT pushed into m_portalMeshes (no double-free). The three
    // glTF nodes are named gate_patina / gate_steel / gate_dark; authoring maps
    // each to a curated surface set + tint by that name.
    std::unique_ptr<x3::asset::IAssetSource>  m_gateAssets;
    std::unique_ptr<x3::asset::IModelLoader>  m_gateLoader;
    x3::asset::Model                          m_gateModel;
    std::vector<x3::asset::ModelDrawable>     m_gateDrawables;
    std::vector<std::string>                  m_gateNames;
    bool                                      m_gateGlbActive = false;
    // Fake-volumetric light shafts (ROUND 3 workstream 2). The glass-cone
    // attempt failed (the glass fallback path lifts alpha by fresnel — a big
    // low-opacity shell goes SOLID at grazing angles), so each shaft is a
    // static column of soft ADDITIVE billboard particles drawn by drawFx()
    // (a second submitParticles batch — the device appends batches per frame).
    struct Shaft {
        float top[3], bot[3];
        float width  = 1.0f;    // bottom-radius scale (m)
        float alpha  = 0.10f;   // per-particle additive alpha
        // Unit vectors spanning the shaft's cross-section (built once).
        float ux = 1, uy = 0, uz = 0;
        float vx = 0, vy = 0, vz = 1;
    };
    std::vector<Shaft>                        m_shafts;

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

    // ---- ROUND 8: the consoles + the catastrophes --------------------------
    // One hanging HOLOTERMINAL per portal (1:1 with m_portals). Held here rather
    // than in RiftPortal so the portal struct stays a trivially-copyable POD-ish
    // value (it is pushed into a vector during build).
    std::vector<HoloTerminal> m_holos;
    int   m_activeConsole = -1;    // portal index whose console is OPEN (-1 = none)
    float m_uiClock = 0.0f;        // drives the control-glow pulse

    // ROOM WARP: the hub visibly BENDS. m_warpEnts is the set of hall props
    // (columns, beams, strip fixtures, machinery) whose base transforms are cached
    // in m_warpBase, so tick() can bow/ripple/drift them around the warp source and
    // put them back EXACTLY when it ends (no drift accumulation).
    float m_warp = 0.0f;           // seconds remaining
    float m_warpSrc[3] = {};       // the gate that tore it
    std::vector<uint32_t> m_warpEnts;
    std::vector<float>    m_warpBase;   // 3 floats per entity (the AUTHORED center)
    bool  m_warpWasOn = false;          // edge: restore the props on the frame it ends

    float m_temporal = 0.0f;       // TEMPORAL RIFT: seconds remaining
    float m_shake    = 0.0f;       // IMPLOSION shockwave
    float m_flash    = 0.0f;       // damage flash [0,1]
    float m_alarmT   = 0.0f;       // alarm-light strobe clock
    std::string m_alarm;           // banner text ("" = quiet)
    // Snapshot of the hall fill lights' authored colours, so the ALARM can strobe
    // them red and put them back EXACTLY (no cumulative drift across events).
    std::vector<float> m_lightBase;   // 3 floats per light in m_lights

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
