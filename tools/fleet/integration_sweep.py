#!/usr/bin/env python3
"""integration_sweep.py — the fleet's "is your headline work actually on main?"
drift auditor. Runs on a schedule (Fleet-Integration-Sweep task), fetches origin,
and reports every origin branch that carries commits NOT yet folded into
origin/main — the integrator's fold backlog — plus stale/rotting lanes.

WHY: the fleet ships faster than it integrates. Great work keeps landing on
feature branches (or local-only, then pushed) and never reaches main. Clone/Sarah,
the sun v5 kill-cam, and 14900K's killable-dreadnought all stranded this way.
This surfaces the drift every run so nothing good rots unnoticed.

LIMITATION (stated in the digest): this sees ORIGIN only. Truly local commits on
another box are invisible until pushed — so the digest reminds boxes to push.

Pure stdlib. Posts a concise digest to #fleet-ops and writes the full report to
fleet-handoff/integration-sweep/latest.md. Run: python tools/fleet/integration_sweep.py
"""
from __future__ import annotations
import json, os, subprocess, sys, urllib.request, urllib.error, urllib.parse
from datetime import datetime, timezone
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]        # G:\X3Native
HS = "https://fleetcommand.slopclaude.com"
DOM = "fleetcommand.slopclaude.com"
ROOM_ALIAS = "#fleet-ops:" + DOM
TOKEN = (Path(os.path.expanduser("~")) / ".claude" / ".matrix_token").read_text().strip()
OUT = REPO / "fleet-handoff" / "integration-sweep"
STALE_DAYS = 10          # a lane untouched this long that isn't folded = rotting
BASE = "origin/main"

# Branch-name prefixes that are EXPLICIT fold candidates (someone staged them to land).
FOLD_PREFIXES = ("integration/", "rescue/", "merge/")


def git(*args: str) -> str:
    return subprocess.run(["git", "-C", str(REPO), *args],
                          capture_output=True, text=True, timeout=120).stdout.strip()


def ancestor(a: str, b: str) -> bool:
    return subprocess.run(["git", "-C", str(REPO), "merge-base", "--is-ancestor", a, b]).returncode == 0


def days_since(iso: str) -> float:
    try:
        d = datetime.fromisoformat(iso)
        if d.tzinfo is None:
            d = d.replace(tzinfo=timezone.utc)
        return (datetime.now(timezone.utc) - d).total_seconds() / 86400.0
    except Exception:
        return -1.0


def collect() -> dict:
    git("fetch", "origin", "--prune", "--quiet")
    base_sha = git("rev-parse", "--short", BASE)
    raw = git("for-each-ref", "--format=%(refname:short)|%(committerdate:iso-strict)|%(authorname)",
              "refs/remotes/origin")
    fold, active, stale = [], [], []
    for line in raw.splitlines():
        parts = line.split("|")
        if len(parts) < 3:
            continue
        ref, cdate, author = parts[0], parts[1], parts[2]
        name = ref[len("origin/"):] if ref.startswith("origin/") else ref
        if name in ("main", "HEAD") or "->" in ref:
            continue
        if ancestor(ref, BASE):          # fully folded — nothing to do
            continue
        ahead = git("rev-list", "--count", f"{BASE}..{ref}")
        behind = git("rev-list", "--count", f"{ref}..{BASE}")
        subj = git("log", "-1", "--format=%s", ref)[:64]
        age = days_since(cdate)
        row = {"name": name, "ahead": int(ahead or 0), "behind": int(behind or 0),
               "subj": subj, "age": age, "author": author}
        if name.startswith(FOLD_PREFIXES):
            fold.append(row)
        elif age >= STALE_DAYS:
            stale.append(row)
        else:
            active.append(row)
    fold.sort(key=lambda r: -r["ahead"])
    active.sort(key=lambda r: r["age"])          # freshest active first
    stale.sort(key=lambda r: -r["age"])          # most-rotted first
    return {"base": base_sha, "fold": fold, "active": active, "stale": stale}


def fmt_row(r: dict) -> str:
    age = f"{r['age']:.0f}d" if r["age"] >= 0 else "?"
    behind = f", {r['behind']} behind" if r["behind"] else ""
    return f"  {r['name']}  (+{r['ahead']}{behind}, {age}) — {r['subj']}"


def build_digest(data: dict) -> str:
    L = [f"[INTEGRATION SWEEP] origin/main @ {data['base']} — un-folded work on origin:"]
    if data["fold"]:
        L.append(f"\n🔴 FOLD CANDIDATES ({len(data['fold'])}) — staged to land, awaiting integrator:")
        L += [fmt_row(r) for r in data["fold"][:8]]
    if data["active"]:
        L.append(f"\n🟡 ACTIVE LANES ({len(data['active'])}) — in-flight, not yet on main:")
        L += [fmt_row(r) for r in data["active"][:8]]
    if data["stale"]:
        L.append(f"\n⚫ STALE ({len(data['stale'])}) — untouched {STALE_DAYS}d+, fold-or-prune:")
        L += [fmt_row(r) for r in data["stale"][:8]]
    if not (data["fold"] or data["active"] or data["stale"]):
        L.append("\n✅ ALL CLEAR — every origin branch is folded into main.")
    L.append("\nNote: this sees ORIGIN only — local-only commits are invisible until pushed. "
             "If your headline work isn't listed AND isn't on main, PUSH IT. "
             "Full report: G:\\X3Native\\fleet-handoff\\integration-sweep\\latest.md  —CommanderIntegrator (auto)")
    return "\n".join(L)


def post(text: str) -> None:
    def get(url):
        r = urllib.request.Request(url, headers={"Authorization": "Bearer " + TOKEN,
                                                 "User-Agent": "fleet-bot/1.0"})
        return json.loads(urllib.request.urlopen(r, timeout=20).read())
    rid = get(f"{HS}/_matrix/client/v3/directory/room/{urllib.parse.quote(ROOM_ALIAS)}")["room_id"]
    txn = f"sweep-{int(datetime.now(timezone.utc).timestamp()*1000)}"
    url = f"{HS}/_matrix/client/v3/rooms/{urllib.parse.quote(rid)}/send/m.room.message/{txn}"
    body = json.dumps({"msgtype": "m.text", "body": text}).encode()
    req = urllib.request.Request(url, data=body, method="PUT",
                                 headers={"Authorization": "Bearer " + TOKEN,
                                          "Content-Type": "application/json",
                                          "User-Agent": "fleet-bot/1.0"})
    urllib.request.urlopen(req, timeout=20)


def main() -> int:
    data = collect()
    digest = build_digest(data)
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "latest.md").write_text(digest + "\n", encoding="utf-8")
    print(digest)
    if "--no-post" not in sys.argv:
        try:
            post(digest)
            print("\n[posted to #fleet-ops]")
        except Exception as e:
            print(f"\n[post failed: {e}]", file=sys.stderr)
            return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
