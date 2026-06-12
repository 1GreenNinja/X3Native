#!/usr/bin/env python3
"""X3Native fleet asset store — content-addressed distribution outside git/LFS.

Implements Phase A of docs/ASSET_DISTRIBUTION.md: big binary assets live in a
content-addressed store on the fleet share (with a local NVMe cache), and the
repo commits only a tiny JSON manifest (assets/manifest.json) of
{repo_path, sha256, size, source_note}.

Store layout (immutable, content-addressed, no extensions):
    <store>/objects/<sha256[0:2]>/<sha256>

Subcommands
    publish <files-or-dirs...> [--note TEXT]
        Hash each file, copy it into the store if absent, upsert its manifest
        entry. Adding an asset = run this + commit the manifest diff.
    fetch [--all | <repo-paths...>]
        For manifest entries missing locally or hash-mismatched, copy the
        object from the store (D: cache first if it already has the object,
        else the G: primary; primary fetches also populate the cache).
        A mismatched local file is moved aside to *.pre-fetch.bak, never deleted.
    verify
        CI-able. Every manifest entry must (a) exist in a reachable store tier
        with the right size and (b) exist locally with a matching SHA-256.
        Exit 0 = green, 1 = problems.
    status
        Quick report: ok / missing-local / mismatched / missing-in-store, plus
        unpublished local files under the managed asset dirs.

Guarantees: NEVER deletes anything (worst case it renames a corrupt local file
to *.pre-fetch.bak). All store writes are temp-file + atomic rename. Network
operations retry on transient OSErrors. Paths with spaces and UNC paths work.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MANIFEST_PATH = REPO_ROOT / "assets" / "manifest.json"

# Store tiers. The manifest's "store" block (if present) provides the defaults;
# env vars X3_ASSET_STORE / X3_ASSET_CACHE override both.
DEFAULT_PRIMARY = r"\\p13700\G\X3AssetStore"
DEFAULT_CACHE = r"D:\Assets\X3AssetStore"

# Directories whose contents are managed by the store (used by `status` to
# flag unpublished local files). Mirrors the pre-commit guard.
MANAGED_DIRS = ("assets/rigged_glb", "assets/converted_glb")

CHUNK = 4 * 1024 * 1024
RETRIES = 4
RETRY_BASE_DELAY = 0.5  # seconds; doubles each attempt (network blips)


def log(msg: str) -> None:
    print(msg, flush=True)


def retry(desc: str, fn):
    """Run fn(), retrying on OSError with exponential backoff."""
    last = None
    for attempt in range(RETRIES):
        try:
            return fn()
        except OSError as e:  # network blip / share hiccup
            last = e
            if attempt + 1 < RETRIES:
                delay = RETRY_BASE_DELAY * (2 ** attempt)
                log(f"  retry {attempt + 1}/{RETRIES - 1} after error ({desc}): {e} — waiting {delay:.1f}s")
                time.sleep(delay)
    raise last


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    def _read():
        h2 = hashlib.sha256()
        with open(path, "rb") as f:
            while True:
                b = f.read(CHUNK)
                if not b:
                    break
                h2.update(b)
        return h2.hexdigest()
    return retry(f"hash {path}", _read)


def load_manifest() -> dict:
    if MANIFEST_PATH.is_file():
        with open(MANIFEST_PATH, "r", encoding="utf-8") as f:
            return json.load(f)
    return {
        "version": 1,
        "store": {"primary": DEFAULT_PRIMARY, "cache": DEFAULT_CACHE},
        "assets": [],
    }


def save_manifest(m: dict) -> None:
    m["assets"] = sorted(m["assets"], key=lambda e: e["repo_path"])
    MANIFEST_PATH.parent.mkdir(parents=True, exist_ok=True)
    tmp = MANIFEST_PATH.with_suffix(".json.tmp")
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        json.dump(m, f, indent=2)
        f.write("\n")
    os.replace(tmp, MANIFEST_PATH)


def store_roots(m: dict) -> tuple[Path, Path]:
    s = m.get("store", {})
    primary = Path(os.environ.get("X3_ASSET_STORE", s.get("primary", DEFAULT_PRIMARY)))
    cache = Path(os.environ.get("X3_ASSET_CACHE", s.get("cache", DEFAULT_CACHE)))
    return primary, cache


def object_path(root: Path, sha: str) -> Path:
    return root / "objects" / sha[:2] / sha


def atomic_copy(src: Path, dst: Path) -> None:
    """Copy src -> dst via temp file + rename. Never leaves partial objects."""
    dst.parent.mkdir(parents=True, exist_ok=True)
    tmp = dst.parent / f"{dst.name}.tmp.{os.getpid()}"
    try:
        retry(f"copy {src} -> {dst}", lambda: shutil.copyfile(src, tmp))
        os.replace(tmp, dst)
    finally:
        if tmp.exists():
            try:
                tmp.unlink()
            except OSError:
                pass


def repo_rel(p: Path) -> str:
    """Repo-relative POSIX path; raises if p is outside the repo."""
    return p.resolve().relative_to(REPO_ROOT).as_posix()


def expand_files(args: list[str]) -> list[Path]:
    out: list[Path] = []
    for a in args:
        p = Path(a)
        if not p.is_absolute():
            p = Path.cwd() / p
        if p.is_dir():
            out.extend(sorted(q for q in p.rglob("*") if q.is_file()))
        elif p.is_file():
            out.append(p)
        else:
            log(f"WARNING: not found, skipped: {a}")
    return out


def fmt_mb(n: int) -> str:
    return f"{n / (1024 * 1024):.1f} MB"


# ---------------------------------------------------------------- publish ----

def cmd_publish(args) -> int:
    m = load_manifest()
    primary, cache = store_roots(m)

    # Writability probe: primary first, else fall back to the cache tier as the
    # publish target (documented Phase A fallback when the share is offline).
    target = None
    for root, name in ((primary, "primary"), (cache, "cache")):
        try:
            (root / "objects").mkdir(parents=True, exist_ok=True)
            probe = root / f".writeprobe.{os.getpid()}"
            probe.write_bytes(b"x")
            probe.unlink()
            target = root
            if name == "cache":
                log(f"NOTE: primary store {primary} not writable — publishing to cache tier {cache}.")
                log("      Re-run 'publish' against the share later (it skips objects already present).")
            break
        except OSError as e:
            log(f"store tier '{name}' ({root}) not writable: {e}")
    if target is None:
        log("ERROR: no writable store tier. Aborting (nothing changed).")
        return 1

    files = expand_files(args.files)
    if not files:
        log("nothing to publish")
        return 1

    by_path = {e["repo_path"]: e for e in m["assets"]}
    published = copied = skipped = 0
    bytes_total = bytes_copied = 0

    for f in files:
        try:
            rel = repo_rel(f)
        except ValueError:
            log(f"SKIP (outside repo, can't be a manifest repo_path): {f}")
            continue
        size = f.stat().st_size
        log(f"hashing  {rel} ({fmt_mb(size)})")
        sha = sha256_of(f)
        obj = object_path(target, sha)

        if obj.is_file() and obj.stat().st_size == size:
            skipped += 1
        else:
            log(f"  -> store {obj}")
            atomic_copy(f, obj)
            # Round-trip integrity: re-hash the store copy before trusting it.
            back = sha256_of(obj)
            if back != sha:
                log(f"ERROR: store copy hash mismatch for {rel} (got {back}); object left for inspection, manifest NOT updated.")
                return 1
            copied += 1
            bytes_copied += size

        entry = {
            "repo_path": rel,
            "sha256": sha,
            "size": size,
            "source_note": args.note or by_path.get(rel, {}).get("source_note", ""),
        }
        if by_path.get(rel) != entry:
            by_path[rel] = entry
        published += 1
        bytes_total += size

    m["assets"] = list(by_path.values())
    save_manifest(m)
    log(f"\npublish: {published} file(s), {fmt_mb(bytes_total)} total; "
        f"{copied} new object(s) uploaded ({fmt_mb(bytes_copied)}), {skipped} already in store.")
    log(f"manifest: {MANIFEST_PATH} ({len(m['assets'])} entries) — commit this file.")
    return 0


# ------------------------------------------------------------------ fetch ----

def find_object(sha: str, size: int, primary: Path, cache: Path):
    """Locate an object: cache first (cheap), then primary. Returns (path, tier)."""
    for root, tier in ((cache, "cache"), (primary, "primary")):
        try:
            obj = object_path(root, sha)
            if obj.is_file() and obj.stat().st_size == size:
                return obj, tier
        except OSError:
            continue
    return None, None


def cmd_fetch(args) -> int:
    m = load_manifest()
    primary, cache = store_roots(m)
    entries = m["assets"]
    if not args.all:
        want = {Path(p).as_posix().lstrip("./") for p in args.paths}
        entries = [e for e in entries if e["repo_path"] in want]
        missing = want - {e["repo_path"] for e in entries}
        for p in missing:
            log(f"WARNING: not in manifest: {p}")
    if not entries:
        log("nothing selected (use --all or list repo paths)")
        return 1

    fetched = ok = failed = 0
    for e in entries:
        dest = REPO_ROOT / e["repo_path"]
        if dest.is_file() and dest.stat().st_size == e["size"] and sha256_of(dest) == e["sha256"]:
            ok += 1
            continue

        obj, tier = find_object(e["sha256"], e["size"], primary, cache)
        if obj is None:
            log(f"MISSING IN STORE: {e['repo_path']} (sha {e['sha256'][:12]}…) — "
                f"checked {cache} and {primary}")
            failed += 1
            continue

        log(f"fetch    {e['repo_path']} ({fmt_mb(e['size'])}) from {tier}")
        tmp = dest.parent / f"{dest.name}.fetch.{os.getpid()}"
        try:
            dest.parent.mkdir(parents=True, exist_ok=True)
            retry(f"copy {obj}", lambda: shutil.copyfile(obj, tmp))
            got = sha256_of(tmp)
            if got != e["sha256"]:
                log(f"  ERROR: fetched bytes hash {got[:12]}… != manifest {e['sha256'][:12]}…; left at {tmp}")
                failed += 1
                continue
            if dest.exists():
                bak = dest.with_name(dest.name + ".pre-fetch.bak")
                log(f"  local file mismatched — preserved as {bak.name}")
                if bak.exists():
                    bak = dest.with_name(f"{dest.name}.pre-fetch.{int(time.time())}.bak")
                os.replace(dest, bak)
            os.replace(tmp, dest)
            fetched += 1
        finally:
            if tmp.exists():
                try:
                    tmp.unlink()
                except OSError:
                    pass

        # Populate the local cache tier so the next fetch skips the network.
        if tier == "primary":
            try:
                cobj = object_path(cache, e["sha256"])
                if not cobj.is_file():
                    atomic_copy(dest, cobj)
            except OSError as ce:
                log(f"  (cache populate skipped: {ce})")

    log(f"\nfetch: {ok} already current, {fetched} fetched, {failed} failed.")
    return 0 if failed == 0 else 1


# ----------------------------------------------------------------- verify ----

def cmd_verify(_args) -> int:
    m = load_manifest()
    primary, cache = store_roots(m)
    bad = 0
    for e in m["assets"]:
        problems = []
        obj, _tier = find_object(e["sha256"], e["size"], primary, cache)
        if obj is None:
            problems.append("not in any reachable store tier")
        dest = REPO_ROOT / e["repo_path"]
        if not dest.is_file():
            problems.append("missing locally")
        elif sha256_of(dest) != e["sha256"]:
            problems.append("local hash mismatch")
        if problems:
            bad += 1
            log(f"FAIL  {e['repo_path']}: {'; '.join(problems)}")
    n = len(m["assets"])
    if bad:
        log(f"\nverify: {bad}/{n} entries FAILED. Run: python tools/asset_store.py fetch --all")
        return 1
    log(f"verify: all {n} manifest entries OK (store object present, local hash matches).")
    return 0


# ----------------------------------------------------------------- status ----

def cmd_status(_args) -> int:
    m = load_manifest()
    primary, cache = store_roots(m)
    ok = miss_local = mismatched = miss_store = 0
    for e in m["assets"]:
        dest = REPO_ROOT / e["repo_path"]
        obj, _ = find_object(e["sha256"], e["size"], primary, cache)
        if obj is None:
            miss_store += 1
            log(f"store-missing   {e['repo_path']}")
        if not dest.is_file():
            miss_local += 1
            log(f"local-missing   {e['repo_path']}")
        elif dest.stat().st_size != e["size"] or sha256_of(dest) != e["sha256"]:
            mismatched += 1
            log(f"mismatched      {e['repo_path']}")
        elif obj is not None:
            ok += 1

    known = {e["repo_path"] for e in m["assets"]}
    unpublished = 0
    for d in MANAGED_DIRS:
        root = REPO_ROOT / d
        if not root.is_dir():
            continue
        for f in sorted(root.rglob("*")):
            if f.is_file() and repo_rel(f) not in known:
                unpublished += 1
                log(f"local-only      {repo_rel(f)}   (publish it: python tools/asset_store.py publish \"{repo_rel(f)}\")")

    log(f"\nstatus: {len(m['assets'])} manifest entries — {ok} ok, {miss_local} missing locally, "
        f"{mismatched} mismatched, {miss_store} missing in store; {unpublished} unpublished local file(s).")
    return 0


# ------------------------------------------------------------------- main ----

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="asset_store.py", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("publish", help="hash + upload file(s)/dir(s) into the store, update the manifest")
    p.add_argument("files", nargs="+")
    p.add_argument("--note", default="", help="source_note recorded in the manifest (provenance)")
    p.set_defaults(fn=cmd_publish)

    f = sub.add_parser("fetch", help="restore manifest assets that are missing/mismatched locally")
    f.add_argument("--all", action="store_true")
    f.add_argument("paths", nargs="*")
    f.set_defaults(fn=cmd_fetch)

    sub.add_parser("verify", help="CI check: store object present + local hash matches, for every entry").set_defaults(fn=cmd_verify)
    sub.add_parser("status", help="report ok/missing/mismatched/unpublished").set_defaults(fn=cmd_status)

    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
