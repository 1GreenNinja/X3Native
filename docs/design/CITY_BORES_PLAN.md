# Plan — dress the four city freeway bores

Status: DRAFT, pre-execution. Written before any code, on purpose.

## Why this document exists

The three portal defects in this lane (grass in the arch, floating wing walls,
sprawling shoulder) were each found by building the thing, capturing it, and
looking. That works, but it costs a full build-capture-look cycle per mistake and
it relies on someone noticing. Two of the three were caught by Tim, not by me.

The failure was never the code. It was starting to emit geometry before writing
down what "correct" would look like. Every one of those defects is expressible as
a condition that could have been stated first:

* "no emitted geometry stands more than N m above the ground it retains"
* "the batter terminates at grade; it never lays a quad on undisturbed terrain"
* "no terrain is visible inside the portal arch opening"

So: conditions first, then execution, then iterate until every condition holds.

## Goal

`registerCityFreewayTunnels()` already registers four real corridors and all four
find hills (North 144 m, East 306 m, South 128 m, West 84 m bores). The terrain is
CUT but nothing is DRESSED — no tube, no portals, no road, no lighting. The city
still draws its two-box graybox per tunnel.

Dress all four with `TunnelCorridorWorld`, and delete the graybox boxes.

## Known constraints (verified, not assumed)

1. **Boot order.** Corridors must register before the first terrain height query.
   Already handled in `app_run` before the spawn probe. Dressing is separate and
   happens later, when a device + physics + scene exist.
2. **The city is a STREAMED region.** `world_stream.cpp` builds it lazily under
   `builder == "city"`. Dressing must therefore be region-owned and must tear down
   cleanly when the region unloads.
3. **Teardown is already leaking.** `--test-worldstream` W5b reports textures
   created 76 / destroyed 112 — a double-free, pre-existing. Four more dressed
   tunnels will add textures to that ledger. This plan must not make it worse and
   should state whether it does.
4. **Light budget.** `TunnelCorridorWorld` spends ~6 real point lights per bore.
   Four bores = ~24 of the engine's 64. That is a real cost and needs measuring,
   not assuming.
5. **`TunnelCorridorWorld` currently assumes one instance.** It owns a
   `SurfaceLibrary` and a texture list. Four instances must not each re-upload the
   same 2K sets.

## Acceptance conditions

Execution iterates until ALL of these hold. Each is checkable — by a test, a log
line, or a named capture. No condition is "looks good".

### Correctness
- [ ] C1. All four bores dress without error; log reports 4/4 dressed.
- [ ] C2. Each dressed tunnel's shell length is within 5 % of the bore length its
      route reported at registration (144/306/128/84 m).
- [ ] C3. The road ribbon is continuous from approach through bore to far portal:
      no gap > 0.5 m in arc length between emitted road segments.
- [ ] C4. The tunnels COLLIDE — a capsule dropped at the bore midpoint rests on
      the road, it does not fall through.

### Geometry quality (the three defects, as conditions)
- [ ] G1. No terrain visible inside any portal arch opening.
- [ ] G2. No emitted wing-wall segment stands more than `kWingProud` above the
      ground beneath it. Assert in code, not by eye.
- [ ] G3. The shoulder batter terminates at grade on every rail; zero quads
      emitted after both rails have landed.

### Budget
- [ ] B1. Total point lights across all four bores <= 32 (half the 64 budget).
      Log the number; if it exceeds, reduce per-bore lights before proceeding.
- [ ] B2. Region build time for the city does not regress by more than 50 ms.
      `--test-worldstream` already prints per-region realize time.
- [ ] B3. The four bores share ONE `SurfaceLibrary`; texture uploads for the
      second, third and fourth tunnel are zero.

### No regressions
- [ ] N1. `--test-worldstream` texture ledger does not get WORSE than the current
      76/112. If dressing adds to the double-free, say so explicitly.
- [ ] N2. `--test-city` stays 20/20; `--test-terraincorridor` stays 9/9.
- [ ] N3. `X3_FREEWAY_TUNNELS=0` still restores the pre-corridor field exactly.

### Evidence
- [ ] E1. One capture per tunnel mouth (4), plus one interior. Committed.
- [ ] E2. Any defect found during iteration is committed as a DEFECT_ frame
      alongside the fixed one, as with the wing walls and the shoulder.

## Execution notes

- Do the SHARED-LIBRARY refactor (B3) first. Dressing four tunnels each with its
  own `SurfaceLibrary` would quadruple texture memory and make B1/N1 unreadable.
- Dress ONE tunnel first (West, 84 m — the shortest, fastest to iterate on) and
  drive it to all conditions before touching the other three.
- Delete the graybox boxes only AFTER a tunnel dresses successfully at that
  location, so there is never a frame with neither.

## Open questions for Tim

1. Should the four city bores use the same lining set as the demo
   (`mw_concrete_panels_b`), or should the city read differently — older, more
   weathered — than a fresh highway tunnel?
2. Light budget: 24 of 64 for tunnels is a lot in a city that also wants street
   lighting. Prefer fewer real lights per bore and lean harder on the emissive
   strips?
