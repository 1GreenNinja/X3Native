#!/usr/bin/env python3
"""Finish a vehicle GLB for the X3Native engine (the HERO-CAR pipeline).

The car packs (RCC v4, Racing Game Creator) ship plain FBX with flat material
colors and NO Unity .mat/.meta files, so convert_unity_pack.py's material
resolution can't apply. Instead:
  1. FBX2glTF (handles ASCII FBX) -> GLB with NAMED nodes + NAMED material slots.
  2. THIS script re-tunes the materials for the engine's PBR + CLEARCOAT stack:
       * a per-material override table (paint / chrome / glass / tyre / lights),
       * material.extras["x3Clearcoat"] = {"intensity":F,"roughness":F} on the
         PAINT materials (the x3Detail pattern: ModelLoader parses the extras,
         the renderer packs it into a spare ObjectData lane),
       * KHR_materials_emissive_strength on the light materials so head/brake
         lights glow + feed the HDR bloom chain at night,
       * drops Collider nodes/meshes (physics proxies must never render).

Usage:
  python tools/convert_car_glb.py <src.fbx|src.glb> <dst.glb> [paintR,paintG,paintB] [--len METRES]
    src   : an FBX (FBX2glTF is invoked first) or an already-converted GLB
    dst   : the engine-ready GLB
    paint : optional linear paint color override (default: deep race red)
    --len : normalize the car so its LONGEST horizontal axis ~= METRES (default
            4.3 m, a real ~1:1 car). Fixes packs that import 1.5x oversized (E30)
            or in cm. Applied as a uniform scale on the root node(s).

Material override table keys match by SUBSTRING of the material name
(lowercased), so "CTR_Body" matches "body", "E30_Chrome" matches "chrome", etc.
"""
import sys, os, subprocess, tempfile

from pygltflib import GLTF2

FBX2GLTF = r"C:\GameDev\tools\FBX2glTF.exe"

def log(*a): print("[car]", *a, flush=True)

# ---------------------------------------------------------------------------
# Material override table. First substring match wins (ordered!). Fields:
#   bc        : linear baseColorFactor rgba
#   metal/rough: scalar PBR factors (the loader synthesizes a 1x1 MR map)
#   clearcoat : (intensity, roughness) -> extras x3Clearcoat (paint only)
#   emissive  : (r, g, b, strength) -> emissiveFactor + KHR emissive_strength
# ---------------------------------------------------------------------------
PAINT_DEFAULT = (0.45, 0.008, 0.018)     # deep race red (linear)

def overrides(paint):
    return [
        # physics proxy — handled by the node strip, belt+braces black it out
        ("collider",        dict(strip=True)),
        # ---- lights (emissive; ordered BEFORE body so "light_..." never
        #      falls through to a paint rule) ----
        ("light_headlight", dict(bc=(0.9, 0.92, 1.0, 1.0), metal=0.0, rough=0.10,
                                  emissive=(1.0, 0.98, 0.92, 5.0))),
        ("light_brake",     dict(bc=(0.5, 0.01, 0.01, 1.0), metal=0.0, rough=0.15,
                                  emissive=(1.0, 0.02, 0.02, 4.0))),
        ("light_tail",      dict(bc=(0.5, 0.01, 0.01, 1.0), metal=0.0, rough=0.15,
                                  emissive=(1.0, 0.02, 0.02, 3.0))),
        ("light_indicator", dict(bc=(0.45, 0.12, 0.0, 1.0), metal=0.0, rough=0.3)),
        ("light_reverse",   dict(bc=(0.6, 0.6, 0.6, 1.0), metal=0.0, rough=0.3)),
        ("lights_fog",      dict(bc=(0.8, 0.8, 0.75, 1.0), metal=0.0, rough=0.2)),
        # ---- emissive light rules (also match the RCC "Lights_*" naming, not
        #      just "light_*"); ordered BEFORE paint so they never fall through ----
        ("lights_headlight",dict(bc=(0.9, 0.92, 1.0, 1.0), metal=0.0, rough=0.10,
                                  emissive=(1.0, 0.98, 0.92, 5.0))),
        ("lowbeam",         dict(bc=(0.9, 0.92, 1.0, 1.0), metal=0.0, rough=0.10,
                                  emissive=(1.0, 0.98, 0.92, 4.0))),
        ("highbeam",        dict(bc=(0.9, 0.92, 1.0, 1.0), metal=0.0, rough=0.10,
                                  emissive=(1.0, 0.98, 0.92, 5.0))),
        ("lights_brake",    dict(bc=(0.5, 0.01, 0.01, 1.0), metal=0.0, rough=0.15,
                                  emissive=(1.0, 0.02, 0.02, 4.0))),
        ("brakelights",     dict(bc=(0.5, 0.01, 0.01, 1.0), metal=0.0, rough=0.15,
                                  emissive=(1.0, 0.02, 0.02, 4.0))),
        # ---- the PAINT (clearcoat) group ----
        ("body",            dict(bc=(*paint, 1.0), metal=0.80, rough=0.40,
                                  clearcoat=(1.0, 0.05))),
        ("hood",            dict(bc=(*paint, 1.0), metal=0.80, rough=0.40,
                                  clearcoat=(1.0, 0.05))),
        # F1 / open-wheel + some packs name the painted shell "chassis"/"color_1".
        ("chassis",         dict(bc=(*paint, 1.0), metal=0.70, rough=0.42,
                                  clearcoat=(1.0, 0.06))),
        ("color_1",         dict(bc=(*paint, 1.0), metal=0.70, rough=0.42,
                                  clearcoat=(1.0, 0.06))),
        ("carbonfiber",     dict(bc=(0.02, 0.02, 0.025, 1.0), metal=0.3, rough=0.35)),
        ("paint",           dict(bc=(*paint, 1.0), metal=0.80, rough=0.40,
                                  clearcoat=(1.0, 0.05))),
        # ---- glass: smoked mirror (opaque near-black metal => env reflections;
        #      the shell has no interior, BLEND would show through to backfaces) ----
        ("windows",         dict(bc=(0.012, 0.012, 0.016, 1.0), metal=1.0, rough=0.06)),
        ("window",          dict(bc=(0.012, 0.012, 0.016, 1.0), metal=1.0, rough=0.06)),
        ("mirror",          dict(bc=(0.9, 0.9, 0.92, 1.0), metal=1.0, rough=0.03)),
        # ---- trim rubbers BEFORE "rim" (else "RubberTrim" matches the rim rule) ----
        ("rubber",          dict(bc=(0.03, 0.03, 0.03, 1.0), metal=0.0, rough=0.65)),
        # ---- brightwork / running gear ----
        ("chrome",          dict(bc=(0.95, 0.95, 0.97, 1.0), metal=1.0, rough=0.06)),
        ("rim",             dict(bc=(0.85, 0.86, 0.88, 1.0), metal=1.0, rough=0.16)),
        ("exhaust",         dict(bc=(0.8, 0.8, 0.82, 1.0), metal=1.0, rough=0.28)),
        ("tyre",            dict(bc=(0.035, 0.035, 0.035, 1.0), metal=0.0, rough=0.75)),
        ("tire",            dict(bc=(0.035, 0.035, 0.035, 1.0), metal=0.0, rough=0.75)),
        ("wheel",           dict(bc=(0.035, 0.035, 0.035, 1.0), metal=0.0, rough=0.75)),
        # ---- trim / black plastics / underbody ----
        ("rubber",          dict(bc=(0.03, 0.03, 0.03, 1.0), metal=0.0, rough=0.65)),
        ("matte",           dict(bc=(0.04, 0.04, 0.045, 1.0), metal=0.0, rough=0.6)),
        ("black",           dict(bc=(0.05, 0.05, 0.055, 1.0), metal=0.0, rough=0.5)),
        ("plastic",         dict(bc=(0.05, 0.05, 0.055, 1.0), metal=0.2, rough=0.45)),
        ("buttom",          dict(bc=(0.03, 0.03, 0.03, 1.0), metal=0.0, rough=0.8)),
        ("bottom",          dict(bc=(0.03, 0.03, 0.03, 1.0), metal=0.0, rough=0.8)),
        ("frame",           dict(bc=(0.06, 0.06, 0.065, 1.0), metal=0.4, rough=0.35)),
        ("grill",           dict(bc=(0.05, 0.05, 0.055, 1.0), metal=0.6, rough=0.3)),
        ("handle",          dict(bc=(0.9, 0.9, 0.92, 1.0), metal=1.0, rough=0.12)),
        ("plate",           dict(bc=(0.7, 0.7, 0.72, 1.0), metal=0.0, rough=0.4)),
    ]

def world_bbox(g):
    """World-space AABB over all mesh primitives, composing node TRS down the
    scene graph (accessor min/max are local; we must apply node transforms)."""
    import numpy as np
    # child -> parent map (none = root)
    parent = {}
    for i, n in enumerate(g.nodes):
        for c in (n.children or []):
            parent[c] = i
    def local_mat(n):
        import numpy as np
        if n.matrix:
            return np.array(n.matrix, dtype=float).reshape(4, 4).T
        T = np.eye(4)
        if n.translation: T[:3, 3] = n.translation
        S = np.diag([*(n.scale or [1, 1, 1]), 1.0])
        R = np.eye(4)
        if n.rotation:
            x, y, z, w = n.rotation
            R[:3, :3] = np.array([
                [1-2*(y*y+z*z),   2*(x*y-z*w),   2*(x*z+y*w)],
                [2*(x*y+z*w),   1-2*(x*x+z*z),   2*(y*z-x*w)],
                [2*(x*z-y*w),     2*(y*z+x*w), 1-2*(x*x+y*y)]])
        return T @ R @ S
    def world_mat(i):
        m = local_mat(g.nodes[i])
        p = parent.get(i)
        while p is not None:
            m = local_mat(g.nodes[p]) @ m
            p = parent.get(p)
        return m
    mn = np.array([1e30] * 3); mx = np.array([-1e30] * 3)
    for i, n in enumerate(g.nodes):
        if n.mesh is None:
            continue
        wm = world_mat(i)
        acc0 = g.meshes[n.mesh].primitives[0].attributes.POSITION
        for prim in g.meshes[n.mesh].primitives:
            a = g.accessors[prim.attributes.POSITION]
            if not (a.min and a.max):
                continue
            for cx in (a.min[0], a.max[0]):
                for cy in (a.min[1], a.max[1]):
                    for cz in (a.min[2], a.max[2]):
                        p = wm @ np.array([cx, cy, cz, 1.0])
                        mn = np.minimum(mn, p[:3]); mx = np.maximum(mx, p[:3])
    return mn, mx

def main():
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(1)
    args = [a for a in sys.argv[1:]]
    target_len = 4.3
    if "--len" in args:
        k = args.index("--len"); target_len = float(args[k + 1]); del args[k:k + 2]
    src, dst = args[0], args[1]
    paint = PAINT_DEFAULT
    if len(args) > 2:
        paint = tuple(float(x) for x in args[2].split(","))[:3]

    # 1) FBX -> raw GLB if needed.
    raw = src
    if src.lower().endswith((".fbx",)):
        tmp = os.path.join(tempfile.gettempdir(), "x3car_raw")
        r = subprocess.run([FBX2GLTF, "--binary", "--input", src, "--output", tmp],
                           capture_output=True, text=True)
        if r.returncode != 0:
            log("FBX2glTF FAILED:", r.stdout[-400:], r.stderr[-400:]); sys.exit(1)
        raw = tmp + ".glb"
    g = GLTF2().load(raw)

    table = overrides(paint)
    ccCount = 0
    for m in g.materials:
        name = (m.name or "").lower()
        rule = None
        for key, r in table:
            if key in name:
                rule = r; break
        if rule is None:
            log("no rule for material:", m.name, "(left as authored)")
            continue
        if rule.get("strip"):
            # belt+braces: invisible-black; the node strip below removes the draw
            if m.pbrMetallicRoughness:
                m.pbrMetallicRoughness.baseColorFactor = [0, 0, 0, 1]
            continue
        p = m.pbrMetallicRoughness
        if p is None:
            continue
        p.baseColorFactor = list(rule["bc"])
        p.metallicFactor  = rule["metal"]
        p.roughnessFactor = rule["rough"]
        p.baseColorTexture = None          # flat-color materials: drop any stray atlas
        p.metallicRoughnessTexture = None
        if "clearcoat" in rule:
            cc = rule["clearcoat"]
            ex = m.extras if isinstance(m.extras, dict) else {}
            ex["x3Clearcoat"] = {"intensity": cc[0], "roughness": cc[1]}
            m.extras = ex
            ccCount += 1
        if "emissive" in rule:
            e = rule["emissive"]
            m.emissiveFactor = [e[0], e[1], e[2]]
            exts = m.extensions if isinstance(m.extensions, dict) else {}
            exts["KHR_materials_emissive_strength"] = {"emissiveStrength": e[3]}
            m.extensions = exts
            if "KHR_materials_emissive_strength" not in (g.extensionsUsed or []):
                g.extensionsUsed = (g.extensionsUsed or []) + ["KHR_materials_emissive_strength"]
        log("tuned", m.name, "->", rule)

    # 2) Strip Collider nodes (don't render the physics proxy). The node keeps
    #    its slot (indices stay valid) but loses its mesh reference.
    stripped = 0
    for n in g.nodes:
        if n.mesh is not None and "collider" in (n.name or "").lower():
            n.mesh = None; stripped += 1
    log(f"clearcoat materials: {ccCount}, collider nodes stripped: {stripped}")

    # 3) SCALE NORMALIZE to ~1:1. Measure the world bbox; if the longest
    #    horizontal axis is off the target, apply a uniform scale on the ROOT
    #    nodes (scene roots = nodes with no parent). Also drops the car onto y=0.
    try:
        import numpy as np
        mn, mx = world_bbox(g)
        dims = mx - mn
        longest = float(max(dims[0], dims[2]))   # horizontal length (X or Z)
        if longest > 0.01:
            scl = target_len / longest
            # only correct meaningful deviations (>5%)
            if abs(scl - 1.0) > 0.05:
                children = set()
                for n in g.nodes:
                    for c in (n.children or []):
                        children.add(c)
                roots = [i for i in range(len(g.nodes)) if i not in children]
                for ri in roots:
                    n = g.nodes[ri]
                    if n.matrix:
                        m = np.array(n.matrix, dtype=float).reshape(4, 4).T
                        m[:3, :] *= scl
                        n.matrix = list(m.T.flatten())
                    else:
                        s = n.scale or [1, 1, 1]
                        n.scale = [s[0] * scl, s[1] * scl, s[2] * scl]
                        if n.translation:
                            n.translation = [t * scl for t in n.translation]
                log(f"scale-normalized: longest {longest:.2f} m -> {target_len:.2f} m (x{scl:.3f})")
            else:
                log(f"scale OK: longest {longest:.2f} m (no change)")
    except Exception as e:
        log("scale-normalize skipped:", e)

    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    g.save(dst)
    log("WROTE", dst, f"({os.path.getsize(dst)} bytes)")

if __name__ == "__main__":
    main()
