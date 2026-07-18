"""
prep_canon_alien.py - headless Blender prep for the CANON ALIENS wave (Rodin
mesh -> game-ready rigged GLB for X3Native's rigged_glb pipeline).

    blender-launcher.exe --background --python prep_canon_alien.py -- \
        <input.(glb|usd|usda|usdc|usdz)> <out.glb> <target_height_m> <target_tris>

Per model:
  1. import (glTF or USD), join all meshes into one
  2. weld (merge-by-distance) FIRST, clear custom split normals
  3. decimate (collapse) down to <target_tris> if above it
  4. measure the bounding box, uniform-scale so height == <target_height_m>,
     center on X/Y, drop feet to Z=0 (Blender Z-up; glTF export flips to Y-up)
  5. add a single "Root" bone armature + skin every vert to it (weight 1.0) so
     tools/animate_creature.py's "core" gait can bake Idle/Walk/Run on top
  6. pack all images (keep the Rodin PBR textures) and export ONE GLB (Y-up)

ENV NOTE (this box): Blender is the Microsoft Store package - the launcher
DETACHES (stdout lost), so this script writes <out>.log + a <out>.done marker
the caller polls. Same convention as animate_creature.py.

Clean-room: public Blender Python API + glTF 2.0 spec only.
"""
import bpy, sys, os, math

ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(ARGV) < 4:
    raise SystemExit("Usage: ... -- <input> <out.glb> <target_height_m> <target_tris>")
SRC, OUT = ARGV[0], ARGV[1]
TARGET_H = float(ARGV[2])
TARGET_TRIS = int(ARGV[3])

LOG_PATH, DONE_PATH = OUT + ".log", OUT + ".done"
_log = []
def log(*a):
    s = "[prep] " + " ".join(str(x) for x in a)
    _log.append(s); print(s)

def flush_log(status):
    try:
        with open(LOG_PATH, "w", encoding="utf-8") as f:
            f.write("\n".join(_log))
        with open(DONE_PATH, "w", encoding="utf-8") as f:
            f.write(status)
    except Exception as e:
        print("[prep] could not write log/marker:", e)

def tri_count(obj):
    obj.data.calc_loop_triangles()
    return len(obj.data.loop_triangles)

def main():
    bpy.ops.wm.read_factory_settings(use_empty=True)

    # ---- 1. import ----
    ext = os.path.splitext(SRC)[1].lower()
    if ext in (".glb", ".gltf"):
        bpy.ops.import_scene.gltf(filepath=SRC)
    else:
        bpy.ops.wm.usd_import(filepath=SRC)
    meshes = [o for o in bpy.data.objects if o.type == 'MESH']
    if not meshes:
        raise RuntimeError("no meshes in " + SRC)
    log("imported", os.path.basename(SRC), "meshes:", len(meshes))

    # Drop any imported lights/cameras/empties parents; keep meshes only.
    bpy.ops.object.select_all(action='DESELECT')
    for o in meshes:
        o.select_set(True)
    bpy.context.view_layer.objects.active = meshes[0]
    # Clear parenting but keep world transform, then apply all transforms.
    bpy.ops.object.parent_clear(type='CLEAR_KEEP_TRANSFORM')
    if len(meshes) > 1:
        bpy.ops.object.join()
    mesh = bpy.context.view_layer.objects.active
    for o in list(bpy.data.objects):
        if o is not mesh:
            bpy.data.objects.remove(o, do_unlink=True)
    bpy.ops.object.select_all(action='DESELECT')
    mesh.select_set(True)
    bpy.context.view_layer.objects.active = mesh
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    t0 = tri_count(mesh)
    log("joined tris:", t0)

    # ---- 2. weld + clear custom split normals ----
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.mesh.remove_doubles(threshold=1e-4)
    bpy.ops.object.mode_set(mode='OBJECT')
    try:
        bpy.ops.mesh.customdata_custom_splitnormals_clear()
    except Exception as e:
        log("splitnormals clear warn:", e)
    t1 = tri_count(mesh)
    log("after weld tris:", t1)

    # ---- 3. decimate ----
    if t1 > TARGET_TRIS:
        ratio = TARGET_TRIS / float(t1)
        mod = mesh.modifiers.new("Decim", 'DECIMATE')
        mod.ratio = ratio
        mod.use_collapse_triangulate = True
        bpy.ops.object.modifier_apply(modifier=mod.name)
        log("decimated ratio", round(ratio, 4), "->", tri_count(mesh), "tris")
    else:
        log("already under budget, no decimate")

    # ---- 4. size + ground ----
    # Blender is Z-up here; glTF/USD importers convert, so height = Z extent.
    from mathutils import Vector
    bb = [mesh.matrix_world @ Vector(c) for c in mesh.bound_box]
    minv = Vector((min(v.x for v in bb), min(v.y for v in bb), min(v.z for v in bb)))
    maxv = Vector((max(v.x for v in bb), max(v.y for v in bb), max(v.z for v in bb)))
    dims = maxv - minv
    log("raw dims (m): x", round(dims.x, 3), "y", round(dims.y, 3), "z", round(dims.z, 3))
    if dims.z <= 1e-6:
        raise RuntimeError("degenerate Z extent")
    s = TARGET_H / dims.z
    mesh.scale = (s, s, s)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    bb = [mesh.matrix_world @ Vector(c) for c in mesh.bound_box]
    minv = Vector((min(v.x for v in bb), min(v.y for v in bb), min(v.z for v in bb)))
    maxv = Vector((max(v.x for v in bb), max(v.y for v in bb), max(v.z for v in bb)))
    cx, cy = (minv.x + maxv.x) * 0.5, (minv.y + maxv.y) * 0.5
    mesh.location.x -= cx
    mesh.location.y -= cy
    mesh.location.z -= minv.z          # feet on the ground plane
    bpy.ops.object.transform_apply(location=True, rotation=False, scale=False)
    log("scaled x", round(s, 4), "-> height", TARGET_H, "m, feet at z=0")

    # Smooth shading with a hard-edge angle (post-decimate normal rebuild).
    bpy.ops.object.shade_smooth()
    try:
        bpy.ops.object.shade_smooth_by_angle(angle=math.radians(50.0))
    except Exception:
        pass  # older Blender: plain smooth is fine

    # ---- 5. single-Root armature so animate_creature.py "core" gait works ----
    arm_data = bpy.data.armatures.new("Arm")
    arm_obj = bpy.data.objects.new("Arm", arm_data)
    bpy.context.scene.collection.objects.link(arm_obj)
    bpy.context.view_layer.objects.active = arm_obj
    bpy.ops.object.mode_set(mode='EDIT')
    eb = arm_data.edit_bones.new("Root")
    eb.head = (0.0, 0.0, 0.0)
    eb.tail = (0.0, 0.0, max(0.25 * TARGET_H, 0.2))
    bpy.ops.object.mode_set(mode='OBJECT')
    vg = mesh.vertex_groups.new(name="Root")
    vg.add(list(range(len(mesh.data.vertices))), 1.0, 'REPLACE')
    mod = mesh.modifiers.new("Armature", 'ARMATURE')
    mod.object = arm_obj
    mesh.parent = arm_obj
    log("armature: single Root bone, all verts weight 1.0")

    # ---- 6. pack textures + export ----
    # NPC texture budget: cap at 1024px (Rodin ships 2048 PNGs; 3 of them per
    # model = ~10 MB decode per spawn, which showed up as ~900 ms of boot for
    # 7 aliens). 1024 is plenty at NPC screen size and quarters the decode.
    MAX_TEX = 1024
    for img in bpy.data.images:
        try:
            if not img.has_data:
                _ = img.pixels[0]
            w, h = img.size
            if max(w, h) > MAX_TEX:
                img.scale(min(w, MAX_TEX), min(h, MAX_TEX))
                log("tex downres", img.name, (w, h), "->", tuple(img.size))
            if not img.packed_file:
                img.pack()
            img.use_fake_user = True
        except Exception as e:
            log("img pack warn", img.name, e)
    for m in bpy.data.materials:
        m.use_fake_user = True

    os.makedirs(os.path.dirname(OUT) or ".", exist_ok=True)
    bpy.ops.export_scene.gltf(
        filepath=OUT, export_format='GLB', export_yup=True, use_selection=False,
        export_animations=False, export_apply=True,
        export_materials='EXPORT', export_image_format='AUTO',
        export_texcoords=True, export_normals=True, export_skins=True)
    log("EXPORTED:", OUT, "tris:", tri_count(mesh))

if __name__ == "__main__":
    status = "OK"
    try:
        main()
    except Exception as e:
        import traceback
        log("FAILED:", e); log(traceback.format_exc()); status = "FAIL: " + str(e)
    flush_log(status)
