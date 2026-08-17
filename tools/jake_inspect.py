"""jake_inspect.py — empirical survey of Jake_44_actions.glb before the root bake.

Prints: scene roots + TRS, which nodes the animation channels target (the go /
no-go for zeroing the Armature root translation), the clip list, and the
bind-pose facing measured from the skeleton itself (toe vs foot Z, nose vs head).

Usage: python tools/jake_inspect.py assets/rigged_glb/Jake_44_actions.glb
"""
import json
import struct
import sys
from collections import Counter

GLB_JSON = 0x4E4F534A
GLB_BIN = 0x004E4942


def load_glb(path):
    data = open(path, "rb").read()
    length = struct.unpack_from("<I", data, 8)[0]
    off, js, bin_off = 12, None, None
    while off < length:
        clen, ctype = struct.unpack_from("<II", data, off)
        if ctype == GLB_JSON:
            js = json.loads(data[off + 8:off + 8 + clen])
        elif ctype == GLB_BIN:
            bin_off = off + 8
        off += 8 + clen
    return data, js, bin_off


def trs_matrix(node):
    import math
    t = node.get("translation", [0, 0, 0])
    q = node.get("rotation", [0, 0, 0, 1])
    s = node.get("scale", [1, 1, 1])
    x, y, z, w = q
    m = [
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ]
    for r in range(3):
        for c in range(3):
            m[r][c] *= s[c]
    return m, t


def world_pos(nodes, parents, idx):
    """Bind-pose (rest) world position of node idx, composing TRS up the chain."""
    chain = []
    n = idx
    while n is not None:
        chain.append(n)
        n = parents.get(n)
    p = [0.0, 0.0, 0.0]
    for n in chain:  # leaf..root: apply from leaf upward
        m, t = trs_matrix(nodes[n])
        p = [m[r][0] * p[0] + m[r][1] * p[1] + m[r][2] * p[2] + t[r] for r in range(3)]
    return p


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "assets/rigged_glb/Jake_44_actions.glb"
    data, js, bin_off = load_glb(path)
    nodes = js["nodes"]
    roots = js["scenes"][js.get("scene", 0)]["nodes"]
    print("scene roots:", roots)
    for r in roots:
        n = nodes[r]
        print(f"  root {r}: '{n.get('name')}' T={n.get('translation')} "
              f"R={n.get('rotation')} S={n.get('scale')}")
        for c in n.get("children", []):
            cn = nodes[c]
            print(f"    child {c}: '{cn.get('name')}' T={cn.get('translation')} "
                  f"R={cn.get('rotation')} mesh={cn.get('mesh')} skin={cn.get('skin')}")

    parents = {}
    for i, n in enumerate(nodes):
        for c in n.get("children", []):
            parents[c] = i

    anims = js.get("animations", [])
    print(f"\nclips ({len(anims)}):", [a.get("name") for a in anims])
    tgt = Counter()
    for a in anims:
        for ch in a["channels"]:
            t = ch["target"]
            tgt[(t.get("node"), t["path"])] += 1
    trans_nodes = sorted({n for (n, p) in tgt if p == "translation"})
    rot_nodes = sorted({n for (n, p) in tgt if p == "rotation"})
    print("translation channels target:",
          [(n, nodes[n].get("name")) for n in trans_nodes])
    arm_idx = next(i for i, n in enumerate(nodes) if n.get("name") == "Armature")
    print(f"Armature node index = {arm_idx}; targeted by rotation channels? "
          f"{arm_idx in rot_nodes}; by translation? {arm_idx in trans_nodes}")

    # Empirical facing: rest-pose world positions of facing-revealing bone pairs.
    by_name = {n.get("name", ""): i for i, n in enumerate(nodes)}
    pairs = [
        ("mixamorigLeftFoot", "mixamorigLeftToeBase"),
        ("mixamorigRightFoot", "mixamorigRightToeBase"),
        ("mixamorigNeck", "mixamorigHead"),
    ]
    print("\nbind-pose facing check (world positions, glTF space):")
    for a, b in pairs:
        if a in by_name and b in by_name:
            pa = world_pos(nodes, parents, by_name[a])
            pb = world_pos(nodes, parents, by_name[b])
            dz = pb[2] - pa[2]
            print(f"  {a} {['%+.4f' % v for v in pa]} -> {b} "
                  f"{['%+.4f' % v for v in pb]}  dZ={dz:+.4f}")
    hips = by_name.get("mixamorigHips")
    if hips is not None:
        print("  hips world:", ["%+.4f" % v for v in world_pos(nodes, parents, hips)])

    # Mesh-level check: skin 0's mesh vertex Z extent front/back asymmetry is a
    # weaker signal; the toe direction above is the decider.


if __name__ == "__main__":
    main()
