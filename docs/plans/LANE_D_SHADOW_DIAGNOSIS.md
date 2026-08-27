# LANE D — Shadow Fade Under Motion: diagnosis plan

*Owner: "the fading shadow of the car and Jake when moving" — shadows wash out at
speed, recover when still. Measure before touching anything (NO_SLOP rule 9).
Report the convicted pass BEFORE fixing it.*

## Suspects (in order)
1. **TAA accumulating the shadowed lit result.** This engine already won the
   object-ghosting war with per-object motion vectors (`r_velocity`). But the
   GROUND under a moving caster has no motion vector — the ground doesn't move —
   so its TAA history says "lit" and the blend washes the moving shadow out.
   (`vk_gi_rt.cpp` confirms: without the velocity pre-pass, TAA runs *camera-only*
   reprojection; with it, per-object MVs feed the history.)
2. **CSM cascade update cadence** — a fast car outruns a reduced-rate cascade
   (`Csm.cpp` fitting, `r_csm_*`).
3. **Soft-shadow temporal filtering** with its own history lacking caster-motion
   rejection.
4. **SSAO / contact darkening** mistaken for "the shadow" — must verify WHICH term
   fades by toggling passes.

## Measurement method
- **Capture pairs**: same framing, stationary vs at-speed, via the existing
  `--shot-drive` staging (driver-POV, `app/app_run.cpp:5681`).
- **Quantify**: shadow-luminance delta at a fixed ground sample between the two
  captures (script the region read; don't eyeball).
- **Bisect cvars** (all live): `r_taa`, `r_csm`, `r_ssao`, `r_shadowforward`,
  `r_csm_debug`. Toggle each pass off and re-measure the delta. The pass whose
  toggle makes the fade DISAPPEAR is the convicted pass.

## Conviction rule
Report which pass fades **before** writing any fix. Fix only that pass; do not
retune lighting globally. Any other defect found (bias, peter-panning) goes in a
LIGHTING NOTES section, not the fix. fps within 2%.

## Status
- Worktree created, mainline merged, pipeline surveyed (TAA/CSM/SSAO cvars located).
- **Blocked on capture** — engine launches are held while sibling lanes / the
  owner have `X3Engine.exe` running (Vulkan contention, NO_SLOP operational rule).
- Next green step: first capture pair + luminance delta, once a launch slot frees.
