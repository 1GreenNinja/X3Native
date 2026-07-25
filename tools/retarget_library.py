"""
retarget_library.py - manifest-driven headless Blender retargeter for X3Native.

    blender-launcher.exe --background --python retarget_library.py -- \
        <target.glb> <out.glb> <manifest.json>

The general successor to retarget_anims.py (StarterAssets locomotion) and
retarget_from_jake.py (Jake Mixamo locomotion+talk). It bakes an ARBITRARY set of
clips onto ONE humanoid ".L/.R" target rig, each clip retargeted from a source of
one of three RIG TYPES, then exports one multi-clip GLB:

  * rig "mixamo"  - source is a Mixamo rig (bones "mixamorig<Name>", e.g.
                    Jake_22_actions.glb). Uses BONE_MAP_MIXAMO.
  * rig "starter" - source is a Unity StarterAssets rig (bones "Left_/Right_...").
                    Uses BONE_MAP_STARTER. Source may be a .fbx or .glb.
  * rig "lr"      - source is ALREADY on the ".L/.R" target rig (identity map,
                    e.g. copy marcus_webb_anim.glb's "Attack" onto chief). Uses
                    BONE_MAP_LR (identity).

All three drive the SAME rest-relative, world-delta orientation transfer used by
retarget_from_jake.py (orientation-safe across differing rest poses / bone rolls),
so a single code path serves every source rig. The manifest is a JSON file:

    {
      "keep_existing": true,          # keep the target GLB's own actions + append
      "clips": [
        {"name":"Hitreaction", "src":"assets/rigged_glb/Jake_22_actions.glb",
         "rig":"mixamo", "action":"Hitreaction", "start":1, "end":10, "loop":false},
        {"name":"Attack2", "src":"assets/rigged_glb/Jake_22_actions.glb",
         "rig":"mixamo", "action":"Tossgrenade", "start":25, "end":72, "loop":false},
        {"name":"Struggle", "src":"assets/rigged_glb/marcus_webb_anim.glb",
         "rig":"lr", "action":"Struggle", "loop":true}   # start/end omitted = whole
      ]
    }

`start`/`end` are Blender frame numbers (glTF imports at 24 fps); omit BOTH to bake
the source action's whole frame range. `loop` is metadata only (the runtime decides
looping); it is recorded in the log for the caller.

Preserves ALL of the original texture-packing/export discipline: pack images early
(before source-import churn can evict them), fake-user materials + kept actions,
export_animation_mode='ACTIONS'.

ENV NOTE (this box): Blender is the Microsoft Store package - direct blender.exe is
ACL-denied and blender-launcher.exe DETACHES, so we report through files: a sidecar
<out>.log and a <out>.done marker the caller polls. ASCII-only source.

Clean-room: built from the public Blender Python API + glTF 2.0 spec only.
No GPL / id Tech / RBDOOM source consulted.
"""
import bpy, sys, os, json, math
from mathutils import Vector

ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(ARGV) < 3:
    raise SystemExit("Usage: ... -- <target.glb> <out.glb> <manifest.json>")
TARGET_GLB, OUT_GLB, MANIFEST = ARGV[0], ARGV[1], ARGV[2]

LOG_PATH  = OUT_GLB + ".log"
DONE_PATH = OUT_GLB + ".done"
_log = []
def log(*a):
    s = "[retarget-lib] " + " ".join(str(x) for x in a)
    _log.append(s); print(s)

def flush_log(status):
    try:
        with open(LOG_PATH, "w", encoding="utf-8") as f:
            f.write("\n".join(_log))
        with open(DONE_PATH, "w", encoding="utf-8") as f:
            f.write(status)
    except Exception as e:
        print("[retarget-lib] could not write log/marker:", e)

ROOT_BONE = "Hips"

# Headless GROUNDED + UPRIGHT assertion facts (one dict per baked clip). main()
# gates on them: every clip must stay GROUNDED (lowest planted foot near the
# target floor -- guards the vertical-bob scale) and every STANDING clip must
# land the pelvis near-vertical (guards against a re-introduced rest-tilt lean).
_ASSERTS = []
UPRIGHT_TOL_DEG = 3.0    # a standing Jake clip must retarget within this of vertical
GROUND_TOL_M    = 0.12   # lowest planted foot must stay within this of the floor

# Object identities we must PRESERVE through source-import churn + cleanup: the
# target armature + the target's own mesh objects. Everything else (donor arms /
# donor meshes like Jake's grenade Icosphere) is removed before export.
TGT_ARM = None
TGT_MESHES = []

# target bone (.L/.R) -> Mixamo source bone.
BONE_MAP_MIXAMO = {
    "Hips": "mixamorigHips", "Spine": "mixamorigSpine", "Chest": "mixamorigSpine2",
    "Neck": "mixamorigNeck", "Head": "mixamorigHead",
    "Shoulder.L": "mixamorigLeftShoulder", "UpperArm.L": "mixamorigLeftArm",
    "LowerArm.L": "mixamorigLeftForeArm", "Hand.L": "mixamorigLeftHand",
    "Shoulder.R": "mixamorigRightShoulder", "UpperArm.R": "mixamorigRightArm",
    "LowerArm.R": "mixamorigRightForeArm", "Hand.R": "mixamorigRightHand",
    "UpperLeg.L": "mixamorigLeftUpLeg", "LowerLeg.L": "mixamorigLeftLeg",
    "Foot.L": "mixamorigLeftFoot",
    "UpperLeg.R": "mixamorigRightUpLeg", "LowerLeg.R": "mixamorigRightLeg",
    "Foot.R": "mixamorigRightFoot",
}
# target bone (.L/.R) -> Unity StarterAssets source bone.
BONE_MAP_STARTER = {
    "Hips": "Hips", "Spine": "Spine", "Chest": "Chest", "Neck": "Neck", "Head": "Head",
    "Shoulder.L": "Left_Shoulder", "UpperArm.L": "Left_UpperArm",
    "LowerArm.L": "Left_LowerArm", "Hand.L": "Left_Hand",
    "Shoulder.R": "Right_Shoulder", "UpperArm.R": "Right_UpperArm",
    "LowerArm.R": "Right_LowerArm", "Hand.R": "Right_Hand",
    "UpperLeg.L": "Left_UpperLeg", "LowerLeg.L": "Left_LowerLeg", "Foot.L": "Left_Foot",
    "UpperLeg.R": "Right_UpperLeg", "LowerLeg.R": "Right_LowerLeg", "Foot.R": "Right_Foot",
}
# identity: source is already on the .L/.R target rig.
BONE_MAP_LR = {k: k for k in BONE_MAP_STARTER.keys()}

# MESHY-humanoid TARGET rig (Meshy auto-rig, Mixamo-ish topology but "Left/Right"
# prefix, lowercase "neck", and a 3-bone spine whose names are REVERSED vs the
# hierarchy: Hips -> Spine02(base) -> Spine01(mid) -> Spine(top/chest, where
# shoulders + neck attach). This is the TARGET (keys) fed a ".L/.R"-rig SOURCE
# (values, e.g. chief_martinez_anim.glb) via rig type "meshy_from_lr". Spine01
# (mid) + toe/head-tip bones are intentionally unmapped -> they hold rest pose.
BONE_MAP_MESHY_FROM_LR = {
    "Hips": "Hips",
    "Spine02": "Spine",   # base spine (nearest Hips) <- chief base Spine
    "Spine": "Chest",     # top spine (shoulders/neck attach) <- chief Chest
    "neck": "Neck", "Head": "Head",
    "LeftShoulder": "Shoulder.L", "LeftArm": "UpperArm.L",
    "LeftForeArm": "LowerArm.L", "LeftHand": "Hand.L",
    "RightShoulder": "Shoulder.R", "RightArm": "UpperArm.R",
    "RightForeArm": "LowerArm.R", "RightHand": "Hand.R",
    "LeftUpLeg": "UpperLeg.L", "LeftLeg": "LowerLeg.L", "LeftFoot": "Foot.L",
    "RightUpLeg": "UpperLeg.R", "RightLeg": "LowerLeg.R", "RightFoot": "Foot.R",
}

# MESHY-humanoid TARGET fed a MIXAMO-rig SOURCE (e.g. Jake_22_actions.glb, which
# carries real Mixamo locomotion/combat clips the .L/.R character GLBs lack). Meshy
# spine is Hips->Spine02(base)->Spine01(mid)->Spine(top); Mixamo is
# Hips->Spine(base)->Spine1->Spine2(top) -- so all THREE spine bones map 1:1.
BONE_MAP_MESHY_FROM_MIXAMO = {
    "Hips": "mixamorigHips",
    "Spine02": "mixamorigSpine", "Spine01": "mixamorigSpine1", "Spine": "mixamorigSpine2",
    "neck": "mixamorigNeck", "Head": "mixamorigHead",
    "LeftShoulder": "mixamorigLeftShoulder", "LeftArm": "mixamorigLeftArm",
    "LeftForeArm": "mixamorigLeftForeArm", "LeftHand": "mixamorigLeftHand",
    "RightShoulder": "mixamorigRightShoulder", "RightArm": "mixamorigRightArm",
    "RightForeArm": "mixamorigRightForeArm", "RightHand": "mixamorigRightHand",
    "LeftUpLeg": "mixamorigLeftUpLeg", "LeftLeg": "mixamorigLeftLeg",
    "LeftFoot": "mixamorigLeftFoot",
    "RightUpLeg": "mixamorigRightUpLeg", "RightLeg": "mixamorigRightLeg",
    "RightFoot": "mixamorigRightFoot",
}

RIG_MAPS = {"mixamo": BONE_MAP_MIXAMO, "starter": BONE_MAP_STARTER, "lr": BONE_MAP_LR,
            "meshy_from_lr": BONE_MAP_MESHY_FROM_LR,
            "meshy_from_mixamo": BONE_MAP_MESHY_FROM_MIXAMO}


def _foot_keys(bmap):
    """TARGET foot-bone keys in a bone map (works for '.L/.R' -> Foot.L/Foot.R
    AND Meshy -> LeftFoot/RightFoot). Excludes toes."""
    return [k for k in bmap if "foot" in k.lower() and "toe" not in k.lower()]


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def import_target(keep_existing):
    bpy.ops.import_scene.gltf(filepath=TARGET_GLB)
    arm = next((o for o in bpy.data.objects if o.type == 'ARMATURE'), None)
    if not arm:
        raise RuntimeError("no armature in target GLB")
    arm.name = "TargetArm"
    arm.animation_data_clear()
    if keep_existing:
        # APPEND mode: keep the target GLB's own actions (Idle/Walk/Run/...); they
        # export alongside the freshly-baked clips. Fake-user so no active slot is
        # needed to survive the source-import churn.
        for a in bpy.data.actions:
            a.use_fake_user = True
        log("keep_existing: preserving", [a.name for a in bpy.data.actions])
    else:
        for a in list(bpy.data.actions):
            bpy.data.actions.remove(a)
    # Lock the character's textures in + pack IMMEDIATELY (before source churn).
    for img in bpy.data.images:
        try:
            if not img.has_data:
                _ = img.pixels[0]
            if not img.packed_file:
                img.pack()
            img.use_fake_user = True
        except Exception as e:
            log("  early-pack warn", img.name, e)
    for m in bpy.data.materials:
        m.use_fake_user = True
    global TGT_ARM, TGT_MESHES
    TGT_ARM = arm
    # ONLY the character's own skinned meshes: parented to the armature OR bound
    # via an Armature modifier that targets it. Excludes stray helper geometry
    # some rigs ship (e.g. Meshy auto-rigs embed an unskinned unit Icosphere) so
    # it neither corrupts the height measurement nor exports into the game asset.
    def _is_tgt_mesh(o):
        if o.type != 'MESH':
            return False
        if o.parent is arm:
            return True
        return any(m.type == 'ARMATURE' and getattr(m, 'object', None) is arm
                   for m in o.modifiers)
    TGT_MESHES = [o for o in bpy.data.objects if _is_tgt_mesh(o)]
    stray_objs = [o for o in bpy.data.objects if o.type == 'MESH' and o not in TGT_MESHES]
    if stray_objs:
        names = [o.name for o in stray_objs]
        # DELETE them now (object + orphaned mesh data) so they can never reach the
        # export -- a Meshy auto-rig embeds an unskinned unit Icosphere that would
        # otherwise ride along and, at -1 m, break the engine's toe-to-floor fit.
        for o in stray_objs:
            md = o.data
            try:
                bpy.data.objects.remove(o, do_unlink=True)
            except Exception as e:
                log("  stray-remove warn", o.name, e)
            try:
                if md and md.users == 0:
                    bpy.data.meshes.remove(md)
            except Exception:
                pass
        log("import_target: DELETED stray non-skinned meshes:", names)
    log("target armature:", arm.name, "bones:", len(arm.data.bones),
        "target meshes:", [o.name for o in TGT_MESHES],
        "images:", [(i.name, i.has_data) for i in bpy.data.images])
    return arm


# Cache of imported source armatures keyed by absolute path so a manifest that
# reuses one source (e.g. many Jake clips) imports it ONCE.
_SRC_CACHE = {}
# Actions that existed BEFORE any source import (target's kept actions + our
# baked clips) so import_source only tag-renames genuinely-new donor actions.
_KNOWN_ACTIONS = set()
_TAGS = ("SRC0_", "SRC1_", "SRC2_", "SRC3_", "SRC4_", "SRC5_", "SRC6_", "SRC7_")

def import_source(path):
    ap = os.path.abspath(path)
    if ap in _SRC_CACHE:
        return _SRC_CACHE[ap]
    before = set(bpy.data.objects)
    ext = os.path.splitext(ap)[1].lower()
    if ext == ".fbx":
        bpy.ops.import_scene.fbx(filepath=ap, ignore_leaf_bones=False,
                                 automatic_bone_orientation=True)
    else:
        bpy.ops.import_scene.gltf(filepath=ap)
    new = [o for o in bpy.data.objects if o not in before]
    src = next((o for o in new if o.type == 'ARMATURE'), None)
    if not src:
        raise RuntimeError("no armature in source " + path)
    tag = "SRC%d_" % len(_SRC_CACHE)
    src.name = "Src_" + tag
    if not src.animation_data:
        src.animation_data_create()
    # RENAME every action THIS import brought in with the tag prefix so (a) a
    # source "Idle"/"Hitreaction" can never bump our baked clip to "*.001", and
    # (b) cleanup removes donor actions by prefix. Only NEW actions are renamed
    # (the target's kept actions, imported earlier, are left alone).
    new_actions = [a for a in bpy.data.actions
                   if not a.name.startswith(("SRC0_", "SRC1_", "SRC2_", "SRC3_",
                                             "SRC4_", "SRC5_", "SRC6_", "SRC7_"))
                   and a not in _KNOWN_ACTIONS]
    for a in new_actions:
        a.name = tag + a.name
        _KNOWN_ACTIONS.add(a)
    _SRC_CACHE[ap] = (src, new, tag)
    log("imported source:", os.path.basename(ap), "arm:", src.name,
        "bones:", len(src.data.bones))
    return _SRC_CACHE[ap]


def find_source_action(src, tag, action_name):
    """Find THIS source's action by (case/space-insensitive) name. Source actions
    were renamed to "<tag><OrigName>" on import, so match tag + name."""
    key = action_name.lower().replace(" ", "")
    for a in bpy.data.actions:
        if not a.name.startswith(tag):
            continue
        if a.name[len(tag):].lower().replace(" ", "") == key:
            return a
    return None


def _depth_order(arm, names):
    depth = {}
    for n in names:
        b = arm.data.bones.get(n)
        d = 0
        while b is not None:
            b = b.parent; d += 1
        depth[n] = d
    return sorted(names, key=lambda n: depth[n])


def _hip_height(arm, root_name, foot_names):
    """Rest hip height = hips-head world Z MINUS the lowest foot-head world Z.
    Robust to a baked Armature-object Y/Z offset (Jake_22_actions shoves the
    Armature node down ~0.95 m, which would otherwise corrupt |hips.z| into a
    near-zero and blow up the vertical-bob scale). hips - foot cancels the
    offset and yields the TRUE pelvis-above-ground height. `root_name`/`foot_names`
    are the ACTUAL bone names on `arm` (already mapped by the caller)."""
    hb = arm.data.bones.get(root_name)
    if hb is None:
        return None
    hz = (arm.matrix_world @ hb.head_local).z
    fz = []
    for k in foot_names:
        fb = arm.data.bones.get(k)
        if fb is not None:
            fz.append((arm.matrix_world @ fb.head_local).z)
    ground = min(fz) if fz else 0.0
    h = hz - ground
    return h if h > 1e-3 else None


def _hips_rest_lean_deg(s_rest):
    """MEASURE the source rig's baked pelvis rest-lean: the angle of the Hips
    along-bone (local +Y) axis away from world +Z. DIAGNOSTIC ONLY -- see the
    caller: the rest-relative transfer already neutralizes a constant rest tilt,
    so nothing is applied. Jake_22_actions.glb reads ~5.2 deg here; a clean
    target-authored .L/.R rig reads ~0."""
    r = s_rest.get(ROOT_BONE)
    if r is None:
        return 0.0
    up_actual = (r @ Vector((0.0, 1.0, 0.0))).normalized()
    return math.degrees(up_actual.angle(Vector((0.0, 0.0, 1.0))))


def _assign_first_slot(arm, act):
    try:
        if getattr(act, "slots", None) is not None:
            if len(act.slots) == 0:
                act.slots.new(id_type='OBJECT', name=arm.name)
            arm.animation_data.action_slot = act.slots[0]
    except Exception as e:
        log("  slot assign warn:", e)


def retarget_one(tgt, src, tag, bmap, clip_name, action_name, fstart, fend):
    sact = find_source_action(src, tag, action_name)
    if sact is None:
        raise RuntimeError("source action not found: " + action_name)
    src.animation_data.action = sact
    _assign_first_slot(src, sact)
    # Whole-action range if not given.
    if fstart is None or fend is None:
        fr = sact.frame_range
        fstart = int(math.floor(fr[0])); fend = int(math.ceil(fr[1]))
    log("---- clip", clip_name, "from", sact.name, "frames", fstart, "-", fend)

    # Vertical-bob scale = target pelvis height / source pelvis height, both
    # measured hips-head-minus-foot-head so a baked Armature-object offset (Jake
    # is shoved down ~0.95 m) can't corrupt the ratio (was |hips.z| ~ 0.19 ->
    # 4.5x bob blow-up; feet-fly). Falls back to |hips.z| only if a foot is
    # missing from the map.
    tgt_foot_names = _foot_keys(bmap)                        # target-side foot bones
    src_foot_names = [bmap[k] for k in tgt_foot_names]       # source-side foot bones
    sh = _hip_height(src, bmap[ROOT_BONE], src_foot_names)
    th = _hip_height(tgt, ROOT_BONE, tgt_foot_names)         # target's own names
    if sh and th:
        loc_scale = th / sh
    else:
        b = src.data.bones.get(bmap[ROOT_BONE])
        sz = abs((src.matrix_world @ b.head_local).z) if b else 1.0
        tb = tgt.data.bones.get(ROOT_BONE)
        tz = abs((tgt.matrix_world @ tb.head_local).z) if tb else 1.0
        loc_scale = (tz / sz) if sz > 1e-6 else 1.0
    log("  loc_scale:", round(loc_scale, 4), "(src hip h", sh, "tgt hip h", th, ")")

    bpy.ops.object.mode_set(mode='OBJECT')
    bpy.context.view_layer.objects.active = tgt

    s_rest, t_rest = {}, {}
    for tbn, sbn in bmap.items():
        tbone = tgt.data.bones.get(tbn)
        sbone = src.data.bones.get(sbn)
        if tbone is None or sbone is None:
            continue
        t_rest[tbn] = (tgt.matrix_world @ tbone.matrix_local).to_3x3().normalized()
        s_rest[tbn] = (src.matrix_world @ sbone.matrix_local).to_3x3().normalized()
    mapped = [tbn for tbn in bmap if tbn in t_rest]
    order = _depth_order(tgt, mapped)
    log("  mapped bones:", len(mapped))

    # SOURCE pelvis rest-lean (diagnostic only -- NOT applied). Jake_22_actions
    # bakes ~5.2 deg of tilt into the Hips rest node, but the rest-relative
    # world-delta transfer (Dm = s_pose . s_rest^-1, applied onto t_rest) is
    # INVARIANT to a constant rest tilt: the target's absolute pose is
    # target_rest composed with the source's motion-FROM-its-own-rest, so a
    # standing (rest-following) source clip lands the target UPRIGHT and the rest
    # tilt never reaches it. A grounded Blender A/B (docs/screenshots/jakecrowd/
    # r_idle_*) confirmed that explicitly "zeroing" the tilt here instead INJECTS
    # a ~5 deg backward lean -- so we measure it, log it, and leave the transfer
    # untouched. The real Jake export bug (a ~0.95 m Armature-node Y offset that
    # blew the vertical-bob scale up ~4.5x) is fixed in loc_scale above.
    log("  source pelvis rest-lean:", round(_hips_rest_lean_deg(s_rest), 3),
        "deg (rest-relative transfer neutralizes it; not applied)")

    for pb in tgt.pose.bones:
        pb.rotation_mode = 'QUATERNION'

    if not tgt.animation_data:
        tgt.animation_data_create()
    act = bpy.data.actions.new(clip_name + "_tmp")
    act.use_fake_user = True
    tgt.animation_data.action = act
    _assign_first_slot(tgt, act)

    root_tbone = tgt.data.bones.get(ROOT_BONE)
    root_rest_head_world = tgt.matrix_world @ root_tbone.head_local
    src_root_pbone = src.pose.bones.get(bmap[ROOT_BONE])
    mid_frame = (fstart + fend) // 2   # representative frame for the upright assert
    # Target rest foot Z (the grounded reference) + the target foot pose bones.
    tgt_foot_pbs = [tgt.pose.bones.get(k) for k in tgt_foot_names
                    if tgt.pose.bones.get(k)]
    foot_rest_z = min([(tgt.matrix_world @ tgt.data.bones[p.name].head_local).z
                       for p in tgt_foot_pbs], default=0.0)
    min_foot_z = 1e9   # lowest planted foot across the clip (grounding guard)
    _mid_pelvis_lean = [0.0]   # captured at mid_frame (upright guard)

    out_frame = 1
    for f in range(fstart, fend + 1):
        bpy.context.scene.frame_set(f)
        bpy.context.view_layer.update()
        for tbn in order:
            sbn = bmap[tbn]
            spb = src.pose.bones.get(sbn)
            tpb = tgt.pose.bones.get(tbn)
            s_pose_w = (src.matrix_world @ spb.matrix).to_3x3().normalized()
            Dm = s_pose_w @ s_rest[tbn].inverted()
            t_pose_w = Dm @ t_rest[tbn]
            loc = tpb.matrix.translation.copy()
            new_m = (tgt.matrix_world.inverted().to_3x3() @ t_pose_w).to_4x4()
            new_m.translation = loc
            tpb.matrix = new_m
            bpy.context.view_layer.update()
            tpb.keyframe_insert("rotation_quaternion", frame=out_frame)
        # Root: keep clip IN-PLACE, preserve only vertical Hips bob.
        s_root_w = src.matrix_world @ src_root_pbone.matrix.translation
        s_root_rest_w = src.matrix_world @ src.data.bones[bmap[ROOT_BONE]].head_local
        delta = (s_root_w - s_root_rest_w) * loc_scale
        new_head = root_rest_head_world.copy()
        new_head.z += delta.z
        rpb = tgt.pose.bones.get(ROOT_BONE)
        m = rpb.matrix.copy()
        m.translation = tgt.matrix_world.inverted() @ new_head
        rpb.matrix = m
        rpb.keyframe_insert("location", frame=out_frame)
        rpb.keyframe_insert("rotation_quaternion", frame=out_frame)
        # GROUNDED assert: track the lowest planted foot across the clip -- with a
        # correct vertical-bob scale it stays near the target floor; the old
        # ~4.5x loc_scale blow-up flung the hips (and via FK the feet) off it.
        for p in tgt_foot_pbs:
            min_foot_z = min(min_foot_z, (tgt.matrix_world @ p.matrix.translation).z)
        # UPRIGHT assert (mid frame): a STANDING Jake clip must land the target
        # pelvis near-vertical (the rest-relative transfer neutralizes Jake's
        # rest tilt; a regressed "zero-the-tilt" would lean it ~5 deg).
        if f == mid_frame:
            t_up = ((tgt.matrix_world @ rpb.matrix).to_3x3().normalized()
                    @ Vector((0.0, 1.0, 0.0)))
            _mid_pelvis_lean[0] = math.degrees(t_up.angle(Vector((0.0, 0.0, 1.0))))
            # DIAG: source vs target foot/hand at the representative frame.
            def _w(a, bn):
                pb = a.pose.bones.get(bn)
                return tuple(round(x, 3) for x in (a.matrix_world @ pb.matrix.translation)) if pb else None
            log("  DIAG@%d src Foot.L=%s Hand.L=%s | tgt %s=%s %s=%s" % (
                f, _w(src, bmap.get("LeftFoot", "Foot.L")) or _w(src, "Foot.L"),
                _w(src, bmap.get("LeftHand", "Hand.L")) or _w(src, "Hand.L"),
                tgt_foot_names[0] if tgt_foot_names else "?",
                _w(tgt, tgt_foot_names[0]) if tgt_foot_names else None,
                "LeftHand", _w(tgt, "LeftHand")))
        out_frame += 1

    baked = tgt.animation_data.action
    baked.name = clip_name
    baked.use_fake_user = True
    log("  baked action:", baked.name, "range", tuple(baked.frame_range))
    # Record the grounded/upright assertion facts for this clip (main() gates).
    is_stance = any(k in action_name.lower() for k in ("idle", "stand", "tpose"))
    _ASSERTS.append({
        "clip": clip_name, "stance": is_stance,
        "pelvis_lean": round(_mid_pelvis_lean[0], 2),
        "foot_z": round(min_foot_z, 4), "foot_rest_z": round(foot_rest_z, 4),
        "foot_off": round(min_foot_z - foot_rest_z, 4),
    })
    tgt.animation_data.action = None
    return baked


def scale_target_to_height(author_h):
    """Uniformly scale the TARGET so its mesh is `author_h` metres tall (feet at
    world 0 stay at 0). Meshy rigs come pre-scaled to LORE height, but the canon
    GLB slot expects the PRE-modelScale AUTHORED height (canon_aliens.cpp applies
    modelScale on top). We scale the ARMATURE object node; the skinned mesh is
    driven purely by the joint matrices (glTF ignores a skinned mesh's own node
    transform), so scaling the armature node scales the rendered mesh. No
    transform_apply -> the scale is written into the exported armature node."""
    bpy.context.view_layer.update()
    lo, hi = 1e9, -1e9
    for m in TGT_MESHES:
        for c in m.bound_box:
            wz = (m.matrix_world @ Vector(c)).z
            lo = min(lo, wz); hi = max(hi, wz)
    cur = hi - lo
    if cur <= 1e-4:
        log("scale_target: could not measure height; skipping"); return
    f = author_h / cur
    TGT_ARM.scale = (TGT_ARM.scale.x * f, TGT_ARM.scale.y * f, TGT_ARM.scale.z * f)
    bpy.context.view_layer.update()
    lo2, hi2 = 1e9, -1e9
    for m in TGT_MESHES:
        for c in m.bound_box:
            wz = (m.matrix_world @ Vector(c)).z
            lo2 = min(lo2, wz); hi2 = max(hi2, wz)
    log("scale_target: %.3f m -> %.3f m (x%.4f), new range [%.3f, %.3f]"
        % (cur, author_h, f, lo2, hi2))


def main():
    with open(MANIFEST, "r", encoding="utf-8") as f:
        man = json.load(f)
    keep = bool(man.get("keep_existing", True))
    clips = man.get("clips", [])
    author_h = man.get("author_height")   # optional: pre-modelScale mesh height (m)

    reset()
    tgt = import_target(keep)
    if author_h:
        scale_target_to_height(float(author_h))
    # Snapshot the actions present before any donor import (target's kept clips).
    for a in bpy.data.actions:
        _KNOWN_ACTIONS.add(a)

    baked = []
    for c in clips:
        rig = c.get("rig", "mixamo")
        bmap = RIG_MAPS.get(rig)
        if bmap is None:
            log("SKIP clip", c.get("name"), "- unknown rig type", rig); continue
        try:
            src, src_objs, tag = import_source(c["src"])
            act = retarget_one(tgt, src, tag, bmap, c["name"], c["action"],
                               c.get("start"), c.get("end"))
            # Register the freshly-baked clip as KNOWN so a LATER source import
            # (a manifest that mixes sources) can't mistake it for a new donor
            # action and tag/remove it -- without this only the last source's
            # clips survive the tag-prefix cleanup.
            _KNOWN_ACTIONS.add(act)
            baked.append((act.name, rig, c.get("action"), c.get("loop", False)))
        except Exception as e:
            import traceback
            log("CLIP FAILED", c.get("name"), ":", e); log(traceback.format_exc())

    log("baked clips:", baked)
    if not baked:
        raise RuntimeError("no clips baked")

    # GROUNDED + UPRIGHT assertion gate. Every baked clip must stay grounded;
    # standing clips must also land near-vertical. Fails the run on a violation.
    bad = []
    for a in _ASSERTS:
        grounded = abs(a["foot_off"]) <= GROUND_TOL_M
        upright = (not a["stance"]) or a["pelvis_lean"] <= UPRIGHT_TOL_DEG
        ok = grounded and upright
        log("  RETARGET-ASSERT", "PASS" if ok else "FAIL", a["clip"],
            "stance=%s pelvis_lean=%.2f deg foot_off=%.3f m"
            % (a["stance"], a["pelvis_lean"], a["foot_off"]))
        if not ok:
            bad.append(a["clip"] + ("(leaning)" if not upright else "") +
                       ("(ungrounded)" if not grounded else ""))
    # The numeric grounded/upright gate assumes the .L/.R target rig's bone-axis
    # conventions (Hips local +Y == world up, foot-head near floor). Other target
    # rigs (e.g. Meshy: Hips +Y is not world-up, foot bones are disconnected) trip
    # it with FALSE negatives, so a manifest may set "skip_assert": true to make it
    # ADVISORY -- the grounded Blender render is then the ground-truth QA gate.
    if bad:
        if man.get("skip_assert"):
            log("  RETARGET-ASSERT advisory only (skip_assert set); NOT gating on:",
                ", ".join(bad), "-- verify via grounded render")
        else:
            raise RuntimeError("grounded/upright assertion FAILED for: " + ", ".join(bad))

    # Remove EVERY object that is not the target armature or one of the target's
    # own meshes (donor armatures + donor meshes like Jake's grenade Icosphere),
    # then drop all tag-prefixed donor actions. Keeps only target arm/meshes +
    # target's kept clips + our baked clips.
    for src, objs, tag in _SRC_CACHE.values():
        try:
            src.animation_data_clear()
        except Exception:
            pass
    keepobjs = set([tgt]) | set(TGT_MESHES)
    for o in list(bpy.data.objects):
        if o not in keepobjs:
            try:
                bpy.data.objects.remove(o, do_unlink=True)
            except Exception:
                pass
    for a in list(bpy.data.actions):
        if any(a.name.startswith(t) for t in _TAGS):
            try:
                bpy.data.actions.remove(a)
            except Exception:
                pass

    for img in bpy.data.images:
        try:
            if not img.has_data:
                _ = img.pixels[0]
            if not img.packed_file:
                img.pack()
        except Exception as e:
            log("  pack warn", img.name, e)
    log("pre-export actions:", [a.name for a in bpy.data.actions],
        "meshes:", [o.name for o in bpy.data.objects if o.type == 'MESH'],
        "images:", [(i.name, bool(i.packed_file)) for i in bpy.data.images])

    os.makedirs(os.path.dirname(OUT_GLB) or ".", exist_ok=True)
    bpy.ops.export_scene.gltf(
        filepath=OUT_GLB, export_format='GLB', export_yup=True, use_selection=False,
        export_animations=True, export_animation_mode='ACTIONS',
        export_nla_strips=False, export_force_sampling=True, export_apply=False,
        export_materials='EXPORT', export_image_format='AUTO',
        export_texcoords=True, export_normals=True, export_tangents=True,
        export_skins=True)
    log("EXPORTED:", OUT_GLB)
    log("RETARGET OK clips:", ",".join(a for a, _, _, _ in baked))


if __name__ == "__main__":
    status = "OK"
    try:
        main()
    except Exception as e:
        import traceback
        log("FAILED:", e); log(traceback.format_exc()); status = "FAIL: " + str(e)
    flush_log(status)
