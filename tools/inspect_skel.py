# Headless Blender skeleton/animation inspector for the X3Native asset pipeline.
# Usage: blender --background --python inspect_skel.py -- <file.glb|file.fbx> [...]
# For each input, prints: armature name, bone count, bone names (hierarchy),
# and any actions (animations) with frame range. Read-only; exports nothing.
import bpy, sys, os

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []

def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)

def imp(path):
    ext = os.path.splitext(path)[1].lower()
    if ext == ".glb" or ext == ".gltf":
        bpy.ops.import_scene.gltf(filepath=path)
    elif ext == ".fbx":
        bpy.ops.import_scene.fbx(filepath=path)
    else:
        raise RuntimeError("unknown ext " + ext)

for path in argv:
    print("\n========================================")
    print("FILE:", path)
    reset()
    try:
        imp(path)
    except Exception as e:
        print("  IMPORT FAILED:", e)
        continue
    arms = [o for o in bpy.data.objects if o.type == 'ARMATURE']
    print("  armatures:", [a.name for a in arms])
    for a in arms:
        bones = a.data.bones
        print("  ARM '%s' bone_count=%d" % (a.name, len(bones)))
        for b in bones:
            parent = b.parent.name if b.parent else "-"
            print("    bone: %-30s parent=%s" % (b.name, parent))
    print("  ACTIONS (%d):" % len(bpy.data.actions))
    for act in bpy.data.actions:
        fr = act.frame_range
        # Blender 5.0 slotted actions: fcurves live under layers/strips/channelbags.
        fcs = []
        if hasattr(act, "fcurves") and len(act.fcurves):
            fcs = list(act.fcurves)
        else:
            for layer in getattr(act, "layers", []):
                for strip in getattr(layer, "strips", []):
                    for cb in getattr(strip, "channelbags", []):
                        fcs.extend(cb.fcurves)
        targets = set()
        for fc in fcs:
            dp = fc.data_path
            if dp.startswith('pose.bones['):
                nm = dp.split('"')[1] if '"' in dp else dp
                targets.add(nm)
        print("    action: %-30s frames=[%.1f,%.1f] fcurves=%d bones=%d"
              % (act.name, fr[0], fr[1], len(fcs), len(targets)))
    meshes = [o for o in bpy.data.objects if o.type == 'MESH']
    print("  meshes:", [m.name for m in meshes])
print("\nINSPECT DONE")
