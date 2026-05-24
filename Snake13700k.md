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
