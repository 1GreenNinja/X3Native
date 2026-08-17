# W-PERF (Lane 3) — HANDOFF

Worktree `.claude/worktrees/agent-a5827ca176041d974`, branch
`worktree-agent-a5827ca176041d974`, based on `a1400d67`.
**Lane 1 (W-TUNNEL) rebases onto this lane's merge — terrain.cpp changes are
deliberately confined to the LOD/mesh path; the carve geometry is untouched.**

## State

**LANE COMPLETE — Lane 1 may rebase.** Three commits, `app/terrain.cpp` +
`app/terrain.h` ONLY (no host files touched, so the carve-interface ripple is
free of conflicts).

| item | status |
|---|---|
| 1. Distance-scope the corridor refine (#33) | **DONE** `2b296d13` — but see FINDING 1: dormant at today's radius |
| 2. Retire `corridorPin && dist < 420`? | **ANSWERED: KEEP IT** — 1.4% tris, 0.000 ms (FINDING 3) |
| 3. Ridge-LOD blade towers (#26) | **DONE** `2b296d13` — 5.96 m -> 0.00 m, captures read |
| 4. Horizon-ring inner-hole seam | **ROOT-CAUSED, left to the host's owner** (FINDING 5) |
| (bonus) safety coupling defect | **FIXED** `3ec146de` |

Commits: `2b296d13` mesher work · `d5a798d6` third A/B instrument + measurements ·
`3ec146de` ridge-filter corridor exclusion decoupled from the debug env var.

## Commit `2b296d13` — what landed

`app/terrain.cpp` + `app/terrain.h` only. Full story is in the commit message;
the short version:

* **Far-field clamp.** Beyond `kCorridorRefineNearM` (500 m) of the tile's
  stream-request focus, a corridor-hot coarse cell keeps its 2 coarse triangles
  and every corner vertex of a *carved* hot cell is pulled to
  `min(y, min(field over the cell's LOD0 lattice))`. Convexity then makes the
  green wedge unrepresentable at 2 tris/cell instead of 32. Seam-safe: a border
  vertex scans its incident cell ring in world space, so both tiles compute the
  same minimum. Bored reaches (depth ~0) exempt — clamping would pull the lid
  onto the portal holes. Focus threaded `init/update -> requestTile -> GenJob
  -> generate`; **null focus = exact everywhere, bit-identical to the old
  mesher**, which is why every existing gate still passes unchanged.
* **Ridge filter (#26).** At coarse LODs an interior vertex takes the MAX of the
  field over the cell-sized block it represents, so narrow crests stop falling
  between point samples (the blade towers). Off on border verts, inside corridor
  influence, and at Full LOD.
* `X3_NO_CORRIDOR_PIN=1` added beside `X3_NO_CORRIDOR_LOD_REFINE=1` and
  `X3_NO_RIDGE_FILTER=1` — three A/B instruments, all measurable from one build.

## Suites (all green at `2b296d13`)

terrain **4/4** (T4 crest deficit 0.000 m) · terraincorridor **16/16** ·
roadnetwork **58/58** · tunnelmouth **8/8** (M7 worst -0.268 m) ·
riverbridge **9/9**.

### The C8b trap — do not "fix" this back

C8b failed on its first run at an absolute 2 cm threshold. The cause was the
**probe, not the clamp**: `terrainTileCorridorWedge` rasterises triangles on a
0.5 m grid while the finest mesh lattice is 1 m, so between two lattice samples
*any* mesh stands slightly above the field — Full LOD included, the surface
collision itself uses. C7 measures that floor at 0.625 m and therefore compares
coarse-to-Full; C8b now does the same. Measured: far-field worst **0.079 m**
against Full's **0.625 m**, worst per-tile excess over Full exactly **0.000 m**.
The clamp sits well *under* the collision surface at range.

## Measurements (quiet GPU, RTX 5090, `--world tunnel`, 9 showcase cameras)

Instrument: the host's own `[tunnel-perf]` harness — fixed camera, 200-frame
settle, GPU ms averaged over the settled 60 frames + tris/draws. Boot **zero
`[ERROR]`** in every config.

| config | env | gpu ms (min..max over 9 cams) | tris @01_approach |
|---|---|---|---|
| A after (lane state) | — | 0.935 .. 1.626 | 2,397,402 |
| B before (exact refine everywhere) | `X3_NO_REFINE_SCOPE=1` | 0.937 .. 1.630 | 2,397,402 |
| C no pin | `X3_NO_CORRIDOR_PIN=1` | 0.935 .. 1.630 | 2,391,540 |

### FINDING 1 — the far-field clamp cannot engage at today's stream radius

A and B are *identical*. Not a null result — arithmetic explains it exactly. A
tile's clamp distance is measured against its **request-time** focus, and a tile
is only ever requested inside the residency ring, so that distance is bounded by
the ring corner:

| mode | radius | ring corner |
|---|---|---|
| interactive | 9 tiles | **407 m** |
| headless (captures) | 14 tiles | 634 m |

`kCorridorRefineNearM` is **500 m**. 407 < 500, so interactively the clamp
**never fires**; headless only the corner tiles do (the 192-triangle delta at
shots 04/05/06). NO_SLOP rule 6 territory — the mechanism is correct and gated
(C8) but the door is shut at the radius the game actually runs.

### FINDING 2 — the terrain triangle load is not what holds the world down

GPU frame time is **0.94–1.63 ms** (≈600–1070 fps equivalent). Interactive at
spawn, `X3_PERF_LOG=1`, 40 s unattended: **165.0 fps flat** — it **saturates the
165 Hz display cap**, so no A/B delta is claimable there, only headroom. The
owner's 104 fps / "varies widely" therefore is **not** GPU triangle cost.

### FINDING 3 — the corridorPin is already nearly free (item 2 answered)

Dropping it saves **1.4%** of triangles and **0.000 ms**. Reason: the refine has
made coarse corridor tiles nearly as dense as Full already, so the pin's extra
work is only the *non-corridor* part of those tiles. **Recommendation: KEEP the
pin.** Retiring it buys nothing measurable and removes a safety net.

### FINDING 4 — the ridge fix, numerically (item 3)

`--test-terrain` T4, worst Quarter-LOD crest deficit along the tunnel ridge:

* `X3_NO_RIDGE_FILTER=1` (before): **5.962 m** lost at (-508, -78)
* filter on (after): **0.000 m**

Nearly six metres of crest vanishing between point samples — that is what makes
the surviving samples read as isolated vertical blades.

### FINDING 5 — the horizon-ring inner hole (item 4), root-caused

`host_tunnel.cpp:697` sets `hr.rInner = 470.0f`, centred on the **route
midpoint** and static. The hole is filled only by streamed tiles, which follow
the **player** at `radius * 32 m`:

| mode | tiles reach | void annulus |
|---|---|---|
| interactive (owner) | 288 m | **182 m wide** |
| headless (every capture) | 448 m | 22 m |

Two defects in one: (a) a **paired-value break** (NO_SLOP rule 4) — `rInner`
and the `radius = headless ? 14 : 9` fifteen lines above are one value and
neither names the other; (b) the proof harness runs at radius 14 where the gap
is 22 m and invisible, so **the capture path structurally cannot see the bug the
owner sees**. Residual beyond the paired fix: the ring is static while the focus
moves, so the hole is only covered while the player is near the route midpoint.

### Captures — READ, not just written (`shots_wperf/`)

* `ridge_D_filter_off.png` vs `ridge_A_filter_on.png` — same camera
  (-301.9, 17.6, -472.2) yaw 2.051 pitch 0.25, aimed at the tunnel ridge 445 m
  out (Quarter LOD, `kRanges[4]`, amp 460 m). **Filter off:** the crest is a
  jagged sawtooth of isolated fins separated by deep V-notches — the blade
  towers. **Filter on:** one continuous rounded massif, notches filled, reads
  as rock. This is task #26's before/after.
* `A_after/`, `B_before_scope/`, `C_nopin/`, `D_noridge/`, `FINAL/` — the 9
  showcase cameras per config. `FINAL/06_mouth_headon.png` and
  `A_after/08_exit_portal.png` show the portal seam tight, terrain below the
  road slab, **no green strip** at any LOD.
* NOTE, unexplained and NOT mine: a pale translucent sail-shaped sliver above
  the treeline at mid-left, **identical in filter-on and filter-off**, so it is
  not the ridge LOD. Distance ~450 m — consistent with the horizon-ring hole of
  FINDING 5. Worth a look by whoever takes that fix.

## Open / next

1. Tile **generation** cost, the untested suspect behind "FPS varies widely":
   the ridge filter roughly **3x**es field sampling on mountain tiles
   (~1,459 -> ~4,435 `terrainHeightAt` calls per tile). Optimisation already
   identified if it measures material: the Full-LOD pass already samples the
   whole LOD0 lattice — cache it in `generate()` and the Half/Quarter max-filters
   become a reduction over that array instead of re-sampling the field.
2. Decide `kCorridorRefineNearM`: left at 500 m as dormant headroom, now
   documented as such. Deliberately NOT lowered — there is no measured perf
   problem for it to solve (1.5 ms GPU), and lowering it would trade exactness
   for triangles nobody needs while adding shoulder-gap risk. Lowering it on
   the strength of "it should help" would be exactly the rule-9 guess this lane
   is supposed to avoid. Revisit when the residency radius grows.
3. Horizon-ring fix (FINDING 5) — a ~2-line paired-value change at
   `host_tunnel.cpp:689-699`. Left to the tunnel host's owner (Lane 1) rather
   than conflicting with an in-flight lane for an unverifiable gain: the
   headless capture path runs radius 14, where the defect is 22 m and invisible,
   so **any fix must be verified interactively at radius 9**, not by a capture.

## Etiquette

`tasklist //FI "IMAGENAME eq X3Engine.exe"` before EVERY launch (~10 sibling
lanes tonight — retry, don't abort). NEVER `--smoketest`. NEVER push.

## Note on dirty files

`assets/converted_glb/nature/*.glb` (modified) + `Vehicles/*.glb`,
`*.pre-fetch.bak` (untracked) were already dirty on arrival and are **not this
lane's** — left untouched, deliberately not committed.
