"""Sculpt: procedural vulva + introitus opening onto MPFB2 body (shape key), export GLB (file-report).

v2 — the vulva is a RECESSED cleft, not a protrusion. Front = -Y (nippleTip group).
Inward (recess into body) = +Y; outward (proud) = -Y. Dominant feature is the cleft
+ introitus recess; labia majora / mons are only subtly proud.
"""
import bpy, sys, os, math
from mathutils import Vector
ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT = ARGV[0] if ARGV else "sculpt_vulva.log"
L = []
def e(*a):
    s = " ".join(str(x) for x in a); L.append(s); print(s)
def flush(status):
    try: open(OUT, "w", encoding="utf-8").write("\n".join(L) + "\n" + status)
    except Exception: pass

CZ = 0.84          # vulva center height (Z) — raised so opening isn't clipped at frame edge
CLEFT_SX = 0.011   # cleft half-width (X sigma) — widened 2x (real ~2cm groove, not a scratch)
CLEFT_SZ = 0.036   # cleft vertical extent (Z sigma) — lengthened so it runs mons->opening unbroken
CLEFT_A = 0.013    # cleft recess depth (+Y, inward) — deepened 2.5x
OPEN_X = 0.009     # introitus half-width (X) — widened 2x
OPEN_A = 0.008     # introitus recess depth (+Y, inward) — shallowed (opening, not a punched hole)
OPEN_Z0 = -0.026   # introitus lower Z (rel CZ)
OPEN_Z1 = -0.006   # introitus upper Z (rel CZ)
LABIA_W = 0.018    # labia majora center (X) — pushed outward to ~2cm off-center
LABIA_SX = 0.005   # labia width (X sigma)
LABIA_A = 0.004    # labia proud depth (-Y, outward) — raised so they read as flanking ridges
MONS_Z = 0.018     # mons pubis center (rel CZ)
MONS_A = 0.003     # mons proud depth (-Y, outward, subtle)

def vulva_disp(x, z):
    wx = abs(x); dz = z - CZ
    dy = 0.0
    # central cleft (recess)
    dy += CLEFT_A * math.exp(-(wx / CLEFT_SX) ** 2) * math.exp(-(dz / CLEFT_SZ) ** 2)
    # introitus opening (deep recess at lower cleft)
    if wx < OPEN_X and OPEN_Z0 < dz < OPEN_Z1:
        dy += OPEN_A * (1.0 - wx / OPEN_X)
    # labia majora (subtle proud edges)
    dy += -LABIA_A * math.exp(-((wx - LABIA_W) / LABIA_SX) ** 2) * math.exp(-(dz / CLEFT_SZ) ** 2)
    # mons pubis (subtle proud at top)
    dy += -MONS_A * math.exp(-(wx / 0.010) ** 2) * math.exp(-((dz - MONS_Z) / 0.012) ** 2)
    return dy

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

    crotch = [i for i, v in enumerate(base.data.vertices)
              if 0.70 <= v.co.z <= 0.94 and abs(v.co.x) <= 0.075 and v.co.y <= -0.10]
    e("crotch verts:", len(crotch))

    base.shape_key_add(name="FemaleGenitals", from_mix=False)
    sk = base.data.shape_keys.key_blocks["FemaleGenitals"]
    n = 0
    for i in crotch:
        v = base.data.vertices[i]
        dy = vulva_disp(v.co.x, v.co.z)
        if abs(dy) > 1e-5:
            sk.data[i].co = Vector((v.co.x, v.co.y + dy, v.co.z))
            n += 1
    sk.value = 1.0
    e("verts displaced:", n)

    outglb = r"D:\GameDev\X3Native\assets\makehuman\female_vulva.glb"
    bpy.ops.export_scene.gltf(filepath=outglb, export_format='GLB', use_selection=True,
                              export_apply=False, export_materials='EXPORT')
    e("exported:", outglb, os.path.getsize(outglb), "bytes")
    flush("OK")
except Exception as ex:
    import traceback
    e("FAILED:", repr(ex)); e(traceback.format_exc()); flush("FAIL")
