# CLAUDENOTES — X3Native session handoff (read this first, then continue)

_Written 2026-05-30 by Opus 4.7 at end-of-context, for the next session (Opus 4.8). Tim is about to /clear._

## ⚠️ STATE CHANGED since these notes were written (check before building!)
As of 2026-05-30 late: branch advanced **`945f366` → `b2acd69`** (new commits NOT mine:
keycard/keypad locks, **PBR shading** normal-map + GGX slices, Unity-pack→GLB converter), AND
there are **~21 UNCOMMITTED modified files** in the working tree — PBR shaders (`mesh.frag/vert`,
`depth.vert`, `shadow.vert`, `sky.frag`), `app/editor/`, `env_art`, `rescue.cpp`,
`VulkanRenderDevice.cpp`, `vcpkg.json`. **This is a PARALLEL SESSION's live WIP — do NOT commit,
revert, or build over it without confirming who owns it.** Run `git status` + `git log --oneline -8`
FIRST. The commit hashes below (`945f366` etc.) are now historical, not HEAD.

## What this project is
**X3Native** — native C++20 / Vulkan 1.3 game engine for "Escape from Lab Zero" (EFLZ).
Repo: `C:\GameDev\X3Native-engine`. Working branch: **`feat/doors-death-anim`**.
Tim = owner/architect on the 14900K/RTX 5090 ("the MASTER 14900k"), directing an AI fleet.

## Current branch state — `feat/doors-death-anim` @ `945f366` (all pushed)
Everything below is committed + pushed to `origin/feat/doors-death-anim`. Nothing uncommitted.

## Fleet / git workflow (IMPORTANT — don't break this)
- **NEVER push to `main`.** 13700K ("Snake", the integrator) owns integration. He does NOT
  merge to main directly either — he stages everything on **`origin/feat/cull-combined`** (a
  consolidation branch; currently version 0.4 with swim/space/mech controllers, weapons
  artpass, glass system, healthbars). He pulls feature branches into cull-combined.
- I push only to `feat/doors-death-anim`. Push every commit as it lands (Tim's standing order).
- **Worktree agents base off the MAIN/glass lineage, NOT my branch.** When dispatching agents
  with `isolation: worktree`, they branch from a FLEET-docs/main-based commit (merge-base is
  old). So: **CHERRY-PICK the single feature commit** (`git cherry-pick -x <hash>`) onto
  feat/doors-death-anim rather than `git merge` (a merge drags all of main's glass/FLEET work
  in). Each agent reports its branch + commit hash for exactly this. This worked cleanly for 5
  agents this session.
- Build gotcha: **kill any running `X3Engine.exe` before building** (it locks
  `build/bin/Release/X3Engine.exe`). PowerShell: `Get-Process X3Engine -EA SilentlyContinue | Stop-Process -Force`.
- Build: `cmake --build C:\GameDev\X3Native-engine\build --config Release` (~50s full, ~4s incremental).
- Launch for playtest: `Start-Process .../X3Engine.exe -ArgumentList "--world","canonlevel"`.

## Third-person view (the big feature this session) — DONE + tuned
- **F1 = first-person, F2 = third-person** (FP is default). Jake is the player avatar.
- Jake = `assets/rigged_glb/Jake_22_actions.glb` (Mixamo, 34 bones, 22 clips, committed via LFS).
- **Jake_22_actions.glb has chronic XYZ authoring issues** ("they NEVER EVER got it right" across
  every prior project). FIXED via skeleton-based fit + Tim-dialed values BAKED as member defaults
  in `app/thirdperson.h`: `m_userYOff=1.03` (feet on floor), `m_userYawOff=1.5708` (+90°, GLB
  faces +X not -Z), `m_camDist=2.3`, `m_camHeight=0.37`. Don't "re-fix" these — they're correct.
- Walk threshold lowered to 0.2 m/s (was 1.5) so any motion animates — `setLocomotionClips(...,0.2f,2.0f)`.
- 3 playtest bugs fixed in `945f366`: spawn-tilt (stale crouch amount on FP→3P re-entry),
  backpedal-spin (now faces look-yaw + plays back clip), and **sustained-fire FREEZE** (root:
  `Skinner::triggerClip()` re-seeded the crossfade clock every frame, pinning the fire pose at
  t=0 — now idempotent).

## ⏭️ IMMEDIATE NEXT JOB (Tim asked for this right before /clear) — Task #53
**Held weapon position is "horribly wrong" and differs shooting-vs-walking.**
- Root cause: the gun is rigidly socketed to `mixamorigRightHand`; the walk clip vs the fire
  clip put the hand bone in different poses → same grip offset → different world placement.
  Plus #46's `kTpGripTable` values (in `thirdperson.h`) were chosen BLIND by an agent, never
  visually verified — so they're just wrong.
- **What Tim wants:** an in-game live-tuning tool — XYZ position sliders + rotation gizmo (or
  cvars, like the proven `jake_yoff`/`jake_yawoff_deg`/`jake_camdist`/`jake_camh` pattern we used
  to dial Jake's body then baked) — on the CURRENT weapon's grip. Tim dials each of ~8 weapons
  (pistol/smg/shotgun/plasma/chaingun/plasma_rifle/lightning/default) until the gun sits right in
  hand, tells you the numbers, then you **bake accurate per-weapon coords into `kTpGripTable`** and
  remove the tuning cvars.
- The proven pattern: register cvars in `main.cpp` (near `r_culldepth`), read them per-frame into
  `ThirdPersonView` setters, apply in `handSocketWorld`/`drawHeldWeapon`. We did exactly this for
  Jake's body (commit `acc274e` → baked `4653aa8`).
- May need a SEPARATE grip for the fire/aim pose, OR tune so it looks right in the aim pose (what
  you see during combat). Code: `kTpGripTable`, `handSocketWorld(weaponName)`, `drawHeldWeapon` in
  `app/thirdperson.{h,cpp}`.

## Held-weapon-on-floor fix (already done, `cca37d2`)
The weapon was lying on the floor because `Arsenal::drawCurrentAt` (3P-only) multiplied the hand
matrix by the weapon GLB's full node transform whose authored FP-viewmodel TRANSLATION flung it
away. Fix: zero the node translation (cols 12-14), keep orientation/scale. (So the *gross*
placement is now in-hand; #53 is the *fine* grip tuning.)

## Open tasks (see TaskList for full set)
- **#53** held-weapon grip live-tune + bake ← DO THIS NEXT
- #20 weapon viewmodel TEXTURES — DON'T blind-agent it: GPU-serialized SD gen (one at a time on
  5090) AND possibly moot now that 13700K's weapons-artpass added new weapon GLBs. Real next step:
  pull 13700K's new weapon GLBs, check if texture problem still exists (5-min look, no agent).
- #50 re-rig `AnnaBodySuit.glb` (Keisha) — asset has no skeleton, renders static (asset work).
- #51 intermittent DEBUG-only startup abort (EXIT 127/1, 0 frames) — PRE-EXISTING/environmental
  (reproduces on unmodified baseline), Release is 100% stable. Low priority.
- #34 propagate culling + data-driven loader to all forks. #39 native Level Editor (big).
- #13 NPC interaction, #17 ECS GPU-driven, #24 elevator sliding doors, #25 companion overlap,
  #26 L2 rescue storyline, #27 cell-pickup glow/spin.

## Recently shipped this session (all on feat/doors-death-anim, pushed)
- `cca37d2` held weapon in hand · `3d0c578` rescue girls animate (Aria+Emily; Keisha=#50)
- `d92d6fa` intro cold-open (`--world intro`) · `06b2336` loading screen
- `ce69ada` 3P polish (per-weapon grip table, synth crouch, over-shoulder aim)
- `945f366` 3P 3-bug fix (tilt/backpedal/freeze)
- `ed05690` canonical level JSON committed (`assets/levels/EscapeLab48_AllFloors_v2.project.json`)
  + path fallback so non-14900K machines (i9 Dell) find it.

## Test/verify discipline
Gate after any change: relevant `--test-*` (e.g. `--test-thirdperson` now 16/16, `--test-anim`,
`--test-locomotion`, `--test-rescue`, `--test-canonplay`, `--test-canonlevel`, `--test-loading`,
`--test-intro`) + Release AND Debug `--smoketest` (default + `--world canonlevel`) must be
0 VUID + allocationCount=0 + clean exit. **Visual correctness is Tim's eyeball job** — don't claim
a 3D/visual thing "works" from headless gating alone; say what you verified vs. what he must check.

## Memory
Auto-memory at `C:\Users\Tim\.claude\projects\C--users-tim\memory\`. The 3P story is in
`project_x3native_thirdperson.md` (has the baked Jake values + Mixamo rig facts). Today = 2026-05-30.
