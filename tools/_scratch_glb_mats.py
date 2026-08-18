"""Scratch: dump a GLB's materials + textures/images (no deps). Not for commit."""
import json
import struct
import sys

path = sys.argv[1]
data = open(path, "rb").read()
magic, ver, length = struct.unpack_from("<III", data, 0)
off = 12
gltf = None
binlen = 0
while off < length:
    clen, ctype = struct.unpack_from("<II", data, off)
    off += 8
    chunk = data[off:off + clen]
    off += clen
    if ctype == 0x4E4F534A:
        gltf = json.loads(chunk.decode("utf-8"))
    elif ctype == 0x004E4942:
        binlen = clen
print("file bytes:", len(data), "bin chunk:", binlen)
print("images:", len(gltf.get("images", [])))
bvs = gltf.get("bufferViews", [])
for i, im in enumerate(gltf.get("images", [])):
    size = bvs[im["bufferView"]]["byteLength"] // 1024 if "bufferView" in im else -1
    print("  img", i, im.get("mimeType"), f"{size}KB", im.get("name", ""))
for i, m in enumerate(gltf.get("materials", [])):
    pbr = m.get("pbrMetallicRoughness", {})
    print("  mat", i, m.get("name", ""),
          "baseTex" if "baseColorTexture" in pbr else "noBaseTex",
          "mrTex" if "metallicRoughnessTexture" in pbr else "noMR",
          "normTex" if "normalTexture" in m else "noNorm",
          "emisTex" if "emissiveTexture" in m else "",
          "metallic=", pbr.get("metallicFactor"), "rough=", pbr.get("roughnessFactor"),
          "base=", pbr.get("baseColorFactor"))
exts = gltf.get("extensionsUsed", [])
print("extensions:", exts)
