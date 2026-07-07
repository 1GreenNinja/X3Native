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
# W6-3 (misc-polish): extended to cap the shoulder cut with a SLEEVE RING (a
# short dark cuff tube) so the crop no longer shows raw open-mesh stumps, and
# to note where the suit texture reads mottled (see the sleeve-ring UV comment
# below) for whoever next revisits the material clamp.
#
# ASCII-only on purpose (see anim_build.ps1 note).
import bpy
import bmesh
import mathutils
import math
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
    bm = bmesh.new()
    bm.from_mesh(baked)
    # W6-3: the glTF import leaves the mesh fully vertex-SPLIT (every triangle
    # owns its own unique BMVert trio, even where positions coincide -- normal
    # for glTF's per-loop attribute model, but it means bmesh topology queries
    # (is_boundary, connected components) see NO shared edges ANYWHERE, i.e.
    # every triangle looks like its own island. Weld coincident-position verts
    # first so the mesh is a real manifold for the island-cleanup + boundary-
    # loop passes below; per-face UV survives (glTF UV is a per-LOOP attribute,
    # not per-vertex, so a weld does not touch it) and skin weights at a welded
    # point are identical anyway (same physical vertex, just UV/normal-split).
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=0.0001)
    log("weld coincident verts: %d verts remain" % len(bm.verts))
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
    log("vertex cut (weight threshold): kept=%d deleted=%d" % (kept, len(doomed)))

    # ---- ISLAND CLEANUP: drop stray disconnected specks ------------------------
    # The per-VERTEX weight threshold above is noisy at the shoulder blend zone:
    # smooth Mixamo skinning leaves scattered low/high-weight outliers, so the
    # raw cut is not one clean seam per arm but thousands of tiny pinhole islands
    # (measured: 71856 boundary edges / 23952 disconnected loops on the first
    # pass here -- a moth-eaten edge, not a crop). Flood-fill the kept mesh into
    # connected components and drop everything below 2% of the largest island's
    # vertex count: that keeps the real arm+hand(+finger) blobs (however many
    # there turn out to be split into) and removes the speckle, so the boundary-
    # loop pass below sees a small number of real loops instead of noise.
    from collections import deque
    visited = set()
    islands = []
    for v0 in bm.verts:
        if v0 in visited:
            continue
        comp = []
        dq = deque([v0]); visited.add(v0)
        while dq:
            cur = dq.popleft()
            comp.append(cur)
            for e in cur.link_edges:
                nv = e.other_vert(cur)
                if nv not in visited:
                    visited.add(nv)
                    dq.append(nv)
        islands.append(comp)
    islands.sort(key=len, reverse=True)
    largest = len(islands[0]) if islands else 0
    thresh = max(50, int(largest * 0.02))
    keep_islands = [c for c in islands if len(c) >= thresh]
    keep_set = set(v for c in keep_islands for v in c)
    stray = [v for v in bm.verts if v not in keep_set]
    if stray:
        bmesh.ops.delete(bm, geom=stray, context="VERTS")
    kept = len(keep_set)
    log("island cleanup: %d islands total (sizes top5 %s), kept %d islands >= %d verts (%d verts), dropped %d stray verts" % (
        len(islands), [len(c) for c in islands[:5]], len(keep_islands), thresh, kept, len(stray)))

    # ---- SLEEVE HEM: fold a short dark lip out of every cut edge ---------------
    # The vertex cut above leaves an open-mesh boundary at the shoulder crop
    # (rough "stumps" - the bug this task fixes). THREE geometry strategies were
    # tried and rejected here (each confirmed via a tools/preview_glb.py render):
    #   1. hand-sorted "fan to a center point" (graph-walk order, then a
    #      centroid-angle sort) -- both produced a self-crossing pleated fan,
    #      because this weight-threshold cut boundary is not the simple convex
    #      ring either sort assumes.
    #   2. bmesh.ops.holes_fill (Blender's own hole-cap op) -- still odd/uneven
    #      because it can only be as clean as its (gnarly) input loop.
    #   3. a synthesized regular N-gon disc sized to the loop's max-radius --
    #      technically clean, but the boundary is elongated/irregular enough
    #      that "max distance from centroid" overshoots badly, producing a
    #      giant flat coin far bigger than the actual gap.
    # This is the fix that holds regardless of how gnarly/elongated the true cut
    # boundary is: work PER EDGE, not per loop/hole. Every boundary edge gets a
    # small quad flap extruded a fixed short distance along that edge's own
    # local outward (vertex-normal) direction -- proportional to the edge
    # itself, never a global shape estimate that can blow up. Across the whole
    # boundary this reads as a short folded/rolled hem hugging the actual cut
    # line, hiding the open-mesh edge without adding a disproportionate shape.
    # UV: reuse the boundary's OWN uv (still valid on the kept verts' remaining
    # faces) so the hem samples the exact dark sleeve-fabric texel at the cut
    # edge - no new material, can't mismatch Jake's suit texture because it IS
    # that texture, clamped to one texel. Both winding orders are emitted per
    # face (SurfaceLibrary::makePanel precedent, app/surface_library.cpp: "wind
    # both ways so panels read regardless of the viewer side").
    uv_layer = bm.loops.layers.uv.active
    if uv_layer is None:
        log("WARNING: no UV layer -- skipping sleeve hem (cut stumps stay bare)")
    else:
        def vert_uv(v):
            for lp in v.link_loops:
                return lp[uv_layer].uv.copy()
            return None

        bm.normal_update()
        boundary_edges = [e for e in bm.edges if e.is_boundary]
        log("boundary edges after cut: %d" % len(boundary_edges))

        def face_both_ways(verts, uv):
            for order in (verts, tuple(reversed(verts))):
                try:
                    f = bm.faces.new(order)
                    for lp in f.loops:
                        lp[uv_layer].uv = uv
                except ValueError:
                    pass   # duplicate/degenerate -- non-fatal, skip

        HEM_DEPTH = 0.015   # 1.5 cm folded lip per edge (proportional, not global)
        hem_faces = 0
        for e in boundary_edges:
            va, vb = e.verts
            nrm = va.normal + vb.normal
            if nrm.length < 1e-6:
                continue
            nrm.normalize()
            uv0 = vert_uv(va) or vert_uv(vb) or mathutils.Vector((0.1, 0.1))
            pa = bm.verts.new(va.co + nrm * HEM_DEPTH)
            pb = bm.verts.new(vb.co + nrm * HEM_DEPTH)
            face_both_ways((va, vb, pb, pa), uv0)
            hem_faces += 1
        bm.normal_update()
        log("sleeve hem: %d edge flaps added (depth=%.3f m)" % (hem_faces, HEM_DEPTH))

    bm.to_mesh(baked)
    bm.free()
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
