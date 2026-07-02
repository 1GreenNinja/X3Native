#!/usr/bin/env python3
"""Finish a clean-geometric SPACE-STATION / SHIP-BRIDGE GLB for X3Native
(the INTRO HERO-ASSET pipeline; sibling of convert_car_glb.py).

The "Sci-Fi Space Stations Creator" pack ships plain FBX (a single DefaultMaterial,
NO embedded textures) plus an EXTERNAL Unity PBR texture SET per family
(<Family>_<Color>.png, _Normal, _MetallicSmoothness, _Emission1/2). FBX2glTF can't
read Unity .mat assignments, so this script:

  1. FBX2glTF (handles ASCII FBX) -> raw GLB with the named station mesh.
  2. BINDS the external Unity texture set onto the material(s):
       * baseColor  <- <Family>_<Color>.png        (sRGB)
       * normal     <- <Family>_Normal.png          (linear)
       * metalRough <- repacked from <Family>_MetallicSmoothness.png
                       (Unity: metal=R, smoothness=A) -> glTF MR (rough=G=1-smooth,
                       metal=B=R), linear. So the chrome/hull reads as metal.
       * emissive   <- <Family>_Emission1.png        (sRGB) — the WINDOW arrays;
                       KHR_materials_emissive_strength boosts it so the windows
                       GLOW warm against cold space + feed the HDR bloom chain.
  3. Tags the hull material extras["x3Clearcoat"] (the glossy Kelvin-Trek hull lobe).
  4. Embeds every PNG into the GLB buffer (bufferView images, like CTR.glb), so the
     result is one self-contained binary glTF the asset store can publish.

Usage:
  python tools/convert_station_glb.py <src.fbx|src.glb> <dst.glb> \\
      --texdir <unity texture dir> --family <Family> \\
      [--color Grey] [--emis-rgb 1.0,0.86,0.55] [--emis-strength 6.0] \\
      [--clearcoat 0.8,0.06] [--no-clearcoat]

  --family    texture filename prefix, e.g. "Cylinderical" or "Bridge"
  --color     base color variant (default Grey -> clean Starfleet hull)
  --emis-rgb  tint multiplied onto the emission map (warm window glow)
  --emis-strength  KHR emissive strength (window bloom punch)
  --clearcoat intensity,roughness  (glossy hull lobe; default 0.8,0.06)

ASCII-only logs. Shares the FBX2glTF + pygltflib path with convert_car_glb.py.
"""
import sys, os, subprocess, tempfile, argparse, io, base64

import numpy as np
from PIL import Image
from pygltflib import GLTF2, Material, PbrMetallicRoughness, Texture, Image as GLTFImage, \
    TextureInfo, NormalMaterialTexture

FBX2GLTF = r"C:\GameDev\tools\FBX2glTF.exe"


def log(*a):
    print("[station]", *a, flush=True)


def find_tex(texdir, family, *suffixes):
    """First existing <texdir>/<family>_<suffix>.png for any suffix (case-insensitive)."""
    for s in suffixes:
        for ext in (".png", ".PNG", ".jpg", ".tga"):
            p = os.path.join(texdir, f"{family}_{s}{ext}")
            if os.path.isfile(p):
                return p
    # fallback: scan the dir
    low = {f.lower(): f for f in os.listdir(texdir)}
    for s in suffixes:
        key = f"{family}_{s}".lower()
        for lf, orig in low.items():
            if lf.startswith(key) and lf.endswith((".png", ".jpg", ".tga")):
                return os.path.join(texdir, orig)
    return None


def png_bytes(img):
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    return buf.getvalue()


def load_rgba(path):
    return Image.open(path).convert("RGBA")


def repack_metallic_smoothness(ms_path, max_metal=1.0, min_rough=0.0):
    """Unity MetallicSmoothness (metal=R, smoothness=A) -> glTF MR PNG (rough=G, metal=B).
    `max_metal` caps metalness (0..1) so a pure-metal hull still takes DIFFUSE light
    in dark space (a fully-metallic hull only shows reflections -> reads black against
    a starfield). `min_rough` floors roughness so the clearcoat reads as a clean gloss
    rather than a mirror."""
    im = load_rgba(ms_path)
    a = np.asarray(im, dtype=np.float32)
    metal = np.minimum(a[..., 0], max_metal * 255.0)
    smooth = a[..., 3]
    rough = np.maximum(255.0 - smooth, min_rough * 255.0)
    out = np.zeros_like(a)
    out[..., 0] = 0          # R unused
    out[..., 1] = rough      # G = roughness (glTF)
    out[..., 2] = metal      # B = metallic (glTF)
    out[..., 3] = 255
    return Image.fromarray(out.astype(np.uint8), "RGBA")


def tint_emission(em_path, rgb):
    """RECOLOR the emission map to a warm window glow. The pack's Emission map is
    cyan-dominant; a plain multiply still reads green. Instead take per-pixel
    luminance as the window MASK and paint it the target warm tint, so the lit
    windows blaze warm against cold space (the Kelvin-Trek look)."""
    im = load_rgba(em_path)
    a = np.asarray(im, dtype=np.float32)
    lum = 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]   # 0..255 window mask
    out = np.zeros_like(a)
    for c in range(3):
        out[..., c] = np.clip(lum * rgb[c], 0, 255)
    out[..., 3] = 255
    return Image.fromarray(out.astype(np.uint8), "RGBA")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--texdir", required=True)
    ap.add_argument("--family", required=True)
    ap.add_argument("--color", default="Grey")
    ap.add_argument("--emis-rgb", default="1.0,0.86,0.55")
    ap.add_argument("--emis-strength", type=float, default=6.0)
    ap.add_argument("--clearcoat", default="0.8,0.06")
    ap.add_argument("--no-clearcoat", action="store_true")
    ap.add_argument("--max-metal", type=float, default=1.0,
                    help="cap hull metalness so it takes diffuse light in dark space")
    ap.add_argument("--min-rough", type=float, default=0.0)
    args = ap.parse_args()

    emis_rgb = [float(x) for x in args.emis_rgb.split(",")][:3]

    # 1) FBX -> raw GLB if needed.
    raw = args.src
    if args.src.lower().endswith(".fbx"):
        tmp = os.path.join(tempfile.gettempdir(), "x3station_raw")
        r = subprocess.run([FBX2GLTF, "--binary", "--input", args.src, "--output", tmp],
                           capture_output=True, text=True)
        if r.returncode != 0:
            log("FBX2glTF FAILED:", r.stdout[-400:], r.stderr[-400:])
            sys.exit(1)
        raw = tmp + ".glb"
    g = GLTF2().load(raw)
    # NOTE: do NOT touch the geometry buffer. Textures are added as base64
    # data-URI images so accessor bufferViews stay byte-identical to FBX2glTF's
    # output (an earlier append-to-buffer approach corrupted vertex/UV accessors
    # -> NaN). cgltf (the engine loader) decodes data-URI PNGs natively.

    # ---- collect + prepare the texture images -----------------------------
    bc_path  = find_tex(args.texdir, args.family, args.color, "White", "Grey", "Silver")
    nrm_path = find_tex(args.texdir, args.family, "Normal")
    ms_path  = find_tex(args.texdir, args.family, "MetallicSmoothness", "MetalSmooth", "Metallic")
    em_path  = find_tex(args.texdir, args.family, "Emission1", "Emission", "Emissive")
    log("baseColor:", bc_path)
    log("normal:   ", nrm_path)
    log("metalRgh: ", ms_path)
    log("emission: ", em_path)

    images = []   # (PIL.Image, name)
    if bc_path:  images.append((load_rgba(bc_path),                      "baseColor", "srgb"))
    if nrm_path: images.append((load_rgba(nrm_path),                     "normal",    "data"))
    if ms_path:  images.append((repack_metallic_smoothness(ms_path, args.max_metal, args.min_rough), "metalRough","data"))
    if em_path:  images.append((tint_emission(em_path, emis_rgb),        "emissive",  "srgb"))

    # ---- add the PNGs as base64 data-URI images (geometry buffer untouched) ---
    idx_of = {}
    for im, role, _ in images:
        data = png_bytes(im)
        gi = GLTFImage()
        gi.uri = "data:image/png;base64," + base64.b64encode(data).decode("ascii")
        gi.mimeType = "image/png"
        gi.name = f"{args.family}_{role}.png"
        g.images.append(gi)
        img_index = len(g.images) - 1
        t = Texture()
        t.source = img_index
        g.textures.append(t)
        idx_of[role] = len(g.textures) - 1

    # ---- wire the material(s) --------------------------------------------
    cc = None if args.no_clearcoat else [float(x) for x in args.clearcoat.split(",")][:2]
    if not g.materials:
        g.materials.append(Material())
    for m in g.materials:
        if m.pbrMetallicRoughness is None:
            m.pbrMetallicRoughness = PbrMetallicRoughness()
        p = m.pbrMetallicRoughness
        p.baseColorFactor = [1, 1, 1, 1]
        if "baseColor" in idx_of:
            p.baseColorTexture = TextureInfo(index=idx_of["baseColor"])
        if "metalRough" in idx_of:
            p.metallicRoughnessTexture = TextureInfo(index=idx_of["metalRough"])
            p.metallicFactor = 1.0
            p.roughnessFactor = 1.0
        else:
            p.metallicFactor = 0.65
            p.roughnessFactor = 0.35
        if "normal" in idx_of:
            m.normalTexture = NormalMaterialTexture(index=idx_of["normal"])
        if "emissive" in idx_of:
            m.emissiveTexture = TextureInfo(index=idx_of["emissive"])
            m.emissiveFactor = [1, 1, 1]
            exts = m.extensions if isinstance(m.extensions, dict) else {}
            exts["KHR_materials_emissive_strength"] = {"emissiveStrength": args.emis_strength}
            m.extensions = exts
            if "KHR_materials_emissive_strength" not in (g.extensionsUsed or []):
                g.extensionsUsed = (g.extensionsUsed or []) + ["KHR_materials_emissive_strength"]
        if cc is not None:
            ex = m.extras if isinstance(m.extras, dict) else {}
            ex["x3Clearcoat"] = {"intensity": cc[0], "roughness": cc[1]}
            m.extras = ex
        m.doubleSided = True   # station hull panels: never back-cull a window-through

    os.makedirs(os.path.dirname(os.path.abspath(args.dst)), exist_ok=True)
    g.save(args.dst)
    log("WROTE", args.dst, f"({os.path.getsize(args.dst)} bytes); textures bound:",
        list(idx_of.keys()), "clearcoat:", cc)


if __name__ == "__main__":
    main()
