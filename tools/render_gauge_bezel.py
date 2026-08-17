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
  assets/ui/gauge_bezel.png   2048^2 RGBA

QUALITY PASS (owner: the cluster should look like "the quality that a game set
30 years after NFS should look like"). What changed and why:
  * 1024 -> 2048, 256 -> 512 samples. The tach draws at 2R = 0.30 * screen
    height, which on a 4K panel is ~648 px of a 1024 source: the rim was being
    magnified, and a magnified highlight is a soft smear. This is the "higher-res
    bake" half of the ask.
  * A SECOND, machined step inboard of the crown. One smooth torus reads as a
    plastic ring; real instrument bezels are turned, and the eye reads the extra
    parallel highlight as precision. This is the single biggest change here.
  * The face is darker (0.006 -> 0.0035) and rougher (0.55 -> 0.66) — X3_WORLD_
    RULES rule 7 wants a near-black inset pane, and rule 5's anti-glare display
    glass is matte so lights give a sheen and not a hot orb over the numerals.
  * A cool low-energy bounce under the rim so the bottom of the ring is not a
    dead black arc (metal with nothing to reflect goes black — rule 5 again).
"""

import bpy
import os
import math
from mathutils import Vector

OUT = os.path.join(os.path.dirname(os.path.abspath(bpy.data.filepath or __file__)),
                   "..", "assets", "ui", "gauge_bezel.png")
OUT = os.path.normpath(os.path.join(os.getcwd(), "assets", "ui", "gauge_bezel.png"))

RES = 2048
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
                                     major_segments=384, minor_segments=96,
                                     location=(0, 0, 0))
    ring = bpy.context.object
    ring.scale = (1, 1, 0.62)                     # squash so it reads as a rim, not a donut
    ring.data.materials.append(metal("BezelMetal", (0.34, 0.36, 0.40), 0.13))
    bpy.ops.object.shade_smooth()

    # ---- MACHINED STEP: a second, narrower turned ring just inboard of the
    # crown. One smooth torus reads as a moulded plastic ring; a real instrument
    # bezel is TURNED, and what sells that is a second parallel highlight
    # running around it. Slightly brighter and smoother than the crown so it
    # catches the key light as a distinct hairline rather than a wash.
    bpy.ops.mesh.primitive_torus_add(major_radius=R_INNER * 1.045,
                                     minor_radius=(R_OUTER - R_INNER) * 0.20,
                                     major_segments=384, minor_segments=48)
    step = bpy.context.object
    step.scale = (1, 1, 0.55)
    step.location.z = 0.028
    step.data.materials.append(metal("BezelStep", (0.52, 0.55, 0.60), 0.085))
    bpy.ops.object.shade_smooth()

    # ---- a thin darker inner lip, the step down to the face
    bpy.ops.mesh.primitive_torus_add(major_radius=R_INNER * 0.985,
                                     minor_radius=R_INNER * 0.030,
                                     major_segments=384, minor_segments=32)
    lip = bpy.context.object
    lip.scale = (1, 1, 0.7)
    lip.data.materials.append(metal("Lip", (0.05, 0.055, 0.07), 0.26))
    bpy.ops.object.shade_smooth()

    # ---- FACE: a slightly domed disc so the gradient across it is REAL
    # (curvature + light), not a painted vignette.
    bpy.ops.mesh.primitive_circle_add(vertices=384, radius=R_INNER * 0.985, fill_type='NGON')
    face = bpy.context.object
    face.location.z = -0.10
    fm = bpy.data.materials.new("Face")
    fm.use_nodes = True
    fb = fm.node_tree.nodes["Principled BSDF"]
    fb.inputs["Base Color"].default_value = (0.0035, 0.0040, 0.0058, 1.0)
    fb.inputs["Metallic"].default_value = 0.0
    fb.inputs["Roughness"].default_value = 0.66   # anti-glare matte (rule 5)
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
    # BOUNCE from below: without it the bottom of the crown has nothing to
    # reflect and goes black (rule 5's "no untextured full-metal" failure mode,
    # arrived at from the other direction — the metal IS textured, it just has
    # an empty hemisphere under it). Weak and cool, so it reads as spill.
    area("Bounce", (0.6, -3.0, -3.4), (math.radians(140), 0, 0),               16.0, 260, (0.55, 0.68, 0.92))

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
    sc.cycles.samples = 512
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
