#!/usr/bin/env python3
# tools/inspect_glb.py
#
# Headless sanity check for converted GLBs: parses each file's binary header +
# JSON chunk and reports mesh / primitive / vertex / triangle / material /
# texture / image counts. Used as the Stage-2 verification step before wiring
# the converted Hospital pack into spire_art.cpp.
#
# Usage:
#   python inspect_glb.py <dir-or-file> [<dir-or-file> ...]
#
# Exit status: 0 if every GLB has > 0 meshes AND > 0 vertices; 1 otherwise
# (Blender export ate it silently). Lets CI / wrap-scripts gate on success.
#
# Clean-room: pure Python stdlib. Parses the GLB 2.0 container directly from
# the published format spec (12-byte header + JSON chunk + optional BIN chunk).

import json
import struct
import sys
from pathlib import Path

GLB_MAGIC = 0x46546C67   # "glTF" little-endian
JSON_CHUNK = 0x4E4F534A  # "JSON"
BIN_CHUNK  = 0x004E4942  # "BIN\0"


def inspect(path: Path) -> dict:
    """Return a stats dict for one GLB. Raises on malformed input."""
    data = path.read_bytes()
    if len(data) < 12:
        raise ValueError('file too small to be a GLB')

    magic, version, total_len = struct.unpack_from('<III', data, 0)
    if magic != GLB_MAGIC:
        raise ValueError(f'bad magic 0x{magic:08x} (expected glTF)')
    if version != 2:
        raise ValueError(f'GLB version {version} (expected 2)')
    if total_len != len(data):
        raise ValueError(
            f'declared length {total_len} != file size {len(data)}'
        )

    # First chunk MUST be JSON.
    off = 12
    chunk_len, chunk_type = struct.unpack_from('<II', data, off)
    if chunk_type != JSON_CHUNK:
        raise ValueError(f'first chunk is 0x{chunk_type:08x}, not JSON')
    off += 8
    gltf = json.loads(data[off:off + chunk_len].rstrip(b' \x00'))
    off += chunk_len

    # Optional BIN chunk; record its size for sanity (geometry usually lives there).
    bin_size = 0
    if off + 8 <= len(data):
        chunk_len2, chunk_type2 = struct.unpack_from('<II', data, off)
        if chunk_type2 == BIN_CHUNK:
            bin_size = chunk_len2

    meshes     = gltf.get('meshes', [])
    accessors  = gltf.get('accessors', [])
    materials  = gltf.get('materials', [])
    textures   = gltf.get('textures', [])
    images     = gltf.get('images', [])

    n_prims    = sum(len(m.get('primitives', [])) for m in meshes)
    n_verts    = 0
    n_tris     = 0

    for m in meshes:
        for prim in m.get('primitives', []):
            pos_idx = prim.get('attributes', {}).get('POSITION')
            if pos_idx is not None and pos_idx < len(accessors):
                n_verts += accessors[pos_idx].get('count', 0)
            idx_idx = prim.get('indices')
            if idx_idx is not None and idx_idx < len(accessors):
                n_tris += accessors[idx_idx].get('count', 0) // 3

    return {
        'file':       path.name,
        'size_kb':    round(len(data) / 1024, 1),
        'bin_kb':     round(bin_size / 1024, 1),
        'meshes':     len(meshes),
        'prims':      n_prims,
        'verts':      n_verts,
        'tris':       n_tris,
        'materials':  len(materials),
        'textures':   len(textures),
        'images':     len(images),
    }


def main():
    if len(sys.argv) < 2:
        print('usage: inspect_glb.py <dir-or-file> [...]', file=sys.stderr)
        return 2

    targets = []
    for arg in sys.argv[1:]:
        p = Path(arg)
        if p.is_dir():
            targets.extend(sorted(p.rglob('*.glb')))
        elif p.is_file():
            targets.append(p)
        else:
            print(f'skip (not found): {p}', file=sys.stderr)

    if not targets:
        print('no GLBs found', file=sys.stderr)
        return 1

    rows = []
    bad  = []
    for g in targets:
        try:
            r = inspect(g)
        except Exception as e:
            print(f'FAIL  {g.name:48s}  {e}')
            bad.append(g.name)
            continue
        rows.append(r)
        flag = '  '
        if r['meshes'] == 0 or r['verts'] == 0:
            flag = ' !'
            bad.append(g.name)
        print(
            f'{flag}{r["file"]:48s} '
            f'mesh={r["meshes"]:2d} prim={r["prims"]:2d} '
            f'v={r["verts"]:6d} t={r["tris"]:6d} '
            f'mat={r["materials"]:2d} tex={r["textures"]:2d} '
            f'img={r["images"]:2d}  bin={r["bin_kb"]:7.1f}KB'
        )

    print()
    print('--- summary ---')
    print(f'files       : {len(rows)}')
    print(f'total verts : {sum(r["verts"] for r in rows):,}')
    print(f'total tris  : {sum(r["tris"] for r in rows):,}')
    print(f'with mats   : {sum(1 for r in rows if r["materials"] > 0)}')
    print(f'with imgs   : {sum(1 for r in rows if r["images"] > 0)}')
    print(f'failing     : {len(bad)}{"  (" + ", ".join(bad) + ")" if bad else ""}')

    return 0 if not bad else 1


if __name__ == '__main__':
    sys.exit(main())
