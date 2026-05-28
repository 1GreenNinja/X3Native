# X3Native Space Engine — Design Spec

**Date:** 2026-05-28
**Status:** DRAFT — pending Tim's review
**Context:** Act 3 of "Escape From Lab Zero" ("Beyond the Stars," ~L36–75). This spec defines the space-engine architecture and decomposes it into ~11 independent subsystems (= parallel agent lanes). The interface contracts in §3 are the load-bearing part — they are what let the lanes build in parallel without colliding.

---

## 1. Goals & non-goals

**Goals:** fly an ultra-detailed ship through space → jump between star systems via a Salvari crystal-matrix wormhole → walk the ship interior during transit (open panels, repair wires, watch space move past real windows, Star-Trek-style) → EVA outside the ship to repair the hull when needed → fly to a planet → cinematic atmospheric descent → land in that planet's world → full space dogfighting along the way. Polish + long-term playability over ship-fast.

**Non-goals (explicitly cut, with reasons):**
- **Seamless no-loading planet descent** (No Man's Sky / Star Citizen). Cut because it requires a floating-origin coordinate retrofit + planet-scale LOD terrain — multi-month foundational work — AND it can't host the interior-repair-during-transit beat. Staged transitions deliver the polished experience without it.
- **Floating-origin / 64-bit world coordinates.** Not needed: every context is its own origin-centered world; transitions are staged.
- **Procedurally infinite galaxy.** Handcrafted systems/planets scaled by story importance (see §2.5).

## 2. Locked design decisions (from brainstorming 2026-05-28)

| # | Decision | Consequence |
|---|---|---|
| 2.1 | **Staged transitions**: wormhole (interstellar) + cinematic descent (orbit→ground) | No floating origin. Reuses `--world` + terrain streaming. |
| 2.2 | **Origin-centered coordinates** per context | No precision retrofit. |
| 2.3 | **Ship interior scales with ship class** (data-driven room manifest) | One interior system; small/large/huge ships are data, not code. |
| 2.4 | **True-portal "Star Trek" windows** (TRANSIT-CONTEXT ONLY): during `WormholeTransit`, ship is static at origin + environment streaks past windows + scaled proxies + light bleed | Interior physics in static ship frame (no moving-platform problem). Motion is purely visual. **Does NOT apply to DeepSpace** — see 2.8. |
| 2.8 | **Ship has TWO representations, switched by Context.** **Exterior** (`DeepSpace` flight + dogfight): a REAL moving entity — 6DOF position/velocity/orientation in a shared world frame, full-detail hull, viewable from cockpit / 3P chase / cinematic external angles, dogfighting real enemy ships. **Interior** (`WormholeTransit`): static walkaround (2.4). | Dogfighting REQUIRES the real moving exterior + multiple external views — can't be a static instance. space-pilot already drives this (6DOF + 1P/3P toggle + weapons). The static-environment trick is transit-only. |
| 2.9 | **EVA spacewalk repair** (`EVA` context): ship at rest/drifting, player exits an airlock and free-floats outside on the hull to repair it. | Zero-G locomotion (thruster pack + mag-boots) reuses the shipped **swim controller** (~80% overlap: 3D free-move + boost + oxygen). Exterior hull (S11) must support close traversal + repair points, not just a distant silhouette. Exterior version of the S7 repair interactions. |
| 2.5 | **Planet surfaces: mix** — major worlds = full open-world (reuse terrain/WorldRegions/City/OceanBase); minor = handcrafted arenas | Scope scales with story weight. |
| 2.6 | **Distance LOD on all exterior content** | Makes true-portal windows + space scene affordable. Needs a runtime LOD-swap system (assets are LOD0-only today). |
| 2.7 | **Full dogfighting combat pillar** | Adds enemy ship AI + targeting + ship damage model. |

## 3. The spine — S0 SpaceLayer (THE shared interface)

Everything plugs into `SpaceLayer`. It owns the current **context** and orchestrates transitions. This interface is frozen first; all other lanes build against it.

```cpp
namespace x3::space {

enum class Context {
    DeepSpace,      // free flight in a star system, combat happens here (real moving ship)
    WormholeTransit,// autopilot; player walks the static ship interior
    EVA,            // ship at rest/drifting; player free-floats outside on the hull (repairs)
    AtmoDescent,    // on-rails orbit->ground cinematic
    Surface,        // handed off to a --world (open-world hub or arena)
};

// Scaled-proxy descriptor for anything visible through windows / at range.
// Real object, but LOD'd hard; NOT true planetary scale.
struct Proxy {
    enum class Kind { Planet, Ship, Station, Asteroid, Wormhole } kind;
    float pos[3];        // scene-space position (scaled, not true distance)
    float radius;        // proxy size
    uint32_t lodAsset;   // handle into the LOD system (S2)
    float tint[4];
};

class SpaceLayer {
public:
    void init(rhi::IRenderDevice&, phys::IPhysicsWorld&);
    Context context() const;

    // Transition requests — S0 drives the state machine; the transition
    // subsystems (S3/S4) register handlers and run the actual sequence.
    void requestWormhole(uint32_t destSystemId);   // -> WormholeTransit -> DeepSpace(dest)
    void requestDescent(uint32_t planetId);        // -> AtmoDescent -> Surface
    void requestAscent();                          // Surface -> DeepSpace

    // The moving-environment model (decision 2.4): the ship is static at
    // origin; S0 owns the environment transform that everything-outside is
    // expressed relative to. Window rendering (S6) + space env (S1) read this.
    void setEnvironmentVelocity(const float vel[3], const float angVel[3]);
    void environmentTransform(float out16[16]) const;

    // Proxy registry — S1 (env) populates; S6 (windows) + S2 (LOD) consume.
    uint32_t addProxy(const Proxy&);
    void     updateProxy(uint32_t id, const Proxy&);
    void     removeProxy(uint32_t id);
    uint32_t proxyCount() const;
    const Proxy& proxy(uint32_t i) const;

    // Per-frame: advances the active transition, updates env transform.
    void update(float dt);

    // Transition subsystems register their sequence runners here.
    using TransitionFn = std::function<bool(float dt)>; // returns true when complete
    void registerWormholeRunner(TransitionFn);
    void registerDescentRunner(TransitionFn);
};

} // namespace x3::space
```

**Self-test `--test-spacelayer`:** context transitions fire in order; environment transform integrates from velocity; proxy registry add/update/remove; transition runners invoked + completion advances context.

## 4. Subsystem catalog (the lanes)

Each = one agent lane. Format: **responsibility · key interface · depends on · files.**

### Foundation
- **S0 · SpaceLayer spine** — context state machine + environment transform + proxy registry (§3). Depends: nothing. Files: `app/space/space_layer.{h,cpp}`. **MUST land before Wave 2.**
- **S2 · Distance-LOD system** — runtime mesh-swap by camera distance; LOD chain per asset (regenerate LOD1/2/3 via Blender decimation since conversion kept LOD0-only, OR generate at load). Interface: `LodSet::select(distance)->meshHandle`. Depends: nothing (rendering util). Files: `app/space/lod.{h,cpp}` + a Blender LOD-gen script in `tools/`.

### Environment & assets (independent — no S0 runtime dep, but populate S0's proxy registry)
- **S1 · Space environment** — skybox/nebula (extends shipped starfield), proxy planets (low-poly LOD spheres + normal/displacement maps from Tim's planet packs), sun + bloom, instanced asteroid fields. Depends: S2 (LOD), shipped starfield. Files: `app/space/space_env.{h,cpp}`.
- **S11 · Ship art + node-animation** — ultra-detailed ship GLBs; landing gear / panels / turrets via **node-transform animation** (NOT skeletal rigging — ships are rigid with articulated parts). **Exterior hull must be detailed + collidable enough for close EVA traversal + carry exterior repair points** (S12), not just a distant combat silhouette. Asset pipeline lane. Depends: nothing. Files: ship GLBs in `assets/rigged_glb/`, node-anim metadata, Blender node-anim export script in `tools/`.

### The signature experience (Wave 2/3 — depend on S0)
- **S3 · Wormhole transit** — crystal-matrix wormhole VFX (shader-driven tunnel) + autopilot state + the transit context manager. Registers a wormhole runner with S0. Lore: Salvari crystal-matrix translation = the mechanism (ties to the caves Salvari-crystal beat in the design corpus). Depends: S0. Files: `app/space/wormhole.{h,cpp}`, `shaders/wormhole.{vert,frag}`.
- **S5 · Ship interior** — data-driven room manifest per ship class (small=cockpit shell, large=multi-room, huge=multi-deck); static-ship frame; walkable via reused Player capsule; crew via NPCController + CompanionController (both shipped). Interface: a `ShipClass` JSON/data format declaring rooms, doors, stations, window placements. Depends: S0. Files: `app/space/ship_interior.{h,cpp}`, ship-class data files.
- **S6 · True-portal windows** — real-opening windows in the hull showing the moving environment (S0 env transform + S1 proxies + S3 wormhole) with parallax + light bleed into the interior. The Star Trek tech. Depends: S5 (interior), S1 (env), S0. Files: `app/space/window_portal.{h,cpp}`, `shaders/portal.{vert,frag}`.
- **S7 · Interior interaction** — open panels, repair-wire minigame, the transit gameplay loop; interactable stations. Depends: S5. Files: `app/space/ship_interact.{h,cpp}`.

### EVA (Wave 2/3 — depends on S0; reuses shipped swim controller)
- **S12 · EVA spacewalk** — zero-G exterior locomotion (thruster pack + mag-boots for hull-walking, oxygen, optional tether) ADAPTED from the shipped swim controller (`feat/swim-controller @ c2eeb18` — 3D free-move + boost + oxygen ≈ thruster + boost + O2; swap buoyancy→zero-G, stroke→thruster, add mag-boot surface-stick). Exterior hull-repair interactions (outside version of S7). `EVA` context, entered from `DeepSpace` via airlock. Depends: S0, S11 (close-traversable hull), swim controller. Files: `app/space/eva.{h,cpp}`.

### Landing
- **S4 · Cinematic atmo descent** — orbit→ground on-rails sequence (hull glow, clouds, fire) masking the load into the surface `--world` (open-world hub via terrain streaming, or arena). Registers a descent runner with S0; hands off to the existing `--world` system. Depends: S0, existing terrain/openworld worlds. Files: `app/space/descent.{h,cpp}`.

### Combat pillar (Wave 2 — depend on S0 + shipped space-pilot, independent of interior)
- **S8 · Enemy ship AI** — fighter squadron behavior + capital-ship behavior; the dogfight sandbox AI. Depends: S0, space-pilot. Files: `app/space/ship_ai.{h,cpp}`.
- **S9 · Targeting / radar / lock-on** — HUD targeting, contact radar, lock-on. Depends: S0. Files: `app/space/targeting.{h,cpp}`.
- **S10 · Ship damage model** — shield/hull/subsystem damage for enemies + capital-ship destructible subsystems (extends what space-pilot has for the player). Depends: space-pilot. Files: `app/space/ship_damage.{h,cpp}`.

### Already shipped (feed in)
- ✅ **space-pilot** 6DOF controller (`feat/space-pilot @ ba16141`)
- ✅ **starfield** (`feat/space-stars @ 8ea322d`)
- ✅ **SPACE_ART_PLAN.md** (skybox/planet/asteroid art pipeline)

## 5. Shared data contracts (freeze these in S0/S5 before Wave 2)

- **Ship-class manifest** (S5): declares `rooms[]` (name, bounds, mesh), `doors[]`, `stations[]` (helm/nav/repair/weapons), `windows[]` (placement + size, consumed by S6), `class` (small/large/huge). One format; every ship is a data file.
- **Proxy** (S0 §3): the only thing S1/S6/S2 exchange about "stuff outside."
- **LOD set** (S2): `{ lod0..lod3 meshHandles, switchDistances[] }`.
- **Transition runner** (S0): `bool(float dt)` returning completion — S3/S4 implement, S0 drives.

## 6. Wave / dispatch plan

| Wave | Lanes | Gate to start |
|---|---|---|
| **1** | S0 spine · S2 LOD · S1 env · S11 ship art | now (S1 stubs against S0's proxy API; S0 is the long pole) |
| **2** | S3 wormhole · S4 descent · S5 interior · S8 enemy AI · S9 targeting · S10 damage | S0 landed + merged |
| **3** | S6 windows · S7 repair interaction | S5 interior landed |

Per-rig concurrency cap: **≤4-5 X3Engine runtime instances at once** (Bug 2 — multi-process Vulkan queue saturation). Stagger test/screenshot runs across lanes. Lane gates: `--test-<subsystem>` + `--world <subsystem>` screenshot pixel-variance (std>15, uniqColors>100) — NOT drawCalls alone. NO `--smoketest` in lane gates until the rig is quiet (run it once at integration).

## 7. Testing

Each subsystem ships a `--test-<name>` self-test (headless, deterministic) + a `--world <name>` showcase for visual verification. Integration test: the full `DeepSpace → wormhole → interior walk + repair → descent → surface` loop as a scripted `--world spacejourney` once Waves 1-2 land.

## 8. Open content questions (non-blocking for the build — answer during content pass)

- How many star systems / planets in Act 3? (handcrafted, scaled by story weight)
- Which ships are which class? (the 4 SpaceShip*.glb + future Rodin ships → small/large/huge assignment)
- Crew roster per ship (reuses the character library + CompanionController)
- Combat encounter design (where the authored dogfights + capital-ship beats sit in the L36–75 arc)
