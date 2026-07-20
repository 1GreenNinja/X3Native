#!/usr/bin/env python3
"""inject_emissive.py — restore the neon the FBX->GLB conversion dropped.

Leartes (Recife) authors most neon as HDRP material EMISSIVE COLOR (lost in
conversion) + a few emissive MAPS (T_BuildBG_Emissive, T_Trim_01_EmissiveMask,
T_Emissive_Poste01 — never wired). This script post-processes armory GLBs:

  A) NAME-DRIVEN FACTOR INJECTION — materials/files whose names read as neon
     (screen/holo/sign/poste/neon/letreiro/...) get emissiveFactor + a
     KHR_materials_emissive_strength (engine: cgltf multiplies them; sRGB OK).
  B) MAP WIRING — materials whose image names reference BuildBG / Trim_01 get
     the matching pack emissive TGA embedded (PNG) as emissiveTexture.

GLB layout is preserved: JSON chunk rewritten (4-byte space padding), PNGs
appended to the END of BIN (no offset shifts), buffer/chunk/total lengths fixed.
Modified files get a .bak sibling once (never overwrite an existing .bak).

Usage: python inject_emissive.py <glb_dir> <textures_dir> [--dry]
"""
import io, json, os, re, struct, sys

# (regex on material/file name, emissiveFactor rgb, strength)
RULES = [
    (r'bigscreen|screen|telao|monitor',      [0.45, 0.62, 1.00], 12.0),   # cool screen glow
    (r'holo|emissor',                        [0.30, 0.95, 1.00], 14.0),   # cyan hologram
    (r'poste|streetlamp|lampiao',            [1.00, 0.80, 0.45], 16.0),   # warm street lamp
    (r'letreiro|sign|sing|neon|luminoso',    [1.00, 0.25, 0.85], 12.0),   # magenta neon
    (r'coinhologram|coin',                   [1.00, 0.85, 0.20], 12.0),   # gold coin holo
    (r'drone',                               [1.00, 0.30, 0.20], 8.0),   # drone tail light
]
# image-name substring -> emissive TGA to embed
MAP_RULES = [
    ('buildbg', 'T_BuildBG_Emissive.TGA',     [1,1,1], 10.0),   # BG tower windows
    ('trim_01', 'T_Trim_01_EmissiveMask.TGA', [0.3,0.9,1.0], 12.0),  # neon trim strips
    ('poste01', 'T_Emissive_Poste01_D.TGA',   [1,1,1], 16.0),
]

def read_glb(path):
    data = open(path, 'rb').read()
    assert data[:4] == b'glTF', path
    total = struct.unpack_from('<I', data, 8)[0]
    off, chunks = 12, []
    while off < total:
        clen, ctyp = struct.unpack_from('<II', data, off)
        chunks.append([ctyp, bytearray(data[off+8:off+8+clen])])
        off += 8 + clen
    return chunks

def write_glb(path, chunks):
    out = io.BytesIO()
    body = b''
    for ctyp, cdat in chunks:
        pad = (4 - len(cdat) % 4) % 4
        cdat = bytes(cdat) + (b' ' if ctyp == 0x4E4F534A else b'\0') * pad
        body += struct.pack('<II', len(cdat), ctyp) + cdat
    out.write(b'glTF' + struct.pack('<II', 2, 12 + len(body)) + body)
    open(path, 'wb').write(out.getvalue())

def tga_png_bytes(tga_path, cache={}):
    if tga_path in cache: return cache[tga_path]
    from PIL import Image
    im = Image.open(tga_path).convert('RGB')
    if max(im.size) > 1024:                      # neon maps don't need 4K
        im.thumbnail((1024, 1024))
    buf = io.BytesIO(); im.save(buf, 'PNG', optimize=True)
    cache[tga_path] = buf.getvalue()
    return cache[tga_path]

def process(path, texdir, dry):
    chunks = read_glb(path)
    js = json.loads(bytes(chunks[0][1]).decode('utf-8'))
    fname = os.path.basename(path).lower()
    mats = js.get('materials', [])
    if not mats: return None
    images = js.get('images', [])
    textures = js.get('textures', [])
    touched = []

    def image_name(mat):
        pbr = mat.get('pbrMetallicRoughness', {})
        bt = pbr.get('baseColorTexture')
        if not bt: return ''
        src = textures[bt['index']].get('source')
        return (images[src].get('name', '') if src is not None and src < len(images) else '').lower()

    for mi, mat in enumerate(mats):
        if 'emissiveTexture' in mat or mat.get('emissiveFactor'): continue  # already lit
        mname = (mat.get('name', '') or '').lower()
        iname = image_name(mat)
        # B) map wiring first (more specific)
        wired = False
        for sub, tga, ef, strength in MAP_RULES:
            if sub in iname or sub in mname:
                tga_path = os.path.join(texdir, tga)
                if not os.path.exists(tga_path): continue
                if not dry:
                    png = tga_png_bytes(tga_path)
                    binc = chunks[1][1]
                    while len(binc) % 4: binc.append(0)
                    bv_off = len(binc); binc.extend(png)
                    js.setdefault('bufferViews', []).append(
                        {'buffer': 0, 'byteOffset': bv_off, 'byteLength': len(png)})
                    js.setdefault('images', []).append(
                        {'mimeType': 'image/png', 'bufferView': len(js['bufferViews'])-1,
                         'name': os.path.splitext(tga)[0]})
                    js.setdefault('textures', []).append({'source': len(js['images'])-1})
                    mat['emissiveTexture'] = {'index': len(js['textures'])-1}
                    mat['emissiveFactor'] = ef
                    mat.setdefault('extensions', {})['KHR_materials_emissive_strength'] = \
                        {'emissiveStrength': strength}
                touched.append(f"{mat.get('name')}<=map:{tga}")
                wired = True
                break
        if wired: continue
        # A) name-driven factor injection
        for rx, ef, strength in RULES:
            if re.search(rx, mname) or re.search(rx, fname):
                if not dry:
                    mat['emissiveFactor'] = ef
                    mat.setdefault('extensions', {})['KHR_materials_emissive_strength'] = \
                        {'emissiveStrength': strength}
                touched.append(f"{mat.get('name')}<=factor:{rx}")
                break

    if not touched: return None
    if not dry:
        if any('map:' in t for t in touched):
            js['buffers'][0]['byteLength'] = len(chunks[1][1])
        ext = js.setdefault('extensionsUsed', [])
        if 'KHR_materials_emissive_strength' not in ext:
            ext.append('KHR_materials_emissive_strength')
        chunks[0][1] = bytearray(json.dumps(js, separators=(',', ':')).encode('utf-8'))
        bak = path + '.bak'
        if not os.path.exists(bak): os.replace(path, bak)
        write_glb(path, chunks)
    return touched

def main():
    glb_dir, texdir = sys.argv[1], sys.argv[2]
    dry = '--dry' in sys.argv
    hits = 0
    for f in sorted(os.listdir(glb_dir)):
        if not f.lower().endswith('.glb'): continue
        t = process(os.path.join(glb_dir, f), texdir, dry)
        if t:
            hits += 1
            print(('DRY  ' if dry else 'LIT  ') + f + '  ' + '; '.join(t[:3]) +
                  (f' (+{len(t)-3})' if len(t) > 3 else ''))
    print(f"{'would light' if dry else 'lit'} {hits} GLBs")

if __name__ == '__main__':
    main()
