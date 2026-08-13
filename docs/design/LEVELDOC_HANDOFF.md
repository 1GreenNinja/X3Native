# LevelDoc as the Generator → Editor Handoff Format

*Level Architect 11.0 lane (`inspx/level-architect`). Companion to
LEVEL_ARCHITECT_ROADMAP.md and LEVEL_EDITOR_VISION.md.*

## Why this document exists

Everything substantial in this project is **generated** — the city block/lot
generator, the terrain-corridor tunnels and their portals, the Echo Harbor
landform. Tuning generated output today means editing a constant and
rebuilding. The concrete case that motivated this lane: a tunnel portal landed
wrong, and the fix was **days** of guessing at `kRimSeamClear` values and
re-running. If the generator had emitted a LevelDoc, someone would have dragged
the portal 40 m in the editor and been done.

So the LevelDoc (`app/editor/editor.h`, loaded by `app/leveldoc_world.*`,
booted by `--world fromdoc`, hot-reloaded on mtime / `level_reload`) is not
just the editor's save file. It is the **handoff format** between generators
and human authoring:

```
generator ──emits──▶ LevelDoc JSON ──`--world fromdoc`──▶ playable world
                        ▲    │
                        │    ▼
                   editor (open, hand-correct, save)  ──▶ hot reload
```

## 1. What the format can express (as of 11.0)

| Concept | Fields | Notes |
|---|---|---|
| Blockout solid | `brushes[]`: `type` (Box/Ramp/Cylinder/Stairs), `pos`, `size`, `yaw`+`pitch`+`roll`, `material`, `tint`, `collide` | full 3-axis rotation; collision follows rotation |
| Placed instance | `entities[]` type `"model"`: `model` (GLB relpath), `pos`, `yaw`+`pitch`+`roll`, `scale`, `emissive` | what a lot/frontage generator places |
| **Portal** | `entities[]` type `"portal"`: `pos`, `yaw`+`pitch`+`roll`, `size` (`[0]`=W, `[1]`=H, `[2]`=slab thickness), `script` = **link id** | see contract below |
| Light | type `"light"`: `pos`, `tint`, `scale` (intensity) | |
| Trigger | type `"trigger"`: `pos`, `size`, `script` | fires `trigger_enter <script>` |
| Player start | `playerStart` | |
| **Provenance** | any brush/entity: `gen` (stable key), `genEdited` (bool) | see §3 |

**Rotation convention** (one convention for gizmo, pick, scene matrices, Jolt
quats, loader): local→world is `R = Ry(yaw) · Rx(pitch) · Rz(roll)`, radians,
`x3::editor::rotYPR()` / `yprToQuat()`. `yaw` about +Y is Tim's original 10.9
axis and is unchanged; `pitch` (+X) and `roll` (+Z) are the 11.0 addition.

**Precision guarantee:** floats serialize with `%.9g`, which round-trips any
IEEE float **bit-exactly**. (The previous `%.4g` moved far-from-origin geometry
by centimetres per save cycle — a generator emitting `x = 123.4567` would drift.)
`pitch`/`roll`/`gen`/`genEdited` are emitted **only when set**, so existing
hand-authored files stay byte-identical.

### The portal contract

A portal is a **plane**, not a behavior. `pos` is the plane center; the plane
is the local X/Y extent (`size[0]` × `size[1]`); the **facing normal is local
+Z** rotated by the orientation. `script` carries the **link id**
(`"tunnel_a_west"`): consumers pair portals / bind traversal by this string and
by nothing else. The loader spawns a non-colliding oriented marker slab so
placement is visible in-game; the editor draws a cyan wireframe frame + normal
arrow. Traversal, PVS, rendering-through semantics belong to the consumer
(the tunnel system) — the doc only says *where, how big, which way, named what*.

A generator that today derives portal placement from constants satisfies the
handoff by emitting one `"portal"` entity per opening, gen-stamped (§3). A
human then drags/orients it in the editor and the correction **persists**.

## 2. Acceptance conditions (all enforced by tests today)

* `--test-editor` E19–E22: 3-axis rotate op with 5° angle snap
  (negative-controlled), OBB pick honors roll (hit-flat / miss-rolled),
  bit-exact rotation + portal JSON round-trip, provenance semantics.
* `--test-loader` L7: a rotated brush survives file save→load **bit-exactly**
  (float `==`, no epsilon) and its built scene transform carries the full
  3-axis rotation (negative control: the yaw-only matrix must NOT match).
* `--test-loader` L8: a portal spawns as an oriented marker slab with **no**
  collision body; link id + provenance survive the round trip.
* `--test-loader` L9 (the **generated-doc gate**): a doc authored in a loop the
  way a generator would emit it — every object gen-stamped and 3-axis-rotated —
  survives save→load with nothing dropped (field-for-field, floats bit-exact;
  the comparator is negative-controlled by a perturbed float), builds through
  the real loader, and tears down leak-free.
* Fallbacks (house rule): `X3_EDITOR_ROT3=0` restores yaw-only behaviour
  exactly; `X3_EDITOR_PORTAL=0` makes portals plain markers again;
  `X3_LEVELDOC_NUM4=1` restores the old float emitter. Each verified to
  actually flip behaviour (the rot3/precision tests fail under the fallback,
  legacy tests still pass).

## 3. Regeneration vs hand edits — the decision

**Chosen: (c) per-entity provenance.** Every generated object carries a
`gen` key — a stable identity minted by its generator
(`"tunnel:portal:a_west"`, `"city:lot:12/crate:3"`) — and a `genEdited` flag
the editor stamps the moment a human touches the object (transform commit,
nudge, Details edit; undoing the edit un-stamps it, which the tests assert).

**The regeneration rule:** a generator re-run may replace **only** objects
whose `gen` key it owns **and** whose `genEdited == false`. Hand-edited objects
are kept as-is, and the generator's fresh replacement for that key is skipped.
Objects with an empty `gen` are hand-authored and never touched by any re-run.

Why not the alternatives:

* **(a) Immutable generated layer + edit overlay** — rejected. It doubles the
  document model (every consumer must compose two docs), and the editor's
  selection/undo/outliner would need layer awareness everywhere. The cost lands
  on every system for a benefit only regeneration needs.
* **(b) One-shot generation, doc becomes source of truth** — rejected on the
  evidence of this very week: the tunnel and terrain generators are re-run
  *constantly* while being tuned. One-shot would mean every re-run discards all
  hand corrections, which recreates the exact guess-and-rebuild loop this lane
  exists to kill.
* **(c)** costs two optional fields and one stamping rule, both already
  implemented and tested, and makes the merge deterministic: same doc + same
  generator output ⇒ same result, no diffing heuristics.

**Not implemented yet (follow-on):** the merge routine itself
(`regenerate(doc, generatorOutput)` applying the rule above) and a
"revert to generated" editor action that clears `genEdited` on a selection.
The format and the editor-side stamping are done; the rule is written here so
the follow-on lane implements it instead of re-deciding it.

## 4. The capstone: AI instructions in the editor

Tim: *"we NEED AI instructions IN the editor… 'Generate a winding curving
tunnel from Tunnel Entrance A to Tunnel Exit B'."*

The full loop this enables:

```
natural-language instruction
  → resolve NAMED markers in the doc ("Tunnel Entrance A" = a portal entity by name)
  → derive a generator SPEC (parameters, not geometry)
  → show the resolved spec to the user; they confirm
  → invoke the DETERMINISTIC generator; it emits gen-stamped LevelDoc entities
  → apply as ONE undo transaction
  → the human drags the 5% the heuristic got wrong
```

The pieces already exist — this connects them, it does not rebuild them:

* **`app/editor/editor_ai.*`** already implements the load-bearing pattern:
  the model emits a *plan*, a validator enforces a hard safety envelope, and
  only then does it apply through the ordinary command API as **one undo
  transaction** (`--test-editor-ai`, deterministic, no model needed). The
  instruction feature is a new op kind in that same pipeline — e.g.
  `InvokeGenerator { generator: "tunnel", from: <marker>, to: <marker>,
  params… }` — not a parallel system.
* **`engine/llm/ILlmSystem.h`** (async submit/poll, `--test-llm`) serves the
  editor panel the same way it serves NPC minds.
* **`app/tunnel_corridor.h`** already exposes a parameterised entry point
  (`TunnelSpec { name, cx, cz, dirX, dirZ, halfLen }` +
  `registerTunnelCorridorFor`). That is the *shape* an instruction targets.
  Note for the tunnel lane (their file, not ours): a straight-run spec cannot
  express "winding curving" — expect the spec to grow a **polyline/spline of
  control points**; "winding" is then a parameter (curvature/segment count),
  not a special case.

Design laws (each blocks a known trap):

1. **Names are the interface.** The instruction resolves named entities in the
   doc; the user places two markers and the AI connects them. Never require
   typed coordinates.
2. **The output must be editable.** The instruction produces LevelDoc entities
   the human can drag. An AI action that bakes geometry directly recreates the
   exact problem this lane exists to solve.
3. **Every AI action is undoable and inspectable.** The panel shows the
   resolved spec (which markers, which parameters) before anything applies;
   apply is one Ctrl+Z (the `beginGroup`/`endGroup` transaction already built
   for the AI Architect).
4. **Determinism (load-bearing).** The LLM's only job is producing the SPEC.
   The generator — deterministic — produces the geometry. Same instruction +
   same doc ⇒ same spec ⇒ same entities, so captures and tests keep meaning
   something. Model output never becomes geometry directly.

Acceptance conditions for the follow-on lane that implements it:

* A mock-backend test (no model, like `--test-editor-ai` today) drives:
  instruction text → resolved spec → generated entities, and asserts the
  entities are gen-stamped, undoable in one step, and byte-stable across two
  identical runs (determinism).
* Marker resolution fails LOUDLY (a named marker missing from the doc rejects
  the plan with the name in the error; nothing half-applies).
* The generated result round-trips per §2's L9 gate (nothing dropped).
* Regeneration honors §3 (re-running the instruction replaces only
  `genEdited == false` output).

## 5. What a generator must do to participate (the contract, short form)

1. Emit a LevelDoc (`toJson()` schema) — brushes/models/lights/triggers/
   portals with full transforms. Do not invent fields; unknown keys are
   skipped by the parser but carry no meaning.
2. Stamp every object with a **stable** `gen` key (stable = same key for the
   same logical object across re-runs; include the generator's namespace).
3. Leave `genEdited` false; never overwrite an object whose `genEdited` is true.
4. Portals: one `"portal"` entity per opening, facing = local +Z, `script` =
   link id. Consumers must read placement from the doc, not from constants.
5. Determinism: same inputs ⇒ byte-identical doc (the `%.9g` emitter makes
   this achievable; L9 shows the shape of the proof).
