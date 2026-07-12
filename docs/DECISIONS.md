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
