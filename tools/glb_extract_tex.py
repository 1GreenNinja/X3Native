"""Extract the embedded images from a weapon GLB so we can SEE the shared texture and
identify which weapon it belongs to. Writes tools/tex_<name>_img<N>_<role>.png"""
import struct, json, os, sys
ROOT=r"C:\GameDev\X3Native-engine\assets\rigged_glb"
name=sys.argv[1] if len(sys.argv)>1 else "WeaponBFG"
p=os.path.join(ROOT,name+".glb")
with open(p,"rb") as f: data=f.read()
_,_,length=struct.unpack_from("<III",data,0); off=12; gltf=None; binoff=0
while off<length:
    clen,ctype=struct.unpack_from("<II",data,off); off+=8
    if ctype==0x4E4F534A: gltf=json.loads(data[off:off+clen].decode("utf-8"))
    elif ctype==0x004E4942: binoff=off
    off+=clen
bvs=gltf["bufferViews"]; imgs=gltf.get("images",[]); texs=gltf.get("textures",[]); mats=gltf.get("materials",[])
# which image is baseColor (via material->baseColorTexture->texture.source)
base_src=None
pbr=mats[0].get("pbrMetallicRoughness",{})
if "baseColorTexture" in pbr:
    base_src=texs[pbr["baseColorTexture"]["index"]].get("source")
for i,im in enumerate(imgs):
    bv=bvs[im["bufferView"]]; o=binoff+bv.get("byteOffset",0); n=bv["byteLength"]
    ext="png" if "png" in im.get("mimeType","") else "jpg"
    role = "baseColor" if i==base_src else ("img%d"%i)
    out=os.path.join(r"C:\GameDev\X3Native-engine\tools", f"tex_{name}_{i}_{role}.{ext}")
    with open(out,"wb") as g: g.write(data[o:o+n])
    print(f"wrote {out}  ({n//1024} KB)  {'<-- BASECOLOR' if i==base_src else ''}")
