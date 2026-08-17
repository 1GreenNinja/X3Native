"""Measure the BOLE of the road-tree GLBs. NO_SLOP rule 9 - the collider
numbers in app/road_trees.cpp must come from the asset, not a plausible guess.

A fixed low band fails on the oak, whose bark mesh carries low branches that
swamp the trunk (15.5 m across measured at 10% height). So: scan 2% slices up
the lower half and take the NARROWEST - that slice is the clean bole.
"""
import json, struct
import numpy as np

D = r"D:/GameDev/X3Native/.claude/worktrees/agent-a54a72d274eadd34a/assets/converted_glb/nature/"
CT = {5120: ('b', 1), 5121: ('B', 1), 5122: ('h', 2), 5123: ('H', 2),
      5125: ('I', 4), 5126: ('f', 4)}
NC = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}


def read(p):
    with open(p, 'rb') as f:
        _m, _v, total = struct.unpack('<4sII', f.read(12))
        js, bin_ = None, b''
        while f.tell() < total:
            ln, ty = struct.unpack('<I4s', f.read(8))
            data = f.read(ln)
            if ty == b'JSON':
                js = json.loads(data)
            elif ty == b'BIN\x00':
                bin_ = data
    return js, bin_


def acc(js, bin_, i):
    a = js['accessors'][i]
    bv = js['bufferViews'][a['bufferView']]
    fmt, sz = CT[a['componentType']]
    n = NC[a['type']]
    stride = bv.get('byteStride') or (sz * n)
    off = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
    out = np.empty((a['count'], n), np.float64)
    for k in range(a['count']):
        out[k] = struct.unpack_from('<' + fmt * n, bin_, off + k * stride)
    return out


for name in ('OakBigTree01.glb', 'PoplarTree001.glb'):
    js, bin_ = read(D + name)
    mats = [m.get('name', '') for m in js.get('materials', [])]
    best = None
    for m in js.get('meshes', []):
        for pr in m['primitives']:
            mn = mats[pr['material']].lower() if 'material' in pr else ''
            if 'leaf' in mn or 'leaves' in mn or 'billboard' in mn:
                continue
            P = acc(js, bin_, pr['attributes']['POSITION'])
            if best is None or len(P) > len(best[1]):
                best = (mn, P)
    mn, P = best
    y0, y1 = P[:, 1].min(), P[:, 1].max()
    H = y1 - y0
    prof = []
    for f in np.arange(0.02, 0.55, 0.02):
        b = P[(P[:, 1] > y0 + f * H) & (P[:, 1] < y0 + (f + 0.02) * H)]
        if len(b) < 8:
            continue
        prof.append((f, max(b[:, 0].max() - b[:, 0].min(),
                            b[:, 2].max() - b[:, 2].min())))
    across = min(w for _, w in prof)
    fmin = [f for f, w in prof if w == across][0]
    clear, hit = H, False
    for f, w in prof:
        if f < fmin:
            continue
        if hit and w > across * 2.5:
            clear = f * H
            break
        hit = True
    print(f"{name:22s} H={H:6.2f}  narrowest slice at {fmin*100:4.0f}% = "
          f"{across:5.3f} m across (halfW {across/2:5.3f})  clearBole={clear:5.2f} m")
    print("      profile:", " ".join(f"{f*100:.0f}%:{w:.2f}" for f, w in prof[:14]))
