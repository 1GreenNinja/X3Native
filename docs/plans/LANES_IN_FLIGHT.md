# LANES IN FLIGHT — merge instructions
*2026-08-16 23:55. Written so ANY session (or Tim) can land tonight's work without
the session lead's context. Base at writing: `a1400d67` on `integration/complete`.*

## How to merge a lane
Each agent works in its own git worktree and NEVER pushes. To land one:

```bash
cd /d/GameDev/x3-laexe
git fetch "D:/GameDev/X3Native/.claude/worktrees/agent-<ID>" worktree-agent-<ID>:wave/<name>
git log --oneline HEAD..wave/<name>          # read what it did
git merge wave/<name> --no-edit -m "merge: <LANE> — <what + receipts>"
# resolve any host_tunnel.cpp conflicts by UNION (keep both lanes' blocks)
cmake --build build --config Release --parallel
cd build/bin/Release && for t in roadnetwork terraincorridor tunnelmouth riverbridge; do
  ./X3Engine.exe --test-$t 2>&1 | grep -E "passed|FAIL" | tail -1; done
# EYES-ON a capture before pushing (NO_SLOP rule 2), then:
git push origin integration/complete
```

## The lanes and their worktree IDs
| Lane | Agent worktree ID | Delivers |
|---|---|---|
| W-PERF (plan lane 3) | `a5827ca176041d974` | corridor refine distance-scope, ridge blades. **MERGE FIRST** |
| W-TUNNEL (lane 1) | `ab90449655966e6ba` | 4-lane bores, divider, sidewalks, doors, garage→summit-lot loop. Rebases on W-PERF |
| W-WATER (lane 2) | `aa28d458b82af9a3b` | one water truth (deletes the minBenchY shim), rain-fed river, the sub |
| W-TOWN (lane 4) | `a54a72d274eadd34a` | Small Mountain Town, pedestrians via AnimatedCharacter |
| W-STATIONS (lane 5) | `a675dfcc10944f5f1` | gas stations + refuel stub |
| W-FACTORY (lane 6) | `ab7c1eb64137a5514` | factory EXTERIOR + golden tickets (interior = djbooth's `feat/factory-annex`) |
| W-MAP (lane 7) | `a3d190a67b58d1f33` | map v3, compass, POIs. **MERGE LAST**; its MapPoi header may be fast-merged early |
| W-TRAFFIC | `a94832ecec7aeb370` | AI traffic on both carriageways, armory roster |
| W-WEAPONS | `a02b8023e5df330fc` | Jake's rifle through the shared anim module |
| W-HANDLING | `a962cd94f9604eaa6` (agent `ac5b8ba2921710bbf` works it) | downforce, 0.50:1 top gear, no pegged redline. **Has a known defect: a checkpoint commit swept in store-served GLB bytes (gotcha 2.5) — verify `git log --stat \| grep -i glb` is clean before merging** |
| W-CLOUDS | `ac78e9cb58a88573a` | soft clouds + ground cloud shadows (code committed; captures were running) |

Merge order target: **PERF → TUNNEL → WATER → TOWN → STATIONS → FACTORY → MAP**, with
TRAFFIC / WEAPONS / HANDLING / CLOUDS landing whenever they report green.

## Known open defects (filed as tasks)
- #32 river drawn plane vs. water table (W-WATER owns the fix; the `minBenchY`
  shim in `road_trees.*` + host is the temporary bridge and must die with it).
- #33 fps variance from the unscoped corridor refine (W-PERF owns).
- Stale `--test-tunneldrive` gates A2/A3/B1/B5b (W-TUNNEL owns).
- `rd_asphalt_01` missing from the manifest → roads fall back to the checker
  texture in fresh checkouts.

## Standing law
`docs/plans/SEVEN_LANE_PLAN.md` (the wave spec), `docs/NO_SLOP.md` (11 rules),
`docs/design/ROAD_NETWORK_SKETCH_V2.png` (route-spec law). Never `--smoketest`;
check `tasklist //FI "IMAGENAME eq X3Engine.exe"` before any engine launch.
