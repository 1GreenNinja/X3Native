# inject_billboard_tex.py — one-off GLB surgery (W-FOREST v2, 2026-08-16).
#
# WHY: assets/converted_glb/nature/{OakBigTree01,PoplarTree001}.glb ship a far-LOD
# "Billboard" cross-card node (two perpendicular quads, atlas UVs already
# authored) whose M_Billboard material is an UNTEXTURED 0.8-grey stand-in —
# which is why road_trees.cpp skips it (NO_SLOP rule 3: no untextured stand-ins
# ship). The source Unity packs DO ship the real billboard atlases:
#   D:\Assets\Big Oak Tree FREE\...\Textures\BigOakBillboard01.tif
#     3192x1024, THREE frames (side A / side B / top view), RGB on a (51,51,51)
#     background — alpha must be KEYED (colour distance from the background).
#   D:\Assets\Big Poplar Tree FREE\...\Textures\PoplarTree001_Billboard.tga
#     2048x1024, FOUR side frames, real alpha channel.
# The card UVs in the GLBs index frames of exactly these atlases (verified:
# oak card1 u 0.000-0.332 / card2 u 0.331-0.663 of the 3-frame sheet; poplar
# card1 u 0.253-0.499 / card2 u 0.499-0.747 of the 4-frame sheet).
#
# WHAT: embed the atlas as a PNG image in the GLB and point M_Billboard at it:
#   baseColorTexture = the atlas, alphaMode MASK (cutoff 0.5), doubleSided,
#   baseColorFactor 1. Geometry untouched. Idempotent (re-running replaces the
# material's texture reference; the old image becomes dead weight only if run
# twice on the same file — run on the pristine store copy).
#
# AFTER RUNNING: python tools/asset_store.py publish   (store-served dir —
# never git-commit the GLBs; see docs/ENGINE_GOTCHAS.md 2.5)
import json
import struct
import sys
from io import BytesIO

from PIL import Image

OAK_GLB = "assets/converted_glb/nature/OakBigTree01.glb"
POP_GLB = "assets/converted_glb/nature/PoplarTree001.glb"
OAK_TEX = r"D:\Assets\Big Oak Tree FREE\Assets\ALP_Assets\Big Oak Tree FREE\Models\Textures\BigOakBillboard01.tif"
POP_TEX = r"D:\Assets\Big Poplar Tree FREE\Assets\ALP_Assets\Poplar Tree FREE\Models\Textures\PoplarTree001_Billboard.tga"


def load_glb(path):
    with open(path, "rb") as f:
        magic, ver, _length = struct.unpack("<III", f.read(12))
        assert magic == 0x46546C67, path
        clen, ctype = struct.unpack("<II", f.read(8))
        assert ctype == 0x4E4F534A
        gltf = json.loads(f.read(clen))
        blen, btype = struct.unpack("<II", f.read(8))
        assert btype == 0x004E4942
        binblob = f.read(blen)
    return gltf, bytearray(binblob)


def save_glb(path, gltf, binblob):
    while len(binblob) % 4:
        binblob.append(0)
    gltf["buffers"][0]["byteLength"] = len(binblob)
    js = json.dumps(gltf, separators=(",", ":")).encode()
    while len(js) % 4:
        js += b" "
    total = 12 + 8 + len(js) + 8 + len(binblob)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(js), 0x4E4F534A))
        f.write(js)
        f.write(struct.pack("<II", len(binblob), 0x004E4942))
        f.write(binblob)


def oak_rgba(path):
    """Key alpha by BORDER FLOOD FILL, not colour distance.

    The atlas is RGB on a (51,51,51) backdrop. Naive colour-distance keying
    fails: the crown's dark shadow greens sit within ~20 of the backdrop grey,
    so the key eats the middle of the tree (verified eyes-on, first attempt).
    The backdrop is the connected region touching the image border — flood
    from the border across near-backdrop pixels, key only what the flood
    reaches, then ERODE the opaque mask ~7 px to trim the baked soft-gauze
    halo (green-over-grey blends the flood cannot reach but that read as a
    puffy grey fringe on the card).
    """
    import numpy as np
    im = Image.open(path).convert("RGB")
    a = np.asarray(im).astype(np.int16)
    # The backdrop is a NEUTRAL grey-to-black vertical gradient (51,51,51 at the
    # top corners, near-black along the bottom), so distance-to-one-colour
    # cannot key it. Backdrop candidates = near-neutral (channel spread <= 10)
    # AND dark (max channel <= 64). The trunk's browns carry ~15 of r-b spread
    # and the foliage is saturated green — both excluded.
    mx = a.max(axis=2)
    mn = a.min(axis=2)
    cand = ((mx - mn) <= 10) & (mx <= 64)
    # flood fill from the border across `cand` (8-connected, BFS via dilation)
    bg = np.zeros_like(cand)
    bg[0, :] = cand[0, :]
    bg[-1, :] = cand[-1, :]
    bg[:, 0] = cand[:, 0]
    bg[:, -1] = cand[:, -1]
    while True:
        grown = bg.copy()
        grown[1:, :] |= bg[:-1, :]
        grown[:-1, :] |= bg[1:, :]
        grown[:, 1:] |= bg[:, :-1]
        grown[:, :-1] |= bg[:, 1:]
        grown &= cand
        if (grown == bg).all():
            break
        bg = grown
    opaque = ~bg
    # erode ~5 px: shave the gauze ring (min-filter via iterative 4-neighbour AND)
    for _ in range(5):
        er = opaque.copy()
        er[1:, :] &= opaque[:-1, :]
        er[:-1, :] &= opaque[1:, :]
        er[:, 1:] &= opaque[:, :-1]
        er[:, :-1] &= opaque[:, 1:]
        opaque = er
    rgba = np.dstack([a.astype(np.uint8), np.where(opaque, 255, 0).astype(np.uint8)])
    return Image.fromarray(rgba, "RGBA")


def pop_rgba(path):
    return Image.open(path).convert("RGBA")


def inject(glb_path, rgba, max_w):
    if rgba.size[0] > max_w:
        h = rgba.size[1] * max_w // rgba.size[0]
        rgba = rgba.resize((max_w, h), Image.LANCZOS)
    buf = BytesIO()
    rgba.save(buf, "PNG", optimize=True)
    png = buf.getvalue()

    gltf, blob = load_glb(glb_path)
    # find M_Billboard
    mi = next(i for i, m in enumerate(gltf["materials"]) if m.get("name") == "M_Billboard")

    off = len(blob)
    while off % 4:
        blob.append(0)
        off += 1
    blob.extend(png)
    gltf.setdefault("bufferViews", []).append(
        {"buffer": 0, "byteOffset": off, "byteLength": len(png)})
    bv = len(gltf["bufferViews"]) - 1
    gltf.setdefault("images", []).append({"mimeType": "image/png", "bufferView": bv,
                                          "name": "BillboardAtlas"})
    img = len(gltf["images"]) - 1
    # reuse sampler 0 if present (the LOD0 textures'), else add one
    if not gltf.get("samplers"):
        gltf["samplers"] = [{"magFilter": 9729, "minFilter": 9987,
                             "wrapS": 10497, "wrapT": 10497}]
    gltf.setdefault("textures", []).append({"sampler": 0, "source": img})
    tex = len(gltf["textures"]) - 1

    mat = gltf["materials"][mi]
    pbr = mat.setdefault("pbrMetallicRoughness", {})
    pbr["baseColorTexture"] = {"index": tex, "texCoord": 0}
    pbr["baseColorFactor"] = [1.0, 1.0, 1.0, 1.0]
    pbr["metallicFactor"] = 0.0
    pbr["roughnessFactor"] = 1.0
    mat["alphaMode"] = "MASK"
    mat["alphaCutoff"] = 0.5
    mat["doubleSided"] = True

    save_glb(glb_path, gltf, blob)
    print(f"{glb_path}: atlas {rgba.size[0]}x{rgba.size[1]} png {len(png)//1024} KB -> material {mi} (M_Billboard)")


if __name__ == "__main__":
    inject(OAK_GLB, oak_rgba(OAK_TEX), 2048)
    inject(POP_GLB, pop_rgba(POP_TEX), 2048)
    print("done — now: python tools/asset_store.py publish")
