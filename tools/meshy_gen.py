#!/usr/bin/env python3
"""meshy_gen.py — generate a textured 3D asset via the Meshy API (text-to-3D).

Tim's premium Meshy pack. Key: MESHY_API_KEY env, else the fleet handoff file
(//p13700/g/X3Native/fleet-handoff/meshy-key.txt).

Flow (Meshy v2): POST preview (geometry) -> poll -> POST refine (PBR textures)
-> poll -> download GLB.

Usage:
  python meshy_gen.py "<prompt>" <out.glb> [--style realistic|sculpture]
"""
import json, os, sys, time, urllib.request

def key():
    k = os.environ.get("MESHY_API_KEY")
    if k: return k.strip()
    for p in (r"//p13700/g/X3Native/fleet-handoff/meshy-key.txt",
              r"\\p13700\g\X3Native\fleet-handoff\meshy-key.txt",
              os.path.expanduser("~/.claude/.meshy_key")):
        try:
            return open(p).read().strip()
        except OSError:
            continue
    sys.exit("no Meshy key found")

BASE = "https://api.meshy.ai/openapi/v2/text-to-3d"
HDR = {"Authorization": f"Bearer {key()}", "Content-Type": "application/json"}

def call(method, url, body=None):
    req = urllib.request.Request(url, method=method, headers=HDR,
                                 data=json.dumps(body).encode() if body else None)
    with urllib.request.urlopen(req, timeout=120) as r:
        return json.loads(r.read().decode())

def poll(task_id, label):
    while True:
        t = call("GET", f"{BASE}/{task_id}")
        st, pct = t.get("status"), t.get("progress", 0)
        print(f"[meshy] {label}: {st} {pct}%", flush=True)
        if st == "SUCCEEDED": return t
        if st in ("FAILED", "CANCELED"):
            sys.exit(f"[meshy] {label} {st}: {t.get('task_error')}")
        time.sleep(12)

def main():
    prompt, out = sys.argv[1], sys.argv[2]
    style = sys.argv[sys.argv.index("--style")+1] if "--style" in sys.argv else "realistic"
    pv = call("POST", BASE, {"mode": "preview", "prompt": prompt,
                             "art_style": style, "should_remesh": True,
                             "topology": "triangle", "target_polycount": 30000})
    pv_id = pv.get("result") or pv.get("id")
    print("[meshy] preview task:", pv_id, flush=True)
    poll(pv_id, "preview")
    rf = call("POST", BASE, {"mode": "refine", "preview_task_id": pv_id,
                             "enable_pbr": True})
    rf_id = rf.get("result") or rf.get("id")
    print("[meshy] refine task:", rf_id, flush=True)
    t = poll(rf_id, "refine")
    glb = (t.get("model_urls") or {}).get("glb")
    if not glb: sys.exit("[meshy] no glb url in result")
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    urllib.request.urlretrieve(glb, out)
    print("[meshy] WROTE", out, os.path.getsize(out), "bytes", flush=True)

if __name__ == "__main__":
    main()
