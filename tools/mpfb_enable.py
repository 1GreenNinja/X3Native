"""Enable the installed MPFB extension (file-report)."""
import bpy, sys
ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT = ARGV[0] if ARGV else "mpfb_enable.log"
L = []
def e(*a):
    s = " ".join(str(x) for x in a); L.append(s); print(s)

exts = bpy.context.preferences.extensions
for repo in exts.repos:
    e("REPO:", getattr(repo, "name", "?"), "| module:", getattr(repo, "module", "?"),
      "| dir:", getattr(repo, "directory", "?"))
    # try to list packages/modules
    for attr in ("modules", "packages", "extensions"):
        if hasattr(repo, attr):
            coll = getattr(repo, attr)
            try:
                items = list(coll)
                e(f"  {attr} count:", len(items))
                for m in items:
                    e("    pkg:", getattr(m, "module", getattr(m, "name", "?")),
                      "| enabled:", getattr(m, "enabled", "n/a"))
            except Exception as ex:
                e(f"  {attr} list failed:", repr(ex)[:120])

# attempt: enable the mpfb module by setting enabled on its repo module
try:
    # walk repos for a module named mpfb
    for repo in exts.repos:
        for attr in ("modules", "packages"):
            if hasattr(repo, attr):
                for m in getattr(repo, attr):
                    if getattr(m, "module", "") == "mpfb":
                        e("FOUND mpfb module; enabled before:", getattr(m, "enabled", None))
                        m.enabled = True
                        e("set enabled=True; now:", getattr(m, "enabled", None))
                        break
except Exception as ex:
    e("enable attempt failed:", repr(ex)[:200])

# save userpref
try:
    bpy.ops.wm.save_userpref()
    e("userpref saved")
except Exception as ex:
    e("save failed:", repr(ex)[:150])

ops = sorted(o for o in dir(bpy.ops) if "mpfb" in o.lower())
e("mpfb operator count (post-enable):", len(ops), ops[:10])

open(OUT, "w", encoding="utf-8").write("\n".join(L))
