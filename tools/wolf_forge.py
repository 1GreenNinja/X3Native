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
    # Smooth (weight-paint op; best effort in background).
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
# Stage: anim -- defined in a later edit pass (stage 2).
# ---------------------------------------------------------------------------
def stage_anim(workdir, out):
    raise SystemExit("anim stage not implemented yet")

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
