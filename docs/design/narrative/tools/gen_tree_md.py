#!/usr/bin/env python3
"""Generate a readable .md script for each x3.chattree/1 file in this pack, mirroring the
existing chat_trees/*.md review style (speaker, line, choices, gates, effects). DERIVED
from the JSON so the script always matches the data.
Usage: python gen_tree_md.py [chat_trees_dir] [file1.json file2.json ...]
"""
import json, sys
from pathlib import Path

# only (re)generate scripts for THIS pack's new trees
PACK = {"kthara", "nordic_steward", "mantis_arbiter", "quartermaster",
        "club1127_vesper_act2", "act2_reyes_club"}


def fmt_ops(ops):
    if not ops:
        return ""
    parts = []
    for o in ops:
        for k, v in o.items():
            if k == "args":
                continue
            if k in ("fire", "set", "clear", "give", "take"):
                parts.append(f"{k} `{v}`")
            elif k == "flag":
                parts.append(f"`{v}`")
            elif k == "follow":
                parts.append("**follow**")
            elif k == "ally":
                parts.append("**+1 ally**")
            elif k in ("any", "not"):
                parts.append(k)
            elif k == "rel":
                parts.append(f"rel[{v[0]}]={v[1]}")
            elif k == "rel_gte":
                parts.append(f"rel[{v[0]}]>={v[1]}")
            elif k == "girl_saved":
                parts.append(f"saved:{v}")
            elif k == "girl_lost":
                parts.append(f"lost:{v}")
            elif k == "timeline":
                parts.append(f"timeline in {v}")
            elif k == "item":
                parts.append(f"has `{v}`")
            elif isinstance(v, (int, float)):
                sign = "+" if v >= 0 else ""
                parts.append(f"{k} {sign}{v}")
            else:
                parts.append(f"{k} {v}")
    return ", ".join(parts)


def gen(path):
    d = json.loads(path.read_text(encoding="utf-8"))
    L = [f"# Chat tree — {d.get('display', d['npc'])} (`{d['npc']}`)\n"]
    if d.get("_bio"):
        L.append(f"> {d['_bio']}\n")
    for tname, tree in d.get("trees", {}).items():
        L.append(f"## Tree: `{tname}`")
        if tree.get("_use"):
            L.append(f"*{tree['_use']}*")
        if tree.get("_scene"):
            L.append(f"*{tree['_scene']}*")
        L.append("")
        if "pool" in tree:
            L.append("Banter pool (weighted, `if`-filtered):\n")
            for e in tree["pool"]:
                g = fmt_ops(e.get("if"))
                g = f"  _[{g}]_" if g else ""
                L.append(f"- \"{e['line']}\"{g}")
            L.append("")
            continue
        L.append(f"_start:_ `{tree.get('start')}`\n")
        for n in tree.get("nodes", []):
            spk = n.get("speaker", d.get("display", d["npc"]))
            head = f"**`{n['id']}` — {spk}:**"
            L.append(head)
            if n.get("if"):
                alt = f" (else → `{n.get('else')}`)" if n.get("else") else ""
                L.append(f"  - _gate:_ {fmt_ops(n['if'])}{alt}")
            L.append(f"  - \"{n.get('line','')}\"")
            if n.get("fx"):
                L.append(f"  - _fx:_ {fmt_ops(n['fx'])}")
            for c in n.get("choices", []):
                g = fmt_ops(c.get("if"))
                g = f" _[{g}]_" if g else ""
                fx = fmt_ops(c.get("fx"))
                fx = f" → {fx}" if fx else ""
                L.append(f"    - **>** \"{c['text']}\"{g} → `{c.get('next')}`{fx}")
            if n.get("next") and not n.get("choices"):
                L.append(f"  - → `{n['next']}`")
            L.append("")
    return "\n".join(L)


def main():
    args = sys.argv[1:]
    d = Path(args[0]) if args and Path(args[0]).is_dir() else Path(__file__).parent.parent / "chat_trees"
    files = [Path(a) for a in args if a.endswith(".json")]
    if not files:
        files = [f for f in sorted(d.glob("*.json")) if f.stem in PACK]
    for f in files:
        out = f.with_suffix(".md")
        out.write_text(gen(f), encoding="utf-8")
        print(f"wrote {out.name}")


if __name__ == "__main__":
    main()
