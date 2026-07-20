#!/usr/bin/env python3
"""unity_scene_to_layout.py — replicate a Unity demo scene in X3Native.

Parses a Unity .unity scene (YAML) + the pack's _Layouts/guid_map.json and emits a
plain-text layout file: one line per placed prefab —
    <glb-filename> px py pz qx qy qz qw sx sy sz
world-space, glTF handedness (Unity's left-handed Z mirrored), ready for the host's
district loader to place per-prefab GLBs from D:/Assets/_glb at the designer's exact
transforms ("build it out just how the designers intended").

Usage:
  python unity_scene_to_layout.py <scene.unity> <guid_map.json> <glb_dir> <out.layout>

Notes:
- The scene's placements are PrefabInstances: m_SourcePrefab guid + m_Modification
  overrides (m_LocalPosition.x, ... m_LocalRotation.x/y/z/w, m_LocalScale.*).
- Parents matter: PrefabInstance.m_TransformParent points at a Transform which may
  itself be a child chain with non-identity TRS — compose the full chain. A parent
  may be a "stripped" Transform belonging to another PrefabInstance; then its world
  transform is that instance's composed transform.
- Unity -> glTF: mirror Z. pos'=(x,y,-z), quat'=(-qx,-qy,qz,qw), scale unchanged.
"""
import json, math, os, re, sys
from collections import defaultdict

def parse_scene(path):
    txt = open(path, encoding='utf-8', errors='replace').read()
    docs = re.split(r'^--- !u!(\d+) &(\d+)( stripped)?\s*$', txt, flags=re.M)
    # docs[0] = header; then groups of (cls, id, stripped, body)
    transforms, prefabs, stripped_owner = {}, {}, {}
    i = 1
    while i + 3 <= len(docs):
        cls, oid, stripped, body = docs[i], docs[i+1], docs[i+2], docs[i+3]
        i += 4
        cls, oid = int(cls), int(oid)
        if cls == 4:  # Transform
            if stripped:
                m = re.search(r'm_PrefabInstance:\s*\{fileID:\s*(\d+)\}', body)
                if m: stripped_owner[oid] = int(m.group(1))
                continue
            def vec(name, d):
                m = re.search(name + r':\s*\{x:\s*([-\d.e]+),\s*y:\s*([-\d.e]+),\s*z:\s*([-\d.e]+)(?:,\s*w:\s*([-\d.e]+))?\}', body)
                if not m: return d
                g = [float(x) for x in m.groups() if x is not None]
                return g
            father = re.search(r'm_Father:\s*\{fileID:\s*(\d+)\}', body)
            transforms[oid] = {
                'pos': vec('m_LocalPosition', [0,0,0]),
                'rot': vec('m_LocalRotation', [0,0,0,1]),
                'scl': vec('m_LocalScale', [1,1,1]),
                'father': int(father.group(1)) if father else 0,
            }
        elif cls == 1001:  # PrefabInstance
            src = re.search(r'm_SourcePrefab:\s*\{fileID:\s*\d+,\s*guid:\s*([0-9a-f]+)', body)
            par = re.search(r'm_TransformParent:\s*\{fileID:\s*(\d+)\}', body)
            mods = {}
            for m in re.finditer(r'propertyPath:\s*(m_Local\w+(?:\.\w+)?)\s*\n\s*value:\s*([-\d.e]+)', body):
                mods[m.group(1)] = float(m.group(2))
            prefabs[oid] = {
                'guid': src.group(1) if src else None,
                'parent': int(par.group(1)) if par else 0,
                'mods': mods,
            }
    return transforms, prefabs, stripped_owner

# ---- minimal TRS math (column-major 4x4 as nested lists) ----
def quat_mat(q):
    x,y,z,w = q
    return [
        [1-2*(y*y+z*z), 2*(x*y-z*w),   2*(x*z+y*w),   0],
        [2*(x*y+z*w),   1-2*(x*x+z*z), 2*(y*z-x*w),   0],
        [2*(x*z-y*w),   2*(y*z+x*w),   1-2*(x*x+y*y), 0],
        [0,0,0,1],
    ]
def trs(p, q, s):
    m = quat_mat(q)
    for r in range(3):
        m[r][0]*=s[0]; m[r][1]*=s[1]; m[r][2]*=s[2]
    m[0][3],m[1][3],m[2][3] = p
    return m
def matmul(a,b):
    return [[sum(a[r][k]*b[k][c] for k in range(4)) for c in range(4)] for r in range(4)]
def mat_decompose(m):
    p = [m[0][3], m[1][3], m[2][3]]
    sx = math.sqrt(m[0][0]**2 + m[1][0]**2 + m[2][0]**2)
    sy = math.sqrt(m[0][1]**2 + m[1][1]**2 + m[2][1]**2)
    sz = math.sqrt(m[0][2]**2 + m[1][2]**2 + m[2][2]**2)
    r = [[m[i][j]/(sx,sy,sz)[j] for j in range(3)] for i in range(3)]
    tr = r[0][0]+r[1][1]+r[2][2]
    if tr > 0:
        s_ = math.sqrt(tr+1.0)*2; qw=0.25*s_; qx=(r[2][1]-r[1][2])/s_; qy=(r[0][2]-r[2][0])/s_; qz=(r[1][0]-r[0][1])/s_
    elif r[0][0] > r[1][1] and r[0][0] > r[2][2]:
        s_ = math.sqrt(1.0+r[0][0]-r[1][1]-r[2][2])*2; qw=(r[2][1]-r[1][2])/s_; qx=0.25*s_; qy=(r[0][1]+r[1][0])/s_; qz=(r[0][2]+r[2][0])/s_
    elif r[1][1] > r[2][2]:
        s_ = math.sqrt(1.0+r[1][1]-r[0][0]-r[2][2])*2; qw=(r[0][2]-r[2][0])/s_; qx=(r[0][1]+r[1][0])/s_; qy=0.25*s_; qz=(r[1][2]+r[2][1])/s_
    else:
        s_ = math.sqrt(1.0+r[2][2]-r[0][0]-r[1][1])*2; qw=(r[1][0]-r[0][1])/s_; qx=(r[0][2]+r[2][0])/s_; qy=(r[1][2]+r[2][1])/s_; qz=0.25*s_
    return p, [qx,qy,qz,qw], [sx,sy,sz]

def main():
    scene, guidmap_p, glb_dir, out_p = sys.argv[1:5]
    guidmap = json.load(open(guidmap_p))
    transforms, prefabs, stripped_owner = parse_scene(scene)
    print(f"scene: {len(transforms)} transforms, {len(prefabs)} prefab instances, {len(stripped_owner)} stripped")

    # GLB name index (case-insensitive stem match).
    glbs = {}
    for f in os.listdir(glb_dir):
        if f.lower().endswith('.glb'):
            glbs[os.path.splitext(f)[0].lower()] = f

    memo = {}
    def world_of_transform(tid, depth=0):
        if tid == 0 or depth > 64: return None
        if tid in memo: return memo[tid]
        if tid in transforms:
            t = transforms[tid]
            local = trs(t['pos'], t['rot'], t['scl'])
            pw = world_of_transform(t['father'], depth+1)
            w = matmul(pw, local) if pw else local
        elif tid in stripped_owner:
            w = world_of_prefab(stripped_owner[tid], depth+1)
        else:
            w = None
        memo[tid] = w
        return w

    pmemo = {}
    def world_of_prefab(pid, depth=0):
        if pid in pmemo: return pmemo[pid]
        pr = prefabs.get(pid)
        if pr is None: return None
        md = pr['mods']
        p = [md.get('m_LocalPosition.x',0.0), md.get('m_LocalPosition.y',0.0), md.get('m_LocalPosition.z',0.0)]
        q = [md.get('m_LocalRotation.x',0.0), md.get('m_LocalRotation.y',0.0), md.get('m_LocalRotation.z',0.0), md.get('m_LocalRotation.w',1.0)]
        s = [md.get('m_LocalScale.x',1.0), md.get('m_LocalScale.y',1.0), md.get('m_LocalScale.z',1.0)]
        local = trs(p, q, s)
        pw = world_of_transform(pr['parent'], depth+1)
        w = matmul(pw, local) if pw else local
        pmemo[pid] = w
        return w

    placed, unmatched = [], defaultdict(int)
    for pid, pr in prefabs.items():
        path = guidmap.get(pr['guid'] or '', '')
        stem = os.path.splitext(os.path.basename(path))[0].lower() if path else ''
        if not stem: unmatched['<no-guid>'] += 1; continue
        glb = glbs.get(stem)
        if not glb:
            unmatched[stem] += 1; continue
        w = world_of_prefab(pid)
        if w is None: continue
        p, q, s = mat_decompose(w)
        # Unity (LH) -> glTF (RH): mirror Z.
        p = [p[0], p[1], -p[2]]
        q = [-q[0], -q[1], q[2], q[3]]
        placed.append((glb, p, q, s))

    with open(out_p, 'w') as f:
        f.write(f"# layout from {os.path.basename(scene)} — {len(placed)} placements\n")
        for glb, p, q, s in placed:
            f.write(f"{glb} {p[0]:.4f} {p[1]:.4f} {p[2]:.4f} "
                    f"{q[0]:.6f} {q[1]:.6f} {q[2]:.6f} {q[3]:.6f} "
                    f"{s[0]:.4f} {s[1]:.4f} {s[2]:.4f}\n")

    xs = [p[0] for _,p,_,_ in placed]; zs = [p[2] for _,p,_,_ in placed]; ys=[p[1] for _,p,_,_ in placed]
    print(f"placed {len(placed)} | footprint X [{min(xs):.0f},{max(xs):.0f}] Z [{min(zs):.0f},{max(zs):.0f}] Y [{min(ys):.0f},{max(ys):.0f}]")
    top = sorted(unmatched.items(), key=lambda kv:-kv[1])[:12]
    print(f"unmatched prefab stems ({sum(unmatched.values())} instances): {top}")

if __name__ == '__main__':
    main()
