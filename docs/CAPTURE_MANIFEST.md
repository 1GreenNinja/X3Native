# The capture manifest — what a `--screenshot` did NOT render

Every `--screenshot*` / `--capture*` / `--ui-demo` run now ends with a block
tagged `[capture]`. Read it before you read the PNG.

```
... 2>&1 | Select-String "\[capture\]"
```

## Why this exists

This project verifies art almost entirely through capture frames, and a frame
gets judged as if it were the game. It is not the game. The capture paths run a
**settle loop**, not the live loop, and a settle loop only ticks what somebody
remembered to tick in it.

The proven case: `WorldStreamer` is `update()`d **only in the live loop**
(`app/world_hosts/host_echotropolis.cpp`, the live `regionStreamer.update(...)`).
`EchoRegionSet::drawAll()/updateAll()` gate every region on that streamer's
residency view (`app/world_hosts/echo_regions.cpp`, M-B DRAW GATE). In a
capture the streamer has never run, so all 18 regions report `Unloaded` and
submit **nothing**: harbour fleet, subway, drones, district dressing, mine and
woodland props — all absent.

That is how "vessels never cross land" survived weeks of screenshot audits
*and* a flood-fill, while a ship sat visibly beached on Tim's monitor. **The
stills were photographing an empty bay.** The bug was never the gate. The bug
was that the frame said nothing about it.

## What it prints

Three sections, only the non-empty ones:

* **`ACTIVE`** (INFO) — declared subsystems that actually ran, with a tick
  count and, for gated ones, how many units got through.
* **`PARTIAL`** (WARN) — gated some of the time. Prints `N drawn / M
  SUPPRESSED`, so a half-empty frame cannot read as a full one.
* **The loud block** (ERROR, `!!!`-prefixed) — declared and then **never ran**.
  Each line names the *content* that is missing, not the class that is missing.
  It closes with:

  > ANY VISUAL CONCLUSION ABOUT THE ABOVE, DRAWN FROM THIS FRAME, IS MEANINGLESS.
  > The frame does not show them missing from the GAME. It shows them missing
  > from the CAPTURE. Absence here is NOT evidence of absence in play.

Same shape as the two precedents that already work here: the `[rhi]
VALIDATION: layers=OFF … a '0 VUID' result from this run is MEANINGLESS`
banner, and `[ERROR] [cvar] !!! THIS RUN DID NOT TEST THOSE.`

## How it avoids going stale

There is **no hardcoded list of subsystems anywhere in the implementation.** A
subsystem appears because *it* declared itself, from its own construction site,
**in this binary**. So:

* A build that does not construct a subsystem never declares it, and correctly
  never reports it. When the Level Architect becomes its own EXE over a shared
  host DLL, the engine-EXE capture and the editor-EXE capture each print
  **their own** truth, and neither has to be taught about the other.
* "Declared but never ticked" is detected **by construction** — that is exactly
  the streamer's shape, and exactly what the next live-loop-only subsystem will
  look like the day someone adds it.
* Region gates report **counts** from inside the gate, not a list, so a region
  added tomorrow is counted the day it is added.

The one thing it cannot know is a subsystem that never calls `declare()` at
all — so the footer says so out loud rather than implying coverage:

```
manifest scope: N subsystem(s) declared by THIS binary. A subsystem that never calls
x3::capture::declare() is NOT tracked here — this list is a floor, not a ceiling.
```

## Adding a subsystem to the manifest

Three calls, in `app/capture_manifest.h`. Cost when not capturing is one
relaxed atomic load per call.

```cpp
// at the CONSTRUCTION site — say what is missing from the FRAME, not from the code
x3::capture::declare("echo.crowd.residents",
    "the 40 rigged CITIZENS were not built (ECHO_RESIDENTS unset) — empty "
    "pavements in this frame say nothing about the crowd in play");

// next to the real update, so ACTIVE means "ran", not "was intended"
x3::capture::tick("echo.crowd.residents");

// or, at a gate, count the verdict instead of asserting it
x3::capture::gate("regions.streamed.draw", drew, gated);
```

Declare **unconditionally** for anything that is opt-in. Declaring an opt-in
subsystem at its build site means a default capture that skipped it produces an
empty street *and* a silent log — the exact failure this exists to kill.

## `--world echotropolis`: streamed content is ON by default

`ECHO_SHOT_STREAMED` used to be opt-in (`=1` to see the fleet). It is now
**on by default**; `ECHO_SHOT_STREAMED=0` restores the old gated frame.

The reasoning, and the numbers behind it, are in the comment block at the
`shotStreamed` lambda in `app/world_hosts/host_echotropolis.cpp`. Short form:

1. **There is no determinism cost.** Dropping the gate does not *start* the
   streamer, it *bypasses* it. No job pool (the streamer is `init()`ed with
   `jobs=nullptr`), no budget slicing, no hysteresis, no wall-clock. What draws
   is the container-side residency `forceAllResident()` already built at boot,
   iterated in graph order, posed off the settle loop's fixed `shotT + i*dt`.
2. **Measured, not assumed.** Two identical runs of
   `--world echotropolis --screenshot --shot-cam -545,4,200,-2.601,-0.058`
   *with streamed content on* produce byte-identical PNGs (md5 `e13758c1…`).
   Do **not** generalise that: on the same binary the plain `--screenshot`
   canonlevel rig is **not** byte-reproducible run to run, while
   `--screenshot-showroom` is. Determinism here is per-rig — another reason not
   to spend the default on it.
3. **The default is the one that lies.** Nobody types a flag they have never
   heard of. An opt-in truth switch means every casual capture — which is
   nearly all of them — keeps omitting the fleet in silence.

`ECHO_SHOT_T=<sec>` still offsets the pose clock so a still can be composed
anywhere in the traffic cycle instead of only at t ≈ 0.38 s.

## Proof

`docs/screenshots/capture-manifest/` — same binary, identical framing,
switched only by `ECHO_SHOT_STREAMED`.

| | |
|---|---|
| `BEFORE_fleet.png` | `ECHO_SHOT_STREAMED=0` — empty bay |
| `AFTER_fleet.png`  | default — the galleon is there, hull on the water |
| `BEFORE_harbor.png` / `AFTER_harbor.png` | wide harbour: the AFTER gains vessels, ridge woodland and a district tower |

Both pairs: `--world echotropolis --legacypost ECHO_TOD=noon`,
`--shot-cam -545,4,200,-2.601,-0.058` (fleet) and
`--shot-cam -20,55,880,-1.5708,-0.30` (harbor).

## Known remaining gaps (enumerated, not fixed)

The streamer is one gated subsystem; it is not the only one. Live-loop-only
subsystems found in the settle loops are listed in `docs/ENGINE_GOTCHAS.md`
§4.1c. The manifest reports the ones that declare; the rest are still silent
and are the next thing to instrument.
