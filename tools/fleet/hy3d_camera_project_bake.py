"""v2: fixes from the first failed pass.
- Voxel remesh BEFORE decimate (preserves the shallow dome curvature; blind
  110x collapse-decimate turned the dome into a faceted cone).
- Glass-dome mask: central-radius + relative-height, not a flat Z threshold
  (the saucer is a lens shape - height alone can't isolate just the bump).
- Real camera projection via the UVProject MODIFIER (generates actual UV
  coords from a camera's view, headless-safe) instead of Window/Camera
  texture-coordinate sockets, which are undefined during a Cycles bake
  (no active camera frustum exists mid-bake -> that's why v1 baked flat grey).
"""
import bpy, os, math
from mathutils import Vector
import numpy as np

SCRATCH = os.path.dirname(os.path.abspath(__file__))
SRC = r"G:\ComfyUI\output\saucer_ufo1_00001_.glb"
TEXTURE_SRC = os.path.join(SCRATCH, "ufo2.png")
OUT_GLB = r"G:\X3Native\assets\rigged_glb\saucer_ufo1_skyhull.glb"

for obj in list(bpy.data.objects):
    bpy.data.objects.remove(obj, do_unlink=True)
bpy.ops.import_scene.gltf(filepath=SRC)
mesh = next(o for o in bpy.data.objects if o.type == 'MESH')
bpy.context.view_layer.objects.active = mesh
mesh.select_set(True)

# ---- bounding box for voxel size + dome-detection thresholds ----
verts = mesh.data.vertices
xs = [v.co.x for v in verts]; ys = [v.co.y for v in verts]; zs = [v.co.z for v in verts]
diag = math.dist((min(xs), min(ys), min(zs)), (max(xs), max(ys), max(zs)))
zmin, zmax = min(zs), max(zs)
max_radius = max(math.hypot(v.co.x, v.co.y) for v in verts)
print(f"diag={diag:.3f} z=[{zmin:.3f},{zmax:.3f}] max_radius={max_radius:.3f}")

# ---- voxel remesh: clean uniform topology that preserves the dome bump ----
remesh = mesh.modifiers.new("remesh", 'REMESH')
remesh.mode = 'VOXEL'
remesh.voxel_size = diag / 160.0
remesh.adaptivity = 0.0
bpy.ops.object.modifier_apply(modifier=remesh.name)
tris_remesh = sum(len(p.vertices) - 2 for p in mesh.data.polygons)
print(f"after voxel remesh: {tris_remesh} tris")

# gentle decimate only if still heavy (background sky object budget ~15k tris)
TARGET_TRIS = 15000
if tris_remesh > TARGET_TRIS:
    dec = mesh.modifiers.new("decimate", 'DECIMATE')
    dec.ratio = TARGET_TRIS / tris_remesh
    bpy.ops.object.modifier_apply(modifier=dec.name)
tris_after = sum(len(p.vertices) - 2 for p in mesh.data.polygons)
print(f"final: {tris_after} tris")

# ---- glass-dome mask: central radius (top-down) AND high relative-Z among
#      that central column - isolates the raised bump, not the whole lens ----
verts = mesh.data.vertices
central_r = max_radius * 0.32
central_zs = [v.co.z for v in verts if math.hypot(v.co.x, v.co.y) < central_r]
if central_zs:
    z_dome_thresh = np.percentile(central_zs, 55)
else:
    z_dome_thresh = zmax * 0.5
vg_glass = mesh.vertex_groups.new(name="glass_dome")
n_glass = 0
for v in verts:
    r = math.hypot(v.co.x, v.co.y)
    if r < central_r and v.co.z > z_dome_thresh:
        vg_glass.add([v.index], 1.0, 'REPLACE')
        n_glass += 1
print(f"dome/glass verts: {n_glass} (r<{central_r:.3f}, z>{z_dome_thresh:.3f})")

# ---- bake-target UV (atlas layout) ----
bpy.ops.object.mode_set(mode='EDIT')
bpy.ops.mesh.select_all(action='SELECT')
bpy.ops.uv.smart_project(angle_limit=math.radians(66), island_margin=0.02)
bpy.ops.object.mode_set(mode='OBJECT')
mesh.data.uv_layers[0].name = "atlas"

# ---- projection UV via UVProject modifier (real camera-space UVs, headless-safe) ----
cam_data = bpy.data.cameras.new("proj_cam")
cam_data.lens = 55
proj_cam = bpy.data.objects.new("proj_cam", cam_data)
bpy.context.collection.objects.link(proj_cam)
proj_cam.location = (2.6, -2.9, 0.15)
d = Vector((0, 0, 0.0)) - proj_cam.location
proj_cam.rotation_euler = d.to_track_quat('-Z', 'Y').to_euler()
bpy.context.view_layer.update()

proj_uv = mesh.data.uv_layers.new(name="project")
mesh.data.uv_layers.active = proj_uv
uvproj = mesh.modifiers.new("uvproject", 'UV_PROJECT')
uvproj.uv_layer = "project"
uvproj.projector_count = 1
uvproj.projectors[0].object = proj_cam
uvproj.aspect_x = 1.0
uvproj.aspect_y = 1.0
bpy.ops.object.modifier_apply(modifier=uvproj.name)
mesh.data.uv_layers.active = mesh.data.uv_layers["atlas"]

# ---- bake material: sample ufo2.png via the "project" UV, output to "atlas" ----
img = bpy.data.images.load(TEXTURE_SRC)
img.colorspace_settings.name = 'sRGB'

mat = bpy.data.materials.new("hull_bake")
mesh.data.materials.clear()
mesh.data.materials.append(mat)
mat.use_nodes = True
nt = mat.node_tree
nt.nodes.clear()

out = nt.nodes.new('ShaderNodeOutputMaterial'); out.location = (900, 0)
bsdf = nt.nodes.new('ShaderNodeBsdfPrincipled'); bsdf.location = (600, 0)
nt.links.new(bsdf.outputs['BSDF'], out.inputs['Surface'])

uvmap_proj = nt.nodes.new('ShaderNodeUVMap'); uvmap_proj.uv_map = "project"; uvmap_proj.location = (-350, 200)
teximg = nt.nodes.new('ShaderNodeTexImage'); teximg.image = img; teximg.location = (-100, 200)
teximg.extension = 'CLIP'
nt.links.new(uvmap_proj.outputs['UV'], teximg.inputs['Vector'])

vgw = nt.nodes.new('ShaderNodeAttribute'); vgw.attribute_name = 'glass_dome'; vgw.attribute_type = 'GEOMETRY'; vgw.location = (-350, -300)

basec = nt.nodes.new('ShaderNodeMix'); basec.data_type = 'RGBA'; basec.location = (200, 100)
basec.inputs[6].default_value = (0.58, 0.6, 0.63, 1)
nt.links.new(teximg.outputs['Color'], basec.inputs[7])
nt.links.new(teximg.outputs['Alpha'], basec.inputs['Factor'])
nt.links.new(basec.outputs[2], bsdf.inputs['Base Color'])

metal = nt.nodes.new('ShaderNodeMix'); metal.data_type = 'FLOAT'; metal.location = (200, -150)
metal.inputs[2].default_value = 0.65; metal.inputs[3].default_value = 0.05
nt.links.new(vgw.outputs['Fac'], metal.inputs['Factor']); nt.links.new(metal.outputs[0], bsdf.inputs['Metallic'])

rough = nt.nodes.new('ShaderNodeMix'); rough.data_type = 'FLOAT'; rough.location = (200, -300)
rough.inputs[2].default_value = 0.28; rough.inputs[3].default_value = 0.05
nt.links.new(vgw.outputs['Fac'], rough.inputs['Factor']); nt.links.new(rough.outputs[0], bsdf.inputs['Roughness'])

transmit = nt.nodes.new('ShaderNodeMix'); transmit.data_type = 'FLOAT'; transmit.location = (200, -450)
transmit.inputs[2].default_value = 0.0; transmit.inputs[3].default_value = 0.92
nt.links.new(vgw.outputs['Fac'], transmit.inputs['Factor']); nt.links.new(transmit.outputs[0], bsdf.inputs['Transmission Weight'])
bsdf.inputs['IOR'].default_value = 1.45

# ---- bake albedo into the atlas UV ----
scene = bpy.context.scene
scene.render.engine = 'CYCLES'
prefs = bpy.context.preferences.addons['cycles'].preferences
for dtype in ('OPTIX', 'CUDA'):
    try:
        prefs.compute_device_type = dtype; prefs.get_devices()
        for d in prefs.devices: d.use = True
        scene.cycles.device = 'GPU'
        break
    except Exception:
        pass
scene.cycles.samples = 16
scene.render.bake.margin = 8

bake_img = bpy.data.images.new("saucer_albedo", 1024, 1024, alpha=False)
bake_img.colorspace_settings.name = 'sRGB'
tgt = nt.nodes.new('ShaderNodeTexImage'); tgt.image = bake_img; tgt.location = (200, 400)
for nd in nt.nodes: nd.select = False
tgt.select = True; nt.nodes.active = tgt

mesh.data.uv_layers.active = mesh.data.uv_layers["atlas"]
bpy.ops.object.select_all(action='DESELECT')
mesh.select_set(True)
bpy.context.view_layer.objects.active = mesh
bpy.ops.object.bake(type='DIFFUSE', pass_filter={'COLOR'})
bake_img.filepath_raw = os.path.join(SCRATCH, "saucer_albedo_v2.png")
bake_img.file_format = 'PNG'
bake_img.save()
print("baked albedo v2")

# ---- rebuild clean export material sampling the baked atlas ----
nt.nodes.clear()
out2 = nt.nodes.new('ShaderNodeOutputMaterial'); out2.location = (500, 0)
bsdf2 = nt.nodes.new('ShaderNodeBsdfPrincipled'); bsdf2.location = (200, 0)
nt.links.new(bsdf2.outputs['BSDF'], out2.inputs['Surface'])
talb = nt.nodes.new('ShaderNodeTexImage'); talb.image = bake_img; talb.location = (-150, 200)
nt.links.new(talb.outputs['Color'], bsdf2.inputs['Base Color'])
vgw2 = nt.nodes.new('ShaderNodeAttribute'); vgw2.attribute_name = 'glass_dome'; vgw2.attribute_type = 'GEOMETRY'; vgw2.location = (-150, -200)
metal2 = nt.nodes.new('ShaderNodeMix'); metal2.data_type = 'FLOAT'; metal2.location = (0, -100)
metal2.inputs[2].default_value = 0.65; metal2.inputs[3].default_value = 0.05
nt.links.new(vgw2.outputs['Fac'], metal2.inputs['Factor']); nt.links.new(metal2.outputs[0], bsdf2.inputs['Metallic'])
rough2 = nt.nodes.new('ShaderNodeMix'); rough2.data_type = 'FLOAT'; rough2.location = (0, -250)
rough2.inputs[2].default_value = 0.28; rough2.inputs[3].default_value = 0.05
nt.links.new(vgw2.outputs['Fac'], rough2.inputs['Factor']); nt.links.new(rough2.outputs[0], bsdf2.inputs['Roughness'])
trans2 = nt.nodes.new('ShaderNodeMix'); trans2.data_type = 'FLOAT'; trans2.location = (0, -400)
trans2.inputs[2].default_value = 0.0; trans2.inputs[3].default_value = 0.92
nt.links.new(vgw2.outputs['Fac'], trans2.inputs['Factor']); nt.links.new(trans2.outputs[0], bsdf2.inputs['Transmission Weight'])
bsdf2.inputs['IOR'].default_value = 1.45

# drop the projection UV (baked already; keep atlas as the only export UV)
mesh.data.uv_layers.remove(mesh.data.uv_layers["project"])
bpy.data.objects.remove(proj_cam, do_unlink=True)
bpy.data.images.remove(img)

bpy.ops.export_scene.gltf(filepath=OUT_GLB, export_format='GLB')
kb = os.path.getsize(OUT_GLB) // 1024
print(f"EXPORTED {OUT_GLB} ({kb} KB, {tris_after} tris)")
