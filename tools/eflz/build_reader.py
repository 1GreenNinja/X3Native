"""Bake every EFLZ design doc + MASTER_GAME_PLAN + a couple others into a single
self-contained HTML file with sidebar nav + search + dark theme.

Markdown -> HTML done in-script (no external deps); handles the subset these docs
actually use (headers, lists, tables, code blocks, blockquotes, hr, bold/italic,
inline code, links, hr's, emoji passthrough).
"""

import html as html_lib
import os
import re
import json

# Resolve REPO from this file's location so the script keeps working if the
# tools/eflz dir moves or the script gets invoked from another cwd. Falls back
# to the historic hard-coded path if structure looks different.
_HERE = os.path.dirname(os.path.abspath(__file__))
if os.path.basename(_HERE) == 'eflz' and os.path.basename(os.path.dirname(_HERE)) == 'tools':
    REPO = os.path.dirname(os.path.dirname(_HERE))
else:
    REPO = r'G:\X3Native'
DESIGN = os.path.join(REPO, 'docs', 'design')
DOCS_TOP = os.path.join(REPO, 'docs')
FLEET = os.path.join(REPO, 'docs', 'fleet')
PLAN_REVIEWS = os.path.join(REPO, 'docs', 'plan-reviews')

# Files to bundle (order matters — MASTER first).
DOCS = [
    # File path                                                    Pretty title                                Group
    (os.path.join(DESIGN, 'EFLZ_MASTER_TASK_LIST.md'),             'Master Task List (START HERE)',            'Index'),
    (os.path.join(DESIGN, 'EFLZ_FEATURE_PACK_2026-05-31.md'),      'Feature Pack (Wife\'s Notes)',             'New'),
    (os.path.join(DESIGN, 'EFLZ_TECH_SYSTEMS.md'),                 'Tech Systems + Bio-Integration Lab',       'New'),
    (os.path.join(DESIGN, 'EFLZ_SKILL_TREES.md'),                  'Skill Trees + Economy',                    'New'),
    (os.path.join(DESIGN, 'EFLZ_CRAFTING_INVENTORY.md'),           'Crafting + Inventory',                     'New'),
    (os.path.join(DESIGN, 'EFLZ_DIALOGUE_CATALOG.md'),             'Dialogue Catalog (60+ scenes)',            'New'),
    (os.path.join(DESIGN, 'EFLZ_BESTIARY_RECONCILE.md'),           'Bestiary Reconcile (Evil Sarah Clone)',    'New'),
    (os.path.join(DESIGN, 'EFLZ_SIDE_QUESTS_ACHIEVEMENTS.md'),     'Side Quests + Achievements + Audio',       'New'),
    (os.path.join(DESIGN, 'EFLZ_ACTS_2_4_GAPS.md'),                'Acts 2/3/4 Reconcile + 12 Endings',        'New'),
    (os.path.join(DESIGN, 'EFLZ_WORLD_STRUCTURE.md'),              'World Structure Digest',                   'Existing'),
    (os.path.join(DESIGN, 'EFLZ_BESTIARY.md'),                     'Bestiary (Original)',                      'Existing'),
    (os.path.join(DESIGN, 'EFLZ_NARRATIVE.md'),                    'Narrative Spine',                          'Existing'),
    (os.path.join(DESIGN, 'EFLZ_MASTER_PLAN.md'),                  'EFLZ Master Plan (Earlier Draft)',         'Existing'),
    (os.path.join(DESIGN, 'X3_WORLD_BLUEPRINT.md'),                'X3 World Blueprint',                       'Existing'),
    (os.path.join(DESIGN, 'WORLD_AND_EDITOR_PLAN.md'),             'World + Editor Plan',                      'Existing'),
    (os.path.join(DESIGN, 'SPACE_ART_PLAN.md'),                    'Space Art Plan',                           'Existing'),
    (os.path.join(DESIGN, 'RAYTRACING_SCOPE.md'),                  'Raytracing Scope',                         'Existing'),
    (os.path.join(DESIGN, 'SPIRE_LEVELARCHITECT_DIMS.md'),         'Spire Level-Architect Dims',               'Existing'),
    (os.path.join(DOCS_TOP, 'MASTER_GAME_PLAN.md'),                'Master Game Plan (Old)',                   'Existing'),
    (os.path.join(DOCS_TOP, 'EFLZ_DESIGN.md'),                     'EFLZ Design',                              'Existing'),
    (os.path.join(DOCS_TOP, 'EFLZ_ACT1_PLAYTEST.md'),              'Act 1 Playtest Guide',                     'Existing'),
    (os.path.join(DOCS_TOP, 'BEYOND_IDTECH8.md'),                  'Beyond idTech 8',                          'Existing'),
    (os.path.join(DOCS_TOP, 'PLAYTEST_GUIDE.md'),                  'Playtest Guide',                           'Existing'),
    (os.path.join(DOCS_TOP, 'ASSET_INVENTORY.md'),                 'Asset Inventory',                          'Existing'),
    (os.path.join(DOCS_TOP, 'FLEET_SPECS.md'),                     'Fleet Specs',                              'Existing'),
    (os.path.join(DOCS_TOP, 'GLASS_MATERIAL_SPEC.md'),             'Glass Material Spec',                      'Existing'),
    # ---- New / moved since the May-31 bake ----
    (os.path.join(FLEET,    'ROSTER.md'),                          'Fleet Roster',                              'Fleet'),
    (os.path.join(DOCS_TOP, 'BRANCH_FEATURE_REPORT.md'),            'Branch Feature Report',                     'Fleet'),
    (os.path.join(PLAN_REVIEWS, 'X3_NATIVE_SLICES-2026-06-05.md'),  'Slices Plan Review (2026-06-05)',           'Plan Reviews'),
    (os.path.join(PLAN_REVIEWS, 'GLASS_MATERIAL_SPEC-2026-05-31.md'),'Glass Material Spec Review (2026-05-31)',  'Plan Reviews'),
]


def auto_discover_docs(curated_paths):
    """Walk docs/ for any *.md not already in the curated list and bundle them
    under their parent directory's name. Keeps the reader current as the
    fleet adds new design docs without code changes."""
    found = []
    for root, _, files in os.walk(DOCS_TOP):
        rel_root = os.path.relpath(root, REPO).replace('\\', '/')
        if 'screenshots' in rel_root.split('/'):
            continue
        for fn in files:
            if not fn.endswith('.md'):
                continue
            full = os.path.join(root, fn)
            if os.path.abspath(full) in curated_paths:
                continue
            base = fn[:-3]
            pretty = re.sub(r'[_\-]+', ' ', base).strip()
            try:
                rel = os.path.relpath(root, DOCS_TOP).replace('\\', '/')
            except ValueError:
                rel = ''
            if rel in ('', '.'):
                group = 'Other docs'
            else:
                group = ' / '.join(p.capitalize() for p in rel.split('/'))
            found.append((full, pretty, group))
    return found


def md_to_html(md):
    """Tiny markdown converter. Handles what these docs use."""
    if not md:
        return ''
    lines = md.split('\n')
    out = []
    i = 0
    in_code = False
    code_lang = ''
    code_buf = []
    in_list = False
    list_type = None  # 'ul' or 'ol'
    in_table = False
    table_rows = []
    in_blockquote = False
    bq_buf = []

    def flush_list():
        nonlocal in_list, list_type
        if in_list:
            out.append(f'</{list_type}>')
            in_list = False
            list_type = None

    def flush_table():
        nonlocal in_table, table_rows
        if in_table:
            if table_rows:
                # First row = header, second = separator (drop), rest = body.
                header = table_rows[0]
                body = table_rows[2:] if len(table_rows) > 2 else []
                out.append('<table>')
                out.append('<thead><tr>')
                for cell in header:
                    out.append(f'<th>{inline_md(cell)}</th>')
                out.append('</tr></thead>')
                if body:
                    out.append('<tbody>')
                    for row in body:
                        out.append('<tr>')
                        for cell in row:
                            out.append(f'<td>{inline_md(cell)}</td>')
                        out.append('</tr>')
                    out.append('</tbody>')
                out.append('</table>')
            in_table = False
            table_rows = []

    def flush_bq():
        nonlocal in_blockquote, bq_buf
        if in_blockquote:
            inner = md_to_html('\n'.join(bq_buf))
            out.append(f'<blockquote>{inner}</blockquote>')
            in_blockquote = False
            bq_buf = []

    def inline_md(text):
        # Escape HTML first, then re-apply markdown inline patterns.
        # But we want to allow some chars in code spans, so handle code spans first.
        # Strategy: tokenize code spans, then escape the non-code, then apply inline patterns.
        tokens = []
        pos = 0
        for m in re.finditer(r'`([^`]+)`', text):
            tokens.append(('text', text[pos:m.start()]))
            tokens.append(('code', m.group(1)))
            pos = m.end()
        tokens.append(('text', text[pos:]))
        out_parts = []
        for kind, val in tokens:
            if kind == 'code':
                out_parts.append(f'<code>{html_lib.escape(val)}</code>')
            else:
                v = html_lib.escape(val)
                # Bold (**...**) — non-greedy
                v = re.sub(r'\*\*([^*]+)\*\*', r'<strong>\1</strong>', v)
                # Italic (_..._ or *...*)
                v = re.sub(r'(?<!\w)\*([^*\n]+)\*(?!\w)', r'<em>\1</em>', v)
                v = re.sub(r'(?<!\w)_([^_\n]+)_(?!\w)', r'<em>\1</em>', v)
                # Links [text](url)
                v = re.sub(r'\[([^\]]+)\]\(([^)]+)\)',
                           lambda m: f'<a href="{m.group(2)}">{m.group(1)}</a>', v)
                # Wiki-links [[name]]
                v = re.sub(r'\[\[([^\]]+)\]\]', r'<span class="wikilink">[[\1]]</span>', v)
                # Strikethrough ~~...~~
                v = re.sub(r'~~([^~]+)~~', r'<del>\1</del>', v)
                out_parts.append(v)
        return ''.join(out_parts)

    while i < len(lines):
        line = lines[i]
        # Code fence
        m = re.match(r'^```(\w*)\s*$', line)
        if m:
            if in_code:
                # close
                out.append(f'<pre><code class="lang-{code_lang}">{html_lib.escape(chr(10).join(code_buf))}</code></pre>')
                in_code = False
                code_buf = []
                code_lang = ''
            else:
                flush_list(); flush_table(); flush_bq()
                in_code = True
                code_lang = m.group(1) or 'plain'
            i += 1
            continue
        if in_code:
            code_buf.append(line)
            i += 1
            continue

        # Blockquote
        if line.startswith('> '):
            flush_list(); flush_table()
            if not in_blockquote:
                in_blockquote = True
            bq_buf.append(line[2:])
            i += 1
            continue
        elif line.strip() == '>':
            if in_blockquote:
                bq_buf.append('')
            i += 1
            continue
        else:
            flush_bq()

        # Horizontal rule
        if re.match(r'^-{3,}\s*$', line) or re.match(r'^_{3,}\s*$', line) or re.match(r'^\*{3,}\s*$', line):
            flush_list(); flush_table()
            out.append('<hr/>')
            i += 1
            continue

        # Headings
        m = re.match(r'^(#{1,6})\s+(.*)$', line)
        if m:
            flush_list(); flush_table()
            level = len(m.group(1))
            content = inline_md(m.group(2).rstrip(' #'))
            anchor = re.sub(r'[^a-z0-9]+', '-', m.group(2).lower()).strip('-')
            out.append(f'<h{level} id="{anchor}">{content}</h{level}>')
            i += 1
            continue

        # Table: line containing | and the next line being a separator
        if '|' in line and i + 1 < len(lines) and re.match(r'^\s*\|?[-:\s\|]+\|[-:\s\|]+\s*$', lines[i + 1]):
            flush_list()
            in_table = True
            table_rows = []
            # Parse header row
            row = [c.strip() for c in line.strip().strip('|').split('|')]
            table_rows.append(row)
            i += 1
            # Skip separator
            table_rows.append(['---'] * len(row))
            i += 1
            # Body rows
            while i < len(lines) and '|' in lines[i] and lines[i].strip():
                row = [c.strip() for c in lines[i].strip().strip('|').split('|')]
                table_rows.append(row)
                i += 1
            flush_table()
            continue

        # Unordered list
        m = re.match(r'^(\s*)[-*+]\s+(.*)$', line)
        if m:
            flush_table()
            if not in_list:
                in_list = True
                list_type = 'ul'
                out.append('<ul>')
            elif list_type != 'ul':
                flush_list()
                in_list = True
                list_type = 'ul'
                out.append('<ul>')
            out.append(f'<li>{inline_md(m.group(2))}</li>')
            i += 1
            continue

        # Ordered list
        m = re.match(r'^(\s*)\d+\.\s+(.*)$', line)
        if m:
            flush_table()
            if not in_list:
                in_list = True
                list_type = 'ol'
                out.append('<ol>')
            elif list_type != 'ol':
                flush_list()
                in_list = True
                list_type = 'ol'
                out.append('<ol>')
            out.append(f'<li>{inline_md(m.group(2))}</li>')
            i += 1
            continue

        # Blank line — close list, paragraph break
        if line.strip() == '':
            flush_list(); flush_table()
            i += 1
            continue

        # Paragraph
        flush_list(); flush_table()
        # Collect contiguous non-blank, non-special lines
        para = [line]
        i += 1
        while i < len(lines):
            nxt = lines[i]
            if nxt.strip() == '':
                break
            if re.match(r'^#{1,6}\s', nxt) or nxt.startswith('```') or nxt.startswith('> ') \
               or re.match(r'^\s*[-*+]\s+', nxt) or re.match(r'^\s*\d+\.\s+', nxt) \
               or re.match(r'^-{3,}\s*$', nxt):
                break
            if '|' in nxt and i + 1 < len(lines) and re.match(r'^\s*\|?[-:\s\|]+\|[-:\s\|]+\s*$', lines[i + 1]):
                break
            para.append(nxt)
            i += 1
        out.append(f'<p>{inline_md(" ".join(para))}</p>')

    flush_list(); flush_table(); flush_bq()
    if in_code and code_buf:
        out.append(f'<pre><code class="lang-{code_lang}">{html_lib.escape(chr(10).join(code_buf))}</code></pre>')
    return '\n'.join(out)


def extract_headings(md):
    """Pull H1/H2/H3 for the per-doc table of contents."""
    out = []
    in_code = False
    for line in md.split('\n'):
        if re.match(r'^```', line):
            in_code = not in_code
            continue
        if in_code:
            continue
        m = re.match(r'^(#{1,3})\s+(.*)$', line)
        if m:
            level = len(m.group(1))
            text = m.group(2).rstrip(' #')
            anchor = re.sub(r'[^a-z0-9]+', '-', text.lower()).strip('-')
            out.append({'level': level, 'text': text, 'anchor': anchor})
    return out


def build():
    # Curated list first (order preserved), then auto-discover everything else.
    curated_abs = {os.path.abspath(p) for (p, _t, _g) in DOCS}
    all_docs = list(DOCS) + auto_discover_docs(curated_abs)

    bundle = []
    for path, title, group in all_docs:
        if not os.path.exists(path):
            print(f'SKIP missing: {path}')
            continue
        with open(path, 'r', encoding='utf-8') as f:
            md = f.read()
        # Dedup ids — same filename in two subdirs would collide otherwise.
        doc_id = os.path.basename(path).replace('.md', '')
        if any(d['id'] == doc_id for d in bundle):
            doc_id = re.sub(r'[^a-zA-Z0-9_-]+', '-', os.path.relpath(path, REPO))[:-3]
        bundle.append({
            'id': doc_id,
            'title': title,
            'group': group,
            'path': os.path.relpath(path, REPO).replace('\\', '/'),
            'md': md,
            'html': md_to_html(md),
            'toc': extract_headings(md),
            'lines': len(md.split('\n')),
            'bytes': len(md.encode('utf-8')),
        })

    print(f'Loaded {len(bundle)} docs, total {sum(d["bytes"] for d in bundle)/1024:.1f} KB markdown')

    # Build the HTML. Embed all docs as JSON; render the selected one client-side
    # by injecting `innerHTML` from the pre-baked HTML strings.
    docs_json = json.dumps([{
        'id': d['id'],
        'title': d['title'],
        'group': d['group'],
        'path': d['path'],
        'html': d['html'],
        'toc': d['toc'],
        'lines': d['lines'],
        'bytes': d['bytes'],
        # Plain text for search (collapsed whitespace + stripped tags)
        'search': re.sub(r'\s+', ' ', re.sub(r'<[^>]+>', ' ', d['html'])).lower(),
    } for d in bundle])

    html_out = HTML_TEMPLATE.replace('__DOCS_JSON__', docs_json)
    out_path = os.path.join(DESIGN, '_reader.html')
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write(html_out)
    print(f'Wrote {out_path}  ({os.path.getsize(out_path)/1024:.1f} KB)')


HTML_TEMPLATE = r'''<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>EFLZ Design Docs Reader</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
:root {
  --bg: #0e1014;
  --bg2: #161922;
  --bg3: #1f2433;
  --border: #2a3145;
  --fg: #e8ebf2;
  --fg-dim: #8e95a8;
  --fg-strong: #ffffff;
  --accent: #7dd3fc;
  --accent2: #f0abfc;
  --warn: #fbbf24;
  --good: #86efac;
  --code-bg: #0a0d12;
  --code-fg: #d4d4d4;
  --shadow: 0 1px 3px rgba(0,0,0,.4);
  --mono: 'JetBrains Mono', 'Consolas', 'Cascadia Code', 'Menlo', monospace;
  --sans: -apple-system, 'Segoe UI', 'Inter', 'Roboto', sans-serif;
}
* { box-sizing: border-box; }
html, body { margin: 0; padding: 0; background: var(--bg); color: var(--fg); font-family: var(--sans); }
body { display: grid; grid-template-columns: 320px 1fr; min-height: 100vh; }

/* --- SIDEBAR --- */
#sidebar {
  background: var(--bg2);
  border-right: 1px solid var(--border);
  overflow-y: auto;
  position: sticky;
  top: 0;
  height: 100vh;
  padding: 0;
}
#sidebar-header {
  padding: 18px 18px 12px;
  border-bottom: 1px solid var(--border);
  background: var(--bg);
}
#sidebar-header h1 {
  margin: 0;
  font-size: 14px;
  font-weight: 600;
  letter-spacing: .04em;
  color: var(--fg-strong);
}
#sidebar-header .sub {
  font-size: 11px;
  color: var(--fg-dim);
  margin-top: 4px;
  font-family: var(--mono);
}
#search {
  width: 100%;
  margin-top: 10px;
  padding: 7px 10px;
  background: var(--bg3);
  border: 1px solid var(--border);
  border-radius: 4px;
  color: var(--fg);
  font: 13px var(--sans);
  outline: none;
}
#search:focus { border-color: var(--accent); }
#nav { padding: 8px 0 24px; }
.nav-group {
  font-size: 10px;
  font-weight: 700;
  text-transform: uppercase;
  letter-spacing: .1em;
  color: var(--fg-dim);
  padding: 14px 18px 6px;
}
.nav-item {
  display: block;
  padding: 7px 18px 7px 22px;
  color: var(--fg);
  text-decoration: none;
  font-size: 13px;
  line-height: 1.35;
  cursor: pointer;
  border-left: 3px solid transparent;
}
.nav-item:hover { background: var(--bg3); }
.nav-item.active { background: var(--bg3); border-left-color: var(--accent); color: var(--fg-strong); }
.nav-item .meta {
  display: block;
  font-size: 10px;
  color: var(--fg-dim);
  font-family: var(--mono);
  margin-top: 1px;
}
.nav-item.hit-search { background: rgba(125, 211, 252, .08); }

/* --- MAIN --- */
main { padding: 0; overflow-y: auto; }
#main-inner {
  max-width: 1080px;
  margin: 0 auto;
  padding: 28px 56px 80px;
}
#doc-header {
  border-bottom: 1px solid var(--border);
  margin-bottom: 24px;
  padding-bottom: 18px;
}
#doc-title { margin: 0 0 4px; color: var(--fg-strong); font-size: 22px; }
#doc-meta { color: var(--fg-dim); font-size: 12px; font-family: var(--mono); }

/* --- DOC CONTENT --- */
#doc-body {
  font-size: 15px;
  line-height: 1.65;
}
#doc-body h1, #doc-body h2, #doc-body h3, #doc-body h4, #doc-body h5, #doc-body h6 {
  color: var(--fg-strong);
  font-weight: 600;
  margin: 1.6em 0 .5em;
  line-height: 1.25;
}
#doc-body h1 { font-size: 1.7em; border-bottom: 1px solid var(--border); padding-bottom: .25em; }
#doc-body h2 { font-size: 1.4em; }
#doc-body h3 { font-size: 1.15em; color: var(--accent); }
#doc-body h4 { font-size: 1.05em; color: var(--accent2); }
#doc-body h5, #doc-body h6 { font-size: 1em; }
#doc-body p { margin: .6em 0; }
#doc-body a { color: var(--accent); text-decoration: none; }
#doc-body a:hover { text-decoration: underline; }
#doc-body strong { color: var(--fg-strong); font-weight: 700; }
#doc-body em { color: var(--good); }
#doc-body del { color: var(--fg-dim); }
#doc-body code {
  font-family: var(--mono);
  background: var(--code-bg);
  border: 1px solid var(--border);
  padding: 1px 5px;
  border-radius: 3px;
  font-size: .9em;
  color: var(--accent);
}
#doc-body pre {
  background: var(--code-bg);
  border: 1px solid var(--border);
  border-radius: 5px;
  padding: 12px 14px;
  overflow-x: auto;
  font-size: 13px;
  line-height: 1.5;
}
#doc-body pre code {
  background: none;
  border: none;
  padding: 0;
  color: var(--code-fg);
  font-size: inherit;
}
#doc-body ul, #doc-body ol { padding-left: 1.6em; margin: .5em 0; }
#doc-body li { margin: .25em 0; }
#doc-body blockquote {
  border-left: 3px solid var(--accent);
  margin: 1em 0;
  padding: .5em 1em;
  background: var(--bg2);
  color: var(--fg-dim);
  border-radius: 0 4px 4px 0;
}
#doc-body blockquote p:first-child { margin-top: 0; }
#doc-body blockquote p:last-child { margin-bottom: 0; }
#doc-body hr { border: none; border-top: 1px solid var(--border); margin: 2em 0; }
#doc-body table {
  border-collapse: collapse;
  margin: 1em 0;
  font-size: 14px;
  display: block;
  overflow-x: auto;
  max-width: 100%;
}
#doc-body table thead { background: var(--bg2); }
#doc-body table th, #doc-body table td {
  border: 1px solid var(--border);
  padding: 7px 11px;
  text-align: left;
  vertical-align: top;
}
#doc-body table th { color: var(--fg-strong); font-weight: 600; }
#doc-body .wikilink { color: var(--accent2); font-family: var(--mono); font-size: .9em; }

/* --- WELCOME / EMPTY --- */
.welcome {
  padding: 60px 0;
  text-align: center;
  color: var(--fg-dim);
}
.welcome h2 { color: var(--fg-strong); margin: 0 0 16px; }

/* --- MOBILE --- */
@media (max-width: 800px) {
  body { grid-template-columns: 1fr; }
  #sidebar { position: relative; height: auto; max-height: 50vh; }
  #main-inner { padding: 18px; }
}

/* Hide scrollbar styling but keep functional */
::-webkit-scrollbar { width: 10px; height: 10px; }
::-webkit-scrollbar-track { background: var(--bg); }
::-webkit-scrollbar-thumb { background: var(--bg3); border-radius: 4px; }
::-webkit-scrollbar-thumb:hover { background: var(--border); }
</style>
</head>
<body>
<aside id="sidebar">
  <div id="sidebar-header">
    <h1>📖 EFLZ Design Reader</h1>
    <div class="sub" id="sub-meta"></div>
    <input id="search" placeholder="Search all docs…" autocomplete="off">
  </div>
  <nav id="nav"></nav>
</aside>
<main>
  <div id="main-inner">
    <div id="doc-header" style="display:none;">
      <h1 id="doc-title"></h1>
      <div id="doc-meta"></div>
    </div>
    <div id="doc-body" class="welcome">
      <h2>📖 Escape From Lab Zero — Design Doc Reader</h2>
      <p>Pick a doc from the sidebar.</p>
      <p style="margin-top:24px;font-size:13px;color:var(--fg-dim);font-family:var(--mono);">
        Self-contained HTML — no internet required.<br>
        Serve over LAN: <code>cd docs/design &amp;&amp; python -m http.server 8000</code>
      </p>
    </div>
  </div>
</main>
<script>
const DOCS = __DOCS_JSON__;

const subMeta = document.getElementById('sub-meta');
const totalKB = (DOCS.reduce((s, d) => s + d.bytes, 0) / 1024).toFixed(1);
const totalLines = DOCS.reduce((s, d) => s + d.lines, 0);
subMeta.textContent = `${DOCS.length} docs · ${totalLines.toLocaleString()} lines · ${totalKB} KB`;

// Build nav grouped
const nav = document.getElementById('nav');
const groups = {};
DOCS.forEach(d => {
  if (!groups[d.group]) groups[d.group] = [];
  groups[d.group].push(d);
});
for (const [group, docs] of Object.entries(groups)) {
  const h = document.createElement('div');
  h.className = 'nav-group';
  h.textContent = group;
  nav.appendChild(h);
  for (const d of docs) {
    const a = document.createElement('a');
    a.className = 'nav-item';
    a.dataset.id = d.id;
    a.innerHTML = `${escapeHtml(d.title)}<span class="meta">${d.lines.toLocaleString()} lines · ${d.id}.md</span>`;
    a.onclick = (e) => { e.preventDefault(); showDoc(d.id); window.history.replaceState(null, '', '#' + d.id); };
    nav.appendChild(a);
  }
}

function escapeHtml(s) {
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

function showDoc(id) {
  const d = DOCS.find(x => x.id === id);
  if (!d) return;
  document.querySelectorAll('.nav-item').forEach(n => {
    n.classList.toggle('active', n.dataset.id === id);
  });
  document.getElementById('doc-header').style.display = '';
  document.getElementById('doc-title').textContent = d.title;
  document.getElementById('doc-meta').textContent = `${d.path} · ${d.lines.toLocaleString()} lines · ${(d.bytes/1024).toFixed(1)} KB`;
  const body = document.getElementById('doc-body');
  body.className = '';
  body.innerHTML = d.html;
  document.querySelector('main').scrollTop = 0;
}

// Search
const search = document.getElementById('search');
search.addEventListener('input', () => {
  const q = search.value.trim().toLowerCase();
  document.querySelectorAll('.nav-item').forEach(n => {
    const d = DOCS.find(x => x.id === n.dataset.id);
    if (!q) {
      n.style.display = '';
      n.classList.remove('hit-search');
      return;
    }
    const hit = d.search.includes(q) || d.title.toLowerCase().includes(q) || d.id.toLowerCase().includes(q);
    n.style.display = hit ? '' : 'none';
    n.classList.toggle('hit-search', hit);
  });
});

// Load from hash or first doc
window.addEventListener('hashchange', () => {
  const id = location.hash.replace('#', '');
  if (id) showDoc(id);
});
const initial = location.hash.replace('#', '') || DOCS[0].id;
showDoc(initial);
</script>
</body>
</html>
'''

if __name__ == '__main__':
    build()
