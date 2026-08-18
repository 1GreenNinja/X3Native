# HANDOFF — W-TOWN v2 (Lane 4): the asset swap and the two open gates

*Branch `lane4/town-asset-swap`, worktree `D:\GameDev\X3N-town2`, three commits
on top of `b10e5ec9` (the merged W-TOWN). **Not pushed** — the session lead
merges. Full detail lives in `docs/design/TOWN_MANIFEST.md`; this is the short
version.*

| | |
|---|---|
| `fe361053` | the kit swap — the houses are houses now |
| `e46350eb` | real windows, a pavement that exists, townspeople who are people |
| `38f8a46a` | the dusk camera stands across the street, not on the guardrail |

---

## Where to start reading

`docs/design/TOWN_MANIFEST.md` — sections **2** (the pack and its two traps),
**3** (the measured asset table), **6** (the pedestrians), **7** (the gate),
**7b** (the fps numbers), **8** (what is still open).

## What I was asked to do, and what happened

**1. The asset swap.** Done. The HouseForge kit was authored as collapsed ruins;
I confirmed it with my own eyes before touching anything. The replacement is the
licensed `Complete Racing Game URP All in One` pack's `Red_House/House_1..4`,
its 7.16 m highway lamp, its billboards and its picket fence — one pack, one
register, authored for a driving game. Eight facades from four shells in two
real photographic paints. 6.5 MB, down from ~120 MB.

**The scout's building recommendation was wrong on scale and I did not follow
it.** `Medium_Building_*` are 45–76 m office blocks, `Tower_*` are 464–618 m
skyscrapers, and `Buildings _Night/Building_1..7` are **empty meshes, 0×0×0**.
Their wall material has no texture anywhere in the pack either. The scout's
*register* call was right; nobody had measured the geometry. Recorded in
manifest section 2 so the next lane does not re-walk it.

**2. The two open gates.** Both closed, one of them amber — see below.

## What I found that was not in the brief

* **A monster was walking Main Street.** The pedestrians came from
  `CrowdSkin::defaultRigs()`, which is cast for the club scene: `marcus_webb` is
  a clawed green-veined mutant and `chief_martinez` is a SWAT operator. Two of
  three townspeople. Replaced with six civilians built by
  `tools/town_people.py` from `City People FREE Samples` — found with the
  owner's own index tool, among the ~700 packages never extracted.
  **crowd_skin is untouched**; the town casts its own roster.
* **Every window in the town burned at noon.** `setNight` defaulted to 1 and
  nothing ever called it. Now defaults to day, driven by `setNightFromSun`.
* **The sidewalk was a mud bank.** `kSidewalkLatM` 16.4 m sat *outside* the
  road's 15.63 m carve, so the loop ran on raw batter. Now 13.6 m, on the apron.
* **`Lot::lat` could put a building in the road.** It meant distance to the bbox
  *centre*; it now means the front-face setback and the placer adds each asset's
  measured front support.
* **21 dead HouseForge entries** were still in `assets/manifest.json` — 120 MB
  every checkout fetched for nothing. Dropped.

## Gates

| gate | result |
|---|---|
| build | **green** (clean reconfigure + rebuild after a sibling deleted `build/` mid-session) |
| boot | **0 `[ERROR]`** |
| `--test-roadnetwork` | **58 / 58** |
| `--test-terraincorridor` | **16 / 16** |
| `--test-tunnelmouth` | **8 / 8** |
| `--test-riverbridge` | **9 / 9** |
| `tools/town_assets.py verify` | **GREEN**, 12 assets |
| `tools/town_people.py verify` | **GREEN**, 6 rigs, `idle=0 walk=1 run=2` per rig at boot |
| eyes-on | five full-res captures in `docs/screenshots/town/`, **read by me**, from a clean rebuild |
| **fps** | **AMBER — two of three cameras below 90 %.** Stated in full below. |

### The fps gate, told straight

Quiet GPU, `X3_TOWN=0` vs `=1`, identical `--shot-cam` poses, 2 reps, 60 settled
frames each:

| camera | off | on | Δ | % of baseline | Δ as % of a 165 fps frame |
|---|---|---|---|---|---|
| main street | 0.719 ms / 1391 fps | 0.829 ms / 1206 fps | +0.110 ms | **86.7 %** | 1.8 % |
| square at dusk | 2.092 ms / 478 fps | 2.161 ms / 463 fps | +0.069 ms | **96.8 %** | 1.1 % |
| from the valley | 0.961 ms / 1041 fps | 1.146 ms / 873 fps | +0.185 ms | **83.9 %** | 3.1 % |

Two readings miss a literal ≥ 90 %. The absolute cost is 0.07–0.19 ms; the
baselines are 0.7–2.1 ms frames, so a tenth of a millisecond *is* 13 % at
1391 fps. Against the owner's 165 fps benchmark (6.06 ms) the town costs
**1.1–3.1 % of the frame budget**. The lever is **draw calls, not triangles**:
the valley shot adds 277 k tris (+31 %) but **+281 draws**, because the EnvArt
overlay issues one draw per instance. Instancing or a merged static batch per
GLB is the fix if it ever matters.

## Still open (manifest section 8)

1. The fps ratio above, and its known lever.
2. **Style seam** — low-poly palette-shaded civilians against photographic
   clapboard. Deliberate trade; a photoreal civilian pack would close it.
3. **No commercial building.** The town is residential. The racing pack's
   `Shop.jpg` / `Shop_2.jpg` are genuinely excellent lit-storefront photographs
   with no right-sized shell to sit on.
4. **6 of 25 authored lots are rejected** by the overlap/slope ledger.
5. Lit panes sit on the measured centre of a real window; on a couple of facades
   they read slightly proud of the frame. Invisible unlit.

## For Lane 7 (W-MAP)

`grep -rn MapPoi` still finds nothing outside the plan. When it lands:

```
MapPoi{ "Mountain Town", x = Town::centerX(), z = Town::centerZ(), icon = town }
```

Read the accessors, do not copy the literals — the spur is hill-climbed at boot
and the centre moves with it. Current value: **(−20.6, 15.7, 4817.5)**.

## New tooling (all committed, all documented in their own headers)

| tool | what it does |
|---|---|
| `tools/town_assets.py` | rewritten: bakes the building kit, injects textures **by material name** (the pack has no `.mat` files at all), transcodes TIFF→JPEG (stb_image cannot read TIFF), `report` prints the measured `kAssets[]` block, `verify` fails on placeholders / unreadable mimes / untextured near-metal |
| `tools/town_people.py` | builds the six civilian rigs straight out of the `.unitypackage`, merges clips as `Idle`/`Walk`/`Run` via the pre-existing `glb-merge-anims.mjs`, `verify` asserts clips + skin + texture |
| `tools/glb_contact_sheet.py` | dependency-free software rasteriser — renders a textured 3/4 view of every GLB in a directory into one sheet. No Blender, no GPU, seconds for a kit. **This is what caught both the ruined houses and the mutant**; the eye gate is only as good as how cheap it is to look. |

## Housekeeping

* Assets are **store-served** (`asset_store.py publish` done for both the Town
  kit and the CityPerson rigs); only `assets/manifest.json` is committed. No GLB
  bytes in git (gotcha 2.5).
* A sibling lane deleted `build/` mid-session; reconfigure + rebuild was clean
  and every suite above ran on that rebuilt binary.
* Two `shots_wmap/*.png` were touched by a boot fetch and restored with
  `git checkout` — they belong to the map lane, not this one.
