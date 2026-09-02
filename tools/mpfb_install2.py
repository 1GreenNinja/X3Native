"""Install MPFB2 from zip into Blender 4.5 (file-report)."""
import bpy, sys
ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT = ARGV[0] if ARGV else "mpfb_install2.log"
L = []
def e(*a):
    s = " ".join(str(x) for x in a); L.append(s); print(s)

ZIP = r"C:\Users\Tim\AppData\Local\Temp\mpfb2\mpfb.zip"
e("blender", ".".join(map(str, bpy.app.version)))

try:
    bpy.ops.extensions.package_install_files(repo="user_default", filepath=ZIP)
    e("install_files OK")
except Exception as ex:
    e("install_files FAILED:", repr(ex)[:250])

try:
    bpy.ops.extensions.repo_refresh_all()
    e("refresh OK")
except Exception as ex:
    e("refresh FAILED:", repr(ex)[:150])

# list packages in the User Default repo
try:
    exts = bpy.context.preferences.extensions
    for repo in exts.repos:
        if getattr(repo, "module", "") == "user_default":
            for attr in ("packages", "modules"):
                if hasattr(repo, attr):
                    coll = getattr(repo, attr)
                    try:
                        items = list(coll)
                        e(f"repo {attr}:", len(items))
                        for m in items:
                            e("  pkg:", getattr(m, "module", getattr(m, "name", "?")),
                              "enabled:", getattr(m, "enabled", "n/a"))
                    except Exception as ex2:
                        e(f"  {attr} list failed:", repr(ex2)[:120])
except Exception as ex:
    e("inspect failed:", repr(ex)[:150])

ops = sorted(o for o in dir(bpy.ops) if "mpfb" in o.lower())
e("mpfb operator count:", len(ops), ops[:12])

try:
    bpy.ops.wm.save_userpref()
    e("userpref saved")
except Exception as ex:
    e("save failed:", repr(ex)[:150])

open(OUT, "w", encoding="utf-8").write("\n".join(L))
