"""
glb_splice.py - graft named animations from a DONOR glb onto a BASE glb without
touching the base's existing clips/meshes/buffers. Requires donor and base to
share node + skin joint ordering (verified by caller). Clean-room: glTF 2.0 spec.

    python glb_splice.py <base.glb> <donor.glb> <out.glb> Attack Hit Death
"""
import sys, json, struct

JSON_T=0x4E4F534A; BIN_T=0x004E4942
COMP_SZ={5120:1,5121:1,5122:2,5123:2,5125:4,5126:4}
TYPE_N={'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4,'MAT2':4,'MAT3':9,'MAT4':16}

def read_glb(path):
    d=open(path,'rb').read()
    assert d[:4]==b'glTF'
    off=12; js=None; binv=b''
    while off<len(d):
        clen,ctype=struct.unpack_from('<II',d,off); off+=8
        chunk=d[off:off+clen]; off+=clen
        if ctype==JSON_T: js=json.loads(chunk.decode('utf-8'))
        elif ctype==BIN_T: binv=chunk
    return js,bytearray(binv)

def write_glb(path,js,binb):
    jb=json.dumps(js,separators=(',',':')).encode('utf-8')
    jb+=b' '*((4-len(jb)%4)%4)
    bb=bytes(binb); bb+=b'\x00'*((4-len(bb)%4)%4)
    total=12+8+len(jb)+8+len(bb)
    with open(path,'wb') as f:
        f.write(b'glTF'); f.write(struct.pack('<II',2,total))
        f.write(struct.pack('<II',len(jb),JSON_T)); f.write(jb)
        f.write(struct.pack('<II',len(bb),BIN_T)); f.write(bb)

def acc_span(acc):
    return acc['count']*COMP_SZ[acc['componentType']]*TYPE_N[acc['type']]

def main():
    base_p,donor_p,out_p=sys.argv[1],sys.argv[2],sys.argv[3]
    names=set(sys.argv[4:])
    bjs,bbin=read_glb(base_p)
    djs,dbin=read_glb(donor_p)
    # sanity: node order + skin joints must match
    bn=[n.get('name') for n in bjs['nodes']]; dn=[n.get('name') for n in djs['nodes']]
    if bn!=dn: raise SystemExit(f"NODE ORDER MISMATCH base={len(bn)} donor={len(dn)}")
    bjs.setdefault('animations',[])
    bjs.setdefault('accessors',[]); bjs.setdefault('bufferViews',[])
    added=[]
    for anim in djs.get('animations',[]):
        if anim.get('name') not in names: continue
        new_samplers=[]
        for s in anim['samplers']:
            new_s={'interpolation':s.get('interpolation','LINEAR')}
            for key in ('input','output'):
                acc=djs['accessors'][s[key]]
                bv=djs['bufferViews'][acc['bufferView']]
                start=bv.get('byteOffset',0)+acc.get('byteOffset',0)
                span=acc_span(acc)
                data=dbin[start:start+span]
                # align append to 4 bytes
                pad=(4-len(bbin)%4)%4; bbin.extend(b'\x00'*pad)
                off=len(bbin); bbin.extend(data)
                new_bv={'buffer':0,'byteOffset':off,'byteLength':span}
                bjs['bufferViews'].append(new_bv); nbv=len(bjs['bufferViews'])-1
                nacc={'bufferView':nbv,'byteOffset':0,
                      'componentType':acc['componentType'],'count':acc['count'],
                      'type':acc['type']}
                if 'max' in acc: nacc['max']=acc['max']
                if 'min' in acc: nacc['min']=acc['min']
                bjs['accessors'].append(nacc)
                new_s[key]=len(bjs['accessors'])-1
            new_samplers.append(new_s)
        new_channels=[]
        for ch in anim['channels']:
            new_channels.append({'sampler':ch['sampler'],
                                 'target':{'node':ch['target']['node'],
                                           'path':ch['target']['path']}})
        bjs['animations'].append({'name':anim['name'],
                                  'samplers':new_samplers,'channels':new_channels})
        added.append(anim['name'])
    # fix buffer length
    bjs['buffers'][0]['byteLength']=len(bbin)
    write_glb(out_p,bjs,bbin)
    print("spliced:",added,"total anims now:",[a.get('name') for a in bjs['animations']])

main()
