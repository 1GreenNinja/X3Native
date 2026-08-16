# NO SLOP — the law, with receipts
*Written 2026-08-16 after a weekend in which every class of slop below actually
shipped and the owner caught every one of them by eye within minutes. Each rule
carries its receipt. If you are an agent in this repo, these are not
suggestions; violating one and shipping anyway is how you get your lane
reverted. Companion law: docs/design/X3_WORLD_RULES.md (models/placement),
docs/AGENT_PLAYBOOK.md (process), docs/ENGINE_GOTCHAS.md (traps).*

## 1. GREP BEFORE YOU BUILD. The wheel is usually already in the engine.
Receipts, one weekend: wheel smoke drawn as gray CUBES through drawMesh while
`IRenderDevice::submitParticles` (billboards, soft-particle depth fade) sat
finished, carrying the rain; TWO hosts hand-rolled consoles while `IConsole` +
`Hud` existed; every host polled ESC with hand-tracked edge flags while GLFW
key callbacks existed; Jake shipped as a white T-pose statue while the
textured, animated rig sat IN THE SAME DIRECTORY; the tunnel host ran a 45 m
shadow box for months while `applyOutdoorCsm` sat one include away.
**Before writing any new system: search the tree for it. Before writing a new
file: read its neighbors. Ten minutes of grep beats a day of duplicate.**

## 2. EYES ON, FULL-RES, AGAINST A REAL REFERENCE — before it ships, not after.
A feature is not done when it compiles or when the numbers look right; it is
done when you have LOOKED at it rendered and it would pass in a shipped game.
The E46 went out as hero car with black untextured panels while the loader
printed SEVEN warnings naming the defect at every boot. The boost model went
to 35 psi under a dial drawn to 20 and nobody looked at the gauge after
changing the number. X3_WORLD_RULES rule 0 already said this; it applies to
EVERYTHING, not just models.

## 3. NO UNTEXTURED STAND-INS IN A SHIPPED BUILD. Hold it instead.
A flat-tinted "temporary" material is not a placeholder, it is slop with a
schedule. The tree pass was HELD because the published GLBs had no leaf
textures — flat-green polygon shards at 20 m. Holding was correct; the owner
has NEVER complained about a missing feature half as loudly as about a broken
one. If the real texture exists in a source pack, go get it; if it does not,
the object does not ship.

## 4. PAIRED VALUES ARE ONE VALUE. Name the pair at both sites.
The boost gauge art and TurboParams::maxPsi drifted (20 vs 35): needle pinned
off the scale while the digits kept counting. wx_snow_in had two owners and
wiped itself. The orphan-proxy net existed in two loaders and only one was
fixed — the "mini car" survived a whole day. **When two places must agree,
each carries a comment naming the other, and a change to one IS a change to
both.**

## 5. CONTACT IS LAW: feet, tires, marks, roots ON the surface. Measured, not guessed.
Tire marks floated a wheel-radius up (spawned at the HUB transform); Jake stood
0.95 m into the earth (the rig's -0.9488 armature offset, DOCUMENTED in anim.h,
ignored). The fix pattern: use MEASURED offsets from the asset itself
(wheel radius, rootYLockRestY) — never a magic constant, never "looks right".
X3_WORLD_RULES rule 4, extended to runtime.

## 6. A FEATURE BEHIND AN UNSET FLAG DOES NOT EXIST.
46 miles of curving road shipped gated behind env vars nobody set; cascaded
shadows were compiled and unreachable in every --world host; r_velocity (the
TAA ghost fix) could not even REACH the device in hosts that never pushed
PostFX. "The feature was built and the door was shut" is this repo's most
repeated defect. **Defaults ON for the world it was built for; the flag is for
turning it OFF.** (Corollary: turning a default ON means eyes-on the world
afterwards — the outer tour's broken bore hid for two days because nobody had
ever SEEN the tour it was part of.)

## 7. EVENTS, NOT POLLING. The platform's callback beats your edge flag.
Every hand-rolled `escWasDown`/`prevEsc`/`hkEscPrev` pair drops presses shorter
than a frame. The GLFW callback cannot. Same rule for anything with an event
source: subscribe, don't sample.

## 8. THE OWNER'S NUMBERS ARE SPEC. Implement them physically; don't sand them off.
"35 psi", "47.6 HP per psi", "7500 redline, titanium retainers" — these are
design decisions, not suggestions to normalize. The right response is a model
that makes the number TRUE (torque ∝ absolute manifold pressure made 47.6
hp/psi fall out of one formula), plus rule 4: retune everything paired to it.
Overruling the owner's number because a "real" car wouldn't = slop of a
different kind. State the tradeoff once, then build it.

## 9. DIAGNOSE WITH MEASUREMENTS, NOT VIBES.
The "lead weights" car was 3,400 rpm of measured clutch slip; the black spike
was A/B-proven to one road's bores in two probe screenshots; the connector gap
was 3,521.9 measured meters. Every one of this weekend's real fixes started
with a NUMBER. Every one of its wild goose chases (ridge LOD! the black car's
drag!) started with a guess that sounded plausible.

## 10. WRITE THE RECEIPT DOWN.
A defect fixed without a comment naming the symptom, the cause and the receipt
grows back. Every rule above exists because the same class shipped at least
twice. When you fix something real, the commit message and the code comment
carry the story — that is why this file could be written at all.
