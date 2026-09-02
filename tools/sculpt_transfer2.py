"""Sculpt step A (no render): align proxy + shrinkwrap-transfer vulva, apply, export GLB (file-report)."""
import bpy, sys, os, math
from mathutils import Vector
ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT = ARGV[0] if ARGV else "sculpt_transfer2.log"
L = []
def e(*a):
    s = " ".join(str(x) for x in a); L.append(s); print(s)
def flush(status):
    try:
        open(OUT, "w", encoding="utf-8").write("\n".join(L) + "\n" + status)
    except Exception:
        pass

try:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    from bl_ext.user_default.mpfb.services import HumanService, TargetService
    macro = TargetService.get_default_macro_info_dict()
    macro["gender"]=0.0; macro["age"]=0.5
    macro["race"]["caucasian"]=1.0; macro["race"]["african"]=0.0; macro["race"]["asian"]=0.0
    macro["muscle"]=0.3; macro["weight"]=0.5; macro["cupsize"]=0.5; macro["firmness"]=0.5
    e("creating female body ...")
    HumanService.create_human(mask_helpers=True, detailed_helpers=True, extra_vertex_groups=True,
                              feet_on_ground=True, scale=0.1, macro_detail_dict=macro)
    base = next(o for o in bpy.data.objects if o.type=="MESH" and o.name.lower()=="human")
    e("base:", base.name, len(base.data.vertices), "verts")

    PROXY = r"D:\GameDev\X3Native\assets\makehuman\female_genitals\female_muscular_simplified_genitals.obj"
    bpy.ops.wm.obj_import(filepath=PROXY)
    proxy = next(o for o in bpy.data.objects if o.type=="MESH" and "genitals" in o.name.lower())
    proxy.scale = (0.1,0.1,0.1)
    proxy.rotation_euler = (math.radians(-90),0,0)
    bpy.context.view_layer.update()
    proxy.location = (0.0, -0.202, 0.7865)
    bpy.context.view_layer.update()
    e("proxy aligned")

    # record genitals region position before
    gidx = base.vertex_groups["genitals"].index
    before = [v.co.copy() for v in base.data.vertices if gidx in [g.group for g in v.groups]]
    e("genitals verts before:", len(before))

    mod = base.modifiers.new("VulvaTransfer", 'SHRINKWRAP')
    mod.target = proxy
    mod.vertex_group = "genitals"
    mod.wrap_method = 'NEAREST_SURFACEPOINT'
    e("shrinkwrap added")

    bpy.context.view_layer.objects.active = base
    base.select_set(True)
    bpy.ops.object.modifier_apply(modifier="VulvaTransfer")
    e("shrinkwrap applied")

    # record after (max displacement)
    after = [v.co for v in base.data.vertices if gidx in [g.group for g in v.groups]]
    maxd = max((a-b).length for a,b in zip(after, before)) if after else 0.0
    e("genitals verts after:", len(after), "max displacement:", round(maxd,4), "m")

    outglb = r"D:\GameDev\X3Native\assets\makehuman\female_vulva_transfer.glb"
    bpy.ops.export_scene.gltf(filepath=outglb, export_format='GLB', use_selection=True,
                              export_apply=False, export_materials='EXPORT')
    e("exported:", outglb, os.path.getsize(outglb), "bytes")
    flush("OK")
except Exception as ex:
    import traceback
    e("FAILED:", repr(ex)); e(traceback.format_exc()); flush("FAIL")
