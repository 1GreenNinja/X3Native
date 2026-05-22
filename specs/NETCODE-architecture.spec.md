# Spec: Netcode & Networked Simulation Architecture (N)

> Written by the ENGINE ARCHITECT. Grounded in X3Native's OWN code + PUBLIC references ONLY.
> ❌ No GPL source, no transcribed function bodies, no RBDOOM/idTech/Doom/other-engine identifiers or paths below this line.
>
> **Clean-room basis:** X3Native's existing `app/scene.*`, `app/terrain.*`, `app/player.*`, `app/monster.*`, `app/main.cpp`, `engine/core/IJobSystem.h`, `engine/physics/IPhysicsWorld.h` + `JoltPhysicsWorld.cpp`, `docs/CONVENTIONS.md`, `docs/IDTECH8_ROADMAP.md`, the J & K specs; plus PUBLIC networking literature: Glenn Fiedler ("Gaffer on Games": *What Every Programmer Needs To Know About Game Networking*, *Networked Physics*, *Reliable Ordered Messages*, *State Synchronization*, *Snapshot Compression*), Valve's *Source Multiplayer Networking* developer docs (prediction, lag compensation, interpolation), the Overwatch GDC 2017 netcode talk (*"Overwatch Gameplay Architecture and Netcode"*, command-frame/ECS rollback), the deterministic-lockstep model from the "1500 Archers on a Pentium II" (Age of Empires) article, the Tribes networking model (priority/relevance-driven state replication), the Quake3 snapshot/delta model (public articles), and public rollback/GGPO material. No game-engine source was consulted.

- **Ledger ID:** subsystem **N** (netcode / networked simulation)
- **Defines interfaces:** `INetworkSystem`, `INetTransport`, `IReplication` (`engine/net/*.h`) + POD command/snapshot types + `NetEntityId`
- **Status:** SPEC (architecture + MP-ready foundations locked NOW; wire/transport built LATER, after SP gameplay is stable)
- **Spec author machine:** 13700K (clean-room) · **Decision owner:** Tim, 2026-05-21
- **Scope of MP goal (Tim):** *"all of it"* — single-player + co-op (PvE) + competitive PvP — for a **MASSIVE open world (~30,000 × 30,000 m)**.

---

## 0. THE #1 PRINCIPLE — always-on client/server, loopback by default (read this first)

**The engine ALWAYS runs the client–server architecture. There is exactly ONE code path, and it is multiplayer-shaped at all times — whether multiplayer is "enabled" or not.**

- **Single-player is not a special case.** SP = a **local server** + a **local client** in the same process, talking over an **in-process loopback transport**. The server simulates the world; the client sends input commands and renders replicated state — exactly as it would over a wire, except the "wire" is a function call / a queue in RAM.
- **"Enabling multiplayer" is a transport swap, nothing more.** You replace the loopback `INetTransport` with a real UDP `INetTransport`. The simulation, entity model, replication, input/command handling, and gameplay code **never know the difference** and are **never recompiled or branched** for MP vs SP.
- **Why (the whole point):** this *prevents SP/MP divergence*. There is no second, MP-only code path that drifts out of sync with the SP one and breaks at the worst time. Every gameplay system written against this architecture is **automatically MP-correct** because it was *never* written any other way. This is the Quake/Source model, and it is the foundation that makes "MP later, without a rewrite" actually true.
- **Authority is intrinsic, not bolted on.** Because the client already cannot do anything except *send commands and render what the server sends back*, server authority (§2) and anti-cheat posture come for free — they are not features added in PvP, they are how SP already worked.

> **Every section below is consistent with this principle.** The sim, the entity model, and the input system all assume client/server with loopback as the default transport. If any later design choice would only work in SP-mono or only in MP, it is wrong — fix the choice, not this section.

```
                       ┌──────────────────────────────────────────────────────┐
   SINGLE-PLAYER       │  one process                                          │
   (default)           │  ┌──────────┐   loopback INetTransport   ┌─────────┐  │
                       │  │  SERVER  │ <========================>  │ CLIENT  │  │
                       │  │ (sim,    │   commands ───────────────> │ (input, │  │
                       │  │  authority)  <─────────── snapshots     │  render)│  │
                       │  └──────────┘                             └─────────┘  │
                       └──────────────────────────────────────────────────────┘

   LISTEN SERVER       one process: local SERVER + local CLIENT (loopback)
   (host + play)       PLUS remote CLIENTS over UDP INetTransport ─────────────► [net clients]

   DEDICATED SERVER    server process, NO local client; all clients over UDP INetTransport
```

The ONLY thing that changes across those three rows is **which `INetTransport` implementation(s) the server accepts connections on**. The boxes labelled SERVER and CLIENT are byte-for-byte the same code.

---

## 1. Topology, the `INetTransport` seam, and the three deployment shapes

### 1.1 The split (today → target)

Today (`app/main.cpp`) there is **no** split: the loop reads GLFW input, calls `player.update(in, dt)`, then `physics->step(dt)`, then renders — all inline, with a **variable** `dt` (`dt = now - prev`, clamped to 0.1 s). Input → simulation → render are fused on one thread with no command boundary and no authority concept. *(This is the SP-mono shape we are replacing; it is fine as a bring-up but it is the divergence trap.)*

Target: factor the loop into three cooperating roles, all in-process by default:

- **Client** — owns: window, input sampling, the camera, prediction, interpolation, and **all** rendering (`IRenderDevice`), HUD, audio, FX. Produces **`NetCommand`** PODs (timestamped input). Consumes **snapshots**. Never mutates authoritative game state.
- **Server** — owns: the authoritative `Scene`/world, the fixed-step sim, `IPhysicsWorld` (Jolt), AI, gameplay rules, replication. Consumes `NetCommand`s, produces snapshots. Has **no** render dependency (a dedicated server links no `IRenderDevice`).
- **Transport** — `INetTransport`: a byte-pipe abstraction. Loopback (default) is one impl; UDP is another. Server and client only ever see `INetTransport`.

### 1.2 The transport seam

`INetTransport` is the single swappable boundary. **Loopback is just another `INetTransport` implementation** — not a fast-path, not a `#ifdef`. It moves command/snapshot byte buffers between the in-process server and client via lock-free queues (no sockets, no serialization-over-wire required — though it MAY still (de)serialize to exercise the same path; see §1.4). UDP is a second impl. A future replay/record impl (logs commands to disk, plays them back) is a third — which is exactly why the J-spec's deterministic anim replay and a netcode "demo system" are the same machinery.

### 1.3 The three deployment shapes (all the same code)

| Shape | Server | Client(s) | Transport(s) | Notes |
|---|---|---|---|---|
| **Single-player** (default) | local | one local | loopback only | EFLZ campaign runs here. Identical to a 1-player listen server. |
| **Listen server** (host & play) | local | one local **+** N remote | loopback (for the host) **+** UDP (for guests) | Co-op host. The host's own client uses loopback; guests use UDP. The server doesn't care which transport a given client arrived on. |
| **Dedicated server** | standalone process | N remote | UDP only | No local client, no `IRenderDevice` linked. PvP / large co-op / region servers (§5). |

The server accepts a **list** of transports and treats every connected client uniformly (a `ClientId`), regardless of whether it came in over loopback or UDP. That uniformity is what makes the listen-server case free.

### 1.4 Loopback discipline (the rule that keeps it honest)

Loopback MUST exercise the **same serialization + command/snapshot flow** as UDP, minus the socket and minus reliability/loss. Concretely: commands and snapshots are still built as the same POD byte buffers and pushed through the same `IReplication` encode/decode and the same apply path. Loopback simply delivers them **reliably, in-order, with ~0 latency**. If loopback skipped serialization or applied state directly, SP would silently diverge from MP and we would have re-created the trap. *(For perf, loopback MAY pass buffers by pointer instead of copying, but it goes through encode→decode so the data layout is identical.)* An optional dev cvar `net_loopback_simlag <ms> <loss%>` injects artificial latency/loss into loopback so prediction/interpolation/reconciliation can be developed and tested **in single-player**, before any real network exists.

---

## 2. Authority model — server-authoritative; one model for co-op and PvP

**The server is authoritative for all gameplay state. Clients send input commands; the server simulates and replicates results.** (Fiedler, *Game Networking*; Valve, *Source Multiplayer Networking*.)

- **Client → server:** a stream of `NetCommand`s (the player's intent for a tick: move axes, look angles, button edges — §4.3). Nothing else. The client cannot teleport itself, set its own HP, kill a monster, or open a door directly; it can only *ask* via a command.
- **Server → client:** snapshots of the entities relevant to that client (§4). Plus reliable events (damage numbers, deaths, level beats, pickups).
- **The client predicts** the local player (and only the local player) from its own commands for responsiveness (§6), and **reconciles** when the authoritative snapshot disagrees. Remote entities are **interpolated**, never predicted.

### 2.1 Co-op (PvE) and PvP share ONE model

There is no separate co-op vs PvP netcode. Both are "N authoritative clients connected to one authoritative server." The differences are *gameplay/relevancy policy*, not architecture:

- **Co-op (PvE):** clients are non-hostile to each other; the server is generous about what it accepts and trusting on timing (lag comp still applies to hitting *monsters*). Cheating mostly hurts the cheater's own session/host; anti-cheat posture is relaxed.
- **PvP:** the same server, with **strict** input validation, **lag-compensated** hit detection between *players* (§6.4), and a hardened anti-cheat posture. No new code path — just stricter validation thresholds and the lag-comp rewind buffer turned on for player-vs-player hits.

Because SP already routes every action through "client sends command → server validates → server simulates," **the validation hooks exist from day one** and are simply *exercised harder* in PvP.

### 2.2 Anti-cheat posture (built into the model, hardened by phase)

- **Never trust the client for state.** Position, HP, hits, cooldowns, ammo, line-of-sight — all server-decided. The client's job is presentation + intent.
- **Validate every command:** clamp move magnitude to the legal max, clamp look-rate, reject commands with implausible timestamps or out-of-order tick numbers, rate-limit commands per client per tick (a flood is dropped, not buffered unbounded).
- **Server-side reconciliation is the cheat catch:** if a client's predicted position drifts beyond a tolerance from the server's authoritative result, the server's value wins and the client is snapped/corrected (§6.3). A client that constantly mispredicts in its own favor is, by construction, ignored.
- **Encryption + auth at the transport (§7.6)** prevents packet forgery/replay; out of scope for the sim layer but a transport requirement before PvP ships.
- This is **posture, not a full anti-cheat product.** Statistical/behavioral detection, attestation, etc. are out of scope here; the architecture simply guarantees the *client can't directly write authoritative state*, which is the necessary foundation everything else builds on.

---

## 3. Simulation & tick model — deterministic fixed-step, decoupled from render

### 3.1 The core change: a fixed server tick, separate from render framerate

Today the sim advances at a **variable** `dt`. That is incompatible with prediction/reconciliation (you cannot re-run a tick you can't reproduce) and with replication (snapshots need a stable tick number). The server MUST run a **fixed simulation timestep**, decoupled from the render framerate. (Fiedler, *Fix Your Timestep!* / *Networked Physics*; the Overwatch "command frame" model.)

- **Server tick rate:** `kSimHz` = **60 Hz** to start (`kSimDt = 1.0f/60.0f`). This *aligns with Jolt's existing internal fixed step* (`kFixedDt = 1.0f/60.0f` in `JoltPhysicsWorld.cpp`), which is a gift: the physics world already steps at exactly this cadence. Make the server tick the authority for that cadence (see §3.4). Tick rate is a tunable (open decision §10) — 30 Hz with interpolation may suffice for the open world and halves bandwidth; 60 Hz favors PvP feel.
- **Server loop (fixed-step accumulator):**
  ```
  accumulator += realFrameDt           // wall-clock since last server iteration
  clamp accumulator to a max (e.g. 0.25 s) to avoid the spiral of death
  while accumulator >= kSimDt:
      drainCommandsForThisTick()       // apply each client's command for tick T
      serverTick(kSimDt)               // AI + gameplay + IPhysicsWorld::step(kSimDt)
      tickNumber++
      buildAndSendSnapshots()          // budgeted; not necessarily every tick (§4.4)
      accumulator -= kSimDt
  ```
- **Client loop (render rate, free):** samples input every render frame, accumulates it into the *current* command, sends commands at `kSimHz` (or a fixed `kCmdHz`), runs prediction at `kSimDt`, **interpolates remote entities** for smoothness, and renders at whatever the GPU delivers (the existing render path is unchanged). Render dt and sim dt are now fully independent — render at 144 Hz over a 60 Hz sim, or 60 over 30, etc.

### 3.2 Determinism stance — **authoritative-state + prediction, NOT cross-machine lockstep** (recommended)

This is a major fork. **Recommendation: do NOT target full cross-machine deterministic lockstep. Target server-authoritative simulation with client-side prediction + reconciliation (the snapshot model).** Reasoning, grounded in X3Native's actual systems:

- **Jolt is single-precision** (vcpkg `joltphysics`; flagged in `IDTECH8_ROADMAP.md` Open Decision #1). Single-precision float physics is **not bit-deterministic across machines** — different CPUs/compilers/SSE-vs-AVX/FMA-contraction settings produce divergent results, and the divergence compounds every tick. Lockstep requires *bit-identical* results on every machine forever; any drift desyncs the whole session. We cannot promise that with single-precision Jolt over a 30 km world.
- **Lockstep also scales poorly to our target.** Deterministic lockstep ("1500 Archers") sends only *inputs* and re-simulates the *entire world* on every client. For a ~30,000 × 30,000 m open world with streaming residency rings (each client only has nearby tiles resident), no client even *has* the whole world in memory — re-simulating all of it everywhere is impossible by design. Lockstep fits bounded RTS battlefields, not a massive seamless open world.
- **Authoritative-state + prediction is the right fit:** the server is the single source of truth, so cross-machine float determinism is **not required** for correctness — only the server's result matters, and the server is one machine. Clients predict locally and get corrected. This is the Quake/Source/Overwatch lineage and it's what scales to large, partially-resident worlds.

**Where determinism still matters (and we DO want it):**
- **Single-machine, fixed-step determinism** — same inputs + same fixed dt → same result *on the same build/machine*. This is achievable and valuable: it powers client prediction re-simulation (re-run ticks T..now identically), demo record/replay, the J-spec's "deterministic anim replay" (T16), and reproducible tests. The fixed-step sim (§3.1) is the prerequisite; it's already half-done because Jolt steps at a fixed dt internally.
- **Seeded determinism for procedural content** — the `TerrainConfig.seed` already makes `terrainHeightAt()` a pure deterministic function of `(worldX, worldZ, cfg)` (`app/terrain.*`). This means **the world is generated identically on server and every client from the seed** — terrain geometry is *not replicated*, it's *regenerated* from the shared seed (huge bandwidth win). Only *dynamic* state (entities) is replicated. AI/gameplay RNG must likewise be seeded and server-owned.

> **Net:** server-authoritative + prediction. Same-machine fixed-step determinism = yes (and we invest in it). Cross-machine bit-lockstep = no (Jolt single-precision + open-world scale make it the wrong tool). Revisit only if a future need (e.g. RTS-style sub-mode) demands it, and only with a fully deterministic fixed-point/double sim — a separate world type, not the default.

### 3.3 Coexistence with the job system (`IJobSystem`)

The server tick must cooperate with the engine's "everything is tasks" job system (`engine/core/IJobSystem.h`, fiber-based, D-JOB), not fight it:

- **The fixed sim step is the synchronization spine.** Within one `serverTick(kSimDt)`, work fans out onto `IJobSystem` (`parallelFor` AI updates, batched anim sampling per J-spec §8/§14, replication snapshot encoding per client) and **joins via `wait(counter)` before the tick advances**. The tick boundary is a hard barrier — no sim job may straddle two ticks, or determinism (§3.2) and snapshot consistency break.
- **Snapshot encoding is job-parallel:** one job per client (or per AoI cell, §5) builds that client's delta snapshot off the frozen post-tick state, signaling a counter the main thread waits on before handing buffers to the transport. This is read-only over a stable post-tick world, so it's safe to parallelize.
- **Terrain streaming already rides the job system** (`TerrainStreamer` submits gen jobs via `IJobSystem`, drains a thread-safe completion queue on the main thread, budgeted uploads). Server-side AoI relevancy (§5) reuses the **same** residency-ring computation, so interest management is nearly free — it's the streaming query the engine already runs.
- **No private thread pool for net.** Per D-JOB, the net system submits to the one scheduler. The transport's *socket I/O*, however, is blocking and goes on the **I/O lane** (`IJobSystem::runIO`), exactly like asset/streaming I/O — never parking a compute worker.

### 3.4 Coexistence with Jolt physics + the fixed step

Jolt's `step()` already runs an internal `1/60` accumulator (`JoltPhysicsWorld.cpp`: `kFixedDt`, `m_accumulator`, clamp at 0.25 s, `while (accum >= kFixedDt) Update(kFixedDt)`). This is *almost* what the server needs, but there's a subtlety:

- **Move the accumulator up to the server tick.** For prediction/reconciliation the server (and the predicting client) must step physics **exactly once per sim tick with exactly `kSimDt`**, deterministically, with no hidden internal sub-stepping that varies with wall-clock. **Recommendation:** make the server loop the owner of the fixed step, and have it call `IPhysicsWorld::step(kSimDt)` once per tick; the physics layer should then perform exactly one internal `Update(kSimDt)` (one-to-one), not its own variable accumulation. Either add a `stepFixed()` that does exactly one internal update, or guarantee the server always feeds exactly `kSimDt` so the internal `while` runs exactly once. *(This is a small, surgical `IPhysicsWorld` clarification, not a rewrite — see Phase 0.)*
- **Single-precision + huge world → origin rebasing (already chosen).** `IDTECH8_ROADMAP.md` Open Decision #1 already leans toward **camera-relative origin rebasing** over `USE_DOUBLE_PRECISION`. Netcode reinforces this: positions far from origin (up to ~21 km from center on a 30 km world) lose float precision badly; replicated positions should be **relative to a per-region/zone origin** (§5), not absolute world coordinates, both for precision *and* for bandwidth (smaller numbers quantize tighter, §4.5). The rebasing the renderer/physics already want is the same rebasing replication wants.
- **Visual-only physics is never replicated.** Per D-PHYS and the K-spec, the GPU debris world is **visual-only, one-way coupled**. It is **not** authoritative and **not** replicated (§6.5). Each client runs its own debris locally, seeded by replicated *break events*. Same for visual ragdolls (J-spec) past the authoritative death moment.

---

## 4. Networked entity & replication model

### 4.1 What exists today (the honest starting point)

`app/scene.h`: a `Scene` is a **flat `std::vector<Entity>`**; an entity's id **== its index** in that vector. `Entity` bundles render handles + a Jolt `BodyId` + a `transform[16]` + a `Tag` (None/Static/Prop/Door/Button/Weapon/Monster) + a 1:1 `link`. Game objects (player, monster, door, weapon, trigger) are **separate systems** (`player.*`, `monster.*`, `door.*`, …) that *reference* scene entity ids and Jolt bodies. **This is not a networked ECS and not even a stable-id store:**

- **The id is an array index, and indices are RECYCLED.** `TerrainStreamer` keeps a `m_freeEntities` free-list and reuses evicted tiles' scene slots. An index is therefore **not stable** — slot 5 can be a terrain tile now and something else after a stream-out/in. A network id MUST be stable for a replicated entity's lifetime; an array index is unusable as a net id.
- **State is scattered** across the per-system objects (HP in `Player`/`MonsterSystem`, door open-fraction in `door.*`, transform in `Scene`, physics in Jolt). There is no single "replicate this entity" surface.

### 4.2 Recommendation on the big fork: **formalize the scene layer into a thin networked entity registry NOW; defer a full ECS.**

Grounded in what exists, **do not** rip out the working `Scene`/systems to adopt a heavyweight ECS this phase. Instead add a **thin networked-entity layer beside the scene** that gives us the three things replication actually needs — *stable ids, an enumerable component set, and a replicate/apply surface* — without rewriting gameplay:

- **A `NetWorld` registry** owns `NetEntityId → record`, where a record holds the entity's archetype + a small set of **replicated component blocks** (POD) and back-references to the scene entity id / Jolt body / owning system. This is the authoritative store; the existing `Scene` becomes (largely) the **client-side render mirror** driven by replication.
- **Why thin-now, ECS-later:** the codebase is mid-flight on SP gameplay (EFLZ Level 1 shipping). A formal data-oriented ECS is a *good* eventual destination (it pairs naturally with the job-system "everything is tasks" pillar and with GPU-driven rendering), but adopting it *and* netcode at once is two rewrites stacked. The thin registry is the minimum that makes MP non-divergent; it is **forward-compatible** with a later ECS (the replicated component blocks are already SoA-friendly PODs, which is what an ECS wants). **Decision: thin networked registry in Phase 0/1; revisit "promote to full ECS" as its own subsystem after MP foundations are proven, if/when the scene layer's flat-array model becomes the bottleneck.**

### 4.3 `NetEntityId` and replicated components

```cpp
// engine/net/NetTypes.h — opaque ids + POD blocks; no lib types, mirrors BodyId/MeshHandle style
struct NetEntityId { uint32_t id = 0; bool valid() const { return id != 0; } };  // 0 == invalid
// id is a generation-tagged handle: low bits = slot, high bits = generation, so a
// recycled slot never collides with a stale id (the array-index reuse bug §4.1).

using NetTick     = uint32_t;     // server simulation tick number (monotone)
using ClientId    = uint32_t;     // a connected client (loopback or UDP), 0 == server/local
using NetArchetype= uint16_t;     // Player / Monster / Door / Projectile / Pickup / ...

// Replicated component blocks (POD, fixed layout, the unit of delta-encoding).
// Each block carries a dirty bit set on the server when a field changes.
struct RepTransform { float pos[3]; float rotQuat[4]; };          // pos relative to region origin (§3.4)
struct RepVelocity  { float lin[3]; float ang[3]; };               // for client extrapolation/interp
struct RepHealth    { int16_t hp; int16_t maxHp; uint8_t flags; }; // alive/iframe/etc.
struct RepAnimState { uint16_t clipId; uint16_t graphParamsPacked; float phase; }; // J-spec pose driver
struct RepDoor      { uint8_t state; float openFrac; };
// ... one small block per replicated aspect; archetypes declare which blocks they carry.
```

- **Stable, generation-tagged ids** solve the index-recycling bug. The server assigns a `NetEntityId` on entity creation; it is constant until despawn; the generation tag makes stale references detectable.
- **Per-component (not per-entity) dirty tracking** so a delta sends only what changed (a door that only opened sends `RepDoor`, not its transform). This is the Quake3/Tribes/Fiedler *state synchronization* model.
- **The transform component is authoritative-from-physics on the server:** `Scene::update()` already syncs transforms FROM Jolt; on the server that synced transform feeds `RepTransform`. On the client, replication writes `RepTransform` → the render mirror `Scene` (the client does *not* run authoritative Jolt for remote entities).

### 4.4 Snapshots — baseline + delta

(Fiedler *Snapshot Compression* / *State Synchronization*; Quake3 delta model.)

- **Baseline snapshot:** a full state of all entities relevant to a client, sent on join (and re-sent if the client falls too far behind / ACK chain breaks).
- **Delta snapshots:** thereafter the server sends, per tick (or per send-interval, §4.5), only the components that changed **since the last snapshot that client ACKed**. The server keeps a small ring of recent per-client snapshots and deltas against the client's last-acked baseline. This needs a reliable-enough ACK channel for *which snapshot* is the baseline (the data itself can be unreliable/most-recent-wins).
- **Reliable events** (death, damage applied, pickup, level beat, door SFX trigger, K-spec break events) ride a separate reliable-ordered channel (§7.3) — these are discrete and must not be lost, unlike continuous state which is "latest wins."
- **Client apply:** decode delta → patch the component blocks in the render-mirror `NetWorld`/`Scene` → existing systems render from it. Remote-entity transforms go into the interpolation buffer (§6.2), not applied raw.

### 4.5 Relevancy, priority, and bandwidth

- **Relevancy = area-of-interest from the streaming grid (§5).** A client only receives entities in/near its resident tile ring. This is the primary bandwidth control for a 30 km world and falls straight out of the existing `TerrainStreamer` residency computation.
- **Priority** within the relevant set (Tribes model): each entity has a per-client priority that rises with proximity, recent change, and gameplay importance (the local player's attacker > a distant idle monster). When a per-tick byte budget is hit, low-priority entities are simply sent **less often** (their priority accumulates until they're picked). No client is ever starved; nothing blocks the tick.
- **Quantization/compression:** positions quantized relative to region origin (§3.4) to a fixed grid (e.g. mm or cm precision is plenty), rotations as smallest-three quaternion (the J-spec already cites smallest-three for anim compression — reuse it), velocities at lower precision. Bit-pack changed-component masks. This is *snapshot compression* (Fiedler), not generic compression.

---

## 5. Interest management & scale — ride the streaming tile grid; shard for 30 km²

### 5.1 AoI replication on the residency ring (single server)

The `TerrainStreamer` (`app/terrain.*`) is the spatial backbone. It already maintains, per focus point, a **camera-centered Chebyshev residency ring** of tiles keyed by **signed `(gx,gz)`** packed into a 64-bit key, with stream-in ahead / stream-out behind, all on the job system. **Reuse this exact structure for replication relevancy:**

- **Cell → subscription mapping:** the unit of interest is the **tile cell** `(gx,gz)`. The server maintains, per cell, the set of `NetEntityId`s currently in it (an entity updates its cell when its tile-floor coords change — the streamer already computes `tileFloor(world)`). A client subscribes to the cells within its AoI radius (≈ its residency ring, possibly +1 ring of look-ahead). The client receives snapshots for entities in subscribed cells only.
- **Subscription handoff as players move:** when a client's focus crosses a tile boundary (the streamer already detects this via `m_lastFocusTX/TZ`), the server diffs the new cell set vs old: newly-entered cells → send baselines for their entities (entity "enters relevancy"); newly-left cells → stop sending, tell the client to drop/freeze those entities (entity "leaves relevancy"). This is the same enter/leave bookkeeping the streamer does for tiles, applied to entities.
- **Bandwidth scales with view, not world size** — a player in a 30 km world costs the same as a player in a small one, because both only ever see ~`(2R+1)²` cells.

### 5.2 Zone/shard topology for the massive world

A single server process cannot simulate the entire 30,000 × 30,000 m world for many players. Shard it spatially, reusing the same grid math:

- **Region servers.** Partition the world into **regions** (large blocks of the tile grid — e.g. a region = an N×N block of tiles, sized so one server handles its population). Each region server is authoritative for entities whose home cell is in its region. This is "many dedicated servers, each owning a slice of the same grid."
- **Seamless boundaries via border overlap + handoff.** Adjacent region servers **share a border band** (a ring of cells) so a player approaching a boundary already sees entities from the neighbor (the neighbor mirrors its border-band entities to adjacent regions, read-only). When a player crosses the boundary, ownership **migrates**: the source region serializes the player's authoritative entity state and hands it to the destination region (entity migration), the client is told to re-home (transparently, like an in-game zone transition with no loading screen if the band overlap is wide enough to cover transfer latency).
- **Entity migration** = serialize the entity's replicated component blocks + system-specific state, transfer over a **server↔server** transport (can be a reliable TCP/QUIC backplane, separate from the client UDP), re-instantiate on the destination with a **new local `NetEntityId`** but a stable **global entity GUID** so clients can correlate it across the handoff.
- **Why this works with the architecture:** because everything is already "client + server over `INetTransport`," a region server is *just another server*, and a player crossing regions is *just* a transport reconnect/handoff under the hood — the client/sim code is unchanged. Sharding is a Phase 4 concern; **nothing in Phases 0–3 prevents it** as long as ids are global-GUID-correlatable and positions are region-relative (§3.4).

> **Phase discipline:** §5.1 (single-server AoI on the residency ring) lands with co-op (Phase 2). §5.2 (multi-region sharding) is Phase 4 (massive scale) and is *designed-for now* (region-relative coords, global GUIDs) but built last.

---

## 6. Prediction, reconciliation, interpolation, lag compensation

(Valve *Source Multiplayer Networking*; Fiedler *Networked Physics*; Overwatch GDC 2017.)

### 6.1 Local-player prediction

- The client applies the **local player's** commands immediately to a **predicted copy** of the player state and steps it at `kSimDt` (using the *same* movement + `IPhysicsWorld` character code the server runs — this is why SP-via-loopback is so valuable: the prediction code IS the server code). The player feels zero input latency.
- The client keeps a ring of **unacknowledged commands** (each tagged with its `NetTick`).

### 6.2 Remote-entity interpolation

- Remote entities (other players, monsters, doors) are **never predicted** — they're **interpolated** between the last two received snapshots, rendered ~`interpDelay` (e.g. 100 ms / a few snapshots) **in the past**. This trades a little visual latency for perfectly smooth, jitter-free remote motion despite packet timing. `RepVelocity` allows short **extrapolation** to cover a missing snapshot before falling back.
- This maps cleanly onto the existing `Scene` render mirror: replication fills an interpolation buffer per remote entity; the client picks the interpolated transform each *render* frame (render rate ≠ snapshot rate, §3.1) and writes it to the mirror `Scene` entity for `Scene::render()`.

### 6.3 Server reconciliation + rollback of mispredictions

- The server's snapshot for the local player carries the **last command tick it processed** for that client. On receipt, the client:
  1. snaps the predicted player to the server's authoritative state **as of that tick**, then
  2. **re-simulates** (rolls forward) all still-unacknowledged commands after that tick, deterministically (single-machine fixed-step determinism, §3.2), to arrive back at "now."
- If the corrected result matches the prediction (the common case), nothing visible happens. If it diverges (a missed collision, a server-applied knockback), the player is corrected — smoothed over a few frames to avoid a visible snap. This is mispredict rollback; it requires the fixed-step, reproducible sim from §3.1.

### 6.4 Lag-compensated hit detection (rewind) — PvP

- The server keeps a short **history ring of entity transforms** (the last ~N ticks, ~0.25–1 s). When it processes a client's "fire" command, it knows the client's interpolation delay + RTT, **rewinds** the relevant entities to where they were on the *attacker's screen* when they fired, performs the hitscan/`rayCast` against that rewound state, then restores. So "I aimed dead-on, it should hit" holds even with latency. (Valve lag compensation.)
- This reuses `IPhysicsWorld::rayCast(... Layer mask)` — the same query the weapon already uses (`monster.cpp` `fire()` raycasts `Layer::Enemy`); lag comp just runs it against rewound transforms. The transform history ring is cheap and is the same data the snapshot system already records.
- **Co-op:** lag comp applies to player→monster hits for feel; PvP additionally applies it to player→player, with the strict validation of §2.

### 6.5 Interaction with GPU destruction (K) and animation (J) — visual vs authoritative

- **Authoritative (replicated):** the **break event** itself (a destructible reached its break threshold → it's broken), per K-spec's `BreakEvent`. The server decides *that* it broke and applies any gameplay consequence (cover removed, path opened, damage). This is a reliable event (§4.4).
- **Visual-only (NOT replicated):** the *thousands of resulting debris fragments* (K-spec GPU debris world) and *ragdoll physics* (J-spec) past the authoritative death frame. Each client spawns and simulates these **locally**, seeded deterministically from the replicated break event (position, impulse, material, seed). They will not be pixel-identical across clients — and **that is fine**, because they don't affect gameplay (D-PHYS one-way coupling: debris never feeds gameplay queries). This keeps destruction's massive entity counts entirely off the wire.
- **Animation:** the server replicates **animation *intent*/state** (`RepAnimState`: which clip/graph state + phase, plus the J-spec notify events that matter to gameplay like the melee hit-frame window). Each client runs the actual skinning/IK/blend locally (J-spec). The hit-frame *gameplay* effect is server-authoritative (the server decides the melee landed on its tick); the *visual* animation is client-local and driven by replicated state. The J-spec's **deterministic anim replay (T16)** and this replicated-anim-state model are the same foundation.

---

## 7. Transport — UDP + reliability, behind `INetTransport`

### 7.1 Why UDP, and the reliability we layer on it

Real-time games use **UDP**, not TCP: TCP's head-of-line blocking + retransmit-everything semantics are wrong for "latest state wins" traffic (Fiedler, *UDP vs TCP*). On top of UDP we need our own: connection handshake, sequencing, ACKs, selective reliability, fragmentation/reassembly (for snapshots > MTU), and congestion/flow control. (Fiedler, *Reliable Ordered Messages*; *Sending Large Blocks of Data*.)

### 7.2 Recommendation on the lib fork: **adopt a permissive networking library; do NOT hand-roll the wire.** Lean: **GameNetworkingSockets (GNS)**.

Grounded in our constraints (solo dev, closed-source shippable, MP-as-a-core-but-later goal):

- **GameNetworkingSockets (Valve, BSD-3)** — strongest fit: production-proven UDP reliability + congestion control, **encryption + authentication built in** (needed for PvP §7.6), message-based with both reliable and unreliable channels, P2P/relay-capable. BSD-3 is shippable closed-source. Heavier dependency (depends on a crypto lib + protobuf), but it solves §7.1 + §7.6 *and* §2's anti-forgery in one. **This is the recommendation.**
- **ENet (MIT)** — lightweight, dead-simple, reliable/unreliable channels, easy to vendor. Great for getting co-op working fast. **Lacks built-in encryption/auth** (you'd add it). Good **fallback / Phase-2 bring-up** choice if GNS integration friction is high; the `INetTransport` seam means we can start on ENet for co-op and swap to GNS for PvP without touching sim code.
- **yojimbo (BSD-3, Fiedler)** — purpose-built for exactly this (dedicated client/server, encrypted, packet-level), matches our references directly. Smaller community/maintenance than GNS. Reasonable alternative to GNS.
- **Hand-rolled** — *rejected for the wire.* Reliability + congestion + crypto is a deep, bug-prone time sink that adds zero game value; our value-add is the *replication model* on top, not the socket layer. (We DO hand-roll the loopback impl — that's trivial.)

> The choice is **firewalled behind `INetTransport`**, so it's reversible. Start co-op on **ENet** if it's faster to stand up; commit to **GNS** by the time PvP/encryption is needed. Either way, `JPH::`-style discipline applies: **no library types appear in `INetTransport.h`** — only opaque connection ids + byte buffers.

### 7.3 `INetTransport` channels

- **Unreliable / sequenced** — snapshots (latest wins; old ones are useless, never retransmit).
- **Reliable / ordered** — commands' reliability is "most-recent matters but don't lose the *stream*" (commands are small and batched with redundancy: each packet re-includes the last few commands so a single drop doesn't lose a tick of input — Fiedler/Quake input redundancy). Reliable-ordered is also used for discrete events (§4.4) and the connection/control messages.
- **Fragmentation/reassembly** for snapshots exceeding the path MTU (target keeping per-packet payload under ~1200 bytes to live within typical MTU and avoid IP fragmentation; baselines fragment, deltas should usually fit).

### 7.4 Loopback impl

Trivial: two lock-free SPSC queues (server→client, client→server) of byte buffers, delivered next drain with zero loss/latency (plus optional injected sim-lag/loss, §1.4). Implements the *same* `INetTransport` interface. This is the default and the bring-up vehicle for everything in §6 before any socket exists.

### 7.5 Congestion / flow control

Adaptive send rate: monitor RTT + loss; back off snapshot frequency / drop to lower-priority-only under congestion (the §4.5 priority system is the throttle). Never let the server's send queue grow unbounded for a slow client — degrade that client's update rate, don't stall the tick.

### 7.6 Encryption & auth (PvP gate)

Per-connection encryption + authenticated handshake (prevents packet forgery, replay, and trivial man-in-the-middle command injection — a hard requirement before competitive PvP, §2.2). **GNS provides this in-box**; with ENet we'd layer a permissive crypto lib. Server validates client identity at connect; session keys per connection. This is a transport-layer concern and never leaks into the sim/replication layers.

---

## 8. Clean engine interfaces (X3Native style: opaque handles, no lib types in headers)

All in `engine/net/`, mirroring the established pattern (`IRenderDevice`/`IPhysicsWorld`/`IJobSystem`/`IAudioSystem`: pure-virtual interface + `createX()` factory + `runXSelfTest()`; POD structs + opaque handle structs; **no** third-party types in public headers).

```cpp
// engine/net/NetTypes.h — POD + opaque handles only (see also §4.3)
namespace x3::net {
struct NetEntityId  { uint32_t id = 0; bool valid() const { return id != 0; } }; // generation-tagged
struct ClientId     { uint32_t id = 0; bool valid() const { return id != 0; } }; // 0 == local/server
struct ConnectionId { uint32_t id = 0; bool valid() const { return id != 0; } }; // transport-level
using  NetTick = uint32_t;

enum class NetChannel : uint8_t { UnreliableSequenced = 0, ReliableOrdered = 1 };

// Timestamped input command — the ONLY thing a client sends to drive the sim.
// Mirrors PlayerInput (app/player.h) but is a wire-POD with a tick stamp.
struct NetCommand {
    NetTick tick;            // sim tick this command is FOR
    float   moveFwd;         // -1..1
    float   moveStrafe;      // -1..1
    float   yaw, pitch;      // absolute look angles (radians; CONVENTIONS.md basis)
    uint32_t buttons;        // bitfield: jump/sprint/use/fire/melee... (edges resolved server-side)
};
}
```

```cpp
// engine/net/INetTransport.h — the swappable seam (loopback OR udp); NO lib types
namespace x3::net {
class INetTransport {
public:
    virtual ~INetTransport() = default;
    virtual bool start(const char* bindOrConnect, bool asServer) = 0; // loopback ignores the addr
    virtual void shutdown() = 0;
    // Connection lifecycle (server side gets accepts; client side gets one connect).
    virtual uint32_t pollEvents(struct NetEvent* out, uint32_t maxOut) = 0; // connect/disconnect
    // Byte-buffer send/recv on a channel; the net system owns (de)serialization.
    virtual bool send(ConnectionId, const void* bytes, uint32_t len, NetChannel) = 0;
    virtual uint32_t recv(ConnectionId, void* outBytes, uint32_t maxLen) = 0; // 0 == nothing
    virtual void flush() = 0;          // push queued sends (per server tick)
    // Dev knobs (loopback + udp): artificial latency/loss for testing prediction in SP.
    virtual void setSimulatedConditions(float latencyMs, float lossPct) = 0;
};
INetTransport* createLoopbackTransport();
INetTransport* createUdpTransport();    // wraps GNS/ENet internally; no lib types here
}
```

```cpp
// engine/net/IReplication.h — encode/apply snapshots; server + client sides
namespace x3::net {
class IReplication {
public:
    virtual ~IReplication() = default;
    // --- server side ---
    virtual NetEntityId spawn(NetArchetype, ClientId owner) = 0;  // owner 0 == server-owned (AI/world)
    virtual void        despawn(NetEntityId) = 0;
    virtual void        markDirty(NetEntityId, uint16_t componentMask) = 0;
    virtual void        setComponent(NetEntityId, uint16_t componentId, const void* pod, uint32_t len) = 0;
    // Build a per-client delta vs that client's last-acked baseline (job-parallel, §3.3).
    virtual uint32_t    encodeSnapshot(ClientId, NetTick, void* outBytes, uint32_t maxLen) = 0;
    virtual void        onClientAck(ClientId, NetTick acked) = 0;
    // AoI: which cells does this client subscribe to (from the streaming ring, §5).
    virtual void        setClientInterest(ClientId, const uint64_t* cellKeys, uint32_t count) = 0;
    // --- client side ---
    virtual void        applySnapshot(const void* bytes, uint32_t len, NetTick* outServerTick) = 0;
    virtual const void* readComponent(NetEntityId, uint16_t componentId, uint32_t* outLen) const = 0;
};
IReplication* createReplication();
}
```

```cpp
// engine/net/INetworkSystem.h — the facade that wires transport + replication + tick
namespace x3::net {
enum class NetRole : uint8_t { LocalLoopback, ListenServer, DedicatedServer, RemoteClient };
class INetworkSystem {
public:
    virtual ~INetworkSystem() = default;
    virtual bool init(NetRole, INetTransport*, IReplication*) = 0;  // SP passes a loopback transport
    virtual void shutdown() = 0;
    // Server: drain commands for `tick`, hand them to the sim, then publish snapshots.
    virtual uint32_t serverDrainCommands(NetTick tick, NetCommand* out, uint32_t maxOut, ClientId* outWho) = 0;
    virtual void     serverPublish(NetTick tick) = 0;
    // Client: submit this frame's command; pull the latest applied server tick.
    virtual void     clientSubmitCommand(const NetCommand&) = 0;
    virtual NetTick  clientLastServerTick() const = 0;
    virtual bool     isServer() const = 0;
    virtual bool     isClient() const = 0;
};
INetworkSystem* createNetworkSystem();
bool runNetworkSelfTest();   // mirrors runPhysicsSelfTest()/runJobSystemSelfTest()
}
```

> Note the deliberate symmetry with `PlayerInput` (`app/player.h`): `NetCommand` is its wire form. The existing `Player::update(const PlayerInput&, dt, physics)` becomes the server-side application of a decoded `NetCommand` AND the client-side prediction step — **one function, two callers**, which is the §0 principle made concrete.

---

## 9. Phased plan — bake in NOW vs build LATER

> **Guiding rule:** Phase 0 changes the *shape* of existing SP code so MP is never a rewrite, while keeping SP fully working at every step. Later phases add capability without re-shaping.

### Phase 0 — Foundations to add SOON (so nothing is rewritten later)
*Goal: SP runs unchanged in behavior, but through MP-shaped seams. NO wire yet.*

1. **Client/server split with a loopback transport.** Refactor `app/main.cpp`'s fused loop into `serverTick()` + `clientFrame()` communicating via `createLoopbackTransport()`. SP = one local client + one local server. Behavior identical; structure MP-shaped.
2. **Deterministic fixed-step sim, decoupled from render.** Replace the variable-`dt` sim with the §3.1 accumulator at `kSimHz=60`; render stays at GPU rate. Make `IPhysicsWorld` step **exactly once per tick at `kSimDt`** (the §3.4 surgical clarification).
3. **Command input.** Introduce `NetCommand`; have the client build it from GLFW (replacing direct GLFW→`player.update`), send via loopback; the server applies it through the existing `Player::update`. Resolve button **edges** server-side from the `buttons` bitfield.
4. **Net entity ids + replicated-state convention.** Add `NetEntityId` (generation-tagged — **fixes the index-recycling bug §4.1**) + the thin `NetWorld` registry beside `Scene`; define the `Rep*` component blocks for the entities that already exist (player, monster, door, pickup). Server writes them post-tick; in SP the loopback "replication" just hands them to the same `Scene` render mirror.

- **Dependencies:** the job system (A) is helpful but not required for Phase 0 (loopback + single client). Jolt step clarification touches `IPhysicsWorld`/`JoltPhysicsWorld.cpp` only.
- **Acceptance:** EFLZ Level 1 plays **identically** through the loopback client/server split; a headless `runNetworkSelfTest()` drives synthetic `NetCommand`s through server tick → snapshot → client apply and reproduces a known end state; same inputs + fixed dt → identical sim result across two runs (single-machine determinism); `net_loopback_simlag` can inject latency and SP still plays (proving the seam is real).

### Phase 1 — Loopback replication + prediction skeleton
*Goal: the full snapshot + prediction pipeline working entirely over loopback (still one machine).*

1. Baseline+delta snapshot encode/decode (`IReplication`), per-component dirty tracking, ACK-based baselines.
2. Local-player prediction + server reconciliation/rollback (§6.1, §6.3) — develop & verify using `net_loopback_simlag` to fake latency.
3. Remote-entity interpolation buffer (§6.2) — exercised by a second *local* simulated client over loopback.
4. Replicated `RepAnimState` driving J-spec playback; replicated K-spec break events with client-local debris (§6.5).

- **Dependencies:** Phase 0; J-spec T0/T1 for anim state; K-spec break events (can stub).
- **Acceptance:** with injected 100 ms/5% loopback conditions, the local player feels lag-free (prediction) and a second simulated client's avatar interpolates smoothly; mispredicts reconcile without visible snaps; a break event reproduces debris on the "remote" view from the seed.

### Phase 2 — Real transport + co-op (PvE)
*Goal: actual machines, 2–4 players, shared PvE world.*

1. `createUdpTransport()` (ENet to start, or GNS) behind the unchanged `INetTransport`.
2. Listen-server (host's client on loopback + guests on UDP) and a headless dedicated-server build (no `IRenderDevice` linked).
3. AoI replication on the **streaming residency ring** (§5.1): per-cell entity sets, subscription handoff on tile-boundary crossings.
4. Lag-compensated player→monster hits (§6.4); command redundancy + fragmentation for snapshots (§7.3).

- **Dependencies:** Phase 1; the streamer (`TerrainStreamer`, exists); job system (A) for parallel snapshot encode + AI; transport lib chosen.
- **Acceptance:** 4 clients in one streamed world over a real LAN; each receives only AoI entities; a guest walking across tile boundaries gets correct enter/leave of remote entities; bandwidth per client is bounded by view, not world size; hits feel correct at ~80 ms RTT.

### Phase 3 — PvP (competitive)
*Goal: hardened, fair, cheat-resistant player-vs-player.*

1. Strict command validation + rate limiting (§2.2); lag-compensated player→player hits with the rewind ring (§6.4).
2. Encryption + authenticated connections (§7.6) — commit to GNS here if not already.
3. Mispredict-rollback tuning for fairness; anti-cheat posture pass (server never trusts client state — audit every gameplay path for accidental client authority).

- **Dependencies:** Phase 2; encryption-capable transport; the reconciliation/rewind systems from Phases 1–2.
- **Acceptance:** two players, one with injected latency, both hit "what they aimed at"; a tampered client (forged position/HP) is rejected/corrected with no gameplay advantage; encrypted sessions; no client can mutate authoritative state.

### Phase 4 — Sharding for massive scale
*Goal: the full 30 km² world across many region servers, seamless.*

1. Region partition over the tile grid; region-relative coordinates everywhere (already true if §3.4 followed); global entity GUIDs.
2. Border-band mirroring between adjacent regions; entity migration on boundary crossing (§5.2) over a server↔server backplane.
3. Seamless client re-home across regions (no loading screen) under the band-overlap latency budget.

- **Dependencies:** Phases 2–3; region-relative coords + global GUIDs in place from earlier phases; a server↔server transport.
- **Acceptance:** a player walks across a region boundary with no hitch and no loss of nearby entities; entity ownership migrates correctly; population beyond a single server's budget is sustained across regions.

---

## 10. Determinism foundation + risks / open decisions for Tim

### 10.1 Determinism foundation (what we commit to and why)
- **Single-machine fixed-step determinism: YES** — same inputs + same fixed `kSimDt` → identical result on the same build/machine. Powers prediction re-sim (§6.3), demo record/replay, the J-spec deterministic anim replay (T16), and reproducible tests. Prerequisite = the §3.1 fixed step (Phase 0).
- **Seeded procedural determinism: YES, already true** — `terrainHeightAt()` is a pure function of `(world, cfg.seed)`; terrain is **regenerated from the seed on every machine, not replicated** (massive bandwidth win). Extend the same discipline to all gameplay RNG (server-owned, seeded).
- **Cross-machine bit-lockstep: NO** (see §3.2) — Jolt single-precision + open-world partial residency make it the wrong model. Authoritative-state + prediction instead.

### 10.2 Open decisions for Tim
1. **Tick rate (`kSimHz`).** 60 Hz (PvP feel, aligns with Jolt's internal `kFixedDt`) vs 30 Hz (half the bandwidth, fine for a streamed open world with interpolation). *Lean: 60 for now (free alignment with Jolt); profile and consider 30 for the open-world/co-op mode.*
2. **Jolt precision / origin rebasing (already flagged, roadmap Open Decision #1).** Netcode *reinforces* "keep single-precision + camera/region-relative origin rebasing" over `USE_DOUBLE_PRECISION` — but rebasing must now be **region-relative for replication** (§3.4), not just camera-relative for rendering. *Decision needed: pin the rebasing origin scheme (per-region) before Phase 0's `RepTransform` layout is frozen.*
3. **ECS vs thin registry.** Recommendation §4.2 is "thin networked registry now, full ECS later as its own subsystem." *Confirm we're NOT doing a full ECS this phase* (I recommend not — two rewrites at once).
4. **Transport lib.** Recommendation §7.2: ENet for co-op bring-up → GNS by PvP (for built-in encryption/auth). *Or commit to GNS from the start if you'd rather not migrate.* yojimbo is a viable single-choice alternative.
5. **`IPhysicsWorld` step contract.** Add a `stepFixed()` (exactly one internal `Update(kSimDt)`) vs guaranteeing the caller always feeds exactly `kSimDt`. *Lean: explicit `stepFixed()` — unambiguous for prediction re-sim.* Small surgical change (Phase 0).
6. **When to actually start Phase 0.** Tim's stated plan is "architecture locked now, wire later, after SP gameplay is stable." Phase 0 is the part that should land *soon* (it's cheap and keeps SP working) precisely so MP is never a rewrite; Phases 1–4 wait. *Confirm the timing: do Phase 0 alongside ongoing SP work, or after Level-1-class content is locked?*

### 10.3 Honest caveats / risks
- **Jolt single-precision is the central risk for a server-authoritative massive world.** Even server-only (one machine), positions far from origin jitter and lose precision; the *server's own* sim quality degrades near the world edge. Region-relative origins (§3.4) are the mitigation, but they add bookkeeping (cross-region coordinate transforms, migration). If precision proves insufficient even region-relative, the fallback is `USE_DOUBLE_PRECISION` for the authoritative server world (perf cost) — a per-build choice the interface already hides. **This is the thing most likely to bite; pin the rebasing scheme early (§10.2.2).**
- **Float precision over a 30 km world** (renderer + physics + replication) is the same root issue surfacing in three places; solve it once with a region-relative origin convention and apply it everywhere.
- **Tickrate vs bandwidth vs feel** is a triangle; the AoI/priority system (§4.5, §5) is what keeps bandwidth bounded, but per-client send budgets, snapshot frequency, and quantization precision all need profiling against real player counts — numbers here are starting points, not measured.
- **MTU / fragmentation:** keep per-packet payload < ~1200 B; baselines (which fragment) must be rare (join / desync recovery), deltas should fit in one packet — verify under worst-case AoI density (a busy cell).
- **The index-recycling bug (§4.1) is a real latent defect** even for SP demo/replay: scene entity ids are not stable across streaming. Generation-tagged `NetEntityId` (Phase 0) fixes it and is worth doing regardless of MP.
- **Determinism scope creep:** resist letting "deterministic" mean "cross-machine." It means single-machine-reproducible here; conflating the two leads back toward lockstep and the Jolt trap.
- **Clean-room integrity:** every model above traces to the public sources cited in the header (Fiedler, Valve docs, Overwatch GDC, Quake3/Tribes/AoE public articles, GGPO material) and to X3Native's own code. No game-engine source consulted; honors LICENSE + PROVENANCE.md.

---

## 11. Public references
- Glenn Fiedler ("Gaffer on Games"): *What Every Programmer Needs To Know About Game Networking*, *UDP vs. TCP*, *Reliable Ordered Messages*, *Sending Large Blocks of Data*, *Snapshot Interpolation*, *Snapshot Compression*, *State Synchronization*, *Networked Physics*, *Fix Your Timestep!*
- Valve Developer Community: *Source Multiplayer Networking* (prediction, lag compensation, entity interpolation), *Latency Compensating Methods in Client/Server In-game Protocol Design* (Bernier).
- "Overwatch Gameplay Architecture and Netcode," GDC 2017 (command frames, rollback, high-bandwidth/predictive model on an ECS).
- "1500 Archers on a Pentium II" (Bettner & Terrano) — deterministic lockstep model + its constraints (why it's NOT used here).
- Quake3 networking (public articles): snapshot + delta-against-acked-baseline model.
- Tribes networking model (public GDC/paper material): priority/relevance-driven state replication.
- Rollback / GGPO public material: prediction + rollback for input-deterministic games (contrast to server-authoritative).
- Jolt Physics docs (single- vs double-precision, fixed-step `Update`) — for the determinism caveats.
- X3Native internal: `docs/CONVENTIONS.md` (coords/quat), `docs/IDTECH8_ROADMAP.md` (D-JOB, D-PHYS, Open Decision #1 origin rebasing), `specs/J-character-animation.spec.md` (deterministic replay, anim state), `specs/K-gpu-destruction.spec.md` (one-way visual coupling, break events).

## 12. Suggested permissive libraries (clean IP, shippable closed-source)
- **GameNetworkingSockets** (Valve, BSD-3) — UDP reliability + congestion + **encryption/auth** in one. *Recommended primary.*
- **ENet** (MIT) — lightweight reliable/unreliable UDP; great for co-op bring-up; no built-in crypto. *Recommended bring-up / fallback.*
- **yojimbo** (BSD-3, Fiedler) — dedicated client/server packet lib with encryption; matches the references directly. *Alternative single-choice.*
- All hidden behind `INetTransport` — **no library types in any `engine/net/*.h` header** (same discipline as `IPhysicsWorld` hiding `JPH::`).
