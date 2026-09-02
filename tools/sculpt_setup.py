"""Sculpt pass step 1: import base body + CC0 genital proxy, inspect/align (file-report)."""
import bpy, sys, os
from mathutils import Vector
ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT = ARGV[0] if ARGV else "sculpt_setup.log"
L = []
def e(*a):
    s = " ".join(str(x) for x in a); L.append(s); print(s)

bpy.ops.wm.read_factory_settings(use_empty=True)

BASE = r"D:\GameDev\X3Native\assets\makehuman\female_base.glb"
PROXY = r"D:\GameDev\X3Native\assets\makehuman\female_genitals\female_muscular_simplified_genitals.obj"

bpy.ops.import_scene.gltf(filepath=BASE)
bpy.ops.wm.obj_import(filepath=PROXY)

meshes = [o for o in bpy.data.objects if o.type == "MESH"]
e("mesh objects:", [(m.name, len(m.data.vertices)) for m in meshes])

for m in meshes:
    dims = m.dimensions
    loc = m.location
    e(f"--- {m.name}")
    e("   location:", [round(x,3) for x in loc])
    e("   dimensions (x,y,z):", [round(x,3) for x in dims])
    # vertex groups
    vgs = [g.name for g in m.vertex_groups]
    e("   vertex_groups:", vgs[:40])

open(OUT, "w", encoding="utf-8").write("\n".join(L))
