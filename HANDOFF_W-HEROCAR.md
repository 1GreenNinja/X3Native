# W-HEROCAR handoff — 2026-08-17

Lane: turn the never-extracted **HDRP GBX COUPE Free Update** pack into the
player's black hero car, and check the two vehicle packs the traffic lane
wanted but could not reach.

---

## 1. THE HEADLINE, STATED PLAINLY

The owner asked for **"acura NSX-S-R 2022 A spec cars, in Black"**.

**There is still no NSX.** The GBX COUPE is a **front-engine GT coupe**, not a
mid-engine supercar — `Engine_MD.FBX` sits under the bonnet at z +0.79…+2.22,
and the car is 4.72 m long on a 2.88 m wheelbase against a real NSX's 4.54 m /
2.63 m. It is the best hero-car mesh the 914-package library actually holds,
and it is a big step up from the Skyline the traffic lane had to substitute —
but it is not the car that was asked for, and nothing downstream should say it
is. Full asset sheet: **`docs/design/VEHICLE_GBX_COUPE.md`**.

## 2. WHAT SHIPPED

**`assets/converted_glb/Vehicles/GBX_Coupe.glb`** — store-served, 13.25 MB,
243,182 tris, 21 materials, 12 real pack textures. Black clearcoat paint,
satin-black rocker, silver rims, smoked glass, emissive lamps. Origin on the
contact plane (y = 0.001), nose = +Z, spinnable `Wheel_FL/FR/RL/RR` nodes.
Built by **`tools/build_gbx_hero_car.py`** (idempotent, re-runnable).

**`app/car_roster.h`** — the per-car `CarSpec` table. Wheel stations,
ride-height drop, body-widen, chassis box and mass now live once, per car,
next to the GLB they were measured from. This is what unblocks a second hero
car at all: those numbers used to be literals in `app/vehicle.cpp`, so any
other GLB inherited CTR's 2.274 m wheelbase (the "broken red sedan" the E46
attempt produced). `DriveDemo::setSpec()` applies a spec at build time.

**`--car <id>`** — `gbx` (default) or `ctr`. Honoured by `--world drive`,
`--world tunnel` (GBX also leads the garage fleet row) and `--screenshot-car`.
Unknown ids are rejected loudly with the valid list.

**Gate**: `--test-vehicle` → **49 passed, 0 failed** (was 37/0 — the 37
pre-existing checks are untouched, and CTR measures 2.274002 m against spec
2.274000, i.e. the refactor moved nothing). The 12 new checks build EVERY
roster entry and read the stations back out of the live Jolt wheel poses.

**Eyes-on**: `docs/screenshots/vehicles/gbx_coupe/` — the raw 3.67 M-tri source
(the render that established "front-engine GT coupe"), plus front-3/4,
rear-3/4, side and ortho of the shipped GLB.

## 3. WHAT IS *NOT* DONE — say this before someone discovers it

**The handling model is not retuned for the GBX.** `car_roster.h` carries the
GEOMETRY; the grip curves, CoM offset, anti-roll rates and torque map in
`app/vehicle.cpp` are still the CTR numbers with their measured
skidpad/slalom/curb receipts. The GBX is ~500 kg heavier on a 0.6 m longer
wheelbase, so it will feel like a CTR that understeers into slow corners until
someone re-runs those measurements against it. That is a physics-lane job and
the spec table is the seam for it.

**No cabin.** LOD0 is exterior + wheels + lights with opaque smoked glass (the
CTR precedent). The pack DOES ship a full interior — `Interior_MD/01/02/03`,
`Seat_MD`, `Door_Interior`, plus leather/suede/fabric textures — which is the
input for a cockpit or dealership (task #42) pass. Add the groups to
`GROUPS` in the builder and switch `Glass` to a tinted blend.

**`app/world_cars.cpp` still hard-codes CTR** for its parked/enterable
showcase. Left alone deliberately: that lane's paint-tint machinery is built
around CTR and it is not the player's hero-car path.

---

## 4. BONUS — the police / tow packs the traffic lane could not reach

### 4a. The real unblocker: the extractor was writing HALF a pack

`tools/unitypackage_extract.py` wrote only the asset bytes and **dropped every
`asset.meta`**. Unity materials reference their textures by GUID and nothing
else, and the GUID→file map lives in those metas — so a freshly-extracted pack
could not resolve a single texture slot or mesh reference, and
`convert_unity_pack.py` / `unity_scene_to_layout.py` came out GREY. (The GBX
hero car had to fall back to matching materials by NODE NAME because of this.)

Fixed: metas are written by default (`--no-meta` restores the old behaviour),
late-arriving metas are handled — the tar can order the three members of a guid
directory any way it likes, and the old flush would have dropped them — plus a
`--guid-map <json>` that dumps `{guid: project path}` for the WHOLE package
including filtered-out files. **Proven end to end**: the HEAVY POLICE CAR
prefab's 35 GUIDs now resolve 35/35, and `convert_unity_pack.py` injected real
textures into a freshly-extracted pack. This applies to all ~700 packs that
had never been extracted.

### 4b. HEAVY POLICE CAR — extracted, high quality, NOT a drop-in, and not a patrol car

`\\p13700\G\Assets\HEAVY POLICE CAR\` (148 MB, 137 files + metas + guids.json).
Unity Standard PBR at 2–4K (`_AlbedoTransparency` / `_Normal` /
`_MetallicSmoothness` / `_AO` / `_Emission`), one folder per part, 19 modular
FBXs and a `PREFABS/POLICE CAR.prefab` with 58 GameObjects / 41 mesh renderers.

Two things the traffic lane needs to know:
* **It is a heavy 6-wheel police TRUCK** (`WheelFrontLeft/Right` +
  `WheelRearLeft/Right (1)` and `(2)`), riot/SWAT shaped — not the patrol
  sedan that today's E30-in-white stands in for.
* **It needs a real prefab composer.** Each FBX is a SHEET of part variants
  (`PoliceCarBumpers_low.fbx` is 11 m of bumpers side by side), so the prefab
  transforms are the assembly. I wrote a throwaway composer, placed 40 of 41
  renderers, **rendered it, and it came out scattered** — the transform chain
  through the nested stripped-prefab instances (`m_Modification` overrides) is
  not handled. Saying so rather than claiming "usable": it is a real lane's
  work, and `tools/unity_scene_to_layout.py` is where that work belongs now
  that the GUIDs resolve.

### 4c. Tow truck — take the FLATBED, not the pack that was named

* `Low Poly 3D Garbage & Tow Trucks` (Innovana) — extracted, complete and
  textured, blue/red tow truck + teal garbage truck, single mesh each. But the
  style is **stylised cartoon low-poly** and the scale is ~2× (11.0 m long).
  Next to a photoreal GT coupe it is a style clash. Technically fine, wrong
  look.
* **`Lowpoly Flatbed Truck` (SpawnCampGames, 0.6 MB) is the better tow.** Now
  extracted to `\\p13700\G\Assets\Lowpoly Flatbed Truck\`. 12,168 tris; a
  white/orange **flatbed recovery truck**; materials already named
  `Tire / Chrome / MatteBlack / BaseColor / Windows / Headlights / Amber /
  Lens / Strap / BedCover` — `tools/convert_car_glb.py`'s substring table
  covers nearly all of them as-is; **emissive Amber beacons** for a recovery
  vehicle; wheel nodes named `FrontLeft/FrontRight/RearLeft/RearRight`; and a
  **`VanityPlate` material with its own texture slot**, which is a ready-made
  home for the owner's TOWBOOK wordmark. Needs a ~0.7 uniform scale
  (11.3 m → 7.9 m) and a lift-bed pivot if the bed should animate.

### 4d. Patrol car alternative — marginal, judge for yourself

`Mobile Optimize-Free Low Poly Cars` (1.1 MB, now extracted) → `Police Car
N_4.fbx`: 3,471 tris (the traffic roster averages ~9 k), correct 5.0 m scale,
roof light bar, **separate `FR/FL/BR/BL Tire` nodes**, and it converts with
textures now. But the pack uses a palette atlas and the car comes out **plain
white with a light bar** — which is essentially the E30-in-patrol-white the
lane already ships. Cheaper and better-shaped, not obviously better-looking.
The same pack also carries a `Military Vehicle_3` armoured POLICE van
(5,630 tris) that has no equivalent in the fleet today.

`Low Poly 3D Vehicles 6 Cars Pack` was checked and **rejected on eyes-on**: its
"Standard/Super Police Car" meshes are 1.6 m toys sitting on a baked display
podium.

---

## 5. FILES

| | |
|---|---|
| `tools/build_gbx_hero_car.py` | the hero-car builder (re-runnable) |
| `app/car_roster.h` | the CarSpec table |
| `docs/design/VEHICLE_GBX_COUPE.md` | asset sheet + facing/origin note |
| `docs/screenshots/vehicles/gbx_coupe/` | the eyes-on receipts |
| `tools/unitypackage_extract.py` | now writes `.meta` + `--guid-map` |
| `assets/manifest.json` | the GLB's sha (bytes are store-served) |

Nothing pushed — the session lead merges. The catalog
(`docs/design/ASSET_CATALOG.json`, 914 packages) was rebuilt and is gitignored,
as it should be.
