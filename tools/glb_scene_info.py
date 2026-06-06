#!/usr/bin/env python3
"""Report a GLB scene's real world extent + contents (does it actually contain the
geometry you expect, or just props?). Walks the node hierarchy, composes TRS, and
transforms each mesh's accessor AABB into world space.
Usage: python glb_scene_info.py <scene.glb>"""
import sys
import numpy as np
import pygltflib

g = pygltflib.GLTF2().load(sys.argv[1])

def node_local(n):
    if n.matrix:
        return np.array(n.matrix, float).reshape(4, 4).T  # glTF flat = column-major
    t = n.translation or [0, 0, 0]
    s = n.scale or [1, 1, 1]
    x, y, z, w = n.rotation or [0, 0, 0, 1]
    R = np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),     0],
        [2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),     0],
        [2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y), 0],
        [0, 0, 0, 1]], float)
    S = np.diag([s[0], s[1], s[2], 1.0])
    T = np.eye(4); T[:3, 3] = t
    return T @ R @ S

wmin = np.array([1e30] * 3); wmax = np.array([-1e30] * 3)
bmin = np.array([1e30] * 3); bmax = np.array([-1e30] * 3)   # BUILDING subset only
tris = 0; mesh_nodes = 0
names = {}
# Structural/furniture meshes = "the building" (excludes scatter: Sapin/Fern/Plant/
# Quad/Glow/Decal/Screen/Terrain/Letter/Number/Tissus/Paint/Back).
BUILD = ("room", "pilar", "plateform", "platform", "stair", "window", "showcase",
         "table", "chair", "carpet", "tube", "halogen", "cache", "tv_screen")

def walk(ni, parent):
    global wmin, wmax, bmin, bmax, tris, mesh_nodes
    n = g.nodes[ni]
    M = parent @ node_local(n)
    if n.mesh is not None:
        mesh_nodes += 1
        m = g.meshes[n.mesh]
        nm = m.name or ("mesh%d" % n.mesh)
        names[nm] = names.get(nm, 0) + 1
        isbuild = any(b in nm.lower() for b in BUILD)
        for pr in m.primitives:
            if pr.indices is not None:
                tris += g.accessors[pr.indices].count // 3
            a = g.accessors[pr.attributes.POSITION]
            if a.min and a.max:
                for xi in (a.min[0], a.max[0]):
                    for yi in (a.min[1], a.max[1]):
                        for zi in (a.min[2], a.max[2]):
                            pw = (M @ np.array([xi, yi, zi, 1.0]))[:3]
                            wmin = np.minimum(wmin, pw); wmax = np.maximum(wmax, pw)
                            if isbuild:
                                bmin = np.minimum(bmin, pw); bmax = np.maximum(bmax, pw)
    for c in (n.children or []):
        walk(c, M)

scene = g.scenes[g.scene or 0]
for ni in scene.nodes:
    walk(ni, np.eye(4))

print("mesh-node instances drawn:", mesh_nodes)
print("total triangles:", f"{tris:,}")
print("world AABB min:", np.round(wmin, 2).tolist())
print("world AABB max:", np.round(wmax, 2).tolist())
print("extent (m):     ", np.round(wmax - wmin, 2).tolist())
print("BUILDING AABB min:", np.round(bmin, 2).tolist(), " max:", np.round(bmax, 2).tolist())
print("BUILDING center:  ", np.round((bmin + bmax) / 2.0, 2).tolist(), " extent:", np.round(bmax - bmin, 2).tolist())
print("unique meshes (%d), [instance count]:" % len(names))
for nm in sorted(names, key=lambda k: -names[k]):
    print("   %4dx  %s" % (names[nm], nm))
