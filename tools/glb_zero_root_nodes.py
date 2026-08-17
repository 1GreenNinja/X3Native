#!/usr/bin/env python3
"""glb_zero_root_nodes.py — flatten a GLB's ROOT node transforms to identity.

WHY THIS EXISTS (W-TRAFFIC2, 2026-08-17). Blender's glTF exporter can emit a
root node carrying the scene's unit conversion even when the mesh vertices are
already in metres and the object transform has been applied. The RCC police
light bar came out with correct vertex data -- accessor extents
1.3775 x 0.0979 x 0.2485 m, centred, base at y=0, exactly as authored -- under a
node with scale 0.0254 and a stray translation. Setting
scene.unit_settings.scale_length = 1.0 and clearing obj.matrix_world INSIDE the
Blender script did not remove it.

The engine caught it, which is the point of measuring: readGlbForLod applies the
full node hierarchy, so app/traffic.cpp's boot log read the light bar as
`0.035 x 0.002 x 0.006 m` -- a 3.5 cm light bar -- instead of eyeballing a
render and wondering why the cop looked bare (X3_WORLD_RULES rule 0).

Rather than keep guessing at exporter flags, this asserts the invariant
directly and VERIFIES it: for every root node whose transform is a pure
translate/scale (no rotation), the transform is folded away and the node is
left at identity. It refuses to touch a node with a rotation or a general
matrix, because folding those into vertex data is a real transform bake and
belongs in the authoring tool, not in a clean-up pass.

This is safe ONLY when the vertex data is already the intended geometry -- i.e.
the transform is residue, not authorship. The tool prints the before/after
world extents so the caller can confirm exactly that.

Usage:
    python tools/glb_zero_root_nodes.py <file.glb> [--expect W,H,D] [--tol 0.01]

--expect makes it a GATE: the post-fix world extents must match, or it exits 1
and writes nothing.
"""
import argparse, sys

from pygltflib import GLTF2


def node_srt(n):
    """(scale, translation) if the node is a pure translate/scale, else None."""
    if n.matrix is not None:
        return None
    if n.rotation is not None and any(
            abs(c - d) > 1e-6 for c, d in zip(n.rotation, (0.0, 0.0, 0.0, 1.0))):
        return None
    s = list(n.scale) if n.scale is not None else [1.0, 1.0, 1.0]
    t = list(n.translation) if n.translation is not None else [0.0, 0.0, 0.0]
    return s, t


def world_extents(g, roots):
    """Axis-aligned world extents of every primitive under `roots`."""
    lo = [float("inf")] * 3
    hi = [float("-inf")] * 3
    for ni in roots:
        n = g.nodes[ni]
        srt = node_srt(n)
        s, t = srt if srt else ([1.0] * 3, [0.0] * 3)
        stack = [(ni, s, t)]
        while stack:
            idx, cs, ct = stack.pop()
            nd = g.nodes[idx]
            if nd.mesh is not None:
                for p in g.meshes[nd.mesh].primitives:
                    a = g.accessors[p.attributes.POSITION]
                    for k in range(3):
                        lo[k] = min(lo[k], a.min[k] * cs[k] + ct[k])
                        hi[k] = max(hi[k], a.max[k] * cs[k] + ct[k])
            for c in (nd.children or []):
                csrt = node_srt(g.nodes[c])
                s2, t2 = csrt if csrt else ([1.0] * 3, [0.0] * 3)
                stack.append((c, [cs[i] * s2[i] for i in range(3)],
                              [ct[i] + cs[i] * t2[i] for i in range(3)]))
    return lo, hi


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("glb")
    ap.add_argument("--expect", help="W,H,D metres the result MUST measure")
    ap.add_argument("--tol", type=float, default=0.01)
    args = ap.parse_args()

    g = GLTF2().load(args.glb)
    scene = g.scenes[g.scene or 0]
    roots = list(scene.nodes)

    lo0, hi0 = world_extents(g, roots)
    print(f"[zeroroot] before: extents "
          f"{hi0[0]-lo0[0]:.4f} x {hi0[1]-lo0[1]:.4f} x {hi0[2]-lo0[2]:.4f} m")

    changed = 0
    for ni in roots:
        n = g.nodes[ni]
        srt = node_srt(n)
        if srt is None:
            print(f"[zeroroot] SKIP node {n.name!r}: has a rotation or a matrix "
                  f"— baking that is authoring, not clean-up")
            continue
        s, t = srt
        if all(abs(v - 1.0) < 1e-9 for v in s) and all(abs(v) < 1e-9 for v in t):
            continue
        print(f"[zeroroot] node {n.name!r}: dropping scale {[round(v,6) for v in s]} "
              f"translation {[round(v,6) for v in t]}")
        n.scale = None
        n.translation = None
        changed += 1

    lo1, hi1 = world_extents(g, roots)
    ext = [hi1[i] - lo1[i] for i in range(3)]
    print(f"[zeroroot] after:  extents {ext[0]:.4f} x {ext[1]:.4f} x {ext[2]:.4f} m "
          f"(min y {lo1[1]:.4f})")

    if args.expect:
        want = [float(v) for v in args.expect.split(",")]
        bad = [i for i in range(3) if abs(ext[i] - want[i]) > args.tol]
        if bad:
            print(f"[zeroroot] GATE FAILED: axes {bad} differ from expected "
                  f"{want} by more than {args.tol} m — nothing written")
            return 1
        print(f"[zeroroot] gate OK against {want}")

    if changed:
        g.save(args.glb)
        print(f"[zeroroot] wrote {args.glb} ({changed} node(s) flattened)")
    else:
        print("[zeroroot] nothing to do")
    return 0


if __name__ == "__main__":
    sys.exit(main())
