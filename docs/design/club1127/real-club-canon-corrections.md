# Club 1127 — canon corrections from Tim (2026-07-27)

Direct from the person who owned the room. These **supersede** the existing canon in
`app/club1127.h` where they conflict. See [club1127-is-a-real-room] — fidelity to the real
room beats anything that merely looks good.

---

## 1. The screens: 8 × 85" LG C1 — NOT the 6-screen mixed multiplex

**Current canon is wrong.** `app/club1127.h` says:

> `* TV MULTIPLEX — 6 screens (80/85/75/65/55/55") on a POE network.`

The real club had **EIGHT screens, all the same: 85" LG C1**. Not a mixed bag of sizes.
Tim: *"I had 8 screens, but we can have a few more."*

### What changes

| | Old canon | Corrected |
|---|---|---|
| Count | 6 | **8 minimum** (more is sanctioned) |
| Sizes | 80/85/75/65/55/55" mixed | **all 85"**, uniform |
| Type | unspecified "POE multiplex" | **LG C1** |

Uniformity matters visually: eight identical large panels read as an installed *video
wall*, whereas six mismatched sizes read as scavenged. That difference is the whole look.

`Stats::tvScreens` currently targets 6 — it must move to 8+, and the self-test with it.

### Panel geometry

⚠ **Check this before modelling.** The LG **C1** OLED line was 48 / 55 / 65 / 77 / **83"** —
there was no 85" C1. LG's 85" sets of that era were NanoCell/QNED LCD. So the real screens
were probably **83" C1 OLEDs** (commonly rounded to "85"), or genuinely 85" QNEDs. Worth a
word from Tim, because it changes the panel proportions slightly and the bezel a lot.

Working dimensions, 16:9 active area:

| Nominal | Diagonal | Width | Height |
|---|---|---|---|
| 83" (C1 OLED) | 2.108 m | **1.837 m** | **1.033 m** |
| 85" (QNED LCD) | 2.159 m | **1.882 m** | **1.058 m** |

Either way they are ~1.85 m wide. Eight of them need ~14.8 m of wall before spacing — the
main room's long axis is `kCW = 30.48 m`, so eight fit comfortably along one wall, and
twelve would still fit with gaps.

**C1 look notes (if OLED):** near-bezel-less, a very thin top two-thirds with a thicker
lower chassis, glossy black panel that reflects the room when dark. Per
`X3_WORLD_RULES` rule 7, dark-glass rounded screens are the display standard — never flat
bright quads.

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
