#!/usr/bin/env python3
"""meshy_scrape_actions.py — rebuild the Meshy ANIMATION ACTION CATALOG.

WHY THIS EXISTS
---------------
Meshy's animation API takes an integer `action_id`, but there is NO API endpoint
that enumerates the actions. Probed and confirmed 404/400:

    /openapi/v1/animations/actions      /openapi/v1/animation/actions
    /openapi/v1/animations/library      /openapi/v2/animations/actions
    /openapi/v1/actions                 /openapi.json   (no published spec)

and `GET /openapi/v1/animations` returns 200 but lists YOUR PAST TASKS, not the
catalog. The catalog IS public, but only as a rendered HTML table in the docs:

    https://docs.meshy.ai/en/api/animation-library

That page is server-rendered plain <table> markup (no JS needed), so we scrape it
into tools/meshy_actions.json and treat that file as the checked-in source of
truth. Re-run this whenever Meshy adds actions.

    python tools/meshy_scrape_actions.py [--out tools/meshy_actions.json]
"""
import argparse, json, os, re, sys, urllib.request

URL = "https://docs.meshy.ai/en/api/animation-library"
DEFAULT_OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "meshy_actions.json")


def fetch(url):
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 (meshy-clip-library)"})
    with urllib.request.urlopen(req, timeout=90) as r:
        return r.read().decode("utf-8", "replace")


def detag(s):
    return (re.sub(r"<[^>]+>", "", s)
            .replace("&amp;", "&").replace("&#x27;", "'").replace("&quot;", '"').strip())


def parse(html):
    rows = []
    for tr in re.findall(r"<tr>(.*?)</tr>", html, re.S):
        tds = re.findall(r"<td[^>]*>(.*?)</td>", tr, re.S)
        if len(tds) < 4:
            continue
        idtxt = detag(tds[0])
        if not re.fullmatch(r"\d+", idtxt):
            continue
        img = re.search(r'src="([^"]+)"', tds[4]) if len(tds) > 4 else None
        preview = img.group(1) if img else None
        rig = None
        if preview:
            m = re.search(r"/preview/([^/]+)/", preview)
            rig = m.group(1) if m else None
        name = detag(tds[1])
        rows.append({
            "id": int(idtxt),
            "name": name,
            "category": detag(tds[2]),
            "subcategory": detag(tds[3]),
            # Meshy ships ~90 duplicate clips suffixed `_inplace`: same motion with
            # the root translation stripped. Those are the ones you want whenever
            # the ENGINE drives movement (character controller / navmesh); the
            # root-motion originals slide the character away from its capsule.
            "inplace": name.endswith("_inplace"),
            "rig_type": rig,
            "preview": preview,
        })
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--html", help="parse a local saved copy instead of fetching")
    a = ap.parse_args()

    html = open(a.html, encoding="utf-8").read() if a.html else fetch(URL)
    rows = parse(html)
    if len(rows) < 400:
        sys.exit(f"only parsed {len(rows)} rows — the docs page layout probably changed; "
                 f"inspect it before trusting this output")
    rows.sort(key=lambda r: r["id"])
    ids = [r["id"] for r in rows]
    doc = {
        "source": URL,
        "count": len(rows),
        "id_min": min(ids),
        "id_max": max(ids),
        "inplace_count": sum(1 for r in rows if r["inplace"]),
        "actions": rows,
    }
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")
    print(f"wrote {a.out}: {len(rows)} actions, ids {min(ids)}..{max(ids)}, "
          f"{doc['inplace_count']} _inplace variants")


if __name__ == "__main__":
    main()
