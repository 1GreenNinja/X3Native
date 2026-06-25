"""
bake_attack_clip.py — append a procedural "Attack" clip to a humanoid *_anim.glb.

    python bake_attack_clip.py <in_anim.glb> [out_anim.glb]

The enemy rigs (chief_martinez_anim / marcus_webb_anim) ship with only
Idle/Jump/Run/Walk — there is NO attack/melee clip anywhere, so an attacking
enemy plays idle during its wind-up (the playtest "attack animation unsolved"
bug). This bakes a short forward arm-swing / lunge clip named "Attack" that the
runtime's m_skinner.findClip({"attack","melee","swing","bite"}) resolves, and
the monster anim-drive plays while m_winding.

Clean-room: pure glTF 2.0 binary (GLB) manipulation, no Blender. We rotate the
right Upper/Lower arm bones about their LOCAL axis through a wind-back -> strike
-> recover cycle, composed onto each bone's BIND rotation so the rest of the
skeleton is untouched. A subtle Spine twist + forward-lean sells the lunge.

Rotation channels are appended as standard LINEAR samplers; new accessor /
bufferView / sampler / channel entries reference appended BIN data. The original
clips are left byte-for-byte intact (we only grow the buffer + add one animation).
"""
import struct, json, sys, math

def quat_mul(a, b):
    # a,b = (x,y,z,w); returns a*b (apply b then a)
    ax,ay,az,aw = a; bx,by,bz,bw = b
    return (
        aw*bx + ax*bw + ay*bz - az*by,
        aw*by - ax*bz + ay*bw + az*bx,
        aw*bz + ax*by - ay*bx + az*bw,
        aw*bw - ax*bx - ay*by - az*bz,
    )

def axis_angle(ax, ay, az, ang):
    h = ang * 0.5
    s = math.sin(h)
    return (ax*s, ay*s, az*s, math.cos(h))

def load_glb(path):
    with open(path, 'rb') as f:
        data = f.read()
    magic, ver, length = struct.unpack('<III', data[:12])
    assert magic == 0x46546C67, "not a GLB"
    off = 12
    chunks = []
    while off < length:
        clen, ctype = struct.unpack('<II', data[off:off+8]); off += 8
        chunks.append((ctype, data[off:off+clen])); off += clen
    js = json.loads(chunks[0][1])
    bin_blob = b''
    for ctype, blob in chunks[1:]:
        if ctype == 0x004E4942:  # BIN
            bin_blob = blob; break
    return js, bytearray(bin_blob)

def write_glb(path, js, bin_blob):
    jb = json.dumps(js, separators=(',', ':')).encode('utf-8')
    jb += b' ' * ((4 - len(jb) % 4) % 4)
    bb = bytes(bin_blob)
    bb += b'\x00' * ((4 - len(bb) % 4) % 4)
    total = 12 + 8 + len(jb) + 8 + len(bb)
    with open(path, 'wb') as f:
        f.write(struct.pack('<III', 0x46546C67, 2, total))
        f.write(struct.pack('<II', len(jb), 0x4E4F534A)); f.write(jb)
        f.write(struct.pack('<II', len(bb), 0x004E4942)); f.write(bb)

def find_node(js, *names):
    low = [n.get('name', '').lower() for n in js['nodes']]
    for nm in names:
        for i, l in enumerate(low):
            if l == nm.lower():
                return i
    for nm in names:
        for i, l in enumerate(low):
            if nm.lower() in l:
                return i
    return -1

def bind_rot(js, idx):
    r = js['nodes'][idx].get('rotation')
    return tuple(r) if r else (0.0, 0.0, 0.0, 1.0)

def main():
    inp = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else inp
    js, bin_blob = load_glb(inp)

    names = [a.get('name', '') for a in js.get('animations', [])]
    if any(n.lower() == 'attack' for n in names):
        print('[bake] Attack already present in', inp, '-> skip'); return
    if not js.get('buffers'):
        print('[bake] no buffer -> skip', inp); return

    upper = find_node(js, 'UpperArm.R', 'RightArm', 'mixamorigRightArm', 'upper_arm.R')
    lower = find_node(js, 'LowerArm.R', 'RightForeArm', 'mixamorigRightForeArm', 'forearm.R')
    spine = find_node(js, 'Spine', 'spine', 'mixamorigSpine')
    if upper < 0:
        print('[bake] no right-arm bone found -> skip', inp); return

    # Attack timeline (seconds): anticipation (wind back) -> strike (swing fwd)
    # -> recover. Times + LOCAL-X swing angle (rad) per bone, composed on bind.
    #   t:   0.00  0.22  0.40  0.62  0.90
    #   up:  0    -0.9   1.3   0.4   0      (back, then hard forward overhead)
    #   lo:  0    -0.3   1.6   0.6   0      (forearm whips through)
    #   sp:  0     0.10 -0.18 -0.05  0      (torso twist into the blow)
    times = [0.0, 0.22, 0.40, 0.62, 0.90]
    up_a  = [0.0, -0.90, 1.30, 0.40, 0.0]
    lo_a  = [0.0, -0.30, 1.60, 0.60, 0.0]
    sp_a  = [0.0,  0.10, -0.18, -0.05, 0.0]

    def rot_track(node_idx, angles, axis):
        b = bind_rot(js, node_idx)
        out = []
        for a in angles:
            q = quat_mul(b, axis_angle(axis[0], axis[1], axis[2], a))
            out.extend(q)
        return out

    tracks = []  # (node_idx, path, floats)
    tracks.append((upper, 'rotation', rot_track(upper, up_a, (1, 0, 0))))
    if lower >= 0:
        tracks.append((lower, 'rotation', rot_track(lower, lo_a, (1, 0, 0))))
    if spine >= 0:
        tracks.append((spine, 'rotation', rot_track(spine, sp_a, (0, 1, 0))))

    # Append BIN data 4-byte aligned. One shared TIME accessor + one per track.
    js.setdefault('accessors', [])
    js.setdefault('bufferViews', [])

    def pad():
        while len(bin_blob) % 4:
            bin_blob.append(0)

    def add_bufferview(blob):
        pad()
        off = len(bin_blob)
        bin_blob.extend(blob)
        js['bufferViews'].append({'buffer': 0, 'byteOffset': off, 'byteLength': len(blob)})
        return len(js['bufferViews']) - 1

    # TIME accessor (shared input).
    tblob = struct.pack('<%df' % len(times), *times)
    tbv = add_bufferview(tblob)
    js['accessors'].append({
        'bufferView': tbv, 'componentType': 5126, 'count': len(times),
        'type': 'SCALAR', 'min': [times[0]], 'max': [times[-1]],
    })
    time_acc = len(js['accessors']) - 1

    samplers = []
    channels = []
    for node_idx, path, floats in tracks:
        ncomp = 4 if path == 'rotation' else 3
        cnt = len(floats) // ncomp
        oblob = struct.pack('<%df' % len(floats), *floats)
        obv = add_bufferview(oblob)
        js['accessors'].append({
            'bufferView': obv, 'componentType': 5126, 'count': cnt,
            'type': 'VEC4' if ncomp == 4 else 'VEC3',
        })
        out_acc = len(js['accessors']) - 1
        samplers.append({'input': time_acc, 'interpolation': 'LINEAR', 'output': out_acc})
        channels.append({'sampler': len(samplers) - 1, 'target': {'node': node_idx, 'path': path}})

    js.setdefault('animations', [])
    js['animations'].append({'name': 'Attack', 'samplers': samplers, 'channels': channels})

    # Single embedded buffer -> update byteLength to the grown BIN size.
    js['buffers'][0]['byteLength'] = len(bin_blob)

    write_glb(out, js, bin_blob)
    print('[bake] wrote Attack clip to', out,
          '(upper=%d lower=%d spine=%d, %d keys)' % (upper, lower, spine, len(times)))

if __name__ == '__main__':
    main()
