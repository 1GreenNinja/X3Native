# Headless Blender FBX -> GLB converter for the X3Native asset pipeline.
# Usage: blender --background --python convert_fbx_glb.py -- <src.fbx> <dst.glb>
# Imports one FBX into an empty scene and exports a single GLB (binary glTF 2.0).
import bpy, sys, os

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(argv) < 2:
    print("ERR: need <src.fbx> <dst.glb>")
    sys.exit(1)
src, dst = argv[0], argv[1]

os.makedirs(os.path.dirname(dst), exist_ok=True)

# Empty scene, import the FBX, export GLB.
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=src)
bpy.ops.export_scene.gltf(
    filepath=dst,
    export_format='GLB',
    export_yup=True,            # glTF is Y-up (matches the engine's loader)
    export_apply=True,          # apply modifiers
)
print("CONVERTED OK:", src, "->", dst)
