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

USAGE
  python tools/unitypackage_extract.py "<pack.unitypackage>" <out_dir>
  python tools/unitypackage_extract.py <pack> <out> --only .fbx .png
  python tools/unitypackage_extract.py <pack> <out> --match Exterior Wheels
  python tools/unitypackage_extract.py <pack> <out> --list        # dry run

NEVER commit extracted bytes (docs/ENGINE_GOTCHAS.md gotcha 2.5) — convert,
publish to the asset store, and commit only assets/manifest.json.
"""
import argparse, os, sys, tarfile


def log(*a): print("[upkg-x]", *a, flush=True)


def extract(pack, out_dir, only=None, match=None, list_only=False):
    only = tuple(e.lower() for e in only) if only else None
    n_written = n_skipped = 0
    total_bytes = 0
    with tarfile.open(pack, "r:gz") as tf:
        # One streaming pass: buffer each guid's parts until we can pair
        # pathname with asset. Packs are small enough per-entry that holding a
        # single asset's bytes is fine; we flush as soon as a pair completes.
        pend = {}
        for m in tf:
            if not m.isfile():
                continue
            parts = m.name.split("/")
            if len(parts) < 2:
                continue
            guid, leaf = parts[0], parts[-1]
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
            if "path" in slot and "data" in slot:
                path, data = slot["path"], slot["data"]
                del pend[guid]
                if not path:
                    continue
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
                n_written += len(data) >= 0 and 1 or 0
                total_bytes += len(data)
                if n_written % 50 == 0:
                    log(f"...{n_written} files")
        # A folder entry has a pathname and no asset — that is normal.
    log(f"{'listed' if list_only else 'extracted'} {n_written} file(s), "
        f"{total_bytes / (1024*1024):.1f} MB; {n_skipped} filtered out"
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
    a = ap.parse_args()
    if not os.path.exists(a.pack):
        log(f"no such package: {a.pack}")
        return 1
    extract(a.pack, a.out, a.only, a.match, a.list)
    return 0


if __name__ == "__main__":
    sys.exit(main())
