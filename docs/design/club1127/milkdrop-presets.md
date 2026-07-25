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

## RESOLVED — the presets are the MilkDrop2077 pack

Located 2026-07-25 at:

```
C:\GameDev\OneDrive\2026\July\MilkDrop3\Milkdrop3\presets\
```

501 preset files (388 `.milk`, 113 `.milk2`). `R###` is **not** a list index — it is part of
the filename. The pack runs `R0`–`R301`. Tim's notes map one-to-one:

| Note | File | Size |
|---|---|---|
| R142 — Green Swirls | `MilkDrop2077.R142.milk` | 17.9 KB |
| R146 — blue squiggly lines | `MilkDrop2077.R146.milk` | 9.6 KB |
| R156 — "Good" | `MilkDrop2077.R156.milk` | 11.8 KB |
| R157 — blue silver ring + starfield | `MilkDrop2077.R157.milk` | 12.5 KB |
| R180 — blue spectrum analyzer | `MilkDrop2077.R180.milk` | 7.1 KB |
| **R231 — Favorite Blue Patterns** | `MilkDrop2077.R231.milk` | 16.6 KB |
| 255 — "GOOD" (12-10-23) | `MilkDrop2077.R255.milk` | 12.4 KB |

The bare `142` heading the note is almost certainly R142 too, matching the Green Swirls line
beneath it.

Descriptions corroborate the files: **R180** is `nWaveMode=7` with `wave_b=0.800` (a
blue-dominant spectrum mode); **R231** carries `fRating=1.000`, the top rating.

## ⚠ LICENSING — do not ship these presets

`LICENSE.txt` in the distribution:

```
MilkDrop 3 is free for personal use,
commercial and professional usage is NOT allowed.
No NFT usage too.
contact: https://twitter.com/MilkDrop2077
```

The bundled BSD-3-Clause covers the **BeatDrop / MilkDrop2 plugin source**. The MilkDrop3
distribution these presets ship in is **personal use only**. Therefore:

- **Never commit `.milk`/`.milk2` files to this repo.** `X3Native` is public on GitHub;
  committing personal-use-only content is redistribution.
- **Do not translate a preset's per-frame equations or warp/comp shaders into our source.**
  That is a derivative work, and it violates the clean-room provenance rule stated in
  `app/jukebox.h` ("No foreign source").

### The clean path

**Tim's written descriptions above ARE the specification.** "Green Swirls", "blue silver ring
and starfield", "blue spectrum analyzer", "favorite blue patterns" are plain-English
descriptions, authored by Tim, of his own room. Author **original** shaders to those
descriptions. The preset files remain on local disk as a visual reference to look at while
authoring — never as source to port.

This is not a compromise on fidelity: the target was always the look Tim remembers from the
real club, and the descriptions capture exactly that.

If shipping the real presets ever matters, the route is explicit permission from the author
(contact link above) — worth asking, but do not plan around it.

## Scope reminder

Per the merge spec §10: the wall is **original shaders authored to the descriptions above**,
driven by the jukebox beat grid so they run phase-locked rather than beat-inferred. A `.milk`
interpreter remains out of scope — and note that shipping one would not solve the licensing
issue, since the presets themselves are the restricted artifact.
