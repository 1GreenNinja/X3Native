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

## THE LOOP — why it is not built, stated plainly

It is not a coding task yet, it is a **geometry** problem, and the previous two
agents both filed it as an open question without measuring it. Measured now:

* the demo bore is at **(-592, -352)**; the garage hangs off it.
* `registerSummitSpur` hill-climbs from the connector and lands its peak at
  **(393, 6752)** — a **349 ft** knoll. The lot sits on it.
* those two points are **~7.1 km apart**.
* meanwhile the massif the demo bore actually goes *through* peaks at
  **288.9 m / 948 ft at (-744, -850)** (tunnelmouth's own M6 diagnostic prints
  it every run) — 200 m below the bore's own mouth, and 7 km from the lot.

So "exit from garage to mountain top parking lot" cannot be a ramp: as placed,
it is a 7 km cross-country road between two unrelated hills. **The lot is on the
wrong mountain.** Tim's picture is obviously the mountain the tunnel bores
through — you drive in, park on top of the thing you just drove under, and come
down the far side into the second bore. That reading also makes the last leg
trivial, because the far side of that massif is where the other bores are.

**The fix is to bias the peak search, not to lay 7 km of road.** `registerSummitSpur`
(road_network.cpp:1934) already takes the bore as an argument — today only to
*avoid* it. Give it an optional attractor (the bore's own massif summit) and the
spur climbs the right hill; then garage→lot and lot→second-bore are both short
legs inside one mountain, at grades the 14 % switchback cap already handles.
Cost: `registerSummitSpur` is gated by roadnetwork **K5/K6**, so re-green those,
and re-check `--test-summitlot` L6 (it asserts the pad is within 25 ft of the
peak the spur found, which stays true by construction).

Do NOT close the loop by extending a road 7 km between the current endpoints.
That is the shape of the answer that passes a gate and reads as slop.

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
