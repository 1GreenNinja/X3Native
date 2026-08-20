# Meshy's rigging pass returns the mesh with its NORMAL MAP dropped (it emits
# baseColor + emissive only). On armored characters that is the entire surface
# detail, so we graft the original's normal texture back onto the rigged result.
# Pure glTF-JSON surgery: append the PNG bytes to the BIN chunk, add the
# bufferView/image/texture, and point material.normalTexture at it.
import json, struct, sys

def read(p):
    f=open(p,'rb'); struct.unpack('<III',f.read(12))
    jl,_=struct.unpack('<II',f.read(8)); j=json.loads(f.read(jl).decode('utf-8','replace'))
    bl,_=struct.unpack('<II',f.read(8)); b=f.read(bl)
    return j,b

def write(p,j,b):
    jn=json.dumps(j,separators=(',',':')).encode(); jn+=b' '*((4-len(jn)%4)%4)
    b+=b'\0'*((4-len(b)%4)%4)
    o=open(p,'wb')
    o.write(struct.pack('<III',0x46546C67,2,12+8+len(jn)+8+len(b)))
    o.write(struct.pack('<II',len(jn),0x4E4F534A)); o.write(jn)
    o.write(struct.pack('<II',len(b),0x004E4942)); o.write(b)

src_path, dst_path = sys.argv[1], sys.argv[2]
sj,sb = read(src_path); dj,db = read(dst_path)

# locate the source normal image bytes
mat = next(m for m in sj['materials'] if 'normalTexture' in m)
tex = sj['textures'][mat['normalTexture']['index']]
img = sj['images'][tex['source']]
bv  = sj['bufferViews'][img['bufferView']]
off, ln = bv.get('byteOffset',0), bv['byteLength']
png = sb[off:off+ln]
mime = img.get('mimeType','image/png')

# append into the destination BIN
pad = (4 - len(db) % 4) % 4
db += b'\0'*pad
new_off = len(db)
db += png
dj['bufferViews'].append({'buffer':0,'byteOffset':new_off,'byteLength':len(png)})
dj['buffers'][0]['byteLength'] = len(db) + ((4-len(db)%4)%4)
dj.setdefault('images',[]).append({'mimeType':mime,'bufferView':len(dj['bufferViews'])-1})
smp = tex.get('sampler')
if smp is not None:
    dj.setdefault('samplers',[]).append(sj['samplers'][smp])
    sampler_idx = len(dj['samplers'])-1
dj.setdefault('textures',[]).append(
    {'source':len(dj['images'])-1, **({'sampler':sampler_idx} if smp is not None else {})})
for m in dj['materials']:
    m['normalTexture'] = {'index': len(dj['textures'])-1}
write(dst_path, dj, db)
print(f"grafted {len(png)} byte normal map from {src_path.split('/')[-1]} -> {dst_path.split('/')[-1]}")
