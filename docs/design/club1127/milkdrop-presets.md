# Club 1127 — MilkDrop Preset Notes (from the real club)

Source: Tim's own DJ-booth notes, supplied 2026-07-25. These are the presets actually run on
the real club's screen wall. Preset files live on the **real DJ booth desktop** — not on the
14900K dev box and not on `\\p13700\G`.

This file is the reference for the visualizer wall in spec #3. Treat it as canon: the goal is
to reproduce *these* looks, not to author new ones.

---

## Verbatim notes

```
142

6-24-23

R142 - Green Swirls
R146 - Good - Random Patterns and Blue Squiggly lines

R180 - Blue Spectrum analyzer - looks good
R156 - Good
R157 - Blue Silver Ring and starfield
R231 - Favorite Blue Patterns


255- GOOD 12-10-23
```

## Parsed

| Ref | Description | Notes |
|---|---|---|
| R142 | Green Swirls | |
| R146 | Random patterns + blue squiggly lines | marked "Good" |
| R156 | — | marked "Good" |
| R157 | Blue silver ring and starfield | |
| R180 | Blue spectrum analyzer | "looks good" |
| R231 | Blue patterns | **"Favorite"** |
| 255 | — | marked "GOOD", dated 12-10-23 |
| 142 | — | bare number at the top of the note, dated 6-24-23 |

Session dates recorded: **2023-06-24** and **2023-12-10**.

## Observations

**The wall was overwhelmingly blue.** Five of the seven described entries are blue —
squiggly lines, spectrum analyzer, silver ring + starfield, and the "Favorite" blue patterns.
Green Swirls (R142) is the lone outlier.

This matches the club canon independently: commit `878dde1` is
*"club1127: PURE BLUE-UV blacklights (kill the pink-violet)"*. The real room's lighting and its
visualizer wall shared one blue identity. **Any port that drifts warm or magenta is wrong**,
regardless of how good it looks in isolation.

**R231 is the priority.** It is the only one marked "Favorite" — if exactly one preset gets
ported first, it is this one.

**R180 is doubly relevant.** A blue *spectrum analyzer* is both a MilkDrop preset and the
visual Tim asked for by name ("Spectrum and visualizers like Milkdrop"). It is the natural
bridge between the now-playing data and the visualizer wall.

## Open — needed from the DJ booth desktop

The `R###` numbering is **index-local to Tim's own MilkDrop install/preset pack**; the numbers
cannot be resolved to preset names or authors without that machine. Required to proceed:

1. The preset directory itself (`.milk` files) — typically under a Winamp install at
   `Plugins\Milkdrop2\presets\`, or a standalone/projectM preset folder.
2. The pack's file listing **in sort order**, so `R142`/`R231` etc. can be mapped to filenames.

The written descriptions above are enough to confirm a match by eye once the pack is in hand
("Green Swirls", "blue silver ring and starfield" are distinctive), but the numbering alone is
not portable.

## Scope reminder

Per the merge spec §10: these get **hand-ported to native shaders**, not run through a `.milk`
interpreter. Each preset is a readable set of per-frame equations plus warp/comp shaders; the
port targets the same look, driven by the jukebox's beat grid so it runs phase-locked rather
than beat-inferred.
