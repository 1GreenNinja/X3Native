# fleet-status widget — Element sidebar iframe app

Phase 2 of the fleet messaging spec. A tiny static HTML/JS/CSS bundle Element loads inside an iframe in the room sidebar. Polls `fleet-status.json` every 60s and renders the machine grid + integration queue.

## Files

| File | Purpose |
|---|---|
| `index.html` | Widget shell — header, warnings container, machines list, queue section, error fallback |
| `widget.css` | Cyberpunk-X3Native palette (same as `tools/element-theme/`), per-machine identity colors |
| `widget.js` | Polls `fleet-status.json`, renders the JSON document. ~120 lines, no framework |
| `sample-data.json` | Example JSON for local testing — open `index.html` after renaming this to `fleet-status.json` |
| `README.md` | This file |

## Local preview (no homeserver needed)

```powershell
# In tools/fleet-status/widget/
Copy-Item sample-data.json fleet-status.json
# Then serve the directory
& 'C:\Program Files\nodejs\node.exe' "$env:APPDATA\npm\node_modules\http-server\bin\http-server" -p 8181 -c-1
# Open http://127.0.0.1:8181/ — should see the cyberpunk-styled fleet grid
```

## Deploying alongside Element Web

On 13700K:

```powershell
# Copy widget bundle into Element Web's webroot at a known path
Copy-Item -Recurse `
  D:\GameDev\X3Native\tools\fleet-status\widget\* `
  C:\opt\element-web\fleet-widget\

# fleet-status.json is generated separately by tools/fleet-status/generate.ps1
# and lands at C:\opt\element-web\fleet-status.json — but the widget polls
# fleet-status.json *relative to its own location*, so symlink it:
New-Item -ItemType SymbolicLink `
  -Path C:\opt\element-web\fleet-widget\fleet-status.json `
  -Target C:\opt\element-web\fleet-status.json
```

The widget will then be reachable at `https://chat.<CHOSEN_DOMAIN>/fleet-widget/` and shareable in Element via the room-info → "Widgets" → "Add Custom Widget" feature.

## Adding the widget to a Matrix room

In Element:

1. Open the room (`#fleet-ops`)
2. Click the **info** icon → **Widgets** → **Add Custom**
3. URL: `https://chat.<CHOSEN_DOMAIN>/fleet-widget/`
4. Name: `Fleet Status`
5. Save → widget appears in the room's right sidebar

Element will iframe-embed the widget. The widget is read-only (just displays JSON) so there are no auth concerns. The fleet-status.json itself can be public — it carries no secrets, just branch SHAs and presence dots.

## Schema compatibility

The widget consumes the exact JSON shape defined in `tools/fleet-status/schema.json`. If you change the schema:

1. Bump `widget.js`'s rendering logic
2. Update `sample-data.json` accordingly
3. Re-run `tools/fleet-status/generate.ps1` so the output matches

## What's intentionally NOT here

- **Interactive controls** (kick a branch from the queue, ping a machine) — Phase 3. The widget is read-only for now to keep the security model trivial.
- **WebSocket / SSE** — polling every 60s is fine for a fleet of <10 machines.
- **Tim's mobile** — Element iOS/Android iframe-embeds widgets the same way; the widget will look the same on phone (responsive CSS minimal).
- **Auth gating** — fleet-status.json is public (no secrets in it). If we later add commit messages with secrets in them, we'd add a `?token=` query string or move behind Matrix's media authentication.
