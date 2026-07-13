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
  unchanged. Splash audio is real (tools/gen_water_audio.py -> assets/audio/
  water/, entry + surface-exit takes); while swimming the FP weapon LOWERS out
  of the water line and firing is refused (a soft click); the 3P avatar reads
  PRONE at the surface with the walk clip at 0.6x as the stroke stand-in (a
  retargeted stroke clip is the follow-up). EXCEPTION (the water zap, below): the
  LIGHTNING gun is NOT lowered and IS allowed to fire while swimming.
- **The fish (app/fish.h)**: THE RIVER LIVES — 60 fish in 7 ambient SCHOOLS (5
  along the river reach nearest the facility, seeded on the `worldRiverNodes`
  spline; 2 in the sea shallows at the estuary). Boids-lite schooling (drifting
  water-probed school center + slot cohesion / separation / alignment),
  depth-bound between bed and surface, FLEE from the player within 2.5 m (swim
  through a shoal and it PARTS). Kinematic (no bodies), deterministic (one LCG),
  per-school range gate + `kStreamedExteriorRoom` PVS.
  - **REAL SPECIES (Rodin scans).** `assets/rigged_glb/Fish_{Rudd,Bream,Perch,
    Pike}.glb`, built by `tools/fish_bake.py` from 90k-500k-tri Rodin models:
    decimated to 1.3-4k tris with the FINS PROTECTED (a thickness raycast puts
    thin geometry in a vertex group the decimator is told to spare — otherwise
    the collapser eats the dorsal and the caudal fork FIRST, and the fork IS the
    silhouette), maps down to 512² (1024² for the pike), then RIGGED with a
    4-bone spine and POSE-BAKED.
  - **The species table** (`fishSpecies()`): rudd (26 cm) + bream (30 cm) SHOAL;
    perch (24 cm) run in small loose GANGS of 5; the **PIKE (92 cm) IS A LONER** —
    one fish, `solitary`, half the yaw rate and a third the speed of the shoal.
    Predators do not shoal, and it is the fish you notice.
  - **The swim is a MESH SWAP, not runtime skinning.** The engine has no instanced
    skinned draw (the RHI keys the joint palette by MeshHandle — one palette per
    mesh — and the only skinned path is one MonsterSystem per instance at ~15-25 ms
    of spawn, whose `setPropPose(pos, yaw)` has no ROLL and so cannot express a fish
    banking, let alone floating belly-up). So `fish_bake.py` evaluates the rig
    offline at 29 phases (16 cruise + 12 flee-burst + 1 slack-dead) and freezes the
    DEFORMED mesh at each; the runtime just picks the pose for a fish's
    `time * beatHz + phase`. Real skinned deformation (fins bend WITH the body,
    nothing shears the way a rigid hinge-chain would scissor a dorsal that spans a
    joint) for ZERO per-frame vertex work, and ONE draw per fish instead of three.
  - **Full PBR**: `Entity{tex, normalTex, mrTex}` routes the fish through
    `drawMeshPBR` — the scales/stripes live in the NORMAL MAP, not the 1.4k-tri
    geometry (`MeshVertex` has no tangents; `mesh.frag` rebuilds the TBN from
    screen-space derivatives). The submerged river is nearly unlit, so the albedo
    is *also* bound as `emissiveTex` at 0.35 — an ambient fill in the fish's OWN
    colours, so the pike's olive back and red caudal lift out of the gloom instead
    of going black, and a black texel stays black.
  - **FALLBACK, never a statue**: a missing/failed species GLB degrades that species
    to the original procedural lofted body (countershaded, 3 hinged pieces, the
    travelling-sine S-flex) — asserted by `--test-waterzap` Z9.
  - Cost: 60 fish = **85,408 tris across 60 draws**; ~140 ms of one-time boot for
    the four GLB loads.
- **The water zap (app/waterzap.h)**: "Lightning gun will electrify the water..
  one Zap, and the player takes half health damage, and all the fish around die"
  (Tim). A LIGHTNING shot whose ray MEETS the water (`findWaterEntry` marches it
  against `worldWaterLevelAt`) — or one fired by a shooter who is IN the water —
  makes the surface go LIVE: jagged arcs (the CombatFx bolt path) spider out to
  **12 m** (`kWaterZapRadius`) with a flash + the Vefects zap take. ONE zap per
  trigger pull (`WaterZapper` latch + a **1.75 s** cooldown — a held beam does
  NOT re-zap). DAMAGE: the player, if he is IN the water (feet below the surface)
  inside the radius, loses **HALF OF MAX HEALTH (50 of 100), once**; every live
  fish in the radius DIES (belly-up, floats proud of the surface, drifts,
  despawns after 26 s); anything wading in it takes 150 Energy; crowds scatter.
  Gate: `--test-waterzap` (Z1-Z8). NOTE (engine): normal glass replays in the
  depth pre-pass (engine/rhi/vk/vk_passes.cpp), so LIVE fish under the surface
  are hidden when seen from the bank — they read underwater/while swimming, and
  the DEAD ones float proud of the plane so the aftermath reads from the bank.
- **The wanted system (polish)**: the facility AlertSystem (app/alert.h) is
  ARMED in canonlevel — canon hostiles are its eyes/ears, gunshots/bodies/keypad
  tampers feed heat, effects land on the canon world (reinforcements through the
  nearest door via the 1/frame deferred-spawn queue, the level-3 LOCKDOWN over
  canonDoors, red-shifted room lights, the HUD alert chip). SCOPE: facility-
  interior only — observations are gated on the event position resolving to a
  tower room, so the streamed outdoors / crowd scatter never raises it.

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
