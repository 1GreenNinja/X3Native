"""Top-down PLAN raster of a GLB (Lane-5 scratch): projects every triangle to XZ
and writes a PNG where brightness = max height. Rule-0 verification for placing a
building: tells me exactly where the walls, the openings, the canopy and the pump
islands sit, in engine metres, without trusting a 3/4 preview render."""
import sys, json, struct
from PIL import Image

path, out = sys.argv[1], sys.argv[2]
PPM = 8  # pixels per metre

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
    a = js['accessors'][i]; bv = js['bufferViews'][a['bufferView']]
    base = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
    n = {'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4}[a['type']]
    fmt = {5126:'f',5125:'I',5123:'H',5121:'B'}[a['componentType']]
    sz = {'f':4,'I':4,'H':2,'B':1}[fmt]
    stride = bv.get('byteStride', n*sz)
    return [struct.unpack_from('<'+fmt*n, bin_, base + k*stride) for k in range(a['count'])]

tris = []
lo = [1e30]*3; hi = [-1e30]*3
for mesh in js['meshes']:
    for p in mesh['primitives']:
        P = acc(p['attributes']['POSITION']); I = [x[0] for x in acc(p['indices'])]
        for k in range(0, len(I), 3):
            t = [P[I[k]], P[I[k+1]], P[I[k+2]]]
            tris.append(t)
            for v in t:
                for q in range(3):
                    lo[q] = min(lo[q], v[q]); hi[q] = max(hi[q], v[q])

W = int((hi[0]-lo[0])*PPM)+1; H = int((hi[2]-lo[2])*PPM)+1
buf = [[0.0]*W for _ in range(H)]
def px(v): return int((v[0]-lo[0])*PPM), int((v[2]-lo[2])*PPM)
for t in tris:
    pts = [px(v) for v in t]
    ys = [p[1] for p in pts]; xs = [p[0] for p in pts]
    x0, x1 = max(0, min(xs)), min(W-1, max(xs))
    y0, y1 = max(0, min(ys)), min(H-1, max(ys))
    ymax = max(v[1] for v in t)
    # cheap: fill the triangle's bbox only when it is thin, else barycentric fill
    (ax, ay), (bx, by), (cx, cy) = pts
    den = (by-cy)*(ax-cx) + (cx-bx)*(ay-cy)
    for yy in range(y0, y1+1):
        for xx in range(x0, x1+1):
            if den == 0:
                inside = True
            else:
                l1 = ((by-cy)*(xx-cx) + (cx-bx)*(yy-cy)) / den
                l2 = ((cy-ay)*(xx-cx) + (ax-cx)*(yy-cy)) / den
                inside = l1 >= -0.02 and l2 >= -0.02 and (l1+l2) <= 1.02
            if inside and ymax > buf[yy][xx]:
                buf[yy][xx] = ymax

im = Image.new('RGB', (W, H), (0, 0, 0))
pxs = im.load()
span = max(0.001, hi[1]-lo[1])
for y in range(H):
    for x in range(W):
        v = buf[y][x]
        if v <= 0.001: continue
        f = (v-lo[1])/span
        pxs[x, y] = (int(40+215*f), int(90+120*f), int(200-150*f))
# metre grid every 10 m + the origin cross
for gx in range(int(lo[0]//10)*10, int(hi[0])+1, 10):
    X = int((gx-lo[0])*PPM)
    if 0 <= X < W:
        for y in range(H): pxs[X, y] = (60, 60, 60) if buf[y][X] <= 0.001 else pxs[X, y]
for gz in range(int(lo[2]//10)*10, int(hi[2])+1, 10):
    Y = int((gz-lo[2])*PPM)
    if 0 <= Y < H:
        for x in range(W): pxs[x, Y] = (60, 60, 60) if buf[Y][x] <= 0.001 else pxs[x, Y]
ox, oy = int((0-lo[0])*PPM), int((0-lo[2])*PPM)
for d in range(-16, 17):
    if 0 <= ox+d < W and 0 <= oy < H: pxs[ox+d, oy] = (255, 0, 0)
    if 0 <= ox < W and 0 <= oy+d < H: pxs[ox, oy+d] = (255, 0, 0)
im.save(out)
print('plan %dx%d px  X %.2f..%.2f  Z %.2f..%.2f  Y %.2f..%.2f  (+X right, +Z down, grid 10 m)'
      % (W, H, lo[0], hi[0], lo[2], hi[2], lo[1], hi[1]))
