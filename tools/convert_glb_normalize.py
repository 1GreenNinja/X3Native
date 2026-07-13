# Headless Blender GLB -> GLB normalizer for the X3Native asset pipeline.
#
# Fixes store-bought GLBs (HouseForge, unity-hero, ...) that the M2 loader
# rejects with 'prims=0 mats=N'. The loader (ModelLoader.cpp L743-759) hard-
# skips primitives that are (1) non-triangle, (2) Draco-compressed, or
# (3) missing POSITION. Blender bundles the Draco DECODER, so importing and
# re-exporting decompresses KHR_draco_mesh_compression and emits plain indexed
# triangles - same recipe as the 2026-05 convert_fbx_glb.py pass, which is why
# the old converted_glb store loads.
#
# Usage (single file):
#   blender --background --python convert_glb_normalize.py -- <src.glb> <dst.glb>
# Usage (batch a directory tree):
#   blender --background --python convert_glb_normalize.py -- <srcDir> <dstDir>
import bpy, sys, os, glob

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(argv) < 2:
    print("ERR: need <src.glb|srcDir> <dst.glb|dstDir>")
    sys.exit(1)
src, dst = argv[0], argv[1]

def convert(one_src, one_dst):
    os.makedirs(os.path.dirname(one_dst) or ".", exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=one_src)   # Draco decoded here
    bpy.ops.export_scene.gltf(
        filepath=one_dst,
        export_format='GLB',
        export_yup=True,          # engine loader expects glTF Y-up
        export_apply=True,        # apply modifiers; exporter triangulates
    )
    kb = os.path.getsize(one_dst) // 1024
    print("NORMALIZED OK: %s -> %s (%d KB)" % (one_src, one_dst, kb))

if os.path.isdir(src):
    files = glob.glob(os.path.join(src, "**", "*.glb"), recursive=True)
    print("batch: %d GLBs under %s" % (len(files), src))
    fails = []
    for f in files:
        rel = os.path.relpath(f, src)
        try:
            convert(f, os.path.join(dst, rel))
        except Exception as e:
            fails.append((rel, str(e)))
            print("FAIL: %s: %s" % (rel, e))
    print("batch done: %d ok, %d failed" % (len(files) - len(fails), len(fails)))
    for rel, err in fails:
        print("  FAILED %s: %s" % (rel, err))
else:
    convert(src, dst)
