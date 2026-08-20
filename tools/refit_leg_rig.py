# =============================================================================
# Headless Blender LEG RE-RIG for characters whose auto-rigger collapsed both
# leg chains onto the body centreline.
#
#   blender --background --python tools/refit_leg_rig.py -- <in.glb> <out.glb>
#
# WHY. Sarah.glb carries a valid-looking Meshy 24-joint skeleton — right names,
# right order, right count — but its leg joints sit almost on top of each other:
#
#       rest world X separation      Sarah     a healthy rig (JakeClone)
#         Left/RightUpLeg            0.080          0.202
#         Left/RightLeg              0.115          0.286
#         Left/RightFoot             0.075          0.341
#
#   ...and not even symmetrically about the midline (LeftFoot x=+0.014 vs
# RightFoot x=-0.061). Both legs are therefore driven by bones that nearly
# coincide, so ANY clip that steps tears or fuses the mesh. No clip source and
# no retargeter can fix that — it is the rig, not the animation. (Chased through
# four failed clip-transfer routes first; see app/sarah.cpp.)
#
# WHAT THIS DOES. It does not guess where the legs "should" be — it measures
# them. For each leg joint's own height, it takes the mesh cross-section at that
# height, splits it at the body's centre X, and puts the joint at the centroid
# of its own side. That places each chain inside the limb it is supposed to
# drive, symmetric by construction because each side is measured independently.
# Then it re-binds with automatic weights.
#
# VERIFY AFTERWARDS, ALWAYS: pose ONE leg and confirm the other does not move
# (--verify prints the opposite foot's displacement). A rig that fails this is
# still fused no matter how good the clips look in isolation, and the failure
# only shows up once something makes the legs separate.
#
# PELVIS REFINEMENT — TRIED 2026-08-16, NONE OF IT HELPED. Sarah's pelvis reads
# slightly broad in the WALK pose (it is fine at Idle). Three changes were made
# and measured against the same grounded render; all are REVERTED, recorded here
# so the next person does not pay for them again:
#   * Sampling the UpLeg joint partway down toward the knee (0.35), on the theory
#     that the hip-height cross-section includes pelvis mass and drags the joint
#     outward. It goes the WRONG WAY — at thigh height the cross-section IS the
#     thigh, whose centroid sits FURTHER out than the crotch-inclusive slice at
#     hip height. Separation 0.257 -> 0.311 and a visibly broader pelvis.
#   * Adding Hips/Spine to the candidate bone set so sub-hip pelvis vertices can
#     bind to the root instead of being forced onto a thigh. Principled, and it
#     does change the weights — but produced NO visible difference.
#   * Narrowing the hip crossfade band 0.10 -> 0.05 of body height. No visible
#     difference either.
# Conclusion: what remains is not a localized weighting error. It is her body
# shape plus pelvis rotation in the walk clip, and it would want a proper weight
# paint or a corrective shape, not another parameter. Do not re-run this file
# expecting a different asset than the committed Sarah_anim.glb — the two are in
# sync deliberately.
#
# Clean-room: public Blender Python API + glTF 2.0 spec only.
# =============================================================================
import bpy, sys, os
from mathutils import Vector

ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(ARGV) < 2:
    print("ERR: need <in.glb> <out.glb> [--verify]")
    sys.exit(1)
IN_GLB, OUT_GLB = ARGV[0], ARGV[1]
VERIFY = "--verify" in ARGV

ROOT_BONE = "Hips"
LEG_CHAINS = [
    ["LeftUpLeg",  "LeftLeg",  "LeftFoot",  "LeftToeBase"],
    ["RightUpLeg", "RightLeg", "RightFoot", "RightToeBase"],
]


def log(*a): print("[refit-leg]", *a)


def main():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=IN_GLB)

    arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE')
    meshes = [o for o in bpy.data.objects if o.type == 'MESH']
    if not meshes:
        raise RuntimeError("no mesh in GLB")
    mesh = max(meshes, key=lambda o: len(o.data.vertices))

    for img in bpy.data.images:
        try:
            if not img.has_data:
                _ = img.pixels[0]
            if not img.packed_file:
                img.pack()
            img.use_fake_user = True
        except Exception as e:
            log("  pack warn", img.name, e)
    for m in bpy.data.materials:
        m.use_fake_user = True

    # NORMALIZE TO METRES FIRST. These rigs are authored in CENTIMETRES (Sarah's
    # Hips translation is 97.5, not 0.975) and carry a 0.01 armature scale to
    # compensate. Re-parenting under parent_set() does not survive that cleanly —
    # the first run exported her at 169 m tall. Baking the transforms in makes
    # bone coordinates and vertices agree in one unit before anything is moved.
    for o in bpy.context.selected_objects:
        o.select_set(False)
    mesh.select_set(True); arm.select_set(True)
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    log(f"applied transforms; armature scale now {tuple(round(v,4) for v in arm.scale)}")

    # World-space vertex cloud, once.
    mw = mesh.matrix_world
    verts = [mw @ v.co for v in mesh.data.vertices]
    zs = [v.z for v in verts]
    body_cx = sum(v.x for v in verts) / len(verts)
    log(f"mesh: {len(verts)} verts, z {min(zs):.3f}..{max(zs):.3f}, body centre x={body_cx:.4f}")

    def side_centroid_x(z, want_left, band):
        """Centroid X of the mesh cross-section at height z, on one side of the
        body centre. `band` widens until enough verts are captured."""
        for scale in (1.0, 2.0, 4.0, 8.0):
            h = band * scale
            xs = [v.x for v in verts
                  if abs(v.z - z) < h and ((v.x > body_cx) if want_left else (v.x < body_cx))]
            if len(xs) >= 40:
                return sum(xs) / len(xs), len(xs)
        return None, 0

    # Report the old separations so the fix is auditable in the log.
    def sep(name_l, name_r, space):
        bl = arm.data.bones.get(name_l); br = arm.data.bones.get(name_r)
        if not bl or not br: return None
        return (arm.matrix_world @ bl.head_local).x - (arm.matrix_world @ br.head_local).x

    for l, r in zip(*LEG_CHAINS):
        s = sep(l, r, arm)
        if s is not None:
            log(f"  BEFORE {l}/{r} separation = {abs(s):.4f}")

    height = max(zs) - min(zs)
    band = height * 0.02

    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode='EDIT')
    eb = arm.data.edit_bones

    # Disconnect first so moving a parent's tail does not drag a child's head
    # somewhere we then overwrite anyway; parenting is preserved.
    for chain in LEG_CHAINS:
        for n in chain:
            b = eb.get(n)
            if b is not None:
                b.use_connect = False

    moved = 0
    for chain, want_left in zip(LEG_CHAINS, (True, False)):
        for n in chain:
            b = eb.get(n)
            if b is None:
                log("  MISSING bone", n); continue
            for attr in ("head", "tail"):
                p = getattr(b, attr)
                wz = (arm.matrix_world @ p).z
                cx, cnt = side_centroid_x(wz, want_left, band)
                if cx is None:
                    log(f"  {n}.{attr}: no cross-section at z={wz:.3f}, left as-is")
                    continue
                local = arm.matrix_world.inverted() @ Vector((cx, (arm.matrix_world @ p).y,
                                                              (arm.matrix_world @ p).z))
                setattr(b, attr, Vector((local.x, p.y, p.z)))
                moved += 1
    # FIX THE TAILS. glTF stores no bone tails, so the importer synthesizes them
    # — and on this rig it produced garbage: tails at z = -35 on a body that
    # spans 0..1.68 m. That matters because automatic weighting uses the bone's
    # head->tail VOLUME to decide which vertices it owns, so a tail 35 m under
    # the floor makes the bone claim vertices all over the model. This is the
    # more likely source of the fused legs than the head positions alone.
    # Standard fix: every bone's tail is its child's head; the leaf gets a short
    # stub continuing the chain direction.
    fixed_tails = 0
    for chain in LEG_CHAINS:
        present = [eb.get(n) for n in chain]
        for i, b in enumerate(present):
            if b is None:
                continue
            child = present[i + 1] if i + 1 < len(present) else None
            if child is not None:
                b.tail = child.head.copy()
            else:
                prev = present[i - 1] if i > 0 else None
                d = (b.head - prev.head) if prev is not None else Vector((0, 0, -1))
                if d.length < 1e-6:
                    d = Vector((0, 0, -1))
                b.tail = b.head + d.normalized() * (height * 0.04)
            fixed_tails += 1
    log(f"rebuilt {fixed_tails} bone tails from child heads")

    bpy.ops.object.mode_set(mode='OBJECT')
    log(f"repositioned {moved} joint endpoints")

    for l, r in zip(*LEG_CHAINS):
        s = sep(l, r, arm)
        if s is not None:
            log(f"  AFTER  {l}/{r} separation = {abs(s):.4f}")

    # Re-bind with automatic weights.
    for o in bpy.context.selected_objects:
        o.select_set(False)
    mesh.select_set(True)
    arm.select_set(True)
    bpy.context.view_layer.objects.active = arm
    for m in list(mesh.modifiers):
        if m.type == 'ARMATURE':
            mesh.modifiers.remove(m)

    def leg_weight_centroid():
        """Weighted centroid X of each leg group — the number that has to CHANGE
        for a re-bind to have meant anything."""
        out = {}
        for gname in ("LeftUpLeg", "RightUpLeg", "LeftFoot", "RightFoot"):
            g = mesh.vertex_groups.get(gname)
            if not g:
                out[gname] = None; continue
            sx = sw = 0.0
            for v in mesh.data.vertices:
                for ge in v.groups:
                    if ge.group == g.index and ge.weight > 0.5:
                        p = mesh.matrix_world @ v.co
                        sx += p.x * ge.weight; sw += ge.weight
            out[gname] = (sx / sw) if sw else None
        return out

    before_w = leg_weight_centroid()

    # WEIGHT THE LEGS OURSELVES. bpy.ops.object.parent_set(ARMATURE_AUTO) does
    # NOT work under --background: with the mesh already bound it is a silent
    # no-op (first run returned weights identical byte-for-byte), and with the
    # groups cleared it produces NO groups at all. Blender's heat solve needs an
    # evaluated context we do not have headless. So the leg weights are solved
    # here, in plain Python, which is deterministic and context-free.
    #
    # Rule: every vertex BELOW the hip is reassigned to the leg bone whose
    # head->tail SEGMENT it is closest to, with inverse-distance falloff blended
    # over the two nearest bones so joints bend smoothly instead of creasing.
    # Vertices above the hip keep their original weights untouched — the torso,
    # arms and head were never the problem and must not be disturbed.
    def seg_dist(p, a, b):
        ab = b - a
        L2 = ab.dot(ab)
        t = 0.0 if L2 < 1e-12 else max(0.0, min(1.0, (p - a).dot(ab) / L2))
        return (p - (a + ab * t)).length

    leg_names = [n for chain in LEG_CHAINS for n in chain]
    segs = {}
    for n in leg_names:
        db = arm.data.bones.get(n)
        if db is not None:
            segs[n] = (arm.matrix_world @ db.head_local, arm.matrix_world @ db.tail_local)
    hip_bone = arm.data.bones.get(ROOT_BONE)
    hip_z = (arm.matrix_world @ hip_bone.head_local).z

    # Snapshot the original weights so untouched vertices survive verbatim.
    orig = []
    gnames = [g.name for g in mesh.vertex_groups]
    for v in mesh.data.vertices:
        orig.append([(gnames[ge.group], ge.weight) for ge in v.groups
                     if ge.group < len(gnames)])

    mesh.vertex_groups.clear()
    groups = {}
    def group(name):
        if name not in groups:
            groups[name] = mesh.vertex_groups.get(name) or mesh.vertex_groups.new(name=name)
        return groups[name]

    # CROSSFADE THE SEAM. A hard cut at hip_z leaves a visible pinch at the waist
    # where solved leg weights butt against the original torso weights. Blend the
    # two sets over a band around the hip so the transition is continuous.
    blend = height * 0.10
    reweighted = blended = 0
    for i, v in enumerate(mesh.data.vertices):
        p = mesh.matrix_world @ v.co
        # a = how much of the SOLVED leg weighting this vertex takes
        if not segs or p.z >= hip_z + blend:
            a = 0.0
        elif p.z <= hip_z - blend:
            a = 1.0
        else:
            t = (hip_z + blend - p.z) / (2.0 * blend)
            a = t * t * (3.0 - 2.0 * t)          # smoothstep
        if a <= 0.0:
            for gn, w in orig[i]:
                group(gn).add([i], w, 'REPLACE')
            continue
        acc = {}
        if a < 1.0:
            for gn, w in orig[i]:
                acc[gn] = acc.get(gn, 0.0) + w * (1.0 - a)
            blended += 1
        ranked = sorted(((seg_dist(p, h_, t_), n) for n, (h_, t_) in segs.items()))[:2]
        inv = [(1.0 / max(d, 1e-4), n) for d, n in ranked]
        tot = sum(w for w, _ in inv)
        for w, n in inv:
            acc[n] = acc.get(n, 0.0) + (w / tot) * a
        s = sum(acc.values()) or 1.0
        for gn, w in acc.items():
            group(gn).add([i], w / s, 'REPLACE')
        reweighted += 1
    log(f"re-weighted {reweighted} vertices across {len(segs)} leg bones "
        f"({blended} crossfaded over a {2*blend:.3f} m band at the hip)")

    mod = mesh.modifiers.new(name="Armature", type='ARMATURE')
    mod.object = arm
    if mesh.parent is not arm:
        mesh.parent = arm
    after_w = leg_weight_centroid()

    changed = False
    for k in before_w:
        b_, a_ = before_w[k], after_w[k]
        log(f"  weight centroid {k:11s} {b_ if b_ is None else round(b_,4)} -> "
            f"{a_ if a_ is None else round(a_,4)}")
        if b_ is not None and a_ is not None and abs(b_ - a_) > 1e-4:
            changed = True
    if not changed:
        raise RuntimeError("re-bind produced IDENTICAL weights — automatic weighting "
                           "did not run; do not ship this file")
    log("re-bound with automatic weights (verified changed)")

    if VERIFY:
        # Pose ONE leg and measure the OTHER leg's MESH. Measuring the other
        # leg's BONE proves nothing — sibling bones cannot move each other, so
        # that test passes even on a fused rig. What matters is whether the
        # right leg's VERTICES follow the left leg's bone, which is exactly the
        # weight bleed that tore every previous attempt.
        import math
        dg = bpy.context.evaluated_depsgraph_get()

        # Fixed index set chosen from the REST pose, so both samples describe the
        # same vertices (an evaluated mesh can differ in count between calls).
        leg_z = 0.45 * (max(zs) - min(zs))
        idx = [i for i, p in enumerate(verts) if p.z < leg_z and p.x < body_cx - 0.02]

        def right_leg_pts():
            d2 = bpy.context.evaluated_depsgraph_get()
            ev = mesh.evaluated_get(d2)
            me = ev.to_mesh()
            mw2 = ev.matrix_world
            pts = [mw2 @ me.vertices[i].co for i in idx if i < len(me.vertices)]
            ev.to_mesh_clear()
            return pts

        bpy.context.view_layer.objects.active = arm
        bpy.ops.object.mode_set(mode='POSE')
        base = right_leg_pts()
        ul = arm.pose.bones.get("LeftUpLeg")
        ul.rotation_mode = 'XYZ'
        ul.rotation_euler.x = math.radians(45)
        bpy.context.view_layer.update()
        dg = bpy.context.evaluated_depsgraph_get()
        moved_pts = right_leg_pts()
        if base and len(base) == len(moved_pts):
            d = [(a - b).length for a, b in zip(moved_pts, base)]
            log(f"VERIFY: LeftUpLeg 45deg -> RIGHT-LEG MESH moved max={max(d):.4f} "
                f"mean={sum(d)/len(d):.4f} over {len(d)} verts (want ~0)")
        else:
            log("VERIFY: vertex counts differ, skipped")
        ul.rotation_euler.x = 0.0
        bpy.context.view_layer.update()
        bpy.ops.object.mode_set(mode='OBJECT')

    for a in list(bpy.data.actions):
        bpy.data.actions.remove(a)
    outdir = os.path.dirname(OUT_GLB)
    if outdir:
        os.makedirs(outdir, exist_ok=True)
    bpy.ops.export_scene.gltf(
        filepath=OUT_GLB, export_format='GLB', export_yup=True,
        use_selection=False, export_animations=False,
        export_materials='EXPORT', export_image_format='AUTO',
        export_texcoords=True, export_normals=True, export_tangents=True,
        export_skins=True,
    )
    log("EXPORTED:", OUT_GLB)


main()
