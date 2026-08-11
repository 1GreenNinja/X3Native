# Vulkan validation: what the gate actually proves

> **Short version.** `--smoketest` on its own proves the frame did not crash.
> It does **not** prove "0 VUID" unless the layers were loaded, and it proves
> nothing at all about barriers unless **synchronization validation** was on.
> Both are now visible in the log. Quote that line or the claim is unfalsifiable.

## The two holes this document exists to close

**1. Release had no validation layers, compiled out.**
`app/main.cpp` used to read:

```cpp
#ifdef _DEBUG
    desc.validation = true;
#else
    desc.validation = false;
#endif
```

Every "Release `--smoketest`: 0 VUID" this project ever reported therefore meant
only that **validation was ABSENT**. It was a measurement of nothing, and it was
repeatedly quoted as evidence of a clean frame. The flag is now a runtime
decision with the same per-config *default*, so nothing silently got slower, but
it can be turned on anywhere.

**2. Standard validation does not check synchronization.**
The validation layers check *API usage* and report `VUID-*`. They do not ask
"was this read ordered against the write before it?" unless
`VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT` is explicitly
enabled. A run with layers on, 0 VUID, and sync validation off says nothing
about whether the render graph's barriers are correct. When it was first
switched on, this engine reported **4079 sync hazards**.

## How to run it

| Flag / env | Effect |
|---|---|
| *(nothing)* | Layers **ON in Debug**, **OFF in Release** — the historical default |
| `--validate` / `X3_VK_VALIDATION=1` | Force layers ON (works in **Release**) |
| `--no-validate` / `X3_VK_VALIDATION=0` | Force layers OFF (e.g. timing a Debug build) |
| `--vksync` / `X3_VK_SYNC_VALIDATION=1` | **+ synchronization validation.** Implies `--validate` |

An explicit flag always beats the environment variable.

```powershell
# The sync gate. Debug is the config that must be clean.
.\build\bin\Debug\X3Engine.exe --smoketest --vksync
.\build\bin\Debug\X3Engine.exe --smoketest --world canonlevel --vksync

# Release can run it too now — much faster to iterate on, same findings.
.\build\bin\Release\X3Engine.exe --smoketest --validate --vksync
```

Sync validation is **expensive** (the layer shadow-tracks every memory access;
expect a multi-second boot penalty), which is why it is opt-in rather than
always-on. It is not needed for routine smoketests — it is needed whenever
anyone touches a barrier, a `ResourceUse`, a `loadOp`/`storeOp`, or a layout.

## Reading the result

The device logs one line at init and the smoketest restates it at the end:

```
[rhi] VALIDATION: layers=ON sync-validation=ON
smoketest: VALIDATION layers=ON sync-validation=ON  (VUID + SYNC-HAZARD checking active)
```

With checking off it says so in the loudest terms available:

```
[rhi] VALIDATION: layers=OFF sync-validation=OFF  <-- NO VUID CHECKING: a '0 VUID'
result from this run is MEANINGLESS  <-- NO SYNC CHECKING: missing barriers will
NOT be reported
smoketest: VALIDATION layers=OFF sync-validation=OFF  (NO CHECKING AT ALL - a
'0 VUID' claim from this run is meaningless; add --validate)
```

**Any report of "0 VUID" or "zero hazards" must quote one of these lines.**

### Counting hazards honestly

The layer silences a message after `duplicate_message_limit` (default **10**)
repeats. A hazard firing every pass of every frame is reported 10 times and then
goes quiet, so the count tells you nothing about how many call sites are
affected — and a *partial* fix looks identical to no fix. When `--vksync` is on
the engine sets `VK_LAYER_DUPLICATE_MESSAGE_LIMIT=0` (unlimited) unless you
already set it, so `N -> 0` is a real before/after measure and a per-site fix
shows up as a stepwise drop. Count with:

```powershell
(Select-String -Path run.log -Pattern "SYNC-HAZARD-" -AllMatches).Matches.Count
```

Match `SYNC-HAZARD-` **with the trailing dash** — `SYNC-HAZARD` alone also
matches the engine's own "(VUID + SYNC-HAZARD checking active)" status line.

## Known-benign output (allow-list, do not chase)

* `[stutter] shader module created after first frame (frame 31)` — Debug only.
  `r_strictpso` defaults to 1 in Debug / 0 in Release (`app_run.cpp`). It is a
  PSO-hygiene warning, not a validation error.
* `VUID-vkCmdDraw-None-09600` ×5 from Debug `--screenshot-showroom-ragdoll` —
  pre-existing, verified byte-identical on untouched `main`. A standard VUID, not
  a sync hazard.

## Current state

Release **and** Debug, default world **and** `--world canonlevel`, with
`--vksync`: **0 sync hazards, 0 VUID, `allocationCount=0`, exit 0.** No residue.

## The four defect shapes that produced 4079 hazards

Worth knowing, because each is a pattern that will recur:

1. **Src *stage* naming the wrong command.** A mip chain barrier used
   `COPY_BIT` for a mip that `vkCmdBlitImage` (which runs in `BLIT_BIT`) had
   written. The access mask was right; only the stage was wrong. `ALL_TRANSFER`
   covers both if you do not want to reason about which command produced it.
2. **`LOAD_OP_LOAD` without `COLOR_ATTACHMENT_READ`.** `vkCmdBeginRendering`
   *reads* an attachment it loads, and blending reads it again. Declaring only
   `..._WRITE` is a read-after-write hazard. Same for a loaded depth attachment
   and `DEPTH_STENCIL_ATTACHMENT_READ`.
3. **`STORE_OP_STORE` is a write.** A depth attachment that is only *tested*
   still writes at `LATE_FRAGMENT_TESTS` via `vkCmdEndRendering` if its storeOp
   is `STORE`. Declare it, or the next pass's transition is unordered against it.
4. **A persistent image re-imported `TOP_OF_PIPE`/`0` every frame.** Correct
   *within* a frame; across submits on the same queue it orders against nothing,
   so this frame's first layout transition can overlap the previous frame's
   in-flight writes. Entry *layout* `UNDEFINED` (discard the contents) is fine —
   the entry *scope* still has to name what last touched the image. Windowed
   swapchain images are exempt: the acquire semaphore orders them.

`engine/rhi/RenderGraph.cpp` also had a structural version of (2): it skipped
the barrier for read-after-read in the same layout. That is only safe with
respect to the two reads — it silently dropped the dependency on the *write*
before them, which had only ever been made visible to the **first** reader's
stage/access. It now also emits when a read needs scope bits the tracked state
does not cover, and accumulates read scopes between writes instead of
overwriting them.
