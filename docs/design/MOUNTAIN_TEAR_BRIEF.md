# BRIEF: fix the mountain tear (one task, start here)

Branch `inspx/mountain-tunnels` (pushed). The previous tunnel agent CANNOT be
resumed — its transcript is gone. This is a standalone task; do this and nothing
else until it is done.

## The bug, in one line
One env flag toggles between two mutually exclusive bugs, and neither ships.

| `X3_TUNNEL_PORTAL_CUT` | bore | mountain |
|---|---|---|
| `1` (default) | DRIVABLE end to end, residual 0 m | **TORN OPEN** — a full-height rip through the peak, sky visible through it, long jagged shards hanging from the rim |
| `0` | packed with earth, undrivable | intact |

Both must be true at once. Tim saw the tear instantly from the approach road.

## What is NOT wrong
The terrain FIELD is fine — the run reports worst soil cover **23.94 m**, so the
rock is there. Do not go looking in `mountainHeight()` or the RangeDef table.

## Diagnosis — MINE, AND UNVERIFIED. Measure before you trust it.
The portal-hole change in `terrain.h/.cpp` drops surface triangles (and skirts)
that dip into the tube envelope at the mouth. That test appears to be **unbounded
vertically**, so instead of removing a mouth-sized opening it deletes the entire
column of triangles above the bore, up to the summit.

Bound it on all three axes. A triangle should be dropped ONLY if it is:
1. within the tube cross-section, AND
2. within the mouth's arc-length window, AND
3. **below crown + shell thickness** — anything above the crown must survive.
   That is the mountain.

Then cap the rim. Dropping triangles leaves a ragged boundary with no cap, so the
shards persist even once the extent is right; the portal ring / headwall should
cover it.

## THE VERIFICATION RULE — this lane paid for it twice
`--test-tunneldrive` passes happily **while the summit is missing**. A still
cannot prove traversability. You need BOTH, every time:

* `--test-tunneldrive` — traversability, negative-controlled (cut off must stall
  the car at the earth ramp)
* `--screenshot-tunnel <dir>` — then **LOOK at `01_approach` AND `04_saddle`**
  and confirm the mountain is whole

Ship neither claim without the other. A test caught what a screenshot could not,
and then a screenshot caught what the test could not. That is this lane's history.

## Ground rules
* Fallback env must keep working: `X3_TUNNEL_PORTAL_CUT=0` restores the prior
  field + mesh exactly.
* Keep green: `--test-terraincorridor`, `--test-terrain`, `--test-terrainplace`,
  `--test-city`, `--test-worldregions`, `--test-cityblocks`, `--test-tunneldrive`.
  `--test-worldstream` is 16/2 from a PRE-EXISTING texture double-free
  (76 created / 112 destroyed) — not yours, do not hide it.
* Build fails `LNK1104` if the game is running — kill `X3Engine.exe` first.
* Commit message states honestly what is NOT done.

## Two traps that already cost hours here
* A python `.replace()` silently no-ops on a `\r\n` vs `\n` mismatch. ASSERT every
  edit landed, then verify by grep, before you build.
* Do not trust a header comment about what is implemented. Verify at the
  implementation level.

## Build
```
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && cd /d D:\X3Native-dev && cmake --build build-ninja --target X3Engine -j 20'
```
Binary: `build-ninja\bin\X3Engine.exe`.

## After this, and only after
`docs/design/TUNNEL_HANDOFF_BRIEF.md` has the rest of the lane's open work.
`docs/design/TUNNEL_INTERIOR_PLAN.md` ends with Tim's verbatim interior brief —
note the pull-off shoulders WIDEN THE BORE, so that cross-section decision gates
the walkways and railings.
