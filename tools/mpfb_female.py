"""Generate a female adult MPFB2 body headlessly (direct HumanService call)."""
import bpy, sys, os
ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT = ARGV[0] if ARGV else "mpfb_female.log"
L = []
def e(*a):
    s = " ".join(str(x) for x in a); L.append(s); print(s)

from bl_ext.user_default.mpfb.services import HumanService, TargetService

macro = TargetService.get_default_macro_info_dict()
e("default macro keys:", sorted(macro.keys()))
e("race subkeys:", sorted(macro.get("race", {}).keys()) if isinstance(macro.get("race"), dict) else macro.get("race"))

# female, young adult, caucasian
macro["gender"] = 0.0
macro["age"] = 0.5
macro["race"]["caucasian"] = 1.0
macro["race"]["african"] = 0.0
macro["race"]["asian"] = 0.0
macro["muscle"] = 0.3
macro["weight"] = 0.5
macro["cupsize"] = 0.5
macro["firmness"] = 0.5

e("creating female adult body ...")
try:
    basemesh = HumanService.create_human(
        mask_helpers=True, detailed_helpers=True, extra_vertex_groups=True,
        feet_on_ground=True, scale=0.1, macro_detail_dict=macro)
    e("create_human OK ->", getattr(basemesh, "name", basemesh))
except Exception as ex:
    e("create_human FAILED:", repr(ex)[:400])

meshes = [o for o in bpy.data.objects if o.type == "MESH" and o.name.lower() != "cube"]
for m in meshes:
    e("mesh:", m.name, "| verts:", len(m.data.vertices), "| faces:", len(m.data.polygons),
      "| dims:", [round(x,3) for x in m.dimensions[:]])

# export for later inspection
if meshes:
    outdir = r"D:\GameDev\X3Native\assets\makehuman"
    os.makedirs(outdir, exist_ok=True)
    outglb = os.path.join(outdir, "female_base.glb")
    bpy.ops.object.select_all(action='DESELECT')
    meshes[0].select_set(True)
    bpy.context.view_layer.objects.active = meshes[0]
    bpy.ops.export_scene.gltf(filepath=outglb, export_format='GLB', use_selection=True,
                              export_apply=False, export_materials='EXPORT')
    e("exported:", outglb, os.path.getsize(outglb), "bytes")

open(OUT, "w", encoding="utf-8").write("\n".join(L))
