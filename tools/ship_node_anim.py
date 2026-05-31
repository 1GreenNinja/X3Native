#!/usr/bin/env python3
"""S11 — author ARTICULATED ship parts as NAMED NODES for ShipNodeAnim.

WHY THIS EXISTS
---------------
Ultra-detailed Act-3 ships are RIGID hulls with a handful of moving parts —
landing gear, hull panels, turrets — driven by NODE-TRANSFORM animation, NOT
skeletal skinning. The engine runtime is `app/space/ship_anim.{h,cpp}`
(`x3::space::ShipNodeAnim`):

    anim.bind(scene, shipRootEntity);
    anim.addPart("landing_gear", poseRetracted, poseDeployed, gearChildEntity);
    anim.setPart("landing_gear", 0.0 .. 1.0);   // 0 = retracted, 1 = deployed
    anim.update(dt, scene);                       // lerps + writes the child xform

For that to drive REAL geometry, the ship GLB must carry each moving part as a
SEPARATE, NAMED glTF child node, with TWO key-poses authored on it:
  * key-pose 0 (t = 0)  -> retracted / closed / stowed
  * key-pose 1 (t = 1)  -> deployed  / open   / extended

The four SHIPPED SpaceShip*.glb assets are single-hull (their only nodes are
`Root` / `Character` / `Armature` — no articulated parts), so the
`--world shipanim` showcase drives a SYNTHETIC gear child to prove the runtime.
This script is the authoring path so future ships get REAL named nodes that
`ShipNodeAnim::setPart` drives directly.

NAMING CONTRACT (what the engine looks up by name)
--------------------------------------------------
  landing_gear          single gear assembly
  landing_gear_fl/fr/.. per-strut gear (front-left, front-right, ...)
  panel_A, panel_B, ..  hull access / cargo panels
  turret_main, turret_* rotating weapon mounts
Each is a direct child of the hull root node, and its LOCAL transform at
key-pose 0 vs key-pose 1 is what `setPart(0)` vs `setPart(1)` interpolates.

HOW TO AUTHOR IN BLENDER
------------------------
1.  Model the hull as the root object. Parent each moving part to it as its own
    object and RENAME the object to the contract name (e.g. `landing_gear`).
    Blender object names become glTF node names on export.
2.  Set the part's RETRACTED local transform (its rest position) — this is
    key-pose 0.
3.  Decide the DEPLOYED local transform (gear dropped & rotated out, panel slid
    open, turret swung). This is key-pose 1. You DON'T need a glTF animation
    track — `ShipNodeAnim` stores the two poses itself and lerps between them.
    But authoring a 2-keyframe action (frame 0 = retracted, frame 1 = deployed)
    is the convenient way to capture both matrices; this script can dump them.
4.  Export glTF Binary (.glb):  File > Export > glTF 2.0
        Format: GLB
        Include: [x] Selected Objects (hull + parts) ; [x] Custom Properties
        Transform: +Y Up (default)
        Geometry: [x] Apply Modifiers, [x] UVs, [x] Normals, [x] Materials
        Animation: keep OFF unless you also want a baked clip — node-transform
                   articulation does NOT need a glTF animation; the two poses
                   are supplied to addPart() in code.
5.  Drop the .glb in  assets/rigged_glb/  (git-lfs already tracks *.glb).

WIRING IT UP IN THE ENGINE (host side, per part)
------------------------------------------------
    Model m = loader->load("MyShip.glb");
    auto drawables = makeDrawables(m);
    // find the named node + its retracted/deployed local matrices:
    int gearNode = -1;
    for (int i = 0; i < (int)m.nodes.size(); ++i)
        if (m.nodes[i].name == "landing_gear") gearNode = i;
    // poseRetracted = m.nodes[gearNode].localTransform (as authored).
    // poseDeployed  = the deployed local matrix (from the 2nd keyframe or code).
    anim.addPart("landing_gear", poseRetracted, poseDeployed, gearChildEntity);

USAGE OF THIS SCRIPT
--------------------
  # Inspect a GLB: list nodes and flag the ones matching the naming contract.
  python tools/ship_node_anim.py inspect assets/rigged_glb/SpaceShip.glb

  # Dump a named node's local transform (the retracted key-pose, as a 4x4
  # column-major matrix ready to paste into addPart()).
  python tools/ship_node_anim.py pose assets/rigged_glb/MyShip.glb landing_gear

  # (Blender) print the two key-pose matrices of a 2-keyframe action. Run from
  # inside Blender's Python console / `blender --background --python`:
  python tools/ship_node_anim.py blender-keyposes landing_gear

This script has NO third-party deps for inspect/pose (parses the GLB JSON chunk
directly); the `blender-keyposes` mode only runs inside Blender (imports `bpy`).
"""
import json
import struct
import sys

PART_PREFIXES = ("landing_gear", "panel_", "turret_")


def _read_glb_json(path):
    with open(path, "rb") as f:
        magic, version, length = struct.unpack("<III", f.read(12))
        if magic != 0x46546C67:  # 'glTF'
            raise ValueError("not a GLB file: %s" % path)
        clen, ctype = struct.unpack("<II", f.read(8))
        if ctype != 0x4E4F534A:  # 'JSON'
            raise ValueError("first chunk is not JSON")
        return json.loads(f.read(clen))


def _node_local_matrix(node):
    """Return the node's local 4x4 (column-major, 16 floats).

    glTF nodes carry EITHER an explicit `matrix` OR TRS (translation/rotation/
    scale). We compose TRS -> matrix so the result is paste-ready for addPart().
    """
    if "matrix" in node:
        return list(node["matrix"])  # already column-major
    import math  # local import: only needed for TRS compose
    t = node.get("translation", [0.0, 0.0, 0.0])
    r = node.get("rotation", [0.0, 0.0, 0.0, 1.0])  # quat xyzw
    s = node.get("scale", [1.0, 1.0, 1.0])
    x, y, z, w = r
    # Rotation matrix from quaternion (column-major).
    xx, yy, zz = x * x, y * y, z * z
    xy, xz, yz = x * y, x * z, y * z
    wx, wy, wz = w * x, w * y, w * z
    m = [
        (1 - 2 * (yy + zz)) * s[0], (2 * (xy + wz)) * s[0], (2 * (xz - wy)) * s[0], 0.0,
        (2 * (xy - wz)) * s[1], (1 - 2 * (xx + zz)) * s[1], (2 * (yz + wx)) * s[1], 0.0,
        (2 * (xz + wy)) * s[2], (2 * (yz - wx)) * s[2], (1 - 2 * (xx + yy)) * s[2], 0.0,
        t[0], t[1], t[2], 1.0,
    ]
    return m


def cmd_inspect(path):
    js = _read_glb_json(path)
    nodes = js.get("nodes", [])
    print("GLB: %s" % path)
    print("  nodes=%d  meshes=%d  animations=%d"
          % (len(nodes), len(js.get("meshes", [])), len(js.get("animations", []))))
    matched = 0
    for i, n in enumerate(nodes):
        name = n.get("name", "<unnamed>")
        is_part = name.startswith(PART_PREFIXES)
        if is_part:
            matched += 1
        print("  [%2d] %-24s %s" % (i, name, "<-- ARTICULATED PART" if is_part else ""))
    if matched == 0:
        print("  WARNING: no nodes match the naming contract %s." % (PART_PREFIXES,))
        print("           ShipNodeAnim has nothing to drive on this hull — author")
        print("           named child nodes (see this file's header) before export.")
    else:
        print("  OK: %d articulated part node(s) found." % matched)
    return 0


def _fmt_matrix(m):
    rows = []
    for r in range(4):
        rows.append("    " + ", ".join("%9.5ff" % m[c * 4 + r] for c in range(4)))
    return "{\n" + ",\n".join(rows) + "\n}"


def cmd_pose(path, node_name):
    js = _read_glb_json(path)
    for n in js.get("nodes", []):
        if n.get("name") == node_name:
            m = _node_local_matrix(n)
            print("// local transform (column-major) of node '%s' in %s" % (node_name, path))
            print("// This is the RETRACTED key-pose (t=0). Author the DEPLOYED")
            print("// pose (t=1) separately and pass both to addPart().")
            print("const float %s_poseRetracted[16] = %s;" % (node_name, _fmt_matrix(m)))
            return 0
    print("node '%s' not found in %s" % (node_name, path), file=sys.stderr)
    return 1


def cmd_blender_keyposes(node_name):
    try:
        import bpy  # noqa: F401  (only available inside Blender)
    except ImportError:
        print("blender-keyposes must run inside Blender (bpy unavailable).",
              file=sys.stderr)
        return 2
    obj = bpy.data.objects.get(node_name)
    if obj is None:
        print("object '%s' not found in the .blend" % node_name, file=sys.stderr)
        return 1
    scene = bpy.context.scene
    for frame, label in ((0, "Retracted (t=0)"), (1, "Deployed (t=1)")):
        scene.frame_set(frame)
        m = obj.matrix_local  # Blender Matrix, row-major; transpose -> column-major
        col = [m[r][c] for c in range(4) for r in range(4)]
        print("// %s key-pose of '%s'" % (label, node_name))
        print(_fmt_matrix(col))
    return 0


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    cmd = argv[1]
    if cmd == "inspect" and len(argv) == 3:
        return cmd_inspect(argv[2])
    if cmd == "pose" and len(argv) == 4:
        return cmd_pose(argv[2], argv[3])
    if cmd == "blender-keyposes" and len(argv) == 3:
        return cmd_blender_keyposes(argv[2])
    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
