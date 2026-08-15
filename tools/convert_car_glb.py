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
  python tools/convert_car_glb.py <src.fbx|src.glb> <dst.glb> [paintR,paintG,paintB]
    src   : an FBX (FBX2glTF is invoked first) or an already-converted GLB
    dst   : the engine-ready GLB
    paint : optional linear paint color override (default: deep race red)

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
        # ---- the PAINT (clearcoat) group ----
        ("body",            dict(bc=(*paint, 1.0), metal=0.80, rough=0.40,
                                  clearcoat=(1.0, 0.05))),
        ("hood",            dict(bc=(*paint, 1.0), metal=0.80, rough=0.40,
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

def main():
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(1)
    src, dst = sys.argv[1], sys.argv[2]
    paint = PAINT_DEFAULT
    if len(sys.argv) > 3:
        paint = tuple(float(x) for x in sys.argv[3].split(","))[:3]

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

    # 2) Strip Collider nodes (don't render the physics proxy).
    #
    # THE "MINI CAR" BUG (found 2026-08-14). Detaching the node is NOT enough:
    # clearing n.mesh leaves the mesh sitting in g.meshes referenced by NOBODY,
    # and an ORPHANED mesh is exactly what a loader's "don't silently drop it"
    # safety net will emit — at IDENTITY. Every real car part hangs off a node
    # scaled 2.54, so CTR.glb's orphaned 60,349-triangle 'Collider' rendered at
    # 1/2.54 = 39% size at the origin: a whole second, smaller car under the real
    # one, plus 60k triangles of waste every frame.
    #
    # So detach the node AND remove the mesh, remapping every remaining mesh
    # index so nothing else shifts under us.
    stripped = 0
    doomed = set()
    for n in g.nodes:
        if n.mesh is not None and "collider" in (n.name or "").lower():
            doomed.add(n.mesh); n.mesh = None; stripped += 1
    # Belt and braces: any mesh whose OWN name marks it a proxy goes too, even if
    # no node ever pointed at it (already-orphaned input).
    for i, msh in enumerate(g.meshes or []):
        if "collider" in (msh.name or "").lower() or "collision" in (msh.name or "").lower():
            doomed.add(i)
    removed = 0
    if doomed:
        keep = [i for i in range(len(g.meshes)) if i not in doomed]
        remap = {old: new for new, old in enumerate(keep)}
        g.meshes = [g.meshes[i] for i in keep]
        for n in g.nodes:
            if n.mesh is not None:
                n.mesh = remap.get(n.mesh)      # None if it pointed at a doomed mesh
        removed = len(doomed)
    # Assert the invariant this function exists to guarantee: no mesh may be left
    # unreferenced, or the loader will draw it.
    used = {n.mesh for n in g.nodes if n.mesh is not None}
    orphans = [i for i in range(len(g.meshes or [])) if i not in used]
    if orphans:
        names = ", ".join(repr((g.meshes[i].name or "")) for i in orphans)
        raise SystemExit(f"ERROR: {len(orphans)} mesh(es) left unreferenced and would "
                         f"render at identity scale: {names}")
    log(f"clearcoat materials: {ccCount}, collider nodes stripped: {stripped}, "
        f"proxy meshes removed: {removed}, orphans remaining: 0")

    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    g.save(dst)
    log("WROTE", dst, f"({os.path.getsize(dst)} bytes)")

if __name__ == "__main__":
    main()
