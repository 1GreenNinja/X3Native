"""saurian_forge.py -- headless Blender rig+skin+tail forge for canon_saurian.

The Saurian Soldier shipped with ONE bone (Root): every clip slides the frozen
mesh. This tool (the wolf_forge pattern applied to a hunched biped) builds a
MESHY-NAMED 24-joint humanoid skeleton fitted to the saurian's measured
anatomy PLUS a 4-bone tail chain, skins it with capsule-distance weights +
geodesic shell ownership, and (stage tail) layers procedural tail sway onto
every retargeted clip. The Meshy bone names make the grey's articulated clip
set retargetable via tools/retarget_library.py rig "meshy_from_meshy".

Stages (launcher DETACHES: poll <workdir>/<stage>.done):

  blender-launcher.exe --background --python tools/saurian_forge.py -- \
      rig <src.glb> <workdir> <out_rigged.glb>
    Fit + build skeleton, skin, QA stress renders, export rigged GLB (no
    clips) + save saurian_rig.blend.

  blender-launcher.exe --background --python tools/saurian_forge.py -- \
      tail <retargeted.glb> <workdir> <out_final.glb>
    Add lagged sine tail sway to every action, export the final GLB.

Anatomy (measured, Blender Z-up after glTF import; head at -Y, tail at +Y,
feet on z=0, height 2.18 m): hunched raptor -- torso leans forward, tail
trails low behind (z<0.6, y up to +0.74), arms hang wide at x ~ +/-0.40.

Clean-room: public Blender Python API + glTF 2.0 spec only.
"""
import bpy, sys, os, math, heapq, time
import numpy as np
from mathutils import Vector

ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(ARGV) < 4:
    raise SystemExit("usage: -- rig <src.glb> <workdir> <out.glb> | -- tail <src.glb> <workdir> <out.glb>")
STAGE, SRC, WORKDIR, OUT = ARGV[0], ARGV[1], ARGV[2], ARGV[3]

_log = []
LOG_PATH = os.path.join(WORKDIR, "saurian_%s.log" % STAGE)
DONE_PATH = os.path.join(WORKDIR, "saurian_%s.done" % STAGE)
def log(*a):
    s = "[saurian_forge] " + " ".join(str(x) for x in a)
    _log.append(s); print(s, flush=True)
def flush(status):
    try:
        with open(LOG_PATH, "w", encoding="utf-8") as f: f.write("\n".join(_log))
        with open(DONE_PATH, "w", encoding="utf-8") as f: f.write(status)
    except Exception as e:
        print("[saurian_forge] log write failed:", e)

D = math.radians

def seg_dist(P, a, b):
    d = b - a
    l2 = max(1e-12, float(d @ d))
    t = np.clip(((P - a) @ d) / l2, 0.0, 1.0)
    proj = a + t[:, None] * d
    return np.linalg.norm(P - proj, axis=1)

# ---------------------------------------------------------------------------
# Regions for ownership (Meshy names + tail)
# ---------------------------------------------------------------------------
REGION_BONES = {
    "TORSO": ["Hips", "Spine02", "Spine01", "Spine"],
    "HEAD":  ["neck", "Head"],
    "ARM_L": ["LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand"],
    "ARM_R": ["RightShoulder", "RightArm", "RightForeArm", "RightHand"],
    "LEG_L": ["LeftUpLeg", "LeftLeg", "LeftFoot", "LeftToeBase"],
    "LEG_R": ["RightUpLeg", "RightLeg", "RightFoot", "RightToeBase"],
    "TAIL":  ["Tail1", "Tail2", "Tail3", "Tail4"],
}
BONE2REGION = {b: r for r, bs in REGION_BONES.items() for b in bs}
REGIONS = list(REGION_BONES)
RIDX = {r: i for i, r in enumerate(REGIONS)}
# blend adjacency (which region pairs may share weights at their frontier)
ADJ_PAIRS = {frozenset(p) for p in [
    ("TORSO", "HEAD"), ("TORSO", "ARM_L"), ("TORSO", "ARM_R"),
    ("TORSO", "LEG_L"), ("TORSO", "LEG_R"), ("TORSO", "TAIL"),
    ("LEG_L", "LEG_R"), ("LEG_L", "TAIL"), ("LEG_R", "TAIL"),
]}

_RADII = {
    "Hips": 0.20, "Spine02": 0.20, "Spine01": 0.20, "Spine": 0.19,
    "neck": 0.11, "Head": 0.14,
    "LeftShoulder": 0.10, "LeftArm": 0.085, "LeftForeArm": 0.07, "LeftHand": 0.07,
    "RightShoulder": 0.10, "RightArm": 0.085, "RightForeArm": 0.07, "RightHand": 0.07,
    "LeftUpLeg": 0.13, "LeftLeg": 0.095, "LeftFoot": 0.075, "LeftToeBase": 0.07,
    "RightUpLeg": 0.13, "RightLeg": 0.095, "RightFoot": 0.075, "RightToeBase": 0.07,
    "Tail1": 0.16, "Tail2": 0.13, "Tail3": 0.10, "Tail4": 0.08,
}

def reset_factory():
    bpy.ops.wm.read_factory_settings(use_empty=True)

def pack_textures():
    for img in bpy.data.images:
        try:
            if not img.has_data: _ = img.pixels[0]
            if not img.packed_file: img.pack()
            img.use_fake_user = True
        except Exception as e:
            log("img pack warn", img.name, e)
    for m in bpy.data.materials:
        m.use_fake_user = True

def world_verts(mesh):
    n = len(mesh.data.vertices)
    co = np.empty(n * 3, dtype=np.float64)
    mesh.data.vertices.foreach_get("co", co)
    co = co.reshape(-1, 3)
    M = np.array(mesh.matrix_world)
    return co @ M[:3, :3].T + M[:3, 3]

# ---------------------------------------------------------------------------
# Fit the skeleton to the mesh
# ---------------------------------------------------------------------------
def centroid(W, m):
    return W[m].mean(axis=0) if m.any() else None

def fit_skeleton(W):
    """Measured landmark fit. Head at -Y, tail +Y, feet z=0."""
    x, y, z = W[:, 0], W[:, 1], W[:, 2]
    # tail verts: behind the body, low
    tail_m = (y > 0.12) & (z < 0.95)
    # feet
    L = x > 0
    lm = {}
    for side, sm in (("Left", L), ("Right", ~L)):
        foot = sm & (z < 0.10) & ~tail_m
        ank = sm & (z > 0.10) & (z < 0.24) & ~tail_m & (np.abs(x) > 0.05)
        knee = sm & (z > 0.52) & (z < 0.70) & ~tail_m & (np.abs(x) > 0.05) & (y < 0.15)
        c_foot = centroid(W, foot); c_ank = centroid(W, ank); c_knee = centroid(W, knee)
        # toes = forwardmost foot verts
        toe = foot & (y < (c_foot[1] - 0.05 if c_foot is not None else -0.2))
        c_toe = centroid(W, toe)
        lm[side + "Foot"] = c_ank
        lm[side + "Toe"] = c_toe if c_toe is not None else c_foot
        lm[side + "Knee"] = c_knee
    # arms: hang vertically beside the torso (x ~ +/-0.28, y ~ -0.5) from the
    # shoulder (z~1.32) down to the clawed hand (z~0.83). Order along -Z.
    for side, sm in (("Left", L), ("Right", ~L)):
        armm = sm & (np.abs(x) > 0.24) & (z > 0.78) & (z < 1.32) & (y < -0.2)
        if armm.sum() < 40:
            armm = sm & (np.abs(x) > 0.20) & (z > 0.72) & (z < 1.38) & (y < -0.1)
        A = W[armm]
        t = -A[:, 2]                       # top (shoulder) -> bottom (hand)
        qs = np.quantile(t, [0.03, 0.45, 0.80, 0.98])
        pts = []
        for q0, q1 in zip(qs[:-1], qs[1:]):
            mm = (t >= q0) & (t <= q1)
            pts.append(A[mm].mean(axis=0))
        lm[side + "ArmChain"] = pts  # shoulder, elbow, hand
    # torso / head
    hips_m = (z > 0.95) & (z < 1.25) & (np.abs(x) < 0.25) & ~tail_m
    lm["Hips"] = centroid(W, hips_m)
    chest_m = (z > 1.30) & (z < 1.55) & (np.abs(x) < 0.30) & (y < 0.1)
    lm["Chest"] = centroid(W, chest_m)
    neck_m = (z > 1.60) & (z < 1.80) & (np.abs(x) < 0.2)
    lm["NeckBase"] = centroid(W, neck_m)
    head_m = z > 1.85
    lm["HeadC"] = centroid(W, head_m)
    snout = head_m & (y < np.quantile(y[head_m], 0.06))
    lm["Snout"] = centroid(W, snout)
    # tail chain by y quantiles
    T = W[tail_m]
    ty = T[:, 1]
    qs = np.quantile(ty, [0.02, 0.30, 0.55, 0.78, 0.98])
    tpts = []
    for q0, q1 in zip(qs[:-1], qs[1:]):
        mm = (ty >= q0) & (ty <= q1)
        tpts.append(T[mm].mean(axis=0))
    lm["TailChain"] = tpts
    return lm

def skeleton_def(lm):
    """Meshy-named bones from landmarks. Returns [(name, head, tail, parent)]."""
    V = lambda p: Vector((float(p[0]), float(p[1]), float(p[2])))
    hips = V(lm["Hips"]); chest = V(lm["Chest"])
    neckb = V(lm["NeckBase"]); headc = V(lm["HeadC"]); snout = V(lm["Snout"])
    # spine: 3 points between hips and chest, then neck
    s1 = hips.lerp(chest, 0.4); s2 = hips.lerp(chest, 0.75)
    B = [
        ("Hips",    hips, s1, None),
        ("Spine02", s1, s2, "Hips"),
        ("Spine01", s2, chest, "Spine02"),
        ("Spine",   chest, neckb, "Spine01"),
        ("neck",    neckb, neckb.lerp(headc, 0.6), "Spine"),
        ("Head",    neckb.lerp(headc, 0.6), headc + (headc - neckb) * 0.35, "neck"),
        ("head_end", headc + (headc - neckb) * 0.35,
                     headc + (headc - neckb) * 0.55, "Head"),
        ("headfront", headc, snout, "Head"),
    ]
    tp = [V(p) for p in lm["TailChain"]]
    B += [
        ("Tail1", tp[0], tp[1], "Hips"),
        ("Tail2", tp[1], tp[2], "Tail1"),
        ("Tail3", tp[2], tp[3], "Tail2"),
        ("Tail4", tp[3], tp[3] + (tp[3] - tp[2]) * 0.8, "Tail3"),
    ]
    for side in ("Left", "Right"):
        sh, el, ha = [V(p) for p in lm[side + "ArmChain"]]
        shoulder_root = chest.lerp(sh, 0.35)
        B += [
            (side + "Shoulder", shoulder_root, sh, "Spine"),
            (side + "Arm", sh, el, side + "Shoulder"),
            (side + "ForeArm", el, ha, side + "Arm"),
            (side + "Hand", ha, ha + (ha - el) * 0.45, side + "ForeArm"),
        ]
        kn = V(lm[side + "Knee"]); an = V(lm[side + "Foot"]); to = V(lm[side + "Toe"])
        upl = hips.lerp(kn, 0.18); upl.z = hips.z - 0.05
        B += [
            (side + "UpLeg", upl, kn, "Hips"),
            (side + "Leg", kn, an, side + "UpLeg"),
            (side + "Foot", an, an.lerp(to, 0.6), side + "Leg"),
            (side + "ToeBase", an.lerp(to, 0.6), to, side + "Foot"),
        ]
    return B

# ---------------------------------------------------------------------------
# Skinning: capsule + geodesic shell ownership (wolf-pattern, no originals)
# ---------------------------------------------------------------------------
def skin(mesh, arm):
    t0 = time.time()
    n = len(mesh.data.vertices)
    W = world_verts(mesh)
    segs = []
    M = arm.matrix_world
    for b in arm.data.bones:
        if not b.use_deform or b.name not in BONE2REGION:
            continue
        h = np.array(M @ b.head_local); t = np.array(M @ b.tail_local)
        segs.append((b.name, h, t, _RADII[b.name]))
    names = [s[0] for s in segs]
    nb = len(segs)
    dist = np.empty((nb, n))
    for i, (nm, h, t, r) in enumerate(segs):
        dist[i] = seg_dist(W, h, t)
    radii = np.array([s[3] for s in segs])
    ndist = dist / radii[:, None]
    reg_nd = np.full((len(REGIONS), n), 1e9)
    for i, nm in enumerate(names):
        ri = RIDX[BONE2REGION[nm]]
        reg_nd[ri] = np.minimum(reg_nd[ri], ndist[i])
    order = np.argsort(reg_nd, axis=0)
    best, second = order[0], order[1]
    nd_best = reg_nd[best, np.arange(n)]
    nd_second = reg_nd[second, np.arange(n)]
    seeds = (nd_best < 1.1) & (nd_second > 1.3 * nd_best)
    log("seeds:", int(seeds.sum()), "/", n)

    ne = len(mesh.data.edges)
    ev = np.empty(ne * 2, dtype=np.int64)
    mesh.data.edges.foreach_get("vertices", ev)
    ev = ev.reshape(-1, 2)
    elen = np.linalg.norm(W[ev[:, 0]] - W[ev[:, 1]], axis=1)
    adj = [[] for _ in range(n)]
    for (a, b), l in zip(ev, elen):
        adj[a].append((b, l)); adj[b].append((a, l))
    owner = np.full(n, -1, dtype=np.int32)
    gdist = np.full(n, np.inf)
    pq = []
    for v in np.nonzero(seeds)[0]:
        owner[v] = best[v]; gdist[v] = 0.0
        heapq.heappush(pq, (0.0, int(v)))
    while pq:
        dv, v = heapq.heappop(pq)
        if dv > gdist[v] + 1e-12:
            continue
        for u, l in adj[v]:
            nd2 = dv + l
            if nd2 < gdist[u] - 1e-12:
                gdist[u] = nd2; owner[u] = owner[v]
                heapq.heappush(pq, (nd2, u))
    un = np.nonzero(owner < 0)[0]
    if len(un):
        for v in un:
            owner[v] = best[v]
    for _ in range(3):
        flips = 0
        no = owner.copy()
        for v in range(n):
            cnt = {}
            for u, _ in adj[v]:
                cnt[owner[u]] = cnt.get(owner[u], 0) + 1
            if not cnt:
                continue
            tr, tc = max(cnt.items(), key=lambda kv: kv[1])
            if tr != owner[v] and tc >= 0.7 * len(adj[v]) and reg_nd[tr, v] < 3.5:
                no[v] = tr; flips += 1
        owner = no
        if flips == 0:
            break
    log("ownership:", {REGIONS[r]: int((owner == r).sum()) for r in range(len(REGIONS))})

    # frontier blend band (geodesic)
    band = 0.14
    fdist = np.full(n, np.inf)
    fpair = [None] * n
    pq = []
    for a, b in ev:
        ra, rb = int(owner[a]), int(owner[b])
        if ra == rb:
            continue
        pair = frozenset((REGIONS[ra], REGIONS[rb]))
        if pair not in ADJ_PAIRS:
            continue
        for v in (int(a), int(b)):
            if fdist[v] > 0.0:
                fdist[v] = 0.0; fpair[v] = pair
                heapq.heappush(pq, (0.0, v))
    while pq:
        dv, v = heapq.heappop(pq)
        if dv > fdist[v] + 1e-12 or dv > band:
            continue
        for u, l in adj[v]:
            nd2 = dv + l
            if nd2 < fdist[u] - 1e-12 and nd2 <= band:
                fdist[u] = nd2; fpair[u] = fpair[v]
                heapq.heappush(pq, (nd2, u))

    bone_idx_by_region = {r: [i for i, nm in enumerate(names) if BONE2REGION[nm] == r]
                          for r in REGIONS}
    inv = np.power(np.maximum(ndist, 0.12), -3.0)
    mesh.vertex_groups.clear()
    groups = {nm: mesh.vertex_groups.new(name=nm) for nm in names}
    for v in range(n):
        rset = {REGIONS[owner[v]]}
        if fpair[v] is not None and fdist[v] <= band:
            rset |= set(fpair[v])
        bidx = []
        for r in rset:
            bidx.extend(bone_idx_by_region[r])
        wv = inv[bidx, v]
        top = np.argsort(wv)[::-1][:4]
        wmax = wv[top[0]]
        picked = [(bidx[t], float(wv[t])) for t in top if wv[t] > 0.06 * wmax]
        tot = sum(x for _, x in picked)
        for bi, x in picked:
            groups[names[bi]].add([v], x / tot, 'REPLACE')
    for o in bpy.data.objects: o.select_set(False)
    mesh.select_set(True)
    bpy.context.view_layer.objects.active = mesh
    try:
        bpy.ops.object.mode_set(mode='WEIGHT_PAINT')
        bpy.ops.object.vertex_group_smooth(group_select_mode='ALL', factor=0.5,
                                           repeat=3, expand=0.0)
        bpy.ops.object.mode_set(mode='OBJECT')
    except Exception as e:
        log("smooth skipped:", e)
        try: bpy.ops.object.mode_set(mode='OBJECT')
        except Exception: pass
    bpy.ops.object.vertex_group_limit_total(group_select_mode='ALL', limit=4)
    bpy.ops.object.vertex_group_clean(group_select_mode='ALL', limit=0.015)
    bpy.ops.object.vertex_group_normalize_all(group_select_mode='ALL', lock_active=False)
    unweighted = sum(1 for v in mesh.data.vertices
                     if not any(g.weight > 1e-4 for g in v.groups))
    log("skin done in %.1fs, unweighted:" % (time.time() - t0), unweighted)

# ---------------------------------------------------------------------------
# QA scene (grounded floor + level cam)
# ---------------------------------------------------------------------------
def qa_scene():
    scn = bpy.context.scene
    bpy.ops.mesh.primitive_plane_add(size=30, location=(0, 0, 0))
    floor = bpy.context.object; floor.name = "QAFloor"
    fm = bpy.data.materials.new("QAFloorMat"); fm.use_nodes = True
    fm.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (0.32, 0.33, 0.36, 1)
    floor.data.materials.append(fm)
    sun = bpy.data.objects.new("QASun", bpy.data.lights.new("QASun", 'SUN'))
    sun.data.energy = 4.0; sun.rotation_euler = (D(50), 0, D(35))
    bpy.context.collection.objects.link(sun)
    w = bpy.data.worlds.new("QAW"); scn.world = w; w.use_nodes = True
    w.node_tree.nodes["Background"].inputs[0].default_value = (0.18, 0.20, 0.24, 1)
    cam = bpy.data.objects.new("QACam", bpy.data.cameras.new("QACam"))
    bpy.context.collection.objects.link(cam); scn.camera = cam
    try: scn.render.engine = 'BLENDER_EEVEE_NEXT'
    except Exception: scn.render.engine = 'BLENDER_EEVEE'
    scn.render.resolution_x = 1152; scn.render.resolution_y = 964

def qa_shot(path, view="side"):
    deps = bpy.context.evaluated_depsgraph_get()
    mins = Vector((1e9, 1e9, 1e9)); maxs = Vector((-1e9, -1e9, -1e9))
    for o in bpy.data.objects:
        if o.type != 'MESH' or o.name == "QAFloor":
            continue
        oe = o.evaluated_get(deps)
        for c in oe.bound_box:
            wc = oe.matrix_world @ Vector(c)
            for i in range(3):
                mins[i] = min(mins[i], wc[i]); maxs[i] = max(maxs[i], wc[i])
    center = (mins + maxs) * 0.5
    h = max(1.0, maxs.z - mins.z)
    cam = bpy.data.objects["QACam"]
    dist = h * 2.2
    camz = min(center.z, mins.z + h * 0.5)
    if view == "side":
        cam.location = (center.x - dist, center.y, camz)
        cam.rotation_euler = (D(86), 0, D(-90))
    elif view == "front":
        cam.location = (center.x, center.y - dist, camz)
        cam.rotation_euler = (D(86), 0, 0)
    else:
        cam.location = (center.x - dist * 0.75, center.y - dist * 0.75, camz)
        cam.rotation_euler = (D(82), 0, D(-45))
    cam.data.type = 'PERSP'; cam.data.lens = 45
    bpy.context.scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    log("render:", os.path.basename(path))

def qrest(pb):
    return pb.bone.matrix_local.to_quaternion()

def set_wrot(arm, name, rx=0.0, ry=0.0, rz=0.0):
    from mathutils import Euler
    pb = arm.pose.bones.get(name)
    if not pb: return None
    Wq = Euler((rx, ry, rz), 'XYZ').to_quaternion()
    qr = qrest(pb)
    pb.rotation_mode = 'QUATERNION'
    pb.rotation_quaternion = qr.inverted() @ Wq @ qr
    return pb

def clear_pose(arm):
    for pb in arm.pose.bones:
        pb.rotation_mode = 'QUATERNION'
        pb.rotation_quaternion = (1, 0, 0, 0)
        pb.location = (0, 0, 0); pb.scale = (1, 1, 1)

# ---------------------------------------------------------------------------
# Stage rig
# ---------------------------------------------------------------------------
def stage_rig():
    reset_factory()
    bpy.ops.import_scene.gltf(filepath=SRC)
    for a in list(bpy.data.actions):
        bpy.data.actions.remove(a)
    mesh = max([o for o in bpy.data.objects if o.type == 'MESH'],
               key=lambda o: len(o.data.vertices))
    mesh.name = "Character"
    for o in list(bpy.data.objects):
        if o.type == 'MESH' and o is not mesh and len(o.data.vertices) < 1000:
            log("removing stray:", o.name)
            bpy.data.objects.remove(o, do_unlink=True)
        elif o.type == 'ARMATURE':
            log("removing old armature:", o.name)
            bpy.data.objects.remove(o, do_unlink=True)
    mesh.parent = None
    for mod in list(mesh.modifiers):
        mesh.modifiers.remove(mod)
    mesh.vertex_groups.clear()
    pack_textures()

    W = world_verts(mesh)
    lm = fit_skeleton(W)
    for k, v in lm.items():
        if isinstance(v, list):
            log("landmark", k, [tuple(round(float(c), 2) for c in p) for p in v])
        elif v is not None:
            log("landmark", k, tuple(round(float(c), 2) for c in v))
    arm_data = bpy.data.armatures.new("Arm")
    arm = bpy.data.objects.new("Arm", arm_data)
    bpy.context.collection.objects.link(arm)
    for o in bpy.data.objects: o.select_set(False)
    arm.select_set(True); bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode='EDIT')
    ebs = {}
    for name, head, tail, parent in skeleton_def(lm):
        eb = arm_data.edit_bones.new(name)
        eb.head = head; eb.tail = tail; eb.use_connect = False
        if parent: eb.parent = ebs[parent]
        d = (tail - head).normalized()
        zt = Vector((1, 0, 0)).cross(d)
        if zt.length > 1e-4:
            eb.align_roll(zt.normalized())
        ebs[name] = eb
    bpy.ops.object.mode_set(mode='OBJECT')
    log("built skeleton:", len(arm_data.bones), "bones")

    # bind mesh to armature
    mesh.parent = arm
    mod = mesh.modifiers.new("Armature", 'ARMATURE')
    mod.object = arm
    skin(mesh, arm)

    blend = os.path.join(WORKDIR, "saurian_rig.blend")
    bpy.ops.wm.save_as_mainfile(filepath=blend)

    pack_textures()
    bpy.ops.export_scene.gltf(
        filepath=OUT, export_format='GLB', export_yup=True, use_selection=False,
        export_animations=False, export_materials='EXPORT',
        export_image_format='AUTO', export_texcoords=True, export_normals=True,
        export_tangents=True, export_skins=True)
    log("EXPORTED:", OUT, os.path.getsize(OUT), "bytes")

    # stress renders
    qa_scene()
    clear_pose(arm)
    qa_shot(os.path.join(WORKDIR, "rig_bind_side.png"), "side")
    qa_shot(os.path.join(WORKDIR, "rig_bind_front.png"), "front")
    set_wrot(arm, "LeftArm", rx=D(-70)); set_wrot(arm, "RightArm", rx=D(-70))
    set_wrot(arm, "LeftForeArm", rx=D(-20)); set_wrot(arm, "RightForeArm", rx=D(-20))
    qa_shot(os.path.join(WORKDIR, "rig_armsup_front.png"), "front")
    qa_shot(os.path.join(WORKDIR, "rig_armsup_side.png"), "side")
    clear_pose(arm)
    set_wrot(arm, "LeftUpLeg", rx=D(-40)); set_wrot(arm, "LeftLeg", rx=D(35))
    set_wrot(arm, "Tail1", rz=D(20)); set_wrot(arm, "Tail2", rz=D(25))
    set_wrot(arm, "Tail3", rz=D(30)); set_wrot(arm, "Tail4", rz=D(30))
    set_wrot(arm, "neck", rz=D(25)); set_wrot(arm, "Head", rz=D(15))
    qa_shot(os.path.join(WORKDIR, "rig_legtail_side.png"), "side")
    qa_shot(os.path.join(WORKDIR, "rig_legtail_persp.png"), "persp")
    clear_pose(arm)
    log("stage rig OK")

# ---------------------------------------------------------------------------
# Stage tail: lagged sine sway layered onto every action
# ---------------------------------------------------------------------------
def set_wloc(arm, name, x=0.0, y=0.0, z=0.0):
    pb = arm.pose.bones.get(name)
    if not pb: return None
    qr = qrest(pb)
    pb.location = qr.inverted() @ Vector((x, y, z))
    return pb

def mesh_min_z(arm):
    deps = bpy.context.evaluated_depsgraph_get()
    mz = 1e9
    for o in bpy.data.objects:
        if o.type != 'MESH' or o.name == "QAFloor":
            continue
        oe = o.evaluated_get(deps)
        me = oe.to_mesh()
        M = oe.matrix_world
        for v in me.vertices:
            w = M @ v.co
            if w.z < mz: mz = w.z
        oe.to_mesh_clear()
    return mz

def ground_clamp(arm, act, frames):
    """Shift the Hips location keys so the mesh's lowest point over the
    sampled frames sits on the floor (kills retarget float)."""
    arm.animation_data.action = act
    try:
        if getattr(act, "slots", None):
            arm.animation_data.action_slot = act.slots[0]
    except Exception:
        pass
    mz = 1e9
    for f in frames:
        bpy.context.scene.frame_set(int(f))
        bpy.context.view_layer.update()
        mz = min(mz, mesh_min_z(arm))
    if abs(mz) < 0.02:
        return 0.0
    pb = arm.pose.bones["Hips"]
    delta = qrest(pb).inverted() @ Vector((0, 0, -mz))
    for fc in act.fcurves:
        if fc.data_path == 'pose.bones["Hips"].location':
            for kp in fc.keyframe_points:
                kp.co[1] += delta[fc.array_index]
                kp.handle_left[1] += delta[fc.array_index]
                kp.handle_right[1] += delta[fc.array_index]
    log("ground_clamp %s: shifted %.3f m" % (act.name, -mz))
    return -mz

def author_death(arm):
    """Authored forward-collapse Death for the saurian (the grey's Death
    retargets upside-down on these proportions)."""
    old = bpy.data.actions.get("Death")
    if old is not None:
        bpy.data.actions.remove(old)
    act = bpy.data.actions.new("Death")
    act.use_fake_user = True
    arm.animation_data.action = act
    try:
        if getattr(act, "slots", None) is not None and len(act.slots) == 0:
            act.slots.new(id_type='OBJECT', name=arm.name)
            arm.animation_data.action_slot = act.slots[0]
    except Exception:
        pass
    clear_pose(arm)
    def key(f, bone, rx=0, ry=0, rz=0, loc=None):
        pb = set_wrot(arm, bone, rx, ry, rz)
        if pb is None: return
        pb.keyframe_insert("rotation_quaternion", frame=f)
        if loc is not None:
            set_wloc(arm, bone, *loc)
            pb.keyframe_insert("location", frame=f)
    def pose(f, hz, chest, head, ul, ll, foot, armr, tail, roll=0.0):
        key(f, "Hips", rx=chest * 0.4, ry=roll, loc=(0, 0, hz))
        key(f, "Spine01", rx=chest * 0.3); key(f, "Spine", rx=chest * 0.3)
        key(f, "neck", rx=head * 0.5); key(f, "Head", rx=head * 0.5)
        for sd in ("Left", "Right"):
            m = 1.0 if sd == "Left" else 0.85
            key(f, sd + "UpLeg", rx=ul * m)
            key(f, sd + "Leg", rx=ll * m)
            key(f, sd + "Foot", rx=foot * m)
            key(f, sd + "Arm", rx=armr * m, rz=D(18) * (1 if sd == "Left" else -1))
            key(f, sd + "ForeArm", rx=armr * 0.6 * m)
        for k, tb in enumerate(("Tail1", "Tail2", "Tail3", "Tail4")):
            key(f, tb, rx=tail * (0.5 + 0.3 * k))
    #     f    hipZ  chest    head     upleg    lowleg  foot    arm      tail
    pose(1,   0.00,  0,       0,       0,       0,      0,      0,       0)
    pose(6,  -0.06,  D(-8),   D(-10),  D(6),    D(-8),  D(4),   D(-25),  D(6))          # hit recoil
    pose(14, -0.42,  D(18),   D(14),   D(-55),  D(70),  D(-20), D(-35),  D(12))         # buckle to knees
    pose(24, -0.80,  D(52),   D(30),   D(-78),  D(95),  D(-30), D(-15),  D(20), D(12))  # pitch forward
    pose(32, -0.94,  D(66),   D(42),   D(-85),  D(100), D(-34), D(5),    D(10), D(20))  # down
    pose(40, -0.96,  D(68),   D(45),   D(-86),  D(100), D(-35), D(8),    D(6),  D(22))  # settle
    log("authored Death: 40 frames")
    return act

def stage_tail():
    reset_factory()
    bpy.ops.import_scene.gltf(filepath=SRC)
    arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE')
    if not arm.animation_data:
        arm.animation_data_create()
    author_death(arm)
    for nm, frames in (("Idle", (1, 15, 30)), ("Walk", (1, 8, 15, 22)),
                       ("Run", (1, 6, 11))):
        a = bpy.data.actions.get(nm)
        if a is not None:
            ground_clamp(arm, a, frames)
    fps = bpy.context.scene.render.fps
    TAILS = ["Tail1", "Tail2", "Tail3", "Tail4"]
    # per-clip sway character: (amp_deg, cycles_per_clip, vertical_amp)
    tune = {"Idle": (8, 1.0, 2.5), "Walk": (12, 2.0, 3), "Run": (14, 3.0, 5),
            "Attack": (16, 1.0, 6), "Attack2": (16, 1.0, 6),
            "Hitreaction": (10, 1.0, 4), "Death": (6, 0.5, -8),
            "Taunt": (14, 2.0, 4)}
    for act in list(bpy.data.actions):
        amp, cyc, vamp = tune.get(act.name, (10, 1.5, 3))
        arm.animation_data.action = act
        try:
            if getattr(act, "slots", None):
                arm.animation_data.action_slot = act.slots[0]
        except Exception:
            pass
        f0, f1 = act.frame_range
        nfr = max(2, int(f1 - f0) + 1)
        loop = act.name in ("Idle", "Walk", "Run", "Taunt")
        for i in range(nfr):
            f = f0 + i
            p = i / float(max(1, nfr - 1))
            for k, tb in enumerate(TAILS):
                ph = math.tau * cyc * p - 0.6 * k
                rz = D(amp) * (0.5 + 0.5 * k) * math.sin(ph)
                rx = D(vamp) * 0.4 * (0.5 + 0.5 * k) * math.sin(2 * math.tau * cyc * p - 0.5 * k)
                if loop and i == nfr - 1:
                    ph0 = -0.6 * k
                    rz = D(amp) * (0.5 + 0.5 * k) * math.sin(ph0)
                    rx = D(vamp) * 0.4 * (0.5 + 0.5 * k) * math.sin(-0.5 * k)
                pb = set_wrot(arm, tb, rx=rx, rz=rz)
                if pb:
                    pb.keyframe_insert("rotation_quaternion", frame=f)
        log("tail sway on", act.name, "frames", nfr)
    arm.animation_data.action = None
    clear_pose(arm)
    pack_textures()
    bpy.ops.export_scene.gltf(
        filepath=OUT, export_format='GLB', export_yup=True, use_selection=False,
        export_animations=True, export_animation_mode='ACTIONS',
        export_nla_strips=False, export_force_sampling=True, export_apply=False,
        export_materials='EXPORT', export_image_format='AUTO',
        export_texcoords=True, export_normals=True, export_tangents=True,
        export_skins=True)
    log("EXPORTED:", OUT, os.path.getsize(OUT), "bytes")
    # QA renders
    qa_scene()
    shots = {"Walk": [4, 11, 19], "Run": [3, 8, 13], "Idle": [5, 30],
             "Attack": [8, 14], "Death": [20, 34]}
    for nm, frames in shots.items():
        act = bpy.data.actions.get(nm)
        if act is None:
            continue
        arm.animation_data.action = act
        try:
            if getattr(act, "slots", None):
                arm.animation_data.action_slot = act.slots[0]
        except Exception:
            pass
        for f in frames:
            bpy.context.scene.frame_set(int(f))
            bpy.context.view_layer.update()
            qa_shot(os.path.join(WORKDIR, "clip_%s_f%02d_side.png" % (nm, f)), "side")
    arm.animation_data.action = None
    log("stage tail OK")

if __name__ == "__main__":
    status = "OK"
    try:
        os.makedirs(WORKDIR, exist_ok=True)
        if STAGE == "rig":
            stage_rig()
        elif STAGE == "tail":
            stage_tail()
        else:
            raise SystemExit("unknown stage " + STAGE)
    except Exception as e:
        import traceback
        log("FAILED:", e); log(traceback.format_exc()); status = "FAIL: " + str(e)
    flush(status)
