# Fleet Messaging Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up a self-hosted Matrix-based fleet messaging fabric on 13700K: Conduit homeserver + Cloudflare Tunnel for remote access + Element Web client + bot daemons on DJBOOTH and 13700K + the first fleet room (#fleet-ops) + Slack-to-Matrix mirroring during the 30-day soak.

**Architecture:** Conduit (Rust binary, SQLite, ~12 MB) on 13700K bound to 127.0.0.1:6167, fronted by `cloudflared` exposing `fleetcommand.slopclaude.com` to the public internet (free Cloudflare Tunnel). Per-machine Matrix accounts (`@djbooth`, `@13700k`, etc.). Each fleet PC runs a Node.js daemon using `matrix-bot-sdk` that maintains a persistent sync connection, writes incoming messages to a local inbox file, and exposes a named-pipe outbox for Claude sessions to post replies. Element Web hosted on the same Cloudflare Tunnel as a static asset. Slack daemon (already running) gets a one-way cross-post hook to mirror to Matrix during the soak.

**Tech Stack:** Conduit (Rust homeserver), `cloudflared` (Cloudflare Tunnel client), Element Web (React static SPA), Node.js v24.16 (already installed at `C:\Program Files\nodejs\`), `matrix-bot-sdk` npm package, `rclone` for backups, PowerShell for Windows Scheduled Tasks.

**Pre-conditions before this plan can execute:**
- Tim has answered the §9 open questions from the design spec (domain, Cloudflare account, backup destination, registration token style, Element Web placement, widget priority).
- 13700K is reachable on the LAN and Tim has admin shell access to it.
- 13700K has Node.js v24+ installed (matches DJBOOTH's setup).

**Branch:** `feat/fleet-messaging-design` (this plan committed alongside the spec).

---

## Phase 1.A — Pre-flight (Tim resolves §9 open questions)

### Task 1: Lock the production domain

**Files:** `docs/superpowers/specs/2026-05-27-fleet-messaging-design.md` (update §9 with chosen domain)

- [x] **Step 1: ✅ Domain locked.** Tim chose `fleetcommand.slopclaude.com` (subdomain of the existing slopclaude.com he already owns). Confirmed 2026-05-28. No new domain registration needed — just a DNS record at the slopclaude.com zone.

- [ ] **Step 2: Verify the slopclaude.com zone is on Cloudflare DNS.** Cloudflare Tunnel REQUIRES Cloudflare-managed DNS. If slopclaude.com is on another registrar, transfer nameservers to Cloudflare.

```powershell
nslookup slopclaude.com 1.1.1.1
# Expected: returns *.ns.cloudflare.com nameservers
```

### Task 2: Verify Cloudflare account is ready

- [ ] **Step 1:** Tim creates a free Cloudflare account at `https://dash.cloudflare.com/sign-up` (if no existing one).

- [ ] **Step 2:** Add the chosen domain as a "Site" in the dashboard. Cloudflare guides through nameserver changes.

- [ ] **Step 3:** Verify the domain is showing "Active" status in the Cloudflare dashboard. DNS propagation may take up to 24h but usually completes in minutes.

- [ ] **Step 4:** Confirm by running on DJBOOTH:

```bash
nslookup fleetcommand.slopclaude.com 1.1.1.1
# Expected: returns Cloudflare nameservers (e.g., walt.ns.cloudflare.com)
```

### Task 3: Confirm 13700K access

- [ ] **Step 1:** Tim ssh's or RDP's into 13700K and verifies:
  - PowerShell admin shell available
  - Node.js installed (`node --version` returns v24+)
  - At least 20 GB free disk
  - The machine stays on 24/7 (verify in Windows Power Options → Sleep = Never)

- [ ] **Step 2:** If Node.js missing on 13700K, install:

```powershell
winget install OpenJS.NodeJS.LTS --silent --accept-package-agreements --accept-source-agreements
```

- [ ] **Step 3:** Document 13700K's LAN IP (for fleet hosts config later):

```powershell
$lanIP = (Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.PrefixOrigin -eq 'Dhcp' -or $_.PrefixOrigin -eq 'Manual' } | Where-Object { $_.IPAddress -notlike '169.*' -and $_.IPAddress -ne '127.0.0.1' } | Select-Object -First 1).IPAddress
Write-Output "13700K LAN IP: $lanIP"
```

---

## Phase 1.B — Deploy Conduit on 13700K

### Task 4: Create the Conduit install directory

**Files:**
- Create: `C:\opt\conduit\` (directory on 13700K)
- Create: `C:\opt\conduit\db\` (directory)
- Create: `C:\opt\conduit\logs\` (directory)

- [ ] **Step 1: On 13700K, create the directory tree.**

```powershell
New-Item -ItemType Directory -Force -Path 'C:\opt\conduit\db'
New-Item -ItemType Directory -Force -Path 'C:\opt\conduit\logs'
```

- [ ] **Step 2: Verify.**

```powershell
Test-Path 'C:\opt\conduit\db'
# Expected: True
```

### Task 5: Download the Conduit binary

**Files:**
- Create: `C:\opt\conduit\conduit.exe`

- [ ] **Step 1: Find the latest Windows release.** Conduit's releases are at `https://gitlab.com/famedly/conduit/-/releases`. Look for the latest `conduit-x86_64-pc-windows-msvc.exe` artifact. As of 2026-05 the latest tag is 0.10.x; pin to the latest stable.

- [ ] **Step 2: Download via PowerShell.**

```powershell
$conduitVersion = "v0.10.5"  # update to current latest stable
$conduitUrl = "https://gitlab.com/famedly/conduit/-/jobs/artifacts/$conduitVersion/raw/conduit-x86_64-pc-windows-msvc.exe?job=build"
Invoke-WebRequest -Uri $conduitUrl -OutFile 'C:\opt\conduit\conduit.exe'
```

- [ ] **Step 3: Verify the binary runs.**

```powershell
& 'C:\opt\conduit\conduit.exe' --version
# Expected: conduit 0.10.x
```

- [ ] **Step 4: Compute and record SHA256 for the install log.**

```powershell
Get-FileHash 'C:\opt\conduit\conduit.exe' -Algorithm SHA256 | Format-List
# Record the hash in C:\opt\conduit\logs\install.log
```

### Task 6: Write the Conduit config file

**Files:**
- Create: `C:\opt\conduit\conduit.toml`

- [ ] **Step 1: Generate a 32-byte hex registration token.**

```powershell
$bytes = New-Object byte[] 32
([System.Security.Cryptography.RandomNumberGenerator]::Create()).GetBytes($bytes)
$regToken = [System.BitConverter]::ToString($bytes).Replace('-','').ToLower()
Write-Output "Registration token: $regToken"
# Save to a secure note — Tim needs this to register accounts via Element
```

- [ ] **Step 2: Write `conduit.toml`.** Replace `fleetcommand.slopclaude.com` with the actual domain from Task 1.

```powershell
$config = @"
[global]
server_name = "fleetcommand.slopclaude.com"
database_backend = "sqlite"
database_path = "C:/opt/conduit/db"
port = 6167
address = "127.0.0.1"
allow_registration = true
registration_token = "$regToken"
allow_federation = false
allow_encryption = true
allow_check_for_updates = true
max_request_size = 20_000_000
trusted_servers = []
default_room_version = "10"
"@
Set-Content -Path 'C:\opt\conduit\conduit.toml' -Value $config -Encoding utf8
```

- [ ] **Step 3: Verify config syntax.**

```powershell
& 'C:\opt\conduit\conduit.exe' --config 'C:\opt\conduit\conduit.toml' --help
# Expected: prints help without parse errors
```

### Task 7: First boot of Conduit (manual, foreground)

- [ ] **Step 1: Start Conduit in foreground to confirm it boots.**

```powershell
$env:CONDUIT_CONFIG = 'C:\opt\conduit\conduit.toml'
& 'C:\opt\conduit\conduit.exe' 2>&1 | Tee-Object -FilePath 'C:\opt\conduit\logs\first-boot.log'
```

- [ ] **Step 2: From another shell on 13700K, verify the server responds.**

```powershell
Invoke-RestMethod -Uri 'http://127.0.0.1:6167/_matrix/client/versions'
# Expected: JSON with a "versions" array, e.g. {"versions":["r0.6.1","v1.10",...]}
```

- [ ] **Step 3: Ctrl+C to stop the foreground process.** Conduit's SQLite DB now exists at `C:\opt\conduit\db\`.

### Task 8: Install Conduit as a Windows Scheduled Task

**Files:**
- Modify: Task Scheduler (registers `Conduit-Homeserver`)

- [ ] **Step 1: Register the scheduled task to auto-start at boot.**

```powershell
$action = New-ScheduledTaskAction -Execute 'C:\opt\conduit\conduit.exe' -WorkingDirectory 'C:\opt\conduit'
$trigger = New-ScheduledTaskTrigger -AtStartup
$settings = New-ScheduledTaskSettingsSet -StartWhenAvailable -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) -ExecutionTimeLimit (New-TimeSpan -Days 365) -MultipleInstances IgnoreNew
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType S4U -RunLevel Highest

Register-ScheduledTask -TaskName 'Conduit-Homeserver' -Action $action -Trigger $trigger -Settings $settings -Principal $principal -Description 'Matrix Conduit homeserver for X3Native fleet messaging.'
```

- [ ] **Step 2: Set the CONDUIT_CONFIG env var for the task's environment.**

```powershell
[System.Environment]::SetEnvironmentVariable('CONDUIT_CONFIG','C:\opt\conduit\conduit.toml','Machine')
```

- [ ] **Step 3: Start the task and verify.**

```powershell
Start-ScheduledTask -TaskName 'Conduit-Homeserver'
Start-Sleep -Seconds 5
Get-ScheduledTask -TaskName 'Conduit-Homeserver' | Get-ScheduledTaskInfo | Format-List
# Expected: LastTaskResult = 0
Invoke-RestMethod -Uri 'http://127.0.0.1:6167/_matrix/client/versions'
# Expected: same JSON as Task 7 Step 2
```

### Task 9: Smoketest reboot resilience

- [ ] **Step 1: Reboot 13700K.**

```powershell
Restart-Computer -Force
```

- [ ] **Step 2: After 13700K boots back up, wait 2 min, then verify Conduit auto-started.**

```powershell
Invoke-RestMethod -Uri 'http://127.0.0.1:6167/_matrix/client/versions'
# Expected: JSON response (Conduit auto-started via the scheduled task)
```

- [ ] **Step 3: If response fails, check the task's last run.**

```powershell
Get-ScheduledTask -TaskName 'Conduit-Homeserver' | Get-ScheduledTaskInfo | Format-List
# Investigate non-zero LastTaskResult
```

---

## Phase 1.C — Cloudflare Tunnel

### Task 10: Install cloudflared on 13700K

**Files:**
- Create: `C:\Program Files\cloudflared\cloudflared.exe` (via winget)

- [ ] **Step 1: Install via winget.**

```powershell
winget install Cloudflare.cloudflared --silent --accept-package-agreements --accept-source-agreements
```

- [ ] **Step 2: Verify.**

```powershell
& 'C:\Program Files\cloudflared\cloudflared.exe' --version
# Expected: cloudflared version 2025.x.x
```

### Task 11: Authenticate cloudflared with the Cloudflare account

- [ ] **Step 1: Run the interactive auth flow.** This opens a browser tab where Tim selects the Cloudflare account + domain.

```powershell
& 'C:\Program Files\cloudflared\cloudflared.exe' tunnel login
```

- [ ] **Step 2: Verify cert.pem was written.**

```powershell
Test-Path "$env:USERPROFILE\.cloudflared\cert.pem"
# Expected: True
```

### Task 12: Create the tunnel

- [ ] **Step 1: Create a named tunnel.**

```powershell
$tunnel = & 'C:\Program Files\cloudflared\cloudflared.exe' tunnel create djbooth-fleet-chat 2>&1
Write-Output $tunnel
# Records the UUID — note it
```

- [ ] **Step 2: List tunnels to confirm.**

```powershell
& 'C:\Program Files\cloudflared\cloudflared.exe' tunnel list
# Expected: djbooth-fleet-chat with a UUID
```

- [ ] **Step 3: Note the credentials file path.**

```powershell
ls "$env:USERPROFILE\.cloudflared\*.json"
# Expected: <UUID>.json — the tunnel credentials
```

### Task 13: Configure DNS in Cloudflare dashboard

- [ ] **Step 1: Add a CNAME via cloudflared.**

```powershell
$tunnelUuid = '<UUID-FROM-TASK-12>'
& 'C:\Program Files\cloudflared\cloudflared.exe' tunnel route dns djbooth-fleet-chat "fleetcommand.slopclaude.com"
```

- [ ] **Step 2: Verify the CNAME exists in the Cloudflare dashboard.** Navigate to DNS → Records and confirm `fleetcommand.slopclaude.com` → `<UUID>.cfargotunnel.com` is listed and proxied (orange cloud ON).

- [ ] **Step 3: Verify DNS resolves.**

```powershell
nslookup "fleetcommand.slopclaude.com" 1.1.1.1
# Expected: returns Cloudflare IPs (104.x.x.x or 172.x.x.x)
```

### Task 14: Write cloudflared config.yml

**Files:**
- Create: `%USERPROFILE%\.cloudflared\config.yml` (on 13700K)

- [ ] **Step 1: Write the ingress config.**

```powershell
$tunnelUuid = '<UUID-FROM-TASK-12>'
$config = @"
tunnel: $tunnelUuid
credentials-file: $env:USERPROFILE\.cloudflared\$tunnelUuid.json
ingress:
  - hostname: fleetcommand.slopclaude.com
    service: http://127.0.0.1:6167
  - service: http_status:404
"@
Set-Content -Path "$env:USERPROFILE\.cloudflared\config.yml" -Value $config -Encoding utf8
```

- [ ] **Step 2: Verify config parses.**

```powershell
& 'C:\Program Files\cloudflared\cloudflared.exe' tunnel ingress validate
# Expected: "OK" / "Validating rules from ... — valid"
```

### Task 15: Run cloudflared in foreground to smoketest

- [ ] **Step 1: Start the tunnel.**

```powershell
& 'C:\Program Files\cloudflared\cloudflared.exe' tunnel --config "$env:USERPROFILE\.cloudflared\config.yml" run djbooth-fleet-chat
# Leave running in this shell; expect "Connection registered" logs
```

- [ ] **Step 2: From DJBOOTH or any external box, curl the public URL.**

```bash
curl -sI "https://fleetcommand.slopclaude.com/_matrix/client/versions"
# Expected: HTTP/2 200 + json content-type
```

- [ ] **Step 3: From a phone (over cellular, NOT wifi), open the URL in a browser.**

Expected: Conduit's JSON response shows in the browser — this proves end-to-end external reachability.

- [ ] **Step 4: Ctrl+C in the cloudflared shell to stop. Daemonize next.**

### Task 16: Install cloudflared as a Windows Scheduled Task

- [ ] **Step 1: Register the scheduled task.**

```powershell
$tunnelUuid = '<UUID-FROM-TASK-12>'
$action = New-ScheduledTaskAction -Execute 'C:\Program Files\cloudflared\cloudflared.exe' -Argument "tunnel --config `"$env:USERPROFILE\.cloudflared\config.yml`" run djbooth-fleet-chat"
$trigger = New-ScheduledTaskTrigger -AtStartup
$settings = New-ScheduledTaskSettingsSet -StartWhenAvailable -RestartCount 5 -RestartInterval (New-TimeSpan -Minutes 1) -ExecutionTimeLimit (New-TimeSpan -Days 365) -MultipleInstances IgnoreNew
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType S4U -RunLevel Highest

Register-ScheduledTask -TaskName 'CloudflareTunnel-Fleet' -Action $action -Trigger $trigger -Settings $settings -Principal $principal -Description 'Cloudflare Tunnel for fleetcommand.slopclaude.com → Conduit.'

Start-ScheduledTask -TaskName 'CloudflareTunnel-Fleet'
```

- [ ] **Step 2: Verify the tunnel reconnected.**

```bash
curl -sI "https://fleetcommand.slopclaude.com/_matrix/client/versions"
# Expected: HTTP/2 200
```

- [ ] **Step 3: Reboot 13700K and verify both Conduit and cloudflared came back up.**

```powershell
Restart-Computer -Force
# After reboot, wait 2 min, then:
curl -sI "https://fleetcommand.slopclaude.com/_matrix/client/versions"
# Expected: HTTP/2 200
```

---

## Phase 1.D — Element Web client

### Task 17: Download Element Web

**Files:**
- Create: `C:\opt\element-web\` (directory)

- [ ] **Step 1: Download the latest Element Web tarball.** Releases: `https://github.com/element-hq/element-web/releases`. Pin to a known version.

```powershell
$elementVersion = "v1.11.78"  # update to latest stable
$elementUrl = "https://github.com/element-hq/element-web/releases/download/$elementVersion/element-$elementVersion.tar.gz"
Invoke-WebRequest -Uri $elementUrl -OutFile "$env:TEMP\element-web.tar.gz"
```

- [ ] **Step 2: Extract to `C:\opt\element-web\`.**

```powershell
New-Item -ItemType Directory -Force -Path 'C:\opt\element-web'
tar -xzf "$env:TEMP\element-web.tar.gz" -C 'C:\opt\element-web' --strip-components=1
```

- [ ] **Step 3: Verify.**

```powershell
Test-Path 'C:\opt\element-web\index.html'
# Expected: True
```

### Task 18: Configure Element Web

**Files:**
- Create: `C:\opt\element-web\config.json`

- [ ] **Step 1: Write the config that points Element at our Conduit homeserver.**

```powershell
$elementConfig = @"
{
  "default_server_config": {
    "m.homeserver": {
      "base_url": "https://fleetcommand.slopclaude.com",
      "server_name": "fleetcommand.slopclaude.com"
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
  "room_directory": { "servers": ["fleetcommand.slopclaude.com"] }
}
"@
Set-Content -Path 'C:\opt\element-web\config.json' -Value $elementConfig -Encoding utf8
```

### Task 19: Serve Element Web through Cloudflare Tunnel

**Files:**
- Modify: `%USERPROFILE%\.cloudflared\config.yml` (add ingress for Element Web)
- Create: small Node.js static-file server, OR use a path-based ingress

- [ ] **Step 1: Install a tiny static-file server on 13700K.**

```powershell
& 'C:\Program Files\nodejs\npm.cmd' install -g http-server
```

- [ ] **Step 2: Start it on port 8080 (Scheduled Task).**

```powershell
$action = New-ScheduledTaskAction -Execute 'C:\Program Files\nodejs\node.exe' -Argument "C:\Users\$env:USERNAME\AppData\Roaming\npm\node_modules\http-server\bin\http-server -p 8080 -d false -c-1 --silent" -WorkingDirectory 'C:\opt\element-web'
$trigger = New-ScheduledTaskTrigger -AtStartup
$settings = New-ScheduledTaskSettingsSet -StartWhenAvailable -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) -ExecutionTimeLimit (New-TimeSpan -Days 365) -MultipleInstances IgnoreNew
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType S4U
Register-ScheduledTask -TaskName 'ElementWeb-Static' -Action $action -Trigger $trigger -Settings $settings -Principal $principal -Description 'Static HTTP server for Element Web at C:\opt\element-web.'
Start-ScheduledTask -TaskName 'ElementWeb-Static'
```

- [ ] **Step 3: Update cloudflared config.yml to route the bare hostname to Element, and the `/_matrix/*` path to Conduit.**

```yaml
tunnel: <UUID>
credentials-file: ...
ingress:
  - hostname: fleetcommand.slopclaude.com
    path: /_matrix/.*
    service: http://127.0.0.1:6167
  - hostname: fleetcommand.slopclaude.com
    path: /.well-known/.*
    service: http://127.0.0.1:6167
  - hostname: fleetcommand.slopclaude.com
    service: http://127.0.0.1:8080
  - service: http_status:404
```

- [ ] **Step 4: Restart the CloudflareTunnel-Fleet task.**

```powershell
Stop-ScheduledTask -TaskName 'CloudflareTunnel-Fleet'
Start-ScheduledTask -TaskName 'CloudflareTunnel-Fleet'
```

- [ ] **Step 5: Verify Element Web loads.**

```bash
curl -s "https://fleetcommand.slopclaude.com/" | head -20
# Expected: HTML containing "Element" or "<title>Element</title>"
```

### Task 20: Tim logs in via Element

- [ ] **Step 1:** Open `https://fleetcommand.slopclaude.com/` in any browser (laptop or phone).

- [ ] **Step 2:** Click "Create Account" → uses registration token from Task 6. Username: `tim`. Password: Tim's choice (save to password manager).

- [ ] **Step 3:** After signup, Tim is logged in as `@tim:fleetcommand.slopclaude.com`.

- [ ] **Step 4:** From Tim's account, raise privileges to admin via Conduit's admin command (Conduit auto-promotes the first registered user to admin if `allow_registration` is true and `registration_token` is set).

- [ ] **Step 5:** Install Element on Tim's phone (App Store / Play Store). Log in with `tim` + password. Verify push notifications work by self-DMing.

---

## Phase 1.E — Initial bot accounts

### Task 21: Generate registration tokens per bot

- [ ] **Step 1:** From `@tim` in Element, open a DM to `@conduit` (the admin bot Conduit auto-creates).

- [ ] **Step 2:** Send: `!admin create-token djbooth-bot-token`

- [ ] **Step 3:** Conduit replies with a new registration token scoped to one use. Record it.

- [ ] **Step 4:** Repeat for `13700k-bot-token`.

### Task 22: Register `@djbooth` and `@13700k` accounts

- [ ] **Step 1: From DJBOOTH (this machine), register via Matrix API.**

```bash
TOKEN_DJ='<djbooth-bot-token-from-task-21>'
# Generate a random password for the bot account
BOT_PW=$(openssl rand -hex 24)
curl -s -X POST "https://fleetcommand.slopclaude.com/_matrix/client/v3/register" \
  -H "Content-Type: application/json" \
  -d "{
    \"username\": \"djbooth\",
    \"password\": \"$BOT_PW\",
    \"auth\": {
      \"type\": \"m.login.registration_token\",
      \"token\": \"$TOKEN_DJ\"
    }
  }" > /tmp/djbooth-register.json
cat /tmp/djbooth-register.json
# Expected: JSON with "access_token", "user_id":"@djbooth:fleetcommand.slopclaude.com", "device_id"
# Save the access_token to ~/.claude/.matrix_token (mode 700)
umask 077
jq -r .access_token /tmp/djbooth-register.json > "$HOME/.claude/.matrix_token"
echo "Bot password (save in password manager): $BOT_PW"
```

- [ ] **Step 2: Verify the token.**

```bash
TOKEN=$(cat "$HOME/.claude/.matrix_token")
curl -s "https://fleetcommand.slopclaude.com/_matrix/client/v3/account/whoami" \
  -H "Authorization: Bearer $TOKEN"
# Expected: {"user_id":"@djbooth:fleetcommand.slopclaude.com",...}
```

- [ ] **Step 3: Repeat the same flow on 13700K for `@13700k`.**

---

## Phase 1.F — DJBOOTH matrix-bot-sdk daemon

### Task 23: Create the daemon project skeleton

**Files:**
- Create: `C:\Users\Tim Smith\.claude\matrix-daemon\package.json`
- Create: `C:\Users\Tim Smith\.claude\matrix-daemon\daemon.js`
- Create: `C:\Users\Tim Smith\.claude\matrix-daemon\config.js`
- Create: `C:\Users\Tim Smith\.claude\matrix-daemon\tests\`

- [ ] **Step 1: Create the directory.**

```bash
mkdir -p "/c/Users/Tim Smith/.claude/matrix-daemon/tests"
cd "/c/Users/Tim Smith/.claude/matrix-daemon"
```

- [ ] **Step 2: Initialize npm.**

```bash
export PATH="/c/Program Files/nodejs:$PATH"
npm init -y
```

- [ ] **Step 3: Install dependencies.**

```bash
npm install matrix-bot-sdk node-named-pipe winston
npm install --save-dev jest
```

- [ ] **Step 4: Verify `package.json`.**

```bash
cat package.json
# Expected: dependencies include matrix-bot-sdk, node-named-pipe, winston
```

### Task 24: Write the config module

**Files:**
- Create: `C:\Users\Tim Smith\.claude\matrix-daemon\config.js`
- Create: `C:\Users\Tim Smith\.claude\matrix-daemon\tests\config.test.js`

- [ ] **Step 1: Write the failing test.**

```javascript
// tests/config.test.js
const config = require('../config');

describe('config', () => {
  test('homeserver URL is reachable from env or default', () => {
    expect(config.homeserverUrl).toMatch(/^https:\/\//);
  });
  test('access token is loaded from a file path that exists', () => {
    expect(typeof config.accessTokenPath).toBe('string');
    expect(config.accessTokenPath.length).toBeGreaterThan(0);
  });
  test('inbox path is set', () => {
    expect(config.inboxPath).toContain('.matrix_inbox.jsonl');
  });
  test('pipe name is set with machine identifier', () => {
    expect(config.pipeName).toMatch(/^\\\\\.\\pipe\\matrix-/);
  });
});
```

- [ ] **Step 2: Run, verify fails.**

```bash
npx jest tests/config.test.js
# Expected: FAIL — config module not found
```

- [ ] **Step 3: Write `config.js`.**

```javascript
// config.js
const os = require('os');
const path = require('path');

const machineName = (process.env.MATRIX_BOT_MACHINE || os.hostname()).toLowerCase();
const claudeDir = process.env.CLAUDE_DIR || path.join(os.homedir(), '.claude');

module.exports = {
  homeserverUrl: process.env.MATRIX_HOMESERVER_URL || 'https://fleetcommand.slopclaude.com',
  accessTokenPath: path.join(claudeDir, '.matrix_token'),
  inboxPath: path.join(claudeDir, '.matrix_inbox.jsonl'),
  outboxLogPath: path.join(claudeDir, '.matrix_outbox.jsonl'),
  seenPath: path.join(claudeDir, '.matrix_seen.json'),
  logPath: path.join(claudeDir, '.matrix-daemon.log'),
  pipeName: `\\\\.\\pipe\\matrix-${machineName}`,
  machineName,
  syncFilter: { room: { timeline: { limit: 20 } } },
  presenceIntervalMs: 5 * 60 * 1000,  // 5 min
  reconnectBaseMs: 1000,
  reconnectMaxMs: 60_000,
};
```

- [ ] **Step 4: Run test again.**

```bash
npx jest tests/config.test.js
# Expected: PASS
```

- [ ] **Step 5: Commit.**

```bash
cd "/d/GameDev/X3Native"
git add ".."  # the matrix-daemon directory is outside the repo
# Actually — matrix-daemon lives in ~/.claude/ — separate "repo" or version control later
# For now: just track changes to the spec
```

### Task 25: Write the login + token-load module

**Files:**
- Create: `C:\Users\Tim Smith\.claude\matrix-daemon\login.js`
- Create: `C:\Users\Tim Smith\.claude\matrix-daemon\tests\login.test.js`

- [ ] **Step 1: Write the failing test.**

```javascript
// tests/login.test.js
const fs = require('fs');
const path = require('path');
const { loadToken, createClient } = require('../login');

describe('login', () => {
  const tmpTokenPath = path.join(__dirname, 'tmp-token.txt');
  beforeEach(() => fs.writeFileSync(tmpTokenPath, 'xoxb-fake-test-token\n'));
  afterEach(() => fs.existsSync(tmpTokenPath) && fs.unlinkSync(tmpTokenPath));

  test('loadToken strips whitespace', () => {
    expect(loadToken(tmpTokenPath)).toBe('xoxb-fake-test-token');
  });
  test('loadToken throws on missing file', () => {
    expect(() => loadToken('/does/not/exist')).toThrow();
  });
  test('createClient returns a MatrixClient instance', () => {
    const client = createClient('https://example.com', 'token');
    expect(client.constructor.name).toBe('MatrixClient');
  });
});
```

- [ ] **Step 2: Run, verify fails.**

```bash
npx jest tests/login.test.js
# Expected: FAIL
```

- [ ] **Step 3: Implement `login.js`.**

```javascript
// login.js
const fs = require('fs');
const { MatrixClient, SimpleFsStorageProvider, AutojoinRoomsMixin } = require('matrix-bot-sdk');
const path = require('path');
const os = require('os');

function loadToken(tokenPath) {
  if (!fs.existsSync(tokenPath)) {
    throw new Error(`access token file not found: ${tokenPath}`);
  }
  return fs.readFileSync(tokenPath, 'utf8').trim();
}

function createClient(homeserverUrl, accessToken, storagePath) {
  const storage = new SimpleFsStorageProvider(
    storagePath || path.join(os.homedir(), '.claude', 'matrix-daemon-storage.json')
  );
  const client = new MatrixClient(homeserverUrl, accessToken, storage);
  AutojoinRoomsMixin.setupOnClient(client);
  return client;
}

module.exports = { loadToken, createClient };
```

- [ ] **Step 4: Run test again.**

```bash
npx jest tests/login.test.js
# Expected: PASS
```

### Task 26: Write the inbox writer

**Files:**
- Create: `C:\Users\Tim Smith\.claude\matrix-daemon\inbox.js`
- Create: `C:\Users\Tim Smith\.claude\matrix-daemon\tests\inbox.test.js`

- [ ] **Step 1: Write the failing test.**

```javascript
// tests/inbox.test.js
const fs = require('fs');
const path = require('path');
const { appendIncoming } = require('../inbox');

describe('inbox', () => {
  const tmpInbox = path.join(__dirname, 'tmp-inbox.jsonl');
  beforeEach(() => fs.existsSync(tmpInbox) && fs.unlinkSync(tmpInbox));
  afterEach(() => fs.existsSync(tmpInbox) && fs.unlinkSync(tmpInbox));

  test('appendIncoming writes one JSON line per message', () => {
    appendIncoming(tmpInbox, {
      source: 'matrix',
      room: '!fleet-ops:fleetcommand.slopclaude.com',
      sender: '@tim:fleetcommand.slopclaude.com',
      text: 'hello djbooth',
      eventId: '$evt1',
      ts: 1700000000000,
    });
    appendIncoming(tmpInbox, {
      source: 'matrix',
      room: '!fleet-ops:fleetcommand.slopclaude.com',
      sender: '@13700k:fleetcommand.slopclaude.com',
      text: 'how is gating?',
      eventId: '$evt2',
      ts: 1700000001000,
    });
    const lines = fs.readFileSync(tmpInbox, 'utf8').trim().split('\n');
    expect(lines).toHaveLength(2);
    expect(JSON.parse(lines[0]).text).toBe('hello djbooth');
    expect(JSON.parse(lines[1]).sender).toBe('@13700k:fleetcommand.slopclaude.com');
  });
});
```

- [ ] **Step 2: Run, verify fails.**

```bash
npx jest tests/inbox.test.js
# Expected: FAIL
```

- [ ] **Step 3: Write `inbox.js`.**

```javascript
// inbox.js
const fs = require('fs');

function appendIncoming(inboxPath, message) {
  const line = JSON.stringify(message) + '\n';
  fs.appendFileSync(inboxPath, line, { encoding: 'utf8' });
}

module.exports = { appendIncoming };
```

- [ ] **Step 4: Run test again.** Expected PASS.

### Task 27: Write the outbox named-pipe listener

**Files:**
- Create: `C:\Users\Tim Smith\.claude\matrix-daemon\outbox.js`
- Create: `C:\Users\Tim Smith\.claude\matrix-daemon\tests\outbox.test.js`

- [ ] **Step 1: Write the failing test.**

```javascript
// tests/outbox.test.js
const { parseOutgoing } = require('../outbox');

describe('outbox', () => {
  test('parseOutgoing accepts valid JSON', () => {
    const buf = Buffer.from(JSON.stringify({ room: '!x:y', text: 'hi' }));
    const msg = parseOutgoing(buf);
    expect(msg.room).toBe('!x:y');
    expect(msg.text).toBe('hi');
  });
  test('parseOutgoing rejects malformed JSON', () => {
    expect(() => parseOutgoing(Buffer.from('{not json'))).toThrow();
  });
  test('parseOutgoing requires room+text fields', () => {
    expect(() => parseOutgoing(Buffer.from(JSON.stringify({ room: '!x:y' })))).toThrow(/text/);
  });
});
```

- [ ] **Step 2: Run, verify fails.**

```bash
npx jest tests/outbox.test.js
# Expected: FAIL
```

- [ ] **Step 3: Write `outbox.js`.**

```javascript
// outbox.js
const net = require('net');

function parseOutgoing(buffer) {
  const msg = JSON.parse(buffer.toString('utf8'));
  if (!msg.room) throw new Error('outbox message missing field: room');
  if (!msg.text) throw new Error('outbox message missing field: text');
  return msg;
}

function startOutboxListener(pipeName, onMessage) {
  const server = net.createServer(socket => {
    const chunks = [];
    socket.on('data', d => chunks.push(d));
    socket.on('end', () => {
      try {
        const msg = parseOutgoing(Buffer.concat(chunks));
        onMessage(msg).then(
          result => socket.write(JSON.stringify({ ok: true, ...result })),
          err => socket.write(JSON.stringify({ ok: false, error: err.message })),
        ).finally(() => socket.end());
      } catch (e) {
        socket.write(JSON.stringify({ ok: false, error: e.message }));
        socket.end();
      }
    });
  });
  server.listen(pipeName);
  return server;
}

module.exports = { parseOutgoing, startOutboxListener };
```

- [ ] **Step 4: Run test again.** Expected PASS.

### Task 28: Wire daemon.js — the main event loop

**Files:**
- Create: `C:\Users\Tim Smith\.claude\matrix-daemon\daemon.js`

- [ ] **Step 1: Write daemon.js.**

```javascript
// daemon.js
const config = require('./config');
const { loadToken, createClient } = require('./login');
const { appendIncoming } = require('./inbox');
const { startOutboxListener } = require('./outbox');
const winston = require('winston');

const logger = winston.createLogger({
  format: winston.format.combine(winston.format.timestamp(), winston.format.json()),
  transports: [
    new winston.transports.Console(),
    new winston.transports.File({ filename: config.logPath }),
  ],
});

async function main() {
  const token = loadToken(config.accessTokenPath);
  const client = createClient(config.homeserverUrl, token);

  // ---- inbox writer on every text message in joined rooms or DMs ----
  client.on('room.message', async (roomId, event) => {
    if (!event.content || event.content.msgtype !== 'm.text') return;
    if (event.sender === await client.getUserId()) return;  // skip our own posts
    const text = event.content.body || '';
    appendIncoming(config.inboxPath, {
      source: 'matrix',
      room: roomId,
      sender: event.sender,
      text,
      eventId: event.event_id,
      ts: event.origin_server_ts,
    });
    logger.info({ event: 'inbox-append', room: roomId, sender: event.sender });
  });

  // ---- outbox: named-pipe server accepts {room, text, ...} from Claude ----
  startOutboxListener(config.pipeName, async msg => {
    const eventId = await client.sendMessage(msg.room, {
      msgtype: 'm.text',
      body: msg.text,
      ...(msg.mention ? { 'm.mentions': { user_ids: msg.mention } } : {}),
    });
    logger.info({ event: 'outbox-sent', room: msg.room, eventId });
    return { eventId };
  });

  // ---- presence: emit "online" every 5 min ----
  setInterval(async () => {
    try {
      await client.setPresenceStatus('online', `DJBOOTH alive @ ${new Date().toISOString()}`);
    } catch (e) {
      logger.warn({ event: 'presence-fail', error: e.message });
    }
  }, config.presenceIntervalMs);

  logger.info({ event: 'starting-sync', machine: config.machineName });
  await client.start();
}

main().catch(err => {
  logger.error({ event: 'fatal', error: err.message, stack: err.stack });
  process.exit(1);
});
```

- [ ] **Step 2: Smoketest with the real token (DJBOOTH only).**

```bash
cd "/c/Users/Tim Smith/.claude/matrix-daemon"
export PATH="/c/Program Files/nodejs:$PATH"
node daemon.js &
sleep 10
# Check log
tail -10 "$HOME/.claude/.matrix-daemon.log"
# Expected: { "event": "starting-sync", "machine": "djbooth", ... }
# Kill it for now
pkill -f "node daemon.js"
```

### Task 29: Install daemon as Windows Scheduled Task

- [ ] **Step 1: Register.**

```powershell
$daemonPath = "$env:USERPROFILE\.claude\matrix-daemon\daemon.js"
$action = New-ScheduledTaskAction -Execute 'C:\Program Files\nodejs\node.exe' -Argument $daemonPath -WorkingDirectory "$env:USERPROFILE\.claude\matrix-daemon"
$trigger = New-ScheduledTaskTrigger -AtLogOn
$settings = New-ScheduledTaskSettingsSet -StartWhenAvailable -RestartCount 5 -RestartInterval (New-TimeSpan -Minutes 1) -ExecutionTimeLimit (New-TimeSpan -Days 365) -MultipleInstances IgnoreNew
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive
Register-ScheduledTask -TaskName 'DJBOOTH-MatrixDaemon' -Action $action -Trigger $trigger -Settings $settings -Principal $principal -Description 'Matrix bot daemon (matrix-bot-sdk) for @djbooth.'
Start-ScheduledTask -TaskName 'DJBOOTH-MatrixDaemon'
```

- [ ] **Step 2: Verify.**

```powershell
Get-ScheduledTask -TaskName 'DJBOOTH-MatrixDaemon' | Get-ScheduledTaskInfo
# Expected: LastTaskResult = 0
```

- [ ] **Step 3: Tail the log.**

```bash
tail -f "$HOME/.claude/.matrix-daemon.log"
# Should see sync events streaming
```

---

## Phase 1.G — Replicate daemon to 13700K

### Task 30: Copy daemon project to 13700K

- [ ] **Step 1: On 13700K, mirror the directory structure.**

```powershell
$src = "\\DJBOOTH\C$\Users\Tim Smith\.claude\matrix-daemon"
$dst = "$env:USERPROFILE\.claude\matrix-daemon"
robocopy $src $dst /E /XD node_modules
```

- [ ] **Step 2: Install dependencies on 13700K.**

```powershell
cd $dst
& 'C:\Program Files\nodejs\npm.cmd' install
```

- [ ] **Step 3: Use 13700K's bot token.**

The `~/.claude/.matrix_token` file on 13700K must contain 13700K's bot access token from Task 22 step 3 (NOT DJBOOTH's token).

- [ ] **Step 4: Run + verify.**

```powershell
$env:MATRIX_BOT_MACHINE = '13700k'
& 'C:\Program Files\nodejs\node.exe' "$env:USERPROFILE\.claude\matrix-daemon\daemon.js"
# Expect "starting-sync" in the log; pipe name is \\.\pipe\matrix-13700k
```

- [ ] **Step 5: Register Scheduled Task (same as Task 29 with TaskName `Integrator-MatrixDaemon`).**

---

## Phase 1.H — #fleet-ops channel + first cross-message

### Task 31: Create the #fleet-ops room

- [ ] **Step 1: Tim creates the room from Element Web.** Settings: public to fleet members, name "Fleet Ops", topic "Fleet coordination + cross-machine RPC."

- [ ] **Step 2: Tim invites `@djbooth` and `@13700k` to the room.**

- [ ] **Step 3: Verify both bots auto-joined.** AutojoinRoomsMixin in `login.js` handles this. Check the daemon log on both machines:

```bash
tail -5 "$HOME/.claude/.matrix-daemon.log"
# Expected: events showing room join
```

### Task 32: First cross-machine message

- [ ] **Step 1: From DJBOOTH (this machine), send a message to the room via the named pipe.**

```powershell
$pipeStream = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'matrix-djbooth', [System.IO.Pipes.PipeDirection]::InOut)
$pipeStream.Connect(2000)
$msg = @{ room = '!FLEET-OPS-ROOM-ID:fleetcommand.slopclaude.com'; text = 'DJBOOTH online via Matrix — first cross-fleet ping' } | ConvertTo-Json -Compress
$writer = New-Object System.IO.StreamWriter($pipeStream)
$writer.Write($msg)
$writer.Flush()
$pipeStream.Dispose()
```

- [ ] **Step 2: From 13700K, tail the inbox.**

```bash
tail -1 "$HOME/.claude/.matrix_inbox.jsonl"
# Expected: {"source":"matrix","room":"!FLEET-OPS-ROOM-ID:...","sender":"@djbooth:...","text":"DJBOOTH online via Matrix...",...}
```

- [ ] **Step 3: Reply from 13700K via its named pipe.**

```powershell
$pipeStream = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'matrix-13700k', [System.IO.Pipes.PipeDirection]::InOut)
$pipeStream.Connect(2000)
$msg = @{ room = '!FLEET-OPS-ROOM-ID:fleetcommand.slopclaude.com'; text = '13700K integrator copies — round trip OK' } | ConvertTo-Json -Compress
$writer = New-Object System.IO.StreamWriter($pipeStream)
$writer.Write($msg)
$writer.Flush()
$pipeStream.Dispose()
```

- [ ] **Step 4: From DJBOOTH, tail its inbox.**

```bash
tail -1 "$HOME/.claude/.matrix_inbox.jsonl"
# Expected: 13700K's reply
```

- [ ] **Step 5: Tim sees both messages in Element on his phone.**

---

## Phase 1.I — Slack mirroring during the soak

### Task 33: Extend slack-daemon.ps1 to cross-post outbound to Matrix

**Files:**
- Modify: `C:\Users\Tim Smith\.claude\slack-daemon.ps1`

- [ ] **Step 1: After the existing inbox write block, add Matrix cross-post.**

Add this block after the "captured X new message(s)" log line in `slack-daemon.ps1`:

```powershell
# --- Matrix mirror: also write to .matrix_inbox.jsonl so the Matrix-side ---
# --- /loop drains via the same handler. Idempotent: same JSON line shape.  ---
foreach ($m in $newMessages) {
  $mirrored = [PSCustomObject]@{
    source       = 'slack-mirror'
    slack_channel = $m.channel
    slack_user   = $m.user
    text         = $m.text
    ts           = $m.ts
  }
  ($mirrored | ConvertTo-Json -Compress) | Add-Content -Path "$env:USERPROFILE\.claude\.matrix_inbox.jsonl" -Encoding utf8
}
```

- [ ] **Step 2: Trigger a run + verify both inboxes get the line.**

```powershell
& "$env:USERPROFILE\.claude\slack-daemon.ps1"
Write-Output "=== slack inbox ==="
Get-Content "$env:USERPROFILE\.claude\.slack_inbox.jsonl" -Tail 1
Write-Output "=== matrix inbox (should have same payload mirrored) ==="
Get-Content "$env:USERPROFILE\.claude\.matrix_inbox.jsonl" -Tail 1
```

### Task 34: Forward Matrix outbox sends back to Slack (optional, Phase 3 deferred)

This is the Phase 3 work — for Phase 1 we leave it one-way: Slack → Matrix only. Tim's primary input is now Matrix on his phone; Slack acts as a fallback.

---

## Phase 1.J — Backup script

### Task 35: Nightly Conduit DB backup

**Files:**
- Create: `C:\opt\conduit\backup.ps1`

- [ ] **Step 1: Install rclone via winget.**

```powershell
winget install Rclone.Rclone --silent --accept-package-agreements --accept-source-agreements
```

- [ ] **Step 2: Configure rclone to point at Dropbox.**

```powershell
& 'C:\Program Files\Rclone\rclone.exe' config
# Interactive: name=dropbox, type=dropbox, follow OAuth prompts in browser
```

- [ ] **Step 3: Write the backup script.**

```powershell
$backupScript = @'
$source = "C:\opt\conduit\db"
$dest = "dropbox:fleet-backups/conduit/$(Get-Date -Format yyyy-MM-dd)"
& "C:\Program Files\Rclone\rclone.exe" sync $source $dest --log-file C:\opt\conduit\logs\backup-$(Get-Date -Format yyyyMMdd).log
# Prune older than 14 days
$cutoff = (Get-Date).AddDays(-14)
& "C:\Program Files\Rclone\rclone.exe" delete "dropbox:fleet-backups/conduit" --min-age 14d
'@
Set-Content -Path 'C:\opt\conduit\backup.ps1' -Value $backupScript -Encoding utf8
```

- [ ] **Step 4: Schedule nightly at 03:00.**

```powershell
$action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument '-NoProfile -ExecutionPolicy Bypass -File C:\opt\conduit\backup.ps1'
$trigger = New-ScheduledTaskTrigger -Daily -At '03:00'
$settings = New-ScheduledTaskSettingsSet -StartWhenAvailable -ExecutionTimeLimit (New-TimeSpan -Minutes 30)
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType S4U
Register-ScheduledTask -TaskName 'Conduit-NightlyBackup' -Action $action -Trigger $trigger -Settings $settings -Principal $principal -Description 'Nightly Conduit DB backup to Dropbox.'
```

- [ ] **Step 5: Trigger a manual run + verify destination.**

```powershell
Start-ScheduledTask -TaskName 'Conduit-NightlyBackup'
Start-Sleep -Seconds 30
& 'C:\Program Files\Rclone\rclone.exe' ls "dropbox:fleet-backups/conduit/$(Get-Date -Format yyyy-MM-dd)"
# Expected: lists Conduit's SQLite files
```

---

## Phase 1 success criteria checklist (from spec §11)

- [ ] Tim DMs `@djbooth` from iPhone over cellular → DJBOOTH's next Claude session receives + responds
- [ ] DJBOOTH mentions `@13700k` in #fleet-ops → 13700K daemon writes it to inbox in <1 second
- [ ] Conduit's DB survives a 13700K reboot without manual intervention
- [ ] Cloudflare Tunnel auto-reconnects after WAN flap (test: unplug 13700K's ethernet for 30s, plug back, verify Element reconnects)
- [ ] Element Web at `fleetcommand.slopclaude.com` shows the basic config (custom theme is Phase 2)
- [ ] Backup script wrote a fresh DB snapshot to Dropbox last night
- [ ] System has been up 168 consecutive hours (1 week) without manual intervention

---

## Self-review notes

- **Spec coverage:** §1–§11 of the spec are covered. §4.5 (custom theme) is Phase 2 — explicitly noted in success criteria. §4.6 (sidebar widget) is Phase 2 — not in this plan.
- **No placeholders:** all code blocks are complete. Steps that depend on Tim-resolved §9 questions (e.g., `fleetcommand.slopclaude.com`) are clearly marked with `<...>` placeholders Tim fills before execution.
- **Type consistency:** functions exported from each module (`loadToken`, `createClient`, `appendIncoming`, `parseOutgoing`, `startOutboxListener`) are referenced consistently in `daemon.js`.
- **Scope:** Phase 1 only. Phase 2 (theme, widget, remaining daemons) gets a separate plan.

---

## Execution handoff

**Plan complete and saved to `docs/superpowers/plans/2026-05-27-fleet-messaging-phase1-plan.md`. Two execution options when Tim wakes:**

1. **Subagent-Driven (recommended)** — fresh subagent per task, review between tasks
2. **Inline Execution** — batch through tasks with checkpoints at major boundaries (after Conduit deploy, after Tunnel up, after daemon running)

Phase 1.A is Tim-gated (he picks the domain, confirms Cloudflare account, etc.). Phase 1.B onwards can be subagent-driven once those answers are in.
