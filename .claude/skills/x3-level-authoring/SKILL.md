---
name: x3-level-authoring
description: Use when authoring, editing, or repairing ANY level geometry in X3Native (rooms, halls, doors, floors, transitions) — the Fable-powered level-authoring doctrine: connectivity rules, seam/gap law, height-transition vocabulary, the geometric lint gate, and the visual review gate. Agents building levels MUST follow this or ship disconnected boxes.
---

# X3 Level Authoring — the Doctrine

Born 2026-07-01 from Tim's playthrough verdict: "level design architecture is Abysmal — gaps,
doors/halls don't connect, height changes not dealt with." Cause: agents freehand-placing
boxes. Cure: these laws + the lint + the visual gate. **No level geometry ships that hasn't
passed all three.**

## LAW 1 — Rooms connect through OPENINGS, never by proximity
A connection between two spaces is an OPENING cut in a SHARED wall plane — never "two boxes
placed near each other." Every doorway is: one wall, one hole, one frame seated in that hole.
- The door leaf + frame dimensions MUST match the opening (frame overlaps the cut by its bezel).
- A door NEVER floats in open space, sits mid-room, or spans a gap between two walls.

## LAW 2 — The Seam Law: flush or shared, never gapped, never doubled
Where two rooms/halls meet:
- They SHARE one wall plane (one wall, opening cut in it), OR their walls are FLUSH (surfaces
  touch exactly — same plane coordinate, zero gap).
- **Gap = you can see the void/skybox between rooms → FAIL.**
- **Doubled = two coplanar overlapping faces → z-fighting shimmer → FAIL.** Offset interior
  vs exterior faces by wall thickness, never stack faces at the same coordinate.
- Floors and ceilings obey the same law at room boundaries: continuous walking surface, no
  slivers, no overlaps.

## LAW 3 — The Height-Transition Vocabulary
A walkable height change is NEVER a bare ledge or an abrupt floor jump. The ONLY legal
transitions:
- **Step**: single riser ≤ 0.25 m (player auto-step).
- **Stairs**: risers 0.15–0.20 m, treads ≥ 0.28 m, landing every ≤ 3 m of rise.
- **Ramp**: ≤ 30°, with side edging where it meets walls.
- **Elevator / hatch / ladder**: for large or vertical-shaft changes (the elevator is the
  building's spine; hatches for secrets).
Floors within one story sit at ONE height unless a transition justifies otherwise. Corridor
floor heights match the rooms they serve at every doorway (zero step inside a doorway).

## LAW 4 — Standards make cohesion
Use the canonical dimensions (docs/design/SPIRE_LEVELARCHITECT_DIMS.md + the LevelArchitect
project data): story height, the WIDE main hall, cell dims, door sizes. Pick from the
standard door/hall/ceiling sizes — a building where every opening is a different arbitrary
size reads as chaos even when connected. Trim (baseboards, door frames, ceiling cornices)
hides seams and sells construction — prefer a frame around every opening.

## LAW 5 — Author in data, through the loader
Levels are LevelDoc/canonical JSON built through the data-driven loader — not ad-hoc addBox
calls scattered in C++. The editor (--editor, F8) and the LevelArchitect JSON are the
authoring media. If you must generate geometry in code, generate it INTO the doc format so
the lint can see it.

## GATE A — The Geometric Lint (run it; it must be green)
`X3Engine --test-levellint [--world <w>]` (tools/level_lint if run standalone) validates:
1. **Door-seat check**: every door entity sits inside a wall opening whose dims match.
2. **Seam check**: adjacent room boundaries flush/shared; reports GAPS (visible void) and
   DOUBLED coplanar faces (z-fight risk) with positions.
3. **Height-transition check**: every walkable floor-height change > 0.25 m has a legal
   transition primitive connecting it.
4. **Reachability flood-fill**: from the player spawn, every room marked reachable IS
   reachable by walking (openings + transitions only). Unreachable room = FAIL (unless
   flagged `secret`/`sealed`).
5. **Containment**: no geometry floating disconnected from the structure; no room outside
   the building shell.
If the lint tool doesn't exist yet in your branch, BUILDING IT IS PART OF YOUR TASK — it
lives in app/level_lint.{h,cpp} + the --test-levellint flag, reading the built world's
rooms/doors/brushes.

## GATE B — The Visual Review (your own eyes, mandatory)
After lint-green: screenshot every junction type you touched (door threshold, hall corner,
height transition, room boundary) + a walkthrough set. **Read the images yourself.** Hunt:
sky/void peeking through seams, z-fight striping, floating slabs, doors hanging in air,
texture seams at boundaries. Score honestly /10; iterate ≤3 rounds. An agent that reports
beauty without reading its own screenshots is lying to everyone including itself.

## GATE C — The Playthrough Trace
Headless-walk the golden path (spawn → objective chain → exit) with collision on. It must
complete without noclip. Any spot needing noclip = a connectivity FAIL you must fix.

## Repair procedure (for existing broken levels)
1. Run the lint → get the violation list (gaps/doubles/unseated doors/illegal heights).
2. Fix by CATEGORY (all gaps, then all doors, then all heights) — not room by room.
3. Re-lint after each category; visual gate at the end; playthrough trace last.
4. Commit per category with the violation count delta in the message.
