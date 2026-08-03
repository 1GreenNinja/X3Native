#!/usr/bin/env python3
"""leartes_reskin.py — heal untextured Leartes GLBs by NAME-FAMILY texture matching.

The Recife .unitypackage ships EMPTY placeholder .mats (all slots {fileID:0}) — the
GUID chain is dead. But the pack's textures follow a name convention that maps from
material names: MI_Concrete_01 -> T_Concrete_D.TGA (+_N, +_MRAOH/_RMA). This
post-pass walks every GLB, and for each material WITHOUT a baseColor texture,
fuzzy-resolves its texture family from the pack's Art/Textures and injects:
  baseColor (T_*_D / _albedo), normal (T_*_N), metallicRoughness built from
  MRAOH/RMA channels (glTF: G=roughness, B=metallic; occlusion R left in).

Usage: python leartes_reskin.py <glb_dir> <textures_dir>
"""
import io, os, re, sys
import numpy as np
from PIL import Image
from pygltflib import (GLTF2, Image as GImage, Texture, TextureInfo, Sampler,
                       NormalMaterialTexture, BufferView)

MAX_TEX = 1024

def norm(s):
    s = re.sub(r'^(mi_|m_)', '', s.lower())
    s = re.sub(r'[^a-z]', '', s)          # letters only
    return s

def tex_families(tdir):
    """family stem -> {kind: path} where kind in d/n/mr."""
    fams = {}
    for f in os.listdir(tdir):
        m = re.match(r'^t_(.+?)_(d|albedo|n|mraoh|rma|mask)\.(tga|png|bmp|jpg)$', f.lower())
        if not m: continue
        stem, kind = norm(m.group(1)), m.group(2)
        kind = {'albedo': 'd', 'mraoh': 'mr', 'rma': 'mr', 'mask': 'mr'}.get(kind, kind)
        fams.setdefault(stem, {}).setdefault(kind, os.path.join(tdir, f))
    return fams

def best_family(mat, fams):
    mn = norm(mat)
    if not mn: return None
    best, score = None, 0
    for stem, kinds in fams.items():
        if 'd' not in kinds: continue
        if stem in mn or mn in stem:
            s = len(stem)
            if s > score: best, score = kinds, s
    return best

def png_bytes(path, mode=None, transform=None):
    im = Image.open(path)
    im = im.convert(mode or ('RGBA' if im.mode == 'RGBA' else 'RGB'))
    w, h = im.size
    if max(w, h) > MAX_TEX:
        sc = MAX_TEX / max(w, h)
        im = im.resize((max(1, int(w*sc)), max(1, int(h*sc))), Image.LANCZOS)
    if transform: im = transform(im)
    buf = io.BytesIO(); im.save(buf, 'PNG'); return buf.getvalue()

def mraoh_to_mr(im):
    """MRAOH/RMA -> glTF metallicRoughness (R=occl, G=rough, B=metal)."""
    a = np.asarray(im.convert('RGBA'), dtype=np.uint8)
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    out = np.stack([b, g, r], axis=-1)   # heuristics: R=metal,G=rough,B=AO -> occl/rough/metal
    return Image.fromarray(out, 'RGB')

def add_image(g, blob, name):
    # append to binary blob via a new bufferView on buffer 0
    data = g.binary_blob()
    off = len(data); pad = (4 - off % 4) % 4
    g.set_binary_blob(data + b'\x00'*pad + blob)
    g.bufferViews.append(BufferView(buffer=0, byteOffset=off+pad, byteLength=len(blob)))
    g.images.append(GImage(bufferView=len(g.bufferViews)-1, mimeType='image/png', name=name))
    g.textures.append(Texture(source=len(g.images)-1, sampler=0 if g.samplers else None))
    return len(g.textures)-1

def heal(path, fams):
    g = GLTF2().load(path)
    if not g.materials: return 0
    if not g.samplers: g.samplers = [Sampler()]
    touched = 0
    for m in g.materials:
        pbr = m.pbrMetallicRoughness
        if pbr is None or pbr.baseColorTexture is not None: continue
        fam = best_family(m.name or '', fams)
        if not fam: continue
        ti = add_image(g, png_bytes(fam['d']), m.name + '_D')
        pbr.baseColorTexture = TextureInfo(index=ti)
        pbr.baseColorFactor = [1.0, 1.0, 1.0, 1.0]
        if 'n' in fam:
            tn = add_image(g, png_bytes(fam['n']), m.name + '_N')
            m.normalTexture = NormalMaterialTexture(index=tn)
        if 'mr' in fam:
            tm = add_image(g, png_bytes(fam['mr'], transform=mraoh_to_mr), m.name + '_MR')
            pbr.metallicRoughnessTexture = TextureInfo(index=tm)
            pbr.metallicFactor = 1.0; pbr.roughnessFactor = 1.0
        else:
            pbr.metallicFactor = 0.0; pbr.roughnessFactor = 0.85
        touched += 1
    if touched:
        g.save(path)
    return touched

def main():
    glb_dir, tdir = sys.argv[1], sys.argv[2]
    fams = tex_families(tdir)
    print(f"[reskin] {len(fams)} texture families")
    total = files = 0
    for f in sorted(os.listdir(glb_dir)):
        if not f.lower().endswith('.glb'): continue
        try:
            t = heal(os.path.join(glb_dir, f), fams)
        except Exception as e:
            print(f"[reskin] FAIL {f}: {e}"); continue
        if t: files += 1; total += t
    print(f"[reskin] DONE — {total} materials textured across {files} GLBs")

if __name__ == '__main__':
    main()
