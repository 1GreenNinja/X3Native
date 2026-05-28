# Conduit deployment prep

Staging directory for Conduit homeserver deployment on 13700K.

## Files

| File | Purpose |
|---|---|
| `conduit-v0.10.12.tar.gz` | Conduit source archive (Feb 2026 release). Used if Tim picks the "build from source" deployment path. |
| `CONDUIT-DEPLOY.md` | Three deployment options (Docker / WSL2 / native build) with tradeoffs and setup outlines |
| `docker-compose.yml` | Recommended path: containerized Conduit + cloudflared together on 13700K |
| `conduit.toml.template` | Homeserver config template; substitute domain + registration token before deploy |
| `cloudflared-config.yml.template` | Cloudflare Tunnel ingress routing; substitute tunnel UUID + domain |
| `README.md` | This file |

## Deployment workflow

1. **Tim answers §9 spec questions** — domain, Cloudflare account
2. **Pick deployment path** — Docker (recommended) per CONDUIT-DEPLOY.md
3. **On 13700K:**
   - `winget install Docker.DockerDesktop` + reboot if WSL2 setup needed
   - `winget install Cloudflare.cloudflared`
   - `cloudflared tunnel login` (browser OAuth, picks Cloudflare account)
   - `cloudflared tunnel create djbooth-fleet-chat` (generates UUID + creds JSON)
   - `cloudflared tunnel route dns djbooth-fleet-chat fleetcommand.slopclaude.com`
   - Copy `conduit.toml.template` → `C:\opt\conduit\config\conduit.toml`; substitute domain + generate registration token
   - Copy `cloudflared-config.yml.template` → `C:\opt\conduit\cloudflared\config.yml`; substitute tunnel UUID + domain
   - Copy the `<UUID>.json` credentials file → `C:\opt\conduit\cloudflared\<UUID>.json`
   - `cd D:\GameDev\X3Native\tools\conduit-prep && docker compose up -d`
4. **Verify:**
   - `Invoke-RestMethod http://127.0.0.1:6167/_matrix/client/versions` — Conduit on localhost ✓
   - `curl -I https://fleetcommand.slopclaude.com/_matrix/client/versions` from external network — tunnel ✓
5. **Create Tim's admin account** via Element Web (first registered user with the token becomes admin)
6. **Create bot accounts** for DJBOOTH + 13700K via the same registration flow
7. **Save bot tokens** to each machine's `~/.claude/.matrix_token` (mode 700)
8. **Run `matrix-daemon`** on DJBOOTH + 13700K (see `tools/matrix-daemon/README.md`)
9. **Create `#fleet-ops`** room in Element and invite both bots
10. **First Claude-to-Claude RPC test** — see Plan Task 32

## What about Element Web?

Element Web is a separate static SPA — it's served from a different process (a Node `http-server` on port 8080). The docker-compose here only handles Conduit + cloudflared. Element Web setup is in `tools/element-web/setup.md`. The cloudflared ingress in `cloudflared-config.yml.template` routes the bare hostname to Element Web and `/_matrix/*` paths to Conduit, so both surfaces share `fleetcommand.slopclaude.com`.
