# Headless Blender OBJ -> GLB converter for the X3Native weapon viewmodels.
#
# Usage:
#   blender --background --python convert_obj_glb.py -- <src.obj> <dst.glb> [texdir] [donefile]
#
# ROOT-CAUSE NOTE (2026-07): passing ONE shared <texdir> for every weapon is what
# skinned four of the five arsenal weapons with a foreign atlas (the "camo/pale
# slab" bug). Each Rodin weapon ships its OWN texture set NEXT TO its .obj, so
# <texdir> now DEFAULTS to the OBJ's own directory -- convert each weapon from its
# own folder and it gets its own maps. Only override <texdir> deliberately.
#
# Imports one Wavefront OBJ into an empty scene, REPLACES its material with a
# Principled BSDF wired to that weapon's OWN PBR texture set found in <texdir>:
#     BaseColor  <- texture_diffuse.png
#     Normal     <- texture_normal.png   (Non-Color -> Normal Map node)
#     Metallic   <- texture_metallic.png (Non-Color, R channel)
#     Roughness  <- texture_roughness.png(Non-Color, R channel)
# then exports a single binary glTF 2.0 (.glb). The glTF exporter auto-packs the
# metallic + roughness inputs into one metallicRoughness texture (G=rough, B=metal).
#
# Writes the result + the literal token CONVERTED_OK / CONVERT_FAIL to <donefile>
# (if given) so a headless caller can poll for completion -- the Store Blender
# launcher detaches, so the caller invokes the real blender.exe and waits on this.
#
# ASCII-only on purpose (Store-Blender .py gotcha from prior runs).
import bpy, sys, os

def log(*a):
    print("[obj2glb]", *a)

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(argv) < 2:
    log("ERR: need <src.obj> <dst.glb> [texdir] [donefile]")
    sys.exit(1)

src, dst = argv[0], argv[1]
# Default texdir = the OBJ's OWN directory (each weapon carries its own maps).
texdir = argv[2] if len(argv) > 2 else os.path.dirname(os.path.abspath(src))
donefile = argv[3] if len(argv) > 3 else None

def write_done(token, msg=""):
    if donefile:
        try:
            with open(donefile, "w") as f:
                f.write(token + "\n")
                if msg:
                    f.write(msg + "\n")
        except Exception as e:
            log("WARN: could not write donefile:", e)

try:
    os.makedirs(os.path.dirname(dst), exist_ok=True)

    diffuse_p   = os.path.join(texdir, "texture_diffuse.png")
    normal_p    = os.path.join(texdir, "texture_normal.png")
    metallic_p  = os.path.join(texdir, "texture_metallic.png")
    roughness_p = os.path.join(texdir, "texture_roughness.png")
    for p in (diffuse_p, normal_p, metallic_p, roughness_p):
        if not os.path.exists(p):
            raise RuntimeError("missing texture: " + p)

    # Empty scene, import the OBJ (Blender 4.x operator).
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.wm.obj_import(filepath=src)

    mesh_objs = [o for o in bpy.context.scene.objects if o.type == 'MESH']
    if not mesh_objs:
        raise RuntimeError("no mesh objects imported from " + src)
    log("imported", len(mesh_objs), "mesh object(s)")

    # Build one shared PBR material from the texture set.
    mat = bpy.data.materials.new(name="WeaponPBR")
    mat.use_nodes = True
    nt = mat.node_tree
    nodes, links = nt.nodes, nt.links
    nodes.clear()

    out = nodes.new("ShaderNodeOutputMaterial"); out.location = (600, 0)
    bsdf = nodes.new("ShaderNodeBsdfPrincipled"); bsdf.location = (260, 0)
    links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])

    def tex(path, colorspace, x, y):
        n = nodes.new("ShaderNodeTexImage")
        n.image = bpy.data.images.load(path, check_existing=True)
        try:
            n.image.colorspace_settings.name = colorspace
        except Exception:
            pass
        n.location = (x, y)
        return n

    # Base color (sRGB).
    diff = tex(diffuse_p, "sRGB", -500, 300)
    links.new(diff.outputs["Color"], bsdf.inputs["Base Color"])

    # Metallic / Roughness (Non-Color, single channel each).
    met = tex(metallic_p, "Non-Color", -500, 40)
    links.new(met.outputs["Color"], bsdf.inputs["Metallic"])
    rgh = tex(roughness_p, "Non-Color", -500, -220)
    links.new(rgh.outputs["Color"], bsdf.inputs["Roughness"])

    # Normal map (Non-Color -> Normal Map node -> Normal input).
    nrm = tex(normal_p, "Non-Color", -500, -480)
    nmap = nodes.new("ShaderNodeNormalMap"); nmap.location = (-180, -480)
    links.new(nrm.outputs["Color"], nmap.inputs["Color"])
    links.new(nmap.outputs["Normal"], bsdf.inputs["Normal"])

    # Assign the material to every imported mesh (replace all slots).
    for o in mesh_objs:
        o.data.materials.clear()
        o.data.materials.append(mat)

    bpy.ops.object.select_all(action='SELECT')

    bpy.ops.export_scene.gltf(
        filepath=dst,
        export_format='GLB',
        export_yup=True,        # glTF is Y-up (matches the engine loader)
        export_apply=True,      # apply modifiers/transforms
        export_image_format='AUTO',
        export_materials='EXPORT',
    )

    if not os.path.exists(dst) or os.path.getsize(dst) == 0:
        raise RuntimeError("export produced no/empty file: " + dst)

    log("CONVERTED_OK", src, "->", dst, os.path.getsize(dst), "bytes")
    write_done("CONVERTED_OK", "%s -> %s (%d bytes)" % (src, dst, os.path.getsize(dst)))
except Exception as e:
    import traceback
    traceback.print_exc()
    log("CONVERT_FAIL", str(e))
    write_done("CONVERT_FAIL", str(e))
    sys.exit(2)
