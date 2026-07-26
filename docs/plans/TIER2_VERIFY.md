# TIER 2 — WorldStreamer Adoption: Verification Procedures

Companion to `docs/plans/TIER2_STREAMING_PLAN.md`. This is WP-5's deliverable
(plan sec 4): exact commands, using `scripts/echo_stream_ab.ps1`, for the
milestone A-D gates in plan sec 5, plus the risk-register checks from sec 7.

Run everything from the repo root (`D:\GameDev\EchoHarbor`) in PowerShell 7.
The script defaults to `build\bin\Release\EchoHarbor.exe`; override with
`-ExePath` if you're testing a different build.

**Before every run:** the script refuses to launch if `EchoHarbor.exe` is
already running (you may be playing) or if the exe is missing/locked — it
fails loudly with a clear message and a non-zero exit code. See
`scripts/echo_stream_ab.ps1` header for the full subcommand reference.

## Baseline status (this WP-5 pass, 2026-07-26)

Baselined against the exe on disk at branch `echotropolis` (pre-WP-0 — no
`EchoRegionSet`/`ECHO_STREAM` in `host_echotropolis.cpp` yet; WP-4's
env_art/npc_skin changes were present but don't affect this host). No running
`EchoHarbor.exe` was found, so all four subcommands were exercised for real:

- `capture -Label pre-wp0-golden -Tod golden` — **6/6 shots OK**, saved to
  `captures/tier2/pre-wp0-golden/` (kept in the tree as the M-A "before" set —
  see Milestone A below).
- `compare` of two independent capture runs at the same TOD/cams — **PASS,
  byte-identical on all 6 cams** (SHA-256 equal). This is the real-world
  confirmation that `--legacypost` + fixed `ECHO_TOD` + `--shot-cam` are
  deterministic on this build (risk #3), which is what Milestone A's gate
  depends on.
- `flyacross -DurationSec 6 -Force` (smoke test only, no manual flying) —
  ran clean: 0 `[worldstream]` lines and 0 proxy engages (both **expected**
  pre-WP-0), FPS samples captured fine (5 samples, ~10-30 FPS during the
  boot ramp in a 6 s window — too short to be a real number, just a mechanism
  check).
- `boottime -Runs 1 -BootAutoExitSec 6` (smoke test) — first FPS line at
  **~19.0 s** wall clock. This is a real number worth carrying forward as the
  pre-D reference (echotropolis boots the whole island + all districts +
  woodlands today; Milestone D's spawn-region-only boot should cut this
  substantially — re-run `boottime -Runs 3` for a real median once D lands).

All four subcommands are confirmed working against the real exe. Delete or
keep `captures/tier2/pre-wp0-golden/` and `captures/tier2/logs/*` as you see
fit — they're real artifacts of this baselining pass, not fixtures the script
depends on.

## The 6 canonical cams

Defined once in `Get-CanonicalCams` in the script; every coordinate traces to
code, not invention:

| cam | x,y,z,yaw,pitch | derivation |
|---|---|---|
| `postcard` | -450,620,900,-1.02,-0.28 | `go` bookmark `postcard` (`host_echotropolis.cpp` ~3311). High/wide — **should trigger the plan's vista rule** (camY - hf.heightAt > 250 m), so this shot is expected to stay byte-identical across every milestone (plan sec 6 Decision 1). |
| `crown_street` | -30,205,740,1.40,-0.15 | `go` bookmark `drag` (~3313) — the crown's main street (comment at ~3111: "the crown city + drag + districts"). |
| `mine` | -480,260,850,0.00,-0.35 | `go` bookmark `mine` (~3315); matches the `west_shoulder` region anchor in plan sec 1. |
| `district_urban_gate` | 475,40,350,0.00,-0.12 | `assets/districts/districts.txt` URBAN DISTRICT pad (700,350); keep-out rect x[515,885] (`host_echotropolis.cpp` ~1168); cam 40 m west of the keep-out edge, looking east. |
| `district_recife_gate` | 870,40,1250,0.00,-0.12 | districts.txt RECIFE 2050 pad (950,1250); keep-out x[910,1135] (~1167); same convention. |
| `district_hivemind_gate` | 1145,40,1000,0.00,-0.12 | districts.txt HIVEMIND CYBERCITY pad (1340,1000, yOff -23); keep-out x[1185,1495] (~1169); same convention. |

The district-gate Y (40 = plan sec 1's own "~35" flats-bowl approximation +
~5 m eye height) is deliberately approximate — good enough for a *repeatable*
A/B camera, not a final art-review frame. Every capture also records these
exact values (plus TOD/legacypost/settle) in `manifest.json` next to the
PNGs, so a capture set is self-describing months later.

## Milestone A — containers, zero behavior change

Plan sec 5: "WP-1..4 merged; integrator moves content into regions but calls
`forceAllResident()` unconditionally. Verify: byte-compare capture set (fixed
`ECHO_TOD`, `--legacypost`) — byte-identical or A fails; FPS within noise."

```powershell
# 1. BEFORE WP-0 merges (do this NOW, once, and keep the folder):
.\scripts\echo_stream_ab.ps1 capture -Label m-a-before -Tod golden
# (the pre-baselined `pre-wp0-golden/` set from this pass already covers
#  this — reuse it instead of re-capturing if the exe hasn't changed)

# 2. AFTER WP-0 merges Milestone A (forceAllResident() path):
.\scripts\echo_stream_ab.ps1 capture -Label m-a-after -Tod golden

# 3. Byte-compare:
.\scripts\echo_stream_ab.ps1 compare m-a-before m-a-after
```

**Expected outcome:** `RESULT: PASS -- all 6 canonical captures byte-identical.`
A single `FAIL` row is a real milestone-A regression — containers must not
change a single pixel while `forceAllResident()` is unconditional. Also watch
the per-shot FPS-adjacent console lines (there's no FPS line in `--screenshot`
mode, but a `boottime` run before/after is a reasonable proxy — see below) for
"within noise" per the plan.

If A fails, first rule out risk #3 (TAA/AE/sim-clock nondeterminism) by
confirming both sets used `-LegacyPost 1` (the default) and the same `-Tod`
— the script always records both in `manifest.json` for exactly this
triage.

## Milestone B — residency-gated draws

Plan sec 5: "`streamer.update` computes wants; draws gate on residency;
builds still all at boot... street-level FPS (expect the big win), captures
at the 6 cams reviewed (vista cam must be UNCHANGED via the vista rule),
fly-across shows no pop inside load radii."

```powershell
.\scripts\echo_stream_ab.ps1 capture -Label m-b-golden -Tod golden
.\scripts\echo_stream_ab.ps1 compare m-a-after m-b-golden
```

**Expected outcome — read the table row by row, not as a single PASS/FAIL:**
- `postcard`: **must be PASS** (byte-identical to M-A). This is the vista-rule
  regression check (Decision 1) — if `postcard` FAILs here, the vista
  override isn't engaging correctly and every wide/orbit shot in the game is
  now wrong.
- The other 5 rows (`crown_street`, `mine`, 3 district gates): **FAIL is
  expected and correct** — distant regions now stop drawing when not
  resident, so the frame content legitimately changes. Open each PNG and
  review by eye: near content must still be fully present and correct; only
  distant/out-of-radius content should be thinner or absent. A missing
  *nearby* building or a popped-in seam at the cam's own position is a real
  bug, not an expected diff.

Street-level FPS: run `flyacross` and manually loiter at each cam's position
for a few seconds while watching the console's `--world echotropolis: FPS`
line (also summarized by the script's post-run analysis, "FPS samples: ...
avg=..."). Compare the average against a same-duration M-A run — expect a
clear increase away from the crown (distant districts/woodlands no longer
submitting draws).

```powershell
.\scripts\echo_stream_ab.ps1 flyacross -DurationSec 180
```

Fly per the printed steps (spawn crown → `go harbor` → `go drag` → each
district gate at console `speed 3`). After it exits, check:
- `FPS samples: ... avg=...` — should read noticeably higher than an M-A
  equivalent flight, especially once away from the crown.
- No pop/hitch crossing region boundaries (visual judgment during the flight
  itself — the log can't catch this, only your eyes can).

### districtLights risk (#4) — night capture per district gate

Plan risk #4: "districtLights leak across evictions (one global vector
today)... M-B review includes a night capture per district gate."

```powershell
.\scripts\echo_stream_ab.ps1 capture -Label m-b-night -Tod night
```

This is a **review-grade check, not a byte-compare** — open
`district_urban_gate.png`, `district_recife_gate.png`,
`district_hivemind_gate.png` from `m-b-night` and confirm each district's
neon/lamp lights are present ONLY for that district (no lights bleeding in
from a district that should be non-resident at that camera position, and no
lights missing from the district that IS resident/near). If a district's
lights are visible from a gate camera that shouldn't have it resident, that's
risk #4 materializing — `appendNearLights` (plan sec 3) is reading a stale or
un-sliced light list.

## Milestone C — true hook lifecycle

Plan sec 5: "evictions actually run: `onTeardown` deactivates containers...
miners/lamps gate correctly, proxy floor never engages at speed 1-3. Verify:
fly-across log shows `-region unloaded` / rebuild cycles, `streamer` leak
counters stable, capture review, `ECHO_PLAYAS_DEMO` still passes."

```powershell
.\scripts\echo_stream_ab.ps1 flyacross -DurationSec 240
```

Fly a longer loop that deliberately crosses region boundaries back and forth
(e.g. crown → mine → crown → district_urban → crown) so both builds AND
evictions fire. After exit, check the printed summary:

- `region builds (+)` and `region evictions (-)` should both be **> 0** and
  roughly balanced (every eviction should have a matching later rebuild if
  you looped back through the same region).
- `proxy engages (risk #3, expect 0)` **must be 0** at `speed 1-3`. A nonzero
  count here is a real finding per plan Decision 3 — grep the full log
  yourself for `PROXY collision floor engaged` to see which region(s) and at
  what point in the flight.
- Scan the full log (`captures/tier2/logs/flyacross_*.log`) by eye for any
  leak-counter lines the integrator may have wired (`meshesDestroyed`,
  `texturesDestroyed`, `bodiesDestroyed` — `WorldStreamer` exposes these as
  getters in `app/world_stream.h` ~207-211; whether/where they get logged is
  up to WP-0). If present, they should stay bounded, not grow unboundedly
  across repeated build/evict cycles of the same region.

Capture review: repeat the M-B capture set and eyeball the 6 PNGs again —
`postcard` still PASS (byte-identical), others reviewed for correctness, same
as M-B.

```powershell
.\scripts\echo_stream_ab.ps1 capture -Label m-c-golden -Tod golden
.\scripts\echo_stream_ab.ps1 compare m-a-after m-c-golden   # postcard row must be PASS
```

**`ECHO_PLAYAS_DEMO` regression** (existing tooling, not owned by this
script): the play-as yaw-arc demo already auto-drives and auto-captures
`captures/playas_*.png` — confirm it still completes cleanly under
streaming:

```powershell
$env:ECHO_PLAYAS_DEMO = '1'
$env:ECHO_AUTOEXIT_SEC = '20'
& .\build\bin\Release\EchoHarbor.exe --world echotropolis
Remove-Item Env:ECHO_PLAYAS_DEMO, Env:ECHO_AUTOEXIT_SEC
```

Per plan Decision 6, `ECHO_PLAYAS_DEMO` forces vista mode for its whole run —
confirm no missing geometry in the resulting `captures/playas_*.png` frames
(vista mode should mean everything is resident regardless of the demo
agent's position).

## Milestone D — spawn-region boot + true destroy

Plan sec 5: "`buildStartRegions` builds `crown` only; neighbors stream in;
`onTeardown` upgraded to `EchoRegion::destroy`. Verify: boot-time delta
logged, spawn integrity (walk immediately, no proxy engage at spawn), VRAM
drop on evict, full WP-5 suite."

```powershell
.\scripts\echo_stream_ab.ps1 boottime -Runs 3
```

**Expected outcome:** average first-FPS-line time should drop noticeably
from the pre-D baseline (this pass measured **~19.0 s** on a single pre-WP-0
smoke run — re-baseline with `-Runs 3` right before D lands for a real
median to diff against, since a 1-run smoke test isn't a fair comparison
point). Report the delta in seconds saved, per the plan's fleet-post
convention (sec 8: "fleet post after ... M-D (the boot number)").

### Spawn integrity (risk #2) — walk at spawn, frame 1

Plan risk #2: "Terrain/collision streamed by mistake → player falls through
world... verification walks at spawn frame 1." This is inherently an
interactive check — no headless capture proves "didn't fall through the
floor" on its own:

1. Launch normally: `.\build\bin\Release\EchoHarbor.exe --world echotropolis`
2. The instant you're in control at spawn (crown), press WASD immediately —
   don't wait. Confirm you're standing on solid ground, not falling.
3. Open the console and confirm `speed`/position report sane Y (not falling
   toward -infinity).
4. Check the log (run under `flyacross` instead of a bare launch to get one
   for free) for `PROXY collision floor engaged` in the first second — there
   should be **none** at spawn; the crown is boot-built (plan Decision 3), so
   the proxy should never engage there.

```powershell
.\scripts\echo_stream_ab.ps1 flyacross -DurationSec 30
# fly nothing -- just stand at spawn and confirm no fall, then check the log:
```

### VRAM drop on evict

Plan sec 5 (D): "VRAM drop on evict (device stats)." This needs a device-stats
readout the WP-5 script doesn't have access to (no CLI/env surfaces device
memory counters today) — check whatever the integrator wires (e.g. a
`--test-worldstream`-style stat dump, or `IRenderDevice` memory stats printed
to the log) and confirm a measurable drop after a region evicts under
`EchoRegion::destroy` (M-D's true GPU teardown, vs M-C's deactivate-only). If
no such log line exists yet, that's a gap to flag back to WP-0/WP-4, not
something this script can currently verify.

### Full WP-5 suite

Re-run everything end to end as the D sign-off:

```powershell
.\scripts\echo_stream_ab.ps1 capture -Label m-d-golden -Tod golden
.\scripts\echo_stream_ab.ps1 compare m-a-after m-d-golden   # postcard row must still be PASS
.\scripts\echo_stream_ab.ps1 capture -Label m-d-night -Tod night   # districtLights re-check
.\scripts\echo_stream_ab.ps1 flyacross -DurationSec 240
.\scripts\echo_stream_ab.ps1 boottime -Runs 3
```

## Rollback check (every milestone)

Plan sec 5: "Rollback at every milestone: `ECHO_STREAM=0` ... the monolithic
behavior, preserved verbatim." Once WP-0 wires this env var:

```powershell
$env:ECHO_STREAM = '0'
.\scripts\echo_stream_ab.ps1 capture -Label rollback-check -Tod golden
Remove-Item Env:ECHO_STREAM
.\scripts\echo_stream_ab.ps1 compare m-a-before rollback-check
```

**Expected outcome:** PASS on all 6 cams, at every milestone A-D — `ECHO_STREAM=0`
must always reproduce the original pre-streaming behavior byte-for-byte,
regardless of how far the integrator has progressed past it.

## Open items for the integrator (WP-0)

- No `ECHO_STREAM` env var exists yet in `host_echotropolis.cpp` as of this
  baseline (expected — WP-0 runs after WP-1..5). The rollback-check section
  above is ready to use the moment it lands.
- No `[worldstream]` log lines are produced by echotropolis yet (same
  reason). `flyacross`'s log analysis already greps for them generically, so
  it will start reporting real numbers automatically once WP-0 wires
  `EchoRegionSet`/`WorldStreamer` into this host — no script change needed.
- `WorldStreamer` exposes leak counters (`meshesDestroyed()` etc.,
  `app/world_stream.h` ~207-211) but nothing currently logs them to stdout.
  Recommend WP-0 add one periodic `[worldstream] stats: ...` line (any
  shape) — `flyacross`'s `[worldstream]`-pattern grep will pick it up for
  free, and it's the only way M-C's "leak counters stable" bullet becomes
  machine-checkable instead of eyeballed.
- No device-level VRAM/memory-stats log line exists for the M-D "VRAM drop on
  evict" check (see above) — flag to WP-0/WP-4 if one isn't added.
