# x3.cutscene/1 — data-driven cutscene format (app/cutscene.{h,cpp})

> Owner module: `x3::cut` (app/cutscene.h / app/cutscene.cpp). Game/slice code only —
> engine/ stays pure. The module is PURE DATA + EVAL + a deterministic player: no
> window, no Vulkan, no GLFW — so `--test-cutscene` drives the whole thing headless.
> The windowed/headless CINEMATIC DRIVER (3D ships + planets + HUD overlays) lives in
> app/main.cpp and consumes this module's evaluated poses each frame.

## File shape

A cutscene is one JSON file (`*.cutscene.json`, repo convention `assets/cutscenes/`).
Top level:

```jsonc
{
  "format":   "x3.cutscene/1",      // REQUIRED magic; rejected if missing/unknown
  "name":     "cold_open",          // log label
  "duration": 68.0,                 // seconds, > 0; the timeline clamps/ends here
  "skippable": true,                // default true
  "skipTo":   52.5,                 // OPTIONAL: where a skip lands (default = duration).
                                    // Lets a skip still play the title-card tail.
  "camera":   { ... },              // REQUIRED track
  "actors":   [ ... ],              // 0+ model actors
  "audio":    [ ... ],              // 0+ one-shot audio cues
  "fades":    [ ... ],              // 0+ full-screen fade ramps
  "letterbox": { ... },             // OPTIONAL cinematic bars
  "titles":   [ ... ],              // 0+ title cards
  "events":   [ ... ]               // 0+ x3.fire events
}
```

All times are SECONDS on one master timeline `[0, duration]`.

## camera (required)

```jsonc
"camera": {
  "keys": [                          // >= 2, t ascending
    { "t": 0.0, "pos": [x,y,z], "look": [x,y,z], "fov": 55.0 },
    ...
  ],
  "shakes": [                        // 0+ camera-shake bursts
    { "t": 47.0, "dur": 1.2, "amp": 0.45, "freq": 13.0 }
  ]
}
```

* `pos` and `look` (a LOOK-AT TARGET point, not a direction) are each interpolated
  with a **Catmull-Rom** spline over the keys (endpoints clamped by duplicating the
  first/last key), so multi-key moves are C1-smooth. Two keys degrade to a lerp.
* `"cut": true` on a key starts a NEW SHOT: a hard cut into that key. Keys partition
  into spans at cut keys, and interpolation (including CR tangent neighbors) never
  crosses a span boundary — so multi-setup shot language (cut, new framing) is one
  flag. A one-key span is a static hold.
* `fov` (degrees, 1..170) lerps between keys — authoring FOV ramps is just two keys.
* yaw/pitch are DERIVED from `pos -> look` in the device convention
  (`forward = (cos p cos y, sin p, cos p sin y)`): `yaw = atan2(fz, fx)`,
  `pitch = asin(fy)`.
* `shakes`: inside `[t, t+dur]` a deterministic 3-axis sine mix offsets the camera
  position by up to `amp` meters at `freq` Hz, decaying linearly to zero over the
  burst. Sum of all active bursts. Fully deterministic (no RNG).

## actors

```jsonc
{
  "id":    "jake_ship",               // unique, non-empty
  "model": "rigged_glb/JakeFighterShip.glb",  // GLB path relative to assetRoot(),
                                      // OR a builtin: "builtin:beam" (unit box),
                                      // "builtin:glow" (unit sphere)
  "size":  9.0,                       // OPTIONAL: uniformly normalize the model so its
                                      // longest AABB axis == this many meters (0/absent = raw)
  "rotOffsetDeg": [0, 0, 0],          // OPTIONAL static yaw/pitch/roll correction (deg)
                                      // applied BEFORE the keyed rotation (fixes GLB facing)
  "color":    [1, 1, 1, 1],           // OPTIONAL baseColorFactor (builtins; tints GLBs)
  "emissive": [0, 0, 0, 0],           // OPTIONAL {r,g,b,strength} emissive override; strength>1
                                      // drives bloom (beams/glows). [0,0,0,0] = model's own.
  "showAt": 0.0, "hideAt": 53.0,      // visibility window (defaults 0 / duration)
  "keys": [                           // >= 1, t ascending
    { "t": 0, "pos": [x,y,z], "rotDeg": [yaw,pitch,roll], "scale": 1.0 },
    ...
  ]
}
```

* `pos` Catmull-Rom over keys (smooth flight paths / the spiral-down); `rotDeg` and
  `scale` lerp. One key = a static pose.
* Model matrix composed `T(pos) * R(yaw)*R(pitch)*R(roll) * R(rotOffset) * S(scale*norm)`
  (column-major, glTF convention), then per-drawable `* nodeTransform`.
* Builtins exist so FX (energy bolts, glows, engine flares) stay data-driven without
  art: `builtin:beam` is a unit box (scale it long+thin via per-axis "stretch", below),
  `builtin:glow` a unit UV-sphere.
* OPTIONAL `"stretch": [sx, sy, sz]` — per-axis scale multiplier (beams).

## audio

```jsonc
{ "t": 36.0, "sound": "alarm",  "gain": 0.8, "music": false }
```

One-shot 2D cues fired the frame the playhead crosses `t` (exactly once). `sound` is a
NAME the driver maps onto whatever the host has (resolveAudio path / a synthesized
fallback). `music: true` routes through playMusic (looped until the next music cue or
cutscene end). Cues are NOT fired when seeking/scrubbing (only x3 events re-fire then).

## fades

```jsonc
{ "t": 0.0, "dur": 2.0, "from": 1.0, "to": 0.0, "color": [0, 0, 0] }
```

A full-screen quad whose alpha ramps `from -> to` over `[t, t+dur]` and then HOLDS `to`
until the next fade with a later `t` begins (last-writer-wins ordering by `t`). Fade-in
from black = `{from:1, to:0}`; smash-to-black = a short `{from:0, to:1}`; the white-out
crash flash = `color:[1,1,1]`. Evaluation is pure: `evalFade(t)` returns {rgba}.

## letterbox

```jsonc
"letterbox": { "inAt": 0.0, "inDur": 1.0, "outAt": 52.0, "outDur": 0.8, "frac": 0.11 }
```

Cinematic bars: each bar's height eases in to `frac * screenH` over `[inAt, inAt+inDur]`
and back out over `[outAt, outAt+outDur]`. `frac` clamped to [0, 0.45].

## titles

```jsonc
{ "t": 54.0, "dur": 6.0, "text": "ESCAPE FROM LAB ZERO", "font": "title",
  "sizeFrac": 0.085, "fadeIn": 1.0, "fadeOut": 1.0, "color": [0.92, 0.95, 1.0] }
```

* `font` maps to FontRole by name: `"title"` (Orbitron-Bold), `"menu"` (Space Grotesk),
  `"news"` (Space Mono), `"mono"` (Roboto Mono). Unknown -> validation error.
* `sizeFrac`: glyph size as a fraction of min(screenW, screenH).
* Alpha ramps 0->1 over the first `fadeIn` seconds, 1->0 over the last `fadeOut`.
  Drawn centered; multiple simultaneously-active titles stack vertically in file order.

## events (x3.fire)

```jsonc
{ "t": 47.0, "name": "fx.impact:jake_ship" },
{ "t": 68.0, "name": "intro_complete", "endState": true }
```

Named events fired through the player's `onEvent` callback exactly once, the frame the
playhead crosses `t` (order: ascending `t`, then file order). `endState: true` marks an
event that MUST still fire when the cutscene is skipped (flags, unlocks, handoff state).

Driver-reserved name shapes (still plain events — the data stays declarative):
* `fx.trail.start:<actorId>` / `fx.trail.stop:<actorId>` — smoke/debris trail emitter
  pinned to that actor's evaluated position.
* `fx.impact:<actorId>` — an impact flash burst at the actor.
* `intro_complete` — the cold-open completion StoryFlag (host sets + persists it).

## Player semantics (x3::cut::CutscenePlayer)

* `tick(dt)`: advances the clock; fires every audio cue + event with
  `prev < t_cue <= now`. Clamps at `duration`; `done()` once there.
* `seek(t)` (scrubbing, `--cuetime`): jumps the clock; events in the jumped-over range
  fire through `onEvent` with `seeked=true` (so the driver can rebuild FX state);
  audio cues do NOT fire. Deterministic: seek(T) then tick leaves the same fired-set
  as playing to T.
* `skip()`: only if `skippable`. Jumps to `skipTo` (default `duration`); events in the
  skipped range fire ONLY if `endState` (others are dropped forever); audio is not
  fired. Playback continues from `skipTo` (the title tail still plays). A second skip
  at/past `skipTo` jumps straight to `duration` (still firing remaining endState
  events). `skipped()` latches.
* Validation (`validate(cs, errors)`): format magic, duration > 0, camera >= 2 sorted
  keys, fov range, times within [0, duration + 0.5], unique non-empty actor ids,
  non-empty models/sounds/texts/names, known font names, letterbox frac range, sorted
  actor keys. `loadCutsceneFile()` parses (self-contained recursive-descent JSON,
  same stance as level_loader.cpp) + validates; errors are returned, never thrown.

## CLI / wiring

* The cold open plays for the cell worlds (level1/elevator/canonlevel/intro) before
  the world build, windowed only — EXCEPT when the `intro_complete` StoryFlag is set
  (it already played on this machine/save), `--skipintro` is passed, or
  `--test-boottime` runs (the boot gate measures the machine, not content).
* `--cutscene <file>` — play THAT file at boot regardless of flags (authoring loop).
* `--cuetime <s>` — start the played cutscene scrubbed to `s` seconds.
* `--cutscene-shot <out.png>` — HEADLESS still: build the cinematic scene, seek to
  `--cuetime`, render + capture one frame, exit. The film-still pipeline.
* `intro_play` console command — replay the cold open mid-game (blocking cinematic
  loop; control + camera return to the player after).
* `--test-cutscene` — headless self-test: format validation, spline/eval checks,
  exactly-once event firing, seek/skip semantics, StoryFlags round-trip, and a parse +
  validate of the real shipped `assets/cutscenes/cold_open.cutscene.json`.

## StoryFlags

`x3::cut::StoryFlags` — a tiny persisted string-flag set (one flag per line) at
`%LOCALAPPDATA%/X3Native/story_flags.txt` (fallback: cwd). `intro_complete` is the
first consumer: set + saved when the cold open finishes (played OR skipped — the
skip path fires the endState event), checked at boot to play the intro once per save.
