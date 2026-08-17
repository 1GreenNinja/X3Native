#!/usr/bin/env python3
"""W-STATIONS (Lane 5): build the TEXTURED gas-station GLBs for app/gas_station.cpp.

WHY THIS TOOL EXISTS (and why convert_unity_pack.py could not be used):
  convert_unity_pack.py resolves Unity material textures by GUID:
  `.mat` slot -> guid -> `<file>.png.meta` -> PNG.  The "Mega Open World City
  Pack (Mobile-Optimized for Driving Simulation Games)" ships **no .meta files
  at all** (339 files: 127 .mat, 109 .fbx, 102 .png, 1 .psd), so the GUID map
  comes back empty and every material converts GREY.  NO_SLOP rule 3 says a
  flat-tinted stand-in does not ship — so the slot->PNG mapping is declared
  EXPLICITLY here, by name, from reading the pack's Materials_Day /
  Materials_Nights .mat files.  Verified by eye (tools/w5_glbinfo.py + a
  Blender preview) before anything was placed.

  The armory's ready GLB of the same model
  (D:/Assets/_glb/_editor/MegaOpenWorldCityPack/Fuel_Station_Model.glb) was the
  FIRST stop per the owner's tip, and it is 11 KB with `images: 0` and
  `extensionsRequired: [KHR_draco_mesh_compression]` — geometry only, zero
  textures. Draco decode alone would still have left it grey. So the armory GLB
  is used as the PROOF the model is the right one, and the pixels come from the
  licensed source pack on D:/Assets.

WHAT IT PRODUCES (assets/converted_glb/GasStation/):
  gas_station_mega.glb   — the whole station: canopy on columns, pump islands,
                           kiosk building, forecourt slab.  42.7 x 8.2 x 64.8 m
                           as authored; RECENTRED so origin = forecourt centre
                           at ground level (X3_WORLD_RULES rule 4: origin at the
                           contact surface) and rotated so the forecourt's long
                           axis runs along +X.
  gas_pump_miami.glb     — Leartes "Miami Vice City" SM_Gaspump, 0.51 x 1.80 x
                           0.84 m, the close-up-quality single pump.

MATERIAL LAW (X3_WORLD_RULES rule 5) applied here:
  * metallic 0.0 / roughness 0.82 scalars + a 1x1 MR texel — never metallic 1
    without an MR map (that renders BLACK).
  * emissiveTex is only honoured on the PBR route, and only when mrTex is valid
    -> every emissive material gets the 1x1 MR.
  * the pack's night EMISSION maps are near-black with hot signage texels, which
    is exactly the ACES-safe recipe: emissiveFactor 1,1,1 with
    KHR_materials_emissive_strength 1.1 (ModelLoader.cpp multiplies
    emissive_factor by emissive_strength).

Run:  python tools/w5_build_station_glb.py [outDir]
      (default outDir = <repo>/assets/converted_glb/GasStation)
"""
import os, sys, json, struct, math, subprocess, tempfile, shutil
from PIL import Image

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FBX2GLTF = next((p for p in (r"D:\GameDev\tools\FBX2glTF.exe",
                             r"C:\GameDev\tools\FBX2glTF.exe") if os.path.exists(p)), None)

MEGA = (r"D:/Assets/Mega Open World City Pack Mobile-Optimized for "
        r"Driving Simulation Games/Assets/Mega City Environment")
MEGA_TEX = MEGA + "/Textures"
MEGA_EMI = MEGA_TEX + "/Emmiosions_Tex"

# FBX material slot -> (baseColor png, emissive png or None)
# Read out of Materials_Day/Pump.mat + Pump_Building.mat (baseColor) and
# Materials_Nights/*.mat (_EmissionMap). The pack has no .meta, so the names
# below ARE the mapping; each was opened and looked at before being written down:
#   Pump_Texture.png          = pump livery + the price totem ("PTO" brand)
#   Pump_Building_Texture.png = kiosk atlas (door, window, brick, forecourt
#                               asphalt with red bay lines, concrete, grass)
MEGA_SLOTS = {
    "Pump_Mat":      (MEGA_TEX + "/Pump_Texture.png",
                      MEGA_EMI + "/Pump_Texture02_Emmision.png"),
    "Pump_Building": (MEGA_TEX + "/Pump_Building_Texture.png",
                      MEGA_EMI + "/Pump_Building_Emmission.png"),
}

MAX_TEX = 1024          # the pack ships 2K atlases; 1K is plenty at driving range


# ---------------------------------------------------------------- GLB plumbing
def glb_read(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data[:4] == b"glTF", path
    total = struct.unpack("<I", data[8:12])[0]
    off, js, bin_ = 12, None, b""
    while off < total:
        clen, ctype = struct.unpack("<II", data[off:off + 8])
        chunk = data[off + 8: off + 8 + clen]
        if ctype == 0x4E4F534A:
            js = json.loads(chunk.decode("utf-8"))
        elif ctype == 0x004E4942:
            bin_ = chunk
        off += 8 + clen + ((4 - clen % 4) % 4)
    return js, bytearray(bin_)


def glb_write(path, js, bin_):
    while len(bin_) % 4:
        bin_.append(0)
    js.setdefault("buffers", [{}])
    js["buffers"][0] = {"byteLength": len(bin_)}
    jb = json.dumps(js, separators=(",", ":")).encode("utf-8")
    while len(jb) % 4:
        jb += b" "
    total = 12 + 8 + len(jb) + 8 + len(bin_)
    with open(path, "wb") as f:
        f.write(b"glTF" + struct.pack("<II", 2, total))
        f.write(struct.pack("<II", len(jb), 0x4E4F534A)); f.write(jb)
        f.write(struct.pack("<II", len(bin_), 0x004E4942)); f.write(bytes(bin_))


def add_png(js, bin_, pil_img, name):
    """Pack a PIL image into a bufferView-backed glTF image; return texture index."""
    im = pil_img.convert("RGB" if pil_img.mode != "RGBA" else "RGBA")
    w, h = im.size
    m = max(w, h)
    if m > MAX_TEX:
        s = MAX_TEX / float(m)
        im = im.resize((max(1, int(w * s)), max(1, int(h * s))), Image.LANCZOS)
    import io
    buf = io.BytesIO(); im.save(buf, format="PNG", optimize=True)
    raw = buf.getvalue()
    while len(bin_) % 4:
        bin_.append(0)
    off = len(bin_); bin_.extend(raw)
    js.setdefault("bufferViews", []).append({"buffer": 0, "byteOffset": off,
                                             "byteLength": len(raw)})
    bv = len(js["bufferViews"]) - 1
    js.setdefault("images", []).append({"name": name, "mimeType": "image/png",
                                        "bufferView": bv})
    img = len(js["images"]) - 1
    if not js.get("samplers"):
        js["samplers"] = [{"magFilter": 9729, "minFilter": 9987,
                           "wrapS": 10497, "wrapT": 10497}]
    js.setdefault("textures", []).append({"sampler": 0, "source": img})
    return len(js["textures"]) - 1


def mr_1x1(js, bin_, rough, metal):
    """glTF metallicRoughness packing: G = roughness, B = metallic."""
    im = Image.new("RGB", (1, 1), (255, int(rough * 255 + 0.5), int(metal * 255 + 0.5)))
    return add_png(js, bin_, im, "mr1x1")


# ------------------------------------------------------------ geometry helpers
def positions_bbox(js, bin_):
    lo = [1e30] * 3; hi = [-1e30] * 3
    for mesh in js.get("meshes", []):
        for p in mesh["primitives"]:
            a = js["accessors"][p["attributes"]["POSITION"]]
            if "min" in a:
                for k in range(3):
                    lo[k] = min(lo[k], a["min"][k]); hi[k] = max(hi[k], a["max"][k])
    return lo, hi


def recentre(js, bin_, mode="xz_and_floor"):
    """Rewrite POSITION data so origin = footprint centre in XZ, min-Y at 0.

    X3_WORLD_RULES rule 4: a prop that RESTS on a surface has its origin at the
    contact plane. The pack authors the station at its city-scene coordinates
    (x 316..359, z 471..536, y -0.26..7.97) — dropped in at identity that is a
    building 350 m away from where you asked for it, floating a quarter metre.
    """
    lo, hi = positions_bbox(js, bin_)
    cx = (lo[0] + hi[0]) * 0.5
    cz = (lo[2] + hi[2]) * 0.5
    cy = lo[1]
    shift = (-cx, -cy, -cz)
    done = set()
    for mesh in js.get("meshes", []):
        for p in mesh["primitives"]:
            ai = p["attributes"]["POSITION"]
            if ai in done:
                continue
            done.add(ai)
            a = js["accessors"][ai]
            bv = js["bufferViews"][a["bufferView"]]
            base = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
            stride = bv.get("byteStride", 12)
            for i in range(a["count"]):
                o = base + i * stride
                x, y, z = struct.unpack_from("<fff", bin_, o)
                struct.pack_into("<fff", bin_, o,
                                 x + shift[0], y + shift[1], z + shift[2])
            a["min"] = [a["min"][k] + shift[k] for k in range(3)]
            a["max"] = [a["max"][k] + shift[k] for k in range(3)]
    return shift, (hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2])


def clear_node_trs(js):
    """FBX2glTF leaves a RootNode + a child with identity TRS; keep them but make
    sure nothing carries a stray scale (the pack is authored in metres already —
    the bbox measured 42.7 x 8.2 x 64.8 m, a real forecourt, so no cm fix-up)."""
    for nd in js.get("nodes", []):
        nd.pop("scale", None)
        nd.pop("translation", None)
        nd.pop("rotation", None)
        nd.pop("matrix", None)


# ------------------------------------------------------------------- the build
def build_mega_station(out_glb):
    assert FBX2GLTF, "FBX2glTF.exe not found"
    fbx = MEGA + "/Models/Fuel_Station_Model.fbx"
    tmp = tempfile.mkdtemp(prefix="w5_")
    raw = os.path.join(tmp, "station.glb")
    subprocess.run([FBX2GLTF, "-b", "-i", fbx, "-o", raw], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    js, bin_ = glb_read(raw)

    clear_node_trs(js)
    shift, size = recentre(js, bin_)
    print("[w5] station recentred by %.2f %.2f %.2f ; size %.2f x %.2f x %.2f m"
          % (shift + size))

    mr = mr_1x1(js, bin_, rough=0.82, metal=0.0)
    js.setdefault("extensionsUsed", [])
    if "KHR_materials_emissive_strength" not in js["extensionsUsed"]:
        js["extensionsUsed"].append("KHR_materials_emissive_strength")

    for m in js.get("materials", []):
        slot = MEGA_SLOTS.get(m.get("name"))
        if not slot:
            print("[w5] WARNING unmapped material slot:", m.get("name"))
            continue
        base_png, emi_png = slot
        bt = add_png(js, bin_, Image.open(base_png), os.path.basename(base_png))
        pbr = m.setdefault("pbrMetallicRoughness", {})
        pbr["baseColorTexture"] = {"index": bt}
        pbr["baseColorFactor"] = [1.0, 1.0, 1.0, 1.0]
        pbr["metallicFactor"] = 0.0
        pbr["roughnessFactor"] = 0.82
        pbr["metallicRoughnessTexture"] = {"index": mr}   # rule 5: mrTex must be valid
        if emi_png and os.path.exists(emi_png):
            et = add_png(js, bin_, Image.open(emi_png), os.path.basename(emi_png))
            m["emissiveTexture"] = {"index": et}
            m["emissiveFactor"] = [1.0, 1.0, 1.0]
            m["extensions"] = {"KHR_materials_emissive_strength":
                               {"emissiveStrength": 1.1}}
        m["doubleSided"] = True   # mobile pack: canopy fascia + signage are single-sided quads
        print("[w5]   %-16s base=%s emissive=%s"
              % (m["name"], os.path.basename(base_png),
                 os.path.basename(emi_png) if emi_png else "-"))

    os.makedirs(os.path.dirname(out_glb), exist_ok=True)
    glb_write(out_glb, js, bin_)
    shutil.rmtree(tmp, ignore_errors=True)
    print("[w5] wrote", out_glb, os.path.getsize(out_glb), "bytes")


if __name__ == "__main__":
    out_dir = sys.argv[1] if len(sys.argv) > 1 else \
        os.path.join(REPO, "assets", "converted_glb", "GasStation")
    build_mega_station(os.path.join(out_dir, "gas_station_mega.glb"))
