#!/usr/bin/env python3
"""F3 one-shot: wire the HorrorHospital bed textures into the staged GLB.

The pack ships no Unity .mat files, so the converter left all 3 materials
untextured (flat grey 0.216, metallic=1 -> dark chrome). This injects the
ALB + NRM maps as embedded buffer images and sets sane scalar M/R per
material (the pack's RMA masks use HDRP channel order, NOT glTF MR order,
so they are deliberately skipped).

Mapping (verified by reading the atlases):
  M_Bed_01a_Plastic -> TX_M_Hospital_Bed_01a  (beige panels/casters atlas)
  M_Bedframe        -> TX_M_Hospital_Bed_01b  (rusted slats/headboard atlas)
  M_Bed_Matress     -> TX_Hospital_Bed_01c    (mattress + plastic cover atlas)

Usage: python tools/f3_inject_bed_tex.py <in.glb> <out.glb>
"""
import sys
from pathlib import Path
import pygltflib

TEXDIR = Path(r"D:\Assets\Modular Abandoned Hospital Horror Hospital Abandoned hospital Hospital\Hivemind\HorrorHospital\HDRP(Default)\Art\Textures")

# material name -> (ALB, NRM, metallicFactor, roughnessFactor)
WIRING = {
    "M_Bed_01a_Plastic": ("TX_M_Hospital_Bed_01a_ALB.png", "TX_M_Hospital_Bed_01a_NRM.PNG", 0.10, 0.60),
    "M_Bedframe":        ("TX_M_Hospital_Bed_01b_ALB.png", "TX_M_Hospital_Bed_01b_NRM.PNG", 0.80, 0.45),
    "M_Bed_Matress":     ("TX_Hospital_Bed_01c_ALB.png",   "TX_Hospital_Bed_01c_NRM.PNG",   0.00, 0.92),
}

def main(src, dst):
    g = pygltflib.GLTF2().load(src)
    blob = bytearray(g.binary_blob())

    if not g.samplers:
        g.samplers.append(pygltflib.Sampler(
            magFilter=pygltflib.LINEAR, minFilter=pygltflib.LINEAR_MIPMAP_LINEAR,
            wrapS=pygltflib.REPEAT, wrapT=pygltflib.REPEAT))

    def add_image(png_path):
        data = Path(png_path).read_bytes()
        while len(blob) % 4:            # 4-byte alignment per spec
            blob.append(0)
        off = len(blob)
        blob.extend(data)
        g.bufferViews.append(pygltflib.BufferView(buffer=0, byteOffset=off, byteLength=len(data)))
        g.images.append(pygltflib.Image(bufferView=len(g.bufferViews) - 1,
                                        mimeType="image/png",
                                        name=Path(png_path).stem))
        g.textures.append(pygltflib.Texture(sampler=0, source=len(g.images) - 1))
        return len(g.textures) - 1

    wired = 0
    for mat in g.materials:
        w = WIRING.get(mat.name or "")
        if not w:
            print(f"  SKIP unmapped material: {mat.name}")
            continue
        alb, nrm, metal, rough = w
        ti_alb = add_image(TEXDIR / alb)
        ti_nrm = add_image(TEXDIR / nrm)
        pbr = mat.pbrMetallicRoughness
        pbr.baseColorTexture = pygltflib.TextureInfo(index=ti_alb)
        pbr.baseColorFactor = [1.0, 1.0, 1.0, 1.0]
        pbr.metallicFactor = metal
        pbr.roughnessFactor = rough
        mat.normalTexture = pygltflib.NormalMaterialTexture(index=ti_nrm)
        wired += 1
        print(f"  wired {mat.name}: ALB={alb} NRM={nrm} m={metal} r={rough}")

    g.buffers[0].byteLength = len(blob)
    g.set_binary_blob(bytes(blob))
    g.save_binary(dst)
    print(f"wrote {dst} ({Path(dst).stat().st_size} bytes), {wired}/3 materials wired")
    if wired != 3:
        sys.exit(1)

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
