# W-TUNNEL (Lane 1, task #30) — HANDOFF

*Written 2026-08-16 by the outgoing W-TUNNEL agent at coordinator cap-warning.
State: SURVEY COMPLETE, ZERO CODE CHANGES YET. Worktree is clean at
`a1400d67` (= origin/integration/complete tip at lane start). Successor: read
this, then the lane spec in `docs/plans/SEVEN_LANE_PLAN.md` (LANE 1), then go.*

## Standing law already read (re-read if you are fresh)
- `docs/plans/SEVEN_LANE_PLAN.md` — lane spec + standing law (NEVER push, never
  --smoketest, check tasklist for X3Engine.exe before launches, eyes-on
  full-res captures, commit local with receipts).
- `CLAUDE.md` (axes/units), `docs/NO_SLOP.md` (esp. rules 4 paired values,
  6 defaults-on, 11 CONTACT LAW), `docs/design/X3_WORLD_RULES.md`,
  `docs/ENGINE_GOTCHAS.md` (esp. 1.1 stale-exe mtime check, 1.4 fresh worktree
  needs `cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake`,
  5.3 worktree agents build their OWN build dir and must `python tools/asset_store.py fetch --all` first).

## GOTCHA hit already: the sandbox refuses git commands containing the literal
string `origin/integration/complete` (misparses as a path). Workaround that
works: resolve the hash via
`git for-each-ref --format='%(refname:short) %(objectname)' 'refs/remotes/origin/integration/*'`
then `git reset --hard <hash>` / `git merge <hash>`. Tip at lane start:
`a1400d6731318267eafd85f1c5a13102dd6983cc`.

## The four sub-tasks (owner spec verbatim in SEVEN_LANE_PLAN.md lane 1)

### 1. WIDEN the bore cross-section (app/tunnel_corridor.{h,cpp})
Current section (tunnel_corridor.h:116-133) is ALREADY 4×12 ft lanes
(kTcRoadHalfWidth 7.32 m) + 3 ft walk decks (kTcWalkDeckW 0.914, kerb
kTcWalkKerbH 0.305) → kTcTubeHalfWidth 8.23, wall 3.80, crown 8.50, shell 0.9,
kTcCorridorHalfW 10.1, falloff 14.0. What is MISSING vs owner spec:
  - LOW CENTER DIVIDER (jersey profile — mirror the freeway median wall; see
    road_network.h:70-92 kFwyMedian* for the F-shape precedent and
    road_network.cpp medianPlan/jersey emit for reference geometry).
  - CONCRETE SHOULDER each side between outer lane and sidewalk kerb.
  - RAISED SIDEWALKS both sides = widen/raise the existing walk deck; must be
    WALKABLE (CONTACT LAW — host_tunnel.cpp on-foot clamp is the pattern; the
    corridor carve is the field, law only pushes UP).
Suggested arithmetic (feet, converting once, same style as the header's block
comment): half = divider half (1 ft) + 24 ft lanes + 6-8 ft shoulder +
5-6 ft sidewalk ≈ 36-39 ft ≈ 11.0-11.9 m interior half → kTcTubeHalfWidth
up from 8.23; keep arch rise proportion 0.571 of half-span (header explains
why absolute crown must NOT be held). Ripple: kTcCorridorHalfW must clear the
new outer shell (old margin ~0.9 m over 9.13 m outer), kTcPortalHalfW
(currently 11.0 — must exceed new outer), backfill lid (tunnelLidHeightAt uses
W0/W1 from the constants — mostly automatic), portal headwalls, and the
M-gates re-derive. Grades hold 4.5% (kTcMaxGrade — do not touch).
  - PAIRED demo-road apron: tunnel_corridor.cpp:1269 `kApronW = 2.6f` carries a
    comment PAIRED with kTcCorridorHalfW — floor slack is
    (kTcCorridorHalfW − kTcRoadHalfWidth); today 2.78 m. When the carve
    widens, grow kApronW toward road_network's 20 ft and UPDATE BOTH comments
    (NO_SLOP rule 4).
Gates to re-green: `--test-tunnelmouth` (M0-M7, incl. M6 regeneration proof
and M7 LOD parity), `--test-terraincorridor` (C7 etc.), `--test-roadnetwork`
(W1/W1b LOD parity), `--test-tunnelfitout`, `--test-tunnelrooms`,
`--test-tunneldrive` (after rewrite, see #4), `--test-routeframe`.
NOTE: fitout lay-bys (tunnel_fitout.h layByExtraHalfW 3.66) add half-width on
top; the shell/carve already accommodate via extraHalfWidthAt — check that
still clears after widening.

### 2. DOORS → STAIRWAYS → HALLS
Machinery EXISTS: app/tunnel_fitout.{h,cpp} places Door fittings every 120 m;
app/tunnel_rooms.{h,cpp} gives a small authored set of doors real program
(EntryStub → Hall → Plant/Signal/Control rooms → Stair (2×14 risers) →
Landing; plus Garage + Ramp kinds), rest are DENIED voids. Tier A = one bore
only. Spec asks "a few working door alcoves with lit stairwells is enough this
pass; full halls can stub behind doors" — so mostly: make sure the doors/
alcoves survive the widened section (wall moved outward ⇒ fitting lat offsets
use kTcTubeHalfWidth, which auto-follows; the ROOM lat envelopes in
tunnel_rooms.cpp addProgramFor may need latIn/latOut bumped), and consider
promoting 1-2 more doors to lit stairwell stubs. Keypad codes: service 1361,
control 3902, garage 5514 (tunnel_rooms.h:223).

### 3. THE LOOP (the genuinely NEW work)
tunnel → ramp → LNSS garage (EXISTS: SpaceKind::Ramp + Garage,
kTrGarageDropM 4.0 below roadway, 15% ramp, LNS shop kit in app/lns_shop.*)
→ NEW: exit from garage UP to the SUMMIT PARKING LOT (mountain is 288.9 m
post-W-MOUNTAIN; sketch `docs/design/ROAD_NETWORK_SKETCH_V2.png` is LAW —
"Parking Lot on Top of Mountain") → NEW: exit from the lot descending into
the OTHER tunnel. Grades ≤14% (switchback cap). Register these as corridors
(registerTunnelCorridorFor / registerTerrainCorridor) so the field carves
them — registry limit kMaxTerrainCorridors, registration MUST happen at boot
before first TerrainStreamer::init (terrain.h contract).
OPEN QUESTIONS I did not get to: which two bores are "the" two tunnels in the
driving world (check X3_FREEWAY_TUNNELS in app_run.cpp:2980 and the
road_network ring bores; the demo bore is at (-592,-352) hdg 157.5°); where
the 288.9 m summit is (grep W-MOUNTAIN's merge, terrain.cpp); whether the lot
is a TerrainCorridor carve or a flat pad + corridors for the access roads.
Coordinate corridor count vs kMaxTerrainCorridors.

### 4. REWRITE the stale --test-tunneldrive gates — RECEIPT CONFIRMED
Grep proves NO code reads X3_TUNNEL_PORTAL_CUT anywhere in app/ or engine/;
the only writer is the test itself (tunnel_corridor.cpp:3474-3480
setPortalCutEnv). Since the cut-and-cover redesign there is no portal-cut
branch, so Phase A ("cut off") builds the IDENTICAL world to Phase B:
  - A2 (`a.residual > 8` — expects the old mouth defect) — cannot pass against
    a construction whose buriedRoadLen is 0 BY DESIGN.
  - A3 (car stopped by earth ramp) — no ramp exists any more.
  - B1 (`holeCount == 2` portal holes) — check terrainPortalHole* in
    terrain.{h,cpp}: if holes are unconditional now, A1 (`holeCount == 0` when
    off) contradicts it; A1/B1 can't both be meaningful without the env branch.
  - B5b road-mount step ≤0.45 m — verify against current fillet geometry.
Rewrite shape: drop the dead A/B env split; keep ONE phase driving the real
rig through the real streamed collision, keep B2 bounded-carve, B3
drive-through, B4 road-level, B5a/B5b mount survey, B6 nothing-in-tube; add
positive assertions that buriedRoadLen == 0 and maxCarve bounded. W-FREEWAY's
report has the original receipt (grep docs/ for W-FREEWAY / freeway report).

## COORDINATION
- Lane 3 (W-PERF) concurrently edits terrain.cpp LOD/refine. OUR terrain.cpp
  footprint must be CARVE-SIDE ONLY. Before finalizing: fetch + merge the
  integration tip (by hash, see gotcha) and re-run all gates.
- Check `tasklist //FI "IMAGENAME eq X3Engine.exe"` before every launch; owner
  plays at night. Retry, don't abort.
- Done bar: build green (exe mtime advanced!), boot zero [ERROR], suites
  green, fps ≥90% baseline, eyes-on full-res captures READ: 4-lane bore with
  divider+sidewalks, door alcove, garage ramp, summit lot, far tunnel entry.
- Commit locally with receipts. NEVER push.

## Build quickstart for this worktree (not yet done here)
1. `python tools/asset_store.py fetch --all` (worktree has LFS pointers only)
2. `cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake`
3. `cmake --build build --config Release` (ALL_BUILD — never --target X3Engine)
4. Verify `build/bin/Release/X3Engine.exe` mtime ADVANCED (gotcha 1.1); MSB3073
   after link is benign (gotcha 1.2) but re-check shaders compiled.
5. Gates: `X3Engine.exe --test-tunnelmouth --test-terraincorridor
   --test-tunnelfitout --test-tunnelrooms --test-routeframe --test-tunneldrive
   --test-roadnetwork` (run individually; bounded timeouts; kill zombies).

## File map (all read this session)
- app/tunnel_corridor.h — constants + API (the widening lives here)
- app/tunnel_corridor.cpp — 3945 lines: deriveRoute ~497, registry ~875,
  build() ~979 (ribbon ~1205, kApronW 1269, fitout draw ~1349, walkway ~1369,
  railing ~1394, subway band ~1449, wall fittings ~1469), lid ~424,
  mouth test ~3050s, drive test 3462-3821, routeframe 3837+
- app/tunnel_fitout.{h,cpp} — lay-bys/lamps/signs/doors placement (pure data)
- app/tunnel_rooms.{h,cpp} — door programs, garage, ramp, stairs, envelope
- app/lns_shop.{h,cpp} — Late Night Speed kit (shared with club1127)
- app/road_network.h — freeway median/jersey constants to mirror
- app/world_hosts/host_tunnel.cpp — the tunnel host (on-foot clamp lives here)
- app/terrain.{h,cpp} — corridor carve, portal holes, buildTileMeshAbs
