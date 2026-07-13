"""
sealife_bake.py -- headless Blender asset baker for the X3Native OCEAN (app/sealife.*).

    blender-launcher.exe --background --python tools/sealife_bake.py -- <species> <out.glb> [donefile]
    blender-launcher.exe --background --python tools/sealife_bake.py -- all <outdir> [donefile]

Sibling of convert_obj_glb.py (OBJ + PBR set -> GLB) and dance_bake.py / swim_bake.py
(sin-driven seamless clip bakes). This one does BOTH, plus the thing neither does:
it DECIMATES a 50k-tri Rodin sculpt down to a game budget WITHOUT SHREDDING THE FINS,
and it RIGS a static statue so the creature actually FLEXES when it swims.

WHY EACH STEP EXISTS
--------------------
1. ORIENT. Every Rodin sculpt lands in its own pose/axis (the shark is Z-long, the
   squid stands upright on its tentacles). We rotate each species into ONE canonical
   game frame:  +X = FORWARD (the way it swims), +Y = UP, +Z = its LEFT.
   The engine yaws the entity about Y, so "forward" must be +X or the creature
   swims sideways. Scale is normalized so the GLB is 1 m long and the game scales
   it to the real animal (see kLen* in app/sealife.h).

2. MATERIAL. Principled BSDF wired to the species' OWN PBR set (diffuse / normal /
   metallic / roughness), exactly like convert_obj_glb.py -- per-species texdir, never
   one shared atlas (that bug skinned four weapons with the wrong maps).
   THE NORMAL MAP IS THE POINT: at 5k tris the shark's skin detail (denticles, gill
   creases, scars) lives ENTIRELY in the normal map. A shark with no normal map is a
   smooth plastic toy. Textures are downscaled to `tex` px and packed into the GLB.
   The glTF exporter emits TANGENTs automatically when a normal texture is bound.

3. DECIMATE -- FINS AND TENTACLES FIRST. A naive Decimate eats thin, flat,
   low-curvature surfaces before anything else, which is EXACTLY the dorsal fin, the
   tail fluke and the tentacles -- i.e. the entire silhouette. So we measure each
   vertex's LOCAL THICKNESS by shooting a ray from just under the surface back along
   -normal and taking the distance to the far wall (a BVH ray_cast). A fin blade or a
   tentacle tube is THIN (small hit distance); the body/mantle is THICK. That
   thickness, normalized by the bbox diagonal, becomes a PROTECT weight in a vertex
   group, and the Decimate modifier's vertex_group_factor spares those verts. The
   body collapses; the silhouette survives. `--report` prints the fin tri budget so a
   regression is visible, not silent.

4. RIG. Bones are built along +X (head -> tail) with roll aligned so that
       pose_bone.rotation_euler.z == LATERAL sweep (side-to-side: sharks, squid)
       pose_bone.rotation_euler.x == VERTICAL sweep (up-down: whale flukes -- a whale
                                     beats its tail UP AND DOWN, a fish beats it
                                     SIDE TO SIDE; get this backwards and it's a fish
                                     in a whale suit)
   Weights are POSITION-BASED (not bone-heat): each vertex blends between the two
   spine bones its axial coordinate falls between, with a smooth ramp -- deterministic
   and immune to the non-manifold junk that makes automatic weights fail on a sculpt.
   Wing rigs (manta) add L/R chains driven by |z|; the squid adds 4 angular TENTACLE
   sectors so its arms drift out of phase instead of swinging as one rigid broom.

5. BAKE -- SEAMLESS BY CONSTRUCTION. Every clip is a travelling sine down the chain:
       angle_i(f) = amp_i * sin(2*pi*f/N - i*lag)
   keyed at EVERY frame f in [0, N] INCLUSIVE, so frame N is bit-identical to frame 0
   and the wrap is continuous in value AND derivative in any engine that loops it.
   amp_i ramps toward the tail, so the body S-flexes and the tail beats hardest -- a
   creature that translates without flexing is a floating prop. Deterministic (no rng).

ENV NOTE (this box): Store-Blender -- blender.exe is ACL-denied and blender-launcher
DETACHES (returns instantly, no stdout), so we report through `<out>.log` + `<out>.done`
and the caller POLLS them. See tools/sealife_bake.ps1.

ASCII-only on purpose (Store-Blender .py gotcha).
Clean-room: public Blender Python API + glTF 2.0 spec only.
"""
import bpy, sys, os, math, json
from mathutils import Vector, Matrix, Euler

ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []

# ----------------------------------------------------------------------------
# THE CAST. src/tex are the Rodin drops; `orient` maps the source axes into the
# canonical game frame (+X forward, +Y up, +Z left).
#
# `orient` = (euler_xyz_degrees) applied to the imported object, chosen per species
# by LOOKING at the sculpt (probe renders under docs/screenshots/sealife/).
#
# THE FORWARD AXIS IS NOT A FREE CHOICE. app/monster.cpp:150 defines a creature's
# facing as  forward(yaw) = (-sin yaw, 0, -cos yaw), so at yaw 0 the engine expects
# the MODEL to look down -Z (the usual glTF character convention), and there is no
# yaw-offset field in MonsterSystem::Tuning to fudge it with. Bake a shark facing +X
# and it swims permanently sideways.
#
# The Blender glTF exporter (export_yup) maps  blender(x,y,z) -> gltf(x, z, -y).
# So  blender +Y  ->  gltf -Z  == the engine's forward.
# Therefore in BLENDER space we author:  +Y = FORWARD (nose),  +Z = UP,  +X = right.
# The OBJ importer has already turned the source's Y-up into Blender's Z-up, so each
# `orient` below only has to swing the nose onto +Y.
# ----------------------------------------------------------------------------
G = "G:/GameModels/rodin_glb"
SRC_EXTRA = "D:/GameDev/_sealife_src"   # textures recovered from the shark's .usdz

SPECIES = {
    # ---- THE HERO. A true great white: conical snout, teeth, gills, crescent tail.
    #      Its OBJ has NO loose textures (the .mtl points at a bogus C:/ path) -- the
    #      real 2048^2 PBR set is PACKED INSIDE base_basic_pbr.usdz, which is just a
    #      zip. tools/sealife_bake.ps1 extracts it to SRC_EXTRA before we run.
    "shark": {
        "src":    f"{G}/GreatWhiteSharkGameReady/greatwhiteshark_50k.obj",
        "tex":    f"{SRC_EXTRA}/GreatWhiteSharkGameReady",
        # Source (Blender, post-import): long axis is Y, NOSE AT -Y, +Z up.
        # Yaw 180 about Z swings the nose onto +Y (= glTF -Z = engine forward).
        # (Mirroring would also land it there but inverts chirality AND winding --
        # never mirror an asymmetric animal to fix an axis.)
        "orient": (0, 0, 180),
        "flip_x": False,
        "tris":   5200,          # hero: you swim up to this one
        "texpx":  2048,          # earns it -- he fills the screen when he bites
        "rig":    "spine",
        "bones":  6,
        "clips": [
            # name,     frames, amp_tip(rad), lag(rad), axis
            ("Cruise",  48, 0.20, 0.85, "lateral"),
            ("Charge",  28, 0.40, 0.70, "lateral"),
        ],
    },
    # ---- THE DEEP-WATER SHARK. Shipped as "sea_hammerhead" by Rodin, but it is
    #      NOT a hammerhead -- no cephalofoil at all (its total width is 8% of its
    #      length; a hammer would span ~25%). What it IS: a lean, long-snouted
    #      shark -- a blue/mako read. So it ships as the SECOND SHARK: deeper water,
    #      longer stalk, rarer commit. The gameplay slot Tim asked for is intact;
    #      only the species name is honest now. See docs/screenshots/sealife/probe_*.
    "blueshark": {
        "src":    f"{G}/sea_hammerhead/sea_hammerhead.obj",
        "tex":    f"{G}/sea_hammerhead",
        "orient": (0, 0, 180),   # same source layout as the great white
        "flip_x": False,
        "tris":   4200,
        "texpx":  1024,
        "rig":    "spine",
        "bones":  6,
        "clips": [
            ("Cruise",  52, 0.22, 0.85, "lateral"),
            ("Charge",  30, 0.42, 0.70, "lateral"),
        ],
    },
    # ---- THE ABYSS. A real squid: pointed finned mantle + a fan of long arms.
    #      Squid swim MANTLE-FIRST, so forward = the mantle tip; the arms trail.
    "squid": {
        "src":    f"{G}/sea_giant_squid/sea_giant_squid_50k.obj",
        "tex":    f"{G}/sea_giant_squid",
        # Source: the squid STANDS on its arms -- mantle tip at +Z, arms at -Z.
        # Roll -90 about X lays it down nose-first: the mantle tip (+Z) swings to +Y.
        "orient": (-90, 0, 0),
        "flip_x": False,
        "tris":   7500,          # tentacles need loops to bend
        "texpx":  1024,
        "rig":    "squid",
        "bones":  6,             # mantle/axis chain
        "tent_sectors": 4,       # angular tentacle chains (so arms drift out of phase)
        "tent_bones": 3,
        "clips": [
            ("Cruise",  72, 0.30, 1.20, "lateral"),   # slow drift + arm sway
        ],
    },
}


# ----------------------------------------------------------------------------
# logging (the launcher detaches -- the log file IS our stdout)
# ----------------------------------------------------------------------------
_LOGF = [None]
_LINES = []
def log(*a):
    s = " ".join(str(x) for x in a)
    _LINES.append(s)
    print("[sealife]", s)
    if _LOGF[0]:
        try:
            with open(_LOGF[0], "w") as f:
                f.write("\n".join(_LINES) + "\n")
        except Exception:
            pass


def clear_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)


# ----------------------------------------------------------------------------
# 2. MATERIAL -- the species' OWN PBR set, downscaled + packed.
# ----------------------------------------------------------------------------
def build_material(obj, texdir, texpx, name):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nt = mat.node_tree
    for n in list(nt.nodes):
        nt.nodes.remove(n)
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
    nt.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])

    def load(fn, non_color):
        p = os.path.join(texdir, fn)
        if not os.path.exists(p):
            log(f"  WARN: no {fn} in {texdir}")
            return None
        img = bpy.data.images.load(p)
        if texpx and max(img.size) > texpx:
            img.scale(texpx, texpx)
        if non_color:
            img.colorspace_settings.name = "Non-Color"
        img.pack()
        n = nt.nodes.new("ShaderNodeTexImage")
        n.image = img
        return n

    base = load("texture_diffuse.png", False)
    if base:
        nt.links.new(base.outputs["Color"], bsdf.inputs["Base Color"])

    # THE NORMAL MAP IS THE WHOLE POINT at 5k tris.
    nrm = load("texture_normal.png", True)
    if nrm:
        nm = nt.nodes.new("ShaderNodeNormalMap")
        nt.links.new(nrm.outputs["Color"], nm.inputs["Color"])
        nt.links.new(nm.outputs["Normal"], bsdf.inputs["Normal"])
    else:
        log("  *** NO NORMAL MAP -- the creature will look like smooth plastic ***")

    rough = load("texture_roughness.png", True)
    if rough:
        nt.links.new(rough.outputs["Color"], bsdf.inputs["Roughness"])
    metal = load("texture_metallic.png", True)
    if metal:
        nt.links.new(metal.outputs["Color"], bsdf.inputs["Metallic"])

    obj.data.materials.clear()
    obj.data.materials.append(mat)
    return mat


# ----------------------------------------------------------------------------
# 1. ORIENT + normalize to 1 m along +X.
# ----------------------------------------------------------------------------
def orient_and_normalize(obj, orient_deg, flip_x):
    rot = Euler([math.radians(d) for d in orient_deg], "XYZ").to_matrix().to_4x4()
    obj.matrix_world = rot @ obj.matrix_world
    bpy.context.view_layer.update()
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)

    if flip_x:
        obj.matrix_world = Matrix.Scale(-1, 4, (1, 0, 0)) @ obj.matrix_world
        bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)
        # mirroring inverts winding -- flip normals back
        import bmesh
        bm = bmesh.new(); bm.from_mesh(obj.data)
        for f in bm.faces:
            f.normal_flip()
        bm.to_mesh(obj.data); bm.free()
        obj.data.update()

    # center on origin, scale so LENGTH (the forward axis, Y) == 1.0 m.
    # The game then scales each creature to its real size (kLen* in app/sealife.h).
    bb = [obj.matrix_world @ Vector(c) for c in obj.bound_box]
    lo = Vector((min(v.x for v in bb), min(v.y for v in bb), min(v.z for v in bb)))
    hi = Vector((max(v.x for v in bb), max(v.y for v in bb), max(v.z for v in bb)))
    ctr = (lo + hi) / 2.0
    length = (hi - lo).y
    obj.matrix_world = (Matrix.Scale(1.0 / length, 4) @ Matrix.Translation(-ctr)) @ obj.matrix_world
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

    bb = [Vector(c) for c in obj.bound_box]
    lo = Vector((min(v.x for v in bb), min(v.y for v in bb), min(v.z for v in bb)))
    hi = Vector((max(v.x for v in bb), max(v.y for v in bb), max(v.z for v in bb)))
    log(f"  oriented: bbox Y(fwd/len)={hi.y-lo.y:.3f} X(width)={hi.x-lo.x:.3f} "
        f"Z(up/height)={hi.z-lo.z:.3f}")


# ----------------------------------------------------------------------------
# 3. DECIMATE -- protect the silhouette.
#
# Local thickness by ray_cast against the mesh's own BVH: from just INSIDE the
# surface, march back along -normal; the distance to the far wall is how thick the
# creature is at that vertex. Fins/tentacles are thin; the body is thick.
# ----------------------------------------------------------------------------
def local_thickness(bvh, co, n, eps, maxd):
    """Distance from just under the surface at `co` to the FAR WALL along -n.

    THE TRAP: a ray fired from a point sitting on the surface re-hits the very
    triangle it started from at distance ~0, so EVERY vertex reads as paper-thin and
    the whole body gets 'protected' (that bug spared 50% of the shark and the
    decimator could not reach its target). Two guards:
      * we only accept a hit whose normal faces the SAME way as the ray -- that is
        the far wall seen from inside (dot(hit_n, dir) > 0). A self-hit or a re-entry
        on a front face has dot < 0 and is stepped past.
      * we step past rejected hits a few times before giving up.
    """
    d = -n
    o = co + d * eps
    travelled = eps
    for _ in range(6):
        hit = bvh.ray_cast(o, d, maxd)
        if not hit or hit[0] is None:
            return maxd
        loc, hn, _idx, dist = hit
        if hn.dot(d) > 0.0:                 # back face from the inside == the far wall
            return travelled + dist
        travelled += dist + eps             # front face: self-hit / re-entry -> step past
        o = loc + d * eps
    return maxd


def protect_weights(obj, thin_frac=0.022, thick_frac=0.070, fin_floor=0.12):
    """Vertex group 'decimate_ok': 1 = collapse me freely, low = I am the silhouette.

    fin_floor > 0 on purpose: a HARD 0 makes those verts uncollapsible, so a model
    with a big fin/tentacle mass can never reach its tri target no matter the ratio.
    A small floor lets fins shed a few tris while the body sheds most of them.
    """
    from mathutils.bvhtree import BVHTree
    me = obj.data
    me.calc_loop_triangles()
    bvh = BVHTree.FromPolygons([v.co for v in me.vertices],
                               [list(t.vertices) for t in me.loop_triangles],
                               all_triangles=True)
    bb = [Vector(c) for c in obj.bound_box]
    lo = Vector((min(v.x for v in bb), min(v.y for v in bb), min(v.z for v in bb)))
    hi = Vector((max(v.x for v in bb), max(v.y for v in bb), max(v.z for v in bb)))
    diag = (hi - lo).length
    eps = diag * 0.0008
    maxd = diag * 0.6

    thin_t = diag * thin_frac      # <= this -> a fin/tentacle: PROTECT
    thick_t = diag * thick_frac    # >= this -> body: decimate freely

    vg = obj.vertex_groups.new(name="decimate_ok")
    nprot = 0
    for v in me.vertices:
        t = local_thickness(bvh, v.co, v.normal, eps, maxd)
        if t <= thin_t:
            w = fin_floor
        elif t >= thick_t:
            w = 1.0
        else:
            u = (t - thin_t) / (thick_t - thin_t)
            s = u * u * (3.0 - 2.0 * u)      # smoothstep
            w = fin_floor + (1.0 - fin_floor) * s
        if w < 0.5:
            nprot += 1
        vg.add([v.index], w, "REPLACE")
    pct = 100.0 * nprot / max(1, len(me.vertices))
    log(f"  protect: {nprot}/{len(me.vertices)} verts ({pct:.0f}%) are fin/tentacle-thin "
        f"(thin<={thin_t:.4f} thick>={thick_t:.4f}, diag {diag:.3f}, floor {fin_floor})")
    if pct > 45.0:
        log("  *** WARNING: >45% 'thin' -- the thickness probe is probably self-hitting; "
            "fins will not be protected and the target will be missed ***")
    return vg


def decimate(obj, target_tris, vg):
    """Iterate: one Decimate pass cannot hit the target when a protected mass refuses
    to collapse, so we re-measure and re-apply until we converge or stop making
    progress. The protect group survives a collapse (Blender interpolates weights)."""
    obj.data.calc_loop_triangles()
    start = len(obj.data.loop_triangles)
    bpy.context.view_layer.objects.active = obj
    for it in range(5):
        obj.data.calc_loop_triangles()
        cur = len(obj.data.loop_triangles)
        if cur <= target_tris * 1.04:
            break
        m = obj.modifiers.new(f"dec{it}", "DECIMATE")
        m.decimate_type = "COLLAPSE"
        m.ratio = max(0.02, float(target_tris) / float(cur))
        m.vertex_group = vg.name
        m.vertex_group_factor = 1.0    # weight 1 -> full ratio, low weight -> spared
        m.use_collapse_triangulate = True
        bpy.ops.object.modifier_apply(modifier=m.name)
        obj.data.calc_loop_triangles()
        got = len(obj.data.loop_triangles)
        log(f"    pass {it}: {cur} -> {got} (ratio {m.ratio:.3f})")
        if got >= cur * 0.97:          # no progress -> the rest is protected mass
            break
    obj.data.calc_loop_triangles()
    log(f"  decimate: {start} -> {len(obj.data.loop_triangles)} tris (target {target_tris})")
    if vg.name in [g.name for g in obj.vertex_groups]:
        obj.vertex_groups.remove(obj.vertex_groups[vg.name])
    for p in obj.data.polygons:
        p.use_smooth = True


# ----------------------------------------------------------------------------
# 4. RIG. Bones along +X, roll aligned so local Z == world up.
#    -> rotation_euler.z = LATERAL sweep (fish), .x = VERTICAL sweep (whale fluke)
# ----------------------------------------------------------------------------
def build_spine(obj, nbones, y_head, y_tail, name="Spine"):
    """Bones run NOSE (+Y) -> TAIL (-Y). align_roll(+Z) puts bone-local Z on world up,
    which is what makes  euler.z == lateral sweep  and  euler.x == vertical sweep."""
    arm_data = bpy.data.armatures.new("rig")
    arm = bpy.data.objects.new("rig", arm_data)
    bpy.context.collection.objects.link(arm)
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode="EDIT")
    bones = []
    prev = None
    for i in range(nbones):
        a = y_head + (y_tail - y_head) * (i / nbones)
        b = y_head + (y_tail - y_head) * ((i + 1) / nbones)
        eb = arm_data.edit_bones.new(f"{name}{i}")
        eb.head = Vector((0, a, 0))
        eb.tail = Vector((0, b, 0))
        eb.align_roll(Vector((0, 0, 1)))   # bone local Z == world up
        if prev:
            eb.parent = prev
            eb.use_connect = True
        prev = eb
        bones.append(eb.name)
    bpy.ops.object.mode_set(mode="OBJECT")
    return arm, bones


def weight_spine(obj, arm, bones, y_head, y_tail):
    """Position-based: blend each vertex between the two spine bones it lies between."""
    for b in bones:
        obj.vertex_groups.new(name=b)
    n = len(bones)
    span = (y_tail - y_head)
    for v in obj.data.vertices:
        u = (v.co.y - y_head) / span * n   # bone-space coordinate in [0, n]
        u = max(0.0, min(float(n) - 1e-5, u))
        i = int(math.floor(u - 0.5))
        f = (u - 0.5) - i                 # 0..1 between bone i and i+1
        i0 = max(0, min(n - 1, i))
        i1 = max(0, min(n - 1, i + 1))
        if i0 == i1:
            obj.vertex_groups[bones[i0]].add([v.index], 1.0, "REPLACE")
        else:
            s = f * f * (3.0 - 2.0 * f)   # smoothstep -> no creasing at bone seams
            obj.vertex_groups[bones[i0]].add([v.index], 1.0 - s, "REPLACE")
            obj.vertex_groups[bones[i1]].add([v.index], s, "REPLACE")
    mod = obj.modifiers.new("skin", "ARMATURE")
    mod.object = arm
    obj.parent = arm


def build_squid_rig(obj, spec):
    """Axis chain (mantle -> arms) + N angular TENTACLE sectors so the arms drift
    out of phase instead of swinging as one rigid broom."""
    bb = [Vector(c) for c in obj.bound_box]
    y0 = min(v.y for v in bb); y1 = max(v.y for v in bb)   # y1 = mantle tip (+Y, forward)
    # Arms trail behind (-Y). The mantle occupies roughly the front 45%.
    y_arm = y0 + (y1 - y0) * 0.55
    nsec = spec["tent_sectors"]; ntb = spec["tent_bones"]

    arm_data = bpy.data.armatures.new("rig")
    arm = bpy.data.objects.new("rig", arm_data)
    bpy.context.collection.objects.link(arm)
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode="EDIT")

    # axis chain runs mantle tip (+Y) back to the arm root
    nb = spec["bones"]
    axis_bones = []
    prev = None
    for i in range(nb):
        a = y1 - (y1 - y_arm) * (i / nb)
        b = y1 - (y1 - y_arm) * ((i + 1) / nb)
        eb = arm_data.edit_bones.new(f"Axis{i}")
        eb.head = Vector((0, a, 0)); eb.tail = Vector((0, b, 0))
        eb.align_roll(Vector((0, 0, 1)))
        if prev:
            eb.parent = prev; eb.use_connect = True
        prev = eb
        axis_bones.append(eb.name)
    root = prev

    # one chain per angular sector, running back along -Y through the arm fan
    sec_bones = []
    for s in range(nsec):
        chain = []
        p = root
        for j in range(ntb):
            a = y_arm - (y_arm - y0) * (j / ntb)
            b = y_arm - (y_arm - y0) * ((j + 1) / ntb)
            eb = arm_data.edit_bones.new(f"Tent{s}_{j}")
            eb.head = Vector((0, a, 0)); eb.tail = Vector((0, b, 0))
            eb.align_roll(Vector((0, 0, 1)))
            eb.parent = p
            eb.use_connect = (j > 0)
            p = eb
            chain.append(eb.name)
        sec_bones.append(chain)
    bpy.ops.object.mode_set(mode="OBJECT")

    for b in axis_bones:
        obj.vertex_groups.new(name=b)
    for chain in sec_bones:
        for b in chain:
            obj.vertex_groups.new(name=b)

    # weights
    for v in obj.data.vertices:
        if v.co.y >= y_arm:
            # mantle: blend along the axis chain
            u = (y1 - v.co.y) / max(1e-6, (y1 - y_arm)) * nb
            u = max(0.0, min(float(nb) - 1e-5, u))
            i = int(math.floor(u - 0.5)); f = (u - 0.5) - i
            i0 = max(0, min(nb - 1, i)); i1 = max(0, min(nb - 1, i + 1))
            if i0 == i1:
                obj.vertex_groups[axis_bones[i0]].add([v.index], 1.0, "REPLACE")
            else:
                s = f * f * (3.0 - 2.0 * f)
                obj.vertex_groups[axis_bones[i0]].add([v.index], 1.0 - s, "REPLACE")
                obj.vertex_groups[axis_bones[i1]].add([v.index], s, "REPLACE")
        else:
            # arms: angular sector lobes (around the forward axis Y) x axial position
            th = math.atan2(v.co.z, v.co.x)
            u = (y_arm - v.co.y) / max(1e-6, (y_arm - y0)) * ntb
            u = max(0.0, min(float(ntb) - 1e-5, u))
            j = int(math.floor(u - 0.5)); f = (u - 0.5) - j
            j0 = max(0, min(ntb - 1, j)); j1 = max(0, min(ntb - 1, j + 1))
            sm = f * f * (3.0 - 2.0 * f)
            lobes = []
            tot = 0.0
            for s in range(nsec):
                thk = 2.0 * math.pi * s / nsec
                c = math.cos(th - thk)
                w = max(0.0, c) ** 2      # cos^2 lobe -> smooth blend across sectors
                lobes.append(w); tot += w
            if tot < 1e-6:
                lobes = [1.0 / nsec] * nsec; tot = 1.0
            for s in range(nsec):
                w = lobes[s] / tot
                if w <= 1e-4:
                    continue
                if j0 == j1:
                    obj.vertex_groups[sec_bones[s][j0]].add([v.index], w, "REPLACE")
                else:
                    obj.vertex_groups[sec_bones[s][j0]].add([v.index], w * (1.0 - sm), "REPLACE")
                    obj.vertex_groups[sec_bones[s][j1]].add([v.index], w * sm, "REPLACE")

    mod = obj.modifiers.new("skin", "ARMATURE")
    mod.object = arm
    obj.parent = arm
    return arm, axis_bones, sec_bones


# ----------------------------------------------------------------------------
# 5. BAKE -- travelling sine, keyed at every frame in [0, N] INCLUSIVE so the wrap
#    is exact (frame N == frame 0).
# ----------------------------------------------------------------------------
def bake_clip(arm, chains, clip_name, frames, amp_tip, lag, axis):
    """chains: list of (bone_name_list, amp_scale, phase_offset)."""
    for pb in arm.pose.bones:
        pb.rotation_mode = "XYZ"
        pb.rotation_euler = (0, 0, 0)

    act = bpy.data.actions.new(clip_name)
    if not arm.animation_data:
        arm.animation_data_create()
    arm.animation_data.action = act

    idx = {"vertical": 0, "lateral": 2}[axis]

    for f in range(frames + 1):                 # INCLUSIVE -> frame N == frame 0
        phase = 2.0 * math.pi * f / frames
        for bones, amp_scale, ph_off in chains:
            n = len(bones)
            for i, bn in enumerate(bones):
                pb = arm.pose.bones[bn]
                # amplitude ramps toward the tail: the body S-flexes, the tail beats
                ramp = (i + 1) / n
                amp = amp_tip * amp_scale * ramp * ramp
                ang = amp * math.sin(phase - i * lag + ph_off)
                e = [0.0, 0.0, 0.0]
                e[idx] = ang
                pb.rotation_euler = e
                pb.keyframe_insert("rotation_euler", frame=f, group=bn)

    for fc in act.fcurves:
        for kp in fc.keyframe_points:
            kp.interpolation = "BEZIER"
            kp.handle_left_type = "AUTO_CLAMPED"
            kp.handle_right_type = "AUTO_CLAMPED"

    act.use_fake_user = True
    arm.animation_data.action = None
    return act


# ----------------------------------------------------------------------------
def bake_species(key, out_glb):
    spec = SPECIES[key]
    log(f"=== {key} -> {out_glb}")
    clear_scene()
    bpy.context.scene.render.fps = 24

    if not os.path.exists(spec["src"]):
        raise RuntimeError(f"source missing: {spec['src']}")
    bpy.ops.wm.obj_import(filepath=spec["src"])
    objs = [o for o in bpy.context.scene.objects if o.type == "MESH"]
    if not objs:
        raise RuntimeError("no mesh in OBJ")
    obj = objs[0]
    obj.name = key
    obj.data.calc_loop_triangles()
    log(f"  imported: {len(obj.data.vertices)} verts, {len(obj.data.loop_triangles)} tris")

    orient_and_normalize(obj, spec["orient"], spec.get("flip_x", False))
    build_material(obj, spec["tex"], spec["texpx"], f"{key}_mat")

    vg = protect_weights(obj)
    decimate(obj, spec["tris"], vg)

    if not obj.data.uv_layers:
        log("  *** NO UVs -- textures cannot bind ***")

    bb = [Vector(c) for c in obj.bound_box]
    y0 = min(v.y for v in bb); y1 = max(v.y for v in bb)   # y1 = nose, y0 = tail

    actions = []
    if spec["rig"] == "squid":
        arm, axis_bones, sec_bones = build_squid_rig(obj, spec)
        for (cn, fr, amp, lag, ax) in spec["clips"]:
            chains = [(axis_bones, 0.5, 0.0)]
            for s, chain in enumerate(sec_bones):
                # each arm sector gets its own phase -> the fan drifts, not swings
                chains.append((chain, 1.0, 2.0 * math.pi * s / len(sec_bones)))
            actions.append(bake_clip(arm, chains, cn, fr, amp, lag, ax))
            log(f"  clip {cn}: {fr}f @24fps ({fr/24.0:.2f}s) amp={amp} lag={lag} {ax}")
    else:
        # spine: nose at +Y, tail at -Y -> bones run FORWARD-to-BACK
        arm, bones = build_spine(obj, spec["bones"], y1, y0)
        weight_spine(obj, arm, bones, y1, y0)
        for (cn, fr, amp, lag, ax) in spec["clips"]:
            actions.append(bake_clip(arm, [(bones, 1.0, 0.0)], cn, fr, amp, lag, ax))
            log(f"  clip {cn}: {fr}f @24fps ({fr/24.0:.2f}s) amp={amp} lag={lag} {ax}")

    os.makedirs(os.path.dirname(out_glb), exist_ok=True)
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.export_scene.gltf(
        filepath=out_glb,
        export_format="GLB",
        use_selection=True,
        export_animations=True,
        export_animation_mode="ACTIONS",   # every action -> its own glTF animation
        export_bake_animation=True,
        export_skins=True,
        export_yup=True,
        export_tangents=True,              # normal map needs tangents
        export_normals=True,
        export_apply=False,
    )
    sz = os.path.getsize(out_glb)
    obj.data.calc_loop_triangles()
    log(f"  EXPORTED {out_glb} {sz/1e6:.2f} MB, {len(obj.data.loop_triangles)} tris, "
        f"clips={[a.name for a in actions]}")
    return sz


def main():
    if len(ARGV) < 2:
        print("usage: sealife_bake.py -- <species|all> <out.glb|outdir> [donefile]")
        sys.exit(1)
    which, out = ARGV[0], ARGV[1]
    done = ARGV[2] if len(ARGV) > 2 else None
    _LOGF[0] = (out + ".log") if which != "all" else os.path.join(out, "sealife_bake.log")

    status = "OK"
    try:
        if which == "all":
            for k in SPECIES:
                bake_species(k, os.path.join(out, f"{k}.glb"))
        else:
            if which not in SPECIES:
                raise RuntimeError(f"unknown species '{which}' (have {list(SPECIES)})")
            bake_species(which, out)
    except Exception as e:
        import traceback
        log("ERR:", e)
        log(traceback.format_exc())
        status = "FAIL " + str(e)

    if done:
        with open(done, "w") as f:
            f.write(status + "\n")
    log("status:", status)


main()
