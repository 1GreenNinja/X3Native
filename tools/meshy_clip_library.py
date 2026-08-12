#!/usr/bin/env python3
"""meshy_clip_library.py — portfolio-wide animation clip pipeline on Meshy.

    rig a character ONCE (5 cr)  ->  bake a ROLE SET of clips (3 cr each)
                                 ->  merge them onto ANY of our characters (FREE)

THE THREE FACTS THAT SHAPE THIS TOOL
------------------------------------
1. There is NO API to list animations. `action_id` is an integer in [0,696] and
   the only public enumeration is an HTML table in Meshy's docs. We scrape it to
   tools/meshy_actions.json (see tools/meshy_scrape_actions.py) and resolve
   human-readable names against that file, so nobody hand-types magic numbers.

2. A model can only EXPORT the last 20 actions selected on it. 20 is therefore a
   hard cap per baked variant. We answer that with ROLE SETS (tools/
   meshy_role_sets.json): bake NORMAL / FIGHTING / CASTING / DANCING separately,
   then fuse whatever one character needs with tools/glb-merge-anims.mjs, which
   is local, free, and has no cap.

3. CREDITS ARE A SHARED FLEET RESOURCE. Baking is the ONLY thing here that spends
   them, and it refuses to run unless you pass `--yes-spend <exact_credit_count>`
   matching what the tool independently computed. Everything else -- catalog,
   search, planning, cost estimates, merging -- is free and offline.

COMMANDS
--------
  sets                          list role sets + credit cost of each
  show    <set>                 resolve a set to concrete action ids
  search  <keyword>...          grep the local catalog (free, offline)
  cost    <set>...              price a batch before asking for approval
  rig     <mesh.glb|task_id>    5 cr -- one-time per character
  bake    <set> --rig <id> --out <dir> --yes-spend <n>     3 cr per clip
  merge   <set> --mesh <glb> --clips <dir> --out <glb>     FREE
  balance                       report remaining credits

TYPICAL RUN
-----------
  python tools/meshy_clip_library.py rig assets/characters/jake.glb --height 1.8
  python tools/meshy_clip_library.py cost x3_fps_combat
  python tools/meshy_clip_library.py bake x3_fps_combat --rig <rig_task_id> \
      --out assets/anim/meshy/x3_fps_combat --yes-spend 60
  python tools/meshy_clip_library.py merge x3_fps_combat \
      --mesh assets/characters/jake.glb \
      --clips assets/anim/meshy/x3_fps_combat \
      --out  assets/characters/jake_combat.glb
"""
import argparse, base64, json, mimetypes, os, subprocess, sys, time
import urllib.request, urllib.error

TOOLS = os.path.dirname(os.path.abspath(__file__))
CATALOG = os.path.join(TOOLS, "meshy_actions.json")
ROLE_SETS = os.path.join(TOOLS, "meshy_role_sets.json")
MERGE_JS = os.path.join(TOOLS, "glb-merge-anims.mjs")

API = "https://api.meshy.ai/openapi/v1"
RIG, ANIM = API + "/rigging", API + "/animations"

CREDITS_PER_CLIP = 3
CREDITS_PER_RIG = 5


# ---------------------------------------------------------------- key / http

def api_key():
    k = os.environ.get("MESHY_API_KEY")
    if k:
        return k.strip()
    for p in (r"//p13700/g/X3Native/fleet-handoff/meshy-key.txt",
              r"\\p13700\g\X3Native\fleet-handoff\meshy-key.txt",
              r"G:\X3Native\fleet-handoff\meshy-key.txt",
              os.path.expanduser("~/.claude/.meshy_key")):
        try:
            with open(p) as f:
                return f.read().strip()
        except OSError:
            continue
    sys.exit("no Meshy API key (set MESHY_API_KEY or provide meshy-key.txt)")


def call(method, url, body=None, timeout=300):
    hdr = {"Authorization": "Bearer " + api_key(), "Content-Type": "application/json"}
    req = urllib.request.Request(url, method=method, headers=hdr,
                                 data=json.dumps(body).encode() if body else None)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        sys.stderr.write(f"HTTP {e.code} {method} {url}\n{e.read().decode()[:800]}\n")
        raise


# ---------------------------------------------------------------- catalog

def load_catalog():
    if not os.path.exists(CATALOG):
        sys.exit(f"missing {CATALOG} -- run: python tools/meshy_scrape_actions.py")
    with open(CATALOG, encoding="utf-8") as f:
        doc = json.load(f)
    return doc["actions"], {a["name"]: a for a in doc["actions"]}


def load_sets():
    with open(ROLE_SETS, encoding="utf-8") as f:
        return json.load(f)["sets"]


def resolve(setname):
    """Role set -> [(action_id, meshy_name, engine_name)], validated against the catalog."""
    sets = load_sets()
    if setname not in sets:
        sys.exit(f"unknown role set '{setname}'. known: {', '.join(sorted(sets))}")
    _, byname = load_catalog()
    out, bad = [], []
    for meshy_name, engine_name in sets[setname]["clips"]:
        a = byname.get(meshy_name)
        if a is None:
            bad.append(meshy_name)
        else:
            out.append((a["id"], meshy_name, engine_name))
    if bad:
        sys.exit(f"role set '{setname}' references actions absent from the catalog: {bad}")
    cap = 20
    if len(out) > cap:
        sys.exit(f"role set '{setname}' has {len(out)} clips but Meshy's export cap is {cap}")
    return out


# ---------------------------------------------------------------- free cmds

def cmd_sets(_a):
    sets = load_sets()
    print(f"{'set':<18} {'project':<16} {'role':<9} {'clips':>5} {'credits':>8}")
    print("-" * 62)
    tot = 0
    for name in sorted(sets, key=lambda s: (sets[s]["project"], s)):
        s = sets[name]
        n = len(s["clips"])
        tot += n * CREDITS_PER_CLIP
        print(f"{name:<18} {s['project']:<16} {s['role']:<9} {n:>5} {n*CREDITS_PER_CLIP:>8}")
    print("-" * 62)
    print(f"{'ALL SETS':<18} {'':<16} {'':<9} {'':>5} {tot:>8}  (+{CREDITS_PER_RIG}/character to rig)")


def cmd_show(a):
    sets = load_sets()
    s = sets[a.set] if a.set in sets else sys.exit(f"unknown set {a.set}")
    print(f"# {a.set} -- {s['project']} / {s['role']}\n# {s['desc']}\n")
    rows = resolve(a.set)
    for aid, mname, ename in rows:
        print(f"  {aid:>4}  {mname:<40} -> {ename}")
    print(f"\n  {len(rows)} clips = {len(rows)*CREDITS_PER_CLIP} credits")
    print(f"  action ids: {','.join(str(r[0]) for r in rows)}")


def cmd_search(a):
    actions, _ = load_catalog()
    terms = [t.lower() for t in a.terms]
    hits = [x for x in actions
            if all(t in (x["name"] + " " + x["category"] + " " + x["subcategory"]).lower()
                   for t in terms)]
    if a.inplace_only:
        hits = [h for h in hits if h["inplace"]]
    for h in hits:
        flag = " [inplace]" if h["inplace"] else ""
        print(f"  {h['id']:>4}  {h['name']:<44} {h['category']}/{h['subcategory']}{flag}")
    print(f"\n{len(hits)} match(es) of {len(actions)} actions")


def cmd_cost(a):
    total = 0
    for name in a.sets:
        rows = resolve(name)
        c = len(rows) * CREDITS_PER_CLIP
        total += c
        print(f"  {name:<18} {len(rows):>3} clips  {c:>4} credits")
    print(f"\n  TOTAL {total} credits"
          f"  (+{CREDITS_PER_RIG} per character that still needs rigging)")
    print("  Merging is free and unlimited; only baking spends credits.")


def cmd_balance(_a):
    for path in ("/users/me", "/balance"):
        try:
            print(json.dumps(call("GET", "https://api.meshy.ai/openapi/v1" + path), indent=2))
            return
        except Exception:
            continue
    print("no balance endpoint responded; check the Meshy web dashboard")


# ---------------------------------------------------------------- paid cmds

def poll(url, label):
    while True:
        t = call("GET", url)
        st = t.get("status")
        print(f"  [{label}] {st} {t.get('progress', 0)}%", flush=True)
        if st == "SUCCEEDED":
            return t
        if st in ("FAILED", "CANCELED", "EXPIRED"):
            sys.exit(f"[{label}] {st}: {t.get('task_error')}")
        time.sleep(8)


def glb_url(t):
    res = t.get("result") if isinstance(t.get("result"), dict) else {}
    return (res.get("rigged_character_glb_url") or res.get("animation_glb_url")
            or t.get("animation_glb_url") or (t.get("model_urls") or {}).get("glb")
            or res.get("glb"))


def download(t, out):
    u = glb_url(t)
    if not u:
        sys.exit(f"no glb url in result: {json.dumps(t)[:500]}")
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    urllib.request.urlretrieve(u, out)
    print(f"  [write] {out}  {os.path.getsize(out)} bytes", flush=True)


def cmd_rig(a):
    if not a.yes_spend == CREDITS_PER_RIG:
        sys.exit(f"rigging costs {CREDITS_PER_RIG} credits -- "
                 f"re-run with --yes-spend {CREDITS_PER_RIG} to confirm")
    body = {"height_meters": a.height}
    if os.path.exists(a.src):
        mime = mimetypes.guess_type(a.src)[0] or "model/gltf-binary"
        with open(a.src, "rb") as f:
            body["model_url"] = f"data:{mime};base64," + base64.b64encode(f.read()).decode()
        print(f"[rig] uploading {a.src} ({os.path.getsize(a.src)} bytes)")
    else:
        body["input_task_id"] = a.src
    r = call("POST", RIG, body)
    tid = r.get("result") or r.get("id")
    print(f"[rig] task {tid}  (spent {CREDITS_PER_RIG} credits)")
    t = poll(f"{RIG}/{tid}", "rig")
    if a.out:
        download(t, a.out)
    print(f"\nRIG TASK ID (use this for `bake --rig`): {tid}")


def cmd_bake(a):
    rows = resolve(a.set)
    cost = len(rows) * CREDITS_PER_CLIP
    print(f"[bake] set '{a.set}': {len(rows)} clips x {CREDITS_PER_CLIP} = {cost} CREDITS")
    if a.yes_spend != cost:
        sys.exit(f"REFUSING to spend. This batch costs {cost} credits; "
                 f"re-run with --yes-spend {cost} to confirm.")
    os.makedirs(a.out, exist_ok=True)
    manifest, spent = [], 0
    for aid, mname, ename in rows:
        dest = os.path.join(a.out, f"{ename}.glb")
        if os.path.exists(dest) and not a.force:
            print(f"  [skip] {ename} already downloaded (0 credits)")
            manifest.append({"action_id": aid, "meshy_name": mname,
                             "engine_name": ename, "file": os.path.basename(dest)})
            continue
        r = call("POST", ANIM, {"rig_task_id": a.rig, "action_id": aid})
        tid = r.get("result") or r.get("id")
        spent += CREDITS_PER_CLIP
        print(f"  [anim] {ename} (action {aid}) task {tid}  -- {spent}/{cost} credits spent")
        download(poll(f"{ANIM}/{tid}", ename), dest)
        manifest.append({"action_id": aid, "meshy_name": mname,
                         "engine_name": ename, "file": os.path.basename(dest)})
    mpath = os.path.join(a.out, "clips.json")
    with open(mpath, "w", encoding="utf-8") as f:
        json.dump({"set": a.set, "rig_task_id": a.rig, "credits_spent": spent,
                   "clips": manifest}, f, indent=1)
    print(f"\n[bake] done. {len(manifest)} clips in {a.out}; {spent} credits spent this run.")
    print(f"[bake] manifest {mpath}")


# ---------------------------------------------------------------- free merge

def cmd_merge(a):
    """Fuse baked clip GLBs onto a character mesh. FREE -- no API, no credits."""
    rows = resolve(a.set) if a.set else None
    mpath = os.path.join(a.clips, "clips.json")
    if os.path.exists(mpath):
        with open(mpath, encoding="utf-8") as f:
            entries = json.load(f)["clips"]
        pairs = [(os.path.join(a.clips, e["file"]), e["engine_name"]) for e in entries]
    elif rows:
        pairs = [(os.path.join(a.clips, f"{e}.glb"), e) for _, _, e in rows]
    else:
        sys.exit(f"no clips.json in {a.clips} and no role set given")
    missing = [p for p, _ in pairs if not os.path.exists(p)]
    if missing:
        sys.exit(f"missing clip files: {missing}")

    cmd = ["node", MERGE_JS, "--mesh", a.mesh,
           "--clips", ",".join(p for p, _ in pairs),
           "--names", ",".join(n for _, n in pairs),
           "--out", a.out]
    if a.bone_prefix:
        cmd += ["--bone-prefix", a.bone_prefix]
    print("[merge]", " ".join(cmd[:2]), f"({len(pairs)} clips -> {a.out})")
    r = subprocess.run(cmd)
    if r.returncode != 0:
        sys.exit(r.returncode)
    print("[merge] FREE -- 0 credits spent.")


# ---------------------------------------------------------------- cli

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("sets", help="list role sets + credit costs").set_defaults(fn=cmd_sets)
    sub.add_parser("balance", help="report credit balance").set_defaults(fn=cmd_balance)

    p = sub.add_parser("show", help="resolve a role set to action ids")
    p.add_argument("set"); p.set_defaults(fn=cmd_show)

    p = sub.add_parser("search", help="search the offline catalog")
    p.add_argument("terms", nargs="+")
    p.add_argument("--inplace-only", action="store_true")
    p.set_defaults(fn=cmd_search)

    p = sub.add_parser("cost", help="price one or more role sets")
    p.add_argument("sets", nargs="+"); p.set_defaults(fn=cmd_cost)

    p = sub.add_parser("rig", help=f"rig a character ({CREDITS_PER_RIG} credits)")
    p.add_argument("src", help="path to a .glb, or an existing Meshy task id")
    p.add_argument("--height", type=float, default=1.75)
    p.add_argument("--out", help="also download the rigged GLB here")
    p.add_argument("--yes-spend", type=int, default=0)
    p.set_defaults(fn=cmd_rig)

    p = sub.add_parser("bake", help=f"bake a role set ({CREDITS_PER_CLIP} credits/clip)")
    p.add_argument("set")
    p.add_argument("--rig", required=True, help="rig_task_id")
    p.add_argument("--out", required=True, help="output directory for clip GLBs")
    p.add_argument("--yes-spend", type=int, default=0)
    p.add_argument("--force", action="store_true", help="re-bake clips already downloaded")
    p.set_defaults(fn=cmd_bake)

    p = sub.add_parser("merge", help="fuse clips onto a character mesh (FREE)")
    p.add_argument("set", nargs="?")
    p.add_argument("--mesh", required=True)
    p.add_argument("--clips", required=True, help="directory of baked clip GLBs")
    p.add_argument("--out", required=True)
    p.add_argument("--bone-prefix")
    p.set_defaults(fn=cmd_merge)

    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
