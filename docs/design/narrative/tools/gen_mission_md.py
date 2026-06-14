#!/usr/bin/env python3
"""Generate a readable .md script for each x3.mission/1 file, so a human can review the
beats without parsing JSON. The MD is DERIVED from the JSON (objectives, edges, gates,
fx) plus the authored `_hooks`/`_voice` prose, guaranteeing the script matches the data.
Usage: python gen_mission_md.py [missions_dir]   (skips act2_first_light.md — hand-authored)
"""
import json, sys
from pathlib import Path

SKIP = {"act2_first_light"}  # hand-authored exemplar


def fmt_ops(ops):
    if not ops:
        return ""
    parts = []
    for o in ops:
        for k, v in o.items():
            if k == "args":
                continue
            if k == "fire":
                parts.append(f"fire `{v}`")
            elif k == "set":
                parts.append(f"set `{v}`")
            elif k == "clear":
                parts.append(f"clear `{v}`")
            elif k == "flag":
                parts.append(f"`{v}` set")
            elif k in ("give", "take"):
                parts.append(f"{k} `{v}`")
            elif k == "follow":
                parts.append("she follows (companion)")
            elif k == "ally":
                parts.append("+1 alliance")
            elif k in ("any", "not"):
                parts.append(k)
            elif k == "counter":
                parts.append(f"counter `{v[0]}` >= {v[1]}")
            elif k == "counter_lt":
                parts.append(f"counter `{v[0]}` < {v[1]}")
            elif isinstance(v, list):
                parts.append(f"{k} {v}")
            else:
                sign = "+" if isinstance(v, (int, float)) and v >= 0 else ""
                parts.append(f"{k} {sign}{v}")
    return ", ".join(parts)


def gen(path):
    d = json.loads(path.read_text(encoding="utf-8"))
    L = []
    lvl = d.get("level")
    lvl_s = "/".join(str(x) for x in lvl) if isinstance(lvl, list) else str(lvl)
    L.append(f"# Mission — {d['title']} (`{d['id']}`)\n")
    L.append(f"**Act {d.get('act')} · Level {lvl_s} · {d.get('location','')}**\n")
    L.append(f"> {d.get('summary','')}\n")
    give = d.get("give")
    if give:
        L.append(f"**Offered when:** {fmt_ops(give)}\n")
    L.append(f"**Starts at:** `{d['start']}`\n")
    L.append("## Beats (objective graph)\n")
    for o in d.get("objectives", []):
        oid = o["id"]
        L.append(f"### `{oid}` — {o.get('text','')}")
        if o.get("if"):
            L.append(f"- *gate:* {fmt_ops(o['if'])} (else → `{o.get('else', o.get('next'))}`)")
        if o.get("on_enter"):
            L.append(f"- *on enter:* {fmt_ops(o['on_enter'])}")
        if o.get("timer"):
            L.append(f"- *timer:* {o['timer'].get('seconds')}s → `{o['timer'].get('on_expire')}`")
        if o.get("complete_when"):
            L.append(f"- *complete when:* {fmt_ops(o['complete_when'])}")
        if o.get("fx"):
            L.append(f"- *on complete:* {fmt_ops(o['fx'])}")
        for b in o.get("branch", []):
            extra = f" — {fmt_ops(b['fx'])}" if b.get("fx") else ""
            L.append(f"- *branch:* if {fmt_ops(b.get('if'))} → `{b.get('next')}`{extra}")
        if o.get("fail"):
            f = o["fail"]
            extra = f" — {fmt_ops(f.get('fx'))}" if f.get("fx") else ""
            L.append(f"- *fail:* when {fmt_ops(f.get('when'))} → `{f.get('next')}`{extra}")
        nxt = o.get("next")
        if nxt:
            L.append(f"- *then:* → `{nxt}`")
        L.append("")
    if d.get("_hooks"):
        L.append("## Hooks into canon / existing branches\n")
        L.append(d["_hooks"] + "\n")
    if d.get("_voice"):
        L.append("## Voice note\n")
        L.append(d["_voice"] + "\n")
    return "\n".join(L)


def main():
    mdir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent.parent / "missions"
    n = 0
    for f in sorted(mdir.glob("*.json")):
        if f.stem in SKIP:
            continue
        out = f.with_suffix(".md")
        out.write_text(gen(f), encoding="utf-8")
        n += 1
        print(f"wrote {out.name}")
    print(f"{n} mission scripts generated")


if __name__ == "__main__":
    main()
