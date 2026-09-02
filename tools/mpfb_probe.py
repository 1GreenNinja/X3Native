"""Probe Blender 4.5 extension API surface (file-report)."""
import bpy, sys
ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT = ARGV[0] if ARGV else "mpfb_probe.log"
L = []
def e(*a): L.append(" ".join(str(x) for x in a)); print(*a)

e("blender", ".".join(map(str, bpy.app.version)))
e("has extensions ops:", hasattr(bpy.ops, "extensions"))
if hasattr(bpy.ops, "extensions"):
    e("extensions ops:", sorted(o for o in dir(bpy.ops.extensions) if not o.startswith("_")))
# legacy addons collection
e("legacy addon modules:", [a.module for a in bpy.context.preferences.addons])
# any extension-ish collections on preferences
for name in dir(bpy.context.preferences):
    if "ext" in name.lower():
        e("pref attr:", name)
open(OUT, "w", encoding="utf-8").write("\n".join(L))
