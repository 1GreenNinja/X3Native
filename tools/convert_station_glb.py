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
import sys, os, subprocess, tempfile, struct, argparse, io

import numpy as np
from PIL import Image
from pygltflib import GLTF2, Material, PbrMetallicRoughness, Texture, Image as GLTFImage, \
    TextureInfo, NormalMaterialTexture, BufferView, Buffer

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


def repack_metallic_smoothness(ms_path):
    """Unity MetallicSmoothness (metal=R, smoothness=A) -> glTF MR PNG (rough=G, metal=B)."""
    im = load_rgba(ms_path)
    a = np.asarray(im, dtype=np.uint8)
    metal = a[..., 0]
    smooth = a[..., 3]
    rough = 255 - smooth
    out = np.zeros_like(a)
    out[..., 0] = 0          # R unused
    out[..., 1] = rough      # G = roughness (glTF)
    out[..., 2] = metal      # B = metallic (glTF)
    out[..., 3] = 255
    return Image.fromarray(out, "RGBA")


def tint_emission(em_path, rgb):
    """Multiply the emission map by a warm tint so windows glow the right color."""
    im = load_rgba(em_path)
    a = np.asarray(im, dtype=np.float32)
    for c in range(3):
        a[..., c] *= rgb[c]
    a = np.clip(a, 0, 255).astype(np.uint8)
    return Image.fromarray(a, "RGBA")


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
    g.convert_buffers(None)  # ensure data buffers are inline bytes we can extend

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
    if ms_path:  images.append((repack_metallic_smoothness(ms_path),     "metalRough","data"))
    if em_path:  images.append((tint_emission(em_path, emis_rgb),        "emissive",  "srgb"))

    # ---- append the PNGs into the GLB's single binary buffer --------------
    blob = g.binary_blob() or b""
    blob = bytearray(blob)
    # pad to 4
    while len(blob) % 4:
        blob.append(0)

    idx_of = {}
    for im, role, _ in images:
        data = png_bytes(im)
        off = len(blob)
        bv = BufferView()
        bv.buffer = 0
        bv.byteOffset = off
        bv.byteLength = len(data)
        g.bufferViews.append(bv)
        bv_index = len(g.bufferViews) - 1
        gi = GLTFImage()
        gi.bufferView = bv_index
        gi.mimeType = "image/png"
        gi.name = f"{args.family}_{role}.png"
        g.images.append(gi)
        img_index = len(g.images) - 1
        t = Texture()
        t.source = img_index
        g.textures.append(t)
        idx_of[role] = len(g.textures) - 1
        blob += data
        while len(blob) % 4:
            blob.append(0)

    # rewrite the single buffer
    if not g.buffers:
        g.buffers.append(Buffer())
    g.buffers[0].byteLength = len(blob)
    g.buffers[0].uri = None
    g.set_binary_blob(bytes(blob))

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
