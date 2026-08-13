#!/usr/bin/env python3
"""Generate docs/dashboard.html — a single self-contained, local status page.

Why this exists: lane/slice/status information is scattered across git branches,
two design docs (CITY_BORES_PLAN.md, TUNNEL_NEXT.md) and screenshot folders, and
the only way to get a rundown was to ask an agent. This script DERIVES everything
it can from the repo at generation time — nothing on the page is hand-typed —
so re-running it refreshes the page and it cannot rot silently:

    python tools/gen_dashboard.py            # writes docs/dashboard.html
    python tools/gen_dashboard.py --out X    # writes elsewhere

Derived data:
  * Branch/lane status  — git branch -r (origin/inspx/*, origin/integration/*),
    plus local inspx/* / integration/* lane branches; ahead/behind counts vs
    origin/main, merged-ness, last commit date + subject.
  * Acceptance conditions — parsed from docs/design/CITY_BORES_PLAN.md
    ("Acceptance conditions" checkbox groups).
  * Defect list — parsed from docs/design/TUNNEL_NEXT.md section 4.
  * Open work — section headings + first sentence from both docs.
  * Contradiction checks — docs that exist only on unmerged branches, and
    commit SHAs the docs cite as DONE/FIXED that are not on origin/main.
  * Screenshot progressions — walk of docs/screenshots/ with DEFECT_/BEFORE/
    MID/AFTER pairing by filename convention; per-dir README first paragraph.

If a source is missing or unparseable the page says "not available" for that
section; it never invents a number. If a doc is absent from the working tree it
is read from the unmerged branch that carries it (via `git show`) and that fact
is surfaced as a warning — the docs the plan depends on are not on main yet.

The output is one .html file: inline CSS, no JS, no CDN, no external fonts.
Images are relative references into docs/screenshots/ so the page works opened
as file:// from the repo. Dark theme.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import html
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_OUT = REPO_ROOT / "docs" / "dashboard.html"

CITY_BORES = "docs/design/CITY_BORES_PLAN.md"
TUNNEL_NEXT = "docs/design/TUNNEL_NEXT.md"
SCREENSHOT_DIR = "docs/screenshots"

IMG_EXT = {".png", ".jpg", ".jpeg", ".gif", ".webp"}


# --------------------------------------------------------------------------- git

def run_git(*args: str) -> str | None:
    """Run git in the repo root; return stdout or None on failure."""
    try:
        cp = subprocess.run(
            ["git", "-C", str(REPO_ROOT), *args],
            capture_output=True, text=True, encoding="utf-8", errors="replace",
            timeout=60,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if cp.returncode != 0:
        return None
    return cp.stdout


def git_lines(*args: str) -> list[str]:
    out = run_git(*args)
    if out is None:
        return []
    return [ln for ln in out.splitlines() if ln.strip()]


def collect_branches() -> tuple[list[dict], bool]:
    """All origin/inspx/*, origin/integration/* branches plus local-only lanes.

    Returns (branches, git_ok). Each branch dict: name, local_only, ahead,
    behind, merged, date, subject. ahead/behind may be None if git failed.
    """
    remote = git_lines("branch", "-r", "--format=%(refname:short)")
    if not remote and run_git("rev-parse", "--git-dir") is None:
        return [], False

    names: list[tuple[str, bool]] = []  # (ref, local_only)
    remote_short = set()
    for r in remote:
        if r.startswith(("origin/inspx/", "origin/integration/")):
            names.append((r, False))
            remote_short.add(r[len("origin/"):])
    for l in git_lines("branch", "--format=%(refname:short)"):
        if l.startswith(("inspx/", "integration/")) and l not in remote_short:
            names.append((l, True))

    branches = []
    for ref, local_only in names:
        ahead_s = run_git("rev-list", "--count", f"origin/main..{ref}")
        behind_s = run_git("rev-list", "--count", f"{ref}..origin/main")
        merged = subprocess.run(
            ["git", "-C", str(REPO_ROOT), "merge-base", "--is-ancestor",
             ref, "origin/main"],
            capture_output=True,
        ).returncode == 0
        last = run_git("log", "-1", "--format=%as\x1f%s", ref) or ""
        date, _, subject = last.strip().partition("\x1f")
        branches.append({
            "name": ref,
            "local_only": local_only,
            "ahead": int(ahead_s) if ahead_s and ahead_s.strip().isdigit() else None,
            "behind": int(behind_s) if behind_s and behind_s.strip().isdigit() else None,
            "merged": merged,
            "date": date or "?",
            "subject": subject or "?",
        })
    return branches, True


# --------------------------------------------------------------------- doc load

def load_doc(relpath: str, fallback_branches: list[str]) -> tuple[str | None, str | None]:
    """Return (text, source). Prefer the working tree; else `git show` the
    first unmerged branch that carries the file. source is 'working tree' or
    the branch name; (None, None) if the doc is nowhere to be found."""
    p = REPO_ROOT / relpath
    if p.is_file():
        try:
            return p.read_text(encoding="utf-8", errors="replace"), "working tree"
        except OSError:
            pass
    for br in fallback_branches:
        out = run_git("show", f"{br}:{relpath}")
        if out is not None:
            return out, br
    return None, None


# -------------------------------------------------------------------- md parse

CHECKBOX_RE = re.compile(r"^- \[( |x|X)\]\s?(.*)$")
HEADING_RE = re.compile(r"^(#{1,4})\s+(.*)$")


def parse_checkbox_groups(md: str, start_heading: str | None = None,
                          stop_level: int = 2) -> list[dict]:
    """Parse `- [ ]`/`- [x]` items grouped under headings.

    If start_heading is given, only the region from that heading until the next
    heading of level <= stop_level is scanned. Continuation lines (indented)
    are folded into the item text. Returns [{group, items:[{done,text}]}].
    """
    lines = md.splitlines()
    lo, hi = 0, len(lines)
    if start_heading is not None:
        lo = None
        for i, ln in enumerate(lines):
            m = HEADING_RE.match(ln)
            if m and start_heading.lower() in m.group(2).lower():
                lo = i + 1
                for j in range(i + 1, len(lines)):
                    m2 = HEADING_RE.match(lines[j])
                    if m2 and len(m2.group(1)) <= stop_level:
                        hi = j
                        break
                break
        if lo is None:
            return []

    groups: list[dict] = []
    cur_group = None
    cur_item = None

    def close_item():
        nonlocal cur_item
        if cur_item is not None and cur_group is not None:
            cur_item["text"] = re.sub(r"\s+", " ", cur_item["text"]).strip()
            cur_group["items"].append(cur_item)
        cur_item = None

    for ln in lines[lo:hi]:
        m = HEADING_RE.match(ln)
        if m:
            close_item()
            cur_group = {"group": m.group(2).strip(), "items": []}
            groups.append(cur_group)
            continue
        m = CHECKBOX_RE.match(ln)
        if m:
            close_item()
            if cur_group is None:
                cur_group = {"group": "", "items": []}
                groups.append(cur_group)
            cur_item = {"done": m.group(1).lower() == "x", "text": m.group(2)}
            continue
        if cur_item is not None and (ln.startswith("  ") or ln.startswith("\t")):
            cur_item["text"] += " " + ln.strip()
            continue
        close_item()
    close_item()
    return [g for g in groups if g["items"]]


def first_sentence(paragraph: str, max_len: int = 240) -> str:
    text = re.sub(r"\s+", " ", paragraph).strip()
    for m in re.finditer(r"\.\s", text):
        if m.start() >= 40:
            text = text[: m.start() + 1]
            break
    if len(text) > max_len:
        text = text[: max_len - 1].rstrip() + "…"
    return text


def parse_sections(md: str, level: int = 2) -> list[dict]:
    """Return [{title, summary}] for every heading of `level`, summary being
    the first sentence of the first paragraph beneath it."""
    lines = md.splitlines()
    out: list[dict] = []
    i = 0
    while i < len(lines):
        m = HEADING_RE.match(lines[i])
        if m and len(m.group(1)) == level:
            title = m.group(2).strip()
            para: list[str] = []
            j = i + 1
            while j < len(lines):
                ln = lines[j]
                if HEADING_RE.match(ln):
                    break
                s = ln.strip()
                if s in ("", "---"):
                    if para:
                        break
                    j += 1
                    continue
                para.append(s)
                j += 1
            out.append({"title": title,
                        "summary": first_sentence(" ".join(para)) if para else ""})
        i += 1
    return out


SHA_RE = re.compile(r"\b[0-9a-f]{8,40}\b")


def sha_mentions(md: str) -> list[dict]:
    """Commit hashes cited in a doc, resolved against git; flags lines that
    claim DONE/FIXED for a commit that is not on origin/main."""
    seen: set[str] = set()
    out: list[dict] = []
    for ln in md.splitlines():
        for m in SHA_RE.finditer(ln):
            sha = m.group(0)
            if sha in seen:
                continue
            seen.add(sha)
            subj = run_git("log", "-1", "--format=%s", sha)
            if subj is None:
                continue  # not a commit in this repo; skip, don't invent
            merged = subprocess.run(
                ["git", "-C", str(REPO_ROOT), "merge-base", "--is-ancestor",
                 sha, "origin/main"],
                capture_output=True,
            ).returncode == 0
            snippet = re.sub(r"\s+", " ", ln).strip()
            if len(snippet) > 140:
                snippet = snippet[:139] + "…"
            claims_done = bool(re.search(r"(?i)\b(done|fixed|deleted|resolved)\b", ln))
            out.append({"sha": sha, "subject": subj.strip(), "merged": merged,
                        "line": snippet, "claims_done": claims_done})
    return out


# ------------------------------------------------------------------ screenshots

MARK_KINDS = ("defect", "before", "mid", "after")


def classify_shot(stem: str) -> tuple[str, str, str]:
    """Return (kind, base, desc) for a filename stem."""
    if stem.startswith("DEFECT_"):
        return "defect", stem[len("DEFECT_"):], ""
    for kind, tag in (("before", "_BEFORE"), ("mid", "_MID"), ("after", "_AFTER")):
        idx = stem.upper().find(tag)
        if idx > 0:
            base = stem[:idx]
            desc = stem[idx + len(tag):].lstrip("_")
            return kind, base, desc
    return "plain", stem, ""


def scan_screenshots() -> list[dict]:
    """Walk docs/screenshots/. Returns per-directory dicts:
    {name, count, readme (first paragraph or None), readme_path, groups, others}
    where groups are progression sets and others a small sample of plain shots."""
    root = REPO_ROOT / SCREENSHOT_DIR
    if not root.is_dir():
        return []
    dirs = []
    entries = sorted(root.iterdir(), key=lambda p: p.name.lower())
    # Treat loose top-level images as a pseudo-directory.
    loose = [p for p in entries if p.is_file() and p.suffix.lower() in IMG_EXT]
    subdirs = [p for p in entries if p.is_dir()]
    if loose:
        subdirs.insert(0, root)  # handled specially below

    for d in subdirs:
        if d == root:
            images = sorted(loose, key=lambda p: p.name.lower())
            name = "(top level)"
            readme_file = root / "README.md"
        else:
            images = sorted((p for p in d.iterdir()
                             if p.is_file() and p.suffix.lower() in IMG_EXT),
                            key=lambda p: p.name.lower())
            name = d.name
            readme_file = d / "README.md"
        if not images and not readme_file.is_file():
            continue

        readme = None
        readme_rel = None
        if readme_file.is_file():
            try:
                txt = readme_file.read_text(encoding="utf-8", errors="replace")
                para: list[str] = []
                for ln in txt.splitlines():
                    s = ln.strip()
                    if s.startswith("#") or s == "":
                        if para:
                            break
                        continue
                    para.append(s)
                if para:
                    readme = first_sentence(" ".join(para), 280)
                readme_rel = readme_file.relative_to(REPO_ROOT).as_posix()
            except OSError:
                pass

        by_stem = {p.stem: p for p in images}
        classified = {p.stem: classify_shot(p.stem) for p in images}
        plains = {s for s, (k, _, _) in classified.items() if k == "plain"}

        groups: dict[str, dict] = {}
        used: set[str] = set()
        for stem, (kind, base, desc) in classified.items():
            if kind == "plain":
                continue
            g = groups.setdefault(base, {"base": base, "shots": []})
            g["shots"].append((kind, stem))
            used.add(stem)
        # Attach the fixed/current frame to each group by name convention.
        for base, g in groups.items():
            kinds = {k for k, _ in g["shots"]}
            if "after" in kinds:
                continue
            descs = [classified[s][2] for _, s in g["shots"] if classified[s][2]]
            cand = None
            for dsc in descs:
                if f"{base}_{dsc}" in plains:
                    cand = f"{base}_{dsc}"
                    break
            if cand is None and base in plains:
                cand = base
            if cand is None:
                prefixed = [s for s in plains if s.startswith(base + "_")]
                if len(prefixed) == 1:
                    cand = prefixed[0]
            if cand is not None:
                g["shots"].append(("fixed", cand))
                used.add(cand)

        order = {"defect": 0, "before": 1, "mid": 2, "after": 3, "fixed": 4}
        glist = []
        for base in sorted(groups):
            shots = sorted(groups[base]["shots"], key=lambda t: order[t[0]])
            glist.append({
                "base": base,
                "shots": [{
                    "kind": k,
                    "file": by_stem[s].relative_to(REPO_ROOT).as_posix(),
                    "name": by_stem[s].name,
                } for k, s in shots],
            })

        others = [by_stem[s] for s in sorted(by_stem) if s not in used]
        dirs.append({
            "name": name,
            "count": len(images),
            "readme": readme,
            "readme_rel": readme_rel,
            "groups": glist,
            "others": [p.relative_to(REPO_ROOT).as_posix() for p in others],
        })
    return dirs


# ----------------------------------------------------------------------- html

CSS = """
:root {
  --bg: #0e1216; --surface: #171d24; --surface2: #1e2630;
  --ink: #e6edf3; --muted: #94a3b1; --line: #2b3640;
  --accent: #58a6ff; --accent-dim: #274a75;
  --good: #3fb950; --warn: #d29922; --bad: #f85149;
}
* { box-sizing: border-box; }
body {
  margin: 0; background: var(--bg); color: var(--ink);
  font: 15px/1.5 "Segoe UI", system-ui, -apple-system, sans-serif;
}
main { max-width: 1200px; margin: 0 auto; padding: 24px 28px 80px; }
h1 { font-size: 26px; margin: 0 0 4px; }
h2 { font-size: 19px; margin: 40px 0 12px; border-bottom: 1px solid var(--line);
     padding-bottom: 6px; }
h3 { font-size: 15px; margin: 18px 0 8px; color: var(--ink); }
a { color: var(--accent); text-decoration: none; }
a:hover { text-decoration: underline; }
.meta { color: var(--muted); font-size: 13px; }
code, .mono { font-family: Consolas, "Cascadia Mono", monospace; font-size: 13px; }

.tiles { display: flex; flex-wrap: wrap; gap: 12px; margin: 18px 0; }
.tile { background: var(--surface); border: 1px solid var(--line);
        border-radius: 8px; padding: 12px 18px; min-width: 150px; }
.tile .num { font-size: 28px; font-weight: 600; line-height: 1.1; }
.tile .lbl { color: var(--muted); font-size: 12.5px; margin-top: 2px; }
.tile.warn .num { color: var(--warn); }

.scroll { overflow-x: auto; background: var(--surface); border: 1px solid var(--line);
          border-radius: 8px; }
table { border-collapse: collapse; width: 100%; font-size: 13.5px; }
th, td { text-align: left; padding: 7px 12px; border-top: 1px solid var(--line);
         white-space: nowrap; vertical-align: top; }
thead th { border-top: none; color: var(--muted); font-weight: 600; font-size: 12.5px; }
td.subj { white-space: normal; min-width: 260px; color: var(--muted); }
tr:hover td { background: var(--surface2); }

.badge { display: inline-block; padding: 1px 8px; border-radius: 10px;
         font-size: 11.5px; font-weight: 600; border: 1px solid; }
.badge.unmerged { color: var(--warn); border-color: var(--warn); }
.badge.merged { color: var(--good); border-color: var(--good); }
.badge.local { color: var(--accent); border-color: var(--accent-dim); }
.badge.defect { color: var(--bad); border-color: var(--bad); }
.badge.kind { color: var(--muted); border-color: var(--line); }

.barwrap { display: inline-block; width: 140px; height: 10px;
           background: var(--surface2); border-radius: 4px; vertical-align: middle;
           margin-right: 8px; }
.bar { height: 10px; background: var(--accent); border-radius: 4px 0 0 4px; }

.panel { background: var(--surface); border: 1px solid var(--line);
         border-radius: 8px; padding: 14px 18px; margin: 10px 0; }
.panel.alert { border-left: 4px solid var(--warn); }
.two-col { display: grid; grid-template-columns: 1fr 1fr; gap: 18px; }
@media (max-width: 900px) { .two-col { grid-template-columns: 1fr; } }

ul.checks { list-style: none; padding: 0; margin: 6px 0; }
ul.checks li { padding: 4px 0 4px 26px; position: relative; font-size: 13.5px; }
ul.checks li::before { position: absolute; left: 2px; font-family: Consolas, monospace; }
ul.checks li.done { color: var(--muted); }
ul.checks li.done::before { content: "[x]"; color: var(--good); }
ul.checks li.todo::before { content: "[ ]"; color: var(--warn); }
.gcount { color: var(--muted); font-weight: 400; font-size: 12.5px; }

.openwork li { margin: 6px 0; }
.openwork .sum { color: var(--muted); font-size: 13.5px; }

details { margin: 10px 0; }
summary { cursor: pointer; color: var(--muted); font-size: 13.5px; }
.shotdir { margin: 14px 0; }
.shotrow { display: flex; flex-wrap: wrap; gap: 10px; margin: 8px 0; }
figure { margin: 0; background: var(--surface); border: 1px solid var(--line);
         border-radius: 8px; padding: 6px; max-width: 300px; }
figure img { max-width: 100%; display: block; border-radius: 4px; }
figcaption { font-size: 11.5px; color: var(--muted); padding: 4px 2px 0;
             word-break: break-all; }
.na { color: var(--muted); font-style: italic; }
footer { margin-top: 48px; color: var(--muted); font-size: 12.5px;
         border-top: 1px solid var(--line); padding-top: 12px; }
"""


def esc(s: str) -> str:
    return html.escape(s, quote=True)


def md_inline(s: str) -> str:
    """Escape, then lightly render **bold** and `code`."""
    s = esc(s)
    s = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", s)
    s = re.sub(r"`([^`]+)`", r"<code>\1</code>", s)
    return s


def rel_from_out(repo_rel: str, out_path: Path) -> str:
    """Path of a repo file relative to the output html's directory."""
    target = REPO_ROOT / repo_rel
    try:
        rel = target.relative_to(out_path.parent)
        return rel.as_posix()
    except ValueError:
        # out sits deeper/elsewhere; fall back to walking up, and to an
        # absolute file:// URI when out is on a different drive entirely.
        import os
        try:
            return Path(os.path.relpath(target, out_path.parent)).as_posix()
        except ValueError:
            return target.resolve().as_uri()


def render(out_path: Path) -> str:
    now = _dt.datetime.now().strftime("%Y-%m-%d %H:%M")
    head_branch = (run_git("rev-parse", "--abbrev-ref", "HEAD") or "?").strip()
    head_sha = (run_git("rev-parse", "--short", "HEAD") or "?").strip()
    main_sha = (run_git("rev-parse", "--short", "origin/main") or "?").strip()
    main_date = (run_git("log", "-1", "--format=%as", "origin/main") or "?").strip()

    branches, git_ok = collect_branches()
    unmerged = [b for b in branches if not b["merged"]]
    merged = [b for b in branches if b["merged"]]
    unmerged.sort(key=lambda b: (-(b["ahead"] or 0), b["name"]))
    merged.sort(key=lambda b: b["date"], reverse=True)

    # Doc fallback order: unmerged origin lanes, newest last-commit first.
    fallback = [b["name"] for b in unmerged
                if b["name"].startswith("origin/") and (b["ahead"] or 0) > 0]
    fallback.sort(key=lambda n: next((b["date"] for b in unmerged if b["name"] == n), ""),
                  reverse=True)

    bores_md, bores_src = load_doc(CITY_BORES, fallback)
    tunnel_md, tunnel_src = load_doc(TUNNEL_NEXT, fallback)

    acceptance = parse_checkbox_groups(bores_md, "Acceptance conditions") if bores_md else []
    defects = []
    if tunnel_md:
        m = re.search(r"(?ms)^## 4\..*?(?=^## |\Z)", tunnel_md)
        if m:
            defects = parse_checkbox_groups(m.group(0))

    tunnel_sections = []
    if tunnel_md:
        tunnel_sections = [s for s in parse_sections(tunnel_md)
                           if re.match(r"\d+\.", s["title"])]
    bores_sections = parse_sections(bores_md) if bores_md else []

    warnings: list[str] = []
    if not git_ok:
        warnings.append("git was not available — branch data is <b>not available</b>.")
    for rel, src in ((CITY_BORES, bores_src), (TUNNEL_NEXT, tunnel_src)):
        if src and src != "working tree":
            warnings.append(
                f"<code>{esc(rel)}</code> is <b>not in this checkout / not on origin/main</b> — "
                f"read from unmerged branch <code>{esc(src)}</code>. The plan of record "
                f"currently lives only on that lane.")
    contradictions: list[dict] = []
    for md_text, docname in ((bores_md, CITY_BORES), (tunnel_md, TUNNEL_NEXT)):
        if not md_text:
            continue
        for men in sha_mentions(md_text):
            men["doc"] = docname
            if men["claims_done"] and not men["merged"]:
                contradictions.append(men)

    shots = scan_screenshots()

    # ---- assemble
    o: list[str] = []
    o.append("<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>")
    o.append("<meta name='viewport' content='width=device-width, initial-scale=1'>")
    o.append("<title>X3Native Dashboard</title>")
    o.append(f"<style>{CSS}</style></head><body><main>")

    o.append("<h1>X3Native — project dashboard</h1>")
    o.append(f"<p class='meta'>Generated {esc(now)} from <code>{esc(head_branch)}</code>@"
             f"<code>{esc(head_sha)}</code> · origin/main @ <code>{esc(main_sha)}</code> "
             f"({esc(main_date)}) · regenerate with "
             f"<code>python tools/gen_dashboard.py</code> — everything below is derived "
             f"from the repo at generation time.</p>")

    # ---- unmerged first: the recurring pain.
    o.append("<h2>Unmerged work — what is parked, and how much</h2>")
    if not git_ok:
        o.append("<p class='na'>not available (git failed)</p>")
    else:
        parked = sum(b["ahead"] or 0 for b in unmerged)
        biggest = unmerged[0] if unmerged else None
        o.append("<div class='tiles'>")
        o.append(f"<div class='tile warn'><div class='num'>{len(unmerged)}</div>"
                 "<div class='lbl'>unmerged lanes</div></div>")
        o.append(f"<div class='tile warn'><div class='num'>{parked}</div>"
                 "<div class='lbl'>commits parked off main</div></div>")
        if biggest:
            o.append(f"<div class='tile'><div class='num'>+{biggest['ahead']}</div>"
                     f"<div class='lbl'>largest: <span class='mono'>{esc(biggest['name'])}"
                     f"</span></div></div>")
        o.append(f"<div class='tile'><div class='num'>{len(merged)}</div>"
                 "<div class='lbl'>lanes already merged</div></div>")
        o.append("</div>")

        if unmerged:
            max_ahead = max((b["ahead"] or 0) for b in unmerged) or 1
            o.append("<div class='scroll'><table><thead><tr>"
                     "<th>branch</th><th>status</th><th>ahead of main</th>"
                     "<th>behind</th><th>last commit</th><th>subject</th>"
                     "</tr></thead><tbody>")
            for b in unmerged:
                ahead = b["ahead"]
                bar = ""
                if ahead is not None:
                    w = max(2, round(140 * ahead / max_ahead))
                    bar = (f"<span class='barwrap'><span class='bar' "
                           f"style='width:{w}px'></span></span>+{ahead}")
                else:
                    bar = "<span class='na'>not available</span>"
                loc = " <span class='badge local'>local only</span>" if b["local_only"] else ""
                behind = b["behind"] if b["behind"] is not None else "—"
                o.append(f"<tr><td class='mono'>{esc(b['name'])}{loc}</td>"
                         f"<td><span class='badge unmerged'>unmerged</span></td>"
                         f"<td>{bar}</td><td>{behind}</td><td>{esc(b['date'])}</td>"
                         f"<td class='subj'>{esc(b['subject'])}</td></tr>")
            o.append("</tbody></table></div>")
        else:
            o.append("<p>No unmerged inspx/integration branches. Clean slate.</p>")

        o.append(f"<details><summary>{len(merged)} merged branches (history)</summary>")
        o.append("<div class='scroll'><table><thead><tr><th>branch</th>"
                 "<th>status</th><th>last commit</th><th>subject</th></tr></thead><tbody>")
        for b in merged:
            o.append(f"<tr><td class='mono'>{esc(b['name'])}</td>"
                     f"<td><span class='badge merged'>merged</span></td>"
                     f"<td>{esc(b['date'])}</td><td class='subj'>{esc(b['subject'])}</td></tr>")
        o.append("</tbody></table></div></details>")

    # ---- warnings / contradictions
    if warnings or contradictions:
        o.append("<h2>Warnings — docs vs git</h2>")
        for w in warnings:
            o.append(f"<div class='panel alert'>{w}</div>")
        for c in contradictions:
            o.append(
                "<div class='panel alert'>"
                f"<b>Doc claims done, git says unmerged:</b> <code>{esc(c['doc'])}</code> "
                f"cites <code>{esc(c['sha'])}</code> (&ldquo;{esc(c['subject'])}&rdquo;) in a "
                f"line that reads as resolved, but that commit is <b>not on origin/main</b>."
                f"<br><span class='meta mono'>{esc(c['line'])}</span></div>")
    elif git_ok:
        o.append("<h2>Warnings — docs vs git</h2>")
        o.append("<p class='meta'>No contradictions detected between the parsed docs "
                 "and git at generation time.</p>")

    # ---- checklists
    o.append("<div class='two-col'>")

    o.append("<div><h2>Acceptance conditions — city bores</h2>")
    if acceptance:
        total = sum(len(g["items"]) for g in acceptance)
        done = sum(1 for g in acceptance for i in g["items"] if i["done"])
        src_note = "" if bores_src == "working tree" else f" · source: <code>{esc(bores_src or '?')}</code>"
        link = ""
        if bores_src == "working tree":
            link = f" · <a href='{esc(rel_from_out(CITY_BORES, out_path))}'>open doc</a>"
        o.append(f"<p class='meta'><b>{done}/{total}</b> conditions met · from "
                 f"<code>{esc(CITY_BORES)}</code>{src_note}{link}</p>")
        for g in acceptance:
            gd = sum(1 for i in g["items"] if i["done"])
            o.append(f"<h3>{md_inline(g['group'])} "
                     f"<span class='gcount'>{gd}/{len(g['items'])}</span></h3>")
            o.append("<ul class='checks'>")
            for it in g["items"]:
                cls = "done" if it["done"] else "todo"
                o.append(f"<li class='{cls}'>{md_inline(it['text'])}</li>")
            o.append("</ul>")
    else:
        o.append(f"<p class='na'>not available — could not read or parse "
                 f"{esc(CITY_BORES)}</p>")
    o.append("</div>")

    o.append("<div><h2>Tim's defect list — tunnels</h2>")
    if defects:
        total = sum(len(g["items"]) for g in defects)
        done = sum(1 for g in defects for i in g["items"] if i["done"])
        src_note = "" if tunnel_src == "working tree" else f" · source: <code>{esc(tunnel_src or '?')}</code>"
        link = ""
        if tunnel_src == "working tree":
            link = f" · <a href='{esc(rel_from_out(TUNNEL_NEXT, out_path))}'>open doc</a>"
        o.append(f"<p class='meta'><b>{done}/{total}</b> resolved · from "
                 f"<code>{esc(TUNNEL_NEXT)}</code> §4{src_note}{link}</p>")
        for g in defects:
            o.append("<ul class='checks'>")
            for it in g["items"]:
                cls = "done" if it["done"] else "todo"
                txt = it["text"]
                if len(txt) > 300:
                    txt = txt[:299].rstrip() + "…"
                o.append(f"<li class='{cls}'>{md_inline(txt)}</li>")
            o.append("</ul>")
    else:
        o.append(f"<p class='na'>not available — could not read or parse "
                 f"{esc(TUNNEL_NEXT)} section 4</p>")
    o.append("</div></div>")

    # ---- open work
    o.append("<h2>Open work — narrative sections</h2>")
    for title, rel, src, sections in (
        ("TUNNEL_NEXT.md", TUNNEL_NEXT, tunnel_src, tunnel_sections),
        ("CITY_BORES_PLAN.md", CITY_BORES, bores_src, bores_sections),
    ):
        o.append(f"<h3>{esc(title)}</h3>")
        if not sections:
            o.append("<p class='na'>not available</p>")
            continue
        if src == "working tree":
            o.append(f"<p class='meta'><a href='{esc(rel_from_out(rel, out_path))}'>"
                     f"open {esc(rel)}</a></p>")
        else:
            o.append(f"<p class='meta'>source: <code>{esc(src or '?')}</code> "
                     "(not in this checkout — no local link)</p>")
        o.append("<ul class='openwork'>")
        for s in sections:
            o.append(f"<li><b>{md_inline(s['title'])}</b>"
                     + (f"<div class='sum'>{md_inline(s['summary'])}</div>" if s["summary"] else "")
                     + "</li>")
        o.append("</ul>")

    # ---- screenshots
    o.append("<h2>Screenshot progression</h2>")
    if not shots:
        o.append(f"<p class='na'>not available — {esc(SCREENSHOT_DIR)} not found</p>")
    else:
        o.append("<p class='meta'>Walked from <code>docs/screenshots/</code>. "
                 "<span class='badge defect'>DEFECT</span> frames are deliberately "
                 "kept &ldquo;this was wrong&rdquo; records paired with their fixed "
                 "versions — evidence, not failures. Directories without progression "
                 "pairs are listed compactly.</p>")
        rich = [d for d in shots if d["groups"] or d["readme"]]
        plainlist = [d for d in shots if not d["groups"] and not d["readme"]]
        for d in rich:
            o.append(f"<div class='shotdir'><h3>{esc(d['name'])} "
                     f"<span class='gcount'>{d['count']} captures</span></h3>")
            if d["readme"]:
                link = (f" <a href='{esc(rel_from_out(d['readme_rel'], out_path))}'>"
                        "README</a>" if d["readme_rel"] else "")
                o.append(f"<p class='meta'>{md_inline(d['readme'])}{link}</p>")
            for g in d["groups"]:
                o.append("<div class='shotrow'>")
                for s in g["shots"]:
                    kind = s["kind"]
                    badge = ("<span class='badge defect'>DEFECT · kept on purpose</span>"
                             if kind == "defect" else
                             f"<span class='badge kind'>{esc(kind.upper())}</span>")
                    src_rel = rel_from_out(s["file"], out_path)
                    o.append(f"<figure><a href='{esc(src_rel)}'>"
                             f"<img src='{esc(src_rel)}' loading='lazy' "
                             f"alt='{esc(s['name'])}'></a>"
                             f"<figcaption>{badge} {esc(s['name'])}</figcaption></figure>")
                unresolved = not any(s["kind"] in ("after", "fixed") for s in g["shots"])
                o.append("</div>")
                if unresolved:
                    o.append("<p class='meta'>no paired fixed frame found by naming "
                             "for the group above</p>")
            extra = d["others"]
            if extra:
                o.append("<div class='shotrow'>")
                for f in extra[:4]:
                    src_rel = rel_from_out(f, out_path)
                    nm = Path(f).name
                    o.append(f"<figure><a href='{esc(src_rel)}'>"
                             f"<img src='{esc(src_rel)}' loading='lazy' alt='{esc(nm)}'></a>"
                             f"<figcaption>{esc(nm)}</figcaption></figure>")
                o.append("</div>")
                if len(extra) > 4:
                    o.append(f"<p class='meta'>+{len(extra) - 4} more in "
                             f"<code>docs/screenshots/{esc(d['name'])}/</code></p>")
            o.append("</div>")
        if plainlist:
            o.append(f"<details><summary>{len(plainlist)} more capture directories "
                     "(no README / no progression pairs)</summary><ul>")
            for d in plainlist:
                o.append(f"<li class='mono'>docs/screenshots/{esc(d['name'])} "
                         f"<span class='gcount'>{d['count']} captures</span></li>")
            o.append("</ul></details>")

    o.append("<footer>Derived: branch table (git), checklists and open-work summaries "
             "(parsed from the two design docs), contradiction checks (doc SHAs vs "
             "origin/main), screenshot walk (filesystem). Nothing on this page is "
             "hand-typed; if a source could not be read it says &ldquo;not "
             "available&rdquo;. Page is a snapshot — regenerate after fetching.</footer>")
    o.append("</main></body></html>")
    return "\n".join(o)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default=str(DEFAULT_OUT),
                    help="output html path (default docs/dashboard.html)")
    args = ap.parse_args()
    out_path = Path(args.out).resolve()
    html_text = render(out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(html_text, encoding="utf-8")
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
