#!/usr/bin/env python3
"""Build the GBX COUPE hero car GLB from the HDRP GBX COUPE Unity pack.

WHY THIS EXISTS (2026-08-17). The owner asked for "acura NSX-S-R 2022 A spec
cars, in Black". The 914-package library has no NSX and no mid-engine supercar;
the traffic lane said so honestly and substituted a Skyline. `HDRP GBX COUPE
Free Update` (654 MB, cache-only until tools/unitypackage_extract.py landed) is
the best hero-car mesh the library actually owns: a photoreal front-engine GT
coupe, 1.93 x 1.24 x 4.72 m, shipped as SEPARATE part meshes with 74 textures.
It is NOT an NSX and it is not mid-engine — see docs/design/VEHICLE_GBX_COUPE.md.

THREE PROBLEMS THE PACK HANDS YOU, AND THE ANSWERS HERE:

 1. IT IS 3.67 MILLION TRIANGLES (exterior alone; the front grille is 2.12 M
    because the artist modelled every wire). Hero-car LOD0 budget in this engine
    is ~150 k (CTR.glb, the incumbent, is 106 k). meshoptimizer via
    `npx @gltf-transform/cli simplify` does the decimation, per PART GROUP with
    its own ratio — one global ratio either destroys the body panels or leaves
    the grille at half a million triangles.

 2. THE FBX CARRIES ONE MATERIAL for all 27 exterior meshes ("HDRP_CheckBox_UV")
    and the .mat files are Shader Graph with hashed property names + GUID-only
    texture refs, and the pack ships no .meta, so there is no GUID->file map.
    So materials are assigned by NODE NAME (the ScansFactory / Mega-pack
    precedent: tools/convert_scansfactory.py, tools/w5_build_station_glb.py).
    The node names are excellent — Body_F_Bonnet_03_M, Body_Window_Rubber_01_M,
    A_Wheels_FL_04_Tire — so the table below reads like a paint sheet.

 3. THE PACK SHIPS ONLY THE LEFT WHEELS (Unity mirrored them in the prefab).
    The right pair is generated here by negating X in the vertex data and
    flipping triangle winding — NOT by a negative node scale, which would flip
    the winding at draw time and get the wheels backface-culled.

CONTRACT THE ENGINE EXPECTS (app/vehicle.cpp DriveDemo::skin, verified against
Vehicles/CTR.glb):
  * nose = +Z, origin ON THE GROUND PLANE (kBodySkin bakes the 180-deg flip and
    the -0.76 m chassis-center drop). GBX is already nose=+Z with the tyre
    contact patch at y=0 -- X3_WORLD_RULES rule 4 satisfied with no fixup.
  * wheels live on nodes whose names contain Wheel_FL / Wheel_FR / Wheel_RL /
    Wheel_RR; skin() ZEROES their world translation and keeps rotation+scale,
    so each wheel's geometry must be CENTRED ON ITS OWN HUB.
  * mesh-local axle on +-X (CTR's is; kWheelAxisFix is identity).
  * car paint declares material.extras["x3Clearcoat"] = {intensity, roughness}.

USAGE
  python tools/build_gbx_hero_car.py [--out assets/converted_glb/Vehicles/GBX_Coupe.glb]
                                     [--pack <GBX_Coupe_HDRP dir>] [--work <scratch>]
                                     [--keep-raw] [--no-simplify]
"""
from __future__ import annotations

import argparse
import base64
import json
import math
import os
import struct
import subprocess
import sys
import tempfile
from io import BytesIO

import numpy as np
from PIL import Image

Image.MAX_IMAGE_PIXELS = None

FBX2GLTF = r"D:\GameDev\tools\FBX2glTF.exe"
PACK_DEFAULT = r"\\p13700\G\Assets\HDRP GBX COUPE\Assets\GBX_Coupe_HDRP"

# The pack's FBX unit is 1 = 10 cm (FBX2glTF hands back a 47-unit-long car for a
# 4.72 m coupe). ONE number, used once, at vertex-bake time.
UNIT_TO_M = 0.1

COMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}
DT = {5126: np.float32, 5123: np.uint16, 5125: np.uint32,
      5121: np.uint8, 5122: np.int16, 5120: np.int8}


def log(*a):
    print("[gbx]", *a, flush=True)


# ---------------------------------------------------------------------------
# glTF read
# ---------------------------------------------------------------------------
def load_glb(path):
    with open(path, "rb") as f:
        magic, _ver, _tot = struct.unpack("<III", f.read(12))
        assert magic == 0x46546C67, path
        clen, _ = struct.unpack("<II", f.read(8))
        gltf = json.loads(f.read(clen))
        blen, _ = struct.unpack("<II", f.read(8))
        return gltf, f.read(blen)


def read_acc(g, b, i):
    a = g["accessors"][i]
    n = COMP[a["type"]]
    bv = g["bufferViews"][a["bufferView"]]
    off = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
    dt = DT[a["componentType"]]
    isz = n * np.dtype(dt).itemsize
    stride = bv.get("byteStride")
    if stride and stride != isz:
        # INTERLEAVED. The last element occupies only `isz` of its stride, so
        # reading count*stride overruns the view by (stride - isz) bytes —
        # gltf-transform writes exactly that layout (byteStride 32, three
        # accessors at byteOffset 0/12/24) and a naive read raises
        # "buffer is smaller than requested size" on the LAST attribute.
        span = stride * (a["count"] - 1) + isz
        raw = np.frombuffer(b, np.uint8, span, off)
        pad = np.zeros(stride * a["count"], np.uint8)
        pad[:span] = raw
        return np.ascontiguousarray(
            pad.reshape(a["count"], stride)[:, :isz]).view(dt).reshape(a["count"], n)
    return np.frombuffer(b, dt, a["count"] * n, off).reshape(a["count"], n)


def node_local(n):
    if "matrix" in n:
        return np.array(n["matrix"], float).reshape(4, 4).T
    t = n.get("translation", [0, 0, 0])
    s = n.get("scale", [1, 1, 1])
    x, y, z, w = n.get("rotation", [0, 0, 0, 1])
    R = np.array([[1-2*(y*y+z*z), 2*(x*y-z*w),   2*(x*z+y*w),   0],
                  [2*(x*y+z*w),   1-2*(x*x+z*z), 2*(y*z-x*w),   0],
                  [2*(x*z-y*w),   2*(y*z+x*w),   1-2*(x*x+y*y), 0],
                  [0, 0, 0, 1]], float)
    T = np.eye(4)
    T[:3, 3] = t
    return T @ R @ np.diag(list(s) + [1.0])


def parts_from_glb(path, scale=UNIT_TO_M):
    """Every mesh PRIMITIVE -> one part with WORLD-BAKED, metre-scaled geometry.

    Each part also carries the source material NAME, so the final assembly pairs
    geometry to materials by name and never by index — gltf-transform is free to
    reorder or renumber materials between passes."""
    g, b = load_glb(path)
    mnames = [m.get("name", "mat%d" % i) for i, m in enumerate(g.get("materials", []))]
    out = []

    def walk(ni, M):
        n = g["nodes"][ni]
        W = M @ node_local(n)
        if "mesh" in n:
            mesh = g["meshes"][n["mesh"]]
            for pr in mesh["primitives"]:
                if pr.get("mode", 4) != 4:
                    continue
                P = read_acc(g, b, pr["attributes"]["POSITION"]).astype(np.float64)
                P = ((W[:3, :3] @ P.T).T + W[:3, 3]) * scale
                if "NORMAL" in pr["attributes"]:
                    N = read_acc(g, b, pr["attributes"]["NORMAL"]).astype(np.float64)
                    N = (np.linalg.inv(W[:3, :3]).T @ N.T).T
                    ln = np.linalg.norm(N, axis=1, keepdims=True)
                    N = N / np.where(ln < 1e-12, 1.0, ln)
                else:
                    N = np.tile([0.0, 1.0, 0.0], (len(P), 1))
                UV = (read_acc(g, b, pr["attributes"]["TEXCOORD_0"]).astype(np.float32)
                      if "TEXCOORD_0" in pr["attributes"]
                      else np.zeros((len(P), 2), np.float32))
                IDX = (read_acc(g, b, pr["indices"]).reshape(-1).astype(np.uint32)
                       if "indices" in pr else np.arange(len(P), dtype=np.uint32))
                out.append(dict(name=n.get("name", mesh.get("name", "?")),
                                mat=(mnames[pr["material"]] if "material" in pr else ""),
                                P=P.astype(np.float32), N=N.astype(np.float32),
                                UV=UV, IDX=IDX))
        for c in n.get("children", []):
            walk(c, W)

    for r in g["scenes"][g.get("scene", 0)]["nodes"]:
        walk(r, np.eye(4))
    return out


# ---------------------------------------------------------------------------
# MATERIAL TABLE -- keyed on the lowercased NODE name, first substring wins.
# The FBX carries one material for everything, so this table IS the paint sheet.
# `tex` names a set in TEXSETS below; `bc` is a LINEAR base colour; `cc` is the
# clearcoat (intensity, roughness) that becomes extras["x3Clearcoat"].
# ---------------------------------------------------------------------------
PAINT_BLACK = (0.014, 0.014, 0.016, 1.0)   # the owner's BLACK, linear; near-zero
                                           # albedo + clearcoat = jet-black metallic

MATRULES = [
    # ---- WHEELS (before every generic rule: "_m_wheels" would hit "metal") ----
    ("_04_tire",        "Tyre",          dict(bc=(0.35, 0.35, 0.35, 1), tex="tyre",     metal=0.0, rough=0.92)),
    ("_05_side",        "TyreSidewall",  dict(bc=(0.42, 0.42, 0.42, 1), tex="tyreside", metal=0.0, rough=0.85)),
    ("_02_m_wheels",    "Rim",           dict(bc=(0.72, 0.73, 0.76, 1), tex="alu",      metal=1.0, rough=0.22)),

    # ---- LIGHTS. Ordered before the body so "Head_Lights_01_M" never falls
    #      through to a paint rule. Glow lamps carry KHR emissive strength so
    #      they feed the HDR bloom chain at night (rule 5's texture-gated law
    #      does not apply: these ARE the light-boxes).
    ("head_lights_06",  "HeadLampGlow",  dict(bc=(0.90, 0.92, 1.00, 1), metal=0.0, rough=0.10,
                                              emis=(1.0, 0.97, 0.90, 4.0))),
    ("head_lights_07",  "HeadLampGlass", dict(bc=(0.055, 0.058, 0.065, 1), metal=1.0, rough=0.05)),
    ("head_lights",     "HeadLampBody",  dict(bc=(0.62, 0.63, 0.66, 1), tex="chrome", metal=1.0, rough=0.16)),
    ("rear_lights_trunk_02", "TailLampGlassC", dict(bc=(0.32, 0.010, 0.012, 1), metal=0.0, rough=0.12,
                                              emis=(1.0, 0.05, 0.03, 2.6))),
    ("rear_lights_01",  "TailLampGlass", dict(bc=(0.32, 0.010, 0.012, 1), metal=0.0, rough=0.12,
                                              emis=(1.0, 0.05, 0.03, 2.6))),
    ("rear_lights",     "TailLampBody",  dict(bc=(0.055, 0.055, 0.06, 1), tex="metal", metal=0.85, rough=0.30)),

    # ---- GLASS. Smoked mirror, OPAQUE. The LOD0 shell ships no cabin, so an
    #      alpha-blended pane would show the inside of the far bodywork (the
    #      CTR precedent, tools/convert_car_glb.py, made the same call).
    ("_glass",          "Glass",         dict(bc=(0.010, 0.010, 0.014, 1), metal=1.0, rough=0.045)),
    ("window_gt",       "Glass",         dict(bc=(0.010, 0.010, 0.014, 1), metal=1.0, rough=0.045)),
    ("window_black",    "TrimBlack",     dict(bc=(0.020, 0.020, 0.022, 1), tex="plastic", metal=0.10, rough=0.55)),
    ("window_rubber",   "WeatherRubber", dict(bc=(0.028, 0.028, 0.030, 1), tex="rubber", metal=0.0, rough=0.80)),

    # ---- REAR REFLECTOR STRIP ----
    ("bumper_red_chrome", "ReflectorChrome", dict(bc=(0.30, 0.012, 0.014, 1), tex="chrome",
                                              metal=1.0, rough=0.10, emis=(1.0, 0.04, 0.03, 1.6))),
    ("bumper_red",      "Reflector",     dict(bc=(0.30, 0.012, 0.014, 1), metal=0.0, rough=0.16,
                                              emis=(1.0, 0.04, 0.03, 1.6))),

    # ---- GRILLE / INTAKES ----
    ("bumper_grill",    "GrilleMesh",    dict(bc=(0.030, 0.030, 0.033, 1), tex="darkmetal",
                                              metal=0.85, rough=0.34)),
    ("exterior_dummy",  "IntakeBack",    dict(bc=(0.015, 0.015, 0.016, 1), tex="darkmetal",
                                              metal=0.30, rough=0.70)),

    # ---- BRIGHTWORK / UNDERBODY ----
    # SATIN BLACK chrome, not bright chrome. The part is a ROCKER blade running
    # the length of the car at 20 cm; at the pack's stock chrome value it read as
    # a white ledge under a black car in the eyes-on sheet. The pack itself ships
    # HDRP_ChromeSatinBlack for exactly this — the owner asked for BLACK.
    ("side_chrome",     "ChromeSatinBlack", dict(bc=(0.075, 0.075, 0.080, 1), tex="chrome",
                                              metal=1.0, rough=0.20)),
    ("bumper_metal",    "SatinMetal",    dict(bc=(0.30, 0.30, 0.32, 1), tex="metal", metal=1.0, rough=0.28)),
    ("under_bottom",    "CarbonFloor",   dict(bc=(1.0, 1.0, 1.0, 1), tex="carbon", metal=0.15, rough=0.35)),
    ("numberplate",     "Plate",         dict(bc=(0.68, 0.68, 0.70, 1), tex="plastic", metal=0.0, rough=0.42)),

    # ---- THE PAINT. Last, so every named part above wins first; anything the
    #      table did not name is bodywork and gets painted (Body_Kit_M, the
    #      bonnets, the trunk lids, the bumper skins).
    ("body_",           "PaintBlack",    dict(bc=PAINT_BLACK, tex="flake", metal=0.80, rough=0.33,
                                              cc=(1.0, 0.04))),
]
FALLBACK = ("PaintBlack", dict(bc=PAINT_BLACK, tex="flake", metal=0.80, rough=0.33, cc=(1.0, 0.04)))


def match_rule(node_name):
    nm = node_name.lower()
    for key, mat, spec in MATRULES:
        if key in nm:
            return mat, spec
    return FALLBACK


# ---------------------------------------------------------------------------
# TEXTURE SETS -- resolved by FILE NAME out of the pack (no .meta => no GUIDs).
#   bc   : sRGB albedo
#   nm   : tangent-space normal
#   mask : HDRP MaskMap (R=metal G=AO B=detail A=smooth) -> glTF MR
#          (R=AO, G=rough=1-A, B=metal). Same conversion as tools/tex_curate.py's
#          "rma" branch; ENGINE_GOTCHAS 3.6 -- never inject a packed mask raw.
# ---------------------------------------------------------------------------
TEXSETS = {
    "flake":     dict(nm="CarFlakes_NM.png"),
    "carbon":    dict(bc="CarbonFiber_BC.png", nm="CarbonFiber_NM.png", mask="CarbonFiber_M.png"),
    "tyre":      dict(bc="Rubber_BC.png",      nm="Rubber_NM.png"),
    "tyreside":  dict(bc="Tire_Side_D.png",    nm="Rubber_NM.png"),
    "alu":       dict(mask="AluminumBrushed_M.png"),
    "chrome":    dict(mask="ChromeScratched_M.png"),
    "metal":     dict(mask="Metal_M.png"),
    "darkmetal": dict(mask="MetalScratched_M.png"),
    "plastic":   dict(mask="PlasticDirt_M.png"),
    "rubber":    dict(nm="Rubber_NM.png"),
}
BC_SIZE, NM_SIZE, MR_SIZE = 1024, 1024, 512


def find_tex(pack, fname):
    for sub in ("HDRP/Textures", "Textures"):
        p = os.path.join(pack, *sub.split("/"), fname)
        if os.path.exists(p):
            return p
    return None


def png_bytes(im, size):
    if max(im.size) > size:
        im = im.resize((size, size), Image.LANCZOS)
    buf = BytesIO()
    im.save(buf, "PNG", optimize=True)
    return buf.getvalue()


def build_textures(pack):
    """name -> png bytes, plus set-name -> {slot: image index}."""
    images, index = [], {}
    cache = {}

    def add(key, maker):
        if key in cache:
            return cache[key]
        data = maker()
        if data is None:
            return None
        images.append((key, data))
        cache[key] = len(images) - 1
        return cache[key]

    for sname, slots in TEXSETS.items():
        got = {}
        if "bc" in slots:
            p = find_tex(pack, slots["bc"])
            if p:
                got["bc"] = add("bc:" + slots["bc"],
                                lambda p=p: png_bytes(Image.open(p).convert("RGB"), BC_SIZE))
        if "nm" in slots:
            p = find_tex(pack, slots["nm"])
            if p:
                got["nm"] = add("nm:" + slots["nm"],
                                lambda p=p: png_bytes(Image.open(p).convert("RGB"), NM_SIZE))
        if "mask" in slots:
            p = find_tex(pack, slots["mask"])
            if p:
                got["mr"] = add("mr:" + slots["mask"], lambda p=p: mask_to_mr(p))
        index[sname] = got
        missing = [k for k in slots if k not in ("bc", "nm", "mask")]
        if missing:
            log("WARN unknown slot(s)", missing, "in set", sname)
    return images, index


def mask_to_mr(path):
    """HDRP MaskMap -> glTF metallicRoughness (R=AO, G=roughness, B=metallic)."""
    im = Image.open(path)
    if im.mode != "RGBA":
        im = im.convert("RGBA")
    if max(im.size) > MR_SIZE:
        im = im.resize((MR_SIZE, MR_SIZE), Image.LANCZOS)
    a = np.asarray(im)
    out = np.zeros((a.shape[0], a.shape[1], 3), np.uint8)
    out[..., 0] = a[..., 1]            # AO   <- G
    out[..., 1] = 255 - a[..., 3]      # rough<- 1 - smoothness(A)
    out[..., 2] = a[..., 0]            # metal<- R
    buf = BytesIO()
    Image.fromarray(out).save(buf, "PNG", optimize=True)
    return buf.getvalue()


# ---------------------------------------------------------------------------
# glTF write
# ---------------------------------------------------------------------------
class GlbWriter:
    def __init__(self):
        self.bin = bytearray()
        self.views = []
        self.accessors = []
        self.meshes = []
        self.nodes = []
        self.images = []
        self.textures = []
        self.materials = []
        self.samplers = [{"magFilter": 9729, "minFilter": 9987,
                          "wrapS": 10497, "wrapT": 10497}]

    def _pad(self, n=4):
        while len(self.bin) % n:
            self.bin.append(0)

    def view(self, data, target=None):
        self._pad()
        off = len(self.bin)
        self.bin += data
        v = {"buffer": 0, "byteOffset": off, "byteLength": len(data)}
        if target:
            v["target"] = target
        self.views.append(v)
        return len(self.views) - 1

    def acc(self, arr, typ, ctype, target, minmax=False):
        arr = np.ascontiguousarray(arr)
        vi = self.view(arr.tobytes(), target)
        a = {"bufferView": vi, "componentType": ctype,
             "count": int(arr.shape[0]), "type": typ}
        if minmax:
            a["min"] = [float(x) for x in arr.min(0)]
            a["max"] = [float(x) for x in arr.max(0)]
        self.accessors.append(a)
        return len(self.accessors) - 1

    def prim(self, P, N, UV, IDX, mat):
        return {"attributes": {
                    "POSITION": self.acc(P.astype(np.float32), "VEC3", 5126, 34962, True),
                    "NORMAL":   self.acc(N.astype(np.float32), "VEC3", 5126, 34962),
                    "TEXCOORD_0": self.acc(UV.astype(np.float32), "VEC2", 5126, 34962)},
                "indices": self.acc(IDX.astype(np.uint32).reshape(-1, 1), "SCALAR", 5125, 34963),
                "material": mat, "mode": 4}

    def save(self, path):
        gltf = {"asset": {"version": "2.0", "generator": "x3 build_gbx_hero_car.py"},
                "scene": 0, "scenes": [{"nodes": list(range(len(self.nodes)))}],
                "nodes": self.nodes, "meshes": self.meshes,
                "buffers": [{"byteLength": len(self.bin)}],
                "bufferViews": self.views, "accessors": self.accessors,
                "materials": self.materials}
        if self.images:
            gltf["images"] = self.images
            gltf["textures"] = self.textures
            gltf["samplers"] = self.samplers
        exts = set()
        for m in self.materials:
            exts |= set((m.get("extensions") or {}).keys())
        if exts:
            gltf["extensionsUsed"] = sorted(exts)
        js = json.dumps(gltf, separators=(",", ":")).encode()
        js += b" " * ((4 - len(js) % 4) % 4)
        self._pad()
        blob = bytes(self.bin)
        total = 12 + 8 + len(js) + 8 + len(blob)
        with open(path, "wb") as f:
            f.write(struct.pack("<III", 0x46546C67, 2, total))
            f.write(struct.pack("<II", len(js), 0x4E4F534A))
            f.write(js)
            f.write(struct.pack("<II", len(blob), 0x004E4942))
            f.write(blob)
        return total


def make_material(w, name, spec, texidx):
    pbr = {"baseColorFactor": list(spec.get("bc", (0.8, 0.8, 0.8, 1.0))),
           "metallicFactor": float(spec.get("metal", 0.0)),
           "roughnessFactor": float(spec.get("rough", 0.5))}
    m = {"name": name, "pbrMetallicRoughness": pbr, "doubleSided": False}
    slots = texidx.get(spec.get("tex", ""), {})
    if "bc" in slots:
        pbr["baseColorTexture"] = {"index": slots["bc"]}
    if "mr" in slots:
        pbr["metallicRoughnessTexture"] = {"index": slots["mr"]}
    if "nm" in slots:
        m["normalTexture"] = {"index": slots["nm"]}
    if "cc" in spec:
        m["extras"] = {"x3Clearcoat": {"intensity": spec["cc"][0], "roughness": spec["cc"][1]}}
    if "emis" in spec:
        e = spec["emis"]
        m["emissiveFactor"] = [e[0], e[1], e[2]]
        m["extensions"] = {"KHR_materials_emissive_strength": {"emissiveStrength": e[3]}}
    w.materials.append(m)
    return len(w.materials) - 1


def merge_parts(parts):
    """Concatenate a list of parts into one (P, N, UV, IDX)."""
    P = np.vstack([p["P"] for p in parts])
    N = np.vstack([p["N"] for p in parts])
    UV = np.vstack([p["UV"] for p in parts])
    idx, base = [], 0
    for p in parts:
        idx.append(p["IDX"].astype(np.uint32) + base)
        base += len(p["P"])
    return P, N, UV, np.concatenate(idx)


def mirror_x(P, N, IDX):
    """Right-hand copy of a left-hand part. Negating X flips handedness, so the
    triangle winding must be reversed or every face is backface-culled."""
    P2 = P.copy(); P2[:, 0] *= -1.0
    N2 = N.copy(); N2[:, 0] *= -1.0
    I2 = IDX.reshape(-1, 3)[:, ::-1].reshape(-1).copy()
    return P2, N2, I2


def gtf(args, label):
    """Run @gltf-transform/cli; returns True on success."""
    cmd = ["npx", "--yes", "@gltf-transform/cli"] + args
    log("gltf-transform", label, " ".join(args[:4]))
    r = subprocess.run(cmd, capture_output=True, text=True, shell=(os.name == "nt"))
    if r.returncode != 0:
        log("FAILED:", (r.stdout or "")[-800:], (r.stderr or "")[-800:])
        return False
    return True


def simplify(src, dst, ratio, error):
    ok = gtf(["simplify", src, dst, "--ratio", str(ratio), "--error", str(error)],
             f"ratio={ratio} error={error}")
    return ok and os.path.exists(dst)


def tri_count(path):
    g, _ = load_glb(path)
    n = 0
    for m in g.get("meshes", []):
        for pr in m["primitives"]:
            if "indices" in pr:
                n += g["accessors"][pr["indices"]]["count"] // 3
    return n


# ---------------------------------------------------------------------------
# Part groups. Each group is decimated on its own budget: the grille is a wire
# mesh nobody can read at 3 m, the bonnet is a reflective surface where every
# collapsed edge shows as a crease in the clearcoat.
#   (group, node-name substrings, simplify ratio, simplify error)
# ---------------------------------------------------------------------------
GROUPS = [
    ("grille",  ["bumper_grill_02", "bumper_grill_03"],        0.004, 0.02),
    ("shell",   ["body_kit", "body_under_bottom"],             0.05,  0.004),
    ("panels",  ["body_f_bonnet", "body_rear_trunk", "body_f_bumper_m",
                 "body_rear_bumper", "body_window", "body_side_chrome",
                 "body_f_numberplate", "body_rear_numberplate",
                 "body_f_bumper_grill_01", "body_f_exterior_dummy"], 0.14, 0.006),
    ("lights",  ["head_lights", "rear_lights"],                0.10,  0.006),
]


def group_of(name):
    nm = name.lower()
    for gname, keys, _r, _e in GROUPS:
        for k in keys:
            if k in nm:
                return gname
    return "panels"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pack", default=PACK_DEFAULT)
    ap.add_argument("--out", default=os.path.join("assets", "converted_glb",
                                                  "Vehicles", "GBX_Coupe.glb"))
    ap.add_argument("--work", default=os.path.join(tempfile.gettempdir(), "gbx_build"))
    ap.add_argument("--no-simplify", action="store_true")
    a = ap.parse_args()

    models = os.path.join(a.pack, "Models")
    os.makedirs(a.work, exist_ok=True)

    # ---- 1. FBX -> raw GLB (cached; FBX2glTF on the 100 MB exterior is slow) --
    raw = {}
    for stem in ("Exterior_MD", "Wheels_MD", "Light_MD"):
        dst = os.path.join(a.work, stem + ".glb")
        if not os.path.exists(dst):
            src = os.path.join(models, stem + ".FBX")
            log("FBX2glTF", stem)
            r = subprocess.run([FBX2GLTF, "-b", "-i", src, "-o", os.path.join(a.work, stem)],
                               capture_output=True, text=True)
            if r.returncode != 0 or not os.path.exists(dst):
                log("FBX2glTF FAILED for", stem, r.stdout[-400:], r.stderr[-400:])
                return 1
        raw[stem] = dst

    # ---- 2. read parts, metre-scaled, world-baked --------------------------
    ext = parts_from_glb(raw["Exterior_MD"])
    lit = parts_from_glb(raw["Light_MD"])
    whl = parts_from_glb(raw["Wheels_MD"])
    log(f"source parts: exterior {len(ext)}, lights {len(lit)}, wheels {len(whl)}; "
        f"{sum(len(p['IDX'])//3 for p in ext+lit+whl):,} triangles")

    # ---- 3. BODY intermediate: one primitive per (group, material) ----------
    #        (grouping cuts draw calls AND lets each group carry its own budget)
    body_bins = {}
    for p in ext + lit:
        mat, spec = match_rule(p["name"])
        body_bins.setdefault((group_of(p["name"]), mat), []).append(p)
    for (gname, mat), ps in sorted(body_bins.items()):
        log(f"  body group {gname:8s} / {mat:16s} {sum(len(q['IDX'])//3 for q in ps):>9,} tris"
            f"  ({len(ps)} node(s))")

    # ---- 4. WHEEL intermediate: ONE wheel, hub-centred ---------------------
    #        Pack ships FL + RL only. Hub centre = the union AABB centre of the
    #        wheel's own meshes (the artist's group pivot is the hub FACE, which
    #        would sit the drawn wheel ~3 cm inboard of the physics pose).
    wheel_bins = {"FL": {}, "RL": {}}
    for p in whl:
        side = "FL" if "_fl_" in p["name"].lower() else "RL"
        mat, spec = match_rule(p["name"])
        wheel_bins[side].setdefault(mat, []).append(p)
    hub = {}
    for side in ("FL", "RL"):
        allp = [q for ps in wheel_bins[side].values() for q in ps]
        lo = np.min([q["P"].min(0) for q in allp], axis=0)
        hi = np.max([q["P"].max(0) for q in allp], axis=0)
        hub[side] = (lo + hi) * 0.5
        log(f"  wheel {side}: hub {np.round(hub[side], 4).tolist()} m, "
            f"radius {(hi[1]-lo[1])/2:.4f} m, width {hi[0]-lo[0]:.4f} m, "
            f"{sum(len(q['IDX'])//3 for q in allp):,} tris")

    # ---- 5. write the two intermediates (no textures yet: simplify only
    #        needs geometry, and a 15 MB texture payload would be rewritten
    #        by every gltf-transform pass) --------------------------------
    def write_stage(bins, path, centre=None):
        w = GlbWriter()
        matidx, prims = {}, {}
        for key, ps in sorted(bins.items()):
            gname, mat = key if isinstance(key, tuple) else ("wheel", key)
            spec = dict(match_rule(ps[0]["name"])[1])
            if mat not in matidx:
                matidx[mat] = make_material(w, mat, spec, {})
            P, N, UV, IDX = merge_parts(ps)
            if centre is not None:
                P = P - centre
            prims.setdefault(gname, []).append(w.prim(P, N, UV, IDX, matidx[mat]))
        for gname, pr in prims.items():
            w.meshes.append({"name": gname, "primitives": pr})
            w.nodes.append({"name": gname, "mesh": len(w.meshes) - 1})
        w.save(path)
        return path

    stages = {}
    for gname, _keys, ratio, err in GROUPS:
        sub = {k: v for k, v in body_bins.items() if k[0] == gname}
        if not sub:
            continue
        src = os.path.join(a.work, f"stg_{gname}.glb")
        write_stage(sub, src)
        dst = os.path.join(a.work, f"dec_{gname}.glb")
        if a.no_simplify or not simplify(src, dst, ratio, err):
            dst = src
        log(f"  {gname:8s} {tri_count(src):>9,} -> {tri_count(dst):>8,} tris")
        stages[gname] = dst
    for side in ("FL", "RL"):
        src = os.path.join(a.work, f"stg_wheel{side}.glb")
        write_stage(wheel_bins[side], src, centre=hub[side])
        dst = os.path.join(a.work, f"dec_wheel{side}.glb")
        if a.no_simplify or not simplify(src, dst, 0.09, 0.008):
            dst = src
        log(f"  wheel{side} {tri_count(src):>7,} -> {tri_count(dst):>8,} tris")
        stages["wheel" + side] = dst

    # ---- 6. final assembly: textures + the four wheel nodes ---------------
    imgs, texidx = build_textures(a.pack)
    w = GlbWriter()
    for name, data in imgs:
        w.images.append({"name": name, "mimeType": "image/png",
                         "bufferView": w.view(data)})
        w.textures.append({"sampler": 0, "source": len(w.images) - 1})
    log(f"textures embedded: {len(imgs)} "
        f"({sum(len(d) for _n, d in imgs)/1048576:.1f} MB)")

    matidx = {}

    def emit(path, node_name, translation=None, mirror=False):
        parts = parts_from_glb(path, scale=1.0)     # stages are already metres
        prims = []
        for part in parts:
            mname = part["mat"] or FALLBACK[0]
            if mname not in matidx:
                spec = next((s for _k, m, s in MATRULES if m == mname), FALLBACK[1])
                matidx[mname] = make_material(w, mname, spec, texidx)
            P, N, UV, IDX = part["P"], part["N"], part["UV"], part["IDX"]
            if mirror:
                P, N, IDX = mirror_x(P, N, IDX)
            prims.append(w.prim(P, N, UV, IDX, matidx[mname]))
        w.meshes.append({"name": node_name, "primitives": prims})
        node = {"name": node_name, "mesh": len(w.meshes) - 1}
        if translation is not None:
            node["translation"] = [float(x) for x in translation]
        w.nodes.append(node)
        return sum(len(p["IDX"]) // 3 for p in parts)

    total = 0
    for gname, _k, _r, _e in GROUPS:
        if gname in stages:
            total += emit(stages[gname], "Body_" + gname.capitalize())
    # WHEELS. skin() zeroes the node translation and keeps rotation+scale, so
    # the geometry is hub-centred (step 4) and the node translation is the only
    # thing carrying the station. Left pair as authored, right pair mirrored.
    for slot, side, mirror in (("Wheel_FL", "FL", False), ("Wheel_FR", "FL", True),
                               ("Wheel_RL", "RL", False), ("Wheel_RR", "RL", True)):
        t = hub[side].copy()
        if mirror:
            t[0] = -t[0]
        total += emit(stages["wheel" + side], slot, translation=t, mirror=mirror)

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    size = w.save(a.out)
    log(f"WROTE {a.out}  {size/1048576:.2f} MB  {total:,} triangles  "
        f"{len(w.materials)} materials  {len(w.nodes)} nodes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
