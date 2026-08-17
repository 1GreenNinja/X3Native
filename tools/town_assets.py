#!/usr/bin/env python3
"""W-TOWN asset pipeline: armory prefabs -> assets/converted_glb/Town/.

WHY THIS EXISTS (the receipt, NO_SLOP rule 10)
----------------------------------------------
The Small Mountain Town is built from the HouseForge kit in Tim's armory
(``D:/Assets/_glb/prefab_buildings/HouseForge``) — whole, curated, PBR-textured
BUILDINGS rather than modular wall panels, which is what
``.claude/skills/x3native-environments`` means by mining a pack for curation
instead of scattering a lattice.

Those armory GLBs are baked "WebP+Draco, node-safe optimized"
(``D:/Assets/_glb/prefab_buildings/PREFAB_MANIFEST.md``). Both encodings are a
problem for X3Native, and only one of them is obvious:

* ``KHR_draco_mesh_compression`` — the engine DOES decode this
  (``engine/asset/ModelLoader.cpp`` links draco), but a decoded file removes a
  whole class of doubt and costs nothing at runtime.
* ``EXT_texture_webp`` — **there is no WebP decoder anywhere in the tree.**
  ``grep -rn "webp" engine/ app/`` returns nothing but this file's siblings.
  Every one of these prefabs would therefore have loaded with zero textures and
  rendered flat grey: NO_SLOP rule 3, "no untextured stand-ins in a shipped
  build". Caught by inspecting the GLB, not by shipping it.

``gltf-transform copy`` strips draco cleanly. Its ``png`` command FAILS on these
files (libvips: ``value "32" ... invalid for property 'space' of type
'VipsInterpretation'``), so the image pass is done here with Pillow.

USAGE
-----
    python tools/town_assets.py convert     # armory -> assets/converted_glb/Town
    python tools/town_assets.py report      # the measured table app/town.cpp holds

``report`` prints, per prefab, the numbers ``kAssets[]`` in ``app/town.cpp``
carries: bbox centre in asset space (these prefabs are NOT centred on their
origin), footprint half extents, the vertical span about the origin plane (loY
is negative — the kit ships a foundation below the ground-floor origin), and
the MEASURED front direction, taken as bbox-centre -> named ``SM_*_Door*`` node.
X3_WORLD_RULES rule 0 says verify, never assume; rule 3 says orientation is
documented per asset. This is that verification and that document, and
``docs/design/TOWN_MANIFEST.md`` repeats it in prose. If the kit is re-baked,
re-run ``report`` and update the table — PAIRED VALUES ARE ONE VALUE
(NO_SLOP rule 4).

Deps: Pillow, numpy, and node/npx for @gltf-transform/cli.
"""
from __future__ import annotations

import glob
import io
import json
import math
import os
import struct
import subprocess
import sys

SRC = r"D:/Assets/_glb/prefab_buildings/HouseForge"
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "assets", "converted_glb", "Town")
STAGE = os.path.join(os.environ.get("TEMP", "/tmp"), "x3_town_stage")

# The curated selection. PF_PrimitiveHouse01/02 are DELIBERATELY absent: their
# one untextured material is a wall / roof end, and a 0.8-grey slab facing main
# street is NO_SLOP rule 3 in the flesh. 03/04's untextured material is a roof
# overhang — small, high, and patched by app/town.cpp's MaterialOverride.
PREFABS = [
    "PF_StoneHouse01_JustBuilding", "PF_StoneHouse01_WithSetDressing",
    "PF_StoneHouse02",
    "PF_WoodenHouse01_JustBuilding", "PF_WoodenHouse02", "PF_WoodenHouse03",
    "PF_PrimitiveHouse01", "PF_PrimitiveHouse02",
    "PF_PrimitiveHouse03", "PF_PrimitiveHouse04",
    "PF_StorageMarket_01a", "PF_StorageMarket_01c",
    "PF_StorageMarket_01e", "PF_StorageMarket_01f",
    "PF_WoodCart_01a", "PF_WoodCart_02a",
    "PF_WoodLightTorch_01a", "PF_WoodLightTorch_01b",
    "PF_WoodStorage_01b", "PF_WoodStorage_01c", "PF_StockageWood_01a",
]

MAX_DIM = 1024   # VRAM: a 23-material building at 4K is ~1 GB of atlas


# --------------------------------------------------------------------------- glb io
def read_glb(p):
    with open(p, "rb") as f:
        magic, _ver, total = struct.unpack("<4sII", f.read(12))
        if magic != b"glTF":
            raise ValueError(f"{p}: not a GLB")
        js, bin_ = None, b""
        while f.tell() < total:
            ln, ty = struct.unpack("<I4s", f.read(8))
            data = f.read(ln)
            if ty == b"JSON":
                js = json.loads(data)
            elif ty == b"BIN\x00":
                bin_ = data
        return js, bin_


def write_glb(p, js, bin_):
    jb = json.dumps(js, separators=(",", ":")).encode("utf-8")
    jb += b" " * ((4 - len(jb) % 4) % 4)
    bb = bin_ + b"\x00" * ((4 - len(bin_) % 4) % 4)
    total = 12 + 8 + len(jb) + (8 + len(bb) if bb else 0)
    with open(p, "wb") as f:
        f.write(struct.pack("<4sII", b"glTF", 2, total))
        f.write(struct.pack("<I4s", len(jb), b"JSON"))
        f.write(jb)
        if bb:
            f.write(struct.pack("<I4s", len(bb), b"BIN\x00"))
            f.write(bb)


def unwebp(path):
    """Transcode EXT_texture_webp images to PNG (alpha) / JPEG (opaque)."""
    from PIL import Image
    js, bin_ = read_glb(path)
    if "EXT_texture_webp" not in (js.get("extensionsUsed") or []):
        return "no webp"

    views = js.get("bufferViews", [])
    out = bytearray()
    newviews = []

    def add_view(payload):
        while len(out) % 4:
            out.append(0)
        off = len(out)
        out.extend(payload)
        newviews.append({"buffer": 0, "byteOffset": off, "byteLength": len(payload)})
        return len(newviews) - 1

    imgviews = {im["bufferView"] for im in js.get("images", []) if "bufferView" in im}
    remap = {}
    for i, v in enumerate(views):
        if i in imgviews:
            continue
        off = v.get("byteOffset", 0)
        ni = add_view(bin_[off:off + v["byteLength"]])
        for k in ("byteStride", "target"):
            if k in v:
                newviews[ni][k] = v[k]
        remap[i] = ni

    for im in js.get("images", []):
        v = views[im["bufferView"]]
        off = v.get("byteOffset", 0)
        img = Image.open(io.BytesIO(bin_[off:off + v["byteLength"]]))
        img.load()
        if max(img.size) > MAX_DIM:
            sc = MAX_DIM / max(img.size)
            img = img.resize((max(1, int(img.width * sc)), max(1, int(img.height * sc))),
                             Image.LANCZOS)
        alpha = img.mode in ("RGBA", "LA") and img.getextrema()[-1][0] < 255
        buf = io.BytesIO()
        if alpha:
            img.convert("RGBA").save(buf, "PNG", optimize=True)
            im["mimeType"] = "image/png"
        else:
            img.convert("RGB").save(buf, "JPEG", quality=90)
            im["mimeType"] = "image/jpeg"
        im["bufferView"] = add_view(buf.getvalue())

    for a in js.get("accessors", []):
        if "bufferView" in a:
            a["bufferView"] = remap[a["bufferView"]]
        sp = a.get("sparse")
        if sp:
            for k in ("indices", "values"):
                if k in sp:
                    sp[k]["bufferView"] = remap[sp[k]["bufferView"]]

    for t in js.get("textures", []):
        ext = t.get("extensions", {}).pop("EXT_texture_webp", None)
        if ext:
            t["source"] = ext["source"]
        if not t.get("extensions"):
            t.pop("extensions", None)
    for key in ("extensionsUsed", "extensionsRequired"):
        if key in js:
            js[key] = [e for e in js[key] if e != "EXT_texture_webp"]
            if not js[key]:
                del js[key]

    js["bufferViews"] = newviews
    js["buffers"] = [{"byteLength": len(out)}]
    write_glb(path, js, bytes(out))
    return f"{len(js.get('images', []))} images"


# --------------------------------------------------------------------------- convert
def convert():
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(STAGE, exist_ok=True)
    bad = 0
    for n in PREFABS:
        src = os.path.join(SRC, n + ".glb")
        if not os.path.exists(src):
            print("MISSING", src)
            bad += 1
            continue
        mid = os.path.join(STAGE, n + ".mid.glb")
        dst = os.path.join(OUT, n + ".glb")
        r = subprocess.run(f'npx --yes @gltf-transform/cli copy "{src}" "{mid}"',
                           shell=True, capture_output=True, text=True)
        if r.returncode != 0:
            print("DRACO DECODE FAILED", n, (r.stderr or "")[-300:])
            bad += 1
            continue
        with open(mid, "rb") as a, open(dst, "wb") as b:
            b.write(a.read())
        print(f"{n:40s} {unwebp(dst):>14s}  {os.path.getsize(dst) // 1024:6d} KB")
    verify()
    return bad


def verify():
    """Every shipped GLB must require NO extension the engine cannot read."""
    fail = 0
    for p in sorted(glob.glob(OUT + "/*.glb")):
        js, _ = read_glb(p)
        req = js.get("extensionsRequired", [])
        mimes = sorted({i.get("mimeType", "?") for i in js.get("images", [])})
        if req or any(m not in ("image/png", "image/jpeg") for m in mimes):
            print(f"  FAIL {os.path.basename(p)}: requires {req}, mimes {mimes}")
            fail += 1
    print(f"verify: {fail} file(s) still carry an extension/mime the loader cannot read")
    return fail


# --------------------------------------------------------------------------- report
def _trs(n):
    import numpy as np
    if "matrix" in n:
        return np.array(n["matrix"], float).reshape(4, 4).T
    M = np.eye(4)
    s = n.get("scale", [1, 1, 1])
    r = n.get("rotation", [0, 0, 0, 1])
    t = n.get("translation", [0, 0, 0])
    x, y, z, w = r
    M[:3, :3] = np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ]) @ np.diag(s)
    M[:3, 3] = t
    return M


def report():
    import numpy as np
    print("// name, cx, cz, hx, hz, loY, hiY, frontDeg   |  untextured materials")
    for p in sorted(glob.glob(OUT + "/*.glb")):
        js, _ = read_glb(p)
        parts = []

        def walk(ni, M):
            n = js["nodes"][ni]
            W = M @ _trs(n)
            if "mesh" in n:
                lo = np.array([1e30] * 3)
                hi = np.array([-1e30] * 3)
                for pr in js["meshes"][n["mesh"]]["primitives"]:
                    ai = pr.get("attributes", {}).get("POSITION")
                    if ai is None:
                        continue
                    a = js["accessors"][ai]
                    if "min" not in a:
                        continue
                    for cx in (a["min"][0], a["max"][0]):
                        for cy in (a["min"][1], a["max"][1]):
                            for cz in (a["min"][2], a["max"][2]):
                                v = W @ np.array([cx, cy, cz, 1.0])
                                lo = np.minimum(lo, v[:3])
                                hi = np.maximum(hi, v[:3])
                if lo[0] < 1e29:
                    parts.append((n.get("name", ""), (lo + hi) * 0.5, lo, hi))
            for c in n.get("children", []):
                walk(c, W)

        for sc in js.get("scenes", [{}]):
            for ni in sc.get("nodes", []):
                walk(ni, np.eye(4))
        if not parts:
            continue
        lo = np.min([q[2] for q in parts], axis=0)
        hi = np.max([q[3] for q in parts], axis=0)
        ctr = (lo + hi) * 0.5
        half = (hi - lo) * 0.5
        doors = [q for q in parts if "door" in q[0].lower()]
        if doors:
            v = max((q[1] - ctr for q in doors),
                    key=lambda a: a[0] * a[0] + a[2] * a[2])
            front = math.degrees(math.atan2(-v[0], -v[2]))
        else:
            front = 0.0
        untex = [m.get("name", "?") for m in js.get("materials", [])
                 if "baseColorTexture" not in m.get("pbrMetallicRoughness", {})]
        print(f'    {{ "Town/{os.path.basename(p)}", {ctr[0]:7.2f}f, {ctr[2]:7.2f}f, '
              f'{half[0]:6.2f}f, {half[2]:6.2f}f, {lo[1]:6.2f}f, {hi[1]:6.2f}f, '
              f'{front:7.1f}f }},   // {",".join(untex) or "fully textured"}')


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "report"
    if cmd == "convert":
        sys.exit(1 if convert() else 0)
    elif cmd == "verify":
        sys.exit(1 if verify() else 0)
    else:
        report()
