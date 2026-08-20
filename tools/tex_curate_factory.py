#!/usr/bin/env python3
"""Curate the CONFECTION ANNEX (feat/factory-annex, Phase 5 art pass) surface
sets from the Z: pack library into assets/surface_library/fa_*/.

Same channel law as tools/tex_curate.py (ART_BIBLE 4): the engine samples the
glTF MR convention — G = roughness, B = metallic (mesh.frag metallic = mr.b);
R carries AO when the source has one, else 255. Unity sources store
metallic in RGB and SMOOTHNESS in alpha, so roughness = 1 - A ("unity_ms");
sources with no smoothness data get a per-set flat fallback.

Everything is downscaled to 1024^2 (matches the existing library) and
re-encoded PNG. Sources are read from the Z: fileserver mirror of the pack
library (\\i9devpc\assets); run on a box with Z: mounted.

Sets (Phase-5 art-direction table):
  fa_iron_wall     corrugated riveted iron (ScansFactory)  -> annex shell walls
  fa_copper_aged   patinated old copper (DLNK MetalRusted02) -> vats, pipes
  fa_brass_worn    smudged bright metal (DLNK BareDirtMetal00), brass-tinted in-engine
  fa_enamel_cream  smooth painted metal (Cyberpunk kit)    -> machine bodies, sign
  fa_wood_planks   dark plank run (DLNK WoodPlanks00)      -> footbridge decks
  fa_tile_checker  red/cream checkerboard (DLNK WallTiles01) -> B inlay + C floor
  fa_marble_white  polished white marble (DLNK MarbleWhite00) -> the burst dais
  fa_fabric_pad    strapped pale tarp bale (Industrial Fabric Pack 04) -> padded room
"""
import os
import sys

import numpy as np
from PIL import Image

Image.MAX_IMAGE_PIXELS = None
LIB = os.path.join("assets", "surface_library")
OUT_RES = 1024

DLNK = r"Z:\Assets\Stronghold Village\_DLNK\_DLNK Source\_DLNK Libraries\[dlnk Texture Library]\Materials"
SCANS = r"Z:\Assets\Warehouse - Abandoned Factory District\ScansFactory\Warehouse\Common\Textures"
CYBER = r"Z:\Assets\Cyberpunk City Cyberpunk Cyberpunk City Sci-Fi City\HIVEMIND\CyberpunkCity\HDRP(Default)\Art\Textures\Tileable\Metals"
FABRIC = r"Z:\Assets\Industrial Fabric Pack\FabricPack\Textures"

# name -> dict(albedo, normal, [mask 'unity_ms'], [ao], fallback (metal, rough))
SETS = {
    "fa_iron_wall": {
        "albedo": SCANS + r"\t_MetalRustyWall_01_01_bc.png",
        "normal": SCANS + r"\t_MetalRustyWall_01_01_n.png",
        # _m is flat white metallic with no smoothness data -> flat fallback.
        "fallback": (0.85, 0.60),
    },
    "fa_copper_aged": {
        "albedo": DLNK + r"\Metal\MetalRusted02\MetalRusted02_Albedo.png",
        "normal": DLNK + r"\Metal\MetalRusted02\MetalRusted02_Normal.png",
        "mask":   DLNK + r"\Metal\MetalRusted02\MetalRusted02_Metallic.png",
        # Patina is mostly dielectric; keep a metal floor so rims still ping.
        "metal_floor": 0.35,
    },
    "fa_brass_worn": {
        "albedo": DLNK + r"\Metal\BareDirtMetal00\BareDirtMetal00_Albedo.png",
        "normal": DLNK + r"\Metal\BareDirtMetal00\BareDirtMetal00_Normal.png",
        "mask":   DLNK + r"\Metal\BareDirtMetal00\BareDirtMetal00_Metallic.png",  # RGB only
        "ao":     DLNK + r"\Metal\BareDirtMetal00\BareDirtMetal00_Occlusion.png",
        "fallback": (0.85, 0.42),
        "metal_floor": 0.85,   # brass IS metal; the source's 0.43 reads plastic
    },
    "fa_enamel_cream": {
        "albedo": CYBER + r"\T_PaintedMetal_D.png",
        "normal": CYBER + r"\T_PaintedMetal_N.png",
        "fallback": (0.10, 0.32),
    },
    "fa_wood_planks": {
        "albedo": DLNK + r"\Wood\WoodPlanks00\WoodPlanks00.png",
        "normal": DLNK + r"\Wood\WoodPlanks00\WoodPlanks00_NORM.png",
        "mask":   DLNK + r"\Wood\WoodPlanks00\WoodPlanks00_mns.png",
        "ao":     DLNK + r"\Wood\WoodPlanks00\WoodPlanks00_AO.png",
    },
    "fa_tile_checker": {
        "albedo": DLNK + r"\Tiles\WallTiles01\WallTiles01_alb3.png",
        "normal": DLNK + r"\Tiles\WallTiles01\WallTiles01_nrm.png",
        "mask":   DLNK + r"\Tiles\WallTiles01\WallTiles01_mns.png",
        "ao":     DLNK + r"\Tiles\WallTiles01\WallTiles01_ao.png",
    },
    "fa_marble_white": {
        "albedo": DLNK + r"\Marble\MarbleWhite00\MarbleWhite00_Albedo.png",
        "normal": DLNK + r"\Marble\MarbleWhite00\MarbleWhite00_Normal.png",
        "mask":   DLNK + r"\Marble\MarbleWhite00\MarbleWhite00_Metallic.png",
        "ao":     DLNK + r"\Marble\MarbleWhite00\MarbleWhite00_Occlusion.png",
    },
    "fa_fabric_pad": {
        "albedo": FABRIC + r"\T_FabricPack04_AlbedoTransparency.png",
        "normal": FABRIC + r"\T_FabricPack04_Normal.png",
        "mask":   FABRIC + r"\T_FabricPack04_MetallicSmoothness.png",
        "rough_ceil": 0.95,
    },
}


def load_rgba(path):
    return np.asarray(Image.open(path).convert("RGBA"), dtype=np.float32) / 255.0


def save_png(arr01, path, mode="RGB"):
    a = np.clip(arr01 * 255.0 + 0.5, 0, 255).astype(np.uint8)
    im = Image.fromarray(a, mode)
    if im.size != (OUT_RES, OUT_RES):
        im = im.resize((OUT_RES, OUT_RES), Image.LANCZOS)
    im.save(path)


def curate(name, spec):
    outdir = os.path.join(LIB, name)
    os.makedirs(outdir, exist_ok=True)

    alb = load_rgba(spec["albedo"])
    save_png(alb[..., :3], os.path.join(outdir, "albedo.png"))

    nrm = load_rgba(spec["normal"])
    save_png(nrm[..., :3], os.path.join(outdir, "normal.png"))

    h, w = alb.shape[:2]
    mr = np.zeros((h, w, 3), dtype=np.float32)
    mr[..., 0] = 1.0                                   # AO default
    if "mask" in spec:                                 # unity_ms: RGB=metal A=smooth
        m = load_rgba(spec["mask"])
        if m.shape[:2] != (h, w):
            m = np.asarray(Image.fromarray(
                (m * 255).astype(np.uint8), "RGBA").resize((w, h), Image.LANCZOS),
                dtype=np.float32) / 255.0
        metal = m[..., 0]
        rough = 1.0 - m[..., 3]
        if float(m[..., 3].min()) > 0.999:             # no smoothness data
            rough = np.full_like(metal, spec.get("fallback", (0.5, 0.5))[1])
    else:
        fm, fr = spec["fallback"]
        metal = np.full((h, w), fm, dtype=np.float32)
        rough = np.full((h, w), fr, dtype=np.float32)
    if "metal_floor" in spec:
        metal = np.maximum(metal, spec["metal_floor"])
    if "rough_ceil" in spec:
        rough = np.minimum(rough, spec["rough_ceil"])
    if "ao" in spec:
        ao = load_rgba(spec["ao"])
        if ao.shape[:2] != (h, w):
            ao = np.asarray(Image.fromarray(
                (ao * 255).astype(np.uint8), "RGBA").resize((w, h), Image.LANCZOS),
                dtype=np.float32) / 255.0
        mr[..., 0] = ao[..., 0]
    mr[..., 1] = rough
    mr[..., 2] = metal
    save_png(mr, os.path.join(outdir, "mr.png"))
    print(f"[fa-curate] {name}: albedo {alb.shape[1]}x{alb.shape[0]} -> {OUT_RES} "
          f"(metal mean {float(np.mean(metal)):.2f}, rough mean {float(np.mean(rough)):.2f})")


def main():
    only = set(sys.argv[1:])
    for name, spec in SETS.items():
        if only and name not in only:
            continue
        curate(name, spec)


if __name__ == "__main__":
    main()
