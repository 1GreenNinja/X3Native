"""
build_rifthub_gate.py — ROUND 5: the RIFTHUB gate, authored in Blender as
ROUNDED INDUSTRIAL PIPEWORK (owner's directive, verbatim):

    "Your center is perfect. ENLARGEN IT. Just MAKE a model in Blender,
     ROUNDED PIPE, with TEXTURES FROM SD3.5 bASED On Groks Image"
    "make the surrounding industrial gray metal electronic unit look like an
     industrial portal generator! Not basic squared off shapes!!!"

    blender-launcher.exe --background --python tools/build_rifthub_gate.py -- <out.glb>

WHY THIS IS A REWRITE, NOT A TWEAK:

 1. VOCABULARY. The round-3/4 gate was swept RECTANGULAR profiles — flat square
    slabs glued in a circle. This one is built from ROUND primitives only: a
    swept circular-section TORUS is the ring body, and everything bolted to it
    is a cylinder, a torus collar, a sphere joint, or a round-bevelled pipe
    curve. Rounded surfaces carry a specular streak ALONG their length, and that
    streak is what makes metal read as machined metal instead of cardboard.
    Boxes appear exactly once (heat-sink fins) and even those are bevelled.

 2. NORMALS — THE BUG THAT ATE FOUR ART ROUNDS. The old hand-rolled sweep()
    emitted 30-43% of its hub-facing triangles INSIDE-OUT (measured off the
    shipped GLB: normals inverted AND wound backwards, so they were also
    backface-culled). That is the real "ghost glass / X-ray" the last round
    chased into the SSR code, and it is why the gate's entire front annulus
    rendered BLACK no matter how much light was thrown at it — the round-3/4
    gate only ever "read" at all because it was faking it with a fake
    self-emissive. Every shape here comes from a watertight bpy primitive (or a
    bevelled curve), and normals_outward() additionally verifies each object's
    SIGNED VOLUME is positive and flips it otherwise. The gate is now provably
    outward-facing, so it can finally take a light.

 3. SHADING. shade_smooth + an EDGE SPLIT at 40 deg: round surfaces stay smooth
    (so a tube takes a highlight along its length), hard machined edges stay crisp.

 4. THE BORE IS BIGGER ("ENLARGEN IT"). The ring is a torus of centerline
    R=2.35 / tube r=0.45, so its throat — a ROUNDED throat, not a square barrel —
    opens at r=1.90. The engine's membrane grows to R=1.895 (1.58 in round 4,
    1.655 after the two-sided fix): +20% radius / +44% area, filling the opening.

LOCAL SPACE CONTRACT (must match rifthub.cpp's portal basis) — unchanged:
  the gate stands in the local XY plane, +Y = 12 o'clock, hole axis = +Z, FRONT
  (hub-facing, where the engine's amber chevron slits + ratchet track sit proud)
  = -Z. Ring center at the origin; the engine translates it to (cx, kRingY=2.2,
  cz), so the floor is local y = -2.2. We author in that frame, then rotate +90
  deg about X so Blender's Y-up glTF exporter lands it back in the contract frame.

ENGINE ANCHORS HONOURED (rifthub.cpp round-5 constants):
  membrane R=1.895, fresnel rim R=1.868 -> the torus throat at 1.90 clears both;
  amber ratchet track segs at r=2.02, front face z=-0.461 -> a rounded TRACK BED
  collar is seated with its front face at z=-0.436, so the segs sit 0.025 proud;
  9 chevron slits at r=2.35, z=-0.762 -> the clamp housings' front caps land at
  z=-0.750, so each slit floats 0.012 proud of its housing (the powered-lock read).
  9 clamps at th = 90 - c*40 deg (chevron 0 at 12 o'clock, clockwise).

ENV NOTE (this box): Store-Blender — blender.exe is ACL-denied, blender-launcher
DETACHES (no stdout); we report through `<out>.log` + `<out>.done` (poll them).
Driver: tools/gate_build.ps1 (keep it ASCII-only).

Clean-room: public Blender Python API + glTF 2.0 spec only. All-original
procedural authorship — no third-party gate models consulted or copied.
"""
import bpy, bmesh, math, os, random, sys
from mathutils import Vector, Matrix

ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT = ARGV[0] if ARGV else os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                         "..", "assets", "converted_glb", "rifthub",
                                         "gate_ring.glb")
OUT = os.path.abspath(OUT)
LOG_PATH, DONE_PATH = OUT + ".log", OUT + ".done"

_log = []
def log(*a):
    s = "[gatebuild] " + " ".join(str(x) for x in a)
    _log.append(s)
    print(s)

def flush_log(status):
    try:
        with open(LOG_PATH, "w", encoding="utf-8") as f:
            f.write("\n".join(_log))
        with open(DONE_PATH, "w", encoding="utf-8") as f:
            f.write(status)
    except Exception:
        pass

TAU = math.pi * 2.0
def D(deg):
    return math.radians(deg)
R = random.Random(90210)

# ---- geometry constants (the local contract frame) --------------------------
RING_R      = 2.35     # torus centerline radius
RING_TUBE   = 0.45     # tube radius -> throat opens at 1.90, rim reaches 2.80
BORE        = RING_R - RING_TUBE          # 1.90 (the rounded throat)
TRACK_R     = 2.02     # the engine's amber ratchet segs ride here
CHEV_R      = 2.35     # the engine's amber chevron slits ride here (tube crest)
CHEV_CAP_Z  = -0.750   # clamp housing front-cap plane (slits float 0.012 proud)
FLOOR_Y     = -2.20    # world y=0 in the local frame

GROUPS = {"patina": [], "steel": [], "dark": []}
def register(ob, group):
    GROUPS[group].append(ob)
    return ob

# ---------------------------------------------------------------------------
# Primitive helpers. EVERY shape is a watertight bpy primitive or a bevelled
# curve -> correct winding + outward normals by construction (the round-4 bug).
# ---------------------------------------------------------------------------
def _apply(ob, name):
    bpy.ops.object.select_all(action='DESELECT')
    ob.select_set(True)
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.modifier_apply(modifier=name)

def _finish(ob, bev=0.0):
    if bev > 0.0:
        md = ob.modifiers.new("bev", 'BEVEL')
        md.width = bev
        md.segments = 2
        md.limit_method = 'ANGLE'
        md.angle_limit = D(40)
        _apply(ob, md.name)
    bpy.ops.object.select_all(action='DESELECT')
    ob.select_set(True)
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.shade_smooth()
    # EDGE SPLIT: curved surfaces stay smooth, hard machined edges stay crisp.
    md = ob.modifiers.new("es", 'EDGE_SPLIT')
    md.split_angle = D(40)
    _apply(ob, md.name)
    return ob

def torus(name, major, minor, loc=(0, 0, 0), rot=(0, 0, 0), mseg=64, nseg=20,
          zscale=1.0):
    bpy.ops.mesh.primitive_torus_add(major_radius=major, minor_radius=minor,
                                     major_segments=mseg, minor_segments=nseg,
                                     location=loc, rotation=rot)
    ob = bpy.context.active_object
    ob.name = name
    if zscale != 1.0:                     # flattened donut = a rounded COLLAR
        ob.scale = (1.0, 1.0, zscale)
        bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    return _finish(ob)

def cyl(name, r, depth, loc, rot=(0, 0, 0), verts=20, bev=0.014):
    bpy.ops.mesh.primitive_cylinder_add(vertices=verts, radius=r, depth=depth,
                                        location=loc, rotation=rot)
    ob = bpy.context.active_object
    ob.name = name
    return _finish(ob, bev)

def ball(name, r, loc, seg=16):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=seg, ring_count=max(6, seg // 2),
                                         radius=r, location=loc)
    ob = bpy.context.active_object
    ob.name = name
    return _finish(ob)

def cube(name, hx, hy, hz, loc, rot=(0, 0, 0), bev=0.03):
    bpy.ops.mesh.primitive_cube_add(size=2.0, location=loc, rotation=rot)
    ob = bpy.context.active_object
    ob.name = name
    ob.scale = (hx, hy, hz)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    return _finish(ob, bev)

def pipe(name, pts, radius, res=6):
    """A round-bevelled PIPE through `pts` (the owner's 'ROUNDED PIPE'): a poly
    curve with a circular bevel, converted to a watertight capped mesh."""
    cu = bpy.data.curves.new(name, 'CURVE')
    cu.dimensions = '3D'
    cu.bevel_depth = radius
    cu.bevel_resolution = res
    cu.use_fill_caps = True
    sp = cu.splines.new('POLY')
    sp.points.add(len(pts) - 1)
    for i, p in enumerate(pts):
        sp.points[i].co = (p[0], p[1], p[2], 1.0)
    ob = bpy.data.objects.new(name, cu)
    bpy.context.collection.objects.link(ob)
    bpy.ops.object.select_all(action='DESELECT')
    ob.select_set(True)
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.convert(target='MESH')
    return _finish(bpy.context.active_object)

def arc_pts(r, a0, a1, z, n=16):
    return [(r * math.cos(a0 + (a1 - a0) * i / n),
             r * math.sin(a0 + (a1 - a0) * i / n), z) for i in range(n + 1)]

def radial(th, r, z):
    return (r * math.cos(th), r * math.sin(th), z)

def aim(vec):
    """Euler that points a Z-axis primitive (cylinder) along `vec`."""
    return Vector(vec).normalized().to_track_quat('Z', 'Y').to_euler()

def clamp_angles():
    return [D(90) - c * D(40) for c in range(9)]

# ---------------------------------------------------------------------------
# 1) THE RING — one swept circular profile. Its inner surface IS the rounded
#    throat the membrane fills: no square barrel, no flat annulus.
# ---------------------------------------------------------------------------
def build_ring():
    register(torus("ring_body", RING_R, RING_TUBE, mseg=96, nseg=28), "patina")
    # Rounded TRACK BED collar on the front face (the engine's amber ratchet
    # segments seat 0.025 proud of it) — a flattened donut, not a flat disc.
    register(torus("trackbed", TRACK_R, 0.185, loc=(0, 0, -0.36), zscale=0.42,
                   mseg=96, nseg=16), "dark")
    # Rounded inner lips either side of the throat: they catch the membrane light.
    for i, z in enumerate((-0.30, 0.30)):
        register(torus("lip%d" % i, BORE + 0.055, 0.075, loc=(0, 0, z),
                       zscale=0.75, mseg=96, nseg=12), "steel")
    # OUTER FLANGE rings — big rounded collars hugging the rim (the reference's
    # stacked flange depth), front and back, plus a proud centre band.
    register(torus("flange_f", 2.62, 0.20, loc=(0, 0, -0.26), zscale=0.55,
                   mseg=80, nseg=16), "steel")
    register(torus("flange_b", 2.62, 0.20, loc=(0, 0, 0.26), zscale=0.55,
                   mseg=80, nseg=16), "steel")
    register(torus("band_mid", 2.78, 0.11, mseg=80, nseg=14), "patina")

# ---------------------------------------------------------------------------
# 2) CLAMP / EMITTER HOUSINGS — 9 cylindrical units seated on the tube crest,
#    each a machined can with a rounded bezel, coil rings, a pivot boss and two
#    actuator rods on ball joints. This is the "portal generator" hardware.
# ---------------------------------------------------------------------------
def build_clamps():
    for c, th in enumerate(clamp_angles()):
        depth = 0.62
        cz = CHEV_CAP_Z + depth * 0.5
        register(cyl("clamp%d_can" % c, 0.205, depth, radial(th, CHEV_R, cz),
                     verts=24), "dark")
        # Rounded bezel around the amber slit (the engine's emitter sits inside).
        register(torus("clamp%d_bezel" % c, 0.215, 0.055,
                       loc=radial(th, CHEV_R, CHEV_CAP_Z + 0.03),
                       zscale=0.8, mseg=24, nseg=10), "steel")
        # Coil rings stacked along the can (rounded, not painted on).
        for k in range(2):
            register(torus("clamp%d_coil%d" % (c, k), 0.225, 0.045,
                           loc=radial(th, CHEV_R, CHEV_CAP_Z + 0.20 + 0.16 * k),
                           zscale=0.9, mseg=24, nseg=10), "steel")
        # Pivot boss where the can meets the ring.
        register(ball("clamp%d_boss" % c, 0.135, radial(th, CHEV_R, -0.10)), "dark")
        # Two ACTUATOR RODS on ball joints, raking back to the rim.
        for s in (-1, 1):
            aoff = th + s * D(9)
            a = radial(aoff, CHEV_R + 0.10, -0.42)
            b = radial(aoff, 2.72, 0.16)
            d = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
            ln = math.sqrt(sum(x * x for x in d))
            mid = ((a[0] + b[0]) * 0.5, (a[1] + b[1]) * 0.5, (a[2] + b[2]) * 0.5)
            register(cyl("clamp%d_rod%d" % (c, s), 0.052, ln, mid, rot=aim(d),
                         verts=12), "dark")
            register(ball("clamp%d_jA%d" % (c, s), 0.072, a, seg=12), "steel")
            register(ball("clamp%d_jB%d" % (c, s), 0.062, b, seg=12), "steel")

# ---------------------------------------------------------------------------
# 3) CAPACITOR BANKS + COIL EMITTERS — chunky cylinders ringing the outer rim
#    between the clamps: the thing that says "this machine GENERATES a portal".
# ---------------------------------------------------------------------------
def build_banks():
    for k in range(6):
        th = D(20) + k * D(60)
        register(cyl("bank%d_can" % k, 0.20, 0.86, radial(th, 2.98, 0.0),
                     verts=20), "steel")
        for i in range(3):
            register(torus("bank%d_coil%d" % (k, i), 0.235, 0.042,
                           loc=radial(th, 2.98, -0.28 + 0.28 * i),
                           mseg=20, nseg=10), "dark")
        for i, z in enumerate((-0.47, 0.47)):
            register(ball("bank%d_cap%d" % (k, i), 0.185, radial(th, 2.98, z),
                          seg=16), "dark")
        # Feed pipe: bank -> ring rim (a real bent pipe, generous radius).
        a = radial(th, 2.98, -0.30)
        m = radial(th - D(4), 2.86, -0.42)
        b = radial(th - D(8), 2.62, -0.30)
        register(pipe("bank%d_feed" % k, [a, m, b], 0.055), "dark")

# ---------------------------------------------------------------------------
# 4) PIPE RAILS + CABLE BUNDLES — round-bevelled runs hugging and entering the
#    ring (the reference's copper hairpins), on cylindrical standoffs.
# ---------------------------------------------------------------------------
def build_pipes():
    for s, (a0, a1, rr, pz) in enumerate(((D(200), D(340), 3.05, -0.22),
                                          (D(215), D(325), 3.16, 0.20))):
        register(pipe("rail%d" % s, arc_pts(rr, a0, a1, pz, 22), 0.070), "steel")
        for k in range(4):
            th = a0 + (a1 - a0) * (k + 0.5) / 4.0
            a = radial(th, rr, pz)
            b = radial(th, 2.74, pz * 0.5)
            d = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
            ln = math.sqrt(sum(x * x for x in d))
            mid = ((a[0] + b[0]) * 0.5, (a[1] + b[1]) * 0.5, (a[2] + b[2]) * 0.5)
            register(cyl("rail%d_sd%d" % (s, k), 0.045, ln, mid, rot=aim(d),
                         verts=10), "dark")
    # CABLE BUNDLES climbing from the cradle INTO the ring's lower flanks.
    for s in (-1, 1):
        for k in range(3):
            off = 0.09 * (k - 1)
            pts = [(s * (1.30 + off), FLOOR_Y + 0.10, -0.30 + off * 0.5),
                   (s * (1.55 + off), FLOOR_Y + 0.75, -0.34),
                   (s * (1.90 + off), -1.35, -0.30),
                   (s * (2.16 + off), -0.85, -0.24),
                   (s * (2.30 + off), -0.45, -0.16)]
            register(pipe("cable%d_%d" % (s, k), pts, 0.048), "dark")
        register(cyl("cablecl%d" % s, 0.16, 0.14, (s * 1.95, -1.30, -0.30),
                     rot=aim((s * 0.5, 1.0, 0.0)), verts=16), "steel")

# ---------------------------------------------------------------------------
# 5) BOLT STUDS + HEAT-SINK FINS — rounded studs on the collars; the only boxes
#    in the gate are the fins, and they are bevelled.
# ---------------------------------------------------------------------------
def build_details():
    for k in range(28):
        th = k * TAU / 28
        register(cyl("stud_f%d" % k, 0.036, 0.055, radial(th, 2.22, -0.46),
                     verts=8, bev=0.008), "dark")
    for k in range(20):
        th = D(9) + k * TAU / 20
        register(ball("stud_r%d" % k, 0.040, radial(th, 2.62, -0.40), seg=8), "dark")
    for k in range(8):
        th = D(22) + k * D(45)
        register(cube("fin%d" % k, 0.30, 0.035, 0.16, radial(th, 2.70, 0.42),
                      rot=(0, 0, th), bev=0.02), "steel")

# ---------------------------------------------------------------------------
# 6) CRADLE — the gate is INSTALLED. Rounded trunnion + curved legs + a round
#    base pad (no square plinth).
# ---------------------------------------------------------------------------
def build_cradle():
    register(cyl("trunnion", 0.16, 3.30, (0.0, -1.55, 0.36),
                 rot=aim((1.0, 0.0, 0.0)), verts=20), "steel")
    for s in (-1, 1):
        register(ball("trun_end%d" % s, 0.20, (s * 1.65, -1.55, 0.36)), "dark")
        pts = [(s * 1.62, -1.60, 0.30),
               (s * 1.80, -1.95, 0.20),
               (s * 1.86, FLOOR_Y + 0.42, 0.05),
               (s * 1.72, FLOOR_Y + 0.14, -0.02)]
        register(pipe("leg%d" % s, pts, 0.145), "dark")
        register(cyl("foot%d" % s, 0.42, 0.14, (s * 1.72, FLOOR_Y + 0.07, -0.02),
                     rot=aim((0.0, 1.0, 0.0)), verts=24), "steel")
    for s in (-1, 1):
        a = (s * 1.05, -1.95, -0.30)
        b = (s * 1.55, FLOOR_Y + 0.16, -0.22)
        d = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
        ln = math.sqrt(sum(x * x for x in d))
        mid = ((a[0] + b[0]) * 0.5, (a[1] + b[1]) * 0.5, (a[2] + b[2]) * 0.5)
        register(cyl("strut%d" % s, 0.085, ln, mid, rot=aim(d), verts=12), "dark")
    register(cyl("pad", 1.95, 0.16, (0.0, FLOOR_Y + 0.08, 0.02),
                 rot=aim((0.0, 1.0, 0.0)), verts=48), "steel")
    register(torus("pad_lip", 1.95, 0.09, loc=(0.0, FLOOR_Y + 0.10, 0.02),
                   rot=(D(90), 0, 0), mseg=48, nseg=10), "dark")

# ---------------------------------------------------------------------------
# Assembly
# ---------------------------------------------------------------------------
MAT_SPECS = {
    "patina": ((0.45, 0.50, 0.48, 1.0), 0.5, 0.65),
    "steel":  ((0.38, 0.40, 0.43, 1.0), 0.6, 0.60),
    "dark":   ((0.10, 0.10, 0.11, 1.0), 0.8, 0.62),
}

def make_material(name):
    base, metal, rough = MAT_SPECS[name]
    m = bpy.data.materials.new("gate_" + name)
    m.use_nodes = True
    bsdf = m.node_tree.nodes.get("Principled BSDF")
    if bsdf:
        bsdf.inputs["Base Color"].default_value = base
        bsdf.inputs["Metallic"].default_value = metal
        bsdf.inputs["Roughness"].default_value = rough
    return m

def normals_outward(ob):
    """Guarantee outward normals: recalc, then flip if the signed volume came out
    negative. THE round-4 bug (30-43% inside-out hub-facing tris, backface-culled,
    unlightable) dies here."""
    me = ob.data
    bm = bmesh.new()
    bm.from_mesh(me)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    if bm.calc_volume(signed=True) < 0.0:
        bmesh.ops.reverse_faces(bm, faces=bm.faces)
    bm.to_mesh(me)
    bm.free()

def join_group(name, objs):
    for o in objs:
        normals_outward(o)
    bpy.ops.object.select_all(action='DESELECT')
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]
    bpy.ops.object.join()
    j = bpy.context.active_object
    j.name = "gate_" + name
    j.data.name = "gate_" + name
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    return j

def uv_unwrap(ob, scale):
    bpy.ops.object.select_all(action='DESELECT')
    ob.select_set(True)
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.uv.smart_project(angle_limit=D(66), island_margin=0.02)
    bpy.ops.object.mode_set(mode='OBJECT')
    layer = ob.data.uv_layers.active
    if layer:
        for d in layer.data:
            d.uv[0] *= scale
            d.uv[1] *= scale

def main():
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()

    build_ring()
    build_clamps()
    build_banks()
    build_pipes()
    build_details()
    build_cradle()
    log("authored objects:", sum(len(v) for v in GROUPS.values()))

    rot = Matrix.Rotation(math.pi / 2, 4, 'X')
    total = 0
    for name in ("patina", "steel", "dark"):
        j = join_group(name, GROUPS[name])
        uv_unwrap(j, 2.2 if name == "patina" else 1.8)
        j.data.materials.clear()
        j.data.materials.append(make_material(name))
        j.matrix_world = rot @ j.matrix_world
        bpy.ops.object.select_all(action='DESELECT')
        j.select_set(True)
        bpy.context.view_layer.objects.active = j
        bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
        j.data.calc_loop_triangles()
        tris = len(j.data.loop_triangles)
        total += tris
        log("group gate_%s: %d tris" % (name, tris))
    log("TOTAL tris:", total)

    os.makedirs(os.path.dirname(OUT) or ".", exist_ok=True)
    bpy.ops.export_scene.gltf(
        filepath=OUT, export_format='GLB', export_yup=True, use_selection=False,
        export_animations=False, export_apply=True,
        export_materials='EXPORT', export_texcoords=True, export_normals=True)
    log("EXPORTED:", OUT)

if __name__ == "__main__":
    status = "OK"
    try:
        main()
    except Exception as e:
        import traceback
        log("FAILED:", e)
        log(traceback.format_exc())
        status = "FAIL: " + str(e)
    flush_log(status)
