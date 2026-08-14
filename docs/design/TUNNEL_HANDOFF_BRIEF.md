# Tunnel lane — brief for the next agent

The previous tunnel agent CANNOT BE RESUMED (its transcript is gone). This is the
replacement brief. Branch `inspx/mountain-tunnels`, currently 569753a3, pushed.

## THE ONE BLOCKER: one flag, two mutually exclusive bugs

`X3_TUNNEL_PORTAL_CUT=1` (default) -> the bore is DRIVABLE end to end (residual
0 m, proven by `--test-tunneldrive`) but the mesher TEARS THE MOUNTAIN OPEN: a
full-height rip through the peak with sky visible through it and long jagged
shards hanging from the torn rim. Tim saw it immediately from the approach.

`X3_TUNNEL_PORTAL_CUT=0` -> the mountain is whole but the bore is PACKED WITH
EARTH again (a smooth ramp filling the arch).

Neither state ships. Both must be true at once.

### Diagnosis (mine, unverified by measurement — verify it)
The terrain FIELD is fine: the run reports worst soil cover 23.94 m, so the rock
is there. It is the MESH being deleted. The portal-hole change drops surface
triangles (and skirts) that dip into the tube envelope at the mouth; that test
appears to be UNBOUNDED VERTICALLY, so it removes the whole column of triangles
above the bore instead of only those actually intersecting the tube.

Bound it on all three axes: a triangle should only be dropped if it is within the
tube cross-section AND within the mouth's arc-length window AND BELOW crown +
shell thickness. Anything above the crown must survive — that IS the mountain.
Then cap the rim: dropping triangles leaves a ragged boundary with no cap, so the
shards will persist even once the extent is right. The portal ring/headwall
should cover it.

### VERIFY BOTH WAYS — this is the whole lesson of this lane
`--test-tunneldrive` passes happily while the summit is missing. A still cannot
prove traversability. You need BOTH:
* the drive test (traversability, negative-controlled — cut off must stall the car)
* `--screenshot-tunnel` and LOOK at 01_approach AND 04_saddle (mountain intact)
Ship neither claim without the other.

## What already landed on this lane (do not redo)
* assetRoot() matched an unrelated D:\Assets (case-insensitive) so EVERY repo
  asset was invisible — fixed; that unlocked all real art.
* Rock splat was `sr_concrete_01` (concrete on every cliff) -> `fw_rock_cliff`.
* A real mountain: 5th RangeDef, 55 m hummock -> ~162 m peak. NOTE the fifth
  range was silently absent for hours because a python .replace() no-opped on a
  line-ending mismatch AND `mountainHeight()` hardcoded `i < 4`.
* Rock-scale relief (~45 m + ~15 m ridged noise) — a range built only from
  landform frequencies is a smooth dune however tall. Do not gate detail on the
  `ridge` mask; that suppresses it exactly where the flanks need it.
* Bluff terracing; wing walls (grass no longer climbs into the arch); concrete
  shoulder + apron; road-end feather; a ~19 deg scree fillet making the road edge
  climbable (worst mount step 0.14 m vs 0.43 m clearance).
* Second rock band + 5 curated textures published to the ASSET STORE
  (assets/surface_library is gitignored — LFS budget is exhausted fleet-wide).
* Snow retuned 180/265 -> 118/185 (the ridge peaks ~162 m, under the old floor).

## Open, smaller
* Mouth cliff faces: stretched triangles + floating shards (may share a cause
  with the tear).
* `terrain_rock_grey`, `terrain_bluff_clay`, `terrain_bluff_dark` published but
  unwired. No trees yet.
* `docs/design/TUNNEL_INTERIOR_PLAN.md` is written, not built.
* The car skin transform: `kWheelAxisFix` is now IDENTITY and Tim confirmed by
  driving that the wheels point correctly — the old `Rz+90` WAS the defect. That
  change is currently sitting in commit a68bc210 under a tunnels message (my
  sloppiness); consider moving it to the vehicle lane.
* ESC in host_tunnel now PAUSES rather than quits, but with NO on-screen
  indication — it reads as a freeze. Needs a real menu.

## Standing rules
Clean-room; fallback cvar/env per feature; negative-controlled tests; commit
messages state honestly what is NOT done. Build fails LNK1104 if the game is
running — kill X3Engine.exe first. `--test-worldstream` is 16/2 from a
PRE-EXISTING texture double-free (76 created / 112 destroyed) — not this lane's.
