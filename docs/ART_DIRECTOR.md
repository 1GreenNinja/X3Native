# The Art Director pass

**Why this exists.** `docs/NO_SLOP.md` has eleven rules and **nine are correctness
rules** — grep first, the contact law, paired values, unset flags, events not
polling, measure don't vibe, write the receipt. Only #2 (*eyes on against a real
reference*) and #3 (*no untextured stand-ins*) concern **look**, and both are
unfalsifiable prose.

That is exactly how W-UNDERRIVER v2 shipped. It obeyed rule 11 — it *cites* rule 11
in its own source — measured everything, and wrote an honest receipt with real
numbers. And it looks like one grey tube, because every round went to correctness:
the water was the club's grotto ribbon, then came back as "crumpled chrome foil";
the beach apron was built with its winding one line off so every triangle faced the
floor and was culled; a tree was growing in the cave. The cinematic instruction was
never reached. **Nobody skipped the brief — the lane never got past making it work.**

And the rock reads as one stretched texture for a reason worth quoting:

> `cv_rock_flume` turned out to be **A PICTORIAL ILLUSTRATION OF A WATERFALL**, not
> a tiling rock — *"I'd checked it had bytes, which is not what check-what-is-published means."*

The fallback was `terrain_rock_grey` cobble, a **terrain** texture, applied to walls,
floor and ceiling alike. The material was damage control, not art direction.

---

## The pass, in order

The Art Director runs **before the lane closes**, and its output is a **material
assignment committed to the code** — never a report. A report becomes another
document that gets cited and not followed.

### 1. Name every distinct surface in the scene
Not "the cave" — *wet limestone wall above the waterline*, *dry rubble floor*,
*ceiling strata*, *rock beach at the waterline*. If two surfaces get the same
material at the same scale, the scene reads as a tube. **That is the failure.**

### 2. Shop the library. It exists now.
`D:\UnityPacks` — **914 packs, 442,271 files**, extracted 2026-08-17. Before that
date an art director could not exist here; there was nothing to shop in and lanes
grabbed whatever was already mounted.

```
python tools/unitypackage_index.py --search limestone cavern wet
python tools/unitypackage_extract.py --list
```

### 3. VALIDATE the candidate before assigning it
This is the step whose absence produced the waterfall-illustration bug. Having bytes
is not being a texture. For each candidate:

- **Is it a tiling SURFACE or a picture OF something?** View it. A pictorial
  illustration has a composition, a horizon, a subject.
- **Does it tile?** Compare the left column against the right, top row against
  bottom. A seam that jumps is a decal, not a material.
- **Texel density** against its neighbours. One set at 4x the scale of the wall it
  abuts reads as a mismatch even when both are beautiful.
- **Normal map sane?** `python tools/audit_normal_maps.py --paths <normal.png>` —
  this already caught a blue channel decoding to z = -0.87 that every eye missed.

### 4. Light it, then measure it
One dominant key with real falloff. RiftHub's whole power is contrast: deep blacks,
a bright source, and greebles catching rim light.

```
python tools/audit_look.py --gate <capture>.png
```

### 5. Eyes on, full-res, beside the reference
`docs/screenshots/rifthub_r10/R10B_ibl_hero.png`. The gate is **necessary, not
sufficient** — it proves a frame is not flat; it cannot prove a frame is good.

---

## The gate

`tools/audit_look.py` measures two things, because those are the two that
**discriminated when tested**:

| shot | p99 | range | |
|---|---|---|---|
| RiftHub hero | 0.798 | 0.791 | PASS — the bar |
| RiftHub gate | 0.787 | 0.780 | PASS |
| tunnel approach | 0.924 | 0.708 | PASS |
| elevator cab | 0.816 | 0.664 | FAIL — no range |
| cavern wide | 0.564 | 0.525 | FAIL — not lit, no range |
| cavern great hall | 0.502 | 0.476 | FAIL — not lit, no range |

**Two metrics were tried and thrown away, with their numbers**, because shipping a
check that does not discriminate is its own kind of slop:

- **Detail** (mean |Laplacian|) — the flat cavern scores **0.0219** against
  RiftHub's **0.0192**. A noisy tiled rock makes as much high-frequency energy as
  designed greebles. It cannot tell *busy* from *detailed*.
- **Hue variety** — cavern 3 buckets, RiftHub 2. Colour count says nothing about
  whether colour was directed.

The first cut also measured in **linear** luminance, where ±0.1 spans most of what
the eye calls dark — and duly failed RiftHub itself. Perceptual space, calibrated
against a known-good frame and a known-flat one. **Calibrate a gate against both
ends or it is decoration.**

---

## The one rule that carries the rest

A frame is not finished because it is **correct**. Correct is where the lane starts.
