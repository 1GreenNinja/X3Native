# LATE NIGHT SPEED — the car on the lift

Spec from Tim, 2026-07-26. The car on the garage-bay lift in the club is **not** a
generic prop: it is a specific car, specified to the tyre. Treat this as canon, the same
way the PA rig and the MilkDrop presets are.

## The car

**2003 Ford SVT Mustang Cobra — "Terminator" — BLACK.**

- SN-95 / New Edge body, supercharged 4.6 L DOHC V8 (Eaton M112), IRS.
- Black. Not "dark", not a tinted grey — black.

Reference dimensions (2003 SVT Cobra coupe), for proportion-correct blockout or for
sanity-checking an imported model:

| | inches | metres |
|---|---|---|
| Length | 183.2 | 4.653 |
| Width | 73.1 | 1.857 |
| Height | 52.5 | 1.334 |
| Wheelbase | 101.3 | 2.573 |

## Wheels and tyres — the part that must be right

**18" Saleen wheels**, staggered, wrapped in **Nitto NT05**:

| | Tyre | Section width | Sidewall | Overall diameter | Radius |
|---|---|---|---|---|---|
| **Front** | 275/35R18 | 275 mm | 96.25 mm | **649.7 mm** | 0.3248 m |
| **Rear** | 315/30R18 | 315 mm | 94.5 mm | **646.2 mm** | 0.3231 m |

Derivation (so a future pass can re-check rather than re-guess):
`overall Ø = wheel Ø + 2 × (section width × aspect ratio)`, wheel Ø 18" = 457.2 mm.

- Front: 457.2 + 2 × (275 × 0.35) = 457.2 + 192.5 = **649.7 mm**
- Rear:  457.2 + 2 × (315 × 0.30) = 457.2 + 189.0 = **646.2 mm**

**Note the front is 3.5 mm TALLER than the rear.** That is correct for this stagger and
is not a mistake to "fix". The visual signature is a much WIDER rear contact patch at
essentially the same rolling diameter — not a raked, big-and-little look. Getting this
wrong is the most likely way to make the car read as generic.

Nitto NT05 is a summer performance tyre: a squared-off shoulder, a directional-looking
asymmetric tread, and a modest sidewall with visible NITTO lettering. On a lift, the
sidewall and tread face the viewer at eye level, so tread and lettering carry more of the
read than the bodywork does.

## Why it matters

**LATE NIGHT SPEED is Tim's own marque** — the neon sign is over the garage bay in the
club. The lift car is the marque's centrepiece. Same rule as
[the club being a real room](club1127-is-a-real-room): the question is not "would a cool
car look good here" but "is this the actual car".

## Status — NOT IMPLEMENTED

Not built as of 2026-07-26. Open questions for whoever picks this up:

1. **Where is the lift car authored?** Not in `app/club1127.*` and not in `app/*.cpp` by
   any obvious name. It comes from the LNS garage work (`x3-lnsgarage` / `x3-lnspolish`
   lineage) folded in via `integration/unified`. Find it before designing anything.
2. **Is it a GLB or procedural?** No car GLB exists in this tree's `assets/converted_glb`
   or `assets/rigged_glb`. If it is an imported pack model, the wheels are probably not
   separable and the tyre spec cannot be honoured without replacing them.
3. **Are the wheels separate meshes?** The tyre spec is only meaningful if front and rear
   can differ. If the model has one wheel mesh instanced four times, the stagger needs
   either a per-instance scale or four separate meshes.
4. **Sourcing.** A licence-clean black 2003 Cobra is the real blocker. The armory packs
   should be checked first; note that a commercial release makes model licensing a live
   concern, exactly as it does for the MilkDrop presets.

Per `docs/design/X3_WORLD_RULES.md`: 1 unit = 1 metre, origin at the contact surface
(tyre contact patch — though on a lift the car is raised, so the lift arms carry it), and
verify with an ortho top+side render before believing any of it.
