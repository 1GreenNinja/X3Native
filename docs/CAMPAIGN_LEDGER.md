# CAMPAIGN LEDGER — where Escape From Lab Zero stands
*Snapshot at docs/fable-codex branch time (2026-07-07, Fable's final day). Branch:
`integration/honor-fable-final` (main untouched at the promoted empire-fold). Read this
FIRST in a new session, then ENGINE_GOTCHAS.md, then AGENT_PLAYBOOK.md.*

## Shipped, per wave (one line + anchor commit)
- **R1-R5 cell calibration** — the hand-polished reference room; frozen. `62c506c`
- **Wave 1** (enemy PBR · trapdoor/secret-room/holo-terminal ported to canon · cot +
  glazing · the fixture-cluster kill) — `a6fa119`
- **Wave 2** (all SFX committed+wired · FP arms · attack/death anims · door seating +
  `--test-levellint` · the 12-item wiring round) — `b0ec8d8`
- **Art direction** (ART_BIBLE law · fog+grade engine levers · surface library: 2,626
  sets cataloged / 20 curated · 66-shot survey + red-line) — `d494f7d`
- **Wave 3** (room-recipe system + Floor 1 dressed · FULL 7-FLOOR TOWER from data, 124
  rooms lint-0, elevator spine · exterior tower + space starfield · ocean sub-base) —
  `e531357` / `f73156e`
- **Wave 4** (Keisha/Emily/Aria in F2 wards + escort/extraction · 6-floor boss ladder ·
  VIGIL on the cell terminal (chattree 50/50; in-engine LLM, model-optional) · guard
  patrols + species barks · rescue set-piece design doc) — `9218d61`
- **Wave 5** (interrupt set-piece: tiers/ring/struggle-bake, rescue 26/26 · Sarah + THE
  ENDING: clone-gated F7 rescue → helipad win card, `--test-goldenpath` 9/9 · LEVEL 4.5:
  the Nexus void reveal, whisper beat, dormant apex) — `d04e39c` + wiring `9932d9e`
- **In flight at snapshot**: W5-4 loop-channel audio; W6-1 engine-debt sweep; W6-2
  ships-PBR + white tower; W6-3 terrain/club/sleeve polish; W7 codex (this).

## OPEN — Tim's decisions (surface these, don't decide them)
1. **"Dr. Chen"** — F2 boss's DATA name is banned in Tim's writing canon. Strings show
   "Mutated Overseer"; the JSON value awaits his ruling (rename vs canon divergence).
2. **Interrupt-tier feel** — time-to-kill (shipped) vs the diegetic pip ring (shipped,
   `setRingEnabled`/cvar). He A/Bs in playtest and picks.
3. **The apex creature** — 4.5's Apex Arena holds a ×3 Verthani STAND-IN; the true
   70-80 ft monster is an asset commission he owns.
4. **Playtest gates (feel, unverifiable headless)**: the elevator ride (E in any lobby),
   VIGIL live typing on the glass, escort/follow feel, set-piece pacing + ring read,
   patrol cadence, the whisper-beat volume, FP-arms look (5/10, needs sleeve pass).

## Follow-up backlog (with the owning context)
- Hall/lobby terminal interactivity (VIGIL is cell-terminal-only; consoles elsewhere are
  props — needs interact plumbing; W4-2's report).
- Per-floor corridor accent variation (corridors read similar F2-F7; W3-2's residual).
- Sub-base interior + entry transition + collision (visual-only zone; W3-4's Wave-5 list).
- Club1127 architecture textures (pattern established by ocean-base; W6-3 may cover).
- **The desc-field gold**: floors' room `desc` fields carry unmined design ("Coolant
  System — sabotage = boss weakness", "EMP device craftable here") — a whole content
  pass authored in data (W5-1's find).
- Frosted-glass limit (glass pass pre-blends; true clear glass = engine work; F3/W3-3).
- Romance safe-room scenes (`loc.private` gate exists in trees; unbuilt; design doc §4).
- RESCUE_SETPIECE_DESIGN.md remaining items: assault staging animation on the victim
  side, lost-tier transform visualization (currently a hard cut).
- Screenshot-path white-hot viewmodel on F1 (artifact, gotchas 4.2); AD-2's bug list
  (ragdoll-host segfault, menu 0x0, worldmap labels, RT speckle, showroom-fp).
- Ships-onto-PBR + hull readability in space (W6-2 in flight at snapshot).
- Task backlog carried from before the campaign: #6 save-unify, #7 settings menu,
  #8 test-runner speedup, #10 LLM tier B, #11 audio polish/HRTF, #13 Phase B LFS
  rewrite [HUMAN], #15 MSB3073 + Debug smoketest crash, #17 meshlets.

## Michigan (Tim's remote 7700K/GTX 1060 3GB box)
Base: `X3_Portable_1060.zip.part1/2` + `JOIN_X3.bat` (CRD 500 MB cap forced the split) —
package root has PLAY.bat (sets X3_ASSET_STORE into the package). Updates staged on
D:\GameDev\: `X3_Update_Wave3.zip` (exe+shaders), `X3_Assets_Wave3.zip` (219.6 MB surface
library + manifest), `X3_Update_Wave4_exe.zip` + `_data.zip`. Post-Wave-5 the pattern
continues: exe+shaders zip over build\bin\Release\ + assets-delta zip when the manifest
grew. Pascal = SSR-only automatically; 3 GB VRAM fine at 1080p today; watch VRAM as
texture floors multiply (texture-cap cvar is the planned mitigation).

## The state of truth
- Gates that define "not broken": `--test-levellint` (124 rooms, 0) · `--test-goldenpath`
  (9/9, cell→win) · `--test-rescue` (26) · canonlevel (16) · secretroom (8) · chattree
  (50) · smoketest 0 VUID / allocationCount=0.
- The game is COMPLETABLE: wake in the cell → code 1278 → hatch → tower → optional ward
  rescues (tiered) → optional 4.5 descent → F7 clone → Sarah → helipad → "YOU GOT HER
  OUT." with the rescue tally.
- Art law: docs/design/ART_BIBLE.md. Plans: WAVE3_WAVE4_PLAN.md, FABLE_FINAL_DAY_PLAN.md,
  RESCUE_SETPIECE_DESIGN.md. Method: AGENT_PLAYBOOK.md. Traps: ENGINE_GOTCHAS.md.
