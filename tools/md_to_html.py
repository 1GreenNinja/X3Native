#!/usr/bin/env python3
"""Minimal, dependency-free Markdown -> styled HTML converter.

Supports the subset used by X3Native docs reports: ATX headings (#..######),
pipe tables, unordered lists, horizontal rules, paragraphs, blockquotes,
and inline **bold**, `code`, and [text](url) links. Emits a self-contained
dark-themed HTML file.

Usage: python md_to_html.py <input.md> <output.html> ["Page Title"]
"""
import sys, html, re


def inline(text: str) -> str:
    """Escape HTML, then apply inline markdown (code, bold, links)."""
    # Protect code spans first so their contents aren't escaped twice.
    spans = []

    def stash(m):
        spans.append(m.group(1))
        return f"\x00{len(spans) - 1}\x00"

    text = re.sub(r"`([^`]+)`", stash, text)
    text = html.escape(text, quote=False)
    text = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", text)
    text = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", r'<a href="\2">\1</a>', text)

    def unstash(m):
        return "<code>" + html.escape(spans[int(m.group(1))], quote=False) + "</code>"

    return re.sub(r"\x00(\d+)\x00", unstash, text)


def convert(md: str) -> str:
    lines = md.split("\n")
    out, i, n = [], 0, len(lines)
    while i < n:
        line = lines[i]
        stripped = line.strip()

        if not stripped:
            i += 1
            continue

        # Horizontal rule
        if re.fullmatch(r"-{3,}", stripped):
            out.append("<hr>")
            i += 1
            continue

        # Heading
        h = re.match(r"(#{1,6})\s+(.*)", stripped)
        if h:
            lvl = len(h.group(1))
            out.append(f"<h{lvl}>{inline(h.group(2))}</h{lvl}>")
            i += 1
            continue

        # Table: a header row followed by a |---|---| separator
        if stripped.startswith("|") and i + 1 < n and re.match(r"\s*\|?[\s:|-]+\|", lines[i + 1]) and "-" in lines[i + 1]:
            def cells(row):
                row = row.strip().strip("|")
                return [c.strip() for c in row.split("|")]

            header = cells(lines[i])
            i += 2  # skip header + separator
            out.append('<table><thead><tr>')
            out.extend(f"<th>{inline(c)}</th>" for c in header)
            out.append("</tr></thead><tbody>")
            while i < n and lines[i].strip().startswith("|"):
                out.append("<tr>")
                out.extend(f"<td>{inline(c)}</td>" for c in cells(lines[i]))
                out.append("</tr>")
                i += 1
            out.append("</tbody></table>")
            continue

        # Unordered list
        if re.match(r"[-*]\s+", stripped):
            out.append("<ul>")
            while i < n and re.match(r"\s*[-*]\s+", lines[i]):
                item = re.sub(r"\s*[-*]\s+", "", lines[i], count=1)
                out.append(f"<li>{inline(item)}</li>")
                i += 1
            out.append("</ul>")
            continue

        # Blockquote
        if stripped.startswith(">"):
            buf = []
            while i < n and lines[i].strip().startswith(">"):
                buf.append(lines[i].strip()[1:].strip())
                i += 1
            out.append(f"<blockquote>{inline(' '.join(buf))}</blockquote>")
            continue

        # Paragraph (gather until blank line / block element)
        buf = []
        while i < n and lines[i].strip() and not re.match(r"(#{1,6}\s|[-*]\s|>|\|)", lines[i].strip()) and not re.fullmatch(r"-{3,}", lines[i].strip()):
            buf.append(lines[i].strip())
            i += 1
        if buf:
            out.append(f"<p>{inline(' '.join(buf))}</p>")
    return "\n".join(out)


CSS = """
:root{--bg:#0b0f14;--panel:#121822;--ink:#dce6f2;--muted:#8aa0b8;--line:#1f2a38;
--accent:#39d4ff;--accent2:#7c5cff;--row:#0f151d;--rowalt:#0c1118}
*{box-sizing:border-box}
body{margin:0;background:linear-gradient(160deg,#0a0e13,#0d1420 60%,#0a0e13);
color:var(--ink);font:15px/1.6 'Segoe UI',system-ui,-apple-system,sans-serif;
padding:0 0 80px}
.wrap{max-width:1060px;margin:0 auto;padding:0 28px}
header.hero{padding:48px 28px 28px;border-bottom:1px solid var(--line);
background:radial-gradient(900px 300px at 20% -10%,rgba(57,212,255,.12),transparent),
radial-gradient(700px 260px at 90% -20%,rgba(124,92,255,.12),transparent)}
.hero .wrap{padding:0}
h1{font-size:30px;margin:0 0 6px;letter-spacing:.3px;
background:linear-gradient(90deg,var(--accent),var(--accent2));
-webkit-background-clip:text;background-clip:text;color:transparent}
h2{margin:38px 0 12px;font-size:21px;padding-bottom:8px;border-bottom:1px solid var(--line)}
h3{margin:26px 0 10px;font-size:16px;color:var(--accent)}
p{margin:10px 0;color:var(--ink)}
em{color:var(--muted)}
a{color:var(--accent);text-decoration:none}a:hover{text-decoration:underline}
code{background:#0a0e14;border:1px solid var(--line);border-radius:5px;
padding:1px 6px;font:13px/1.5 'Cascadia Code',Consolas,monospace;color:#9fe8ff}
strong{color:#fff}
hr{border:0;border-top:1px solid var(--line);margin:30px 0}
table{width:100%;border-collapse:collapse;margin:14px 0;font-size:14px;
border:1px solid var(--line);border-radius:10px;overflow:hidden}
thead th{background:linear-gradient(180deg,#16202c,#111923);text-align:left;
padding:10px 12px;color:#bfe9ff;font-weight:600;border-bottom:1px solid var(--line)}
tbody td{padding:9px 12px;border-bottom:1px solid var(--line);vertical-align:top}
tbody tr:nth-child(odd){background:var(--row)}
tbody tr:nth-child(even){background:var(--rowalt)}
tbody tr:hover{background:#15202c}
ul{margin:10px 0;padding-left:22px}li{margin:4px 0}
blockquote{margin:14px 0;padding:8px 16px;border-left:3px solid var(--accent2);
background:#101622;color:var(--muted);border-radius:0 8px 8px 0}
.footer{margin-top:40px;color:var(--muted);font-size:12px}
""".strip()


def main():
    if len(sys.argv) < 3:
        print("usage: md_to_html.py <in.md> <out.html> [title]", file=sys.stderr)
        sys.exit(2)
    src, dst = sys.argv[1], sys.argv[2]
    title = sys.argv[3] if len(sys.argv) > 3 else "Report"
    with open(src, "r", encoding="utf-8") as f:
        body = convert(f.read())
    doc = (
        "<!doctype html><html lang='en'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        f"<title>{html.escape(title)}</title><style>{CSS}</style></head>"
        f"<body><header class='hero'><div class='wrap'></div></header>"
        f"<div class='wrap'>{body}</div></body></html>"
    )
    with open(dst, "w", encoding="utf-8") as f:
        f.write(doc)
    print(f"wrote {dst} ({len(doc)} bytes)")


if __name__ == "__main__":
    main()
