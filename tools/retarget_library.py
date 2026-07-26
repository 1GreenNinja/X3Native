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
# identity on the MESHY auto-rig: BOTH source and target are Meshy 24-joint standard
# humanoids, whose bones are "Hips"/"LeftUpLeg"/"RightHand"/"Head" - NOT mixamorig* and
# NOT the ".L/.R" convention. Same names on both sides, but each character's REST POSE
# differs (Meshy fits the skeleton to the mesh: on Sarah vs JakeClone the leg/shoulder
# rest rotations differ a lot), so this still goes through the rest-relative orientation
# transfer below. A raw glTF channel copy would look wrong. Leaf helpers (head_end,
# headfront) are deliberately left out - nothing skins to them.
BONE_MAP_MESHY = {k: k for k in (
    "Hips", "Spine", "Spine01", "Spine02", "neck", "Head",
    "LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand",
    "RightShoulder", "RightArm", "RightForeArm", "RightHand",
    "LeftUpLeg", "LeftLeg", "LeftFoot", "LeftToeBase",
    "RightUpLeg", "RightLeg", "RightFoot", "RightToeBase",
)}

RIG_MAPS = {"mixamo": BONE_MAP_MIXAMO, "starter": BONE_MAP_STARTER, "lr": BONE_MAP_LR,
            "meshy": BONE_MAP_MESHY}


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
    TGT_MESHES = [o for o in bpy.data.objects if o.type == 'MESH']
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

    b = src.data.bones.get(bmap[ROOT_BONE])
    sz = abs((src.matrix_world @ b.head_local).z) if b else 1.0
    tb = tgt.data.bones.get(ROOT_BONE)
    tz = abs((tgt.matrix_world @ tb.head_local).z) if tb else 1.0
    loc_scale = (tz / sz) if sz > 1e-6 else 1.0

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
        out_frame += 1

    baked = tgt.animation_data.action
    baked.name = clip_name
    baked.use_fake_user = True
    log("  baked action:", baked.name, "range", tuple(baked.frame_range))
    tgt.animation_data.action = None
    return baked


def main():
    with open(MANIFEST, "r", encoding="utf-8") as f:
        man = json.load(f)
    keep = bool(man.get("keep_existing", True))
    clips = man.get("clips", [])

    reset()
    tgt = import_target(keep)
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
            baked.append((act.name, rig, c.get("action"), c.get("loop", False)))
        except Exception as e:
            import traceback
            log("CLIP FAILED", c.get("name"), ":", e); log(traceback.format_exc())

    log("baked clips:", baked)
    if not baked:
        raise RuntimeError("no clips baked")

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
