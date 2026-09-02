"""Render the vulva GLB for QA (file-report). Plain EEVEE, crotch front + side."""
import bpy, sys, os, math
from mathutils import Vector
ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT = ARGV[0] if ARGV else "sculpt_render.log"
OUTDIR = r"D:\GameDev\X3Native\docs\screenshots\sculpt"
os.makedirs(OUTDIR, exist_ok=True)
L = []
def e(*a):
    s = " ".join(str(x) for x in a); L.append(s); print(s)
def flush(status):
    try: open(OUT, "w", encoding="utf-8").write("\n".join(L) + "\n" + status)
    except Exception: pass

try:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    GLB = r"D:\GameDev\X3Native\assets\makehuman\female_vulva.glb"
    bpy.ops.import_scene.gltf(filepath=GLB)
    meshes = [o for o in bpy.data.objects if o.type == "MESH"]
    body = next(o for o in meshes if o.name.lower() != "cube" and len(o.data.vertices) > 1000)
    e("body:", body.name, len(body.data.vertices), "verts")
    # set FemaleGenitals shape key to 1 if it survived import
    if body.data.shape_keys:
        for kb in body.data.shape_keys.key_blocks:
            if "female" in kb.name.lower() or "genital" in kb.name.lower():
                kb.value = 1.0
                e("set shape key", kb.name, "= 1.0")

    # neutral-ish material
    if body.data.materials:
        mat = body.data.materials[0]
        if mat.node_tree and mat.node_tree.nodes.get("Principled BSDF"):
            mat.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (0.85, 0.76, 0.70, 1.0)
            mat.node_tree.nodes["Principled BSDF"].inputs["Roughness"].default_value = 0.6

    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_WORKBENCH'
    scene.render.resolution_x = 1024; scene.render.resolution_y = 1024
    scene.render.image_settings.file_format = 'PNG'

    cam_data = bpy.data.cameras.new("QA"); cam = bpy.data.objects.new("QA", cam_data)
    scene.collection.objects.link(cam); scene.camera = cam
    cam_data.lens = 60

    light_data = bpy.data.lights.new("L", 'AREA'); light = bpy.data.objects.new("L", light_data)
    scene.collection.objects.link(light); light.location = (0, -1.0, 1.2); light.data.energy = 500

    def aim(campos, lookat):
        cam.location = Vector(campos)
        d = Vector(lookat) - Vector(campos)
        cam.rotation_euler = d.to_track_quat('-Z','Y').to_euler()

    # front (vulva is on -Y side, at ~Z 0.82)
    aim((0.0, -0.45, 0.82), (0.0, -0.10, 0.82))
    scene.render.filepath = os.path.join(OUTDIR, "vulva_front.png")
    bpy.ops.render.render(write_still=True)
    e("rendered vulva_front.png")

    # 3/4 from below-front to catch the opening
    aim((0.25, -0.40, 0.70), (0.0, -0.10, 0.80))
    scene.render.filepath = os.path.join(OUTDIR, "vulva_angle.png")
    bpy.ops.render.render(write_still=True)
    e("rendered vulva_angle.png")
    flush("OK")
except Exception as ex:
    import traceback
    e("FAILED:", repr(ex)); e(traceback.format_exc()); flush("FAIL")
