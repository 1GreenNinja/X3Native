# Off-screen geometry contributed the WRONG material to every DDGI / reflection ray

Branch `fix/rt-stable-material-index`, off `perf/tlas-static-dynamic` @ `dc9757fa`.
This is the follow-up §4.2 of `PERF_TLAS_SPLIT.md` named as "the right next piece of
work, and it pays twice". It pays once. That is still worth it.

---

## 1. The bug, confirmed

`instanceCustomIndex` on each TLAS instance carried the record's row in the **per-object
SSBO** (`ObjectData`), and the ray shaders used it to fetch the hit surface's material
(`shaders/ddgi_rays.comp`, `shaders/refl.comp`).

That buffer is **compacted by visibility**. In `prepareFrameData`'s group-emit loop, the
two cull paths are asymmetric:

* **GPU cull path** — no `continue`; every record gets a row and an `ObjectData` entry.
* **CPU cull path** — a frustum-culled instance hits `continue` **before**
  `m_recordSsboRow[ri] = row`, so it keeps the `assign(n, 0u)` initial value.

Meanwhile the TLAS build loop admits **every non-glass record regardless of visibility** —
correctly, because RT needs geometry the camera cannot see. So culled instances entered the
TLAS carrying `instanceCustomIndex = 0`, and **every DDGI or reflection ray that hit
off-screen geometry shaded with object row 0's albedo/emissive**.

**The CPU path is the shipping default.** `r_cullpath` is now a deprecated compat alias;
the live cvar is `r_vis`, registered `"1"` (`app/app_run.cpp:462`), and `resolveVisPolicy`
maps level 1 to `cullPath = 0` (`engine/rhi/Visibility.h`). Not a debug-only path.

### Measured, not argued

`--world echotropolis`, boot camera, steady state, `ECHO_RT=1 ECHO_RESIDENTS=1`:

```
96,056 TLAS instances
60,191 (62.7%) carry a BOGUS instanceCustomIndex     <- frustum-culled, kept row 0
15,946 of those resolve to a materially DIFFERENT albedo/emissive than their own
```

The other ~44 k culled instances happen to share row 0's `baseColorFactor`/`emissive`
values — city geometry that differs only in its *texture*, which these shaders never read.
So 16.6% of the RT scene was being shaded with visibly the wrong material, and 62.7% was
being addressed wrongly.

---

## 2. Why no existing gate saw it

**Every RT capture rig in the repo is a null case.** With the diagnostic patched onto the
base and each host run to its capture frame:

| rig | TLAS instances | carrying a bogus index |
|---|---:|---:|
| `--screenshot-ddgi` | 13 | **0** |
| `--screenshot-rtshadows` | 577 | **0** |
| `--screenshot-reflverify` | 16 | **0** |
| `--screenshot-csm` | 10 | **0** |
| **`--screenshot-rtmatverify` (new)** | 5 | **3 (60%)** |

`--screenshot-reflverify` looks like it should have caught this — its whole design is three
saturated bars "deliberately OUTSIDE the vertical frustum". But they are 28 m wide, giving
a ~14 m bounding **sphere** that still crosses the frustum, and the cull test is a
conservative sphere test. They are off *screen* and never culled. That rig verified that
per-object material lookup works; it could not verify that the *index* is right.

**And `X3_TLAS_VERIFY` was structurally blind to it.** Its byte-compare derives
`instanceCustomIndex` the same way on both sides — the partial update and the naive repack —
so it reported **42/42 byte-identical** while 62.7% of the scene pointed at the wrong
material. A harness that cannot fail on the bug it is standing next to is worse than no
harness, because it is cited as evidence.

---

## 3. The fix — approach 2, and why not approach 1

`instanceCustomIndex` is now **the draw-record index**, addressing a new table with one
32-byte row per record, visible or not (`RtMaterialGpu`). Only the two fields the ray
shaders actually read live there (`baseColorFactor`, `emissive`), so the *uncompacted*
table is a quarter the size of the compacted 160 B `ObjectData` it replaces for RT
purposes. Rows are written only when a material actually changes.

**Approach 1 ("row the culled, draw only the visible") cannot work as stated on this
renderer.** The raster path draws each group as `firstInstance = baseRow`,
`instanceCount = drawn`, so a group's visible instances must be **contiguous** in the
object buffer. Giving culled records rows there either interleaves them into that range —
drawing everything, i.e. deleting the CPU frustum cull — or needs a compaction indirection
the CPU path does not have. It also would not have removed the index churn.

### Where the write lives, and why it matters

Measured three placements on the steady-state city (96 k records, ~16 MB of `DrawRecord`),
p10 over 40 steady-state windows:

| placement | `cpu.preparedata` | `as.instance_pack` |
|---|---:|---:|
| base (no table) | 3.957 | 1.402 |
| standalone loop over `m_drawRecords` | 4.923 **(+0.97)** | 1.439 |
| folded into `emitGroup`'s walk | 4.923 **(+0.97)** | 1.439 |
| **in the AS instance walk (shipped)** | 4.098 (+0.14) | 1.525 (+0.12) |

A standalone loop pays a **second full stream** of the record array. Folding it into
`emitGroup` keeps the records hot but visits them in **group order**, which turns the 3 MB
shadow into a cache miss per record — same cost, different reason. The AS instance walk is
the only loop in the frame that is sequential in the record index *and* already streaming
the records. The shadow is per frame-slot, so each slot converges on its own buffer with no
cross-slot bookkeeping.

---

## 4. The churn — removed, and worth less than advertised

```
                       before                        after
static      70,661 (untouched)            95,705 (untouched)
dynamic     25,395 (rewritten)               ~372 (rewritten)
  of which  25,039 pure index shifts            0 pure index shifts
```

Exactly as `PERF_TLAS_SPLIT.md` §4 predicted: only ~373 instances actually move, and with a
stable index the dynamic set collapses to them.

**But the ~0.5 ms did not follow.** `as.instance_pack` did not drop — it went *up* 0.12 ms.
The 25 k avoided row rewrites are ~1.6 MB of write-combined stores, small against the
~16 MB of `DrawRecord` traffic the walk pays either way. The lane's model attributed that
zone's cost to the row writes; the measurement says the zone is dominated by the record
stream. Net effect of the whole change is **~+0.26 ms** across the two zones, inside the
~±0.15 ms run-to-run spread this shared box shows on *untouched* zones (`cpu.drawmesh`
moved −0.14 ms between the same two runs). **Call it perf-neutral.** The reason to make
this change is correctness.

---

## 5. Correctness — how it was actually proved

### 5.1 The harness grew an arm that can fail

`verifyRtInstanceRows` now also resolves each instance's `instanceCustomIndex` — taken from
the instance **mirror**, the image of what was written to the device — through the material
**shadow**, exactly as the ray shaders resolve it, and compares against the record's own
material. End to end, against what the GPU will actually see.

**Proved it has teeth**: with `custom = i` temporarily reverted to the old expression and
everything else left alone, the harness reports

```
[ERROR] [rt] TLAS VERIFY FAIL: 38,942/96,054 instances resolve to the WRONG MATERIAL
        via instanceCustomIndex (first instance 80 = draw record 80)
```

on the very first frame. (That count is a *sensitivity* result, not a second measurement of
the bug's size — reverting only the writer leaves the two index spaces mismatched. The
bug's real magnitude is the 60,191 / 15,946 in §1.)

With the fix in place, `--world echotropolis` over 2,101 frames:

```
[rt] TLAS VERIFY ok: 96,077/96,077 rows byte-exact vs a full repack,
     96,077/96,077 instances resolve to their OWN material (0 bad)
```

and the same under `X3_TLAS_VERIFY=2` synthetic streaming churn (splice frames rewrite
78,084 rows and 1,910 materials, still 0 bad).

### 5.2 A rig that can see it — `--screenshot-rtmatverify`

Every coloured object is **compact** and placed so its whole bounding sphere clears a
frustum plane by metres, so the CPU cull drops it every frame with certainty. The first
object drawn is a saturated **orange** matte surface with zero emissive, so it owns object
row 0 — the row a stale index resolves to.

*A head-on mirror wall does not work here*: `refl.comp:172` fades out any reflection ray
heading back toward the camera (`backFade`), so that geometry is discarded by construction.
The rig uses a **grazing mirror floor** pitched 45° down, where `dot(R, V) = 0` keeps
`backFade` at 1 and the reflection ray leaves at 45° forward-and-up — so the subjects hang
high and ahead, above a frustum top plane that at 45° pitch points 15° *down*.

**Arm 1 — RT reflection.** Mean colour inside each reflected panel:

| panel | BEFORE (R,G,B) | AFTER (R,G,B) | channel skew |
|---|---|---|---|
| red | (95.5, 105.7, 132.7) | **(208.5, 127.1, 142.1)** | 37.1 → 81.4 |
| green | (91.1, 101.7, 130.5) | **(120.3, 211.6, 167.9)** | 39.4 → 91.3 |
| cyan | (95.8, 106.2, 133.2) | **(137.5, 207.2, 209.5)** | 37.4 → 72.0 |

Before, the three panels are **within 5/255 of each other** — one indistinguishable slate
smudge, because all three read row 0. After, each reads its authored colour.

**Arm 2 — DDGI colour bleed.** An off-screen saturated-green emissive panel behind the
camera, bleeding onto a white wall. Before: no bounce. After: mean **G +24.8** against
R +3.6, B +2.5 — a 7:1 channel skew. Emissive goes through the same lookup, and row 0's
emissive is zero, so the broken build produced no glow at all.

**The three RT-off control shots (`mirror_refl_off`, `mirror_ssr_only`, `bleed_ddgi_off`)
are BYTE-IDENTICAL before and after** (max |delta| = 0). That is both the scope check — the
change touches only the RT material lookup — and the proof that this rig is deterministic,
so the deltas above are real and not run-to-run noise.

### 5.3 Honest limits of the visual proof

* The corrected panels are **desaturated toward the blue IBL**. `refl.comp` caps the RT
  fallback's confidence at 0.45, so 55% of the pixel remains prefiltered env specular. The
  fix restores the *hue* unambiguously; the *magnitude* is still attenuated by a cap that
  pre-dates it and is unchanged here.
* Arm 2's absolute brightness is set by auto-exposure, so the DDGI evidence is the channel
  **skew**, not the luminance.
* Both arms are procedural. No GLB-based scene was used, for the reason
  `rt-refl-verify/README.md` already documents.

### 5.4 The existing RT capture suite: 22/22 byte-identical, and that is correct

`--screenshot-ddgi`, `--screenshot-rtshadows`, `--screenshot-reflverify`,
`--screenshot-csm` — 22 captures, hashed before and after: **all identical**. That is not
the fix doing nothing. It is the §2 table: those four rigs contain **0** mis-indexed
instances between them, so there is nothing in them for this change to correct. The scenes
that do change are the ones with off-screen TLAS geometry — the new rig, and any real
world.

---

## 6. Gates

| gate | result |
|---|---|
| Release `--smoketest` (default / `echotropolis` / `canonlevel`) | exit 0, `allocationCount=0`, 0 `[ERROR]` |
| **Debug** `--smoketest` (same three) | exit 0, **0 VUID**, `allocationCount=0` |
| **`--vksync`** × Debug smoketest × same three | **0 sync hazards**, 0 VUID, `allocationCount=0` |
| `--test-csm` | 21 passed, 0 failed |
| `--test-clusterlights` | 37 passed, 0 failed |
| `--test-refldenoise` | 14 passed, 0 failed |
| `--test-geolod` | 23 passed, 0 failed |
| `--test-rtshadows`, `--test-ddgi` | exit 0, 0 `[ERROR]`, `allocationCount=0` |
| `X3_TLAS_VERIFY=1` and `=2`, echotropolis, 2,101 frames | 0 verify fails, materials 100% correct |
| 22 RT-dependent captures, before vs after | 22/22 byte-identical (see §5.4) |

The single Debug `[ERROR]` is `[stutter] shader module created after first frame
(frame 31)` — on the `docs/VALIDATION.md` known-benign allow-list, `r_strictpso` defaults
to 1 in Debug and 0 in Release. It is not a VUID and this diff creates no shader modules or
pipelines.

`--test-worldstream`: **16 passed, 2 failed** — identical to the base branch's own
documented result. Pre-existing, not worsened.

---

## 7. What is left

* **The 0.45 confidence cap** in `refl.comp` is now the dominant error in off-screen
  reflections, not the material index. Worth a look by whoever owns that shader.
* **PVS narrows the blast radius and deserves its own look.** With `r_vis 1`, room-invisible
  entities are never submitted, so they never become draw records and never enter the TLAS
  at all. That is a *different* off-screen-geometry gap in RT — geometry missing from the
  TLAS rather than mis-shaded in it — and this lane did not touch it.
* The material table carries only `baseColorFactor` and `emissive` because that is all the
  ray shaders read today. Per-triangle material (uv / texture fetch) still needs an SBT
  tier that does not exist here.
