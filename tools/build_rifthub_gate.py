"""
build_rifthub_gate.py — ROUND 11: the RIFTHUB gate is *ONE LARGE MACHINED STEEL TUBE*.

Owner (verbatim, R7 + R8 + R10):
    "the gate.. is supposed to be ONE LARGE metallic Tube"
    "No chevrons needed"
    "build the tube, with an lcd panel on it, some buttons, glowing led displays"
    R10 (reference render of machined steel under an overcast sky):
    "There are no lights. just shiny reflections from the white cloudy sky."

    blender-launcher.exe --background --python tools/build_rifthub_gate.py -- <out.glb>

THE ROUND-BY-ROUND FAILURE LADDER (each one only became visible once the last was fixed)
----------------------------------------------------------------------------------------
R9  = BLACK.       The gate was instanced through a MIRROR (basis det -1), so back-face
                   culling drew the tube's inner shell. Fixed on art/rifthub-canon.
R10 = MARSHMALLOW. Every feature was authored with gauss() / smoothstep -- both C1, so
                   nothing creased, EDGE_SPLIT never fired, and with no crease there is
                   no specular EDGE LINE. A smooth dent on a round tube is not a groove;
                   it is QUILTING, which is how a cushion is made. Fixed by making the
                   section a TURNED LATHE PROFILE: flat plate chords, LINEAR chamfers,
                   flat groove floors -- C0 joints that crease and catch light.
R11 = TYRE.        <-- THIS ROUND. The machining was right, but the FORM was a wheel.

WHY R10 READ AS A TYRE, AND WHAT R11 DOES ABOUT IT
----------------------------------------------------------------------------------------
Two causes, both structural, neither fixable by surface detail:

 1. THE SEAMS SWEPT UNBROKEN RINGS. R10's 16 plate seams sat at the same v in EVERY
    circumferential band, so each one ran continuously all the way around the ring.
    Continuous circumferential grooves at a regular pitch is not "a hint of tread" --
    it is the literal, exact geometry of tyre tread.
    => R11-1: BRICK BOND. Alternate bands offset the plate grid by HALF A PLATE, so
    every seam DIES at the next circumferential join and picks up half a plate over.
    The plates INTERLOCK, like every real plated hull ever built. No groove survives a
    band boundary, so no tread line can form. The phase flips at a join CENTRE, which
    is the floor of a groove on every join type -- exactly where a real staggered butt
    joint hides its step. The bolt rows across a flange come out staggered for free.

 2. THE SECTION WAS A PERFECT CIRCLE. A circular tube of this girth IS the silhouette
    of a wheel, and no surface detail argues with a silhouette.
    => R11-3: the section is a SUPERELLIPSE (squircle, n=3) -- a machined CASING with a
    flat outer face, a flat FRONT face (the one the player stands in front of), a flat
    back, a flat throat and cut corners between. Same mass, same bore, same outer rim
    ("ONE LARGE metallic Tube" is intact) -- but it reads as industrial HOUSING.

 3. R11-2: FEWER, CHUNKIER, IRREGULAR JOINS. R10's 12 evenly-spaced joins repeated at
    tread frequency. Now 8 joins at UNEVEN angles, only 3 of them heavy bolted
    structural FLANGES (the ring is cast in 3 big segments). Repetition at tread
    frequency is the enemy; an irregular structural rhythm is the friend.

 4. R11-4: BREAK THE SYMMETRY. A rotationally-uniform ring of identical plates is a
    wheel; a machine has a heavy base, service gear a technician can reach, and cables
    entering at ONE point. Added: a HEAVY LOWER BASE CASTING (the R10 cradle was a thin
    pad on two spindly legs -- an axle stand holding a wheel), a junction box on one
    flank only, a bolted access hatch elsewhere, and vents clustered on the UPPER flanks
    at irregular angles. Nothing is mirrored; nothing sits on a rotational rhythm.

KEPT FROM R10 (do not regress): the C0 chamfer creases and their specular edge lines;
adaptive sampling (verts land ON feature boundaries); bolts/flanges as REAL geometry
bolted ON, never sculpted in; UV density (UT=27 / VT=7, both INTEGER so the wrap is
seamless, ~0.60 m per tile isotropic, V laid out by ARC LENGTH); no chevrons, no dashes,
no hazard ring; the ONE continuous recessed INDICATOR GROOVE (cut independently of the
plate grid so the brick offset can never move it).

UNCHANGED CONTRACT: RING_R 2.60 / TUBE_R 0.66 (throat 1.94, rim 3.26), FLOOR_Y, the
material-group names (patina/steel/dark/screen/led -- rifthub.cpp keys off them), the
operator panel's bay pose, the feed cables. The gate does not move; the hall is not
touched; collision is procedural (the GLB is visual only).

CLEARANCES (checked in main(); the membrane must never clip the tube): the flange rib
RAISE tapers to zero across the throat (THROAT_A/THROAT_B) and bolt heads are suppressed
inside |v| > 138 deg, so nothing can protrude into the bore.

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
TUBE_R    = 0.66      # tube "radius" (minor)  -> throat 1.94, outer rim 3.26
FLOOR_Y   = -2.20     # world y = 0 in the local frame (engine kRingY)

# minor-angle landmarks (v): 0 = outward equator, -90 = FRONT (-Z, hub side),
# +90 = back, 180 = the throat.
V_FRONT   = -math.pi * 0.5
V_TRACK   = D(-152.0)     # the recessed indicator groove (inner-front shoulder)

# ---------------------------------------------------------------------------
# R11-3: THE CROSS-SECTION IS A MACHINED CASING, NOT A TORUS.
#
# Half the "tyre" read was the section itself: a perfectly circular tube of this girth
# IS the silhouette of a wheel, and no amount of surface detail argues with a
# silhouette. A SUPERELLIPSE (squircle, exponent n) keeps the mass the owner asked for
# -- "ONE LARGE metallic Tube", and the bore and outer rim are UNCHANGED -- but gives
# the section FLAT FACES with chamfered shoulders: a flat outer face at v=0, a flat
# FRONT face at v=-90 (the one the player stands in front of), a flat back, a flat
# throat, and cut corners between. That is an industrial HOUSING, not a wheel.
SQUIRCLE_N = 3.0
# A squircle is FATTER than its circle between the axes (n=3 peaks ~1.14x at 45 deg),
# and the plate chords circumscribe it on top of that (+2.5% at a plate edge). Both
# eat the THROAT: unscaled, the innermost point landed at 1.8965 m against a membrane
# at 1.895 -- a 2 mm clearance, i.e. a z-fight waiting to happen. Scale the section so
# the bore keeps a real margin. Costs 18 mm off the outer rim (3.260 -> 3.242), which
# is invisible; buys ~20 mm of bore, which is not.
SECTION_S  = 0.972
def R_base(v):
    c = abs(math.cos(v)); sn = abs(math.sin(v))
    return (TUBE_R * SECTION_S) * (c ** SQUIRCLE_N + sn ** SQUIRCLE_N) ** (-1.0 / SQUIRCLE_N)

# --- the plate system (the cross-section) ---
NV        = 12                     # 12 plates around the section (was 16: chunkier)
V_STEP    = TAU / NV               # 30 deg -> ~0.35 m of plate
SEAM_GH   = D(1.15)                # seam groove: half-width of its FLAT FLOOR
SEAM_CH   = D(1.15)                # seam: angular width of the CHAMFER wall
SEAM_D    = 0.024                  # seam groove depth below the casing surface

# R11-1: BRICK BOND. THE point of this round. In R10 the 16 plate seams sat at the same
# v in EVERY circumferential band, so each one swept an UNBROKEN GROOVE ALL THE WAY
# AROUND THE RING -- which is precisely and exactly the geometry of tyre tread. Real
# plated hulls stagger their joints (brick bond / shifted butt joints) so no seam runs
# continuously: the plates INTERLOCK. Alternate bands are offset by HALF A PLATE, so
# every seam DIES at the next circumferential join and picks up half a plate over. The
# tread lines cannot form, because no line survives a band boundary.
BRICK     = V_STEP * 0.5

# The ONE continuous circumferential line left is the INDICATOR GROOVE, and that is
# canon (the recessed slit the engine's indicator line seats in). One is a machined
# detail; sixteen is a tyre. It is cut INDEPENDENTLY of the plate grid, so the brick
# offset can never move it.
TRACK_GH  = D(5.0)
TRACK_CH  = D(1.6)
TRACK_D   = 0.058

# --- R11-2: FEWER, CHUNKIER, IRREGULAR CIRCUMFERENTIAL JOINS ----------------
# R10 had 12 evenly-spaced joins -- a repeat at exactly tread frequency. Repetition at
# tread frequency is the enemy; an irregular STRUCTURAL rhythm is the friend. 8 joins
# at uneven angles, of which only 3 are heavy bolted structural FLANGES (the ring is
# built from 3 big cast segments); the rest are plain machined butt seams.
JOIN_DEG  = [0.0, 48.0, 96.0, 140.0, 186.0, 232.0, 280.0, 330.0]
JOIN_U    = [D(a) for a in JOIN_DEG]
NBAND     = len(JOIN_U)                     # must be EVEN so the brick phase wraps
FLANGE    = set([0, 2, 5])                  # heavy flanges at 0, 96, 232 deg

JOIN_GU   = 0.0100                 # plain seam: half-width of the flat groove floor
JOIN_CU   = 0.0048                 # plain seam: chamfer wall
JOIN_D    = 0.024                  # plain seam depth
RIB_JG    = 0.0065                 # flange: half-width of the JOINT groove floor
RIB_JC    = 0.0042                 # flange: joint groove chamfer
RIB_JD    = 0.030                  # flange: joint groove depth (below the casing)
RIB_W     = 0.0760                 # flange: half-width of the raised rib top (CHUNKY)
RIB_C     = 0.0085                 # flange: rib shoulder chamfer
RIB_H     = 0.030                  # flange: rib height ABOVE the casing (HEAVY)
BOLT_U    = 0.040                  # bolt row offset from the joint line (on the rib)
BOLT_R    = 0.022
BOLT_VMAX = D(138.0)               # no bolts in the throat (they would enter the bore)
THROAT_A  = D(140.0)               # the flange RIB tapers out across the throat so it
THROAT_B  = D(166.0)               # can never eat the membrane's bore clearance

U_BASE    = 140                    # base silhouette sampling (feature rings are added)

# operator panel bay (unchanged pose)
BAY_U     = D(-20.0)
BAY_V     = V_FRONT
BAY_DU    = 0.122
BAY_DV    = 0.345
BAY_WU    = 0.020
BAY_WV    = 0.050
BAY_DEPTH = 0.100

# --- R11-4: BREAK THE SYMMETRY ----------------------------------------------
# A perfectly rotationally-uniform ring of identical plates IS a wheel. A machine is
# not uniform: it has a heavy base where it carries its load, service gear where a
# technician can reach it, and cable runs entering at ONE point. Greeble density
# differs top vs bottom, and NOTHING here is mirrored.
VENT_U    = [D(62.0), D(118.0), D(155.0)]   # 3 vents, UPPER flanks only, irregular
V_VENT    = D(-38.0)
JBOX_U    = D(205.0)                        # junction box: one side only
JBOX_V    = D(-58.0)
HATCH_U   = D(78.0)                         # bolted access hatch: upper-left, alone
HATCH_V   = D(-44.0)

# texture tiling: INTEGER in both axes (seamless wrap), ~0.60 m per tile both ways.
UT, VT    = 27.0, 7.0

GROUPS = {"patina": [], "steel": [], "dark": [], "screen": [], "led": []}
def register(ob, group):
    GROUPS[group].append(ob)
    return ob

def wrap(a):
    return (a + math.pi) % TAU - math.pi


# ---------------------------------------------------------------------------
# THE BRICK-BONDED PLATE PROFILE.
#
# Same machined law as R10 -- FLAT PLATE CHORD -> LINEAR CHAMFER -> FLAT GROOVE FLOOR,
# all C0, so every joint creases and EDGE_SPLIT gives it a specular edge line -- but
# the seam PHASE now depends on which circumferential band we are in, so the seams
# INTERLOCK instead of sweeping rings.
#
# Each plate is a true straight CHORD tangent to the casing at its midpoint:
# q = A / cos(dv) is exactly a straight line in the section's polar frame, with
# A = R_base(plate centre). Sampling it at its ends + midpoint is EXACT.
# ---------------------------------------------------------------------------
def band_of(u):
    """Index of the circumferential band u falls in (bands are separated by joins)."""
    x = u % TAU
    b = 0
    for i in range(NBAND):
        if x >= JOIN_U[i]:
            b = i
    return b

def phase_of(band):
    """BRICK BOND: alternate bands step the plate grid by half a plate."""
    return BRICK if (band % 2) else 0.0

def q_plates(v, phase):
    """The plate / chamfer / seam profile at minor angle v for a given brick phase."""
    half = V_STEP * 0.5
    k    = round((v - phase) / V_STEP)
    sk   = phase + k * V_STEP                 # nearest seam centre
    ds   = wrap(v - sk)
    floor = R_base(v) - SEAM_D                # the seam's flat machined floor
    if abs(ds) <= SEAM_GH:
        return floor
    vc = sk + (half if ds > 0.0 else -half)   # this plate's centre
    A  = R_base(vc)                           # chord tangent to the casing at vc
    E0 = half - SEAM_GH - SEAM_CH             # plate face half-width
    dv = wrap(v - vc)
    d  = abs(dv)
    if d <= E0:
        return A / math.cos(dv)
    qe = A / math.cos(E0)                     # the plate's edge, before the chamfer
    t  = (d - E0) / max(1e-9, SEAM_CH)        # LINEAR ramp -> a crease at both ends
    t  = min(max(t, 0.0), 1.0)
    return qe + (floor - qe) * t

def q_profile(v, phase):
    q = q_plates(v, phase)
    # THE INDICATOR GROOVE: cut independently of the plate grid (the brick offset must
    # never move it), with the same linear-walled, flat-floored machining.
    d  = abs(wrap(v - V_TRACK))
    tf = R_base(v) - TRACK_D
    if d <= TRACK_GH:
        return tf
    if d <= TRACK_GH + TRACK_CH:
        t = (d - TRACK_GH) / TRACK_CH
        return tf + (q - tf) * t
    return q


# ---------------------------------------------------------------------------
# THE CIRCUMFERENTIAL JOINS (a radial offset along u). Piecewise LINEAR, so every wall
# creases. 3 heavy bolted flange ribs + 5 plain machined seams, irregularly spaced. The
# rib RAISE tapers out across the throat so it can never intrude on the membrane's
# bore; the grooves stay cut everywhere.
# ---------------------------------------------------------------------------
def _piecewise(a, knots):
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

def throat_fade(v):
    a = abs(wrap(v))
    if a <= THROAT_A: return 1.0
    if a >= THROAT_B: return 0.0
    t = (THROAT_B - a) / (THROAT_B - THROAT_A)
    return t * t * (3.0 - 2.0 * t)

def nearest_join(u):
    bd = 1e9; ji = 0
    for i, u0 in enumerate(JOIN_U):
        d = abs(wrap(u - u0))
        if d < bd:
            bd = d; ji = i
    return ji, bd

def join_offset(u, v):
    ji, bd = nearest_join(u)
    off = _piecewise(bd, RIB_KNOTS if ji in FLANGE else PLAIN_KNOTS)
    if off > 0.0:                      # only the RAISED rib fades; grooves stay cut
        off *= throat_fade(v)
    return off


def _trap(d, half, wall):
    a = abs(d)
    if a <= half:  return 1.0
    if a >= half + wall: return 0.0
    return (half + wall - a) / wall          # LINEAR wall -> a crease, not a blend

def bay_mask(u, v):
    return (_trap(wrap(u - BAY_U), BAY_DU, BAY_WU) *
            _trap(wrap(v - BAY_V), BAY_DV, BAY_WV))

def rr(u, v):
    q  = q_profile(v, phase_of(band_of(u))) + join_offset(u, v)
    kb = bay_mask(u, v)
    if kb > 0.0:
        q = (1.0 - kb) * q + kb * (R_base(v) - BAY_DEPTH)
    return q

def surf(u, v):
    q = rr(u, v)
    rad = RING_R + q * math.cos(v)
    return Vector((rad * math.cos(u), rad * math.sin(u), q * math.sin(v)))

def nrm(u, v):
    return Vector((math.cos(v) * math.cos(u), math.cos(v) * math.sin(u), math.sin(v)))


def u_samples():
    """Base rings PLUS a ring exactly on every join breakpoint. The brick phase FLIPS
    at a join CENTRE -- which is the floor of a groove on every join type, plain or
    flange -- so the half-plate step between bands is hidden down inside the joint,
    exactly where a real staggered butt joint hides it."""
    s = set()
    for i in range(U_BASE):
        s.add(round(i * TAU / U_BASE, 9))
    for i, u0 in enumerate(JOIN_U):
        knots = RIB_KNOTS if i in FLANGE else PLAIN_KNOTS
        for x, _ in knots:
            s.add(round((u0 + x) % TAU, 9))
            s.add(round((u0 - x) % TAU, 9))
        s.add(round(u0 % TAU, 9))
    for x in (BAY_DU, BAY_DU + BAY_WU):
        s.add(round((BAY_U + x) % TAU, 9))
        s.add(round((BAY_U - x) % TAU, 9))
    return sorted(s)

def v_samples():
    """The union of BOTH brick phases' control points. One v-grid serves every band: a
    control point from the other phase lands on THIS phase's flat chord, where it is
    exactly collinear -- so it costs a vertex and distorts nothing."""
    s = set()
    half = V_STEP * 0.5
    for phase in (0.0, BRICK):
        for k in range(NV):
            sk = phase + k * V_STEP
            for x in (-SEAM_GH - SEAM_CH, -SEAM_GH, 0.0, SEAM_GH, SEAM_GH + SEAM_CH):
                s.add(round(wrap(sk + x), 9))
            s.add(round(wrap(sk + half), 9))          # plate centre
    for x in (-TRACK_GH - TRACK_CH, -TRACK_GH, 0.0, TRACK_GH, TRACK_GH + TRACK_CH):
        s.add(round(wrap(V_TRACK + x), 9))
    for x in (BAY_DV, BAY_DV + BAY_WV):
        s.add(round(wrap(BAY_V + x), 9))
        s.add(round(wrap(BAY_V - x), 9))
    v0 = min(s)
    return sorted(v0 + (v - v0) % TAU for v in s)


def build_tube():
    """Revolve the brick-bonded machined casing into ONE closed, watertight,
    arc-length-UV'd mesh. Topology stays a plain torus grid, so winding is uniform and
    the outward normal is guaranteed by construction (see normals_outward)."""
    us = u_samples()
    vs = v_samples()
    NUS, NVS = len(us), len(vs)

    # V by ARC LENGTH along the section (phase 0 as the reference), so chamfers and
    # groove floors get their fair share of texels instead of being squeezed to nothing.
    pts2 = [(q_profile(v, 0.0) * math.cos(v), q_profile(v, 0.0) * math.sin(v)) for v in vs]
    cum = [0.0]
    for j in range(1, NVS + 1):
        a = pts2[j - 1]; b = pts2[j % NVS]
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
    md = ob.modifiers.new("es", 'EDGE_SPLIT')
    md.split_angle = D(30)
    bpy.ops.object.modifier_apply(modifier=md.name)
    register(ob, "patina")
    log("tube: %d verts / %d quads (u rings=%d, v samples=%d)"
        % (len(verts), len(faces), NUS, NVS))
    log("      section: SQUIRCLE n=%.1f -- a machined casing, NOT a torus" % SQUIRCLE_N)
    log("      plates : %d/band, BRICK-BONDED (phase alternates by %.1f deg) -> "
        "no seam survives a band boundary" % (NV, math.degrees(BRICK)))
    log("      joins  : %d irregular [%s], of which %d are heavy bolted flanges"
        % (NBAND, ",".join("%.0f" % a for a in JOIN_DEG), len(FLANGE)))
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
    """A real bolt row either side of each of the 3 HEAVY flange joints. The bolts land
    on the PLATE CENTRES of the band on that side -- and because the bands are
    brick-bonded, the two rows across one flange are STAGGERED relative to each other.
    That is what a real staggered joint looks like, and it is one more thing a tyre
    cannot do. Suppressed inside the throat so nothing intrudes on the bore."""
    n = 0
    half = V_STEP * 0.5
    for ji in sorted(FLANGE):
        u0 = JOIN_U[ji]
        for s in (-1, 1):
            # the band on THIS side of the joint (and therefore ITS brick phase)
            ub    = u0 + s * BOLT_U
            phase = phase_of(band_of(ub % TAU))
            for pk in range(NV):
                vc = phase + pk * V_STEP + half          # a plate centre in that band
                if abs(wrap(vc)) > BOLT_VMAX:
                    continue                             # throat: no bolts
                bolt("bolt_%d_%d_%d" % (ji, pk, s), ub, wrap(vc), RIB_H)
                n += 1
    log("flange bolts:", n, "(3 heavy joins x 2 rows, rows STAGGERED by the brick bond)")


def build_vents():
    """Machined VENT HOUSINGS, bolted on. R9 sank louvres into the hull with smoothstep
    walls, which is to say it sank nothing at all. R11: they cluster on the UPPER
    flanks only -- greeble density differs top vs bottom, because a machine is not
    rotationally uniform and a wheel is."""
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
# R11-4: THE ASYMMETRIC HARDWARE. A junction box on ONE side, a bolted access hatch
# somewhere else entirely. Nothing mirrored, nothing on a rotational rhythm — the eye
# reads "a machine somebody maintains", which a wheel never is.
# ---------------------------------------------------------------------------
def build_service_gear():
    # --- JUNCTION BOX (one flank only): a chunky bolted casting with a conduit gland
    set_stamp(frame(JBOX_U, JBOX_V))
    stamp(register(cube("jbox_base", 0.205, 0.150, 0.014, (0, 0, 0.012), bev=0.008),
                   "steel"))
    stamp(register(cube("jbox_body", 0.170, 0.118, 0.055, (0, 0, 0.070), bev=0.012),
                   "dark"))
    stamp(register(cube("jbox_lid", 0.140, 0.092, 0.010, (0, 0, 0.132), bev=0.006),
                   "steel"))
    for sx in (-1, 1):
        for sy in (-1, 1):
            stamp(register(cyl("jbox_b%d%d" % (sx, sy), 0.013, 0.016,
                               (sx * 0.182, sy * 0.128, 0.020), verts=6, bev=0.002),
                           "steel"))
            stamp(register(cyl("jbox_l%d%d" % (sx, sy), 0.010, 0.012,
                               (sx * 0.120, sy * 0.074, 0.140), verts=6, bev=0.002),
                           "steel"))
    stamp(register(cyl("jbox_gland", 0.038, 0.070, (0.0, -0.150, 0.060),
                       rot=(D(90), 0, 0), verts=12, bev=0.005), "steel"))

    # --- BOLTED ACCESS HATCH (alone, upper-left): a recessed plate + 6 fasteners
    set_stamp(frame(HATCH_U, HATCH_V))
    stamp(register(cube("hatch_plate", 0.230, 0.165, 0.013, (0, 0, 0.011), bev=0.009),
                   "steel"))
    stamp(register(cube("hatch_face", 0.196, 0.132, 0.008, (0, 0, 0.024), bev=0.005),
                   "dark"))
    for i in range(3):
        for sy in (-1, 1):
            stamp(register(cyl("hatch_b%d%d" % (i, sy), 0.012, 0.015,
                               ((i - 1) * 0.150, sy * 0.142, 0.019), verts=6,
                               bev=0.002), "steel"))
    stamp(register(cube("hatch_handle", 0.052, 0.016, 0.016, (0.120, 0.0, 0.036),
                        bev=0.005), "steel"))
    log("service gear: junction box @%.0f deg + access hatch @%.0f deg (asymmetric)"
        % (math.degrees(JBOX_U), math.degrees(HATCH_U)))


# ---------------------------------------------------------------------------
# CRADLE — the gate is INSTALLED, and R11 makes it look like it. The R10 cradle was a
# thin pad + two spindly legs, which under a fat round ring reads as an AXLE STAND
# holding a wheel. Now there is a HEAVY LOWER BASE CASTING: a chunky chamfered plinth
# with a stepped machined top the tube sits down INTO, bolted to the deck. Mass at the
# bottom is what tells the eye this thing is INSTALLED, not rolling.
# ---------------------------------------------------------------------------
def build_cradle():
    # THE BORE IS A NO-GO VOLUME. The membrane is a disk of R 1.895 lying in the ring's
    # OWN plane (z ~ 0), so ANY hardware within 1.895 m of the ring centre and near z=0
    # punches straight through the portal -- which is exactly what the first cut of this
    # casting did (a slab at local y = -1.36 sits 1.36 m from centre: inside the disk).
    # The floor is at y = -2.20 and the bore's lowest point is y = -1.895, so a plinth
    # standing on the deck has only ~0.30 m of legal height at x = 0. It gets its MASS
    # from depth and width instead of height, and the buttresses that do climb are kept
    # out at |x| >= 1.55 (r > 1.9) and off the membrane plane at |z| = 0.62.
    register(cube("base_slab", 1.62, 0.10, 0.98, (0.0, FLOOR_Y + 0.10, 0.02),
                  bev=0.030), "dark")                       # top at y = -2.00: legal
    register(cube("base_step", 1.30, 0.05, 0.80, (0.0, FLOOR_Y + 0.24, 0.02),
                  bev=0.020), "steel")                      # top at y = -1.91: legal
    # heavy chamfered BUTTRESS GUSSETS: the mass that says "installed", kept clear of
    # both the bore (|x| 1.55 -> r >= 1.95) and the membrane plane (|z| 0.62).
    for sx in (-1, 1):
        for sz in (-1, 1):
            register(cube("gusset%d%d" % (sx, sz), 0.11, 0.44, 0.30,
                          (sx * 1.58, FLOOR_Y + 0.52, sz * 0.62), bev=0.022), "dark")
            register(cyl("gussetbolt%d%d" % (sx, sz), 0.026, 0.030,
                         (sx * 1.58, FLOOR_Y + 0.14, sz * 0.62),
                         rot=aim((0.0, 1.0, 0.0)), verts=6, bev=0.004), "steel")
    # deck fasteners around the casting's flange
    for i in range(6):
        x = -1.55 + i * 0.62
        for sz in (-1, 1):
            register(cyl("basebolt%d_%d" % (i, sz), 0.030, 0.034,
                         (x, FLOOR_Y + 0.14, sz * 0.78),
                         rot=aim((0.0, 1.0, 0.0)), verts=6, bev=0.005), "steel")
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


MEMB_R = 1.895      # the membrane disk's radius (rifthub.cpp)
MEMB_Z = 0.12       # half-thickness of the plate it occupies, with margin

def check_membrane_clearance():
    """NO-GO VOLUME: the membrane is a disk of R 1.895 lying in the ring's OWN plane.
    Any bolted-on part that strays within that radius AND near z=0 punches through the
    portal -- which is precisely what the first R11 base casting did (it appeared as a
    black slab floating in the bottom of the rift). Geometry is easy to author blind
    here because the offending part looks fine in isolation; only the render shows it.
    So we assert it, per object, every build."""
    bad = []
    for grp, objs in GROUPS.items():
        if grp == "patina":
            continue                       # the tube IS the bore wall; check_bore() owns it
        for ob in objs:
            m = ob.matrix_world
            for vt in ob.data.vertices:
                p = m @ vt.co
                if math.hypot(p.x, p.y) < MEMB_R and abs(p.z) < MEMB_Z:
                    bad.append(ob.name)
                    break
    return bad


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
    build_service_gear()
    build_panel()
    build_cradle()
    build_cables()
    log("authored objects:", sum(len(v) for v in GROUPS.values()))

    intruders = check_membrane_clearance()
    if intruders:
        log("!! MEMBRANE INTRUSION (%d parts inside R %.3f / |z| %.2f): %s"
            % (len(intruders), MEMB_R, MEMB_Z, ", ".join(sorted(set(intruders))[:8])))
        raise RuntimeError("bolted-on geometry punches through the membrane: "
                           + ", ".join(sorted(set(intruders))[:8]))
    log("membrane clearance: OK (no bolted-on part inside R %.3f / |z| < %.2f)"
        % (MEMB_R, MEMB_Z))

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
