# GROUNDED pose QA render — the ONLY valid way to judge an X3Native character clip.
#
#   blender --background --python pose_render_grounded.py -- <glb> <outdir> <clip:frame> [...]
#
# WHY this exists next to pose_render.py: pose_render.py frames the figure with NO
# floor and aims the camera at the body's vertical MID-POINT. That is a no-floor
# close-up, and it LIES — with no ground plane and a raised aim point there is no
# reference for where the feet actually are, so a perfectly planted pose reads as
# "floating" and a neutral spine reads as "pitched back". Two reviewers have been
# fooled by exactly that. This renderer instead uses the grounded method:
#
#   * a real FLOOR plane at world z = 0 (the character's authored ground), with a
#     checker so foot contact / penetration is unmistakable;
#   * a LEVEL camera — eye at ~1.0 m, ~3.4 m out, pitch ~0 (it looks straight ahead,
#     it does not tilt down at the figure), so vertical position is not distorted;
#   * every requested clip rendered the SAME way, so each one can be judged against
#     Idle rendered identically. Judge AFTER-vs-Idle, never a pose in isolation.
#
# Output: <outdir>/<clip>_f<frame>.png
import bpy, sys, os, math
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
glb, outdir = argv[0], argv[1]
shots = []
for spec in argv[2:]:
    name, _, fr = spec.partition(":")
    shots.append((name, int(fr) if fr else 1))

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=glb)

arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE')
if not arm.animation_data:
    arm.animation_data_create()

# ---- FLOOR at z = 0, checkered so contact/penetration is legible ----------------
bpy.ops.mesh.primitive_plane_add(size=40, location=(0, 0, 0))
floor = bpy.context.object
floor.name = "QA_Floor"
mat = bpy.data.materials.new("QA_FloorMat"); mat.use_nodes = True
nt = mat.node_tree
bsdf = nt.nodes["Principled BSDF"]
checker = nt.nodes.new("ShaderNodeTexChecker")
checker.inputs["Scale"].default_value = 40.0
checker.inputs["Color1"].default_value = (0.32, 0.33, 0.36, 1)
checker.inputs["Color2"].default_value = (0.20, 0.21, 0.24, 1)
nt.links.new(checker.outputs["Color"], bsdf.inputs["Base Color"])
bsdf.inputs["Roughness"].default_value = 0.9
floor.data.materials.append(mat)

# ---- LEVEL camera: eye ~1.0 m, ~3.4 m out, ZERO pitch --------------------------
EYE = 1.0
DIST = 3.4
AZ = math.radians(22.0)          # slight 3/4 so the pose reads; still a level cam
cam_data = bpy.data.cameras.new("Cam"); cam_data.lens = 50
cam = bpy.data.objects.new("Cam", cam_data)
bpy.context.collection.objects.link(cam)
cam.location = Vector((math.sin(AZ) * DIST, -math.cos(AZ) * DIST, EYE))
# Look horizontally: pitch is exactly 90 deg from +Z (i.e. dead level), yaw only.
cam.rotation_euler = (math.radians(90.0), 0.0, AZ)
bpy.context.scene.camera = cam

# ---- Lights --------------------------------------------------------------------
for (lx, ly, lz, e) in [(2.5, -3.0, 2.6, 600), (-3.0, -1.5, 2.0, 320), (0, 3.0, 2.2, 220)]:
    ld = bpy.data.lights.new("L", 'POINT'); ld.energy = e
    lo = bpy.data.objects.new("L", ld); lo.location = Vector((lx, ly, lz))
    bpy.context.collection.objects.link(lo)
sun = bpy.data.lights.new("Sun", 'SUN'); sun.energy = 2.5
so = bpy.data.objects.new("Sun", sun); so.rotation_euler = (math.radians(50), 0, math.radians(30))
bpy.context.collection.objects.link(so)

sc = bpy.context.scene
engines = [e.identifier for e in type(sc.render).bl_rna.properties['engine'].enum_items]
sc.render.engine = 'BLENDER_EEVEE_NEXT' if 'BLENDER_EEVEE_NEXT' in engines else 'BLENDER_EEVEE'
sc.render.resolution_x = 720
sc.render.resolution_y = 900
sc.render.film_transparent = False
sc.world = bpy.data.worlds.new("W"); sc.world.use_nodes = True
sc.world.node_tree.nodes["Background"].inputs[0].default_value = (0.07, 0.08, 0.10, 1)

os.makedirs(outdir, exist_ok=True)

# The Store-package blender-launcher.exe DETACHES (no stdout reaches the caller), so
# mirror every line into <outdir>/render.log and drop a <outdir>/render.done marker at
# the end for the wrapper to poll. Same discipline as tools/anim_build.ps1.
_log = []
def log(msg):
    print(msg)
    _log.append(msg)


def lowest_mesh_z():
    """World-space lowest point of the CHARACTER meshes (floor is at 0)."""
    deps = bpy.context.evaluated_depsgraph_get()
    lo = 1e9
    for o in bpy.data.objects:
        if o.type != 'MESH' or o.name.startswith("QA_Floor"):
            continue
        if 'icosphere' in o.name.lower():
            continue
        oe = o.evaluated_get(deps)
        for c in oe.bound_box:
            lo = min(lo, (oe.matrix_world @ Vector(c)).z)
    return lo


for name, frame in shots:
    act = bpy.data.actions.get(name)
    if act is None:
        for a in bpy.data.actions:
            if name.lower() in a.name.lower():
                act = a; break
    if act is None:
        log("GROUNDED SKIP - no action %s  have: %s"
            % (name, [a.name for a in bpy.data.actions]))
        continue
    arm.animation_data.action = act
    try:
        if getattr(act, "slots", None):
            arm.animation_data.action_slot = act.slots[0]
    except Exception as e:
        print("slot assign warn:", e)
    bpy.context.scene.frame_set(frame)
    bpy.context.view_layer.update()
    foot = lowest_mesh_z()
    out = os.path.join(outdir, "%s_f%d.png" % (name, frame))
    sc.render.filepath = out
    bpy.ops.render.render(write_still=True)
    # The number that matters: how far the lowest vertex sits off the floor plane.
    # ~0 == planted. Positive == FLOATING. Negative == sunk through the floor.
    log("GROUNDED WROTE %s  action=%s frame=%d  lowestZ=%+.4f m (0 = planted)"
        % (out, act.name, frame, foot))

log("GROUNDED DONE")
with open(os.path.join(outdir, "render.log"), "w", encoding="utf-8") as f:
    f.write("\n".join(_log))
with open(os.path.join(outdir, "render.done"), "w", encoding="utf-8") as f:
    f.write("OK")
