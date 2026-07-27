"""
crowd_gesture_bake.py - headless Blender LIVING-WORLD gesture baker for X3Native.

    blender-launcher.exe --background --python crowd_gesture_bake.py -- \
        <name_anim.glb> <out.glb> [clips]

APPEND-mode civilian-gesture counterpart to attack_death_bake.py. Imports a crowd
humanoid ".L/.R" rig (e.g. AnnaCasual_anim.glb: Idle/Run/Talk/Walk), KEEPS those
actions, and bakes looping civilian gestures onto the rig's OWN bones (rest-relative
euler authoring, orientation-safe on Z-up-authored rigs), then re-exports one GLB
with all clips. CrowdSkin maps its agent STATES (Converse/Work/Play/Idle) to these
clips via setCalmLoop(fuzzyName); rigs lacking a clip keep the procedural fallback.

Authored gestures (all looping unless noted):
  * LookAround - subtle idle variant: slow head turns + torso sway + weight shift.
  * Converse   - conversational gesture: head nods, hand/forearm gesturing, breath.
  * Work       - task loop: lean in + both forearms reach/tinker, head down at work.
  * Sit        - seated idle: hips lowered/back, thighs forward, shins down, sway.
  * Drink      - raise one hand toward the mouth, hold, lower (loop).
  * Cheer      - both arms up with a small clap/pump + bounce.
  * CheckDevice- glance down at a two-hand handheld at chest height (reading scan).
  * CarryIdle  - stand holding something at the waist, slow weight-balance shift.

The runtime plays these one-clip-at-a-time as calm loops (no blending needed), so
each is a self-contained loop that starts and ends at the same pose.

ENV NOTE (this box): Blender is the Microsoft Store package - blender.exe is
ACL-denied and blender-launcher.exe DETACHES, so we report through files: sidecar
<out>.log + <out>.done marker the caller polls. ASCII-only source.

Clean-room: public Blender Python API + glTF 2.0 spec only.
"""
import bpy, sys, os, math

ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(ARGV) < 2:
    raise SystemExit("Usage: ... -- <name_anim.glb> <out.glb> [clips]")
TARGET, OUT = ARGV[0], ARGV[1]
CLIPS = [c.strip().lower() for c in
         (ARGV[2] if len(ARGV) > 2 else
          "lookaround,converse,work,sit,drink,cheer").split(",") if c.strip()]

LOG_PATH  = OUT + ".log"
DONE_PATH = OUT + ".done"
_log = []
def log(*a):
    s = "[gesture] " + " ".join(str(x) for x in a)
    _log.append(s); print(s)

def flush_log(status):
    try:
        with open(LOG_PATH, "w", encoding="utf-8") as f:
            f.write("\n".join(_log))
        with open(DONE_PATH, "w", encoding="utf-8") as f:
            f.write(status)
    except Exception as e:
        print("[gesture] could not write log/marker:", e)

D = math.radians
TAU = math.tau

# --- bone name resolution (case-insensitive; .L/.R or Left/Right) ---------
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
    g["hips"]  = find_bone(bones, "hips") or find_bone(bones, "pelvis")
    g["head"]  = find_bone(bones, "head")
    g["neck"]  = find_bone(bones, "neck")
    g["spine"] = [b.name for b in bones if "spine" in b.name.lower()]
    g["chest"] = find_bone(bones, "chest") or (g["spine"][-1] if g["spine"] else None)
    for sd in ("L", "R"):
        g["arm"+sd]  = ([b.name for b in bones
                         if b.name.lower().endswith("arm."+sd.lower())
                         or b.name.lower().endswith(sd.lower()+"arm")] or
                        [find_bone(bones, "upperarm", side=sd) or find_bone(bones, "arm", side=sd)])[0]
        g["fore"+sd] = find_bone(bones, "forearm", side=sd) or find_bone(bones, "lowerarm", side=sd)
        g["hand"+sd] = find_bone(bones, "hand", side=sd)
        g["upleg"+sd]= find_bone(bones, "upperleg", side=sd) or find_bone(bones, "upleg", side=sd) or find_bone(bones, "thigh", side=sd)
        g["loleg"+sd]= (find_bone(bones, "lowerleg", side=sd) or find_bone(bones, "shin", side=sd)
                        or next((b.name for b in bones
                                 if (("left" in b.name.lower()) if sd=="L" else ("right" in b.name.lower()))
                                 and b.name.lower().endswith("leg") and "upleg" not in b.name.lower()), None))
        g["foot"+sd] = find_bone(bones, "foot", side=sd) or find_bone(bones, "ankle", side=sd)
    return g

# --- scene / action helpers (mirror animate_creature.py) ------------------
def import_target():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=TARGET)
    arm = next((o for o in bpy.data.objects if o.type == 'ARMATURE'), None)
    if not arm:
        raise RuntimeError("no armature in " + TARGET)
    for a in bpy.data.actions:
        a.use_fake_user = True           # APPEND: keep imported clips
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

def ke(arm, bone, frame, rx=0.0, ry=0.0, rz=0.0):
    if not bone: return
    pb = arm.pose.bones.get(bone)
    if not pb: return
    pb.rotation_euler = (rx, ry, rz)
    pb.keyframe_insert("rotation_euler", frame=frame)

def kl(arm, bone, frame, x=0.0, y=0.0, z=0.0):
    if not bone: return
    pb = arm.pose.bones.get(bone)
    if not pb: return
    pb.location = (x, y, z)
    pb.keyframe_insert("location", frame=frame)

# --- gesture bakers -------------------------------------------------------
def bake_lookaround(arm, g, frames=90):
    new_action(arm, "LookAround"); zero_pose(arm)
    for i in range(frames):
        f = i + 1; p = (i / frames) * TAU
        ke(arm, g["head"], f, rz=math.sin(p) * D(22), rx=math.sin(p * 2 + 1.0) * D(5))
        ke(arm, g["neck"], f, rz=math.sin(p) * D(8))
        if g["spine"]: ke(arm, g["spine"][0], f, rz=math.sin(p) * D(4), rx=math.sin(p * 2) * D(1.5))
        # subtle weight shift hip sway
        kl(arm, g["hips"], f, x=math.sin(p) * 0.02, z=abs(math.sin(p * 2)) * 0.006)

def bake_converse(arm, g, frames=80):
    new_action(arm, "Converse"); zero_pose(arm)
    for i in range(frames):
        f = i + 1; p = (i / frames) * TAU
        ke(arm, g["head"], f, rx=math.sin(p * 2) * D(8), rz=math.sin(p) * D(10))
        ke(arm, g["neck"], f, rx=math.sin(p * 2 + 0.5) * D(4))
        if g["chest"]: ke(arm, g["chest"], f, rx=math.sin(p * 2) * D(2), rz=math.sin(p) * D(3))
        # right hand does most of the gesturing, left lighter
        ke(arm, g["armR"], f, rz=D(-16) + math.sin(p) * D(9), rx=math.sin(p * 2 + 1) * D(7))
        ke(arm, g["foreR"], f, rz=D(-24) + math.sin(p * 2) * D(22), ry=math.sin(p) * D(10))
        ke(arm, g["handR"], f, rx=math.sin(p * 3) * D(15))
        ke(arm, g["armL"], f, rz=D(12) + math.sin(p + math.pi) * D(5))
        ke(arm, g["foreL"], f, rz=D(16) + math.sin(p * 2 + math.pi) * D(11))

def bake_work(arm, g, frames=72):
    new_action(arm, "Work"); zero_pose(arm)
    for i in range(frames):
        f = i + 1; p = (i / frames) * TAU
        # lean in over the task + look down
        if g["spine"]: ke(arm, g["spine"][0], f, rx=D(16) + math.sin(p * 2) * D(2))
        if g["chest"]: ke(arm, g["chest"], f, rx=D(8))
        ke(arm, g["head"], f, rx=D(20) + math.sin(p * 2) * D(4))
        # both arms forward + forearms working up/down out of phase (tinker/type)
        for sd, ph in (("R", 0.0), ("L", math.pi)):
            ke(arm, g["arm"+sd], f, rx=D(38), rz=(D(-20) if sd == "R" else D(20)))
            ke(arm, g["fore"+sd], f, rx=D(-55) + math.sin(p * 2 + ph) * D(18),
               rz=(D(-10) if sd == "R" else D(10)))
            ke(arm, g["hand"+sd], f, rx=math.sin(p * 4 + ph) * D(12))
        kl(arm, g["hips"], f, z=-0.02)

def bake_sit(arm, g, frames=70):
    new_action(arm, "Sit"); zero_pose(arm)
    for i in range(frames):
        f = i + 1; p = (i / frames) * TAU
        # thighs forward ~90, shins down to bring feet under; hips drop + back
        for sd in ("L", "R"):
            ke(arm, g["upleg"+sd], f, rx=D(88), rz=(D(6) if sd == "L" else D(-6)))
            ke(arm, g["loleg"+sd], f, rx=-D(92))
            ke(arm, g["foot"+sd], f, rx=D(10))
        if g["spine"]: ke(arm, g["spine"][0], f, rx=D(6) + math.sin(p) * D(2))
        ke(arm, g["head"], f, rz=math.sin(p) * D(9), rx=math.sin(p * 2) * D(3))
        # hands rest on thighs
        ke(arm, g["armR"], f, rx=D(30), rz=D(-8)); ke(arm, g["foreR"], f, rx=-D(35))
        ke(arm, g["armL"], f, rx=D(30), rz=D(8));  ke(arm, g["foreL"], f, rx=-D(35))
        kl(arm, g["hips"], f, y=-0.42, z=-0.10)     # lower + back onto the seat
    return

def bake_drink(arm, g, frames=90):
    new_action(arm, "Drink"); zero_pose(arm)
    for i in range(frames):
        f = i + 1
        t = i / (frames - 1.0)
        # raise 0..0.35, hold 0.35..0.65, lower 0.65..1 (smooth)
        if t < 0.35:   k = math.sin(t / 0.35 * math.pi * 0.5)
        elif t < 0.65: k = 1.0
        else:          k = math.cos((t - 0.65) / 0.35 * math.pi * 0.5)
        # Upper arm lifts forward + slightly across; forearm folds up hard so the
        # hand arrives at MOUTH height (not the chest). Tuned on AnnaCasual.
        ke(arm, g["armR"], f, rx=D(82) * k, rz=D(-30) * k, ry=D(12) * k)
        ke(arm, g["foreR"], f, rx=-D(120) * k, rz=D(15) * k)   # fold to the mouth
        ke(arm, g["handR"], f, rx=-D(15) * k)
        ke(arm, g["head"], f, rx=D(14) * k)          # tip head to meet the cup
        # idle left arm + subtle sway
        ke(arm, g["armL"], f, rz=D(8))
        if g["spine"]: ke(arm, g["spine"][0], f, rz=math.sin((i/frames)*TAU) * D(2))

def bake_cheer(arm, g, frames=48):
    new_action(arm, "Cheer"); zero_pose(arm)
    for i in range(frames):
        f = i + 1; p = (i / frames) * TAU
        pump = 0.5 + 0.5 * math.sin(p * 2)           # 2 pumps/loop
        # Arms up into a WIDE V beside (not over) the head: rx lifts them up,
        # a big rz SPREAD sends the hands out to the sides so they clear the face;
        # only a light forearm pump. Avoids the arms-through-skull clipping.
        for sd in ("L", "R"):
            ke(arm, g["arm"+sd], f, rx=-D(112) - pump * D(12),
               rz=(D(58) if sd == "L" else -D(58)))
            ke(arm, g["fore"+sd], f, rx=-D(12) - pump * D(18),
               rz=(D(20) if sd == "L" else -D(20)))
        ke(arm, g["head"], f, rx=-D(5) - pump * D(4))
        kl(arm, g["hips"], f, z=abs(math.sin(p * 2)) * 0.04)    # little bounce

def bake_check_device(arm, g, frames=96):
    # Glance down at a handheld: both arms come forward-up (NEG rx raises the arm
    # on this rig), forearms fold so the hands meet at chest reading height, head
    # tips down with a slow scan, then relax (smooth up-hold-down loop).
    new_action(arm, "CheckDevice"); zero_pose(arm)
    for i in range(frames):
        f = i + 1; t = i / (frames - 1.0); p = (i / frames) * TAU
        if t < 0.22:   k = math.sin(t / 0.22 * math.pi * 0.5)
        elif t < 0.82: k = 1.0
        else:          k = math.cos((t - 0.82) / 0.18 * math.pi * 0.5)
        for sd, sgn in (("R", -1.0), ("L", 1.0)):
            ke(arm, g["arm"+sd], f, rx=-D(50) * k, rz=sgn * D(10) * k)
            ke(arm, g["fore"+sd], f, rx=-D(60) * k, rz=sgn * D(7) * k)
            ke(arm, g["hand"+sd], f, rx=-D(6) * k)
        # head tips DOWN to the screen + a slow reading scan
        ke(arm, g["head"], f, rx=D(20) * k + math.sin(p * 2) * D(2),
           rz=math.sin(p) * D(3) * k)
        ke(arm, g["neck"], f, rx=D(5) * k)
        if g["spine"]: ke(arm, g["spine"][0], f, rx=D(3) * k + math.sin(p) * D(1.0))
        kl(arm, g["hips"], f, x=math.sin(p) * 0.012)

def bake_carry_idle(arm, g, frames=84):
    # Stand holding something at the waist: upper arms a touch forward, forearms
    # folded UP so the hands carry in front, a slow weight-balance shift. Arms
    # stay MID-RANGE (no overhead extreme -> clean shoulder deformation).
    new_action(arm, "CarryIdle"); zero_pose(arm)
    for i in range(frames):
        f = i + 1; p = (i / frames) * TAU
        for sd, sgn, ph in (("R", -1.0, 0.0), ("L", 1.0, math.pi)):
            ke(arm, g["arm"+sd], f, rx=-D(26), rz=sgn * D(6) + math.sin(p + ph) * D(2))
            ke(arm, g["fore"+sd], f, rx=-D(80) + math.sin(p + ph) * D(3), rz=sgn * D(10))
            ke(arm, g["hand"+sd], f, rx=-D(8))
        ke(arm, g["head"], f, rz=math.sin(p) * D(6), rx=D(4) + math.sin(p * 2) * D(2))
        if g["spine"]: ke(arm, g["spine"][0], f, rx=D(6) + math.sin(p) * D(1.5))
        kl(arm, g["hips"], f, x=math.sin(p) * 0.015, z=abs(math.sin(p * 2)) * 0.005)

BAKERS = {"lookaround": bake_lookaround, "converse": bake_converse,
          "work": bake_work, "sit": bake_sit, "drink": bake_drink,
          "cheer": bake_cheer, "checkdevice": bake_check_device,
          "carryidle": bake_carry_idle}

def main():
    arm = import_target()
    before = [a.name for a in bpy.data.actions]
    g = classify(arm.data.bones)
    log("target:", os.path.basename(TARGET), "bones:", len(arm.data.bones))
    log("resolved:", {k: v for k, v in g.items() if v and k != "spine"}, "spine:", g["spine"])
    log("existing (kept):", before, "baking:", CLIPS)
    for c in CLIPS:
        fn = BAKERS.get(c)
        if fn is None:
            log("  unknown gesture, skipping:", c); continue
        fn(arm, g)
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
