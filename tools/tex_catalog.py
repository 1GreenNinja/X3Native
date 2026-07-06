#!/usr/bin/env python3
"""Catalog PBR texture SETS across the local Unity-pack library (ART_BIBLE 4 realism
mandate). Groups albedo/normal/mask maps by directory+stem into sets, records the mask
channel convention implied by the suffix, and emits docs/tex_catalog.json + .md.

Filename zoo handled (case-insensitive, trailing-token match):
  albedo : basecolor, base_color, albedo, alb, bc, diff, diffuse, col, color, d
  normal : normal, normalmap, nrm, norm, n
  packed : rma, orm, mads, maskmap, mrao, metallicsmoothness, ms, mask
  single : metallic/metal/mt, roughness/rough/rgh, smoothness, ao/occlusion

Usage: python tools/tex_catalog.py [--roots D:/Assets D:/GameModels] [--out docs]
"""
import argparse
import json
import os
import re
import sys
from collections import defaultdict

IMG_EXT = {".png", ".tga", ".jpg", ".jpeg", ".tif", ".tiff", ".bmp"}
SKIP_DIR_TOKENS = ("editor", "gizmos", "ui", "icons", "icon", "sprites", "sprite",
                   "lightmap", "cubemap", "skybox", "lut", "fonts", "audio", "sounds")

# token -> (slot, convention_hint)
TOKENS = [
    # packed masks first (longest match wins)
    ("metallicsmoothness", ("mask", "unity_ms")),   # RGB=metal, A=smooth
    ("maskmap",            ("mask", "hdrp_mads")),  # R=metal G=AO B=detail A=smooth
    ("mads",               ("mask", "hdrp_mads")),
    ("mrao",               ("mask", "mrao")),        # R=metal G=rough B=ao (usually)
    ("rma",                ("mask", "rma")),         # R=rough G=metal A=ao (varies!)
    ("orm",                ("mask", "orm")),         # R=ao G=rough B=metal (glTF-friendly)
    ("mask",               ("mask", "unknown")),
    ("ms",                 ("mask", "unity_ms")),
    # normals
    ("normalmap",          ("normal", None)),
    ("normal",             ("normal", None)),
    ("nrm",                ("normal", None)),
    ("norm",               ("normal", None)),
    ("n",                  ("normal", None)),
    # albedo
    ("basecolor",          ("albedo", None)),
    ("base_color",         ("albedo", None)),
    ("albedo",             ("albedo", None)),
    ("alb",                ("albedo", None)),
    ("diffuse",            ("albedo", None)),
    ("diff",               ("albedo", None)),
    ("color",              ("albedo", None)),
    ("col",                ("albedo", None)),
    ("bc",                 ("albedo", None)),
    ("d",                  ("albedo", None)),
    # singles
    ("metallic",           ("metallic", None)),
    ("metal",              ("metallic", None)),
    ("mt",                 ("metallic", None)),
    ("roughness",          ("roughness", None)),
    ("rough",              ("roughness", None)),
    ("rgh",                ("roughness", None)),
    ("smoothness",         ("smoothness", None)),
    ("occlusion",          ("ao", None)),
    ("ao",                 ("ao", None)),
]

SPLIT_RE = re.compile(r"[ _\-.]+")

def classify(name_noext):
    """Return (stem, slot, hint) or None. Matches the TRAILING token only."""
    parts = SPLIT_RE.split(name_noext.lower())
    if len(parts) < 2:
        return None
    tail = parts[-1]
    # also try last-two joined (base_color)
    tail2 = "_".join(parts[-2:]) if len(parts) >= 2 else None
    for tok, (slot, hint) in TOKENS:
        if tail == tok or (tail2 is not None and tail2 == tok):
            n = 2 if (tail2 == tok and tail != tok) else 1
            stem = "_".join(parts[:-n])
            if stem:
                return stem, slot, hint
    return None

def png_size(path):
    try:
        from PIL import Image
        with Image.open(path) as im:
            return im.size
    except Exception:
        return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--roots", nargs="*", default=["D:/Assets", "D:/GameModels"])
    ap.add_argument("--out", default="docs")
    args = ap.parse_args()

    # (dirpath, stem) -> {slot: filename}
    groups = defaultdict(dict)
    hints = {}
    scanned = 0
    for root in args.roots:
        if not os.path.isdir(root):
            print(f"[skip] {root} not found", file=sys.stderr)
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            low = dirpath.lower()
            if any(t in SPLIT_RE.split(low.replace("\\", "/").replace("/", " ")) for t in SKIP_DIR_TOKENS):
                dirnames[:] = []
                continue
            for fn in filenames:
                ext = os.path.splitext(fn)[1].lower()
                if ext not in IMG_EXT:
                    continue
                scanned += 1
                c = classify(os.path.splitext(fn)[0])
                if not c:
                    continue
                stem, slot, hint = c
                key = (dirpath, stem)
                # keep the first file per slot (prefer png over tga implicitly by walk order)
                groups[key].setdefault(slot, fn)
                if hint:
                    hints[key] = hint

    # sets = albedo + normal at minimum
    sets = []
    for (dirpath, stem), slots in groups.items():
        if "albedo" not in slots or "normal" not in slots:
            continue
        rel_root = None
        pack = None
        for root in args.roots:
            rr = os.path.normpath(root)
            if os.path.normpath(dirpath).lower().startswith(rr.lower()):
                rel_root = rr
                rest = os.path.normpath(dirpath)[len(rr):].lstrip("\\/")
                pack = rest.split(os.sep)[0] if rest else os.path.basename(rr)
                break
        entry = {
            "dir": dirpath,
            "stem": stem,
            "pack": pack or "?",
            "files": slots,
            "maskConvention": hints.get((dirpath, stem), ("none" if "mask" not in slots else "unknown")),
        }
        sz = png_size(os.path.join(dirpath, slots["albedo"]))
        if sz:
            entry["albedoSize"] = list(sz)
        sets.append(entry)

    os.makedirs(args.out, exist_ok=True)
    jpath = os.path.join(args.out, "tex_catalog.json")
    with open(jpath, "w", encoding="utf-8") as f:
        json.dump({"scannedImages": scanned, "sets": sets}, f, indent=1)

    per_pack = defaultdict(int)
    for s in sets:
        per_pack[s["pack"]] += 1
    mpath = os.path.join(args.out, "tex_catalog.md")
    with open(mpath, "w", encoding="utf-8") as f:
        f.write(f"# Texture-set catalog\n\nImages scanned: {scanned}\n")
        f.write(f"PBR sets (albedo+normal minimum): {len(sets)}\n\n| pack | sets |\n|---|---|\n")
        for pack, n in sorted(per_pack.items(), key=lambda kv: -kv[1]):
            f.write(f"| {pack} | {n} |\n")
    print(f"[tex_catalog] {scanned} images scanned, {len(sets)} sets -> {jpath}")

if __name__ == "__main__":
    main()
