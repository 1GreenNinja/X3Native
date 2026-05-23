# Headless Blender skeleton dumper that writes to a FILE (not stdout).
#
#   blender-launcher.exe --background --python skel_dump.py -- <out.txt> <model.glb> [more...]
#
# WHY a file: on this box Blender is the Microsoft Store package. Direct
# blender.exe is ACL-denied; only the `blender-launcher.exe` app-execution alias
# runs, and it DETACHES (the caller gets no stdout). So any headless tool here
# must report through a file the caller can poll. Mirror this in every X3Native
# Blender tool. Read-only; exports nothing.
import bpy, sys, os

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(argv) < 2:
    out = os.path.join(os.environ.get("TEMP", "."), "x3_skel.txt")
    argv = [out] + argv
OUT, MODELS = argv[0], argv[1:]

lines = []
def emit(*a): lines.append(" ".join(str(x) for x in a))

def reset(): bpy.ops.wm.read_factory_settings(use_empty=True)

def imp(path):
    ext = os.path.splitext(path)[1].lower()
    if ext in (".glb", ".gltf"): bpy.ops.import_scene.gltf(filepath=path)
    elif ext == ".fbx": bpy.ops.import_scene.fbx(filepath=path)
    else: raise RuntimeError("unknown ext " + ext)

for path in MODELS:
    emit("\n========================================")
    emit("FILE:", path)
    reset()
    try:
        imp(path)
    except Exception as e:
        emit("  IMPORT FAILED:", e); continue
    arms = [o for o in bpy.data.objects if o.type == 'ARMATURE']
    emit("  armatures:", [a.name for a in arms])
    for a in arms:
        emit("  ARM '%s' bone_count=%d" % (a.name, len(a.data.bones)))
        for b in a.data.bones:
            # head_local gives a sense of body region (Z up in Blender).
            h = b.head_local
            emit("    bone: %-26s parent=%-20s head=(%.2f,%.2f,%.2f)"
                 % (b.name, (b.parent.name if b.parent else "-"), h.x, h.y, h.z))
    emit("  ACTIONS (%d):" % len(bpy.data.actions))
    for act in bpy.data.actions:
        fr = act.frame_range
        emit("    action: %-24s frames=[%.1f,%.1f]" % (act.name, fr[0], fr[1]))
    meshes = [o for o in bpy.data.objects if o.type == 'MESH']
    emit("  meshes:", [m.name for m in meshes])

emit("\nSKEL_DUMP DONE")
with open(OUT, "w", encoding="utf-8") as f:
    f.write("\n".join(lines))
