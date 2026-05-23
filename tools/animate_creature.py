"""
animate_creature.py — headless Blender PROCEDURAL locomotion baker for X3Native.

    blender-launcher.exe --background --python animate_creature.py -- \
        <target.glb> <out.glb> [gait]

Bakes three looping clips — **Idle / Walk / Run** — onto a character's OWN
armature with NO external mocap, then exports one multi-clip `<name>_anim.glb`.
The X3Native runtime (MonsterSystem / Skinner) auto-prefers `<name>_anim.glb` and
drives the Idle/Walk/Run blend from AI speed, so the creature comes alive.

Why procedural instead of retarget_anims.py: the bestiary "animals"
(alien_crawler / sea_*) are NOT humanoid-rigged — most carry a single `Root`
bone, so the StarterAssets humanoid Walk/Run can't map. This tool adapts to
whatever bones exist:

  * gait="biped"  — a real leg-swing + arm-counterswing + hip-bob walk cycle
                    (needs UpperLeg/LowerLeg .L/.R, e.g. sea_hammerhead or any
                    future properly-rigged enemy).
  * gait="core"   — single-bone / unrigged creatures: bob + yaw-waggle + a
                    forward lean while moving (the Verthani scuttle).
  * gait="swim"   — undulate the spine/tail chain + bob (fish / manta / shark).
  gait auto-detects from the rig if omitted.

ENV NOTE (this box): Blender is the Microsoft Store package — direct blender.exe
is ACL-denied and the `blender-launcher.exe` alias DETACHES, so we report through
files: a sidecar `<out>.log` and a `<out>.done` marker the caller polls. Mirror
this in every X3Native Blender tool.

Clean-room: built from the public Blender Python API + glTF 2.0 spec only.
"""
import bpy, sys, os, math

ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(ARGV) < 2:
    raise SystemExit("Usage: ... -- <target.glb> <out.glb> [gait]")
TARGET, OUT = ARGV[0], ARGV[1]
GAIT = ARGV[2].lower() if len(ARGV) > 2 else "auto"

LOG_PATH  = OUT + ".log"
DONE_PATH = OUT + ".done"
_log = []
def log(*a):
    s = "[animate] " + " ".join(str(x) for x in a)
    _log.append(s); print(s)

def flush_log(status):
    try:
        with open(LOG_PATH, "w", encoding="utf-8") as f:
            f.write("\n".join(_log))
        with open(DONE_PATH, "w", encoding="utf-8") as f:
            f.write(status)
    except Exception as e:
        print("[animate] could not write log/marker:", e)

TAU = math.tau

# ---------------------------------------------------------------------------
# Bone classification (case-insensitive name match) so the baker adapts to rigs.
# ---------------------------------------------------------------------------
def side(name):
    n = name.lower()
    if n.endswith(".l") or "left" in n or "_l_" in n:  return "L"
    if n.endswith(".r") or "right" in n or "_r_" in n: return "R"
    return ""

def has(name, *keys):
    n = name.lower()
    return any(k in n for k in keys)

def classify(bones):
    """Return dicts of named pose bones we know how to drive."""
    g = {"upperleg": {}, "lowerleg": {}, "foot": {},
         "upperarm": {}, "lowerarm": {}, "shoulder": {},
         "spine": [], "root": None, "hips": None, "head": None}
    for b in bones:
        nm = b.name; s = side(nm)
        if   has(nm, "upperleg", "thigh"):              g["upperleg"][s or "C"] = nm
        elif has(nm, "lowerleg", "shin", "calf", "knee"): g["lowerleg"][s or "C"] = nm
        elif has(nm, "foot", "ankle"):                  g["foot"][s or "C"] = nm
        elif has(nm, "upperarm") or (has(nm, "arm") and not has(nm, "lower", "fore")):
            g["upperarm"][s or "C"] = nm
        elif has(nm, "lowerarm", "forearm"):            g["lowerarm"][s or "C"] = nm
        elif has(nm, "shoulder", "clavicle"):           g["shoulder"][s or "C"] = nm
        elif has(nm, "hips", "pelvis"):                 g["hips"] = nm
        elif has(nm, "head"):                           g["head"] = nm
        elif has(nm, "spine", "chest", "neck", "tail", "abdomen", "thorax", "back"):
            g["spine"].append(nm)
        elif has(nm, "root"):                           g["root"] = nm
    return g

def pick_gait(g):
    if g["upperleg"]:                       return "biped"
    if len(g["spine"]) >= 2:                return "swim"
    return "core"

# ---------------------------------------------------------------------------
def reset_factory():
    bpy.ops.wm.read_factory_settings(use_empty=True)

def import_target():
    bpy.ops.import_scene.gltf(filepath=TARGET)
    arm = next((o for o in bpy.data.objects if o.type == 'ARMATURE'), None)
    if not arm:
        raise RuntimeError("no armature in " + TARGET)
    arm.name = "Arm"
    arm.animation_data_clear()
    for a in list(bpy.data.actions):
        bpy.data.actions.remove(a)
    # Lock textures into the file so the exported GLB keeps its materials.
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
    # Blender 5.0 slotted-actions forward-compat (no-op on 4.5 LTS).
    try:
        if getattr(act, "slots", None) is not None and len(act.slots) == 0:
            act.slots.new(id_type='OBJECT', name=arm.name)
            arm.animation_data.action_slot = act.slots[0]
    except Exception:
        pass
    return act

def key_euler(arm, bone, frame, rx=0.0, ry=0.0, rz=0.0):
    pb = arm.pose.bones.get(bone)
    if not pb: return
    pb.rotation_euler = (rx, ry, rz)
    pb.keyframe_insert("rotation_euler", frame=frame)

def key_loc(arm, bone, frame, x=0.0, y=0.0, z=0.0):
    pb = arm.pose.bones.get(bone)
    if not pb: return
    pb.location = (x, y, z)
    pb.keyframe_insert("location", frame=frame)

D = math.radians

# ---------------------------------------------------------------------------
# Clip bakers. Each writes a looping action over `frames` keys. Amplitude `amp`
# scales the whole motion (idle small, walk 1.0, run ~1.6).
# ---------------------------------------------------------------------------
def bake_biped(arm, g, name, frames, amp, hz=1.0):
    act = new_action(arm, name)
    zero_pose(arm)
    thigh = D(28) * amp; knee = D(36) * amp; armsw = D(22) * amp
    bob = 0.04 * amp
    for i in range(frames):
        f = i + 1
        p = (i / frames) * hz * TAU
        # Legs swing in opposition; knees bend on the back-swing.
        for sgn, sd in ((+1, "L"), (-1, "R")):
            ph = p if sd == "L" else p + math.pi
            if g["upperleg"].get(sd): key_euler(arm, g["upperleg"][sd], f, rx=math.sin(ph)*thigh)
            if g["lowerleg"].get(sd): key_euler(arm, g["lowerleg"][sd], f, rx=max(0.0, -math.sin(ph))*knee)
            # Arms counter-swing the legs.
            if g["upperarm"].get(sd): key_euler(arm, g["upperarm"][sd], f, rx=-math.sin(ph)*armsw)
        # Hip bob (twice per stride) + gentle roll sway.
        if g["hips"]:
            key_loc(arm, g["hips"], f, z=abs(math.sin(p)) * bob)
            key_euler(arm, g["hips"], f, ry=math.sin(p) * D(5) * amp)
        if g["spine"]:
            key_euler(arm, g["spine"][0], f, ry=-math.sin(p) * D(4) * amp)
    return act

def bake_swim(arm, g, name, frames, amp, hz=1.0):
    act = new_action(arm, name)
    zero_pose(arm)
    chain = g["spine"][:] or ([g["root"]] if g["root"] else [])
    if g["head"]: chain.append(g["head"])
    sway = D(14) * amp
    for i in range(frames):
        f = i + 1
        p = (i / frames) * hz * TAU
        # Travelling wave down the chain (each bone lags the previous).
        for k, bn in enumerate(chain):
            key_euler(arm, bn, f, rz=math.sin(p - k * 0.7) * sway)
        # Pectoral "flap" if arms exist.
        for sgn, sd in ((+1, "L"), (-1, "R")):
            if g["upperarm"].get(sd):
                key_euler(arm, g["upperarm"][sd], f, ry=sgn * (0.4 + 0.6 * math.sin(p)) * D(18) * amp)
        if g["root"]:
            key_loc(arm, g["root"], f, z=math.sin(p * 2) * 0.03 * amp)
    return act

def bake_core(arm, g, name, frames, amp, hz=1.0):
    """Single-bone / unrigged: bob + yaw-waggle + forward lean while moving."""
    act = new_action(arm, name)
    zero_pose(arm)
    drv = g["root"] or g["hips"] or (arm.pose.bones[0].name if len(arm.pose.bones) else None)
    if not drv:
        return act
    waggle = D(9) * amp; lean = D(7) * (amp - 0.2 if amp > 0.2 else 0.0)
    for i in range(frames):
        f = i + 1
        p = (i / frames) * hz * TAU
        key_euler(arm, drv, f,
                  rx=lean + math.sin(p * 2) * D(2) * amp,   # forward lean + small pulse
                  rz=math.sin(p) * waggle)                  # side-to-side scuttle
        key_loc(arm, drv, f, z=abs(math.sin(p)) * 0.035 * amp)
    return act

def bake(arm, g, gait, name, frames, amp, hz):
    if gait == "biped": return bake_biped(arm, g, name, frames, amp, hz)
    if gait == "swim":  return bake_swim(arm, g, name, frames, amp, hz)
    return bake_core(arm, g, name, frames, amp, hz)

# ---------------------------------------------------------------------------
def main():
    reset_factory()
    arm = import_target()
    g = classify(arm.data.bones)
    gait = GAIT if GAIT != "auto" else pick_gait(g)
    log("target:", os.path.basename(TARGET), "bones:", len(arm.data.bones),
        "gait:", gait, "legs:", list(g["upperleg"].values()), "spine:", len(g["spine"]),
        "root:", g["root"])

    # Idle (subtle, slow), Walk (1.0), Run (bigger + faster cycle).
    bake(arm, g, gait if gait != "biped" else "core", "Idle", 60, 0.18, 1.0)
    bake(arm, g, gait, "Walk", 28, 1.0, 1.0)
    bake(arm, g, gait, "Run",  20, 1.6, 1.0)
    arm.animation_data.action = None

    names = [a.name for a in bpy.data.actions]
    log("baked actions:", names)

    # Re-pack textures defensively, then export ONE multi-clip GLB.
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
