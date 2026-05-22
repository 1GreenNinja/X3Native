# =============================================================================
# Headless Blender locomotion retargeter for the X3Native asset pipeline.
#
#   blender --background --python retarget_anims.py -- <target.glb> <out.glb>
#
# Produces ONE multi-clip GLB (Idle + Walk + Run + Jump) for a single character
# by retargeting the Unity StarterAssets ThirdPerson locomotion FBX clips onto
# the character's own armature. The character GLBs (chief_martinez / marcus_webb)
# use a 19-bone humanoid rig with ".L/.R" names; the StarterAssets source uses a
# 68-bone rig with "Left_/Right_" names and a DIFFERENT rest pose, so a direct
# action copy is wrong. We retarget by world-space orientation transfer:
#
#   For each source clip we drive a duplicate of the target armature with
#   Copy Rotation (world space) + Copy Location (root only, for Hips) constraints
#   from the source pose bones via a bone-name map, then BAKE the result into a
#   new action on the target armature. World-space Copy Rotation transfers each
#   source bone's animated world orientation regardless of differing bone rolls /
#   rest poses, which is the orientation-safe way to retarget across rigs that
#   share a humanoid topology but not a rest pose.
#
# Clean-room: built from the public Blender Python API + glTF 2.0 spec only.
# No GPL / id Tech / RBDOOM source consulted.
# =============================================================================
import bpy, sys, os, math
from mathutils import Vector

ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(ARGV) < 2:
    print("ERR: need <target.glb> <out.glb>")
    sys.exit(1)
TARGET_GLB, OUT_GLB = ARGV[0], ARGV[1]

SA = ("G:/Unity_Projects/EscapeFromLabZero/Assets/StarterAssets/"
      "ThirdPersonController/Character/Animations/")

# Each entry: (clip_name, fbx_path, frame_start, frame_end, loop)
# Frame ranges come from inspect_skel.py on each source FBX.
SRC_CLIPS = [
    ("Idle", SA + "Stand--Idle.anim.fbx",          2,  150, True),
    ("Walk", SA + "Locomotion--Walk_N.anim.fbx",   1,  30,  True),
    ("Run",  SA + "Locomotion--Run_N.anim.fbx",    1,  21,  True),
    ("Jump", SA + "Jump--Jump.anim.fbx",           21, 75,  False),
]

# Map: target bone (.L/.R rig) -> source bone (Left_/Right_ rig).
# Only the bones present on BOTH rigs that drive locomotion are mapped.
BONE_MAP = {
    "Hips":        "Hips",
    "Spine":       "Spine",
    "Chest":       "Chest",
    "Neck":        "Neck",
    "Head":        "Head",
    "Shoulder.L":  "Left_Shoulder",
    "UpperArm.L":  "Left_UpperArm",
    "LowerArm.L":  "Left_LowerArm",
    "Hand.L":      "Left_Hand",
    "Shoulder.R":  "Right_Shoulder",
    "UpperArm.R":  "Right_UpperArm",
    "LowerArm.R":  "Right_LowerArm",
    "Hand.R":      "Right_Hand",
    "UpperLeg.L":  "Left_UpperLeg",
    "LowerLeg.L":  "Left_LowerLeg",
    "Foot.L":      "Left_Foot",
    "UpperLeg.R":  "Right_UpperLeg",
    "LowerLeg.R":  "Right_LowerLeg",
    "Foot.R":      "Right_Foot",
}
ROOT_BONE = "Hips"   # only this bone also copies (scaled) location


def log(*a): print("[retarget]", *a)


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def find_armature():
    return next((o for o in bpy.data.objects if o.type == 'ARMATURE'), None)


def import_target():
    bpy.ops.import_scene.gltf(filepath=TARGET_GLB)
    arm = find_armature()
    if not arm:
        raise RuntimeError("no armature in target GLB")
    arm.name = "TargetArm"
    arm.animation_data_clear()
    # Purge ALL pre-existing actions (the GLB's original "Idle"); we only want
    # our freshly-baked clips in the output, named exactly Idle/Walk/Run/Jump.
    for a in list(bpy.data.actions):
        bpy.data.actions.remove(a)
    # Lock the character's textures into memory + pack them IMMEDIATELY, before
    # any source-FBX import churn can evict the original GLB's image data. Doing
    # this early is what reliably keeps the textures in the exported GLB.
    for img in bpy.data.images:
        try:
            if not img.has_data:
                _ = img.pixels[0]      # force decode into memory
            if not img.packed_file:
                img.pack()
            img.use_fake_user = True   # protect from orphan-purge during churn
        except Exception as e:
            log("  early-pack warn", img.name, e)
    # Also fake-user the materials so source-object removal can't orphan them.
    for m in bpy.data.materials:
        m.use_fake_user = True
    log("target armature:", arm.name, "bones:", len(arm.data.bones),
        "images:", [(i.name, i.has_data) for i in bpy.data.images])
    return arm


def import_source(fbx):
    """Import a source FBX into the current scene; return its armature object."""
    before = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=fbx, ignore_leaf_bones=False,
                             automatic_bone_orientation=True)
    new = [o for o in bpy.data.objects if o not in before]
    src = next((o for o in new if o.type == 'ARMATURE'), None)
    if not src:
        raise RuntimeError("no armature in source " + fbx)
    return src, new


def get_action(arm):
    ad = arm.animation_data
    return ad.action if ad else None


def src_world_hips_height(src):
    b = src.data.bones.get(BONE_MAP[ROOT_BONE])
    if not b: return 1.0
    return abs((src.matrix_world @ b.head_local).z)


def tgt_world_hips_height(tgt):
    b = tgt.data.bones.get(ROOT_BONE)
    if not b: return 1.0
    return abs((tgt.matrix_world @ b.head_local).z)


def retarget_one(tgt, clip_name, fbx, fstart, fend):
    log("---- clip", clip_name, "from", os.path.basename(fbx),
        "frames", fstart, "-", fend)
    actions_before = set(bpy.data.actions)
    src, src_objs = import_source(fbx)
    src_actions = [a for a in bpy.data.actions if a not in actions_before]

    # Make the source action active on the source armature.
    sact = get_action(src)
    if sact is None:
        sact = src_actions[-1] if src_actions else bpy.data.actions[-1]
        if not src.animation_data:
            src.animation_data_create()
        src.animation_data.action = sact
    log("  source action:", sact.name)

    # Root-translation scale (source FBX is cm-scale; target GLB is m-scale).
    # Use the rest WORLD head height of Hips above the armature origin as the
    # length yardstick (robust to per-FBX import scale).
    sz = src_world_hips_height(src)
    tz = tgt_world_hips_height(tgt)
    loc_scale = (tz / sz) if sz > 1e-6 else 1.0
    log("  src hips world h=%.3f tgt=%.3f loc_scale=%.5f" % (sz, tz, loc_scale))

    # ---- Rest-relative delta retarget (orientation-safe across differing rest
    # poses / bone rolls). For each mapped bone and frame:
    #   D = R_src_pose_world * R_src_rest_world^-1      (animated world delta)
    #   R_tgt_pose_world = D * R_tgt_rest_world         (apply to target rest)
    #   R_tgt_local = R_tgt_parent_pose_world^-1 * R_tgt_pose_world
    # We resolve target bones parent-first so parent world poses are current.
    bpy.ops.object.mode_set(mode='OBJECT')
    bpy.context.view_layer.objects.active = tgt

    # Precompute rest WORLD matrices (rotation only) for both rigs.
    s_rest = {}   # src bone name -> 3x3 world rest rotation
    t_rest = {}   # tgt bone name -> 3x3 world rest rotation
    for tb, sb in BONE_MAP.items():
        tbone = tgt.data.bones.get(tb)
        sbone = src.data.bones.get(sb)
        if tbone is None or sbone is None:
            continue
        t_rest[tb] = (tgt.matrix_world @ tbone.matrix_local).to_3x3().normalized()
        s_rest[tb] = (src.matrix_world @ sbone.matrix_local).to_3x3().normalized()
    mapped = [tb for tb in BONE_MAP if tb in t_rest]
    # Order target bones parent-first (root -> leaves).
    order = _depth_order(tgt, mapped)
    log("  mapped bones:", len(mapped))

    # Pose bones must use quaternion rotation so keyframe_insert("rotation_quaternion")
    # is the channel that drives them.
    for pb in tgt.pose.bones:
        pb.rotation_mode = 'QUATERNION'

    # Fresh action on the target for this clip.
    if not tgt.animation_data:
        tgt.animation_data_create()
    act = bpy.data.actions.new(clip_name + "_tmp")
    act.use_fake_user = True
    tgt.animation_data.action = act
    _assign_first_slot(tgt, act)

    # Root rest world translation (target) for re-basing Hips location.
    root_tbone = tgt.data.bones.get(ROOT_BONE)
    root_rest_head_world = tgt.matrix_world @ root_tbone.head_local
    src_root_pbone = src.pose.bones.get(BONE_MAP[ROOT_BONE])

    out_frame = 1
    for f in range(fstart, fend + 1):
        bpy.context.scene.frame_set(f)
        bpy.context.view_layer.update()
        # Per-frame target world pose rotations we compute, keyed by bone name.
        tgt_world = {}
        for tb in order:
            sb = BONE_MAP[tb]
            spb = src.pose.bones.get(sb)
            tpb = tgt.pose.bones.get(tb)
            # source animated world rotation:
            s_pose_w = (src.matrix_world @ spb.matrix).to_3x3().normalized()
            D = s_pose_w @ s_rest[tb].inverted()
            t_pose_w = D @ t_rest[tb]
            tgt_world[tb] = t_pose_w
            # Assign the bone's pose matrix in ARMATURE-OBJECT space (what
            # PoseBone.matrix expects). Preserve the bone's current translation;
            # only the rotation is retargeted (Hips translation handled below).
            loc = tpb.matrix.translation.copy()
            new_m = (tgt.matrix_world.inverted().to_3x3() @ t_pose_w).to_4x4()
            new_m.translation = loc
            tpb.matrix = new_m
            bpy.context.view_layer.update()   # so children read updated parent
            tpb.keyframe_insert("rotation_quaternion", frame=out_frame)
        # Root translation: keep the clip IN-PLACE (the engine drives world
        # position from player movement) but preserve the vertical Hips bob so
        # the gait reads correctly. Take only the source Hips delta from its own
        # rest, scaled, and apply ONLY the vertical (Z) part on top of the
        # target's rest Hips position; horizontal drift is zeroed.
        s_root_w = src.matrix_world @ src_root_pbone.matrix.translation
        s_root_rest_w = src.matrix_world @ src.data.bones[BONE_MAP[ROOT_BONE]].head_local
        delta = (s_root_w - s_root_rest_w) * loc_scale
        new_head = root_rest_head_world.copy()
        new_head.z += delta.z          # vertical bob only; X/Y stay in place
        rpb = tgt.pose.bones.get(ROOT_BONE)
        m = rpb.matrix.copy()
        m.translation = tgt.matrix_world.inverted() @ new_head
        rpb.matrix = m
        rpb.keyframe_insert("location", frame=out_frame)
        rpb.keyframe_insert("rotation_quaternion", frame=out_frame)
        out_frame += 1

    baked = tgt.animation_data.action
    # Remove the raw source actions so the export stays tidy.
    for a in src_actions:
        if a is not baked and a.name in bpy.data.actions:
            try: bpy.data.actions.remove(a)
            except Exception: pass
    baked.name = clip_name
    baked.use_fake_user = True
    log("  baked action:", baked.name, "range", tuple(baked.frame_range))

    # Detach the action from the target so the next clip retargets fresh.
    tgt.animation_data.action = None

    # Remove the imported source objects to keep the scene clean.
    for o in src_objs:
        bpy.data.objects.remove(o, do_unlink=True)
    return baked


def _depth_order(arm, names):
    """Return `names` ordered root-first by armature bone hierarchy depth."""
    depth = {}
    for n in names:
        b = arm.data.bones.get(n)
        d = 0
        while b is not None:
            b = b.parent; d += 1
        depth[n] = d
    return sorted(names, key=lambda n: depth[n])


def _assign_first_slot(arm, act):
    """Blender 5.0 slotted actions: ensure a slot exists + is assigned so
    keyframe_insert lands in this action."""
    try:
        if getattr(act, "slots", None) is not None:
            if len(act.slots) == 0:
                act.slots.new(id_type='OBJECT', name=arm.name)
            arm.animation_data.action_slot = act.slots[0]
    except Exception as e:
        log("  slot assign warn:", e)


def _action_fcurves(act):
    if hasattr(act, "fcurves") and len(act.fcurves):
        return list(act.fcurves)
    fcs = []
    for layer in getattr(act, "layers", []):
        for strip in getattr(layer, "strips", []):
            for cb in getattr(strip, "channelbags", []):
                fcs.extend(cb.fcurves)
    return fcs


def main():
    reset()
    tgt = import_target()

    baked_actions = []
    for clip_name, fbx, fs, fe, _loop in SRC_CLIPS:
        if not os.path.exists(fbx):
            log("MISSING source FBX, skipping:", fbx)
            continue
        try:
            act = retarget_one(tgt, clip_name, fbx, fs, fe)
            baked_actions.append(act)
        except Exception as e:
            import traceback
            log("CLIP FAILED", clip_name, ":", e)
            traceback.print_exc()

    log("baked clips:", [a.name for a in baked_actions])
    if not baked_actions:
        raise RuntimeError("no clips baked")

    # Defensive: re-confirm textures are resident + packed (they were locked in
    # right after import; this is a cheap belt-and-suspenders before export).
    for img in bpy.data.images:
        try:
            if not img.has_data:
                _ = img.pixels[0]
            if not img.packed_file:
                img.pack()
        except Exception as e:
            log("  pack warn", img.name, e)
    log("pre-export images:", [(i.name, bool(i.packed_file)) for i in bpy.data.images],
        "materials:", [m.name for m in bpy.data.materials])

    # Export ONE multi-clip GLB. export_animation_mode='ACTIONS' makes the
    # exporter emit every action with a fake user as a separate glTF animation.
    os.makedirs(os.path.dirname(OUT_GLB), exist_ok=True)
    # Select target + its mesh children for export.
    for o in bpy.context.selected_objects:
        o.select_set(False)
    tgt.select_set(True)
    for o in bpy.data.objects:
        if o.type == 'MESH':
            o.select_set(True)

    bpy.ops.export_scene.gltf(
        filepath=OUT_GLB,
        export_format='GLB',
        export_yup=True,
        use_selection=False,
        export_animations=True,
        export_animation_mode='ACTIONS',
        export_nla_strips=False,
        export_force_sampling=True,
        export_apply=False,
        # Keep the character's PBR material + its diffuse/normal textures so the
        # output GLB renders identically to the original (the source GLB embeds
        # 2x 2048 textures; without these the GLB ships meshes-only at ~0.2 MB).
        export_materials='EXPORT',
        export_image_format='AUTO',
        export_texcoords=True,
        export_normals=True,
        export_tangents=True,
        export_skins=True,
    )
    log("EXPORTED:", OUT_GLB)
    log("RETARGET OK clips:", ",".join(a.name for a in baked_actions))


main()
