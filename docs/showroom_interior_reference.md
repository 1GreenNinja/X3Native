# ShowRoom_Vol30 — Interior Architecture + Look Reference (DAY target)

Reference for faithfully rebuilding the Unity "ShowRoom_Vol30" gallery interior in the
X3Native engine. Read-only study of the Unity reference screenshots cross-referenced
against the imported GLB (`assets/converted_glb/ShowRoom_Vol30/Example_01.glb`) using
`tools/glb_node_bounds.py`.

Source scene: Unity 6.3 LTS (6000.3.9f1), HDRP, scene name `Example_01`.
This document describes the **DAY** look (bright, snowbound, overcast-bright sky). It is
the target for the DAY side of the planned day↔night toggle.

---

## 0. Coordinate frame & global bounds (from the GLB)

All numbers are GLB **world space** (the units the loader sees), Y up, building footprint
centered around X≈72, Z≈-111. The exterior snow ground sits at **Y = -9**.

- Building bounds: **min (12.6, -9, -139.1) → max (131.7, 88.5, -82.9)** (windows/spire push higher).
- Footprint: ~119 wide (X) × ~57 deep (Z). The visible *interior gallery* is the inner
  ring roughly X 40→104, Z -100→-122, centered on (72, _, -111).
- Spire core rises over ~(72, -100) up to Y≈97.
- **The interior is a single tall circular atrium**, not a stack of enclosed floors. The
  "floors" read as concentric **disc/ring platforms** stepping up toward the glass shell,
  with a vaulted glass dome (the Tube_Half shells) over the center and the spire punching
  through the top.

Vertical level summary (Y is up; "top-Y" = walkable surface where relevant):

| Level | Y (approx) | What it is | Primary GLB nodes |
|---|---|---|---|
| Sub-floor / plinth | -12 → -9 | exterior approach discs / building base ring under the snow line | `Plateform_01`, `Plateform_02` (top -8/-9) |
| **GROUND floor** | walk on **-9** | main circular gallery floor, the blue central pad, carpets, low display cases | `Carpet_01` (y -9), `Showcase_06/07`, `Stair` bottom, `Pilar_05` (low kiosk bases) |
| Ground display tier | -9 → -5.5 | tall showcase plinths / paint panels standing on the ground floor | `Paint_Showcase_01` (-9→-5.5), `Showcase_07` (-9→-5.5) |
| Mid step (½-floor) | walk on **-5.5 → -4** | a stepped ring just above the ground pad (showcases sit on it) | `Showcase_04` (-5.5→-4), `Plateform_05/06` tops (-4.4) |
| **2ND floor / mezzanine** | walk on **-3 → 3** | the raised ring platform / inner room deck reachable by the stairs | `Room_01` (-3→3), `Showcase_01/03/05` (-2.7 base), `Carpet_02` (-2.7), `Showcase_03` glass dividers to +2.5 |
| Mezzanine wall band | 3 → 26.7 | the tall flat panel walls / window mullion band rising off the 2nd floor | `Pilar_02` (3→26.7, 1.7 thin = wall/glass panels) |
| Structural ring frame | -9 → 27 | the big square structural pylon frame (corner posts + lintel ring) defining the atrium | `Pilar_01` (-9→27, the 4 m-thick frame members) |
| **Vaulted glass dome** | 6 → 13.6 | three stacked half-tube shells forming the stepped glass roof over the center | `Tube_Half_01` (rings at y6–7, 9.2–10.1, 12.8–13.6) |
| Top ring / cornice | ~27 | the ring where the walls cap and the spire begins | tops of `Pilar_01` / `Pilar_02` (~27) |
| **Spire core** | 27 → 97 | the central tower shaft that the atrium opens up into | `Pilar_03` (27→97) |
| Spire glazing | 37.6 → 90.4 | tall vertical window strip up the spire face | `Windows_01` (37.6→90.4) |

> Note on disc count: from the exterior elevations (114752, 133643) the building reads as
> **two big outer ground discs** (the wide flat apron + the slightly raised gallery ring)
> plus the central tripod/strut cluster carrying the spire. The interior screenshots show
> the gallery ring is itself **terraced** (ground pad → mid step → 2nd-floor ring), which
> is exactly the -9 / -5.5 / -3 clustering the Y-histogram confirms.

---

## 1. Architecture — the multi-tier circular layout

The hero interior shot (`013904`) reads, from center outward and bottom to top:

1. **Central blue circular pad (ground floor heart).** A low, glossy mid-blue disc at the
   very center of the atrium, ringed by a low curb/curved bench. This is the social heart
   of the ground floor. Maps to the central `Carpet_*`/floor meshes at **Y = -9** under the
   spire centerline (~X72, Z-111). The blue tint is a floor material, not lighting.

2. **Concentric ring platforms (the terraces / mezzanine tiers).** Around the blue pad the
   floor steps up in concentric arcs: ground pad (-9) → a mid step (~-5.5/-4) → the raised
   **2nd-floor ring** (-3→3). Each terrace is a curved walkable band with a glass/metal
   railing at its outer/inner lip. Maps to `Plateform_05/06` (mid), `Room_01` + `Showcase_01/03/05`
   deck (2nd floor), `Carpet_02` (2nd-floor runners).

3. **Angled "/" support struts.** The most distinctive structural motif (clearest in
   `120931`, `120945`, exterior `114752`/`133643`): pairs of **canted, blade-like struts**
   that lean inward/upward, carrying the upper ring/disc edges and the dome. They read as
   white-clad angled fins. In the GLB these correspond to the **canted members of the
   `Pilar_01` frame and the diagonal faces of `Plateform_05/06`/`Pilar_05`**, plus the
   sloped underside of the disc rings. On the exterior they fan out as the **tripod legs**
   under the spire. *This is the architectural feature a hidden door could be set into
   (see §5).*

4. **Curved floor-to-ceiling glass shell + vaulted dome.** The whole gallery is wrapped in
   a curved, slightly tinted glass curtain wall that leans inward as it rises, capped by a
   stepped vaulted glass dome. The dome is built from `Tube_Half_01` half-tube shells in
   three concentric rings (y 6–7, 9.2–10.1, 12.8–13.6) — i.e. a stepped/coffered glass
   vault, not a smooth dome. The vertical glazing is `Pilar_02` (thin 1.7 panels, 3→26.7).

5. **Spire core.** Dead center, the floor/atrium opens up into the spire shaft (`Pilar_03`,
   27→97) with a tall vertical window strip (`Windows_01`). From inside, looking up past the
   dome you see the underside of the spire; from outside it's the dominant vertical tower.

**Where each functional level sits:**
- **GROUND floor** = the Y -9 disc (blue pad + outer gallery ring + low showcases). Civilians
  mingle here.
- **2ND floor** = the raised ring at Y -3→3 (`Room_01` deck), reached by the two `Stair`
  runs. Workstation/mezzanine figures live here behind glass railings.
- **ABOVE-gallery (new)** = there is headroom between the 2nd-floor deck top (~Y3) and the
  dome/ring cornice (~Y27). A new hidden gallery level could sit at roughly **Y 8–14**,
  tucked behind the dome shells / inside the `Pilar_02` wall band, overlooking the atrium
  (see §5).

---

## 2. Materials / look

- **White panel cladding** — dominant material. Matte-to-satin off-white architectural panels
  with fine seam lines, on the struts, disc edges, ring fascia, spire skin. Slight warm-grey
  in shadow. Low specular, soft sheen — think powder-coated aluminium / painted GRP, NOT
  glossy plastic. (`Plateform_*`, `Pilar_01/02/05`, `Tube_Half` frames, spire skin.)
- **Dark / tinted glass** — the curtain wall and dome glazing are smoky blue-grey, semi-
  reflective, ~25–40% transmission. From inside, the snow reads through it as a bright,
  slightly desaturated wash; from outside the glass goes near-black with bright sky
  reflections (`013642`, `120931`). Railing glass is a lighter, clearer tint.
- **Metal struts / mullions** — the window mullions and railing posts are brushed/anodized
  mid-grey metal, thin and crisp; cooler than the white cladding.
- **Blue central floor** — the heart pad is a polished mid-blue (slightly teal) terrazzo/resin
  with a soft gloss and faint reflection of the dome above. Highest-chroma surface in the room.
- **Carpets / runners** — neutral light-grey low-pile runners on the gallery floor and 2nd-floor
  ring (`Carpet_01/02`), zero thickness in the GLB (flat decals).
- **Showcase plinths** — white/grey boxes and thin glass-front display cases (`Showcase_01–07`),
  some with a back glass panel (`Showcase_03`, 5.3 tall). Read as museum vitrines.
- **Emissive / ceiling light** — the dome ring and the underside of the upper discs carry
  thin, soft cool-white emissive strips/cove lighting (visible glowing along the ring edges in
  `120850`/`013904`). Subtle — most of the interior brightness is daylight, not emissives.
- Overall finish: clean, near-monochrome white-and-glass with the single blue accent pad;
  reflections are soft and broad (rough metal + tinted glass), not mirror-sharp.

---

## 3. Lighting & palette — DAY (grade target)

This is the bright-daytime, snow-on-the-ground look. Match these for the DAY side of the toggle.

- **Key light (sun).** A single HDRP **Directional Light**, named "Directional Light", with
  (from the Unity Inspector, `151709`):
  - **Rotation X = 69.31°** (steep, high sun — ~21° above horizon complement; reads as a high
    midday/early-afternoon winter sun), **Y = 9.7°** (azimuth, almost due-scene-forward),
    **Z = 0**.
  - Position Y = 17.26 (irrelevant for a directional light's direction).
  - A `Turn_Move` script drives **Turn Y = 15** (slow Y rotation), i.e. the sun slowly orbits
    — so a static match at ~Y10–25° azimuth is fine.
  - **For X3Native: aim the DAY sun ~20° above the horizon, azimuth roughly aligned with the
    scene's +screen-forward, casting from high and slightly to one side.** Steep enough that
    shadows are short and pooled under the discs.
- **Sky / ambient.** Bright, high-key overcast-to-clear winter sky: a pale cool-blue zenith
  fading to a warm-grey/white haze at the horizon (`114752`, `133643`). Strong uniform sky
  ambient — interiors are filled with soft skylight, very few truly dark corners.
- **Snow bounce.** The biggest interior light source after the sun is **bounced light off the
  snowfield** coming up through the glass — a bright, slightly cool fill that lifts the floor
  and the undersides of the discs. Grade the interior fill toward this: bright, low-contrast,
  faintly blue-cool. Floors and white panels near the glass should read almost blown-bright.
- **Exposure / white balance.** High-key, near-overexposed whites on the cladding and snow;
  the camera clearly auto-exposes for a bright scene (interior `013904` and exterior spire
  `120945`/`124524` push the white cladding to near-clip). White balance is **neutral-to-cool**
  (the snow is white, glass and shadow go cool blue). Keep midtones bright, let white panels
  near windows bloom slightly.
- **Shadows.** **Soft** and relatively short (high sun + heavy skylight fill). Contact shadows
  under showcases and disc edges are gentle, not crisp. Ambient occlusion is subtle in the
  recesses under the rings and behind struts. No hard black shadows anywhere in DAY.
- **Palette.** White → cool-grey architecture, smoky blue-grey glass, the one mid-blue floor
  accent, snow-white exterior, pale-blue sky. Essentially **monochrome white + a blue accent**,
  high value, low saturation. Any warm note comes only from the low horizon haze.

(The night target — for contrast when you build the toggle — would invert this: kill the sun,
let the emissive cove strips + blue pad + spire glazing carry the room, sky goes deep blue.
Several exterior shots like `151616`/`013642` already show the dusk/cool-blue end of the range.)

---

## 4. Zones (where people / activity go)

From the interior shots (`013904`, `120850`):

- **Ground floor — lounge / social (Y -9).** The blue central pad + its ring bench is the
  lounge. Low seating and the curved benches sit around it. Civilians gather and mill here.
  Display cases (`Showcase_06/07`) ring the outer ground floor like a museum lobby.
- **Ground floor — gallery walk.** The outer ring of the ground floor (carpet runners
  `Carpet_01`, tall paint panels `Paint_Showcase_01`, vitrines) is a gallery promenade.
- **2nd floor / mezzanine (Y -3→3).** The raised `Room_01` ring deck holds the
  **workstation / mezzanine figures** (standing/working NPCs) behind **glass railings**.
  Display cases `Showcase_01/03/05` and runner `Carpet_02` define the mezzanine exhibits.
  This deck overlooks the atrium and the blue pad below.
- **Glass railings.** Both the 2nd-floor ring lip and the stair runs are edged with low
  clear-glass-and-metal railings (`Showcase_03` reads as the glass divider/balustrade panels,
  the metal posts are the mullion-style struts). Visible curving away in `120850`/`120945`.
- **Stairs.** Two `Stair` runs (bottom Y -9, top Y -1.5; one near X44–54/Z-118, one near
  X90–100/Z-110) climb from the ground pad up to the 2nd-floor ring. These are the public
  vertical circulation.

---

## 5. Build mapping — concrete notes for the X3Native rebuild

Loader-facing guidance keyed to GLB nodes/levels (the loader is the spine — drive this from
the level JSON, don't hand-place boxes).

- **Civilian GROUND floor** = walkable plane at **Y = -9**, inside the gallery ring
  (~X40→104, Z-100→-122). Source nodes: `Carpet_01`, `Showcase_06/07`, central blue pad,
  `Pilar_05` kiosks, base of `Stair`. Spawn lounge/social NPCs here.
- **Civilian 2ND floor** = walkable deck at **Y = -3 → 3** (`Room_01`), reached by the two
  `Stair` runs (top Y -1.5, so a short final step onto the deck). Source nodes: `Room_01`,
  `Showcase_01/03/05`, `Carpet_02`. Spawn workstation/mezzanine NPCs + glass railings here.
- **Hidden door in the angled "/" strut/panel.** The canted strut/panel faces — the diagonal
  blades of the `Pilar_01` frame and the sloped `Plateform_05/06` / `Pilar_05` faces on the
  *inner* side of the ground ring (around X60–84, Z-88 or Z-133, where the struts meet the
  floor) — are large flat clad surfaces ideal for setting a **flush hidden door**. The seam
  hides in the existing panel seam lines. Pick the strut face least visible from the central
  pad (the back/outer strut at Z-133) for the secret entrance.
- **Internal stairs → "elevator level."** Reuse / extend the existing `Stair` runs as the
  start of a concealed climb. A hidden stair behind the strut door can run up inside the
  **`Pilar_02` wall band (Y 3→26.7)** — that thin (1.7) wall slab has the vertical depth to
  hide a switchback stair or a lift shaft climbing from the 2nd floor (Y3) up toward the
  cornice ring (~Y27). Treat ~Y10–14 as the "elevator level" landing.
- **Hidden ANALYST GALLERY with dark one-way glass, ABOVE the 2nd floor.** Place it at
  roughly **Y 8–14**, tucked into the volume behind the **lower dome shells (`Tube_Half_01`
  rings at y6–7 and 9.2–10.1)** and the inner face of the **`Pilar_02` wall band**, on the
  outer arc of the ring (e.g. behind the wall around X40–60 or X84–104). From there a
  **dark, near-opaque one-way glass panel** (reuse the tinted-glass material, crank tint to
  ~90% so it reads black from below but transparent from inside) looks straight down onto the
  blue pad and the 2nd-floor deck. The dome shells and wall band naturally hide the room's
  back from the public atrium. Reach it via the hidden stair/elevator above.
- **Glass shell + dome** = `Pilar_02` (vertical curtain wall, Y3→26.7) + `Tube_Half_01`
  (three stepped vault rings, Y6→13.6). Use the tinted-glass material; keep it a single
  curved shell so the snow/day light reads through it.
- **Spire** = `Pilar_03` (27→97) + `Windows_01` glazing; mostly decorative/skybox-facing for
  interior play but defines the atrium ceiling opening.
- **Exterior approach** = `Plateform_01/02` discs at Y -12→-8 are the snow-level apron/ramp
  up to the ground floor; the `Stair`/ramp at the building edge (exterior `120906`) is the
  public entrance from the snowfield.

---

## 6. Key images (most informative)

- `Screenshot 2026-05-31 013904.png` — **HERO interior.** Wide atrium: blue central pad,
  concentric terraced rings, glass shell with snow beyond, dome ring cove light, the angled
  struts. The single best whole-room reference.
- `Screenshot 2026-05-31 120850.png` — interior wide, looking across the gallery: shows the
  glass railings, curved tinted shell, mezzanine, and the soft daylight fill.
- `Screenshot 2026-05-31 120931.png` — close-up of the **curved glass railing + an angled "/"
  strut** meeting the floor; best material/strut detail (white cladding, tinted glass, metal).
- `Screenshot 2026-05-31 120945.png` — mezzanine railing + canted struts + dome edge; shows
  how the 2nd-floor ring lip and struts read in light.
- `Screenshot 2026-05-31 120758.png` — interior **stairs** detail (the public ground→2nd-floor
  stair runs and railings).
- `Screenshot 2026-05-31 013642.png` — **HERO exterior**, low angle: spire, tinted-glass shell
  going near-black, tripod struts, snow — the day-glass-reflection reference.
- `Screenshot 2026-05-31 114752.png` / `133643.png` — clean full-building **exterior
  elevations**: concentric ground discs, tripod strut cluster, spire, snowfield + tree line +
  pale-blue day sky. Best for overall massing and DAY sky palette.
- `Screenshot 2026-05-31 151709.png` — Unity **Inspector for the Directional Light** (sun
  Rotation X69.31 / Y9.7 / Z0, Turn_Move Turn Y=15). The DAY key-light direction reference.

**Not the showroom (noted, not analyzed):** `013100`, `014048`, `000137`, `020118`, `020510`,
`021026` are a lone skinned figure under colored neon light-panels / a portal-neon city test
(the Riftforged "L_NeonBlock01" Neon Sprawl scene). `153707` (car + figure studio) and
`164736` (a CMS "New post" web page) are unrelated.
