#!/usr/bin/env python3
"""Convert a Unity asset pack's meshes -> textured GLB for the X3Native engine.

Unity FBX carry NO texture paths (they live in the .mat materials + atlas PNGs),
and many packs ship ASCII FBX that Blender refuses. So:
  1. FBX2glTF (Autodesk FBX SDK; reads ASCII) -> GLB geometry + NAMED material slots.
  2. Resolve textures from Unity: .mat slot -> texture GUID -> <file>.png.meta GUID -> PNG.
  3. Repack Unity's _MetallicGlossMap (metallic=R, smoothness=A) into glTF
     metallicRoughness (B=metallic, G=roughness=1-smoothness). Convert .tif->png.
  4. Inject the maps into the glTF material (matched by name) + save a self-contained GLB.

Usage:
  python convert_unity_pack.py <pack_assets_dir> <fbx|all> <out_dir>
    pack_assets_dir : folder containing Meshes/, Textures/, Meshes/Materials/
    fbx             : a single .FBX filename (under Meshes/, recursive) or "all"
    out_dir         : where the .glb files are written
"""
import sys, os, re, glob, subprocess, tempfile
from PIL import Image
import numpy as np
import pygltflib
from pygltflib import GLTF2, TextureInfo, Texture, Sampler, Image as GImage

FBX2GLTF = r"C:\GameDev\tools\FBX2glTF.exe"
_GUID = re.compile(r"guid:\s*([0-9a-fA-F]{32})")
MAX_TEX = 512  # cap atlas dimension: shared 4K atlases embedded per-mesh balloon the GLB
               # (for production prefer shared external textures or convert only used meshes)

def log(*a): print("[conv]", *a, flush=True)

def _fit(im):
    w, h = im.size
    m = max(w, h)
    if m > MAX_TEX:
        s = MAX_TEX / float(m)
        im = im.resize((max(1, int(w * s)), max(1, int(h * s))), Image.LANCZOS)
    return im

def build_guid_map(textures_dir):
    """guid -> absolute texture file path (from every <file>.meta in Textures/)."""
    out = {}
    for meta in glob.glob(os.path.join(textures_dir, "*.meta")):
        try:
            head = open(meta, "r", encoding="utf-8", errors="ignore").read(400)
        except Exception:
            continue
        m = _GUID.search(head)
        if m:
            tex = meta[:-5]  # strip ".meta"
            if os.path.exists(tex):
                out[m.group(1).lower()] = tex
    return out

# Unity material slot -> the texenv key in the .mat
_SLOTS = ("_MainTex", "_BumpMap", "_MetallicGlossMap", "_OcclusionMap", "_EmissionMap")

def parse_materials(materials_dir):
    """material name -> {slot: guid, 'glossScale': f, 'emisColor': (r,g,b)}."""
    mats = {}
    for mat in glob.glob(os.path.join(materials_dir, "*.mat")):
        txt = open(mat, "r", encoding="utf-8", errors="ignore").read()
        name_m = re.search(r"m_Name:\s*(.+)", txt)
        name = name_m.group(1).strip() if name_m else os.path.splitext(os.path.basename(mat))[0]
        rec = {}
        for slot in _SLOTS:
            # _Slot:\n  m_Texture: {fileID: N, guid: <hex>, type: 3}
            m = re.search(slot + r":\s*\n\s*m_Texture:\s*\{[^}]*?guid:\s*([0-9a-fA-F]{32})", txt)
            if m:
                rec[slot] = m.group(1).lower()
        gm = re.search(r"_GlossMapScale:\s*([0-9.]+)", txt)
        rec["glossScale"] = float(gm.group(1)) if gm else 1.0
        ec = re.search(r"_EmissionColor:\s*\{r:\s*([0-9.eE-]+),\s*g:\s*([0-9.eE-]+),\s*b:\s*([0-9.eE-]+)", txt)
        rec["emisColor"] = (float(ec.group(1)), float(ec.group(2)), float(ec.group(3))) if ec else (0.0, 0.0, 0.0)
        mats[name] = rec
    return mats

def to_png(src_path, cache):
    """Ensure src is a PNG (convert .tif/.psd). Returns a PNG path (cached)."""
    if src_path in cache:
        return cache[src_path]
    ext = os.path.splitext(src_path)[1].lower()
    if ext == ".png":
        cache[src_path] = src_path
        return src_path
    out = os.path.join(tempfile.gettempdir(), "x3conv_" + os.path.splitext(os.path.basename(src_path))[0] + ".png")
    try:
        _fit(Image.open(src_path).convert("RGBA")).save(out)
        cache[src_path] = out
        return out
    except Exception as e:
        log("WARN to_png failed", src_path, e)
        cache[src_path] = None
        return None

def repack_mr(metalgloss_path, gloss_scale, cache):
    """Unity _MetallicGlossMap (R=metallic, A=smoothness) -> glTF metallicRoughness
       (G=roughness=1-smoothness*scale, B=metallic). Returns a PNG path."""
    key = ("mr", metalgloss_path, round(gloss_scale, 4))
    if key in cache:
        return cache[key]
    try:
        im = _fit(Image.open(metalgloss_path).convert("RGBA"))
        a = np.asarray(im).astype(np.float32) / 255.0
        metallic = a[..., 0]                       # Unity: metallic in R
        smooth = a[..., 3] if im.mode == "RGBA" else a[..., 0]  # smoothness in A
        rough = np.clip(1.0 - smooth * gloss_scale, 0.0, 1.0)
        h, w = metallic.shape
        out = np.zeros((h, w, 3), np.float32)
        out[..., 1] = rough        # G = roughness
        out[..., 2] = metallic     # B = metallic
        op = os.path.join(tempfile.gettempdir(), "x3mr_" + os.path.splitext(os.path.basename(metalgloss_path))[0] + ".png")
        Image.fromarray((out * 255.0).astype(np.uint8), "RGB").save(op)
        cache[key] = op
        return op
    except Exception as e:
        log("WARN repack_mr failed", metalgloss_path, e)
        cache[key] = None
        return None

def _add_image(gltf, png_path, srgb_sampler_idx, texcache):
    """Append an image (by file uri) + texture; dedup repeats within this GLB
       (the kit's meshes share a few atlases, so the same map recurs across slots)."""
    if png_path in texcache:
        return texcache[png_path]
    img_idx = len(gltf.images)
    gltf.images.append(GImage(uri=png_path.replace("\\", "/")))
    tex_idx = len(gltf.textures)
    gltf.textures.append(Texture(source=img_idx, sampler=srgb_sampler_idx))
    texcache[png_path] = tex_idx
    return tex_idx

def convert_one(fbx, out_glb, guidmap, matmap, png_cache):
    tmp = os.path.join(tempfile.gettempdir(), "x3geo_" + os.path.splitext(os.path.basename(fbx))[0] + ".glb")
    r = subprocess.run([FBX2GLTF, "-b", "-i", fbx, "-o", tmp], capture_output=True, text=True)
    if not os.path.exists(tmp):
        log("FBX2glTF FAILED", fbx, r.stderr[-400:] if r.stderr else r.stdout[-400:])
        return False
    gltf = GLTF2().load(tmp)
    if not gltf.samplers:
        gltf.samplers.append(Sampler())  # default repeat/linear
    samp = 0
    n_tex = 0
    texcache = {}
    for gm in gltf.materials:
        rec = matmap.get(gm.name)
        if not rec:
            continue
        if gm.pbrMetallicRoughness is None:
            from pygltflib import PbrMetallicRoughness
            gm.pbrMetallicRoughness = PbrMetallicRoughness()
        pbr = gm.pbrMetallicRoughness
        # base color
        g = rec.get("_MainTex");  f = guidmap.get(g) if g else None
        if f and (p := to_png(f, png_cache)):
            pbr.baseColorTexture = TextureInfo(index=_add_image(gltf, p, samp, texcache)); n_tex += 1
        # normal
        g = rec.get("_BumpMap");  f = guidmap.get(g) if g else None
        if f and (p := to_png(f, png_cache)):
            from pygltflib import NormalMaterialTexture
            gm.normalTexture = NormalMaterialTexture(index=_add_image(gltf, p, samp, texcache)); n_tex += 1
        # metallic-roughness (repacked)
        g = rec.get("_MetallicGlossMap"); f = guidmap.get(g) if g else None
        if f and (p := repack_mr(f, rec.get("glossScale", 1.0), png_cache)):
            pbr.metallicRoughnessTexture = TextureInfo(index=_add_image(gltf, p, samp, texcache))
            pbr.metallicFactor = 1.0; pbr.roughnessFactor = 1.0; n_tex += 1
        # occlusion
        g = rec.get("_OcclusionMap"); f = guidmap.get(g) if g else None
        if f and (p := to_png(f, png_cache)):
            from pygltflib import OcclusionTextureInfo
            gm.occlusionTexture = OcclusionTextureInfo(index=_add_image(gltf, p, samp, texcache)); n_tex += 1
        # emissive
        g = rec.get("_EmissionMap"); f = guidmap.get(g) if g else None
        if f and (p := to_png(f, png_cache)):
            gm.emissiveTexture = TextureInfo(index=_add_image(gltf, p, samp, texcache))
            gm.emissiveFactor = [1.0, 1.0, 1.0]; n_tex += 1
    # Embed all the file-uri images so the GLB is self-contained. pygltflib 1.16.5
    # cannot pack images into BUFFERVIEWs, so use DATAURI (base64 in the JSON chunk) —
    # still a valid GLB, and the engine's cgltf loader decodes data-URI images.
    gltf.convert_images(pygltflib.ImageFormat.DATAURI)
    os.makedirs(os.path.dirname(out_glb), exist_ok=True)
    gltf.save_binary(out_glb)
    log("OK %-28s materials=%d textures+=%d -> %.0f KB"
        % (os.path.basename(fbx), len(gltf.materials), n_tex, os.path.getsize(out_glb) / 1024))
    return True

def main():
    pack, which, out_dir = sys.argv[1], sys.argv[2], sys.argv[3]
    tex_dir = os.path.join(pack, "Textures")
    mat_dir = os.path.join(pack, "Meshes", "Materials")
    guidmap = build_guid_map(tex_dir)
    matmap = parse_materials(mat_dir)
    log("guidmap=%d textures, matmap=%d materials" % (len(guidmap), len(matmap)))
    png_cache = {}
    if which.lower() == "all":
        fbxs = glob.glob(os.path.join(pack, "Meshes", "**", "*.FBX"), recursive=True)
    else:
        fbxs = glob.glob(os.path.join(pack, "Meshes", "**", which), recursive=True) or [which]
    ok = 0
    for fbx in fbxs:
        stem = os.path.splitext(os.path.basename(fbx))[0]
        if convert_one(fbx, os.path.join(out_dir, stem + ".glb"), guidmap, matmap, png_cache):
            ok += 1
    log("DONE %d/%d converted" % (ok, len(fbxs)))

if __name__ == "__main__":
    main()
