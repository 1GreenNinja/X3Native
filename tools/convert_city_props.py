#!/usr/bin/env python
"""
convert_city_props.py  (feat/city-aaa, item 2 — PROP CLUTTER)

Convert curated HIVEMIND street-clutter FBX props -> clean base-centered GLBs with
their REAL dedicated PBR texture sets (Dumpster/Cond/Trash/Trashbag/Lamp/Billboard;
tileable metal/wood for the untextured ones). Output to assets/converted_glb/CityProps.
Scattered densely in city.cpp so the eye never finds empty asphalt.

Pattern mirrors build_city_facades.py: FBX2glTF -> rebuild a minimal single-mesh glTF
(the highest-vert mesh node, i.e. LOD0) -> recenter base-center -> one material with
embedded real textures -> save_binary. Idempotent.
"""
import base64, io, os, struct, subprocess, sys, tempfile
import pygltflib
from pygltflib import (GLTF2, Scene, Node, Mesh, Primitive, Attributes, Accessor,
                       BufferView, Buffer, Material, PbrMetallicRoughness, TextureInfo,
                       Texture, Sampler, Image, NormalMaterialTexture)
from PIL import Image as PILImage

FBX2GLTF = "D:/GameDev/tools/FBX2glTF.exe"
MESH = ("D:/Assets/Cyberpunk City Cyberpunk Cyberpunk City Sci-Fi City/HIVEMIND/"
        "CyberpunkCity/HDRP(Default)/Art/Meshes")
TEX = ("D:/Assets/Cyberpunk City Cyberpunk Cyberpunk City Sci-Fi City/HIVEMIND/"
       "CyberpunkCity/HDRP(Default)/Art/Textures")
OUT_DIR = os.path.normpath(os.path.join(os.path.dirname(__file__), "..",
                           "assets/converted_glb/CityProps"))
RES = 512

# (out_name, fbx, base, orm, normal, emissive|None)
def U(sub, name): return f"{TEX}/Unique/{sub}/{name}"
PROPS = [
    ("Dumpster",   f"{MESH}/Props/Trash/SM_Dumpster.fbx",
     U("Dumpster","T_Dumpster_BaseColor.png"), U("Dumpster","T_Dumpster_ARM.png"),
     U("Dumpster","T_Dumpster_Normal.png"), None),
    ("Cond_A",     f"{MESH}/Props/SM_Cond_A.fbx",
     U("Cond_A","T_Cond_A_BaseColor.png"), U("Cond_A","T_Cond_A_ARM.png"),
     U("Cond_A","T_Cond_A_Normal.png"), None),
    ("Cond_B",     f"{MESH}/Props/SM_Cond_B.fbx",
     U("Cond_B","T_COnd_B_BaseColor.png"), U("Cond_B","T_COnd_B_ARM.png"),
     U("Cond_B","T_COnd_B_Normal.png"), None),
    ("Trash_01",   f"{MESH}/Props/Trash/SM_Trash_01.fbx",
     f"{TEX}/T_Trash_01_albedo.PNG", f"{TEX}/T_Trash_01_ARM.PNG",
     f"{TEX}/T_Trash_01_normal.PNG", None),
    ("Trash_02",   f"{MESH}/Props/Trash/SM_Trash_02.fbx",
     f"{TEX}/T_Trash_02_albedo.PNG", f"{TEX}/T_Trash_02_ARM.PNG",
     f"{TEX}/T_Trash_02_normal.PNG", None),
    ("Trashbag_01", f"{MESH}/Props/Trash/SM_Trashbag_01.fbx",
     f"{TEX}/T_Trashbag_BaseColor.PNG", f"{TEX}/T_Trashbag_ARM.PNG",
     f"{TEX}/T_Trashbag_Normal.PNG", None),
    ("Trashbag_02", f"{MESH}/Props/Trash/SM_Trashbag_02.fbx",
     f"{TEX}/T_Trashbag_BaseColor.PNG", f"{TEX}/T_Trashbag_ARM.PNG",
     f"{TEX}/T_Trashbag_Normal.PNG", None),
    ("Pallet",     f"{MESH}/Props/Trash/SM_Pallet.fbx",
     f"{TEX}/Tileable/Wood/T_Wood_Painted_Base_Color.png",
     f"{TEX}/Tileable/Wood/T_Wood_Painted_AO_Rough_Metal.png",
     f"{TEX}/Tileable/Wood/T_Wood_Painted_Normal.png", None),
    ("TrashCan",   f"{MESH}/Props/Trash/SM_TrashCan.fbx",
     f"{TEX}/Tileable/Metals/T_MetalRust_D.png", None,
     f"{TEX}/Tileable/Metals/T_MetalRust_N.png", None),
    ("Vent_Sq",    f"{MESH}/Props/Vent/SM_Vent_Square_A.fbx",
     f"{TEX}/Tileable/Metals/T_PaintedMetal_D.png", None,
     f"{TEX}/Tileable/Metals/T_PaintedMetal_N.png", None),
    ("Vent_Round", f"{MESH}/Props/Vent/SM_Vent_Round_A.fbx",
     f"{TEX}/Tileable/Metals/T_MetalRust_D.png", None,
     f"{TEX}/Tileable/Metals/T_MetalRust_N.png", None),
    ("Pipe_Wall",  f"{MESH}/Props/Pipe/SM_Pipe_Wall_Side_A.fbx",
     f"{TEX}/Tileable/Metals/T_MetalRust_D.png", None,
     f"{TEX}/Tileable/Metals/T_MetalRust_N.png", None),
    ("LampStreet", f"{MESH}/Props/SM_Lamp_Street.fbx",
     U("LampStreet","T_Lamp_STreet_BaseColor.png"), U("LampStreet","T_Lamp_STreet_ARM.png"),
     U("LampStreet","T_Lamp_STreet_Normal.png"), U("LampStreet","T_Lamp_STreet_Emissive.png")),
    ("Billboard",  f"{MESH}/Props/BillBoard/SM_Billboard_Front_Long.fbx",
     U("Billboard","T_Billboard.png"), None,
     f"{TEX}/DefaultNormal.png", U("Billboard","T_Billboard.png")),
]

COMPONENT_SIZE = {5120:1,5121:1,5122:2,5123:2,5125:4,5126:4}
TYPE_NCOMP = {"SCALAR":1,"VEC2":2,"VEC3":3,"VEC4":4}
ARRAY_BUFFER, ELEMENT_ARRAY_BUFFER = 34962, 34963
def acc_len(a): return a.count*TYPE_NCOMP[a.type]*COMPONENT_SIZE[a.componentType]

def datauri(png): return "data:image/png;base64," + base64.b64encode(png).decode("ascii")
def load_png(path, srgb=True):
    im = PILImage.open(path).convert("RGB").resize((RES,RES), PILImage.LANCZOS)
    buf = io.BytesIO(); im.save(buf, "PNG", optimize=True); return buf.getvalue()
def solid_orm(rough=0.6, metal=0.85):
    im = PILImage.new("RGB",(4,4),(255,int(rough*255),int(metal*255)))
    buf = io.BytesIO(); im.save(buf,"PNG"); return buf.getvalue()

def build_one(name, fbx, base, orm, nrm, emis, tmp):
    raw = os.path.join(tmp, name+"_raw.glb")
    subprocess.run([FBX2GLTF,"-b","-i",fbx,"-o",raw], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    src = GLTF2().load(raw); blob = src.binary_blob()
    # pick the mesh node with the most vertices (LOD0). Prefer names ending _LOD0.
    best_ni, best_v = None, -1
    for i,n in enumerate(src.nodes):
        if n.mesh is None: continue
        vs = sum(src.accessors[p.attributes.POSITION].count for p in src.meshes[n.mesh].primitives)
        lod0 = (n.name or "").lower().endswith("_lod0")
        score = vs + (10_000_000 if lod0 else 0)
        if score > best_v: best_v, best_ni = score, i
    if best_ni is None: raise RuntimeError(name+": no mesh node")
    src_mesh = src.meshes[src.nodes[best_ni].mesh]

    out = GLTF2(); nb = bytearray()
    def add_acc(ai, mm=False):
        a = src.accessors[ai]; bv = src.bufferViews[a.bufferView]
        st = (bv.byteOffset or 0)+(a.byteOffset or 0); ln = acc_len(a)
        # respect interleaved stride
        stride = bv.byteStride or (TYPE_NCOMP[a.type]*COMPONENT_SIZE[a.componentType])
        while len(nb)%4: nb.append(0)
        off = len(nb)
        for k in range(a.count):
            nb.extend(blob[st+k*stride: st+k*stride + TYPE_NCOMP[a.type]*COMPONENT_SIZE[a.componentType]])
        tgt = ELEMENT_ARRAY_BUFFER if a.type=="SCALAR" else ARRAY_BUFFER
        out.bufferViews.append(BufferView(buffer=0, byteOffset=off, byteLength=len(nb)-off, target=tgt))
        na = Accessor(bufferView=len(out.bufferViews)-1, byteOffset=0,
                      componentType=a.componentType, count=a.count, type=a.type)
        if mm and a.min and a.max: na.min=list(a.min); na.max=list(a.max)
        out.accessors.append(na); return len(out.accessors)-1

    out.samplers.append(Sampler())
    def tex(png, srgb):
        out.images.append(Image(uri=datauri(png), mimeType="image/png"))
        out.textures.append(Texture(source=len(out.images)-1, sampler=0))
        return len(out.textures)-1
    pbr = PbrMetallicRoughness(baseColorFactor=[1,1,1,1], metallicFactor=1.0, roughnessFactor=1.0)
    pbr.baseColorTexture = TextureInfo(index=tex(load_png(base), True))
    pbr.metallicRoughnessTexture = TextureInfo(index=tex(load_png(orm) if orm else solid_orm(), False))
    mat = Material(name="X3_"+name, pbrMetallicRoughness=pbr)
    if nrm: mat.normalTexture = NormalMaterialTexture(index=tex(load_png(nrm), False))
    if emis:
        mat.emissiveTexture = TextureInfo(index=tex(load_png(emis), True))
        mat.emissiveFactor = [1.0,0.85,0.5] if name=="LampStreet" else [0.7,0.8,1.0]
    out.materials.append(mat)

    prims=[]; bmin=[1e30]*3; bmax=[-1e30]*3
    for p in src_mesh.primitives:
        na = Attributes()
        pi = add_acc(p.attributes.POSITION, True); na.POSITION=pi
        pa=out.accessors[pi]
        if pa.min and pa.max:
            for k in range(3): bmin[k]=min(bmin[k],pa.min[k]); bmax[k]=max(bmax[k],pa.max[k])
        if p.attributes.NORMAL is not None: na.NORMAL=add_acc(p.attributes.NORMAL)
        if p.attributes.TEXCOORD_0 is not None: na.TEXCOORD_0=add_acc(p.attributes.TEXCOORD_0)
        idx = add_acc(p.indices) if p.indices is not None else None
        prims.append(Primitive(attributes=na, indices=idx, material=0))
    out.meshes.append(Mesh(name=name+"_LOD0", primitives=prims))
    # bake the source node's world scale (FBX cm->m etc) into the recenter is skipped;
    # FBX2glTF -b already yields meters. Recenter base-center on X/Z, base Y=0.
    tx=-(bmin[0]+bmax[0])/2; ty=-bmin[1]; tz=-(bmin[2]+bmax[2])/2
    out.nodes.append(Node(name="RootNode", children=[1]))
    out.nodes.append(Node(name=name, children=[2]))
    out.nodes.append(Node(name=name+"_LOD0", mesh=0, translation=[tx,ty,tz]))
    out.scenes.append(Scene(nodes=[0])); out.scene=0
    while len(nb)%4: nb.append(0)
    out.buffers.append(Buffer(byteLength=len(nb))); out.set_binary_blob(bytes(nb))
    os.makedirs(OUT_DIR, exist_ok=True)
    outp = f"{OUT_DIR}/{name}.glb"; out.save_binary(outp)
    return outp, (bmax[0]-bmin[0], bmax[1]-bmin[1], bmax[2]-bmin[2])

def main():
    tmp = tempfile.mkdtemp(prefix="prop_")
    only = sys.argv[1:]
    for row in PROPS:
        if only and row[0] not in only: continue
        try:
            p, size = build_one(*row, tmp)
            print(f"{row[0]:12} {os.path.getsize(p)/1e6:5.2f}MB  WxHxD = "
                  f"{size[0]:.2f} x {size[1]:.2f} x {size[2]:.2f} m")
        except Exception as e:
            print(f"{row[0]:12} FAILED: {e}")
    print("out:", OUT_DIR)

if __name__ == "__main__":
    main()
