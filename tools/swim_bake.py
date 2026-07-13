"""
swim_bake.py — headless Blender SWIM loop baker for X3Native (Jake swims).

    blender-launcher.exe --background --python swim_bake.py -- <in.glb> <out.glb>

Sibling of dance_bake.py / struggle_bake.py (same append protocol): imports a
rigged GLB, KEEPS every existing action (Jake's 22), bakes TWO extra seamless
swim loops, and re-exports ONE GLB carrying all clips.

  * "Swim"     — 56f @24fps (~2.33 s): a BREASTSTROKE. From the streamlined
      glide the arms sweep OUT and wide with a high elbow (the catch), the
      forearms press down and the hands scull IN under the chest (the pull),
      squeeze together at the chin, then shoot forward past the head (recovery).
      The frog KICK is offset ~half a cycle: the knees draw up + spread while
      the hands are under the chest, and the whip fires THROUGH the arm recovery
      (propulsion when the body is streamlined — the real stroke's timing). The
      head lifts on the pull (the breath) and settles on the glide; the hips
      undulate up on the kick. A GLIDE HOLD (~25% of the cycle) keeps it from
      reading as frantic paddling.
  * "SwimIdle" — 48f (~2.0 s): TREAD WATER. Sculling arm circles at the sides,
      a slow alternating leg flutter, quiet body sway.

WHY IT READS RIGHT WHEN THE ENGINE PITCHES HIM PRONE: ThirdPersonView lays the
avatar down (basis pitched about the local right axis, belly-down), so in the
CLIP's own upright frame:
      +UP  (head-ward)  ->  the swim direction (world forward)
      +FWD (belly/nose) ->  world DOWN (into the water)
      -FWD (his back)   ->  world UP (the surface he breaks)
So "arms overhead" = arms extended past the head through the water; "forearm
pressing toward +FWD" = the catch pressing DOWN on the water; "knee flexion
folding the heel toward -FWD" = heels lifting OUT of the water; "face toward
+UP" = the swimmer looking forward for the breath. Everything below is authored
in that character frame.

REST-POSE-AGNOSTIC: Jake's rest pose is NOT a T-pose (the arms hang down), so
nothing here is a bone-local euler. The character's UP / LEFT / FWD are DERIVED
from the rest skeleton (hips->head, right hand->left hand, ankle->toe), each
limb segment's REST DIRECTION is read from its bone matrix, and every key is an
AIM: "point this segment at THIS direction in character coords". The pose
channel is then
      basis = L^-1 * (Rparent^-1 * A) * L,   A = rot(rest_dir -> target_dir)
with L = bone.matrix_local (rest, armature space) and Rparent the accumulated
absolute rotation of the ancestors — so a segment lands where it is aimed
regardless of the rig's rest pose. Torso/head keys stay plain rotations.

SEAMLESS BY CONSTRUCTION: every channel is a PERIODIC key table interpolated
with smoothstep (zero slope at every key) and sampled at EVERY frame, so value
AND derivative are continuous across the wrap: frame N-1 flows into frame 0.
Deterministic (no rng).

ENV NOTE (this box): Store-Blender — blender.exe is ACL-denied, blender-launcher
DETACHES; we report through `<out>.log` + `<out>.done` (poll them).

Clean-room: public Blender Python API + glTF 2.0 spec only.
"""
import bpy, sys, os, math
from mathutils import Matrix, Vector

ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(ARGV) < 2:
    raise SystemExit("Usage: ... -- <in.glb> <out.glb>")
TARGET, OUT = ARGV[0], ARGV[1]

LOG_PATH, DONE_PATH = OUT + ".log", OUT + ".done"
_log = []
def log(*a):
    s = "[swimbake] " + " ".join(str(x) for x in a)
    _log.append(s); print(s)

def flush_log(status):
    try:
        with open(LOG_PATH, "w", encoding="utf-8") as f: f.write("\n".join(_log))
        with open(DONE_PATH, "w", encoding="utf-8") as f: f.write(status)
    except Exception as e:
        print("[swimbake] could not write log/marker:", e)

D = math.radians


# ---------------------------------------------------------------------------
# Periodic smoothstep interpolation over a key table (the "animator's keys").
#   pwf: scalar keys  [(t, v), ...]
#   pwv: vector keys  [(t, (f,l,u)), ...]  -> normalized direction (nlerp)
# Zero slope at every key => C1 across the t=1 -> t=0 seam.
# ---------------------------------------------------------------------------
def _seg(t, keys):
    t = t % 1.0
    n = len(keys)
    for i in range(n):
        t0 = keys[i][0]
        t1 = keys[(i + 1) % n][0]
        span = (t1 - t0) % 1.0
        if span <= 1e-9:
            continue
        u = (t - t0) % 1.0
        if u < span - 1e-9 or i == n - 1:
            x = min(u / span, 1.0)
            return i, (i + 1) % n, x * x * (3.0 - 2.0 * x)
    return 0, 0, 0.0

def pwf(t, keys):
    a, b, s = _seg(t, keys)
    return keys[a][1] + (keys[b][1] - keys[a][1]) * s

def pwv(t, keys):
    a, b, s = _seg(t, keys)
    va, vb = Vector(keys[a][1]), Vector(keys[b][1])
    v = va.lerp(vb, s)
    return v.normalized() if v.length > 1e-6 else va.normalized()


def rot_from_to(a, b):
    """Minimal rotation carrying unit dir a onto unit dir b (3x3)."""
    a = a.normalized(); b = b.normalized()
    d = max(-1.0, min(1.0, a.dot(b)))
    if d > 0.999999:
        return Matrix.Identity(3)
    if d < -0.999999:                      # anti-parallel: any perpendicular axis
        ax = a.cross(Vector((1, 0, 0)))
        if ax.length < 1e-3: ax = a.cross(Vector((0, 1, 0)))
        return Matrix.Rotation(math.pi, 3, ax.normalized())
    return Matrix.Rotation(math.acos(d), 3, a.cross(b).normalized())


SLOTS = {
    "hips": ["hips", "pelvis"],
    "spine": ["spine"], "spine1": ["spine1", "chest"], "spine2": ["spine2", "upperchest"],
    "neck": ["neck"], "head": ["head"],
    "shoulderL": ["leftshoulder"], "shoulderR": ["rightshoulder"],
    "armL": ["leftarm"],      "armR": ["rightarm"],
    "foreL": ["leftforearm"], "foreR": ["rightforearm"],
    "handL": ["lefthand"],    "handR": ["righthand"],
    "upLegL": ["leftupleg", "leftupperleg"], "upLegR": ["rightupleg", "rightupperleg"],
    "legL": ["leftleg", "leftlowerleg"],     "legR": ["rightleg", "rightlowerleg"],
    "footL": ["leftfoot"],    "footR": ["rightfoot"],
}
# parent slot of every driven bone (for the absolute-rotation chain)
PARENT = {
    "hips": None, "spine": "hips", "spine1": "spine", "spine2": "spine1",
    "neck": "spine2", "head": "neck",
    "shoulderL": "spine2", "armL": "shoulderL", "foreL": "armL", "handL": "foreL",
    "shoulderR": "spine2", "armR": "shoulderR", "foreR": "armR", "handR": "foreR",
    "upLegL": "hips", "legL": "upLegL", "footL": "legL",
    "upLegR": "hips", "legR": "upLegR", "footR": "legR",
}

def norm(n):
    s = "".join(c for c in n.lower() if c.isalnum())
    for p in ("mixamorig1", "mixamorig2", "mixamorig"):
        if s.startswith(p):
            return s[len(p):]
    return s

def fmt(v):
    return "(%+.2f,%+.2f,%+.2f)" % (v.x, v.y, v.z)


class Rig:
    """Bone map + character frame + the aim/rotate keying primitives."""
    def __init__(self):
        self.arm = next((o for o in bpy.data.objects if o.type == 'ARMATURE'), None)
        if not self.arm:
            raise RuntimeError("no armature in " + TARGET)
        names = {norm(b.name): b.name for b in self.arm.pose.bones}
        self.b = {}
        for slot, keys in SLOTS.items():
            self.b[slot] = next((names[k] for k in keys if k in names), None)
        miss = [k for k, v in self.b.items() if not v]
        log("bones:", len(self.arm.pose.bones), "| unresolved:", miss or "none")

        bones = self.arm.data.bones
        H = lambda s: Vector(bones[self.b[s]].head_local)
        hips, hd = H("hips"), H("head")
        lh, rh = H("handL"), H("handR")
        # The frame MUST be symmetric: derive UP + LEFT from the (symmetric)
        # spine/hand pairs and take FORWARD as their cross product — do NOT take
        # forward from one foot's toe. Jake's bind pose splays the feet ~20 deg,
        # so a single ankle->toe vector yaws the whole authoring frame and the
        # stroke comes out twisted (caught by the L/R asymmetry in the FK check).
        # The toes are used only to pick the SIGN of forward.
        self.UP = (hd - hips).normalized()
        lat = (lh - rh).normalized()
        self.LAT = (lat - self.UP * lat.dot(self.UP)).normalized()       # his LEFT
        fwd = self.LAT.cross(self.UP).normalized()
        toeSum = Vector((0, 0, 0))
        for side in ("left", "right"):
            tn = next((n for n in names if "toe" in n and n.startswith(side)), None)
            fn = next((n for n in names if n == side + "foot"), None)
            if tn and fn:
                toeSum += (Vector(bones[names[tn]].head_local) -
                           Vector(bones[names[fn]].head_local))
        if toeSum.dot(fwd) < 0.0: fwd = -fwd
        self.FWD = fwd                                                   # his FACING (belly)
        self.height = (hd - hips).length
        self.unit = 1.0 if self.height < 3.0 else 100.0
        log("frame: UP=%s LEFT=%s FWD=%s hips->head=%.3f" %
            (fmt(self.UP), fmt(self.LAT), fmt(self.FWD), self.height))

        # rest rotation + rest SEGMENT DIRECTION (char coords) per slot
        self.L, self.rest = {}, {}
        for slot, n in self.b.items():
            if not n: continue
            L = bones[n].matrix_local.to_3x3().normalized()
            self.L[slot] = L
            d = (L @ Vector((0, 1, 0))).normalized()      # bone +Y = down the bone
            self.rest[slot] = d
        log("rest dirs (F,L,U): " + " ".join(
            "%s=%s" % (s, self.charstr(self.rest[s]))
            for s in ("armL", "foreL", "handL", "upLegL", "legL", "footL", "shoulderL")
            if s in self.rest))
        self.absR = {}

    # ---- char-coord helpers -------------------------------------------------
    def vec(self, f, l, u):
        """A direction given in CHARACTER coords (fwd/left/up) -> armature space."""
        v = self.FWD * f + self.LAT * l + self.UP * u
        return v.normalized()

    def charstr(self, v):
        return "(F%+.2f L%+.2f U%+.2f)" % (v.dot(self.FWD), v.dot(self.LAT), v.dot(self.UP))

    # ---- keying -------------------------------------------------------------
    def _write(self, slot, frame, Rown, Rabs):
        n = self.b.get(slot)
        self.absR[slot] = Rabs
        if not n: return
        pb = self.arm.pose.bones[n]
        L = self.L[slot]
        pb.rotation_mode = 'QUATERNION'
        pb.rotation_quaternion = (L.inverted() @ Rown @ L).to_quaternion()
        pb.keyframe_insert("rotation_quaternion", frame=frame)

    def parentAbs(self, slot):
        p = PARENT.get(slot)
        while p and p not in self.absR:
            p = PARENT.get(p)
        return self.absR.get(p, Matrix.Identity(3))

    def rot(self, slot, frame, R):
        """Rotate a bone by R (armature space), on top of its parents' rotation."""
        if slot not in self.L: return
        Rp = self.parentAbs(slot)
        self._write(slot, frame, R, Rp @ R)

    def aim(self, slot, frame, target):
        """Point the segment at `target` (armature-space dir), ABSOLUTELY —
        cancelling the parents' accumulated rotation so the aim lands."""
        if slot not in self.L: return
        A = rot_from_to(self.rest[slot], target)      # absolute rotation of this bone
        Rp = self.parentAbs(slot)
        self._write(slot, frame, Rp.transposed() @ A, A)

    def loc(self, slot, frame, d_char):
        n = self.b.get(slot)
        if not n: return
        pb = self.arm.pose.bones[n]
        pb.location = self.L[slot].inverted() @ d_char
        pb.keyframe_insert("location", frame=frame)

    def toward(self, a, b, ang):
        ax = a.cross(b)
        if ax.length < 1e-6: return Matrix.Identity(3)
        return Matrix.Rotation(ang, 3, ax.normalized())

    def new_action(self, name):
        if not self.arm.animation_data: self.arm.animation_data_create()
        act = bpy.data.actions.new(name)
        act.use_fake_user = True
        self.arm.animation_data.action = act
        try:
            if getattr(act, "slots", None) is not None and len(act.slots) == 0:
                act.slots.new(id_type='OBJECT', name=self.arm.name)
                self.arm.animation_data.action_slot = act.slots[0]
        except Exception:
            pass
        for pb in self.arm.pose.bones:                # clear residue -> rest
            pb.rotation_mode = 'QUATERNION'
            pb.rotation_quaternion = (1, 0, 0, 0)
            pb.location = (0, 0, 0)
        self.absR = {}
        return act

    # ---- FK self-check ------------------------------------------------------
    def probe(self, frame, slots):
        bpy.context.scene.frame_set(frame)
        bpy.context.view_layer.update()
        hips = self.arm.pose.bones[self.b["hips"]].matrix.translation
        out = []
        for s in slots:
            n = self.b.get(s)
            if not n: continue
            p = self.arm.pose.bones[n].matrix.translation - hips
            out.append("%s%s" % (s, self.charstr(p)))
        return " ".join(out)


# ===========================================================================
# THE BREASTSTROKE — key tables. Directions are CHARACTER coords (F,L,U):
#   +U = past the head (the swim direction) | +F = belly side (DOWN in water)
#   +L = his left (mirrored for the right arm/leg)
# Times: 0.00 glide -> 0.30 catch -> 0.48 pull -> 0.58 squeeze -> 0.72 shoot
#        -> 0.86 glide.  Legs run the SAME clock but their motion lives in
#        0.48..0.86 (draw -> whip) — the kick fires as the arms recover.
# ===========================================================================
K_ARM = [   # upper arm (shoulder -> elbow)
    (0.00, (0.06, 0.20, 0.98)),   # glide: extended past the head, barely spread
    (0.14, (0.08, 0.24, 0.97)),   # streamline hold
    (0.30, (0.30, 0.78, 0.55)),   # CATCH: swept wide, high elbow, still forward
    (0.48, (0.62, 0.62, -0.48)),  # PULL: elbow driving down + back along the ribs
    (0.58, (0.55, 0.30, -0.78)),  # SQUEEZE: elbows tucked in at the chest
    (0.72, (0.30, 0.28, 0.91)),   # SHOOT: arms firing forward past the head
    (0.86, (0.06, 0.20, 0.98)),   # glide
]
K_FORE = [  # forearm (elbow -> wrist)
    (0.00, (0.06, 0.12, 0.99)),
    (0.14, (0.08, 0.16, 0.98)),
    (0.30, (0.74, 0.56, -0.36)),  # CATCH: forearm pressing DOWN into the water
    (0.48, (0.66, -0.30, 0.69)),  # PULL: sculling IN + up under the chest
    (0.58, (0.34, -0.55, 0.76)),  # SQUEEZE: hands together under the chin
    (0.72, (0.22, 0.10, 0.97)),   # SHOOT
    (0.86, (0.06, 0.12, 0.99)),
]
K_HAND = [
    (0.00, (0.06, 0.10, 0.99)),
    (0.14, (0.08, 0.12, 0.99)),
    (0.30, (0.80, 0.40, -0.45)),  # palm pitched back on the catch
    (0.48, (0.70, -0.35, 0.62)),
    (0.58, (0.30, -0.60, 0.74)),
    (0.72, (0.20, 0.08, 0.98)),
    (0.86, (0.06, 0.10, 0.99)),
]
K_CLAV = [  # clavicle: reaches on the glide, settles on the pull
    (0.00, (0.12, 0.94, 0.32)),
    (0.30, (0.18, 0.92, -0.35)),
    (0.58, (0.14, 0.88, -0.46)),
    (0.72, (0.14, 0.92, 0.20)),
    (0.86, (0.12, 0.94, 0.32)),
]
K_ULEG = [  # thigh (hip -> knee)
    (0.00, (0.02, 0.10, -0.99)),  # streamlined, legs together
    (0.30, (0.02, 0.10, -0.99)),
    (0.48, (0.30, 0.28, -0.91)),  # DRAW begins: knees coming forward + apart
    (0.62, (0.55, 0.42, -0.72)),  # DRAWN: knees up + spread (the frog)
    (0.74, (0.30, 0.50, -0.81)),  # WHIP: legs sweeping out and back
    (0.86, (0.02, 0.10, -0.99)),  # snapped together -> glide
]
K_LLEG = [  # shin (knee -> ankle)
    (0.00, (0.00, 0.06, -1.00)),  # straight
    (0.30, (0.00, 0.06, -1.00)),
    (0.48, (-0.35, 0.10, -0.93)),
    (0.62, (-0.42, 0.18, 0.89)),  # heels folded up to the butt (OUT of the water)
    (0.74, (-0.10, 0.62, -0.78)),  # whip: feet sweep out wide and back down
    (0.86, (0.00, 0.06, -1.00)),
]
K_FOOT = [
    (0.00, (0.30, 0.10, -0.95)),  # toes pointed (streamlined)
    (0.30, (0.30, 0.10, -0.95)),
    (0.48, (0.60, 0.40, -0.69)),
    (0.62, (0.55, 0.62, -0.56)),  # dorsiflexed + turned OUT (the frog's catch)
    (0.74, (0.45, 0.30, -0.84)),
    (0.86, (0.30, 0.10, -0.95)),
]
K_HEADUP = [(0.00, 8), (0.30, 20), (0.48, 34), (0.58, 30), (0.72, 12), (0.86, 8)]
K_ARCH   = [(0.00, 3), (0.30, 6), (0.48, 12), (0.58, 10), (0.72, 4), (0.86, 3)]
K_RISE   = [(0.00, 0.00), (0.30, 0.005), (0.48, 0.015), (0.62, 0.05),
            (0.78, 0.03), (0.90, 0.00)]


def mirror(v):
    return (v[0], -v[1], v[2])


def bake_swim(rig, frames=56):
    rig.new_action("Swim")
    for i in range(frames):
        f = i + 1
        t = i / float(frames)
        arm  = pwv(t, K_ARM);  fore = pwv(t, K_FORE); hand = pwv(t, K_HAND)
        clav = pwv(t, K_CLAV)
        uleg = pwv(t, K_ULEG); lleg = pwv(t, K_LLEG); foot = pwv(t, K_FOOT)
        armM  = pwv(t, [(k[0], mirror(k[1])) for k in K_ARM])
        foreM = pwv(t, [(k[0], mirror(k[1])) for k in K_FORE])
        handM = pwv(t, [(k[0], mirror(k[1])) for k in K_HAND])
        clavM = pwv(t, [(k[0], mirror(k[1])) for k in K_CLAV])
        ulegM = pwv(t, [(k[0], mirror(k[1])) for k in K_ULEG])
        llegM = pwv(t, [(k[0], mirror(k[1])) for k in K_LLEG])
        footM = pwv(t, [(k[0], mirror(k[1])) for k in K_FOOT])
        headA, arch, rise = D(pwf(t, K_HEADUP)), D(pwf(t, K_ARCH)), pwf(t, K_RISE)

        # torso: the undulation lift is along -FWD = UP in the water when prone
        rig.loc("hips", f, -rig.FWD * (rise * rig.unit))
        rig.rot("hips",  f, rig.toward(rig.UP, -rig.FWD, rise * 1.2))   # slight body wave
        rig.rot("spine",  f, rig.toward(rig.FWD, rig.UP, arch * 0.5))
        rig.rot("spine1", f, rig.toward(rig.FWD, rig.UP, arch * 0.3))
        rig.rot("spine2", f, rig.toward(rig.FWD, rig.UP, arch * 0.2))
        rig.rot("neck",   f, rig.toward(rig.FWD, rig.UP, headA * 0.45))
        rig.rot("head",   f, rig.toward(rig.FWD, rig.UP, headA * 0.55))
        # arms (aims are absolute, so the torso arch doesn't drag them off target)
        rig.aim("shoulderL", f, rig.vec(*clav));  rig.aim("shoulderR", f, rig.vec(*clavM))
        rig.aim("armL",  f, rig.vec(*arm));   rig.aim("armR",  f, rig.vec(*armM))
        rig.aim("foreL", f, rig.vec(*fore));  rig.aim("foreR", f, rig.vec(*foreM))
        rig.aim("handL", f, rig.vec(*hand));  rig.aim("handR", f, rig.vec(*handM))
        # legs — the frog kick, offset behind the arms
        rig.aim("upLegL", f, rig.vec(*uleg)); rig.aim("upLegR", f, rig.vec(*ulegM))
        rig.aim("legL",   f, rig.vec(*lleg)); rig.aim("legR",   f, rig.vec(*llegM))
        rig.aim("footL",  f, rig.vec(*foot)); rig.aim("footR",  f, rig.vec(*footM))


# ===========================================================================
# TREAD WATER (SwimIdle): arms sculling at the sides, slow flutter kick.
# ===========================================================================
def bake_swim_idle(rig, frames=48):
    rig.new_action("SwimIdle")
    for i in range(frames):
        f = i + 1
        t = i / float(frames)
        s1 = math.sin(2 * math.pi * t)
        c1 = math.cos(2 * math.pi * t)
        rig.loc("hips", f, -rig.FWD * (0.012 * rig.unit * (0.5 + 0.5 * math.sin(4 * math.pi * t))))
        rig.rot("hips",  f, rig.toward(rig.UP, rig.LAT, D(3) * s1))
        rig.rot("spine", f, rig.toward(rig.FWD, rig.UP, D(7)))
        rig.rot("spine2", f, rig.toward(rig.FWD, rig.UP, D(4)))
        rig.rot("neck",  f, rig.toward(rig.FWD, rig.UP, D(15)))
        rig.rot("head",  f, rig.toward(rig.FWD, rig.UP, D(17)))
        for s, m, ph in (("L", 1.0, 0.0), ("R", -1.0, math.pi)):
            sa = math.sin(2 * math.pi * t + ph)
            ca = math.cos(2 * math.pi * t + ph)
            # arms out at the sides, forearms forward, hands orbiting (the scull)
            rig.aim("shoulder" + s, f, rig.vec(0.15, m * 0.95, 0.10))
            rig.aim("arm" + s, f, rig.vec(0.30 + 0.10 * sa, m * (0.88 + 0.05 * ca), -0.30))
            rig.aim("fore" + s, f, rig.vec(0.72 + 0.12 * ca, m * (0.55 - 0.18 * sa), -0.25 + 0.12 * sa))
            rig.aim("hand" + s, f, rig.vec(0.80 + 0.10 * sa, m * (0.40 - 0.20 * ca), -0.20))
        for s, m, ph in (("L", 1.0, 0.0), ("R", -1.0, math.pi)):
            sl = math.sin(2 * math.pi * t + ph)
            cl = math.cos(2 * math.pi * t + ph)
            # flutter: thighs swing gently, knees soft and trailing
            rig.aim("upLeg" + s, f, rig.vec(0.32 + 0.16 * sl, m * 0.20, -0.90))
            rig.aim("leg" + s, f, rig.vec(-0.30 + 0.18 * cl, m * 0.10, -0.93))
            rig.aim("foot" + s, f, rig.vec(0.45, m * 0.15, -0.86))


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


def main():
    import_target()
    before = [a.name for a in bpy.data.actions]
    log("existing actions (kept):", len(before), before)
    rig = Rig()

    bake_swim(rig)
    # FK self-check, in character coords relative to the HIPS.
    #   GLIDE  : hands high +U (past the head), near the midline
    #   CATCH  : hands out +/-L, forearms pressed +F (down into the water)
    #   PULL   : hands swept IN (small |L|), +F, chest height
    #   DRAWN  : knees +F and spread, heels folded up (+U-ward, out of the water)
    ps = ["head", "handL", "handR", "legL", "footL", "footR"]
    for lab, ph in (("glide  t=.00", 0.00), ("catch  t=.30", 0.30),
                    ("pull   t=.48", 0.48), ("squeeze t=.58", 0.58),
                    ("shoot  t=.72", 0.72), ("drawn/whip t=.66", 0.66)):
        log("[fk Swim]", lab, "::", rig.probe(int(round(ph * 56)) + 1, ps))
    bake_swim_idle(rig)
    for lab, ph in (("tread t=.00", 0.0), ("tread t=.50", 0.5)):
        log("[fk SwimIdle]", lab, "::", rig.probe(int(round(ph * 48)) + 1, ps))

    if rig.arm.animation_data: rig.arm.animation_data.action = None
    log("actions now:", len(bpy.data.actions), [a.name for a in bpy.data.actions])

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
    log("EXPORTED:", OUT, os.path.getsize(OUT), "bytes")


if __name__ == "__main__":
    status = "OK"
    try:
        main()
    except Exception as e:
        import traceback
        log("FAILED:", e); log(traceback.format_exc()); status = "FAIL: " + str(e)
    flush_log(status)
