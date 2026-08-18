#!/usr/bin/env python3
"""meshy_gen.py - generate, rig, and animate 3D assets via the Meshy API.

Tim's premium Meshy pack. Key: MESHY_API_KEY env, else the fleet handoff file
(//p13700/g/X3Native/fleet-handoff/meshy-key.txt).

Two-stage text-to-3D (per fleet doctrine: inspect geometry BEFORE spending
texture credits) plus rigging + per-clip animation for characters, and a
one-shot `full` mode for when a quick static prop is genuinely fine.

Subcommands:
  balance
  preview  "<prompt>" <out_preview.glb> [--style realistic|sculpture] [--polycount N]
  refine   <preview_task_id> "<texture_prompt>" <out_final.glb>
  full     "<prompt>" <out.glb> [--style realistic|sculpture]   (legacy one-shot)
  rig      <input_task_id> <out_rigged.glb> [--height 1.75]
  anim     <rig_task_id> <action_id> <out_clip.glb>

preview/refine/full print "TASK_ID <id>" on their own line so callers can
grab the id from stdout. rig prints "RIG_TASK_ID <id>" plus any basic
walking/running clip URLs Meshy bundles for free.
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

HDR = {"Authorization": f"Bearer {key()}", "Content-Type": "application/json"}
T3D = "https://api.meshy.ai/openapi/v2/text-to-3d"
RIG = "https://api.meshy.ai/openapi/v1/rigging"
ANIM = "https://api.meshy.ai/openapi/v1/animations"
BAL = "https://api.meshy.ai/openapi/v1/balance"

def call(method, url, body=None):
    req = urllib.request.Request(url, method=method, headers=HDR,
                                 data=json.dumps(body).encode() if body else None)
    with urllib.request.urlopen(req, timeout=120) as r:
        return json.loads(r.read().decode())

def poll(base, task_id, label, interval=12, timeout=1800):
    t0 = time.time()
    while True:
        t = call("GET", f"{base}/{task_id}")
        st, pct = t.get("status"), t.get("progress", 0)
        print(f"[meshy] {label}: {st} {pct}%", flush=True)
        if st == "SUCCEEDED": return t
        if st in ("FAILED", "CANCELED"):
            sys.exit(f"[meshy] {label} {st}: {t.get('task_error')}")
        if time.time() - t0 > timeout:
            sys.exit(f"[meshy] {label} TIMEOUT after {timeout}s")
        time.sleep(interval)

def download(url, out):
    os.makedirs(os.path.dirname(os.path.abspath(out)) or ".", exist_ok=True)
    urllib.request.urlretrieve(url, out)
    print("[meshy] WROTE", out, os.path.getsize(out), "bytes", flush=True)

def opt(args, name, default=None, cast=str):
    return cast(args[args.index(name) + 1]) if name in args else default

def cmd_balance(args):
    print(json.dumps(call("GET", BAL), indent=2))

def cmd_preview(args):
    prompt, out = args[0], args[1]
    style = opt(args, "--style", "realistic")
    polycount = opt(args, "--polycount", 30000, int)
    pv = call("POST", T3D, {"mode": "preview", "prompt": prompt,
                             "art_style": style, "should_remesh": True,
                             "topology": "triangle", "target_polycount": polycount})
    pv_id = pv.get("result") or pv.get("id")
    print("TASK_ID", pv_id, flush=True)
    t = poll(T3D, pv_id, "preview")
    glb = (t.get("model_urls") or {}).get("glb")
    thumb = t.get("thumbnail_url")
    if glb: download(glb, out)
    if thumb: print("[meshy] thumbnail_url:", thumb)

def cmd_refine(args):
    pv_id, texture_prompt, out = args[0], args[1], args[2]
    rf = call("POST", T3D, {"mode": "refine", "preview_task_id": pv_id,
                             "texture_prompt": texture_prompt, "enable_pbr": True})
    rf_id = rf.get("result") or rf.get("id")
    print("TASK_ID", rf_id, flush=True)
    t = poll(T3D, rf_id, "refine")
    glb = (t.get("model_urls") or {}).get("glb")
    if not glb: sys.exit("[meshy] no glb url in refine result")
    download(glb, out)

def cmd_full(args):
    prompt, out = args[0], args[1]
    style = opt(args, "--style", "realistic")
    pv = call("POST", T3D, {"mode": "preview", "prompt": prompt,
                             "art_style": style, "should_remesh": True,
                             "topology": "triangle", "target_polycount": 30000})
    pv_id = pv.get("result") or pv.get("id")
    print("[meshy] preview task:", pv_id, flush=True)
    poll(T3D, pv_id, "preview")
    rf = call("POST", T3D, {"mode": "refine", "preview_task_id": pv_id, "enable_pbr": True})
    rf_id = rf.get("result") or rf.get("id")
    print("TASK_ID", rf_id, flush=True)
    t = poll(T3D, rf_id, "refine")
    glb = (t.get("model_urls") or {}).get("glb")
    if not glb: sys.exit("[meshy] no glb url in result")
    download(glb, out)

def cmd_rig(args):
    input_task_id, out = args[0], args[1]
    height = opt(args, "--height", 1.75, float)
    rg = call("POST", RIG, {"input_task_id": input_task_id, "height_meters": height})
    rg_id = rg.get("result") or rg.get("id")
    print("RIG_TASK_ID", rg_id, flush=True)
    t = poll(RIG, rg_id, "rig")
    glb = t.get("rigged_character_glb_url")
    if not glb: sys.exit(f"[meshy] no rigged glb url; keys={list(t.keys())}")
    download(glb, out)
    ba = t.get("basic_animations") or {}
    for clip in ("walking", "running"):
        url = (ba.get(clip) or {}).get("glb_url") if isinstance(ba.get(clip), dict) else None
        if url:
            print(f"[meshy] basic clip '{clip}':", url)

def cmd_anim(args):
    rig_task_id, action_id, out = args[0], int(args[1]), args[2]
    an = call("POST", ANIM, {"rig_task_id": rig_task_id, "action_id": action_id})
    an_id = an.get("result") or an.get("id")
    print("ANIM_TASK_ID", an_id, flush=True)
    t = poll(ANIM, an_id, "anim", interval=6)
    glb = (t.get("result") or {}).get("animation_glb_url") or t.get("animation_glb_url")
    if not glb: sys.exit(f"[meshy] no animation glb url; keys={list(t.keys())} result_keys={list((t.get('result') or {}).keys())}")
    download(glb, out)

def cmd_rigfile(args):
    """rigfile <local.glb> <out_rigged.glb> [--height 1.75] [--spec out.json]

    Rig a mesh WE already own, rather than one Meshy generated. `rig` above only
    accepts an input_task_id, so it can only ever re-rig Meshy's own output — but
    the assets that actually need rigging here are the cast models in
    assets/rigged_glb, and the reason they need it is that their existing skin
    WEIGHTS are degenerate: they hold a standing idle and then collapse into
    stretched sheets under any real joint flexion (the ward bent-over pose
    reproduces it every time). Meshy's rigging endpoint takes a `model_url`, and a
    data: URI is a URL — the route the 2026-07-25 pass proved. ~5 credits, ~20 s.

    The task JSON is written next to the output (or to --spec) so the skeleton, the
    credit cost and the expiring asset URLs stay on the record.
    """
    import base64
    src, out = args[0], args[1]
    height = opt(args, "--height", 1.75, float)
    spec = opt(args, "--spec", os.path.splitext(out)[0] + "_rig.json")
    blob = open(src, "rb").read()
    print(f"[meshy] rigging {os.path.basename(src)} ({len(blob)} bytes) "
          f"at height {height} m", flush=True)
    uri = "data:model/gltf-binary;base64," + base64.b64encode(blob).decode()
    rg = call("POST", RIG, {"model_url": uri, "height_meters": height})
    rg_id = rg.get("result") or rg.get("id")
    print("RIG_TASK_ID", rg_id, flush=True)
    t = poll(RIG, rg_id, "rig")
    res = t.get("result") or {}
    glb = res.get("rigged_character_glb_url") or t.get("rigged_character_glb_url")
    if not glb:
        sys.exit(f"[meshy] no rigged glb url; keys={list(t.keys())} / {list(res.keys())}")
    download(glb, out)
    os.makedirs(os.path.dirname(os.path.abspath(spec)) or ".", exist_ok=True)
    with open(spec, "w", encoding="utf-8") as f:
        json.dump(t, f, indent=2)
    print("[meshy] spec ->", spec, "| consumed_credits:",
          t.get("consumed_credits"), flush=True)


CMDS = {"balance": cmd_balance, "preview": cmd_preview, "refine": cmd_refine,
        "full": cmd_full, "rig": cmd_rig, "rigfile": cmd_rigfile, "anim": cmd_anim}

def main():
    if len(sys.argv) < 2 or sys.argv[1] not in CMDS:
        sys.exit(f"usage: meshy_gen.py <{'|'.join(CMDS)}> ...  (see module docstring)")
    CMDS[sys.argv[1]](sys.argv[2:])

if __name__ == "__main__":
    main()
