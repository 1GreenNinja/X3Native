"""Smoke-test MPFB2 create_human headlessly (file-report)."""
import bpy, sys
ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT = ARGV[0] if ARGV else "mpfb_createhuman.log"
L = []
def e(*a):
    s = " ".join(str(x) for x in a); L.append(s); print(s)

scene = bpy.context.scene
# NH_ prefix (SceneConfigSet prefix="NH_")
def setp(name, val):
    try:
        setattr(scene, "NH_" + name, val)
        e("set NH_" + name, "=", val)
    except Exception as ex:
        e("FAILED set NH_" + name, repr(ex)[:120])

setp("scale_factor", "METER")
setp("add_phenotype", True)
setp("phenotype_gender", "female")
setp("phenotype_age", "young")
setp("add_breast", True)

try:
    bpy.ops.mpfb.create_human()
    e("create_human: OK")
except Exception as ex:
    e("create_human FAILED:", repr(ex)[:300])

# report what got created
meshes = [o for o in bpy.data.objects if o.type == "MESH"]
e("mesh objects after create:", len(meshes))
for m in meshes:
    e("  mesh:", m.name, "| verts:", len(m.data.vertices), "| faces:", len(m.data.polygons),
      "| shape_keys:", list(m.data.shape_keys.key_blocks.keys())[:8] if m.data.shape_keys else None)

open(OUT, "w", encoding="utf-8").write("\n".join(L))
