import sys, json, struct, os

def info(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'glTF':
        print('NOT GLB', path); return
    n = struct.unpack('<I', data[12:16])[0]
    j = json.loads(data[20:20+n].decode('utf-8'))
    print('=' * 90)
    print(path, os.path.getsize(path), 'bytes')
    print('  extensionsUsed    :', j.get('extensionsUsed'))
    print('  extensionsRequired:', j.get('extensionsRequired'))
    print('  meshes:', len(j.get('meshes', [])), ' materials:', len(j.get('materials', [])),
          ' images:', len(j.get('images', [])), ' textures:', len(j.get('textures', [])),
          ' nodes:', len(j.get('nodes', [])))
    for m in j.get('materials', []):
        pbr = m.get('pbrMetallicRoughness', {})
        print('   MAT', m.get('name'), 'baseTex=', 'baseColorTexture' in pbr,
              'mr=', 'metallicRoughnessTexture' in pbr,
              'metallic=', pbr.get('metallicFactor'), 'rough=', pbr.get('roughnessFactor'),
              'base=', pbr.get('baseColorFactor'),
              'normTex=', 'normalTexture' in m, 'emisTex=', 'emissiveTexture' in m)
    for im in j.get('images', []):
        print('   IMG', im.get('name'), im.get('mimeType'), 'uri=' + str(im.get('uri'))[:80] if im.get('uri') else 'bufferView')
    # bbox from POSITION accessors min/max
    lo = [1e30]*3; hi = [-1e30]*3
    accs = j.get('accessors', [])
    for mesh in j.get('meshes', []):
        for p in mesh['primitives']:
            a = accs[p['attributes']['POSITION']]
            if 'min' in a:
                for k in range(3):
                    lo[k] = min(lo[k], a['min'][k]); hi[k] = max(hi[k], a['max'][k])
    if lo[0] < 1e29:
        print('  bbox min %.3f %.3f %.3f  max %.3f %.3f %.3f  size %.3f %.3f %.3f'
              % (lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], hi[0]-lo[0], hi[1]-lo[1], hi[2]-lo[2]))
    for nd in j.get('nodes', [])[:20]:
        print('   NODE', nd.get('name'), 'mesh=' + str(nd.get('mesh')), 'T=' + str(nd.get('translation')),
              'S=' + str(nd.get('scale')))

for p in sys.argv[1:]:
    info(p)
