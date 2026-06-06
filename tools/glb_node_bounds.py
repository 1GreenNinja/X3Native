#!/usr/bin/env python3
"""Per-node world-space AABBs for chosen substrings, so we can find floor tiers,
stairs, and walls in a baked Unity scene GLB. Usage:
  python glb_node_bounds.py <scene.glb> [substr1 substr2 ...]
If no substrings given, uses the BUILD set. Prints each matching mesh-node's name,
world AABB min/max, center, and extent. Also prints a Y-histogram of platform tops.
"""
import sys
import numpy as np
import pygltflib

g = pygltflib.GLTF2().load(sys.argv[1])
SUBS = [s.lower() for s in sys.argv[2:]] or [
    "room", "pilar", "plateform", "platform", "stair", "window",
    "showcase", "tube", "fence", "carpet"]

def node_local(n):
    if n.matrix:
        return np.array(n.matrix, float).reshape(4, 4).T
    t = n.translation or [0, 0, 0]
    s = n.scale or [1, 1, 1]
    x, y, z, w = n.rotation or [0, 0, 0, 1]
    R = np.array([
        [1 - 2*(y*y+z*z), 2*(x*y-z*w),     2*(x*z+y*w),     0],
        [2*(x*y+z*w),     1 - 2*(x*x+z*z), 2*(y*z-x*w),     0],
        [2*(x*z-y*w),     2*(y*z+x*w),     1 - 2*(x*x+y*y), 0],
        [0, 0, 0, 1]], float)
    S = np.diag([s[0], s[1], s[2], 1.0])
    T = np.eye(4); T[:3, 3] = t
    return T @ R @ S

rows = []  # (name, wmin, wmax)

def walk(ni, parent):
    n = g.nodes[ni]
    M = parent @ node_local(n)
    if n.mesh is not None:
        m = g.meshes[n.mesh]
        nm = (m.name or ("mesh%d" % n.mesh))
        # also try node name
        nn = (n.name or "")
        key = (nm + " " + nn).lower()
        if any(s in key for s in SUBS):
            amin = np.array([1e30]*3); amax = np.array([-1e30]*3)
            for pr in m.primitives:
                a = g.accessors[pr.attributes.POSITION]
                if a.min and a.max:
                    for xi in (a.min[0], a.max[0]):
                        for yi in (a.min[1], a.max[1]):
                            for zi in (a.min[2], a.max[2]):
                                pw = (M @ np.array([xi, yi, zi, 1.0]))[:3]
                                amin = np.minimum(amin, pw); amax = np.maximum(amax, pw)
            if amin[0] < 1e29:
                rows.append((nm + ("|" + nn if nn else ""), amin, amax))
    for c in (n.children or []):
        walk(c, M)

scene = g.scenes[g.scene or 0]
for ni in scene.nodes:
    walk(ni, np.eye(4))

# sort by min-Y so floor tiers are easy to read
rows.sort(key=lambda r: r[1][1])
print("matching nodes (%d), sorted by minY:" % len(rows))
print("%-26s %26s %26s %20s" % ("name", "min(x,y,z)", "max(x,y,z)", "extent(x,y,z)"))
for nm, mn, mx in rows:
    ext = mx - mn
    print("%-26s %26s %26s %20s" % (
        nm[:26],
        str(np.round(mn, 1).tolist()),
        str(np.round(mx, 1).tolist()),
        str(np.round(ext, 1).tolist())))

# Histogram of TOP-Y values (mx[1]) rounded to nearest 1m — floor levels cluster.
print("\nTop-Y histogram (max-Y rounded to 1m -> count):")
from collections import Counter
topc = Counter(int(round(mx[1])) for _, _, mx in rows)
for y in sorted(topc):
    print("  topY ~%4d : %3d nodes" % (y, topc[y]))
print("\nMin-Y histogram (min-Y rounded to 1m -> count):")
minc = Counter(int(round(mn[1])) for _, mn, _ in rows)
for y in sorted(minc):
    print("  minY ~%4d : %3d nodes" % (y, minc[y]))
