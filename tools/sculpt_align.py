"""Sculpt step 2: create female body + import proxy, align, verify (file-report)."""
import bpy, sys, os, math
from mathutils import Vector
ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT = ARGV[0] if ARGV else "sculpt_align.log"
L = []
def e(*a):
    s = " ".join(str(x) for x in a); L.append(s); print(s)

bpy.ops.wm.read_factory_settings(use_empty=True)

from bl_ext.user_default.mpfb.services import HumanService, TargetService
macro = TargetService.get_default_macro_info_dict()
macro["gender"] = 0.0; macro["age"] = 0.5
macro["race"]["caucasian"] = 1.0; macro["race"]["african"] = 0.0; macro["race"]["asian"] = 0.0
macro["muscle"] = 0.3; macro["weight"] = 0.5; macro["cupsize"] = 0.5; macro["firmness"] = 0.5
HumanService.create_human(mask_helpers=True, detailed_helpers=True, extra_vertex_groups=True,
                          feet_on_ground=True, scale=0.1, macro_detail_dict=macro)

PROXY = r"D:\GameDev\X3Native\assets\makehuman\female_genitals\female_muscular_simplified_genitals.obj"
bpy.ops.wm.obj_import(filepath=PROXY)

def world_aabb(obj):
    cs = [obj.matrix_world @ Vector(c) for c in obj.bound_box]
    mn = Vector((min(c.x for c in cs), min(c.y for c in cs), min(c.z for c in cs)))
    mx = Vector((max(c.x for c in cs), max(c.y for c in cs), max(c.z for c in cs)))
    return mn, mx

base = next(o for o in bpy.data.objects if o.type == "MESH" and o.name.lower() == "human")
proxy = next(o for o in bpy.data.objects if o.type == "MESH" and "genitals" in o.name.lower())

def report(tag):
    bmn, bmx = world_aabb(base); pmn, pmx = world_aabb(proxy)
    e(f"[{tag}] base  min {[round(v,3) for v in bmn]} max {[round(v,3) for v in bmx]}")
    e(f"[{tag}] proxy min {[round(v,3) for v in pmn]} max {[round(v,3) for v in pmx]}")

report("before")

# align proxy: scale 0.1 (dm -> m), rotate -90deg about X (Y-up -> Z-up)
proxy.scale = (0.1, 0.1, 0.1)
proxy.rotation_euler = (math.radians(-90), 0, 0)
bpy.context.view_layer.update()
report("after scale0.1 + rotX-90")

open(OUT, "w", encoding="utf-8").write("\n".join(L))
