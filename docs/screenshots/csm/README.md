# Cascaded shadow maps (Lane 3) — verification captures

Regenerate everything here with, from the worktree root:

```
X3Engine.exe --screenshot-csm docs/screenshots/csm
```

1280x720, headless (no window). The rig is deliberately **synthetic**: an
analytic sky, one flat 600 m apron at y = 0, and a "ruler" of 9 pillars at
15 / 26 / 38 / 52 / 72 / 100 / 135 / 180 / 235 m from a fixed camera 8 m up.
SSAO / SSGI / TAA are off and haze is 0. The camera looks **across the sun
azimuth**, which is where the legacy 45 m box is tightest (looking down-sun it
stretches to ~140 m of ground and flatters itself).

Why not a dressed world: the first version of this rig ran on the streamed
terrain with a horizon ring. That scene has tens of metres of relief plus 140
concentric ring bands, and **both read as large dark ground shapes that are
visually indistinguishable from shadows**. That was caught by forcing
`sampleShadow()` to return 1.0 and re-capturing — the "shadows" were still
there, byte-identical. Any shadow-range claim made by eyeballing that scene
would have been wrong.

## Files

| File | What it shows |
|---|---|
| `ab_near_csm0/1.png` | steep framing, 15-52 m stations, `r_csm` 0 vs 1 |
| `ab_mid_csm0/1.png` | the legacy 45 m cutoff crosses the frame |
| `ab_far_csm0/1.png` | shallow framing out to the 235 m station |
| `ab_range_csm0/1.png` | **the range shot** — a 10 deg lens on exactly the 47-250 m band |
| `legacy_footprint_debug.png` | `r_csm_debug 1` with `r_csm 0`: everything **outside** the single 45 m box painted black |
| `legacy_footprint_range_debug.png` | the same, in the range framing |
| `cascades_debug.png` | `r_csm_debug 1` with `r_csm 1`: visibility stepped per cascade |
| `pan_csm1_0..5.png` | camera pan at a fixed position (texel-snap check) |
| `boundary_csm1.png` / `_noblend.png` | cascade blend band vs hard split |
| `interim_forward30_csm0.png` | `r_shadowforward 30` — the cheap interim |

## The measurement (this, not the eyeballing, is the evidence)

`legacy_footprint_range_debug.png` is the clearest single image: in the
telephoto framing **the entire visible ground is painted black**, i.e. every
pixel of the 47-250 m band is outside the legacy cascade. Today's sun casts
nothing there at all.

Quantified by re-building with `sampleShadow()` forced to `return 1.0` and
diffing against the real capture (pixels differing by more than 6/255):

| capture | pixels that are actually shadowed |
|---|---|
| `ab_range_csm0` (legacy single cascade) | **0** |
| `ab_range_csm1` (4 cascades) | **3717** |

Zero versus several thousand, in the band that matters. That is the whole
feature: the legacy box contributes literally nothing past ~45 m, cascades
cover it out to 250 m.

## Honest visual verdict

* **`legacy_footprint_*_debug.png` and `cascades_debug.png`: convincing.** They
  show the 45 m box and the cascade selection directly and unambiguously.
* **The `ab_*` A/B pairs: NOT visually convincing, and I am not going to
  pretend otherwise.** The apron carries a pre-existing depth-banded shading
  artifact (evenly spaced horizontal bands on a large flat quad viewed at a
  grazing angle). Those bands survive with sun shadowing *completely disabled*,
  so they are not shadows — but they are the same dark blue-grey as a shadow and
  they dominate the frame. The real 3717-pixel difference is buried in them.
  Chasing it down cost this lane a long time; SSAO, SSGI, TAA, the texture and
  the haze were each eliminated in turn and the bands remained. It is an
  artifact of the flat test surface, it is identical in both A and B, and it
  does not affect the measurement — but it does mean **the A/B images should be
  read alongside the numbers, not instead of them.**
* **The pan sequence is inconclusive as an image comparison.** Consecutive
  frames differ mostly because the camera moved, so a pixel diff cannot separate
  "shadow edge crawled" from "everything shifted". The rigorous stability proof
  is `--test-csm` C3, which asserts on the snapped origin directly: a sub-texel
  camera move must not change it at all, and a one-texel move must move it by
  exactly one texel. That test fails on the unsnapped implementation (C7).
* **The blend band is real but subtle** (`boundary_csm1` vs `_noblend` differ by
  ~1000 px). With 4 cascades over 250 m the splits land at 17.5 / 39.7 / 86.6 m
  and adjacent cascades already agree closely, so the band is a soft gradient
  rather than a dramatic effect. That is the intended outcome — a *visible*
  blend band would mean the cascades disagreed badly.

## Frame cost

From the run log (`--screenshot-csm`), mean GPU frame time over 80 settled
frames:

```
r_csm 0 (1 cascade)  = 0.192 ms
r_csm 1 (4 cascades) = 0.213 ms   delta +0.021 ms (+10.7%)
```

**Read this number carefully.** This rig submits only 2 draw groups, so it
measures the *fixed* per-cascade overhead (one extra begin/endRendering, one
push constant, one clear) and almost none of the geometry cost. The shadow pass
re-submits every opaque draw group once per cascade, so on a heavy scene the
cost scales with draw count, not with this figure. Budget accordingly.
