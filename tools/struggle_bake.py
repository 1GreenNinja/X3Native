"""
struggle_bake.py — headless Blender STRUGGLE loop baker for X3Native (W5-2).

    blender-launcher.exe --background --python struggle_bake.py -- \
        <name_anim.glb> <out.glb> [style]

Sibling of attack_death_bake.py (same append protocol): imports a multi-clip
`*_anim.glb`, KEEPS every existing action (Idle/Walk/Run/Attack/Death...), bakes
ONE extra clip — **Struggle** (~1.6 s seamless LOOP: the creature looming over a
victim, weight shifting, arms working downward — threat-posture, never explicit,
per RESCUE_SETPIECE_DESIGN.md §3) — and re-exports one GLB carrying all clips.
MonsterSystem::setCalmLoop("struggle") plays it as the calm-state tableau pose.

  * style="biped" — hunched loom: spine pitched over, both arms reaching down
                    with asymmetric working motion, hips low + swaying.
  * style="core"  — quadruped/loose rig: rear-up + press-down pulse on the root.
  style auto-detects (biped when arm+leg bones resolve).

All curves are built from sin(2*pi*t) terms so frame N-1 flows into frame 0 —
the loop is seamless by construction.

ENV NOTE (this box): Store-Blender — blender.exe ACL-denied, blender-launcher
DETACHES; we report through `<out>.log` + `<out>.done` (poll them).

Clean-room: public Blender Python API + glTF 2.0 spec only.
"""
import bpy, sys, os, math

ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(ARGV) < 2:
    raise SystemExit("Usage: ... -- <name_anim.glb> <out.glb> [style]")
TARGET, OUT = ARGV[0], ARGV[1]
STYLE = ARGV[2].lower() if len(ARGV) > 2 else "auto"

LOG_PATH, DONE_PATH = OUT + ".log", OUT + ".done"
_log = []
def log(*a):
    s = "[strugglebake] " + " ".join(str(x) for x in a)
    _log.append(s); print(s)

def flush_log(status):
    try:
        with open(LOG_PATH, "w", encoding="utf-8") as f: f.write("\n".join(_log))
        with open(DONE_PATH, "w", encoding="utf-8") as f: f.write(status)
    except Exception as e:
        print("[strugglebake] could not write log/marker:", e)

D = math.radians

# ---- bone lookup (both naming families) — mirrors attack_death_bake.py ------
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
    g["armR"]  = find_bone(bones, "upperarm", side="R") or find_bone(bones, "arm", side="R")
    g["armL"]  = find_bone(bones, "upperarm", side="L") or find_bone(bones, "arm", side="L")
    g["foreR"] = find_bone(bones, "forearm", side="R")
    g["foreL"] = find_bone(bones, "forearm", side="L")
    g["hips"]  = find_bone(bones, "hips") or find_bone(bones, "pelvis")
    g["head"]  = find_bone(bones, "head")
    g["spine"] = [b.name for b in bones if "spine" in b.name.lower()]
    g["upLegL"]= find_bone(bones, "upleg", side="L") or find_bone(bones, "upperleg", side="L")
    g["root"]  = find_bone(bones, "root")
    return g

def pick_style(g):
    return "biped" if (g.get("armR") and (g.get("upLegL") or g.get("hips"))) else "core"

def import_target():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=TARGET)
    arm = next((o for o in bpy.data.objects if o.type == 'ARMATURE'), None)
    if not arm:
        raise RuntimeError("no armature in " + TARGET)
    for a in bpy.data.actions: a.use_fake_user = True
    for img in bpy.data.images:
        try:
            if not img.has_data: _ = img.pixels[0]
            if not img.packed_file: img.pack()
            img.use_fake_user = True
        except Exception as e:
            log("img pack warn", img.name, e)
    for m in bpy.data.materials: m.use_fake_user = True
    return arm

def zero_pose(arm):
    for pb in arm.pose.bones:
        pb.rotation_mode = 'XYZ'
        pb.rotation_euler = (0.0, 0.0, 0.0)
        pb.location = (0.0, 0.0, 0.0)
        pb.scale = (1.0, 1.0, 1.0)

def new_action(arm, name):
    if not arm.animation_data: arm.animation_data_create()
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

# ---------------------------------------------------------------------------
# STRUGGLE (~1.6 s @24fps = 38 frames, seamless loop): the loom. A held base
# pose (spine over, arms down) + sin-cycle working motion on top. Suggestion,
# not depiction: the whole read is posture + weight, at tableau distance.
# ---------------------------------------------------------------------------
def bake_struggle_biped(arm, g, frames=38):
    act = new_action(arm, "Struggle")
    zero_pose(arm)
    for i in range(frames):
        f = i + 1
        t = i / float(frames)           # 0..<1 so sin(2*pi*t) loops seamlessly
        s1 = math.sin(2.0 * math.pi * t)
        s2 = math.sin(4.0 * math.pi * t + 0.7)
        # BASE loom: spine pitched hard over, head down, hips dropped.
        spineRx = D(34) + D(5) * s1
        headRx  = D(22) + D(4) * s2
        hipsZ   = -0.16 + 0.025 * s1
        hipsRy  = D(6) * s1
        # ARMS working downward, asymmetric phase (the struggle read).
        armRx_R = -D(72) + D(14) * s1
        armRx_L = -D(66) - D(14) * s1
        foreRx  = -D(30) + D(10) * s2
        if g["spine"]:
            key_euler(arm, g["spine"][0], f, rx=spineRx, ry=hipsRy)
            if len(g["spine"]) > 1:
                key_euler(arm, g["spine"][1], f, rx=spineRx * 0.5)
        key_euler(arm, g.get("head"), f, rx=headRx)
        key_euler(arm, g.get("armR"), f, rx=armRx_R, rz=D(12) * s2)
        key_euler(arm, g.get("armL"), f, rx=armRx_L, rz=-D(12) * s2)
        key_euler(arm, g.get("foreR"), f, rx=foreRx)
        key_euler(arm, g.get("foreL"), f, rx=foreRx * 0.8)
        key_loc(arm, g.get("hips"), f, z=hipsZ)
    return act

def bake_struggle_core(arm, g, frames=38):
    act = new_action(arm, "Struggle")
    zero_pose(arm)
    drv = g.get("root") or g.get("hips") or (arm.pose.bones[0].name if len(arm.pose.bones) else None)
    if not drv: return act
    for i in range(frames):
        f = i + 1
        t = i / float(frames)
        s1 = math.sin(2.0 * math.pi * t)
        s2 = math.sin(4.0 * math.pi * t + 0.9)
        # Rear-up + press-down pulse: pitched over the spot, working downward.
        key_euler(arm, drv, f, rx=D(18) + D(9) * s1, ry=D(5) * s2)
        key_loc(arm, drv, f, z=-0.10 + 0.05 * s1)
    return act

# ---------------------------------------------------------------------------
def main():
    arm = import_target()
    before = [a.name for a in bpy.data.actions]
    g = classify(arm.data.bones)
    style = STYLE if STYLE != "auto" else pick_style(g)
    log("target:", os.path.basename(TARGET), "bones:", len(arm.data.bones), "style:", style)
    log("existing actions (kept):", before)

    if style == "biped": bake_struggle_biped(arm, g)
    else:                bake_struggle_core(arm, g)
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
