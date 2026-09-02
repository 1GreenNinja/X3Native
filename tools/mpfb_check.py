"""mpfb_check.py — headless MPFB2 load/verify for Blender 4.5 (file-report protocol).

   blender-launcher.exe --background --python mpfb_check.py -- <out.log>
"""
import bpy, sys

ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT = ARGV[0] if ARGV else "mpfb_check.log"
lines = []
def emit(*a):
    s = " ".join(str(x) for x in a)
    lines.append(s); print(s)

emit("blender", ".".join(map(str, bpy.app.version)))

# 1. enable as legacy add-on
try:
    bpy.ops.preferences.addon_enable(module="mpfb")
    emit("enable: OK")
except Exception as e:
    emit("enable FAILED:", repr(e))

emit("mpfb in addons:", "mpfb" in bpy.context.preferences.addons)

# 2. operators present (proof of registration)
ops = sorted(o for o in dir(bpy.ops) if "mpfb" in o.lower())
emit("mpfb operator count:", len(ops))
emit("sample ops:", ops[:15])

# 3. persist
try:
    bpy.ops.wm.save_userpref()
    emit("userpref saved")
except Exception as e:
    emit("save failed:", repr(e))

open(OUT, "w", encoding="utf-8").write("\n".join(lines))
