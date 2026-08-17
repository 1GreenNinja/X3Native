"""
bentover_bake.py — headless Blender BENT-OVER HELD POSE baker for X3Native
(the F2 ward assault tableau).

    blender-launcher.exe --background --python bentover_bake.py -- \
        <name.glb> <out.glb> [supportY] [--dump]

Sibling of struggle_bake.py / attack_death_bake.py (same append protocol): imports
a rigged GLB, KEEPS every existing action (Idle/Walk/Run/Talk/...), bakes ONE extra
clip — **BentOver** — and re-exports one GLB carrying all clips.

WHAT THE CLIP IS. A HELD POSE, not a motion: the captive standing on the FLOOR,
folded forward at the hips over a waist-height support (the ward's instrument
cart), both hands planted on its top, weight on her arms, head up and turned away
from what is behind her. ~2.4 s seamless loop of breathing + a small brace tremor
on top of that held pose, so she reads as alive rather than as a statue — the exact
defect this clip exists to kill ("she stands bolt upright like a statue").

REGISTER. Posture only. This is the interruptible tableau the player bursts in on,
staged at conversational distance — threat-posture, never explicit, per
RESCUE_SETPIECE_DESIGN.md §3, identical to the register struggle_bake.py bakes for
the attacker half of the same pair. Nothing here touches anatomy, contact, or
clothing; it is a spine pitch, an arm reach and a head turn.

GEOMETRY CONTRACT (why `supportY` is an argument). The pose is authored so her
HANDS land at `supportY` metres above her own foot plane — the scene anchors her
against a prop whose top is at a known height, so the pose has to know that height
or the hands float above / sink through the cart. Default 0.85 m = the ward
recipe's Crate Short instrument cart (0.25 m lift + 0.600 m box). The baker
MEASURES the resulting hand height off the posed armature and reports it in the
log, so the contract is verified rather than asserted.

  * family "anna"  — 20-joint Anna rigs: Hips/Spine/Chest/Neck/Head,
                     UpperArm.L/LowerArm.L/Hand.L, UpperLeg.L/LowerLeg.L/Foot.L
  * family "meshy" — 24-joint Meshy humanoid: Hips/Spine/Spine01/Spine02/neck/Head,
                     LeftArm/LeftForeArm/LeftHand, LeftUpLeg/LeftLeg/LeftFoot
  Both resolve through the same key-based lookup; no family flag is needed.

`--dump` prints the resolved bone map and the measured hand/foot heights and skips
nothing else — use it to re-tune the constants below against a render.

ENV NOTE (this box): Store-Blender — blender.exe is ACL-denied and blender-launcher
DETACHES; we report through `<out>.log` + `<out>.done` (poll them). Keep any
wrapper .ps1 ASCII-only.

Clean-room: public Blender Python API + glTF 2.0 spec only.
"""
import bpy, sys, os, math
from mathutils import Vector, Matrix

ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(ARGV) < 2:
    raise SystemExit("Usage: ... -- <name.glb> <out.glb> [supportY] [--dump]")
TARGET, OUT = ARGV[0], ARGV[1]
SUPPORT_Y = 0.85
DUMP = "--dump" in ARGV
NOARMS = "--noarms" in ARGV   # diagnostic: fold the torso, leave the arms at rest
for a in ARGV[2:]:
    if not a.startswith("--"):
        try: SUPPORT_Y = float(a)
        except ValueError: pass

LOG_PATH, DONE_PATH = OUT + ".log", OUT + ".done"
_log = []
def log(*a):
    s = "[bentover] " + " ".join(str(x) for x in a)
    _log.append(s); print(s)

def flush_log(status):
    try:
        with open(LOG_PATH, "w", encoding="utf-8") as f: f.write("\n".join(_log))
        with open(DONE_PATH, "w", encoding="utf-8") as f: f.write(status)
    except Exception as e:
        print("[bentover] could not write log/marker:", e)

D = math.radians

# ---------------------------------------------------------------------------
# THE POSE CONSTANTS (all degrees; +rx pitches a bone FORWARD, matching the
# convention struggle_bake.py bakes the attacker's loom with).
#
# Read it as a chain: the hips tip forward a little and drop; the spine carries
# the rest of the fold so the torso ends up near horizontal over the support; the
# thighs counter-rotate by the hip pitch so the LEGS STAY VERTICAL and the feet
# stay flat on the floor (this is the grounding half of the pose — a fold that
# rotates the legs with the hips lifts her heels and she reads as levitating);
# the arms reach forward-down onto the support with the elbows nearly straight
# (weight-bearing); the head counter-rotates UP off the chest so the neck is not
# folded shut and her face is visible in the shot.
# ---------------------------------------------------------------------------
HIP_PITCH   = 16.0     # pelvis tips forward
HEAD_LIFT   = -46.0    # counter-pitch: chin off the chest, looking ahead not down
HEAD_TURN   = 16.0     # face turned away over the shoulder
KNEE_BEND   = 9.0      # a soft knee (locked knees read as a mannequin)
SHOULDER_ROLL = 6.0    # shoulders forward/up, braced

# The pose's actual TARGET, in world terms rather than in bone-local guesses: how
# far the hips->neck line tips off vertical. 64 deg reads unmistakably as "folded
# over the thing in front of her" while keeping her face up and visible; a pure
# 90 deg would put her head below the support and hide the performance. Solved,
# not authored, because a given euler angle lands differently on every rig's rest
# orientation (the first cut authored 76 deg of bone rotation and rendered ~46).
TORSO_PITCH = 38.0

FRAMES = 58            # ~2.4 s @ 24 fps

# ---- UNIT SCALE ------------------------------------------------------------
# Every length above is in METRES, against a nominal ~1.8 m character whose head
# BONE sits ~1.60 m up. But rigs do not all import at that scale: the 20-joint Anna
# family lands at 1:1, while the 24-joint Meshy export lands ~11x larger (its first
# bake measured a 19 m head and 44 m hands). A metre-valued support height is
# meaningless in those units, so the baker measures the rig's OWN rest head height
# and converts. UNIT is "rig units per metre", resolved per target in main().
NOMINAL_HEAD_M = 1.48
UNIT = 1.0
def M(v):
    """Metres -> this rig's units."""
    return v * UNIT

# ---- bone lookup (both naming families) — mirrors struggle_bake.py ----------
def side_ok(n, side):
    if side is None: return True
    if side == "L": return ("left" in n or n.endswith(".l") or "_l_" in n or n.endswith("_l"))
    return ("right" in n or n.endswith(".r") or "_r_" in n or n.endswith("_r"))

def find_bone(bones, *keys, side=None, exclude=()):
    for b in bones:
        n = b.name.lower()
        if not side_ok(n, side): continue
        if any(x in n for x in exclude): continue
        if all(k in n for k in keys):
            return b.name
    return None

def depth(b):
    d, p = 0, b.parent
    while p: d += 1; p = p.parent
    return d

def classify(bones):
    g = {}
    g["hips"] = find_bone(bones, "hips") or find_bone(bones, "pelvis")
    g["head"] = find_bone(bones, "head", exclude=("end", "front", "top"))
    # A tip bone for the skull, when the rig has one. It exists purely to give the
    # head aim a REAL direction: glTF stores no bone tails, and the importer's
    # synthesized tail on these rigs points ~10 m into space, so aiming "the head"
    # by its tail rotates it almost arbitrarily and drags the whole head-weighted
    # region (hair included) into a sheet. With a tip bone the direction is real.
    g["headEnd"] = find_bone(bones, "head", "end") or find_bone(bones, "head", "top")
    g["neck"] = find_bone(bones, "neck")
    # Spine chain: everything that reads as spine/chest, ROOT-FIRST so the fold can
    # be spread from the lumbar up (a single-bone fold snaps at the waist).
    sp = [b for b in bones
          if ("spine" in b.name.lower() or "chest" in b.name.lower())
          and "hips" not in b.name.lower()]
    g["spine"] = [b.name for b in sorted(sp, key=depth)]
    for s, S in (("L", "L"), ("R", "R")):
        g["arm" + S]   = (find_bone(bones, "upperarm", side=s)
                          or find_bone(bones, "arm", side=s,
                                       exclude=("fore", "lower", "shoulder")))
        g["fore" + S]  = (find_bone(bones, "lowerarm", side=s)
                          or find_bone(bones, "forearm", side=s))
        g["hand" + S]  = find_bone(bones, "hand", side=s)
        g["shldr" + S] = find_bone(bones, "shoulder", side=s) or find_bone(bones, "clavicle", side=s)
        g["thigh" + S] = (find_bone(bones, "upperleg", side=s)
                          or find_bone(bones, "upleg", side=s)
                          or find_bone(bones, "thigh", side=s))
        g["shin" + S]  = (find_bone(bones, "lowerleg", side=s)
                          or find_bone(bones, "leg", side=s,
                                       exclude=("upper", "upleg", "up")))
        g["foot" + S]  = find_bone(bones, "foot", side=s, exclude=("end", "toe"))
        g["toe" + S]   = find_bone(bones, "toe", side=s, exclude=("end",))
    return g

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

def prune_crossbody_weights(arm, g):
    """Drop skin influences that no anatomy can justify, by DISTANCE.

    An auto-rigger binds the mesh in the pose it is given, and these characters are
    modelled with their arms hanging against their hips. Every surface that touches
    another in that pose gets cross-weighted: hip vertices pick up the upper arm,
    and arm vertices pick up the hips and spine. A standing idle never shows it —
    nothing moves far relative to anything else — but fold the torso and the arms
    are half-dragged with it, tearing a web of triangles between torso and arm that
    renders as a solid skin-coloured sheet. Four renders in this lane showed that
    sheet, and it survived every change to the POSE, because it was never a posing
    fault.

    The rule: an influence is implausible when the vertex sits further from that
    bone's own segment than skin ever gets. Applied to every bone, in both
    directions, in the REST pose. A vertex can never be left unbound — if the rule
    would strip everything, its nearest influence is kept — and survivors are
    renormalised, so the mesh stays fully skinned.
    """
    # How far from a bone its skin can plausibly lie, in metres.
    reach_m = {}
    def put(key, r):
        n = g.get(key)
        if n: reach_m[n] = r
    for S in ("L", "R"):
        put("arm" + S, 0.19);  put("fore" + S, 0.15); put("hand" + S, 0.13)
        put("shldr" + S, 0.20)
        put("thigh" + S, 0.22); put("shin" + S, 0.17); put("foot" + S, 0.15)
        put("toe" + S, 0.12)
    put("hips", 0.30); put("neck", 0.15); put("head", 0.22)
    for n in (g.get("spine") or []): reach_m[n] = 0.28

    zero_pose(arm)
    bpy.context.view_layer.update()
    seg = {}
    for n in reach_m:
        pb = arm.pose.bones.get(n)
        if pb: seg[n] = (wh(arm, pb), wt(arm, pb))

    def dist_to_seg(pt, a, b):
        ab = b - a
        L2 = ab.dot(ab)
        if L2 < 1e-12: return (pt - a).length
        u = max(0.0, min(1.0, (pt - a).dot(ab) / L2))
        return (pt - (a + ab * u)).length

    pruned = kept = 0
    for ob in [o for o in bpy.data.objects if o.type == 'MESH']:
        vg = ob.vertex_groups
        idx2name = {vg[n].index: n for n in seg if n in vg}
        if not idx2name: continue
        drops = {}          # group index -> [vertex indices]
        renorm = []         # (vertex index, [(group index, weight)])
        for v in ob.data.vertices:
            gs = [(x.group, x.weight) for x in v.groups if x.weight > 0.0]
            if len(gs) < 2: continue
            wpt = ob.matrix_world @ v.co
            dists = {}
            for gi, _w in gs:
                n = idx2name.get(gi)
                if n: dists[gi] = dist_to_seg(wpt, *seg[n])
            if not dists: continue
            drop = {gi for gi, _w in gs
                    if gi in dists and dists[gi] > M(reach_m[idx2name[gi]])}
            if len(drop) == len(gs):        # never unbind a vertex
                drop.discard(min(dists, key=dists.get))
                kept += 1
            if not drop: continue
            for gi in drop:
                drops.setdefault(gi, []).append(v.index)
            pruned += len(drop)
            keep = [(gi, w) for gi, w in gs if gi not in drop]
            tot = sum(w for _gi, w in keep)
            if tot > 1e-6:
                renorm.append((v.index, [(gi, w / tot) for gi, w in keep]))
        # WRITE through the vertex-group API. Assigning MeshVertex.groups[i].weight
        # looks like it works and reports edits, but it does not survive to the
        # armature evaluation or the exporter — an earlier pass "pruned" 20k
        # influences and the render came back pixel-identical. remove()/add() is the
        # path that actually rebinds the mesh.
        for gi, verts in drops.items():
            vg[gi].remove(verts)
        for vi, ws in renorm:
            for gi, w in ws:
                vg[gi].add([vi], w, 'REPLACE')
    log("weight repair: pruned", pruned, "out-of-reach influences (",
        kept, "vertices kept their nearest bone to stay bound )")
    return pruned

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

def set_euler(arm, bone, rx=0.0, ry=0.0, rz=0.0):
    if not bone: return
    pb = arm.pose.bones.get(bone)
    if not pb: return
    pb.rotation_euler = (rx, ry, rz)

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
# The held pose, evaluated at loop phase t in [0,1). Everything additive on top of
# the base pose is a sin(2*pi*t) term so frame N-1 flows into frame 0 — the loop is
# seamless by construction (same law as struggle_bake.py).
# ---------------------------------------------------------------------------
def pose_at(arm, g, t, lean=None):
    breathe = math.sin(2.0 * math.pi * t)          # slow chest rise/fall
    tremor  = math.sin(4.0 * math.pi * t + 0.9)    # faster brace shiver in the arms
    fwd = REST_FWD

    # --- pelvis then spine: AIMED up an arc from vertical toward `fwd` ------
    # Not eulered. Bone-local rx folds the 20-joint Anna rig forward but sends the
    # 24-joint Meshy rig somewhere else entirely — the solver still reported the
    # requested "pitch off vertical" because a sideways curl tips the hips->neck
    # line just as far off vertical as a forward fold does, and the render showed
    # a woman crumpled into a ball. Stating the fold as a world DIRECTION per bone
    # removes the ambiguity: each link points along a known heading, so there is no
    # axis to guess and no bisection to run.
    def PB(k):
        n = g.get(k)
        return arm.pose.bones.get(n) if n else None

    def leanDir(a):
        return UP * math.cos(a) + fwd * math.sin(a)

    chain = g["spine"]
    hp = PB("hips")
    if hp and chain:
        # Aim the pelvis by hips-head -> spine-root-head (never by the tail: Hips
        # parents both thighs and the spine, so its synthesized tail is a coin flip).
        aim_dir(arm, hp, leanDir(D(HIP_PITCH)), ref=arm.pose.bones.get(chain[0]))
        # NO pelvis TRANSLATION. A "settle" of a few centimetres reads well on a
        # normal rig, but a pose bone's location is expressed in ITS OWN space, and
        # these auto-rigs carry synthesized bone axes ~10 m long — so a 0.055 value
        # is not 5 cm, it is metres, and it rips the pelvis away from the mesh. That
        # is the sheet: it barely changed when the fold went from 64 deg to 38 deg,
        # because it never came from the fold at all. The pose is pure rotation now.
        bpy.context.view_layer.update()

    for i, b in enumerate(chain):
        pb = arm.pose.bones.get(b)
        if not pb: continue
        # Fan the fold from the pelvis angle up to the full torso pitch, weighted so
        # the lumbar carries most of it (a hinge at the waist, not a banana back).
        # The hips->neck line the target describes is the chain AVERAGE, so the
        # ramp has to overshoot: fanning 16..55 deg averaged 36 and rendered as a
        # shrug. End at 2*target - hips so the mean lands on the target.
        f = ((i + 1) / float(len(chain))) ** 0.72
        top = 2.0 * TORSO_PITCH - HIP_PITCH
        a = D(HIP_PITCH + (top - HIP_PITCH) * f + 1.2 * breathe)
        nxt = (arm.pose.bones.get(chain[i + 1]) if i + 1 < len(chain)
               else (PB("neck") or PB("head")))
        aim_dir(arm, pb, leanDir(a), ref=nxt)
    bpy.context.view_layer.update()

    # --- everything below the spine is AIMED, not eulered -------------------
    # Bone-local degrees do not transfer between rigs: the same authored numbers
    # that fold the 20-joint Anna rig correctly put the 24-joint Meshy rig's head
    # BELOW its own hands, because the two families rest their bones on different
    # axes. Legs, feet and head are therefore stated as world-space intents —
    # "shins plumb", "soles flat and forward", "face up and turned away" — and
    # aimed. That is also what makes the grounding law hold by construction rather
    # than by a cancellation sum that only balances on one skeleton.
    plant_legs(arm, g, breathe)
    for S in ("L", "R"):
        set_euler(arm, g.get("shldr" + S), rx=D(SHOULDER_ROLL))
    bpy.context.view_layer.update()
    if not NOARMS: plant_hands(arm, g, tremor, lean=lean)
    aim_head(arm, g, breathe, tremor)
    return


UP = Vector((0.0, 0.0, 1.0))
REST_FWD = Vector((0.0, -1.0, 0.0))   # resolved per target in main()

def plant_legs(arm, g, breathe):
    """Shins plumb under the hips, a soft knee, soles flat and facing forward."""
    fwd = REST_FWD
    for S in ("L", "R"):
        th = arm.pose.bones.get(g["thigh" + S]) if g.get("thigh" + S) else None
        sh = arm.pose.bones.get(g["shin"  + S]) if g.get("shin"  + S) else None
        ft = arm.pose.bones.get(g["foot"  + S]) if g.get("foot"  + S) else None
        if not th or not sh: continue
        kb = math.tan(D(KNEE_BEND))
        # Knee straight down from the hip, nudged forward by the knee bend...
        aim_dir(arm, th, (-UP + fwd * kb), ref=sh)
        # ...ankle straight down from the knee, nudged BACK by the same amount, so
        # the foot lands under the hip and the leg reads bent, not translated.
        aim_dir(arm, sh, (-UP - fwd * kb), ref=ft)
        toe = arm.pose.bones.get(g["toe" + S]) if g.get("toe" + S) else None
        if ft:
            aim_dir(arm, ft, fwd, ref=toe)              # sole flat, toes forward

def aim_head(arm, g, breathe, tremor):
    """Face UP off the chest and turned away — so the shot has a face in it."""
    fwd = REST_FWD
    side = Vector((-fwd.y, fwd.x, 0.0))
    nk = arm.pose.bones.get(g["neck"]) if g.get("neck") else None
    hd = arm.pose.bones.get(g["head"]) if g.get("head") else None
    # HEAD_LIFT is authored as a counter-pitch off the folded chest; as a world
    # intent it means "the head axis tips back up toward horizontal".
    lift = math.tan(D(-HEAD_LIFT * 0.55))
    if nk:
        aim_dir(arm, nk, (fwd + UP * lift), ref=hd)
    he = arm.pose.bones.get(g["headEnd"]) if g.get("headEnd") else None
    if hd and he:
        turn = math.tan(D(HEAD_TURN + 2.2 * tremor))
        # The skull axis runs UP through the head, so the "look" direction is mostly
        # up with the forward/turn terms leaning it: aiming head-to-tip.
        aim_dir(arm, hd, (UP + fwd * (0.35 + 0.05 * breathe) + side * turn * 0.5),
                ref=he)


# ---------------------------------------------------------------------------
# PLANTING THE HANDS — why this is IK and not another euler constant.
#
# The first cut of this pose set the arm bones with hand-authored euler angles and
# checked only the resulting hand HEIGHT. It hit 0.85 m exactly — with the arms
# hanging straight down at her sides, because a height constraint alone is also
# satisfied by "don't move the arms at all". The render showed it immediately: a
# woman leaning forward with her hands by her thighs, not a woman with her hands
# ON something. A support pose is defined by WHERE the hands are in space, so the
# hands get solved for a world POINT, in all three axes, and the arm angles fall
# out of that.
#
# Two-bone analytic IK (upper arm + forearm), pole vector pushing the elbow out
# and back so the elbows read as bracing rather than chicken-winging inward.
# ---------------------------------------------------------------------------
# EVERYTHING below is WORLD space. The first cut of the IK mixed spaces — bone
# heads read in armature-local space against a foot plane and an "up" axis read in
# world space — and since the glTF importer leaves the armature rotated (Y-up art
# into Blender's Z-up scene), armature-local Z is not world up at all. The pose
# rendered as a mangled crouch. Hence these wrappers: no raw pb.head/pb.tail is
# used for geometry anywhere, and aim_bone converts back through matrix_world.
def wh(arm, pb):  return arm.matrix_world @ pb.head
def wt(arm, pb):  return arm.matrix_world @ pb.tail
def wvec(arm, pb): return wt(arm, pb) - wh(arm, pb)

def _rest(pb):
    """Park the bone on its socket: pure rotation, no translation, no scale."""
    pb.location = (0.0, 0.0, 0.0)
    pb.scale = (1.0, 1.0, 1.0)
    bpy.context.view_layer.update()

def aim_dir(arm, pb, direction, ref=None):
    """Rotate `pb` so the vector pb.head -> (ref.head, or pb.tail) points along
    `direction`. ROTATION ONLY.

    WHY `ref` EXISTS. glTF stores no bone tails; Blender's importer synthesizes
    them, and for a joint with several children (Hips parents both thighs AND the
    spine) it can pick any of them. Aiming such a tail "up" can therefore flip the
    pelvis and fold the whole character in half — which is exactly what collapsed
    the 24-joint rigs (head measured 0.55 m off the floor). Passing the CHILD whose
    direction is actually meant removes the guess: parent-head to child-head is
    unambiguous on every skeleton.
    """
    _rest(pb)
    h = wh(arm, pb)
    v0 = (wh(arm, ref) - h) if ref is not None else (wt(arm, pb) - h)
    if v0.length < 1e-6 or direction.length < 1e-6: return
    _aim_vec(arm, pb, v0, direction.normalized() * v0.length)

def aim_bone(arm, pb, target):
    """Rotate `pb` in place — ROTATION ONLY — so its tail points at world `target`.

    WHY THE ZERO-SANDWICH. Assigning `pb.matrix` is the standard way to aim a bone,
    but Blender decomposes that matrix into loc/rot/scale, so a translation and a
    scale fall out of it alongside the rotation. In Blender's own viewport a
    connected bone ignores that translation, so the live pose (and every measurement
    taken off it) looks perfect — but the glTF exporter writes the translation and
    scale channels anyway, and on re-import the arm is yanked out of its socket and
    the mesh between shoulder and hand renders as a stretched sheet. That is exactly
    what the first two profile renders showed, and why the numbers said the pose was
    right while the picture said it was ruined.

    So: park the bone on its socket FIRST (so the head is the true joint position
    the aim is computed from), aim, then park it again. The second park restores
    the head without disturbing the rotation — the aim only ever depended on the
    head, which the first park already fixed — leaving a clip that is pure rotation
    and survives the export/import round trip.
    """
    _rest(pb)
    headw = wh(arm, pb)
    v0, v1 = (wt(arm, pb) - headw), (target - headw)
    if v0.length < 1e-6 or v1.length < 1e-6: return
    _aim_vec(arm, pb, v0, v1)

def _aim_vec(arm, pb, v0, v1):
    """Rotate `pb` so world vector v0 (rooted at its head) lands on v1.

    The rotation axis is v0 x v1 — EXCEPT when the two are near-(anti)parallel,
    where that cross product collapses to numerical noise and the axis it yields
    is arbitrary. That is not a rare edge case here: a limb whose rest direction
    already points near the target hits it constantly, and an arbitrary axis at a
    ~180 deg angle flips the limb, tearing the mesh into a sheet. Two limbs pose
    symmetrically but only one is near-antiparallel, which is exactly why the
    damage kept appearing on ONE side while its mirror looked perfect. In that
    case rotate in the SAGITTAL plane (about the body's lateral axis), which is
    the anatomically meaningful choice for a limb swinging forward or back.
    """
    headw = wh(arm, pb)
    a, b = v0.normalized(), v1.normalized()
    axis = a.cross(b)
    if axis.length < 0.08:
        side = Vector((-REST_FWD.y, REST_FWD.x, 0.0))
        axis = side if side.length > 1e-6 else Vector((1.0, 0.0, 0.0))
    from mathutils import Quaternion
    q = Quaternion(axis.normalized(), a.angle(b))
    Mw = arm.matrix_world @ pb.matrix
    newMw = (Matrix.Translation(headw) @ q.to_matrix().to_4x4()
             @ Matrix.Translation(-headw) @ Mw)
    pb.matrix = arm.matrix_world.inverted() @ newMw
    _rest(pb)

def rest_forward(arm, g):
    """Which way this character FACES, measured off its own anatomy in the REST
    pose: ankle -> toe. Every humanoid rig in the project has a foot bone whose
    tail is the toe, so this is a reference that cannot be wrong — unlike reading
    a pose that the fold itself produced (the earlier hips->neck version fed the
    fold its own output and let a sideways curl pass as a forward one)."""
    d = Vector((0.0, 0.0, 0.0))
    for S in ("L", "R"):
        b = arm.pose.bones.get(g["foot" + S]) if g.get("foot" + S) else None
        if not b: continue
        toe = arm.pose.bones.get(g["toe" + S]) if g.get("toe" + S) else None
        v = (wh(arm, toe) if toe else wt(arm, b)) - wh(arm, b)
        v.z = 0.0
        if v.length > 1e-6: d += v.normalized()
    if d.length > 1e-5: return d.normalized()
    return Vector((0.0, -1.0, 0.0))

def foot_plane(arm, g):
    """The ground the character stands on: the lowest toe joint (or, on rigs with
    no toe bone, the foot bone's toe-ward tail)."""
    zs = []
    for S in ("L", "R"):
        toe = arm.pose.bones.get(g["toe" + S]) if g.get("toe" + S) else None
        ft  = arm.pose.bones.get(g["foot" + S]) if g.get("foot" + S) else None
        if toe:  zs.append(wh(arm, toe).z)
        elif ft: zs.append(wt(arm, ft).z)
    return min(zs) if zs else 0.0

ARM_HANG   = 16.0    # upper arm: degrees forward of straight down
FORE_LEAN  = 62.0    # forearm: degrees forward of straight down (SOLVED per rig)

def _arm_dirs(hang, lean):
    fwd = REST_FWD
    a, b = D(hang), D(lean)
    return (-UP * math.cos(a) + fwd * math.sin(a),
            -UP * math.cos(b) + fwd * math.sin(b))

def plant_hands(arm, g, tremor, lean=None):
    """Both arms down and forward onto the support — as a pair of AIMS, not IK.

    WHY NOT IK. A two-bone IK solve did put the wrists exactly on the support, and
    the numbers passed every check — but reaching a target from an arbitrary start
    needs a large rotation, and the minimal-arc rotation that gets there carries an
    arbitrary ROLL about the bone axis. On one arm that roll came out near 180 deg
    and the limb rendered as a flat sheet. Three attempts to fix it from the outside
    (unit scale, bone translation, a symmetric elbow pole) each left the sheet
    exactly where it was, because none of them touched the roll.

    A bent-over figure does not need IK anyway: with the torso folded, the shoulders
    are already low and forward, and arms simply hanging down-and-forward land on a
    waist-height surface on their own. Aiming each arm bone a short way off its REST
    direction keeps every rotation small, so there is no roll to go wrong — and the
    one number that must be exact, the wrist height, is solved as a single scalar
    (the forearm's lean) shared by both arms, which cannot break symmetry.
    """
    d_up, d_fore = _arm_dirs(ARM_HANG, FORE_LEAN if lean is None else lean)
    for S in ("L", "R"):
        up   = arm.pose.bones.get(g["arm"  + S]) if g.get("arm"  + S) else None
        fore = arm.pose.bones.get(g["fore" + S]) if g.get("fore" + S) else None
        if not up or not fore: continue
        hnd = arm.pose.bones.get(g["hand" + S]) if g.get("hand" + S) else None
        # A whisper of brace tremor so the planted arms are not dead-still.
        jitter = UP * (0.004 * tremor)
        aim_dir(arm, up, d_up + jitter, ref=fore)
        aim_dir(arm, arm.pose.bones[fore.name], d_fore - jitter, ref=hnd)
        if hnd:  # palm laid FLAT on the support: aim the hand along the surface
            aim_dir(arm, hnd, REST_FWD)

def key_current(arm, g, frame):
    for name in ("hips", "neck", "head"):
        key_euler_from(arm, g.get(name), frame)
    for b in g["spine"]:
        key_euler_from(arm, b, frame)
    for S in ("L", "R"):
        for k in ("shldr", "thigh", "shin", "foot"):
            key_euler_from(arm, g.get(k + S), frame)
        # The arm chain is solved by IK, but aim_bone() leaves it as pure ROTATION
        # (see its docstring) — so rotation is the only channel there is to key.
        # Keying location here is what blew the arms into sheets on re-import.
        for k in ("arm", "fore", "hand"):
            key_euler_from(arm, g.get(k + S), frame)
    # No location channel anywhere: the clip is pure rotation by construction.

def key_euler_from(arm, bone, frame):
    if not bone: return
    pb = arm.pose.bones.get(bone)
    if not pb: return
    pb.keyframe_insert("rotation_euler", frame=frame)

# ---- MEASUREMENT: prove the contract instead of asserting it ---------------
def measure(arm, g):
    """Hand and foot heights in the armature's OWN object space (metres),
    evaluated at the current pose. `hands - feet` is what has to equal supportY."""
    dg = bpy.context.evaluated_depsgraph_get()
    bpy.context.view_layer.update()
    def head_z(bone):
        if not bone: return None
        pb = arm.pose.bones.get(bone)
        if not pb: return None
        return (arm.matrix_world @ pb.tail).z
    hs = [z for z in (joint_z(arm, g, "handL"), joint_z(arm, g, "handR"))
          if z is not None]
    hand = sum(hs) / len(hs) if hs else None
    return hand, foot_plane(arm, g)

def torso_pitch(arm, g):
    """Degrees the hips->neck line tips off vertical, at the current pose."""
    bpy.context.view_layer.update()
    hp = arm.pose.bones.get(g["hips"]) if g.get("hips") else None
    nk = (arm.pose.bones.get(g["neck"]) if g.get("neck")
          else (arm.pose.bones.get(g["head"]) if g.get("head") else None))
    if not hp or not nk: return None
    v = (arm.matrix_world @ nk.head) - (arm.matrix_world @ hp.head)
    if v.length < 1e-5: return None
    return math.degrees(math.acos(max(-1.0, min(1.0, v.normalized().z))))

def joint_z(arm, g, key):
    """World height of a JOINT CENTRE (bone head). Never a tail: glTF carries no
    tails, and the importer's synthesized one for the Head bone on the 24-joint
    rigs sits metres away in space — reading it made a 1.5 m character measure
    21 m tall, which in turn scaled the whole pose by 13x and threw the hands
    into the ceiling. Heads are real joint positions; tails are a guess."""
    n = g.get(key)
    pb = arm.pose.bones.get(n) if n else None
    return None if not pb else (arm.matrix_world @ pb.head).z

def head_height(arm, g):
    bpy.context.view_layer.update()
    z = joint_z(arm, g, "head")
    return None if z is None else (z - foot_plane(arm, g))

# ---- SOLVE the one number that has to be exact: where the wrists land -------
# Everything else in the pose is a stated direction. The wrist HEIGHT is the single
# quantity the scene depends on (her hands have to be ON the cart top, not floating
# above it or buried in it), and it depends on the rig's own limb proportions — so
# it is fitted, once, as a scalar shared by both arms. Monotonic: leaning the
# forearm further forward lowers the wrist.
def foot_plane_target(arm, g):
    """Wrist height wanted above the floor, in this rig's units."""
    return M(SUPPORT_Y)

def solve_forearm_lean(arm, g, targetAboveFloor):
    def wrist_at(lean):
        zero_pose(arm); pose_at(arm, g, 0.0, lean=lean)
        h, f = measure(arm, g)
        return None if (h is None or f is None) else (h - f)
    lo, hi = 0.0, 110.0
    wlo, whi = wrist_at(lo), wrist_at(hi)
    if wlo is None or whi is None:
        return FORE_LEAN, None
    if not (min(wlo, whi) - 1e-4 <= targetAboveFloor <= max(wlo, whi) + 1e-4):
        l, w = (lo, wlo) if abs(wlo - targetAboveFloor) < abs(whi - targetAboveFloor) else (hi, whi)
        log("solve: wrist height %.3f is outside the reachable band [%.3f .. %.3f] "
            "— clamping to lean %.1f deg" % (targetAboveFloor, min(wlo, whi),
                                             max(wlo, whi), l))
        return l, w
    for _ in range(22):
        lm = 0.5 * (lo + hi)
        wm = wrist_at(lm)
        if wm is None: break
        if (wlo - targetAboveFloor) * (wm - targetAboveFloor) <= 0.0: hi, whi = lm, wm
        else:                                                         lo, wlo = lm, wm
    l = 0.5 * (lo + hi)
    return l, wrist_at(l)


def bake(arm, g, lean):
    act = new_action(arm, "BentOver")
    for i in range(FRAMES):
        zero_pose(arm)
        pose_at(arm, g, i / float(FRAMES), lean=lean)
        key_current(arm, g, i + 1)
    return act

# ---------------------------------------------------------------------------
def main():
    arm = import_target()
    before = [a.name for a in bpy.data.actions]
    g = classify(arm.data.bones)
    log("target:", os.path.basename(TARGET), "bones:", len(arm.data.bones),
        "supportY:", SUPPORT_Y)
    log("resolved:", {k: v for k, v in g.items() if v})
    log("existing actions (kept):", before)
    missing = [k for k in ("hips", "head", "armL", "armR", "thighL", "thighR") if not g.get(k)]
    if missing:
        raise RuntimeError("unresolved bones (rig not humanoid?): " + ", ".join(missing))

    # ---- resolve this rig's unit scale and its facing, off its OWN rest pose
    global UNIT, REST_FWD
    zero_pose(arm)
    bpy.context.view_layer.update()
    rest_head = head_height(arm, g)
    if rest_head and rest_head > 1e-4:
        UNIT = rest_head / NOMINAL_HEAD_M
    REST_FWD = rest_forward(arm, g)
    log("rig rest head height: %.3f units -> UNIT = %.4f units/metre "
        "(1.0 means the rig is authored in metres)"
        % (rest_head if rest_head else -1.0, UNIT))
    log("rig rest FORWARD (ankle->toe): (%.3f, %.3f, %.3f)"
        % (REST_FWD.x, REST_FWD.y, REST_FWD.z))

    prune_crossbody_weights(arm, g)   # needs UNIT (its reach limits are in metres)

    # Fit the wrist height, then build the held frame and MEASURE it. Every other
    # joint is aimed at a stated world direction, so the rest of these numbers are
    # a check rather than a fit.
    lean, _ = solve_forearm_lean(arm, g, foot_plane_target(arm, g))
    zero_pose(arm); pose_at(arm, g, 0.0, lean=lean)
    pitch = torso_pitch(arm, g)
    log("SOLVED forearm lean: %.1f deg forward of plumb (seed %.1f)"
        % (lean, FORE_LEAN))
    hand, foot = measure(arm, g)
    hh = head_height(arm, g)
    fwd = REST_FWD
    log("MEASURED torso pitch: %.1f deg off vertical (target %.1f)"
        % (pitch if pitch is not None else -1.0, TORSO_PITCH))
    if hand is not None and foot is not None:
        log("MEASURED hand height above foot plane: %.3f m (target supportY %.3f m, "
            "delta %+.3f m)" % ((hand - foot) / UNIT, SUPPORT_Y, (hand - foot) / UNIT - SUPPORT_Y))
    else:
        log("MEASURE skipped (hand/foot bones unresolved)")
    if hh is not None:
        log("MEASURED head height above foot plane: %.3f m "
            "(bent-over reads ~1.1-1.4 m; upright would be ~1.6-1.7 m)" % (hh / UNIT))
    # How far the hands reach out in front of the hips — the number the FIRST cut
    # of this pose got wrong (hands at the right height but beside her thighs).
    hp = arm.pose.bones.get(g["hips"])
    reach_out = []
    for S in ("L", "R"):
        hb = arm.pose.bones.get(g["hand" + S]) if g.get("hand" + S) else None
        if hb: reach_out.append(fwd.dot(wh(arm, hb) - wh(arm, hp)))
    if reach_out:
        log("MEASURED hand reach ahead of hips: %.3f m (hands ON a support read "
            ">= ~0.30 m; hands at her sides read ~0.0)"
            % (sum(reach_out) / len(reach_out) / UNIT))
    if DUMP:
        # Per-joint world heights, rest vs posed — the only way to see WHERE a
        # collapse happens rather than guessing from the summary numbers.
        order = ["hips"] + g["spine"] + ["neck", "head",
                 "shldrL", "armL", "foreL", "handL",
                 "thighL", "shinL", "footL", "toeL"]
        zero_pose(arm); bpy.context.view_layer.update()
        rest = {}
        for k in order:
            n = g.get(k, k)
            pb = arm.pose.bones.get(n) if n else None
            if pb: rest[k] = wh(arm, pb).z
        zero_pose(arm); pose_at(arm, g, 0.0)
        bpy.context.view_layer.update()
        for k in order:
            n = g.get(k, k)
            pb = arm.pose.bones.get(n) if n else None
            if not pb: continue
            log("  %-10s %-14s rest z=%8.3f  posed z=%8.3f  (%+8.3f)"
                % (k, n, rest.get(k, 0.0), wh(arm, pb).z,
                   wh(arm, pb).z - rest.get(k, 0.0)))
        log("DUMP-ONLY: no export"); return

    # ---- THE CONTRACT GATE -------------------------------------------------
    # Refuse to write a clip that does not read as the pose. Two bakes in this
    # lane produced numbers that looked plausible while the render showed a
    # crumpled figure, and a silently-exported bad clip is worse than no clip: it
    # ships as a shrug in a set-piece nobody re-checks. Fail loudly instead.
    bad = []
    if hand is None or foot is None:
        bad.append("hand/foot bones unresolved")
    else:
        dh = (hand - foot) / UNIT - SUPPORT_Y
        if abs(dh) > 0.12:
            bad.append("hands %.3f m off the support (limit 0.12)" % dh)
    if hh is None or hh / UNIT < 0.95:
        bad.append("head only %.3f m up — the figure is collapsed, not folded"
                   % ((hh / UNIT) if hh else -1.0))
    if reach_out and sum(reach_out) / len(reach_out) / UNIT < 0.30:
        bad.append("hands are not out in front (reach %.3f m, need >= 0.30)"
                   % (sum(reach_out) / len(reach_out) / UNIT))
    if pitch is not None and abs(pitch - TORSO_PITCH) > 12.0:
        bad.append("torso pitch %.1f deg vs target %.1f" % (pitch, TORSO_PITCH))
    if bad and not NOARMS:
        raise RuntimeError("pose contract FAILED, refusing to export: "
                           + "; ".join(bad))
    if bad:
        log("DIAGNOSTIC (--noarms): contract waived —", "; ".join(bad))
    log("contract OK — exporting")

    bake(arm, g, lean)

    arm.animation_data.action = None
    log("actions now:", sorted(a.name for a in bpy.data.actions))

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
