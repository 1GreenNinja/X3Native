#!/usr/bin/env python3
"""unity_scene_to_layout.py — replicate a Unity demo scene in X3Native.

Parses a Unity .unity scene (YAML) + the pack's _Layouts/guid_map.json and emits a
plain-text layout file: one line per placed prefab —
    <glb-filename> px py pz qx qy qz qw sx sy sz
world-space, glTF handedness (Unity's left-handed Z mirrored), ready for the host's
district loader to place per-prefab GLBs from D:/Assets/_glb at the designer's exact
transforms ("build it out just how the designers intended").

Usage:
  python unity_scene_to_layout.py <scene.unity> <guid_map.json> <glb_dir> <out.layout> [--flatten <cell>]

  --flatten <cell>: estimate the invisible Unity-Terrain ground surface from the
    pieces' own minimum Y per <cell>-meter XZ cell (nearest-neighbor fill + 3x3
    box blur x2), then subtract it from every piece's Y (clamped >= -0.5). Fixes
    districts that were authored to sit on a terrain we don't import, which
    otherwise float/hang in the air.

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
    transforms, prefabs, stripped_owner, meshfilters = {}, {}, {}, []
    i = 1
    while i + 3 <= len(docs):
        cls, oid, stripped, body = docs[i], docs[i+1], docs[i+2], docs[i+3]
        i += 4
        cls, oid = int(cls), int(oid)
        if cls == 4:  # Transform
            if stripped:
                m = re.search(r'm_PrefabInstance:\s*\{fileID:\s*(\d+)\}', body)
                c = re.search(r'm_CorrespondingSourceObject:\s*\{fileID:\s*(\d+),\s*guid:\s*([0-9a-f]+)', body)
                if m:
                    stripped_owner[oid] = {'owner': int(m.group(1)),
                                           'src': (int(c.group(1)), c.group(2)) if c else None}
                continue
            def vec(name, d):
                m = re.search(name + r':\s*\{x:\s*([-\d.e]+),\s*y:\s*([-\d.e]+),\s*z:\s*([-\d.e]+)(?:,\s*w:\s*([-\d.e]+))?\}', body)
                if not m: return d
                g = [float(x) for x in m.groups() if x is not None]
                return g
            father = re.search(r'm_Father:\s*\{fileID:\s*(\d+)\}', body)
            go = re.search(r'm_GameObject:\s*\{fileID:\s*(\d+)\}', body)
            transforms[oid] = {
                'pos': vec('m_LocalPosition', [0,0,0]),
                'rot': vec('m_LocalRotation', [0,0,0,1]),
                'scl': vec('m_LocalScale', [1,1,1]),
                'father': int(father.group(1)) if father else 0,
                'go': int(go.group(1)) if go else 0,
            }
        elif cls == 33:  # MeshFilter (PLAIN scene object — the architecture class!)
            go = re.search(r'm_GameObject:\s*\{fileID:\s*(\d+)\}', body)
            mm = re.search(r'm_Mesh:\s*\{fileID:\s*\d+,\s*guid:\s*([0-9a-f]{32})', body)
            if go and mm and not mm.group(1).startswith('0000000000000000'):
                meshfilters.append({'go': int(go.group(1)), 'mesh_guid': mm.group(1)})
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
    return transforms, prefabs, stripped_owner, meshfilters

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
    # MIRRORED PIECES (Unity designers flip modular walls with negative scale):
    # det<0 means a mirror lives in this matrix. sqrt() would silently drop it,
    # flipping geometry + pointing normals into the wall (the "black buildings").
    # Carry it on X: negate sx + the rotation's first column stays orthonormal.
    det = (m[0][0]*(m[1][1]*m[2][2]-m[1][2]*m[2][1])
         - m[0][1]*(m[1][0]*m[2][2]-m[1][2]*m[2][0])
         + m[0][2]*(m[1][0]*m[2][1]-m[1][1]*m[2][0]))
    if det < 0: sx = -sx
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

def prefab_transform_tree(prefab_path, cache={}):
    """Everything inside a .prefab: transforms {fileID: {pos,rot,scl,father,go}},
    go->transform index, and [(go, mesh_guid)] for every MeshFilter. Cached."""
    if prefab_path in cache: return cache[prefab_path]
    tree, go2t, meshes = {}, {}, []
    try:
        txt = open(prefab_path, encoding='utf-8', errors='replace').read()
        docs = re.split(r'^--- !u!(\d+) &(\d+)( stripped)?\s*$', txt, flags=re.M)
        i = 1
        while i + 3 <= len(docs):
            cls, oid, body = int(docs[i]), int(docs[i+1]), docs[i+3]; i += 4
            if cls == 33:
                go = re.search(r'm_GameObject:\s*\{fileID:\s*(\d+)\}', body)
                mm = re.search(r'm_Mesh:\s*\{fileID:\s*\d+,\s*guid:\s*([0-9a-f]{32})', body)
                if go and mm and not mm.group(1).startswith('0000000000000000'):
                    meshes.append((int(go.group(1)), mm.group(1)))
                continue
            if cls != 4: continue
            def vec(name, dflt):
                m = re.search(name + r':\s*\{x:\s*([-\d.e]+),\s*y:\s*([-\d.e]+),\s*z:\s*([-\d.e]+)(?:,\s*w:\s*([-\d.e]+))?\}', body)
                if not m: return dflt
                return [float(x) for x in m.groups() if x is not None]
            fa = re.search(r'm_Father:\s*\{fileID:\s*(\d+)\}', body)
            go = re.search(r'm_GameObject:\s*\{fileID:\s*(\d+)\}', body)
            tree[oid] = {'pos': vec('m_LocalPosition', [0,0,0]),
                         'rot': vec('m_LocalRotation', [0,0,0,1]),
                         'scl': vec('m_LocalScale', [1,1,1]),
                         'father': int(fa.group(1)) if fa else 0}
            if go: go2t[int(go.group(1))] = oid
    except OSError:
        pass
    cache[prefab_path] = (tree, go2t, meshes)
    return cache[prefab_path]

def prefab_internal_matrix(prefab_path, node_id):
    """Compose node..root chain INSIDE a prefab, EXCLUDING the root's own TRS (the
    scene's PrefabInstance supplies the root TRS via defaults+mods). Identity if
    the node is the root itself or the prefab is unreadable."""
    tree, _go2t, _meshes = prefab_transform_tree(prefab_path)
    m, tid, depth = None, node_id, 0
    while tid in tree and depth < 64:
        t = tree[tid]
        if t['father'] == 0: break          # reached root: stop, exclude its TRS
        local = trs(t['pos'], t['rot'], t['scl'])
        m = local if m is None else matmul(local, m)
        tid = t['father']; depth += 1
    return m

def prefab_all_meshes(prefab_path):
    """[(mesh_guid, inner_matrix_or_None)] for EVERY MeshFilter inside a prefab —
    the multi-mesh expansion that recovers assembled buildings (a Leartes 'building'
    prefab is dozens of child meshes; root-only resolution kept 1 of them)."""
    tree, go2t, meshes = prefab_transform_tree(prefab_path)
    out = []
    for go, guid in meshes:
        tid = go2t.get(go)
        out.append((guid, prefab_internal_matrix(prefab_path, tid) if tid else None))
    return out

def prefab_root_defaults(prefab_path, cache={}):
    """Authored TRS of a .prefab's ROOT transform (m_Father == 0). Unity scenes only
    write CHANGED axes into m_Modifications — unwritten axes inherit these defaults
    (discarding them scattered pieces into the sky; see docs/COORDINATES.md)."""
    if prefab_path in cache: return cache[prefab_path]
    d = {'p': [0,0,0], 'q': [0,0,0,1], 's': [1,1,1]}
    try:
        txt = open(prefab_path, encoding='utf-8', errors='replace').read()
        docs = re.split(r'^--- !u!(\d+) &(\d+)( stripped)?\s*$', txt, flags=re.M)
        i = 1
        while i + 3 <= len(docs):
            cls, body = int(docs[i]), docs[i+3]; i += 4
            if cls != 4: continue
            fa = re.search(r'm_Father:\s*\{fileID:\s*(\d+)\}', body)
            if not fa or int(fa.group(1)) != 0: continue
            def vec(name, dflt):
                m = re.search(name + r':\s*\{x:\s*([-\d.e]+),\s*y:\s*([-\d.e]+),\s*z:\s*([-\d.e]+)(?:,\s*w:\s*([-\d.e]+))?\}', body)
                if not m: return dflt
                return [float(x) for x in m.groups() if x is not None]
            d = {'p': vec('m_LocalPosition', [0,0,0]),
                 'q': vec('m_LocalRotation', [0,0,0,1]),
                 's': vec('m_LocalScale', [1,1,1])}
            break
    except OSError:
        pass
    cache[prefab_path] = d
    return d

def build_ground_grid(placed, cell):
    """Estimate the invisible ground surface under `placed` pieces: a 2D XZ grid
    (cell size `cell`, meters) of MIN piece-Y per cell, holes filled by nearest
    neighbor, then smoothed with a 3x3 box blur (2 passes; missing/out-of-bounds
    neighbors count as the cell's own value). Returns (grid, ix_min, iz_min, nx, nz)
    for use with grid_bilinear(); grid is a nx x nz list-of-lists of floats."""
    cell_min = {}
    for _glb, p, _q, _s in placed:
        x, y, z = p
        k = (math.floor(x / cell), math.floor(z / cell))
        if k not in cell_min or y < cell_min[k]:
            cell_min[k] = y
    ixs = [k[0] for k in cell_min]; izs = [k[1] for k in cell_min]
    ix_min, iz_min = min(ixs), min(izs)
    nx, nz = max(ixs) - ix_min + 1, max(izs) - iz_min + 1
    grid = [[None] * nz for _ in range(nx)]
    for (ix, iz), y in cell_min.items():
        grid[ix - ix_min][iz - iz_min] = y

    # Fill empty cells by nearest neighbor (multi-source BFS over the grid graph).
    filled = [row[:] for row in grid]
    dist = [[-1] * nz for _ in range(nx)]
    dq = []
    for i in range(nx):
        for j in range(nz):
            if grid[i][j] is not None:
                dist[i][j] = 0
                dq.append((i, j))
    head = 0
    while head < len(dq):
        i, j = dq[head]; head += 1
        for di, dj in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            ni, nj = i + di, j + dj
            if 0 <= ni < nx and 0 <= nj < nz and dist[ni][nj] == -1:
                dist[ni][nj] = dist[i][j] + 1
                filled[ni][nj] = filled[i][j]
                dq.append((ni, nj))

    # 3x3 box blur, 2 iterations; out-of-grid neighbors treated as the cell's own value.
    g = filled
    for _ in range(2):
        newg = [[0.0] * nz for _ in range(nx)]
        for i in range(nx):
            for j in range(nz):
                center = g[i][j]
                total = 0.0
                for di in (-1, 0, 1):
                    for dj in (-1, 0, 1):
                        ni, nj = i + di, j + dj
                        total += g[ni][nj] if (0 <= ni < nx and 0 <= nj < nz) else center
                newg[i][j] = total / 9.0
        g = newg
    return g, ix_min, iz_min, nx, nz

def grid_bilinear(grid, ix_min, iz_min, nx, nz, cell, x, z):
    """Bilinear sample of a ground grid from build_ground_grid() at world (x,z).
    Cell (i,j) holds the ground estimate at its center, i.e. world
    ((ix_min+i+0.5)*cell, (iz_min+j+0.5)*cell); edges clamp (no extrapolation)."""
    fx = x / cell - ix_min - 0.5
    fz = z / cell - iz_min - 0.5
    fx = max(0.0, min(nx - 1, fx))
    fz = max(0.0, min(nz - 1, fz))
    i0 = int(math.floor(fx)); j0 = int(math.floor(fz))
    i1 = min(i0 + 1, nx - 1); j1 = min(j0 + 1, nz - 1)
    tx, tz = fx - i0, fz - j0
    v00, v10 = grid[i0][j0], grid[i1][j0]
    v01, v11 = grid[i0][j1], grid[i1][j1]
    v0 = v00 * (1 - tx) + v10 * tx
    v1 = v01 * (1 - tx) + v11 * tx
    return v0 * (1 - tz) + v1 * tz

def flatten_placed(placed, cell):
    """Subtract the estimated invisible-terrain ground surface from every piece's
    Y (see build_ground_grid docstring for why). Clamps result to >= -0.5. Prints
    move count + before/after Y range; returns the new `placed` list."""
    ys_before = [p[1] for _, p, _, _ in placed]
    grid, ix_min, iz_min, nx, nz = build_ground_grid(placed, cell)
    out, moved = [], 0
    for glb, p, q, s in placed:
        x, y, z = p
        gy = grid_bilinear(grid, ix_min, iz_min, nx, nz, cell, x, z)
        new_y = max(y - gy, -0.5)
        if abs(new_y - y) > 1e-6:
            moved += 1
        out.append((glb, [x, new_y, z], q, s))
    ys_after = [p[1] for _, p, _, _ in out]
    print(f"flatten --flatten {cell}: {moved}/{len(out)} pieces moved | "
          f"Y before [{min(ys_before):.2f},{max(ys_before):.2f}] "
          f"-> after [{min(ys_after):.2f},{max(ys_after):.2f}]")
    return out

def main():
    argv = sys.argv[1:]
    flatten_cell = None
    if '--flatten' in argv:
        idx = argv.index('--flatten')
        flatten_cell = float(argv[idx + 1])
        del argv[idx:idx + 2]
    scene, guidmap_p, glb_dir, out_p = argv[:4]
    guidmap = json.load(open(guidmap_p))
    layouts_root = os.path.dirname(os.path.abspath(guidmap_p))
    transforms, prefabs, stripped_owner, meshfilters = parse_scene(scene)
    print(f"scene: {len(transforms)} transforms, {len(prefabs)} prefab instances, "
          f"{len(stripped_owner)} stripped, {len(meshfilters)} plain mesh objects")
    go2t = {t['go']: tid for tid, t in transforms.items() if t.get('go')}

    # GLB name index (case-insensitive stem match), RECURSIVE — armory packs like
    # HIVEMIND mirror the pack's folder tree. Values are glb_dir-relative paths
    # (forward slashes) so the host's addGlbInstance(relPath) resolves them.
    glbs = {}
    for root, _dirs, files in os.walk(glb_dir):
        for f in files:
            if f.lower().endswith('.glb'):
                rel = os.path.relpath(os.path.join(root, f), glb_dir).replace('\\', '/')
                glbs.setdefault(os.path.splitext(f)[0].lower(), rel)

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
            so = stripped_owner[tid]
            w = world_of_prefab(so['owner'], depth+1)
            # A stripped transform may be a node DEEP INSIDE the source prefab (a
            # building's anchor); compose its prefab-internal chain (root-relative)
            # under the owner's world — treating it as the root scattered every
            # nested-parented piece into the sky.
            if w is not None and so['src'] is not None:
                fid, guid = so['src']
                path = guidmap.get(guid, '')
                if path.endswith('.prefab'):
                    inner = prefab_internal_matrix(os.path.join(layouts_root, path), fid)
                    if inner is not None: w = matmul(w, inner)
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
        # PER-AXIS merge over the source prefab root's AUTHORED defaults (a mod
        # only exists for axes the designer changed in the scene).
        path = guidmap.get(pr['guid'] or '', '')
        dfl = prefab_root_defaults(os.path.join(layouts_root, path)) if path.endswith('.prefab') \
              else {'p': [0,0,0], 'q': [0,0,0,1], 's': [1,1,1]}
        p = [md.get('m_LocalPosition.x',dfl['p'][0]), md.get('m_LocalPosition.y',dfl['p'][1]), md.get('m_LocalPosition.z',dfl['p'][2])]
        q = [md.get('m_LocalRotation.x',dfl['q'][0]), md.get('m_LocalRotation.y',dfl['q'][1]), md.get('m_LocalRotation.z',dfl['q'][2]), md.get('m_LocalRotation.w',dfl['q'][3])]
        s = [md.get('m_LocalScale.x',dfl['s'][0]), md.get('m_LocalScale.y',dfl['s'][1]), md.get('m_LocalScale.z',dfl['s'][2])]
        local = trs(p, q, s)
        pw = world_of_transform(pr['parent'], depth+1)
        w = matmul(pw, local) if pw else local
        pmemo[pid] = w
        return w

    # Fallback resolver: a .prefab whose STEM has no GLB may still carry a MeshFilter
    # whose mesh GUID resolves to the FBX we converted (Recife: 'Cube_1.prefab' ->
    # SM_Build03.fbx -> SM_Build03.glb). Root-level mesh only (v1).
    def prefab_mesh_stem(prefab_path, cache={}):
        if prefab_path in cache: return cache[prefab_path]
        stem = None
        try:
            txt = open(prefab_path, encoding='utf-8', errors='replace').read()
            m = re.search(r'm_Mesh:\s*\{fileID:\s*\d+,\s*guid:\s*([0-9a-f]+)', txt)
            if m:
                mp = guidmap.get(m.group(1), '')
                if mp: stem = os.path.splitext(os.path.basename(mp))[0].lower()
        except OSError:
            pass
        cache[prefab_path] = stem
        return stem

    def emit(glb, w):
        p, q, s = mat_decompose(w)
        # Unity (LH) -> glTF (RH): mirror Z.
        placed.append((glb, [p[0], p[1], -p[2]], [-q[0], -q[1], q[2], q[3]], s))

    placed, unmatched = [], defaultdict(int)
    for pid, pr in prefabs.items():
        path = guidmap.get(pr['guid'] or '', '')
        stem = os.path.splitext(os.path.basename(path))[0].lower() if path else ''
        if not stem: unmatched['<no-guid>'] += 1; continue
        if path.endswith('.prefab'):
            # MULTI-MESH EXPANSION: place EVERY MeshFilter inside the prefab at
            # instance-world x its prefab-internal chain (assembled buildings!).
            inner = prefab_all_meshes(os.path.join(layouts_root, path))
            if inner:
                w0 = world_of_prefab(pid)
                if w0 is None: continue
                # ALL-OR-NOTHING: only use the expansion when EVERY (non-LOD) inner
                # mesh resolves to a GLB — a partial expansion drops the missing
                # pieces (Urban lost its signs/gates); the prefab-stem GLB already
                # contains the whole assembly, so fall back to it instead.
                todo = []
                complete = True
                for guid, im in inner:
                    mp = guidmap.get(guid, '')
                    ms = os.path.splitext(os.path.basename(mp))[0].lower() if mp else ''
                    if re.search(r'_lod[1-9]$', ms): continue   # LOD0 only — prefabs stack LOD1-3 copies
                    g = glbs.get(ms)
                    if not g: complete = False; break
                    todo.append((g, im))
                if complete and todo:
                    for g, im in todo:
                        emit(g, matmul(w0, im) if im else w0)
                    continue
            # fall through: unresolvable/partial expansion -> whole-assembly stem match
        glb = glbs.get(stem)
        if not glb:
            unmatched[stem] += 1; continue
        w = world_of_prefab(pid)
        if w is None: continue
        emit(glb, w)

    # PLAIN SCENE OBJECTS (class-33 MeshFilters): the architecture class. The
    # _Layouts repack dropped these; the ORIGINAL package scene carries them.
    for mf in meshfilters:
        tid = go2t.get(mf['go'])
        if tid is None: unmatched['<mesh-no-transform>'] += 1; continue
        mp = guidmap.get(mf['mesh_guid'], '')
        stem = os.path.splitext(os.path.basename(mp))[0].lower() if mp else ''
        glb = glbs.get(stem)
        if not glb: unmatched[stem or '<mesh-no-guid>'] += 1; continue
        w = world_of_transform(tid)
        if w is None: continue
        emit(glb, w)

    # Skip pack sky-domes/backdrops (they belong to the demo scene's own sky,
    # not the district — a giant textured egg floating over the pad otherwise).
    before = len(placed)
    placed = [e for e in placed if not re.match(r'(?i)(sm_)?sky', e[0])]
    if len(placed) != before: print(f"skipped {before-len(placed)} sky/backdrop pieces")

    # Dedupe exact duplicates (same glb at same rounded position — LOD stacks etc.).
    seen, uniq = set(), []
    for glb, p, q, s in placed:
        k = (glb, round(p[0],2), round(p[1],2), round(p[2],2))
        if k in seen: continue
        seen.add(k); uniq.append((glb, p, q, s))
    if len(uniq) != len(placed):
        print(f"deduped {len(placed)-len(uniq)} co-located duplicates")
    placed = uniq

    if flatten_cell is not None and placed:
        placed = flatten_placed(placed, flatten_cell)

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
