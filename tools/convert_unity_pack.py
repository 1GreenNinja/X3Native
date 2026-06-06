#!/usr/bin/env python3
"""Convert a Unity asset pack's meshes -> textured GLB for the X3Native engine.

Unity FBX carry NO texture paths (they live in the .mat materials + atlas PNGs),
and many packs ship ASCII FBX that Blender refuses. So:
  1. FBX2glTF (Autodesk FBX SDK; reads ASCII) -> GLB geometry + NAMED material slots.
  2. Resolve textures from Unity: .mat slot -> texture GUID -> <file>.png.meta GUID -> PNG.
  3. Repack the finish map into glTF metallicRoughness:
       * Built-in Standard: _MetallicGlossMap (R=metallic, A=smoothness).
       * HDRP/Lit: _MaskMap (R=metallic, G=AO, B=detail, A=smoothness) + per-channel
         remaps (_SmoothnessRemapMin/Max, _MetallicRemapMin/Max, _AORemapMin/Max).
     Both -> ORM (R=occlusion, G=roughness=1-smoothness, B=metallic). Convert .tif->png.
  4. Inject the maps into the glTF material (matched by name) + save a self-contained GLB.

HDRP packs (e.g. ShowRoom_Vol30) differ critically from Built-in:
  - texture slots are _BaseColorMap/_NormalMap/_MaskMap (not _MainTex/_BumpMap/_MetallicGlossMap);
  - ALL color comes from the per-material _BaseColor tint (the shared atlas is grey) — so the
    tint MUST be written to baseColorFactor or every finish collapses to grey;
  - MaskMaps live alongside the .mat in Meshes/Materials/, not Textures/.

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
MAX_TEX = 1024  # cap atlas dimension. Per-mesh FBX conversion embeds atlases per-file so
                # 4K balloons the GLB; the assembled-scene re-skin (repack-glb) DEDUPS shared
                # atlases to one image each, so 1024 stays sharp without bloat.

def log(*a): print("[conv]", *a, flush=True)

def _fit(im):
    w, h = im.size
    m = max(w, h)
    if m > MAX_TEX:
        s = MAX_TEX / float(m)
        im = im.resize((max(1, int(w * s)), max(1, int(h * s))), Image.LANCZOS)
    return im

def build_guid_map(pack_root):
    """guid -> absolute texture file path, from every <file>.meta anywhere under the pack.
       Scanned recursively: HDRP MaskMaps live in Meshes/Materials/, Terrain maps under
       Terrain/, base atlases under Textures/ — all need covering."""
    out = {}
    for meta in glob.glob(os.path.join(pack_root, "**", "*.meta"), recursive=True):
        try:
            head = open(meta, "r", encoding="utf-8", errors="ignore").read(400)
        except Exception:
            continue
        m = _GUID.search(head)
        if m:
            tex = meta[:-5]  # strip ".meta"
            ext = os.path.splitext(tex)[1].lower()
            if os.path.exists(tex) and ext in (".png", ".tif", ".tga", ".psd", ".jpg", ".jpeg", ".exr"):
                out.setdefault(m.group(1).lower(), tex)
    return out

# Unity material texture slots. Built-in Standard + HDRP/Lit both handled.
_SLOTS = ("_MainTex", "_BumpMap", "_MetallicGlossMap", "_OcclusionMap", "_EmissionMap",
          "_BaseColorMap", "_NormalMap", "_MaskMap", "_DetailMap", "_EmissiveColorMap")

def _scalar(txt, key, default):
    m = re.search(r"-?\s*" + key + r":\s*([0-9.eE-]+)", txt)
    return float(m.group(1)) if m else default

def _color(txt, key, default):
    m = re.search(key + r":\s*\{r:\s*([0-9.eE-]+),\s*g:\s*([0-9.eE-]+),\s*b:\s*([0-9.eE-]+)(?:,\s*a:\s*([0-9.eE-]+))?", txt)
    if not m:
        return default
    a = float(m.group(4)) if m.group(4) is not None else 1.0
    return (float(m.group(1)), float(m.group(2)), float(m.group(3)), a)

def parse_materials(pack_root):
    """material name -> rec dict (texture guids + HDRP/Built-in scalars & remaps).
       Keyed by the .mat FILENAME stem, which matches the GLB material names exactly —
       the in-file m_Name is unreliable (a blank/script-header m_Name can precede the
       real one). Scanned recursively so Terrain/ and other subfolders are included."""
    mats = {}
    for mat in glob.glob(os.path.join(pack_root, "**", "*.mat"), recursive=True):
        txt = open(mat, "r", encoding="utf-8", errors="ignore").read()
        name = os.path.splitext(os.path.basename(mat))[0]
        rec = {}
        for slot in _SLOTS:
            # _Slot:\n  m_Texture: {fileID: N, guid: <hex>, type: 3}
            m = re.search(slot + r":\s*\n\s*m_Texture:\s*\{[^}]*?guid:\s*([0-9a-fA-F]{32})", txt)
            if m:
                rec[slot] = m.group(1).lower()
        # HDRP if it carries the HDRP/Lit base-color slot (Built-in has _MainTex instead).
        rec["hdrp"] = "_BaseColorMap:" in txt or "_MaskMap:" in txt
        # emission ONLY when _EMISSION is an ACTIVE keyword (m_ValidKeywords). HDRP also
        # lists _EMISSION under m_InvalidKeywords / the full keyword set when it's OFF, so a
        # blanket search wrongly flags non-emissive mats (e.g. chrome) and a flat emissive
        # then makes whole walls self-glow white regardless of lighting.
        vk = re.search(r"m_ValidKeywords:\s*\n((?:\s*-\s*\S+\n)*)", txt)
        rec["emission"] = bool(vk) and "_EMISSION" in vk.group(1)
        rec["glossScale"] = _scalar(txt, "_GlossMapScale", 1.0)
        # HDRP per-channel remaps applied to the MaskMap (default = identity passthrough).
        rec["smoothMin"] = _scalar(txt, "_SmoothnessRemapMin", 0.0)
        rec["smoothMax"] = _scalar(txt, "_SmoothnessRemapMax", 1.0)
        rec["metalMin"]  = _scalar(txt, "_MetallicRemapMin", 0.0)
        rec["metalMax"]  = _scalar(txt, "_MetallicRemapMax", 1.0)
        rec["aoMin"]     = _scalar(txt, "_AORemapMin", 0.0)
        rec["aoMax"]     = _scalar(txt, "_AORemapMax", 1.0)
        # scalar fallbacks when there is no MaskMap.
        rec["metallic"]   = _scalar(txt, "_Metallic", 0.0)
        rec["smoothness"] = _scalar(txt, "_Smoothness", 0.5)
        # _BaseColor is the ONLY source of color on HDRP atlas materials.
        rec["baseColor"] = _color(txt, "_BaseColor", (1.0, 1.0, 1.0, 1.0))
        rec["emisColor"] = _color(txt, "_EmissiveColor", (0.0, 0.0, 0.0, 1.0))[:3] \
                           if "_EmissiveColor:" in txt \
                           else _color(txt, "_EmissionColor", (0.0, 0.0, 0.0, 1.0))[:3]
        mats[name] = rec
    return mats

def to_png(src_path, cache):
    """Ensure src is a PNG capped at MAX_TEX (convert .tif/.psd, downscale oversized
       atlases — an 8K normal map embedded full-res balloons the GLB). Cached."""
    if src_path in cache:
        return cache[src_path]
    try:
        im = Image.open(src_path)
        fitted = _fit(im)
        ext = os.path.splitext(src_path)[1].lower()
        if fitted is im and ext == ".png":
            cache[src_path] = src_path           # already a small PNG — use as-is
            return src_path
        out = os.path.join(tempfile.gettempdir(), "x3conv_" + os.path.splitext(os.path.basename(src_path))[0] + ".png")
        fitted.save(out)
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

def repack_maskmap(mask_path, rec, cache):
    """HDRP _MaskMap (R=metallic, G=AO, B=detail, A=smoothness) -> glTF ORM
       (R=occlusion, G=roughness=1-smoothness, B=metallic), applying HDRP per-channel
       remaps. occlusionTexture(R) + metallicRoughnessTexture(G,B) can share this image."""
    key = ("mask", mask_path, round(rec["smoothMin"], 4), round(rec["smoothMax"], 4),
           round(rec["metalMin"], 4), round(rec["metalMax"], 4),
           round(rec["aoMin"], 4), round(rec["aoMax"], 4))
    if key in cache:
        return cache[key]
    try:
        im = _fit(Image.open(mask_path).convert("RGBA"))
        a = np.asarray(im).astype(np.float32) / 255.0
        metallic = np.clip(rec["metalMin"] + a[..., 0] * (rec["metalMax"] - rec["metalMin"]), 0.0, 1.0)
        ao       = np.clip(rec["aoMin"]    + a[..., 1] * (rec["aoMax"]    - rec["aoMin"]),    0.0, 1.0)
        smooth   = np.clip(rec["smoothMin"] + a[..., 3] * (rec["smoothMax"] - rec["smoothMin"]), 0.0, 1.0)
        rough = 1.0 - smooth
        h, w = metallic.shape
        out = np.zeros((h, w, 3), np.float32)
        out[..., 0] = ao          # R = occlusion
        out[..., 1] = rough       # G = roughness
        out[..., 2] = metallic    # B = metallic
        op = os.path.join(tempfile.gettempdir(), "x3orm_" + os.path.splitext(os.path.basename(mask_path))[0] + ".png")
        Image.fromarray((out * 255.0).astype(np.uint8), "RGB").save(op)
        cache[key] = op
        return op
    except Exception as e:
        log("WARN repack_maskmap failed", mask_path, e)
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

def apply_materials(gltf, guidmap, matmap, png_cache, rebuild=False):
    """Resolve Unity .mat textures/factors into the GLB's named materials (Built-in or
       HDRP). With rebuild=True, drop the GLB's existing images/textures first and null
       all material texture refs — used to re-skin an already-assembled scene cleanly.
       Returns (n_tex_added, n_materials_matched)."""
    from pygltflib import NormalMaterialTexture, OcclusionTextureInfo, PbrMetallicRoughness
    if rebuild:
        gltf.images = []
        gltf.textures = []
        for gm in gltf.materials:          # avoid dangling indices into the cleared arrays
            if gm.pbrMetallicRoughness:
                gm.pbrMetallicRoughness.baseColorTexture = None
                gm.pbrMetallicRoughness.metallicRoughnessTexture = None
            gm.normalTexture = None
            gm.occlusionTexture = None
            gm.emissiveTexture = None
            gm.emissiveFactor = [0.0, 0.0, 0.0]   # reset: the assembled GLB baked bright
                                                  # wall emissive; re-add only for real fixtures
    if not gltf.samplers:
        gltf.samplers.append(Sampler())    # default repeat/linear
    samp = 0
    n_tex = 0
    matched = 0
    texcache = {}
    for gm in gltf.materials:
        rec = matmap.get(gm.name)
        if not rec:
            continue
        matched += 1
        if gm.pbrMetallicRoughness is None:
            gm.pbrMetallicRoughness = PbrMetallicRoughness()
        pbr = gm.pbrMetallicRoughness
        hdrp = rec.get("hdrp")
        bc_slot   = "_BaseColorMap"     if hdrp else "_MainTex"
        nrm_slot  = "_NormalMap"        if hdrp else "_BumpMap"
        emis_slot = "_EmissiveColorMap" if hdrp else "_EmissionMap"

        # base color: HDRP atlas mats share ONE grey texture; the color lives in _BaseColor.
        g = rec.get(bc_slot);  f = guidmap.get(g) if g else None
        if f and (p := to_png(f, png_cache)):
            pbr.baseColorTexture = TextureInfo(index=_add_image(gltf, p, samp, texcache)); n_tex += 1
        if hdrp:
            pbr.baseColorFactor = list(rec["baseColor"])   # the ONLY color source — always apply

        # normal
        g = rec.get(nrm_slot);  f = guidmap.get(g) if g else None
        if f and (p := to_png(f, png_cache)):
            gm.normalTexture = NormalMaterialTexture(index=_add_image(gltf, p, samp, texcache)); n_tex += 1

        # metallic / roughness (+ occlusion)
        if hdrp and rec.get("_MaskMap") and (f := guidmap.get(rec["_MaskMap"])) \
                and (p := repack_maskmap(f, rec, png_cache)):
            orm = _add_image(gltf, p, samp, texcache)            # ORM: R=AO G=rough B=metal
            pbr.metallicRoughnessTexture = TextureInfo(index=orm)
            gm.occlusionTexture = OcclusionTextureInfo(index=orm)  # HDRP AO is in the same image
            pbr.metallicFactor = 1.0; pbr.roughnessFactor = 1.0; n_tex += 1
        elif (not hdrp) and rec.get("_MetallicGlossMap") \
                and (f := guidmap.get(rec["_MetallicGlossMap"])) \
                and (p := repack_mr(f, rec.get("glossScale", 1.0), png_cache)):
            pbr.metallicRoughnessTexture = TextureInfo(index=_add_image(gltf, p, samp, texcache))
            pbr.metallicFactor = 1.0; pbr.roughnessFactor = 1.0; n_tex += 1
        else:
            # no finish map -> drive from scalars (HDRP _Metallic / _Smoothness).
            pbr.metallicFactor = max(0.0, min(1.0, rec.get("metallic", 0.0)))
            pbr.roughnessFactor = max(0.0, min(1.0, 1.0 - rec.get("smoothness", 0.5)))

        # occlusion (Built-in separate map; HDRP AO already folded into the ORM above)
        if not hdrp:
            g = rec.get("_OcclusionMap"); f = guidmap.get(g) if g else None
            if f and (p := to_png(f, png_cache)):
                gm.occlusionTexture = OcclusionTextureInfo(index=_add_image(gltf, p, samp, texcache)); n_tex += 1

        # emissive — only when _EMISSION is active (gated above) AND a real emissive MAP
        # localizes the glow. A flat full-surface emissiveFactor with no map blows whole
        # walls to white, so we never apply emissive without a map. The HDRP _EmissiveColor
        # is CLAMPED into glTF's [0,1] factor (not normalized — normalizing turned a subtle
        # colour into a near-white glow).
        if (not hdrp) or rec.get("emission"):
            g = rec.get(emis_slot); f = guidmap.get(g) if g else None
            if f and (p := to_png(f, png_cache)):
                gm.emissiveTexture = TextureInfo(index=_add_image(gltf, p, samp, texcache)); n_tex += 1
                ec = rec.get("emisColor", (1.0, 1.0, 1.0))
                gm.emissiveFactor = [min(1.0, max(0.0, c)) for c in ec] if max(ec) > 0 else [1.0, 1.0, 1.0]
    return n_tex, matched

def repack_glb(in_glb, out_glb, guidmap, matmap, png_cache):
    """Re-skin an already-assembled scene GLB in place: keep its geometry, replace every
       named material's textures/factors from the Unity .mat data (HDRP-aware)."""
    gltf = GLTF2().load(in_glb)
    n_tex, matched = apply_materials(gltf, guidmap, matmap, png_cache, rebuild=True)
    gltf.convert_images(pygltflib.ImageFormat.DATAURI)
    os.makedirs(os.path.dirname(os.path.abspath(out_glb)), exist_ok=True)
    gltf.save_binary(out_glb)
    log("REPACK %-24s materials matched=%d/%d textures=%d -> %.0f KB"
        % (os.path.basename(out_glb), matched, len(gltf.materials), n_tex,
           os.path.getsize(out_glb) / 1024))
    return True

def convert_one(fbx, out_glb, guidmap, matmap, png_cache):
    tmp = os.path.join(tempfile.gettempdir(), "x3geo_" + os.path.splitext(os.path.basename(fbx))[0] + ".glb")
    r = subprocess.run([FBX2GLTF, "-b", "-i", fbx, "-o", tmp], capture_output=True, text=True)
    if not os.path.exists(tmp):
        log("FBX2glTF FAILED", fbx, r.stderr[-400:] if r.stderr else r.stdout[-400:])
        return False
    gltf = GLTF2().load(tmp)
    n_tex, _ = apply_materials(gltf, guidmap, matmap, png_cache)
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
    # Mode A (FBX->GLB):   convert_unity_pack.py <pack> <fbx|all> <out_dir>
    # Mode B (re-skin GLB): convert_unity_pack.py repack-glb <pack> <in.glb> <out.glb>
    repack = sys.argv[1].lower() == "repack-glb"
    if repack:
        pack, in_glb, out_glb = sys.argv[2], sys.argv[3], sys.argv[4]
    else:
        pack, which, out_dir = sys.argv[1], sys.argv[2], sys.argv[3]
    guidmap = build_guid_map(pack)   # recursive: Textures/ + Meshes/Materials/ + Terrain/ ...
    matmap = parse_materials(pack)
    n_hdrp = sum(1 for r in matmap.values() if r.get("hdrp"))
    log("guidmap=%d textures, matmap=%d materials (%d HDRP)" % (len(guidmap), len(matmap), n_hdrp))
    png_cache = {}
    if repack:
        repack_glb(in_glb, out_glb, guidmap, matmap, png_cache)
        return
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
