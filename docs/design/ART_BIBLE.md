# X3 / ESCAPE FROM LAB ZERO — ART BIBLE v1
*The art director's eye, written down. Every dressing pass, lighting rig, material tint,
and post setting answers to this document. Commissioned by Tim 2026-07-05 ("a huge
painterly rundown all over the game").*

## 1. The Look, in one sentence
**A cold alien facility that light has to FIGHT its way into** — deep steel shadows,
motivated pools of warm human light, one saturated accent per space, air you can see.
Painterly means: value structure first, hue discipline second, detail third.
References we already trust: Alien Isolation (dread + warm CRT pockets), Doom 3 (pools
in darkness), CP2077 (material richness — Tim's stated realism bar), the R4/R5 cell
calibration (our own proof).

## 2. Value structure (the painterly spine — violated = ugly, obeyed = art)
- **Darks own the frame.** 60% of any frame sits below mid-grey. Ceilings darkest,
  floors dark, walls mid. Light is SPENT, not sprayed: the light budget goes to the
  focal object (terminal, door, objective, face).
- **No pure white, no pure black.** Emitters cap short of bloom-blob (the R5 law:
  emissive strength is a scaled, opted-in value). Shadows get lifted by atmosphere,
  never crushed to void (the R4 ceiling lesson).
- **One key per room.** Every space has ONE dominant light statement (the flickering
  tube, the hall's emergency strips, the lab's cold slab of light). Everything else
  supports at ≤ half its energy.

## 3. Hue discipline — zone palettes (base / pool / ACCENT)
Every space = neutral industrial base + warm-vs-cold contrast + exactly ONE accent hue.
Two accents in one room is a red-line offense.
- **Detention block (cells, wards):** steel blue-grey base · warm tungsten pools
  (2.6/2.05/1.3 family) · **hazard AMBER** accent. Security red exists ONLY as small
  lenses/LEDs, never as a wash wider than a doorway.
- **Main Hall / corridors:** darker + colder than cells · sparse cool strips ·
  **EMERGENCY TEAL-CYAN** accent (the existing strips, disciplined). Long shadow
  rhythm from ceiling ribs — corridors are about DEPTH.
- **Security / armory:** near-black base · tight white task pools · **ALERT RED** accent.
- **Research labs:** clinical grey-white base, flatter/even light (the ONE zone allowed
  brighter walls) · **SICKLY GREEN** accent. Cleanliness reads wrong = correct.
- **Secret room / level 4.5 / monster spaces:** black-brown organic base · minimal warm
  practicals · **BLOOD-RED + BIOLUME GREEN** (the one two-accent exception; they never
  share a sightline with facility accents).
- **Executive Suite (F7):** clean dark luxury — near-black calm surfaces (the
  cleanest concrete, large calm tiling), warm clean key, **BRASS/AMBER** accent.
  Wealth reads as ORDER: no grime, no clutter, the only pristine zone in the tower
  (W3-2 addition; Sarah's holding cell on this floor keeps the WARD treatment —
  her cell is the one detention room in paradise, and the contrast IS the story).
- **Boss arena:** detention palette pushed to extremes — biggest darks, hottest key.
- **Surface / facility exterior:** WHITE CONCRETE + BLACK GLASS BANDS (Tim's tower
  spec) · golden-hour sun key · the sky is the accent.
- **Space / cockpit:** near-black + starfield · instrument BLUE/GREEN/ORANGE (the holo
  terminal language) · engine-burn orange as the only warm.

## 4. Materials speak — REALISM MANDATE (Tim, 2026-07-05: "make it look realistic,
## make use of our massive texture library")
- **Real texture sets from the D:\Assets library on every architectural surface.**
  Flat-tinted kit boxes are a placeholder, not a finish. The 210 packs carry thousands
  of authored PBR sets (ALB/NRM + packed masks); the surface-library pipeline makes
  them first-class: catalog → curate per zone → load → tile. A wall without an albedo
  TEXTURE is a red-line offense once the pipeline lands.
- **Channel law (the F3 lesson, now doctrine):** Unity packs channel-pack
  Metallic/AO/Smoothness (HDRP MADS/RMA etc). glTF/engine expects G=roughness,
  B=metallic, and smoothness = 1−roughness. ALWAYS convert channels at
  catalog/load time; NEVER plug a packed mask in raw.
- **Demo scenes are ground truth:** every quality pack ships an arranged demo scene —
  it tells you which texture belongs on which surface, prop density, and composition.
  Mine it (parse the .unity/.prefab YAML or replicate by eye) before inventing layouts.
- Worn industrial everywhere the player can touch: mid-albedo, real normal relief,
  scalar metal/rough tuned when maps are absent.
- Grime is directional and MOTIVATED (drips run down, scuffs at boot height, wear at
  handles/thresholds), not noise sprayed on.
- Emissives are INSTRUMENTS, not lamps: screens, lenses, strips glow to be READ.
  Lighting comes from light fixtures and falls on surfaces.

## 5. Air (the missing engine levers — see the Painterly Levers workstream)
- **Depth fog, per-zone tinted**, subtle (2-4% per 10 m): atmospheric perspective is
  what makes a corridor a PAINTING. Zone hue rides the fog (teal halls, amber-grey
  cells, green labs).
- **Grade:** filmic S-curve; shadows pulled 5-8% toward teal, highlights 3-5% toward
  warm (complementary spine); saturation capped so accents stay in charge; gentle
  vignette (≤12%). One global LUT, per-zone fog does the local color work.
- Dust motes stay subtle (R4 law) and only inside key pools.

## 6. Composition per room
Each dressed room declares: (1) its FOCAL point (gets the key + the accent), (2) its
leading line (pipe run, floor stripe, light rhythm) pointing at the focal, (3) its
dark anchor (a corner the eye rests in). The room recipe template (Wave 3) carries
these three fields — a recipe without them doesn't merge.

## 7. Enforcement (how this stays true)
- Every visual branch ships a 4-angle shot set; the director reads them against §2/§3
  and red-lines in writing. Two accents, blown whites, unmotivated washes, floating
  props, void ceilings = automatic bounce.
- The lint owns geometry truth (Law 1/seams); the bible owns the paint. Both gates run
  before Tim ever sees a build.
