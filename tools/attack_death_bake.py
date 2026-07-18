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
    raise SystemExit("Usage: ... -- <name_anim.glb> <out.glb> [style]")
TARGET, OUT = ARGV[0], ARGV[1]
STYLE = ARGV[2].lower() if len(ARGV) > 2 else "auto"

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
        if t < 0.4:   k = ease(t / 0.4);        rx, z = -D(18) * k, 0.03 * k
        elif t < 0.7: k = ease((t - 0.4) / 0.3); rx, z = -D(18) + D(46) * k, 0.03 - 0.08 * k
        else:         k = ease((t - 0.7) / 0.3); rx, z = D(28) * (1 - k), -0.05 * (1 - k)
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
# HIT / FLINCH (~0.5 s @24fps = 12 frames): sharp recoil back on impact, brief
# hold, snap back to neutral. One-shot; the runtime plays it on takeDamage and
# returns to locomotion. Real keyframed motion (same authoring as Attack/Death).
# ---------------------------------------------------------------------------
def bake_hit_biped(arm, g, frames=12):
    act = new_action(arm, "Hit")
    zero_pose(arm)
    for i in range(frames):
        f = i + 1
        t = i / (frames - 1.0)
        if t < 0.30:                      # IMPACT: torso + head snap back, arm flails
            k = ease(t / 0.30)
            spineX = -D(26) * k
            headX  = -D(34) * k
            armLz  =  D(40) * k
            armRz  = -D(20) * k
            hipsZ  =  0.05 * k
        elif t < 0.55:                    # HOLD near the recoil peak
            spineX = -D(26); headX = -D(34); armLz = D(40); armRz = -D(20); hipsZ = 0.05
        else:                             # RECOVER to neutral
            k = ease((t - 0.55) / 0.45)
            spineX = -D(26) * (1 - k)
            headX  = -D(34) * (1 - k)
            armLz  =  D(40) * (1 - k)
            armRz  = -D(20) * (1 - k)
            hipsZ  =  0.05 * (1 - k)
        if g["spine"]: key_euler(arm, g["spine"][0], f, rx=spineX)
        key_euler(arm, g.get("head"), f, rx=headX)
        key_euler(arm, g.get("armL"), f, rz=armLz, rx=-D(15) * (armLz / max(D(40), 1e-6)))
        key_euler(arm, g.get("armR"), f, rz=armRz)
        key_loc(arm, g.get("hips"), f, z=hipsZ)
    return act

def bake_hit_core(arm, g, frames=12):
    act = new_action(arm, "Hit")
    zero_pose(arm)
    drv = g.get("root") or g.get("hips") or (arm.pose.bones[0].name if len(arm.pose.bones) else None)
    if not drv: return act
    for i in range(frames):
        f = i + 1
        t = i / (frames - 1.0)
        if t < 0.30:   k = ease(t / 0.30);        rx, z = D(22) * k, 0.04 * k   # rear back
        elif t < 0.55: rx, z = D(22), 0.04                                       # hold
        else:          k = ease((t - 0.55) / 0.45); rx, z = D(22) * (1 - k), 0.04 * (1 - k)
        key_euler(arm, drv, f, rx=rx)
        key_loc(arm, drv, f, z=z)
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

    if style == "biped":
        bake_attack_biped(arm, g)
        bake_hit_biped(arm, g)
        bake_death_biped(arm, g)
    else:
        bake_attack_core(arm, g)
        bake_hit_core(arm, g)
        bake_death_core(arm, g)
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
