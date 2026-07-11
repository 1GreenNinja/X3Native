#!/usr/bin/env python
"""
patch_facade_textures.py  (feat/city-aaa, item 1 — FACADE TEXTURES)

The city facades were re-skinned with SOLID 4x4 textures (concrete = flat grey
0.52,0.53,0.55) -> they read as pale untextured graybox (the #1 AAA tell).

This patches the COMMITTED facade GLBs in place: it swaps each archetype's solid
baseColor / metallic-roughness image for a REAL tileable HIVEMIND PBR map and adds
a NORMAL map (the derivative-TBN in mesh.frag consumes it with no vertex tangents),
so the buildings read as USED, textured concrete + metal that catch the neon + lamp
light. Geometry, UVs, node graph, and the buffer are untouched (images are all
data-URIs) -> lossless, reversible, no FBX2glTF re-convert.

Idempotent. Run:  python tools/patch_facade_textures.py
"""
import base64, io, os, sys
import pygltflib
from pygltflib import GLTF2, Image, Texture, Sampler, TextureInfo, NormalMaterialTexture
from PIL import Image as PILImage

TEX = ("D:/Assets/Cyberpunk City Cyberpunk Cyberpunk City Sci-Fi City/HIVEMIND/"
       "CyberpunkCity/HDRP(Default)/Art/Textures")
OUT_DIR = os.path.join(os.path.dirname(__file__), "..",
                       "assets/converted_glb/CyberpunkCity")
GLBS = [
    "SM_MERGED_BP_Shop_B10.glb", "SM_MERGED_BP_Shop_A20.glb",
    "SM_MERGED_BP_Building9.glb", "SM_MERGED_BP_House_Shop_E10.glb",
    "SM_MERGED_BP_Shop_B15.glb", "SM_MERGED_BP_Building10_2.glb",
]
RES = 512  # embedded tile resolution (UVs tile ~5x -> effective detail is high)

# --- real HIVEMIND maps per archetype (base, orm, normal) ----------------------
MAPS = {
    "concrete": (
        f"{TEX}/Tileable/Concrete_01/T_Concrete_01_Dirt_Base_Color.png",
        f"{TEX}/Tileable/Concrete_01/T_Concrete_01_Dirt_AO_Rough_Metal.png",  # =glTF ORM
        f"{TEX}/Tileable/Concrete_01/T_Concrete_01_Dirt_Normal.png",
    ),
    "steel": (
        f"{TEX}/Tileable/Metals/T_PaintedMetal_D.png",
        None,  # no ARM in pack -> solid ORM (rough 0.42, metal 0.85)
        f"{TEX}/Tileable/Metals/T_PaintedMetal_N.png",
    ),
    "glass": (
        f"{TEX}/Tileable/Winglass/T_Win_House_Details.png",  # window-pane pattern
        None,  # solid ORM (rough 0.10, metal 0.90 = wet mirror glass)
        None,
    ),
}
# baseColorFactor after re-texture: near-white so the real albedo reads (slight cool
# bone tint per WORLD_ART_DIRECTION); glass stays tinted dark + emissive.
BCF = {
    "concrete": [0.90, 0.92, 0.98, 1.0],
    "steel":    [0.85, 0.87, 0.92, 1.0],
    "glass":    [0.10, 0.13, 0.20, 1.0],
}
SOLID_ORM = {  # (R=AO, G=rough, B=metal) linear
    "steel": (1.0, 0.42, 0.85),
    "glass": (1.0, 0.10, 0.90),
}
_cache = {}
def datauri(png_bytes):
    return "data:image/png;base64," + base64.b64encode(png_bytes).decode("ascii")

def load_tex(path):
    if path in _cache: return _cache[path]
    im = PILImage.open(path).convert("RGB").resize((RES, RES), PILImage.LANCZOS)
    buf = io.BytesIO(); im.save(buf, format="PNG", optimize=True)
    _cache[path] = buf.getvalue()
    return _cache[path]

def solid(rgb):
    px = tuple(int(round(max(0, min(1, c)) * 255)) for c in rgb)
    im = PILImage.new("RGB", (4, 4), px)
    buf = io.BytesIO(); im.save(buf, format="PNG"); return buf.getvalue()

def patch(path):
    g = GLTF2().load(path)
    if not g.samplers: g.samplers.append(Sampler())
    # fresh image/texture tables (old solid images become unreferenced -> dropped on save)
    g.images = []; g.textures = []
    def add(png):
        g.images.append(Image(uri=datauri(png), mimeType="image/png"))
        g.textures.append(Texture(source=len(g.images) - 1, sampler=0))
        return len(g.textures) - 1
    for m in g.materials:
        arch = m.name.replace("X3_", "")
        pbr = m.pbrMetallicRoughness
        if arch in MAPS:
            base_p, orm_p, nrm_p = MAPS[arch]
            pbr.baseColorTexture = TextureInfo(index=add(load_tex(base_p)))
            pbr.baseColorFactor = BCF[arch]
            pbr.metallicFactor = 1.0; pbr.roughnessFactor = 1.0
            orm_png = load_tex(orm_p) if orm_p else solid(SOLID_ORM[arch])
            pbr.metallicRoughnessTexture = TextureInfo(index=add(orm_png))
            if nrm_p:
                m.normalTexture = NormalMaterialTexture(index=add(load_tex(nrm_p)))
            else:
                m.normalTexture = None
            # keep glass warm-interior emissive so panes glow at night
            if arch == "glass":
                m.emissiveFactor = [0.30, 0.20, 0.10]
                m.emissiveTexture = TextureInfo(index=add(solid((0.9, 0.6, 0.3))))
            else:
                m.emissiveFactor = [0.0, 0.0, 0.0]; m.emissiveTexture = None
        elif arch == "neon":
            # keep the emissive neon strip; give it a solid emissive + base
            pbr.baseColorTexture = TextureInfo(index=add(solid((0.02, 0.06, 0.08))))
            pbr.metallicRoughnessTexture = TextureInfo(index=add(solid((1.0, 0.4, 0.0))))
            m.emissiveTexture = TextureInfo(index=add(solid((0.1, 1.0, 1.3))))
            m.emissiveFactor = [0.1, 1.0, 1.3]
            m.normalTexture = None
    g.save_binary(path)
    return len(g.images)

def main():
    for name in GLBS:
        p = os.path.normpath(os.path.join(OUT_DIR, name))
        before = os.path.getsize(p)
        n = patch(p)
        after = os.path.getsize(p)
        print(f"{name:34} images={n}  {before/1e6:.2f}MB -> {after/1e6:.2f}MB")
    print("done.")

if __name__ == "__main__":
    main()
