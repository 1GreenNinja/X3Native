# `cpu.host_sim` — SPLIT, AND IT IS ONE ROW AGAIN

**Host-sim lane, 2026-08-12.** Base: `origin/perf/tlas-static-dynamic` @ `dc9757fa`
(carries the per-pass timers, the AS fix and the sync-hazard work). Branch:
`perf/host-sim-split`. Box: I9DEVPC, 14900K + RTX 5090, Release, `--world echotropolis`,
windowed, boot camera. `X3_PASSDUMP=2`; every figure is the **p10 across the windows of a
run**, per §6 of `PERF_FRAME_BREAKDOWN.md`.

---

## 0. The one-line answer

> `cpu.host_sim` was **15.70 ms**. **14.77 ms of it — 94 % — is `sim.opsscreen`**: the
> crown-plaza city-ops dashboard re-rasterising a **1024×1024 RGBA** image and
> **creating and destroying a fresh mipped GPU texture, every single frame**, for text
> that changes a handful of times per in-game day. Nothing else in the living city is
> above **0.39 ms**. The crowd, npcLife schedules, city alert, LLM chatter, ocean,
> camera and audio together cost **0.79 ms**.
>
> It is not a simulation cost at all. It is a cache-invalidation bug wearing a
> simulation's clothes.

Fixing it (a content-equality guard on `setLines`, measured below) takes the frame from
**30.61 ms → 21.63 ms** shipping — **32.7 → 46.2 FPS** — and the frame stops being
CPU-bound.

---

## 1. THE SPLIT

Sixteen exclusive `HostScope` zones nested inside `zSim`, exactly as the previous lane
split `cpu.host_outside`. Because `HostScope` is exclusive and publishes into
`attributed`, the parent `cpu.host_sim` automatically becomes the **residual**.

Measured on the **unmodified** behaviour (`X3_HOLO_DIRTYGUARD=0`), 26 windows, camera
stable throughout (`objects drawn` 35,938–35,949, `drawMesh submitted` 96,153–96,154):

**CPU 31.60 ms/frame (31.6 FPS) · GPU 14.87 ms — still CPU-bound.**

| zone | ms (p10) | % of `host_sim` | % of frame | calls/frame |
|---|---:|---:|---:|---:|
| **`sim.opsscreen`** | **14.768** | **94.1 %** | **46.7 %** | 1.0 |
| `sim.residentskin` | 0.386 | 2.5 % | 1.2 % | 1.0 |
| `sim.npcskin` | 0.248 | 1.6 % | 0.8 % | 1.0 |
| `sim.miners` | 0.090 | 0.6 % | 0.3 % | 1.0 |
| `sim.pointlights` | 0.022 | 0.1 % | 0.1 % | 1.0 |
| `sim.tod_atmos` | 0.017 | 0.1 % | 0.1 % | 2.0 |
| `sim.oh1_heli` | 0.016 | 0.1 % | 0.1 % | 1.0 |
| `sim.residents` | 0.006 | 0.04 % | — | 1.0 |
| `sim.audio` | 0.003 | 0.02 % | — | 1.0 |
| `sim.npclife` | 0.003 | 0.02 % | — | 1.0 |
| `sim.talk_llm` | 0.001 | — | — | 1.0 |
| `sim.streetlamps` | 0.001 | — | — | 1.0 |
| `sim.ocean` | 0.000 | — | — | 1.0 |
| `sim.camera` | 0.000 | — | — | 1.0 |
| `sim.cityalert` | 0.000 | — | — | 1.0 |
| `sim.roadlights` | 0.000 | — | — | **0.0** |
| **`cpu.host_sim` (residual)** | **0.001** | **0.006 %** | — | 1.0 |
| **`sim.TOTAL`** | **15.695** | 100 % | 49.7 % | — |

**The residual is 0.001 ms.** The split is complete to one part in 15,000; there is no
hidden remainder to go looking for.

### Reading the zeros — none of them is the underflow bug

The brief's trap #1 is real: a span that straddles `beginFrame()`'s
`frameAccum().reset()` underflows and reports 0.000 forever. These do not.

* `sim.ocean`, `sim.camera`, `sim.cityalert` report **calls/frame 1.0**, so the span ran;
  and nothing already-attributed executes inside them (no `drawMesh`, no `beginFrame`), so
  `inner` is 0 and the exclusive result *is* the raw span. They are genuinely under the
  0.0005 ms print resolution — a few float writes and a device setter each.
* `sim.roadlights` reports **calls/frame 0.0** — correct. It is gated on
  `tod.sample().cityLightsOn`, and the measured windows are daytime. It is *not* free at
  night; it sorts the entire `roads->lights()` list every frame. Untested here, flagged
  in §5.
* Every zone lies strictly inside `zSim`, which still `stop()`s before `beginFrame()`.

`[perf] TIMED-PASS OVERFLOW` **never fired** in any run (checked across all 16 logs);
25 GPU passes recorded, `pass gpu sum 15.19 vs frame bracket 15.19 (delta 0.00)`.

---

## 2. WHAT `sim.opsscreen` ACTUALLY DOES, EVERY FRAME

`app/world_hosts/host_echotropolis.cpp` frame loop:

```cpp
if (opsBuilt) {                 // CONTROL ROOM: refresh the live dashboard
    opsScreen.setLines(opsLines(todS));
    opsScreen.update(dt);
}
```

`opsLines()` returns eleven fixed strings — population 40, workers 4, gamers 7, all
**hard-coded constants** — plus one time-of-day phase name. `HoloTerminal::setLines()`
then did:

```cpp
void setLines(std::vector<std::string> lines) { m_lines = std::move(lines); m_texDirty = true; }
```

`m_texDirty = true` **unconditionally**. `update()` sees the dirty flag and calls
`regenTexture()`, which per frame:

1. builds a `Canvas(1024)` — **three** zero-initialised `vector<float>` of 1,048,576
   elements each, **12 MB of allocation**;
2. runs `blackGlassBase()` — a per-pixel loop over 1,048,576 pixels with a `sqrt` each
   (scanlines, data-grid, vignette);
3. draws the line-art and CPU-rasterises every glyph through stb_truetype, including a
   wrap-and-shrink fit pass;
4. packs a **4 MB** RGBA vector;
5. calls `HoloPanel::setContent()` → `createTexture(..., mips=true)`: a 4 MB staging
   buffer, a 4 MB `memcpy`, a fresh 1024² image with an **11-level mip chain** generated
   by 11 blits and 22 barriers, plus a bindless-slot registration;
6. `destroyTexture()` on the previous one, pushing it onto the deferred-free list.

Roughly **16 MB of allocation, a million-pixel transcendental loop, a full font raster
and a complete GPU texture create+destroy — per frame — to produce byte-identical
pixels.**

The header three lines above `regenTexture()` already says
`// Called from update() when m_texDirty (input/readout changed) — NOT every frame.`
The design was always "re-bake on change". The echotropolis host is the only caller that
violated it, and it violated it 60 times a second.

---

## 3. THE FIX, AND THE A/B

One content-equality guard (`app/holo_terminal.h`):

```cpp
void setLines(std::vector<std::string> lines) {
    if (holoDirtyGuard() && lines == m_lines) return;   // same text -> same bake
    m_lines = std::move(lines); m_texDirty = true;
}
```

**Why it is safe, not merely small.** `regenTexture()`'s output is a pure function of
`m_lines`, `m_input`, `m_active`, `m_layout` and `m_textColor/m_inkOverride`. Every one of
those has its **own** `m_texDirty` writer (`pushChar`, `backspace`, `submit`,
`clearInput`, `setActive`, `setLayout`, `setTextColor`, `resetTextColor`, `addLine`,
`setLastLine`, `trimBody`). So identical `m_lines` with everything else unchanged cannot
produce a different bake than the one already on the glass. `addLine`/`setLastLine`/
`trimBody` are untouched — they change content by construction.

`X3_HOLO_DIRTYGUARD=0` restores the old always-dirty behaviour, so **the A/B runs on one
binary**.

### Decisive A/B — run twice, in opposite order, camera matched

Every arm: quiet harness, 75 s, `X3_PASSDUMP=2`, identical boot camera
(`objects drawn` 35,938–35,949, `submitted` 96,153–96,154 in **all** arms).

| order | arm | frame CPU (p10) | FPS | GPU | `sim.opsscreen` | `sim.TOTAL` |
|---|---|---:|---:|---:|---:|---:|
| 1st | `off3` (guard OFF) | 33.75 | 29.6 | 14.81 | 15.07 | 16.56 |
| 2nd | `on3` (guard ON) | **21.45** | **46.6** | 14.91 | **0.005** | **0.80** |
| 1st | `on4` (guard ON) | **21.51** | **46.5** | 15.13 | **0.005** | **0.76** |
| 2nd | `off4` (guard OFF) | 37.52 | 26.7 | 15.09 | 16.41 | 17.72 |

Both orders agree. The ON arms are **dead consistent (21.45 / 21.51)**; the OFF arms
spread 31.6–37.5 because an arm doing 15 ms more CPU work per frame is far more sensitive
to contention on a shared box. The cleanest OFF arm measured all session is `off2` at
**31.60**, which is the conservative baseline used in §1.

### Shipping numbers — `X3_PASSTIMERS=0`, instrumentation fully off

| arm | frame CPU (p10) | FPS | GPU |
|---|---:|---:|---:|
| guard OFF (today's engine) | **30.61** | **32.7** | 14.81 |
| guard ON | **21.63 / 22.18** | **45.3 / 46.2** | 14.98 / 15.36 |

**−9.0 ms/frame, +13.5 FPS, shipping.**

### Instrumentation overhead

| arm | timers ON | timers OFF | overhead |
|---|---:|---:|---:|
| guard OFF | 31.60 (`off2`) | 30.61 | **~1.0 ms** |
| guard ON | 21.45 / 21.51 | 21.63 / 22.18 | **~0 (unmeasurable)** |

The sixteen new zones add 32 `__rdtsc` reads/frame against the ~192,000 the draw fan
already does — nothing. In the fixed arm the overhead is not measurable at all, because
the frame is no longer CPU-bound (see §4) and CPU savings hide behind the GPU.

---

## 4. THE FRAME IS NO LONGER CPU-BOUND

With the guard on: **CPU 21.5 ms vs GPU 14.9–15.4 ms**, and the CPU-side cost that
absorbed the change is `cpu.endframe_rest` (2.40 → 5.75 ms) and `cpu.rt_as_wait`
(0.034 → 0.70 ms) — i.e. **the CPU now waits on the GPU**, which it never did before
(`cpu.beginframe` was 0.10 ms and is now 0.09, but the AS-batch drain and endFrame
residual took up the slack). Corroboration: in the arms where the camera happened to face
empty sky, GPU fell to 2.02 ms and the fixed frame fell with it to **14.56 / 14.89 ms**,
while the *unfixed* frame stayed at ~32 ms regardless of what was on screen.

**This retires item 1 of `PERF_FRAME_BREAKDOWN.md` §8 and promotes the GPU list (item 6)
from "not a priority" to co-limiter.** The next honest question is no longer "where is the
host time" — it is "why does the CPU spend 5.75 ms in `endframe_rest` waiting".

---

## 5. WHAT IS LEFT INSIDE `host_sim`, AND WHETHER IT IS REDUCIBLE

After the fix `sim.TOTAL` is **0.79 ms** — 3.7 % of the frame. For completeness, the top
three by the brief's request:

1. **`sim.opsscreen` — 14.77 ms → 0.005 ms. REDUCIBLE, DONE, MEASURED.**
   Rebuilt a structure that essentially never changes. Guarded on content equality. The
   deeper cleanup, if anyone wants it: the dashboard's numbers are hard-coded constants,
   so the *only* thing that ever changes is the time-of-day phase name — it could be
   driven from a change callback and never polled. Also `HoloPanel::setContent` creates a
   whole new mipped texture instead of re-uploading into the existing image; any future
   genuinely-animated holo panel will pay that same cost and should get an in-place
   update path.

2. **`sim.residentskin` — 0.386 ms. Mildly reducible; not worth it yet.**
   `CrowdSkin::update` ticks a full `CharacterSystem` (clip playback + skinning) for every
   skinned crowd slot, every frame. It is already PVS-gated (`scene.roomVisible`) and
   already budgets spawns per frame. The obvious lever is an LOD/distance budget so distant
   citizens tick at a lower rate — but at 1.2 % of the frame it is not the next target.

3. **`sim.npcskin` — 0.248 ms. Same shape, same verdict.**
   The npcLife rigged layer, same per-slot `CharacterSystem::update`. Note the *logic*
   layers are almost free by comparison: `sim.npclife` (schedules, robbery FSM, freeway
   traffic, LLM drain) is **0.003 ms** and `sim.residents` (the whole crowd FSM) is
   **0.006 ms**. The cost is in the skinning tick, not the AI.

**One thing to watch that this measurement could not exercise:** `sim.roadlights` ran
**0 times** — it is gated on `cityLightsOn` and every measured window was daytime. At
night it sorts the **entire** `roads->lights()` vector (with a fresh `vector` +
`push_back` per light) every frame just to take the nearest few under a 64-light cap. That
is an O(n log n) full sort per frame where a partial selection would do, and it is
completely unmeasured. **A night-time run should be the first thing the next lane does.**

---

## 6. SURPRISES WORTH SAYING PLAINLY

1. **The 15 ms "living city simulation" was never simulation.** Every AI/schedule/crowd
   system in echotropolis costs **0.79 ms combined**. The city is cheap. A dashboard
   nobody is looking at was 94 % of the bucket and ~47 % of the entire frame.
2. **The previous document's framing was reasonable and wrong.** §3 of
   `PERF_FRAME_BREAKDOWN.md` named "residents.update, npcLife.update, npcSkin.update,
   residentsSkin.update, miners.update, cityAlert.update" as "the freeway of host cost".
   Those six together are **0.73 ms**. Naming a bucket is not the same as splitting it,
   which is exactly why this lane existed.
3. **The engine already knew.** `regenTexture()` is documented "NOT every frame" and has a
   dirty flag for every input. One host call site ignored the contract; nothing detected
   it because no zone was small enough to see it.
4. **A 1024² CPU raster + mipped texture create/destroy per frame is a pattern, not an
   incident.** `HoloPanel` is the flagship of a platform intended for variants across the
   game (holo terminals, the elevator indicator, the keypad readout). Any of them driven
   from a per-frame `setLines` will cost the same ~15 ms. The guard fixes all of them at
   once, but the in-place-upload gap in `setContent` remains.

---

## 7. MEASUREMENT METHODOLOGY — A TRAP THAT COST THIS LANE TWO ROUNDS

**Do not touch the mouse cursor during an echotropolis measurement, and do not let
anything else touch it.** echotropolis boots in **fly mode**, where the host sets
`GLFW_CURSOR_DISABLED` — the cursor is *captured* and every OS-level cursor write becomes
a mouselook delta. The first harness parked the cursor at screen centre every 1.5 s to
defeat orbit edge-scroll; that spun the fly camera off the island and `objects drawn`
collapsed from ~60,000 to **3**. A run where the window loses and regains focus does the
same thing more slowly, and can also deliver a stray ESC, which opens the pause menu —
whose path `continue`s before `zSim`, so the whole world stops simulating while the
breakdown keeps printing happy-looking 6 ms / 165 FPS windows.

Two defences, both now in the harness (`scratchpad/run2.ps1`) and the parser:

* the harness starts the engine and then **does nothing at all** until it kills it;
* the parser **rejects any window** where `cpu.host_sim` calls/frame < 1.0 or
  `drawMesh submitted` < 90,000. In the very first run, **212 of 234 windows** were
  paused-menu windows that would have silently poisoned every average.

Useful corollary discovered along the way: **frame CPU here is almost independent of what
is on screen** (96k draws are submitted regardless; ~63 % are culled afterwards). The
unfixed frame measured ~32 ms whether 35,940 objects were drawn or 3. That is item 5 of
`PERF_FRAME_BREAKDOWN.md` §8 restated as a measurement.

Shared-box discipline: foreign engines from `X3Native-rtmat` and `EchoHarbor-roadfix` were
live during parts of this session. They were identified by command line, **recorded in
every run's header, and never killed.** External load only adds time, so the p10 across
windows remains the undisturbed estimate; contaminated arms are visible in §3 as the wider
spread on the OFF side.

---

## 8. GATE

```
Release --smoketest  (default | --world echotropolis | --world canonlevel)
    exit 0 | [ERROR] lines 0 | real VUID 0 | VMA live allocationCount=0
Debug   --smoketest  (default | --world echotropolis | --world canonlevel)
    exit 0 | real VUID 0 | VMA live allocationCount=0
Debug   --smoketest --vksync --world echotropolis
    exit 0 | VALIDATION: layers=ON sync-validation=ON | SYNC-HAZARD 0 | VUID 0
X3_PASSTIMERS=0 --smoketest --world echotropolis
    exit 0 | [ERROR] 0 | allocationCount=0        (instrumentation fully removable)
```

Debug emits exactly one `[ERROR]` on the *default* and *canonlevel* worlds:
`[stutter] shader module created after first frame (frame 31)` — the pre-existing
`r_strictpso` zero-stutter audit that defaults to 1 in Debug and 0 in Release. It is not a
VUID and cannot come from this diff, which creates no shader modules or pipelines. It does
**not** appear on `--world echotropolis`.

⚠️ Release compiles validation out (`app/main.cpp`), and now says so itself:
`VALIDATION: layers=OFF ... a '0 VUID' result from this run is MEANINGLESS`. The VUID gate
is Debug-only.

---

## 9. INTEGRATION

Files changed vs `dc9757fa`:

* `engine/core/x3_cpuzones.h` — 16 new zone ids + names + `kSimZoneFirst/Last`.
* `engine/rhi/VulkanRenderDevice.cpp` — print the sim rows under their parent inside the
  leaf partition, plus a `sim.TOTAL` line. `leafSum` counts each row exactly once, so
  `cpu.host_outside` stays honest (it reads 0.008 ms).
* `app/world_hosts/host_echotropolis.cpp` — the 16 `X3_HOST_ZONE`s inside `zSim`
  (instrumentation only; no behaviour change).
* `app/holo_terminal.h` — `holoDirtyGuard()` + the `setLines` content-equality guard
  (**the only behavioural change in this branch**).

Rollbacks: `X3_PASSTIMERS=0` / `r_passtimers 0` removes all measurement;
`X3_HOLO_DIRTYGUARD=0` restores the pre-fix re-bake-every-frame behaviour.
