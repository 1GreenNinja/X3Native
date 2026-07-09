"""
dance_bake.py — headless Blender CLUB DANCE loop baker for X3Native (club max-out).

    blender-launcher.exe --background --python dance_bake.py -- <in.glb> <out.glb>

Sibling of struggle_bake.py (same append protocol): imports a rigged GLB, KEEPS
every existing action (Idle...), bakes TWO extra seamless dance loops, and
re-exports one GLB carrying all clips. MonsterSystem::setCalmLoop plays them on
the Club 1127 dancers (inert idle props — the calm loop replaces Idle).

  * "DanceGroove" — 48f @24fps (~2.0 s): the club two-step. Hips bounce on a
      double-time pulse, hip sway with spine/chest counter-twist, head bob +
      tilt, arms pumping bent at the sides, subtle weight-shift in the legs.
  * "DanceArms"   — 56f (~2.33 s): hands-in-the-air. Both arms raised and
      swaying overhead with the hips rolling a figure-eight underneath.

All curves are sin(2*pi*k*t) terms so frame N-1 flows into frame 0 — seamless
by construction. The Anna rigs use the compact 20-bone family (Hips/Spine/
Chest/Neck/Head, {Shoulder,UpperArm,LowerArm,Hand,UpperLeg,LowerLeg,Foot}.L/R).
AnnaBodySuit ships NODE-animated (no skin/armature) — the baker detects that
and keyframes the named OBJECT hierarchy instead of pose bones.

ENV NOTE (this box): Store-Blender — blender.exe ACL-denied, blender-launcher
DETACHES; we report through `<out>.log` + `<out>.done` (poll them).

Clean-room: public Blender Python API + glTF 2.0 spec only.
"""
import bpy, sys, os, math

ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(ARGV) < 2:
    raise SystemExit("Usage: ... -- <in.glb> <out.glb>")
TARGET, OUT = ARGV[0], ARGV[1]

LOG_PATH, DONE_PATH = OUT + ".log", OUT + ".done"
_log = []
def log(*a):
    s = "[dancebake] " + " ".join(str(x) for x in a)
    _log.append(s); print(s)

def flush_log(status):
    try:
        with open(LOG_PATH, "w", encoding="utf-8") as f: f.write("\n".join(_log))
        with open(DONE_PATH, "w", encoding="utf-8") as f: f.write(status)
    except Exception as e:
        print("[dancebake] could not write log/marker:", e)

D = math.radians

# ---------------------------------------------------------------------------
# Rig access — ARMATURE pose bones when present, named OBJECTS otherwise
# (AnnaBodySuit is node-animated). One driver interface either way.
# ---------------------------------------------------------------------------
class Driver:
    def __init__(self):
        self.arm = next((o for o in bpy.data.objects if o.type == 'ARMATURE'), None)
        self.bones = {}
        names = ([b.name for b in self.arm.pose.bones] if self.arm
                 else [o.name for o in bpy.data.objects])
        def find(*keys, side=None):
            for n in names:
                ln = n.lower()
                if side and not ln.endswith("." + side.lower()): continue
                if side is None and (ln.endswith(".l") or ln.endswith(".r")): continue
                if all(k in ln for k in keys): return n
            return None
        g = self.bones
        g["hips"]  = find("hips") or find("pelvis")
        g["spine"] = find("spine")
        g["chest"] = find("chest")
        g["neck"]  = find("neck")
        g["head"]  = find("head")
        for s in ("L", "R"):
            g["upArm" + s]  = find("upperarm", side=s) or find("arm", side=s)
            g["loArm" + s]  = find("lowerarm", side=s) or find("forearm", side=s)
            g["hand" + s]   = find("hand", side=s)
            g["upLeg" + s]  = find("upperleg", side=s) or find("upleg", side=s)
            g["shoulder" + s] = find("shoulder", side=s)
        log("rig:", "armature" if self.arm else "node-hierarchy",
            "| resolved:", {k: v for k, v in g.items() if v})

    def _pb(self, name):
        if not name: return None
        if self.arm: return self.arm.pose.bones.get(name)
        return bpy.data.objects.get(name)

    def ensure_action(self, name):
        holders = [self.arm] if self.arm else \
                  [bpy.data.objects[v] for v in set(self.bones.values()) if v and v in bpy.data.objects]
        for h in holders:
            if not h.animation_data: h.animation_data_create()
        if self.arm:
            act = bpy.data.actions.new(name)
            act.use_fake_user = True
            self.arm.animation_data.action = act
            try:
                if getattr(act, "slots", None) is not None and len(act.slots) == 0:
                    act.slots.new(id_type='OBJECT', name=self.arm.name)
                    self.arm.animation_data.action_slot = act.slots[0]
            except Exception:
                pass
            self._acts = [act]
        else:
            # Node rig: one action per animated object, prefixed so the glTF
            # exporter merges them into ONE animation named `name` (exporter
            # groups by action name when identical).
            self._acts = []
            for h in holders:
                act = bpy.data.actions.new(name)
                act.use_fake_user = True
                h.animation_data.action = act
                self._acts.append(act)

    def key_rot(self, key, frame, rx=0.0, ry=0.0, rz=0.0):
        t = self._pb(self.bones.get(key))
        if not t: return
        t.rotation_mode = 'XYZ'
        t.rotation_euler = (rx, ry, rz)
        t.keyframe_insert("rotation_euler", frame=frame)

    def key_loc_add(self, key, frame, dx=0.0, dy=0.0, dz=0.0):
        t = self._pb(self.bones.get(key))
        if not t: return
        if self.arm:
            t.location = (dx, dy, dz)
        else:
            base = t.get("_x3b")   # ID-property cache (Blender Objects reject
            if base is None:       # arbitrary python attributes)
                base = tuple(t.location); t["_x3b"] = base
            t.location = (base[0] + dx, base[1] + dy, base[2] + dz)
        t.keyframe_insert("location", frame=frame)

    def zero(self):
        for v in set(self.bones.values()):
            t = self._pb(v)
            if not t: continue
            t.rotation_mode = 'XYZ'
            t.rotation_euler = (0.0, 0.0, 0.0)
            if self.arm: t.location = (0.0, 0.0, 0.0)


def import_target():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=TARGET)
    for a in bpy.data.actions: a.use_fake_user = True
    for img in bpy.data.images:
        try:
            if not img.has_data: _ = img.pixels[0]
            if not img.packed_file: img.pack()
            img.use_fake_user = True
        except Exception as e:
            log("img pack warn", img.name, e)
    for m in bpy.data.materials: m.use_fake_user = True


# ---------------------------------------------------------------------------
# DANCE 1 — "DanceGroove": the club two-step (48f seamless).
# ---------------------------------------------------------------------------
def bake_groove(drv, frames=48):
    drv.ensure_action("DanceGroove")
    drv.zero()
    for i in range(frames):
        f = i + 1
        t = i / float(frames)
        s1 = math.sin(2 * math.pi * t)              # full-cycle sway
        c1 = math.cos(2 * math.pi * t)
        s2 = math.sin(4 * math.pi * t)              # double-time bounce (the beat)
        s3 = math.sin(4 * math.pi * t + 1.2)
        drv.key_loc_add("hips", f, dz=-0.05 + 0.045 * s2)         # bounce
        drv.key_rot("hips",  f, ry=D(10) * s1, rz=D(4) * s1)      # hip sway
        drv.key_rot("spine", f, ry=-D(7) * s1, rx=D(3) * s2)      # counter-twist
        drv.key_rot("chest", f, ry=-D(6) * s1, rz=-D(3) * s1)
        drv.key_rot("head",  f, rx=D(6) * s2, rz=D(4) * s1)       # bob + tilt
        # Arms pumping bent at the sides, asymmetric phase.
        drv.key_rot("upArmL", f, rx=-D(28) - D(16) * s2, rz=-D(10) - D(6) * s1)
        drv.key_rot("upArmR", f, rx=-D(28) + D(16) * s2, rz=D(10) + D(6) * s1)
        drv.key_rot("loArmL", f, rx=-D(62) - D(14) * s3)
        drv.key_rot("loArmR", f, rx=-D(62) + D(14) * s3)
        drv.key_rot("handL", f, rz=-D(8) * s2)
        drv.key_rot("handR", f, rz=D(8) * s2)
        # Weight shift: subtle alternating leg pitch (feet stay planted).
        drv.key_rot("upLegL", f, rx=D(6) * s2)
        drv.key_rot("upLegR", f, rx=-D(6) * s2)


# ---------------------------------------------------------------------------
# DANCE 2 — "DanceArms": hands in the air (56f seamless).
# ---------------------------------------------------------------------------
def bake_arms(drv, frames=56):
    drv.ensure_action("DanceArms")
    drv.zero()
    for i in range(frames):
        f = i + 1
        t = i / float(frames)
        s1 = math.sin(2 * math.pi * t)
        s2 = math.sin(4 * math.pi * t + 0.6)
        c1 = math.cos(2 * math.pi * t)
        drv.key_loc_add("hips", f, dz=-0.04 + 0.035 * s2)
        drv.key_rot("hips",  f, ry=D(12) * s1, rx=D(3) * c1)      # figure-eight roll
        drv.key_rot("spine", f, ry=-D(8) * s1)
        drv.key_rot("chest", f, ry=-D(5) * s1, rx=-D(4))
        drv.key_rot("head",  f, rx=-D(8) + D(5) * s2)             # chin up, bobbing
        # Arms RAISED overhead, swaying side to side together.
        drv.key_rot("shoulderL", f, rz=-D(20))
        drv.key_rot("shoulderR", f, rz=D(20))
        drv.key_rot("upArmL", f, rx=-D(55), rz=-D(115) - D(12) * s1)
        drv.key_rot("upArmR", f, rx=-D(55), rz=D(115) - D(12) * s1)
        drv.key_rot("loArmL", f, rx=-D(20) - D(10) * s2, rz=-D(10) * s1)
        drv.key_rot("loArmR", f, rx=-D(20) + D(10) * s2, rz=-D(10) * s1)
        drv.key_rot("handL", f, rz=-D(14) * s1)
        drv.key_rot("handR", f, rz=-D(14) * s1)
        drv.key_rot("upLegL", f, rx=D(4) * s2)
        drv.key_rot("upLegR", f, rx=-D(4) * s2)


def main():
    import_target()
    before = [a.name for a in bpy.data.actions]
    drv = Driver()
    log("existing actions (kept):", before)
    bake_groove(drv)
    bake_arms(drv)
    if drv.arm and drv.arm.animation_data:
        drv.arm.animation_data.action = None
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
