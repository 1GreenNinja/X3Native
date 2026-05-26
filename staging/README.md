# Staged fixes (apply AFTER the data-driven level rebuild lands)

These are written-and-verified-by-reading patches, parked here so the in-flight
level-rebuild agent (editing level1.cpp / level1_game.cpp / scene.cpp / door.cpp /
env_art / main.cpp / new level_loader.*) can't clobber them. Apply each after the
rebuild merges, rebuild, and re-gate (`--smoketest` 0 VUID + allocationCount=0).

| File | Task | Touches | Conflicts w/ level agent? |
|------|------|---------|---------------------------|
| `barrels_glb_patch.md`   | #31 | `app/barrels.cpp` + `.h` | No (agent doesn't touch barrels.cpp) — apply anytime |
| `blood_fx_patch.md`      | #19 | `app/fx.cpp`             | No — apply anytime |
| `crouch_crawl_patch.md`  | #23 | `app/player.{h,cpp}` + `app/main.cpp` input | main.cpp MAYBE — apply after agent, re-check the input block |
| `girls_dialog.json`      | #25/#28 | data (consumed by rescue.cpp dialog) | No |
| `girls_characters.md`    | #26 | character bible (Emily=blonde scientist, model gap, infection theme) | No |

NOTE: barrels.cpp + fx.cpp are NOT in the level agent's scope, so #31 and #19 can be
applied immediately if desired. Crouch touches main.cpp's input polling, which the
agent may also edit — apply it last and re-locate the input block.
