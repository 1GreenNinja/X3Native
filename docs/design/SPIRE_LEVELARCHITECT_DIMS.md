# Spire — authoritative LevelArchitect dimensions ("Escape From Lab Zero")

> **DIAGNOSIS:** The native Spire compresses **every** floor into one tiny shared plate — `kFloors[]` in `app/level1.cpp` makes all 8 floors **24 × 16 m** (`x0=0, x1=24, zHalf=8`, stacked 5 m apart in +Y). The authoritative Babylon **LevelArchitect** Floor 1 is **~75 × 43 m** (≈ 5× the native plate area). Floors 2–7 have **no authored geometry in the LevelArchitect source at all** (see §"What v10.9 actually contains") — only names. So: re-lay Floor 1 (= native **B1+F1**) to its real ~75×43 m footprint from the data below; Floors 2–7 must be authored fresh (the native plates are placeholders, not transcriptions of lost data).

---

## What v10.9 actually contains (source of truth)

**Source folder:** `G:\GameDev\LevelArchitectFullV10.9\` (note the capital `V`; the task's `LevelArchitectFull10.9` path does not exist).
**Authoritative file:** `G:\GameDev\LevelArchitectFullV10.9\js\Config.js`
- `LA.VERSION = 10.9` / `LA.VERSION_TAG = 'v10.9-facility'` (line 9–10).
- `LA.FLOOR1_DEFAULT` — Floor-1 room array (lines **133–163**).
- `LA.FLOOR1_DOORS` — Floor-1 door/connection array (lines **165–173**).
- `LA.FLOOR_NAMES` — names for floors 1–7 (lines **16–19**).

**Critical fact — only Floor 1 is authored.** `G:\GameDev\LevelArchitectFullV10.9\js\App.js` (lines **534–546**) initializes floors like this:

```js
for (var f = 1; f <= 7; f++) {
  var saved = loadFloorFromStorage(f);          // browser localStorage: key esclab48_f<N>_v10
  if (saved) { ... S.floorData[f] = saved; }
  else if (f === 1) {
    S.floorData[f] = {rooms: JSON.parse(JSON.stringify(LA.FLOOR1_DEFAULT)),
                      doors: JSON.parse(JSON.stringify(LA.FLOOR1_DOORS)), entities: [], triggers: []};
  } else {
    S.floorData[f] = {rooms: [], doors: [], entities: [], triggers: []};   // EMPTY
  }
}
```

So **Floors 2–7 ship empty** in the editor and are only ever populated by the level designer at runtime, persisted to browser **`localStorage`** (`LA.STORAGE_PREFIX='esclab48_f'`, `LA.STORAGE_SUFFIX='_v10'` → keys like `esclab48_f2_v10`). That per-machine browser data is **not present in the extracted folder** — the folder contains only JS source (no `.json`/`.la`/`.proj`/level files exist anywhere under it).

**No other area definitions exist.** A full scan of the v10.9 JS (excluding the `lib/babylon` engine) found **no** surface / Act-2 / desert / cave-level / Floor-4.5 / sub-level room arrays — the only baked level data anywhere is `FLOOR1_DEFAULT` + `FLOOR1_DOORS`. The Floor-1 data already includes the **stairs → Crystal Cavern → Side Grotto** cave bridge (rooms 25–28) as its built-in Act-1→caves transition.

### Floor names (`LA.FLOOR_NAMES`, Config.js 16–19)
| Floor | Name (v10.9) | Authored geometry in source? |
|------|---------------|------------------------------|
| 1 | Detention Level | **Yes** — 29 rooms, 38 doors |
| 2 | Medical Bay | No (empty) |
| 3 | Genetics Lab | No (empty) |
| 4 | Cybernetics Wing | No (empty) |
| 5 | Drone Station | No (empty) |
| 6 | Salvari Level | No (empty) |
| 7 | Executive Suite | No (empty) |

> Note: the **LevelArchitect** floor names (Medical Bay / Genetics Lab / Cybernetics Wing / Drone Station / Salvari Level / Executive Suite) differ from the **native** Spire's enum (`app/level1.h` `L1Floor`: Atrium / Medical wards / Labs / Offices / Synth bay / Executive / Rooftop). Reconcile names when re-laying, but there is **no LevelArchitect room data** to transcribe for these — they must be authored.

---

## Units / scale

**1 world unit = 1 meter.** Confirmed by the walk-mode player capsule and Babylon physics:
- `WalkMode.js:740` — player `ellipsoid = (0.4, 0.85, 0.4)` → capsule ≈ **1.7 m tall** (eye/exit height set to `+1.7`, `WalkMode.js:1175`).
- `WalkMode.js:199` — `scene.gravity = (0, -9.81, 0)` (m/s², real-world).
- Door openings in walk-mode geometry ≈ standard human door width; cell ceilings 3.5–4 m.

Matches the native `docs/CONVENTIONS.md`: **meters**, right-handed, **+X right, +Y up, −Z forward** — same handedness as Babylon, so dimensions transcribe directly (room **center** `(x,y,z)` + **half-extents** `(w/2, h/2, d/2)`).

**Coordinate model (LevelArchitect):** each room is an axis-aligned box centered at `(x,y,z)` with full extents `w` (along X) × `h` (along Y) × `d` (along Z). `y` is the **floor level** of that room (rooms with the same `y` are coplanar; the floor mesh sits at the room's base). Floor 1 ground is `y=0`; the stairs step down to `y=-2.5` and the caves sit at `y=-5`.

---

## FLOOR 1 — "Detention Level" (the only authored floor)

### Footprint
- **X span: −20 → +55 ≈ 75 m wide. Z span: −36 → +7 ≈ 43 m deep.**
- Main detention block (excluding the eastern cave run) is roughly **X −20→+18 (~38 m) × Z −36→+7 (~43 m)**; the **stairs→cave arm** extends east to **x=+55** (Side Grotto), giving the full **~75 × 43 m** bounding footprint.
- Cell ceilings 3.5–4.0 m; hallways 3–4 m wide (Main Hallway 26 m long, Cell Block B Hallway 32 m long); Crystal Cavern 8 m tall × 18×16 m.
- **Y levels:** ground `y=0`; Descending Stairs `y=-2.5` (`stairDown:0:-5`); Cave Tunnel / Crystal Cavern / Side Grotto `y=-5`.

### Rooms (index · name · type · center x,y,z · W×H×D · flag)
| # | Room | Type | x,y,z | W×H×D | f |
|---|------|------|-------|-------|---|
| 0 | Jake's Cell | Jake Cell | 0,0,0 | 7×4×6 | bendable |
| 1 | Cell 2 (Abandoned) | Cell | 0,0,−8 | 6×3.5×5 | |
| 2 | Cell 3 (Failed Exp) | Cell (Monster) | 0,0,−15 | 6×3.5×5 | monster |
| 3 | Cell 4 (Skeleton) | Cell | 0,0,−22 | 6×3.5×5 | |
| 4 | Main Hallway | Hallway | 5.5,0,−12 | 3×3.5×26 | |
| 5 | Guard Station | Guard Station | 11,0,−2 | 5×3.5×5 | |
| 6 | Storage | Storage | 11,0,−9 | 5×3.5×5 | |
| 7 | Medical Bay | Medical Bay | 11,0,−16 | 5×3.5×5 | |
| 8 | Armory | Armory | 11,0,−23 | 5×3.5×5 | |
| 9 | Elevator Lobby | Elevator Lobby | 5.5,0,−27 | 5×4×4 | |
| 10 | Adjacent Cell | Adjacent Cell | 5.5,0,5 | 5×3.5×4 | |
| 11 | Old Armory | Old Armory | −1,0,7 | 7×3.5×5 | |
| 12 | Creepy Passage | Creepy Passage | 16,0,−2 | 4×3×3 | |
| 13 | Cell 5 (Vacated) | Cell | −20,0,−4 | 6×3.5×5 | |
| 14 | Cell 6 (Infected) | Cell (Monster) | −20,0,−11 | 6×3.5×5 | monster |
| 15 | Cell 7 (Dead Guard) | Cell | −20,0,−18 | 6×3.5×5 | |
| 16 | Cell 8 (Containment) | Cell | −20,0,−25 | 6×3.5×5 | |
| 17 | Cell 9 (Collapsed) | Cell | −20,0,−32 | 6×3.5×5 | |
| 18 | Cell 10 (Sarah's - Empty) | Cell | −7,0,−4 | 6×3.5×5 | npc |
| 19 | Cell 11 (Feral) | Cell (Monster) | −7,0,−11 | 6×3.5×5 | monster |
| 20 | Cell 12 (Flooded) | Cell | −7,0,−18 | 6×3.5×5 | |
| 21 | Cell 13 (Mutation) | Cell (Monster) | −7,0,−25 | 6×3.5×5 | monster |
| 22 | Cell 14 (Blood Trail) | Cell | −7,0,−32 | 6×3.5×5 | |
| 23 | Cell Block B Hallway | Hallway | −13.5,0,−18 | 4×3.5×32 | |
| 24 | CB South Connector | Connector Hall | −13.5,0,−36 | 14×3.5×3 | |
| 25 | Descending Stairs | Stairway | 20,−2.5,−2 | 4×5×3 | stairDown:0:−5 |
| 26 | Cave Tunnel | Tunnel | 27,−5,−2 | 10×3.5×3 | |
| 27 | Crystal Cavern | Cave | 41,−5,−2 | 18×8×16 | |
| 28 | Side Grotto | Cave Chamber | 55,−5,1 | 8×6×8 | |

### Doors / connections (`LA.FLOOR1_DOORS`, room-index pairs)
```
[0,4][1,4][2,4][3,4][4,5][4,6][4,7][4,8][4,9][4,10]
[0,18][1,18][1,19][2,19][2,20][3,20][3,21]
[5,12][12,25][25,26][26,27][27,28]
[0,11][10,11]
[13,23][14,23][15,23][16,23][17,23]
[18,23][19,23][20,23][21,23][22,23]
[17,24][22,24][23,24]
```
Reading: the central spine (Jake's cell block 0–3) connects via the **Main Hallway (4)** to the support rooms (Guard Station 5, Storage 6, Medical Bay 7, Armory 8) and the **Elevator Lobby (9)**. The **east arm** runs Guard Station → Creepy Passage (12) → Descending Stairs (25) → Cave Tunnel (26) → Crystal Cavern (27) → Side Grotto (28). Two extra cell blocks (10–14 and 5–9 west) hang off the **Cell Block B Hallway (23)** with a **CB South Connector (24)** at the far south.

### Raw data array — `LA.FLOOR1_DEFAULT` (Config.js 133–163, transcribe directly)
```js
LA.FLOOR1_DEFAULT = [
  {n:"Jake's Cell",t:"Jake Cell",x:0,y:0,z:0,w:7,h:4,d:6,f:'bendable'},
  {n:"Cell 2 (Abandoned)",t:"Cell",x:0,y:0,z:-8,w:6,h:3.5,d:5,f:''},
  {n:"Cell 3 (Failed Exp)",t:"Cell (Monster)",x:0,y:0,z:-15,w:6,h:3.5,d:5,f:'monster'},
  {n:"Cell 4 (Skeleton)",t:"Cell",x:0,y:0,z:-22,w:6,h:3.5,d:5,f:''},
  {n:"Main Hallway",t:"Hallway",x:5.5,y:0,z:-12,w:3,h:3.5,d:26,f:''},
  {n:"Guard Station",t:"Guard Station",x:11,y:0,z:-2,w:5,h:3.5,d:5,f:''},
  {n:"Storage",t:"Storage",x:11,y:0,z:-9,w:5,h:3.5,d:5,f:''},
  {n:"Medical Bay",t:"Medical Bay",x:11,y:0,z:-16,w:5,h:3.5,d:5,f:''},
  {n:"Armory",t:"Armory",x:11,y:0,z:-23,w:5,h:3.5,d:5,f:''},
  {n:"Elevator Lobby",t:"Elevator Lobby",x:5.5,y:0,z:-27,w:5,h:4,d:4,f:''},
  {n:"Adjacent Cell",t:"Adjacent Cell",x:5.5,y:0,z:5,w:5,h:3.5,d:4,f:''},
  {n:"Old Armory",t:"Old Armory",x:-1,y:0,z:7,w:7,h:3.5,d:5,f:''},
  {n:"Creepy Passage",t:"Creepy Passage",x:16,y:0,z:-2,w:4,h:3,d:3,f:''},
  {n:"Cell 5 (Vacated)",t:"Cell",x:-20,y:0,z:-4,w:6,h:3.5,d:5,f:''},
  {n:"Cell 6 (Infected)",t:"Cell (Monster)",x:-20,y:0,z:-11,w:6,h:3.5,d:5,f:'monster'},
  {n:"Cell 7 (Dead Guard)",t:"Cell",x:-20,y:0,z:-18,w:6,h:3.5,d:5,f:''},
  {n:"Cell 8 (Containment)",t:"Cell",x:-20,y:0,z:-25,w:6,h:3.5,d:5,f:''},
  {n:"Cell 9 (Collapsed)",t:"Cell",x:-20,y:0,z:-32,w:6,h:3.5,d:5,f:''},
  {n:"Cell 10 (Sarah's - Empty)",t:"Cell",x:-7,y:0,z:-4,w:6,h:3.5,d:5,f:'npc'},
  {n:"Cell 11 (Feral)",t:"Cell (Monster)",x:-7,y:0,z:-11,w:6,h:3.5,d:5,f:'monster'},
  {n:"Cell 12 (Flooded)",t:"Cell",x:-7,y:0,z:-18,w:6,h:3.5,d:5,f:''},
  {n:"Cell 13 (Mutation)",t:"Cell (Monster)",x:-7,y:0,z:-25,w:6,h:3.5,d:5,f:'monster'},
  {n:"Cell 14 (Blood Trail)",t:"Cell",x:-7,y:0,z:-32,w:6,h:3.5,d:5,f:''},
  {n:"Cell Block B Hallway",t:"Hallway",x:-13.5,y:0,z:-18,w:4,h:3.5,d:32,f:''},
  {n:"CB South Connector",t:"Connector Hall",x:-13.5,y:0,z:-36,w:14,h:3.5,d:3,f:''},
  {n:"Descending Stairs",t:"Stairway",x:20,y:-2.5,z:-2,w:4,h:5,d:3,f:'stairDown:0:-5'},
  {n:"Cave Tunnel",t:"Tunnel",x:27,y:-5,z:-2,w:10,h:3.5,d:3,f:''},
  {n:"Crystal Cavern",t:"Cave",x:41,y:-5,z:-2,w:18,h:8,d:16,f:''},
  {n:"Side Grotto",t:"Cave Chamber",x:55,y:-5,z:1,w:8,h:6,d:8,f:''}
];

LA.FLOOR1_DOORS = [
  [0,4],[1,4],[2,4],[3,4],[4,5],[4,6],[4,7],[4,8],[4,9],[4,10],
  [0,18],[1,18],[1,19],[2,19],[2,20],[3,20],[3,21],
  [5,12],[12,25],[25,26],[26,27],[27,28],
  [0,11],[10,11],
  [13,23],[14,23],[15,23],[16,23],[17,23],
  [18,23],[19,23],[20,23],[21,23],[22,23],
  [17,24],[22,24],[23,24]
];
```

### v10.9 vs v10.5 (Floor 1) — differences
- **The Floor-1 room + door data is IDENTICAL** between the two extracts (byte-for-byte: same 29 rooms, same 38 door pairs, same coords/dims/flags). No Floor-1 geometry changes to flag.
- Two things to be aware of (not geometry changes):
  - The folder `G:\GameDev\LevelArchitect_v10_5\` actually reports `LA.VERSION = 10.3` / `VERSION_TAG = 'v10.3'` internally (the "v10.5" is just the folder name). v10.9 reports `10.9` / `'v10.9-facility'`.
  - The existing transcription `docs/design/FLOOR1_LEVELARCHITECT.md` lists room 18 as **"Cell 10 (Sarah's-Empty)"** (no spaces around the dash). The actual source string in **both** versions is **`"Cell 10 (Sarah's - Empty)"`** (spaces around the dash). Cosmetic only.

---

## FLOORS 2–7 — no LevelArchitect data

There is **no room or door data to transcribe** for Floors 2–7. They are empty in the v10.9 source (App.js 543–544) and any designer-authored geometry lived only in browser `localStorage`, which is not in this folder. Per-floor authoring intent (names only):

| Floor | LevelArchitect name | Native `L1Floor` analogue |
|------|----------------------|----------------------------|
| 2 | Medical Bay | F2 — Medical wards (Aria/Keisha/Emily rescues) |
| 3 | Genetics Lab | F3 — Labs |
| 4 | Cybernetics Wing | F4 — Offices |
| 5 | Drone Station | F5 — Synth bay |
| 6 | Salvari Level | F6 — Executive (Sarah) |
| 7 | Executive Suite | F7 — Rooftop / finale |

(The native engine's **B1** "Basement security" / Jake's spawn maps to the Babylon Floor-1 **detention block**; native **F1** "Atrium" has no Babylon analogue — Babylon Floor 1 is the detention level itself.)

---

## Native-engine mapping (recommended)

**Current native footprints** (`app/level1.cpp` `kFloors[]`, lines 35–44) — every floor is the SAME tiny plate:

| Native floor | `x0,x1,zHalf` | Plate W×D | `ceil` | `y0` |
|--------------|---------------|-----------|--------|------|
| B1 | 0,24,8 | 24 × 16 m | 3.6 | 0 |
| F1 | 0,24,8 | 24 × 16 m | 4.6 | 5 |
| F2 | 0,24,8 | 24 × 16 m | 3.8 | 10 |
| F3 | 0,24,8 | 24 × 16 m | 4.0 | 15 |
| F4 | 0,24,8 | 24 × 16 m | 3.6 | 20 |
| F5 | 0,24,8 | 24 × 16 m | 4.5 | 25 |
| F6 | 0,24,8 | 24 × 16 m | 4.2 | 30 |
| F7 | 0,24,8 | 24 × 16 m | 7.0 | 35 |

The `L1RoomDef` is `{ float x0, x1, zHalf, ceil, y0; }` — a single rectangular plate per floor (floor at `y0`, ceiling at `y0+ceil`, spans `x∈[x0,x1]`, `z∈[-zHalf,+zHalf]`). The shaft is at `x=21` (`kShaftCx`), so a 24-m-wide plate is just big enough to hold the shaft at its +X edge — far too small for the detention layout.

### Recommendation
1. **Grow the Floor-1 plate (native B1, where Jake spawns) to the real detention footprint.** The Babylon Floor 1 bounding box is **~75 × 43 m**; the core detention block (without the eastern cave arm) is **~38 × 43 m** (`x∈[-23,+18]` allowing for half-extents, `z∈[-38,+9]`). Minimum: set the B1 plate to cover **`x0 ≈ -24, x1 ≈ +20` (or +58 if you also build the cave arm), `zHalf ≈ 22`** so the cell blocks fit. The current `x0=0,x1=24,zHalf=8` is ~5× too small.

2. **Keep the vertical-stack model.** Do **not** abandon `floorBaseY` / `kFloorSpacing` (5 m). Floors still stack along +Y with the elevator shaft lined up. Only the **per-floor XZ footprint** (and interior partitions) needs to grow. Note: the `L1RoomDef` single-rectangle model cannot represent the multi-room detention layout by itself — it only sets the outer plate + ceiling. To re-lay the *interior* (cells, hallways, support rooms), either (a) extend `buildLevel1()` to emit the interior cross-walls/doorways per the Floor-1 room table above (the file already has `addCrossWall`/`addWallX`/`addFloor` helpers and a doorway-gap builder), or (b) introduce a richer per-room table mirroring `FLOOR1_DEFAULT` (name/type/center/half-extents) for B1.

3. **Transcribe with a center→half-extent conversion, no axis flip.** Babylon and native are both RH, +X right, +Y up, −Z forward, meters. For each Babylon room `{x,y,z,w,h,d}`:
   - native **center** = `(x, y + h/2, z)` if you want floor-relative (Babylon `y` is the floor; native boxes are built from a floor slab + walls so use `y` as `floorY`),
   - native **half-extents** = `(w/2, h/2, d/2)`,
   - ceiling height for that room = Babylon `h`.
   No negation of any axis is required (unlike a LH→RH port). Coordinates drop in 1:1.

4. **Map the vertical Y.** Babylon Floor 1 lives on a single `y=0` plane (with the stairs/caves dipping to `y=-2.5`/`-5`). The native B1 floor is also `y0=0`, so the detention layout transcribes onto **B1 at y0=0 directly**. The native F1 "Atrium" (y0=5) has no Babylon source — keep it as authored design, or repurpose. If you build the Babylon cave arm (stairs→Crystal Cavern), it descends **below** B1 (to y≈-5), which the current single-stack-upward model doesn't have — add a sub-basement plate or treat the caves as a separate area.

5. **Doors.** Use `LA.FLOOR1_DOORS` (above) to decide which cross-walls get the 1.2 m doorway gap (`addCrossWall(..., withDoorway=true, zDoor=...)`). The five legacy native doors (`doorA..doorE`, `Level1Layout`) and the existing beat logic (cell→corridor→armory→checkpoint→arena→elevator) should be re-anchored onto the real Jake's-Cell / Main-Hallway / Guard-Station / Armory / Elevator-Lobby rooms rather than the abstract sub-zones.

6. **Floors 2–7:** there is nothing to transcribe — author them fresh in the engine (or in LevelArchitect, then re-extract). The native placeholder plates (24×16 m) are fine as stubs until then; size them to the real Floor-1 once you know how big each wing should be.

---

*Generated from `G:\GameDev\LevelArchitectFullV10.9\js\Config.js` (Floor 1) + `App.js`/`WalkMode.js` (scale/init). Native target: `G:\X3Native\app\level1.{h,cpp}`. Coordinate convention: `G:\X3Native\docs\CONVENTIONS.md`. No engine code or Babylon source was modified.*
