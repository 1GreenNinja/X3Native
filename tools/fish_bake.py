"""
fish_bake.py — headless Blender FISH baker for X3Native (the Rodin river fish).

    blender-launcher.exe --background --python fish_bake.py -- <in.glb|in.fbx> <out.glb> <species>

Takes a raw Rodin fish (a 90k-500k-tri STATIC statue with 2K PBR maps) and turns
it into a game fish: decimated to a budget, retextured small, RIGGED with a 4-bone
spine, and carrying two seamless baked swim loops. The engine (app/fish.cpp) then
plays it through the same skinned-character path the monsters/crowds use.

  species := pike | rudd | bream | perch   (drives the budget + texture size)

WHAT IT DOES, in order:

 1. IMPORT (.glb via gltf, .fbx via fbx + loose sibling PNGs bound by name).
 2. MERGE BY DISTANCE. Rodin exports carry SPLIT vertices (bluebream: 68,342 verts
    for 90,000 tris — a closed manifold wants ~45,000). Those splits are seams the
    collapser cannot cross, so a naive Decimate JAMS: asking for 1,500 tris yielded
    2,391 and shredded fins. Welding first (1e-5) restores a clean manifold and the
    ratio maps linearly onto the tri count again. THIS IS THE STEP THAT MAKES
    DECIMATION WORK — do not remove it.
 3. FIN PROTECTION. Decimators eat thin, flat, low-curvature surfaces FIRST — which
    is exactly the dorsal / caudal / pectoral fins, i.e. exactly the silhouette that
    says "fish". So we find them: cast a ray from every vertex INWARD along -normal
    and take the hit distance = LOCAL THICKNESS. Fins come out at < 1.2% of the body
    diagonal (bluebream: 8.2% of verts). Those go in a vertex group weighted kFinW
    (< 1) while the body is weighted 1.0; Blender's Decimate reads the weight as
    "how much to decimate here", so the fins survive at a much finer ratio than the
    slab of the body. Measured on the bluebream at 1,414 tris: length 1.901 (orig
    1.900), height 1.092 (orig 1.092) — the fork and the dorsal are INTACT.
 4. RATIO SEARCH. The weighted decimation is non-linear in `ratio`, so we binary-
    search `ratio` (9 iters, on throwaway copies) until the tri count lands on the
    species budget.
 5. TEXTURES. Rodin ships 2048^2 diffuse / normal / metallic-roughness. A 30 cm fish
    seen underwater at range does not need 12 MB of maps: downscale to 512^2 (1024^2
    for the pike, the hero loner you get close to) and repack. The NORMAL MAP IS THE
    DETAIL — every scale, stripe and wet highlight lives in it, not in the geometry —
    so it is kept, kept Non-Color, and TANGENTS ARE EXPORTED (export_tangents=True)
    so the engine's PBR path has a basis to light it with. A decimated fish with no
    tangents is a smooth plastic minnow.
 6. ORIENT + NORMALIZE. Rodin lands the fish nose-along--Y, up +Z, at an arbitrary
    size. We rotate it into the engine's fish canon — +X = SNOUT, -X = TAIL, +Y = UP
    (after glTF's Z-up -> Y-up flip), lateral = Z — and scale the body to unit
    length, so app/fish.cpp keeps sizing every species with one `size` scalar exactly
    as it sizes the procedural loft today.
 7. RIG. A 4-bone spine laid along the body axis: Fish_Head (rigid — fish heads do
    not bend), Fish_Body, Fish_Tail, Fish_Caudal. Weights are AUTO BY POSITION: a
    continuous bone coordinate u(x) is built from the joint knots and each vertex
    takes the two straddling bones at (1-frac, frac). Linear, 2 influences, no
    islands, no wobble — a fish is a beam, not a face.
 8. BAKE THE SWIM. A TRAVELLING SINE down the spine: bone i yaws about the fish's
    UP axis by A_i * sin(2*pi*cycles*t - phi_i), with the amplitude RAMPING toward
    the tail and the phase LAGGING further back — so the tail trails the mid, the
    body S-flexes, and the tail does the work. Rotations are relative-to-parent, so
    they compound down the chain (that IS the wave).
      * "Swim"     32f @24fps, 2 beats  — cruise.
      * "SwimFast" 32f @24fps, 3 beats, amplitude x1.55 — the flee burst.
    SEAMLESS BY CONSTRUCTION: an integer number of sine cycles across the loop, so
    value and derivative wrap. Deterministic (no rng).

 9. FREEZE THE RIG INTO POSE MESHES, and ship THOSE — not the skeleton.
    THE ENGINE CANNOT INSTANCE SKINNED MESHES. The RHI keys the joint palette by
    MeshHandle (`setSkinnedPalette(MeshHandle, ...)` — one palette per mesh), and
    the only skinned path is one MonsterSystem per instance at ~15-25 ms of spawn
    each, with a pose API (`setPropPose(pos, yaw)`) that has no ROLL — so it cannot
    even express a fish banking into a turn, let alone floating belly-up. Sixty-one
    skinned fish is not on the table.
    So the skinning happens HERE, offline: the armature is evaluated at N sample
    phases and the DEFORMED mesh is frozen out at each one (16 cruise + 12 fast +
    1 slack-dead = 29 meshes). At runtime a fish just swaps its Entity's MeshHandle
    to the pose for its (time * beat + phase). That buys real skinned-quality
    deformation — the fins bend WITH the body, nothing tears at a seam the way a
    rigid hinge-chain would scissor a dorsal that spans the joint — for ZERO
    per-frame vertex work, and ONE draw per fish instead of the loft's three.
    (A rigid 3-piece cut of a REAL fish mesh was the other candidate and it is the
    wrong one: the bream's dorsal and anal fins span the hinge planes, so the
    pieces would shear them apart.)

The axis math is rest-pose-agnostic in the swim_bake.py style: a bone's pose basis
for "yaw by `a` about the armature-space UP axis" is
      basis = L^-1 . Rup(a) . L,        L = bone.matrix_local (rest, armature space)
so the key lands correctly whatever roll the bone was built with.

ENV NOTE (this box): Store-Blender — blender.exe is ACL-denied, blender-launcher
DETACHES; we report through `<out>.log` + `<out>.done` (poll them). Driver:
tools/fish_build.ps1.

Clean-room: public Blender Python API + glTF 2.0 spec only.
"""
import bpy, sys, os, math, time
import bmesh
from mathutils import Vector, Matrix, Quaternion
from mathutils.bvhtree import BVHTree

ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(ARGV) < 3:
    raise SystemExit("Usage: ... -- <in.glb|in.fbx> <out.glb> <species>")
SRC, OUT, SPECIES = ARGV[0], ARGV[1], ARGV[2].lower()

LOG_PATH, DONE_PATH = OUT + ".log", OUT + ".done"
_log = []
def log(*a):
    s = "[fishbake] " + " ".join(str(x) for x in a)
    _log.append(s); print(s)

def flush_log(status):
    try:
        with open(LOG_PATH, "w", encoding="utf-8") as f:
            f.write("\n".join(_log) + "\n")
        with open(DONE_PATH, "w", encoding="utf-8") as f:
            f.write(status)
    except Exception:
        pass

# ---------------------------------------------------------------------------
# THE SPECIES TABLE (bake side). Budgets are set by how close you ever get:
# the shoal fish are 10-40 m away in a shifting cloud (a silhouette + a normal
# map is the whole read); the PIKE is the loner you swim up to.
# ---------------------------------------------------------------------------
SPEC = {
    #            tris   tex   finw
    "pike":  dict(tris=3600, tex=1024, finw=0.35),
    "rudd":  dict(tris=1400, tex=512,  finw=0.35),
    "bream": dict(tris=1400, tex=512,  finw=0.35),
    "perch": dict(tris=1800, tex=512,  finw=0.35),
}
if SPECIES not in SPEC:
    raise SystemExit(f"unknown species '{SPECIES}' (want one of {sorted(SPEC)})")
CFG = SPEC[SPECIES]

FPS      = 24
LOOP_F   = 32          # frames per swim loop (keys 0..32, frame 32 == frame 0)
THIN_FRAC = 0.012      # local thickness below this * bbox-diagonal => "fin"

# POSE SAMPLING: how many frozen meshes each loop is frozen into. 16 phases of a
# ~2 Hz tail beat is ~32 distinct tail positions a second — smooth to the eye on a
# 30 cm fish, and the whole point is that they cost NOTHING at runtime (a mesh
# handle swap). The engine interpolates nothing; it just picks the nearest pose.
N_CRUISE = 16
N_FAST   = 12

# The travelling wave: (relative-to-parent amplitude rad, phase lag rad).
# Amplitude ramps aft, phase lags further aft — the tail trails the mid.
WAVE = [
    ("Fish_Head",   0.045, 0.00),   # a touch of head recoil, leading the wave
    ("Fish_Body",   0.115, 0.70),
    ("Fish_Tail",   0.230, 1.40),
    ("Fish_Caudal", 0.330, 2.10),
]
FAST_GAIN   = 1.55
SWIM_CYCLES = 2        # beats inside one loop  -> 2 / (32/24) = 1.5 Hz
FAST_CYCLES = 3        #                        -> 2.25 Hz


def tri_count(ob):
    ob.data.calc_loop_triangles()
    return len(ob.data.loop_triangles)


def bbox(ob):
    """Read the bbox from the VERTICES, never from ob.bound_box: bound_box is
    evaluated-object cache and is STALE straight after mesh.transform(), which
    silently corrupted the normalise step (it 'scaled' by the pre-rotation extent)
    and then the fin threshold that depends on it."""
    vs = ob.data.vertices
    if not vs:
        return Vector((0, 0, 0)), Vector((0, 0, 0))
    mn = Vector((min(v.co.x for v in vs), min(v.co.y for v in vs), min(v.co.z for v in vs)))
    mx = Vector((max(v.co.x for v in vs), max(v.co.y for v in vs), max(v.co.z for v in vs)))
    return mn, mx


def only_select(ob):
    for o in bpy.context.scene.objects:
        o.select_set(o is ob)
    bpy.context.view_layer.objects.active = ob


def local_thickness(me, diag):
    """Per-vertex local thickness: ray from the vertex INWARD along -normal."""
    bm = bmesh.new()
    bm.from_mesh(me)
    bvh = BVHTree.FromBMesh(bm)
    eps = diag * 1e-4
    out = []
    for v in me.vertices:
        hit = bvh.ray_cast(v.co - v.normal * eps, -v.normal, diag)
        out.append(hit[3] if hit[0] is not None else diag)
    bm.free()
    return out


# ---------------------------------------------------------------------------
def do_import():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    ext = os.path.splitext(SRC)[1].lower()
    if ext in (".glb", ".gltf"):
        bpy.ops.import_scene.gltf(filepath=SRC)
    elif ext == ".fbx":
        bpy.ops.import_scene.fbx(filepath=SRC)
        bind_loose_textures()
    else:
        raise SystemExit("unsupported input: " + ext)
    meshes = [o for o in bpy.context.scene.objects if o.type == 'MESH']
    if not meshes:
        raise SystemExit("no mesh in " + SRC)
    ob = max(meshes, key=lambda o: len(o.data.vertices))
    for o in meshes:
        if o is not ob:
            bpy.data.objects.remove(o, do_unlink=True)
    only_select(ob)
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    log(f"import: {os.path.basename(SRC)} verts={len(ob.data.vertices)} tris={tri_count(ob)}")
    return ob


def bind_loose_textures():
    """The perch ships as FBX + loose PNGs (texture_diffuse/normal/metallic/
    roughness). The FBX importer brings no maps, so build the Principled graph by
    hand from the siblings — and pack metallic(B)+roughness(G) into ONE glTF-style
    ORM image, which is what the exporter wants."""
    d = os.path.dirname(SRC)
    def find(*names):
        for n in names:
            p = os.path.join(d, n + ".png")
            if os.path.exists(p):
                return p
        return None
    p_alb = find("texture_diffuse", "texture_basecolor")
    p_nrm = find("texture_normal")
    p_met = find("texture_metallic")
    p_rgh = find("texture_roughness")
    log(f"fbx loose maps: albedo={bool(p_alb)} normal={bool(p_nrm)} "
        f"metallic={bool(p_met)} roughness={bool(p_rgh)}")
    if not p_alb:
        raise SystemExit("perch FBX: no texture_diffuse.png beside it")

    alb = bpy.data.images.load(p_alb); alb.name = "texture_diffuse"
    alb.colorspace_settings.name = 'sRGB'
    nrm = None
    if p_nrm:
        nrm = bpy.data.images.load(p_nrm); nrm.name = "texture_normal"
        nrm.colorspace_settings.name = 'Non-Color'

    # ORM: glTF reads metal from BLUE and rough from GREEN of one image.
    orm = None
    if p_met and p_rgh:
        im_m = bpy.data.images.load(p_met)
        im_r = bpy.data.images.load(p_rgh)
        w, h = im_m.size
        if im_r.size[0] != w or im_r.size[1] != h:
            im_r.scale(w, h)
        mp = list(im_m.pixels)
        rp = list(im_r.pixels)
        orm = bpy.data.images.new("texture_metallic-texture_roughness", w, h, alpha=False)
        px = [0.0] * (w * h * 4)
        for i in range(w * h):
            px[i * 4 + 0] = 1.0
            px[i * 4 + 1] = rp[i * 4]      # G = roughness
            px[i * 4 + 2] = mp[i * 4]      # B = metallic
            px[i * 4 + 3] = 1.0
        orm.pixels = px
        orm.colorspace_settings.name = 'Non-Color'
        bpy.data.images.remove(im_m); bpy.data.images.remove(im_r)

    for ob in [o for o in bpy.context.scene.objects if o.type == 'MESH']:
        mat = bpy.data.materials.new("model")
        mat.use_nodes = True
        ob.data.materials.clear()
        ob.data.materials.append(mat)
        nt = mat.node_tree
        bsdf = next(n for n in nt.nodes if n.type == 'BSDF_PRINCIPLED')
        ta = nt.nodes.new('ShaderNodeTexImage'); ta.image = alb
        nt.links.new(ta.outputs['Color'], bsdf.inputs['Base Color'])
        if nrm:
            tn = nt.nodes.new('ShaderNodeTexImage'); tn.image = nrm
            nm = nt.nodes.new('ShaderNodeNormalMap')
            nt.links.new(tn.outputs['Color'], nm.inputs['Color'])
            nt.links.new(nm.outputs['Normal'], bsdf.inputs['Normal'])
        if orm:
            to = nt.nodes.new('ShaderNodeTexImage'); to.image = orm
            sep = nt.nodes.new('ShaderNodeSeparateColor')
            nt.links.new(to.outputs['Color'], sep.inputs['Color'])
            nt.links.new(sep.outputs['Green'], bsdf.inputs['Roughness'])
            nt.links.new(sep.outputs['Blue'], bsdf.inputs['Metallic'])
        break


def clear_split_normals(ob, why):
    """The glTF/FBX importers attach CUSTOM SPLIT NORMALS. They survive decimation
    as STALE per-loop normals — the collapsed surface keeps the old high-poly
    normals on the loops it kept, and shade_smooth() does NOT clear them. The
    result renders as a hard-facetted plastic slab with the normal map fighting
    the shading: the first bream bake came out as flat blue polygons down the
    flank. Clearing them makes Blender recompute smooth normals from the decimated
    geometry, which is what the normal map wants to sit on."""
    only_select(ob)
    try:
        bpy.ops.mesh.customdata_custom_splitnormals_clear()
        log(f"normals: cleared custom split normals ({why})")
    except RuntimeError:
        pass                       # none present — fine
    bpy.ops.object.shade_smooth()


def weld(ob):
    """MERGE BY DISTANCE — the step that unjams the decimator (see the header)."""
    before = len(ob.data.vertices)
    clear_split_normals(ob, "post-import")
    only_select(ob)
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.mesh.remove_doubles(threshold=1e-5)
    bpy.ops.mesh.normals_make_consistent(inside=False)
    bpy.ops.object.mode_set(mode='OBJECT')
    log(f"weld: {before} -> {len(ob.data.vertices)} verts (split verts removed)")


def decimate(ob):
    mn, mx = bbox(ob)
    diag = (mx - mn).length
    thin = THIN_FRAC * diag
    th = local_thickness(ob.data, diag)
    fins = [i for i, d in enumerate(th) if d < thin]
    log(f"fins: {len(fins)}/{len(th)} verts ({100.0 * len(fins) / max(1, len(th)):.1f}%) "
        f"thinner than {thin:.4f} ({THIN_FRAC:.3f} of the {diag:.3f} diagonal)")

    vg = ob.vertex_groups.new(name="dec")
    vg.add(list(range(len(th))), 1.0, 'REPLACE')          # body: decimate fully
    vg.add(fins, CFG["finw"], 'REPLACE')                  # fins: decimate LESS

    target = CFG["tris"]
    t0 = tri_count(ob)
    lo, hi, best = 1e-4, 1.0, None
    for _ in range(9):
        r = (lo + hi) * 0.5
        tmp = ob.copy(); tmp.data = ob.data.copy()
        bpy.context.scene.collection.objects.link(tmp)
        m = tmp.modifiers.new("dec", 'DECIMATE')
        m.decimate_type = 'COLLAPSE'
        m.ratio = r
        m.use_collapse_triangulate = True
        m.vertex_group = "dec"
        m.vertex_group_factor = 1.0
        only_select(tmp)
        bpy.ops.object.modifier_apply(modifier="dec")
        n = tri_count(tmp)
        if best is None or abs(n - target) < abs(best[1] - target):
            best = (r, n)
        bpy.data.objects.remove(tmp, do_unlink=True)
        if n > target: hi = r
        else:          lo = r
    ratio = best[0]

    m = ob.modifiers.new("dec", 'DECIMATE')
    m.decimate_type = 'COLLAPSE'
    m.ratio = ratio
    m.use_collapse_triangulate = True
    m.vertex_group = "dec"
    m.vertex_group_factor = 1.0
    only_select(ob)
    bpy.ops.object.modifier_apply(modifier="dec")
    ob.vertex_groups.remove(ob.vertex_groups["dec"])

    mn2, mx2 = bbox(ob)
    th2 = local_thickness(ob.data, diag)
    fin2 = sum(1 for d in th2 if d < thin)
    log(f"decimate: {t0} -> {tri_count(ob)} tris (ratio={ratio:.5f}, "
        f"{100.0 * tri_count(ob) / t0:.2f}% kept, target {target})")
    log(f"  fin verts kept: {fin2}/{len(ob.data.vertices)} "
        f"({100.0 * fin2 / max(1, len(ob.data.vertices)):.0f}% of the budget)")
    log(f"  SILHOUETTE: extents {tuple(round(v, 4) for v in (mx2 - mn2))} "
        f"vs original {tuple(round(v, 4) for v in (mx - mn))}")
    d0, d1 = (mx - mn), (mx2 - mn2)
    for k, ax in enumerate("xyz"):
        if d0[k] > 1e-6 and abs(d1[k] - d0[k]) / d0[k] > 0.03:
            log(f"  WARNING: {ax} extent moved {100.0 * (d1[k] - d0[k]) / d0[k]:+.1f}% "
                f"— a fin may have been eaten")
    clear_split_normals(ob, "post-decimate")


def shrink_textures(ob):
    """2048^2 PBR maps for a 30 cm fish is waste. Downscale + pack."""
    px = CFG["tex"]
    seen = set()
    for mat in ob.data.materials:
        if not mat or not mat.node_tree:
            continue
        for n in mat.node_tree.nodes:
            if n.type == 'TEX_IMAGE' and n.image and n.image.name not in seen:
                im = n.image
                seen.add(im.name)
                w, h = im.size
                if w > px or h > px:
                    im.scale(min(w, px), min(h, px))
                im.pack()
                log(f"texture: {im.name} {w}x{h} -> {im.size[0]}x{im.size[1]} "
                    f"cs={im.colorspace_settings.name}")


def orient(ob):
    """Rodin lands the fish nose-along--Y / up +Z. Rotate into the engine's fish
    canon (+X = snout, -X = tail, +Z = up in Blender => +Y up after the glTF flip)
    and normalise the body to UNIT LENGTH so app/fish.cpp sizes every species with
    one scalar."""
    mn, mx = bbox(ob)
    ext = mx - mn
    axis = max(range(3), key=lambda i: ext[i])          # the body axis
    up = max((i for i in range(3) if i != axis), key=lambda i: ext[i])
    lat = 3 - axis - up
    log(f"orient: body axis={'xyz'[axis]} up={'xyz'[up]} lateral={'xyz'[lat]} "
        f"extents=({ext.x:.3f},{ext.y:.3f},{ext.z:.3f})")

    # Which end is the SNOUT? The head is a fat wedge; the caudal fin is a thin
    # blade. So compare the mean lateral half-width of the two ends: the FAT end
    # is the head.
    lo, hi = mn[axis], mx[axis]
    span = hi - lo
    a = [abs(v.co[lat]) for v in ob.data.vertices if v.co[axis] < lo + 0.15 * span]
    b = [abs(v.co[lat]) for v in ob.data.vertices if v.co[axis] > hi - 0.15 * span]
    ma = sum(a) / max(1, len(a))
    mb = sum(b) / max(1, len(b))
    nose_at_max = mb > ma
    log(f"  half-width: low end={ma:.4f} high end={mb:.4f} => snout at the "
        f"{'+' if nose_at_max else '-'}{'xyz'[axis]} end")

    # Build the basis that sends (body -> +X, up -> +Z, lateral -> +Y).
    src_fwd = Vector((0, 0, 0)); src_fwd[axis] = 1.0 if nose_at_max else -1.0
    src_up  = Vector((0, 0, 0)); src_up[up]    = 1.0
    src_lat = src_up.cross(src_fwd)            # right-handed: lat = up x fwd
    R = Matrix((
        (src_fwd.x, src_fwd.y, src_fwd.z),
        (src_lat.x, src_lat.y, src_lat.z),
        (src_up.x,  src_up.y,  src_up.z),
    ))                                          # rows = the new X/Y/Z in old coords
    ob.data.transform(R.to_4x4())
    ob.data.update()

    # Is the fish upside down? The BACK is where the dorsal fin stands: the deeper
    # half. Compare the mesh's mass above/below the body axis and flip if needed.
    mn, mx = bbox(ob)
    if abs(mn.z) > abs(mx.z):
        ob.data.transform(Matrix.Rotation(math.pi, 4, 'X'))
        ob.data.update()
        log("  flipped: the dorsal was pointing down")

    # Unit length + centred on the body axis, belly-to-back centred.
    mn, mx = bbox(ob)
    L = mx.x - mn.x
    s = 1.0 / L
    mid = (mn + mx) * 0.5
    M = Matrix.Diagonal((s, s, s, 1.0)) @ Matrix.Translation(-mid)
    ob.data.transform(M)
    ob.data.update()
    mn, mx = bbox(ob)
    log(f"  normalised: length {L:.3f} -> 1.000, bbox "
        f"({mn.x:.3f},{mn.y:.3f},{mn.z:.3f})-({mx.x:.3f},{mx.y:.3f},{mx.z:.3f})")


# ---------------------------------------------------------------------------
# THE RIG: 4 bones down the body axis, weighted by position.
# ---------------------------------------------------------------------------
# Joint knots along the normalised body (x = +0.5 snout .. -0.5 tail tip).
# Head is RIGID from the snout back to KNOT[0]; the bend lives behind it.
KNOTS = [0.5, 0.10, -0.10, -0.28, -0.5]      # 4 bones between 5 knots


def rig(ob):
    arm_data = bpy.data.armatures.new("FishRig")
    arm = bpy.data.objects.new("FishRig", arm_data)
    bpy.context.scene.collection.objects.link(arm)
    only_select(arm)
    bpy.ops.object.mode_set(mode='EDIT')
    prev = None
    for i, (name, _amp, _lag) in enumerate(WAVE):
        eb = arm_data.edit_bones.new(name)
        eb.head = Vector((KNOTS[i],     0.0, 0.0))
        eb.tail = Vector((KNOTS[i + 1], 0.0, 0.0))
        eb.roll = 0.0
        if prev:
            eb.parent = prev
            eb.use_connect = True
        prev = eb
    bpy.ops.object.mode_set(mode='OBJECT')
    log("rig: 4-bone spine " + " -> ".join(w[0] for w in WAVE)
        + f"  knots={KNOTS}")

    # ---- WEIGHTS: auto by position along the body axis --------------------
    # u(x) is a continuous "bone coordinate": 0 at the snout, 4 at the tail tip,
    # with the knots as its breakpoints. A vertex takes the two bones straddling
    # its u at (1-frac, frac). Two influences, linear falloff, no islands.
    vgs = [ob.vertex_groups.new(name=w[0]) for w in WAVE]
    nb = len(WAVE)
    for v in ob.data.vertices:
        x = v.co.x
        # find the segment
        seg = 0
        while seg < nb - 1 and x < KNOTS[seg + 1]:
            seg += 1
        x0, x1 = KNOTS[seg], KNOTS[seg + 1]
        t = 0.0 if abs(x1 - x0) < 1e-9 else (x0 - x) / (x0 - x1)
        t = min(1.0, max(0.0, t))
        u = seg + t
        # Bone b owns u in [b-0.5, b+0.5] with a linear ramp to its neighbours:
        # shift by 0.5 so a vertex mid-segment sits fully on that segment's bone.
        c = u - 0.5
        i0 = int(math.floor(c))
        f = c - i0
        for bi, w in ((i0, 1.0 - f), (i0 + 1, f)):
            if w <= 1e-4:
                continue
            bi = min(nb - 1, max(0, bi))
            vgs[bi].add([v.index], w, 'ADD')
    counts = [0] * nb
    for v in ob.data.vertices:
        for g in v.groups:
            if g.weight > 0.01:
                counts[g.group] += 1
    log("weights: verts per bone " + ", ".join(
        f"{WAVE[i][0]}={counts[i]}" for i in range(nb)))

    ob.parent = arm
    mod = ob.modifiers.new("Armature", 'ARMATURE')
    mod.object = arm
    return arm


def bake_clip(arm, name, cycles, gain):
    """A travelling sine down the spine, keyed EVERY frame, seamless by
    construction (an integer number of cycles across the loop)."""
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode='POSE')
    act = bpy.data.actions.new(name)
    if not arm.animation_data:
        arm.animation_data_create()
    arm.animation_data.action = act

    up = Vector((0.0, 0.0, 1.0))          # Blender Z = the fish's UP (pre-export)
    for pb in arm.pose.bones:
        pb.rotation_mode = 'QUATERNION'

    for f in range(LOOP_F + 1):           # 0..LOOP_F, frame LOOP_F == frame 0
        t = (f % LOOP_F) / float(LOOP_F)
        bpy.context.scene.frame_set(f)
        for (bname, amp, lag) in WAVE:
            pb = arm.pose.bones[bname]
            a = gain * amp * math.sin(2.0 * math.pi * cycles * t - lag)
            # Yaw about the ARMATURE-space up axis, expressed in the bone's own
            # rest frame:  basis = L^-1 . Rup(a) . L   (rest-pose agnostic).
            L = pb.bone.matrix_local.to_3x3()
            R = Matrix.Rotation(a, 3, up)
            basis = L.inverted() @ R @ L
            pb.rotation_quaternion = basis.to_quaternion()
            pb.keyframe_insert("rotation_quaternion", frame=f)

    for fc in act.fcurves:
        for kp in fc.keyframe_points:
            kp.interpolation = 'LINEAR'
    act.use_fake_user = True
    bpy.ops.object.mode_set(mode='OBJECT')
    peak = max(gain * w[1] for w in WAVE)
    log(f"clip '{name}': {LOOP_F}f @{FPS}fps ({LOOP_F / FPS:.2f}s), {cycles} beats "
        f"({cycles * FPS / LOOP_F:.2f} Hz), peak tail sweep "
        f"{math.degrees(sum(gain * w[1] for w in WAVE)):.0f} deg "
        f"(max single joint {math.degrees(peak):.0f} deg) — seamless (integer cycles)")
    return act


def bake_poses(ob, arm):
    """FREEZE THE SWIM INTO POSE MESHES — the whole point of this baker.

    The engine has NO instanced skinned draw: the RHI keys the joint palette by
    MeshHandle (one palette per mesh), and the only skinned path is one
    MonsterSystem per instance at ~15-25 ms of spawn each. Sixty-one skinned fish
    is therefore not on the table.

    So we skin the fish HERE, offline, and ship the RESULT: the armature is
    evaluated at N sample phases of each loop and the DEFORMED mesh is frozen out
    at every one. At runtime a fish just swaps its Entity's MeshHandle to the pose
    for its current (time * beat + phase) — real skinned-quality deformation (fins
    bend WITH the body; nothing tears, nothing seams) for ZERO per-frame vertex
    work, and ONE draw per fish instead of the procedural loft's three.

    LAYOUT (the engine reads it back positionally — ModelDrawable carries no name,
    so the pose index is encoded in the node's TRANSLATION X, which the engine
    rounds to an int and then ignores for rendering):
        k in [0, NC)            -> Swim     frame k     (cruise)
        k in [NC, NC+NF)        -> SwimFast frame k-NC  (the flee burst)
        k == NC+NF              -> DEAD: the slack, near-straight body the corpse
                                   floats in (belly-up is a roll in the entity
                                   transform, so the mesh only has to go limp)
    """
    depsgraph = bpy.context.evaluated_depsgraph_get()
    mat = ob.data.materials[0] if ob.data.materials else None
    poses = []

    def freeze(action, frame, tag):
        arm.animation_data.action = action
        # frame_set() takes an int + a SUBFRAME: N_FAST=12 samples of a 32-frame
        # loop land on thirds of a frame, and rounding them would jitter the phase.
        fi = int(math.floor(frame))
        bpy.context.scene.frame_set(fi, subframe=float(frame) - fi)
        depsgraph.update()
        ev = ob.evaluated_get(depsgraph)
        me = bpy.data.meshes.new_from_object(ev, preserve_all_data_layers=True,
                                             depsgraph=depsgraph)
        me.name = f"pose_{len(poses):02d}_{tag}"
        po = bpy.data.objects.new(me.name, me)
        # The pose INDEX rides in the node translation (see the docstring).
        po.location = (float(len(poses)), 0.0, 0.0)
        bpy.context.scene.collection.objects.link(po)
        if mat and not me.materials:
            me.materials.append(mat)
        po.modifiers.clear()
        poses.append(po)

    # Sample exactly ONE TAIL BEAT, not the whole clip. The clip holds `cycles`
    # beats across LOOP_F frames, so sampling the clip end-to-end stores each beat
    # `cycles` times: the first bake shipped poses 8-15 as byte-duplicates of 0-7.
    # One beat = LOOP_F/cycles frames, so N samples of THAT give N distinct poses
    # and twice the temporal resolution for the same memory. Seamless by
    # construction (a whole sine period), and the runtime beat rate is the
    # ENGINE's (cfg.beatHz) — the clip only supplies the wave SHAPE.
    swim = bpy.data.actions["Swim"]
    fast = bpy.data.actions["SwimFast"]
    for k in range(N_CRUISE):
        freeze(swim, k * (LOOP_F / SWIM_CYCLES) / N_CRUISE, "cruise")
    for k in range(N_FAST):
        freeze(fast, k * (LOOP_F / FAST_CYCLES) / N_FAST, "fast")

    # DEAD: the swim goes slack. Key every bone to ~0 and freeze one frame.
    dead = bpy.data.actions.new("Dead")
    arm.animation_data.action = dead
    bpy.context.scene.frame_set(0)
    for (bname, _a, _l) in WAVE:
        pb = arm.pose.bones[bname]
        pb.rotation_quaternion = (1.0, 0.0, 0.0, 0.0)
        pb.keyframe_insert("rotation_quaternion", frame=0)
    freeze(dead, 0, "dead")

    log(f"poses: {len(poses)} frozen meshes "
        f"({N_CRUISE} cruise + {N_FAST} fast + 1 dead) — "
        f"deformation baked, ZERO runtime skinning")

    # Drop the rig + the source mesh: only the frozen poses ship.
    bpy.data.objects.remove(ob, do_unlink=True)
    bpy.data.objects.remove(arm, do_unlink=True)
    return poses


def export(poses):
    for o in bpy.context.scene.objects:
        o.select_set(o in poses)
    bpy.context.view_layer.objects.active = poses[0]
    bpy.ops.export_scene.gltf(
        filepath=OUT,
        export_format='GLB',
        use_selection=True,
        export_yup=True,
        export_apply=False,        # keep the node translation (it IS the pose index)
        export_normals=True,
        export_skins=False,
        export_animations=False,
        export_image_format='AUTO',
    )
    log(f"export: {OUT} ({os.path.getsize(OUT) / 1024.0:.0f} KB, "
        f"{len(poses)} pose meshes)")


def main():
    t0 = time.time()
    log(f"=== {SPECIES.upper()} === target {CFG['tris']} tris, {CFG['tex']}^2 maps")
    ob = do_import()
    weld(ob)
    orient(ob)
    decimate(ob)
    shrink_textures(ob)
    arm = rig(ob)
    bake_clip(arm, "Swim",     SWIM_CYCLES, 1.0)
    bake_clip(arm, "SwimFast", FAST_CYCLES, FAST_GAIN)
    poses = bake_poses(ob, arm)
    export(poses)
    log(f"done in {time.time() - t0:.1f}s")


try:
    main()
    flush_log("OK")
except Exception as e:
    import traceback
    log("ERROR " + repr(e))
    log(traceback.format_exc())
    flush_log("FAIL " + repr(e))
