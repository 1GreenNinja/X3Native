#!/usr/bin/env python3
# tools/mine_unity_scene.py
#
# Mine a Unity .unity scene into a JSON placement manifest for the X3Native
# env-art pipeline (env_art.cpp / spire_art.cpp).
#
# Usage:
#   python mine_unity_scene.py <pack_root> <scene.unity> <out_manifest.json>
#
# Resolution strategy (two-tier):
#   1. GUID -> path map, built by walking <pack_root> for *.meta sidecars.
#      Each .meta carries a `guid:` line whose value identifies the sibling
#      asset (.prefab / .fbx / .png / .mat / .asset).
#   2. NAME -> path map, built by walking <pack_root> for *.prefab files and
#      keying on the file stem (e.g. `SM_Modular_Ceiling_01f`).
#
# Some asset packs ship WITHOUT the .meta sidecars (the pack was extracted
# without ever being imported into a Unity project), so tier 1 may be empty.
# In that case tier 2 carries the resolution: each PrefabInstance in the scene
# has an `m_Modifications` override of `m_Name` -> "<prefab>_<instance#>", and
# stripping the trailing "_<digits>" yields the prefab's file stem.
#
# Output (JSON): a list of
#   { "prefab": "<pack-relative .prefab path>",
#     "name":   "<base prefab name>",
#     "pos":     [x, y, z],
#     "rotQuat": [x, y, z, w],
#     "scale":   [x, y, z] }
# in X3Native's coord frame (Y-up RIGHT-HANDED, +X right, +Y up, -Z forward).
#
# Axis fix (Unity Y-up LEFT-HANDED -> X3Native Y-up RIGHT-HANDED): the coord
# change is a Z-axis mirror. Under that mirror:
#   * positions      -> negate Z.
#   * rotation quat  -> negate X and Y components (preserves rotation direction
#                       under the handedness flip; Z and W unchanged).
#   * scale          -> unchanged.
# VERIFY against a known landmark in the demo before trusting the manifest. If
# the layout reads mirrored, try negating qz/qw instead — the exact convention
# depends on how the source authored its LH frame.
#
# Parent chain: this version treats every PrefabInstance's local transform as
# WORLD (most demo layouts root their objects under an identity-transform empty
# parent, in which case local == world). If a layout puts non-identity transforms
# on parent groups, positions will be off by a constant offset/rotation -- easy
# to spot and a follow-up will add real parent-chain composition.
#
# Parser choice: regex over PyYAML. Unity's emission is deterministic and very
# regular; PyYAML chokes on its !u! tags without a custom Loader AND is slow on
# multi-MB demo scenes. Regex extracts what we need with O(file-size) cost and
# zero non-stdlib dependencies.
#
# Clean-room: built from the Python standard library only. No third-party
# Unity importer source consulted.

import sys
import os
import json
import re
from pathlib import Path

# -- regexes --------------------------------------------------------------------
GUID_RE = re.compile(r'^guid:\s*([0-9a-fA-F]{32})', re.M)
PREFAB_INSTANCE_RE = re.compile(
    r'^---\s*!u!1001\s*&\d+\s*\n'
    r'PrefabInstance:\n'
    r'(.*?)(?=^---|\Z)',
    re.M | re.S,
)
SOURCE_PREFAB_RE = re.compile(
    r'm_SourcePrefab:\s*\{[^}]*guid:\s*([0-9a-fA-F]{32})'
)
TRANSFORM_PARENT_RE = re.compile(
    r'm_TransformParent:\s*\{fileID:\s*(\d+)'
)
MOD_LINE_RE = re.compile(
    r'propertyPath:\s*(\S+)\s*\n\s*value:\s*(.+?)\s*\n'
)
INSTANCE_SUFFIX_RE = re.compile(r'_\d+$')

INDEX_KINDS = {'prefab', 'fbx', 'png', 'mat', 'asset'}


def build_guid_map(pack_root: Path) -> dict:
    """guid -> {path, kind} via walking *.meta files. May be empty if the pack
    shipped without Unity-generated sidecars."""
    out = {}
    for meta_path in pack_root.rglob('*.meta'):
        try:
            text = meta_path.read_text(encoding='utf-8', errors='ignore')
        except Exception:
            continue
        m = GUID_RE.search(text)
        if not m:
            continue
        guid = m.group(1).lower()
        asset = meta_path.with_suffix('')   # strip .meta
        kind = asset.suffix.lower().lstrip('.')
        if kind in INDEX_KINDS:
            try:
                rel = asset.relative_to(pack_root)
            except ValueError:
                rel = asset
            out[guid] = {'path': str(rel).replace('\\', '/'), 'kind': kind}
    return out


def build_name_map(pack_root: Path) -> dict:
    """lowercased prefab stem -> relative path. The robust fallback when .meta
    files are absent."""
    out = {}
    for p in pack_root.rglob('*.prefab'):
        try:
            rel = p.relative_to(pack_root)
        except ValueError:
            rel = p
        out.setdefault(p.stem.lower(), str(rel).replace('\\', '/'))
    return out


def strip_instance_suffix(name: str) -> str:
    """SM_Modular_Ceiling_01f_1 -> SM_Modular_Ceiling_01f.

    Instance names in the scene often append "_N" where N is the duplicate
    index. The actual prefab file is named without that suffix."""
    return INSTANCE_SUFFIX_RE.sub('', name)


def parse_placements(scene_path: Path) -> list:
    """Return [{guid, name, parent, pos, rotQuat, scale}, ...] per PrefabInstance."""
    text = scene_path.read_text(encoding='utf-8', errors='ignore')
    placements = []
    for m in PREFAB_INSTANCE_RE.finditer(text):
        block = m.group(1)
        sm = SOURCE_PREFAB_RE.search(block)
        if not sm:
            continue
        src_guid = sm.group(1).lower()
        pm = TRANSFORM_PARENT_RE.search(block)
        parent_fid = int(pm.group(1)) if pm else 0

        mods = {}
        for mm in MOD_LINE_RE.finditer(block):
            mods[mm.group(1)] = mm.group(2).strip("'\"")

        def fv(key, default=0.0):
            v = mods.get(key)
            if v is None:
                return default
            try:
                return float(v)
            except ValueError:
                return default

        pos = [fv('m_LocalPosition.x'),
               fv('m_LocalPosition.y'),
               fv('m_LocalPosition.z')]
        rot = [fv('m_LocalRotation.x'),
               fv('m_LocalRotation.y'),
               fv('m_LocalRotation.z'),
               fv('m_LocalRotation.w', 1.0)]
        scl = [fv('m_LocalScale.x', 1.0),
               fv('m_LocalScale.y', 1.0),
               fv('m_LocalScale.z', 1.0)]
        placements.append({
            'guid':   src_guid,
            'name':   mods.get('m_Name', ''),
            'parent': parent_fid,
            'pos':    pos,
            'rotQuat': rot,
            'scale':  scl,
        })
    return placements


def unity_to_x3(pos, rot, scale):
    """Z-mirror handedness fix; see header comment for math + verify note."""
    return (
        [pos[0], pos[1], -pos[2]],
        [-rot[0], -rot[1], rot[2], rot[3]],
        list(scale),
    )


def resolve(p, guid_map, name_map):
    """Return (prefab_path_or_None, base_name). Tries GUID then NAME."""
    info = guid_map.get(p['guid'])
    if info and info['kind'] == 'prefab':
        return info['path'], Path(info['path']).stem
    nm = p.get('name') or ''
    base = strip_instance_suffix(nm).strip()
    if base:
        path = name_map.get(base.lower())
        if path:
            return path, base
    return None, base


def main():
    if len(sys.argv) < 4:
        print('usage: mine_unity_scene.py <pack_root> <scene.unity> <out.json>',
              file=sys.stderr)
        sys.exit(1)
    pack_root  = Path(sys.argv[1])
    scene_path = Path(sys.argv[2])
    out_path   = Path(sys.argv[3])

    if not pack_root.is_dir():
        print(f'pack root not a directory: {pack_root}', file=sys.stderr)
        sys.exit(2)
    if not scene_path.is_file():
        print(f'scene not a file: {scene_path}', file=sys.stderr)
        sys.exit(2)

    print(f'walking {pack_root} ...', file=sys.stderr)
    guid_map = build_guid_map(pack_root)
    name_map = build_name_map(pack_root)
    print(f'  GUID map  : {len(guid_map)} entries '
          f'({sum(1 for v in guid_map.values() if v["kind"]=="prefab")} prefab, '
          f'{sum(1 for v in guid_map.values() if v["kind"]=="fbx")} fbx)',
          file=sys.stderr)
    print(f'  NAME map  : {len(name_map)} prefabs (by file stem)', file=sys.stderr)

    print(f'parsing placements from {scene_path} ...', file=sys.stderr)
    placements = parse_placements(scene_path)
    print(f'  {len(placements)} PrefabInstance blocks found', file=sys.stderr)

    out_entries = []
    unresolved = []
    nested_count = 0
    for p in placements:
        if p['parent'] != 0:
            nested_count += 1   # treat as world for now (prototype); see header.
        path, base = resolve(p, guid_map, name_map)
        if not path:
            unresolved.append((p['guid'], p.get('name', '')))
            continue
        pos, rot, scale = unity_to_x3(p['pos'], p['rotQuat'], p['scale'])
        out_entries.append({
            'prefab':  path,
            'name':    base,
            'pos':     pos,
            'rotQuat': rot,
            'scale':   scale,
        })

    print(f'  resolved          : {len(out_entries)}', file=sys.stderr)
    print(f'  unresolved        : {len(unresolved)} '
          f'(first 3: {unresolved[:3]})', file=sys.stderr)
    print(f'  nested-parent     : {nested_count} (treated as world; see header)',
          file=sys.stderr)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(out_entries, indent=2))
    print(f'wrote {out_path} ({out_path.stat().st_size} bytes)', file=sys.stderr)


if __name__ == '__main__':
    main()
