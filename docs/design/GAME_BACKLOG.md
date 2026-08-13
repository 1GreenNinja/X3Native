# Game backlog — the STORY and CONTENT work

Written 2026-08-13 because this list existed ONLY in a Claude Code session task
list. The dashboard reads git branches and design docs; it cannot see session
state, so none of this was durable. The engine work has homes (CITY_BORES_PLAN,
TUNNEL_NEXT, TUNNEL_INTERIOR_PLAN, LEVELDOC_HANDOFF); the story work had none.

Everything below is Tim's, captured from his own words where possible.

---

## 1. EFLZ PREQUEL CANON — the whole arc moved

The largest open item. Tim's direction, from the dogfight cold open:

* If Jake WINS the dogfight he glides down in a cutscene and lands intact,
  "Right outside the white and dark glass huge office building" — so there is a
  LANDING BRANCH, not only the shot-down/captured path.
* Consequence, and this is the hinge: a smooth landing means he does NOT have the
  400% strength. "If he doesnt crash and he lands smoothly, he wont have the 400%
  increased strength, unless he breaks in the right lab and takes the serum."
  Strength becomes SERUM-GATED and optional, not assumed.
* The prequel sits SIX MONTHS EARLIER, so "all the infected are NOT yet infected".
  Dr Chen (Japanese) has made the virus but it is "confined to section 4.5 at
  this point".
* Cast: Keisha, Emily, + a third woman whose name Tim keeps forgetting — RESOLVE
  THIS, and the male cast.
* Tim: "This will take the game on a whole entire different arc!"

Implications: the intro/cutscene system must branch; enemies are humans not
infected in this era; Jake's melee (his 400% strength punch/kick, "fatal to most
enemies") must be gated behind the serum pickup.

Melee bindings Tim settled: F punch, Z or V kick (unresolved which), C crouch,
Left-Ctrl crawl, G noclip (moved off F), R reload, U idkfa, E interact/enter car.

## 2. JAKE'S CELL — port the BEST version from Babylon (BL)

Tim: the BL cell "was the Best of any version". What made it good, to preserve:
wake up RESTRAINED by 4 metallic cuffs; STRAIN to get free; realise he feels
stronger; restraints pop one at a time; when free, read the wall terminal and
learn "subject 7 alpha complete strength enchancement procedure". A usable toilet
(colour indicates enhancement). A highly interactive terminal. Bars that BEND
with his superstrength so he can escape.

Known blocker: the BL version used a broken 22-action GLB whose XYZ did not match
the model. RESOLVED elsewhere — Jake's rig is the 44-clip merge; extend via Meshy
if more actions are needed. Do NOT resurrect the 22-action asset.

## 3. THE ELEVATOR — easter-egg destined, currently "slop-oriented"

Tim wants marble ornate columns surrounding it on whatever level it is accessed
from; "swank and pizzaz and tech shine and opulence INSIDE".

HIDDEN MODE: a floor combination on the panel unlocks FREE FLIGHT.

And the side quest behind it: THE CHOCOLATE FACTORY — explicitly "based on the
BOOK VISUALS, not the insanely craptastic movie". Tim's brief: "glassy, and the
tubes and pipes be so shiny and cool and alive and breathing and pulsing.. and
the machinery shaking as it does its thing".

## 4. CONTENT / WORLD

* BL world port strategy (decided): port the AUTHORED road ring, REGENERATE the
  buildings. Do not port BL's building placement.
* Lift B: echo_region_builders + the city generator onto main (~6-7k lines).
  Lift A proved zero API drift, so risk is low; was blocked on art availability.
* Known BL-era defects to avoid repeating: a huge white tank popping in and out
  near the city centre; interiors existed but lit badly; the elevator shaft never
  rendered right.

## 5. ENGINE DEBT (has no other home)

* TEXTURE DOUBLE-FREE in world_stream region teardown: textures created 76,
  destroyed 112 — MORE destroyed than created. Meshes and bodies balance exactly.
  GLB-backed regions share texture handles and teardown releases per-material
  rather than per-unique-handle. Same shape as the LOD double-free the mines lane
  fixed. `--test-worldstream` W5b. Pre-existing, unowned, sitting in main.
* Stream budget: ocean_base realize 107 ms vs a 33 ms budget (W4) — the
  documented monolithic-builder atomicity floor, now reachable because the GLB
  actually loads.
* `--test-vehparts` P3 fails on main (street 708 > stock 640 ticks).
* MERGE BACKLOG: 142 commits parked on 8 unmerged lanes (v003 +52, v002 +41,
  intro-composite +12, playable-build +10, playtest-launch +6, unified-launch +5,
  wetness +1, mountain-tunnels).
* LFS server 192.168.7.230:9970 unreachable; ~94 assets are pointer-only in fresh
  checkouts.
* `.remember/` is a single shared file and collides across agent worktrees (it
  already blocked a merge). Tim wants LANE-SCOPED memory.

## 6. ARCHITECTURE — decided but unbuilt

* Level Architect as its OWN EXE loading a shared engine DLL. `x3core` is already
  a separate CMake target (static today), so the seam exists. CAVEAT recorded:
  a shared DLL prevents ENGINE drift but not HOST drift — both of this session's
  worst bugs (tunnel lit headless/black interactive; assetRoot resolving by build
  layout) were host-layer. Pair the split with a rule that the editor uses the
  SHIPPING host path.
* Generators must emit editable LevelDocs; regeneration policy decided as
  PER-ENTITY PROVENANCE (see LEVELDOC_HANDOFF.md).
* AI instructions in the editor ("Generate a winding curving tunnel from Tunnel
  Entrance A to Tunnel Exit B"). Load-bearing rule: the LLM produces the SPEC,
  the deterministic generator produces the GEOMETRY. Never let model output
  become geometry directly, or captures and tests stop meaning anything.
