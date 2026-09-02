"""Sculpt step A v3: define crotch region, shrinkwrap->shape-key transfer (file-report)."""
import bpy, sys, os, math
from mathutils import Vector
ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT = ARGV[0] if ARGV else "sculpt_transfer3.log"
L = []
def e(*a):
    s = " ".join(str(x) for x in a); L.append(s); print(s)
def flush(status):
    try: open(OUT, "w", encoding="utf-8").write("\n".join(L) + "\n" + status)
    except Exception: pass

def vgroup_positions(mesh, group):
    gi = group.index
    return [v.co.copy() for v in mesh.vertices if gi in [g.group for g in v.groups]]

try:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    from bl_ext.user_default.mpfb.services import HumanService, TargetService
    macro = TargetService.get_default_macro_info_dict()
    macro["gender"]=0.0; macro["age"]=0.5
    macro["race"]["caucasian"]=1.0; macro["race"]["african"]=0.0; macro["race"]["asian"]=0.0
    macro["muscle"]=0.3; macro["weight"]=0.5; macro["cupsize"]=0.5; macro["firmness"]=0.5
    HumanService.create_human(mask_helpers=True, detailed_helpers=True, extra_vertex_groups=True,
                              feet_on_ground=True, scale=0.1, macro_detail_dict=macro)
    base = next(o for o in bpy.data.objects if o.type=="MESH" and o.name.lower()=="human")
    e("base:", len(base.data.vertices), "verts")

    # determine FRONT direction from nipple/nippleTip groups (front of torso)
    front_sign = None
    for gname in ("nippleTip", "nipple"):
        if gname in base.vertex_groups:
            g = base.vertex_groups[gname]
            pts = vgroup_positions(base.data, g)
            if pts:
                avg_y = sum(p.y for p in pts) / len(pts)
                front_sign = 1.0 if avg_y > 0 else -1.0
                e(f"front dir from {gname}: y={round(avg_y,3)} -> sign {front_sign}")
                break
    if front_sign is None:
        front_sign = -1.0
        e("WARN: no nipple group; assuming front = -Y")

    # define crotch region by position (pubic mound + perineum, front side, between legs)
    def in_crotch(p):
        return (0.70 <= p.z <= 0.92) and (abs(p.x) <= 0.075) and (p.y * front_sign <= 0.12)

    # clear + refill the 'genitals' group
    g = base.vertex_groups["genitals"]
    for v in base.data.vertices:
        g.remove([v.index]) if False else None
    # remove all then re-add selected
    bpy.context.view_layer.objects.active = base
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='DESELECT')
    bpy.ops.object.mode_set(mode='OBJECT')
    n_sel = 0
    for v in base.data.vertices:
        if in_crotch(v.co):
            g.add([v.index], 1.0, 'REPLACE')
            n_sel += 1
    e("crotch verts selected:", n_sel)

    PROXY = r"D:\GameDev\X3Native\assets\makehuman\female_genitals\female_muscular_simplified_genitals.obj"
    bpy.ops.wm.obj_import(filepath=PROXY)
    proxy = next(o for o in bpy.data.objects if o.type=="MESH" and "genitals" in o.name.lower())
    proxy.scale = (0.1,0.1,0.1)
    proxy.rotation_euler = (math.radians(-90),0,0)
    bpy.context.view_layer.update()
    proxy.location = (0.0, -0.202, 0.7865)
    bpy.context.view_layer.update()
    e("proxy aligned")

    mod = base.modifiers.new("VulvaTransfer", 'SHRINKWRAP')
    mod.target = proxy
    mod.vertex_group = "genitals"
    mod.wrap_method = 'NEAREST_SURFACEPOINT'
    bpy.context.view_layer.update()

    # capture as a shape key (works with existing shape keys), then drop the modifier
    bpy.ops.object.shape_key_add(from_mix=True)
    sk = base.data.shape_keys
    new_key = sk.key_blocks[-1]
    new_key.name = "FemaleGenitals"
    e("shape key created:", new_key.name, "total keys:", len(sk.key_blocks))
    base.modifiers.remove(mod)
    e("shrinkwrap removed (shape captured)")

    # report: max displacement in the new key
    basis = sk.key_blocks["Basis"]
    fg = sk.key_blocks["FemaleGenitals"]
    maxd = 0.0; n_moved = 0
    for i, v in enumerate(base.data.vertices):
        d = (fg.data[i].co - basis.data[i].co).length
        if d > 1e-4:
            n_moved += 1
            maxd = max(maxd, d)
    e("verts displaced by FemaleGenitals:", n_moved, "max:", round(maxd,4), "m")

    outglb = r"D:\GameDev\X3Native\assets\makehuman\female_vulva_shapekey.glb"
    bpy.ops.export_scene.gltf(filepath=outglb, export_format='GLB', use_selection=True,
                              export_apply=False, export_materials='EXPORT')
    e("exported:", outglb, os.path.getsize(outglb), "bytes")
    flush("OK")
except Exception as ex:
    import traceback
    e("FAILED:", repr(ex)); e(traceback.format_exc()); flush("FAIL")
