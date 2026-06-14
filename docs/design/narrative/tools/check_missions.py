#!/usr/bin/env python3
"""Validate x3.mission/1 files (and, with --trees, x3.chattree/1 files too):
  * JSON parses; format field correct.
  * Objective ids unique; start + every next/else/branch.next/fail.next/timer.on_expire
    resolves to an objective id, or one of the keywords: end, fail, retry.
  * Every op used anywhere in if / complete_when / fail.when / branch[].if / on_enter / fx
    is in the known story_ops vocabulary (NPC_CHAT_TREE_FORMAT.md §3) plus the structural
    mission ops (counter/counter_lt/counter_set/counter_add). A typo'd op is an error.
Usage: python check_missions.py [missions_dir] [--trees chat_trees_dir]
"""
import json, sys
from pathlib import Path

# --- story_ops vocabulary (from NPC_CHAT_TREE_FORMAT.md §3) ---
COND_OPS = {
    "flag", "karma_gte", "karma_lte",
    "humanity_gte", "love_gte", "trust_gte", "mercy_gte", "redemption_gte",
    "timeline", "girl_saved", "girl_lost", "item", "rel_gte", "chance", "lua",
    # structural (mission only, non-moral)
    "counter", "counter_lt",
    # composition
    "any", "not",
}
FX_OPS = {
    "karma", "humanity", "love", "trust", "mercy", "redemption",
    "set", "clear", "fire", "give", "take", "follow", "rel", "ally", "end",
    # structural (mission only, non-moral)
    "counter_set", "counter_add",
}
KEYWORDS = {"end", "fail", "retry"}


def check_ops(where, items, allowed, errors, ctx):
    """items is a list of {op: ...} dicts. Verify each key is an allowed op."""
    if items is None:
        return
    if not isinstance(items, list):
        errors.append(f"{ctx}: {where} must be a list")
        return
    for entry in items:
        if not isinstance(entry, dict):
            errors.append(f"{ctx}: {where} entry not an object: {entry!r}")
            continue
        for op, val in entry.items():
            # `args` is a companion payload key on the same object as `fire`
            # (e.g. {"fire": "event", "args": {...}}), per NPC_CHAT_TREE_FORMAT.
            # It is not itself an op.
            if op == "args":
                continue
            if op not in allowed:
                errors.append(f"{ctx}: unknown {where} op '{op}'")
            if op == "any" and isinstance(val, list):
                check_ops(where, val, allowed, errors, ctx)
            if op == "not" and isinstance(val, dict):
                check_ops(where, [val], allowed, errors, ctx)


def check_mission(path):
    errors = []
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception as e:
        return [f"JSON parse error: {e}"]
    if data.get("format") != "x3.mission/1":
        errors.append(f"bad format field: {data.get('format')!r}")
    objs = data.get("objectives", [])
    ids = {o["id"] for o in objs if "id" in o}
    if len(ids) != len(objs):
        errors.append(f"{len(objs) - len(ids)} duplicate/missing objective id(s)")

    def ref(oid, kind, target):
        if target is not None and target not in KEYWORDS and target not in ids:
            errors.append(f"{oid}: dangling {kind} -> '{target}'")

    start = data.get("start")
    if start not in ids:
        errors.append(f"start '{start}' not found")
    check_ops("give", data.get("give"), COND_OPS, errors, data.get("id", path.name))

    for o in objs:
        oid = o.get("id", "?")
        ref(oid, "next", o.get("next"))
        ref(oid, "else", o.get("else"))
        check_ops("if", o.get("if"), COND_OPS, errors, oid)
        check_ops("complete_when", o.get("complete_when"), COND_OPS, errors, oid)
        check_ops("on_enter", o.get("on_enter"), FX_OPS, errors, oid)
        check_ops("fx", o.get("fx"), FX_OPS, errors, oid)
        for b in o.get("branch", []):
            check_ops("branch.if", b.get("if"), COND_OPS, errors, oid)
            ref(oid, "branch.next", b.get("next"))
        fail = o.get("fail")
        if fail:
            check_ops("fail.when", fail.get("when"), COND_OPS, errors, oid)
            check_ops("fail.fx", fail.get("fx"), FX_OPS, errors, oid)
            ref(oid, "fail.next", fail.get("next"))
        timer = o.get("timer")
        if timer:
            ref(oid, "timer.on_expire", timer.get("on_expire"))
    return errors


def check_tree(path):
    """Light op-vocabulary check for chat trees (structure is check_chattrees.py's job)."""
    errors = []
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception as e:
        return [f"JSON parse error: {e}"]
    if data.get("format") != "x3.chattree/1":
        errors.append(f"bad format field: {data.get('format')!r}")
    for tname, tree in data.get("trees", {}).items():
        entries = tree.get("nodes", []) + tree.get("pool", [])
        for n in entries:
            check_ops("if", n.get("if"), COND_OPS, errors, f"{tname}/{n.get('id', '?')}")
            check_ops("fx", n.get("fx"), FX_OPS, errors, f"{tname}/{n.get('id', '?')}")
            for c in n.get("choices", []):
                check_ops("if", c.get("if"), COND_OPS, errors, f"{tname}/{n.get('id', '?')}/choice")
                check_ops("fx", c.get("fx"), FX_OPS, errors, f"{tname}/{n.get('id', '?')}/choice")
    return errors


def run(d, checker, label):
    files = sorted(d.glob("*.json"))
    bad = 0
    for f in files:
        errs = checker(f)
        if errs:
            bad += 1
            print(f"FAIL {f.name}")
            for e in errs:
                print(f"     {e}")
        else:
            print(f"OK   {f.name}")
    print(f"{len(files) - bad}/{len(files)} {label} valid\n")
    return bad


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    here = Path(__file__).parent.parent
    mdir = Path(args[0]) if args else here / "missions"
    bad = run(mdir, check_mission, "missions")
    if "--trees" in sys.argv:
        i = sys.argv.index("--trees")
        tdir = Path(sys.argv[i + 1]) if i + 1 < len(sys.argv) else here / "chat_trees"
        bad += run(tdir, check_tree, "trees (op-vocab)")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
