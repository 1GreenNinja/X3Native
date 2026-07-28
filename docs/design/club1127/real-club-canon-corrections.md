# Club 1127 — canon corrections from Tim (2026-07-27)

Direct from the person who owned the room. These **supersede** the existing canon in
`app/club1127.h` where they conflict. See [club1127-is-a-real-room] — fidelity to the real
room beats anything that merely looks good.

---

## 1. The screens: 8 × 85" LG C1 — NOT the 6-screen mixed multiplex

The real club had **EIGHT screens, all the same: LG C1**. Not a mixed bag of sizes.
Tim: *"I had 8 screens, but we can have a few more."*

### What was actually in the code (corrected 2026-07-27)

An earlier revision of this doc claimed the code built 6 mixed-size screens. **It did not.**
That was a stale comment in `club1127.h`; the built code already made **16 screens, 4 per
wall**, and `--test-club` asserted `s.tvScreens == 16`. Only the SIZE was wrong (85").

| | Header comment (stale) | Code before | Now |
|---|---|---|---|
| Count | 6 | 16 (4/wall) | **16, unchanged** |
| Size | 80/85/75/65/55/55" | all 85" | **all 83"** |
| Type | "POE multiplex" | unspecified | **LG C1 OLED** |

**Count left at 16 deliberately.** Tim ran 8 and sanctioned "a few more"; 16 was already
built and already in the shots he approved. Halving his video wall unprompted would be the
wrong call — flagged here instead. If 16 reads as too many, dropping to 8–12 is a one-line
change to the two `for (int n = 0; n < 4; ++n)` loops.

Uniformity is the point either way: identical large panels read as an installed *video
wall*; mismatched sizes read as scavenged.

### Panel geometry — CONFIRMED: 83" LG C1 OLED

Tim confirmed 2026-07-27: **83-inch C1s.** (The C-series never shipped an 85" — C1/C2/C3
all topped out at 83". "85" is the usual rounding. LG's 85"+ panels were all QNED/NanoCell
LCD, which these were *not*.)

Active area, 16:9:

| | Diagonal | Width | Height |
|---|---|---|---|
| **83" C1 OLED** | 2.108 m | **1.837 m** | **1.033 m** |

Eight need ~14.7 m of wall before spacing. The main room's long axis is `kCW = 30.48 m`, so
eight fit comfortably along one wall and twelve still fit with gaps.

**It is an OLED, and that is the important part.** Modelled correctly it must be:

- **Near-bezel-less** — a hairline frame, not a chunky LCD surround.
- **Thin upper two-thirds** over a slightly thicker lower chassis (the C1 profile).
- **Glossy black glass that goes TRUE BLACK.** An OLED between beats is indistinguishable
  from the wall; an LCD glows dark grey. In a dark club that difference is the whole read,
  and it is the reason the visualiser wall will look right — content appears to float on a
  black void rather than sit on eight grey rectangles.
- Reflective when dark: the panel mirrors the room's neon back at the viewer.

Per `X3_WORLD_RULES` rule 7, dark-glass rounded screens are the display standard — never
flat bright quads. An 83" C1 is the archetype of that rule.

### Why this matters beyond decor

These are the panels the **visualiser wall** runs on. `oledEntities()` currently returns 10
(2 DJ + 6 multiplex + 1 back-bar + 1 lounge). With the correction that becomes 2 DJ + **8+**
wall + back-bar + lounge. The screen-allocation plan in the jukebox merge spec §10 should be
re-read against the new count — eight uniform panels is a much better visualiser wall than
six mismatched ones, and closer to what Tim actually stood in front of.

---

## 2. The lift car: MODEL the Cobra, don't source it

Confirmed 2026-07-27: *"We can make a model of my 2003 Black Cobra."*

The car is **Tim's own car**. Full spec in [lns-lift-car](lns-lift-car.md) — black 2003 SVT
Mustang Cobra "Terminator", 18" Saleen wheels, Nitto NT05, 275/35R18 front and 315/30R18
rear.

### Current state (found 2026-07-27)

`app/club1127.cpp:1578-1596` — the hero car is a single GLB:

```cpp
addCharacter(scene, device, physics, convertedGlbRoot(),
             "Vehicles/CTR.glb", carPos, /*scale*/ 1.0f,
             /*standUpZtoY*/ false, kPaint);
```

- Loaded as an inert prop through the MonsterSystem path (deliberately avoids `env_art.h`,
  which drags `fx.h` and reorders `x3::phys` into a build cascade — do not "fix" that).
- Hoisted at `lx = 9.2, lz = 4.4`, `carY = oy + armY + 0.10` (~1.95 m up).
- `kPaint = {1,1,1,1}` — white tint preserves the GLB's own paint. For a black car this
  becomes the paint control.
- Frozen every frame via `setPropPose` so idle-AI yaw sway cannot drift it.
- Falls back to a box if the GLB fails to load, so the hoist is never empty.

**`Vehicles/CTR.glb` (4.5 MB) is the ONLY vehicle model in the armory.** Nothing
Mustang-shaped exists in `assets/converted_glb` or on `\\p13700\G`. Replacement must be
authored.

### Consequences for the tyre spec

`CTR.glb` is one mesh with no separable wheels, so the staggered sizes cannot be honoured
today. Any authored Cobra **must** have front and rear wheels as separate meshes/nodes, or
the 275 front / 315 rear stagger is unrepresentable — and that stagger is the detail that
makes it read as Tim's car rather than a generic muscle car.

### Route

This box generates locally with SD 3.5 (see the root `CLAUDE.md`) — good for **textures**
(paint flake, tyre sidewall with NITTO lettering, Saleen wheel face) but not for geometry.
The body needs actual modelling. Swapping the path string is a one-line change once a GLB
exists; the work is entirely in producing it.

**Licensing note:** X3Native ships commercially. A scratch-built model of Tim's own car is
clean. A ripped or marketplace Mustang model may not be — check the licence before import,
same as the MilkDrop presets.
