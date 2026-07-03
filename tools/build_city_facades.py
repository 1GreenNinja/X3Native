#!/usr/bin/env python
"""
build_city_facades.py

Convert HIVEMIND Cyberpunk City FBX building meshes into clean, engine-ready GLB
facades for X3Native.

For each building:
  1. FBX2glTF -> raw GLB.
  2. Keep ONLY the LOD0 mesh (drop LOD1..LOD4 + ConvexHulls) by rebuilding a
     fresh, minimal glTF from scratch that contains a single mesh.
  3. Recenter to base-center: footprint centered on X/Z=0, base at Y=0.
  4. Re-skin materials BY NAME with solid PBR (baseColor + ORM metallicRoughness +
     emissive solid textures, embedded as data-URIs).
  5. save_binary to the output dir.

The original pack ships no usable .mat/textures, so materials are assigned by
matching the lowercased original material-name substring.

Usage:
  python build_city_facades.py [stem1 stem2 ...]
Defaults to the 6 curated buildings if no stems are given.
Idempotent / re-runnable.
"""

import base64
import io
import os
import struct
import subprocess
import sys
import tempfile

import pygltflib
from pygltflib import (
    GLTF2, Scene, Node, Mesh, Primitive, Attributes, Accessor, BufferView,
    Buffer, Material, PbrMetallicRoughness, TextureInfo, Texture, Sampler, Image,
)
from PIL import Image as PILImage

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
FBX2GLTF = "D:/GameDev/tools/FBX2glTF.exe"
SRC_DIR = ("/d/Assets/Cyberpunk City Cyberpunk Cyberpunk City Sci-Fi City/"
           "HIVEMIND/CyberpunkCity/HDRP(Default)/Art/Meshes/BG")
# Windows path for FBX2glTF.exe (it wants a native path)
SRC_DIR_WIN = ("D:/Assets/Cyberpunk City Cyberpunk Cyberpunk City Sci-Fi City/"
               "HIVEMIND/CyberpunkCity/HDRP(Default)/Art/Meshes/BG")
OUT_DIR = "D:/GameDev/x3-cityuplift/assets/converted_glb/CyberpunkCity"

DEFAULT_STEMS = [
    "SM_MERGED_BP_Shop_B10",
    "SM_MERGED_BP_Shop_A20",
    "SM_MERGED_BP_Building9",
    "SM_MERGED_BP_House_Shop_E10",
    "SM_MERGED_BP_Shop_B15",
    "SM_MERGED_BP_Building10_2",
]

# ---------------------------------------------------------------------------
# glTF component helpers
# ---------------------------------------------------------------------------
COMPONENT_SIZE = {5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4}
TYPE_NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4,
              "MAT2": 4, "MAT3": 9, "MAT4": 16}
ARRAY_BUFFER = 34962
ELEMENT_ARRAY_BUFFER = 34963


def accessor_byte_len(acc):
    return acc.count * TYPE_NCOMP[acc.type] * COMPONENT_SIZE[acc.componentType]


# ---------------------------------------------------------------------------
# Material classification by original name
# ---------------------------------------------------------------------------
def classify(name):
    n = (name or "").lower()
    if "winglass" in n or "glass" in n:
        return "glass"
    if "billboard" in n or "neon" in n or "sign" in n:
        return "neon"
    if "metal" in n or "steel" in n or "roof" in n or "trim" in n:
        return "steel"
    return "concrete"


# Archetype specs. Colors are 0..1 linear-ish values as given by the brief.
# baseColorFactor multiplies the (solid) baseColorTexture.
ARCHETYPES = {
    "glass": {
        "baseColorFactor": (0.02, 0.03, 0.05, 1.0),
        "baseColorTex":    (0.02, 0.03, 0.05),
        "roughness": 0.10, "metallic": 0.95,
        "emissiveTex":    (0.9, 0.55, 0.25),
        "emissiveFactor": (0.25, 0.16, 0.08),
    },
    "neon": {
        "baseColorFactor": (0.02, 0.06, 0.08, 1.0),
        "baseColorTex":    (0.02, 0.06, 0.08),
        "roughness": 0.40, "metallic": 0.0,
        "emissiveTex":    (0.1, 1.0, 1.3),
        "emissiveFactor": (0.1, 1.0, 1.3),
    },
    "steel": {
        "baseColorFactor": (0.45, 0.47, 0.50, 1.0),
        "baseColorTex":    (0.45, 0.47, 0.50),
        "roughness": 0.35, "metallic": 0.90,
        "emissiveTex":    (0.0, 0.0, 0.0),
        "emissiveFactor": (0.0, 0.0, 0.0),
    },
    "concrete": {
        "baseColorFactor": (0.52, 0.53, 0.55, 1.0),
        "baseColorTex":    (0.52, 0.53, 0.55),
        "roughness": 0.80, "metallic": 0.0,
        "emissiveTex":    (0.0, 0.0, 0.0),
        "emissiveFactor": (0.0, 0.0, 0.0),
    },
}


def _lin_to_srgb8(c):
    # Approximate sRGB encode then quantize to 0..255.
    c = max(0.0, min(1.0, c))
    if c <= 0.0031308:
        s = 12.92 * c
    else:
        s = 1.055 * (c ** (1.0 / 2.4)) - 0.055
    return int(round(max(0.0, min(1.0, s)) * 255.0))


def _lin8(c):
    return int(round(max(0.0, min(1.0, c)) * 255.0))


def solid_png_datauri(rgb, srgb=True):
    """4x4 solid PNG as a base64 data-URI. srgb=True applies sRGB encode
    (baseColor/emissive); srgb=False writes linear bytes (ORM)."""
    if srgb:
        px = (_lin_to_srgb8(rgb[0]), _lin_to_srgb8(rgb[1]), _lin_to_srgb8(rgb[2]))
    else:
        px = (_lin8(rgb[0]), _lin8(rgb[1]), _lin8(rgb[2]))
    im = PILImage.new("RGB", (4, 4), px)
    buf = io.BytesIO()
    im.save(buf, format="PNG", optimize=True)
    b64 = base64.b64encode(buf.getvalue()).decode("ascii")
    return "data:image/png;base64," + b64


# ---------------------------------------------------------------------------
# Core: build one clean GLB from a raw FBX2glTF GLB
# ---------------------------------------------------------------------------
def build_one(stem, tmpdir):
    src_fbx = f"{SRC_DIR_WIN}/{stem}.fbx"
    raw_glb = os.path.join(tmpdir, stem + "_raw.glb")
    out_glb = f"{OUT_DIR}/{stem}.glb"

    # 1. Convert -----------------------------------------------------------
    subprocess.run([FBX2GLTF, "-b", "-i", src_fbx, "-o", raw_glb],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    src = GLTF2().load(raw_glb)
    src_blob = src.binary_blob()

    # 2. Find the LOD0 mesh/node ------------------------------------------
    lod0_node = None
    for i, n in enumerate(src.nodes):
        if n.name and n.name.lower().endswith("_lod0"):
            lod0_node = n
            break
    if lod0_node is None or lod0_node.mesh is None:
        raise RuntimeError(f"{stem}: could not locate LOD0 node")
    src_mesh = src.meshes[lod0_node.mesh]

    # 3. Rebuild fresh minimal glTF ---------------------------------------
    out = GLTF2()
    new_blob = bytearray()

    def add_accessor(src_acc_idx, want_minmax=False):
        """Copy one source accessor's bytes into the new buffer; return new idx."""
        a = src.accessors[src_acc_idx]
        bv = src.bufferViews[a.bufferView]
        start = (bv.byteOffset or 0) + (a.byteOffset or 0)
        blen = accessor_byte_len(a)
        data = src_blob[start:start + blen]
        # 4-byte align
        while len(new_blob) % 4 != 0:
            new_blob.append(0)
        byte_off = len(new_blob)
        new_blob.extend(data)
        target = ELEMENT_ARRAY_BUFFER if a.type == "SCALAR" else ARRAY_BUFFER
        out.bufferViews.append(BufferView(
            buffer=0, byteOffset=byte_off, byteLength=blen, target=target))
        bv_idx = len(out.bufferViews) - 1
        na = Accessor(
            bufferView=bv_idx, byteOffset=0, componentType=a.componentType,
            count=a.count, type=a.type)
        if want_minmax and a.min and a.max:
            na.min = list(a.min)
            na.max = list(a.max)
        out.accessors.append(na)
        return len(out.accessors) - 1

    # --- materials: one shared material per archetype actually used ------
    tex_cache = {}   # (kind,rgb) -> texture index
    img_seen = {}    # datauri -> image index

    def get_texture(kind, rgb, srgb):
        key = (kind, tuple(round(c, 4) for c in rgb))
        if key in tex_cache:
            return tex_cache[key]
        uri = solid_png_datauri(rgb, srgb=srgb)
        if uri in img_seen:
            img_idx = img_seen[uri]
        else:
            out.images.append(Image(uri=uri))
            img_idx = len(out.images) - 1
            img_seen[uri] = img_idx
        out.textures.append(Texture(source=img_idx, sampler=0))
        t_idx = len(out.textures) - 1
        tex_cache[key] = t_idx
        return t_idx

    out.samplers.append(Sampler())  # default sampler 0

    arch_material_idx = {}

    def get_material(arch):
        if arch in arch_material_idx:
            return arch_material_idx[arch]
        spec = ARCHETYPES[arch]
        base_t = get_texture("base_" + arch, spec["baseColorTex"], srgb=True)
        # ORM: R=occlusion=1.0, G=roughness, B=metallic  (linear)
        orm_rgb = (1.0, spec["roughness"], spec["metallic"])
        orm_t = get_texture("orm_" + arch, orm_rgb, srgb=False)
        mat = Material(
            name="X3_" + arch,
            pbrMetallicRoughness=PbrMetallicRoughness(
                baseColorFactor=list(spec["baseColorFactor"]),
                baseColorTexture=TextureInfo(index=base_t),
                metallicFactor=1.0,
                roughnessFactor=1.0,
                metallicRoughnessTexture=TextureInfo(index=orm_t),
            ),
            emissiveFactor=list(spec["emissiveFactor"]),
        )
        et = spec["emissiveTex"]
        if any(c > 0 for c in et) or any(c > 0 for c in spec["emissiveFactor"]):
            em_t = get_texture("em_" + arch, et, srgb=True)
            mat.emissiveTexture = TextureInfo(index=em_t)
        out.materials.append(mat)
        idx = len(out.materials) - 1
        arch_material_idx[arch] = idx
        return idx

    # --- primitives ------------------------------------------------------
    new_prims = []
    bbox_min = [float("inf")] * 3
    bbox_max = [float("-inf")] * 3

    for p in src_mesh.primitives:
        attrs = p.attributes
        na = Attributes()
        # POSITION (with min/max) drives bbox
        pos_idx = add_accessor(attrs.POSITION, want_minmax=True)
        na.POSITION = pos_idx
        pa = out.accessors[pos_idx]
        if pa.min and pa.max:
            for k in range(3):
                bbox_min[k] = min(bbox_min[k], pa.min[k])
                bbox_max[k] = max(bbox_max[k], pa.max[k])
        if attrs.NORMAL is not None:
            na.NORMAL = add_accessor(attrs.NORMAL)
        if attrs.TEXCOORD_0 is not None:
            na.TEXCOORD_0 = add_accessor(attrs.TEXCOORD_0)
        # (drop TANGENT + TEXCOORD_1 to keep files lean)
        idx_acc = add_accessor(p.indices) if p.indices is not None else None
        src_mat_name = src.materials[p.material].name if p.material is not None else ""
        arch = classify(src_mat_name)
        mat_idx = get_material(arch)
        new_prims.append(Primitive(
            attributes=na, indices=idx_acc, material=mat_idx))

    out.meshes.append(Mesh(name=stem + "_LOD0", primitives=new_prims))

    # 3b. Recenter translation -------------------------------------------
    tx = -(bbox_min[0] + bbox_max[0]) / 2.0
    ty = -bbox_min[1]
    tz = -(bbox_min[2] + bbox_max[2]) / 2.0

    out.nodes.append(Node(name="RootNode", children=[1]))
    out.nodes.append(Node(name=stem, children=[2]))
    out.nodes.append(Node(name=stem + "_LOD0", mesh=0,
                          translation=[tx, ty, tz]))
    out.scenes.append(Scene(nodes=[0]))
    out.scene = 0

    # 4/5. finalize buffer + save ----------------------------------------
    while len(new_blob) % 4 != 0:
        new_blob.append(0)
    out.buffers.append(Buffer(byteLength=len(new_blob)))
    out.set_binary_blob(bytes(new_blob))
    os.makedirs(OUT_DIR, exist_ok=True)
    out.save_binary(out_glb)

    size = bbox_max[0] - bbox_min[0], bbox_max[1] - bbox_min[1], bbox_max[2] - bbox_min[2]
    return out_glb, size, (bbox_min, bbox_max), (tx, ty, tz)


# ---------------------------------------------------------------------------
# Verification: reload final GLB and check invariants
# ---------------------------------------------------------------------------
def verify(path):
    g = GLTF2().load(path)
    # reachable meshes from scene graph
    reachable_meshes = set()

    def walk(ni):
        n = g.nodes[ni]
        if n.mesh is not None:
            reachable_meshes.add(n.mesh)
        for c in (n.children or []):
            walk(c)

    for ni in g.scenes[g.scene].nodes:
        walk(ni)

    all_have_mr = all(
        m.pbrMetallicRoughness and m.pbrMetallicRoughness.metallicRoughnessTexture
        is not None for m in g.materials)

    # base-centered bbox from POSITION accessors of reachable mesh
    bmin = [float("inf")] * 3
    bmax = [float("-inf")] * 3
    for mi in reachable_meshes:
        for p in g.meshes[mi].primitives:
            a = g.accessors[p.attributes.POSITION]
            if a.min and a.max:
                for k in range(3):
                    bmin[k] = min(bmin[k], a.min[k])
                    bmax[k] = max(bmax[k], a.max[k])
    # apply node translation of the mesh node
    tnode = None
    for n in g.nodes:
        if n.mesh in reachable_meshes:
            tnode = n
    t = (tnode.translation or [0, 0, 0]) if tnode else [0, 0, 0]
    wmin = [bmin[k] + t[k] for k in range(3)]
    wmax = [bmax[k] + t[k] for k in range(3)]
    return {
        "n_meshes_in_scene": len(reachable_meshes),
        "n_materials": len(g.materials),
        "all_have_mr": all_have_mr,
        "wmin": wmin, "wmax": wmax,
        "size": [bmax[k] - bmin[k] for k in range(3)],
    }


def main():
    stems = sys.argv[1:] or DEFAULT_STEMS
    tmpdir = tempfile.mkdtemp(prefix="citytmp_")
    rows = []
    for stem in stems:
        out_glb, size, bbox, t = build_one(stem, tmpdir)
        fsize = os.path.getsize(out_glb)
        v = verify(out_glb)
        rows.append((stem, out_glb, fsize, size, v))
        base_ok = (abs(v["wmin"][0] + size[0] / 2) < 1e-3 and
                   abs(v["wmin"][1]) < 1e-3 and
                   abs(v["wmin"][2] + size[2] / 2) < 1e-3)
        print(f"\n=== {stem} ===")
        print(f"  path : {out_glb}")
        print(f"  size : {fsize/1e6:.2f} MB")
        print(f"  meshes in scene : {v['n_meshes_in_scene']}  (must be 1)")
        print(f"  materials       : {v['n_materials']}")
        print(f"  all mats have metallicRoughnessTexture : {v['all_have_mr']}")
        print(f"  bbox size (X,Y,Z m) : "
              f"({size[0]:.3f}, {size[1]:.3f}, {size[2]:.3f})")
        print(f"  post-recenter min   : "
              f"({v['wmin'][0]:.4f}, {v['wmin'][1]:.4f}, {v['wmin'][2]:.4f})  "
              f"expected ~ (-{size[0]/2:.3f}, 0, -{size[2]/2:.3f})")
        print(f"  base-centered check : {'OK' if base_ok else 'FAIL'}")

    # summary table
    print("\n\n================ SUMMARY TABLE ================")
    hdr = f"{'building':32} {'MB':>6} {'meshes':>6} {'mats':>4} {'mr':>3}  {'W x H x D (m)':>26}"
    print(hdr)
    print("-" * len(hdr))
    for stem, path, fsize, size, v in rows:
        print(f"{stem:32} {fsize/1e6:6.2f} {v['n_meshes_in_scene']:6d} "
              f"{v['n_materials']:4d} {('Y' if v['all_have_mr'] else 'N'):>3}  "
              f"{size[0]:7.2f} x {size[1]:6.2f} x {size[2]:7.2f}")
    print("\nOutput dir:", OUT_DIR)


if __name__ == "__main__":
    main()
