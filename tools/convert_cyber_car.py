#!/usr/bin/env python3
"""Assemble a Modular Cyber Racing Cars vehicle into one engine-ready GLB.

That pack ships each car as SEPARATE FBX files (a body + 4 wheel FBX, all at the
local origin, + modular bumpers/spoilers/etc.). The X3Native vehicle skin path
needs ONE GLB whose 4 wheels are named Wheel_FL/FR/RL/RR and sit at the wheel
stations, so this script:
  1. FBX2glTF the body + 4 wheels -> 4 raw GLBs (named nodes/materials).
  2. MERGE them into one glTF document (append buffers/views/accessors/meshes/
     materials with index remap), parenting each wheel under a NEW node named
     Wheel_FL/FR/RL/RR translated to the derived corner station (from the body
     bbox: just inside the body width, near each end, at wheel-radius height).
  3. Re-tune materials for the engine PBR + clearcoat stack (the same override
     table as convert_car_glb.py: the bare "Color" body material -> clearcoat
     paint; "Brake_light"/"Light_*" -> emissive; "Glass"/"Metal"/"Tire").
  4. Scale-normalize to ~1:1 (these import near 4 m already).

Usage:
  python tools/convert_cyber_car.py <pack_car_dir> <dst.glb> [paintR,paintG,paintB]
    pack_car_dir : .../Meshes/Car_NN  (contains Car_NN.fbx + Car_NN_Wheel_*.fbx)
"""
import sys, os, subprocess, copy
from pygltflib import GLTF2, Node

FBX2GLTF = r"C:\GameDev\tools\FBX2glTF.exe"
PAINT_DEFAULT = (0.45, 0.008, 0.018)

def log(*a): print("[cyber]", *a, flush=True)

# Material override table (substring match, lowercased; first wins). Mirrors
# convert_car_glb.py but tuned for this pack's bare names (Color/Metal/Glass/...).
def overrides(paint):
    return [
        ("light_reverse", dict(bc=(0.6,0.6,0.6,1.0), metal=0.0, rough=0.3)),
        ("brake_light",   dict(bc=(0.5,0.01,0.01,1.0), metal=0.0, rough=0.15,
                               emissive=(1.0,0.02,0.02,4.0))),
        ("brakelight",    dict(bc=(0.5,0.01,0.01,1.0), metal=0.0, rough=0.15,
                               emissive=(1.0,0.02,0.02,4.0))),
        ("headlight",     dict(bc=(0.9,0.92,1.0,1.0), metal=0.0, rough=0.10,
                               emissive=(1.0,0.98,0.92,5.0))),
        ("foglight",      dict(bc=(0.8,0.8,0.75,1.0), metal=0.0, rough=0.2,
                               emissive=(1.0,0.95,0.8,2.5))),
        ("glass",         dict(bc=(0.012,0.012,0.016,1.0), metal=1.0, rough=0.06)),
        ("tire",          dict(bc=(0.035,0.035,0.035,1.0), metal=0.0, rough=0.75)),
        ("tyre",          dict(bc=(0.035,0.035,0.035,1.0), metal=0.0, rough=0.75)),
        ("metal",         dict(bc=(0.82,0.83,0.86,1.0), metal=1.0, rough=0.18)),
        ("decal",         dict(bc=(0.05,0.05,0.06,1.0), metal=0.0, rough=0.5)),
        # the painted shell — this pack names it bare "Color"
        ("color",         dict(bc=(*paint,1.0), metal=0.80, rough=0.40,
                               clearcoat=(1.0,0.05))),
        ("body",          dict(bc=(*paint,1.0), metal=0.80, rough=0.40,
                               clearcoat=(1.0,0.05))),
    ]

def fbx_to_glb(src, tmpbase):
    r = subprocess.run([FBX2GLTF, "--binary", "--input", src, "--output", tmpbase],
                       capture_output=True, text=True)
    if r.returncode != 0:
        log("FBX2glTF FAILED:", src, r.stderr[-300:]); sys.exit(1)
    return tmpbase + ".glb"

def append_doc(dst: GLTF2, src: GLTF2, parent_node_name: str, translate):
    """Append all of `src`'s mesh data into `dst` under a new node (named
    parent_node_name) translated to `translate`. Returns nothing; mutates dst.

    GLB stores ALL data in ONE binary chunk, so we CONCATENATE src's blob onto
    dst's blob and shift src's bufferView byteOffsets — keeping buffer index 0
    as the single buffer. (Naively extending dst.buffers leaves bufferViews
    pointing past the real blob -> loader heap corruption.)"""
    nbv = len(dst.bufferViews); na = len(dst.accessors)
    nm  = len(dst.meshes);  nmat = len(dst.materials);   nimg = len(dst.images)
    ntex = len(dst.textures); nsamp = len(dst.samplers)

    # ---- merge the single binary blob (4-byte aligned) ----
    dblob = bytearray(dst.binary_blob() or b"")
    sblob = src.binary_blob() or b""
    pad = (-len(dblob)) % 4
    dblob += b"\x00" * pad
    base_off = len(dblob)
    dblob += sblob
    dst.set_binary_blob(bytes(dblob))
    if dst.buffers:
        dst.buffers[0].byteLength = len(dblob)

    for bv in copy.deepcopy(src.bufferViews):
        bv.buffer = 0
        bv.byteOffset = (bv.byteOffset or 0) + base_off
        dst.bufferViews.append(bv)
    for a in copy.deepcopy(src.accessors):
        if a.bufferView is not None: a.bufferView += nbv
        dst.accessors.append(a)
    for s in copy.deepcopy(src.samplers or []): dst.samplers.append(s)
    for im in copy.deepcopy(src.images or []):
        if im.bufferView is not None: im.bufferView += nbv
        dst.images.append(im)
    for t in copy.deepcopy(src.textures or []):
        if t.source is not None: t.source += nimg
        if t.sampler is not None: t.sampler += nsamp
        dst.textures.append(t)
    for mat in copy.deepcopy(src.materials or []):
        p = mat.pbrMetallicRoughness
        if p:
            if p.baseColorTexture and p.baseColorTexture.index is not None:
                p.baseColorTexture.index += ntex
            if p.metallicRoughnessTexture and p.metallicRoughnessTexture.index is not None:
                p.metallicRoughnessTexture.index += ntex
        dst.materials.append(mat)
    child_mesh_nodes = []
    for me in copy.deepcopy(src.meshes):
        for prim in me.primitives:
            for attr in ("POSITION","NORMAL","TANGENT","TEXCOORD_0","TEXCOORD_1","COLOR_0"):
                v = getattr(prim.attributes, attr, None)
                if v is not None: setattr(prim.attributes, attr, v + na)
            if prim.indices is not None: prim.indices += na
            if prim.material is not None: prim.material += nmat
        dst.meshes.append(me)
    # one node per appended mesh, all parented under the new transform node
    base_node = len(dst.nodes)
    child_idx = []
    for i in range(len(src.meshes)):
        n = Node(mesh=nm + i)
        dst.nodes.append(n); child_idx.append(len(dst.nodes)-1)
    parent = Node(name=parent_node_name, translation=list(translate), children=child_idx)
    dst.nodes.append(parent)
    parent_idx = len(dst.nodes)-1
    dst.scenes[0].nodes.append(parent_idx)

def main():
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(1)
    car_dir, dst = sys.argv[1], sys.argv[2]
    paint = PAINT_DEFAULT
    if len(sys.argv) > 3:
        paint = tuple(float(x) for x in sys.argv[3].split(","))[:3]

    import tempfile
    name = os.path.basename(os.path.normpath(car_dir))   # "Car_07"
    tmp = tempfile.gettempdir()
    body_fbx = os.path.join(car_dir, f"{name}.fbx")
    if not os.path.isfile(body_fbx):
        log("no body FBX:", body_fbx); sys.exit(1)

    # import convert_car_glb's helpers (world_bbox + the material tuner)
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "ccg", os.path.join(os.path.dirname(__file__), "convert_car_glb.py"))
    ccg = importlib.util.module_from_spec(spec); spec.loader.exec_module(ccg)

    # 1) body
    body_glb = fbx_to_glb(body_fbx, os.path.join(tmp, "cyb_body"))
    g = GLTF2().load(body_glb)
    if not g.scenes:
        log("body has no scene"); sys.exit(1)

    # body bbox -> wheel stations (just inside width, near the ends, ground level)
    mn, mx = ccg.world_bbox(g)
    cx = (mn[0]+mx[0])/2.0
    halfw = (mx[0]-mn[0])/2.0
    # wheel files
    wmap = {"FL": ("Wheel_FL", -1, +1),   # (-x left? we mirror below) (sign_x, sign_z)
            "FR": ("Wheel_FR", +1, +1),
            "BL": ("Wheel_RL", -1, -1),
            "BR": ("Wheel_RR", +1, -1)}
    wheel_x = halfw * 0.78
    front_z = mx[2] - (mx[2]-mn[2]) * 0.20
    rear_z  = mn[2] + (mx[2]-mn[2]) * 0.20
    wheel_y = 0.0   # wheels authored centered at origin; sit them at ground contact
    # estimate wheel radius from a wheel file later; place center at radius height
    for tag, (canon, sx, sz) in wmap.items():
        wf = os.path.join(car_dir, f"{name}_Wheel_{tag}.fbx")
        if not os.path.isfile(wf):
            log("missing wheel:", wf); sys.exit(1)
        wglb = fbx_to_glb(wf, os.path.join(tmp, f"cyb_w_{tag}"))
        wg = GLTF2().load(wglb)
        # wheel radius from its accessor (Y half-extent)
        acc = wg.accessors[wg.meshes[0].primitives[0].attributes.POSITION]
        r = abs(acc.max[1]) if acc.max else 0.45
        z = front_z if sz > 0 else rear_z
        x = cx + sx * wheel_x
        append_doc(g, wg, canon, (x, mn[1] + r, z))

    # 2) re-tune ALL materials with our table (reuse convert_car_glb logic inline)
    table = overrides(paint); cc = 0
    for m in g.materials:
        nm = (m.name or "").lower(); rule = None
        for key, rl in table:
            if key in nm: rule = rl; break
        if rule is None:
            continue
        p = m.pbrMetallicRoughness
        if p is None: continue
        p.baseColorFactor = list(rule["bc"]); p.metallicFactor = rule["metal"]
        p.roughnessFactor = rule["rough"]
        p.baseColorTexture = None; p.metallicRoughnessTexture = None
        if "clearcoat" in rule:
            ex = m.extras if isinstance(m.extras, dict) else {}
            ex["x3Clearcoat"] = {"intensity": rule["clearcoat"][0],
                                 "roughness": rule["clearcoat"][1]}
            m.extras = ex; cc += 1
        if "emissive" in rule:
            e = rule["emissive"]; m.emissiveFactor = [e[0],e[1],e[2]]
            exts = m.extensions if isinstance(m.extensions, dict) else {}
            exts["KHR_materials_emissive_strength"] = {"emissiveStrength": e[3]}
            m.extensions = exts
            if "KHR_materials_emissive_strength" not in (g.extensionsUsed or []):
                g.extensionsUsed = (g.extensionsUsed or []) + ["KHR_materials_emissive_strength"]
    log(f"clearcoat materials: {cc}")

    # 3) scale-normalize to ~1:1 (reuse convert_car_glb world_bbox + root rescale)
    try:
        import numpy as np
        mn2, mx2 = ccg.world_bbox(g); dims = mx2-mn2
        longest = float(max(dims[0], dims[2]))
        if longest > 0.01:
            scl = 4.3 / longest
            if abs(scl-1.0) > 0.05:
                children=set()
                for n in g.nodes:
                    for c in (n.children or []): children.add(c)
                roots=[i for i in range(len(g.nodes)) if i not in children]
                for ri in roots:
                    n=g.nodes[ri]
                    s=n.scale or [1,1,1]; n.scale=[s[0]*scl,s[1]*scl,s[2]*scl]
                    if n.translation: n.translation=[t*scl for t in n.translation]
                log(f"scale-normalized longest {longest:.2f} -> 4.30 m (x{scl:.3f})")
    except Exception as e:
        log("scale skip:", e)

    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    g.save(dst)
    log("WROTE", dst, f"({os.path.getsize(dst)} bytes)")

if __name__ == "__main__":
    main()
