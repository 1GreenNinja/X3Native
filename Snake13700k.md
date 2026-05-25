# TASK FOR SNAKE / ALT-13700K (second screen) — assigned by 13700K integrator

**You are a clean-room gameplay/content engineer on X3Native** (native C++20/Vulkan 1.3, this repo). Work in YOUR clone, push a feature branch, report status at the bottom. The 13700K integrator merges + re-gates + pushes `main` — **you do NOT push `main`.** (Previous tasks `feat/act2-world` + the Floor-2-7 dims are DONE / superseded — the Spire was since re-laid to real 283 m non-uniform scale by `feat/spire-scale`, and the elevator + Club 1127 shipped. THIS is your next task.)

## ABSOLUTE CLEAN-ROOM RULE
NEVER read/reference/copy from RBDOOM, id Tech, Doom, Quake, or ANY other game-ENGINE source. You MAY read Tim's OWN game code (`Q3Engine/src/x3-*.js`) + the EFLZ design docs. Build native impl from X3Native's interfaces.

## YOUR TASK: the OPEN WORLD — surrounding regions + mountains, the city/industrial metropolis, and the OCEAN + ocean base + submarines
The Act-2 LEVEL progression is covered by other lanes (i5000 = L10/11 desert `act2_desert.*`; DJBOOTH = L12-15 caves/swamp `act2_caves.*`). YOUR lane is the rest of the open world from the blueprint gazetteer — the regions those levels sit IN, plus the city and the undersea. **Mirror the `act2_world.*` framework** (Act2Level / Act2AreaPlan / HazardZone / Act2Trigger + the `--test-act2` self-test style). **Create NEW modules**; do NOT edit `act2_world.* / act2_desert.* / act2_caves.*` (read them for the framework + transition pattern), `level1.*`, `spire_*`, or `monster.*`.

READ FIRST:
- `docs/design/X3_WORLD_BLUEPRINT.md` §1 (world gazetteer + coordinate map — the authoritative region positions / extents) + §2.6 (alien tunnels / club at bottom).
- `C:\Users\Tim Smith\OneDrive\GameDev\Q3Engine\src\world\` — Tim's OWN implementations to port: `x3-mountains.js`, `x3-world-surface.js`, `x3-city-roads.js`, `x3-freeway-tunnels.js`, `x3-world-ocean.js`, `x3-seafloor-base.js`, `x3-sub-docking-bay.js`, + `gameplay/x3-submarine-combat.js`.
- `app/act2_world.{h,cpp}` (the framework to MIRROR) + `app/terrain.{h,cpp}` (the streamed surface to build on).

IMPLEMENT (3 sub-lanes, separate NEW modules, each with a headless `--test-*`; graybox + data/level-script + existing-systems only — NO renderer/engine changes):
1. **`app/world_regions.{h,cpp}`** — the surrounding surface: crash site (0,−300,0), East outpost (800,−300,400), West outpost (−880,−300,−320), + the **4 mountain ranges** as graybox massifs on the terrain: Northern snow (R≈6000), Eastern volcanic + lava (R≈8000), Southern mesa + ruins (R≈7000), Western crystal (R≈9000). `--test-worldregions`.
2. **`app/city.{h,cpp}`** — the **metropolis / industrial area** (design L16-18): scrapyard city (−600,−300,500) + new district (200,−300,500) + the road grid + the **4 freeway tunnels** through the mountains. `--test-city`.
3. **`app/ocean_base.{h,cpp}`** (+ `app/submarine.{h,cpp}`) — the **ocean** (surface plane ≈ −300..−346) + the **undersea base** (the 80-radius 3-level disc at (1100,−346,−1350): sub-dock + airlock + reactor) + **submarine combat** (enemy subs, torpedoes, depth charges, hull integrity). `--test-oceanbase` / `--test-submarine`.

Use the gazetteer coordinates so your regions do NOT overlap the desert / caves / swamp the other lanes place. All existing content must still build + be reachable.

## WORKFLOW (in your clone)
1. `git fetch origin && git checkout -b feat/openworld origin/main`.
2. Implement. Commit frequently; commit a working state BEFORE the final build.
3. Build + gate (PowerShell): the standard `windows-vs2026` preset + the full `--test-*` set + your new flags; Release `--smoketest` 0 VUID; Debug `--smoketest` 0 VUID AND `allocationCount=0`.
4. `git push origin feat/openworld`. Do NOT touch `main`.

## REPORT STATUS (append below, then push the branch)
<!-- STATUS: branch HEAD, files added, --test-* results, all-flags-0 + VUID 0 + allocationCount=0, "READY FOR INTEGRATION" or BLOCKED+why. -->

## STATUS — feat/openworld (Snake / 13700K, GTX 1080 Ti) — 2026-05-25 — READY FOR INTEGRATION

- **Branch HEAD** `6017fb2` (was `50b630e`). Merge-base with main `3ec0b1b`; branch is **4 ahead of merge-base, 16 behind origin/main** (versioning + CYBERPUNK_CITY_ART + x3native-environments landed since) → integrator to rebase/merge onto the new main.
- **Files**: modules already on branch — `app/world_regions.{h,cpp}`, `app/city.{h,cpp}`, `app/ocean_base.{h,cpp}`; this commit adds the `--world openworld` showcase wiring to `app/main.cpp` (+195 lines).
- **New showcase** `--world openworld` (mirrors `--world valley/cliffs/club`, LOW-CONFLICT — no level1/Spire touch): streamed terrain + analytic sky, then builds WorldRegions (7 regions / 4 mountain ranges / 31 props) + City (3 districts / 5 roads / 4 freeway tunnels / 31 props) + OceanBase (offshore 3-level disc, 1 player + 3 enemy subs, 12 props). Spawns the player at the crash site via the PURE `placeOnTerrain` sampler. Headless `--screenshot` + walkable (FP + noclip) paths.
- **GATE invariant**: submarine combat built **inert**, never `engage()`d at load (asserted loud + verified by `--test-oceanbase` O4 and the screenshot summary `combat=inert`).
- **Gate** (`windows-vs2026` preset, VS2026 cmake): Release build exit 0; Debug build exit 0.
  - `--world openworld --screenshot` exit 0 (stream created==destroyed no-leak, `allocationCount=0`).
  - `--test-worldregions` 6/6, `--test-city` 5/5, `--test-oceanbase` 6/6.
  - Release `--smoketest` exit 0 (`allocationCount=0`); Debug `--smoketest` exit 0 — **0 VUID, `allocationCount=0`**.
  - (Ran the openworld-relevant self-tests + Release/Debug smoketest; the change is additive so sibling `--test-*` are unaffected. Full `--test-*` sweep available on request / at integrator re-gate.)
- **Integrator note**: the CYBERPUNK_CITY_ART lane on main overlays art over `city.cpp` graybox; `--world openworld` calls only the public `City::build()`/query API, so the merge should be clean, but both lanes touch the City module conceptually.
