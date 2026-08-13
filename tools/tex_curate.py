#!/usr/bin/env python3
"""Curate + channel-convert texture sets from the pack library into the game's
surface library (assets/surface_library/<name>/{albedo,normal,mr}.png).

Channel law (ART_BIBLE 4): engine samples glTF MR — G=roughness, B=metallic
(mesh.frag:726 metallic=mr.b). Conversions:
  orm  : R=AO G=rough B=metal            -> pass-through
  rma  : HDRP MaskMap-style R=metal G=AO B=detail A=smooth -> r=G, g=1-A, b=R
  none : synthesize flat mr from scalars (per-set metal/rough defaults)
4K inputs are downscaled to 2K (store/lean); everything re-encoded PNG.
"""
import os, sys, json
import numpy as np
from PIL import Image

Image.MAX_IMAGE_PIXELS = None
LIB = os.path.join("assets", "surface_library")

MW = r"D:\Assets\Modular Warehouse Warehouse Industrial Warehouse Hangar Factory Warehouse"
SR = r"D:\Assets\Modular Shooting Range Military Base Military Facility Shooting Range Army"
HH = r"D:\Assets\Modular Abandoned Hospital Horror Hospital Abandoned hospital Hospital"
CC = r"D:\Assets\Command Center"
# Terrain splat sources (W6-3 misc-polish task 1 — real 2K albedos feeding
# registerTerrainMaterial(grass/rock/snow/sand); rock reuses sr_concrete_01
# above). Picked from docs/tex_catalog.json via grass/sand/snow/ground/dirt
# stem search (2626 sets scanned): the catalog has ZERO texture sets with
# "snow"/"ice"/"arctic"/"frost"/"winter" anywhere in pack or stem name — no
# true snow asset exists in the library. ADT's MarbleWhite00 (light, cool,
# cracked white stone) is the closest visual substitute and is used AS-IS for
# the snow splat layer; this is a deliberate substitution, not an oversight.
RHEP = r"D:\Assets\Rocky Hills Environment - Whitebark Pine\Toby Fredson\Rocky Hills Environment - Whitebark Pine\RHEWP_Demo\Textures\Textures_Terrain Tiles"
LGP3 = r"D:\Assets\Landscape Ground Pack 3 Desert Dry Land Beach Sea Islands Coast\NatureManufacture Assets\Coast Environment\Ground\Textures"
ADT  = r"D:\Assets\Ancient Desert Town\_DLNK\_DLNK Source\_DLNK Libraries\[dlnk Texture Library]\Materials\Marble\MarbleWhite00"

def find(setdir, stem):
    """Locate files for stem under a root (search recursively; stems are unique enough)."""
    hits = {}
    for dirpath, _, files in os.walk(setdir):
        for f in files:
            fl = f.lower()
            if not fl.startswith(stem.lower()):
                continue
            rest = os.path.splitext(fl[len(stem):])[0].strip("_-. ")
            full = os.path.join(dirpath, f)
            if rest in ("alb", "albedo", "basecolor", "bc", "d", "diff", "col", "color", ""):
                hits.setdefault("albedo", full)
            elif rest in ("n", "nrm", "norm", "normal"):
                hits.setdefault("normal", full)
            elif rest in ("orm",):
                hits.setdefault("mask", full); hits["conv"] = "orm"
            elif rest in ("rma",):
                hits.setdefault("mask", full); hits["conv"] = "rma"
    return hits

# name -> (root, stem, fallback_metal, fallback_rough)
SETS = {
    "mw_metal_panels_a":   (MW, "T_Metal_Panels_Tile_A",   0.85, 0.45),
    "mw_concrete_panels_a":(MW, "T_Concrete_Panels_Tile_A",0.00, 0.85),
    "mw_concrete_panels_b":(MW, "T_Concrete_Panels_Tile_B",0.00, 0.85),
    "mw_metal_grate":      (MW, "T_Metal_Grate_Tile",      0.90, 0.50),
    "mw_metal_trim_a":     (MW, "T_Metal_Trim_A",          0.85, 0.50),
    "mw_metal_trim_b":     (MW, "T_Metal_Trim_B",          0.85, 0.50),
    "mw_plaster_painted":  (MW, "T_Plaster_Painted_Tile_A",0.00, 0.80),
    "mw_thermal_padding":  (MW, "T_ThermalPadding_Tile_A", 0.10, 0.75),
    "mw_wall_plastic":     (MW, "T_Wall_Plastic_Tile_A",   0.05, 0.55),
    "mw_floor_trim":       (MW, "T_Floor_Deco_Trim_A",     0.60, 0.60),
    "sr_concrete_01":      (SR, "T_concrete_01",           0.00, 0.90),
    "sr_concrete_a":       (SR, "T_Concrete_a",            0.00, 0.88),
    "sr_floorstripes":     (SR, "T_FloorStripes",          0.10, 0.70),
    "sr_metal_b":          (SR, "T_metal_b",               0.85, 0.45),
    "sr_metal_lattice":    (SR, "T_metal_lattice",         0.90, 0.50),
    "sr_rubberfloor":      (SR, "T_RubberFloorTile",       0.00, 0.92),
    "hh_wall_01a":         (HH, "TX_Modular_Wall_01a",     0.05, 0.75),
    "hh_floor_01a":        (HH, "TX_Modular_Floor_01a",    0.10, 0.60),
    "hh_ceiling_01a":      (HH, "TX_Modular_Ceiling_01a",  0.05, 0.80),
    "cc_porous_cement":    (CC, "T_Porous_Cement_Wall",    0.00, 0.90),
    "terrain_grass":       (RHEP, "GrassTileRHEP",         0.00, 0.85),
    "terrain_sand":        (LGP3, "T_ground_sand_01",      0.00, 0.80),
    "terrain_snow":        (ADT,  "MarbleWhite00",         0.00, 0.88),
}

# ---------------------------------------------------------------------------
# EXPLICIT-PATH sets (mountain variation, 2026-08-13). Two reasons these are
# not rows in SETS: (1) D:\Assets has since become the content-addressed store
# (the named roots above are STALE — those sets are already published and only
# re-curate from a restored library); the browsable-by-name library now lives
# on Z:\. (2) the source packs use suffixes find() doesn't know (_D/_A/_BC,
# .tif/.tga/.jpg), so the files are named explicitly. Albedo-only sources are
# fine here: the terrain splat (mesh_terrain.glsl) samples ONLY the albedo;
# mr is synthesized flat, normal.png written only when the pack ships one.
#
# Picked by eyeballing a contact sheet of every rock/cliff candidate on Z:
# (UniStorm terrain, Top Down Post Apocalyptic terrain, Shatter Stone,
# Landscape Ground Pack 3, HQ Big Rock). Rejected: HQ Big Rock (a baked model
# skin, not tileable), Shatter Stone T_Stone_* (ground-with-chunks, reads as
# scatter not surface). Seam check: opposite-edge delta ~= interior delta for
# all five (worst lgp3_clay3 top/bottom 18.2 vs 7.4 interior — acceptable at
# splat tiling scales).
UNIST = r"Z:\UniStorm - Volumetric Clouds Sky Modular Weather and Cloud Shadows\UniStorm Weather System\Textures\Terrain"
TDPA  = r"Z:\Top Down Post Apocalyptic Pack\Top_Down_Post-Apocalyptic_Pack\Textures\Terrain"
LGP3Z = r"Z:\Landscape Ground Pack 3 Desert Dry Land Beach Sea Islands Coast\NatureManufacture Assets\Coast Environment\Ground\Textures"

# name -> (albedo path, normal-or-bump path or None, metal, rough)
SETS_EXPLICIT = {
    # THE second rock band: dark blue-grey craggy slate for the high mountain
    # (terrain.cpp registerTerrainMaterial rockHigh slot; the alpine vein tint
    # in mesh_terrain.glsl cools its crevices further).
    "terrain_rock_dark":  (os.path.join(UNIST, "Rock_2_D.tif"),  None, 0.00, 0.92),
    "terrain_rock_grey":  (os.path.join(UNIST, "Rock_1_D.jpg"),
                           os.path.join(UNIST, "Rock_1_Bump.png"), 0.00, 0.90),
    "terrain_scree":      (os.path.join(TDPA, "TD_Rocks_Ground_02_A.tga"),
                           os.path.join(TDPA, "TD_Rocks_Ground_02_N.tga"), 0.00, 0.90),
    "terrain_bluff_clay": (os.path.join(LGP3Z, "T_cliff_clay_01_BC.png"),
                           os.path.join(LGP3Z, "T_cliff_clay_01_N.png"), 0.00, 0.85),
    "terrain_bluff_dark": (os.path.join(LGP3Z, "T_cliff_clay_03_BC.png"),
                           os.path.join(LGP3Z, "T_cliff_clay_03_N.png"), 0.00, 0.85),
}

def load_rgb(p, maxdim=2048):
    im = Image.open(p).convert("RGB")
    if max(im.size) > maxdim:
        im = im.resize((min(im.width, maxdim), min(im.height, maxdim)), Image.LANCZOS)
    return im

def load_rgba(p, maxdim=2048):
    im = Image.open(p).convert("RGBA")
    if max(im.size) > maxdim:
        im = im.resize((min(im.width, maxdim), min(im.height, maxdim)), Image.LANCZOS)
    return im

def main():
    os.makedirs(LIB, exist_ok=True)
    # Optional set-name args curate selectively (the historical SETS roots are
    # stale now that D:\Assets is content-addressed, so a full run reports
    # them MISSING — that is expected, not an error).
    only = set(sys.argv[1:])
    report = []
    for name, (albp, nrmp, fm, fr) in SETS_EXPLICIT.items():
        if only and name not in only:
            continue
        if not os.path.exists(albp):
            report.append((name, "MISSING", albp)); continue
        outd = os.path.join(LIB, name); os.makedirs(outd, exist_ok=True)
        alb = load_rgb(albp); alb.save(os.path.join(outd, "albedo.png"))
        if nrmp and os.path.exists(nrmp):
            load_rgb(nrmp, maxdim=alb.width).save(os.path.join(outd, "normal.png"))
        w, hgt = alb.size
        out = np.zeros((hgt, w, 3), np.float32); out[..., 0] = 1.0; out[..., 1] = fr; out[..., 2] = fm
        Image.fromarray((out * 255).astype(np.uint8)).save(os.path.join(outd, "mr.png"))
        report.append((name, f"{alb.width}px flat R={fr} M={fm}"
                             + ("" if (nrmp and os.path.exists(nrmp)) else " (albedo-only)"), None))
    for name, (root, stem, fm, fr) in SETS.items():
        if only and name not in only:
            continue
        h = find(root, stem)
        if "albedo" not in h or "normal" not in h:
            report.append((name, "MISSING", h)); continue
        outd = os.path.join(LIB, name); os.makedirs(outd, exist_ok=True)
        alb = load_rgb(h["albedo"]); alb.save(os.path.join(outd, "albedo.png"))
        nrm = load_rgb(h["normal"]); nrm.save(os.path.join(outd, "normal.png"))
        conv = h.get("conv", "none")
        if conv == "orm":
            m = load_rgb(h["mask"], maxdim=alb.width)
            m.save(os.path.join(outd, "mr.png"))
            a = np.asarray(m, np.float32) / 255.0
            stats = f"orm pass  AO~{a[...,0].mean():.2f} R~{a[...,1].mean():.2f} M~{a[...,2].mean():.2f}"
        elif conv == "rma":
            # Measured convention for these packs (raw-channel fit, not the suffix):
            # R=roughness, G=metallic, B=AO. (TX_Modular_Wall_01a: R .38 G .00 B .98
            # = glossy painted plaster, non-metal, open AO — sane. The HDRP-MaskMap
            # guess produced 0-rough chrome walls.)
            m = np.asarray(load_rgba(h["mask"], maxdim=alb.width), np.float32) / 255.0
            out = np.stack([m[..., 2], m[..., 0], m[..., 1]], axis=-1)  # AO=B, rough=R, metal=G
            Image.fromarray((out * 255).astype(np.uint8)).save(os.path.join(outd, "mr.png"))
            stats = f"rma remap AO~{out[...,0].mean():.2f} R~{out[...,1].mean():.2f} M~{out[...,2].mean():.2f}"
        else:
            w, hgt = alb.size
            out = np.zeros((hgt, w, 3), np.float32); out[..., 0] = 1.0; out[..., 1] = fr; out[..., 2] = fm
            Image.fromarray((out * 255).astype(np.uint8)).save(os.path.join(outd, "mr.png"))
            stats = f"flat      R={fr} M={fm}"
        report.append((name, f"{alb.width}px {stats}", None))
    for n, s, extra in report:
        print(f"{n:22} {s}" + (f"  !!{extra}" if extra else ""))

if __name__ == "__main__":
    main()
