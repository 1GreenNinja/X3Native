#!/usr/bin/env python3
"""Probe each weapon viewmodel GLB for (a) a muzzle/barrel socket node and
(b) the geometric barrel tip in glTF SCENE space (post-node-transform).

The FP viewmodel world matrix is composeTRS(bx,by,bz, vmScale*2.0, pos) and each
drawable is drawn as model * nodeTransform, so a point expressed in glTF SCENE
space (i.e. after the node transform) maps to the world by `model` alone.
The viewmodel basis maps local +Z -> ~camera forward (vmYaw 193deg flip), so the
barrel tip is the +Z extreme of the geometry.

Usage: python tools/weapon_muzzle_probe.py
"""
import json
import os
import struct
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "rigged_glb")
GLBS = [
    "WeaponEnergyPistol2.glb",
    "WeaponRailgun.glb",
    "WeaponShotgun2.glb",
    "WeaponBFG.glb",
    "WeaponRocketLauncher.glb",
    # Canon-12 armory swap (2026-08): Protofactor SCI FI SHOOTER VOL 3 models
    # for the laser + napalm, and the cyan cryo reskin of the same pack's
    # lightning gun for the freezeray.
    "WeaponSciFiLaserGun.glb",
    "WeaponSciFiMissileLauncher.glb",
    "WeaponFreezeRayCryo.glb",
]

COMPONENT = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2), 5123: ("H", 2), 5125: ("I", 4), 5126: ("f", 4)}
NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


def load_glb(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, ver, length = struct.unpack_from("<III", data, 0)
    assert magic == 0x46546C67, "not a glb"
    off = 12
    js, bin_chunk = None, b""
    while off < length:
        clen, ctype = struct.unpack_from("<II", data, off)
        off += 8
        chunk = data[off:off + clen]
        if ctype == 0x4E4F534A:
            js = json.loads(chunk.decode("utf-8"))
        elif ctype == 0x004E4942:
            bin_chunk = chunk
        off += clen + ((4 - clen % 4) % 4) if clen % 4 else off + clen - off + clen * 0
        off = off  # padding handled below
        # recompute: chunks are 4-byte aligned
    # simpler second pass (the above padding math is fiddly)
    off = 12
    js, bin_chunk = None, b""
    while off + 8 <= len(data):
        clen, ctype = struct.unpack_from("<II", data, off)
        off += 8
        chunk = data[off:off + clen]
        if ctype == 0x4E4F534A:
            js = json.loads(chunk.decode("utf-8"))
        elif ctype == 0x004E4942:
            bin_chunk = chunk
        off += clen
        off += (4 - (off % 4)) % 4
    return js, bin_chunk


def read_accessor(js, bin_chunk, idx):
    acc = js["accessors"][idx]
    ctype, csize = COMPONENT[acc["componentType"]]
    n = NCOMP[acc["type"]]
    count = acc["count"]
    bv = js["bufferViews"][acc["bufferView"]]
    base = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = bv.get("byteStride") or (csize * n)
    out = []
    for i in range(count):
        o = base + i * stride
        out.append(struct.unpack_from("<" + ctype * n, bin_chunk, o))
    return out


def mat_mul(a, b):
    # column-major 4x4
    r = [0.0] * 16
    for c in range(4):
        for row in range(4):
            r[c * 4 + row] = sum(a[k * 4 + row] * b[c * 4 + k] for k in range(4))
    return r


def trs(node):
    if "matrix" in node:
        return list(node["matrix"])
    t = node.get("translation", [0, 0, 0])
    q = node.get("rotation", [0, 0, 0, 1])
    s = node.get("scale", [1, 1, 1])
    x, y, z, w = q
    rot = [
        1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w), 0,
        2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w), 0,
        2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y), 0,
        0, 0, 0, 1,
    ]
    for c in range(3):
        for r in range(3):
            rot[c * 4 + r] *= s[c]
    rot[12], rot[13], rot[14] = t
    return rot


def xform(m, p):
    return (
        m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12],
        m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
        m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14],
    )


def main():
    for name in GLBS:
        path = os.path.abspath(os.path.join(ROOT, name))
        if not os.path.exists(path):
            print("MISSING", path)
            continue
        js, binc = load_glb(path)
        nodes = js.get("nodes", [])
        print("=" * 70)
        print(name)
        print("  nodes:", [n.get("name", "?") for n in nodes][:40])
        # scene-space vertex sweep
        pts = []
        stack = []
        scene = js.get("scenes", [{}])[js.get("scene", 0)]
        for ri in scene.get("nodes", []):
            stack.append((ri, [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]))
        while stack:
            ni, parent = stack.pop()
            node = nodes[ni]
            world = mat_mul(parent, trs(node))
            if "mesh" in node:
                for prim in js["meshes"][node["mesh"]]["primitives"]:
                    pi = prim["attributes"].get("POSITION")
                    if pi is None:
                        continue
                    for p in read_accessor(js, binc, pi):
                        pts.append(xform(world, p))
            for c in node.get("children", []):
                stack.append((c, world))
        if not pts:
            print("  NO GEOMETRY")
            continue
        mn = [min(p[i] for p in pts) for i in range(3)]
        mx = [max(p[i] for p in pts) for i in range(3)]
        ext = [mx[i] - mn[i] for i in range(3)]
        print("  verts=%d  aabb min=(%.3f %.3f %.3f) max=(%.3f %.3f %.3f) ext=(%.3f %.3f %.3f)"
              % (len(pts), mn[0], mn[1], mn[2], mx[0], mx[1], mx[2], ext[0], ext[1], ext[2]))
        # barrel tip: front slice along +Z (viewmodel basis maps local +Z -> camera forward)
        for axis, label in ((2, "+Z"),):
            thresh = mx[axis] - 0.04 * max(ext[axis], 1e-4)
            front = [p for p in pts if p[axis] >= thresh]
            cx = sum(p[0] for p in front) / len(front)
            cy = sum(p[1] for p in front) / len(front)
            cz = sum(p[2] for p in front) / len(front)
            print("  barrel tip (%s front-slice centroid, %d verts): (%.4f, %.4f, %.4f)"
                  % (label, len(front), cx, cy, cz))
            print("    -> muzzleLocal = { %.3ff, %.3ff, %.3ff }" % (cx, cy, cz))


if __name__ == "__main__":
    main()
