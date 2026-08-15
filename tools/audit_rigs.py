# Audit every character GLB: joints, clips, pose-bake, and classify.
# The Saurian's tell was joints==1 WITH animations -- clips driving a single node,
# i.e. a rigid lump sliding. That is the pattern worth finding.
import json, struct, os, sys, glob

def probe(path):
    with open(path,'rb') as f:
        m,v,t = struct.unpack('<III', f.read(12))
        if m != 0x46546C67: return None
        cl,ct = struct.unpack('<II', f.read(8))
        j = json.loads(f.read(cl).decode('utf-8','replace'))
    skins=j.get('skins',[]); anims=j.get('animations',[]); nodes=j.get('nodes',[])
    joints=sum(len(s.get('joints',[])) for s in skins)
    nn=[n.get('name','') or '' for n in nodes]
    posebaked=sum(1 for n in nn if n.startswith('pose_'))
    return dict(joints=joints, anims=len(anims), nodes=len(nodes),
                posebaked=posebaked,
                clips=[a.get('name','?') for a in anims])

rows=[]
for p in sorted(glob.glob('assets/rigged_glb/*.glb')):
    if p.endswith('.bak'): continue
    r=probe(p)
    if r is None: continue
    r['file']=os.path.basename(p); rows.append(r)

def cls(r):
    if r['posebaked']>0: return 'POSE-BAKED (by design)'
    if r['joints']==1 and r['anims']>0: return '*** LUMP: clips on 1 bone ***'
    if r['joints']==1 and r['anims']==0: return 'single-bone static'
    if r['joints']==0 and r['anims']>0:  return 'node-anim, no skin'
    if r['joints']==0: return 'static mesh'
    if r['anims']==0:  return 'rigged, NO clips'
    return 'rigged+animated'

for r in rows: r['cls']=cls(r)
order={'*** LUMP: clips on 1 bone ***':0,'rigged, NO clips':1,'single-bone static':2,
       'node-anim, no skin':3,'static mesh':4,'POSE-BAKED (by design)':5,'rigged+animated':6}
rows.sort(key=lambda r:(order[r['cls']], -r['joints'], r['file']))
print(f"{'FILE':42s} {'JNT':>4s} {'CLIPS':>5s}  CLASS")
print('-'*98)
for r in rows:
    print(f"{r['file']:42s} {r['joints']:4d} {r['anims']:5d}  {r['cls']}")
print(f"\ntotal {len(rows)} GLBs")
