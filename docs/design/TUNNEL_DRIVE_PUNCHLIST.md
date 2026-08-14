# Tunnel drive — Tim's live punchlist, 2026-08-14

Captured while Tim drove `--world tunnel` on `inspx/mountain-tunnels` and called
defects out in real time. Written down immediately for the reason
`GAME_BACKLOG.md` exists: a spoken list lives only in a session and dies at the
next `/clear`.

Status legend: **DONE** = fixed and built · **IN FLIGHT** = actively being worked
· **OPEN** = captured, not started.

---

## DONE this session

### 1. ESC quit the game instead of pausing — DONE (stopgap, see §2)
`host_tunnel.cpp` did `if (ESC) break;`, dropping out of the frame loop and
killing the process mid-screenshot. Now edge-triggered pause: the camera still
orbits so a shot can be framed from any angle, but input, physics, streaming and
scene update all freeze. Close the window to quit.

**This is a stopgap and should be replaced by §2.**

### 2. The car was PLACED facing the wrong way — DONE
Tim: *"The car is PLACED facing the wrong way. I have to TURN it to drive it
forward. Controls make the car behave as it should."*

That last sentence is the whole diagnosis: mesh and rig agree, so this was never
the skin transform (`kBodySkin`) or the wheel axis. It was the spawn yaw.

Engine forward at rest is **-Z**. Rotating `(0,0,-1)` about +Y by `theta` gives
`(-sin theta, 0, -cos theta)`, so facing corridor direction `(dirX, dirZ)`
requires `theta = atan2(-dirX, -dirZ)`. The old code used
`atan2(dirZ, dirX)` — an angle measured from **+X** — then negated it, landing
the car 90 degrees off the road.

A note was added at `kBodySkin` in `app/vehicle.cpp` recording that the matrix is
verified correct, so the next agent does not "fix" it.

---

## IN FLIGHT

### 3. The mountain is TORN OPEN above the portal
A vertical chimney is ripped up through the massif above and behind the tunnel
mouth, with long stalactite shard triangles hanging over the entrance. The tunnel
itself is open, drivable and lit — only the carve is wrong.

Tim: *"the tunnel needs to go through solid mountain, not that mess"* and
crucially *"torn mountain is a bug he created in about 3 minutes when he made the
better looking mountain"* — a recent regression from the mountain-appearance
commits (rock-scale relief / second rock band / bluff terracing), **not** a deep
geometry problem.

Fix direction (Tim): *"we just need the tunnel carver to limit the height of the
carve."* Prime suspect is the portal plug's full-cut depth being computed against
`latMax` with no upper clamp — fine under a 55 m hummock, a canyon under a 160 m
peak. There is precedent for exactly this shape of bug and fix in `TUNNEL_NEXT.md`
§4 ("Road-shaped cutout across the mountain top"), where the bored branch pinned
at `kTcMaxScar`.

Must not reintroduce the "tunnel plugged with earth" bug (`TUNNEL_NEXT.md` §7).
Keep the `X3_TUNNEL_PORTAL_CUT=0` kill switch working.

---

## OPEN

### 4. The console and the pause menu should be in EVERY build
Tim: *"Shouldn't the ESC menu from the main game be in here anyway? We don't need
to re-invent. Also, the console. I think the console and menu system should be in
all builds."*

He is right, and the components already exist:
* `app/ui.h` — `PauseMenu` (RESUME / SETTINGS / QUIT) and `UiController`, whose
  documented contract is already exactly what a host needs:
  `update()` ("while Playing, navBack pauses"), `shouldFreezeSim()`,
  `showCursor()`, `wantQuit()`, `playing()`, optional console.
* `app/hud.h` — `Hud`, the drop-down console front-end over `x3::con::IConsole`.

**Audited 2026-08-14: 28 of 31 world hosts wire NONE of it.** Only
`host_echotropolis` (22 references) and `host_space` (3) do. Every other host
hand-rolls `GLFW_KEY_ESCAPE` -> quit, and has no console at all.

The right fix is NOT to copy the wiring into 28 files. It is a small shared host
shell (console + `UiController` + char callback + the freeze/cursor/quit
plumbing) that any host drops in with a couple of lines, then applied
host-by-host. `host_echotropolis` is the reference for the console half.

### 5. Distance eliminates the mountain
Confirmed by Tim driving: the massif reads as a thin dark sliver, then vanishes,
as you move away. Up close it renders correctly as craggy rock.

Lead: `terrainHeightAt` (`terrain.cpp:1085`) early-returns **before**
`h += mountainHeight(...)` when `cfg.worldFeatures` is false, and `terrain.cpp:1181`
notes "Tests that build their own TerrainConfig keep worldFeatures=false (legacy
field)". Any far-LOD / horizon-ring / vista path that constructs its own
`TerrainConfig` instead of calling `worldTerrainConfig()` therefore renders a
FLAT world — which is exactly what a mountain vanishing with distance looks like.
Check the horizon ring (`terrain.cpp:1225`) and `screenshot_hosts.cpp:2522`.

### 6. Put a sun in the sky, and get lighting working nicely
No sun disc in `--world tunnel`. Note the bore lights are already re-submitted
every frame here (that fix landed — the tunnel is brightly lit now, so
`TUNNEL_NEXT.md` §6 is stale and should be marked resolved).

### 7. Engine sounds and shift points
Ogg Vorbis decode landed on `feat/ogg-vorbis-decode` (folded 08-12), so the audio
path exists. Wants engine note, RPM, and audible shift points.

### 8. Make the car FEEL faster
Perceptual, not top-speed: FOV push with speed, camera shake/lag, motion blur,
roadside furniture whipping past (see `TUNNEL_NEXT.md` §2 — the NFS-1994 research
already concluded the frame reads empty because there is no roadside furniture at
all: no guardrail, marker post, reflector or signage).

### 9. Gravity is wrong coming out of a ditch
Tim: *"Gravity doesn't work so well on the car when it comes out of a ditch."*
The car goes light / floats on the exit lip. Suspension or downforce on rebound.
Do NOT regress the general feel — Tim's verdict on the physics was
*"Physics FEELS kind of FUN though!!!! I feel gravity!"* (`TUNNEL_NEXT.md` §5).

---

## Round 2 — 2026-08-14 07:44, after the spawn-yaw fix

Tim's verdict on the whole thing first, because it sets the priority:
*"Overall this LOOKS like a GOOD GAME!!!!!! We just need to FEEL IT... it has a
FUN factor now."* and *"Minor bugs, lighting, our engine work is paying off!"*
So: **feel > features** from here.

### 10. WRONG BRANCH — Predator's later work was missing
Tim: *"the asphalt, the concrete apron work, etc are not in this, from predator's
work yesterday."* Correct. `inspx/mountain-tunnels` (last commit 08-13 16:40) is
**6 commits behind** `inspx/la-exe`, which is a strict superset and contains:
* `fix(vehicle): the car uses its powerband, and tyres can hold the torque`
* `fix(tunnel): the ESC pause draws itself`
* `build: Level Architect is its own EXE, over a shared host DLL`
* the Level Architect 11.0 editor core + portal authoring

Lesson: check for descendant lanes before building one. `la-exe` is the live lane.

### 11. Car "feels HEAVY", "rolls back and forth like it has no transmission"
**ALREADY FIXED on `la-exe`** by the powerband commit (5e96839a). Root cause in
Predator's own words: Jolt's transmission defaults are ABSOLUTE
(`mShiftUpRPM` 4000 / `mShiftDownRPM` 2000) and know nothing about the authored
engine. Against a 6500 rpm redline that upshifts ~2500 rpm early into the dead
part of the torque curve — *"the shop sold a power upgrade that made the car
slower."* Fix makes shift points a fraction of redline so an upshift lands on the
0.55 torque peak.

### 12. A black car renders UNDER the car — RENDER ONLY
Tim wondered if it was *"dragging it down"*. It cannot: `buildPhysics()` creates
exactly one chassis box (1300 kg) + 4 wheels; the extra geometry never becomes a
body. It is a draw bug.

Mechanism (`app/vehicle.cpp` ~222): every GLB node whose name does NOT match
`Wheel_FL/FR/RL/RR` is pushed into `m_bodyDraw` and drawn. Any extra node in
`CTR.glb` — shadow proxy, collision hull, LOD1 — therefore renders as part of the
car, and an untextured proxy renders black (X3_WORLD_RULES material law 5).
Fix: enumerate the GLB's node names and skip non-visual nodes.

### 13. No engine sound
Still nothing audible. Ogg Vorbis decode exists (`feat/ogg-vorbis-decode`, folded
08-12) and `engine/audio/` has `MiniaudioSystem` + `RtAcoustics`. Wants engine
note tied to RPM plus audible shift points (which now exist, per §11).

### 14. No brake lights, no reverse lights
Headlights DO work (confirmed in a night/tunnel-approach capture). Brake and
reverse lamps are unwired — emissive material swap driven by
`in.brake` / reverse gear.

### 15. Tunnel needs CLIPPING
Tim: *"The Tunnel... should also have clipping."* The chase camera passes
straight through the tunnel shell and terrain — a capture from inside the shell
looking out shows the bore rendered from the wrong side. Wants camera collision.

### 16. Console command to clamp the camera to the ground
Tim: *"a console command that limits the camera to the ground, like most games."*
Pairs with §15. Needs the console first (§4).

### 17. Looking under the ground makes the asphalt disappear
Backface culling on the road ribbon — expected given single-sided geometry, and
largely moot once §15/§16 stop the camera going under. Log it, do not chase it.

### 18. Want an FPS readout
`app/hud.h` already has an FPS / frame-time meter. `host_tunnel` does not wire
`Hud`. **This is another symptom of §4** — wire the shared host shell and FPS,
the console and the pause menu all arrive together.

---

## Also observed, not yet called out by Tim

* The rock above the portal renders **tan/beige** while the same massif is dark
  grey craggy slate from another angle — splat inconsistency across the mouth.
* Wheels appeared detached from the body in two airborne frames. Possibly just
  mid-jump suspension extension; worth a look if it repeats on the ground.
