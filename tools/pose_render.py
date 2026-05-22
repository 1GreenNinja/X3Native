# Render a single posed frame of a clip from a multi-clip GLB, for visual QA of
# the retarget. Loads the GLB, activates the named action, sets a frame, points a
# camera at the armature, and renders a PNG (EEVEE, solid-ish).
# Usage: blender --background --python pose_render.py -- <glb> <action> <frame> <out.png>
import bpy, sys, os, math
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
glb, action_name, frame, out = argv[0], argv[1], int(argv[2]), argv[3]

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=glb)

arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE')
act = bpy.data.actions.get(action_name)
if act is None:
    # fuzzy
    for a in bpy.data.actions:
        if action_name.lower() in a.name.lower(): act = a; break
if not arm.animation_data:
    arm.animation_data_create()
# Blender 5.0 slotted-action assignment: also set the action slot if present.
arm.animation_data.action = act
try:
    if getattr(act, "slots", None):
        arm.animation_data.action_slot = act.slots[0]
except Exception as e:
    print("slot assign warn:", e)

bpy.context.scene.frame_set(frame)
bpy.context.view_layer.update()

# Compute bounds of mesh objects (in world space) for camera framing.
mins = Vector(( 1e9,  1e9,  1e9))
maxs = Vector((-1e9, -1e9, -1e9))
deps = bpy.context.evaluated_depsgraph_get()
for o in bpy.data.objects:
    if o.type != 'MESH':
        continue
    # Skip tiny accessory meshes (e.g. an "Icosphere" eye/prop) so the framing
    # tracks the character body, not a stray accessory.
    if 'icosphere' in o.name.lower() or 'sphere' in o.name.lower():
        continue
    oe = o.evaluated_get(deps)
    for c in oe.bound_box:
        wc = oe.matrix_world @ Vector(c)
        for i in range(3):
            mins[i] = min(mins[i], wc[i]); maxs[i] = max(maxs[i], wc[i])
center = (mins + maxs) * 0.5
size = (maxs - mins)
height = max(size.z, size.y, 1.0)
print("bounds center", tuple(round(v,2) for v in center), "size", tuple(round(v,2) for v in size))

# Camera: front-ish 3/4 view, aimed at the figure's vertical mid-point so it is
# centered in frame (Z is up in the imported Y-up->Blender Z-up GLB).
cam_data = bpy.data.cameras.new("Cam"); cam = bpy.data.objects.new("Cam", cam_data)
bpy.context.collection.objects.link(cam)
aim = Vector((center.x, center.y, center.z))
dist = height * 1.9
cam.location = aim + Vector((dist*0.6, -dist, 0.0))
direction = aim - cam.location
cam.rotation_euler = direction.to_track_quat('-Z', 'Z').to_euler()
cam_data.lens = 60
bpy.context.scene.camera = cam

# Lights
for (lx, ly, lz, e) in [(3,-4,5,1500),(-4,-2,3,800),(0,4,2,500)]:
    ld = bpy.data.lights.new("L", 'POINT'); ld.energy = e
    lo = bpy.data.objects.new("L", ld); lo.location = center + Vector((lx,ly,lz))
    bpy.context.collection.objects.link(lo)
sun = bpy.data.lights.new("Sun", 'SUN'); sun.energy = 3
so = bpy.data.objects.new("Sun", sun); so.rotation_euler = (math.radians(50),0,math.radians(30))
bpy.context.collection.objects.link(so)

sc = bpy.context.scene
sc.render.engine = 'BLENDER_EEVEE_NEXT' if 'BLENDER_EEVEE_NEXT' in [e.identifier for e in type(sc.render).bl_rna.properties['engine'].enum_items] else 'BLENDER_EEVEE'
sc.render.resolution_x = 720
sc.render.resolution_y = 960
sc.render.film_transparent = False
sc.world = bpy.data.worlds.new("W")
sc.world.use_nodes = True
sc.world.node_tree.nodes["Background"].inputs[0].default_value = (0.05,0.06,0.08,1)
sc.render.filepath = out
os.makedirs(os.path.dirname(out), exist_ok=True)
bpy.ops.render.render(write_still=True)
print("POSE RENDER WROTE", out, "action=", act.name if act else None, "frame=", frame)
