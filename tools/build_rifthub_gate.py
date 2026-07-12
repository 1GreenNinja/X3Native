"""
build_rifthub_gate.py — ROUND 8: the RIFTHUB gate is *ONE LARGE METALLIC TUBE*.

Owner (verbatim, R7 + R8):
    "the gate.. is supposed to be ONE LARGE metallic Tube"
    "No chevrons needed"
    "we can have all the grok imagined stuff on it"      (-> textures, not meshes)
    "build the tube, with an lcd panel on it, some buttons, glowing led displays"

    blender-launcher.exe --background --python tools/build_rifthub_gate.py -- <out.glb>

WHAT CHANGED FROM ROUND 5 (and why the R5 gate was rejected)
------------------------------------------------------------
R5 authored 233 SEPARATE PARTS (9 clamp cans + coil rings + ball joints + actuator
rods, 6 capacitor banks, pipe rails, standoffs, 48 studs, 8 fins, chevron housings)
scattered around a thin ring. That reads as scaffolding, and the clutter chopped the
ring's specular highlight into confetti. The owner's read: a toy.

R8 is ONE MASS:
 1. THE TUBE IS A SINGLE SWEPT MESH, not a bpy torus + bolted-on kit. It is revolved
    here by hand (bmesh) so the tube's minor RADIUS is a FIELD rr(u, v) — which means
    every feature is CUT INTO / SET INTO the one surface instead of glued onto it:
      * 12 recessed SEGMENT SEAMS (circumferential joins around the cross-section);
      * 2 longitudinal RECESSED BANDS sweeping the whole torus;
      * a recessed INDICATOR GROOVE on the inner-front shoulder (the engine's amber
        segment track seats INSIDE it — a recessed slit, never a ring of triangles);
      * 7 VENT GRILLES (4 louvre slots each) sunk into the front-outer shoulder;
      * a bevelled, flat-floored OPERATOR PANEL BAY (R8-B) on the lower-right face.
    The mesh stays a closed, watertight torus: no booleans, no holes, no flipped tris.
 2. NO CHEVRONS. No clamp housings, no amber triangles, no capacitor banks, no rods,
    no rails, no fins. They are all deleted.
 3. THE GREEBLES LIVE IN THE TEXTURES. Rivets, plate joins, rust bleed, stencils and
    fine vents come from the SD3.5 img2img sets forged FROM the owner's own reference
    (tools/forge_gate_textures.py -> gate_tube_hull / gate_tube_plate /
    gate_piston_steel), baked into normal/height. A smooth tube + a deep normal map
    reads denser than a forest of cans, and it keeps ONE unbroken specular sweep.
 4. SUBORDINATE GEOMETRY IS MINIMAL: a cradle (pad + 2 curved legs + trunnion) and
    6 feed cables entering the tube's lower flanks. Nothing else touches the tube.

UVs are authored ANALYTICALLY on the sweep (u -> U, v -> V), so the forged tileable
sets wrap the tube without a smart_project seam lottery.

LOCAL SPACE CONTRACT (must match rifthub.cpp's portal basis):
  gate stands in the local XY plane, +Y = 12 o'clock, hole axis = +Z, FRONT
  (hub-facing) = -Z. Ring center at the origin; the engine translates it to
  (cx, kRingY, cz), so the floor is local y = -kRingY. We author in that frame, then
  rotate +90 deg about X so Blender's Y-up glTF exporter lands it back in-contract.

ENGINE ANCHORS (rifthub.cpp round-8 constants — keep in sync):
  tube centerline R = 2.60, tube r = 0.66  -> THROAT (bore) opens at 1.94, rim 3.26
  membrane R 1.895 / fresnel rim 1.868     -> both clear the 1.94 throat
  indicator track: r = 2.02, z = -0.300    -> seats inside the cut groove (v = -152 deg)
  operator panel bay: u = -20 deg, v = -90 deg, floor plane z = -0.560, center
                      (2.443, -0.889) — the engine puts NOTHING there; the panel
                      (LCD + buttons + LED strips) ships INSIDE this GLB.

ENV NOTE (this box): Store-Blender — blender.exe is ACL-denied, blender-launcher
DETACHES (no stdout); we report through `<out>.log` + `<out>.done` (poll them).
Driver: tools/gate_build.ps1 (keep it ASCII-only).

Clean-room: public Blender Python API + glTF 2.0 spec only. All-original procedural
authorship — no third-party gate models consulted or copied.
"""
import bpy, bmesh, math, os, sys
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

# ---- THE TUBE ---------------------------------------------------------------
RING_R    = 2.60      # tube centerline radius (major)
TUBE_R    = 0.66      # tube radius (minor)  -> throat 1.94, outer rim 3.26
BORE      = RING_R - TUBE_R
MSEG      = 256       # major segments (around the gate)  — smooth silhouette
NSEG      = 144       # minor segments (around the tube)  — must RESOLVE the cut features
                      # (a slot narrower than one minor step aliases into spikes)
FLOOR_Y   = -2.20     # world y = 0 in the local frame (engine kRingY)

# minor-angle landmarks (v): 0 = outward equator, -90 = FRONT (-Z, hub side),
# +90 = back, 180 = the throat.
V_FRONT   = -math.pi * 0.5
V_TRACK   = D(-152.0)     # the recessed indicator groove (inner-front shoulder)
V_VENT    = D(-38.0)      # vent grilles on the front-outer shoulder

# operator panel bay (R8-B)
BAY_U     = D(-20.0)
BAY_V     = V_FRONT
BAY_DU    = 0.122         # half-extent in u  -> 0.63 m of tube arc
BAY_DV    = 0.345         # half-extent in v  -> 0.46 m of tube arc
BAY_DEPTH = 0.100         # recess depth (the panel lives INSIDE this)

GROUPS = {"patina": [], "steel": [], "dark": [], "screen": [], "led": []}
def register(ob, group):
    GROUPS[group].append(ob)
    return ob


# ---------------------------------------------------------------------------
# The displacement FIELD. rr(u, v) is the tube's radius at every point of the
# sweep: subtract to CUT a feature in, add to raise one. This is what makes the
# detail a feature OF the tube instead of a mesh sitting on it.
# ---------------------------------------------------------------------------
def wrap(a):
    return (a + math.pi) % TAU - math.pi

def gauss(d, w):
    x = d / w
    return math.exp(-x * x)

def plateau(d, half, soft):
    """Flat-bottomed machined trough: 1.0 inside `half`, ramping to 0 over `soft`."""
    a = abs(d)
    if a <= half:
        return 1.0
    if a >= half + soft:
        return 0.0
    t = (half + soft - a) / soft
    return t * t * (3.0 - 2.0 * t)          # smoothstep


N_SEAM   = 12
VENT_U   = [D(30.0) + k * D(45.0) for k in range(7)]   # 7 grilles; the 8th slot is the bay's

def rr(u, v):
    r = TUBE_R

    # 12 recessed SEGMENT SEAMS: the cast tube's joins, running around the
    # cross-section. A shallow round groove -> a dark line that follows the tube.
    du_seam = wrap(u - round(u / (TAU / N_SEAM)) * (TAU / N_SEAM))
    r -= 0.034 * gauss(du_seam, 0.026)

    # 2 longitudinal RECESSED BANDS sweeping the entire torus (machined relief that
    # rides the specular highlight instead of chopping it up).
    for v0, dep in ((D(34.0), 0.028), (D(-34.0), 0.028)):
        r -= dep * plateau(wrap(v - v0), 0.038, 0.062)

    # INDICATOR GROOVE (inner-front shoulder). The engine's segmented amber track
    # sits INSIDE this — a recessed slit, per R7 addendum 2.
    r -= 0.058 * plateau(wrap(v - V_TRACK), 0.115, 0.070)

    # VENT GRILLES: 7 clusters x 4 louvre slots, sunk into the front-outer shoulder.
    # WIDTH LAW: a slot must be several minor steps wide (2pi/NSEG = 2.5 deg here) or
    # the sweep aliases it into spikes instead of cutting a slot.
    for u0 in VENT_U:
        du = wrap(u - u0)
        if abs(du) > 0.14:
            continue
        ku = plateau(du, 0.085, 0.040)
        if ku <= 0.0:
            continue
        for j in range(4):
            v0 = V_VENT + (j - 1.5) * D(10.5)
            r -= 0.042 * ku * plateau(wrap(v - v0), D(3.0), D(2.6))

    # OPERATOR PANEL BAY: a bevelled, FLAT-FLOORED recess (R8-B).
    kb = (plateau(wrap(u - BAY_U), BAY_DU, 0.030) *
          plateau(wrap(v - BAY_V), BAY_DV, 0.075))
    r -= BAY_DEPTH * kb

    return r


def surf(u, v):
    q = rr(u, v)
    rad = RING_R + q * math.cos(v)
    return Vector((rad * math.cos(u), rad * math.sin(u), q * math.sin(v)))


def build_tube():
    """Revolve the displaced profile into ONE closed, watertight, analytically-UV'd
    mesh. No booleans; the topology is a plain torus grid, so winding is uniform and
    the signed volume is positive by construction (verified in normals_outward)."""
    me = bpy.data.meshes.new("gate_tube")
    verts, faces, uvs = [], [], []
    for i in range(MSEG):
        u = i * TAU / MSEG
        for j in range(NSEG):
            v = -math.pi + j * TAU / NSEG
            verts.append(surf(u, v))
    def vid(i, j):
        return (i % MSEG) * NSEG + (j % NSEG)
    UT, VT = 8.0, 2.6          # texture tiling (major, minor)
    for i in range(MSEG):
        for j in range(NSEG):
            a, b, c, d = vid(i, j), vid(i + 1, j), vid(i + 1, j + 1), vid(i, j + 1)
            faces.append((a, b, c, d))
            u0, u1 = i / MSEG * UT, (i + 1) / MSEG * UT
            v0, v1 = j / NSEG * VT, (j + 1) / NSEG * VT
            uvs.append(((u0, v0), (u1, v0), (u1, v1), (u0, v1)))
    me.from_pydata([tuple(v) for v in verts], [], faces)
    me.update()
    uvl = me.uv_layers.new(name="UVMap")
    for fi, poly in enumerate(me.polygons):
        for k, li in enumerate(poly.loop_indices):
            uvl.data[li].uv = uvs[fi][k]
    ob = bpy.data.objects.new("gate_tube", me)
    bpy.context.collection.objects.link(ob)
    bpy.ops.object.select_all(action='DESELECT')
    ob.select_set(True)
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.shade_smooth()
    # Hard machined edges (vent walls, bay bevel, seam shoulders) stay crisp; the
    # tube body stays smooth so it takes one long highlight.
    md = ob.modifiers.new("es", 'EDGE_SPLIT')
    md.split_angle = D(38)
    bpy.ops.object.modifier_apply(modifier=md.name)
    register(ob, "patina")
    log("tube: %d verts / %d quads (MSEG=%d NSEG=%d)" % (len(verts), len(faces), MSEG, NSEG))
    return ob


# ---------------------------------------------------------------------------
# Small helpers for the (few) subordinate objects.
# ---------------------------------------------------------------------------
def _finish(ob, bev=0.0, smooth=True):
    if bev > 0.0:
        md = ob.modifiers.new("bev", 'BEVEL')
        md.width = bev
        md.segments = 2
        md.limit_method = 'ANGLE'
        md.angle_limit = D(40)
        bpy.ops.object.select_all(action='DESELECT')
        ob.select_set(True)
        bpy.context.view_layer.objects.active = ob
        bpy.ops.object.modifier_apply(modifier=md.name)
    bpy.ops.object.select_all(action='DESELECT')
    ob.select_set(True)
    bpy.context.view_layer.objects.active = ob
    if smooth:
        bpy.ops.object.shade_smooth()
    md = ob.modifiers.new("es", 'EDGE_SPLIT')
    md.split_angle = D(38)
    bpy.ops.object.modifier_apply(modifier=md.name)
    return ob

def cyl(name, r, depth, loc, rot=(0, 0, 0), verts=20, bev=0.010):
    bpy.ops.mesh.primitive_cylinder_add(vertices=verts, radius=r, depth=depth,
                                        location=loc, rotation=rot)
    ob = bpy.context.active_object
    ob.name = name
    return _finish(ob, bev)

def cube(name, hx, hy, hz, loc, rot=(0, 0, 0), bev=0.006):
    bpy.ops.mesh.primitive_cube_add(size=2.0, location=loc, rotation=rot)
    ob = bpy.context.active_object
    ob.name = name
    ob.scale = (hx, hy, hz)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    return _finish(ob, bev)

def ball(name, r, loc, seg=16):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=seg, ring_count=max(6, seg // 2),
                                         radius=r, location=loc)
    ob = bpy.context.active_object
    ob.name = name
    return _finish(ob)

def pipe(name, pts, radius, res=6):
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

def aim(vec):
    return Vector(vec).normalized().to_track_quat('Z', 'Y').to_euler()


# ---------------------------------------------------------------------------
# R8-B: THE OPERATOR PANEL, set INTO the tube's bay.
# Authored in the BAY's local frame (x = along the tube, y = up the tube's face,
# z = out of the surface toward the player), then mapped onto the gate.
# ---------------------------------------------------------------------------
def bay_matrix():
    p = surf(BAY_U, BAY_V)                      # bay FLOOR center (rr already recessed)
    t = Vector((-math.sin(BAY_U), math.cos(BAY_U), 0.0))    # along the tube
    rd = Vector((math.cos(BAY_U), math.sin(BAY_U), 0.0))    # up the face (radial)
    n = Vector((0.0, 0.0, -1.0))                            # out toward the hub
    m = Matrix().to_4x4()
    m[0][0], m[1][0], m[2][0] = t.x, t.y, t.z
    m[0][1], m[1][1], m[2][1] = rd.x, rd.y, rd.z
    m[0][2], m[1][2], m[2][2] = n.x, n.y, n.z
    m[0][3], m[1][3], m[2][3] = p.x, p.y, p.z
    return m

BAY = None
def place(ob):
    ob.matrix_world = BAY @ ob.matrix_world
    bpy.ops.object.select_all(action='DESELECT')
    ob.select_set(True)
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    return ob

def build_panel():
    global BAY
    BAY = bay_matrix()
    p = BAY.translation
    log("panel bay floor at local (%.3f, %.3f, %.3f)" % (p.x, p.y, p.z))

    # Recessed housing plate that lines the bay floor (dark, so the screen pops).
    place(register(cube("panel_plate", 0.275, 0.195, 0.008, (0, 0, 0.008)), "dark"))

    # --- LCD: a bezelled screen sunk into the plate ---
    place(register(cube("panel_lcd", 0.150, 0.105, 0.006, (-0.105, 0.045, 0.019)),
                   "screen"))
    # Round-pipe bezel around the glass (4 bevelled bars = a machined frame).
    for (bx, by, hx, hy) in ((0, 0.118, 0.166, 0.014),
                             (0, -0.118, 0.166, 0.014),
                             (-0.166, 0, 0.014, 0.132),
                             (0.166, 0, 0.014, 0.132)):
        place(register(cube("panel_bez_%d_%d" % (int(bx * 100), int(by * 100)),
                            hx, hy, 0.014, (-0.105 + bx, 0.045 + by, 0.022)), "steel"))

    # --- BUTTONS: a 4x2 cluster of chunky physical keys with travel + a lit cap ---
    for j in range(2):
        for i in range(4):
            bx = 0.095 + i * 0.058
            by = 0.100 - j * 0.062
            place(register(cyl("panel_btn%d%d" % (i, j), 0.024, 0.030, (bx, by, 0.020),
                               verts=14, bev=0.004), "steel"))
            place(register(cyl("panel_cap%d%d" % (i, j), 0.017, 0.012, (bx, by, 0.038),
                               verts=14, bev=0.003), "led"))
    # A chunky ENGAGE key, bigger, off to the side.
    place(register(cyl("panel_engage", 0.036, 0.034, (0.185, -0.055, 0.021),
                       verts=18, bev=0.005), "steel"))
    place(register(cyl("panel_engage_cap", 0.026, 0.014, (0.185, -0.055, 0.042),
                       verts=18, bev=0.004), "led"))

    # --- LED readout strips (integrated, recessed, subtle) ---
    for k in range(3):
        place(register(cube("panel_led%d" % k, 0.140, 0.008, 0.005,
                            (-0.105, -0.078 - k * 0.024, 0.016)), "led"))
    # Two small round status lamps beside the screen.
    for k, x in enumerate((-0.262, -0.262)):
        place(register(cyl("panel_lamp%d" % k, 0.011, 0.010,
                           (x, 0.100 - k * 0.048, 0.016), verts=10, bev=0.002), "led"))
    # A slim ventilated grille bar under the buttons (steel, machined).
    place(register(cube("panel_grille", 0.115, 0.010, 0.010, (0.155, -0.130, 0.014)),
                   "steel"))


# ---------------------------------------------------------------------------
# CRADLE — the gate is INSTALLED. Minimal: a round pad, two curved legs, one
# trunnion through the tube's lower flanks. Nothing that breaks the highlight.
# ---------------------------------------------------------------------------
def build_cradle():
    register(cyl("pad", 2.05, 0.16, (0.0, FLOOR_Y + 0.08, 0.02),
                 rot=aim((0.0, 1.0, 0.0)), verts=56), "steel")
    register(cyl("trunnion", 0.15, 3.6, (0.0, -1.62, 0.30),
                 rot=aim((1.0, 0.0, 0.0)), verts=20), "dark")
    for s in (-1, 1):
        register(ball("trun_end%d" % s, 0.20, (s * 1.80, -1.62, 0.30)), "steel")
        pts = [(s * 1.80, -1.66, 0.26),
               (s * 2.02, -1.98, 0.16),
               (s * 2.06, FLOOR_Y + 0.55, 0.04),
               (s * 1.90, FLOOR_Y + 0.16, -0.02)]
        register(pipe("leg%d" % s, pts, 0.155), "dark")
        register(cyl("foot%d" % s, 0.44, 0.13, (s * 1.90, FLOOR_Y + 0.07, -0.02),
                     rot=aim((0.0, 1.0, 0.0)), verts=28), "steel")


# ---------------------------------------------------------------------------
# FEED CABLES — a few conduits entering the tube's lower flanks. That is ALL the
# subordinate geometry the tube gets.
# ---------------------------------------------------------------------------
def build_cables():
    for s in (-1, 1):
        for k in range(3):
            off = 0.10 * (k - 1)
            th = D(250.0) if s < 0 else D(290.0)
            entry = surf(th + D(4.0) * (k - 1), D(-70.0))
            pts = [(s * (1.15 + off), FLOOR_Y + 0.12, -0.34 + off * 0.4),
                   (s * (1.42 + off), FLOOR_Y + 0.85, -0.40),
                   (s * (1.72 + off), -1.30, -0.42),
                   (entry.x * 0.94, entry.y * 0.94, entry.z - 0.05),
                   (entry.x, entry.y, entry.z)]
            register(pipe("cable%d_%d" % (s, k), pts, 0.052), "dark")
        register(cyl("cableclamp%d" % s, 0.17, 0.14, (s * 1.60, -1.42, -0.40),
                     rot=aim((s * 0.5, 1.0, 0.0)), verts=16), "steel")


# ---------------------------------------------------------------------------
# Assembly
# ---------------------------------------------------------------------------
MAT_SPECS = {
    "patina": ((0.42, 0.45, 0.44, 1.0), 0.45, 0.66),
    "steel":  ((0.38, 0.40, 0.43, 1.0), 0.60, 0.62),
    "dark":   ((0.10, 0.10, 0.11, 1.0), 0.75, 0.64),
    "screen": ((0.05, 0.08, 0.10, 1.0), 0.00, 0.18),   # LCD glass (engine drives emissive)
    "led":    ((0.06, 0.09, 0.09, 1.0), 0.00, 0.30),   # indicator caps + strips
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
    """Recalc + signed-volume check. The R4 gate shipped 30-43% inside-out tris
    (backface-culled, unlightable) — that bug does not come back.

    NOT APPLIED TO THE TUBE, and that is deliberate. recalc_face_normals decides
    "outward" per CONNECTED ISLAND, and EDGE_SPLIT (which we need for crisp machined
    edges) DISCONNECTS every face whose crease exceeds the split angle — so the deep
    cut features (the indicator groove, the vent slots, the panel bay) become their
    own little open islands with no inside, and the recalc happily INVERTS them. That
    is measurable: before this exemption, 100% of the groove's faces (v -142..-163
    deg, 4096 tris) came back inside-out while the rest of the tube was fine.
    The sweep in build_tube() winds every quad (i,j)->(i+1,j)->(i+1,j+1)->(i,j+1),
    whose normal is dP/du x dP/dv = q(R + q cos v)(cos v cos u, cos v sin u, sin v) —
    the outward tube normal, by construction, for ANY displacement field. So the tube
    needs no recalc; it needs to be LEFT ALONE. verify_tube_outward() proves it.
    """
    me = ob.data
    if ob.name.startswith("gate_tube"):
        bm = bmesh.new()
        bm.from_mesh(me)
        vol = bm.calc_volume(signed=True)
        bm.free()
        return vol
    bm = bmesh.new()
    bm.from_mesh(me)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    vol = bm.calc_volume(signed=True)
    if vol < 0.0:
        bmesh.ops.reverse_faces(bm, faces=bm.faces)
    bm.to_mesh(me)
    bm.free()
    return vol

def join_group(name, objs):
    vols = [normals_outward(o) for o in objs]
    neg = sum(1 for v in vols if v < 0.0)
    log("group %s: %d objs, %d had negative signed volume (flipped outward)"
        % (name, len(objs), neg))
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

def uv_project(ob, scale):
    """Smart-project the SUBORDINATE groups only. The tube keeps its analytic UVs."""
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

def verify_tube_outward(ob):
    """RIGOROUS inside-out check for the TUBE — the R4 bug (30-43% inverted,
    backface-culled, unlightable) must never come back.

    A centroid test is MEANINGLESS on a torus (the tube's inner half legitimately
    faces the ring center), so we test against the tube's ANALYTIC AXIS instead: for
    any point P on the tube, the outward direction is P minus the nearest point on
    the centerline circle, i.e. P - (RING_R * unit(P.xy), 0). Every face normal must
    have a POSITIVE dot with it.  NOTE: the object has ALREADY been rotated +90 deg
    about X for the Y-up export, so we un-rotate before testing.
    """
    me = ob.data
    me.calc_loop_triangles()
    inv = Matrix.Rotation(-math.pi / 2, 4, 'X')
    rot3 = inv.to_3x3()
    bad = 0
    hist = {}
    for t in me.loop_triangles:
        p = inv @ (sum((me.vertices[i].co for i in t.vertices), Vector()) / 3.0)
        n = rot3 @ t.normal
        xy = Vector((p.x, p.y, 0.0))
        if xy.length < 1e-6:
            continue
        c = xy.normalized() * RING_R          # nearest centerline point
        if (p - c).dot(n) <= 0.0:
            bad += 1
            d = p - c
            v = math.degrees(math.atan2(d.z, d.dot(xy.normalized())))
            k = int(round(v / 5.0)) * 5       # bucket the minor angle
            hist[k] = hist.get(k, 0) + 1
    if hist:
        top = sorted(hist.items(), key=lambda kv: -kv[1])[:6]
        log("   inside-out by minor angle v (deg -> count):",
            ", ".join("%d:%d" % kv for kv in top))
    return bad, max(1, len(me.loop_triangles))


def signed_volume(ob):
    bm = bmesh.new()
    bm.from_mesh(ob.data)
    v = bm.calc_volume(signed=True)
    bm.free()
    return v

def main():
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()

    tube = build_tube()
    build_panel()
    build_cradle()
    build_cables()
    log("authored objects:", sum(len(v) for v in GROUPS.values()),
        "(tube = 1 of them; R5 shipped 233)")

    rot = Matrix.Rotation(math.pi / 2, 4, 'X')
    total = 0
    for name in ("patina", "steel", "dark", "screen", "led"):
        objs = GROUPS[name]
        if not objs:
            continue
        j = join_group(name, objs)
        if name != "patina":                    # tube keeps its analytic UV grid
            uv_project(j, 1.8 if name in ("steel", "dark") else 1.0)
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
        vol = signed_volume(j)
        if name == "patina":                  # the tube: analytic outward test
            bad, n = verify_tube_outward(j)
            log("group gate_%s: %d tris, signed volume %+.3f, TUBE inside-out %d/%d "
                "(%.3f%%)" % (name, tris, vol, bad, n, 100.0 * bad / n))
            if bad > 0:
                log("!! WARNING: the tube has inside-out triangles — DO NOT SHIP")
        else:
            log("group gate_%s: %d tris, signed volume %+.3f (>0 = outward)"
                % (name, tris, vol))
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
