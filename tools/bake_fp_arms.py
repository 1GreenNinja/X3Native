# bake_fp_arms.py - bpy script: bake Jake's rig into STATIC first-person arms.
#
# Usage (via tools/bake_fp_arms.ps1 - the MS-Store launcher detaches, so this
# script writes <out>.log + <out>.done markers the wrapper polls for):
#   blender-launcher --background --python tools/bake_fp_arms.py -- <in.glb> <out.glb>
#
# What it does:
#   1. Imports the rigged GLB (Jake_22_actions.glb - Mixamo skeleton, 1 skinned mesh).
#   2. Poses the rig with the "Rifleaimingidle" action (frame 1) - a natural
#      two-hand aim pose that reads correctly behind any held weapon.
#   3. Bakes the deformed mesh via the depsgraph (new_from_object) so the pose is
#      frozen into plain vertices - no skeleton in the export.
#   4. Deletes every vertex NOT weighted >= 35% to arm/hand bones (upper arm,
#      forearm, hand, fingers; shoulders/clavicles excluded - classic FP-arms cut
#      starts mid-bicep so the stumps stay behind the camera edges).
#   5. Re-centers the mesh on the NECK-base world position, so the exported
#      origin is where the CAMERA sits - the engine can rotate the arms about
#      the eye point and they swing like a body, not like a prop.
#   6. Exports a GLB (materials + Jake's texture carried through automatically).
#
# ASCII-only on purpose (see anim_build.ps1 note).
import bpy
import sys
import os

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
IN_GLB, OUT_GLB = argv[0], argv[1]
LOG = OUT_GLB + ".log"
DONE = OUT_GLB + ".done"

_lines = []
def log(msg):
    _lines.append(str(msg))
    try:
        with open(LOG, "w") as f:
            f.write("\n".join(_lines) + "\n")
    except OSError:
        pass

def finish(status):
    log("STATUS: " + status)
    with open(DONE, "w") as f:
        f.write(status + "\n")
    # Leave Blender promptly (background mode exits after script anyway).

try:
    # ---- clean scene + import -------------------------------------------------
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=IN_GLB)
    log("imported: " + IN_GLB)

    arm_obj = next((o for o in bpy.data.objects if o.type == "ARMATURE"), None)
    # The GLB can carry stray helper meshes (an Icosphere shipped inside
    # Jake_22_actions.glb) - the CHARACTER is the mesh with skin weights, so pick
    # the one with the most vertex groups, then most vertices.
    meshes = [o for o in bpy.data.objects if o.type == "MESH"]
    for m in meshes:
        log("mesh candidate: %s verts=%d groups=%d" % (m.name, len(m.data.vertices), len(m.vertex_groups)))
    mesh_obj = max(meshes, key=lambda o: (len(o.vertex_groups), len(o.data.vertices))) if meshes else None
    if not arm_obj or not mesh_obj:
        finish("FAIL: no armature/mesh after import")
        raise SystemExit
    log("armature=%s mesh=%s verts=%d groups=%d" % (
        arm_obj.name, mesh_obj.name, len(mesh_obj.data.vertices), len(mesh_obj.vertex_groups)))

    # ---- pick + apply the aim pose ---------------------------------------------
    def score(a):
        n = a.name.lower()
        if "ifleaiming" in n: return 3      # Rifleaimingidle - the money pose
        if "aiming" in n:     return 2
        if n.endswith("idle") or n == "idle": return 1
        return 0
    actions = sorted(bpy.data.actions, key=score, reverse=True)
    if not actions or score(actions[0]) == 0:
        log("WARNING: no aiming/idle action found; using first action")
    act = actions[0]
    if not arm_obj.animation_data:
        arm_obj.animation_data_create()
    arm_obj.animation_data.action = act
    bpy.context.scene.frame_set(1)
    log("pose action: " + act.name)

    # ---- neck anchor (world) BEFORE baking -------------------------------------
    depsgraph = bpy.context.evaluated_depsgraph_get()
    arm_eval = arm_obj.evaluated_get(depsgraph)
    neck = None
    for pb in arm_eval.pose.bones:
        if "neck" in pb.name.lower():
            neck = arm_eval.matrix_world @ pb.head
            log("neck anchor from bone %s: %.4f %.4f %.4f" % (pb.name, neck.x, neck.y, neck.z))
            break
    if neck is None:
        # fall back to the mesh bounds top-center
        from mathutils import Vector
        ws = [mesh_obj.matrix_world @ Vector(c) for c in mesh_obj.bound_box]
        neck = Vector((sum(v.x for v in ws) / 8.0, max(v.y for v in ws), sum(v.z for v in ws) / 8.0))
        log("neck fallback (bounds top-center)")

    # ---- bake the posed mesh ----------------------------------------------------
    mesh_eval = mesh_obj.evaluated_get(depsgraph)
    baked = bpy.data.meshes.new_from_object(
        mesh_eval, preserve_all_data_layers=True, depsgraph=depsgraph)
    baked_obj = bpy.data.objects.new("FPArms", baked)
    baked_obj.matrix_world = mesh_obj.matrix_world.copy()
    bpy.context.scene.collection.objects.link(baked_obj)
    # vertex groups live on the OBJECT - copy the name table so indices line up.
    for vg in mesh_obj.vertex_groups:
        baked_obj.vertex_groups.new(name=vg.name)
    log("baked mesh: %d verts" % len(baked.vertices))

    # ---- classify groups: arms/hands keep, everything else cut -----------------
    def is_arm_group(name):
        n = name.lower()
        if "shoulder" in n:  # clavicle bleeds into the torso - cut at the bicep
            return False
        return ("forearm" in n) or ("hand" in n) or n.endswith("arm") \
               or ("arm" in n and ("left" in n or "right" in n))
    keep_idx = set(vg.index for vg in baked_obj.vertex_groups if is_arm_group(vg.name))
    log("keep groups (%d): %s" % (
        len(keep_idx),
        ", ".join(vg.name for vg in baked_obj.vertex_groups if vg.index in keep_idx)))
    if not keep_idx:
        finish("FAIL: no arm/hand vertex groups matched")
        raise SystemExit

    # ---- delete non-arm vertices -------------------------------------------------
    import bmesh
    bm = bmesh.new()
    bm.from_mesh(baked)
    deform = bm.verts.layers.deform.active
    if deform is None:
        finish("FAIL: baked mesh has no deform layer (weights lost)")
        raise SystemExit
    doomed = []
    kept = 0
    for v in bm.verts:
        w = v[deform]
        total = sum(w.values())
        armw = sum(val for gi, val in w.items() if gi in keep_idx)
        if total <= 0.0 or (armw / total) < 0.35:
            doomed.append(v)
        else:
            kept += 1
    bmesh.ops.delete(bm, geom=doomed, context="VERTS")
    bm.to_mesh(baked)
    bm.free()
    log("vertex cut: kept=%d deleted=%d" % (kept, len(doomed)))
    if kept < 50:
        finish("FAIL: almost nothing kept (%d verts) - group matching wrong" % kept)
        raise SystemExit

    # ---- freeze world transform, then re-center on the neck anchor -------------
    bpy.ops.object.select_all(action="DESELECT")
    baked_obj.select_set(True)
    bpy.context.view_layer.objects.active = baked_obj
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    for v in baked.vertices:
        v.co.x -= neck.x
        v.co.y -= neck.y
        v.co.z -= neck.z
    # AABB after recentering (Blender Z-up; exporter converts to glTF Y-up).
    xs = [v.co.x for v in baked.vertices]
    ys = [v.co.y for v in baked.vertices]
    zs = [v.co.z for v in baked.vertices]
    log("AABB blender-space: min(%.3f %.3f %.3f) max(%.3f %.3f %.3f)" % (
        min(xs), min(ys), min(zs), max(xs), max(ys), max(zs)))

    # ---- delete everything else + export ----------------------------------------
    for o in list(bpy.data.objects):
        if o is not baked_obj:
            bpy.data.objects.remove(o, do_unlink=True)
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.export_scene.gltf(filepath=OUT_GLB, export_format="GLB", use_selection=True)
    sz = os.path.getsize(OUT_GLB)
    log("exported: %s (%d bytes)" % (OUT_GLB, sz))
    finish("OK verts=%d bytes=%d" % (kept, sz))
except SystemExit:
    pass
except Exception as e:
    import traceback
    log(traceback.format_exc())
    finish("FAIL: " + repr(e))
