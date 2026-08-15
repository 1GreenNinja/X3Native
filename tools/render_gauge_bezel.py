"""Render a GLOSSY 3D gauge bezel in Blender (headless) with alpha.

Owner: "I WANT A GLOSSY 3D BEZEL WITH METAL AND GRADIENTS!!!!"

Fair. Fake gloss painted in 2D never looks like metal, because metal IS the
reflection — you need real geometry, a real light rig and a real shading model.
Blender gives all three, and this box has it at D:\\GameDev\\tools\\Blender.

Renders straight down the Z axis, orthographic, transparent film, so the result
drops into the HUD as a plain RGBA sprite. The dial FACE (ticks, numerals,
redline) is composited in 2D afterwards by make_gauge_textures.py — vector text
is crisper drawn than rendered, and it means restyling the scale never needs a
re-render.

Run:
  D:\\GameDev\\tools\\Blender\\blender.exe -b -P tools/render_gauge_bezel.py
Output:
  assets/ui/gauge_bezel.png   1024^2 RGBA
"""

import bpy
import os
import math
from mathutils import Vector

OUT = os.path.join(os.path.dirname(os.path.abspath(bpy.data.filepath or __file__)),
                   "..", "assets", "ui", "gauge_bezel.png")
OUT = os.path.normpath(os.path.join(os.getcwd(), "assets", "ui", "gauge_bezel.png"))

RES = 1024
R_OUTER = 1.00     # bezel outer radius (world units; camera framed to this)
R_INNER = 0.855    # where the dial face begins


def clear():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def metal(name, base, rough, metallic=1.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    b = m.node_tree.nodes["Principled BSDF"]
    b.inputs["Base Color"].default_value = (*base, 1.0)
    b.inputs["Metallic"].default_value = metallic
    b.inputs["Roughness"].default_value = rough
    return m


def build():
    clear()

    # ---- BEZEL: a torus-ish ring made from a bevelled circle, so it catches a
    # real highlight along its crown instead of looking like a flat annulus.
    bpy.ops.mesh.primitive_torus_add(major_radius=(R_OUTER + R_INNER) / 2,
                                     minor_radius=(R_OUTER - R_INNER) / 2,
                                     major_segments=256, minor_segments=64,
                                     location=(0, 0, 0))
    ring = bpy.context.object
    ring.scale = (1, 1, 0.62)                     # squash so it reads as a rim, not a donut
    ring.data.materials.append(metal("BezelMetal", (0.34, 0.36, 0.40), 0.13))
    bpy.ops.object.shade_smooth()

    # ---- a thin darker inner lip, the step down to the face
    bpy.ops.mesh.primitive_torus_add(major_radius=R_INNER * 0.985,
                                     minor_radius=R_INNER * 0.030,
                                     major_segments=256, minor_segments=32)
    lip = bpy.context.object
    lip.scale = (1, 1, 0.7)
    lip.data.materials.append(metal("Lip", (0.06, 0.065, 0.08), 0.30))
    bpy.ops.object.shade_smooth()

    # ---- FACE: a slightly domed disc so the gradient across it is REAL
    # (curvature + light), not a painted vignette.
    bpy.ops.mesh.primitive_circle_add(vertices=256, radius=R_INNER * 0.985, fill_type='NGON')
    face = bpy.context.object
    face.location.z = -0.10
    fm = bpy.data.materials.new("Face")
    fm.use_nodes = True
    fb = fm.node_tree.nodes["Principled BSDF"]
    fb.inputs["Base Color"].default_value = (0.006, 0.007, 0.010, 1.0)
    fb.inputs["Metallic"].default_value = 0.0
    fb.inputs["Roughness"].default_value = 0.55
    face.data.materials.append(fm)

    # NOTE: an earlier version put a clear dome over the face for "gloss". It
    # read as a petri dish and mirrored the area lights as hard rectangles. On a
    # real instrument the gloss you notice is the BEZEL's specular, not a lens —
    # so the shine now comes from the ring's own curvature and roughness.

    # ---- LIGHTS: a large key from upper-left (the classic instrument look), a
    # cool rim from lower-right, and a broad fill so the metal has somewhere to
    # reflect. Area lights, because metal needs shaped reflections.
    def area(name, loc, rot, size, energy, color):
        d = bpy.data.lights.new(name, 'AREA')
        d.size = size
        d.energy = energy
        d.color = color
        o = bpy.data.objects.new(name, d)
        o.location = loc
        o.rotation_euler = rot
        bpy.context.collection.objects.link(o)
        return o

    area("Key",  (-3.2, 3.4, 4.6), (math.radians(-38), math.radians(-26), 0), 14.0, 1500, (1.0, 0.98, 0.95))
    area("Rim",  (3.4, -2.8, 2.4), (math.radians(58), math.radians(30), 0),  11.0, 700, (0.45, 0.68, 1.0))
    area("Fill", (0.0, 0.0, 6.0),  (0, 0, 0),                                18.0, 120, (0.80, 0.86, 1.0))

    # dim ambient world: metal with nothing to reflect just goes black
    w = bpy.data.worlds.new("W"); w.use_nodes = True
    w.node_tree.nodes["Background"].inputs[0].default_value = (0.035, 0.040, 0.050, 1.0)
    w.node_tree.nodes["Background"].inputs[1].default_value = 1.0
    bpy.context.scene.world = w

    # ---- CAMERA: orthographic, straight down, framed exactly to the bezel
    cam_d = bpy.data.cameras.new("Cam")
    cam_d.type = 'ORTHO'
    cam_d.ortho_scale = R_OUTER * 2.06
    cam = bpy.data.objects.new("Cam", cam_d)
    cam.location = (0, 0, 6)
    bpy.context.collection.objects.link(cam)
    bpy.context.scene.camera = cam

    sc = bpy.context.scene
    sc.render.engine = 'CYCLES'
    try:
        sc.cycles.device = 'GPU'
        prefs = bpy.context.preferences.addons['cycles'].preferences
        prefs.compute_device_type = 'OPTIX'
        prefs.get_devices()
        for d in prefs.devices:
            d.use = True
    except Exception as e:
        print("GPU setup skipped:", e)
    sc.cycles.samples = 256
    sc.cycles.use_denoising = True
    sc.render.film_transparent = True
    sc.render.resolution_x = RES
    sc.render.resolution_y = RES
    sc.render.image_settings.file_format = 'PNG'
    sc.render.image_settings.color_mode = 'RGBA'
    sc.render.filepath = OUT


def main():
    build()
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    bpy.ops.render.render(write_still=True)
    print("WROTE", OUT)


main()
