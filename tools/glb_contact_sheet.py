#!/usr/bin/env python3
"""Render a textured 3/4 view of every GLB in a directory into one contact sheet.

WHY: X3_WORLD_RULES rule 0 — "before any authored asset ships into a scene it is
rendered and LOOKED AT". The repo's existing eye-gate (`tools/preview_glb.py`)
needs Blender, and the MS-Store Blender launcher DETACHES (see anim_build.ps1),
which makes it awkward to use inside an agent turn. This is a dependency-free
software rasteriser: numpy + Pillow, no GPU, no Blender, a few seconds for a
whole kit. It is deliberately crude — flat-shaded with the real baseColor
TEXTURE sampled per pixel — because the question it answers is not "is the
lighting right" but "is this asset actually textured, the right shape, and the
right way up", which is exactly the question that shipped a ruined-village kit
as a town.

    python tools/glb_contact_sheet.py <dir-or-glb> <out.png> [--yaw 35] [--cols 4]
"""
from __future__ import annotations

import argparse
import glob
import json
import math
import os
import struct
from io import BytesIO

import numpy as np
from PIL import Image, ImageDraw

COMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}
DT = {5126: np.float32, 5123: np.uint16, 5125: np.uint32, 5121: np.uint8, 5122: np.int16, 5120: np.int8}


def load_glb(path):
    with open(path, "rb") as f:
        magic, _v, _l = struct.unpack("<III", f.read(12))
        assert magic == 0x46546C67, path
        clen, _ct = struct.unpack("<II", f.read(8))
        gltf = json.loads(f.read(clen))
        blen, _bt = struct.unpack("<II", f.read(8))
        return gltf, f.read(blen)


def acc(gltf, bin_, i):
    a = gltf["accessors"][i]
    n = COMP[a["type"]]
    bv = gltf["bufferViews"][a["bufferView"]]
    off = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
    stride = bv.get("byteStride")
    dt = DT[a["componentType"]]
    if stride and stride != n * np.dtype(dt).itemsize:
        raw = np.frombuffer(bin_, dtype=np.uint8, count=stride * a["count"], offset=off)
        raw = raw.reshape(a["count"], stride)[:, : n * np.dtype(dt).itemsize]
        out = np.ascontiguousarray(raw).view(dt).reshape(a["count"], n)
    else:
        out = np.frombuffer(bin_, dtype=dt, count=a["count"] * n, offset=off).reshape(a["count"], n)
    return out.astype(np.float32) if a["componentType"] == 5126 else out


def node_world(gltf):
    """Compose each node's world matrix (these kits are shallow, but do it right)."""
    mats = {}

    def trs(nd):
        if "matrix" in nd:
            return np.array(nd["matrix"], dtype=np.float64).reshape(4, 4).T
        m = np.eye(4)
        if "scale" in nd:
            m = np.diag(list(nd["scale"]) + [1.0]) @ m
        if "rotation" in nd:
            x, y, z, w = nd["rotation"]
            r = np.array([
                [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 0],
                [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), 0],
                [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), 0],
                [0, 0, 0, 1]])
            m = r @ m
        if "translation" in nd:
            t = np.eye(4); t[:3, 3] = nd["translation"]; m = t @ m
        return m

    def walk(i, parent):
        nd = gltf["nodes"][i]
        w = parent @ trs(nd)
        mats[i] = w
        for c in nd.get("children", []):
            walk(c, w)

    roots = gltf["scenes"][gltf.get("scene", 0)]["nodes"] if gltf.get("scenes") else range(len(gltf.get("nodes", [])))
    for r in roots:
        walk(r, np.eye(4))
    return mats


def material_image(gltf, bin_, mi):
    """Return (np RGB float array, baseColorFactor) for a material."""
    fac = np.array([0.75, 0.75, 0.75])
    if mi is None:
        return None, fac
    mat = gltf["materials"][mi]
    pbr = mat.get("pbrMetallicRoughness", {})
    if "baseColorFactor" in pbr:
        fac = np.array(pbr["baseColorFactor"][:3], dtype=np.float64)
    bct = pbr.get("baseColorTexture")
    if bct is None:
        return None, fac
    tex = gltf["textures"][bct["index"]]
    img = gltf["images"][tex["source"]]
    if img.get("bufferView") is None:
        return None, fac
    bv = gltf["bufferViews"][img["bufferView"]]
    raw = bin_[bv["byteOffset"]: bv["byteOffset"] + bv["byteLength"]]
    try:
        im = Image.open(BytesIO(raw)).convert("RGB")
    except Exception:
        return None, fac
    if max(im.size) > 512:
        im.thumbnail((512, 512))
    return np.asarray(im, dtype=np.float64) / 255.0, fac


def render(path, size=460, yaw_deg=35.0, pitch_deg=22.0):
    gltf, bin_ = load_glb(path)
    mats = node_world(gltf)
    tris = []          # (v0,v1,v2, uv0,uv1,uv2, matindex)
    lo = np.array([9e9] * 3); hi = np.array([-9e9] * 3)
    for ni, nd in enumerate(gltf.get("nodes", [])):
        if "mesh" not in nd:
            continue
        M = mats.get(ni, np.eye(4))
        for pr in gltf["meshes"][nd["mesh"]]["primitives"]:
            if pr.get("mode", 4) != 4:
                continue
            pos = acc(gltf, bin_, pr["attributes"]["POSITION"]).astype(np.float64)
            pos = (M[:3, :3] @ pos.T).T + M[:3, 3]
            uv = (acc(gltf, bin_, pr["attributes"]["TEXCOORD_0"]).astype(np.float64)
                  if "TEXCOORD_0" in pr["attributes"] else np.zeros((len(pos), 2)))
            idx = (acc(gltf, bin_, pr["indices"]).reshape(-1).astype(np.int64)
                   if "indices" in pr else np.arange(len(pos)))
            lo = np.minimum(lo, pos.min(0)); hi = np.maximum(hi, pos.max(0))
            tris.append((pos, uv, idx, pr.get("material")))
    if not tris:
        return None, (0, 0, 0)
    ctr = (lo + hi) * 0.5
    ext = float(np.linalg.norm(hi - lo))
    # camera basis
    y, p = math.radians(yaw_deg), math.radians(pitch_deg)
    fwd = np.array([math.sin(y) * math.cos(p), -math.sin(p), math.cos(y) * math.cos(p)])
    eye = ctr - fwd * ext * 1.15
    up0 = np.array([0.0, 1.0, 0.0])
    right = np.cross(fwd, up0); right /= np.linalg.norm(right)
    up = np.cross(right, fwd)
    sun = np.array([0.45, 0.8, 0.35]); sun /= np.linalg.norm(sun)

    S = size
    zbuf = np.full((S, S), 1e18)
    col = np.zeros((S, S, 3))
    # ground-plane tint so a dark asset is still readable against the sheet
    col[:, :] = np.array([0.42, 0.47, 0.55])
    scale = S / (ext * 1.05)

    for pos, uv, idx, mi in tris:
        tex, fac = material_image(gltf, bin_, mi)
        rel = pos - eye
        cx = rel @ right; cy = rel @ up; cz = rel @ fwd
        sx = S * 0.5 + cx * scale
        sy = S * 0.5 - cy * scale
        tri = idx.reshape(-1, 3)
        # cull tris fully behind or offscreen fast
        for a, b, c in tri:
            if cz[a] <= 0.01 or cz[b] <= 0.01 or cz[c] <= 0.01:
                continue
            x0, y0, x1, y1, x2, y2 = sx[a], sy[a], sx[b], sy[b], sx[c], sy[c]
            minx = int(max(0, math.floor(min(x0, x1, x2)))); maxx = int(min(S - 1, math.ceil(max(x0, x1, x2))))
            miny = int(max(0, math.floor(min(y0, y1, y2)))); maxy = int(min(S - 1, math.ceil(max(y0, y1, y2))))
            if minx > maxx or miny > maxy:
                continue
            area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
            if abs(area) < 1e-9:
                continue
            n = np.cross(pos[b] - pos[a], pos[c] - pos[a])
            nl = np.linalg.norm(n)
            if nl < 1e-12:
                continue
            n = n / nl
            lam = 0.35 + 0.65 * abs(float(n @ sun))
            xs = np.arange(minx, maxx + 1)
            ys = np.arange(miny, maxy + 1)
            px, py = np.meshgrid(xs + 0.5, ys + 0.5)
            w0 = ((x1 - x0) * (py - y0) - (px - x0) * (y1 - y0)) / area
            w1 = ((px - x0) * (y2 - y0) - (x2 - x0) * (py - y0)) / area
            w2 = 1.0 - w0 - w1
            m = (w0 >= -1e-6) & (w1 >= -1e-6) & (w2 >= -1e-6)
            if not m.any():
                continue
            z = w2 * cz[a] + w1 * cz[b] + w0 * cz[c]
            sub = zbuf[miny:maxy + 1, minx:maxx + 1]
            m &= z < sub
            if not m.any():
                continue
            if tex is not None:
                u = w2 * uv[a, 0] + w1 * uv[b, 0] + w0 * uv[c, 0]
                v = w2 * uv[a, 1] + w1 * uv[b, 1] + w0 * uv[c, 1]
                th, tw = tex.shape[:2]
                ui = np.clip((np.mod(u, 1.0) * (tw - 1)).astype(int), 0, tw - 1)
                vi = np.clip((np.mod(v, 1.0) * (th - 1)).astype(int), 0, th - 1)
                base = tex[vi, ui]
            else:
                base = np.broadcast_to(fac, m.shape + (3,))
            shaded = np.clip(base * lam, 0, 1)
            sub[m] = z[m]
            tile = col[miny:maxy + 1, minx:maxx + 1]
            tile[m] = shaded[m]
    img = Image.fromarray((np.clip(col, 0, 1) * 255).astype(np.uint8))
    return img, (hi - lo)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src"); ap.add_argument("out")
    ap.add_argument("--yaw", type=float, default=35.0)
    ap.add_argument("--cols", type=int, default=4)
    ap.add_argument("--size", type=int, default=460)
    a = ap.parse_args()
    files = ([a.src] if a.src.lower().endswith(".glb")
             else sorted(glob.glob(os.path.join(a.src, "*.glb"))))
    if not files:
        raise SystemExit("no GLBs at " + a.src)
    cells = []
    for f in files:
        im, ext = render(f, a.size, a.yaw)
        if im is None:
            print("  (empty geometry)", os.path.basename(f)); continue
        cells.append((os.path.basename(f)[:-4], im, ext))
        print(f"  rendered {os.path.basename(f):22s} {ext[0]:6.1f} x {ext[1]:6.1f} x {ext[2]:6.1f} m")
    C = min(a.cols, len(cells)); R = (len(cells) + C - 1) // C
    H = a.size + 24
    sheet = Image.new("RGB", (C * a.size, R * H), (22, 22, 26))
    d = ImageDraw.Draw(sheet)
    for i, (nm, im, ext) in enumerate(cells):
        x, y = (i % C) * a.size, (i // C) * H
        sheet.paste(im, (x, y + 24))
        d.text((x + 5, y + 7), f"{nm}   {ext[0]:.1f} x {ext[1]:.1f} x {ext[2]:.1f} m", fill=(255, 232, 130))
    sheet.save(a.out)
    print("wrote", a.out)


if __name__ == "__main__":
    main()
