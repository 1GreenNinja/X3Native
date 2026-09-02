"""Install MPFB2 as a Blender 4.5 extension (file-report)."""
import bpy, sys, os
ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT = ARGV[0] if ARGV else "mpfb_install.log"
L = []
def e(*a):
    s = " ".join(str(x) for x in a); L.append(s); print(s)

e("blender", ".".join(map(str, bpy.app.version)))
SRC = r"C:\Users\Tim\AppData\Local\Temp\mpfb2\src\mpfb"

# 1. remove any prior manual copy from the repo (avoid double-install)
repo_dir = r"C:\Users\Tim\AppData\Roaming\Blender Foundation\Blender\4.5\extensions\user_default\mpfb"
if os.path.isdir(repo_dir):
    import shutil
    shutil.rmtree(repo_dir)
    e("removed prior manual copy")

# 2. install from source dir (installing enables it)
for kw in (dict(directory=SRC), dict(filepath=SRC)):
    try:
        bpy.ops.extensions.package_install_files(repo="user_default", **kw)
        e("package_install_files OK with", list(kw))
        break
    except Exception as ex:
        e("package_install_files FAILED with", list(kw), "->", repr(ex)[:200])

# 3. refresh + report
try:
    bpy.ops.extensions.repo_refresh_all()
    e("repo_refresh_all OK")
except Exception as ex:
    e("repo_refresh_all FAILED:", repr(ex)[:150])

# 4. list what got discovered
try:
    exts = bpy.context.preferences.extensions
    e("extensions attr type:", type(exts).__name__)
    for rname in ("repos", "packages", "modules"):
        if hasattr(exts, rname):
            coll = getattr(exts, rname)
            e(f"  {rname}:", list(coll)[:10] if hasattr(coll, "__iter__") else coll)
except Exception as ex:
    e("inspect extensions failed:", repr(ex)[:150])

# 5. operators present (proof of registration)
ops = sorted(o for o in dir(bpy.ops) if "mpfb" in o.lower())
e("mpfb operator count:", len(ops))
e("sample ops:", ops[:12])

open(OUT, "w", encoding="utf-8").write("\n".join(L))
