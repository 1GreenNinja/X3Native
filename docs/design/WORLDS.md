# THE ONE WORLD — world modes after the merge (SEAM 4)

*2026-07-09, session 392f6e4d. Tim's world-merge order 2→3→1→4 executed. This doc
is the policy: which `--world` values are THE GAME and which are dev shortcuts.*

## THE GAME

**`canonlevel` (the DEFAULT — a bare launch/START boots it).** One continuous
world, no loading screens:

- **The facility**: the 7-floor canonical LevelArchitect tower (loadCanonTower),
  cell R4-R7 polish, secret room + trapdoor + holo terminal, recipe-dressed rooms,
  boss ladder + captives + Sarah endgame, deferred F2-F7 squads.
- **The vertical spine (seam 1)**: the real x3-elevator.js-parity cab in the
  Elevator Lobby spine — E summons it to your landing; OLED telemetry, muzak,
  cable creaks, horror events, the once-only cable-slip; code **1127** = loud
  disco descent through THE DESCENT strata to **Club 1127** at Y=-200 (arrival
  hands you off at the club entrance).
- **The skin (seam 2)**: the glass curtain-wall exterior wraps the real tower
  (facility_exterior module, factored from the surface host); walk Main Hall →
  Entrance → breach → apron under the golden-hour sky.
- **The planet (seam 3)**: WorldStreamer regions from `assets/world/regions.canon.json`
  (city / ocean_base / surface_landmarks; spire_f1 deliberately absent — the tower
  is the master world). Terrain ring + horizon stitch + 15 km far plane; Crash
  Site at (140,205) off the +Z breach face; streamed entities live in the
  `kStreamedExteriorRoom` PVS zone (drawn only when the eye is outdoors/at the
  breach — "outdoors" = above -2 m OR above local terrain - 15 m, so the below-
  grade river valley and the sea count); residency is suppressed below Y=-20 so
  nothing interferes with the club/strata underworld.
- **The water (W10 — SWIMMING)**: THE RIVER (terrain wave 1ebe8e6) and the sea
  at -10 are REAL water. `worldWaterLevelAt(x,z)` (app/terrain.h, pure — same
  spline/half-width as the carve + ribbon, same `kWorldSeaLevel` as the ocean
  plane) feeds the Player via `setWaterQuery`; depth > 1.35 m enters the swim
  state (gravity off, buoyancy rests the eye just above the surface, move along
  the full look at ~60% walk speed, Space strokes up, Ctrl/C dives), depth <
  1.05 m exits onto the bank. Camera under a surface = dense blue-green fog,
  restored to the room-recipe fog on surfacing. Wading in the shallows is
  unchanged. (3P swim animation: none yet — FP is the game view; follow-up.)

`intro` is the same world entered through the cold-open.

## DEV SHORTCUTS (kept, supported, NOT the game)

Jump straight to a slice for iteration/testing/screenshots. Never author new
game content against these when the canon world can host it — that is how the
two-line split happened (see ATTENTION_FableAAA.md).

| Mode | What it is now |
|---|---|
| `level1` | the legacy hand-coded spire (pre-canon reference; still fully souped) |
| `surface` | Act-1 cold-open landing slice (exterior module's other caller) |
| `streamed` | the streamer's own tour world (regions.json WITH spire_f1 leveldoc) |
| `elevator` / `elevator-showcase` | walkable/beauty elevator showcase (own cab kit) |
| `club` | Club 1127 standalone (the same module the canon descent builds) |
| `strata` | THE DESCENT standalone |
| `terrain` / `valley` / `cliffs` / `ocean` | biome slices |
| `city` | city lane standalone |
| `drive` / `boat` / `fly` | vehicle slices |
| `showroom` | asset showroom family (screenshot hosts) |
| `space` | intro space combat slice |
| `fromdoc` | live-edit LevelDoc world (editor loop) |
| `destruct` / `physjoint` / `ragdoll` | physics test benches |

## Rules

1. New gameplay/content lands in the canon world (or data it loads). A dev world
   may host the *iteration*, but the wiring PR must land it in canonlevel.
2. Anything the canon world reuses from a dev slice must be a shared module
   (facility_exterior, club1127, strata, elevator are the pattern), never a copy.
3. The region graph for the game is `regions.canon.json`; `regions.json` belongs
   to the `streamed` dev tour. Keep spire_f1 out of the canon graph — the tower
   must never double-build.
4. Screenshot/test hosts stay — they are the Fable gate's proof machinery.
