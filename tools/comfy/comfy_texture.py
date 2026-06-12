#!/usr/bin/env python3
"""comfy_texture.py — minimal ComfyUI client: prompt -> image(s).

Drives ANY ComfyUI server's HTTP API from a workflow exported via ComfyUI's
"Save (API Format)". Injects your prompt into the workflow's positive-prompt
node, queues it, polls until done, and downloads the output images. Points at the
14900K's 5090/FLUX ComfyUI over the LAN (set COMFY_URL), or a local ComfyUI.

Usage:
  python comfy_texture.py "<prompt>" [--url http://HOST:8188] [--workflow wf.json]
                          [--out DIR] [--node <id>] [--neg <id>] [--seed N]
                          [--width N] [--height N]

Env: COMFY_URL overrides --url (default http://127.0.0.1:8188).

The ComfyUI HTTP API this speaks:
  POST {url}/prompt        body {"prompt": <graph>, "client_id": id}  -> {"prompt_id": ...}
  GET  {url}/history/{id}  -> once present, .outputs[node].images[] = {filename,subfolder,type}
  GET  {url}/view?filename=..&subfolder=..&type=output  -> the PNG bytes

Stdlib only (urllib) so it runs anywhere with no pip install.
"""
from __future__ import annotations
import sys, os, json, time, uuid, argparse
import urllib.request, urllib.parse, urllib.error


def http_json(method: str, url: str, body=None, timeout=120):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method,
                                 headers={'Content-Type': 'application/json'})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as e:
        sys.exit(f"comfy_texture: HTTP {e.code} on {method} {url}: {e.read()[:300]!r}")
    except urllib.error.URLError as e:
        sys.exit(f"comfy_texture: cannot reach {url} ({e.reason}). Is ComfyUI up + reachable?")


def http_bytes(url: str, timeout=120) -> bytes:
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return r.read()


def find_prompt_node(wf: dict, explicit=None):
    """Pick the positive CLIP-text node to inject into. API-format workflow is
    {node_id: {class_type, inputs, _meta}}. Prefer a *Text* node whose _meta title
    says 'positive'; else the first text-encode node with a 'text' string input."""
    if explicit and explicit in wf:
        return explicit
    cands = [(nid, n) for nid, n in wf.items()
             if 'text' in n.get('inputs', {}) and isinstance(n['inputs'].get('text'), str)
             and 'TextEncode' in n.get('class_type', '')]
    if not cands:
        cands = [(nid, n) for nid, n in wf.items()
                 if isinstance(n.get('inputs', {}).get('text'), str)]
    for nid, n in cands:
        if 'pos' in (n.get('_meta', {}) or {}).get('title', '').lower():
            return nid
    return cands[0][0] if cands else None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('prompt')
    ap.add_argument('--url', default=os.environ.get('COMFY_URL', 'http://127.0.0.1:8188'))
    ap.add_argument('--workflow', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'workflow_sd15_api.json'))
    ap.add_argument('--out', default='comfy_out')
    ap.add_argument('--node', default=None, help='positive-prompt node id to inject into')
    ap.add_argument('--neg', default=None, help='negative-prompt node id (skip injection)')
    ap.add_argument('--seed', type=int, default=None)
    ap.add_argument('--width', type=int, default=None)
    ap.add_argument('--height', type=int, default=None)
    a = ap.parse_args()
    url = a.url.rstrip('/')

    with open(a.workflow, 'r', encoding='utf-8') as f:
        wf = json.load(f)
    if 'prompt' in wf and isinstance(wf['prompt'], dict):   # tolerate {"prompt": {...}}
        wf = wf['prompt']

    node = find_prompt_node(wf, a.node)
    if not node:
        sys.exit("comfy_texture: couldn't find a prompt node — pass --node <id>")
    if a.neg and a.neg == node:
        sys.exit("comfy_texture: --node and --neg are the same node")
    wf[node]['inputs']['text'] = a.prompt
    print(f"[comfy] prompt -> node {node} ({wf[node].get('class_type')})")

    for nid, n in wf.items():
        inp = n.get('inputs', {})
        if a.seed is not None:
            if 'seed' in inp: inp['seed'] = a.seed
            if 'noise_seed' in inp: inp['noise_seed'] = a.seed
        if a.width is not None and 'width' in inp: inp['width'] = a.width
        if a.height is not None and 'height' in inp: inp['height'] = a.height

    client_id = uuid.uuid4().hex
    res = http_json('POST', url + '/prompt', {'prompt': wf, 'client_id': client_id})
    pid = res.get('prompt_id')
    if not pid:
        sys.exit(f"comfy_texture: server rejected the workflow: {res}")
    print(f"[comfy] queued {pid} @ {url} — generating...")

    hist = None
    for _ in range(900):                         # up to ~15 min for big FLUX gens
        h = http_json('GET', url + f'/history/{pid}')
        if pid in h:
            hist = h[pid]; break
        time.sleep(1)
    if not hist:
        sys.exit("comfy_texture: timed out waiting for the result")

    os.makedirs(a.out, exist_ok=True)
    saved = []
    for nid, out in hist.get('outputs', {}).items():
        for img in out.get('images', []):
            q = urllib.parse.urlencode({'filename': img['filename'],
                                        'subfolder': img.get('subfolder', ''),
                                        'type': img.get('type', 'output')})
            data = http_bytes(url + '/view?' + q)
            p = os.path.join(a.out, img['filename'])
            with open(p, 'wb') as f:
                f.write(data)
            saved.append(p)
            print(f"[comfy] saved {p} ({len(data)} bytes)")
    if not saved:
        sys.exit("comfy_texture: finished but returned no images (check the workflow's SaveImage node)")
    print(f"[comfy] DONE — {len(saved)} image(s) in {a.out}/")
    return 0


if __name__ == '__main__':
    sys.exit(main())
