"""Sculpt step A: align proxy + shrinkwrap-transfer vulva to base 'genitals' vgroup, render crotch (file-report)."""
import bpy, sys, os, math
from mathutils import Vector
ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT = ARGV[0] if ARGV else "sculpt_transfer.log"
OUTDIR = r"D:\GameDev\X3Native\docs\screenshots\sculpt"
os.makedirs(OUTDIR, exist_ok=True)
L = []
def e(*a):
    s = " ".join(str(x) for x in a); L.append(s); print(s)

bpy.ops.wm.read_factory_settings(use_empty=True)

from bl_ext.user_default.mpfb.services import HumanService, TargetService
macro = TargetService.get_default_macro_info_dict()
macro["gender"]=0.0; macro["age"]=0.5
macro["race"]["caucasian"]=1.0; macro["race"]["african"]=0.0; macro["race"]["asian"]=0.0
macro["muscle"]=0.3; macro["weight"]=0.5; macro["cupsize"]=0.5; macro["firmness"]=0.5
HumanService.create_human(mask_helpers=True, detailed_helpers=True, extra_vertex_groups=True,
                          feet_on_ground=True, scale=0.1, macro_detail_dict=macro)

PROXY = r"D:\GameDev\X3Native\assets\makehuman\female_genitals\female_muscular_simplified_genitals.obj"
bpy.ops.wm.obj_import(filepath=PROXY)

base = next(o for o in bpy.data.objects if o.type=="MESH" and o.name.lower()=="human")
proxy = next(o for o in bpy.data.objects if o.type=="MESH" and "genitals" in o.name.lower())

# align proxy: scale 0.1, rot -90 X, translate to coincide with base
proxy.scale = (0.1,0.1,0.1)
proxy.rotation_euler = (math.radians(-90),0,0)
bpy.context.view_layer.update()
proxy.location = (0.0, -0.202, 0.7865)
bpy.context.view_layer.update()
e("proxy aligned; has 'genitals' vgroup:", "genitals" in [g.name for g in base.vertex_groups])

# shrinkwrap-transfer the vulva onto the base genitals region
mod = base.modifiers.new("VulvaTransfer", 'SHRINKWRAP')
mod.target = proxy
mod.vertex_group = "genitals"
mod.wrap_method = 'NEAREST_SURFACEPOINT'
e("shrinkwrap added:", mod.name, "method:", mod.wrap_method)

# ---- render setup (crotch region, front + side) ----
scene = bpy.context.scene
scene.render.engine = 'BLENDER_EEVEE_NEXT' if hasattr(bpy.types, 'BLENDER_EEVEE_NEXT') else 'BLENDER_EEVEE'
scene.render.resolution_x = 1024
scene.render.resolution_y = 1024
scene.render.image_settings.file_format = 'PNG'

# hide proxy from render (we want to see the base's transferred vulva)
proxy.hide_render = True

# material for base: light neutral so we can read the form
if base.data.materials:
    mat = base.data.materials[0]
else:
    mat = bpy.data.materials.new("neutral")
    base.data.materials.append(mat)
if mat.node_tree and mat.node_tree.nodes.get("Principled BSDF"):
    bsdf = mat.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (0.82, 0.74, 0.70, 1.0)

# camera
cam_data = bpy.data.cameras.new("QA")
cam = bpy.data.objects.new("QA", cam_data)
scene.collection.objects.link(cam)
scene.camera = cam

def aim(campos, lookat):
    cam.location = Vector(campos)
    d = Vector(lookat) - Vector(campos)
    cam.rotation_euler = d.to_track_quat('-Z','Y').to_euler()

# light
light_data = bpy.data.lights.new("L", 'AREA')
light = bpy.data.objects.new("L", light_data)
scene.collection.objects.link(light)
light.location = (0, -1.5, 1.6)
light.data.energy = 400

# view 1: front (camera on -Y looking toward +Y at crotch z=0.8)
aim((-0.0, -0.7, 0.80), (0.0, 0.0, 0.80))
scene.render.filepath = os.path.join(OUTDIR, "crotch_front.png")
bpy.ops.render.render(write_still=True)
e("rendered crotch_front.png")

# view 2: side (camera on +X)
aim((0.7, 0.0, 0.80), (0.0, 0.0, 0.80))
scene.render.filepath = os.path.join(OUTDIR, "crotch_side.png")
bpy.ops.render.render(write_still=True)
e("rendered crotch_side.png")

open(OUT, "w", encoding="utf-8").write("\n".join(L))
