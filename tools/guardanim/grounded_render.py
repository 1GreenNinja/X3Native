# Grounded QA render for retargeted clips — the repo's HARD RULE for judging
# character animation (see memory: no-floor close-ups falsely show floating /
# back-pitch). Renders the named action at a given frame with:
#   * a floor plane at z=0 (world ground truth), checkerboard so contact reads
#   * a LEVEL camera at hip height (no downward tilt to fake grounding)
# Usage:
#   blender-launcher --background --python grounded_render.py -- \
#       <glb> <action> <frame> <out.png>
# Writes <out.png> and a <out.png>.done marker (Store-Blender detaches).
import bpy, sys, os, math
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
glb, action_name, frame, out = argv[0], argv[1], int(argv[2]), argv[3]

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=glb)

arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE')
act = bpy.data.actions.get(action_name)
if act is None:
    for a in bpy.data.actions:
        if action_name.lower() in a.name.lower():
            act = a
            break
if not arm.animation_data:
    arm.animation_data_create()
arm.animation_data.action = act
try:
    if getattr(act, "slots", None):
        arm.animation_data.action_slot = act.slots[0]
except Exception as e:
    print("slot assign warn:", e)

bpy.context.scene.frame_set(frame)
bpy.context.view_layer.update()

# Bounds of the character meshes (world space; glTF Y-up imports as Blender Z-up).
mins = Vector((1e9, 1e9, 1e9)); maxs = Vector((-1e9, -1e9, -1e9))
deps = bpy.context.evaluated_depsgraph_get()
for o in bpy.data.objects:
    if o.type != 'MESH' or 'sphere' in o.name.lower():
        continue
    oe = o.evaluated_get(deps)
    for c in oe.bound_box:
        wc = oe.matrix_world @ Vector(c)
        for i in range(3):
            mins[i] = min(mins[i], wc[i]); maxs[i] = max(maxs[i], wc[i])
center = (mins + maxs) * 0.5
height = max(maxs.z - mins.z, 1.0)
print("bounds z=[%.3f, %.3f] (floor gap %.3f m)" % (mins.z, maxs.z, mins.z))

# FLOOR at z=0 — checker material so foot contact is legible.
bpy.ops.mesh.primitive_plane_add(size=30, location=(center.x, center.y, 0.0))
floor = bpy.context.active_object
fm = bpy.data.materials.new("Floor"); fm.use_nodes = True
nt = fm.node_tree
checker = nt.nodes.new("ShaderNodeTexChecker")
checker.inputs["Scale"].default_value = 30.0
checker.inputs["Color1"].default_value = (0.25, 0.25, 0.27, 1)
checker.inputs["Color2"].default_value = (0.6, 0.6, 0.62, 1)
bsdf = nt.nodes["Principled BSDF"]
nt.links.new(checker.outputs["Color"], bsdf.inputs["Base Color"])
floor.data.materials.append(fm)

# LEVEL camera: hip height, HORIZONTAL view axis (pitch 0 — the grounded rule).
# Explicit Euler setup — rotation (90deg, 0, yaw) is a level camera BY
# CONSTRUCTION (roll 0, horizon horizontal); track-quat proved roll-prone.
cam_data = bpy.data.cameras.new("Cam"); cam = bpy.data.objects.new("Cam", cam_data)
bpy.context.collection.objects.link(cam)
eye_z = height * 0.5                     # hip/mid height
dist = height * 2.6
yaw = math.radians(30.0)                 # 3/4 view
# With rotation (90,0,yaw) the camera forward (-Z) is (-sin yaw, cos yaw, 0);
# place it so that forward points at the subject.
cam.location = Vector((center.x + dist * math.sin(yaw),
                       center.y - dist * math.cos(yaw), eye_z))
cam.rotation_euler = (math.radians(90.0), 0.0, yaw)
cam_data.lens = 50
bpy.context.scene.camera = cam

for (lx, ly, lz, e) in [(3, -4, 5, 1500), (-4, -2, 3, 800), (0, 4, 2, 500)]:
    ld = bpy.data.lights.new("L", 'POINT'); ld.energy = e
    lo = bpy.data.objects.new("L", ld)
    lo.location = Vector((center.x + lx, center.y + ly, lz))
    bpy.context.collection.objects.link(lo)
sun = bpy.data.lights.new("Sun", 'SUN'); sun.energy = 3
so = bpy.data.objects.new("Sun", sun)
so.rotation_euler = (math.radians(50), 0, math.radians(30))
bpy.context.collection.objects.link(so)

sc = bpy.context.scene
engines = [e.identifier for e in type(sc.render).bl_rna.properties['engine'].enum_items]
sc.render.engine = 'BLENDER_EEVEE_NEXT' if 'BLENDER_EEVEE_NEXT' in engines else 'BLENDER_EEVEE'
sc.render.resolution_x = 960
sc.render.resolution_y = 720
sc.world = bpy.data.worlds.new("W")
sc.world.use_nodes = True
sc.world.node_tree.nodes["Background"].inputs[0].default_value = (0.05, 0.06, 0.08, 1)
sc.render.filepath = out
os.makedirs(os.path.dirname(out), exist_ok=True)
bpy.ops.render.render(write_still=True)
print("GROUNDED RENDER WROTE", out, "action=", act.name if act else None,
      "frame=", frame, "floor_gap=%.3f" % mins.z)
with open(out + ".done", "w") as f:
    f.write("OK")
