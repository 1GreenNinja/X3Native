# Headless Blender: generate a distance-LOD chain from a single LOD0 GLB.
#
# Assets ship LOD0-only (see assets/converted_glb/CATALOG.md); the S2 distance-
# LOD system (app/space/lod.{h,cpp}) needs LOD1/2/3 meshes to swap to as the
# camera recedes. This script loads a GLB, applies a COLLAPSE DECIMATE modifier
# at three ratios (LOD1=0.5, LOD2=0.25, LOD3=0.1 of the LOD0 triangle count) and
# re-exports each as a sibling GLB. It mirrors the decimation approach of
# tools/rodin_to_glb/downscale_glb.py (same modifier type + apply pattern).
#
# Usage (PowerShell / bash — `--` separates Blender args from script args):
#
#   blender --background --python tools/gen_lod.py -- <in.glb> [out_prefix] [r1 r2 r3]
#
#   <in.glb>     the LOD0 source mesh (required)
#   out_prefix   output path stem; defaults to the input path without ".glb".
#                Files written: <prefix>_lod1.glb, <prefix>_lod2.glb, <prefix>_lod3.glb
#   r1 r2 r3     decimation ratios for LOD1/2/3 (default 0.5 0.25 0.1).
#                Ratio is fraction of LOD0 triangles KEPT (Blender COLLAPSE ratio).
#
# Examples:
#   blender --background --python tools/gen_lod.py -- assets/converted_glb/probe.glb
#   blender --background --python tools/gen_lod.py -- ship.glb build/ship 0.4 0.2 0.08
#
# Output: three sibling GLBs forming the LOD1..LOD3 chain. LOD0 is the untouched
# input (the engine registers all four via rhi createMesh and feeds the handles
# to LodSystem::makeFromChain). Run engine-side `--world lod` to see the swap.
#
# Clean-room: standard Blender decimation; no idTech/Doom/Quake source consulted.

import bpy, sys, os

argv = sys.argv
argv = argv[argv.index("--") + 1:] if "--" in argv else []
if not argv:
    print("ERROR: usage: blender --background --python gen_lod.py -- <in.glb> "
          "[out_prefix] [r1 r2 r3]")
    sys.exit(1)

INP = argv[0]
PREFIX = argv[1] if len(argv) > 1 else (INP[:-4] if INP.lower().endswith(".glb") else INP)
# Default LOD ratios: LOD1=0.5, LOD2=0.25, LOD3=0.1 (fraction of LOD0 tris kept).
RATIOS = [0.5, 0.25, 0.1]
if len(argv) >= 5:
    RATIOS = [float(argv[2]), float(argv[3]), float(argv[4])]


def clear_scene():
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    for coll in (bpy.data.meshes, bpy.data.materials, bpy.data.images):
        for d in list(coll):
            if d.users == 0:
                coll.remove(d)


def total_tris():
    n = 0
    for o in bpy.data.objects:
        if o.type == 'MESH':
            o.data.calc_loop_triangles()
            n += len(o.data.loop_triangles)
    return n


def decimate_all(ratio):
    # Apply a COLLAPSE decimate modifier to every mesh object (mirrors
    # downscale_glb.py's decimation lever). ratio is fraction of tris KEPT.
    for o in list(bpy.data.objects):
        if o.type == 'MESH':
            m = o.modifiers.new("lod_dec", 'DECIMATE')
            m.decimate_type = 'COLLAPSE'
            m.ratio = ratio
            bpy.context.view_layer.objects.active = o
            bpy.ops.object.modifier_apply(modifier=m.name)


# Generate each LOD by re-importing LOD0 fresh and decimating to the cumulative
# ratio (ratio is always relative to the ORIGINAL LOD0 triangle count).
for lvl, ratio in enumerate(RATIOS, start=1):
    clear_scene()
    bpy.ops.import_scene.gltf(filepath=INP)
    base = total_tris()
    decimate_all(ratio)
    out = "%s_lod%d.glb" % (PREFIX, lvl)
    bpy.ops.export_scene.gltf(filepath=out, export_format='GLB')
    kept = total_tris()
    print("LOD%d  ratio=%.3f  %d -> %d tris  -> %s (%d KB)"
          % (lvl, ratio, base, kept, out,
             os.path.getsize(out) // 1024 if os.path.exists(out) else 0))

print("===== gen_lod: wrote LOD1..LOD%d from %s =====" % (len(RATIOS), os.path.basename(INP)))
