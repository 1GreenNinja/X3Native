#!/usr/bin/env python3
"""W-TOWN asset pipeline: the Small Mountain Town kit -> assets/converted_glb/Town/.

WHY THIS FILE WAS REWRITTEN (the receipt, NO_SLOP rule 10)
----------------------------------------------------------
Round one built the town out of the armory's **HouseForge** kit and the result
was eyes-on WRONG: `docs/screenshots/town/02_shop_front.png` shows a dark,
spiky, broken silhouette, because that kit is authored as COLLAPSED RUINS.
`docs/design/TOWN_ASSET_SCOUT.md` reached the same verdict independently. The
geometry, textures, scale and grounding were all correct — the *kit* was a
ruined-settlement kit. So the kit is swapped and the placer is untouched.

THE NEW KIT, and why it is the right one
----------------------------------------
Everything structural now comes from ONE pack — the licensed
`Complete Racing Game URP All in One` (`\\p13700\\G\\Assets\\...`). One pack means
one register: it was authored for a DRIVING game, so the town reads as a place
on this world's road network rather than as a medieval village that a freeway
happens to pass.

* **Houses** — `Models/Level_Design/Models/Red_House/HighPoly/House_1..4.fbx`.
  Clean, whole, pitched-roof houses, 15-37 m wide, 9-25.7 m tall: exactly the
  massing a mountain main street wants.
* **Street lamp** — `Light 2/Light_2.fbx` (7.2 m, its own diffuse + normal).
  Replaces round one's medieval torches.
* **Road signs** — `Billboards/Billboard_1..2.fbx` (2.3-2.4 m).
* **Fence** — `Red_House/HighPoly/Wood_Fence.fbx`.
* Benches (`nature/SM_*Bench*`) and the parked fleet (`Vehicles/`) are already
  in-tree and unchanged.

TWO TRAPS THIS TOOL EXISTS TO DEFUSE
------------------------------------
1. **The pack ships NO Unity material metadata.** `find ... -name '*.mat'`
   returns 0 across the whole pack, and there are no `.meta` files either, so
   `tools/convert_unity_pack.py`'s GUID->texture resolution has nothing to
   resolve. FBX2glTF therefore writes **1x1 white placeholder PNGs** for every
   texture slot ("Warning: could not find a image file for texture"). Shipping
   that is a flat-grey town — NO_SLOP rule 3. **This tool injects the pack's
   real textures by MATERIAL NAME** (the `PAINTS` table below), which is what
   the artist's own naming makes unambiguous: material `Wall` wants `Wall.tif`,
   `Roof` wants `Roof_*.tif`, `Base` wants `Brick.tif`.
2. **stb_image cannot read TIFF.** `engine/asset/ModelLoader.cpp:639` decodes
   embedded images with `stbi_load_from_memory`, and three of the pack's four
   wall/roof albedos are `.tif`. They are transcoded to JPEG here. (The same
   check is why the loader can never be handed WebP — there is no WebP decoder
   anywhere in the tree, which is what bit round one's armory GLBs.)

WHY THE PAINT VARIANTS
----------------------
Four house meshes is thin for a 17-plot street, and repeating a mesh is the
"uniform lattice" the x3native-environments skill forbids. The pack's own UVs
are authored to TILE (`Wall` spans u -6.7..7.7), so the same shell takes a
different real photographic siding convincingly. Each mesh is therefore baked
in two paints — RED clapboard (`Wall.tif`) with scalloped shingle, and WHITE
clapboard (`Wood.jpg`) with plank roof — giving 8 distinct facades from 4
shells, every one carrying a real photograph. That is variety by material,
which is how a real street of the same builder's houses actually looks.

USAGE
-----
    python tools/town_assets.py convert    # pack -> assets/converted_glb/Town
    python tools/town_assets.py report     # the measured table app/town.cpp holds
    python tools/town_assets.py verify     # assert nothing unreadable shipped

`report` prints, per asset, the numbers `kAssets[]` in `app/town.cpp` carries:
bbox centre in asset space, footprint half extents, the vertical span about the
origin plane, and the MEASURED front. X3_WORLD_RULES rule 0 says verify, never
assume; rule 3 says orientation is documented per asset. **If the kit is
re-baked, re-run `report` and update the table — PAIRED VALUES ARE ONE VALUE
(NO_SLOP rule 4).**

THE FRONT IS MEASURED, NOT GUESSED. These meshes carry no door *nodes* (round
one's HouseForge kit did), but they carry door *materials* — `Door`,
`Door_2`, `Door_Garage`. The front is the horizontal direction from the bbox
centre to the **centroid of the door material's triangles**. For `House_2`,
which has no door material, it is the centroid of the `Glass` material (its
glazing is all on one elevation). Both are printed by `report` and repeated in
`docs/design/TOWN_MANIFEST.md`.

Deps: Pillow, numpy; `D:/GameDev/tools/FBX2glTF.exe`.
AFTER RUNNING: `python tools/asset_store.py publish assets/converted_glb/Town`
— store-served, never git-committed (docs/ENGINE_GOTCHAS.md gotcha 2.5).
"""
from __future__ import annotations

import json
import math
import os
import struct
import subprocess
import sys
from io import BytesIO

import numpy as np
from PIL import Image

PACK = r"\\p13700\G\Assets\Complete Racing Game URP All in One\Racing_Game\Models\Level_Design\Models"
FBX2GLTF = r"D:\GameDev\tools\FBX2glTF.exe"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets", "converted_glb", "Town")
STAGE = os.path.join(os.environ.get("TEMP", "/tmp"), "x3_town_stage")

RH = os.path.join(PACK, "Red_House", "HighPoly")
RHT = os.path.join(RH, "Textures")
BLD = os.path.join(PACK, "Buildings", "map")

MAX_DIM = 1024   # VRAM sanity; the pack's albedos are 512-1024 already.

# ---------------------------------------------------------------------------
# THE PAINT TABLE — material name -> what that material actually is.
#
# `tex`   a source image, injected as the baseColorTexture (tiling REPEAT; the
#         pack's UVs run well outside 0..1 and are authored for it).
# `color` a baseColorFactor for materials that legitimately have no albedo map:
#         glass (a window's albedo IS its tint) and the degenerate-UV `Metal`
#         slot, whose UVs collapse to a point so no texture could ever show.
# `mr`    (metallic, roughness). X3_WORLD_RULES rule 5: never ship metallic 1.0
#         without an environment to reflect or it renders BLACK, so the metal
#         trim is clamped to 0.55.
# ---------------------------------------------------------------------------
GLASS = {"color": [0.022, 0.030, 0.038, 1.0], "mr": (0.0, 0.14)}
METAL = {"color": [0.42, 0.44, 0.46, 1.0], "mr": (0.55, 0.42)}

PAINTS = {
    # variant suffix -> {material name -> spec}
    "Red": {
        "Wall":        {"tex": os.path.join(RHT, "Wall.tif"),   "mr": (0.0, 0.82)},
        "Base":        {"tex": os.path.join(RHT, "Brick.tif"),  "mr": (0.0, 0.90)},
        "Roof":        {"tex": os.path.join(RHT, "Roof_2.tif"), "mr": (0.0, 0.85)},
        "Frame":       {"tex": os.path.join(RHT, "Wood.jpg"),   "mr": (0.0, 0.70)},
        "Door":        {"tex": os.path.join(RHT, "Wood.jpg"),   "mr": (0.0, 0.62)},
        "Door_2":      {"tex": os.path.join(RHT, "Wood.jpg"),   "mr": (0.0, 0.62)},
        "Door_Garage": {"tex": os.path.join(RHT, "Wood.jpg"),   "mr": (0.0, 0.62)},
        "Glass":       GLASS,
        "Metal":       METAL,
    },
    "White": {
        "Wall":        {"tex": os.path.join(RHT, "Wood.jpg"),   "mr": (0.0, 0.78)},
        "Base":        {"tex": os.path.join(RHT, "Brick.tif"),  "mr": (0.0, 0.90)},
        "Roof":        {"tex": os.path.join(RHT, "Roof_1.tif"), "mr": (0.0, 0.86)},
        "Frame":       {"tex": os.path.join(RHT, "Wall.tif"),   "mr": (0.0, 0.70)},
        "Door":        {"tex": os.path.join(RHT, "Wall.tif"),   "mr": (0.0, 0.62)},
        "Door_2":      {"tex": os.path.join(RHT, "Wall.tif"),   "mr": (0.0, 0.62)},
        "Door_Garage": {"tex": os.path.join(RHT, "Wall.tif"),   "mr": (0.0, 0.62)},
        "Glass":       GLASS,
        "Metal":       METAL,
    },
}

# Props: one paint each, no variants.
PROP_PAINT = {
    "Wood":          {"tex": os.path.join(RHT, "Wood.jpg"),  "mr": (0.0, 0.75)},
    # Light_2 ships its own diffuse + normal beside the FBX.
    "Holder":        {"tex": os.path.join(PACK, "Light 2", "Light_2_Diffuse.png"), "mr": (0.30, 0.55)},
    "Metal":         METAL,
    # The lamp head and the sign's lightbox: a warm near-white the town's own
    # emissive pass lifts at dusk (town.cpp owns the glow; a baked-bright albedo
    # here would clip under ACES per X3_WORLD_RULES rule 5).
    "Light":         {"color": [0.86, 0.82, 0.70, 1.0], "mr": (0.0, 0.45)},
    "Cement":        {"tex": os.path.join(BLD, "TCom_Roads0059_1_seamless_M.jpg"), "mr": (0.0, 0.93)},
    "Billboard":     {"tex": os.path.join(PACK, "Billboards", "Materials", "Billboard_1.png"), "mr": (0.0, 0.55)},
    "Red_Billboard": {"tex": os.path.join(PACK, "Billboards", "Materials", "Billboard_2.png"), "mr": (0.0, 0.55)},
    "White_Plastic": {"color": [0.80, 0.80, 0.78, 1.0], "mr": (0.0, 0.55)},
}

# source FBX (relative to PACK) -> [(output name, paint set)]
BUILDINGS = [
    ("Red_House/HighPoly/House_1.fbx", [("House_1_Red", "Red"), ("House_1_White", "White")]),
    ("Red_House/HighPoly/House_2.fbx", [("House_2_Red", "Red"), ("House_2_White", "White")]),
    ("Red_House/HighPoly/House_3.fbx", [("House_3_Red", "Red"), ("House_3_White", "White")]),
    ("Red_House/HighPoly/House_4.fbx", [("House_4_Red", "Red"), ("House_4_White", "White")]),
]
PROPS = [
    ("Red_House/HighPoly/Wood_Fence.fbx", "Wood_Fence"),
    ("Light 2/Light_2.fbx",               "Light_2"),
    ("Billboards/Billboard_1.fbx",        "Billboard_1"),
    ("Billboards/Billboard_2.fbx",        "Billboard_2"),
]

# THE FRONT REFERENCE, in priority TIERS. The first tier a mesh actually has
# wins; lower tiers are ignored even if present.
#
# The tiers are not cosmetic. `House_3`'s `Door` slot is NOT a door: its UVs run
# u -5.15..6.15, i.e. it is a TILING surface (a clad wall), and averaging it in
# dragged the measured front round to the blank flank — caught by rendering the
# result and looking at it (X3_WORLD_RULES rule 0), not by reading the name.
# `Door_Garage` / `Door_2` have 0..1 UVs and are genuinely single openings.
FRONT_TIERS = (
    ("Door_Garage", "Door_2"),   # a real, single, unwrapped opening
    ("Door",),                   # only when nothing better exists
    ("Glass",),                  # last resort; see FRONT_OVERRIDE
)

# EYES-ON OVERRIDES (X3_WORLD_RULES rule 0 — the render is the arbiter).
# `House_2` has no door material at all, so the Glass tier fired and returned
# -121.1 deg. Rendering the mesh at 0/90/180/270 showed the actual entrance —
# a centred door with a porch step — on the 180 deg elevation; -121 deg is a
# blank gable. The measurement is kept honest by naming what overrode it.
FRONT_OVERRIDE = {
    "House_2": (180.0, "no door material; entrance identified by ortho render at 0/90/180/270"),
}


# ---------------------------------------------------------------------------
# GLB read / write
# ---------------------------------------------------------------------------
def load_glb(path):
    with open(path, "rb") as f:
        magic, _ver, _len = struct.unpack("<III", f.read(12))
        assert magic == 0x46546C67, f"not a GLB: {path}"
        clen, ctype = struct.unpack("<II", f.read(8))
        assert ctype == 0x4E4F534A, "expected JSON chunk"
        gltf = json.loads(f.read(clen))
        blen, btype = struct.unpack("<II", f.read(8))
        assert btype == 0x004E4942, "expected BIN chunk"
        bin_ = bytearray(f.read(blen))
    return gltf, bin_


def save_glb(path, gltf, bin_):
    while len(bin_) % 4:
        bin_.append(0)
    gltf.setdefault("buffers", [{}])
    gltf["buffers"][0] = {"byteLength": len(bin_)}
    js = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    while len(js) % 4:
        js += b" "
    total = 12 + 8 + len(js) + 8 + len(bin_)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(js), 0x4E4F534A)); f.write(js)
        f.write(struct.pack("<II", len(bin_), 0x004E4942)); f.write(bytes(bin_))


def add_image(gltf, bin_, raw, mime):
    """Append image bytes as a bufferView; return the new image index."""
    while len(bin_) % 4:
        bin_.append(0)
    off = len(bin_)
    bin_ += raw
    gltf.setdefault("bufferViews", []).append({"buffer": 0, "byteOffset": off, "byteLength": len(raw)})
    gltf.setdefault("images", []).append({"bufferView": len(gltf["bufferViews"]) - 1, "mimeType": mime})
    return len(gltf["images"]) - 1


def encode_texture(path):
    """Load any source image (incl. TIFF, which stb_image CANNOT read) and
    return (bytes, mime) the engine's stb_image decoder can definitely open."""
    im = Image.open(path)
    im = im.convert("RGBA" if "A" in im.getbands() else "RGB")
    if max(im.size) > MAX_DIM:
        s = MAX_DIM / max(im.size)
        im = im.resize((max(1, int(im.width * s)), max(1, int(im.height * s))), Image.LANCZOS)
    buf = BytesIO()
    if im.mode == "RGBA":
        im.save(buf, "PNG", optimize=True)
        return buf.getvalue(), "image/png"
    im.save(buf, "JPEG", quality=90)
    return buf.getvalue(), "image/jpeg"


def repaint(gltf, bin_, paint, label):
    """Point every material at its real texture / authored constants."""
    cache = {}
    # One REPEAT sampler for everything: the pack's UVs tile well outside 0..1
    # and CLAMP would smear the edge texel across a whole wall.
    gltf.setdefault("samplers", [])
    gltf["samplers"].append({"wrapS": 10497, "wrapT": 10497, "magFilter": 9729, "minFilter": 9987})
    samp = len(gltf["samplers"]) - 1
    missing = []
    for mat in gltf.get("materials", []):
        name = mat.get("name", "")
        spec = paint.get(name)
        if spec is None:
            missing.append(name)
            continue
        pbr = mat.setdefault("pbrMetallicRoughness", {})
        met, rough = spec.get("mr", (0.0, 0.8))
        pbr["metallicFactor"] = met
        pbr["roughnessFactor"] = rough
        if "tex" in spec:
            src = spec["tex"]
            if src not in cache:
                if not os.path.exists(src):
                    raise SystemExit(f"[town] MISSING SOURCE TEXTURE {src} (for material {name})")
                raw, mime = encode_texture(src)
                img = add_image(gltf, bin_, raw, mime)
                gltf.setdefault("textures", []).append({"source": img, "sampler": samp})
                cache[src] = len(gltf["textures"]) - 1
            pbr["baseColorTexture"] = {"index": cache[src], "texCoord": 0}
            pbr["baseColorFactor"] = [1.0, 1.0, 1.0, 1.0]
        else:
            pbr.pop("baseColorTexture", None)
            pbr["baseColorFactor"] = spec["color"]
    if missing:
        print(f"    ! {label}: NO PAINT for materials {missing} — they keep FBX2glTF's "
              f"1x1 placeholder. Add them to PAINTS or the town ships grey (NO_SLOP rule 3).")
    return missing


def strip_placeholders(gltf):
    """Drop FBX2glTF's 1x1 white data-URI images once real ones are bound.
    Leaving them costs nothing at runtime but `verify` asserts on them, because
    a 1x1 white image left BOUND is precisely the untextured-stand-in defect."""
    used = set()
    for t in gltf.get("textures", []):
        used.add(t.get("source"))
    for i, im in enumerate(gltf.get("images", [])):
        if i not in used and str(im.get("uri", "")).startswith("data:"):
            im["uri"] = ""  # unreferenced; keep the index stable
    return gltf


# ---------------------------------------------------------------------------
# measurement (X3_WORLD_RULES rule 0)
# ---------------------------------------------------------------------------
def read_accessor(gltf, bin_, idx):
    a = gltf["accessors"][idx]
    bv = gltf["bufferViews"][a["bufferView"]]
    ncomp = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[a["type"]]
    dt = {5126: np.float32, 5123: np.uint16, 5125: np.uint32, 5121: np.uint8}[a["componentType"]]
    off = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
    arr = np.frombuffer(bytes(bin_), dtype=dt, count=a["count"] * ncomp, offset=off)
    return arr.reshape(-1, ncomp) if ncomp > 1 else arr


def measure(path):
    gltf, bin_ = load_glb(path)
    lo = np.array([9e9] * 3); hi = np.array([-9e9] * 3)
    tiers = [[] for _ in FRONT_TIERS]
    for mesh in gltf.get("meshes", []):
        for pr in mesh["primitives"]:
            pos = read_accessor(gltf, bin_, pr["attributes"]["POSITION"])
            lo = np.minimum(lo, pos.min(0)); hi = np.maximum(hi, pos.max(0))
            mi = pr.get("material")
            nm = gltf["materials"][mi].get("name", "") if mi is not None else ""
            for t, names in enumerate(FRONT_TIERS):
                if nm in names:
                    tiers[t].append(pos.mean(0))
    ctr = (lo + hi) * 0.5
    ref, src = [], "none"
    for t, got in enumerate(tiers):
        if got:
            ref, src = got, "/".join(FRONT_TIERS[t])
            break
    frontDeg = 0.0
    if ref:
        c = np.mean(ref, axis=0)
        dx, dz = float(c[0] - ctr[0]), float(c[2] - ctr[2])
        if abs(dx) + abs(dz) > 1e-4:
            # engine yaw, AXES LAW: 0 = -Z
            frontDeg = math.degrees(math.atan2(-dx, -dz))
    stem = os.path.basename(path)[:-4]
    for key, (deg, why) in FRONT_OVERRIDE.items():
        if stem.startswith(key):
            frontDeg, src = deg, "EYES-ON: " + why
            break
    panes = measure_panes(gltf, bin_, ctr, lo, frontDeg)
    return dict(lo=lo, hi=hi, ctr=ctr, frontDeg=frontDeg, src=src, panes=panes)


def measure_panes(gltf, bin_, ctr, lo, frontDeg):
    """WHERE THE LIT WINDOWS ACTUALLY ARE — measured, not assumed.

    Round one placed its two dusk panes at hard-coded storey heights (2.35 m and
    5.20 m above ground) because the HouseForge kit gave nothing better to go on.
    This kit does: every house models its glazing as a separate `Glass` material,
    so the panes can be put exactly where the artist put the windows. That
    matters here because the kit's houses run 9 m to 25.7 m tall — one fixed pair
    of storey heights would have put House_2's upper pane inside its roof.

    Returns up to two (heightAboveBase, alongOffset) pairs for the glass nearest
    the FRONT elevation, in the asset's own front frame:
        along = the horizontal axis parallel to the front wall
        height = metres above the bbox BOTTOM (which is what the placer grounds)
    """
    fx, fz = -math.sin(math.radians(frontDeg)), -math.cos(math.radians(frontDeg))
    ax, az = -fz, fx                      # along-wall axis
    pts = []
    for mesh in gltf.get("meshes", []):
        for pr in mesh["primitives"]:
            mi = pr.get("material")
            if mi is None or gltf["materials"][mi].get("name") != "Glass":
                continue
            pos = read_accessor(gltf, bin_, pr["attributes"]["POSITION"])
            idx = read_accessor(gltf, bin_, pr["indices"]).reshape(-1) if "indices" in pr else None
            tri = pos[idx].reshape(-1, 3, 3) if idx is not None else pos.reshape(-1, 3, 3)
            cen = tri.mean(axis=1)
            for c in cen:
                dx, dz = float(c[0] - ctr[0]), float(c[2] - ctr[2])
                pts.append((dx * fx + dz * fz,            # depth along the front normal
                            dx * ax + dz * az,            # along the wall
                            float(c[1] - lo[1])))         # height above the bbox bottom
    if not pts:
        return []
    depth = np.array([p[0] for p in pts])
    # Front elevation only: the glass in the outer 35% of the front-facing depth.
    keep = depth >= depth.max() - 0.35 * (depth.max() - depth.min() + 1e-6)
    sel = [p for p, k in zip(pts, keep) if k]
    if not sel:
        return []
    hs = np.array([p[2] for p in sel])
    # Split into a lower and an upper storey at the largest gap in the heights.
    order = np.argsort(hs); sh = hs[order]
    groups = [sel]
    if len(sh) > 4:
        gaps = np.diff(sh)
        gi = int(np.argmax(gaps))
        if gaps[gi] > 1.2:                      # a real storey break, not scatter
            cut = 0.5 * (sh[gi] + sh[gi + 1])
            groups = [[p for p in sel if p[2] <= cut], [p for p in sel if p[2] > cut]]
    out = []
    for g in groups:
        if not g:
            continue
        out.append((round(float(np.mean([p[2] for p in g])), 2),
                    round(float(np.mean([p[1] for p in g])), 2)))
    return out[:2]


# ---------------------------------------------------------------------------
# subcommands
# ---------------------------------------------------------------------------
def fbx_to_glb(rel, stem):
    os.makedirs(STAGE, exist_ok=True)
    src = os.path.join(PACK, rel.replace("/", os.sep))
    if not os.path.exists(src):
        raise SystemExit(f"[town] missing source FBX: {src}")
    local = os.path.join(STAGE, os.path.basename(src))
    if not os.path.exists(local) or os.path.getmtime(local) < os.path.getmtime(src):
        with open(src, "rb") as a, open(local, "wb") as b:
            b.write(a.read())
    out = os.path.join(STAGE, stem + ".raw.glb")
    r = subprocess.run([FBX2GLTF, "-b", "-i", local, "-o", out],
                       capture_output=True, text=True)
    if not os.path.exists(out):
        raise SystemExit(f"[town] FBX2glTF failed for {rel}:\n{r.stdout}\n{r.stderr}")
    return out


def cmd_convert():
    os.makedirs(OUT, exist_ok=True)
    problems = []
    for rel, variants in BUILDINGS:
        raw = fbx_to_glb(rel, os.path.basename(rel)[:-4])
        for name, paintkey in variants:
            gltf, bin_ = load_glb(raw)
            miss = repaint(gltf, bin_, PAINTS[paintkey], name)
            problems += [(name, m) for m in miss]
            strip_placeholders(gltf)
            dst = os.path.join(OUT, name + ".glb")
            save_glb(dst, gltf, bin_)
            print(f"  {name:18s} {os.path.getsize(dst)//1024:6d} KB  paint={paintkey}")
    for rel, name in PROPS:
        raw = fbx_to_glb(rel, name)
        gltf, bin_ = load_glb(raw)
        miss = repaint(gltf, bin_, PROP_PAINT, name)
        problems += [(name, m) for m in miss]
        strip_placeholders(gltf)
        dst = os.path.join(OUT, name + ".glb")
        save_glb(dst, gltf, bin_)
        print(f"  {name:18s} {os.path.getsize(dst)//1024:6d} KB  prop")
    if problems:
        print("\n  UNPAINTED MATERIALS (NO_SLOP rule 3 risk):")
        for n, m in problems:
            print(f"    {n}: {m}")
    print(f"\n[town] wrote {OUT}")


def cmd_report():
    rows = []
    for f in sorted(os.listdir(OUT)):
        if not f.endswith(".glb"):
            continue
        m = measure(os.path.join(OUT, f))
        lo, hi, ctr = m["lo"], m["hi"], m["ctr"]
        rows.append((f[:-4], hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2],
                     ctr[0], ctr[2], (hi[0] - lo[0]) * 0.5, (hi[2] - lo[2]) * 0.5,
                     lo[1], hi[1], m["frontDeg"], m["src"]))
    print(f"{'asset':22s} {'W':>7s}{'H':>7s}{'D':>7s} | {'cx':>7s}{'cz':>7s}{'hx':>7s}{'hz':>7s}"
          f"{'loY':>7s}{'hiY':>7s} {'front':>8s}  ref")
    for r in rows:
        print(f"{r[0]:22s} {r[1]:7.2f}{r[2]:7.2f}{r[3]:7.2f} | {r[4]:7.2f}{r[5]:7.2f}"
              f"{r[6]:7.2f}{r[7]:7.2f}{r[8]:7.2f}{r[9]:7.2f} {r[10]:8.1f}  {r[11]}")
    print("\n// paste into kAssets[] in app/town.cpp")
    print("// { glb, cx, cz, hx, hz, loY, hiY, frontDeg, {win0Y,win0A}, {win1Y,win1A} }")
    for r in rows:
        m = measure(os.path.join(OUT, r[0] + ".glb"))
        p = m["panes"] + [(0.0, 0.0)] * (2 - len(m["panes"]))
        pad = " " * max(0, 26 - len(r[0]))
        print(f'    {{ "Town/{r[0]}.glb",{pad} {r[4]:7.2f}f,{r[5]:7.2f}f,'
              f'{r[6]:7.2f}f,{r[7]:7.2f}f,{r[8]:6.2f}f,{r[9]:7.2f}f,{r[10]:8.1f}f, '
              f'{{{p[0][0]:6.2f}f,{p[0][1]:7.2f}f}}, {{{p[1][0]:6.2f}f,{p[1][1]:7.2f}f}} }},')


def cmd_verify():
    """Assert the shipped kit carries nothing the engine cannot read, and that
    no material is left on a placeholder. This is the gate that would have
    caught round one's WebP, and it now also catches TIFF and 1x1 whites."""
    ok = True
    stb_ok = {"image/png", "image/jpeg"}
    for f in sorted(os.listdir(OUT)):
        if not f.endswith(".glb"):
            continue
        gltf, bin_ = load_glb(os.path.join(OUT, f))
        for ext in gltf.get("extensionsRequired", []):
            print(f"  FAIL {f}: extensionsRequired {ext}"); ok = False
        bound = {t.get("source") for t in gltf.get("textures", [])}
        for i, im in enumerate(gltf.get("images", [])):
            if i not in bound:
                continue
            mime = im.get("mimeType", "")
            if im.get("bufferView") is None:
                print(f"  FAIL {f}: bound image {i} has no bufferView"); ok = False
                continue
            if mime not in stb_ok:
                print(f"  FAIL {f}: bound image {i} mime {mime!r} (stb_image reads PNG/JPEG only)"); ok = False
            bv = gltf["bufferViews"][im["bufferView"]]
            raw = bytes(bin_[bv["byteOffset"]:bv["byteOffset"] + bv["byteLength"]])
            w, h = Image.open(BytesIO(raw)).size
            if w <= 2 and h <= 2:
                print(f"  FAIL {f}: bound image {i} is {w}x{h} — a placeholder (NO_SLOP rule 3)"); ok = False
        for mat in gltf.get("materials", []):
            pbr = mat.get("pbrMetallicRoughness", {})
            if pbr.get("metallicFactor", 1.0) >= 0.9 and "metallicRoughnessTexture" not in pbr:
                print(f"  FAIL {f}: material {mat.get('name')} metallic>=0.9 untextured "
                      f"(X3_WORLD_RULES rule 5 — renders BLACK)"); ok = False
        print(f"  {'ok  ' if ok else '    '}{f}: {len(gltf.get('materials', []))} materials, "
              f"{len(bound)} bound textures")
    print("\n[town] verify", "GREEN" if ok else "RED")
    return 0 if ok else 1


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "convert"
    if cmd == "convert":
        cmd_convert()
    elif cmd == "report":
        cmd_report()
    elif cmd == "verify":
        sys.exit(cmd_verify())
    else:
        raise SystemExit(__doc__)
