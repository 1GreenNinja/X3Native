# Beyond id Tech 8 — Where It's Brilliant, Where It's Bounded, and How X3Native Goes Broader

> **Status:** strategy / architecture analysis. Started 2026-05-22 (13700K clean-room rig).
> **Audience:** Tim (owner) + every parallel agent, as a roadmap input.
> **Companion doc:** `docs/IDTECH8_ROADMAP.md` (the parity north-star + §9 "T3 beyond" set). This doc is the *strategic why* behind that §9 list.

## 0. Read this first — the honest framing

id Tech 8 (Doom: The Dark Ages, 2025) is a **masterpiece for its goals**. Those goals are narrow on purpose: a **locked-60-fps**, **fully ray-traced**, **GPU-driven**, **single-player**, **level/arena-based** action campaign with huge enemy counts and near-zero load times. Inside that envelope it is arguably the best-engineered game engine shipping today, and Tiago Sousa's SIGGRAPH 2025 *"Fast as Hell: idTech 8 Global Illumination"* talk is a master class in real-time GI.

X3Native is **not** trying to be a better Doom engine. It is a **broader-scope** engine: an **open-world, multiplayer-capable, hardware-spanning sandbox** that must run on a GTX 1080 Ti dev floor *and* an RTX 5090, support seamless 30 km worlds, persistent shared-world MP, vehicles/flight/underwater/space, and Teardown-tier destruction.

So almost every "X3Native does better" in this doc is one of two things, and the doc is careful to say which:

1. **Different/broader scope** — id Tech 8 *chose not to do* this because it would cost them frame-time or iteration speed they need for their product. It is a deliberate, correct tradeoff *for them*. X3Native does it because it is a *different product*. (Most entries.)
2. **A genuine axis improvement** — a small number of places where X3Native's architecture is honestly broader/more flexible on a real engineering axis (hardware breadth being the clearest). (Few entries — flagged explicitly.)

**This is not a claim that X3Native is "better than id Tech 8."** Today it is a graybox engine with a single playable level; id Tech 8 shipped a polished AAA game. The closing sections (§10–§11) are blunt about where id Tech 8 is far ahead and what that means for our backlog. The value here is *strategic clarity*: knowing exactly which of id Tech 8's limits are real opportunities for a broader engine, versus which are tradeoffs we'd be foolish to "fix."

> **Clean-room note.** Every id Tech 8 statement below comes from **public** material only: id Software interviews, Tiago Sousa's public SIGGRAPH 2025 GI slides/recording, Digital Foundry analysis, Bethesda's published system requirements, and press. No game-engine source was read or referenced. X3Native claims are grounded in this repo's own specs/docs/code (cited inline). This honors `PROVENANCE.md` and `LICENSE`.

---

## 1. Open-world scale & seamless streaming

**What id Tech 8 does.** Hand-authored **levels / arenas**, larger than Doom Eternal's (id says ~5–10× by area) with auto vista LODs, contribution culling, and heavy background streaming inside a level. Digital Foundry's analysis frames the game as fast-paced, **level-based progression**, not an open world. Loads are 2–4 s.

**The limitation.** It is not a seamless, effectively-infinite world. You traverse discrete (if large) maps; there is no 30 km × 30 km continuous landscape you can walk across without a level boundary. The world is *authored*, not *generated-and-streamed-without-bound*.

**WHY (the tradeoff).** Bounded, hand-authored levels are exactly what lets id hit **locked 60 fps** and their famous combat pacing. A known, finite playspace means: predictable worst-case draw load per arena, art-directed encounters, baked-friendly GI volumes per level, and tight memory budgets. An open world trades all of that predictability away. For a *Doom*, the bounded arena is not a weakness — it is the design.

**How X3Native does better (different/broader scope).** X3Native targets a **seamless, effectively-infinite, procedurally-generated streamed world**. This is **BUILT** today, not specced:

- `app/terrain.*` — `TerrainStreamer` keeps a **camera-centered Chebyshev residency ring** of tiles keyed by *signed* `(gx, gz)` packed into a 64-bit key. As the focus moves, tiles **stream in ahead / out behind** so resident count + memory stay **constant no matter how far you travel** (`runStreamingSelfTest()` asserts exactly this: bounded residency, load-ahead/unload-behind, no leak, no fall-through).
- Heavy per-tile work (noise → 3 LOD meshes + collision soup) runs **off-thread on `IJobSystem`**; the main thread drains a completion queue **budgeted** per frame (`maxUploadsPerFrame`). Distance-based **LOD** with skirts hides cracks. One static-collision body per tile (LOD0).
- Height is a **pure deterministic function** `terrainHeightAt(cfg, x, z)` of `(world, seed)` — so the world is *regenerated*, not stored, and is *identical on every machine from the seed* (a property the netcode spec leans on hard — terrain is never replicated).

**Status in X3Native today:** **BUILT** (streaming + LOD + per-tile collision; bounded-residency self-test passing). Texturing/biomes/auth-tooling for the world are still ahead.

---

## 2. Massive, persistent, shared-world multiplayer

**What id Tech 8 does.** id's multiplayer lineage is **arena/match-scale** (small bounded sessions, classic deathmatch-lineage), tightly tuned for feel. The Dark Ages is single-player-focused. There is no persistent, MMO-scale shared world in the engine's public feature set.

**The limitation.** No always-on client/server foundation for a persistent shared world; no spatial sharding for thousands of players across a continuous map; co-op + competitive PvP in one **seamless 30 km world** is out of scope.

**WHY (the tradeoff).** id's product is a single-player campaign. Building MMO-grade netcode (interest management at world scale, sharding/handoff, region servers, lag-compensated PvP) into the engine would be enormous effort delivering zero value to their actual game, and the always-on client/server discipline adds cost (every gameplay action routed through commands → server → replication) that a pure-SP engine can skip. For a SP campaign, *not* building it is correct.

**How X3Native does better (different/broader scope).** Tim's stated MP goal is *"all of it"* — SP + co-op (PvE) + competitive PvP — for a **~30,000 × 30,000 m** world. The architecture is fully specced in `specs/NETCODE-architecture.spec.md`:

- **#1 principle: always-on client/server, loopback by default.** The engine *always* runs client/server; **single-player is a local server + local client over an in-process loopback transport**. "Enabling MP" is a **transport swap** (`INetTransport`: loopback → UDP), nothing more — one code path, MP-shaped at all times, so SP and MP can never silently diverge. (Quake/Source lineage.)
- **Interest management rides the streaming grid.** AoI replication reuses the **same residency-ring** the `TerrainStreamer` already computes — a client only receives entities in its subscribed tile cells, so **bandwidth scales with view, not world size** (a player in the 30 km world costs the same as in a tiny one).
- **Sharding for the massive world** (Phase 4): region servers own slices of the same tile grid; border-band overlap + entity migration give seamless cross-region handoff with no loading screen.
- **Server-authoritative + prediction/reconciliation/interpolation/lag-comp**; co-op and PvP share **one** model (PvP just turns on stricter validation + the rewind ring). Anti-cheat posture is intrinsic (the client can only send input commands).

**Status in X3Native today:** **SPEC** (full architecture + phased plan locked; Phase 0 — loopback split + fixed-step sim + `NetCommand` + generation-tagged `NetEntityId` — is the cheap "bake it in now so MP is never a rewrite" work, not yet built). This is the single largest *scope* difference from id Tech 8.

---

## 3. Hardware breadth / RT dependency  ⟵ *genuine axis improvement*

**What id Tech 8 does.** Real-time **ray-traced lighting as the foundation**, not an option. Per Sousa's talk, GI is a hybrid of **cascaded light grids + irradiance volumes (Ambient Probes)** with a real-time **path-traced irradiance cache** updating it — cinematic, fully-dynamic, **WYSIWYG (no per-scene bake/wait at edit time)**, at 60 Hz.

**The limitation.** Per Bethesda's published requirements, **The Dark Ages will not launch on a GPU without hardware ray-tracing** — minimum is roughly an RTX 2060 Super / RX 6600, and there is **no option to disable RT** because the lighting *is* RT. Pre-RTX cards (GTX 10-series, including our 1080 Ti dev floor) are simply unsupported.

**WHY (the tradeoff).** This is a clear-eyed bet by id: RT-capable hardware crossed a market threshold, and making RT mandatory **removes the entire cost of a non-RT fallback path** — no second lighting pipeline to author, tune, QA, or keep at parity. That fallback removal is a real engineering and art-pipeline win, and it's *why* they can ship fully-dynamic GI with no bake step. For id's audience and ship date, dropping pre-RTX cards was the right call.

**How X3Native does better (genuine axis improvement — broader hardware envelope).** X3Native's clean-room build machine is a **GTX 1080 Ti (Pascal) with zero hardware RT cores** (`docs/IDTECH8_ROADMAP.md` §2). Copying "RT required, no fallback" would orphan the dev box *and* a large slice of real players. So the locked decision **D-RT** makes lighting **hybrid and hardware-gated**:

- **GPU-driven *raster/compute* path is the floor** and runs *everywhere*, including the 1080 Ti — bindless + multidraw-indirect + compute culling are **BUILT** (`engine/rhi/`, confirmed in `docs/NOTE_TO_14900K.md` and `RENDERING_SPEED.md`), plus directional shadows, analytic sky, 16 point lights, HDR/bloom/emissive, and **SSAO**.
- **GI is split:** a **compute-based irradiance/probe GI** (Sousa-style cascaded grids — runs on the 1080 Ti, *no RT cores needed*) is the cross-hardware tier (subsystem **F**); a **hardware-RT reflections / path-tracing tier** (subsystem **G**) is enabled *only* on RT-capable GPUs (5090). RT is a **quality tier, never the foundation.**

This is the one axis where X3Native is honestly *broader* than id Tech 8 by construction: **it spans more hardware** — same engine, raster/compute floor on Pascal, RT showcase on Blackwell. The cost is exactly the fallback path id chose to delete; X3Native pays it deliberately to keep the hardware envelope wide.

**Status in X3Native today:** **hybrid raster/compute path BUILT** (runs on the 1080 Ti, validation-clean). **Compute GI (F) not yet built** — see §10, this is where id Tech 8 is genuinely ahead. **HW-RT tier (G) is future**, 5090-only.

---

## 4. Vehicles, flight, underwater & space

**What id Tech 8 does.** On-foot (and mounted/dragon set-pieces in The Dark Ages) combat traversal. It is not a general vehicle/flight/sub/space-sim engine; those are not its problem domain.

**The limitation.** No general-purpose vehicle physics sandbox — cars/aircraft/submarines/spaceships as first-class force-driven rigid bodies, water/buoyancy, space flight.

**WHY (the tradeoff).** Vehicles and free-form sim domains add huge surface area (buoyancy, aerodynamics, 6-DOF control, fluid) that a tightly-paced FPS doesn't need. Spending engine budget there would dilute the locked-60 on-foot combat that *is* the product. Correct to skip.

**How X3Native does better (different/broader scope).** EFLZ's later acts demand exactly this range — Act 2 vehicle combat, **Act 3 is a space journey** (the *Storm Runner* ship hub, orbital battles, fighter craft), and Act 4 has tanks (`docs/EFLZ_DESIGN.md`). The engine plan treats vehicles as **rigid body + force controller** on the existing physics base:

- **Jolt CPU physics is BUILT and authoritative** (`engine/physics/`, M3 done) with character controller; the K-spec already extends `IPhysicsWorld` with `addConvexHull`/`addCompound`, velocity/rotation getters/setters, and body user-data — the same primitives a force-driven vehicle controller needs.
- **Water/ocean is the next engine-layer item in flight** (`docs/NOTE_TO_14900K.md`: "What I'm building next: Water/ocean → …"), the precursor for buoyancy/underwater. The Babylon X3 had an ocean/oxygen/swim system (`X3_NATIVE_FEASIBILITY_2026-04-27.md`) as the design reference to port.

**Status in X3Native today:** **physics base BUILT; water WIP; vehicles DESIGNED** (EFLZ acts + force-controller-on-Jolt approach), not yet built. Clearly behind on this axis but it's *in scope by design*, where for id it isn't.

---

## 5. Destructibility

**What id Tech 8 does.** id Tech 8 has GPU-driven physics and reactive on-screen effects, but published destruction is largely **scripted / localized** (set-piece breakage, craters/debris tied to authored moments) rather than free-form structural collapse of arbitrary geometry.

**The limitation.** Not Red-Faction/Teardown-tier **structural** destruction — you can't generally collapse a building by removing its supports, with arbitrary fracture propagating through a connectivity graph.

**WHY (the tradeoff).** Free-form structural destruction is brutal on a locked-60 budget *and* on level design (it breaks art direction, cover layout, navmesh, and predictable encounter flow). Scripted/localized destruction gives the *spectacle* with bounded, predictable cost — exactly what a frame-time-disciplined arena shooter wants. Correct tradeoff.

**How X3Native does better (different/broader scope, T3 beyond).** `specs/K-gpu-destruction.spec.md` specs a **two-world hybrid** (Jolt authoritative + a GPU-compute *visual* debris world, one-way coupled per **D-PHYS**) with explicit tiers:

- **T2 (parity):** GPU compute debris world — **50k+** fragments on the 1080 Ti, **1M+** on the 5090 — in SoA SSBOs, indirect-drawn, with **smart sleep/wake persistence** so resting debris costs ~0; CPU→GPU handoff of small/sleeping pieces; async-compute + timeline-semaphore sync.
- **T3 (beyond):** **structural-connectivity destruction** — an authored adjacency graph + a connected-components pass means any sub-graph no longer connected to a support anchor **wakes and falls**: progressive building collapse, **Red Faction Geo-Mod / Teardown tier**. Plus nested/hierarchical fracture (chunks re-fracture on later impacts).
- Net-aware: only the **break *event*** is authoritative/replicated; the thousands of fragments are client-local visual-only (so destruction's scale never hits the wire).

**Status in X3Native today:** **SPEC** (fully tiered T0→T3 with interface contracts + 11 acceptance tests). Downstream of the renderer core + jobs. This is a genuine *capability* X3Native intends that id Tech 8 deliberately bounds.

---

## 6. Data-driven gameplay, scripting & modding

**What id Tech 8 does.** id Tech is **C++-heavy** for gameplay; iteration is fast for *id's own team* with their tooling, but it is not a broadly script-/mod-exposed engine the way some others are.

**The limitation.** Gameplay logic largely lives in compiled C++; there is no shipped first-class embedded scripting/modding surface for external creators.

**WHY (the tradeoff).** C++ gameplay maximizes performance and fits a single tightly-integrated studio team with mature internal tools. A scripting VM adds an FFI boundary, GC/perf considerations, and an API-stability burden — overhead a single-studio SP product doesn't need to expose. Correct for a closed, in-house pipeline.

**How X3Native does better (different/broader scope).** X3Native plans **Lua via sol3** as the gameplay/content layer (`specs/M4-script-vm.spec.md`): entity behavior, weapons, AI state machines, level scripts, cutscene sequencing authored in Lua with **hot-reload**, while the engine stays C++. The engine exposes a *curated* API; Lua never touches raw internals. EFLZ's breadth (150 crafting recipes, 12 endings, branching timeline, companion skill trees per `docs/EFLZ_DESIGN.md`) is exactly the kind of sprawling, iteration-heavy content that wants data-driven scripting rather than recompiles.

**Status in X3Native today:** **SPEC / early** (M4 spec ready; sol3 + LuaJIT chosen, both MIT/clean; not yet wired). Honest: this is an *intent*, and id's C++ pipeline is more mature *today*.

---

## 7. Determinism, replay & netcode foundation

**What id Tech 8 does.** Not a published focus — a SP campaign doesn't need cross-machine determinism, demo/replay-from-inputs, or a deterministic sim spine as a core engine pillar.

**The limitation.** No public deterministic fixed-step sim / replay-from-commands foundation as an engine feature.

**WHY (the tradeoff).** A pure SP engine simulates at whatever cadence is convenient; it has no client to reconcile, no demo system requirement, no replay obligation. Building a deterministic fixed-step spine + command/replay machinery would be cost with no SP payoff. Correct to skip.

**How X3Native does better (different/broader scope — and it's load-bearing for §2/§5).** X3Native commits to a **deterministic fixed-step sim** as the shared foundation for prediction, replay, and netcode (`specs/NETCODE-architecture.spec.md` §3, §10):

- **Single-machine fixed-step determinism: YES** — same inputs + same fixed `kSimDt` → identical result on the same build. This powers client-prediction re-simulation, **demo record/replay** (a replay is just a third `INetTransport` that logs/plays commands), the J-spec's **deterministic anim replay (T16)**, and reproducible tests. Jolt already steps at a fixed `1/60` internally — half the work is done.
- **Seeded procedural determinism: YES, already true** — `terrainHeightAt()` is pure in `(world, seed)`, so the world regenerates identically everywhere.
- **Cross-machine bit-lockstep: deliberately NO** — Jolt is single-precision (not bit-deterministic across CPUs/compilers) and a 30 km partially-resident world can't re-sim everything everywhere. Server-authoritative + prediction is the right tool; the doc is explicit about *not* over-reaching into lockstep.

**Status in X3Native today:** **SPEC / foundation** (the fixed-step + generation-tagged ids land in netcode Phase 0; the deterministic anim-replay tier is J-spec T3). Note: the **index-recycling bug** (scene ids are array indices reused by the streamer) is a real *current* latent defect that generation-tagged `NetEntityId` fixes — worth doing for SP demo/replay regardless of MP.

---

## 8. Other credible axes (grounded both ways)

### 8a. Very-high-refresh & decoupled render rate  *(scope/flexibility)*
- **id Tech 8:** tuned around a **locked 60 fps** target with exceptional frame-time stability — that *discipline* is a strength, not a flaw.
- **X3Native:** the netcode spec **decouples render rate from sim rate** — render at 144+ Hz over a 60 Hz (or 30 Hz) fixed sim, with interpolation for remote entities. The GPU-driven path + upscaling presets (DLSS/FSR, per `RENDERING_SPEED.md`) aim render throughput above a fixed cap for high-refresh displays.
- **Honest:** id's locked-60 *consistency at AAA scale* is something we have **not** demonstrated (graybox only). Decoupled-rate flexibility is real architecturally; the *discipline* to hold a stable cap under AAA load is something we still have to earn (§10).
- **Status:** decoupled-rate is **SPEC** (netcode §3.1); upscaling presets **planned**.

### 8b. Procedural generation as a first-class world source  *(scope)*
- **id Tech 8:** hand-authored levels (procedural generation is not its world model).
- **X3Native:** the world *is* procedurally generated and streamed (§1) from a seed, with self-implemented value-noise + fBm (`app/terrain.*`). This is the substrate that makes the infinite world + cross-machine seed-determinism + bandwidth-free terrain possible.
- **Status:** **BUILT** (heightfield + streaming); biomes/texturing ahead.

### 8c. Save-anywhere / persistence  *(scope, downstream of §7)*
- **id Tech 8:** checkpoint/level-progression save model (fits arena structure).
- **X3Native:** a server-authoritative, deterministic, region-relative world model (§2/§5/§7) is the natural foundation for **save-anywhere persistence and shared-world state** — the same replicated component blocks that go on the wire serialize to disk.
- **Status:** **implied by spec** (not separately specced yet) — flagged as a strategic opportunity, not a built claim.

### 8d. Cross-play / dedicated-server topology  *(scope)*
- **id Tech 8:** SP-focused; no published cross-play shared-world dedicated topology.
- **X3Native:** the `INetTransport` seam + headless dedicated-server build (no `IRenderDevice` linked) + transport-agnostic clients (loopback/UDP treated uniformly) make dedicated servers and cross-play a *topology choice*, not a rewrite.
- **Status:** **SPEC** (netcode §1.3, Phase 2+).

---

## 9. The "do better" axes at a glance

| # | Axis | Type | id Tech 8 today | X3Native intent | X3Native status |
|---|---|---|---|---|---|
| 1 | Open-world scale & streaming | broader scope | level/arena based | infinite procedural streamed 30 km world | **BUILT** |
| 2 | Persistent shared-world MP | broader scope | arena-scale MP | always-on C/S, AoI on the grid, sharding, co-op+PvP | SPEC |
| 3 | Hardware breadth / RT not required | **genuine axis** | RT required, no fallback | hybrid raster/compute floor + RT *tier* | floor **BUILT**; GI(F)/RT(G) ahead |
| 4 | Vehicles / flight / sub / space | broader scope | on-foot focus | rigid-body + force controllers; water | physics **BUILT**; water WIP; vehicles designed |
| 5 | Structural destructibility | broader scope | scripted/localized | GPU debris + Teardown-tier structural collapse | SPEC |
| 6 | Data-driven gameplay / scripting / modding | broader scope | C++-heavy | Lua/sol3 hot-reload, data-driven | SPEC/early |
| 7 | Determinism / replay / netcode foundation | broader scope | not a focus | deterministic fixed-step sim spine | SPEC/foundation |
| 8a | Very-high-refresh / decoupled rate | scope/flex | locked-60 (a strength) | render-rate decoupled from sim | SPEC |
| 8b | Procedural generation as world source | broader scope | hand-authored | seed-driven generation + streaming | **BUILT** |
| 8c | Save-anywhere / persistence | broader scope | checkpoint/level | server-authoritative serializable world | implied (not specced) |
| 8d | Cross-play / dedicated topology | broader scope | SP-focused | transport-agnostic dedicated servers | SPEC |

> Cross-reference: this maps onto `docs/IDTECH8_ROADMAP.md` **§9 (T3 "beyond")** for the J/K subsystem features (motion matching, full-body IK, GPU crowd ragdolls, structural-connectivity destruction, 1M+ debris, deterministic replay). §9 is the *subsystem-level* beyond set; this table is the *engine-strategy-level* one.

---

## 10. Where id Tech 8 is genuinely ahead of us TODAY (the honest to-do list)

This section keeps the doc credible. These are not tradeoffs in our favor — they are **real gaps** and **real targets**:

1. **Fully-dynamic GI quality & maturity.** id Tech 8 ships cinematic, WYSIWYG, real-time GI (Sousa's cascaded grids + irradiance volumes + path-traced cache) at a locked 60. **Our compute-GI (subsystem F) is NOT BUILT yet** — we have raster + shadows + SSAO + 16 point lights, not real-time GI. This is the biggest single rendering gap. *Target: build F (compute irradiance/probe GI) on the 1080 Ti floor.*
2. **Shipped-game polish & completeness.** id Tech 8 is in a finished, acclaimed AAA product. X3Native is a **graybox engine with one playable Level-1 slice**. Everything from content volume to bug-bar to feel is years behind a shipped title.
3. **Art-pipeline maturity.** id has virtualized geometry, mature material/asset pipelines, vista LOD authoring, and a battle-tested DCC→engine path. Ours is glTF/GLB + KTX2 + a CSG-bake plan (`I`) that's largely *ahead*, not built.
4. **Locked-60 frame-time discipline at AAA scale.** id's defining achievement is *stable* 60 fps (low variance) under heavy load. We have the *techniques* (GPU-driven, jobs, async) but have **not proven the discipline** at AAA content density — and our open-world scope makes it *harder*, not easier. Variance, not just average, has to become a gate (it's stated as one in K/J perf targets; we have to live it).
5. **Animation depth shipped.** id ships rich, GPU-skinned, high-count animation. We ship **J1 (CPU skinning + idle clip)**; the parity tiers (GPU compute skinning, active ragdoll, crowd VAT) are specced (J-spec T2/T3) but not built.
6. **Job-system spine.** id's "everything is tasks" is shipping and proven. Our fiber-based `IJobSystem` (subsystem A) is the spine *in progress* — terrain streaming already rides a job system, but the full scheduler is being designed/built.

**Net:** id Tech 8 is ahead on **execution and polish within its scope**; X3Native is broader in **scope and hardware reach** but *earlier* in maturity. Both statements are true at once.

---

## 11. Priority table (roadmap input)

Effort is rough (S/M/L/XL = days / weeks / months / quarters of solo-dev time). "Built" = shippable today; "Spec'd" = architecture locked, not coded; "Not started" = idea/intent only.

| Axis (do-better) | State | Rough effort to parity-or-beyond | Notes / dependency |
|---|---|---|---|
| Open-world streaming (§1) | **Built** | — (harden: biomes/texturing M) | self-test green; world art ahead |
| Procedural generation (§8b) | **Built** | — (extend: biomes) | substrate for §1/§2/§7 |
| Hybrid raster/compute floor (§3) | **Built** | — | the everywhere-path; 1080 Ti verified |
| Physics base for vehicles (§4) | **Built** | M (force controllers) | Jolt M3 done; K T0 adds shapes |
| **Compute GI — subsystem F (§10.1)** | Not started | **L–XL** | *highest-value rendering gap*; needs C/D done |
| Water / ocean (§4) | WIP | M | next engine-layer item |
| Netcode Phase 0 (§2/§7) | Spec'd | M | loopback split + fixed-step + net ids; cheap, do soon |
| Netcode Phase 1–2 co-op (§2) | Spec'd | L | snapshots, prediction, AoI on the grid |
| Netcode Phase 3–4 PvP/shard (§2) | Spec'd | XL | encryption, lag-comp, region servers |
| GPU destruction T2 (§5) | Spec'd | L | needs A + C/D + compute path |
| Structural destruction T3 (§5) | Spec'd | L | adjacency graph + connectivity pass; 5090 showcase |
| Lua/sol3 scripting (§6) | Spec'd | M | M4 ready; FFI + hot-reload |
| Animation parity T2 (§10.5) | Spec'd | L | GPU skinning + active ragdoll; needs C/D |
| Save-anywhere / persistence (§8c) | Not started | M | falls out of netcode serialization |
| Locked-60 discipline at scale (§10.4) | Ongoing | continuous | Tracy + variance-as-a-gate, every milestone |

**Suggested near-term ordering** (balancing value vs. unblock-cost): finish **renderer core C/D** → **netcode Phase 0** (cheap, prevents a future rewrite) → **compute GI (F)** (closes the biggest visual gap) → **animation T2** + **GPU destruction T2**, with **water** slotting in alongside per `docs/NOTE_TO_14900K.md`.

---

## 12. Open strategic questions for Tim

1. **GI is the credibility gap.** Subsystem **F** (compute GI on the 1080 Ti floor) is our single biggest rendering deficit vs id Tech 8 (§10.1). Do we prioritize it *before* breadth features (destruction, more MP), or is "looks great" deferrable behind "does more"?
2. **Netcode Phase 0 timing.** The spec's Phase 0 (loopback split, fixed-step sim, generation-tagged ids) is *cheap* and stops MP from ever being a rewrite — and it fixes the real index-recycling bug today. Do it alongside ongoing SP work *now*, or wait until Level-1-class content is locked? (Netcode §10.2.6 asks this too.)
3. **Scope honesty in marketing.** This doc deliberately frames most wins as *broader scope*, not *better*. Are you comfortable positioning X3Native publicly as "broader & hardware-spanning" rather than "beats id Tech 8" — i.e., competing on a different axis instead of head-to-head on GI/polish where they win today?
4. **Hardware-floor lifespan.** The 1080 Ti floor is the thing that *forces* our genuine hardware-breadth advantage (§3) but also *costs* us the fallback path id deleted. How long do we hold the no-RT-required floor? (It's a real recurring tax on the renderer.)
5. **Determinism scope.** Confirm we stay at single-machine fixed-step determinism + server-authoritative (not cross-machine lockstep) — the netcode spec strongly recommends this; it's worth an explicit owner sign-off before Phase 0 freezes `RepTransform`.

---

## Sources (public, clean-room)

- Bethesda Support — *DOOM: The Dark Ages PC System Requirements* (RT-capable GPU required; min RTX 2060 Super / RX 6600; tiers).
- Neowin / PCGamesN / Club386 / gamegpu — coverage confirming **no option to disable ray tracing** (lighting *is* RT) and pre-RTX cards unsupported.
- Tiago Sousa, *"Fast as Hell: idTech 8 Global Illumination,"* SIGGRAPH 2025 (Advances in Real-Time Rendering in Games) — public slides + recording: cascaded light grids, irradiance volumes / Ambient Probes, real-time path-traced irradiance cache, 60 Hz; used in Indiana Jones & The Great Circle (early) and DOOM: The Dark Ages.
- Digital Foundry — *Doom: The Dark Ages* DF deep-dive (id Tech 8: RT, physics, virtualized geometry; level-based fast-paced design; 60 fps).
- XDA, *"Exploring the gory brilliance of id Tech 8 in Doom: The Dark Ages"* — hands-on engine overview.
- **X3Native internal:** `docs/IDTECH8_ROADMAP.md` (pillars, D-RT/D-PHYS/D-JOB, §9 T3 set), `docs/RENDERING_SPEED.md`, `docs/NOTE_TO_14900K.md`, `docs/CONVENTIONS.md`, `docs/EFLZ_DESIGN.md`, `specs/NETCODE-architecture.spec.md`, `specs/K-gpu-destruction.spec.md`, `specs/J-character-animation.spec.md`, `specs/M4-script-vm.spec.md`, `app/terrain.h`/`app/terrain.cpp`, `engine/rhi/IRenderDevice.h`, `engine/physics/`, `PROVENANCE.md`, `README.md`.

> *No game-engine source code was read or referenced in producing this document. All id Tech 8 statements derive from the public sources above; all X3Native statements derive from this repository's own specs, docs, and code.*
