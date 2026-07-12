# RIFTHUB AAA REBUILD — Plan (Stargate portal, Fable-style)

_Written 2026-07-11 (post-crash, saved BEFORE coding so the insight survives a crash).
Branch: `feat/rifthub-aaa` (worktree `D:/GameDev/X3Native-rifthub`). Goal: one unified,
AAA-quality portal hub that meets Task #1._

## Task #1 goal (verbatim)
> Stargate portal rebuild (rifthub AAA): port DJBooth's hub to playable-build + smooth ring
> mesh + liquid membrane + blue light + synthesized hum/kawoosh/whoosh.

## Landscape (5 divergent branches — this is debt to consolidate)
| Branch | Content | Base | Verdict |
|--------|---------|------|---------|
| `feat/portal-hub` (d8298f0) | **Clean 7-file core**: 8 cosmetic rifts in a hub circle, blue energy core + wormhole housing + eerie audio, shimmer pulse, trigger→"rift activated" HUD (signposting only, NO runtime world-switch) | **playable-build** ✅ | **USE AS BASE** |
| `feat/rifthub-portal-visuals` (42b3384) | **Stargate rework**: thick grey-stone ring (40 seg), 9 amber chevron clamps, event-horizon membrane pool | cull-combined (134-file diff) | **HARVEST IDEAS** (don't merge wholesale) |
| `feat/portal-hub-polished` (084a4ce) | polish variant | cull-combined (134) | harvest / then delete |
| `feat/portal-hub-rebased` (d398c78) | rebased variant | cull-combined (180) | delete after harvest |
| `feat/rifthub-aaa` (31f4c7e) | **empty** (fresh @ playable-build) | playable-build | **BUILD HERE** |

**Base decision:** `feat/portal-hub` is the only one that ports to playable-build without
dragging 130+ cull-combined files in. Cherry-pick its 7-file core onto `feat/rifthub-aaa`,
then elevate. Harvest the *ideas* (stone gate form, chevrons, membrane) from
`-portal-visuals` by re-implementing, not merging.

## Architecture (from portal-hub core — keep this)
- `app/rifthub.{cpp,h}`, namespace `x3::game`, clean-room (Scene/trigger/mesh_prims + engine
  interfaces only; NO id Tech/Doom/Quake).
- 8 portals on a circle radius ~14 m around spawn, one per `--world` target.
- Trigger id range **200–207** (`RifthubTrigger` enum) — fresh, no collision with Act-1
  (10/30/40/50), Act-2 host (80–82), caves (100–108).
- `Rifthub::tick(dt)` drives per-frame animation; per-portal entity-id ranges let tick poke
  `emissive[3]` in-place without re-issuing render calls.
- Signposting only for now (latch "rift activated" + HUD line); runtime world-switch = FUTURE.

## The 4 AAA gaps + how to close them
### 1. Smooth ring mesh ❌ (currently 40 tangent BOX segments, 9° each, chunky-butt overlap)
- Replace with a **true procedural TORUS**: ring centerline R≈2.05 m, tube radius ≈0.40 m,
  ~64 major × ~16 minor segments, correct per-vertex normals (smooth-shaded), UVs for the
  stone material. Add a subtle bevel/chamfer read.
- Keep the walk-through scale (0.8 m band, 0.9 m deep) so it still reads as a substantial gate.
- Grey stone base material (`{0.44,0.45,0.50}`) with faint emissive self-lift (~0.30), NOT a glow.

### 2. Liquid membrane ⚠️ (event-horizon pool exists but likely a flat emissive disk)
- Fill the ring opening (inner radius ≈1.65 m) with an **animated liquid event-horizon**:
  - Rippling surface — scrolling/animated normals (sin-sum or a small noise), so it shimmers
    like Stargate water.
  - **Fresnel edge** — brighter blue at grazing angles / at the ring's inner rim.
  - Slow rotation / caustic swirl toward the center.
  - On ACTIVATION: a one-shot **kawoosh** — the membrane bulges out (puddle-splash) then
    settles, synced to the kawoosh SFX.
- If a bespoke shader is too heavy for the first pass: animate emissive + a scrolling normal
  on a fine-tessellated disk via the existing material path; document the true-shader upgrade.

### 3. Blue light ⚠️ (CONFLICT: portal-hub = blue core; visuals branch made ring grey non-glowing)
- **Resolve = best of both:** keep the grey STONE ring form, but drive a **blue light** from
  the membrane/core that CASTS onto the stone (point/emissive light at the ring center, cool
  blue ≈`{0.3,0.6,1.0}`), so the stone gate is lit blue from its own event horizon. Pulses
  with the hum. Chevron clamps stay amber (warm accent against the cool blue = contrast).

### 4. Synth hum / kawoosh / whoosh ⚠️ (has ambient hum; missing distinct events)
- **Idle hum** — looping low synth drone while a portal is dormant (per-portal, subtle).
- **Kawoosh** — one-shot on portal ACTIVATION (trigger enter): the whooshing splash-out.
- **Whoosh** — on TRAVERSE (when runtime switch lands; for now, on the "relaunch" latch).
- Synthesize procedurally (miniaudio) or use existing SFX pack; wire through the audio system
  with 2D/3D placement at the portal.

## Build steps (in order)
1. **Base:** cherry-pick `feat/portal-hub`'s 7-file core (rifthub.{cpp,h} + wiring:
   CMakeLists, main/app_run include+tick+draw, any HUD line) onto `feat/rifthub-aaa`. Build +
   `--smoketest` clean FIRST (prove the clean core lands on playable-build).
2. **Ring:** swap the box-segment ring → procedural torus (new mesh_prims helper if needed).
   Visual check.
3. **Membrane:** animated liquid event-horizon (ripple + fresnel + blue), + kawoosh bulge hook.
4. **Light:** blue core light casting on the grey stone; amber chevrons; pulse-with-hum.
5. **Audio:** idle hum loop + kawoosh (activate) + whoosh (latch/traverse).
6. **Chevrons/plate:** port the 9 amber chevron clamps + floor plate from `-portal-visuals`
   (re-implement, phased pulse).
7. **Gate:** add/keep `--test-rifthub` (portal count, trigger ids 200-207, tick advances
   emissive, membrane animates). Release + Debug `--smoketest` (default + a rifthub world/flag)
   = 0 VUID + allocationCount=0 + clean exit. Kill stale exe before each build.
8. **Consolidate:** once AAA lands + is visually approved, DELETE the other 4 portal branches
   after confirming nothing unharvested remains.

## Gotchas / discipline
- Kill any running `X3Engine.exe` before building (locks `build/bin/Release/X3Engine.exe`).
- Run/launch from the repo ROOT so asset relative paths (regions.canon.json etc.) resolve.
- Clean-room: NO id Tech/Doom/Quake/other-engine source. Original procedural design only.
- **Visual correctness is Tim's eyeball** — headless gating proves no-crash/no-leak, not "looks
  AAA." Launch windowed for each visual milestone; report what's verified vs. what he must judge.
- Commit + push after each milestone (crash durability — this box has chronic KernelPower crashes;
  14900K RMA pending).

## Open question flagged in review
The membrane-shader + audio "⚠️" are inferences from code STRUCTURE, not a deep read of the
`-portal-visuals` shader/audio. Confirm their actual state when harvesting (step 3/5) before
assuming they need full rebuilds vs. tuning.
