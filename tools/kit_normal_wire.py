#!/usr/bin/env python3
"""Phase 1 normal-map burn-down, batch 2: wire the sci-fi kits' REAL normal
atlases into the engine's kit GLBs.

The 2026-08-25 audit (docs/plans/NORMAL_MAP_AUDIT_0825.md) found 63 kit GLBs
shipping flat. The source packs ship real `*_Norm.png` atlases beside the
diffuse atlases the whole time — the original conversion just never wired
them. This tool matches by MATERIAL NAME: material `Wall_Atlas_08_Dif` wants
`Wall_Atlas_08_Norm.png`, the same naming law town_assets.py leans on.

Derived normals are NOT generated here on purpose: these are real authored
atlases, and where no atlas exists (e.g. `Material #89798`) the material
stays flat rather than getting invented bumps.

USAGE
  python tools/kit_normal_wire.py           # wire all four kit dirs, report
  python tools/kit_normal_wire.py --dry     # report matches, write nothing
"""
from __future__ import annotations
import json
import os
import struct
import sys
from io import BytesIO
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KIT_DIRS = ["SciFiKit3", "ScifiKitVol2Decoded", "ScifiKitVol3Decoded",
            "SciFi_Warehouse_Kit", "rifthub"]
SOURCE_ROOTS = [
    r"D:\Assets\3D Scifi Kit Starter Kit",
    r"D:\Assets\3D Scifi Kit Vol 2",
    r"D:\Assets\Scifi Modular Interior Space Station",
    r"\\p13700\G\Assets\3D Scifi Kit Vol 3",
]
MAX_DIM = 1024


def index_normals():
    idx = {}
    for root in SOURCE_ROOTS:
        if not os.path.isdir(root):
            print(f"  ! source root missing, skipped: {root}")
            continue
        for dp, _dn, fn in os.walk(root):
            for f in fn:
                low = f.lower()
                if low.endswith("_norm.png") or low.endswith("_normal.png"):
                    stem = f[: low.rfind("_")].lower()
                    idx.setdefault(stem, os.path.join(dp, f))
    return idx


def stems_for(mat_name):
    """Candidate atlas stems for a material name (lowered)."""
    n = mat_name.lower().strip()
    out = [n]
    if n.startswith("m_"):          # Vol 2's `M_<mesh name>` material convention
        out.append(n[2:])
    for base in list(out):
        for suf in ("_dif", "_diffuse", "_d", "_flat", "_albedo"):
            if base.endswith(suf):
                out.append(base[: -len(suf)])
    return out


def encode_normal(path):
    im = Image.open(path).convert("RGB")
    if max(im.size) > MAX_DIM:
        s = MAX_DIM / max(im.size)
        im = im.resize((max(1, int(im.width * s)), max(1, int(im.height * s))), Image.LANCZOS)
    buf = BytesIO()
    im.save(buf, "PNG", optimize=True)
    return buf.getvalue()


def read_glb(path):
    d = open(path, "rb").read()
    jlen = struct.unpack_from("<I", d, 12)[0]
    gltf = json.loads(d[20:20 + jlen])
    boff = 20 + jlen + 8
    bin_ = bytearray(d[boff:]) if len(d) > boff else bytearray()
    return gltf, bin_


def write_glb(path, gltf, bin_):
    while len(bin_) % 4:
        bin_.append(0)
    if gltf.get("buffers"):
        gltf["buffers"][0]["byteLength"] = len(bin_)
    js = json.dumps(gltf, separators=(",", ":")).encode()
    while len(js) % 4:
        js += b" "
    total = 12 + 8 + len(js) + 8 + len(bin_)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(js), 0x4E4F534A)); f.write(js)
        f.write(struct.pack("<II", len(bin_), 0x004E4942)); f.write(bytes(bin_))


def wire(path, idx, dry):
    gltf, bin_ = read_glb(path)
    mats = gltf.get("materials", [])
    cache, wired, unmatched = {}, 0, []
    for mat in mats:
        if "normalTexture" in mat:
            continue
        name = mat.get("name", "")
        hit = None
        for s in stems_for(name):
            if s in idx:
                hit = idx[s]
                break
        if hit is None:
            unmatched.append(name or "<unnamed>")
            continue
        if dry:
            wired += 1
            continue
        if hit not in cache:
            raw = encode_normal(hit)
            while len(bin_) % 4:
                bin_.append(0)
            off = len(bin_)
            bin_ += raw
            gltf.setdefault("bufferViews", []).append(
                {"buffer": 0, "byteOffset": off, "byteLength": len(raw)})
            gltf.setdefault("images", []).append(
                {"bufferView": len(gltf["bufferViews"]) - 1, "mimeType": "image/png"})
            gltf.setdefault("textures", []).append(
                {"source": len(gltf["images"]) - 1})
            cache[hit] = len(gltf["textures"]) - 1
        mat["normalTexture"] = {"index": cache[hit], "texCoord": 0}
        wired += 1
    if wired and not dry:
        write_glb(path, gltf, bin_)
    return wired, len(mats), unmatched


def main():
    dry = "--dry" in sys.argv
    idx = index_normals()
    print(f"[kit-nrm] indexed {len(idx)} normal atlases across {len(SOURCE_ROOTS)} packs")
    total_glb, total_wired = 0, 0
    for kd in KIT_DIRS:
        d = os.path.join(ROOT, "assets", "converted_glb", kd)
        if not os.path.isdir(d):
            continue
        for f in sorted(os.listdir(d)):
            if not f.endswith(".glb"):
                continue
            p = os.path.join(d, f)
            wired, nmats, unmatched = wire(p, idx, dry)
            total_glb += 1
            total_wired += 1 if wired else 0
            tag = "WIRED" if wired else "  no match"
            extra = f"  (unmatched: {', '.join(unmatched[:3])})" if unmatched and wired == 0 else ""
            print(f"  {tag} {kd}/{f}: {wired}/{nmats} materials{extra}")
    print(f"[kit-nrm] {'DRY: would wire' if dry else 'wired'} {total_wired}/{total_glb} GLBs")


if __name__ == "__main__":
    main()
