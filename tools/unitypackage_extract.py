#!/usr/bin/env python3
"""Extract a Unity .unitypackage into a normal Assets/ tree — no Unity needed.

WHY (2026-08-17): the owner owns 914 Asset Store packages and only ~210 were
ever extracted to \\p13700\\G\\Assets. tools/unitypackage_index.py made the other
~700 SEARCHABLE; this makes them USABLE. The first customer was an NSX hunt —
`--search nsx` found nothing, but "HDRP GBX COUPE Free Update" (654 MB, 12
meshes, 74 textures, cache-only) never had a chance to be considered because
nobody could open it.

A .unitypackage is a gzipped tar. Each asset is a directory named for its GUID:
    <guid>/asset        the real bytes            (absent for folder entries)
    <guid>/asset.meta   Unity's meta yaml
    <guid>/pathname     TEXT: the original project-relative path
    <guid>/preview.png  optional (usually a generic icon, not a render)
So extraction is: read every `pathname`, write its sibling `asset` there.

WRITE THE .meta TOO — IT IS THE GUID MAP (added 2026-08-17, W-HEROCAR).
The first version of this tool wrote only `asset`, and that made every
freshly-extracted pack HALF extracted. Unity materials reference their textures
by GUID and NOTHING ELSE; the GUID->file map lives in the per-file `.meta`
(and, equivalently, in the tar's own directory names). Without it,
tools/convert_unity_pack.py cannot resolve a single texture slot and
tools/unity_scene_to_layout.py cannot resolve a single mesh reference, so a
converted pack comes out GREY — which is exactly the failure the
x3native-environments skill opens with. The GBX COUPE hero car had to fall back
to matching materials by NODE NAME because of this. Metas are tiny (a few
hundred bytes) and are what Unity itself writes, so they are ON by default;
`--no-meta` restores the old behaviour.

USAGE
  python tools/unitypackage_extract.py "<pack.unitypackage>" <out_dir>
  python tools/unitypackage_extract.py <pack> <out> --only .fbx .png
  python tools/unitypackage_extract.py <pack> <out> --match Exterior Wheels
  python tools/unitypackage_extract.py <pack> <out> --list        # dry run
  python tools/unitypackage_extract.py <pack> <out> --no-meta     # bytes only
  python tools/unitypackage_extract.py <pack> <out> --guid-map guids.json

NEVER commit extracted bytes (docs/ENGINE_GOTCHAS.md gotcha 2.5) — convert,
publish to the asset store, and commit only assets/manifest.json.
"""
import argparse, json, os, sys, tarfile


def log(*a): print("[upkg-x]", *a, flush=True)


def extract(pack, out_dir, only=None, match=None, list_only=False,
            write_meta=True, guid_map_path=None):
    only = tuple(e.lower() for e in only) if only else None
    n_written = n_skipped = n_meta = 0
    total_bytes = 0
    guid_map = {}
    with tarfile.open(pack, "r:gz") as tf:
        # One streaming pass: buffer each guid's parts until we can pair
        # pathname with asset. Packs are small enough per-entry that holding a
        # single asset's bytes is fine; we flush as soon as a pair completes.
        #
        # ORDERING NOTE: the three members of a guid directory can arrive in ANY
        # order, and asset.meta may land AFTER asset. So a guid is only flushed
        # once BOTH pathname and asset are in hand, and the meta is written from
        # whatever is buffered at that moment plus a second sweep over whatever
        # arrived late (`pend` below). The old code deleted the slot on flush,
        # which would have silently dropped every late-arriving meta.
        pend = {}
        done = {}          # guid -> written destination path (for late metas)

        def flush_meta(guid, dst, meta):
            nonlocal n_meta
            if not (write_meta and meta and dst):
                return
            with open(dst + ".meta", "wb") as f:
                f.write(meta)
            n_meta += 1

        for m in tf:
            if not m.isfile():
                continue
            parts = m.name.split("/")
            if len(parts) < 2:
                continue
            guid, leaf = parts[0], parts[-1]
            if leaf == "asset.meta" and guid in done:
                fh = tf.extractfile(m)          # meta arrived after the bytes
                if fh:
                    flush_meta(guid, done[guid], fh.read())
                continue
            slot = pend.setdefault(guid, {})
            if leaf == "pathname":
                fh = tf.extractfile(m)
                if fh:
                    txt = fh.read().decode("utf-8", "replace").splitlines()
                    slot["path"] = txt[0].strip() if txt else ""
            elif leaf == "asset":
                fh = tf.extractfile(m)
                if fh:
                    slot["data"] = fh.read()
            elif leaf == "asset.meta":
                fh = tf.extractfile(m)
                if fh:
                    slot["meta"] = fh.read()
            if "path" in slot and "data" in slot:
                path, data = slot["path"], slot["data"]
                meta = slot.get("meta")
                del pend[guid]
                if not path:
                    continue
                # THE GUID MAP. Recorded for EVERY asset, before the filters —
                # a .mat that survives the filter still references textures that
                # did not, and a half map resolves to grey.
                guid_map[guid] = path
                keep = True
                if only and not path.lower().endswith(only):
                    keep = False
                if match and not any(s.lower() in path.lower() for s in match):
                    keep = False
                if not keep:
                    n_skipped += 1
                    continue
                if list_only:
                    log(f"{len(data):>10,}  {path}")
                    n_written += 1
                    total_bytes += len(data)
                    continue
                # Unity paths are project-relative and always forward-slashed.
                dst = os.path.join(out_dir, *path.split("/"))
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with open(dst, "wb") as f:
                    f.write(data)
                done[guid] = dst
                flush_meta(guid, dst, meta)
                n_written += 1
                total_bytes += len(data)
                if n_written % 50 == 0:
                    log(f"...{n_written} files")
        # A folder entry has a pathname and no asset — that is normal.
    if guid_map_path and not list_only:
        os.makedirs(os.path.dirname(os.path.abspath(guid_map_path)), exist_ok=True)
        with open(guid_map_path, "w", encoding="utf-8") as f:
            json.dump(guid_map, f, indent=1, sort_keys=True)
        log(f"guid map: {len(guid_map)} entries -> {guid_map_path}")
    log(f"{'listed' if list_only else 'extracted'} {n_written} file(s), "
        f"{total_bytes / (1024*1024):.1f} MB, {n_meta} .meta; {n_skipped} filtered out"
        + ("" if list_only else f" -> {out_dir}"))
    return n_written


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pack", help="path to the .unitypackage")
    ap.add_argument("out", nargs="?", default=".", help="output directory")
    ap.add_argument("--only", nargs="+", help="keep only these extensions (.fbx .png ...)")
    ap.add_argument("--match", nargs="+", help="keep only paths containing any of these")
    ap.add_argument("--list", action="store_true", help="dry run — print what would be written")
    ap.add_argument("--no-meta", action="store_true",
                    help="do NOT write the .meta sidecars (they are the GUID map — "
                         "without them Unity materials cannot be resolved and the "
                         "pack converts GREY)")
    ap.add_argument("--guid-map", help="also write a {guid: project path} JSON for the "
                                       "WHOLE package (including filtered-out files)")
    a = ap.parse_args()
    if not os.path.exists(a.pack):
        log(f"no such package: {a.pack}")
        return 1
    extract(a.pack, a.out, a.only, a.match, a.list,
            write_meta=not a.no_meta, guid_map_path=a.guid_map)
    return 0


if __name__ == "__main__":
    sys.exit(main())
