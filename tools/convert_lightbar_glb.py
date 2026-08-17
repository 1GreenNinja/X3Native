# Headless Blender: the RCC POLICE LIGHT BAR, FBX -> engine-ready GLB.
#
# WHY A DEDICATED SCRIPT (and not tools/convert_fbx_glb.py verbatim): the raw
# FBX carries three defects that would each ship a visible bug, and every one
# was MEASURED out of the first conversion's dump rather than guessed at:
#
#   1) A STRAY 55-METRE OBJECT. The FBX holds two meshes: `Police_Siren`
#      (6057 verts, object scale 0.0254 = the inch->metre factor) and `Glass`
#      (80 verts, scale 1.0 — the inch numbers left RAW). Unscaled, `Glass`
#      measures 56 x 10 x 4 in BLENDER UNITS, i.e. a 56-metre slab. Exported
#      as-is it would blow the model bbox to 56 m, and traffic.cpp's aspect
#      gate would (correctly) DROP the whole light bar with a warning. Only
#      `Police_Siren` survives here.
#   2) AUTHORED AT A CAR'S ROOF, NOT AT ITS OWN ORIGIN. The mesh sits 1.36-1.61 m
#      off origin on the depth axis — it was modelled in place on some RCC
#      body. X3_WORLD_RULES rule 4 wants the origin at the CONTACT SURFACE, so
#      this re-centres X/Z on the measured bbox and drops the BASE to y=0:
#      mounting the bar is then "put the origin on the roof", with no magic
#      offset at the call site.
#   3) NO WAY TO TELL THE RED LENS FROM THE BLUE ONE AT DRAW TIME. The bar is
#      ONE mesh with ten material slots, so the engine's makeDrawables() yields
#      one drawable per material with no material NAME attached. The flash
#      needs red and blue driven independently. Fix: bake the identity into a
#      field that DOES survive into ModelDrawable — emissiveFactor. Red lens
#      gets (1,0,0), blue gets (0,0,1), everything else (0,0,0); traffic.cpp
#      classifies on that and multiplies by the live flash intensity. No name
#      lookup, no index guessing, no drift.
#
# Materials are otherwise flat factors (the RCC packs ship no .mat data —
# tools/convert_car_glb.py's header documents the same thing for the cars).
# That is correct for this object rather than a NO_SLOP rule 3 stand-in: a
# light bar IS black plastic and two coloured lenses. The lenses are set
# near-black so that UNLIT they read as dark lenses (rule 5's "texture-gated
# emissive over near-black", with the primitive doing the gating that a
# texture would) and only bloom when the flash drives them.
#
# Store-launcher protocol (ENGINE_GOTCHAS 5.1): the MS-Store Blender launcher
# DETACHES with no stdout, so this writes its own .log and .done markers.
#
# Usage (via the launcher, which ignores argv after --python):
#   blender-launcher.exe --background --python tools/convert_lightbar_glb.py
import bpy, os, traceback

SRC = (r"\\p13700\G\Assets\Realistic Car Controller\RealisticCarControllerV4"
       r"\Models\Siren\Model_Police_Siren.FBX")
OUT = (r"D:\GameDev\X3Native\.claude\worktrees\agent-a724124606ae12a70"
       r"\assets_staging\traffic2\LightBar.glb")
KEEP = "Police_Siren"

# Material tuning, by name substring (first match wins) — the convert_car_glb
# pattern. bc = linear baseColorFactor rgb; em = emissiveFactor (the CLASSIFIER,
# see note 3 above); metal/rough = scalar PBR factors.
MATS = [
    ("siren_red",   dict(bc=(0.035, 0.004, 0.004), em=(1.0, 0.0, 0.0), metal=0.0, rough=0.22)),
    ("siren_blue",  dict(bc=(0.004, 0.006, 0.040), em=(0.0, 0.0, 1.0), metal=0.0, rough=0.22)),
    ("siren_glass", dict(bc=(0.020, 0.021, 0.024), em=(0, 0, 0),       metal=0.0, rough=0.15)),
    ("siren_base",  dict(bc=(0.030, 0.030, 0.033), em=(0, 0, 0),       metal=0.0, rough=0.45)),
    ("",            dict(bc=(0.035, 0.035, 0.038), em=(0, 0, 0),       metal=0.0, rough=0.45)),
]

log, status = [], "FAIL"
try:
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=SRC)

    # ---- 1) keep ONE object -------------------------------------------------
    for o in list(bpy.data.objects):
        if o.name != KEEP:
            log.append(f"DROP object {o.name!r} (scale {tuple(round(s,4) for s in o.scale)})")
            bpy.data.objects.remove(o, do_unlink=True)
    obj = bpy.data.objects.get(KEEP)
    if obj is None:
        raise RuntimeError(f"{KEEP!r} not in the FBX")

    # ---- 2) bake the inch->metre scale into the mesh ------------------------
    bpy.ops.object.select_all(action='DESELECT')
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

    # ---- 3) re-origin: centred in X/Z, BASE on y=0 (Blender is Z-up here) ---
    vs = [obj.matrix_world @ v.co for v in obj.data.vertices]
    mn = [min(v[i] for v in vs) for i in range(3)]
    mx = [max(v[i] for v in vs) for i in range(3)]
    log.append("pre-centre bbox (blender Z-up, m): " +
               " ".join(f"{a:.4f}..{b:.4f}" for a, b in zip(mn, mx)))
    off = (-(mn[0] + mx[0]) * 0.5, -(mn[1] + mx[1]) * 0.5, -mn[2])
    for v in obj.data.vertices:
        v.co[0] += off[0]; v.co[1] += off[1]; v.co[2] += off[2]
    obj.location = (0.0, 0.0, 0.0)
    log.append(f"re-origin offset {tuple(round(o,4) for o in off)}")
    log.append("post-centre extents (m): "
               f"X {mx[0]-mn[0]:.4f}  Y {mx[1]-mn[1]:.4f}  Z {mx[2]-mn[2]:.4f}")

    # ---- 4) materials -------------------------------------------------------
    for m in obj.data.materials:
        if m is None:
            continue
        low = m.name.lower()
        spec = next(s for k, s in MATS if k in low)
        m.use_nodes = True
        bsdf = next((n for n in m.node_tree.nodes if n.type == 'BSDF_PRINCIPLED'), None)
        if bsdf is None:
            bsdf = m.node_tree.nodes.new('ShaderNodeBsdfPrincipled')
        bsdf.inputs['Base Color'].default_value = (*spec['bc'], 1.0)
        bsdf.inputs['Metallic'].default_value = spec['metal']
        bsdf.inputs['Roughness'].default_value = spec['rough']
        if 'Emission Color' in bsdf.inputs:
            bsdf.inputs['Emission Color'].default_value = (*spec['em'], 1.0)
            bsdf.inputs['Emission Strength'].default_value = 1.0 if any(spec['em']) else 0.0
        log.append(f"mat {m.name!r} -> bc{spec['bc']} em{spec['em']} "
                   f"metal {spec['metal']} rough {spec['rough']}")

    bpy.ops.export_scene.gltf(filepath=OUT, export_format='GLB',
                              export_yup=True, export_apply=True)
    log.append("CONVERTED OK -> " + OUT)
    status = "OK"
except Exception:
    log.append(traceback.format_exc())

with open(OUT + ".log", "w", encoding="utf-8") as f:
    f.write("\n".join(log) + "\n")
with open(OUT + ".done", "w", encoding="utf-8") as f:
    f.write(status + "\n")
