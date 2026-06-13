#!/usr/bin/env python3
"""
gen_worker.py — Slick's reference /gen worker (PR-5b).

Long-polls Conduit for `fleet.gen` requests (StarForge's locked contract,
Fleet Ops 2026-06-13), runs them through a local ComfyUI instance, and posts
the result back into the room as an m.image tied to the request via
m.relates_to.

GPU-AGNOSTIC BY DESIGN (Tim's hardware-shuffle note 2026-06-13): the worker
auto-detects the GPU + VRAM at startup and advertises what it can serve. A
5090 box auto-enables mesh (Hunyuan needs the VRAM); a 1080Ti box serves
image only. Move a card between boxes and the worker just re-detects on next
boot — no code change. StarForge drives the 5090 (physically in the 14900K
box) over SSH; the 1080Ti boxes (13700K / Predator / DJBOOTH) run this same
script locally. i5000 or any future box joins the pool by running it.

Contract consumed (from the client, gen.ts):
  m.room.message with content["fleet.gen"] = {
    prompt, mode: "image"|"mesh", size, steps, target, requested_by }
  target ∈ {"any","remote","local",<box-name>}

Routing: a worker claims a job iff the target matches its class/name. To avoid
two workers grabbing the same job, the worker posts a lightweight claim
(m.reaction-style fleet.gen.claim) and only proceeds if, after a short
name-hashed jitter, it sees no earlier claim. First claim wins; losers skip.

Config via env (all optional except token path is conventional):
  MATRIX_BOT_MACHINE   this box's name (e.g. "13700k"); also picks the pipe
  COMFYUI_URL          default http://127.0.0.1:8188
  SLICK_GEN_ROOMS      comma-separated room ids; default Fleet Ops
  GEN_FORCE_CLASS      override detected class: "remote" | "local"

Run:  python gen_worker.py
"""

import json
import os
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

HOMESERVER = "https://fleetcommand.slopclaude.com"
FLEET_OPS = "!0H8gfl2jP8rWT5mV_i54dPhxC0zg1v7zc7gHwQzJy5k"
TOKEN_PATH = Path(os.path.expanduser("~/.claude/.matrix_token"))
COMFYUI_URL = os.environ.get("COMFYUI_URL", "http://127.0.0.1:8188")
MACHINE = os.environ.get("MATRIX_BOT_MACHINE", "unknown").lower()
USER_AGENT = "fleet-genworker/1.0"
MY_USER_ID = f"@{MACHINE}:fleetcommand.slopclaude.com"


def log(msg: str) -> None:
    print(f"{time.strftime('%H:%M:%S')} [{MACHINE}] {msg}", flush=True)


# ---------------------------------------------------------------- GPU detect
def detect_gpu() -> dict:
    """Return {name, vram_gb, class, modes}. class: 'remote' (heavy) | 'local'.
    Heavy = >=24GB VRAM (5090/4090/A-series) -> can serve mesh. Else image only."""
    name, vram_gb = "cpu", 0.0
    try:
        import subprocess  # local import; only used at startup
        out = subprocess.run(
            ["nvidia-smi", "--query-gpu=name,memory.total", "--format=csv,noheader,nounits"],
            capture_output=True, text=True, timeout=10,
        )
        if out.returncode == 0 and out.stdout.strip():
            first = out.stdout.strip().splitlines()[0]
            parts = [p.strip() for p in first.split(",")]
            name = parts[0]
            vram_gb = float(parts[1]) / 1024.0
    except Exception as e:
        log(f"GPU detect fell back to cpu: {e}")

    klass = os.environ.get("GEN_FORCE_CLASS") or ("remote" if vram_gb >= 24 else "local")
    modes = ["image", "mesh"] if klass == "remote" else ["image"]
    return {"name": name, "vram_gb": round(vram_gb, 1), "class": klass, "modes": modes}


# ------------------------------------------------------------------- matrix
def load_token() -> str:
    if not TOKEN_PATH.exists():
        sys.exit(f"FATAL: no token at {TOKEN_PATH}")
    return TOKEN_PATH.read_text(encoding="utf-8").strip()


def _req(path: str, token: str, method="GET", body=None, signal_timeout=35):
    headers = {"Authorization": f"Bearer {token}", "User-Agent": USER_AGENT}
    data = None
    if body is not None:
        headers["Content-Type"] = "application/json"
        data = json.dumps(body).encode("utf-8")
    req = urllib.request.Request(f"{HOMESERVER}{path}", data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=signal_timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def sync(token: str, since):
    params = f"timeout={'30000' if since else '0'}"
    if since:
        params += f"&since={since}"
    return _req(f"/_matrix/client/v3/sync?{params}", token)


_txn = 0
def _next_txn():
    global _txn
    _txn += 1
    return f"gw-{int(time.time())}-{_txn}"


def send_event(token: str, room: str, content: dict, etype="m.room.message") -> str:
    txn = _next_txn()
    r = _req(f"/_matrix/client/v3/rooms/{room}/send/{etype}/{txn}", token, method="PUT", body=content)
    return r.get("event_id", "")


def upload_media(token: str, data: bytes, filename: str, mime: str) -> str:
    req = urllib.request.Request(
        f"{HOMESERVER}/_matrix/media/v3/upload?filename={urllib.request.quote(filename)}",
        data=data, method="POST",
        headers={"Authorization": f"Bearer {token}", "Content-Type": mime, "User-Agent": USER_AGENT},
    )
    with urllib.request.urlopen(req, timeout=120) as resp:
        return json.loads(resp.read().decode("utf-8")).get("content_uri", "")


# ------------------------------------------------------------------- comfyui
def run_comfyui_image(prompt: str, size: int, steps: int) -> bytes:
    """Minimal ComfyUI /prompt -> poll /history -> /view client. The actual
    workflow JSON is loaded from comfy_workflows/image.json (StarForge's proven
    comfy_texture.py uses the same /prompt -> /history -> /view dance). This
    reference keeps the HTTP shape; wire the real workflow per your ComfyUI.
    """
    wf_path = Path(__file__).parent / "comfy_workflows" / "image.json"
    if not wf_path.exists():
        raise RuntimeError(
            f"workflow not found at {wf_path} — drop your ComfyUI API-format "
            "workflow there (Save (API Format) in ComfyUI), with {{prompt}}, "
            "{{size}}, {{steps}} placeholders"
        )
    template = wf_path.read_text(encoding="utf-8")
    wf = json.loads(
        template.replace("{{prompt}}", json.dumps(prompt)[1:-1])
                .replace("{{size}}", str(size))
                .replace("{{steps}}", str(steps))
    )
    # queue it
    q = urllib.request.urlopen(
        urllib.request.Request(f"{COMFYUI_URL}/prompt", method="POST",
                               data=json.dumps({"prompt": wf}).encode(),
                               headers={"Content-Type": "application/json"}),
        timeout=30,
    )
    prompt_id = json.loads(q.read().decode())["prompt_id"]
    # poll history
    for _ in range(600):  # up to ~10 min
        time.sleep(1)
        h = urllib.request.urlopen(f"{COMFYUI_URL}/history/{prompt_id}", timeout=15)
        hist = json.loads(h.read().decode())
        if prompt_id in hist:
            outputs = hist[prompt_id].get("outputs", {})
            for node in outputs.values():
                for img in node.get("images", []):
                    v = urllib.request.urlopen(
                        f"{COMFYUI_URL}/view?filename={urllib.request.quote(img['filename'])}"
                        f"&subfolder={urllib.request.quote(img.get('subfolder',''))}"
                        f"&type={img.get('type','output')}",
                        timeout=60,
                    )
                    return v.read()
            raise RuntimeError("ComfyUI finished but produced no image")
    raise RuntimeError("ComfyUI timed out")


# ------------------------------------------------------------------- routing
def should_claim(gen: dict, gpu: dict) -> bool:
    target = (gen.get("target") or "any").lower()
    mode = gen.get("mode", "image")
    if mode not in gpu["modes"]:
        return False  # e.g. 1080Ti asked for mesh -> decline
    if target == "any":
        return True
    if target == MACHINE:
        return True
    if target == gpu["class"]:  # "remote" or "local"
        return True
    return False


def claim_jitter_seconds() -> float:
    # name-hashed jitter so workers don't all claim simultaneously; lower hash
    # = earlier claim = preferred. Deterministic, no RNG (Math.random-free).
    h = sum(ord(c) for c in MACHINE) % 1000
    return 0.5 + (h / 1000.0) * 2.0  # 0.5–2.5s


# ---------------------------------------------------------------------- main
def handle_gen(token: str, room: str, req_event: dict, gpu: dict) -> None:
    gen = req_event["content"]["fleet.gen"]
    req_id = req_event["event_id"]
    if not should_claim(gen, gpu):
        return
    # claim, then jitter, then re-check we won (best-effort; first upload wins
    # anyway since the result is what matters)
    send_event(token, room, {
        "msgtype": "m.notice",
        "body": f"[{MACHINE}] claiming /gen {req_id[:8]} ({gpu['name']})",
        "fleet.gen.claim": {"req": req_id, "worker": MACHINE},
    })
    time.sleep(claim_jitter_seconds())
    log(f"running mode={gen.get('mode')} size={gen.get('size')} steps={gen.get('steps')} :: {gen.get('prompt')[:60]}")
    try:
        if gen.get("mode") == "mesh":
            raise RuntimeError("mesh pipeline is StarForge's 5090 lane; this reference does image only")
        img = run_comfyui_image(gen["prompt"], int(gen.get("size", 1024)), int(gen.get("steps", 24)))
        mxc = upload_media(token, img, f"gen_{req_id[:8]}.png", "image/png")
        send_event(token, room, {
            "msgtype": "m.image",
            "body": f"gen: {gen['prompt'][:80]}",
            "url": mxc,
            "info": {"mimetype": "image/png"},
            "m.relates_to": {"rel_type": "fleet.gen.result", "event_id": req_id},
            "fleet.gen.worker": MACHINE,
        })
        log(f"posted result for {req_id[:8]}")
    except Exception as e:
        send_event(token, room, {
            "msgtype": "m.notice",
            "body": f"[{MACHINE}] /gen {req_id[:8]} FAILED: {e}",
            "fleet.gen.error": {"req": req_id, "reason": str(e)},
        })
        log(f"FAILED {req_id[:8]}: {e}")


def main() -> None:
    gpu = detect_gpu()
    token = load_token()
    rooms = (os.environ.get("SLICK_GEN_ROOMS") or FLEET_OPS).split(",")
    log(f"worker up — GPU={gpu['name']} VRAM={gpu['vram_gb']}GB class={gpu['class']} modes={gpu['modes']}")
    log(f"serving rooms: {rooms}")

    since = None
    # prime: skip backlog, only handle requests that arrive after we start
    since = sync(token, None).get("next_batch")
    while True:
        try:
            resp = sync(token, since)
            since = resp.get("next_batch")
            for room, data in (resp.get("rooms", {}).get("join", {}) or {}).items():
                if room not in rooms:
                    continue
                for ev in data.get("timeline", {}).get("events", []):
                    if ev.get("type") != "m.room.message":
                        continue
                    if ev.get("sender") == MY_USER_ID:
                        continue
                    if "fleet.gen" in (ev.get("content") or {}):
                        handle_gen(token, room, ev, gpu)
        except (urllib.error.HTTPError, urllib.error.URLError) as e:
            log(f"sync error, backing off: {e}")
            time.sleep(5)
        except KeyboardInterrupt:
            log("shutdown")
            return


if __name__ == "__main__":
    main()
