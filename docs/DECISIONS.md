# DECISIONS & CANON — X3Native

**Append-only. Every session adds; nothing is deleted.**
This exists because the fleet kept rebuilding things that already existed (the holo terminal was
independently "fixed" **four times**, each time in a different colour) and re-litigating settled
calls. If it's decided, it's written here. **Check here before you design anything.**

---

## 🛠️ FLEET PROTOCOL — THE ONE-LINE RULE (2026-07-12)

**The disease:** two integration lines on two machines (`integration/playable-build` on the
14900K, `integration/playline-fold` on the 13700K). **Neither person ever saw the whole game.**
Tim kept reporting bugs that were already fixed — *on the other box*. "We fixed that 9 times"
was **true**; it just never happened in the same place twice. 227 branches: 97 already merged,
**103 dead**, only **13 that mattered**.

**The rule, from now on:**
1. **ONE line.** `main` is the game. (It is a strict ancestor of playable-build — a free
   fast-forward.) Everyone pulls `main`. Tim plays `main`.
2. **Feature branches are SHORT-LIVED** — merge within a day or two.
3. **NO second integration branch. Ever.** If you need staging, stage on the line behind a flag.
4. **Post your landing to FleetCommand with the commit hash.** The channel is the ledger of
   *where things are*, not chatter.
5. **Assets:** self-hosted LFS (GitHub's budget is dead and its metered pricing is a trap on
   274GB). Blobs live on our own hardware, mirrored to `G:`.

---

## 🖖 CANON: ALL SPACESHIPS ARE SELF-LIT (Tim, 2026-07-12)
> *"All spaceships shall have built in lighting like in StarTrek. New canon rule."*

Every ship carries its own lighting. **A ship must NEVER render as a black silhouette**,
regardless of where the star is. Emissive hull accents, running/nav lights, lit window rows
(per-texel emissive), engine glow (brightest, but not the *only* thing), plus a modest
self-illumination/rim term so form reads on the unlit side.
**Nuance:** this is a deliberate *stylistic* self-light, **not** a return to the crutches. The
hull must still shade honestly from the directional star — the self-light only lifts the dark
side off zero. (Fixes: the Overlord looking great on entry and going black on its second
appearance.) See `docs/CANON_RULES.md`.

## ☀️ CANON: A STAR IS A DIRECTIONAL LIGHT, NOT A POINT LIGHT
Space was lit with **point lights**, whose inverse-square falloff delivers ~**1/22,500** of their
intensity at fleet distance. That is why Jake's ship was black. Stars are effectively at infinity:
**parallel rays, no falloff.** Never light space with point lights.

## 🌌 CANON: STARFIELDS ARE FINE, VARIED POINTS (Tim, 2026-07-12)
Tiny (sub-pixel to 1–2 px), with **varied** size, brightness, and colour (cool blue-white →
white → warm amber; a few standouts carry the eye). Deterministic per-star properties — they
must **not** shimmer or crawl with the camera. Not uniform white blobs.

## 🖥️ CANON: THE HOLO PANEL (Tim, locked)
**BLACK GLASS** slab · glowing **BLUE / GREEN / ORANGE** status text (**blue, NOT cyan** — Tim's
own words, in a code comment on `feat/holo-glass-platform`) · **SHINY METALLIC ROUND-PIPE** frame ·
a single support pipe to the ceiling so it **HANGS**, not floats. Crisp and **readable at player
distance** — the panel exists to be READ; decoration loses to legibility, and text must never
fight the line-art behind it. **ONE implementation** (the HoloPanel platform, 4 variants:
terminal / elevator / keypad / placard) serving the cell terminal, rifthub consoles, elevator
panel, keypads and placards. *If two holo implementations exist, that is a bug.*

## 🌀 CANON: THE RIFT HUB
It is **the fast-travel room** — the place you teleport to anywhere in the world. Reached in-game:
a locked **elevator stop at Y = −78** (access code **4790**, found on a lore terminal in Security),
then a 33 m approach hall where the blue glow builds before you round the corner. Gates open when
you walk into them; the consoles let you **dial the parameters** — and get them wrong: **ROOM WARP**,
**TEMPORAL RIFT**, **IMPLOSION** (that gate is dead forever). One large machined **tube**, no chevrons.
The membrane is **real footage** from Tim's reference video (idle / surge / open throat).

---

## 🎨 THE POLISH RECIPE (proven — this is *why* the rifthub, cell and elevator look good)
1. **Honest lighting** — no fake self-emissive, no over-unity albedo, no exposure hacks.
2. **Ambient is NOT light.** It's omnidirectional; raising it lights a room *by destroying its
   contrast*. Bring it DOWN (the rifthub hall: 0.100 → 0.032). Light with real sources.
3. **VALUE, NOT LUMENS.** If a surface reads wrong, fix its **albedo** first. (The gate tube was
   black; 5× the key barely moved it. Renormalizing albedo fixed it.)
4. **Real PBR materials** — rivets/wear/grime in the **normal maps**. Never flat colours.
5. **Per-texel emissive** on every screen (`emissiveMap`) — glow where the image is bright, dark
   where it isn't. Flat emissive floods the pane into a slab.
6. **Atmosphere** — wet reflective floors, fog, light shafts, practicals with visible housings.
   Emissive confined to small lit cores/slits/trims; never whole objects glowing.
7. **Every prop must EARN its place** — story-relevant, interactive, or it motivates a light.
   Otherwise cut it. (The cell had **23 props and 17 lights**; emptiness *is* the feeling.)

## 🧪 REGRESSION DISCIPLINE
"The panel exists" and "the panel *shows something*" are **not** the same assertion. Tests must
measure the **thing that actually broke** — e.g. `holoReadoutInkFraction()` probes **ink** in the
baked text band, and ships with a **negative control** proving the probe *can* fail. A blank or
washed-out screen must **FAIL the test**, not silently pass. Mutation-test your own guard.

## 🏗️ GENERATE, DON'T HAND-CARVE (2026-07-11)
Everything we **generated** worked immediately; everything hand-carved was rejected 3×.
- **Image → texture:** SD3.5 forge (incl. `--img2img` from a concept image) → `G:` surface library.
- **Video → animation:** flipbook atlas baked from reference footage (the rift membrane).
- **Image → geometry:** Rodin (already how ~65 characters and the weapons were made).
- **Procedural authoring:** headless Blender (`bpy`) for the gate tube.
*Stop reverse-engineering a beautiful concept by gluing boxes together.*

## 🔧 CANON: WEAPON ATTACHMENTS + THE WEAPON BENCH (2026-07-12)

**FIVE SLOTS** per weapon — `Optic / Barrel / Magazine / Underbarrel / Coating`. Which slots a
weapon accepts is DATA on the weapon (`WeaponDef::attachSlots`, a bitmask): a pistol has no
underbarrel, the BFG-class energy guns are **Coating-only**, the chaingun has no optic mount.

**ATTACHMENTS ARE ITEMS.** `assets/items/items.json`, category `attachment` + an `attach` block.
The item DB **is** the attachment DB — there is no second data path, and no second economy.

**COATING is the ENERGY slot** (Salvari tech). It is deliberately just a multiplier block + a
`DamageType` override, so a **charge model** landing separately can compose with it by reading the
fitted coating / the effective `DamageType`. Nothing here owns charge.

**THE STACKING RULE (two layers, one fold each):**
1. **Attachments** → `applyAttachments(base, loadout)` → an **effective `WeaponDef`**. Fractions
   compose **multiplicatively** (`prod(1+f)`); `critChance` is flat and **additive**. This is the
   ONLY place a WeaponDef is modified; the roster is never mutated (base + an effective cache).
2. **Skills / mod items** → `PlayerStatMods`, applied at the existing read points. Unchanged.
The two layers multiply (mults) / add (crit). Damage is quantized ONCE per layer. Asserted in
`--test-attachments` against an independently-computed expectation.

**EVERY ATTACHMENT GIVES AND TAKES.** A free-lunch attachment is a bug — `--test-attachments`
fails any attachment with no downside.

**THE OPTIC IS A FEATURE, NOT A NUMBER.** Real ADS: the gun is **aligned behind the glass** by a
closed-form solve (`Arsenal::solveAdsOffsets`) that puts the optic's LENS CENTRE exactly on the
camera axis, so **the sight line, the reticle and the fire ray are ONE line**. The camera FOV
genuinely narrows to the optic's `adsFovDeg` (magnification is a real FOV change, never a zoomed
sprite). A **full scope hides the weapon model** while you are behind it — with the eyepiece at
your eye, the tube would otherwise fill its own lens. The scope PICTURE is an honest screen-space
overlay, **not** a render-to-texture lens (an RTT lens needs a second camera pass into an
offscreen target; it is a follow-on).

**ONE SIGHT POINT.** `attachSightLocal()` is the single source for the lens centre — the drawn
glass AND the ADS solve both call it. If they ever diverge, the reticle lies about where the
bullet goes. `--test-attachments` measures the scoped lens centre against the fire ray (< 1 mm)
and ships **two negative controls** proving the probe can fail.

**MOUNTS ARE MEASURED, NOT GUESSED.** Barrel mods ride the measured `vmMuzzle` (346f5e7). Optic /
magazine / underbarrel / coating are **BORE-relative** in Y (everything on a gun is arranged
around the bore) and **origin-relative** in Z (the weapon GLBs are authored centred, so the origin
IS the receiver). The measured viewmodel **box** (`WeaponDef::vmBounds`, from real mesh bounds)
supplies only what a length cannot know: the centre-line and the flank. Anchoring on the box's
extremes hangs parts off the gun's tallest/lowest point and they **float in mid-air** — that bug
was built, photographed, and fixed.

**THE BENCH.** Attachments are **FITTED AT A BENCH** (the F1 **Security Station** — a room you
already have to earn), never hot-swapped from the backpack. Canon: LATE NIGHT SPEED tunes cars;
this tunes guns. `[E]` opens it; the screen **is a HoloPanel** (`bakeBench` — a new BAKER on the
ONE holo platform, blue/green/orange, never cyan). Adding a holo variant means adding a baker.

**VERIFY MATERIALS UNDER A NEUTRAL LIGHT.** The parts first "rendered orange-red" and the reflex
was to blame the renderer. They are light grey. **Jake's cell is washed by a red alarm light** —
a grey surface under a red light is red, and the same parts read blue-grey at the bench under its
blue panel glow. The lighting was honest; the *reading* was wrong. (The inverse of THE PATTERN,
and just as expensive.)
