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

## 🌀 CANON: THE GATE IS ONE LARGE MACHINED METALLIC TUBE (Tim, said 4+ times)
**No dashes. No chevrons. No hazard tape.** The 48-segment amber "ratchet track" ringing the
gate face WAS a **dashed yellow ring** — each segment covered ~53% of its arc — and it read as
caution tape wrapped around the portal. It is not in the reference. It is dead. If the ring
needs an accent it is **subtle machined trim or ONE thin recessed light line** — continuous,
dim, cool, integrated into a machined groove. Never a repeating warm pattern.

## 🔍 PROVE THE SURFACE CAN BE LIT BEFORE YOU TOUCH A LIGHT (2026-07-12)
The rifthub tube ate **nine rounds** of art because everyone tuned the *wrong variable*. The
order of operations that actually cracked it:
1. **Is the surface receiving light at all?** Put a **flat WHITE albedo** on it and a bright
   probe light next to it. If it is *still* black, **no albedo and no lamp will ever fix it** —
   the bug is geometry/topology (see KNOWN_BUGS **R3: the gate was mirrored**, det −1).
2. Only then, **VALUE** (albedo).
3. Only then, **LUMENS**.
Doing this backwards is how a light rig gets tuned to 70 against an object that is physically
incapable of responding — and how "5× the key light barely moved it" gets misread as "the art
is too dark" instead of "the object is inside-out."

**A/B probe that settles it in one build:** drop a GLB **cube** carrying the suspect's *exact*
material beside it. If the cube blows out white and the suspect stays black, it is **not** the
material and **not** the lighting.

**THE INSTRUMENTS EXIST NOW — USE THEM BEFORE YOU GUESS (`r_debugview`, 2026-07-12):**
| | |
|---|---|
| `r_debugview 1` | **shading normals** — "is it inside-out?" (R3) |
| `r_debugview 2` | **the point-light term ALONE** — no albedo, no ambient, no sun. Whatever is on the light path glows; whatever is not is black. There is nowhere for a photon to hide. |
| `r_debugview 3` | **albedo alone** |
| `r_debugview 4` | **the ambient/IBL term alone** — everything a surface gets that did NOT come from a lamp |
| `r_debugview 5` | **step 1 of the recipe above, automated**: real lighting, albedo forced to a flat 0.5 |
| `r_flashlight 0` | judge a room on its OWN practicals — never on the light riding the camera |
| `--set r_autoexposure 0 --set r_exposure 1.0` | **pin the exposure or your before/after is fiction.** Auto-exposure re-normalises every capture; two shots of the same wall are not comparable until you nail it down. |

**AND: THE HUE TELL CAN LIE.** "A surface lit by a warm lamp reads warm, so a blue surface gets no
light" cost a full investigation (KNOWN_BUGS **R5**). It is only true if the albedo is *neutral* and
the ambient is *honest*. A blue-biased albedo can overturn a warm lamp, and until 2026-07-12 every
scene carried an invisible **blue-sky ambient that `setAmbient` could not turn off** (KNOWN_BUGS
**R4**). Both forge the exact fingerprint of "not on the light path". **Measure, don't infer.**

## 🧪 REGRESSION DISCIPLINE
"The panel exists" and "the panel *shows something*" are **not** the same assertion. Tests must
measure the **thing that actually broke** — e.g. `holoReadoutInkFraction()` probes **ink** in the
baked text band, and ships with a **negative control** proving the probe *can* fail. A blank or
washed-out screen must **FAIL the test**, not silently pass. Mutation-test your own guard.

## 🔦 ASK WHAT THE SURFACE *SEES* BEFORE YOU TOUCH A LAMP (2026-07-12)
**If a surface won't respond to light, its ENVIRONMENT is the suspect — not the lamp, not the albedo.**
The same root cause bit us from both ends in one day:
- **The rift-hub gate** was *"a mirror aimed at a black room"* — `setIblProbe(true)` bakes the env cube
  **from the scene**, and the scene is a deliberately dark hall. Its specular came back ~0 and fell
  through to a flat constant. It rendered as grey mush **no matter how good the mesh was**, and no
  key light could fix it. **Metal is lit by being SHINY AT SOMETHING BRIGHT. There was nothing bright.**
- **Every windowless interior** was the mirror image: an env cube baked **by default, from the analytic
  BLUE SKY**. So the cells and corridors were lit by a full-strength sky, in a basement.

**And `setAmbient()` alone is a DEAD DIAL.** The engine has TWO ambients: `iblAmbient()`'s baked-env path
takes diffuse from `irradianceCube` + specular from `prefilterCube` and **never reads its `ambient`
argument**. With `setAmbient(0)` the probe still measured **55/255 of blue sky**. Every "I brought the
ambient down" tune in a baked-env room did **nothing**.
Someone had even tinted the ambient **blue** to fight a blue sky they couldn't see and couldn't switch
off — so when the sky died, **the tint became the bug**.

**THE RULE:** a room must DECLARE its air — `setWorldAtmosphere` (scene IBL probe + IBL intensity +
a NEUTRAL low ambient). Never `setAmbient` and hope. And the diagnostic order is:
**(1) can this surface be lit at all?** (white-albedo + probe-light, or `r_debugview 5` = albedo forced
flat — *if a room of 50% reflectors is still dark, the surfaces are innocent*) →
**(2) what is its environment?** → **(3) VALUE** → **(4) lumens, last.**

## 🏗️ GENERATE, DON'T HAND-CARVE (2026-07-11)
Everything we **generated** worked immediately; everything hand-carved was rejected 3×.
- **Image → texture:** SD3.5 forge (incl. `--img2img` from a concept image) → `G:` surface library.
- **Video → animation:** flipbook atlas baked from reference footage (the rift membrane).
- **Image → geometry:** Rodin (already how ~65 characters and the weapons were made).
- **Procedural authoring:** headless Blender (`bpy`) for the gate tube.
*Stop reverse-engineering a beautiful concept by gluing boxes together.*

## 📐 COMPUTE THE DETERMINANT BEFORE YOU BLAME THE ART (2026-07-12)
A model matrix's upper 3×3 must be a **rotation**: **det = +1**. **det < 0 is a mirror** — the mesh
draws inside-out and **cannot be lit by anything**, at any albedo, under any light. It looks exactly
like an art problem (perfect silhouette, right albedo, right relief, coherent specular), which is why
it ate nine art rounds on the rift gate and every prop on the descent slide.
- **Never hand-roll a basis from a direction vector.** `app/basis.h::basisFromOutward()` — one helper,
  guaranteed right-handed, and the copy-pasted sites route through it.
- **When a surface "won't light", prove the surface CAN be lit before you touch a light**: drop a cube
  carrying its exact material into the same room. Cube blown out + object black = **mirror**, not art.
- **The invariant is TOTAL, not a list.** `--test-basis` walks every entity of every built world. A
  list of known sites rots the moment someone pastes the idiom into a new file.
