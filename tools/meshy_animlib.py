#!/usr/bin/env python3
"""Meshy Animation-Library driver for X3Native character clips.

Flow (docs.meshy.ai/en/api/animation):
  1. POST /openapi/v1/rigging  {model_url: data-URI GLB, height_meters}
     -> rig task (5 cr) with rig_task_id + free basic Walking/Running clips.
  2. POST /openapi/v1/animations {rig_task_id, action_id}  (~3 cr each)
     -> one library clip baked onto that rig, downloadable GLB.

Commands:
  python tools/meshy_animlib.py balance
  python tools/meshy_animlib.py rig <base_glb> <height_m> <out_dir> <name>
        rigs the mesh, writes <out_dir>/<name>_rig.json (full task incl.
        rig_task_id + consumed_credits), downloads <name>_rigged.glb and the
        free basic clips <name>_basic_Walk.glb / <name>_basic_Run.glb.
  python tools/meshy_animlib.py animate <spec.json> <out_dir>
        spec = {"rig_task_id": "...",
                "prefix": "grey",
                "clips": [{"name":"Idle","action_id":0}, ...]}
        Submits ALL clips, then polls; downloads <prefix>_<name>.glb per clip;
        writes <out_dir>/<prefix>_animate_result.json with per-task credits.

Key discovery mirrors meshy_gen.py (fleet share fallback).
"""
import sys, os, json, time, base64, urllib.request, urllib.error

RIG_API  = "https://api.meshy.ai/openapi/v1/rigging"
ANIM_API = "https://api.meshy.ai/openapi/v1/animations"
BAL_API  = "https://api.meshy.ai/openapi/v1/balance"

CANDIDATE_KEY_PATHS = [
    os.environ.get("MESHY_KEY_PATH"),
    os.path.join(os.getcwd(), "tools", "meshy-key.txt"),
    r"G:\X3Native\fleet-handoff\meshy-key.txt",
]

def find_key():
    for p in CANDIDATE_KEY_PATHS:
        if p and os.path.isfile(p):
            return open(p).read().strip()
    raise SystemExit("no Meshy key found; looked in " + repr(CANDIDATE_KEY_PATHS))

def req(key, url, method="GET", body=None):
    data = json.dumps(body).encode() if body is not None else None
    r = urllib.request.Request(url, data=data, method=method,
        headers={"Authorization": f"Bearer {key}", "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(r, timeout=300) as resp:
            return json.loads(resp.read())
    except urllib.error.HTTPError as e:
        detail = e.read().decode(errors="replace")[:500]
        raise SystemExit(f"HTTP {e.code} on {method} {url}: {detail}")

def download(url, path):
    urllib.request.urlretrieve(url, path)
    print(f"  downloaded {os.path.basename(path)} ({os.path.getsize(path)} bytes)")

def poll(key, url_base, task_id, label, interval=5, timeout_s=900):
    t0 = time.time()
    while True:
        t = req(key, f"{url_base}/{task_id}")
        st = t.get("status")
        if st in ("SUCCEEDED", "FAILED", "CANCELED"):
            print(f"  [{label}] {st} after {int(time.time()-t0)}s "
                  f"(credits: {t.get('consumed_credits')})")
            return t
        if time.time() - t0 > timeout_s:
            raise SystemExit(f"[{label}] poll timeout; last status {st}")
        time.sleep(interval)

def cmd_balance(key):
    print(req(key, BAL_API))

def cmd_rig(key, glb_path, height_m, out_dir, name):
    os.makedirs(out_dir, exist_ok=True)
    raw = open(glb_path, "rb").read()
    uri = "data:model/gltf-binary;base64," + base64.b64encode(raw).decode()
    print(f"rigging {os.path.basename(glb_path)} ({len(raw)} bytes, "
          f"height {height_m} m) ...")
    resp = req(key, RIG_API, "POST", {"model_url": uri,
                                      "height_meters": float(height_m)})
    task_id = resp.get("result") or resp.get("id")
    print("  rig task:", task_id)
    t = poll(key, RIG_API, task_id, name)
    with open(os.path.join(out_dir, f"{name}_rig.json"), "w") as f:
        json.dump(t, f, indent=2)
    if t["status"] != "SUCCEEDED":
        raise SystemExit(f"rig FAILED: {t.get('task_error')}")
    res = t["result"]
    download(res["rigged_character_glb_url"],
             os.path.join(out_dir, f"{name}_rigged.glb"))
    basic = res.get("basic_animations") or {}
    if basic.get("walking_glb_url"):
        download(basic["walking_glb_url"],
                 os.path.join(out_dir, f"{name}_basic_Walk.glb"))
    if basic.get("running_glb_url"):
        download(basic["running_glb_url"],
                 os.path.join(out_dir, f"{name}_basic_Run.glb"))
    print(f"RIG OK {name}: rig_task_id={task_id} credits={t.get('consumed_credits')}")

def cmd_animate(key, spec_path, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    spec = json.load(open(spec_path))
    rig_id = spec["rig_task_id"]; prefix = spec.get("prefix", "clip")
    tasks = []
    for c in spec["clips"]:
        resp = req(key, ANIM_API, "POST",
                   {"rig_task_id": rig_id, "action_id": int(c["action_id"])})
        tid = resp.get("result") or resp.get("id")
        print(f"submitted {prefix}_{c['name']} (action {c['action_id']}) -> {tid}")
        tasks.append((c, tid))
    results, total = [], 0
    for c, tid in tasks:
        t = poll(key, ANIM_API, tid, f"{prefix}_{c['name']}")
        rec = {"name": c["name"], "action_id": c["action_id"], "task_id": tid,
               "status": t["status"], "credits": t.get("consumed_credits"),
               "error": t.get("task_error")}
        if t["status"] == "SUCCEEDED":
            url = t["result"].get("animation_glb_url")
            dest = os.path.join(out_dir, f"{prefix}_{c['name']}.glb")
            download(url, dest)
            rec["glb"] = dest
            total += t.get("consumed_credits") or 0
        results.append(rec)
    with open(os.path.join(out_dir, f"{prefix}_animate_result.json"), "w") as f:
        json.dump(results, f, indent=2)
    print(f"ANIMATE DONE {prefix}: {sum(1 for r in results if r['status']=='SUCCEEDED')}"
          f"/{len(results)} ok, credits consumed {total}")

def main():
    key = find_key()
    argv = sys.argv[1:]
    if not argv:
        raise SystemExit(__doc__)
    cmd = argv[0]
    if cmd == "balance":
        cmd_balance(key)
    elif cmd == "rig":
        cmd_rig(key, argv[1], argv[2], argv[3], argv[4])
    elif cmd == "animate":
        cmd_animate(key, argv[1], argv[2])
    else:
        raise SystemExit("unknown command " + cmd)

if __name__ == "__main__":
    main()
