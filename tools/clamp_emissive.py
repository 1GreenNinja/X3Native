#!/usr/bin/env python3
"""Clamp KHR_materials_emissive_strength in armory GLBs to a max, in place.
Kills the blown-white-blob slop from over-hot inject_emissive strengths.
Idempotent (re-runnable). Usage: clamp_emissive.py <glb_dir> <maxStrength>"""
import io, json, os, struct, sys
def read_glb(p):
    d=open(p,'rb').read(); assert d[:4]==b'glTF'; total=struct.unpack_from('<I',d,8)[0]
    off,ch=12,[]
    while off<total:
        cl,ct=struct.unpack_from('<II',d,off); ch.append([ct,bytearray(d[off+8:off+8+cl])]); off+=8+cl
    return ch
def write_glb(p,ch):
    body=b''
    for ct,cd in ch:
        pad=(4-len(cd)%4)%4; cd=bytes(cd)+(b' ' if ct==0x4E4F534A else b'\0')*pad
        body+=struct.pack('<II',len(cd),ct)+cd
    open(p,'wb').write(b'glTF'+struct.pack('<II',2,12+len(body))+body)
glb_dir, mx = sys.argv[1], float(sys.argv[2])
n=0
for root,_,files in os.walk(glb_dir):
    for f in files:
        if not f.lower().endswith('.glb'): continue
        p=os.path.join(root,f)
        try: ch=read_glb(p)
        except Exception: continue
        js=json.loads(bytes(ch[0][1]).decode('utf-8')); ch_changed=False
        for m in js.get('materials',[]):
            es=m.get('extensions',{}).get('KHR_materials_emissive_strength')
            if es and es.get('emissiveStrength',1.0)>mx:
                es['emissiveStrength']=mx; ch_changed=True
        if ch_changed:
            ch[0][1]=bytearray(json.dumps(js,separators=(',',':')).encode('utf-8'))
            write_glb(p,ch); n+=1
print(f"clamped {n} GLBs to <= {mx}")
