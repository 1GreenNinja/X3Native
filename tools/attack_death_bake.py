"""
attack_death_bake.py — headless Blender ATTACK + DEATH one-shot baker for X3Native.

    blender-launcher.exe --background --python attack_death_bake.py -- \
        <name_anim.glb> <out.glb> [style]

APPEND-mode counterpart to animate_creature.py: imports an existing multi-clip
`*_anim.glb` (Idle/Walk/Run[/Jump]), KEEPS those actions, bakes two extra one-shot
clips — **Attack** (~0.8 s swing) and **Death** (~1.2 s collapse, ends held on the
floor) — and re-exports one GLB carrying all clips. MonsterSystem's W2-D wiring
fuzzy-finds "attack"/"death" and plays them as one-shots (attack preempts
locomotion during the wind-up; death suppresses the rigid topple).

  * style="biped"  — right-arm haymaker: shoulder+arm wind back, spine twist,
                     strike through with weight shift. Death: knees buckle, spine
                     folds, hips drop, held on the last frame.
                     Handles BOTH Rigify-style names (UpperLeg.L) AND Mixamo
                     names (mixamorig:LeftUpLeg / LeftArm / Spine1...).
  * style="core"   — single/loose rig: rear-back + lunge-snap pulse (attack);
                     roll-over sink (death).
  style auto-detects (biped when arm+leg bones are found).

ENV NOTE (this box): Blender is the Microsoft Store package — blender.exe is
ACL-denied and `blender-launcher.exe` DETACHES, so we report through files:
sidecar `<out>.log` + `<out>.done` marker the caller polls (same protocol as
animate_creature.py).

Clean-room: public Blender Python API + glTF 2.0 spec only.
"""
import bpy, sys, os, math

ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(ARGV) < 2:
    raise SystemExit("Usage: ... -- <name_anim.glb> <out.glb> [style] [clips]")
TARGET, OUT = ARGV[0], ARGV[1]
STYLE = ARGV[2].lower() if len(ARGV) > 2 else "auto"
# Which one-shots to bake (comma-separated). Default keeps the original behaviour.
# Recognised: attack, death, hitreact, attack2. Enemies that already carry
# Attack/Death (chief/marcus) pass "hitreact,attack2"; the aliens (no combat
# clips) pass "attack,death,hitreact".
CLIPS = [c.strip().lower() for c in
         (ARGV[3] if len(ARGV) > 3 else "attack,death").split(",") if c.strip()]

LOG_PATH  = OUT + ".log"
DONE_PATH = OUT + ".done"
_log = []
def log(*a):
    s = "[atkbake] " + " ".join(str(x) for x in a)
    _log.append(s); print(s)

def flush_log(status):
    try:
        with open(LOG_PATH, "w", encoding="utf-8") as f:
            f.write("\n".join(_log))
        with open(DONE_PATH, "w", encoding="utf-8") as f:
            f.write(status)
    except Exception as e:
        print("[atkbake] could not write log/marker:", e)

D = math.radians

# ---------------------------------------------------------------------------
# Bone lookup that understands BOTH naming families on the retargeted rigs:
#   Rigify-ish:  UpperArm.R / LowerLeg.L / Spine / Hips / Head
#   Mixamo:      mixamorig:RightArm / mixamorig:LeftUpLeg / mixamorig:Spine1 ...
# ---------------------------------------------------------------------------
def find_bone(bones, *keys, side=None):
    """First bone whose lowered name contains ALL keys (and the side token)."""
    for b in bones:
        n = b.name.lower()
        if side == "L" and not ("left" in n or n.endswith(".l") or "_l_" in n): continue
        if side == "R" and not ("right" in n or n.endswith(".r") or "_r_" in n): continue
        if all(k in n for k in keys):
            return b.name
    return None

def classify(bones):
    g = {}
    # Upper arm: mixamo "RightArm" (not ForeArm); rigify "UpperArm.R".
    g["armR"]   = find_bone(bones, "upperarm", side="R") or \
                  next((b.name for b in bones
                        if "arm" in b.name.lower() and "fore" not in b.name.lower()
                        and "lower" in b.name.lower() is False and "right" in b.name.lower()
                        and "forearm" not in b.name.lower()), None) or \
                  find_bone(bones, "arm", side="R")
    # The generic find above can hit ForeArm; prefer exact-ish:
    exact = [b.name for b in bones if b.name.lower().endswith("rightarm") or b.name.lower().endswith("arm.r")]
    if exact: g["armR"] = exact[0]
    g["foreR"]  = find_bone(bones, "forearm", side="R") or find_bone(bones, "lowerarm", side="R")
    g["foreL"]  = find_bone(bones, "forearm", side="L") or find_bone(bones, "lowerarm", side="L")
    g["armL"]   = ([b.name for b in bones if b.name.lower().endswith("leftarm") or b.name.lower().endswith("arm.l")] or
                   [find_bone(bones, "arm", side="L")])[0]
    g["spine"]  = [b.name for b in bones if "spine" in b.name.lower() or "chest" in b.name.lower()]
    g["hips"]   = find_bone(bones, "hips") or find_bone(bones, "pelvis")
    g["head"]   = find_bone(bones, "head")
    g["upLegL"] = find_bone(bones, "upleg", side="L") or find_bone(bones, "upperleg", side="L") or find_bone(bones, "thigh", side="L")
    g["upLegR"] = find_bone(bones, "upleg", side="R") or find_bone(bones, "upperleg", side="R") or find_bone(bones, "thigh", side="R")
    # Lower leg: mixamo "LeftLeg" (contains 'leg', NOT 'upleg'); rigify LowerLeg/shin.
    def lowleg(sd):
        cand = find_bone(bones, "lowerleg", side=sd) or find_bone(bones, "shin", side=sd) or find_bone(bones, "calf", side=sd)
        if cand: return cand
        for b in bones:
            n = b.name.lower()
            sok = ("left" in n) if sd == "L" else ("right" in n)
            if sok and n.endswith("leg") and "upleg" not in n:
                return b.name
        return None
    g["legL"] = lowleg("L"); g["legR"] = lowleg("R")
    g["root"] = find_bone(bones, "root")
    return g

def pick_style(g):
    return "biped" if (g.get("armR") and (g.get("upLegL") or g.get("hips"))) else "core"

# ---------------------------------------------------------------------------
def import_target():
    # NOTE: unlike animate_creature.py we KEEP imported actions (append mode).
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=TARGET)
    arm = next((o for o in bpy.data.objects if o.type == 'ARMATURE'), None)
    if not arm:
        raise RuntimeError("no armature in " + TARGET)
    for a in bpy.data.actions:
        a.use_fake_user = True          # survive even with no active slot
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
    # Idempotent RE-BAKE: if a clip of this exact name already exists (e.g. we are
    # re-baking Hitreaction/Attack onto an already-enriched GLB), drop the old one
    # first so Blender doesn't dedup us to "Hitreaction.001" and break the wired
    # fuzzy-find. Actions we are NOT re-baking are left untouched.
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

def key_loc(arm, bone, frame, x=0.0, y=0.0, z=0.0):
    if not bone: return
    pb = arm.pose.bones.get(bone)
    if not pb: return
    pb.location = (x, y, z)
    pb.keyframe_insert("location", frame=frame)

def ease(t):        # smoothstep
    return t * t * (3.0 - 2.0 * t)

# ---------------------------------------------------------------------------
# ATTACK (~0.8 s @24fps = 20 frames): wind back 0..40%, strike 40..70%, recover.
# ---------------------------------------------------------------------------
def bake_attack_biped(arm, g, frames=20):
    act = new_action(arm, "Attack")
    zero_pose(arm)
    for i in range(frames):
        f = i + 1
        t = i / (frames - 1.0)
        if t < 0.4:                       # WIND-UP: arm back+up, spine twists away
            k = ease(t / 0.4)
            armRx, armRz = -D(70) * k, D(30) * k
            foreRx = -D(50) * k
            spineY = D(18) * k
            hipsZ  = 0.02 * k
        elif t < 0.7:                     # STRIKE: whip through
            k = ease((t - 0.4) / 0.3)
            armRx, armRz = -D(70) + D(150) * k, D(30) - D(45) * k
            foreRx = -D(50) + D(45) * k
            spineY = D(18) - D(34) * k
            hipsZ  = 0.02 - 0.05 * k
        else:                             # RECOVER
            k = ease((t - 0.7) / 0.3)
            armRx, armRz = D(80) * (1 - k), -D(15) * (1 - k)
            foreRx = -D(5) * (1 - k)
            spineY = -D(16) * (1 - k)
            hipsZ  = -0.03 * (1 - k)
        key_euler(arm, g.get("armR"), f, rx=armRx, rz=armRz)
        key_euler(arm, g.get("foreR"), f, rx=foreRx)
        if g["spine"]: key_euler(arm, g["spine"][0], f, ry=spineY, rx=D(6) * math.sin(t * math.pi))
        key_euler(arm, g.get("armL"), f, rx=-armRx * 0.25)      # counter-balance
        key_loc(arm, g.get("hips"), f, z=hipsZ)
    return act

def bake_attack_core(arm, g, frames=20):
    act = new_action(arm, "Attack")
    zero_pose(arm)
    drv = g.get("root") or g.get("hips") or (arm.pose.bones[0].name if len(arm.pose.bones) else None)
    if not drv: return act
    for i in range(frames):
        f = i + 1
        t = i / (frames - 1.0)
        # POLISH: the prior bake peaked at only +28 deg -> a weak forward bow. Now a
        # COMMITTED lunge: coil back, then whip the whole body forward ~46 deg with
        # a downward drop (weight behind the strike), then settle back to rest.
        if t < 0.35:   k = ease(t / 0.35);        rx, z = -D(24) * k, 0.05 * k              # coil back + rise
        elif t < 0.60: k = ease((t - 0.35) / 0.25); rx, z = -D(24) + D(64) * k, 0.05 - 0.12 * k  # LUNGE fwd + drop
        else:          k = ease((t - 0.60) / 0.40); rx, z = D(40) * (1 - k), -0.07 * (1 - k)      # settle back
        key_euler(arm, drv, f, rx=rx)
        key_loc(arm, drv, f, z=z)
    return act

# ---------------------------------------------------------------------------
# DEATH (~1.2 s @24fps = 29 frames): stagger, knees buckle, fold to the floor,
# HELD on the final frame (the runtime freezes at clip end).
# ---------------------------------------------------------------------------
def bake_death_biped(arm, g, frames=29):
    act = new_action(arm, "Death")
    zero_pose(arm)
    for i in range(frames):
        f = i + 1
        t = i / (frames - 1.0)
        k = ease(min(1.0, t * 1.15))              # reach the floor slightly early, hold
        stag = D(8) * math.sin(min(t, 0.3) / 0.3 * math.pi)   # brief backward stagger
        key_euler(arm, g.get("upLegL"), f, rx=-D(95) * k)     # knees buckle under
        key_euler(arm, g.get("upLegR"), f, rx=-D(80) * k)
        key_euler(arm, g.get("legL"), f, rx=D(110) * k)
        key_euler(arm, g.get("legR"), f, rx=D(95) * k)
        if g["spine"]:
            key_euler(arm, g["spine"][0], f, rx=D(55) * k - stag)   # torso folds forward
            if len(g["spine"]) > 1:
                key_euler(arm, g["spine"][1], f, rx=D(30) * k)
        key_euler(arm, g.get("head"), f, rx=D(35) * k)
        key_euler(arm, g.get("armR"), f, rx=D(20) * k, rz=-D(35) * k)  # arms slack out
        key_euler(arm, g.get("armL"), f, rx=D(15) * k, rz=D(40) * k)
        key_loc(arm, g.get("hips"), f, z=-0.85 * k)            # hips drop to the floor
    return act

def bake_death_core(arm, g, frames=29):
    act = new_action(arm, "Death")
    zero_pose(arm)
    drv = g.get("root") or g.get("hips") or (arm.pose.bones[0].name if len(arm.pose.bones) else None)
    if not drv: return act
    for i in range(frames):
        f = i + 1
        t = i / (frames - 1.0)
        k = ease(min(1.0, t * 1.15))
        key_euler(arm, drv, f, rz=D(88) * k, rx=D(10) * math.sin(t * math.pi))  # roll over
        key_loc(arm, drv, f, z=-0.30 * k)                                       # sink
    return act

# ---------------------------------------------------------------------------
# HITREACTION (~0.4 s @24fps = 10 frames): a SHARP, CONTAINED flinch that RETURNS
# to neutral so the runtime can hard-cut back to locomotion with no pop. Authored
# in the target's own bone space (rest-relative euler), so it is orientation-safe
# on Z-up-authored rigs (no full-body pitch like a mocap knockback would give).
# The single half-sine pulse: snap back on impact (~35%), settle back to rest.
# ---------------------------------------------------------------------------
def bake_hitreact_biped(arm, g, frames=10):
    act = new_action(arm, "Hitreaction")
    zero_pose(arm)
    for i in range(frames):
        f = i + 1
        t = i / (frames - 1.0)
        # Fast rise to a peak near t=0.25 (frame ~3 of 10), then ease back to 0.
        # POLISH: the prior bake stacked spine(-20)+spine(-12)+head(-24) -> the
        # head pitched ~55 deg back (chin-to-sky, near-knockdown). Now the
        # cumulative head+torso recoil is ~20 deg: a CRISP readable snap-back, not
        # a backbend. Feet stay planted (no leg/hip keys) + a defensive shoulder
        # hitch (shoulders/forearms jerk up to guard) sells the impact.
        tp = 0.25
        k = math.sin(min(t / tp, 1.0) * math.pi * 0.5) if t < tp \
            else math.cos((t - tp) / (1.0 - tp) * math.pi * 0.5)
        if g["spine"]:
            key_euler(arm, g["spine"][0], f, rx=-D(8) * k, ry=D(5) * k)   # torso jerks back+twist
            if len(g["spine"]) > 1:
                key_euler(arm, g["spine"][1], f, rx=-D(5) * k)
        key_euler(arm, g.get("head"), f, rx=-D(9) * k, rz=D(8) * k)       # head snaps back
        # SHOULDER HITCH: upper arms + forearms jerk up/in (a flinch guard).
        key_euler(arm, g.get("armR"), f, rx=-D(20) * k, rz=-D(18) * k)
        key_euler(arm, g.get("armL"), f, rx=-D(18) * k, rz=D(20) * k)
        key_euler(arm, g.get("foreR"), f, rx=-D(38) * k)
        key_euler(arm, g.get("foreL"), f, rx=-D(35) * k)
    return act

def bake_hitreact_core(arm, g, frames=10):
    act = new_action(arm, "Hitreaction")
    zero_pose(arm)
    drv = g.get("root") or g.get("hips") or (arm.pose.bones[0].name if len(arm.pose.bones) else None)
    if not drv: return act
    for i in range(frames):
        f = i + 1
        t = i / (frames - 1.0)
        # POLISH: the prior bake (rx-16, z+0.02) was near-invisible vs Idle. A
        # single-bone rig can ONLY move the whole body, so PUNCH IT: a sharp fast
        # snap straight back (big base-pivot tip throws the whole body rearward) +
        # a clear upward hitch, peaking early (~frame 2) then settling to rest.
        tp = 0.2
        k = math.sin(min(t / tp, 1.0) * math.pi * 0.5) if t < tp \
            else math.cos((t - tp) / (1.0 - tp) * math.pi * 0.5)
        key_euler(arm, drv, f, rx=-D(30) * k, rz=D(14) * k)   # hard recoil back + side flick
        key_loc(arm, drv, f, z=0.10 * k)                      # upward jolt/hitch
    return act

# ---------------------------------------------------------------------------
# ATTACK2 (~0.75 s): a distinct SECOND attack so successive strikes vary. Biped:
# a LEFT-hand cross/hook (mirrors the right-hand haymaker of Attack) with a
# forward step-in. Core: a low double-jab lunge (two quick forward pulses).
# ---------------------------------------------------------------------------
def bake_attack2_biped(arm, g, frames=19):
    act = new_action(arm, "Attack2")
    zero_pose(arm)
    for i in range(frames):
        f = i + 1
        t = i / (frames - 1.0)
        if t < 0.35:                        # WIND-UP: left arm back, spine coils
            k = ease(t / 0.35)
            armLx, armLz = -D(60) * k, -D(28) * k
            foreLx = -D(45) * k
            spineY = -D(16) * k
        elif t < 0.65:                      # HOOK through
            k = ease((t - 0.35) / 0.3)
            armLx, armLz = -D(60) + D(120) * k, -D(28) + D(50) * k
            foreLx = -D(45) + D(40) * k
            spineY = -D(16) + D(30) * k
        else:                               # RECOVER
            k = ease((t - 0.65) / 0.35)
            armLx, armLz = D(60) * (1 - k), D(22) * (1 - k)
            foreLx = -D(5) * (1 - k)
            spineY = D(14) * (1 - k)
        key_euler(arm, g.get("armL"), f, rx=armLx, rz=armLz)
        key_euler(arm, g.get("foreL") or g.get("foreR"), f, rx=foreLx)
        if g["spine"]: key_euler(arm, g["spine"][0], f, ry=spineY, rx=D(5) * math.sin(t * math.pi))
        key_euler(arm, g.get("armR"), f, rx=-armLx * 0.22)     # counter-balance
        key_loc(arm, g.get("hips"), f, z=0.05 * math.sin(t * math.pi))  # step-in bob
    return act

def bake_attack2_core(arm, g, frames=19):
    act = new_action(arm, "Attack2")
    zero_pose(arm)
    drv = g.get("root") or g.get("hips") or (arm.pose.bones[0].name if len(arm.pose.bones) else None)
    if not drv: return act
    for i in range(frames):
        f = i + 1
        t = i / (frames - 1.0)
        # Two quick forward jabs (a 2 Hz lunge pulse) that decay back to rest.
        env = (1.0 - t)
        pulse = max(0.0, math.sin(t * 2.0 * math.tau))
        key_euler(arm, drv, f, rx=D(30) * pulse * env)
        key_loc(arm, drv, f, z=-0.05 * pulse * env)
    return act

# ---------------------------------------------------------------------------
def main():
    arm = import_target()
    before = [a.name for a in bpy.data.actions]
    g = classify(arm.data.bones)
    style = STYLE if STYLE != "auto" else pick_style(g)
    log("target:", os.path.basename(TARGET), "bones:", len(arm.data.bones), "style:", style)
    log("bones resolved:", {k: v for k, v in g.items() if v and k != "spine"},
        "spine:", g["spine"][:3])
    log("existing actions (kept):", before)

    biped = (style == "biped")
    BAKERS = {
        "attack":   bake_attack_biped   if biped else bake_attack_core,
        "death":    bake_death_biped    if biped else bake_death_core,
        "hitreact": bake_hitreact_biped if biped else bake_hitreact_core,
        "attack2":  bake_attack2_biped  if biped else bake_attack2_core,
    }
    log("baking clips:", CLIPS)
    for c in CLIPS:
        fn = BAKERS.get(c)
        if fn is None:
            log("  unknown clip requested, skipping:", c); continue
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
