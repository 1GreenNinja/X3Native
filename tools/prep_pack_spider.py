"""
prep_pack_spider.py - headless Blender harvest of an ALREADY-RIGGED + ANIMATED
spider from a licensed Unity character pack into an X3Native rigged_glb.

    blender-launcher.exe --background --python prep_pack_spider.py -- \
        <src.fbx> <diffuse.png> <out.glb> [tex_size] [zlift]

WHY a dedicated prep instead of plain tools/convert_fbx_glb.py: the pack ships a
single self-contained FBX per spider that ALREADY carries every anim stack
(idle / idle_2 / walk / attack_1 / attack_2 / dead) on a real 70-bone arachnid
rig, so no clip fusing is needed - but three things still stand between it and
the engine:

  1. TEXTURE. The FBX's material references the pack's diffuse by an authoring
     -machine path, so a straight convert exports an UNTEXTURED grey material.
     We bind <diffuse.png> into Base Color ourselves, downsize to <tex_size>
     (2048 source is overkill for a ~1 m prop) and PACK it into the GLB.
  2. CLIP NAMES. MonsterSystem's fuzzy resolver (app/monster.cpp) wants
     idle / walk / run / attack / attack2 / death. The pack's "dead" matches
     NEITHER "death" nor "die", and "attack_2" does not match "attack2", so two
     slots would silently resolve to -1. We rename the ACTIONS to the canonical
     names, which makes the default resolver land every slot with ZERO
     MonsterSystem::overrideClip calls at the call site.
  3. NO RUN CLIP. The pack has walk but no run. We derive Run by copying Walk
     and compressing its keyframe timing (RUN_TIME_SCALE) - a scuttle. Flagged
     in the log because it is DERIVED, not authored.

Orientation: X3Native's rigged character GLBs are authored facing +Z in glTF
space (see the FACING FIX in app/monster.cpp). Blender's default FBX import
(-Z forward / Y up) lands the pack's Unity +Z-facing spider on Blender -Y, which
export_yup maps straight back to glTF +Z. So no rotation is applied - the QA
render (tools/pose_render_grounded.py, camera on -Y looking toward +Y) shows the
spider's FRONT when this is right.

ENV NOTE (this box): Blender is the Microsoft Store package - the launcher
DETACHES (stdout lost), so this script writes <out>.log + a <out>.done marker
the caller polls. Same convention as tools/animate_creature.py / prep_canon_alien.py.

Clean-room: public Blender Python API + glTF 2.0 spec only.
"""
import bpy, sys, os

ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(ARGV) < 3:
    raise SystemExit("Usage: ... -- <src.fbx> <diffuse.png> <out.glb> [tex_size] [zlift]")
SRC, TEX, OUT = ARGV[0], ARGV[1], ARGV[2]
TEX_SIZE = int(ARGV[3]) if len(ARGV) > 3 else 1024
ZLIFT    = float(ARGV[4]) if len(ARGV) > 4 else 0.0

RUN_TIME_SCALE = 0.62   # Walk keyframes compressed to this fraction -> Run scuttle

# Pack clip name (lowercase, after the "Armature|" prefix) -> engine clip name.
CLIP_MAP = {
    "idle":     "Idle",
    "idle_2":   "Idle2",
    "walk":     "Walk",
    "attack_1": "Attack",
    "attack_2": "Attack2",
    "dead":     "Death",
}

LOG_PATH, DONE_PATH = OUT + ".log", OUT + ".done"
_log = []
def log(*a):
    s = "[spider] " + " ".join(str(x) for x in a)
    _log.append(s); print(s)

def flush(status):
    try:
        with open(LOG_PATH, "w", encoding="utf-8") as f:
            f.write("\n".join(_log))
        with open(DONE_PATH, "w", encoding="utf-8") as f:
            f.write(status)
    except Exception as e:
        print("[spider] could not write log/marker:", e)


def main():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)

    # ---- 1. import the FBX (all anim stacks come in as separate actions) ----
    # FACING: X3Native's rigged GLBs are authored facing +Z in glTF space (the
    # FACING FIX in app/monster.cpp yaws the mesh 180 on top of the AI heading).
    # Blender's DEFAULT fbx axis conversion (-Z forward) lands this pack's spider
    # facing +Y in Blender == -Z in glTF: exactly BACKWARDS (verified with an
    # orthographic top-down axis render - carapace toward +Y, abdomen toward -Y).
    # Importing with axis_forward='Z' is the same conversion rotated 180 about up,
    # so the spider lands facing -Y in Blender == +Z in glTF. Doing it at IMPORT
    # bakes the flip through the mesh, the rest pose AND the animation curves in
    # one consistent step - far safer than rotating an object whose actions also
    # key the object transform (they do: see the non-pose fcurve count below).
    # (use_manual_orientation is REQUIRED - without it the axis_* args are ignored
    # and the FBX's own axis metadata wins, which is how the first pass silently
    # shipped a backwards spider.)
    bpy.ops.import_scene.fbx(filepath=SRC, use_manual_orientation=True,
                             axis_forward='Z', axis_up='Y')
    meshes = [o for o in bpy.data.objects if o.type == 'MESH']
    arms   = [o for o in bpy.data.objects if o.type == 'ARMATURE']
    if not meshes or not arms:
        raise RuntimeError("expected a skinned mesh + armature in " + SRC)
    log("imported", os.path.basename(SRC), "meshes:", len(meshes),
        "armatures:", len(arms), "bones:", len(arms[0].data.bones),
        "actions:", len(bpy.data.actions))

    # ---- 2. bind + pack the pack's diffuse into Base Color ----
    img = bpy.data.images.load(TEX)
    if TEX_SIZE > 0 and (img.size[0] > TEX_SIZE or img.size[1] > TEX_SIZE):
        log("texture", img.size[0], "x", img.size[1], "-> scaled to", TEX_SIZE)
        img.scale(TEX_SIZE, TEX_SIZE)
    img.pack()
    bound = 0
    for mesh in meshes:
        for slot in mesh.material_slots:
            mat = slot.material
            if mat is None:
                continue
            mat.use_nodes = True
            nt = mat.node_tree
            bsdf = next((n for n in nt.nodes if n.type == 'BSDF_PRINCIPLED'), None)
            if bsdf is None:
                bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
                out = next((n for n in nt.nodes if n.type == 'OUTPUT_MATERIAL'), None) \
                      or nt.nodes.new("ShaderNodeOutputMaterial")
                nt.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])
            texnode = nt.nodes.new("ShaderNodeTexImage")
            texnode.image = img
            texnode.image.colorspace_settings.name = 'sRGB'
            nt.links.new(texnode.outputs["Color"], bsdf.inputs["Base Color"])
            # ---- FORCE OPAQUE. The FBX importer wires the pack's FBX
            # TransparencyFactor into Principled.Alpha through a Math chain and
            # leaves blend_method HASHED. On this pack that alpha evaluates to ~0,
            # so the spider exports as a 100% TRANSPARENT mesh - it imports and
            # skins fine, reports correct bounds, and renders NOTHING. (That is
            # exactly what a first pass here did: six QA frames of empty floor.)
            # Unity ignored the factor; we must too. Kill the alpha link, pin
            # Alpha = 1, and drop the now-dangling Math nodes so the glTF exporter
            # writes alphaMode OPAQUE.
            for link in list(nt.links):
                if link.to_node is bsdf and link.to_socket.name == "Alpha":
                    nt.links.remove(link)
            bsdf.inputs["Alpha"].default_value = 1.0
            for n in list(nt.nodes):
                if n.type == 'MATH' and not any(o.is_linked for o in n.outputs):
                    nt.nodes.remove(n)
            for n in list(nt.nodes):   # second sweep: the freed chain upstream
                if n.type == 'MATH' and not any(o.is_linked for o in n.outputs):
                    nt.nodes.remove(n)
            try:
                mat.blend_method = 'OPAQUE'
            except Exception:
                pass
            if hasattr(mat, "surface_render_method"):
                mat.surface_render_method = 'DITHERED'
            bsdf.inputs["Roughness"].default_value = 0.72   # chitin/hair, not plastic
            if "Metallic" in bsdf.inputs:
                bsdf.inputs["Metallic"].default_value = 0.0
            if "Specular IOR Level" in bsdf.inputs:
                bsdf.inputs["Specular IOR Level"].default_value = 0.35
            bound += 1
    log("bound diffuse to", bound, "material slot(s):", os.path.basename(TEX))

    # ---- 3. rename the pack's actions to the engine's canonical clip names ----
    renamed = {}
    for act in list(bpy.data.actions):
        raw = act.name.split("|")[-1].strip().lower()
        new = CLIP_MAP.get(raw)
        if new:
            act.name = new
            renamed[raw] = new
        else:
            log("UNMAPPED action kept as-is:", act.name)
    log("renamed clips:", ", ".join("%s->%s" % kv for kv in sorted(renamed.items())))

    # ---- 4. derive Run from Walk (time-compressed scuttle) ----
    walk = bpy.data.actions.get("Walk")
    if walk and not bpy.data.actions.get("Run"):
        run = walk.copy()
        run.name = "Run"
        run.use_fake_user = True
        for fc in run.fcurves:
            for kp in fc.keyframe_points:
                kp.co.x          *= RUN_TIME_SCALE
                kp.handle_left.x  *= RUN_TIME_SCALE
                kp.handle_right.x *= RUN_TIME_SCALE
            fc.update()
        log("derived Run from Walk at x%.2f timing (DERIVED, not authored)" % RUN_TIME_SCALE)
    for act in bpy.data.actions:
        act.use_fake_user = True     # keep every clip alive through export

    # ---- 5. optional ground lift (feet to z=0); object-level, actions are pose-only ----
    if abs(ZLIFT) > 1e-6:
        for a in arms:
            a.location.z += ZLIFT
        log("applied zlift", ZLIFT)

    # Report whether any action drives the OBJECT transform (would fight a zlift).
    objchan = sum(1 for a in bpy.data.actions for fc in a.fcurves
                  if not fc.data_path.startswith("pose.bones"))
    log("non-pose-bone fcurves across all actions:", objchan)

    # ---- 6. export ONE multi-clip GLB, Y-up, textures packed ----
    bpy.ops.object.select_all(action='SELECT')
    kwargs = dict(filepath=OUT, export_format='GLB', export_yup=True,
                  export_apply=False, export_animations=True,
                  export_animation_mode='ACTIONS', export_image_format='AUTO')
    try:
        bpy.ops.export_scene.gltf(**kwargs)
    except TypeError:
        kwargs.pop("export_animation_mode", None)
        bpy.ops.export_scene.gltf(**kwargs)
    log("EXPORTED", OUT, os.path.getsize(OUT) // 1024, "KB")


try:
    main()
    flush("OK")
except Exception as e:
    import traceback
    log("ERROR:", e); log(traceback.format_exc())
    flush("ERR " + str(e))
