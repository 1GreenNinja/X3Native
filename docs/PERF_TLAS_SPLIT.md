# The TLAS static/dynamic split — what it cost, what it bought, and what is still there

**Branch** `perf/tlas-static-dynamic` · **Box** I9DEVPC, 14900K + RTX 5090 · **Build** Release
**Scene** `--world echotropolis`, windowed, boot (crown) camera · **Method** `X3_PASSDUMP=2`,
p10 across 61–62 two-second windows per arm (see §6 of `PERF_FRAME_BREAKDOWN.md` on why the
median is not trustworthy on this shared box). **Every arm was run twice, in opposite order**
(round 1 base-then-new, round 2 new-then-base), because this box is shared and run order is
a real confound; both rounds are quoted below and they agree to ±0.4 ms.

Base for every "before" figure: `origin/fix/vk-sync-hazards` @ `772bf11b` merged with
`origin/lane6/perf-timers-main` @ `8c68df68` — i.e. `origin/main` plus the sync-validation
gate and the per-pass timers, and nothing else.

---

## 0. The headline

| | before | after | delta |
|---|---:|---:|---|
| **frame CPU, instrumentation OFF (shipping)** | **32.62 / 32.50 ms — 30.7 FPS** | **27.93 / 28.12 ms — 35.7 FPS** | **−4.5 ms · +5.0 FPS** |
| frame CPU, instrumentation ON | 33.59 / 34.29 ms — 29.6 FPS | 29.26 / 30.01 / 30.05 ms — 33.5 FPS | −4.3 ms · +3.9 FPS |
| frame GPU | 14.74 ms | 14.83 ms | +0.09 ms (noise) |
| **total acceleration-structure CPU** | **6.454 ms** | **2.516 / 2.500 ms** | **−3.95 ms** |
| TLAS instances | 96,077 | 96,077 | unchanged |
| RT capability | DDGI + RT shadows + reflections + AO + acoustics | **identical** | — |

*(paired values = round 1 / round 2; the third timers-ON "after" figure is a same-build
repeat. Spread across five independent 130 s runs of the same binary is 0.8 ms, which is the
box, not the change.)*

Every RT consumer is byte-for-byte unchanged: **42 of 42** RT-dependent captures
(`--screenshot-ddgi`, `--screenshot-rtshadows`, `--screenshot-reflverify`,
`--screenshot-csm`) hash identically before and after, as do full stills of
`--world echotropolis` and `--world canonlevel` with `ECHO_RT=1 ECHO_RESIDENTS=1`.

---

## 1. FIRST: the brief's premise was wrong, and measuring it changed the whole plan

The task arrived with a measured budget — `cpu.rt_as_build` 6.75 ms, split
`blas_refit 3.53 / instance_pack 1.26 / tlas_build 1.94` — and a sibling lane's claim that
**"~5.5 ms of the 6.75 is blocking fence waits."** Three new sub-zones
(`as.blas_wait`, `as.tlas_pack`, `as.tlas_wait`) measured it instead of taking it:

```
cpu.rt_as_build        6.554
  as.blas_refit        3.499
    as.blas_wait       3.251   <- submit + vkWaitForFences for ~15 skinned BLAS refits
    (record loop)      0.248
  as.instance_pack     1.152   <- the 96k draw-record walk
    as.signature       0.000   <- already short-circuited by lane 6
  as.tlas_build        1.884
    as.tlas_pack       1.393   <- second 96k walk + 6.1 MB memcpy
    as.tlas_wait       0.465   <- submit + fence for the 96,076-instance TLAS build
```

**Fence waits: 3.72 ms. O(96 k) CPU work: 2.55 ms.** Not 5.5 / 1.2.

And the headline number inverts the brief's framing entirely: **building a 96,076-instance
TLAS cost 0.465 ms of waited time.** The expensive submit was the *other* one — 3.25 ms for
**fifteen** skinned-character BLAS refits. The thing the brief was named after was the
cheapest term in it.

That is why this lane's largest single win is not the split.

---

## 2. What was actually built, and why

### (a) One AS submit per frame, and fifteen barriers deleted — **−1.51 ms**

`ensureSkinnedBlas` emitted a full `AS_BUILD → AS_BUILD` memory barrier **after every
refit**. Each refit reads and writes its own backing through its own resident scratch;
two refits of different meshes touch disjoint memory and the spec imposes no ordering
between them. The barrier bought nothing and forced fifteen tiny AS updates to execute
strictly one at a time with a pipeline flush between each.

The TLAS build then paid a *second* submit and a *second* blocking fence. It is now
recorded into the same command buffer (`recordTlasBuild`), preceded by the one barrier
that genuinely is required — every BLAS write before the TLAS reads it.

`as.blas_wait 3.251 + as.tlas_wait 0.465 = 3.716` → **one merged wait of 2.204 ms.**

### (b) The static/dynamic split — **−1.37 ms**

**One TLAS, not two.** A separate dynamic TLAS would force `mesh.frag`, `rtao.comp`,
`refl.comp`, `ddgi_rays.comp` and the audio-ray pass to each trace both and merge hits —
closest-hit merging for reflections and DDGI, a second AS binding in five descriptor sets,
and a new class of "which structure answered" bugs — to save GPU time this frame is not
short of (GPU 14.7 vs CPU 33.6). Instead the **instance buffer** is partitioned: it is
persistently mapped, never cleared, and only the rows that changed are rewritten. Untouched
rows keep last frame's value, so the static portion is written once and then simply
persists. Every consumer is untouched.

**There is no classifier, on purpose.** The renderer gets a fresh flat list of 96 k draw
records every frame with no object identity. A declarative "this is static" flag would have
to be maintained by dozens of host call sites and would silently corrupt the TLAS the first
time one of them lied. So an instance is dynamic **iff its description differs from what is
already in the buffer at that slot**, compared against a CPU-side shadow. Consequences:

* A parked car that starts driving needs no transition event — it starts failing the
  compare. Static→dynamic and back are free and cannot desync.
* A streaming region load or unload changes the row count; every row after the splice
  point compares unequal and is rewritten. One frame at roughly the old price,
  self-correcting, no residency bookkeeping.
* A buffer reallocation reports `invalidated` and everything is rewritten.

Two walks became one, the `TlasInstance` staging vector and the 6.1 MB bulk memcpy are
gone, `tlasSignature()` is deleted (the per-row compare answers "did anything move?"
exactly and for free), and the TLAS scratch is persistent instead of a multi-megabyte
`vmaCreateBuffer`/`vmaDestroyBuffer` **every frame**.

> **A trap worth recording.** The first cut wrote the row fields directly into the mapped
> buffer. That memory is host-visible **device** memory (write-combined over PCIe BAR), and
> `instanceCustomIndex` is a 24-bit bitfield and `mask` is a byte — so those assignments
> compiled to *read-modify-writes of WC memory*, ~2 µs each. `as.instance_pack` went to
> **51.29 ms** and the frame to 82 ms. The fix is to build the 64-byte row on the stack and
> `memcpy` it as one full write-combine burst. **Never assign into mapped device memory
> field by field.**

### (c) Submit now, wait later — **−1.21 ms**

The batch is submitted before the render graph is recorded and the fence is waited
immediately before the frame's own `vkQueueSubmit2`, so the AS build runs on the GPU while
the CPU records the frame. `as.tlas_wait 2.204` → `cpu.rt_as_wait 0.991`.

**The wait was not removed.** Replacing it with same-queue submission-order assumptions is
the write-after-write hazard class that standard validation does not report — one of the
five fixed in `772bf11b`. Only its *position* moved; the command buffer is still not reset
while pending, the scratch is still not rewritten while in flight, the instance buffer is
still not rewritten while the build reads it, and the ray-query passes still cannot execute
before the build completes. `beginFrame` drains as a belt-and-braces net for endFrame's
early-out paths, and `shutdown` drains before destroying anything.

`cpu.rt_as_wait` is a **named leaf** subtracted from `cpu.endframe_rest`, so the smaller
`cpu.rt_as_build` cannot be quoted as the whole story.

---

## 3. The ledger

| bucket | before | after | note |
|---|---:|---:|---|
| `cpu.rt_as_build` | 6.454 | **1.434** | |
| ↳ `as.blas_refit` | 3.480 | 0.184 | the wait left this bucket |
| ↳ `as.instance_pack` | 1.106 | 1.189 | one walk now, but it writes 25 k rows (§4) |
| ↳ `as.tlas_build` | 1.858 | 0.029 | record only |
| `cpu.rt_as_wait` *(new leaf)* | — | **0.991** | the single relocated fence wait |
| **AS total** | **6.454** | **2.42** | **−4.03 ms** |
| `cpu.graph_record` | 1.191 | 1.144 | now overlaps the AS build on the GPU |
| frame CPU | 33.59 | 29.26 | |

---

## 4. The honest limit: 25,042 of 25,416 "dynamic" rows did not move

Telemetry (`[perf] TLAS instance rows:`) on the steady-state city:

```
static 70,661 (untouched)   dynamic 25,415 (rewritten, of which 25,042 were pure
                                            SSBO-row shifts)   = 96,076
```

**Only ~373 instances per frame actually move.** The other 25,042 rows are rewritten
because their `instanceCustomIndex` changed — nothing else.

`instanceCustomIndex` is `m_recordSsboRow[i]`, the row in the **cull-compacted** per-object
SSBO (`vk_passes.cpp`, `emitGroup`). On the CPU cull path `row` only increments for records
that survive the frustum test, so with an orbiting camera the surviving set changes every
frame (`objects drawn` moves 35,938–35,946) and **every row index after the first change
shifts**. That is inherent to compacting the object buffer by visibility, not to the split.

Two things follow, and the second one matters more than the first:

1. **~0.5 ms is still on the table** — with a stable custom index the dynamic set drops to
   the hundreds and `as.instance_pack` loses most of its write traffic.
2. **There is a pre-existing correctness bug here.** `m_recordSsboRow` is `assign(n, 0)`
   before the pass, so a **frustum-culled** instance keeps row **0** — and culled
   instances still enter the TLAS (correctly: RT needs off-screen geometry). So today,
   every DDGI or reflection ray that hits something off-screen reads **object row 0's**
   albedo/emissive. This lane did not introduce it and did not fix it: decoupling the RT
   material lookup from the compacted SSBO changes what `ddgi_rays.comp` and `refl.comp`
   read, which is exactly the "silently wrong GI" class that needs its own lane with its
   own visual gate. **It is the right next piece of work and it pays twice.**

---

## 5. Correctness — how it was actually proved

A wrongly-partitioned TLAS produces wrong shadows/reflections/GI, often only in motion or
only after streaming, and a smoketest cannot see it. So the invariant was checked directly.

**`X3_TLAS_VERIFY=1`** keeps `m_rtRowMirror`, the exact byte image of everything the partial
path wrote into the (unreadable, write-combined) device buffer, and every frame
independently repacks **every** row the naive way and `memcmp`s the two. A mismatch is a
hard `[ERROR]` naming the first stale row.

**`X3_TLAS_VERIFY=2`** adds synthetic streaming churn: a large contiguous block of draw
records is withheld from the middle of the list for 45 frames and re-admitted, block
position and size walking each cycle. Withholding records is exactly what a region unload
does to the instance array — it splices every row index after it — and re-admitting them is
exactly what a load does.

| run | frames checked | bad rows |
|---|---:|---:|
| echotropolis, `=1`, RT + residents, 150 s | 4,501 | **0** |
| echotropolis, `=2` (churn), RT + residents, 150 s | 4,501 | **0** |
| `--world streamed`, `=1`, 75 s | 12,001 | **0** |
| `--test-framepacing`, `--test-rt/-ddgi/-rtshadows/-reflections`, 3 screenshot rigs | all | **0** |

Under churn the row count swings **76,842 ↔ 96,077** and splice frames rewrite **78,084**
rows — every one byte-exact. The natural run also moved the count (96,054 → 96,077 →
96,076) without help.

Both modes are off by default; neither mirror vector is allocated and neither branch taken.

### Gates

| gate | result |
|---|---|
| Release `--smoketest` (default / `--world canonlevel` / `--world echotropolis`) | exit 0, 0 `[ERROR]`, **allocationCount = 0** |
| **Debug** `--smoketest` (same three) | exit 0, **0 VUID**, allocationCount = 0, only the known `r_strictpso [stutter]` line |
| **`--vksync`** × smoketest ×3, `--test-rt`, `--test-ddgi`, `--test-rtshadows`, `--test-reflections` | `VALIDATION: layers=ON sync-validation=ON`, **0 hazards** |
| `--vksync` **under synthetic churn** (`X3_TLAS_VERIFY=2`) × 4 configs | **0 hazards**, 0 verify fails |
| `--test-csm`, `--test-clusterlights`, `--test-refldenoise`, `--test-geolod`, `--test-framepacing`, `--test-gpucull`, `--test-frustumcull`, `--test-acoustics`, `--test-rt`, `--test-ddgi`, `--test-rtshadows`, `--test-reflections`, `--test-streaming` | all exit 0 |
| 42 RT-dependent captures, before vs after | **42/42 byte-identical** |

**`--test-worldstream` fails identically on the base**: base `15 passed, 3 failed`
(W1, W4, W5b); this branch `16 passed, 2 failed` (W4, W5b). W4 is a wall-clock budget on a
shared box and W5b reports `textures created 79 / destroyed 115` — an accounting bug in a
stub device this branch never touches. **Pre-existing, and strictly not worse.**

---

## 6. What is left, in measured order

1. **Stabilise `instanceCustomIndex`** (§4) — worth ~0.5 ms *and* fixes off-screen RT hits
   currently shading as object row 0. Needs its own visual gate on DDGI/reflections.
2. **`as.instance_pack` 1.19 ms** — the floor is "touch 96 k draw records once". Removing it
   means the renderer learning object identity across frames (retained/GPU-driven
   submission), which is the same work that would also delete `cpu.drawmesh` (1.95) and
   `cpu.host_drawfan` (2.98). That is a much bigger lane and it is where the next 5 ms live.
3. **`cpu.rt_as_wait` 0.99 ms** — the residual stall. A true retire-later ring would remove
   it, but it needs the instance buffer, the batch scratch and the per-mesh skinned-BLAS
   scratch all ringed, plus an explicit AS-write → ray-query-read barrier in the frame
   command buffer. Every one of those is a hazard standard validation will not report, for
   1 ms. **Do it only behind `--vksync` and only with `X3_TLAS_VERIFY` running.**
4. **`cpu.host_sim` 15.1 ms (52 % of the new frame)** — untouched by any of this, and now
   more than half the frame. It is the whole game.
