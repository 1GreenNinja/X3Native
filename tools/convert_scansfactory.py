#!/usr/bin/env python3
"""Convert ScansFactory-vendor Unity packs (Warehouse - Abandoned Factory
District, Urban Abandoned District) -> TEXTURED GLB for X3Native.

WHY A SECOND CONVERTER AND NOT tools/convert_unity_pack.py:
  That one resolves textures the Unity way — `.mat` slot -> texture GUID ->
  `<file>.png.meta` GUID -> PNG. These packs, as they sit on the share, have
  had every `.mat` and every `.meta` stripped out (the `_Layouts/` split moved
  the YAML aside and replaced the metas with one `guid_map.json`). So the GUID
  chain has nothing to walk and convert_unity_pack.py would emit GREY meshes —
  which is exactly the untextured stand-in NO_SLOP rule 3 forbids shipping.
  FBX2glTF's own output confirms the defect: it writes 1x1 white PNG stubs for
  every image (`data:image/png;base64,iVBOR...` 68 bytes) while naming the
  material slots correctly.

WHAT THIS DOES INSTEAD — the vendor's naming IS the map, and it is exact:
      material  mi_Chimney_01_01
      textures  t_Chimney_01_01_bc.png   base colour
                t_Chimney_01_01_n.png    normal
                t_Chimney_01_01_m.png    metallic (single channel)
                t_Chimney_01_01_r.png    roughness (single channel)
                t_Chimney_01_01_rma.png  packed
                t_Chimney_01_01_a.png    alpha (masked foliage/fence mesh)
                t_Chimney_01_01_e.png    emissive
  So: FBX2glTF for geometry + named slots, then inject by NAME.

CHANNEL LAW (docs/ENGINE_GOTCHAS.md 3.6, pixel-verified twice — do not guess):
  glTF/engine metallicRoughness is **G = roughness, B = metallic** (mesh.frag
  samples metallic from mr.b), R = occlusion. This vendor's `_rma` packs are
  literally **R = rough, G = metal, B = AO**, so the remap is
  ORM.r = rma.b, ORM.g = rma.r, ORM.b = rma.g. Separate `_r`/`_m` files go
  straight into G and B. Never inject a packed mask raw.

LOD/HULL PRUNING: these FBX ship `<mesh>_LOD0`, `_LOD1` and `_ConvexHulls` as
  sibling nodes. The engine has no LOD-node convention for env props, so all
  three would draw on top of each other (triple cost, z-fighting shells). Only
  LOD0 survives; `--keep-lods` disables the prune.

Usage:
  python tools/convert_scansfactory.py <pack_common_dir> <out_dir> [mesh ...]
    pack_common_dir : the folder holding Mesh/ (or Meshes/) + Textures/
    out_dir         : where the .glb are written
    mesh...         : FBX stems (no extension). Omit for "every FBX" (slow).
"""
import sys, os, re, json, glob, base64, struct, subprocess, tempfile, io

try:
    from PIL import Image
except ImportError:
    print("need Pillow: pip install Pillow"); sys.exit(2)

FBX2GLTF = next((p for p in (r"D:\GameDev\tools\FBX2glTF.exe",
                             r"C:\GameDev\tools\FBX2glTF.exe") if os.path.exists(p)), None)
MAX_TEX = 1024   # atlas cap: images embed PER GLB here, so 4K balloons the file

def log(*a): print("[scansf]", *a, flush=True)


def texture_index(tex_dir):
    """lowercase 'chimney_01_01' -> {suffix: path}."""
    idx = {}
    for p in glob.glob(os.path.join(tex_dir, "*")):
        b = os.path.basename(p)
        m = re.match(r"^t_(.+)_([a-z0-9]+)\.(png|tga|tif|tiff|jpg)$", b, re.I)
        if not m:
            continue
        key, suf = m.group(1).lower(), m.group(2).lower()
        idx.setdefault(key, {})[suf] = p
    return idx


def _load(path):
    im = Image.open(path)
    if im.mode not in ("RGB", "RGBA", "L"):
        im = im.convert("RGB")
    w, h = im.size
    m = max(w, h)
    if m > MAX_TEX:
        s = MAX_TEX / float(m)
        im = im.resize((max(1, int(w * s)), max(1, int(h * s))), Image.LANCZOS)
    return im


def _png_datauri(im):
    buf = io.BytesIO()
    im.save(buf, format="PNG", optimize=False)
    return "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode("ascii")


def build_orm(files):
    """glTF ORM: R=occlusion, G=roughness, B=metallic. Returns a PIL image or None."""
    rma = files.get("rma")
    if rma:
        im = _load(rma).convert("RGB")
        r, g, b = im.split()
        # vendor packing is R=rough G=metal B=AO (ENGINE_GOTCHAS 3.6)
        return Image.merge("RGB", (b, r, g))
    rp, mp = files.get("r"), files.get("m")
    if not rp and not mp:
        return None
    size = None
    rough = metal = None
    if rp:
        rough = _load(rp).convert("L"); size = rough.size
    if mp:
        metal = _load(mp).convert("L"); size = size or metal.size
    if rough is None:
        rough = Image.new("L", size, 190)     # 0.75 rough: a sane dielectric default
    if metal is None:
        metal = Image.new("L", size, 0)
    if rough.size != size: rough = rough.resize(size, Image.LANCZOS)
    if metal.size != size: metal = metal.resize(size, Image.LANCZOS)
    ao = Image.new("L", size, 255)
    return Image.merge("RGB", (ao, rough, metal))


def convert(fbx, tex_idx, out_glb, keep_lods=False):
    if not FBX2GLTF:
        log("FBX2glTF.exe not found"); return False
    tmp = tempfile.mktemp(suffix=".glb")
    r = subprocess.run([FBX2GLTF, "-b", "-i", fbx, "-o", tmp],
                       capture_output=True, text=True)
    # FBX2glTF exits nonzero on the stub-texture warning; trust the file.
    if not os.path.exists(tmp):
        log("FAIL fbx2gltf", os.path.basename(fbx), r.stderr[-300:]); return False

    raw = open(tmp, "rb").read()
    os.remove(tmp)
    jlen = struct.unpack("<I", raw[12:16])[0]
    j = json.loads(raw[20:20 + jlen])
    bin_off = 20 + jlen + 8
    binchunk = raw[bin_off:]

    # ---- inject textures by material name ---------------------------------
    hits = misses = 0
    for mat in j.get("materials", []):
        name = mat.get("name", "")
        m = re.match(r"^(?:mi|m|mat)_(.+)$", name, re.I)
        key = (m.group(1) if m else name).lower()
        files = tex_idx.get(key)
        if not files:
            # some slots name a shared atlas without the numeric tail
            base = re.sub(r"_\d+_\d+$", "", key)
            cand = [k for k in tex_idx if k.startswith(base)]
            files = tex_idx[sorted(cand)[0]] if cand else None
        if not files:
            misses += 1
            continue

        def add_image(im):
            j.setdefault("images", []).append({"uri": _png_datauri(im)})
            j.setdefault("samplers", [{"wrapS": 10497, "wrapT": 10497}])
            j.setdefault("textures", []).append(
                {"sampler": 0, "source": len(j["images"]) - 1})
            return len(j["textures"]) - 1

        pbr = mat.setdefault("pbrMetallicRoughness", {})
        if "bc" in files:
            im = _load(files["bc"])
            if "a" in files:                      # masked cutout (fences, grates)
                a = _load(files["a"]).convert("L")
                im = im.convert("RGB"); a = a.resize(im.size, Image.LANCZOS)
                im = Image.merge("RGBA", (*im.split(), a))
                mat["alphaMode"] = "MASK"; mat["alphaCutoff"] = 0.5
            pbr["baseColorTexture"] = {"index": add_image(im), "texCoord": 0}
            pbr["baseColorFactor"] = [1.0, 1.0, 1.0, 1.0]
        if "n" in files:
            mat["normalTexture"] = {"index": add_image(_load(files["n"]).convert("RGB")),
                                    "texCoord": 0}
        orm = build_orm(files)
        if orm is not None:
            pbr["metallicRoughnessTexture"] = {"index": add_image(orm), "texCoord": 0}
            pbr["metallicFactor"] = 1.0
            pbr["roughnessFactor"] = 1.0
        else:
            # NO finish map: clamp metalness. X3_WORLD_RULES rule 5 — an
            # untextured full-metal has no diffuse lobe and renders BLACK.
            pbr["metallicFactor"] = min(0.35, pbr.get("metallicFactor", 0.0))
            pbr["roughnessFactor"] = max(0.45, pbr.get("roughnessFactor", 0.5))
        if "e" in files:
            mat["emissiveTexture"] = {"index": add_image(_load(files["e"]).convert("RGB")),
                                      "texCoord": 0}
            mat["emissiveFactor"] = [1.0, 1.0, 1.0]
        hits += 1

    # ---- prune LOD1 / collision-hull sibling nodes ------------------------
    dropped = 0
    if not keep_lods:
        bad = set()
        for i, mesh in enumerate(j.get("meshes", [])):
            nm = mesh.get("name", "")
            if re.search(r"(_LOD[1-9]\d*|_ConvexHulls?|_UCX.*)$", nm, re.I):
                bad.add(i)
        if bad:
            drop_node = [nd.get("mesh") in bad and not nd.get("children")
                         for nd in j["nodes"]]
            def keep(nodes):
                nonlocal dropped
                out = []
                for ni in nodes:
                    if drop_node[ni]:
                        dropped += 1
                        continue
                    nd = j["nodes"][ni]
                    if "children" in nd:
                        nd["children"] = keep(nd["children"])
                    out.append(ni)
                return out
            for sc in j.get("scenes", []):
                sc["nodes"] = keep(sc.get("nodes", []))

    jb = json.dumps(j, separators=(",", ":")).encode("utf-8")
    jb += b" " * ((4 - len(jb) % 4) % 4)
    bb = binchunk + b"\0" * ((4 - len(binchunk) % 4) % 4)
    total = 12 + 8 + len(jb) + 8 + len(bb)
    os.makedirs(os.path.dirname(out_glb), exist_ok=True)
    with open(out_glb, "wb") as f:
        f.write(b"glTF" + struct.pack("<II", 2, total))
        f.write(struct.pack("<I", len(jb)) + b"JSON" + jb)
        f.write(struct.pack("<I", len(bb)) + b"BIN\0" + bb)
    log("%-40s %d mat textured, %d unmatched, %d LOD/hull nodes dropped, %.1f MB"
        % (os.path.basename(out_glb), hits, misses, dropped, total / 1048576.0))
    return hits > 0


def main():
    if len(sys.argv) < 3:
        print(__doc__); return 2
    common, out_dir = sys.argv[1], sys.argv[2]
    stems = sys.argv[3:]
    mesh_dir = next((os.path.join(common, d) for d in ("Mesh", "Meshes")
                     if os.path.isdir(os.path.join(common, d))), None)
    tex_dir = os.path.join(common, "Textures")
    if not mesh_dir or not os.path.isdir(tex_dir):
        log("no Mesh/ + Textures/ under", common); return 2
    tex_idx = texture_index(tex_dir)
    log("texture index:", len(tex_idx), "asset keys")

    if stems:
        fbxs = []
        for s in stems:
            hit = glob.glob(os.path.join(mesh_dir, "**", s + ".[fF][bB][xX]"), recursive=True)
            if hit: fbxs.append(hit[0])
            else:   log("MISSING fbx:", s)
    else:
        fbxs = glob.glob(os.path.join(mesh_dir, "**", "*.[fF][bB][xX]"), recursive=True)

    ok = 0
    for f in fbxs:
        stem = os.path.splitext(os.path.basename(f))[0]
        if convert(f, tex_idx, os.path.join(out_dir, stem + ".glb")):
            ok += 1
    log("converted %d/%d" % (ok, len(fbxs)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
