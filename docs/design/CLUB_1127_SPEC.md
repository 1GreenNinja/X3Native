# Club 1127 — "THE DEEP" — Architectural Spec (Merged)

Source of truth = Tim's original Babylon module
`Q3Engine/js/Level_Club1127.js` (586 lines, ASCII floor-plan + real dims),
PLUS Tim's grand extensions (2026-07 architecture pass). The native C++/Vulkan
port lives in `app/club1127.cpp` + `club1127.h`, built at world Y = -200 and
reached via the elevator's 1127 disco descent (or `--world club` / `--screenshot-crowd`).

Philosophy (Tim): "AS LONG AS they are there, I can fill in the blanks." Every
feature ROUGHED IN (blockout geometry + a clear material) and PRESENT beats any
single feature over-polished. Coverage > polish.

---

## ROOM SHELL

| | Babylon original | Native (before) | **THIS pass (extended)** |
|---|---|---|---|
| Width (X) | 43 ft (13.1 m) | 50 ft (15.24 m) | **60 ft (18.29 m)** |
| Length (Z) | 100 ft (30.48 m) | 100 ft (30.48 m) | 100 ft (30.48 m) |
| Ceiling (Y) | 20 ft (6.1 m) | 30 ft (9.14 m) | **42 ft (12.80 m)** |

The room is WIDENED + made TALL so a **suspended catwalk** at ~21 ft has real
headroom above it. Sealed solid shell (floor, ceiling, 4 walls); elevator entrance
gap on the EAST wall near the SE corner; engine-room gap on the NORTH wall.

## FEATURE LEDGER (Babylon-original vs Tim-extension)

1. **Larger tall room** — *extension*. 60x100x42 ft, sealed.
2. **85" OLED screens, 4 PER WALL** — *extension* (Babylon had a single 85" + 65").
   16 emissive EQ/visualizer glass panels, 4 evenly spaced on every wall, per-texel
   emissive (glow where the EQ is bright).
3. **U-shaped granite bar, centered at ONE END** — *extension* (Babylon had a
   west-wall ground bar). Dark polished granite horseshoe on the SOUTH end, reflective
   PBR, back-bar glowing bottle wall behind it, stools around the U.
4. **Suspended catwalk + round half-moon private areas** — *extension, THE signature*.
   Mezzanine walkway ring at ~21 ft above the floor (stairs up), railed, lined with
   semicircular (half-moon) private booth alcoves overlooking the dancers.
5. **Lounge (2nd story) + suspended DJ booth + aerial bar** — *Babylon original*.
   2-story engine room on the north: lounge floor at 15 ft, railing, 12-step stair;
   suspended DJ booth (turntables, mixer, 2 OLED, keypad door); neon aerial bar.
6. **More subwoofers + satellite speakers** — *extension*. Expanded PA: 4 SVS corner
   subs, 16 JBL JRX line-array, 8 JBL 18" subs (was 4), 24 surrounds (was 16),
   2 DJ tower stacks.
7. **VIP couch, 12-step stair, railings** — *Babylon original*. SW VIP couch, SE
   couches + end table, engine-room stair + rails.

## LIGHTING

Blue-UV blacklights (fixed length/height — see below), 4 beat-synced ceiling
moving heads projecting down, orbiting ring/spot lights, mirror-ball sparkle
cluster, corner-sub pulse, dance-floor key + perimeter fills. **64 point-light
cap** is hard: the bigger room + catwalk pressure it — new catwalk/booth/OLED
work is EMISSIVE (bloom, not point lights) to stay under. Reported in the build log.

### Blacklight fix (Tim, 2026-07-17 screenshot review)
Tubes were "half as long as they should be, and too low to the ground."
- Length: 4 ft (1.22 m) -> **8 ft (2.44 m)** — doubled.
- Center height: 5 ft (1.5 m) -> **~8.5 ft (2.6 m)** — raised (floor-to-mid-wall).
- Kept: pure blue-UV color, dim signature bloom, 12 ft spacing, per-tube cast light.

## VALIDATE
- `--test-club` green (dims/spawn/fixture census updated to the new room).
- `--screenshot-crowd` renders; VMA `allocationCount=0`.
- Screenshots (docs/screenshots/club1127/): U-bar, 4-per-wall OLEDs, catwalk +
  half-moon booths (below & from above), DJ booth, lounge, wide, blacklight tube.
