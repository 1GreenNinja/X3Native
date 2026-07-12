"""
build_rifthub_gate.py — ROUND 10: the RIFTHUB gate is *ONE LARGE MACHINED STEEL TUBE*.

Owner (verbatim, R7 + R8 + R10):
    "the gate.. is supposed to be ONE LARGE metallic Tube"
    "No chevrons needed"
    "build the tube, with an lcd panel on it, some buttons, glowing led displays"
    R10: it reads as a PILLOWY INFLATED CUSHION -- a marshmallow -- not machined steel.

    blender-launcher.exe --background --python tools/build_rifthub_gate.py -- <out.glb>

WHY R9 WAS A MARSHMALLOW (and what R10 changes)
------------------------------------------------------------------------------
R8/R9 built the tube as a smooth torus plus a DISPLACEMENT FIELD rr(u, v) whose
every feature was authored with `gauss()` and a smoothstep-walled `plateau()`.
Both are C1-continuous bumps. A C1 bump has NO CREASE, so:

  * EDGE_SPLIT (38 deg) never fired on a single one of them -- the whole tube
    shaded as one smooth blob;
  * with no crease there is no specular EDGE LINE, and an edge line is the entire
    visual signature of machined metal. A machined plate reads hard because its
    chamfer catches a thin, bright, straight highlight. Remove the chamfer and the
    same plate reads as an inflated pad;
  * the two soft longitudinal "recessed bands" at v = +/-34 deg were wide, smooth,
    ~0.028-deep dents. On a round tube a smooth dent does not read as a groove --
    it reads as QUILTING. That is literally how a cushion is made.

No material, tint or light can fix that: it is C1 geometry. R10 re-authors the
PROCEDURE (docs/DECISIONS.md, "GENERATE, DON'T HAND-CARVE").

R10: THE TUBE IS A TURNED LATHE PROFILE, NOT A FIELD
------------------------------------------------------------------------------
 1. THE CROSS-SECTION IS AN EXPLICIT MACHINED POLYLINE (`build_profile`), not a
    smooth function. Walking around the tube's minor circle you hit, in order and
    forever:
        FLAT PLATE CHORD -> LINEAR CHAMFER -> FLAT GROOVE FLOOR -> LINEAR CHAMFER ->
        FLAT PLATE CHORD -> ...
    16 plates, each a true straight CHORD (q = TUBE_R / cos(dv) is exactly a
    polygon side in the cross-section's polar frame), each bounded by a ~68-deg
    chamfer dropping into a 24 mm-wide, 22 mm-deep flat-floored seam. Those are
    C0 joints: real creases, at 68 deg -- so EDGE_SPLIT fires, the normals step,
    and every chamfer takes a hard specular line. THAT is the machined read.
 2. ADAPTIVE SAMPLING. The vertex rings are placed ON the feature boundaries
    (`u_samples` / `build_profile` emit a vert at every breakpoint) instead of on a
    uniform grid that has to be dense enough to accidentally hit them. Crisper
    features at LOWER cost than R9's brute 256x144.
 3. THE TUBE IS BOLTED TOGETHER. 12 circumferential joins; 6 of them are raised,
    chamfered FLANGE RIBS with a deep joint groove down the middle and a real BOLT
    ROW on each side (hex heads, actual geometry, sitting on the rib -- "bolted ON",
    not sculpted in). The other 6 are plain machined seams.
 4. GREEBLES ARE BOLTED-ON HOUSINGS, not field dents. R9 sank its vent louvres into
    the tube with smoothstep walls (soft -> invisible). R10 bolts 4 machined vent
    housings onto the front-outer shoulder, and puts a bolted bezel frame around the
    operator bay. Every added part is a chamfered plate with visible fasteners.
 5. UV DENSITY x3.4. R9 tiled at UT=8 / VT=2.6 -> ~2.0 m of tube per texture tile,
    which washes the panel/rivet detail into mush at player distance, AND VT=2.6 is
    non-integer so the minor wrap had a hard texture seam. R10: UT=27 / VT=7, both
    INTEGER (seamless wrap), ~0.60 m per tile in BOTH directions (isotropic texels).
    V is laid out by ARC LENGTH along the profile polyline, so the chamfers and
    groove floors get proportional texture instead of being squeezed.
 6. NO CHEVRONS. NO DASHES. NO HAZARD RING. Unchanged from R8: the form is ONE big
    tube; the only accent is the single continuous recessed INDICATOR GROOVE, which
    is now simply the k=0 seam of the plate system, widened and deepened, so the
    engine's indicator line seats in a real machined channel.

UNCHANGED CONTRACT: RING_R 2.60 / TUBE_R 0.66 (throat 1.94, rim 3.26), FLOOR_Y,
the material-group names (patina/steel/dark/screen/led -- rifthub.cpp keys off
them), the operator panel's bay pose, the cradle and the feed cables. The gate does
not move and the hall is not touched.

CLEARANCES (checked in main(), the membrane must never clip the tube):
  bore floor  = RING_R - max_q  where max_q includes the flange ribs (+0.018)
                -> 1.922 vs membrane R 1.895.
  bolt heads are suppressed inside |v| > 140 deg so nothing protrudes into the bore.

LOCAL SPACE CONTRACT (must match rifthub.cpp's portal basis):
  gate stands in the local XY plane, +Y = 12 o'clock, hole axis = +Z, FRONT
  (hub-facing) = -Z. Ring center at the origin; the engine translates it to
  (cx, kRingY, cz), so the floor is local y = -kRingY. We author in that frame, then
  rotate +90 deg about X so Blender's Y-up glTF exporter lands it back in-contract.

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
FLOOR_Y   = -2.20     # world y = 0 in the local frame (engine kRingY)

# minor-angle landmarks (v): 0 = outward equator, -90 = FRONT (-Z, hub side),
# +90 = back, 180 = the throat.
V_FRONT   = -math.pi * 0.5
V_TRACK   = D(-152.0)     # the recessed indicator groove (inner-front shoulder)

# --- the plate system (the cross-section) ---
NV        = 16                     # 16 plates around the tube
V_STEP    = TAU / NV               # 22.5 deg
SEAM_GH   = D(1.05)                # seam groove: half-width of its FLAT FLOOR
SEAM_CH   = D(1.05)                # seam: angular width of the CHAMFER wall
SEAM_D    = 0.022                  # seam groove depth below nominal TUBE_R
# the k=0 seam is the INDICATOR GROOVE: wider + deeper, the engine's indicator
# line seats inside it (a recessed slit, never a ring of triangles).
TRACK_GH  = D(5.0)
TRACK_CH  = D(1.6)
TRACK_D   = 0.058

# --- the circumferential joins (along u) ---
NU        = 12                     # 12 joins; every OTHER one is a bolted flange
U_STEP    = TAU / NU
JOIN_GU   = 0.0096                 # plain seam: half-width of the flat groove floor
JOIN_CU   = 0.0046                 # plain seam: chamfer wall
JOIN_D    = 0.022                  # plain seam depth
RIB_JG    = 0.0060                 # flange: half-width of the JOINT groove floor
RIB_JC    = 0.0040                 # flange: joint groove chamfer
RIB_JD    = 0.026                  # flange: joint groove depth (below nominal)
RIB_W     = 0.0550                 # flange: half-width of the raised rib top
RIB_C     = 0.0070                 # flange: rib shoulder chamfer
RIB_H     = 0.018                  # flange: rib height ABOVE nominal
BOLT_U    = 0.030                  # bolt row offset from the joint line (on the rib)
BOLT_R    = 0.020
BOLT_VMAX = D(140.0)               # no bolts in the throat (they'd enter the bore)

U_BASE    = 160                    # base silhouette sampling (feature rings are added)

# operator panel bay (unchanged pose)
BAY_U     = D(-20.0)
BAY_V     = V_FRONT
BAY_DU    = 0.122         # half-extent in u  -> 0.63 m of tube arc
BAY_DV    = 0.345         # half-extent in v  -> 0.46 m of tube arc
BAY_WU    = 0.020         # crisp milled wall (u)
BAY_WV    = 0.050         # crisp milled wall (v)
BAY_DEPTH = 0.100         # recess depth (the panel lives INSIDE this)

# bolted-on vent housings (front-outer shoulder)
VENT_U    = [D(50.0), D(140.0), D(230.0), D(320.0)]
V_VENT    = D(-38.0)

# texture tiling: INTEGER in both axes (seamless wrap), ~0.60 m per tile both ways.
UT, VT    = 27.0, 7.0

GROUPS = {"patina": [], "steel": [], "dark": [], "screen": [], "led": []}
def register(ob, group):
    GROUPS[group].append(ob)
    return ob


# ---------------------------------------------------------------------------
# THE MACHINED CROSS-SECTION.
#
# A machined tube's section is a TURNED PROFILE: straight runs joined by straight
# chamfers, with CORNERS between them. We author it as an explicit polyline of
# (v, q) control points -- q = distance from the tube's centerline circle -- and the
# sweep interpolates LINEARLY between them. Linear interpolation between control
# points is what produces the C0 creases that R9's gauss()/smoothstep() could not.
#
# Per seam k (at v = V_TRACK + k*V_STEP) we emit, and per plate between two seams:
#     [seam center: FLOOR] [floor edge] [chamfer top] [plate center] [chamfer top]
#     [floor edge] [next seam center: FLOOR] ...
# The plate face between two chamfer tops is a true straight CHORD: in the polar
# frame of the cross-section a straight line at distance A from the centre is
# q = A / cos(dv), and taking A = TUBE_R makes the chord tangent to the nominal
# tube at the plate's midpoint -- i.e. the plates are the facets of a 16-gon
# circumscribing the tube, and the chamfers cut their corners off. Sampling that
# chord at its two ends + its midpoint is EXACT (all three lie on the line), so a
# plate face is dead flat with a constant normal, and adjacent plates step by
# 22.5 deg across a 68-deg chamfer. That step is the machining.
# ---------------------------------------------------------------------------
def wrap(a):
    return (a + math.pi) % TAU - math.pi

def seam_params(k):
    """(groove-floor half width, chamfer width, floor radius) for seam k."""
    if k % NV == 0:
        return TRACK_GH, TRACK_CH, TUBE_R - TRACK_D    # the indicator groove
    return SEAM_GH, SEAM_CH, TUBE_R - SEAM_D

def build_profile():
    """The tube's minor cross-section, as an ordered list of (v, q) samples over a
    full 2*pi (first sample repeats at +2*pi; we drop the duplicate)."""
    pts = []
    for k in range(NV):
        vs   = V_TRACK + k * V_STEP                  # this seam's centre
        vsn  = V_TRACK + (k + 1) * V_STEP            # the next seam's centre
        gh,  ch,  fl  = seam_params(k)
        ghn, chn, fln = seam_params(k + 1)

        # --- this seam's groove: flat floor, then the chamfer up to the plate ---
        pts.append((vs - gh, fl))                    # floor, entering edge
        pts.append((vs,      fl))                    # floor, centre (the dark line)
        pts.append((vs + gh, fl))                    # floor, leaving edge

        # --- the plate face: a straight chord between the two chamfer tops ---
        e0 = vs  + gh  + ch                          # chamfer top (start of plate)
        e1 = vsn - ghn - chn                         # chamfer top (end of plate)
        vc = 0.5 * (e0 + e1)                         # plate midpoint
        A  = TUBE_R                                  # apothem: chord tangent at vc
        for vv in (e0, vc, e1):
            pts.append((vv, A / math.cos(vv - vc)))
    # de-dup / sort into a clean monotone list over one turn
    pts.sort(key=lambda p: p[0])
    out = []
    for v, q in pts:
        if out and abs(v - out[-1][0]) < 1e-9:
            continue
        out.append((v, q))
    return out

PROFILE = build_profile()
V_SAMP  = [p[0] for p in PROFILE]
Q_SAMP  = [p[1] for p in PROFILE]
V0      = V_SAMP[0]

def q_profile(v):
    """Linear interpolation along the machined polyline (periodic in v)."""
    x = V0 + (v - V0) % TAU
    n = len(V_SAMP)
    # binary search
    lo, hi = 0, n
    while lo < hi:
        mid = (lo + hi) // 2
        if V_SAMP[mid] <= x:
            lo = mid + 1
        else:
            hi = mid
    i = lo - 1
    if i >= n - 1:
        v0, q0 = V_SAMP[n - 1], Q_SAMP[n - 1]
        v1, q1 = V_SAMP[0] + TAU, Q_SAMP[0]
    else:
        v0, q0 = V_SAMP[i], Q_SAMP[i]
        v1, q1 = V_SAMP[i + 1], Q_SAMP[i + 1]
    t = 0.0 if v1 - v0 < 1e-12 else (x - v0) / (v1 - v0)
    return q0 + t * (q1 - q0)


# ---------------------------------------------------------------------------
# THE CIRCUMFERENTIAL JOINS (a radial offset field along u).
#
# Same law as the cross-section: PIECEWISE LINEAR, so every wall creases. Even
# stations are bolted FLANGE RIBS (raised, chamfered, with a deep joint groove down
# the centre); odd stations are plain machined seams.
# ---------------------------------------------------------------------------
def _piecewise(a, knots):
    """knots = ascending [(|du|, offset)]; flat-extrapolate the last to 0 beyond."""
    if a >= knots[-1][0]:
        return 0.0
    for i in range(len(knots) - 1):
        x0, y0 = knots[i]
        x1, y1 = knots[i + 1]
        if a <= x1:
            if a <= x0:
                return y0
            t = (a - x0) / max(1e-12, x1 - x0)
            return y0 + t * (y1 - y0)
    return 0.0

PLAIN_KNOTS = [(0.0, -JOIN_D), (JOIN_GU, -JOIN_D), (JOIN_GU + JOIN_CU, 0.0)]
RIB_KNOTS   = [(0.0, -RIB_JD), (RIB_JG, -RIB_JD), (RIB_JG + RIB_JC, RIB_H),
               (RIB_W, RIB_H), (RIB_W + RIB_C, 0.0)]

def is_flange(k):
    return (k % 2) == 0

def join_offset(u):
    """Radial offset of the nearest circumferential join (+ = raised rib)."""
    k  = int(round(u / U_STEP))
    du = abs(wrap(u - k * U_STEP))
    return _piecewise(du, RIB_KNOTS if is_flange(k) else PLAIN_KNOTS)


# ---------------------------------------------------------------------------
# THE OPERATOR BAY: a crisply-walled MILLED POCKET. Its flat floor OVERRIDES the
# plate/seam/rib structure (a pocket milled after assembly erases what it cuts
# through), so we blend to a plain circular floor rather than subtracting a depth
# from the machined profile and leaving ghost ripples in the pocket.
# ---------------------------------------------------------------------------
def _trap(d, half, wall):
    a = abs(d)
    if a <= half:
        return 1.0
    if a >= half + wall:
        return 0.0
    return (half + wall - a) / wall          # LINEAR wall -> a crease, not a blend

def bay_mask(u, v):
    return (_trap(wrap(u - BAY_U), BAY_DU, BAY_WU) *
            _trap(wrap(v - BAY_V), BAY_DV, BAY_WV))

def rr(u, v):
    q  = q_profile(v) + join_offset(u)
    kb = bay_mask(u, v)
    if kb > 0.0:
        q = (1.0 - kb) * q + kb * (TUBE_R - BAY_DEPTH)
    return q

def surf(u, v):
    q = rr(u, v)
    rad = RING_R + q * math.cos(v)
    return Vector((rad * math.cos(u), rad * math.sin(u), q * math.sin(v)))

def nrm(u, v):
    """Outward normal of the nominal tube (good enough to seat bolted-on parts)."""
    return Vector((math.cos(v) * math.cos(u), math.cos(v) * math.sin(u), math.sin(v)))


def u_samples():
    """Base silhouette rings PLUS a ring exactly on every feature boundary. This is
    what buys crisp joins cheaply: R9 spent 256 uniform rings and still smeared the
    walls; we spend ~240 and land verts ON them."""
    s = set()
    for i in range(U_BASE):
        s.add(round(i * TAU / U_BASE, 9))
    for k in range(NU):
        u0 = k * U_STEP
        knots = RIB_KNOTS if is_flange(k) else PLAIN_KNOTS
        for x, _ in knots:
            s.add(round((u0 + x) % TAU, 9))
            s.add(round((u0 - x) % TAU, 9))
    for x in (BAY_DU, BAY_DU + BAY_WU):
        s.add(round((BAY_U + x) % TAU, 9))
        s.add(round((BAY_U - x) % TAU, 9))
    return sorted(s)

def v_samples():
    """The profile's own control points, plus the bay's milled v-walls."""
    s = set(round(v, 9) for v in V_SAMP)
    for x in (BAY_DV, BAY_DV + BAY_WV):
        s.add(round(wrap(BAY_V + x), 9))
        s.add(round(wrap(BAY_V - x), 9))
    # keep the list ordered starting from V0 and covering exactly one turn
    return sorted(V0 + (v - V0) % TAU for v in s)


def build_tube():
    """Revolve the machined profile into ONE closed, watertight, arc-length-UV'd mesh.
    No booleans; the topology is a plain torus grid, so winding is uniform and the
    outward normal is guaranteed by construction (see normals_outward)."""
    us = u_samples()
    vs = v_samples()
    NUS, NVS = len(us), len(vs)

    # V by ARC LENGTH along the cross-section polyline (so chamfers and groove
    # floors get their fair share of texels instead of being squeezed to nothing).
    pts2 = [(q_profile(v) * math.cos(v), q_profile(v) * math.sin(v)) for v in vs]
    cum = [0.0]
    for j in range(1, NVS + 1):
        a = pts2[j - 1]
        b = pts2[j % NVS]
        cum.append(cum[-1] + math.hypot(b[0] - a[0], b[1] - a[1]))
    total = cum[-1]

    me = bpy.data.meshes.new("gate_tube")
    verts, faces, uvs = [], [], []
    for i in range(NUS):
        for j in range(NVS):
            verts.append(surf(us[i], vs[j]))
    def vid(i, j):
        return (i % NUS) * NVS + (j % NVS)
    for i in range(NUS):
        u0 = us[i] / TAU * UT
        u1 = (us[i + 1] if i + 1 < NUS else TAU) / TAU * UT
        for j in range(NVS):
            a, b, c, d = vid(i, j), vid(i + 1, j), vid(i + 1, j + 1), vid(i, j + 1)
            faces.append((a, b, c, d))
            v0 = cum[j] / total * VT
            v1 = cum[j + 1] / total * VT
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
    # 30 deg: BELOW every machined crease we author (plate->chamfer 68 deg,
    # chamfer->floor 68 deg, rib shoulder 45 deg, pocket wall 60+ deg) and ABOVE the
    # tube's own curvature step (2.25 deg), so the plates stay smooth along the sweep
    # and EVERY chamfer becomes a hard edge with its own specular line.
    md = ob.modifiers.new("es", 'EDGE_SPLIT')
    md.split_angle = D(30)
    bpy.ops.object.modifier_apply(modifier=md.name)
    register(ob, "patina")
    log("tube: %d verts / %d quads (u rings=%d, v samples=%d; R9 was a uniform 256x144)"
        % (len(verts), len(faces), NUS, NVS))
    log("      profile: %d plates, seam %.0fmm wide x %.0fmm deep, chamfer slope ~%.0f deg"
        % (NV, math.degrees(SEAM_GH) * 2 * TUBE_R * TAU / 360.0 * 1000.0 / 1.0,
           SEAM_D * 1000.0,
           math.degrees(math.atan2(SEAM_D + (TUBE_R / math.cos(SEAM_CH) - TUBE_R),
                                   SEAM_CH * TUBE_R))))
    return ob


# ---------------------------------------------------------------------------
# Small helpers for the bolted-on parts.
# ---------------------------------------------------------------------------
def _finish(ob, bev=0.0, smooth=True, split=30):
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
    md.split_angle = D(split)
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
# BOLTED ON, NOT SCULPTED IN.
#
# frame(u, v, lift) is the surface frame at a point on the tube: x = along the tube,
# y = up the tube's face, z = out of the surface. Parts are authored flat in that
# frame and stamped onto the gate -- so a housing sits on the hull the way a real
# one would, and its fasteners land on the hull, not floating.
# ---------------------------------------------------------------------------
def frame(u, v, lift=0.0):
    n = nrm(u, v)
    p = surf(u, v) + n * lift
    t  = Vector((-math.sin(u), math.cos(u), 0.0))          # along the tube
    b  = n.cross(t).normalized()                            # up the face
    m = Matrix().to_4x4()
    for r in range(3):
        m[r][0] = t[r]; m[r][1] = b[r]; m[r][2] = n[r]; m[r][3] = p[r]
    return m

_STAMP = None
def stamp(ob):
    ob.matrix_world = _STAMP @ ob.matrix_world
    bpy.ops.object.select_all(action='DESELECT')
    ob.select_set(True)
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    return ob

def set_stamp(m):
    global _STAMP
    _STAMP = m

def bolt(name, u, v, lift, r=BOLT_R, h=0.026):
    """A HEX head, half-buried in the hull, standing proud by ~h/2. Six-sided on
    purpose: at player distance a 6-gon head reads as a FASTENER; a smooth 12-gon
    reads as a stud, and a bump reads as nothing at all."""
    n = nrm(u, v)
    p = surf(u, v) + n * (lift + h * 0.30)
    ob = cyl(name, r, h, (p.x, p.y, p.z), rot=aim(n), verts=6, bev=0.0035)
    return register(ob, "steel")


def build_flange_bolts():
    """A real bolt row either side of every flange joint, one per plate, suppressed
    inside the throat so nothing intrudes on the membrane's bore."""
    n = 0
    for k in range(NU):
        if not is_flange(k):
            continue
        u0 = k * U_STEP
        for pk in range(NV):
            vs   = V_TRACK + pk * V_STEP
            vsn  = V_TRACK + (pk + 1) * V_STEP
            gh, ch, _   = seam_params(pk)
            ghn, chn, _ = seam_params(pk + 1)
            vc = 0.5 * ((vs + gh + ch) + (vsn - ghn - chn))     # plate centre
            if abs(wrap(vc)) > BOLT_VMAX:
                continue                                        # throat: no bolts
            for s in (-1, 1):
                bolt("bolt_%d_%d_%d" % (k, pk, s), u0 + s * BOLT_U, vc, RIB_H)
                n += 1
    log("flange bolts:", n, "(6 bolted joins x 2 rows; throat suppressed)")


def build_vents():
    """Machined VENT HOUSINGS, bolted to the front-outer shoulder. R9 sank louvres
    into the hull with smoothstep walls, which is to say it sank nothing at all."""
    for idx, u0 in enumerate(VENT_U):
        set_stamp(frame(u0, V_VENT))
        # chamfered base plate (the housing flange)
        stamp(register(cube("vent%d_base" % idx, 0.150, 0.098, 0.012, (0, 0, 0.010),
                            bev=0.008), "steel"))
        # a raised, chamfered body with 4 louvre bars cut across it
        stamp(register(cube("vent%d_body" % idx, 0.118, 0.070, 0.020, (0, 0, 0.026),
                            bev=0.006), "dark"))
        for j in range(4):
            y = 0.045 - j * 0.030
            stamp(register(cube("vent%d_lv%d" % (idx, j), 0.108, 0.009, 0.010,
                                (0, y, 0.044), rot=(D(28), 0, 0), bev=0.003), "steel"))
        # corner fasteners: this thing was BOLTED ON
        for sx in (-1, 1):
            for sy in (-1, 1):
                stamp(register(cyl("vent%d_b%d%d" % (idx, sx, sy), 0.014, 0.016,
                                   (sx * 0.132, sy * 0.080, 0.018), verts=6,
                                   bev=0.002), "steel"))
    log("vent housings:", len(VENT_U), "(bolted on, 4 louvres + 4 fasteners each)")


# ---------------------------------------------------------------------------
# THE OPERATOR PANEL, set INTO the tube's milled bay, with a BOLTED BEZEL FRAME
# around the pocket rim.
# ---------------------------------------------------------------------------
def build_panel():
    # --- bolted bezel frame around the pocket rim (sits on the HULL, not the floor)
    set_stamp(frame(BAY_U, BAY_V))
    for (bx, by, hx, hy) in ((0.0,  0.255, 0.360, 0.038),
                             (0.0, -0.255, 0.360, 0.038),
                             (-0.322, 0.0, 0.038, 0.218),
                             (0.322, 0.0,  0.038, 0.218)):
        stamp(register(cube("bay_frame_%d_%d" % (int(bx * 100), int(by * 100)),
                            hx, hy, 0.016, (bx, by, 0.012), bev=0.007), "steel"))
    for sx in (-1, 1):
        for sy in (-1, 1):
            stamp(register(cyl("bay_bolt%d%d" % (sx, sy), 0.015, 0.018,
                               (sx * 0.322, sy * 0.255, 0.020), verts=6, bev=0.002),
                           "steel"))

    # --- the panel itself, on the pocket FLOOR ---
    set_stamp(frame(BAY_U, BAY_V, lift=-BAY_DEPTH))
    p = _STAMP.translation
    log("panel bay floor at local (%.3f, %.3f, %.3f)" % (p.x, p.y, p.z))

    stamp(register(cube("panel_plate", 0.275, 0.195, 0.008, (0, 0, 0.008)), "dark"))
    stamp(register(cube("panel_lcd", 0.150, 0.105, 0.006, (-0.105, 0.045, 0.019)),
                   "screen"))
    for (bx, by, hx, hy) in ((0, 0.118, 0.166, 0.014),
                             (0, -0.118, 0.166, 0.014),
                             (-0.166, 0, 0.014, 0.132),
                             (0.166, 0, 0.014, 0.132)):
        stamp(register(cube("panel_bez_%d_%d" % (int(bx * 100), int(by * 100)),
                            hx, hy, 0.014, (-0.105 + bx, 0.045 + by, 0.022)), "steel"))
    for j in range(2):
        for i in range(4):
            bx = 0.095 + i * 0.058
            by = 0.100 - j * 0.062
            stamp(register(cyl("panel_btn%d%d" % (i, j), 0.024, 0.030, (bx, by, 0.020),
                               verts=14, bev=0.004), "steel"))
            stamp(register(cyl("panel_cap%d%d" % (i, j), 0.017, 0.012, (bx, by, 0.038),
                               verts=14, bev=0.003), "led"))
    stamp(register(cyl("panel_engage", 0.036, 0.034, (0.185, -0.055, 0.021),
                       verts=18, bev=0.005), "steel"))
    stamp(register(cyl("panel_engage_cap", 0.026, 0.014, (0.185, -0.055, 0.042),
                       verts=18, bev=0.004), "led"))
    for k in range(3):
        stamp(register(cube("panel_led%d" % k, 0.140, 0.008, 0.005,
                            (-0.105, -0.078 - k * 0.024, 0.016)), "led"))
    for k in range(2):
        stamp(register(cyl("panel_lamp%d" % k, 0.011, 0.010,
                           (-0.262, 0.100 - k * 0.048, 0.016), verts=10, bev=0.002),
                       "led"))
    stamp(register(cube("panel_grille", 0.115, 0.010, 0.010, (0.155, -0.130, 0.014)),
                   "steel"))


# ---------------------------------------------------------------------------
# CRADLE — the gate is INSTALLED. Minimal: a round pad, two curved legs, one
# trunnion through the tube's lower flanks, and BOLTED trunnion saddles.
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
        # bolted foot plate: 4 fasteners into the deck
        for a in range(4):
            th = D(45.0) + a * D(90.0)
            register(cyl("footbolt%d_%d" % (s, a), 0.026, 0.030,
                         (s * 1.90 + 0.33 * math.cos(th), FLOOR_Y + 0.13,
                          -0.02 + 0.33 * math.sin(th)),
                         rot=aim((0.0, 1.0, 0.0)), verts=6, bev=0.004), "steel")


# ---------------------------------------------------------------------------
# FEED CABLES — conduits entering the tube's lower flanks, each landing under a
# BOLTED saddle clamp (a conduit that just vanishes into a hull reads as a cheat).
# ---------------------------------------------------------------------------
def build_cables():
    for s in (-1, 1):
        for k in range(3):
            off = 0.10 * (k - 1)
            th = D(250.0) if s < 0 else D(290.0)
            uu = th + D(4.0) * (k - 1)
            entry = surf(uu, D(-70.0))
            pts = [(s * (1.15 + off), FLOOR_Y + 0.12, -0.34 + off * 0.4),
                   (s * (1.42 + off), FLOOR_Y + 0.85, -0.40),
                   (s * (1.72 + off), -1.30, -0.42),
                   (entry.x * 0.94, entry.y * 0.94, entry.z - 0.05),
                   (entry.x, entry.y, entry.z)]
            register(pipe("cable%d_%d" % (s, k), pts, 0.052), "dark")
        # a machined saddle where the three conduits enter the hull, with fasteners
        uu = (D(250.0) if s < 0 else D(290.0))
        set_stamp(frame(uu, D(-70.0)))
        stamp(register(cube("saddle%d" % s, 0.185, 0.085, 0.018, (0, 0, 0.010),
                            bev=0.008), "steel"))
        for sx in (-1, 1):
            stamp(register(cyl("saddlebolt%d_%d" % (s, sx), 0.014, 0.018,
                               (sx * 0.160, 0.0, 0.018), verts=6, bev=0.002), "steel"))
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
    "outward" per CONNECTED ISLAND, and EDGE_SPLIT (which we need for the machined
    chamfers) DISCONNECTS every face whose crease exceeds the split angle — so the
    deep cut features become their own little open islands with no inside, and the
    recalc happily INVERTS them. The sweep in build_tube() winds every quad
    (i,j)->(i+1,j)->(i+1,j+1)->(i,j+1), whose normal is the outward tube normal by
    construction for ANY radius field q(u,v) > 0. So the tube needs no recalc; it
    needs to be LEFT ALONE. verify_tube_outward() proves it.
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
    backface-culled, unlightable) must never come back. A centroid test is
    MEANINGLESS on a torus, so we test against the tube's ANALYTIC AXIS: for any
    point P the outward direction is P minus the nearest point on the centerline
    circle. Every face normal must have a POSITIVE dot with it. (The object has
    ALREADY been rotated +90 deg about X for the Y-up export; un-rotate first.)"""
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
        c = xy.normalized() * RING_R
        if (p - c).dot(n) <= 0.0:
            bad += 1
            d = p - c
            v = math.degrees(math.atan2(d.z, d.dot(xy.normalized())))
            k = int(round(v / 5.0)) * 5
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


def check_bore():
    """The membrane (R 1.895) must never clip the tube. Sample the throat."""
    worst = 1e9
    for i in range(720):
        u = i * TAU / 720
        for j in range(181):
            v = math.pi * (0.5 + j / 360.0)          # v in [90, 270] deg: the inner half
            q = rr(u, wrap(v))
            worst = min(worst, RING_R + q * math.cos(wrap(v)))
    return worst


def main():
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()

    bore = check_bore()
    log("BORE CHECK: innermost tube point = %.4f m (membrane R = 1.895) -> %s"
        % (bore, "clear by %.0f mm" % ((bore - 1.895) * 1000.0)
           if bore > 1.895 else "*** MEMBRANE CLIPS THE TUBE ***"))

    build_tube()
    build_flange_bolts()
    build_vents()
    build_panel()
    build_cradle()
    build_cables()
    log("authored objects:", sum(len(v) for v in GROUPS.values()))

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
