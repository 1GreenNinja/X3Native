# Element Web setup on 13700K

Static-file deployment of Element Web served alongside Conduit through the same Cloudflare Tunnel hostname (`chat.<CHOSEN_DOMAIN>`). The bare hostname serves Element Web; `/_matrix/*` paths route to Conduit. This is Plan Tasks 17–19 in `docs/superpowers/plans/2026-05-27-fleet-messaging-phase1-plan.md`.

## Why Element Web specifically?

| Client | Why pick / skip |
|---|---|
| **Element Web** ✓ | Mature, polished, supports custom themes, runs in any browser. |
| Element Desktop | Electron wrapper around the same SPA. Tim can install on laptop later — same login as web. |
| Element iOS / Android | Stock app from App Store / Play Store. Tim picks the chat homeserver during signup. |
| Cinny / Hydrogen / FluffyChat | Lighter alternatives. Mention in spec §10 as future options if Element falls short. |

Element Web is just static HTML/JS/CSS. No backend. The whole "client" is a single SPA bundle.

## Download

Releases: `https://github.com/element-hq/element-web/releases`. As of late 2026, latest stable is `v1.11.78` or so — pin to the latest at deploy time.

On 13700K:

```powershell
# Pick a version — check the GitHub releases page for the current stable
$elementVersion = 'v1.11.78'
$dest = 'C:\opt\element-web'
New-Item -ItemType Directory -Force -Path $dest

# Download the tarball
$url = "https://github.com/element-hq/element-web/releases/download/$elementVersion/element-$elementVersion.tar.gz"
Invoke-WebRequest -Uri $url -OutFile "$env:TEMP\element-web.tar.gz"

# Extract
tar -xzf "$env:TEMP\element-web.tar.gz" -C $dest --strip-components=1

# Sanity check
Test-Path "$dest\index.html"
# Expected: True
```

## config.json — point Element at our homeserver

Create `C:\opt\element-web\config.json` (substituting `<CHOSEN_DOMAIN>`):

```json
{
  "default_server_config": {
    "m.homeserver": {
      "base_url": "https://chat.<CHOSEN_DOMAIN>",
      "server_name": "<CHOSEN_DOMAIN>"
    }
  },
  "brand": "X3Native Fleet",
  "branding": {
    "welcome_background_url": "/welcome-bg.png",
    "auth_header_logo_url": "/logo.png",
    "auth_footer_links": [
      { "text": "X3Native repo", "url": "https://github.com/1GreenNinja/X3Native" }
    ]
  },
  "default_country_code": "US",
  "show_labs_settings": true,
  "feature_threadenabled": true,
  "room_directory": { "servers": ["<CHOSEN_DOMAIN>"] },
  "default_theme": "cyberpunk-x3native"
}
```

The `default_theme` value points at the custom theme we declared in `tools/element-theme/theme.json`. To make Element load the custom theme automatically, also drop `theme.css` from `tools/element-theme/` into Element's webroot at `themes/cyberpunk-x3native.css`.

## Static HTTP server (port 8080)

Element Web is just files. Need a tiny static server to serve them:

```powershell
# Install http-server globally (~50 KB Node package)
& 'C:\Program Files\nodejs\npm.cmd' install -g http-server

# Path varies by user — npm puts it under your global modules dir
$httpServer = "$env:APPDATA\npm\node_modules\http-server\bin\http-server"
ls $httpServer

# Test manually first
& 'C:\Program Files\nodejs\node.exe' $httpServer C:\opt\element-web -p 8080 -d false -c-1 --silent
# Browse http://127.0.0.1:8080/ — should load Element's UI
# Ctrl+C to stop
```

## Register as Scheduled Task

```powershell
$httpServer = "$env:APPDATA\npm\node_modules\http-server\bin\http-server"
$action = New-ScheduledTaskAction -Execute 'C:\Program Files\nodejs\node.exe' `
  -Argument "`"$httpServer`" -p 8080 -d false -c-1 --silent" `
  -WorkingDirectory 'C:\opt\element-web'
$trigger = New-ScheduledTaskTrigger -AtStartup
$settings = New-ScheduledTaskSettingsSet -StartWhenAvailable `
  -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) `
  -ExecutionTimeLimit (New-TimeSpan -Days 365) -MultipleInstances IgnoreNew
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType S4U
Register-ScheduledTask -TaskName 'ElementWeb-Static' `
  -Action $action -Trigger $trigger -Settings $settings -Principal $principal `
  -Description 'Static HTTP server for Element Web at C:\opt\element-web. Reached via Cloudflare Tunnel.'
Start-ScheduledTask -TaskName 'ElementWeb-Static'
```

## Cloudflare Tunnel routing

The `cloudflared-config.yml.template` in `tools/conduit-prep/` already includes the ingress for routing the bare hostname to Element Web. Re-summarized here:

```yaml
ingress:
  - hostname: chat.<CHOSEN_DOMAIN>
    path: /_matrix/.*
    service: http://conduit:6167         # or 127.0.0.1:6167 if cloudflared runs on the host

  - hostname: chat.<CHOSEN_DOMAIN>
    path: /.well-known/.*
    service: http://conduit:6167

  - hostname: chat.<CHOSEN_DOMAIN>
    service: http://host.docker.internal:8080   # Element Web on host (from inside container)

  - service: http_status:404
```

If cloudflared runs as a native Windows service instead of in the docker-compose, use `http://127.0.0.1:8080` instead of `host.docker.internal`.

## Verification

After all three services are up:

```bash
# Conduit on the inside
curl -s http://127.0.0.1:6167/_matrix/client/versions | head -c 200

# Element static on the inside
curl -s http://127.0.0.1:8080/ | grep -o '<title>[^<]*</title>'
# Expected: <title>Element</title>

# External — through the tunnel — should hit Element Web
curl -sI https://chat.<CHOSEN_DOMAIN>/
# Expected: HTTP/2 200; content-type: text/html

# External — through the tunnel — Matrix API
curl -sI https://chat.<CHOSEN_DOMAIN>/_matrix/client/versions
# Expected: HTTP/2 200; content-type: application/json
```

Then **Tim opens `https://chat.<CHOSEN_DOMAIN>/`** in his browser. Should see the cyberpunk-themed Element login. Register with the token from `conduit.toml`. Become admin.

## File checklist after setup

```
C:\opt\
├── conduit\
│   ├── config\conduit.toml                       (substituted from template)
│   ├── data\...                                  (Conduit's SQLite DB)
│   └── cloudflared\
│       ├── config.yml                            (substituted from template)
│       └── <UUID>.json                           (from `cloudflared tunnel create`)
└── element-web\
    ├── index.html                                (from the tarball)
    ├── config.json                               (substituted)
    ├── themes\cyberpunk-x3native.css             (copy of tools/element-theme/theme.css)
    ├── welcome-bg.png                            (optional branding)
    ├── logo.png                                  (optional branding)
    └── ... (the rest of the Element Web bundle)
```

## Upgrading Element later

```powershell
# Periodically check for new Element releases
$dest = 'C:\opt\element-web'
$elementVersion = 'v1.11.NEW_VERSION'
$url = "https://github.com/element-hq/element-web/releases/download/$elementVersion/element-$elementVersion.tar.gz"
Invoke-WebRequest -Uri $url -OutFile "$env:TEMP\element-web-new.tar.gz"

# Back up the config.json before extract
Copy-Item "$dest\config.json" "$env:TEMP\element-config.json.bak"
Copy-Item -Recurse "$dest\themes" "$env:TEMP\element-themes.bak"

# Extract over the existing tree
tar -xzf "$env:TEMP\element-web-new.tar.gz" -C $dest --strip-components=1

# Restore config + themes
Copy-Item "$env:TEMP\element-config.json.bak" "$dest\config.json"
Copy-Item -Recurse "$env:TEMP\element-themes.bak" "$dest\themes"

# Cycle the static server
Stop-ScheduledTask -TaskName 'ElementWeb-Static'
Start-ScheduledTask -TaskName 'ElementWeb-Static'
```

Selectors in `theme.css` may need updating on major Element releases since class names occasionally shift. Test the cyberpunk theme renders correctly after each upgrade.
