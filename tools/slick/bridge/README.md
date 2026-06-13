# Slick /gen workers

Reference worker for Slick's `/gen` (PR-5b). Long-polls Conduit for the
`fleet.gen` contract, runs ComfyUI locally, posts the result back in-room.

## Who runs what

| Box | GPU | Runs | Serves |
|---|---|---|---|
| 14900K | RTX 5090 | StarForge's gateway (driven over SSH) | image **+ mesh** (FLUX→rembg→Hunyuan→texture) |
| 13700K | 1080Ti ×2 | `gen_worker.py` | image |
| Predator | 1080Ti ×2 | `gen_worker.py` | image |
| DJBOOTH | 1080Ti ×1 | `gen_worker.py` | image |
| i5000 / idle boxes | whatever lands | `gen_worker.py` | auto-detected |

**GPU-agnostic:** the worker runs `nvidia-smi` at startup, reads VRAM, and
picks its class — ≥24 GB → `remote` (mesh-capable), else `local` (image).
Move a card between boxes and it just re-detects on next boot. Tim's
hardware-shuffle plan (redistribute 1080Ti's, upgrade to 50-series, cascade
Pascals to idle boxes / i5000) needs zero code changes here.

## Prereqs

- Snake's cu121 torch fix for Pascal (1080Ti = sm_61):
  `pip install torch==2.5.1 torchvision==0.20.1 --index-url https://download.pytorch.org/whl/cu121`
- ComfyUI running locally (default `http://127.0.0.1:8188`)
- A ComfyUI API-format workflow saved at `bridge/comfy_workflows/image.json`
  with `{{prompt}}`, `{{size}}`, `{{steps}}` placeholders (Save (API Format)
  in ComfyUI, then template the three fields)
- `~/.claude/.matrix_token` present; `MATRIX_BOT_MACHINE` set to the box name

## Run

```powershell
$env:MATRIX_BOT_MACHINE = "13700k"      # or predator / djbooth / i5000
$env:COMFYUI_URL = "http://127.0.0.1:8188"
python G:\X3Native\tools\slick\bridge\gen_worker.py
```

Install as a Scheduled Task `Fleet-GenWorker-<BOX>` for persistence (same
pattern as the matrix daemon). The worker primes past the backlog on start —
it only handles requests that arrive after it's up.

## Routing

The client's `/gen` panel sets `target`:
- `any` — first worker whose class/modes match claims it
- `remote` — StarForge's 5090 (mesh-capable)
- `local` — any 1080Ti
- `<box>` — a specific box by name

Workers post a `fleet.gen.claim` notice before running and jitter by a
name-hash so they don't all grab the same job; first result uploaded wins.
Mesh requests are declined by image-only (1080Ti) workers automatically.

## Contract (locked by StarForge, Fleet Ops 2026-06-13)

Request — a normal `m.room.message` so it shows in any client:
```json
{
  "msgtype": "m.text",
  "body": "/gen <prompt>",
  "fleet.gen": { "prompt": "...", "mode": "image|mesh", "size": 1024,
                 "steps": 24, "target": "any", "requested_by": "@tim:..." }
}
```
Result — `m.image` related back to the request:
```json
{
  "msgtype": "m.image", "url": "mxc://...",
  "m.relates_to": { "rel_type": "fleet.gen.result", "event_id": "<req>" },
  "fleet.gen.worker": "13700k"
}
```
Slick renders that `m.image` inline at full-res (PR-4), so the result lands
as a real picture in the channel.
