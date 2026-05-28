"""Inspect weapon GLBs' material/texture/UV setup to find why one looks right and the
rest are garbled. Parses the glTF JSON chunk (no deps). Usage: python glb_inspect.py"""
import struct, json, os, sys

WEAPONS = ["WeaponEnergyPistol2","WeaponRailgun","WeaponShotgun2","WeaponBFG",
           "WeaponRocketLauncher","WeaponEnergyPistol","WeaponShotGun"]
ROOT = r"C:\GameDev\X3Native-engine\assets\rigged_glb"

def load_gltf(path):
    with open(path,"rb") as f:
        data=f.read()
    magic,ver,length = struct.unpack_from("<III", data, 0)
    off=12; gltf=None; bin_len=0
    while off < length:
        clen,ctype = struct.unpack_from("<II", data, off); off+=8
        chunk=data[off:off+clen]; off+=clen
        if ctype==0x4E4F534A: gltf=json.loads(chunk.decode("utf-8"))
        elif ctype==0x004E4942: bin_len=clen
    return gltf, bin_len, len(data)

for w in WEAPONS:
    p=os.path.join(ROOT,w+".glb")
    if not os.path.exists(p): continue
    g,binlen,total = load_gltf(p)
    mats=g.get("materials",[]); imgs=g.get("images",[]); texs=g.get("textures",[])
    bvs=g.get("bufferViews",[])
    # image byte sizes (via bufferView) + mime
    imginfo=[]
    for im in imgs:
        mime=im.get("mimeType","?")
        if "bufferView" in im and im["bufferView"]<len(bvs):
            imginfo.append(f"{mime.split('/')[-1]}:{bvs[im['bufferView']].get('byteLength',0)//1024}KB")
        else:
            imginfo.append(f"{mime}:URI={im.get('uri','?')[:20]}")
    # UVs present on any primitive?
    has_uv=False; prim_attrs=set()
    for m in g.get("meshes",[]):
        for pr in m.get("primitives",[]):
            for a in pr.get("attributes",{}): prim_attrs.add(a)
            if "TEXCOORD_0" in pr.get("attributes",{}): has_uv=True
    # material summary
    matsum=[]
    for m in mats:
        pbr=m.get("pbrMetallicRoughness",{})
        bct = "tex#%s"%pbr["baseColorTexture"]["index"] if "baseColorTexture" in pbr else "NO-TEX"
        bcf = pbr.get("baseColorFactor","-")
        emt = "EMIS" if ("emissiveTexture" in m or m.get("emissiveFactor",[0,0,0])!=[0,0,0]) else ""
        matsum.append(f"{bct} factor={bcf} {emt}")
    print(f"\n=== {w}.glb ({total//1024}KB total, bin {binlen//1024}KB) ===")
    print(f"  materials={len(mats)} textures={len(texs)} images={len(imgs)}  UVs(TEXCOORD_0)={'YES' if has_uv else 'NO'}  attrs={sorted(prim_attrs)}")
    print(f"  images: {imginfo}")
    for i,ms in enumerate(matsum): print(f"  mat[{i}]: {ms}")
