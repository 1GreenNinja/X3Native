"""guard_clip_forge.py -- headless Blender AUTHORED Death + Struggle clips for the
facility guards (chief_martinez_anim.glb / marcus_webb_anim.glb, the ".L/.R"
19-bone humanoid rig).

WHY AUTHORED: the shipped Death (both guards) and Struggle (marcus) are DEAD
0.00-rad clips, and an inventory of EVERY biped GLB in the asset store found the
same disease fleet-wide (canon aliens, Oracle, RexBouncer, BossBreederQueen all
carry 0.00-rad Death) while Jake_22_actions.glb has no death/collapse clip at
all -- there is no donor to retarget from. So the clips are authored here
procedurally on the guard rig, exactly like tools/wolf_forge.py authored the
CyberWolf's 10 clips (the proven free path).

Stages (Blender Store launcher DETACHES: each stage writes <workdir>/guard_<stage>.log
and guard_<stage>.done for the caller to poll):

    blender-launcher.exe --background --python tools/guardanim/guard_clip_forge.py -- \
        probe <target.glb> <workdir>
      Import the guard, render grounded rest + Idle frames (floor plane, level
      camera) so the author can verify facing/sign conventions before forging.

    blender-launcher.exe --background --python tools/guardanim/guard_clip_forge.py -- \
        forge <target.glb> <out.glb> <workdir>
      Import the guard, DROP the dead Death/Struggle actions, author real ones
      (Death = shot-impact stagger -> knee buckle -> backward collapse -> floor
      settle; Struggle = looming assault-in-progress loop, arms CONTAINED),
      export ONE multi-clip Y-up GLB (ACTIONS mode, textures packed), render
      grounded QA frames per clip + an Idle reference into <workdir>.

Facing (verified via probe render): after glTF import the guard STANDS ON +Z,
FACES Blender -Y. World-axis sign map at rest:
    rx > 0 pitches the body FORWARD (toward -Y, the facing),
    rx < 0 pitches BACKWARD (+Y),
    rz spins about the vertical, ry rolls about the facing axis.
Backward fall therefore uses NEGATIVE rx on the torso chain.

Clean-room: public Blender Python API + glTF 2.0 spec only. ASCII-only source.
"""
import bpy, sys, os, math
from mathutils import Vector, Euler

ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(ARGV) < 3:
    raise SystemExit("usage: -- probe <target.glb> <workdir> | -- forge <target.glb> <out.glb> <workdir>")
STAGE = ARGV[0]

_log = []
LOG_PATH = DONE_PATH = None
def log(*a):
    s = "[guard_forge] " + " ".join(str(x) for x in a)
    _log.append(s); print(s)
def flush(status):
    try:
        with open(LOG_PATH, "w", encoding="utf-8") as f: f.write("\n".join(_log))
        with open(DONE_PATH, "w", encoding="utf-8") as f: f.write(status)
    except Exception as e:
        print("[guard_forge] log write failed:", e)

D = math.radians
TAU = math.tau
FPS = 24.0

# BACK/FWD pitch sign for the torso chain: rx = BACK * degrees pitches the body
# BACKWARD (away from the -Y facing). Set from the probe render.
BACK = -1.0

# ---------------------------------------------------------------------------
# Import (retarget_library.py discipline: pack textures EARLY, fake-user
# materials, delete stray non-skinned meshes).
# ---------------------------------------------------------------------------
def reset_factory():
    bpy.ops.wm.read_factory_settings(use_empty=True)

def import_target(path, drop_actions=()):
    bpy.ops.import_scene.gltf(filepath=path)
    arm = next((o for o in bpy.data.objects if o.type == 'ARMATURE'), None)
    if not arm:
        raise RuntimeError("no armature in " + path)
    arm.animation_data_clear()
    for name in drop_actions:
        a = bpy.data.actions.get(name)
        if a is not None:
            log("dropping dead action:", name)
            bpy.data.actions.remove(a)
    for a in bpy.data.actions:
        a.use_fake_user = True
    for img in bpy.data.images:
        try:
            if not img.has_data: _ = img.pixels[0]
            if not img.packed_file: img.pack()
            img.use_fake_user = True
        except Exception as e:
            log("img pack warn", img.name, e)
    for m in bpy.data.materials:
        m.use_fake_user = True
    def _is_tgt_mesh(o):
        if o.type != 'MESH': return False
        if o.parent is arm: return True
        return any(md.type == 'ARMATURE' and getattr(md, 'object', None) is arm
                   for md in o.modifiers)
    meshes = [o for o in bpy.data.objects if _is_tgt_mesh(o)]
    for o in [o for o in bpy.data.objects if o.type == 'MESH' and o not in meshes]:
        log("deleting stray mesh:", o.name)
        bpy.data.objects.remove(o, do_unlink=True)
    log("target:", os.path.basename(path), "bones:", len(arm.data.bones),
        "kept actions:", [a.name for a in bpy.data.actions])
    return arm, meshes

# World-axis pose helpers (wolf_forge.py convention): channel =
# q_rest^-1 * W * q_rest so a pose reads as a WORLD rotation about the bone head
# at rest; parents' deviations compose on top (standard FK layering).
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

    def key(self, f, bone, rx=0.0, ry=0.0, rz=0.0, loc=None):
        pb = set_wrot(self.arm, bone, rx, ry, rz)
        if pb is None:
            log("WARN missing bone", bone); return
        pb.keyframe_insert("rotation_quaternion", frame=f)
        if loc is not None:
            set_wloc(self.arm, bone, *loc)
            pb.keyframe_insert("location", frame=f)

# ---------------------------------------------------------------------------
# Grounded QA scene (HARD RULE: floor plane + level camera; no-floor close-ups
# falsely show floating/back-pitch).
# ---------------------------------------------------------------------------
def qa_scene():
    scn = bpy.context.scene
    if "QAFloor" not in bpy.data.objects:
        bpy.ops.mesh.primitive_plane_add(size=30, location=(0, 0, 0))
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

def qa_shot(path, view="side", center=None, diag=2.4):
    c = center or Vector((0, 0, 0.9))
    scn = bpy.context.scene
    cam = bpy.data.objects["QACam"]
    if view == "side":     # near-level profile camera; floor reads in frame
        cam.location = (c.x - diag * 2.4, c.y, 1.35)
        cam.rotation_euler = (D(82), 0, D(-90))
        cam.data.type = 'PERSP'; cam.data.lens = 48
    elif view == "front":  # level camera looking down -Y->+Y at the face side
        cam.location = (c.x, c.y - diag * 2.6, 1.05)
        cam.rotation_euler = (D(87), 0, 0)
        cam.data.type = 'PERSP'; cam.data.lens = 48
    else:                  # low 3/4 perspective, level-ish
        cam.location = (c.x - diag * 1.8, c.y - diag * 1.8, 1.25)
        cam.rotation_euler = (D(78), 0, D(-45))
        cam.data.type = 'PERSP'; cam.data.lens = 42
    scn.render.filepath = path
    bpy.ops.render.render(write_still=True)
    log("render:", os.path.basename(path))

# ---------------------------------------------------------------------------
# Clip authors. Bones: Hips Spine Chest Neck Head, Shoulder/UpperArm/LowerArm/
# Hand .L/.R, UpperLeg/LowerLeg/Foot .L/.R. Rig faces -Y; rx = BACK*a pitches
# backward. Hips rest head sits at z ~= 0.864 (feet at z=0).
# ---------------------------------------------------------------------------
def clip_death(arm):
    """Shot from the front: impact recoil -> stagger, knees buckle -> falls
    BACKWARD onto his back, knees up, arms staying close to the body (contained)
    -> lands with a small settle bounce and lies still. 40 frames ~1.67 s,
    freeze-on-last-frame (the engine holds the final pose as the corpse)."""
    pw = PW(arm, "Death")
    B = BACK
    # World-axis sign facts (probe-verified, rig faces -Y): +rx tilts the torso
    # up-vector (+Z) FORWARD but tilts a downward bone (-Z legs/arms) BACKWARD.
    # So a leg swings forward with NEGATIVE own-rx. Deviations about the shared
    # world X axis compose additively down the chain (hips + own).
    def pose(f, back, hz, hy, knee, shin, foot, sp, ne, armx, army, elb, roll=0.0):
        # back: hips backward pitch deg (B sign applied); hz: hips DROP (m);
        # hy: hips shift toward +Y/backward (m); knee/shin/foot: OWN world rx
        # deviation of thigh/lower-leg/foot (+ = backward for these downward
        # bones, - = forward); sp: spine+chest extra arch; ne: neck deg; armx:
        # arm own pitch; army: arm outward flare; elb: elbow bend.
        pw.key(f, "Hips", rx=B * D(back), ry=D(roll),
               loc=(0.0, -B * hy, -hz))
        pw.key(f, "Spine", rx=B * D(sp))
        pw.key(f, "Chest", rx=B * D(sp))
        pw.key(f, "Neck", rx=B * D(ne))
        pw.key(f, "Head", rx=B * D(ne) * 0.6, rz=D(roll) * 1.5)
        for sd, m in ((".L", 1.0), (".R", 0.85)):
            s = 1.0 if sd == ".L" else -1.0
            pw.key(f, "Shoulder" + sd)
            pw.key(f, "UpperArm" + sd, rx=D(armx) * m, rz=s * D(army) * m)
            pw.key(f, "LowerArm" + sd, rx=D(elb) * m)
            pw.key(f, "Hand" + sd)
            pw.key(f, "UpperLeg" + sd, rx=D(knee) * (1.0 if sd == ".L" else 0.92))
            pw.key(f, "LowerLeg" + sd, rx=D(shin))
            pw.key(f, "Foot" + sd, rx=D(foot))
    # Choreography (feet planted at y~0; the body falls onto its back BEHIND
    # them, ending head +Y / feet -Y, legs near-straight with a slight knee-up
    # bend, arms settling to the sides). Leg values solved so knee/ankle stay
    # ON the floor plane through the fall (thigh theta = -back + knee, etc.).
    #      f  back    hz     hy   knee shin foot  sp   ne  armx army elb roll
    pose(  1,  0.0, 0.000, 0.00,   0,   0,   0,   0,   0,   0,   0,   0)       # standing
    pose(  4,  6.0, 0.030, 0.00,   4,   6,   2,   3,  10,  -8,   6,   8)       # impact recoil
    pose( 10, 22.0, 0.170, 0.03,  10,  21,   8,   6,  14, -12,   9,  14)       # stagger, knees give
    pose( 16, 48.0, 0.400, 0.12,  13,  65,  15,   9,  10, -16,  12,  18, 4)    # buckle: heels lift, sinking
    pose( 22, 72.0, 0.620, 0.26,   5,  55,  12,   9,   2,  -8,  12,  16, 7)    # rolling onto the back
    pose( 25, 86.0, 0.720, 0.34, -12,  30,   6,   8,  -6,   3,   9,  12, 8)    # floor impact, legs unfold
    pose( 28, 83.0, 0.705, 0.33, -10,  28,   6,   8,  -2,   5,   8,  10, 8)    # settle bounce
    pose( 32, 86.0, 0.720, 0.34, -12,  30,   6,   8,  -5,   3,   7,   9, 8)    # settle
    pose( 40, 86.0, 0.720, 0.34, -12,  30,   6,   8,  -5,   3,   7,   9, 8)    # still (corpse)
    return 40

def clip_struggle(arm):
    """W5-2 ward tableau calm loop: the assault IN PROGRESS. The guard looms
    bent over the victim -- torso pitched forward, knees flexed, arms low in
    front pinning down and pumping alternately, head down with sharp small
    shakes. Arms stay DOWN + CLOSE to the body (contained; no raised arms).
    40 frames ~1.67 s, seamless loop (pure sinusoids, f1 == f41)."""
    pw = PW(arm, "Struggle")
    N = 40
    # +rx = torso forward pitch; a LEG swings forward with NEGATIVE rx (downward
    # bone); arms come forward-IN around the body via rz yaw (left arm needs
    # negative rz, right positive -- i.e. -s * yaw).
    for i in range(N + 1):
        f = i + 1
        p = i / float(N)
        rock  = math.sin(2 * TAU * p)            # 2 body rocks per loop
        shake = math.sin(3 * TAU * p + 0.7)      # 3 head shakes per loop
        # torso: bent forward over the victim, rocking with the effort
        pw.key(f, "Hips", rx=D(6.0 + 1.5 * rock), rz=D(3.0) * math.sin(2 * TAU * p + 1.1),
               loc=(0.012 * math.sin(2 * TAU * p + 0.5), 0.0, -(0.12 + 0.02 * rock)))
        pw.key(f, "Spine", rx=D(10.0 + 1.5 * rock))
        pw.key(f, "Chest", rx=D(13.0 + 2.0 * rock), rz=D(3.0) * rock)
        pw.key(f, "Neck", rx=D(10.0))
        pw.key(f, "Head", rx=D(5.0), rz=D(9.0) * shake)
        # legs: staggered brace (left forward, right back), knees soft, planted
        pw.key(f, "UpperLeg.L", rx=D(-16.0 - 2.0 * rock))
        pw.key(f, "LowerLeg.L", rx=D(12.0))
        pw.key(f, "Foot.L", rx=D(8.0))
        pw.key(f, "UpperLeg.R", rx=D(10.0 + 2.0 * rock))
        pw.key(f, "LowerLeg.R", rx=D(20.0))
        pw.key(f, "Foot.R", rx=D(12.0))
        # arms: swung forward-IN and DOWN (pinning), pumping alternately;
        # contained -- never raised above the shoulders
        for sd, ph in ((".L", 0.0), (".R", math.pi)):
            s = 1.0 if sd == ".L" else -1.0
            pu = math.sin(2 * TAU * p + ph)
            pw.key(f, "Shoulder" + sd, rx=D(3.0) * pu)
            pw.key(f, "UpperArm" + sd, rx=D(28.0 + 8.0 * pu),
                   rz=-s * D(32.0 + 5.0 * pu))
            pw.key(f, "LowerArm" + sd, rx=D(26.0 + 8.0 * pu))
            pw.key(f, "Hand" + sd, rx=D(8.0 * pu))
    return N

# ---------------------------------------------------------------------------
# Stages
# ---------------------------------------------------------------------------
def stage_probe(target, workdir):
    reset_factory()
    arm, meshes = import_target(target)
    qa_scene()
    clear_pose(arm)
    qa_shot(os.path.join(workdir, "probe_rest_front.png"), "front")
    qa_shot(os.path.join(workdir, "probe_rest_side.png"), "side")
    idle = bpy.data.actions.get("Idle")
    if idle is not None:
        if not arm.animation_data: arm.animation_data_create()
        arm.animation_data.action = idle
        try:
            if getattr(idle, "slots", None) is not None and len(idle.slots) > 0:
                arm.animation_data.action_slot = idle.slots[0]
        except Exception:
            pass
        bpy.context.scene.frame_set(30)
        qa_shot(os.path.join(workdir, "probe_idle_front.png"), "front")
        qa_shot(os.path.join(workdir, "probe_idle_side.png"), "side")
    # facing probe: pitch the whole body +20 deg rx and render -- tells the sign
    arm.animation_data.action = None
    clear_pose(arm)
    set_wrot(arm, "Hips", rx=D(20))
    qa_shot(os.path.join(workdir, "probe_rx+20_side.png"), "side")
    log("probe OK")

def stage_forge(target, out, workdir):
    reset_factory()
    arm, meshes = import_target(target, drop_actions=("Death", "Struggle"))
    bpy.context.scene.render.fps = int(FPS)

    lengths = {}
    for builder in (clip_death, clip_struggle):
        n = builder(arm)
        nm = arm.animation_data.action.name
        lengths[nm] = n
        log("authored %-9s %2d frames  %.2fs" % (nm, n, n / FPS))
    arm.animation_data.action = None
    clear_pose(arm)

    for img in bpy.data.images:
        try:
            if not img.has_data: _ = img.pixels[0]
            if not img.packed_file: img.pack()
        except Exception as e:
            log("pack warn", img.name, e)
    log("pre-export actions:", [a.name for a in bpy.data.actions])

    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    bpy.ops.export_scene.gltf(
        filepath=out, export_format='GLB', export_yup=True, use_selection=False,
        export_animations=True, export_animation_mode='ACTIONS',
        export_nla_strips=False, export_force_sampling=True, export_apply=False,
        export_materials='EXPORT', export_image_format='AUTO',
        export_texcoords=True, export_normals=True, export_tangents=True,
        export_skins=True)
    log("EXPORTED:", out, os.path.getsize(out), "bytes")

    # grounded QA renders: new clips at key frames + an Idle reference frame
    qa_scene()
    tag = os.path.splitext(os.path.basename(target))[0].replace("_anim", "")
    shots = {"Death": [1, 10, 16, 22, 25, 40], "Struggle": [1, 10, 20, 30],
             "Idle": [30]}
    for nm, frames in shots.items():
        act = bpy.data.actions.get(nm)
        if act is None:
            log("QA WARN: no action", nm); continue
        arm.animation_data.action = act
        try:
            if getattr(act, "slots", None) is not None and len(act.slots) > 0:
                arm.animation_data.action_slot = act.slots[0]
        except Exception:
            pass
        for f in frames:
            bpy.context.scene.frame_set(f)
            c = Vector((0, 0, 0.55)) if nm == "Death" and f >= 16 else Vector((0, 0, 0.9))
            qa_shot(os.path.join(workdir, "%s_%s_f%02d_side.png" % (tag, nm, f)), "side", c)
            if nm == "Death" and f in (22, 25, 40):
                qa_shot(os.path.join(workdir, "%s_%s_f%02d_34.png" % (tag, nm, f)), "persp", c)
            if nm == "Struggle" and f in (10, 30):
                qa_shot(os.path.join(workdir, "%s_%s_f%02d_front.png" % (tag, nm, f)), "front", c)
    arm.animation_data.action = None
    log("forge OK:", {k: v for k, v in lengths.items()})

if __name__ == "__main__":
    status = "OK"
    # ABSOLUTE paths only: Blender's render writer resolves relative paths
    # against its own notion of cwd (Store package), NOT the caller's.
    ARGV = [ARGV[0]] + [os.path.abspath(a) for a in ARGV[1:]]
    if STAGE == "probe":
        target, workdir = ARGV[1], ARGV[2]
        os.makedirs(workdir, exist_ok=True)
        LOG_PATH = os.path.join(workdir, "guard_probe.log")
        DONE_PATH = os.path.join(workdir, "guard_probe.done")
        try:
            stage_probe(target, workdir)
        except Exception as e:
            import traceback
            log("FAILED:", e); log(traceback.format_exc()); status = "FAIL: " + str(e)
        flush(status)
    elif STAGE == "forge":
        target, out, workdir = ARGV[1], ARGV[2], ARGV[3]
        os.makedirs(workdir, exist_ok=True)
        LOG_PATH = os.path.join(workdir, "guard_forge.log")
        DONE_PATH = os.path.join(workdir, "guard_forge.done")
        try:
            stage_forge(target, out, workdir)
        except Exception as e:
            import traceback
            log("FAILED:", e); log(traceback.format_exc()); status = "FAIL: " + str(e)
        flush(status)
    else:
        raise SystemExit("unknown stage: " + STAGE)
