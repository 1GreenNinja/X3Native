"""
retarget_from_jake.py - headless Blender retargeter that bakes a companion's
locomotion + talk set from Jake_22_actions.glb onto a humanoid ".L/.R" rig.

    blender-launcher.exe --background --python retarget_from_jake.py -- \
        <target.glb> <jake.glb> <out.glb>

Produces ONE multi-clip GLB (Idle + Walk + Run + Talk) for a single humanoid
character (e.g. AnnaCasual.glb) by retargeting Jake's in-file Mixamo actions onto
the target's own ".L/.R" armature, then synthesizing a Talk gesture on the
target rig (Jake has no pure talk clip). This is the same world-space, rest-
relative orientation transfer used by retarget_anims.py (which produced
chief_martinez_anim.glb / marcus_webb_anim.glb), only the SOURCE is Jake's GLB
actions instead of the now-removed StarterAssets FBX clips.

  For each source clip we read Jake's animated WORLD orientation per mapped bone:
      D = R_src_pose_world * R_src_rest_world^-1      (animated world delta)
      R_tgt_pose_world = D * R_tgt_rest_world         (apply to target rest)
  then convert to the target bone's armature-space matrix and keyframe it. This
  is orientation-safe across rigs that share humanoid topology but not rest pose
  (Mixamo T-pose vs the target's A/rest pose, different bone rolls).

ENV NOTE (this box): Blender is the Microsoft Store package - direct blender.exe
is ACL-denied and blender-launcher.exe DETACHES, so we report through files: a
sidecar <out>.log and a <out>.done marker the caller polls. ASCII-only source.

Clean-room: built from the public Blender Python API + glTF 2.0 spec only.
No GPL / id Tech / RBDOOM source consulted.
"""
import bpy, sys, os, math
from mathutils import Vector

ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(ARGV) < 3:
    raise SystemExit("Usage: ... -- <target.glb> <jake.glb> <out.glb>")
TARGET_GLB, JAKE_GLB, OUT_GLB = ARGV[0], ARGV[1], ARGV[2]

LOG_PATH  = OUT_GLB + ".log"
DONE_PATH = OUT_GLB + ".done"
_log = []
def log(*a):
    s = "[jake-retarget] " + " ".join(str(x) for x in a)
    _log.append(s); print(s)

def flush_log(status):
    try:
        with open(LOG_PATH, "w", encoding="utf-8") as f:
            f.write("\n".join(_log))
        with open(DONE_PATH, "w", encoding="utf-8") as f:
            f.write(status)
    except Exception as e:
        print("[jake-retarget] could not write log/marker:", e)

# Each entry: (clip_name, jake_action_name, frame_start, frame_end)
# Frame windows chosen from skel_dump of Jake_22_actions.glb. Idle's source is
# 0..197 (very long); we take one clean settled loop window.
SRC_CLIPS = [
    ("Idle", "Idle",      40,  160),   # long calm idle -> trim to a stable loop
    ("Walk", "Walking",   1,   33),    # full walk cycle (0..32.8)
    ("Run",  "Riflerun",  1,   18),    # clean run cycle (0..17.6)
]
# Talk has no Jake donor clip; synthesized procedurally below.
TALK_CLIP = ("Talk", 90)              # name, frame count

# Map: target bone (.L/.R rig) -> source bone (Jake mixamorig* rig).
BONE_MAP = {
    "Hips":        "mixamorigHips",
    "Spine":       "mixamorigSpine",
    "Chest":       "mixamorigSpine2",
    "Neck":        "mixamorigNeck",
    "Head":        "mixamorigHead",
    "Shoulder.L":  "mixamorigLeftShoulder",
    "UpperArm.L":  "mixamorigLeftArm",
    "LowerArm.L":  "mixamorigLeftForeArm",
    "Hand.L":      "mixamorigLeftHand",
    "Shoulder.R":  "mixamorigRightShoulder",
    "UpperArm.R":  "mixamorigRightArm",
    "LowerArm.R":  "mixamorigRightForeArm",
    "Hand.R":      "mixamorigRightHand",
    "UpperLeg.L":  "mixamorigLeftUpLeg",
    "LowerLeg.L":  "mixamorigLeftLeg",
    "Foot.L":      "mixamorigLeftFoot",
    "UpperLeg.R":  "mixamorigRightUpLeg",
    "LowerLeg.R":  "mixamorigRightLeg",
    "Foot.R":      "mixamorigRightFoot",
}
ROOT_BONE = "Hips"


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def import_target():
    bpy.ops.import_scene.gltf(filepath=TARGET_GLB)
    arm = next((o for o in bpy.data.objects if o.type == 'ARMATURE'), None)
    if not arm:
        raise RuntimeError("no armature in target GLB")
    arm.name = "TargetArm"
    arm.animation_data_clear()
    # Purge the target GLB's own pre-existing action(s); we only want our clips.
    for a in list(bpy.data.actions):
        bpy.data.actions.remove(a)
    # Lock the character's textures in + pack them IMMEDIATELY so source import
    # churn cannot evict the original GLB's image data before export.
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
    log("target armature:", arm.name, "bones:", len(arm.data.bones),
        "images:", [(i.name, i.has_data) for i in bpy.data.images])
    return arm


def import_jake():
    """Import Jake once; keep his armature + all 22 actions resident."""
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=JAKE_GLB)
    new = [o for o in bpy.data.objects if o not in before]
    src = next((o for o in new if o.type == 'ARMATURE'), None)
    if not src:
        raise RuntimeError("no armature in Jake GLB")
    src.name = "JakeArm"
    if not src.animation_data:
        src.animation_data_create()
    # Prefix EVERY Jake source action so the names can never collide with our
    # baked clip names (e.g. Jake's own "Idle" would otherwise bump our baked
    # "Idle" to "Idle.001" and ship a wrongly-named glTF animation).
    for a in bpy.data.actions:
        if not a.name.startswith("JK_"):
            a.name = "JK_" + a.name
    # Hide Jake's meshes; they are donor-only and must not export.
    jake_meshes = [o for o in new if o.type == 'MESH']
    log("jake armature:", src.name, "bones:", len(src.data.bones),
        "actions:", len(bpy.data.actions),
        "meshes:", [m.name for m in jake_meshes])
    return src, new, jake_meshes


def _depth_order(arm, names):
    depth = {}
    for n in names:
        b = arm.data.bones.get(n)
        d = 0
        while b is not None:
            b = b.parent; d += 1
        depth[n] = d
    return sorted(names, key=lambda n: depth[n])


def _assign_first_slot(arm, act):
    try:
        if getattr(act, "slots", None) is not None:
            if len(act.slots) == 0:
                act.slots.new(id_type='OBJECT', name=arm.name)
            arm.animation_data.action_slot = act.slots[0]
    except Exception as e:
        log("  slot assign warn:", e)


def src_world_hips_height(src):
    b = src.data.bones.get(BONE_MAP[ROOT_BONE])
    if not b:
        return 1.0
    return abs((src.matrix_world @ b.head_local).z)


def tgt_world_hips_height(tgt):
    b = tgt.data.bones.get(ROOT_BONE)
    if not b:
        return 1.0
    return abs((tgt.matrix_world @ b.head_local).z)


def find_jake_action(name):
    # Jake source actions are prefixed "JK_" on import (see import_jake) so they
    # cannot collide with our baked clip names. Match against the un-prefixed
    # source name, tolerant of case / spaces.
    key = name.lower().replace(" ", "")
    for a in bpy.data.actions:
        an = a.name
        if an.startswith("JK_"):
            an = an[3:]
        if an.lower().replace(" ", "") == key:
            return a
    return None


def retarget_one(tgt, src, clip_name, jake_action_name, fstart, fend):
    sact = find_jake_action(jake_action_name)
    if sact is None:
        raise RuntimeError("Jake action not found: " + jake_action_name)
    src.animation_data.action = sact
    log("---- clip", clip_name, "from Jake action", sact.name,
        "frames", fstart, "-", fend)

    sz = src_world_hips_height(src)
    tz = tgt_world_hips_height(tgt)
    loc_scale = (tz / sz) if sz > 1e-6 else 1.0
    log("  src hips world h=%.3f tgt=%.3f loc_scale=%.5f" % (sz, tz, loc_scale))

    bpy.ops.object.mode_set(mode='OBJECT')
    bpy.context.view_layer.objects.active = tgt

    # Precompute rest WORLD rotations (3x3) for both rigs.
    s_rest, t_rest = {}, {}
    for tb, sb in BONE_MAP.items():
        tbone = tgt.data.bones.get(tb)
        sbone = src.data.bones.get(sb)
        if tbone is None or sbone is None:
            continue
        t_rest[tb] = (tgt.matrix_world @ tbone.matrix_local).to_3x3().normalized()
        s_rest[tb] = (src.matrix_world @ sbone.matrix_local).to_3x3().normalized()
    mapped = [tb for tb in BONE_MAP if tb in t_rest]
    order = _depth_order(tgt, mapped)
    log("  mapped bones:", len(mapped))

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
    src_root_pbone = src.pose.bones.get(BONE_MAP[ROOT_BONE])

    out_frame = 1
    for f in range(fstart, fend + 1):
        bpy.context.scene.frame_set(f)
        bpy.context.view_layer.update()
        for tb in order:
            sb = BONE_MAP[tb]
            spb = src.pose.bones.get(sb)
            tpb = tgt.pose.bones.get(tb)
            s_pose_w = (src.matrix_world @ spb.matrix).to_3x3().normalized()
            D = s_pose_w @ s_rest[tb].inverted()
            t_pose_w = D @ t_rest[tb]
            loc = tpb.matrix.translation.copy()
            new_m = (tgt.matrix_world.inverted().to_3x3() @ t_pose_w).to_4x4()
            new_m.translation = loc
            tpb.matrix = new_m
            bpy.context.view_layer.update()
            tpb.keyframe_insert("rotation_quaternion", frame=out_frame)
        # Root: keep clip IN-PLACE, preserve only vertical Hips bob.
        s_root_w = src.matrix_world @ src_root_pbone.matrix.translation
        s_root_rest_w = src.matrix_world @ src.data.bones[BONE_MAP[ROOT_BONE]].head_local
        delta = (s_root_w - s_root_rest_w) * loc_scale
        new_head = root_rest_head_world.copy()
        new_head.z += delta.z
        rpb = tgt.pose.bones.get(ROOT_BONE)
        m = rpb.matrix.copy()
        m.translation = tgt.matrix_world.inverted() @ new_head
        rpb.matrix = m
        rpb.keyframe_insert("location", frame=out_frame)
        rpb.keyframe_insert("rotation_quaternion", frame=out_frame)
        out_frame += 1

    baked = tgt.animation_data.action
    baked.name = clip_name
    baked.use_fake_user = True
    log("  baked action:", baked.name, "range", tuple(baked.frame_range))
    tgt.animation_data.action = None
    return baked


def synth_talk(tgt, clip_name, frames):
    """Jake has no talk clip. Synthesize a conversational gesture on the target
    rig: relaxed idle base + head nods/turns + light hand/forearm gesturing +
    breathing chest. All in the target's own bone space (rest-relative)."""
    log("---- clip", clip_name, "SYNTH (no Jake donor) frames 1 -", frames)
    bpy.ops.object.mode_set(mode='OBJECT')
    bpy.context.view_layer.objects.active = tgt

    # Use Euler for the synthesized gesture (simple, readable channels).
    for pb in tgt.pose.bones:
        pb.rotation_mode = 'XYZ'
        pb.rotation_euler = (0.0, 0.0, 0.0)
        pb.location = (0.0, 0.0, 0.0)

    if not tgt.animation_data:
        tgt.animation_data_create()
    act = bpy.data.actions.new(clip_name + "_tmp")
    act.use_fake_user = True
    tgt.animation_data.action = act
    _assign_first_slot(tgt, act)

    D = math.radians
    TAU = math.tau

    def key_e(bone, frame, rx=0.0, ry=0.0, rz=0.0):
        pb = tgt.pose.bones.get(bone)
        if not pb:
            return
        pb.rotation_euler = (rx, ry, rz)
        pb.keyframe_insert("rotation_euler", frame=frame)

    for i in range(frames):
        f = i + 1
        p = (i / frames) * TAU
        # Head: gentle nods (2x) + slow side glances (1x).
        key_e("Head", f, rx=math.sin(p * 2) * D(7), rz=math.sin(p) * D(9))
        key_e("Neck", f, rx=math.sin(p * 2 + 0.5) * D(4))
        # Chest: subtle breathing + emphasis turn.
        key_e("Chest", f, rx=math.sin(p * 2) * D(2), rz=math.sin(p) * D(3))
        key_e("Spine", f, rz=math.sin(p) * D(2))
        # Right arm gestures (raised + moving forearm), left arm lighter.
        # Lift upper arms inward a little, then animate the forearms.
        key_e("UpperArm.R", f, rz=D(-18) + math.sin(p) * D(8),
              rx=math.sin(p * 2 + 1.0) * D(6))
        key_e("LowerArm.R", f, rz=D(-22) + math.sin(p * 2) * D(20),
              ry=math.sin(p) * D(10))
        key_e("Hand.R", f, rx=math.sin(p * 3) * D(14))
        key_e("UpperArm.L", f, rz=D(14) + math.sin(p + math.pi) * D(5))
        key_e("LowerArm.L", f, rz=D(18) + math.sin(p * 2 + math.pi) * D(12))
        key_e("Hand.L", f, rx=math.sin(p * 3 + 1.5) * D(9))

    baked = tgt.animation_data.action
    baked.name = clip_name
    baked.use_fake_user = True
    log("  synth action:", baked.name, "range", tuple(baked.frame_range))
    tgt.animation_data.action = None
    return baked


def main():
    reset()
    tgt = import_target()
    src, src_objs, jake_meshes = import_jake()

    baked = []
    for clip_name, jact, fs, fe in SRC_CLIPS:
        try:
            baked.append(retarget_one(tgt, src, clip_name, jact, fs, fe))
        except Exception as e:
            import traceback
            log("CLIP FAILED", clip_name, ":", e)
            log(traceback.format_exc())

    try:
        baked.append(synth_talk(tgt, TALK_CLIP[0], TALK_CLIP[1]))
    except Exception as e:
        import traceback
        log("TALK FAILED:", e); log(traceback.format_exc())

    log("baked clips:", [a.name for a in baked])
    if not baked:
        raise RuntimeError("no clips baked")

    # Remove Jake's donor objects + Jake's raw source actions so the export is
    # clean (only the target armature/meshes + our 4 actions remain).
    keep = set(a.name for a in baked)
    src.animation_data_clear()
    for o in src_objs:
        try:
            bpy.data.objects.remove(o, do_unlink=True)
        except Exception:
            pass
    for a in list(bpy.data.actions):
        if a.name not in keep:
            try:
                bpy.data.actions.remove(a)
            except Exception:
                pass

    # Defensive re-pack of textures before export.
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
        filepath=OUT_GLB,
        export_format='GLB',
        export_yup=True,
        use_selection=False,
        export_animations=True,
        export_animation_mode='ACTIONS',
        export_nla_strips=False,
        export_force_sampling=True,
        export_apply=False,
        export_materials='EXPORT',
        export_image_format='AUTO',
        export_texcoords=True,
        export_normals=True,
        export_tangents=True,
        export_skins=True,
    )
    log("EXPORTED:", OUT_GLB)
    log("RETARGET OK clips:", ",".join(a.name for a in baked))


if __name__ == "__main__":
    status = "OK"
    try:
        main()
    except Exception as e:
        import traceback
        log("FAILED:", e); log(traceback.format_exc()); status = "FAIL: " + str(e)
    flush_log(status)
