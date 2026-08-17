"""Cluster a GLB's triangles into connected components and print each part's AABB.
Lane-5 scratch tool: tells me where the canopy / pumps / kiosk / wall actually are
in the recentred Fuel_Station_Model, in engine metres, so placement is MEASURED
(X3_WORLD_RULES rule 0) and not eyeballed off a preview render."""
import sys, json, struct
from collections import defaultdict

path = sys.argv[1]
data = open(path, 'rb').read()
total = struct.unpack('<I', data[8:12])[0]
off, js, bin_ = 12, None, b''
while off < total:
    clen, ctype = struct.unpack('<II', data[off:off+8])
    c = data[off+8:off+8+clen]
    if ctype == 0x4E4F534A: js = json.loads(c.decode('utf-8'))
    elif ctype == 0x004E4942: bin_ = c
    off += 8 + clen + ((4 - clen % 4) % 4)

def acc(i):
    a = js['accessors'][i]
    bv = js['bufferViews'][a['bufferView']]
    base = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
    ctype = a['componentType']; t = a['type']
    n = {'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4}[t]
    fmt = {5126:'f',5125:'I',5123:'H',5121:'B'}[ctype]
    sz = {'f':4,'I':4,'H':2,'B':1}[fmt]
    stride = bv.get('byteStride', n*sz)
    out = []
    for k in range(a['count']):
        out.append(struct.unpack_from('<'+fmt*n, bin_, base + k*stride))
    return out

for mi, mesh in enumerate(js['meshes']):
    for pi, p in enumerate(mesh['primitives']):
        matname = js['materials'][p['material']]['name'] if 'material' in p else '?'
        P = acc(p['attributes']['POSITION'])
        I = [x[0] for x in acc(p['indices'])]
        # union-find over vertices sharing a triangle, keyed by quantised position
        # so split-vertex duplicates (uv seams) still weld.
        key = {}
        rep = {}
        def find(a):
            while rep[a] != a: rep[a] = rep[rep[a]]; a = rep[a]
            return a
        def uni(a, b):
            ra, rb = find(a), find(b)
            if ra != rb: rep[ra] = rb
        for vi, v in enumerate(P):
            k = (round(v[0]*100), round(v[1]*100), round(v[2]*100))
            if k not in key: key[k] = vi; rep[vi] = vi
            rep[vi] = key[k]
        for t in range(0, len(I), 3):
            a, b, c = key[tuple(round(P[I[t+j]][q]*100) for q in range(3))] if False else (
                key[(round(P[I[t]][0]*100), round(P[I[t]][1]*100), round(P[I[t]][2]*100))],
                key[(round(P[I[t+1]][0]*100), round(P[I[t+1]][1]*100), round(P[I[t+1]][2]*100))],
                key[(round(P[I[t+2]][0]*100), round(P[I[t+2]][1]*100), round(P[I[t+2]][2]*100))])
            uni(a, b); uni(b, c)
        groups = defaultdict(lambda: [ [1e30]*3, [-1e30]*3, 0 ])
        for t in range(0, len(I), 3):
            v0 = P[I[t]]
            k = (round(v0[0]*100), round(v0[1]*100), round(v0[2]*100))
            g = groups[find(key[k])]
            for j in range(3):
                v = P[I[t+j]]
                for q in range(3):
                    g[0][q] = min(g[0][q], v[q]); g[1][q] = max(g[1][q], v[q])
            g[2] += 1
        print('--- prim %d  material=%s  tris=%d  parts=%d' % (pi, matname, len(I)//3, len(groups)))
        rows = sorted(groups.values(), key=lambda g: -g[2])
        for lo, hi, nt in rows:
            print('   tris %5d  X %8.2f..%8.2f  Y %6.2f..%6.2f  Z %8.2f..%8.2f   (%.1f x %.1f x %.1f)'
                  % (nt, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2],
                     hi[0]-lo[0], hi[1]-lo[1], hi[2]-lo[2]))
