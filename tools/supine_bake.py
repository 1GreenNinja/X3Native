"""
supine_bake.py — headless Blender SUPINE pleasure-with-dread loop baker for the
X3Native rescue-arc ward tableau (WARD_SCENE_REGISTER.md).

APPEND-mode counterpart to attack_death_bake.py: imports an existing multi-clip
`*_anim.glb` (the rigged Anna cast — Idle/Walk/Run already baked), KEEPS those
actions, bakes one NEW looping clip — **SupineIdle** — and re-exports one GLB
carrying all clips. The clip poses the standing rig onto its back (knees up,
legs apart) and layers a gentle 1 Hz pleasure loop (hip rock + back arch + leg
settle) on top. It is a LOOP, not a one-shot: the runtime plays it held for the
duration of the ward tableau.

Register (Tim 2026-08-15): the captive is with a partner she's attracted to and
is ENJOYING it; the horror is only the Alien DNA. So this clip reads as arousal,
not escape — hips moving with the thrusts, legs up and engaged, back arched, head
back — the dread lives in the dialog/audio, not the pose.

ENV NOTE (this box): Blender is the Microsoft Store package — blender.exe is
ACL-denied and `blender-launcher.exe` DETACHES, so report through files:
sidecar `<out>.log` + `<out>.done` marker the caller polls (see supine_build.ps1).

Clean-room: public Blender Python API + glTF 2.0 spec only.
"""
import bpy, sys, os, math

ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(ARGV) < 2:
    raise SystemExit("Usage: ... -- <name_anim.glb> <out.glb>")
TARGET, OUT = ARGV[0], ARGV[1]

# ---------------------------------------------------------------------------
# Supine pose parameters (RADIANS). Tune after the first grounded render.
# ---------------------------------------------------------------------------
LIE_BACK_RX = math.radians(-90)   # Hips X: tip the standing body back to lying
KNEE_UP      = math.radians(75)    # UpperLeg X: hip flexion (knees toward chest)
KNEE_BEND    = math.radians(85)    # LowerLeg X: knee bend
LEG_ABDUCT   = math.radians(28)    # UpperLeg Z: legs apart
ARM_OUT      = math.radians(22)    # UpperArm Z: arms ease out from the sides
SPINE_ARCH   = math.radians(10)    # Spine/Chest X: back arch (pleasure)
HEAD_BACK    = math.radians(15)    # Head X: tossed back

# Loop (1 Hz thrust rhythm; amplitudes are gentle so it reads as motion, not
# thrash). phases stagger so hips -> chest -> head reads as a travelling wave.
LOOP_FRAMES  = 48                  # 2 s @ 24 fps
LOOP_CYCLES  = 2                   # 2 thrusts per loop (1 Hz)
HIP_ROCK     = math.radians(7)     # Hips X oscillation
CHEST_ROCK   = math.radians(9)     # Chest X oscillation (back arch pulse)
LEG_SWAY     = math.radians(4)     # UpperLeg Z open/close pulse

LOG_PATH  = OUT + ".log"
DONE_PATH = OUT + ".done"
_log = []
def log(*a):
    s = "[supine] " + " ".join(str(x) for x in a)
    _log.append(s); print(s)

def flush_log(status):
    try:
        with open(LOG_PATH, "w", encoding="utf-8") as f:
            f.write("\n".join(_log))
        with open(DONE_PATH, "w", encoding="utf-8") as f:
            f.write(status)
    except Exception as e:
        print("[supine] could not write log/marker:", e)

# ---------------------------------------------------------------------------
# Bone lookup (same families as attack_death_bake.py).
# ---------------------------------------------------------------------------
def find_bone(bones, *keys, side=None):
    for b in bones:
        n = b.name.lower()
        if side == "L" and not ("left" in n or n.endswith(".l") or "_l_" in n): continue
        if side == "R" and not ("right" in n or n.endswith(".r") or "_r_" in n): continue
        if all(k in n for k in keys):
            return b.name
    return None

def classify(bones):
    g = {}
    g["hips"]   = find_bone(bones, "hips") or find_bone(bones, "pelvis")
    g["spine"]  = [b.name for b in bones if "spine" in b.name.lower() or "chest" in b.name.lower()]
    g["chest"]  = next((b.name for b in bones if "chest" in b.name.lower()), None) or \
                  (g["spine"][-1] if g["spine"] else None)
    g["head"]   = find_bone(bones, "head")
    g["neck"]   = find_bone(bones, "neck")
    g["upLegL"] = find_bone(bones, "upleg", side="L") or find_bone(bones, "upperleg", side="L") or find_bone(bones, "thigh", side="L")
    g["upLegR"] = find_bone(bones, "upleg", side="R") or find_bone(bones, "upperleg", side="R") or find_bone(bones, "thigh", side="R")
    g["legL"]   = find_bone(bones, "lowerleg", side="L") or find_bone(bones, "shin", side="L")
    g["legR"]   = find_bone(bones, "lowerleg", side="R") or find_bone(bones, "shin", side="R")
    g["armL"]   = find_bone(bones, "upperarm", side="L") or find_bone(bones, "arm", side="L")
    g["armR"]   = find_bone(bones, "upperarm", side="R") or find_bone(bones, "arm", side="R")
    return g

# ---------------------------------------------------------------------------
def import_target():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=TARGET)
    arm = next((o for o in bpy.data.objects if o.type == 'ARMATURE'), None)
    if not arm:
        raise RuntimeError("no armature in " + TARGET)
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
    return arm

def zero_pose(arm):
    for pb in arm.pose.bones:
        pb.rotation_mode = 'XYZ'
        pb.rotation_euler = (0.0, 0.0, 0.0)
        pb.location = (0.0, 0.0, 0.0)
        pb.scale = (1.0, 1.0, 1.0)

def new_action(arm, name):
    if not arm.animation_data:
        arm.animation_data_create()
    for old in [a for a in bpy.data.actions if a.name == name]:
        old.use_fake_user = False
        bpy.data.actions.remove(old)
    act = bpy.data.actions.new(name)
    act.use_fake_user = True
    arm.animation_data.action = act
    try:
        if getattr(act, "slots", None) is not None and len(act.slots) == 0:
            act.slots.new(id_type='OBJECT', name=arm.name)
            arm.animation_data.action_slot = act.slots[0]
    except Exception:
        pass
    return act

def key_euler(arm, bone, frame, rx=0.0, ry=0.0, rz=0.0):
    if not bone: return
    pb = arm.pose.bones.get(bone)
    if not pb: return
    pb.rotation_euler = (rx, ry, rz)
    pb.keyframe_insert("rotation_euler", frame=frame)

# ---------------------------------------------------------------------------
# SUPINE loop: base pose (on back, knees up, legs apart) + gentle pleasure loop.
# ---------------------------------------------------------------------------
def bake_supine(arm, g):
    act = new_action(arm, "SupineIdle")
    zero_pose(arm)
    N, C = LOOP_FRAMES, LOOP_CYCLES
    for i in range(N):
        f = i + 1
        ph = 2.0 * math.pi * C * (i / float(N))   # full cycles over the loop
        s   = math.sin(ph)
        s_l = math.sin(ph + 0.6)                   # legs lag the hips slightly
        # Base pose + loop oscillation.
        key_euler(arm, g["hips"],  f, rx=LIE_BACK_RX + HIP_ROCK * s)
        # Legs: knees up + bent + apart, with a gentle settle pulse.
        key_euler(arm, g["upLegL"], f, rx=KNEE_UP,              rz=+LEG_ABDUCT + LEG_SWAY * s_l)
        key_euler(arm, g["upLegR"], f, rx=KNEE_UP,              rz=-LEG_ABDUCT - LEG_SWAY * s_l)
        key_euler(arm, g["legL"],   f, rx=KNEE_BEND)
        key_euler(arm, g["legR"],   f, rx=KNEE_BEND)
        # Torso: back arch pulsing with the thrust rhythm.
        key_euler(arm, g["chest"],  f, rx=SPINE_ARCH + CHEST_ROCK * s)
        if g["neck"]: key_euler(arm, g["neck"], f, rx=HEAD_BACK * 0.6)
        key_euler(arm, g["head"],   f, rx=HEAD_BACK)
        # Arms: ease out from the sides, still (no flail).
        key_euler(arm, g["armL"],   f, rz=+ARM_OUT)
        key_euler(arm, g["armR"],   f, rz=-ARM_OUT)
    return act

# ---------------------------------------------------------------------------
def main():
    arm = import_target()
    before = [a.name for a in bpy.data.actions]
    g = classify(arm.data.bones)
    log("target:", os.path.basename(TARGET), "bones:", len(arm.data.bones))
    log("resolved:", {k: v for k, v in g.items() if v and k not in ("spine",)})
    log("existing actions (kept):", before)
    bake_supine(arm, g)
    arm.animation_data.action = None
    log("actions now:", [a.name for a in bpy.data.actions])

    for img in bpy.data.images:
        try:
            if not img.has_data: _ = img.pixels[0]
            if not img.packed_file: img.pack()
        except Exception:
            pass
    os.makedirs(os.path.dirname(OUT) or ".", exist_ok=True)
    bpy.ops.export_scene.gltf(
        filepath=OUT, export_format='GLB', export_yup=True, use_selection=False,
        export_animations=True, export_animation_mode='ACTIONS',
        export_nla_strips=False, export_force_sampling=True, export_apply=False,
        export_materials='EXPORT', export_image_format='AUTO',
        export_texcoords=True, export_normals=True, export_skins=True)
    log("EXPORTED:", OUT)

if __name__ == "__main__":
    status = "OK"
    try:
        main()
    except Exception as e:
        import traceback
        log("FAILED:", e); log(traceback.format_exc()); status = "FAIL: " + str(e)
    flush_log(status)
