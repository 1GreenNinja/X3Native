# X3Native ENGINE GOTCHAS — the trap ledger
*Written by Fable on its final day (2026-07-07), from the Waves 1–5 campaign. Every entry
was paid for. Read this BEFORE touching the build, the assets, or the dressing systems.
Format: SYMPTOM → CAUSE → FIX → DETECTION.*

## 0. NAMING — READ FIRST

### 0.1 The world flag is `--world echoharbor`, NOT `--world echotropolis`
- Owner, 2026-08-18: *"Echo Harbor should Not be called echotropolis."* The display
  name was always **Echo Harbor**; the internal id was the odd one out. The
  canonical registry key and `--world` flag are now `echoharbor`, and the host is
  `app/world_hosts/host_echoharbor.cpp` / `hostEchoHarbor`.
- `--world echotropolis` is a **PERMANENT alias** and will never be removed —
  ~350 references across 74 files (scripts, test recipes, screenshot commands,
  fleet docs, session notes) type it. Both flags dispatch to the same host.
  Older entries in this file and in `docs/` still say `echotropolis`; they are
  not wrong, just old. **Write `echoharbor` in anything new.**
- Two things deliberately KEEP the old spelling because they are wire formats,
  not labels: the asset paths (`echotropolis_props.glb`,
  `regions.echotropolis.json`, `assets/audio/echotropolis/**`, the
  `echotropolis.rpg.txt` save) and the runtime log prefix
  `--world echotropolis: ...`, which `scripts/echo_stream_ab.ps1` parses.
- DETECTION: `--test-rifthub` D12 fails if the alias ever stops dispatching,
  stops being a reasoned exclusion, or stops resolving to the `echoharbor` row.

### 0.2 Every world host wires the SHARED shell — there is a gate now
- A `--world` host must use `HostShell` (`app/world_hosts/host_shell.h`) for its
  console, pause menu and FPS overlay. Echo Harbor kept a bespoke one for months
  and therefore shipped with NONE of the ~118 shared commands (`noclip` printed
  "unknown"); nothing caught it until a manual audit.
- DETECTION: `app/host_shell_lint.cpp`, run as destinations **D13** inside
  `--test-rifthub`. It sweeps `app/world_hosts/host_*.cpp` and fails on a world
  host that never references `HostShell` (R1) or that calls
  `drawConsole()`/`toggleConsole()` on a Hud of its own (R2). Driving the shared
  console through `shell.hudForCallbacks()` is the one sanctioned route. Four
  controls (negative/positive/sneak/mediated) run every time, so the probe's
  failure mode is proven rather than assumed.

## 1. BUILD SYSTEM

### 1.1 The stale-exe trap (X3Engine vs x3engine case-collision) — THE #1 KILLER
- SYMPTOM: build exits 0, tests "pass", but your changes aren't in the binary. Agents
  false-green on this constantly.
- CAUSE: `cmake --build --target X3Engine` (or `--preset`) resolves to the x3engine LIB
  target on case-insensitive Windows and never relinks the app exe.
- FIX: always `cmake --build build --config Release` (ALL_BUILD). Fallback:
  `msbuild build/app/X3Engine.vcxproj`.
- DETECTION (mandatory, every build): capture `stat -c %Y build/bin/Release/X3Engine.exe`
  before/after; the mtime MUST advance. Report only from a freshly-relinked exe.

### 1.2 MSB3073 — vcpkg applocal fails AFTER the exe links
- SYMPTOM: `error MSB3073 ... applocal.ps1 ...` and the build "fails" — but the exe is fine.
- CAUSE: the vcpkg post-build DLL-copy step (Store-PowerShell invocation) fails
  nondeterministically (became persistent on the main checkout 2026-07-05).
- FIX: none landed yet (task #15). The exe links BEFORE this step — check its mtime; if
  fresh, proceed.
- CASCADE: the failure can abort the SHADERS custom target queued behind the app target →
  new .spv files never compile → "[rhi] shader not found: ...fog.frag.spv" →
  "render device init failed" (presents as a crash/segfault-ish exit on hosts that keep
  going). If a shader is missing, compile manually:
  `/c/VulkanSDK/1.4.341.1/Bin/glslc.exe shaders/foo.frag -o build/shaders_spv/foo.frag.spv`
  then copy to `build/bin/Release/shaders/`. Shader rule: app/CMakeLists.txt ~137-240
  (plain `glslc <src> -o <out>`, plus a ray-query variant block).

### 1.3 New shaders need a RE-CONFIGURE
- SYMPTOM: you added shaders/foo.frag, built, and the .spv doesn't exist.
- CAUSE: the shader list is captured at CMake configure time (explicit list at
  app/CMakeLists.txt ~140-205; new entries land via configure, not build).
- FIX: `cmake -B build -S .` before building whenever ANY new file (shader or source) was
  added — cheap, do it on every merge.

### 1.4 vcpkg toolchain on FRESH build dirs (worktree agents)
- SYMPTOM: fresh-worktree configure fails on vk-bootstrap / vcpkg's `Stb` find-module; and
  once a plain configure has failed, the toolchain is silently ignored forever after.
- FIX: on a FRESH build dir:
  `cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake`.
  If you configured wrong first, DELETE build/ entirely and redo — the cache is poisoned.
  (The main checkout's existing build dir already carries the toolchain; plain re-configure
  there is fine.)

## 2. ASSET STORE + GIT

### 2.5 Store conventions (content-addressed, D:\Assets\X3AssetStore primary)
- Managed dirs (tools/asset_store.py:57): `assets/rigged_glb`, `assets/converted_glb`,
  `assets/surface_library`. Files there are STORE-SERVED: publish with
  `python tools/asset_store.py publish` (writes manifest), materialize with `fetch --all`.
- NEVER git-commit store-served GLBs (W3-2 did; the director had to `git rm --cached` +
  .gitignore them). The manifest sha + fetch-on-boot carries them.
- STALE-COPY TRAP: boot fetch covers MISSING files, not stale ones. After merging a branch
  that store-published updated assets (e.g. re-baked anim GLBs), run `fetch --all` in the
  checkout or the loader silently uses old bytes (W2-D's Attack/Death clips were invisible
  until fetched: symptom was `attack=-1 death=-1` in the loader log).
- MERGE BLOCKERS: `fetch` leaves `*.pre-fetch.bak` files and materializes real bytes over
  tracked LFS pointers — both block merges. Fix: `git checkout -- <tracked glbs>` +
  `rm -f *.pre-fetch.bak`, merge, then `fetch --all` again.
- On OTHER machines: set `X3_ASSET_STORE`/`X3_ASSET_CACHE` env vars (PLAY.bat in the
  portable package does this) — the D:-primary default only exists on I9DEVPC.

## 3. DRESSING / RENDERING SEMANTICS

### 3.1 `CellDressing::place()` is YAW-ONLY (app/cell_dressing.cpp:159)
No pitch/roll. Anything "vertical" that isn't authored vertical renders as a floating
horizontal plank (the R3 "vertical conduit" = Tim's "black wires"). RoomDressing panels
inherit the same convention.

### 3.2 Instance emissive is the fallback for EVERY drawable
Pass an emissive to `place()` and every primitive without material emissive glows — the
whole console body became a cyan toy (R5). Rule: emissive=nullptr + accent LIGHTS for
props; opt-in emissive only for actual light-boxes (sconces, exit signs).

### 3.3 `emissive[3]` SCALES material-authored emissive (cell_dressing.cpp:791-801)
alpha 0 (the default) SUPPRESSES authored GLB emissive (kit pieces carry surprise glow —
a yellow hazard panel in SM_Pipes_A); alpha>0 scales it. Monsters have the analogous
`Tuning::emissiveScale` (F2's pass).

### 3.4 Wall panels: window cutouts + panel width
SM_Wall_B/C carry WINDOW CUTOUTS at each panel center (unglazed holes into neighbors until
F3's glass). panelW = 3 m × (roomH / 4.403) ≈ 2.725 m in 4 m rooms — seams/solid piers are
at multiples of THAT, not 3 m. Only guaranteed-solid: panel seams + graybox door walls.

### 3.5 The 0.14 m inset law
Graybox slabs are 0.2 m thick CENTERED on the plane → inner face at ±0.10. Dress surfaces
at 0.14 in from the plane; 0.10 = coplanar z-fight. Ceiling panels: same law
(kCeilAabb underside anchored at ceilY−0.14). Arrival decks on platforms ride 2 cm proud.

### 3.6 Texture channel law (pixel-verified, twice)
Unity HDRP packs channel-pack masks. glTF/engine convention: **G=roughness, B=metallic**
(mesh.frag samples metallic from mr.b), smoothness = 1−roughness. AD-3 pixel-verified the
"RMA" packs are literally **R=rough, G=metal, B=AO** (this CONTRADICTED F3's earlier
"R=metal, A=smoothness" guess — the bed's scalar M/R workaround predates the correction).
NEVER inject a packed mask raw; convert at curate time (tools/tex_curate.py does).

### 3.7 Fog/grade (AD-1) architecture facts
Fog = its own render-graph pass ("depth-fog", vk_graph.cpp:1454) inserted only when a host
opts in — non-opted worlds are basin-identical by construction. Grade lives in
composite.frag behind `gradeStrength > 0`. Zone values applied per-frame from the player's
room (`applyZoneAtmosphere`); cvars r_fogdensity/r_fogstart/r_gradestrength/r_vignette
(−1 = keep authored). Fog-only zones (ZCave) MUST null-guard the recipe path — rooms with
no surface sets segfaulted room_dressing until W5-1's guard.

### 3.8 Room classification: elevation rule OUTRANKS name rule
room_dressing.cpp classify() (~136): F2's named wards ("Ward A: Keisha") classified
ZMedical by elevation before the name rule ran — silently killing ward-specific dressing
(W5-2's door-tell strip). If a zone recipe "doesn't apply", check classify order FIRST.

## 4. CAPTURE / VERIFICATION

### 4.1 --shot-cam quirks
Format `--screenshot out.png --shot-cam "x,y,z,yaw,pitch"`. A LEADING NEGATIVE first
number parses as a flag and is SILENTLY IGNORED (identical default shot) — prefix a space
inside the quotes: `" -0.3,..."`. Camera dir = (cos yaw, 0, sin yaw). F1 cell: x −1.5..5.5,
z 37..43, floor −2, eye ≈ −0.35. Known-good cameras (W3-1, data-derived): hall
"2.5,-0.8,44.5,0.0,0.02" · security "18.0,-0.55,38.0,0,0" · lab "18.0,-0.55,30.0,0,0" ·
boss "14.0,-2.3,-9.0,-0.6,0" · ward WR-1 "7.6,-0.15,38.9,0.45,0". Cameras embedded in
boundary walls see floating doors/void (doors PVS-cull per-room since W2-A2's gate;
walls always did) — derive cameras from room data, never eyeball coordinates.

### 4.1b `--screenshot --world echotropolis` DOES NOT RENDER STREAMED CONTENT
`regionSet.drawAll()` / `updateAll()` are gated on the WorldStreamer's residency state
(`bindStreamerForDrawGate`, TIER-2 M-B), and the streamer is **only ticked in the live
loop** — never in the capture settle loop. So every still of this world shows the
streamed regions according to a streamer that has never run: the harbour fleet is
neither posed nor drawn, and the pose clock never leaves t ≈ 0.38 s. This is how the
"boat lanes cross dry land" defect survived two filings AND a flood-fill audit while
being plainly visible on Tim's monitor: **every screenshot-based check was looking at an
empty bay.** Two opt-in levers (both default-off, no existing reference capture moves):
`ECHO_SHOT_STREAMED=1` drops the draw gate for the still; `ECHO_SHOT_T=<sec>` offsets the
pose clock so a still can be composed anywhere in the traffic cycle. If you are verifying
ANY moving or streamed content in a still, you must pass both.

### 4.2 The screenshot path and the viewmodel
Until W2-A2, `--screenshot` NEVER drew the FP viewmodel — the "pistol" in old cell shots
was the hovering pickup prop. It draws now, but F1's cell lighting blows the pistol
white-hot IN SCREENSHOTS ONLY (in-game and on other floors it's fine) — unfixed artifact,
don't chase it as a regression.

### 4.3 Headless devices no-op texture loads
Headless/test devices skip texture creation — content falls back to graybox and tests
assert structure, not pixels (--test-oceanbase stays green with zero textures). Never
conclude "textures broken" from a headless test, and never claim "textured" without a
real render.

### 4.4 Motion can't be proven by one still
Patrols/animations: verify by state-machine self-test (spawn, tick, assert displacement /
clip index) + say plainly what the eye couldn't confirm. There is no settle-frame knob in
the screenshot path for staggered captures.

## 5. PIPELINES

### 5.1 Blender (MS-Store install) headless protocol
The Store launcher DETACHES with no stdout. Run `blender --background --python <script>`
with explicit log redirection and .log/.done marker files
(docs/NOTE_TO_FLEET_unity_pack_pipeline.md is the reference; tools/attack_death_bake.py,
struggle_bake.py, bake_fp_arms.py all follow it). ASCII-only .ps1 wrappers.
convert_fbx_glb.py is a bpy script — it does NOT run under system python.

### 5.2 Anim bake conventions
Bakes append clips to the existing GLBs and store-publish (52-105 MB) — see 2.5's
stale-copy trap. Loader log prints per-GLB clip lists (`attack=0 death=1 ...`) — that log
line is the cheap verification. Rigs: marcus/martinez = Rigify-style full biped;
crawler = single-Root core; Anna variants: Casual/Tactical GPU-skinned, BodySuit STATIC
(not skinnable).

### 5.3 Worktree agents build their OWN build dir
Never share the main checkout's build (file locks, config drift). Fresh dir + toolchain
(1.4). Fetch assets first (`fetch --all`) — worktrees start with pointer files only.

## 6. MISC RUNTIME
- `[phys] removeBody: invalid/stale id (warned once)` = benign shutdown-order double
  release (level1_game.cpp ~531 documents it); warn-once by design; not reproducible in
  isolation.
- robocopy exit code 1 = SUCCESS (files copied) — harness flags it as an error; read the
  output, not the code. Same for `grep -c` returning 0 matches (exit 1).
- The audio system had ONE looping channel (music); ambient "loops" were retrigger timers
  until W5-4 (feat/loop-channels) — check that branch's state before adding ambience.
- Known open visual bugs (AD-2 survey): showroom-ragdoll shutdown segfault (exit 139 after
  writing its PNG), main-menu "RESOLUTION: 0 x 0", worldmap POI label overlap, RT emitter
  speckle, showroom-fp near-black camera.
