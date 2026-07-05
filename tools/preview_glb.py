# preview_glb.py - bpy: render a quick preview PNG of a GLB (eye-gate for baked
# assets when no engine screenshot path can frame them). Camera auto-frames the
# object from a 3/4 view; one sun + one fill.
#   blender-launcher --background --python tools/preview_glb.py -- <in.glb> <out.png> [yaw_deg]
# Writes <out.png>.log/.done markers (MS-Store launcher detaches; see anim_build.ps1).
import bpy, sys, math, os

argv = sys.argv[sys.argv.index("--") + 1:]
IN_GLB, OUT_PNG = argv[0], argv[1]
YAW = math.radians(float(argv[2])) if len(argv) > 2 else math.radians(35.0)
LOG = OUT_PNG + ".log"; DONE = OUT_PNG + ".done"
def log(m):
    with open(LOG, "a") as f: f.write(str(m) + "\n")
def finish(s):
    log("STATUS: " + s)
    with open(DONE, "w") as f: f.write(s + "\n")
try:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=IN_GLB)
    meshes = [o for o in bpy.data.objects if o.type == "MESH"]
    if not meshes: finish("FAIL: no mesh"); raise SystemExit
    # bounds over all meshes (world)
    from mathutils import Vector
    mn = Vector((1e9,)*3); mx = Vector((-1e9,)*3)
    for o in meshes:
        for c in o.bound_box:
            w = o.matrix_world @ Vector(c)
            mn = Vector(map(min, mn, w)); mx = Vector(map(max, mx, w))
    ctr = (mn + mx) * 0.5; ext = (mx - mn).length
    log("bounds min=%s max=%s ext=%.3f" % (tuple(mn), tuple(mx), ext))
    cam_d = bpy.data.cameras.new("c"); cam = bpy.data.objects.new("c", cam_d)
    bpy.context.scene.collection.objects.link(cam)
    dist = max(ext * 1.2, 0.5)
    cam.location = (ctr.x + dist * math.sin(YAW), ctr.y - dist * math.cos(YAW), ctr.z + ext * 0.25)
    look = ctr - Vector(cam.location)
    cam.rotation_euler = look.to_track_quat("-Z", "Y").to_euler()
    bpy.context.scene.camera = cam
    sun_d = bpy.data.lights.new("s", "SUN"); sun_d.energy = 3.0
    sun = bpy.data.objects.new("s", sun_d); sun.rotation_euler = (math.radians(50), 0, math.radians(20))
    bpy.context.scene.collection.objects.link(sun)
    fill_d = bpy.data.lights.new("f", "AREA"); fill_d.energy = 200.0; fill_d.size = 3.0
    fill = bpy.data.objects.new("f", fill_d); fill.location = (ctr.x - 1.5, ctr.y - 1.5, ctr.z + 1.0)
    bpy.context.scene.collection.objects.link(fill)
    sc = bpy.context.scene
    sc.render.engine = "BLENDER_EEVEE_NEXT" if hasattr(bpy.types, "SceneEEVEE") else "BLENDER_EEVEE"
    sc.render.resolution_x = 960; sc.render.resolution_y = 720
    sc.render.filepath = OUT_PNG
    bpy.ops.render.render(write_still=True)
    finish("OK " + str(os.path.getsize(OUT_PNG)))
except SystemExit:
    pass
except Exception as e:
    import traceback; log(traceback.format_exc()); finish("FAIL: " + repr(e))
