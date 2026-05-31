# Headless Blender FBX -> GLB with a LOG + DONE marker.
#
# The MS-Store Blender must be run via blender-launcher.exe, which DETACHES (no
# stdout). So this script writes <dst>.log (diagnostics / traceback) and <dst>.done
# ("OK" / "ERR") that the host polls + reads — same pattern as animate_creature.py.
#
# Usage: blender-launcher --background --python convert_pack_glb.py -- <src.fbx> <dst.glb>
# Reports objects/meshes/materials/images after import so we can see whether the
# FBX carries its textures (Unity packs usually DON'T — textures live in .mat/.png).
import bpy, sys, os, traceback

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
src = argv[0] if len(argv) > 0 else ""
dst = argv[1] if len(argv) > 1 else ""
log = dst + ".log"
done = dst + ".done"
lines = []

def emit(s):
    lines.append(str(s))

def finish(status):
    try:
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(log, "w", encoding="utf-8") as f:
            f.write("\n".join(lines))
        with open(done, "w", encoding="utf-8") as f:
            f.write(status)
    except Exception:
        pass

try:
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    emit("SRC: " + src)
    emit("DST: " + dst)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=src)
    objs = list(bpy.data.objects)
    meshes = [o for o in objs if o.type == 'MESH']
    emit("objects=%d meshes=%d materials=%d images=%d"
         % (len(objs), len(meshes), len(bpy.data.materials), len(bpy.data.images)))
    emit("materials: " + ", ".join(m.name for m in bpy.data.materials))
    emit("images: " + ", ".join(i.name for i in bpy.data.images))
    bpy.ops.export_scene.gltf(filepath=dst, export_format='GLB',
                              export_yup=True, export_apply=True)
    emit("exported bytes=%d" % (os.path.getsize(dst) if os.path.exists(dst) else -1))
    finish("OK")
except Exception:
    emit(traceback.format_exc())
    finish("ERR")
