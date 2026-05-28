# Fleet Messaging — Deploy Quickstart

Distilled "happy path" if Tim picks all of DJBOOTH's recommended defaults. Companion to the 1262-line Phase 1 plan; this is the 12-step subset that gets a working Matrix homeserver + 2 daemons live in **~2 hours focused work on 13700K**.

If Tim wants to deviate (different deployment option, custom backup path, etc.), the full plan at `docs/superpowers/plans/2026-05-27-fleet-messaging-phase1-plan.md` is the source of truth.

---

## Defaults assumed (override any of these if you disagree)

| § | Decision | Default |
|---|---|---|
| 9.1 | Domain | Pick `fleetcommand.slopclaude.com` ($1-3/yr at Cloudflare Registrar) |
| 9.2 | Cloudflare account | Create one if missing (free tier fine) |
| 9.3 | Conduit deployment | Docker Desktop on 13700K |
| 9.4 | Backup destination | `D:\Dropbox\fleet-backups\conduit\` via rclone |
| 9.5 | Element web placement | Same subdomain (`fleetcommand.slopclaude.com`) |
| 9.6 | Widget priority | Phase 2 (not Phase 1) |

If those defaults all work for you, the below is your full deploy.

---

## Step 1 — Register the domain (15 min)

Go to **https://dash.cloudflare.com/?to=/:account/registrar** and register `fleetcommand.slopclaude.com` (or your pick). If you already have the domain elsewhere, change nameservers to Cloudflare's per the dashboard wizard.

**Verify:**
```bash
nslookup fleetcommand.slopclaude.com 1.1.1.1
# Should return Cloudflare's nameservers
```

## Step 2 — Install Docker Desktop + cloudflared on 13700K (10 min)

```powershell
winget install Docker.DockerDesktop --silent --accept-package-agreements --accept-source-agreements
winget install Cloudflare.cloudflared --silent --accept-package-agreements --accept-source-agreements
# Reboot if Docker Desktop prompts for WSL2 setup
```

## Step 3 — Create the Cloudflare Tunnel (5 min)

```powershell
cd $env:USERPROFILE
& 'C:\Program Files\cloudflared\cloudflared.exe' tunnel login
# Browser opens; pick your Cloudflare account + fleetcommand.slopclaude.com
& 'C:\Program Files\cloudflared\cloudflared.exe' tunnel create djbooth-fleet-chat
# Note the UUID + credentials file path it prints
& 'C:\Program Files\cloudflared\cloudflared.exe' tunnel route dns djbooth-fleet-chat fleetcommand.slopclaude.com
```

## Step 4 — Set up Conduit config directories on 13700K (3 min)

```powershell
New-Item -ItemType Directory -Force -Path 'C:\opt\conduit\data','C:\opt\conduit\config','C:\opt\conduit\cloudflared','C:\opt\conduit\logs'

# Copy the tunnel credentials
Copy-Item "$env:USERPROFILE\.cloudflared\*.json" 'C:\opt\conduit\cloudflared\'

# Generate the registration token
$bytes = New-Object byte[] 32
([System.Security.Cryptography.RandomNumberGenerator]::Create()).GetBytes($bytes)
$regToken = [System.BitConverter]::ToString($bytes).Replace('-','').ToLower()
Write-Output "REGISTRATION TOKEN (save this!): $regToken"
```

## Step 5 — Materialize conduit.toml + cloudflared config.yml (5 min)

```powershell
$repo = 'D:\GameDev\X3Native'  # adjust if cloned elsewhere
$domain = 'fleetcommand.slopclaude.com'
$tunnelUuid = '<UUID-FROM-STEP-3>'

# Conduit config
(Get-Content "$repo\tools\conduit-prep\conduit.toml.template" -Raw) `
  -replace 'fleetcommand.slopclaude.com', $domain `
  -replace '<REGISTRATION_TOKEN>', $regToken `
  | Set-Content 'C:\opt\conduit\config\conduit.toml' -Encoding utf8

# Cloudflared ingress
(Get-Content "$repo\tools\conduit-prep\cloudflared-config.yml.template" -Raw) `
  -replace 'fleetcommand.slopclaude.com', $domain `
  -replace '<TUNNEL_UUID>', $tunnelUuid `
  | Set-Content 'C:\opt\conduit\cloudflared\config.yml' -Encoding utf8
```

## Step 6 — Bring up Conduit + cloudflared (5 min)

```powershell
cd 'D:\GameDev\X3Native\tools\conduit-prep'
docker compose up -d
docker compose ps   # both services should show "Up"
```

**Verify:**
```powershell
# Internal
Invoke-RestMethod -Uri 'http://127.0.0.1:6167/_matrix/client/versions'
# External (through Cloudflare Tunnel)
Invoke-RestMethod -Uri 'https://fleetcommand.slopclaude.com/_matrix/client/versions'
```

Both should return JSON with a `versions` array.

## Step 7 — Deploy Element Web on 13700K (10 min)

```powershell
$elementVersion = 'v1.11.78'   # check github.com/element-hq/element-web/releases for latest
$dest = 'C:\opt\element-web'
New-Item -ItemType Directory -Force -Path $dest
Invoke-WebRequest -Uri "https://github.com/element-hq/element-web/releases/download/$elementVersion/element-$elementVersion.tar.gz" -OutFile "$env:TEMP\element-web.tar.gz"
tar -xzf "$env:TEMP\element-web.tar.gz" -C $dest --strip-components=1

# Config
$domain = 'fleetcommand.slopclaude.com'
@"
{
  "default_server_config": {
    "m.homeserver": {
      "base_url": "https://chat.$domain",
      "server_name": "$domain"
    }
  },
  "brand": "X3Native Fleet",
  "default_country_code": "US",
  "show_labs_settings": true,
  "feature_threadenabled": true
}
"@ | Set-Content "$dest\config.json" -Encoding utf8

# Install http-server + register Scheduled Task
& 'C:\Program Files\nodejs\npm.cmd' install -g http-server
$httpServer = "$env:APPDATA\npm\node_modules\http-server\bin\http-server"
$action = New-ScheduledTaskAction -Execute 'C:\Program Files\nodejs\node.exe' -Argument "`"$httpServer`" -p 8080 -d false -c-1 --silent" -WorkingDirectory $dest
$trigger = New-ScheduledTaskTrigger -AtStartup
$settings = New-ScheduledTaskSettingsSet -StartWhenAvailable -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) -ExecutionTimeLimit (New-TimeSpan -Days 365) -MultipleInstances IgnoreNew
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType S4U
Register-ScheduledTask -TaskName 'ElementWeb-Static' -Action $action -Trigger $trigger -Settings $settings -Principal $principal
Start-ScheduledTask -TaskName 'ElementWeb-Static'
```

**Verify in browser:** Open `https://fleetcommand.slopclaude.com/` — Element login screen should appear.

## Step 8 — Create your admin account in Element (3 min)

In the browser tab from Step 7:

1. Click **"Create Account"**
2. Username: `tim`, password: pick one (save to password manager)
3. **Registration token:** paste the one from Step 4
4. Submit → you're logged in as `@tim:fleetcommand.slopclaude.com`

The first registered user becomes admin automatically.

## Step 9 — Register bot accounts for DJBOOTH + 13700K (10 min)

Generate registration tokens for each bot. From your admin account in Element, DM `@conduit` (the admin bot):

```
!admin create-token djbooth-bot-token
```

Conduit replies with a one-use token. Repeat for `13700k-bot-token`.

Then on each machine, register the bot via the Matrix API. On DJBOOTH:

```bash
TOKEN_NEW='<djbooth-token-from-conduit>'
BOT_PW=$(openssl rand -hex 24)
curl -s -X POST "https://fleetcommand.slopclaude.com/_matrix/client/v3/register" \
  -H "Content-Type: application/json" \
  -d "{\"username\":\"djbooth\",\"password\":\"$BOT_PW\",\"auth\":{\"type\":\"m.login.registration_token\",\"token\":\"$TOKEN_NEW\"}}" \
  > /tmp/djbooth-register.json

# Extract access token to ~/.claude/.matrix_token
"/c/Users/Tim Smith/AppData/Local/Programs/Python/Python313/python.exe" -c "
import json, pathlib
d = json.loads(open('/tmp/djbooth-register.json').read())
pathlib.Path(r'C:\\Users\\Tim Smith\\.claude\\.matrix_token').write_text(d['access_token'], encoding='utf-8')
"
echo "Bot password (save in password manager): $BOT_PW"
```

Repeat on 13700K for `@13700k`.

## Step 10 — Start the matrix-daemon on DJBOOTH (5 min)

```powershell
$dst = "$env:USERPROFILE\.claude\matrix-daemon"
New-Item -ItemType Directory -Force -Path $dst
Copy-Item -Recurse -Force "D:\GameDev\X3Native\tools\matrix-daemon\*" $dst
cd $dst
& 'C:\Program Files\nodejs\npm.cmd' install

# Smoketest
& 'C:\Program Files\nodejs\node.exe' daemon.js
# Expect "identified" log line with user_id @djbooth:fleetcommand.slopclaude.com
# Ctrl+C to stop

# Register Scheduled Task
$daemonPath = "$dst\daemon.js"
$action = New-ScheduledTaskAction -Execute 'C:\Program Files\nodejs\node.exe' -Argument $daemonPath -WorkingDirectory $dst
$trigger = New-ScheduledTaskTrigger -AtLogOn
$settings = New-ScheduledTaskSettingsSet -StartWhenAvailable -RestartCount 5 -RestartInterval (New-TimeSpan -Minutes 1) -ExecutionTimeLimit (New-TimeSpan -Days 365) -MultipleInstances IgnoreNew
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive
Register-ScheduledTask -TaskName 'DJBOOTH-MatrixDaemon' -Action $action -Trigger $trigger -Settings $settings -Principal $principal
Start-ScheduledTask -TaskName 'DJBOOTH-MatrixDaemon'
```

Repeat on 13700K with `MATRIX_BOT_MACHINE='13700k'` env var set first.

## Step 11 — Create the #fleet-ops room + first ping (3 min)

In Element:

1. Click **"+"** next to room list → "Create Room" → name: `fleet-ops`
2. Settings → Members → invite `@djbooth:fleetcommand.slopclaude.com` and `@13700k:fleetcommand.slopclaude.com`
3. Both bots auto-accept (AutojoinRoomsMixin handles it)
4. Type a message in the room — both daemons should write it to their inbox

**Verify on DJBOOTH:**
```bash
tail -1 ~/.claude/.matrix_inbox.jsonl
# Should show your @tim message
```

## Step 12 — Set up Slack→Matrix mirror + backup (5 min)

The slack-daemon is already running. Verify the mirror block writes to `.matrix_inbox.jsonl`:

```bash
# Send yourself a Slack DM to @djbooth from your phone
# Wait 5 min, then:
tail -1 ~/.claude/.matrix_inbox.jsonl
# Should show source="slack-mirror" entry
```

Set up nightly backup:

```powershell
winget install Rclone.Rclone
& 'C:\Program Files\Rclone\rclone.exe' config   # follow prompts: name=dropbox, type=dropbox, OAuth

# Backup script
@'
$source = "C:\opt\conduit\data"
$dest = "dropbox:fleet-backups/conduit/$(Get-Date -Format yyyy-MM-dd)"
& "C:\Program Files\Rclone\rclone.exe" sync $source $dest --log-file "C:\opt\conduit\logs\backup-$(Get-Date -Format yyyyMMdd).log"
'@ | Set-Content 'C:\opt\conduit\backup.ps1' -Encoding utf8

$action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument '-NoProfile -ExecutionPolicy Bypass -File C:\opt\conduit\backup.ps1'
$trigger = New-ScheduledTaskTrigger -Daily -At '03:00'
$settings = New-ScheduledTaskSettingsSet -StartWhenAvailable -ExecutionTimeLimit (New-TimeSpan -Minutes 30)
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType S4U
Register-ScheduledTask -TaskName 'Conduit-NightlyBackup' -Action $action -Trigger $trigger -Settings $settings -Principal $principal
```

---

## You're done with Phase 1

- ✅ Matrix homeserver running on 13700K, externally reachable
- ✅ Element Web at fleetcommand.slopclaude.com
- ✅ Tim's admin account + 2 bot accounts (DJBOOTH, 13700K)
- ✅ #fleet-ops room with both bots
- ✅ Slack→Matrix mirror live (both inboxes drain through /loop)
- ✅ Nightly backup to Dropbox

Total time: ~2 hours focused work. Soak for 24 hours, then run the Phase 2 plan to add the remaining 6 fleet machines + the cyberpunk theme + the sidebar widget.

If anything goes sideways: roll back is `docker compose down` + delete the C:\opt\conduit volumes + unregister Scheduled Tasks. Nothing landed on `main`; everything is on `feat/fleet-messaging-design`.
