# Plan — Level Architect as its own EXE, over shared engine + host DLLs

Status: DRAFT, pre-execution. Conditions written before any code, on purpose.

Owner: InspectorX, lane `inspx/mountain-tunnels` (split to `inspx/la-exe` on start).
Decision: Tim, 2026-08-13 — "Real DLLs (x3core + x3app SHARED)".

## Why, honestly

The obvious reason to do this is the one that is half wrong, so it goes first.

"A shared engine DLL stops the editor and the game from drifting apart." That is
true and it is *not* what bit us today. Both of the day's worst bugs were
HOST-layer divergence, and a `x3core.dll` boundary would have caught neither:

* the tunnel was lit in headless captures and black when driven — same engine,
  different frame loops (`screenshot_hosts.cpp` vs `host_tunnel.cpp`);
* `assetRoot()` resolved differently purely from build layout, making every
  committed asset in the repo invisible in one configuration.

So the boundary is drawn around **`app/` as well as `engine/`**. `x3app.dll`
carries `app_run.cpp` and every `world_hosts/*` — the code that actually
diverged. If only one of the two splits survives contact with reality, keep
`x3app`. The engine split is for crash isolation and build time; the host split
is the one that buys correctness.

## Shape

```
engine/ -> x3core.dll   (SHARED)   renderer, rhi, ecs, physics, core
app/    -> x3app.dll    (SHARED)   200 TUs: hosts, world gen, editor, tests
                                   (everything except the entry point)

X3Engine.exe          thin: WinMain -> x3AppMain(argc, argv)
X3LevelArchitect.exe  thin: WinMain -> x3AppMain with --editor forced in
```

Both exes land in the same `bin/`, link the same import libs, and load the same
two DLLs. The editor exe is a launcher, not a fork: it injects argv and calls the
identical entry.

## Known constraints (verified, not assumed)

1. `x3core` is `add_library(... STATIC)` today (`engine/CMakeLists.txt:1`), and
   `X3Engine` is the ONLY `add_executable` anywhere in the tree — on every
   branch, `inspx/level-architect` included. Verified by grep, not memory.
2. The editor is NOT a standalone host. `editorMode` is threaded through
   `app_run.cpp` at :1071, :8195, :8554, :9688 of a ~9,700-line render loop. This
   plan does NOT decouple it — it launches it. Decoupling is a separate lane.
3. MSVC auto-export (`CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS`) exports functions and
   data but NOT templates instantiated across the boundary, and data symbols pay
   an indirection without `dllimport`. Expect a link-error tail, not a clean
   first build.
4. Statics/globals become PER-DLL. Any singleton that both DLLs touch must live
   in exactly one. Known registries to audit: the tunnel corridor registry
   (`registerTunnelCorridorFor`/`tunnelRouteCount`), the terrain corridor +
   portal-hole registries, the console/cvar table, and the log sink.
5. vcpkg runtime DLLs (glfw, imgui) must stage next to the exes or nothing
   launches.
6. `--test-worldstream` already reports a texture double-free (76 created / 112
   destroyed). DLL teardown ordering can make that worse. Measure, don't assume.

## Acceptance conditions

Iterate until ALL hold. Each is checkable by a test, a log line, or a file that
exists. None is "looks good".

### Build & shape
- [ ] S1. `x3core.dll`, `x3app.dll`, `X3Engine.exe`, `X3LevelArchitect.exe` all
      present in `build-ninja/bin/`.
- [ ] S2. Both exes are THIN: neither entry TU exceeds 120 lines.
- [ ] S3. `X3LevelArchitect.exe` contains no host/world/editor logic of its own —
      it forces argv and calls the same `x3AppMain` as the game.

### The drift conditions (the reason this lane exists)
- [ ] D1. Both exes resolve the SAME asset root. New `--print-assetroot` prints
      it; the two outputs are byte-identical. This is the direct regression test
      for the bug that hid every asset in the repo.
- [ ] D2. `X3LevelArchitect.exe --test-editor` and `X3Engine.exe --test-editor`
      produce identical pass/fail output. Same code, therefore same result — if
      they ever differ, the split has already failed.
- [ ] D3. The editor boots a level through the SHIPPING host path. Assert in code
      that the editor's world entry is the same function the game calls, so this
      cannot rot into an editor-specific variant.

### No regressions
- [ ] N1. Full self-test suite passes from `X3Engine.exe` — same set, same count
      as before the split. Record both numbers.
- [ ] N2. `--test-tunneldrive` stays 11/11 (it is 11/11 as of today, measured).
- [ ] N3. `--test-worldstream` texture ledger is NOT worse than 76/112. If DLL
      teardown makes it worse, say so explicitly rather than shipping it.
- [ ] N4. Every registry in constraint 4 has exactly one instance at runtime.
      Verify by counting, not by reasoning about linkage.

### Evidence
- [ ] E1. One capture from `X3LevelArchitect.exe` showing the editor UI live.
- [ ] E2. The before/after test counts committed in the message, not just claimed.

## Execution order

1. `x3app` SHARED first, with `X3Engine.exe` thin over it. Prove the game still
   runs and N1/N2 hold BEFORE adding a second exe. This is the risky half.
2. Then `X3LevelArchitect.exe` — cheap once step 1 holds.
3. Then `x3core` STATIC -> SHARED. Least valuable, most likely to spray link
   errors; doing it last means a failure here does not block the deliverable.

Rationale: if the lane has to stop early, stopping after step 2 still ships the
thing Tim asked for.

## Explicitly NOT in this lane

* Decoupling the editor from `app_run.cpp` (constraint 2).
* Hot-reload of the engine DLL. The split makes it *possible*; it is not this.
* Fixing the pre-existing texture double-free (task #20).
