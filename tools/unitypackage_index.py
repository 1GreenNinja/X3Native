#!/usr/bin/env python3
"""Index Unity Asset Store .unitypackage archives WITHOUT extracting them.

WHY THIS EXISTS (2026-08-17): the owner owns 914 Asset Store packages, but only
211 were ever extracted to \\p13700\\G\\Assets. Every asset hunt in this repo had
therefore been searching ~23% of the library — the town lane burned an entire
agent session rejecting medieval villages while "Mega Open World City Pack
(Mobile-Optimized for Driving Simulation Games)" sat unextracted in the Unity
download cache. This tool makes the whole library searchable.

HOW A .unitypackage IS BUILT (no Unity needed to read one):
  It is a gzipped tar. Every asset in it is stored as a directory named after
  the asset's GUID, containing up to four members:
      <guid>/asset        the real file bytes (absent for folder entries)
      <guid>/asset.meta   Unity's meta yaml
      <guid>/pathname     TEXT: the original project-relative path, e.g.
                          "Assets/Meshes/SM_GasPump_01a.fbx"
      <guid>/preview.png  optional thumbnail
  Reading only the `pathname` members gives a pack's complete file listing for
  the cost of a streaming tar pass — no extraction, no disk churn.

USAGE
  python tools/unitypackage_index.py                      # index the default cache
  python tools/unitypackage_index.py --cache <dir> ...    # extra cache dirs
  python tools/unitypackage_index.py --out docs/design/ASSET_CATALOG.json
  python tools/unitypackage_index.py --search gas station # query an existing index
  python tools/unitypackage_index.py --markdown           # also write a .md summary

INCREMENTAL: an existing catalog is reused; a pack is re-read only when its
size or mtime changed. Safe to re-run while the owner is still downloading.

NEVER commit extracted asset bytes (docs/ENGINE_GOTCHAS.md gotcha 2.5). This
tool only ever writes the catalog.
"""
import argparse, json, os, sys, tarfile, time

DEFAULT_CACHES = [
    os.path.expandvars(r"%APPDATA%\Unity\Asset Store-5.x"),
    r"\\p13700\G\Assets",           # the extracted packs, for "already extracted" flags
]
# Extensions worth remembering per pack. Everything else is counted, not listed.
INTERESTING = (".fbx", ".obj", ".glb", ".gltf", ".prefab", ".unity", ".mat",
               ".png", ".tga", ".tif", ".tiff", ".jpg", ".psd", ".wav", ".mp3", ".ogg")


def log(*a):
    print("[upkg]", *a, flush=True)


def index_package(path):
    """Return a dict describing one .unitypackage, read via its pathname members."""
    entry = {
        "name": os.path.splitext(os.path.basename(path))[0],
        "path": path,
        "size": os.path.getsize(path),
        "mtime": int(os.path.getmtime(path)),
        "files": [],          # interesting file paths, original project-relative
        "counts": {},         # extension -> count, for EVERY file
        "error": None,
    }
    try:
        with tarfile.open(path, "r:gz") as tf:
            for member in tf:
                if not member.isfile() or not member.name.endswith("/pathname"):
                    continue
                fh = tf.extractfile(member)
                if fh is None:
                    continue
                # pathname is a short text blob; some packs append a newline+flag.
                p = fh.read().decode("utf-8", "replace").splitlines()
                if not p:
                    continue
                p = p[0].strip()
                ext = os.path.splitext(p)[1].lower()
                entry["counts"][ext] = entry["counts"].get(ext, 0) + 1
                if ext in INTERESTING:
                    entry["files"].append(p)
    except Exception as exc:                      # a truncated download, say
        entry["error"] = f"{type(exc).__name__}: {exc}"
    return entry


def build(cache_dirs, out_path, extracted_root):
    old = {}
    if os.path.exists(out_path):
        try:
            with open(out_path, "r", encoding="utf-8") as fh:
                for e in json.load(fh).get("packages", []):
                    old[e["path"]] = e
            log(f"reusing {len(old)} entries from {out_path}")
        except Exception as exc:
            log(f"existing catalog unreadable ({exc}); rebuilding from scratch")

    pkgs, reused, read = [], 0, 0
    for cache in cache_dirs:
        if not os.path.isdir(cache):
            log(f"skip (missing): {cache}")
            continue
        for root, _dirs, files in os.walk(cache):
            for fn in files:
                if not fn.lower().endswith(".unitypackage"):
                    continue
                full = os.path.join(root, fn)
                prev = old.get(full)
                try:
                    st = os.stat(full)
                except OSError:
                    continue
                if prev and prev.get("size") == st.st_size and prev.get("mtime") == int(st.st_mtime):
                    pkgs.append(prev); reused += 1
                else:
                    pkgs.append(index_package(full)); read += 1
                    if read % 25 == 0:
                        log(f"read {read} packages...")

    # Which packs are ALREADY extracted on the share? Name match is good enough:
    # the extracted dirs are named after the pack.
    extracted = set()
    if extracted_root and os.path.isdir(extracted_root):
        extracted = {d.lower() for d in os.listdir(extracted_root)
                     if os.path.isdir(os.path.join(extracted_root, d))}
    for p in pkgs:
        p["extracted"] = p["name"].lower() in extracted

    pkgs.sort(key=lambda p: p["name"].lower())
    catalog = {
        "generated": time.strftime("%Y-%m-%d %H:%M:%S"),
        "caches": cache_dirs,
        "package_count": len(pkgs),
        "extracted_count": sum(1 for p in pkgs if p["extracted"]),
        "packages": pkgs,
    }
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as fh:
        json.dump(catalog, fh, indent=1)
    errs = [p for p in pkgs if p.get("error")]
    log(f"indexed {len(pkgs)} packages ({read} read, {reused} reused), "
        f"{catalog['extracted_count']} already extracted, {len(errs)} unreadable "
        f"-> {out_path}")
    for p in errs[:5]:
        log(f"  UNREADABLE {p['name']}: {p['error']}")
    return catalog


def search(catalog, terms):
    """Rank packages by how many search terms hit their name or file list."""
    terms = [t.lower() for t in terms]
    hits = []
    for p in catalog["packages"]:
        name = p["name"].lower()
        matched_files = [f for f in p["files"] if any(t in f.lower() for t in terms)]
        name_hits = sum(1 for t in terms if t in name)
        if name_hits or matched_files:
            hits.append((name_hits * 50 + len(matched_files), p, matched_files))
    hits.sort(key=lambda h: -h[0])
    for score, p, mf in hits[:25]:
        flag = "extracted" if p["extracted"] else "CACHED-ONLY"
        meshes = sum(v for k, v in p["counts"].items() if k in (".fbx", ".obj", ".glb"))
        print(f"\n=== {p['name']}  [{flag}]  score {score}  ({meshes} meshes, "
              f"{p['size'] // (1024*1024)} MB)")
        for f in mf[:8]:
            print(f"    {f}")
        if len(mf) > 8:
            print(f"    ... {len(mf) - 8} more matching files")
    if not hits:
        print("no matches")


def write_markdown(catalog, md_path):
    with open(md_path, "w", encoding="utf-8") as fh:
        fh.write("# Asset catalog — every owned Unity package\n\n")
        fh.write(f"*Generated {catalog['generated']} by `tools/unitypackage_index.py`. "
                 f"{catalog['package_count']} packages, "
                 f"{catalog['extracted_count']} already extracted to the share. "
                 "Query it with `python tools/unitypackage_index.py --search <terms>` "
                 "instead of guessing what the library holds.*\n\n")
        fh.write("| Package | Extracted | MB | Meshes | Textures |\n|---|---|---|---|---|\n")
        for p in catalog["packages"]:
            meshes = sum(v for k, v in p["counts"].items() if k in (".fbx", ".obj", ".glb"))
            texs = sum(v for k, v in p["counts"].items()
                       if k in (".png", ".tga", ".tif", ".tiff", ".jpg", ".psd"))
            fh.write(f"| {p['name']} | {'yes' if p['extracted'] else 'cache only'} | "
                     f"{p['size'] // (1024*1024)} | {meshes} | {texs} |\n")
    log(f"wrote {md_path}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cache", action="append", default=None,
                    help="a directory to scan for .unitypackage files (repeatable)")
    ap.add_argument("--extracted-root", default=r"\\p13700\G\Assets",
                    help="where extracted packs live, for the 'extracted' flag")
    ap.add_argument("--out", default="docs/design/ASSET_CATALOG.json")
    ap.add_argument("--markdown", action="store_true", help="also write a .md table")
    ap.add_argument("--search", nargs="+", help="query the catalog instead of rebuilding")
    args = ap.parse_args()

    if args.search:
        if not os.path.exists(args.out):
            log(f"no catalog at {args.out} — build it first"); return 1
        with open(args.out, "r", encoding="utf-8") as fh:
            search(json.load(fh), args.search)
        return 0

    caches = args.cache or [DEFAULT_CACHES[0]]
    catalog = build(caches, args.out, args.extracted_root)
    if args.markdown:
        write_markdown(catalog, os.path.splitext(args.out)[0] + ".md")
    return 0


if __name__ == "__main__":
    sys.exit(main())
