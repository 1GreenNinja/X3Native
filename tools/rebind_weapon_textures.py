# Rebind each weapon viewmodel GLB to ITS OWN Rodin PBR texture set.
#
# ROOT-CAUSE FIX for the "camo/pale slab" weapons: every weapon GLB had been
# built (and then reskin_weapon_glb.py-tinted) with ONE shared texture atlas, so
# four of the five weapons wore a foreign skin sampled through their own UVs ->
# garbage. Each weapon actually ships its OWN baked-albedo + normal + metallic +
# roughness maps in its Rodin source folder; this tool binds the CORRECT set into
# each GLB, geometry untouched (byte-identical meshes -> zero scale/pose drift).
#
# For each weapon it writes a clean glTF PBR material:
#     baseColorTexture         <- texture_diffuse.png   (sRGB, real albedo)
#     normalTexture            <- texture_normal.png     (linear)
#     metallicRoughnessTexture <- combine(metallic->B, roughness->G)  (linear)
#   baseColorFactor=[1,1,1,1] metallic=1 roughness=1 emissive=0  (NO tint/fake glow;
#   the energy accents are BAKED into the albedo already).
#
# Usage: python tools/rebind_weapon_textures.py [--check]
# ASCII-only on purpose.
import struct, json, os, sys, io
from PIL import Image

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GLB_DIR = os.path.join(REPO, "assets", "rigged_glb")
RODIN = r"D:\GameDev\EscapeLab3D-RBDOOM\base\models\x3\rodin"

# target GLB (in repo)      -> Rodin source folder (its own textures)
WEAPONS = {
    "WeaponEnergyPistol2.glb": "WeaponEnergyPistol",
    "WeaponRailgun.glb":       "WeaponRailGun",
    "WeaponBFG.glb":           "WeaponBFG",
    "WeaponRocketLauncher.glb":"WeaponRocketLauncher",
}

JSON_CHUNK = 0x4E4F534A
BIN_CHUNK  = 0x004E4942

def load_glb(path):
    d = open(path, "rb").read()
    magic, ver, length = struct.unpack_from("<III", d, 0)
    assert magic == 0x46546C67, "not a GLB"
    off = 12; g = None; blob = b""
    while off < length:
        clen, ctype = struct.unpack_from("<II", d, off); off += 8
        ch = d[off:off+clen]; off += clen
        if ctype == JSON_CHUNK: g = json.loads(ch.decode("utf-8"))
        elif ctype == BIN_CHUNK: blob = ch
    return g, blob

def save_glb(path, g, blob):
    js = json.dumps(g, separators=(",", ":")).encode("utf-8")
    js += b" " * ((4 - len(js) % 4) % 4)
    bn = blob + b"\x00" * ((4 - len(blob) % 4) % 4)
    total = 12 + 8 + len(js) + 8 + len(bn)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(js), JSON_CHUNK)); f.write(js)
        f.write(struct.pack("<II", len(bn), BIN_CHUNK));  f.write(bn)

def png_bytes(img):
    b = io.BytesIO(); img.save(b, format="PNG"); return b.getvalue()

def build_maps(srcdir):
    diff = Image.open(os.path.join(srcdir, "texture_diffuse.png")).convert("RGB")
    norm = Image.open(os.path.join(srcdir, "texture_normal.png")).convert("RGB")
    met  = Image.open(os.path.join(srcdir, "texture_metallic.png")).convert("L")
    rgh  = Image.open(os.path.join(srcdir, "texture_roughness.png")).convert("L")
    if met.size != rgh.size: met = met.resize(rgh.size)
    # glTF metallicRoughness: G=roughness, B=metallic, R=occlusion(=1 here).
    occ = Image.new("L", rgh.size, 255)
    mr = Image.merge("RGB", (occ, rgh, met))
    return png_bytes(diff), png_bytes(norm), png_bytes(mr)

def rebind(glb_path, srcdir, name):
    g, blob = load_glb(glb_path)
    images = g.get("images", [])
    img_bv = set(im["bufferView"] for im in images if "bufferView" in im)

    # Keep every non-image bufferView (all geometry); remap old->new index.
    old_bvs = g["bufferViews"]
    keep = [i for i in range(len(old_bvs)) if i not in img_bv]
    remap = {old: new for new, old in enumerate(keep)}

    new_blob = bytearray()
    new_bvs = []
    def place(data, extra=None):
        pad = (4 - len(new_blob) % 4) % 4
        new_blob.extend(b"\x00" * pad)
        off = len(new_blob)
        new_blob.extend(data)
        bv = {"buffer": 0, "byteOffset": off, "byteLength": len(data)}
        if extra: bv.update(extra)
        new_bvs.append(bv)

    for old in keep:
        bv = old_bvs[old]
        o = bv.get("byteOffset", 0); l = bv["byteLength"]
        extra = {k: bv[k] for k in ("byteStride", "target") if k in bv}
        place(bytes(blob[o:o+l]), extra)

    diff_png, norm_png, mr_png = build_maps(srcdir)
    base_img_idx = len(new_bvs)
    place(diff_png); place(norm_png); place(mr_png)  # bv indices base+0,+1,+2

    # Remap accessor bufferView references.
    for a in g.get("accessors", []):
        if "bufferView" in a: a["bufferView"] = remap[a["bufferView"]]
        if "sparse" in a:
            sp = a["sparse"]
            for key in ("indices", "values"):
                if key in sp and "bufferView" in sp[key]:
                    sp[key]["bufferView"] = remap[sp[key]["bufferView"]]

    g["bufferViews"] = new_bvs
    g["buffers"] = [{"byteLength": len(new_blob)}]
    g["images"] = [
        {"mimeType": "image/png", "bufferView": base_img_idx + 0, "name": "baseColor"},
        {"mimeType": "image/png", "bufferView": base_img_idx + 1, "name": "normal"},
        {"mimeType": "image/png", "bufferView": base_img_idx + 2, "name": "metallicRoughness"},
    ]
    g["samplers"] = [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}]
    g["textures"] = [
        {"sampler": 0, "source": 0},  # 0 baseColor
        {"sampler": 0, "source": 1},  # 1 normal
        {"sampler": 0, "source": 2},  # 2 metallicRoughness
    ]
    g["materials"] = [{
        "name": "Weapon_" + name,
        "pbrMetallicRoughness": {
            "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
            "metallicFactor": 1.0,
            "roughnessFactor": 1.0,
            "baseColorTexture": {"index": 0, "texCoord": 0},
            "metallicRoughnessTexture": {"index": 2, "texCoord": 0},
        },
        "normalTexture": {"index": 1, "scale": 1.0, "texCoord": 0},
        "emissiveFactor": [0.0, 0.0, 0.0],
        "alphaMode": "OPAQUE",
        "doubleSided": True,
    }]
    # Drop the fake-glow extension.
    used = [e for e in g.get("extensionsUsed", []) if e != "KHR_materials_emissive_strength"]
    if used: g["extensionsUsed"] = used
    elif "extensionsUsed" in g: del g["extensionsUsed"]

    # Every primitive uses material 0.
    for m in g.get("meshes", []):
        for pr in m.get("primitives", []):
            pr["material"] = 0

    save_glb(glb_path, g, new_blob)
    print("[rebind] %-26s <- %s  (%d bv, buffer %d KB)" %
          (os.path.basename(glb_path), os.path.basename(srcdir), len(new_bvs), len(new_blob)//1024))

def main():
    for glb, src in WEAPONS.items():
        gp = os.path.join(GLB_DIR, glb); sd = os.path.join(RODIN, src)
        if not os.path.exists(gp): print("MISSING GLB", gp); continue
        if not os.path.isdir(sd): print("MISSING SRC", sd); continue
        rebind(gp, sd, src)

if __name__ == "__main__":
    main()
