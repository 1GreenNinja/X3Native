"""
build_rifthub_gate.py — headless Blender AUTHOR of the RIFTHUB industrial gate
(ROUND 3 "density round", docs/RIFTHUB_ART_TARGET.md).

    blender-launcher.exe --background --python tools/build_rifthub_gate.py -- <out.glb>

Procedurally builds ONE dense Stargate-style industrial gate mesh — segmented
stacked ring plates, 9 chunky chamfered clamp housings with jaw flanges +
pivot bosses, piston/actuator rods bridging ring segments, accumulator
cylinders, pipe runs hugging the outer rim with collars, bolt rings, vent
slits, and a base plinth + angled shoulder skirt that mates with the engine's
gate cradle — then joins everything into THREE material-group meshes
(gate_patina / gate_steel / gate_dark), smart-UV-unwraps each, and exports a
single GLB the engine instances 8x (app/rifthub.cpp gate-GLB path).

LOCAL SPACE CONTRACT (must match rifthub.cpp's portal basis):
  glTF space after export_yup=True: gate stands in the local XY plane,
  +Y = 12 o'clock, hole axis = +Z, FRONT (hub-facing, where the engine's
  amber chevron slits + ratchet track sit proud) = -Z. Ring center at the
  origin; the engine translates it to (cx, kRingY=2.2, cz). The floor is
  therefore local y = -2.2.
  We AUTHOR in that same XY/-Z frame in Blender, then rotate the finished
  meshes +90 deg about X (authoring (x,y,z) -> Blender (x,-z,y)) so Blender's
  Y-up GLB exporter lands them back exactly in the contract frame.

KEY ENGINE DIMENSIONS HONOURED (rifthub.cpp):
  ring centerline R=2.05, tube r=0.40 (band 1.65..2.45), membrane R=1.58,
  fresnel rim R=1.615 — the inner throat stays >= 1.66 so nothing clips;
  amber ratchet track segs at r=1.80, z=-0.443..-0.479 -> a recessed track
  BED ring is authored at r 1.70..1.92 with its face at z=-0.468 so the
  engine's segs sit ~0.011 proud (seated, not floating);
  chevron slits at r=2.02, z=-0.762 -> clamp cap faces at z=-0.75 so each
  slit floats 0.012 proud of its housing (the powered-lock read).
  9 clamps at th = 90 - c*40 deg (chevron 0 at 12 o'clock, clockwise).

ENV NOTE (this box): Store-Blender — blender.exe ACL-denied, blender-launcher
DETACHES; we report through `<out>.log` + `<out>.done` (poll them).

Clean-room: public Blender Python API + glTF 2.0 spec only. All-original
procedural authorship — no third-party gate models consulted or copied.
"""
import bpy, bmesh, math, os, random, sys

ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT = ARGV[0] if ARGV else os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                         "..", "assets", "converted_glb", "rifthub",
                                         "gate_ring.glb")
OUT = os.path.abspath(OUT)
LOG_PATH, DONE_PATH = OUT + ".log", OUT + ".done"

_log = []
def log(*a):
    s = "[gatebuild] " + " ".join(str(x) for x in a)
    _log.append(s); print(s)

def flush_log(status):
    try:
        with open(LOG_PATH, "w", encoding="utf-8") as f: f.write("\n".join(_log))
        with open(DONE_PATH, "w", encoding="utf-8") as f: f.write(status)
    except Exception as e:
        print("[gatebuild] could not write log/marker:", e)

TAU = math.tau
D = math.radians
R = random.Random(0xF4B7E)   # deterministic jitter

# Material groups: every builder appends its object to one of these.
GROUPS = {"patina": [], "steel": [], "dark": []}

def register(obj, group):
    GROUPS[group].append(obj)
    return obj

# ---------------------------------------------------------------------------
# Geometry helpers
# ---------------------------------------------------------------------------
def new_object(name, bm):
    me = bpy.data.meshes.new(name)
    bm.normal_update()
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new(name, me)
    bpy.context.collection.objects.link(ob)
    return ob

def sweep(name, profile, a0, a1, steps):
    """Sweep a closed (r,z) polygon around the gate axis: (r,z) at angle a ->
    (r*cos a, r*sin a, z). Partial arcs get end-cap n-gons; full circles wrap.
    Normals recalculated outward (closed solid)."""
    bm = bmesh.new()
    n = len(profile)
    full = abs((a1 - a0) - TAU) < 1e-5
    ring_count = steps if full else steps + 1
    rings = []
    for s in range(ring_count):
        a = a0 + (a1 - a0) * s / steps
        ca, sa = math.cos(a), math.sin(a)
        rings.append([bm.verts.new((r * ca, r * sa, z)) for (r, z) in profile])
    for s in range(steps):
        r0 = rings[s]
        r1 = rings[(s + 1) % ring_count]
        for i in range(n):
            j = (i + 1) % n
            bm.faces.new((r0[i], r0[j], r1[j], r1[i]))
    if not full:
        bm.faces.new(rings[0])
        bm.faces.new(list(reversed(rings[-1])))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    return new_object(name, bm)

def rect_profile(r0, r1, z0, z1, c=0.0):
    """Chamfered rectangle in the (r,z) plane (r0<r1, z0<z1). c = corner cut."""
    if c <= 1e-6:
        return [(r0, z0), (r1, z0), (r1, z1), (r0, z1)]
    return [(r0 + c, z0), (r1 - c, z0), (r1, z0 + c), (r1, z1 - c),
            (r1 - c, z1), (r0 + c, z1), (r0, z1 - c), (r0, z0 + c)]

def circle_profile(rc, zc, pr, n=10):
    return [(rc + pr * math.cos(TAU * k / n), zc + pr * math.sin(TAU * k / n))
            for k in range(n)]

def apply_bevel(ob, width, segments=1, angle=40.0):
    md = ob.modifiers.new("bev", 'BEVEL')
    md.width = width
    md.segments = segments
    md.limit_method = 'ANGLE'
    md.angle_limit = D(angle)
    bpy.ops.object.select_all(action='DESELECT')
    ob.select_set(True)
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.modifier_apply(modifier=md.name)

def box(name, hx, hy, hz, loc, rotz=0.0, rot=None, bev=0.02, seg=1):
    """Chamfered box. hx/hy/hz half-extents; rotz = Z euler; rot overrides."""
    bpy.ops.mesh.primitive_cube_add(size=2.0, location=loc)
    ob = bpy.context.active_object
    ob.name = name
    ob.scale = (hx, hy, hz)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    if bev > 0:
        apply_bevel(ob, bev, seg)
    ob.rotation_euler = rot if rot else (0.0, 0.0, rotz)
    return ob

def cyl(name, r, depth, loc, rot=(0.0, 0.0, 0.0), verts=12, bev=0.012):
    bpy.ops.mesh.primitive_cylinder_add(vertices=verts, radius=r, depth=depth,
                                        location=loc, rotation=rot)
    ob = bpy.context.active_object
    ob.name = name
    if bev > 0:
        # Bevel BEFORE the rotation matters is fine: modifier works in local space.
        apply_bevel(ob, bev, 1, angle=35.0)
    return ob

def radial(th, r, z):
    return (r * math.cos(th), r * math.sin(th), z)

# ---------------------------------------------------------------------------
# 1) RING CORE — inner throat barrel + track bed + segmented face/rim plates
# ---------------------------------------------------------------------------
def build_ring():
    # Inner throat barrel (the wall you see through the ring's opening).
    register(sweep("barrel", rect_profile(1.66, 1.73, -0.46, 0.46, 0.015),
                   0, TAU, 96), "dark")
    # Recessed track bed on the FRONT face — the engine's amber ratchet segs
    # (r 1.745..1.855, front z -0.479) sit ~0.011 proud of the bed's -0.468 face.
    register(sweep("trackbed", rect_profile(1.70, 1.93, -0.468, -0.44, 0.008),
                   0, TAU, 96), "dark")
    # FRONT plate ring: 18 weathered arc plates (r 1.94 out), varying depth.
    for s in range(18):
        a0 = s * TAU / 18 + D(1.2)
        a1 = (s + 1) * TAU / 18 - D(1.2)
        r_out = 2.30 + 0.07 * R.random()
        z1 = -0.505 - 0.035 * (s % 2) - 0.012 * R.random()
        register(sweep("fplate%d" % s,
                       rect_profile(1.94, r_out, z1, -0.452, 0.013), a0, a1, 8),
                 "patina")
    # BACK plate ring: 18 plates offset half a slot.
    for s in range(18):
        a0 = s * TAU / 18 + D(10) + D(1.2)
        a1 = s * TAU / 18 + D(10) + TAU / 18 - D(1.2)
        r_out = 2.32 + 0.05 * R.random()
        z1 = 0.505 + 0.030 * ((s + 1) % 2) + 0.010 * R.random()
        register(sweep("bplate%d" % s,
                       rect_profile(1.74, r_out, 0.452, z1, 0.013), a0, a1, 8),
                 "patina")
    # OUTER rim layer A: 12 heavy arc plates spanning the depth.
    for s in range(12):
        a0 = s * TAU / 12 + D(1.6)
        a1 = (s + 1) * TAU / 12 - D(1.6)
        register(sweep("rimA%d" % s,
                       rect_profile(2.34, 2.50 + 0.03 * R.random(),
                                    -0.42, 0.42, 0.02), a0, a1, 10),
                 "patina")
    # OUTER rim layer B: nested over-plates on 8 of the 12 slots, jittered spans
    # (the stacked/varying-radial-depth read from the reference stills).
    slots = [0, 1, 3, 4, 6, 7, 9, 10]
    for s in slots:
        mid = (s + 0.5) * TAU / 12
        half = D(8.0 + 2.5 * R.random())
        z0 = -0.34 + 0.10 * R.random()
        z1 = 0.20 + 0.16 * R.random()
        register(sweep("rimB%d" % s,
                       rect_profile(2.50, 2.60 + 0.03 * R.random(), z0, z1, 0.018),
                       mid - half, mid + half, 8),
                 "patina")

# ---------------------------------------------------------------------------
# 2) CLAMP HOUSINGS — 9 chunky chamfered stacks at the chevron angles
# ---------------------------------------------------------------------------
def clamp_angles():
    return [D(90) - c * TAU / 9 for c in range(9)]

def build_clamps():
    for c, th in enumerate(clamp_angles()):
        rz = th + math.pi / 2      # box local X -> ring tangent
        # Main body: grips the tube band, wraps front + into the depth.
        register(box("clamp%d_body" % c, 0.30, 0.46, 0.36,
                     radial(th, 2.02, -0.26), rotz=rz, bev=0.03, seg=2), "dark")
        # Stepped face cap — front face at z=-0.75 (engine slit floats 0.012 proud).
        register(box("clamp%d_cap" % c, 0.21, 0.27, 0.075,
                     radial(th, 2.02, -0.675), rotz=rz, bev=0.02, seg=2), "dark")
        # Jaw flanges either side (the mechanical bite).
        tx, ty = -math.sin(th), math.cos(th)
        for side in (-1, 1):
            loc = (2.02 * math.cos(th) + tx * 0.37 * side,
                   2.02 * math.sin(th) + ty * 0.37 * side, -0.40)
            register(box("clamp%d_jaw%d" % (c, side), 0.07, 0.35, 0.15,
                         loc, rotz=rz, bev=0.015), "dark")
        # Pivot boss: a fat cylinder through the clamp's outer end (axis tangent).
        register(cyl("clamp%d_boss" % c, 0.105, 0.78,
                     radial(th, 2.44, -0.10), rot=(math.pi / 2, 0, th), verts=14),
                 "dark")
        for side in (-1, 1):
            loc = (2.44 * math.cos(th) + tx * 0.44 * side,
                   2.44 * math.sin(th) + ty * 0.44 * side, -0.10)
            register(cyl("clamp%d_nut%d" % (c, side), 0.055, 0.10, loc,
                         rot=(math.pi / 2, 0, th), verts=8, bev=0.008), "steel")
        # Vent slits on the cap face (flank the engine's amber slit).
        for k in (-1, 1):
            loc = radial(th, 2.02 + 0.15 * k, -0.755)
            register(box("clamp%d_vent%d" % (c, k), 0.13, 0.018, 0.012,
                         loc, rotz=rz, bev=0.0), "dark")

# ---------------------------------------------------------------------------
# 3) PISTON / ACTUATOR rods bridging ring segments + accumulator cylinders
# ---------------------------------------------------------------------------
def build_actuators():
    for c in (0, 1, 3, 4, 6, 7):
        th = D(90) - (c + 0.5) * TAU / 9
        rot = (math.pi / 2, 0, th)
        tx, ty = -math.sin(th), math.cos(th)
        register(cyl("rod%d" % c, 0.045, 1.00, radial(th, 2.50, -0.05),
                     rot=rot, verts=10, bev=0.0), "dark")
        register(cyl("sleeve%d" % c, 0.085, 0.46,
                     (2.50 * math.cos(th) + tx * 0.18, 2.50 * math.sin(th) + ty * 0.18,
                      -0.05), rot=rot, verts=12), "steel")
        for side in (-1, 1):
            loc = (2.44 * math.cos(th) + tx * 0.42 * side,
                   2.44 * math.sin(th) + ty * 0.42 * side, -0.05)
            register(box("rodmnt%d_%d" % (c, side), 0.09, 0.14, 0.09, loc,
                         rotz=th + math.pi / 2, bev=0.015), "dark")
    # Two heavy accumulator cylinders on the lower flanks (reference: the
    # stacked side tanks). Kept clear of the floor (y > -2.2) and the pipes.
    for k, th in enumerate((D(195), D(345))):
        rot = (math.pi / 2, 0, th)
        tx, ty = -math.sin(th), math.cos(th)
        register(cyl("accum%d" % k, 0.15, 1.05, radial(th, 2.60, 0.05),
                     rot=rot, verts=16), "steel")
        for j, o in enumerate((-0.30, 0.0, 0.30)):
            loc = (2.60 * math.cos(th) + tx * o, 2.60 * math.sin(th) + ty * o, 0.05)
            register(cyl("accol%d_%d" % (k, j), 0.18, 0.055, loc, rot=rot,
                         verts=16, bev=0.008), "dark")
        for side in (-1, 1):
            loc = (2.48 * math.cos(th) + tx * 0.40 * side,
                   2.48 * math.sin(th) + ty * 0.40 * side, 0.05)
            register(box("acmnt%d_%d" % (k, side), 0.10, 0.16, 0.20, loc,
                         rotz=th + math.pi / 2, bev=0.02), "dark")

# ---------------------------------------------------------------------------
# 4) PIPE RUNS hugging the outer rim, with collars + end flanges
# ---------------------------------------------------------------------------
def build_pipes():
    runs = [
        ("pipeA", 2.70, -0.12, D(25), D(150), 44),
        ("pipeB0", 2.74, 0.16, D(195), D(230), 14),
        ("pipeB1", 2.74, 0.16, D(310), D(345), 14),
    ]
    for name, rr, zz, a0, a1, steps in runs:
        register(sweep(name, circle_profile(rr, zz, 0.045, 10), a0, a1, steps),
                 "dark")
        # Collars every ~21 deg + end flanges tying into the rim.
        n_col = max(2, int((a1 - a0) / D(21)))
        for k in range(n_col + 1):
            th = a0 + (a1 - a0) * k / n_col
            register(cyl(name + "_col%d" % k, 0.068, 0.07, radial(th, rr, zz),
                         rot=(math.pi / 2, 0, th), verts=12, bev=0.006), "steel")
        for th in (a0, a1):
            register(box(name + "_end%.0f" % math.degrees(th), 0.07, 0.10, 0.10,
                         radial(th, rr - 0.08, zz), rotz=th + math.pi / 2,
                         bev=0.012), "dark")

# ---------------------------------------------------------------------------
# 5) BOLT RINGS — instanced low-poly studs on the face plates
# ---------------------------------------------------------------------------
def build_bolts():
    cths = clamp_angles()
    def clear_of_clamps(th):
        for ct in cths:
            d = (th - ct) % TAU
            if min(d, TAU - d) <= D(9):
                return False
        return True
    n = 0
    for k in range(36):
        th = k * TAU / 36 + D(5)
        if not clear_of_clamps(th):
            continue
        for rr in (1.99, 2.29):
            register(cyl("boltF%d_%d" % (k, int(rr * 100)), 0.028, 0.05,
                         radial(th, rr, -0.535), verts=6, bev=0.0), "dark")
            n += 1
    for k in range(24):
        th = k * TAU / 24 + D(7.5)
        if not clear_of_clamps(th):
            continue
        register(cyl("boltB%d" % k, 0.028, 0.05, radial(th, 2.28, 0.525),
                     verts=6, bev=0.0), "dark")
        n += 1
    log("bolts:", n)

# ---------------------------------------------------------------------------
# 6) BASE — plinth + angled shoulder skirt + foot pads (mates with the cradle)
# ---------------------------------------------------------------------------
def build_base():
    floor = -2.2
    # Plinth (top at -1.68, matching the engine skirt's visual height band).
    register(box("plinth", 1.90, 0.26, 0.85, (0, -1.94, 0), bev=0.04, seg=2),
             "steel")
    # Angled shoulder blocks OUTSIDE the ring band, top leaning INTO the gate
    # (the A-frame cradle read) — the walk-through opening (r < 1.65) stays clear.
    for side in (-1, 1):
        register(box("shoulder%d" % side, 0.42, 0.58, 0.68,
                     (1.85 * side, -1.62, 0),
                     rot=(0, 0, D(20) * side), bev=0.05, seg=2), "steel")
        register(box("foot%d" % side, 0.62, 0.075, 0.55,
                     (1.95 * side, floor + 0.075, 0), bev=0.015), "dark")
        for j in (-1, 0, 1):
            register(box("shvent%d_%d" % (side, j), 0.02, 0.02, 0.30,
                         (2.13 * side, -1.52 + 0.14 * j, 0),
                         rot=(0, 0, D(20) * side), bev=0.0), "dark")
    # Neck collar: a heavy arc shroud tying the ring bottom into the plinth.
    register(sweep("neck", rect_profile(2.34, 2.62, -0.30, 0.30, 0.02),
                   D(242), D(298), 12), "steel")

# ---------------------------------------------------------------------------
# Materials / join / UV / export
# ---------------------------------------------------------------------------
MAT_SPECS = {
    "patina": ((0.45, 0.50, 0.48, 1.0), 0.9, 0.55),
    "steel":  ((0.38, 0.40, 0.43, 1.0), 0.9, 0.45),
    "dark":   ((0.10, 0.10, 0.11, 1.0), 0.85, 0.50),
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

def join_group(name, objs):
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
    for d in layer.data:
        d.uv[0] *= scale
        d.uv[1] *= scale

def main():
    # Wipe the default scene.
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()

    build_ring()
    build_clamps()
    build_actuators()
    build_pipes()
    build_bolts()
    build_base()
    log("authored objects:", sum(len(v) for v in GROUPS.values()))

    from mathutils import Matrix
    rot = Matrix.Rotation(math.pi / 2, 4, 'X')
    total_tris = 0
    for name in ("patina", "steel", "dark"):
        j = join_group(name, GROUPS[name])
        # UV scale: finer tiling on the big plates, coarser on hardware.
        uv_unwrap(j, 2.6 if name == "patina" else 2.0)
        j.data.materials.clear()
        j.data.materials.append(make_material(name))
        # Authoring frame -> Blender frame so export_yup lands the contract space.
        j.matrix_world = rot @ j.matrix_world
        bpy.ops.object.select_all(action='DESELECT')
        j.select_set(True)
        bpy.context.view_layer.objects.active = j
        bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
        j.data.calc_loop_triangles()
        tris = len(j.data.loop_triangles)
        total_tris += tris
        log("group gate_%s: %d tris" % (name, tris))
    log("TOTAL tris:", total_tris)

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
