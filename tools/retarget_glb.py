# =============================================================================
# Headless Blender GLB -> GLB clip retargeter for the X3Native asset pipeline.
#
#   blender --background --python tools/retarget_glb.py -- \
#       <target.glb> <source.glb> <out.glb> [Src=Dst,Src2=Dst2,...]
#
# WHY THIS EXISTS, and why the obvious approach fails.
#
# Two characters can share a skeleton's BONE NAMES exactly and still be
# impossible to transfer clips between by copying animation channels. That is
# the Sarah case: Sarah.glb and JakeClone_player.glb both carry the Meshy
# 24-joint rig, byte-identical in joint name AND order -- yet their REST POSES
# differ by
#       LeftArm 94.5 deg   RightArm 86.2 deg   LeftUpLeg 39.2 deg   Hips 39.0 deg
# (measured 2026-08-15). A glTF rotation channel is a bone's LOCAL rotation,
# which only means the same thing on both rigs if both rigs rest in the same
# pose. Copy those channels across a 90-degree rest delta and the arms end up
# 90 degrees wrong: the figure splays and inflates. That is exactly what the
# 2026-07-27 attempt produced ("DEFORMS her badly"), and what a direct
# tools/glb-merge-anims.mjs merge produces too -- the merge is CORRECT for its
# own job (same-rig variants) and simply cannot express a rest-pose change.
#
# THE FIX is to transfer WORLD-SPACE ORIENTATION instead of local rotation:
#
#       D          = R_src_posed_world * R_src_rest_world^-1      (what the
#                                                                  source bone
#                                                                  actually DID)
#       R_tgt_world = D * R_tgt_rest_world                        (do that same
#                                                                  thing to the
#                                                                  target's rest)
#
# D is the motion with the source's rest pose divided out, so it carries no
# assumption about either rig's rest orientation or bone roll. Bones are solved
# parent-first so each child reads an already-updated parent world matrix. This
# is the same math tools/retarget_anims.py uses; that script is hardcoded to one
# rig pair and FBX sources, this one takes any two GLBs and maps bones by name.
#
# Root handling matches retarget_anims.py: clips stay IN PLACE (the engine drives
# world position), but the source's VERTICAL hip travel is kept -- scaled by the
# hip-height ratio, so a taller source does not make a shorter target bob more --
# because without it a gait reads as a glide.
#
# Clean-room: built from the public Blender Python API + the glTF 2.0 spec only.
# =============================================================================
import bpy, sys, os

ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(ARGV) < 3:
    print("ERR: need <target.glb> <source.glb> <out.glb> [Src=Dst,...]")
    sys.exit(1)
TARGET_GLB, SOURCE_GLB, OUT_GLB = ARGV[0], ARGV[1], ARGV[2]

# Optional "SourceClip=OutputName,..." selection. When omitted every source
# action is retargeted under its own name.
CLIP_MAP = {}
if len(ARGV) > 3 and ARGV[3].strip():
    for pair in ARGV[3].split(","):
        if "=" in pair:
            s, d = pair.split("=", 1)
            CLIP_MAP[s.strip()] = d.strip()
        elif pair.strip():
            CLIP_MAP[pair.strip()] = pair.strip()

ROOT_BONE = "Hips"


def log(*a): print("[retarget-glb]", *a)


def _depth_order(arm, names):
    depth = {}
    for n in names:
        b = arm.data.bones.get(n); d = 0
        while b is not None:
            b = b.parent; d += 1
        depth[n] = d
    return sorted(names, key=lambda n: depth[n])


def _assign_first_slot(arm, act):
    """Blender 5.x slotted actions: ensure a slot exists + is assigned so
    keyframe_insert lands in this action."""
    try:
        if getattr(act, "slots", None) is not None:
            if len(act.slots) == 0:
                act.slots.new(id_type='OBJECT', name=arm.name)
            arm.animation_data.action_slot = act.slots[0]
    except Exception as e:
        log("  slot assign warn:", e)


def import_target():
    bpy.ops.import_scene.gltf(filepath=TARGET_GLB)
    arm = next((o for o in bpy.data.objects if o.type == 'ARMATURE'), None)
    if not arm:
        raise RuntimeError("no armature in target GLB")
    arm.name = "TargetArm"
    arm.animation_data_clear()
    for a in list(bpy.data.actions):
        bpy.data.actions.remove(a)
    # Lock the target's textures into memory and pack them BEFORE the source
    # import churns the datablocks -- this is what keeps them in the export.
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
    log("target:", os.path.basename(TARGET_GLB), "bones:", len(arm.data.bones))
    return arm


def import_source():
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=SOURCE_GLB)
    new = [o for o in bpy.data.objects if o not in before]
    src = next((o for o in new if o.type == 'ARMATURE'), None)
    if not src:
        raise RuntimeError("no armature in source GLB")
    src.name = "SourceArm"
    log("source:", os.path.basename(SOURCE_GLB), "bones:", len(src.data.bones),
        "actions:", len(bpy.data.actions))
    return src, new


def retarget_one(tgt, src, action, out_name, bones, loc_scale):
    """Bake ONE source action onto the target armature; return the new action."""
    if not src.animation_data:
        src.animation_data_create()
    src.animation_data.action = action
    _assign_first_slot(src, action)

    fstart, fend = (int(round(v)) for v in action.frame_range)
    if fend <= fstart:
        fend = fstart + 1

    # Rest WORLD rotations for both rigs (rotation only -- scale/translation of
    # the rest pose are irrelevant to an orientation transfer).
    s_rest, t_rest = {}, {}
    for b in bones:
        s_rest[b] = (src.matrix_world @ src.data.bones[b].matrix_local).to_3x3().normalized()
        t_rest[b] = (tgt.matrix_world @ tgt.data.bones[b].matrix_local).to_3x3().normalized()
    order = _depth_order(tgt, bones)

    for pb in tgt.pose.bones:
        pb.rotation_mode = 'QUATERNION'
    if not tgt.animation_data:
        tgt.animation_data_create()
    act = bpy.data.actions.new(out_name + "_tmp")
    act.use_fake_user = True
    tgt.animation_data.action = act
    _assign_first_slot(tgt, act)

    root_tbone = tgt.data.bones.get(ROOT_BONE)
    root_rest_head_world = tgt.matrix_world @ root_tbone.head_local
    src_root_pbone = src.pose.bones.get(ROOT_BONE)
    src_root_rest_w = src.matrix_world @ src.data.bones[ROOT_BONE].head_local

    out_frame = 1
    for f in range(fstart, fend + 1):
        bpy.context.scene.frame_set(f)
        bpy.context.view_layer.update()
        for b in order:                      # parent-first
            spb = src.pose.bones[b]; tpb = tgt.pose.bones[b]
            s_pose_w = (src.matrix_world @ spb.matrix).to_3x3().normalized()
            D = s_pose_w @ s_rest[b].inverted()      # source motion, rest divided out
            t_pose_w = D @ t_rest[b]                 # same motion on target's rest
            loc = tpb.matrix.translation.copy()      # rotation only
            new_m = (tgt.matrix_world.inverted().to_3x3() @ t_pose_w).to_4x4()
            new_m.translation = loc
            tpb.matrix = new_m
            bpy.context.view_layer.update()          # children read updated parent
            tpb.keyframe_insert("rotation_quaternion", frame=out_frame)
        # Root: in place horizontally, vertical bob preserved and scaled.
        s_root_w = src.matrix_world @ src_root_pbone.matrix.translation
        delta = (s_root_w - src_root_rest_w) * loc_scale
        new_head = root_rest_head_world.copy()
        new_head.z += delta.z
        rpb = tgt.pose.bones[ROOT_BONE]
        m = rpb.matrix.copy()
        m.translation = tgt.matrix_world.inverted() @ new_head
        rpb.matrix = m
        rpb.keyframe_insert("location", frame=out_frame)
        rpb.keyframe_insert("rotation_quaternion", frame=out_frame)
        out_frame += 1

    baked = tgt.animation_data.action
    baked.name = out_name
    baked.use_fake_user = True
    log(f"  baked '{out_name}' from '{action.name}' frames {fstart}-{fend}")
    return baked


def main():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    tgt = import_target()
    src, src_objs = import_source()

    # Bones present on BOTH rigs, by name. Identity map -- these rigs share the
    # naming convention; what they do NOT share is the rest pose, which is
    # precisely what the world-space transfer above absorbs.
    bones = [b.name for b in tgt.data.bones if src.data.bones.get(b.name)]
    if ROOT_BONE not in bones:
        raise RuntimeError(f"'{ROOT_BONE}' missing from one of the rigs")
    log("shared bones:", len(bones), "of", len(tgt.data.bones))

    # Scale the hip bob by the hip-height ratio so a taller source does not make
    # a shorter target bob further than its own legs allow.
    s_h = abs((src.matrix_world @ src.data.bones[ROOT_BONE].head_local).z) or 1.0
    t_h = abs((tgt.matrix_world @ tgt.data.bones[ROOT_BONE].head_local).z) or 1.0
    loc_scale = t_h / s_h
    log(f"hip heights: src={s_h:.3f} tgt={t_h:.3f} -> loc_scale={loc_scale:.3f}")

    todo = [a for a in bpy.data.actions if not CLIP_MAP or a.name in CLIP_MAP]
    if not todo:
        raise RuntimeError("no source actions matched: " + ",".join(CLIP_MAP))

    baked = []
    for action in list(todo):
        out_name = CLIP_MAP.get(action.name, action.name)
        try:
            baked.append(retarget_one(tgt, src, action, out_name, bones, loc_scale))
        except Exception as e:
            import traceback
            log("CLIP FAILED", action.name, ":", e)
            traceback.print_exc()
    if not baked:
        raise RuntimeError("no clips baked")

    # Drop the source objects and every action that is not one of ours, so the
    # exporter emits exactly the retargeted set.
    keep = {a.name for a in baked}
    for o in src_objs:
        bpy.data.objects.remove(o, do_unlink=True)
    for a in list(bpy.data.actions):
        if a.name not in keep:
            bpy.data.actions.remove(a)

    for img in bpy.data.images:
        try:
            if not img.has_data:
                _ = img.pixels[0]
            if not img.packed_file:
                img.pack()
        except Exception as e:
            log("  pack warn", img.name, e)

    outdir = os.path.dirname(OUT_GLB)
    if outdir:
        os.makedirs(outdir, exist_ok=True)
    bpy.ops.export_scene.gltf(
        filepath=OUT_GLB, export_format='GLB', export_yup=True,
        use_selection=False, export_animations=True,
        export_animation_mode='ACTIONS', export_nla_strips=False,
        export_force_sampling=True, export_apply=False,
        export_materials='EXPORT', export_image_format='AUTO',
        export_texcoords=True, export_normals=True, export_tangents=True,
        export_skins=True,
    )
    log("EXPORTED:", OUT_GLB)
    log("RETARGET OK clips:", ",".join(a.name for a in baked))


main()
