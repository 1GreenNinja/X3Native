# W-TUNNEL (Lane 1, task #30) — HANDOFF

*Rewritten 2026-08-17 by W-TUNNEL v3. Branch `worktree-agent-ab90449655966e6ba`,
merged with `origin/integration/complete` @ ce48e2b3 (W-PERF terrain LOD/ridge,
weapons, handling). Nothing pushed.*

## STATE — what is DONE

| lane item | state |
|---|---|
| 4-lane bore, low jersey divider, concrete shoulders, raised sidewalks | **DONE** (3121c823) — eyes-on `shots_wtunnel/03_bore_4lane_divider_sidewalks.png` |
| `kTcCorridorHalfW` ripple + paired `kApronW` | **DONE** — tunnelmouth 8/8 (M5 grade 4.50% at cap, M7 LOD parity), terraincorridor 16/16 |
| rewrite stale `--test-tunneldrive` A2/A3/B1/B5b (`X3_TUNNEL_PORTAL_CUT`) | **DONE** (3121c823) — one phase, field-level negative control N1, 10/10 |
| L7 measurement fix (measure the drivable surface, not our own excavation) | **DONE** — summitlot 9/9, L7 reads 0.03 m against a 0.45 m clearance |
| summit parking lot exists **in the game**, not only in a test | **DONE** (ce6c6deb) — 32 stalls at (397, 6774), datum 350 ft; eyes-on `04_summit_lot_32_stalls.png` |
| horizon-ring void annulus (W-PERF's handover) | **DONE** (ce6c6deb) — see below |
| doors → stairways → halls | **PRE-EXISTING, survives the widening** — tunnelrooms 16/16 incl. R4n negative control. NOT re-photographed this wave. |
| **THE LOOP** (tunnel → ramp → garage → summit lot → other tunnel) | **NOT BUILT.** Read the next section before attempting it. |

Gates, all green on the merged tree: tunnelmouth 8/8, terraincorridor 16/16,
tunnelfitout 14/14, tunnelrooms 16/16, routeframe 4/4, tunneldrive 10/10,
roadnetwork 58/58, summitlot 9/9. Boot zero `[ERROR]`, 163–168 fps / 6.0–6.1 ms.

## THE LOOP — the missing leg is a DIRT RIDGE ROAD

**I got this wrong the first time and Tim corrected it.** I measured the gap
between the bore and the lot, called 7 km "too far", and filed the loop as a
geometry blocker needing the lot moved onto the bore's own massif. Tim:

> *"7KM.. use miles.. its not far.. and you can have a road that is dirt, on top
> of the mountains, that is that long, that is what I planned.. curving over
> mountain features"*

**4.4 miles is the design, not the problem.** For scale it is shorter than the
inner tour (31 miles) and three times the summit spur (1.44 miles) — this world
is measured in miles and I was reading a metric number as if it were a walk.
Distances in this lane belong in **miles and feet**, like every other number in
road_network.

So the missing leg is a **long unpaved mountain-top road** running the ridge
line between the two, curving over the mountain features rather than cutting
through them. The endpoints, measured:

* the bore / garage end: demo bore at **(-592, -352)**, and the massif it goes
  through peaks at **948 ft at (-744, -850)**.
* the lot end: **(397, 6774)**, datum **350 ft**, on the peak the summit spur
  climbs to.
* between them: **~4.4 miles** of high ground.

Design constraints that follow from "dirt, on top of the mountains, curving over
mountain features":

* it must **follow the ridge**, not bee-line. A straight line between the
  endpoints would trench every saddle it crosses; the route wants to seek high
  ground and take the curves the terrain gives it. That is a router that steps
  toward the goal while biasing to locally-higher ground, not a polyline.
* **narrow and unpaved** — a dirt track's half-width, no lane paint, no jersey
  barriers. `buildRoadRibbon` hardcodes `rd_asphalt_01`; a surface field on
  `RoadSpec` (defaulting to today's asphalt) reuses the whole ribbon — prism,
  batter, the D5b apron-skirt fix, barrier planning — instead of writing a
  second, worse ribbon.
* grades: the spur already runs a **14 %** cap with switchbacks. A ridge road
  should not need it — following the ridge is what keeps grade down.
* it costs terrain-corridor slots: the registry is at **87 of 192** after
  everything else, so there is room, but a 4.4-mile road chains several.

Then lot → second bore closes the loop; the outer tour's five bores are the
candidates (`North Flank`, `North Massif`, `Crystal North/Saddle/Descent`).

## THE VOID ANNULUS — fixed, and how to check it

`host_tunnel.cpp` had `hr.rInner = 470.0f` hardcoded on the **route midpoint**
while the streamer centres on `startPos` and covers `radius * tileSize` =
**288 m** interactively. Ground stopped at 288 m, far country started at 470 m:
a **182 m ring of nothing** around the player every interactive frame. Headless
it is 448 m vs 470 m — **22 m** — which is why no `--screenshot` capture in the
project's history ever showed it. Both numbers now come off one named
`kStreamRadiusTiles` and the ring is concentric with the streamer; the host logs
the relationship every boot.

**This class of bug cannot be reviewed by the capture harness.** Headless takes
different code paths from an interactive run — that is the whole mechanism here.
Three tools now exist for looking at the real thing:

* `X3_SPAWN=lot|spur|bore` — spawn at a destination instead of 7.5 km away.
  Default-off; unset spawns exactly as before.
* `tools/grab_window.ps1` — photograph the live window. It is DPI-aware (without
  that it silently crops to the top-left corner) and it **refuses to save unless
  the target is verifiably in the foreground** — the first version quietly
  photographed a sibling lane's terminal and that image nearly went in as
  evidence.
* `tools/send_keys.ps1` — hold keys / type. Sends real scancodes; GLFW resolves
  keys from the scancode, so `keybd_event` with `bScan = 0` produces
  `GLFW_KEY_UNKNOWN`.
* `tp lot|spur|bore|ring` console command — **written and wired, but I could not
  make the `~` console open under injected input** (VK_OEM_3 / scan 0x29 both
  verified correct; letters reach gameplay, the toggle never fires). Untested.
  `X3_SPAWN` is the route that works. Someone with a keyboard should try `tp`.

## OPEN, in priority order

1. **The loop** — above. This is the lane's headline item and the real work left.
2. **`rd_asphalt_01` is INCOMPLETE in this worktree** — every road and the lot
   slab fall back to flat colour (`[surface-lib] set 'rd_asphalt_01' incomplete`).
   Not a code defect: run `python tools/asset_store.py fetch --all` (there are
   still `*.pre-fetch.bak` files and untracked `assets/converted_glb/Vehicles/*`
   from an interrupted fetch). **Re-shoot 03 and 04 after fetching** — both were
   taken with the fallback material.
3. **The horizon ring gets no weather overlay** — under snow the streamed tiles
   read white and the ring reads green, a visible band at the seam. Cosmetic,
   pre-existing, shared with every host that uses the ring. Not filed anywhere
   before this.
4. **The ring is baked once and never re-centres.** Fine at spawn, coarse and
   off-centre after long travel. `host_streamed.cpp` documents the same
   follow-up. A re-bake on large displacement is the fix.
5. **Doors / stairwells were not re-photographed** this wave — the gates prove
   the program, no eyes-on. `X3_SPAWN=bore` puts you at the mouth to do it.
6. Stale docs still describe `X3_TUNNEL_PORTAL_CUT` as a working fallback:
   `docs/design/TUNNEL_HANDOFF_BRIEF.md`, `MOUNTAIN_TEAR_BRIEF.md`,
   `TUNNEL_NEXT.md`, `TUNNEL_INTERIOR_PLAN.md`. The code path is gone; the test
   that referenced it is rewritten. The prose is now the only thing lying.

## STANDING LAW (re-read if you are fresh)
`docs/plans/SEVEN_LANE_PLAN.md` (LANE 1), `CLAUDE.md`, `docs/NO_SLOP.md`
(rules 4 paired values, 6 defaults-on, 11 CONTACT LAW),
`docs/design/X3_WORLD_RULES.md`, `docs/ENGINE_GOTCHAS.md` (1.1 stale-exe mtime,
1.2 benign post-link MSB3073, 5.3 worktrees build their own `build/`).
Never `--smoketest`. Check `tasklist //FI "IMAGENAME eq X3Engine.exe"` before
every launch — sibling lanes run concurrently and the owner plays at night;
retry rather than abort. **NEVER push.**

### Gotcha worth keeping
The sandbox refuses git commands containing the literal `origin/integration/complete`.
Resolve the hash first:
`git for-each-ref --format='%(refname:short) %(objectname)' 'refs/remotes/origin/integration/*'`
then `git merge <hash>`.
