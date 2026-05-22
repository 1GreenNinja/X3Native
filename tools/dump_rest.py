# Dump rest-pose local bone rotations (quaternion) for a few key bones, to decide
# whether two rigs share a rest pose (direct local-rotation copy OK) or need
# rest-delta compensation during retarget.
# Usage: blender --background --python dump_rest.py -- <file> <armname> <bone1> [bone2 ...]
import bpy, sys, os
from mathutils import Quaternion

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
path = argv[0]; armname = argv[1]; bones = argv[2:]

bpy.ops.wm.read_factory_settings(use_empty=True)
ext = os.path.splitext(path)[1].lower()
if ext in (".glb", ".gltf"):
    bpy.ops.import_scene.gltf(filepath=path)
else:
    bpy.ops.import_scene.fbx(filepath=path)

arm = bpy.data.objects.get(armname) or next(o for o in bpy.data.objects if o.type=='ARMATURE')
print("ARM:", arm.name)
for bn in bones:
    eb = arm.data.bones.get(bn)
    if not eb:
        print("  MISSING", bn); continue
    # rest matrix in armature space; and local (parent-relative) rest matrix
    if eb.parent:
        local = eb.parent.matrix_local.inverted() @ eb.matrix_local
    else:
        local = eb.matrix_local
    q = local.to_quaternion()
    print("  %-18s restLocalQuat=(%.3f,%.3f,%.3f,%.3f)  headWS=%s" %
          (bn, q.x, q.y, q.z, q.w, tuple(round(v,3) for v in eb.head_local)))
print("DUMP DONE")
