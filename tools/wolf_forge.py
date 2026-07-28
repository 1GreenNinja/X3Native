"""wolf_forge.py -- headless Blender quadruped RIG + ANIMATION forge for the
X3Native CyberWolf (assets/rigged_glb/alien_crawler_anim.glb, the "Jackal dog" /
F3 boss "Experiment #7").

The shipped asset has ONE bone ("Root") so all 6 Meshy clips slide the whole
body -- legs never move. This tool replaces that with a real 24-bone quadruped
skeleton fitted to the measured mesh landmarks, automatic-weight skinning, and
procedurally authored wolf-gait clips.

Stages (Blender launcher DETACHES on this box: each stage writes
<workdir>/wolf_<stage>.log and wolf_<stage>.done for the caller to poll):

    blender-launcher.exe --background --python tools/wolf_forge.py -- rig <src.glb> <workdir>
        Import src, drop the old 1-bone rig + Meshy Icosphere artifact, build
        the quadruped skeleton, skin with automatic weights (+ influence limit
        + weight QA report), save <workdir>/wolf_rig.blend, render bind pose +
        stress poses (legs bent / jaw open / body rolled) grounded on a floor.

    blender-launcher.exe --background --python tools/wolf_forge.py -- anim <workdir> <out.glb>
        Load wolf_rig.blend, author the 10 clips (Idle/Walk/Run/Attack/
        Hitreaction/Death/Struggle/Leap/Crouch/Snarl), export ONE multi-clip
        Y-up GLB (ACTIONS mode, textures packed), render grounded multi-frame
        QA sheets per clip.

Mesh anatomy (measured, Blender Z-up after glTF import; head at -Y, tail +Y,
feet on z=0): body barrel y[-0.38..0.75] z[0.83..1.55]. The big spiked arc over
the back is the TAIL: it roots at the shoulder hump (y~-0.45 z~1.65), sweeps
backward over the body (apex z~1.77), descends behind the pelvis and curls
down-forward to a hanging tip at (y~0.90, z~0.57) between the hind legs --
scorpion-style. Front feet plant ~y-0.80, elbows high ~z0.90 y-0.37; hind
hocks ~y0.93 z0.28, knees ~y0.45 z0.86.

Clean-room: public Blender Python API + glTF 2.0 spec only.
"""
import bpy, sys, os, math
from mathutils import Vector, Euler, Quaternion

ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(ARGV) < 3:
    raise SystemExit("usage: -- rig <src.glb> <workdir> | -- anim <workdir> <out.glb>")
STAGE = ARGV[0]

_log = []
LOG_PATH = DONE_PATH = None
def log(*a):
    s = "[wolf_forge] " + " ".join(str(x) for x in a)
    _log.append(s); print(s)
def flush(status):
    try:
        with open(LOG_PATH, "w", encoding="utf-8") as f: f.write("\n".join(_log))
        with open(DONE_PATH, "w", encoding="utf-8") as f: f.write(status)
    except Exception as e:
        print("[wolf_forge] log write failed:", e)

D = math.radians
TAU = math.tau

# ---------------------------------------------------------------------------
# Skeleton definition (Blender Z-up space; head -Y, tail +Y, feet z=0).
# Roll convention: local X == world X for every bone (sagittal swing = local X
# rotation with one sign convention fleet-wide across the rig).
# ---------------------------------------------------------------------------
def skeleton_def():
    B = [
        ("Root",   (0, 0.00, 0.00), (0, 0.00, 0.25), None),
        ("Pelvis", (0, 0.72, 1.28), (0, 0.45, 1.32), "Root"),
        ("Spine1", (0, 0.45, 1.32), (0, 0.08, 1.34), "Pelvis"),
        ("Chest",  (0, 0.08, 1.34), (0, -0.38, 1.32), "Spine1"),
        ("Neck",   (0, -0.52, 1.38), (0, -0.92, 1.44), "Chest"),
        ("Head",   (0, -0.92, 1.44), (0, -1.22, 1.36), "Neck"),
        ("Jaw",    (0, -1.06, 1.20), (0, -1.32, 1.06), "Head"),
        # The big spiked arc IS the tail: it roots at the SHOULDER hump (thick
        # end merges into the shoulder armor, y~-0.45 z~1.65), sweeps back over
        # the body (apex z~1.77 mid-back), descends behind the pelvis and curls
        # down-forward to a hanging tip at (y~0.90, z~0.57). Six bones so it
        # can properly lash.
        ("Tail1",  (0, -0.42, 1.66), (0, 0.05, 1.77), "Chest"),
        ("Tail2",  (0, 0.05, 1.77), (0, 0.55, 1.72), "Tail1"),
        ("Tail3",  (0, 0.55, 1.72), (0, 0.95, 1.45), "Tail2"),
        ("Tail4",  (0, 0.95, 1.45), (0, 1.20, 1.05), "Tail3"),
        ("Tail5",  (0, 1.20, 1.05), (0, 0.98, 0.72), "Tail4"),
        ("Tail6",  (0, 0.98, 0.72), (0, 0.86, 0.56), "Tail5"),
    ]
    for s, sd in ((1.0, ".L"), (-1.0, ".R")):
        B += [
            ("Shoulder" + sd,   (0.13*s, -0.42, 1.38), (0.17*s, -0.56, 1.16), "Chest"),
            ("FrontUpper" + sd, (0.17*s, -0.56, 1.16), (0.18*s, -0.37, 0.90), "Shoulder" + sd),
            ("FrontLower" + sd, (0.18*s, -0.37, 0.90), (0.185*s, -0.62, 0.34), "FrontUpper" + sd),
            ("FrontFoot" + sd,  (0.185*s, -0.62, 0.34), (0.19*s, -0.88, 0.05), "FrontLower" + sd),
            ("HindUpper" + sd,  (0.15*s, 0.68, 1.18), (0.16*s, 0.45, 0.86), "Pelvis"),
            ("HindLower" + sd,  (0.16*s, 0.45, 0.86), (0.21*s, 0.93, 0.28), "HindUpper" + sd),
            ("HindFoot" + sd,   (0.21*s, 0.93, 0.28), (0.26*s, 0.68, 0.05), "HindLower" + sd),
        ]
    return B

# ---------------------------------------------------------------------------
# Shared helpers
# ---------------------------------------------------------------------------
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

def select_only(objs, active=None):
    for o in bpy.data.objects: o.select_set(False)
    for o in objs: o.select_set(True)
    bpy.context.view_layer.objects.active = active or objs[-1]

# World-axis pose helpers: channel = q_rest^-1 * W * q_rest so a pose reads as
# an armature/world-space rotation about the bone head at rest (standard FK
# layering; parents' deviations compose on top).
def qrest(pb):
    return pb.bone.matrix_local.to_quaternion()

def set_wrot(arm, name, rx=0.0, ry=0.0, rz=0.0):
    pb = arm.pose.bones.get(name)
    if not pb: return None
    W = Euler((rx, ry, rz), 'XYZ').to_quaternion()
    qr = qrest(pb)
    pb.rotation_quaternion = qr.inverted() @ W @ qr
    return pb

def set_wloc(arm, name, x=0.0, y=0.0, z=0.0):
    pb = arm.pose.bones.get(name)
    if not pb: return None
    qr = qrest(pb)
    pb.location = qr.inverted() @ Vector((x, y, z))
    return pb

def clear_pose(arm):
    for pb in arm.pose.bones:
        pb.rotation_mode = 'QUATERNION'
        pb.rotation_quaternion = (1, 0, 0, 0)
        pb.location = (0, 0, 0)
        pb.scale = (1, 1, 1)

# Grounded QA scene: floor at z=0, sun, level-ish camera.
def qa_scene(center=Vector((0, 0, 0.9)), diag=3.0):
    scn = bpy.context.scene
    if "QAFloor" not in bpy.data.objects:
        bpy.ops.mesh.primitive_plane_add(size=diag * 10, location=(0, 0, 0))
        floor = bpy.context.object
        floor.name = "QAFloor"
        fm = bpy.data.materials.new("QAFloorMat"); fm.use_nodes = True
        fm.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (0.32, 0.33, 0.36, 1)
        floor.data.materials.append(fm)
        sun = bpy.data.objects.new("QASun", bpy.data.lights.new("QASun", 'SUN'))
        sun.data.energy = 4.0
        sun.rotation_euler = (D(50), 0, D(35))
        bpy.context.collection.objects.link(sun)
        w = bpy.data.worlds.new("QAW"); scn.world = w; w.use_nodes = True
        w.node_tree.nodes["Background"].inputs[0].default_value = (0.18, 0.20, 0.24, 1)
        cam = bpy.data.objects.new("QACam", bpy.data.cameras.new("QACam"))
        bpy.context.collection.objects.link(cam)
        scn.camera = cam
    try:
        scn.render.engine = 'BLENDER_EEVEE_NEXT'
    except Exception:
        scn.render.engine = 'BLENDER_EEVEE'
    scn.render.resolution_x = 1152; scn.render.resolution_y = 864

def qa_shot(path, view="side", center=Vector((0, 0, 0.9)), diag=3.2):
    scn = bpy.context.scene
    cam = bpy.data.objects["QACam"]
    if view == "side":   # near-level profile camera, slightly high so the
        cam.location = (center.x - diag * 2.4, center.y, 1.45)  # floor reads
        cam.rotation_euler = (D(82), 0, D(-90))
        cam.data.type = 'PERSP'; cam.data.lens = 48
    elif view == "front":
        cam.location = (center.x, center.y - diag * 2.6, 0.95)
        cam.rotation_euler = (D(90), 0, 0)
        cam.data.type = 'ORTHO'; cam.data.ortho_scale = diag * 1.1
    else:                # low 3/4 perspective, level-ish
        cam.location = (center.x - diag * 1.7, center.y - diag * 1.7, 1.25)
        cam.rotation_euler = (D(78), 0, D(-45))
        cam.data.type = 'PERSP'; cam.data.lens = 42
    scn.render.filepath = path
    bpy.ops.render.render(write_still=True)
    log("render:", os.path.basename(path))

# ---------------------------------------------------------------------------
# Procedural fallback skinning: weight = (r_bone / dist_to_segment)^3, top-4
# influences, normalized. Radii tuned per bone class to the measured anatomy so
# the wide spine/chest own the barrel + carapace arc and the thin leg/jaw/tail
# bones only grab their own tubes.
# ---------------------------------------------------------------------------
_RADII = {
    "Pelvis": 0.42, "Spine1": 0.48, "Chest": 0.48, "Neck": 0.30, "Head": 0.34,
    "Jaw": 0.10, "Tail1": 0.22, "Tail2": 0.20, "Tail3": 0.18,
    "Tail4": 0.15, "Tail5": 0.13, "Tail6": 0.12,
    "Shoulder": 0.20, "FrontUpper": 0.16, "FrontLower": 0.115, "FrontFoot": 0.13,
    "HindUpper": 0.20, "HindLower": 0.13, "HindFoot": 0.15,
}
def _bone_radius(name):
    base = name.split(".")[0]
    return _RADII.get(base, 0.15)

def fallback_weights(mesh, arm):
    segs = []
    for b in arm.data.bones:
        if not b.use_deform: continue
        h = arm.matrix_world @ b.head_local
        t = arm.matrix_world @ b.tail_local
        segs.append((b.name, h, t - h, max(1e-8, (t - h).length_squared),
                     _bone_radius(b.name)))
    mesh.vertex_groups.clear()
    groups = {nm: mesh.vertex_groups.new(name=nm) for nm, _, _, _, _ in segs}
    mw = mesh.matrix_world
    for v in mesh.data.vertices:
        co = mw @ v.co
        scored = []
        for nm, h, d, dl2, r in segs:
            t = max(0.0, min(1.0, (co - h).dot(d) / dl2))
            dist = (co - (h + d * t)).length
            scored.append((-((r / max(dist, 0.02)) ** 3), nm))
        scored.sort()
        top = scored[:4]
        wmax = -top[0][0]
        top = [(w, nm) for w, nm in top if -w > 0.06 * wmax]
        tot = sum(-w for w, _ in top)
        for w, nm in top:
            groups[nm].add([v.index], -w / tot, 'REPLACE')
    log("fallback skinning done:", len(segs), "deform bones")

# Chain-projection redistribution: capsule falloff alone lets overlapping
# armor shells along a limb disagree with the inner tube (shredding at deep
# folds). For each leg/tail chain, take each vertex's TOTAL chain weight and
# re-split it purely by arc-length projection onto the chain polyline with a
# narrow linear blend window at the joints -- every shell that hugs the limb
# then deforms identically.
def chain_redistribute(mesh, arm):
    chains = []
    for sd in (".L", ".R"):
        chains.append(["FrontUpper" + sd, "FrontLower" + sd, "FrontFoot" + sd])
        chains.append(["HindUpper" + sd, "HindLower" + sd, "HindFoot" + sd])
    chains.append(["Tail1", "Tail2", "Tail3", "Tail4", "Tail5", "Tail6"])
    groups = {g.name: g for g in mesh.vertex_groups}
    name2idx = {g.name: g.index for g in mesh.vertex_groups}
    mw = mesh.matrix_world
    geom = []   # per chain: (pts, radii, group-index set)
    for chain in chains:
        pts = [arm.matrix_world @ arm.data.bones[b].head_local for b in chain]
        pts.append(arm.matrix_world @ arm.data.bones[chain[-1]].tail_local)
        geom.append((pts, [_bone_radius(b) for b in chain],
                     set(name2idx[b] for b in chain if b in name2idx)))
    all_chain_idx = set()
    for _, _, idxs in geom: all_chain_idx |= idxs
    idx2vg = {g.index: g for g in mesh.vertex_groups}
    moved = 0
    for v in mesh.data.vertices:
        cap_total = sum(ge.weight for ge in v.groups if ge.group in all_chain_idx)
        # snapshot body entries BEFORE any writes (writes may grow v.groups)
        body_entries = [(ge.group, ge.weight) for ge in v.groups
                        if ge.group not in all_chain_idx]
        co = mw @ v.co
        # Per-chain: min distance, nearest-segment radius, per-segment soft
        # weights (inv-dist^4 -- winner-take-all projection is discontinuous at
        # the outer corner of a folded joint, exactly where hock points/fins
        # sit, and shreds them).
        per_chain = []
        for pts, radii, _ in geom:
            raw = []; dmin = 1e9; rnear = radii[0]
            for k in range(len(radii)):
                a, b = pts[k], pts[k + 1]
                d = b - a
                t = max(0.0, min(1.0, (co - a).dot(d) / max(1e-8, d.length_squared)))
                dist = (co - (a + d * t)).length
                if dist < dmin: dmin = dist; rnear = radii[k]
                raw.append((radii[k] / max(dist, 0.02)) ** 4)
            per_chain.append((dmin, rnear, raw))
        # GEOMETRIC chain ownership: capsule+smoothing history differs between
        # interleaved shells (inner tube vs armor plate), leaving salt-and-
        # pepper body-owned verts INSIDE a limb that shear when it folds. A
        # vert well inside a chain's tube belongs to that chain regardless of
        # history; ownership fades smoothly at the hip/shoulder creases.
        tgeo = []
        for dmin, rnear, _ in per_chain:
            closeness = rnear * 1.35 / max(dmin, 0.02)
            tgeo.append(max(0.0, min(1.0, (closeness - 0.85) / 0.45)))
        if cap_total < 1e-4 and max(tgeo) <= 0.0:
            continue
        # inter-chain competition (tail tip hangs between the hind legs):
        # sharp inverse-distance share so exactly one chain wins locally.
        share = [(1.0 / max(d, 0.02)) ** 6 for d, _, _ in per_chain]
        stot = sum(share)
        masses = []
        for ci in range(len(chains)):
            s = share[ci] / stot
            masses.append(max(cap_total * s, tgeo[ci] * s))
        chain_total = min(1.0, sum(masses))
        scale = chain_total / max(1e-8, sum(masses))
        for ci, (chain, (dmin, rnear, raw)) in enumerate(zip(chains, per_chain)):
            cm = masses[ci] * scale
            rtot = sum(raw)
            for bname, r in zip(chain, raw):
                groups[bname].add([v.index], cm * r / rtot, 'REPLACE')
        # rescale the vert's body (non-chain) weights to the remainder
        body_now = sum(w for _, w in body_entries)
        remainder = 1.0 - chain_total
        if body_now > 1e-8:
            f = remainder / body_now
            for gidx, w in body_entries:
                vg = idx2vg.get(gidx)
                if vg is not None:
                    vg.add([v.index], w * f, 'REPLACE')
        moved += 1
    log("chain_redistribute: resolved", moved, "limb/tail verts across",
        len(chains), "chains")

# ---------------------------------------------------------------------------
# Stage: rig
# ---------------------------------------------------------------------------
def stage_rig(src, workdir):
    reset_factory()
    bpy.ops.import_scene.gltf(filepath=src)
    # Record old clip durations (for authoring-time parity), then drop old rig.
    fps = bpy.context.scene.render.fps
    for a in list(bpy.data.actions):
        log("old action:", a.name, "frames", tuple(round(v, 1) for v in a.frame_range),
            "dur %.2fs" % ((a.frame_range[1] - a.frame_range[0]) / fps))
        bpy.data.actions.remove(a)
    mesh = bpy.data.objects.get("Character")
    if mesh is None:
        mesh = next(o for o in bpy.data.objects
                    if o.type == 'MESH' and len(o.data.vertices) > 1000)
        mesh.name = "Character"
    ico = bpy.data.objects.get("Icosphere")
    if ico is not None:
        log("removing Meshy Icosphere artifact (", len(ico.data.vertices), "verts )")
        bpy.data.objects.remove(ico, do_unlink=True)
    for o in [o for o in bpy.data.objects if o.type == 'ARMATURE']:
        log("removing old armature:", o.name, "bones:", [b.name for b in o.data.bones])
        bpy.data.objects.remove(o, do_unlink=True)
    mesh.parent = None
    for mod in list(mesh.modifiers):
        mesh.modifiers.remove(mod)
    mesh.vertex_groups.clear()
    pack_textures()

    # Build the armature.
    arm_data = bpy.data.armatures.new("Arm")
    arm = bpy.data.objects.new("Arm", arm_data)
    bpy.context.collection.objects.link(arm)
    select_only([arm], arm)
    bpy.ops.object.mode_set(mode='EDIT')
    ebs = {}
    for name, head, tail, parent in skeleton_def():
        eb = arm_data.edit_bones.new(name)
        eb.head = Vector(head); eb.tail = Vector(tail)
        eb.use_connect = False
        if parent: eb.parent = ebs[parent]
        d = (Vector(tail) - Vector(head)).normalized()
        zt = Vector((1, 0, 0)).cross(d)
        if zt.length > 1e-4:
            eb.align_roll(zt.normalized())
        ebs[name] = eb
    bpy.ops.object.mode_set(mode='OBJECT')
    # Root is a ground-reference/COM bone OUTSIDE the mesh -- non-deform (a
    # bone fully outside the mesh is a classic bone-heat failure trigger, and
    # nothing should skin to it anyway).
    arm_data.bones["Root"].use_deform = False
    log("built skeleton:", len(arm_data.bones), "bones")

    # Automatic weights (bone heat). On this 68k-vert multi-shell armored mesh
    # the heat solve can fail silently in background mode -- detect empty
    # weights and fall back to procedural capsule-distance skinning.
    select_only([mesh, arm], arm)
    try:
        bpy.ops.object.parent_set(type='ARMATURE_AUTO')
    except Exception as e:
        log("parent_set ARMATURE_AUTO raised:", e)
        select_only([mesh, arm], arm)
        bpy.ops.object.parent_set(type='ARMATURE_NAME')
    weighted = 0
    for v in mesh.data.vertices:
        if any(ge.weight > 1e-4 for ge in v.groups):
            weighted += 1
    log("heat result: %d/%d verts weighted" % (weighted, len(mesh.data.vertices)))
    if weighted < len(mesh.data.vertices) * 0.5:
        log("bone heat FAILED -> procedural capsule-distance fallback")
        fallback_weights(mesh, arm)
    # Smooth FIRST (weight-paint op; best effort in background)...
    try:
        select_only([mesh], mesh)
        bpy.ops.object.mode_set(mode='WEIGHT_PAINT')
        bpy.ops.object.vertex_group_smooth(group_select_mode='ALL', factor=0.5,
                                           repeat=3, expand=0.0)
        bpy.ops.object.mode_set(mode='OBJECT')
        log("weight smooth: OK")
    except Exception as e:
        log("weight smooth skipped:", e)
        try: bpy.ops.object.mode_set(mode='OBJECT')
        except Exception: pass
    # ...then the chain solve gets the LAST WORD on limb/tail weights: the
    # topological smooth works per shell, so overlapping armor shells drift
    # apart again if it runs after the redistribution (=> shredding at folds).
    chain_redistribute(mesh, arm)
    # Influence hygiene: max 4 joints/vert (glTF standard), clean crumbs, renorm.
    select_only([mesh], mesh)
    bpy.ops.object.vertex_group_limit_total(group_select_mode='ALL', limit=4)
    bpy.ops.object.vertex_group_clean(group_select_mode='ALL', limit=0.015)
    bpy.ops.object.vertex_group_normalize_all(group_select_mode='ALL', lock_active=False)

    # ---- Weight QA report ----------------------------------------------
    gi = {g.index: g.name for g in mesh.vertex_groups}
    per_bone = {g.name: 0 for g in mesh.vertex_groups}
    unweighted = 0
    crosstalk_belly = 0   # belly core verts grabbed by feet/lower legs
    crosstalk_skull = 0   # skull verts grabbed by Jaw
    crosstalk_tailleg = 0 # hind-foot region verts grabbed by tail
    mw = mesh.matrix_world
    for v in mesh.data.vertices:
        tot = 0.0; wmap = {}
        for ge in v.groups:
            nm = gi.get(ge.group)
            if nm is None: continue
            tot += ge.weight; wmap[nm] = wmap.get(nm, 0.0) + ge.weight
            if ge.weight > 0.25: per_bone[nm] += 1
        if tot < 1e-6: unweighted += 1
        co = mw @ v.co
        low = sum(w for nm, w in wmap.items()
                  if ("Foot" in nm) or ("Lower" in nm))
        if abs(co.x) < 0.13 and -0.15 < co.y < 0.30 and 0.80 < co.z < 1.10 and low > 0.35:
            crosstalk_belly += 1
        if co.y < -1.0 and co.z > 1.34 and wmap.get("Jaw", 0.0) > 0.5:
            crosstalk_skull += 1
        tw = sum(w for nm, w in wmap.items() if nm.startswith("Tail"))
        hw = sum(w for nm, w in wmap.items() if nm.startswith("Hind"))
        # hind-leg tube verts grabbed by tail, or hanging tail-tip verts
        # grabbed by hind legs (the tip hangs between the hind legs)
        if abs(co.x) > 0.13 and 0.40 < co.y < 1.10 and co.z < 0.50 and tw > 0.3:
            crosstalk_tailleg += 1
        if abs(co.x) < 0.10 and co.y > 0.80 and 0.50 < co.z < 0.78 and hw > 0.3:
            crosstalk_tailleg += 1
    log("weight QA: unweighted verts =", unweighted)
    log("weight QA: crosstalk belly/skull-jaw/tail-leg =",
        crosstalk_belly, crosstalk_skull, crosstalk_tailleg)
    for nm in sorted(per_bone):
        log("  bone %-14s strong-verts(w>0.25) = %d" % (nm, per_bone[nm]))

    # Save the rig BEFORE stress posing.
    blend = os.path.join(workdir, "wolf_rig.blend")
    bpy.ops.wm.save_as_mainfile(filepath=blend)
    log("saved:", blend)

    # ---- Stress-pose renders (skin QA) ---------------------------------
    qa_scene()
    center = Vector((0, 0, 0.95))
    def shots(tag):
        qa_shot(os.path.join(workdir, "rig_%s_side.png" % tag), "side", center)
        qa_shot(os.path.join(workdir, "rig_%s_34.png" % tag), "persp", center)
    clear_pose(arm)
    shots("bind")
    # Pose 1: legs -- big asymmetric swings + bent joints.
    clear_pose(arm)
    set_wrot(arm, "FrontUpper.L", rx=D(-35))
    set_wrot(arm, "FrontLower.L", rx=D(30))
    set_wrot(arm, "FrontUpper.R", rx=D(25))
    set_wrot(arm, "HindUpper.L", rx=D(30))
    set_wrot(arm, "HindLower.L", rx=D(-35))
    set_wrot(arm, "HindUpper.R", rx=D(-25))
    shots("legs")
    # Pose 2: head/jaw/tail -- neck up, head yaw, jaw open, tail curled.
    clear_pose(arm)
    set_wrot(arm, "Neck", rx=D(-20))
    set_wrot(arm, "Head", rz=D(30))
    set_wrot(arm, "Jaw", rx=D(30))
    set_wrot(arm, "Tail2", rz=D(12))
    set_wrot(arm, "Tail3", rz=D(22))
    set_wrot(arm, "Tail4", rz=D(28))
    set_wrot(arm, "Tail5", rz=D(28))
    set_wrot(arm, "Tail6", rz=D(24))
    shots("headtail")
    # Pose 3: death-roll precheck -- pelvis roll + drop, spine follows.
    clear_pose(arm)
    set_wrot(arm, "Pelvis", ry=D(40))
    set_wloc(arm, "Pelvis", z=-0.30)
    set_wrot(arm, "Spine1", ry=D(15))
    set_wrot(arm, "Chest", ry=D(10))
    shots("roll")
    clear_pose(arm)
    log("stage rig OK")

# ---------------------------------------------------------------------------
# Stage: anim -- author the 10 clips + export + grounded QA renders.
#
# World-rotation sign conventions (verified via the stage-rig stress renders):
#   legs:  -rx = protract (swing forward), +rx = retract (swing back)
#   FrontLower +rx = fold foot up;  HindLower -rx = fold hock up
#   Neck/Head: +rx = muzzle down, -rx = muzzle up;  Jaw +rx = OPEN
#   Tail: +rx = raise/curl harder, -rx = flatten;  rz = lateral lash
#   Pelvis loc: -y = lunge toward the head, +z = up. Root loc z = whole body.
# ---------------------------------------------------------------------------
FPS = 24.0

class PW:
    """Pose writer: composes a WORLD-euler pose per bone per frame and keys it."""
    def __init__(self, arm, name):
        self.arm = arm
        if not arm.animation_data:
            arm.animation_data_create()
        act = bpy.data.actions.new(name)
        act.use_fake_user = True
        arm.animation_data.action = act
        try:  # Blender 5.x slotted actions forward-compat
            if getattr(act, "slots", None) is not None and len(act.slots) == 0:
                act.slots.new(id_type='OBJECT', name=arm.name)
                arm.animation_data.action_slot = act.slots[0]
        except Exception:
            pass
        self.act = act
        clear_pose(arm)

    def key(self, f, bone, rx=0.0, ry=0.0, rz=0.0, loc=None, scale=None):
        pb = set_wrot(self.arm, bone, rx, ry, rz)
        if pb is None:
            log("WARN missing bone", bone); return
        pb.keyframe_insert("rotation_quaternion", frame=f)
        if loc is not None:
            set_wloc(self.arm, bone, *loc)
            pb.keyframe_insert("location", frame=f)
        if scale is not None:
            pb.scale = (scale, scale, scale)
            pb.keyframe_insert("scale", frame=f)

LEGS = ("FrontUpper", "FrontLower", "FrontFoot", "HindUpper", "HindLower", "HindFoot")

def _leg_gait(pw, f, side, kind, phase, A, B, C):
    """One leg at cycle phase [0..1). Stance p<0.5 sweeps back, swing p>=0.5
    returns forward with the joint folded so the foot clears the floor."""
    s = math.sin(TAU * phase)          # + first half (stance), - second (swing)
    lift = max(0.0, -s)                # 0 in stance, up to 1 mid-swing
    up = -A * math.cos(TAU * phase)    # -A touchdown ... +A liftoff ... back
    sd = "." + side
    if kind == "F":
        pw.key(f, "FrontUpper" + sd, rx=up)
        pw.key(f, "FrontLower" + sd, rx=B * lift)
        pw.key(f, "FrontFoot" + sd, rx=-C * lift)
    else:
        pw.key(f, "HindUpper" + sd, rx=up)
        pw.key(f, "HindLower" + sd, rx=-B * lift)
        pw.key(f, "HindFoot" + sd, rx=C * 0.6 * lift)

def clip_walk(arm):
    pw = PW(arm, "Walk")
    N = 28
    # classic 4-beat lateral walk: LF, RH, RF, LH each a quarter-cycle apart
    ph = {("F", "L"): 0.00, ("H", "R"): 0.25, ("F", "R"): 0.50, ("H", "L"): 0.75}
    for i in range(N + 1):
        f = i + 1
        p = i / float(N)
        for (kind, side), off in ph.items():
            _leg_gait(pw, f, side, kind, (p + off) % 1.0, D(17), D(26), D(12))
        pw.key(f, "Pelvis", ry=D(3) * math.sin(TAU * p),
               loc=(0, 0, 0.025 * math.sin(2 * TAU * p)))
        pw.key(f, "Chest", ry=D(-2.5) * math.sin(TAU * p))
        pw.key(f, "Neck", rx=D(2.5) * math.sin(2 * TAU * p + 0.8))
        pw.key(f, "Head", rx=D(-1.5) * math.sin(2 * TAU * p + 0.8))
        for k, tb in enumerate(("Tail2", "Tail3", "Tail4", "Tail5", "Tail6")):
            pw.key(f, tb, rz=D(7) * math.sin(TAU * p - 0.55 * k),
                   rx=D(2) * math.sin(2 * TAU * p - 0.5 * k))
    return N

def clip_run(arm):
    pw = PW(arm, "Run")
    N = 16
    # bounding gallop: front pair nearly together, hind pair half a cycle out
    ph = {("F", "L"): 0.00, ("F", "R"): 0.12, ("H", "R"): 0.50, ("H", "L"): 0.62}
    for i in range(N + 1):
        f = i + 1
        p = i / float(N)
        for (kind, side), off in ph.items():
            A = D(34) if kind == "F" else D(40)
            _leg_gait(pw, f, side, kind, (p + off) % 1.0, A, D(44), D(18))
        # spine gallop flex: chest extends when the hinds drive (p~0.55),
        # crunches when the fronts land (p~0.05)
        flex = math.sin(TAU * (p + 0.30))
        pw.key(f, "Spine1", rx=D(7) * flex)
        pw.key(f, "Chest", rx=D(10) * flex)
        pw.key(f, "Pelvis", rx=D(-6) * flex,
               loc=(0, 0, 0.07 * math.sin(TAU * (p + 0.15))))
        pw.key(f, "Neck", rx=D(-6) * flex - D(4))
        pw.key(f, "Head", rx=D(3) * flex + D(2))
        pw.key(f, "Jaw", rx=D(9))          # maw open on the charge
        for k, tb in enumerate(("Tail2", "Tail3", "Tail4", "Tail5", "Tail6")):
            pw.key(f, tb, rx=D(8) * math.sin(TAU * (p + 0.3) - 0.5 * k),
                   rz=D(3) * math.sin(TAU * p - 0.5 * k))
    return N

def clip_idle(arm):
    pw = PW(arm, "Idle")
    N = 60
    for i in range(N + 1):
        f = i + 1
        p = i / float(N)
        br = math.sin(2 * TAU * p)               # 2 breaths per loop
        pw.key(f, "Chest", rx=D(1.3) * br, scale=1.0 + 0.012 * br)
        pw.key(f, "Spine1", rx=D(0.8) * br)
        # head scan: one slow look-around with a lagged head
        pw.key(f, "Neck", rz=D(9) * math.sin(TAU * p),
               rx=D(1.5) * math.sin(2 * TAU * p + 1.2))
        pw.key(f, "Head", rz=D(7) * math.sin(TAU * p - 0.6))
        pw.key(f, "Pelvis", ry=D(1.6) * math.sin(TAU * p + 0.5),
               loc=(0.008 * math.sin(TAU * p), 0, 0))
        for k, tb in enumerate(("Tail3", "Tail4", "Tail5", "Tail6")):
            pw.key(f, tb, rz=D(9) * math.sin(TAU * p - 0.7 * k),
                   rx=D(2.5) * math.sin(TAU * p - 0.7 * k + 0.8))
    return N

def clip_crouch(arm):
    pw = PW(arm, "Crouch")
    N = 48
    for i in range(N + 1):
        f = i + 1
        p = i / float(N)
        sway = math.sin(TAU * p)
        pw.key(f, "Pelvis", ry=D(2.2) * sway, loc=(0, 0, -0.23 + 0.012 * math.sin(2 * TAU * p)))
        # legs folded to keep the feet near the floor under the dropped body
        pw.key(f, "FrontUpper.L", rx=D(11) + D(1.5) * sway)
        pw.key(f, "FrontUpper.R", rx=D(11) - D(1.5) * sway)
        pw.key(f, "FrontLower.L", rx=D(21)); pw.key(f, "FrontLower.R", rx=D(21))
        pw.key(f, "FrontFoot.L", rx=D(-11)); pw.key(f, "FrontFoot.R", rx=D(-11))
        pw.key(f, "HindUpper.L", rx=D(-13)); pw.key(f, "HindUpper.R", rx=D(-13))
        pw.key(f, "HindLower.L", rx=D(-22)); pw.key(f, "HindLower.R", rx=D(-22))
        pw.key(f, "HindFoot.L", rx=D(20)); pw.key(f, "HindFoot.R", rx=D(20))
        pw.key(f, "Chest", rx=D(2))
        pw.key(f, "Neck", rx=D(11))          # head dropped, thrust forward
        pw.key(f, "Head", rx=D(-7))          # eyes level, hunting glare
        pw.key(f, "Jaw", rx=D(6))
        # tail LOW and almost still (stalking)
        for k, tb in enumerate(("Tail2", "Tail3", "Tail4", "Tail5", "Tail6")):
            pw.key(f, tb, rx=D(-6), rz=D(1.5) * math.sin(TAU * p - 0.5 * k))
    return N

def clip_attack(arm):
    pw = PW(arm, "Attack")
    def pose(f, neck, head, jaw, chest, ply, plz, fu, fl, hu):
        pw.key(f, "Neck", rx=neck); pw.key(f, "Head", rx=head)
        pw.key(f, "Jaw", rx=jaw); pw.key(f, "Chest", rx=chest)
        pw.key(f, "Pelvis", loc=(0, ply, plz))
        for sd in (".L", ".R"):
            pw.key(f, "FrontUpper" + sd, rx=fu)
            pw.key(f, "FrontLower" + sd, rx=fl)
            pw.key(f, "HindUpper" + sd, rx=hu)
    pose(1, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    pose(6, D(-20), D(-9), D(20), D(-6), 0.03, 0.02, D(9), D(4), D(-7))   # coil
    pose(9, D(20), D(10), D(48), D(8), -0.19, -0.06, D(-19), D(11), D(6)) # strike+gape
    pose(11, D(24), D(12), D(6), D(8), -0.20, -0.07, D(-21), D(12), D(7)) # SNAP shut
    pose(12, D(24), D(12), D(6), D(8), -0.20, -0.07, D(-21), D(12), D(7)) # hold bite
    pose(16, D(10), D(5), D(14), D(3), -0.07, -0.03, D(-8), D(5), D(2))   # recoil
    pose(22, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    for f in (1, 6, 9, 11, 12, 16, 22):  # tail punctuates the strike
        t = {1: 0, 6: D(8), 9: D(-14), 11: D(-16), 12: D(-16), 16: D(-5), 22: 0}[f]
        for tb in ("Tail3", "Tail4", "Tail5"):
            pw.key(f, tb, rx=t)
    return 22

def clip_hitreaction(arm):
    pw = PW(arm, "Hitreaction")
    def pose(f, neck, head, chest, ply, ful):
        pw.key(f, "Neck", rx=neck); pw.key(f, "Head", rx=head)
        pw.key(f, "Chest", rx=chest); pw.key(f, "Pelvis", loc=(0, ply, 0))
        pw.key(f, "FrontUpper.L", rx=ful)
    pose(1, 0, 0, 0, 0, 0)
    pose(3, D(-24), D(-12), D(-7), 0.06, D(-12))   # sharp recoil, paw jerks
    pose(6, D(6), D(3), D(2), -0.015, D(3))        # overshoot
    pose(10, 0, 0, 0, 0, 0)
    return 10

def clip_death(arm):
    # Distributed collapse: the roll spreads over Pelvis+Spine1+Chest (single-
    # joint 60-degree rolls shred the mesh and bury the head), body sinks only
    # to barrel-rest height, neck stays nearly relaxed so the head settles with
    # the body instead of twisting against it.
    pw = PW(arm, "Death")
    def key_all(f, plz=0.0, ryp=0.0, rys=0.0, ryc=0.0, nrx=0.0, nry=0.0,
                jaw=0.0, fu=0.0, fl=0.0, hu=0.0, hl=0.0, tail=0.0, srx=0.0):
        pw.key(f, "Pelvis", ry=ryp, loc=(0, 0, plz))
        pw.key(f, "Spine1", ry=rys, rx=srx); pw.key(f, "Chest", ry=ryc, rx=srx)
        pw.key(f, "Neck", rx=nrx, ry=nry); pw.key(f, "Jaw", rx=jaw)
        for sd, m in ((".L", 1.0), (".R", 0.8)):
            pw.key(f, "FrontUpper" + sd, rx=fu * m)
            pw.key(f, "FrontLower" + sd, rx=fl * m)
            pw.key(f, "HindUpper" + sd, rx=hu * m)
            pw.key(f, "HindLower" + sd, rx=hl * m)
        for tb in ("Tail2", "Tail3", "Tail4", "Tail5"):
            pw.key(f, tb, rx=tail)
    key_all(1)
    key_all(6, plz=-0.08, nrx=D(8), jaw=D(12),
            fu=D(6), fl=D(10), hu=D(-6), hl=D(-10))                    # stagger
    key_all(12, plz=-0.26, ryp=D(10), nrx=D(12), jaw=D(16),
            fu=D(8), fl=D(28), hu=D(-10), hl=D(-28))                   # buckle
    key_all(20, plz=-0.50, ryp=D(26), rys=D(10), ryc=D(8), nrx=D(8),
            nry=D(12), jaw=D(18), fu=D(28), fl=D(46), hu=D(-16),
            hl=D(-26), tail=D(-14), srx=D(6))                          # fall
    for f in (26, 34):                                                 # settle
        key_all(f, plz=-0.55, ryp=D(30), rys=D(12), ryc=D(9), nrx=D(6),
                nry=D(16), jaw=D(15), fu=D(32), fl=D(50), hu=D(-18),
                hl=D(-28), tail=D(-18), srx=D(8))
    return 34

def clip_struggle(arm):
    pw = PW(arm, "Struggle")
    N = 36
    offs = {("FrontUpper", "L"): 0.0, ("FrontUpper", "R"): 0.5,
            ("HindUpper", "L"): 0.3, ("HindUpper", "R"): 0.8}
    for i in range(N + 1):
        f = i + 1
        p = i / float(N)
        pw.key(f, "Pelvis", rz=D(9) * math.sin(2 * TAU * p),
               ry=D(6) * math.sin(2 * TAU * p + 1.2))
        pw.key(f, "Chest", rz=D(-6) * math.sin(2 * TAU * p))
        for (bn, sd), off in offs.items():
            s = math.sin(2 * TAU * (p + off))
            pw.key(f, bn + "." + sd, rx=D(20) * s)
            pw.key(f, bn.replace("Upper", "Lower") + "." + sd,
                   rx=D(-24) * s if bn.startswith("Hind") else D(24) * s)
        pw.key(f, "Neck", rx=D(10) * math.sin(3 * TAU * p))
        pw.key(f, "Head", rz=D(16) * math.sin(3 * TAU * p + 0.7))
        pw.key(f, "Jaw", rx=D(15) + D(14) * math.sin(2 * TAU * p + 0.3))
        for k, tb in enumerate(("Tail2", "Tail3", "Tail4", "Tail5", "Tail6")):
            pw.key(f, tb, rz=D(16) * math.sin(2 * TAU * p - 0.6 * k))
    return N

def clip_leap(arm):
    pw = PW(arm, "Leap")
    def pose(f, rootz, plz, prx, fu, fl, hu, hl, neck, jaw, tail):
        pw.key(f, "Root", loc=(0, 0, rootz))
        pw.key(f, "Pelvis", rx=prx, loc=(0, 0, plz))
        for sd in (".L", ".R"):
            pw.key(f, "FrontUpper" + sd, rx=fu); pw.key(f, "FrontLower" + sd, rx=fl)
            pw.key(f, "HindUpper" + sd, rx=hu); pw.key(f, "HindLower" + sd, rx=hl)
        pw.key(f, "Neck", rx=neck); pw.key(f, "Jaw", rx=jaw)
        for tb in ("Tail3", "Tail4", "Tail5"):
            pw.key(f, tb, rx=tail)
    #      f  root   plz   prx     fu      fl      hu      hl     neck    jaw   tail
    pose(  1, 0.00,  0.00, 0,      0,      0,      0,      0,     0,      0,    0)
    pose(  4, 0.00, -0.22, D(4),  D(10),  D(16), D(-18), D(-30), D(8),  D(6),  D(10))   # coil
    pose(  7, 0.22,  0.00, D(-12), D(-26), D(22), D(28),  D(18), D(-12), D(18), D(-12)) # launch
    pose( 10, 0.42,  0.00, D(-6),  D(-16), D(42), D(34),  D(-10), D(-6), D(26), D(-8))  # airborne tuck
    pose( 14, 0.16,  0.00, D(10),  D(-12), D(2),  D(-14), D(-24), D(6),  D(12), D(4))   # descend, reach
    pose( 16, 0.00, -0.16, D(4),   D(4),   D(18), D(-8),  D(-16), D(9),  D(4),  D(6))   # land absorb
    pose( 20, 0.00,  0.00, 0,      0,      0,      0,      0,     0,     0,     0)      # settle
    return 20

def clip_snarl(arm):
    pw = PW(arm, "Snarl")
    N = 34
    # sharp head jerks + jaw snap pattern as sparse keys over a coiled base
    jawk = {1: 8, 3: 38, 5: 38, 7: 33, 12: 40, 14: 10, 16: 36, 22: 38, 26: 34, 30: 12, 34: 8}
    yawk = {1: 0, 4: 0, 6: -13, 10: -11, 12: 2, 16: 0, 18: 13, 22: 11, 26: 0, 30: -5, 34: 0}
    frames = sorted(set(list(jawk) + list(yawk)))
    jf = sorted(jawk); yf = sorted(yawk)
    def lerp_at(keys, ks, f):
        if f <= ks[0]: return keys[ks[0]]
        for a, b in zip(ks, ks[1:]):
            if a <= f <= b:
                t = (f - a) / float(b - a)
                return keys[a] * (1 - t) + keys[b] * t
        return keys[ks[-1]]
    for i in range(N + 1):
        f = i + 1
        p = i / float(N)
        # coiled-low aggressive base
        pw.key(f, "Pelvis", loc=(0, 0, -0.06 + 0.006 * math.sin(3 * TAU * p)))
        pw.key(f, "Chest", rx=D(3))
        for sd in (".L", ".R"):
            pw.key(f, "FrontUpper" + sd, rx=D(7))
            pw.key(f, "FrontLower" + sd, rx=D(10))
            pw.key(f, "HindUpper" + sd, rx=D(-6))
        pw.key(f, "Neck", rx=D(18), rz=D(0.5) * lerp_at(yawk, yf, f))
        pw.key(f, "Head", rx=D(-6) + D(1.8) * math.sin(6 * TAU * p),
               rz=D(1.3) * lerp_at(yawk, yf, f))
        pw.key(f, "Jaw", rx=D(1.12) * lerp_at(jawk, jf, f))
        # tail stiff and high with fast tip lashes
        pw.key(f, "Tail2", rx=D(8)); pw.key(f, "Tail3", rx=D(13))
        pw.key(f, "Tail4", rx=D(16), rz=D(7) * math.sin(3 * TAU * p))
        pw.key(f, "Tail5", rz=D(22) * math.sin(3 * TAU * p - 0.7))
        pw.key(f, "Tail6", rz=D(20) * math.sin(3 * TAU * p - 1.2))
    return N

def stage_anim(workdir, out):
    reset_factory()
    blend = os.path.join(workdir, "wolf_rig.blend")
    bpy.ops.wm.open_mainfile(filepath=blend)
    arm = bpy.data.objects["Arm"]
    mesh = bpy.data.objects["Character"]
    for a in list(bpy.data.actions):
        bpy.data.actions.remove(a)
    bpy.context.scene.render.fps = int(FPS)

    builders = [clip_idle, clip_walk, clip_run, clip_attack, clip_hitreaction,
                clip_death, clip_struggle, clip_leap, clip_crouch, clip_snarl]
    lengths = {}
    for b in builders:
        n = b(arm)
        nm = arm.animation_data.action.name
        lengths[nm] = n
        log("clip %-12s %2d frames  %.2fs" % (nm, n, n / FPS))
    arm.animation_data.action = None
    clear_pose(arm)

    pack_textures()
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    bpy.ops.export_scene.gltf(
        filepath=out, export_format='GLB', export_yup=True, use_selection=False,
        export_animations=True, export_animation_mode='ACTIONS',
        export_nla_strips=False, export_force_sampling=True, export_apply=False,
        export_materials='EXPORT', export_image_format='AUTO',
        export_texcoords=True, export_normals=True, export_skins=True)
    log("EXPORTED:", out, os.path.getsize(out), "bytes")

    # ---- grounded multi-frame QA renders --------------------------------
    qa_scene()
    shots = {
        "Walk": [1, 8, 15, 22, 28], "Run": [1, 5, 9, 13],
        "Idle": [1, 30], "Crouch": [1, 24],
        "Attack": [6, 9, 11, 15], "Hitreaction": [3, 6],
        "Death": [12, 20, 34], "Struggle": [10, 28],
        "Leap": [4, 7, 10, 14, 16], "Snarl": [3, 6, 14, 18],
    }
    for nm, frames in shots.items():
        act = bpy.data.actions.get(nm)
        if act is None:
            log("QA WARN: no action", nm); continue
        arm.animation_data.action = act
        for f in frames:
            bpy.context.scene.frame_set(f)
            c = Vector((0, 0, 1.35)) if nm == "Leap" else Vector((0, 0, 0.9))
            qa_shot(os.path.join(workdir, "clip_%s_f%02d_side.png" % (nm, f)), "side", c)
            if nm in ("Run", "Attack", "Leap", "Snarl", "Death"):
                qa_shot(os.path.join(workdir, "clip_%s_f%02d_34.png" % (nm, f)), "persp", c)
    arm.animation_data.action = None
    log("stage anim OK")

if __name__ == "__main__":
    if STAGE == "rig":
        src, workdir = ARGV[1], ARGV[2]
        os.makedirs(workdir, exist_ok=True)
        LOG_PATH = os.path.join(workdir, "wolf_rig.log")
        DONE_PATH = os.path.join(workdir, "wolf_rig.done")
        status = "OK"
        try:
            stage_rig(src, workdir)
        except Exception as e:
            import traceback
            log("FAILED:", e); log(traceback.format_exc()); status = "FAIL: " + str(e)
        flush(status)
    elif STAGE == "anim":
        workdir, out = ARGV[1], ARGV[2]
        LOG_PATH = os.path.join(workdir, "wolf_anim.log")
        DONE_PATH = os.path.join(workdir, "wolf_anim.done")
        status = "OK"
        try:
            stage_anim(workdir, out)
        except Exception as e:
            import traceback
            log("FAILED:", e); log(traceback.format_exc()); status = "FAIL: " + str(e)
        flush(status)
    else:
        raise SystemExit("unknown stage: " + STAGE)
