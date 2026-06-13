#!/usr/bin/env python3
"""Validate x3.chattree/1 files: JSON parses, every tree's `start`, node `next`,
node `else`, and choice `next` resolve to a node id in the same tree (or "end").
Usage: python check_chattrees.py [dir]   (default: ../chat_trees relative to this file)
"""
import json, sys
from pathlib import Path

def check_file(path):
    errors = []
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception as e:
        return [f"JSON parse error: {e}"]
    if data.get("format") != "x3.chattree/1":
        errors.append(f"bad format field: {data.get('format')!r}")
    for tname, tree in data.get("trees", {}).items():
        if "pool" in tree:  # banter pools have no refs
            continue
        nodes = tree.get("nodes", [])
        ids = {n["id"] for n in nodes}
        dupes = len(nodes) - len(ids)
        if dupes:
            errors.append(f"{tname}: {dupes} duplicate node id(s)")
        start = tree.get("start")
        if start not in ids:
            errors.append(f"{tname}: start '{start}' not found")
        def ref(node_id, kind, target):
            if target is not None and target != "end" and target not in ids:
                errors.append(f"{tname}/{node_id}: dangling {kind} -> '{target}'")
        for n in nodes:
            ref(n["id"], "next", n.get("next"))
            ref(n["id"], "else", n.get("else"))
            for c in n.get("choices", []):
                ref(n["id"], "choice next", c.get("next"))
    return errors

def main():
    d = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent.parent / "chat_trees"
    files = sorted(d.glob("*.json"))
    bad = 0
    for f in files:
        errs = check_file(f)
        if errs:
            bad += 1
            print(f"FAIL {f.name}")
            for e in errs:
                print(f"     {e}")
        else:
            print(f"OK   {f.name}")
    print(f"\n{len(files) - bad}/{len(files)} trees valid")
    sys.exit(1 if bad else 0)

if __name__ == "__main__":
    main()
