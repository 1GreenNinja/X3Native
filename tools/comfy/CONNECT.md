# comfy_texture.py — ComfyUI texture client (CONNECT runbook)

A tiny, dependency-free client that turns a text prompt into image(s) by driving
**any ComfyUI server's HTTP API** from a workflow you exported with ComfyUI's
**"Save (API Format)"**. Built so the fleet can point one command at the **5090
ComfyUI** (the FLUX texture forge, which lives on the **14900K's box** — the 5090,
a.k.a. "I9DevPc") over the LAN, or at a local ComfyUI.

## TL;DR

```bash
# point at the 5090 (14900K box) over the LAN:
set COMFY_URL=http://<14900K-LAN-IP>:8188            # Windows
export COMFY_URL=http://<14900K-LAN-IP>:8188         # bash
python comfy_texture.py "weathered gunmetal hull plating, rivets, recessed panel lines, tileable PBR base color" --workflow flux_texture_api.json --out textures

# or against a local ComfyUI (default url http://127.0.0.1:8188), bundled SD1.5 test workflow:
python comfy_texture.py "seamless sci-fi metal floor panel, scuffed, rivets, top-down"
```

Stdlib only (urllib) — runs on any Python 3, no `pip install`.

## What it does (the ComfyUI API it speaks)

1. `POST {url}/prompt` with `{"prompt": <graph>, "client_id": id}` → `{"prompt_id"}`
2. polls `GET {url}/history/{prompt_id}` until the run appears
3. for each output node's `images[]`, downloads `GET {url}/view?filename=..&subfolder=..&type=output` and saves the PNG

It auto-finds the **positive prompt node** (a `*TextEncode` node whose `_meta.title`
says "positive", else the first text-encode node) and injects your prompt. Override
with `--node <id>`. Flags: `--seed N`, `--width N`, `--height N`, `--out DIR`,
`--workflow file.json`, `--url http://host:port`.

## Getting the workflow JSON (one-time, per pipeline)

In the ComfyUI web UI, build/open the workflow you want (e.g. the FLUX-dev texture
graph), then **menu → Save (API Format)** → save the `.json`. That API-format file
is what `--workflow` consumes (it's `{node_id: {class_type, inputs, _meta}}`, NOT
the UI-format `.json`). Drop it next to this script and pass `--workflow`.

## Pointing at the 5090 (the real target)

The FLUX texture forge runs on the **14900K's box** (the 5090). To drive it from any
fleet machine you need three things from the 14900K (the owner) / StarForge:
1. the box's **LAN IP** + ComfyUI **port** (default 8188) → `COMFY_URL`
2. the **FLUX workflow** exported via Save (API Format) → `--workflow`
3. that the ComfyUI server is started with `--listen 0.0.0.0` so it accepts LAN
   connections (by default it only binds localhost)

No SSH needed — it's a plain LAN HTTP service.

## Verified

Tested end-to-end against a local ComfyUI (CPU mode) with the bundled
`workflow_sd15_api.json` (dreamshaper_8): prompt → generation → saved PNG. See
`sample_metal_panel.png` (a generated sci-fi floor panel). The same client + a FLUX
workflow + `COMFY_URL=<5090>` is the production path.

## Gotcha (local ComfyUI on a 1080 Ti)

This box's ComfyUI ships PyTorch **cu129**, which has **no CUDA kernels for Pascal
(GTX 1080 Ti, sm_61)** → GPU runs error `no kernel image is available for execution
on the device`. Workarounds on a 1080 Ti: run ComfyUI with `--cpu` (slow but works,
how the sample above was made), or install a Pascal-compatible torch. The **5090
(Ada, sm_89)** has no such issue — this is only a concern for local 1080 Ti runs.
```
