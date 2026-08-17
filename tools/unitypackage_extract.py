#!/usr/bin/env python3
"""Extract Unity Asset Store .unitypackage archives to a normal folder tree.

WHY THIS EXISTS. The fleet has ~914 packs and, by unitypackage_index.py's own
docstring, only 211 were ever extracted. unitypackage_index.py READS packs
without extracting them (that is its whole point), and convert_unity_pack.py
needs an ALREADY-extracted tree (Meshes/, Textures/, Meshes/Materials/). The
step between them did not exist. This is that step.

Owner, 2026-08-17: the 14900k is out of space on both HDDs; this box has a
nearly-empty 4 TB. So the packs get unpacked HERE and served from here.

HOW A .unitypackage IS BUILT (no Unity needed):
    a gzipped tar, one DIRECTORY PER ASSET named by its Unity GUID:
        <guid>/pathname   - the original "Assets/Foo/Bar.fbx" path, as text
        <guid>/asset      - the file bytes  (absent for folder-only entries)
        <guid>/asset.meta - Unity's .meta   (kept: importer settings + the GUID)
        <guid>/preview.png- thumbnail       (skipped by default, --previews keeps)
So extraction = for every guid dir, read `pathname` and write `asset` there.

WHERE IT WRITES, AND WHY NOT UNDER D:\\Assets.
app/asset_root.h carries a load-bearing warning: assetRoot() picks the first
candidate directory that CONTAINS surface_library/ or converted_glb/, and it
already resolved to an unrelated D:\\Assets once, making every committed asset
invisible. The comment says it is "one `mkdir D:\\Assets\\converted_glb` away"
from silently repointing every deployed build at the wrong tree. Unity packs
are full of folders with arbitrary names, so extracting them under D:\\Assets
is exactly how that trap gets sprung by accident. Default root is therefore a
SEPARATE tree: D:\\UnityPacks\\<pack name>\\.

SAFETY
  * Path traversal: any pathname escaping the pack root is refused, not written.
  * Never deletes. An existing file is skipped unless --force.
  * Incremental: a pack whose marker file records the same size+mtime is skipped,
    so re-running after a partial copy costs a stat, not a re-extract.
  * Windows long paths and reserved names are handled (\\\\?\\ prefix, CON/PRN...).

USAGE
  python tools/unitypackage_extract.py --list
  python tools/unitypackage_extract.py                       # extract default cache
  python tools/unitypackage_extract.py --cache <dir> --out D:\\UnityPacks
  python tools/unitypackage_extract.py --only "Gas Station" --dry-run
"""
import argparse, json, os, re, sys, tarfile, time

DEFAULT_CACHES = [
    os.path.expandvars(r"%APPDATA%\Unity\Asset Store-5.x"),
]
DEFAULT_OUT = r"D:\UnityPacks"
MARKER = ".x3_extracted.json"

# Windows device names that cannot be a path component, whatever the pack says.
_RESERVED = {"CON", "PRN", "AUX", "NUL", *(f"COM{i}" for i in range(1, 10)),
             *(f"LPT{i}" for i in range(1, 10))}


def log(*a):
    print("[unpack]", *a, flush=True)


def human(n):
    for u in ("B", "KB", "MB", "GB", "TB"):
        if n < 1024 or u == "TB":
            return f"{n:,.1f} {u}"
        n /= 1024.0


def safe_component(c):
    c = c.strip().rstrip(".")                     # trailing dots/spaces are illegal
    if not c:
        return "_"
    stem = c.split(".")[0].upper()
    if stem in _RESERVED:
        c = "_" + c
    return re.sub(r'[<>:"|?*\x00-\x1f]', "_", c)


def safe_join(root, pathname):
    """Resolve a pack-relative pathname under root, refusing traversal."""
    parts = [p for p in pathname.replace("\\", "/").split("/") if p not in ("", ".")]
    if any(p == ".." for p in parts):
        return None
    parts = [safe_component(p) for p in parts]
    if not parts:
        return None
    full = os.path.join(root, *parts)
    # Belt and braces: the resolved path must still live under root.
    if os.path.commonpath([os.path.abspath(full), os.path.abspath(root)]) != os.path.abspath(root):
        return None
    return full


def long_path(p):
    """Windows MAX_PATH escape. Unity packs nest deeply and WILL exceed 260."""
    if os.name != "nt":
        return p
    p = os.path.abspath(p)
    return p if p.startswith("\\\\?\\") else "\\\\?\\" + p


def pack_marker(out_dir):
    try:
        with open(os.path.join(out_dir, MARKER), encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def extract_pack(pkg_path, out_root, keep_previews=False, force=False, dry=False):
    """Returns (status, files_written, bytes_written)."""
    name = os.path.splitext(os.path.basename(pkg_path))[0]
    out_dir = os.path.join(out_root, safe_component(name))
    st = os.stat(pkg_path)
    stamp = {"src": pkg_path, "size": st.st_size, "mtime": int(st.st_mtime)}

    prev = pack_marker(out_dir)
    if prev and not force and prev.get("size") == stamp["size"] \
            and prev.get("mtime") == stamp["mtime"]:
        return ("skip", 0, 0)

    if dry:
        return ("would-extract", 0, 0)

    # PASS 1: read every `pathname` member so we know where each guid goes.
    # Streaming, because these archives run to gigabytes.
    routes = {}
    try:
        with tarfile.open(pkg_path, "r:gz") as tf:
            for m in tf:
                if not m.isfile():
                    continue
                parts = m.name.split("/")
                if len(parts) < 2 or parts[-1] != "pathname":
                    continue
                fh = tf.extractfile(m)
                if not fh:
                    continue
                # pathname is text, occasionally with a trailing newline or a
                # second line (Unity writes an optional "00" flag line).
                raw = fh.read().decode("utf-8", "replace").splitlines()
                if raw:
                    routes[parts[0]] = raw[0].strip()
    except (tarfile.TarError, OSError) as e:
        log(f"  !! {name}: cannot read ({e})")
        return ("error", 0, 0)

    if not routes:
        log(f"  !! {name}: no pathname members — not a unitypackage?")
        return ("error", 0, 0)

    # PASS 2: write the payloads.
    wrote = nbytes = 0
    wanted = {"asset", "asset.meta"} | ({"preview.png"} if keep_previews else set())
    try:
        with tarfile.open(pkg_path, "r:gz") as tf:
            for m in tf:
                if not m.isfile():
                    continue
                parts = m.name.split("/")
                if len(parts) < 2 or parts[-1] not in wanted:
                    continue
                route = routes.get(parts[0])
                if not route:
                    continue
                dest = safe_join(out_dir, route)
                if dest is None:
                    log(f"  !! {name}: refused traversal path {route!r}")
                    continue
                if parts[-1] == "asset.meta":
                    dest += ".meta"
                elif parts[-1] == "preview.png":
                    dest += ".preview.png"
                if os.path.exists(long_path(dest)) and not force:
                    continue
                os.makedirs(long_path(os.path.dirname(dest)), exist_ok=True)
                fh = tf.extractfile(m)
                if not fh:
                    continue
                with open(long_path(dest), "wb") as out:
                    while True:
                        chunk = fh.read(1 << 20)
                        if not chunk:
                            break
                        out.write(chunk)
                        nbytes += len(chunk)
                wrote += 1
    except (tarfile.TarError, OSError) as e:
        log(f"  !! {name}: extract failed after {wrote} files ({e})")
        return ("error", wrote, nbytes)

    stamp.update(files=wrote, bytes=nbytes, when=int(time.time()))
    os.makedirs(long_path(out_dir), exist_ok=True)
    with open(long_path(os.path.join(out_dir, MARKER)), "w", encoding="utf-8") as f:
        json.dump(stamp, f, indent=2)
    return ("ok", wrote, nbytes)


def find_packs(caches):
    seen, out = set(), []
    for c in caches:
        if not os.path.isdir(c):
            log(f"cache not present: {c}")
            continue
        for root, _d, files in os.walk(c):
            for fn in files:
                if fn.lower().endswith(".unitypackage"):
                    p = os.path.join(root, fn)
                    if p.lower() not in seen:
                        seen.add(p.lower())
                        out.append(p)
    return sorted(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cache", action="append", default=[],
                    help="extra .unitypackage cache dir (repeatable)")
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--only", default=None, help="substring filter on pack name")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--previews", action="store_true", help="also write preview.png")
    a = ap.parse_args()

    caches = (a.cache or []) + DEFAULT_CACHES
    packs = find_packs(caches)
    if a.only:
        packs = [p for p in packs if a.only.lower() in os.path.basename(p).lower()]

    total = sum(os.path.getsize(p) for p in packs)
    log(f"{len(packs)} pack(s), {human(total)} of archives")
    log(f"destination: {a.out}")
    if a.list:
        for p in packs:
            log(f"  {human(os.path.getsize(p)):>10}  {os.path.basename(p)}")
        return 0

    os.makedirs(long_path(a.out), exist_ok=True)
    t0 = time.time()
    counts = {"ok": 0, "skip": 0, "error": 0, "would-extract": 0}
    files = nbytes = 0
    for i, p in enumerate(packs, 1):
        nm = os.path.basename(p)
        log(f"[{i}/{len(packs)}] {nm} ({human(os.path.getsize(p))})")
        status, w, b = extract_pack(p, a.out, a.previews, a.force, a.dry_run)
        counts[status] = counts.get(status, 0) + 1
        files += w
        nbytes += b
        if status == "ok":
            log(f"        -> {w:,} files, {human(b)}")
        elif status == "skip":
            log("        -> already extracted (size+mtime match)")

    dt = time.time() - t0
    log("")
    log(f"DONE in {dt/60:.1f} min: {counts['ok']} extracted, {counts['skip']} skipped, "
        f"{counts['error']} failed"
        + (f", {counts['would-extract']} would extract" if a.dry_run else ""))
    log(f"     {files:,} files, {human(nbytes)} written to {a.out}")
    return 1 if counts["error"] else 0


if __name__ == "__main__":
    sys.exit(main())
