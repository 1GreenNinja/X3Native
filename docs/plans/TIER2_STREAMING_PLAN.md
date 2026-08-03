# TIER 2 — True WorldStreamer Adoption in Echotropolis

Authored 2026-07-26 by the Fable-5 architect fork, against commit `b9e9b652`
(branch `echotropolis`). Every variable name, coordinate, and API below was
read from the code this morning — not from memory.

## 0. Intent and ground truth

Echotropolis today: **everything builds synchronously at boot and draws every
frame** — the ~33 ms draw-record cost. Content is host-owned `EnvArtSystem`
instances drawn via explicit `->draw()` fans in `host_echotropolis.cpp`
(4,360 lines), **not** Scene entities, so the WorldStreamer's Scene ledger
cannot own it directly. The streamer (`app/world_stream.h/.cpp`) still gives us
everything else we want: distance wants + hysteresis + neighbor warming +
velocity lookahead, a per-frame ms budget, nearest-first realize, chunked
evictions, the proxy floor, and — the key integration point —
`setRegionHooks(onBuild, onTeardown)` which brackets host-owned content into
the region lifecycle (built for CrowdSkin; we extend the same contract to
EnvArt containers).

**Adoption shape**: the WorldStreamer runs as the **residency engine** (wants /
budget / hysteresis / proxy). Echotropolis regions carry **no LevelDoc and no
Scene-ledger content** — every region's content lives in an `EchoRegion`
container built/evicted through the hooks. The ledger machinery runs empty and
is harmless. This is the honest fit; do not force EnvArt into the ledger.

Two engine facts that shape milestones:
- `EnvArtSystem` has **no GPU teardown path** (no destructor releasing meshes —
  verified by grep of env_art.h/.cpp). Until WP-4 adds `destroy(device)`,
  "evict" for EnvArt means **deactivate: stop drawing/updating, keep VRAM**.
  That already wins the frame time (records are the cost, not memory — 5090).
- The terrain collision mesh (host line ~2664: `N=512, EXT=2600` →
  `phys->addStaticMesh`) and the `Heightfield hf` are **persistent forever**.
  Streaming them would drop the player through the world. Never regionize.

## 1. Region table

Anchors/elevations from the code. `loadRadius` is measured from the footprint
EDGE (streamer convention). Neighbors get a 1.5× load-radius warm
automatically; lookahead default 2.5 s.

| id | anchor (x,y,z) | footprint R | load / unload | owns (host variable names) |
|---|---|---|---|---|
| `crown` | (-20, 195, 760) | 500 | 650 / 900 | `towers` (37 Urban Night City), `houses` (5 hero + ~40 ring drape, rings to r≈441), `condos` (3 condo stacks z=842), `infra` + `subwayTrain` (metro + crown road decks), `hackProps`/`hackDrone`/`vtolPolice`, `streetProps` (Meshy vendor carts), `streetLamps` + `lampScene` (draw-gated whole Scene), `drones` (patrol circles over crown), crown slice of `districtLights` |
| `west_shoulder` | (-480, 195, 850) | 220 | 500 / 750 | `mineProps` (mine_site kMineX/Z=-480/850, truck lot kLotX/Z=-556/814), `mineForest` (per-tree EnvArts, R=175 circle), `mineGlowScene` (draw-gated), `beam` (lighthouse -493.24, -0.156, 789.39 — sea-level west coast, inside this footprint), miners crew re-attach (see lanes) |
| `district_urban` | (700, ~35, 350) | 240 | 650 / 900 | the "URBAN DISTRICT" element of `districts` (pad 700,350; keep-out rect 540..860 × 190..510) + its `districtLights` slice |
| `district_recife` | (950, ~35, 1250) | 240 | 650 / 900 | the "RECIFE 2050" element of `districts` (pad 950,1250) + lights slice |
| `district_hivemind` | (1340, ~35, 1000) | 240 | 650 / 900 | the "HIVEMIND CYBERCITY" element of `districts` (pad 1340,1000, yOff -23) + lights slice |
| `harbor_bay` | (200, 0, 300) | 850 | 500 / 800 | `boats` (3 lanes: south bay eastbound -400,330→+x len 760; westbound 340,240; SW inlet -560,260 northbound) + their pose updates |
| `woodlands_NW/N/NE/W/C/E/SW/S/SE` (9 cells) | 3×3 grid over island land (−800..1600 x, 100..1500 z → cell ≈ 800×466) | per-cell half-diagonal | 600 / 850 | the `woodlands` scatter REBUILT as 9 cell-local EnvArtSystems (WP-3; union bit-identical to today's single system) |
| — persistent (never a region) — | | | | `island`, `props` (world-baked island-wide coast GLB), `fissure` (fjord glow, world-baked), `hf` + terrain collision mesh, ocean/water, `ufo`+`ufoFx` (patrols at y=780 R=520 — visible island-wide, Tim's hero prop), `helis`/`planes` (wide sky routes), `cars` (drive the full freeway loop kRoute −160..1060 × 420..1120 — movers cross regions), freeway/road segments of `infra` outside the crown, `subway` train pose, `placed` (player build-menu lots — player content is sacred), `walkScene` + `residents`/`residentsSkin` + `npcLife`/`npcSkin` + talk/LLM, audio, HUD/console, DDGI/atmosphere state |

Elevation note: district pads sit in the east flats bowl; use
`hf.heightAt(anchor)` at runtime for anchor Y rather than the table's ~35.

## 2. Lifecycle lanes (every system, exactly one lane)

**Lane A — Scene ledger (streamer-owned):** *empty in Echotropolis.* No region
carries LevelDoc or builder content. Keep the lane wired (it costs nothing)
so future content (region interiors) can use it.

**Lane B — EchoRegion container (hook-owned EnvArt + per-region Scenes):**
`towers, houses, condos, infra(crown), subwayTrain, hackProps, hackDrone,
vtolPolice, streetProps, mineProps, mineForest, beam, districts[3],
districtLights (per-region slices), boats, woodlands cells, lampScene,
mineGlowScene, drones`. Built in `onBuild`, deactivated in `onTeardown`
(milestone C), truly destroyed via `EnvArtSystem::destroy` (milestone D).

**Lane C — persistent host pools (NEVER torn down):** `island, props, fissure,
hf, terrain collision, ocean, ufo/ufoFx, helis, planes, cars, freeway infra,
placed, walkScene, player, flyCam, HUD, console, audio, talk/LLM`.
Crowd layers: `residentsSkin`/`minersSkin` (existing `deactivate()`/`build()`
zero-reload re-attach), `npcSkin` (WP-4 adds the same contract), and their
brains `residents`/`npcLife`/`miners` stay persistent (parked-cars doctrine:
the rigged-GLB pools and shared meshes must never enter any teardown).
Miners crew: on `west_shoulder` teardown call `minersSkin.deactivate(walkScene)`
and stop `miners.update`; on build, re-`build()` re-attaches the same pool.

## 3. Architecture (new code)

```cpp
// app/world_hosts/echo_regions.h  (WP-1)
namespace x3::game {

struct EchoRegionCtx {                  // everything a builder needs, no host lambdas
    x3::rhi::IRenderDevice& device;
    Heightfield&            hf;
    Scene&                  walkScene;  // for systems that need it
    // asset roots (island/models/districts dirs, assetRoot()) resolved once
    std::string modelsDir, districtsTxt, vegDir, houseForgeDir, cityDir;
};

class EchoRegion {                      // one region's hook-owned content
public:
    // Register content + per-frame hooks. draw() is skipped when !resident.
    void addArt(std::unique_ptr<EnvArtSystem> e);
    void setUpdate(std::function<void(float dt, float t)> fn);   // poses (boats…)
    void setScene(Scene* s);            // optional draw-gated whole Scene (lampScene)
    void addLights(std::vector<x3::rhi::PointLight> l);          // region light slice
    void build(EchoRegionCtx&, const std::function<void(EchoRegion&, EchoRegionCtx&)>& builder);
    void deactivate();                  // stop draw/update; keep GPU memory (M-C)
    void destroy(x3::rhi::IRenderDevice&);   // true GPU free (M-D, needs WP-4)
    void draw(x3::rhi::IRenderDevice&, const x3::rhi::FrameContext&, const Scene& gate);
    void update(float dt, float t);
    bool resident() const;
    const std::vector<x3::rhi::PointLight>& lights() const;
};

class EchoRegionSet {                   // the table + WorldStreamer bridge
public:
    void init(EchoRegionCtx&, WorldStreamer&, const WorldRegionGraph&);
    // registers builders per region id; wires streamer.setRegionHooks to
    // build/deactivate the matching EchoRegion.
    void forceAllResident(EchoRegionCtx&);            // milestone A / vista mode
    void drawAll(x3::rhi::IRenderDevice&, const x3::rhi::FrameContext&);
    void updateAll(float dt, float t);
    void appendNearLights(float ex, float ez, std::vector<x3::rhi::PointLight>& out,
                          uint32_t budget);           // replaces appendDistrictLights
    // vista rule (see §5): when true, wants are overridden to all-resident.
    void setVistaMode(bool);
};
} // namespace
```

The probe fed to `WorldStreamer::update` is the **active camera** position +
velocity (fly/walk/ride/orbit), not the physics player (§5, Decision 1).

## 4. Work packages (Sonnet-5 agents; zero file overlap)

Rules for every agent: never edit `app/world_hosts/host_echotropolis.cpp`,
never edit `app/CMakeLists.txt`, **never run cmake** (WP-0 integrates and
builds). You may READ anything. Copy code from the host verbatim where
directed — the integrator deletes the originals afterward (copy-then-delete
keeps single-writer).

**WP-1 — region core** *(files: `app/world_hosts/echo_regions.h`,
`app/world_hosts/echo_regions.cpp`)*
Implement §3 exactly: EchoRegion / EchoRegionSet / EchoRegionCtx, the
WorldStreamer hook bridge (`setRegionHooks` → build/deactivate by id), vista
override, light slices, draw/update gating. No content knowledge. DONE: header
compiles standalone (agent may verify with a `cl /c`-style syntax check only,
no cmake); every §3 signature present; hook bridge covered by comments naming
the streamer contract lines it relies on.

**WP-2 — region builders** *(files: `app/world_hosts/echo_region_builders.h`,
`app/world_hosts/echo_region_builders.cpp`,
`assets/world/regions.echotropolis.json`)*
Free functions `buildCrown / buildWestShoulder / buildDistrict(tag) /
buildHarborBay` etc. (signature `void(EchoRegion&, EchoRegionCtx&)`), each a
verbatim port of the host build blocks named in §1's table (host lines ~951-2040
— towers/houses/condos/infra/mine/districts/boats/hackables/streetProps blocks),
with host lambdas (`buildXf`, dir env-var resolution) reproduced locally in the
.cpp. Also the regions JSON (§1 table: ids, anchors, radii, load/unload,
neighbors: crown↔west_shoulder, crown↔harbor_bay, crown↔woodlands cells,
district chain along the freeway). Depends on WP-1 header. DONE: every §1
"owns" item has a builder home; JSON parses under `WorldRegionGraph::load`
(agent may run the existing `--test-worldstream` parser via a tiny standalone
check — no cmake; if not feasible, eyeball against x3.regions/1 fields in
world_stream.cpp:60-80).

**WP-3 — woodlands cells + light slices** *(files:
`app/world_hosts/echo_woodlands.h`, `app/world_hosts/echo_woodlands.cpp`)*
Port the woodlands scatter (host ~1143-1260) into
`buildWoodlandsCell(cellIx, cellIz, EchoRegion&, EchoRegionCtx&)`: same
deterministic `hh()` hash over the same island-wide iteration domain, same
keep-outs (district pads, crown R=470, mine R=175, metro strip, kRoute freeway
corridor), but each placement is EMITTED only when it falls inside the cell's
rect — the 9-cell union is **bit-identical** to today's single system (DONE
criterion: a standalone count check — replicate the loop twice, single vs
sliced, equal instance counts + first/last transforms equal). Also: extract
`loadDistrict`'s light harvesting so each district builder returns its own
`vector<PointLight>` slice (feeds `EchoRegion::addLights`).

**WP-4 — teardown contracts** *(files: `app/env_art.h`, `app/env_art.cpp`,
`app/npc_skin.h`, `app/npc_skin.cpp` — this WP owns these four files
exclusively)*
(a) `EnvArtSystem::destroy(x3::rhi::IRenderDevice&)`: track every
`createMesh`/`createTexture` handle the system makes (it already path-caches
loads; add handle vectors) and release them; safe double-call; document that
draw-after-destroy is a bug. (b) `NpcSkin::deactivate(Scene&)` +
re-`build()` re-attach, mirroring CrowdSkin's (hide characters, un-hide
blockout bodies, keep the pool). DONE: additive only — existing callers
compile untouched; self-test hooks noted in comments for the integrator.

**WP-5 — verification kit** *(files: `scripts/echo_stream_ab.ps1`,
`docs/plans/TIER2_VERIFY.md`)*
The A/B discipline as runnable steps: fixed-TOD `--legacypost` shot-cam
capture set (6 canonical cams: postcard, crown street, mine, each district
gate) with byte-compare (milestone A) and review-grade compare (B-D); the
fly-across script (spawn crown → console `go harbor` → `go drag` → each
district at `speed 3`) with the log greps that prove build/evict
(`[worldstream] +/-region`, proxy engages == 0 at sane speeds, FPS lines);
boot-time measurement for D. DONE: scripts run against the CURRENT build
(they must not require streaming to exist — they baseline it).

**WP-0 — INTEGRATOR (the only host writer + only builder; runs after 1-4)**
*(files: `app/world_hosts/host_echotropolis.cpp`, `app/CMakeLists.txt`)*
1. Add new .cpps to CMake. 2. Replace the host build blocks with
`EchoRegionSet` init (+ `WorldStreamer` instance, budget cvar `ws_budget` ms,
graph from WP-2 JSON), keeping a **verbatim `ECHO_STREAM=0` path**: the env
var short-circuits to `regionSet.forceAllResident()` at boot and skips
`streamer.update` — the old world, at the cost of one `if`. 3. Replace the
draw fans with `regionSet.drawAll` + persistent-lane draws; replace
`appendDistrictLights` with `regionSet.appendNearLights`. 4. Feed
`streamer.update(walkScene, …, camX, camY, camZ, camVel…, budget)` per frame
with the active-camera probe + vista rule (§5). 5. Wire miners/lamp/mineGlow
gating per §2. 6. Build (`cmake --build --preset windows-vs2026 --parallel 4`,
5× retry loop — degraded 14900K ICEs on retry-identical builds; LNK1104 means
Tim is playing: wait 60 s and retry), then execute milestones in order.

## 5. Milestones (each shippable, each verified before the next)

**A — containers, zero behavior change.** WP-1..4 merged; integrator moves
content into regions but calls `forceAllResident()` unconditionally. Verify:
WP-5 byte-compare capture set (fixed `ECHO_TOD`, `--legacypost` kills AE/TAA
nondeterminism) — **byte-identical or A fails**; FPS within noise.
**B — residency-gated draws.** `streamer.update` computes wants; draws gate on
residency; builds still all at boot (deactivate ↔ reactivate only). Verify:
street-level FPS (expect the big win — distant districts/woodlands cells stop
submitting), captures at the 6 cams reviewed (vista cam must be UNCHANGED via
the vista rule), fly-across shows no pop inside load radii.
**C — true hook lifecycle.** Boot still builds all (cheap), but evictions
actually run: `onTeardown` deactivates containers (GPU memory retained —
EnvArt reality), miners/lamps gate correctly, proxy floor never engages at
`speed 1-3`. Verify: fly-across log shows `-region unloaded` / rebuild cycles,
`streamer` leak counters stable, capture review, `ECHO_PLAYAS_DEMO` still
passes (demo forces vista mode).
**D — spawn-region boot + true destroy.** `buildStartRegions` builds `crown`
only; neighbors stream in; `onTeardown` upgraded to `EchoRegion::destroy`
(WP-4). Verify: boot-time delta logged (expect seconds saved), spawn
integrity (walk immediately, no proxy engage at spawn), VRAM drop on evict
(device stats), full WP-5 suite.

Rollback at every milestone: `ECHO_STREAM=0` (cost: one env check + the
force-resident call — the monolithic behavior, preserved verbatim).

## 6. Decisions (made, with reasons)

1. **Camera-distance wants, plus a vista override.** Probe = active camera
   (fly/orbit/ride), because in this world the camera IS the player. Orbit
   vista and high flight must see the whole island: when
   `camY − hf.heightAt(camX,camZ) > 250 m` **or** orbit radius > 900 m **or**
   speed > 600 m/s (console-boosted flight), `setVistaMode(true)` → all
   regions wanted. 5 s hysteresis on leaving vista mode so dips don't thrash.
   This keeps the postcard shot whole while street play streams.
2. **Fly speed vs radii.** Base 240 m/s × lookahead 2.5 s = 600 m lead ≈
   loadRadius 650. Above 600 m/s the vista rule takes over — honest answer:
   nobody streams at 4,800 m/s; we stop pretending and draw everything.
3. **Proxy floor / ocean.** Region anchors are on land; over open water no
   footprint applies and the persistent terrain-collision mesh + water plane
   already exist — the proxy is a land-region-only device, and at spawn the
   crown is boot-built so it never engages there. Expected `proxyEngageCount
   == 0` in all verification runs; a nonzero count is a finding.
4. **DDGI.** The 1600 m probe volume stays static and persistent. Evicted
   geometry drains out of the probe field over ~2 s of hysteresis and returns
   the same way — acceptable, and vista mode keeps the island resident in
   exactly the shots where GI coverage matters most. Revisit only if M-C
   review shows visible GI holes at street level.
5. **TLAS / skinned-RT churn.** Evictions change the static draw set → TLAS
   rebuild. Mitigated by: chunked deactivation, evictions being rare
   (hysteresis bands are wide), and M-C measuring rebuild cost in the log
   before M-D makes evictions aggressive. `r_skinnedrt 0` is the live lever
   if churn shows.
6. **Capture harnesses.** The headless shot path keeps its own monolithic
   mini-build (it already constructs `sScene/sNpc` separately) — force
   `ECHO_STREAM=0` semantics there unconditionally. `ECHO_PLAYAS_DEMO`
   forces vista mode for its whole run.

## 7. Risk register

| # | Risk | Mitigation |
|---|---|---|
| 1 | **EnvArt GPU teardown doesn't exist** — naive destroy leaks or crashes | M-C ships deactivate-only (VRAM retained, records gone — the actual perf win); WP-4's `destroy()` lands with handle tracking + double-call guard; M-D is the first user; leak counters logged |
| 2 | **Terrain/collision streamed by mistake** → player falls through world | Lane C is explicit; integrator DONE-list includes "collision mesh + hf built before any region work"; verification walks at spawn frame 1 |
| 3 | **Byte-compare A fails spuriously** (TAA jitter, AE, sim clocks) | WP-5 uses `--legacypost` + fixed `ECHO_TOD` + `todspeed 0`-equivalent captures; only then is a diff a real regression |
| 4 | **districtLights leak across evictions** (one global vector today) | WP-3 slices harvesting per region; `appendNearLights` only reads resident regions; M-B review includes a night capture per district gate |
| 5 | **Movers cross region seams** (cars drive the loop through district pads; boats span the bay) | Movers are Lane C persistent; their draw cost is small (≤ ~40 records); only static mass streams. Revisit per-lane gating only if profiling demands |
| 6 | **Two-writer collisions / concurrent builds** (the repo's known scar) | WP file ownership is disjoint by construction; only WP-0 touches host + CMake + runs cmake; agents that finish early do NOT "help" in others' files |

## 8. Execution order

WP-1..5 in parallel (five Sonnet-5 agents) → integrator WP-0 milestone A →
verify → B → verify → C → verify → D → verify → commit per milestone
(`echotropolis: TIER2 M-A …` etc.), fleet post after M-B (the FPS number) and
M-D (the boot number).
