# matrix-daemon — per-machine Matrix bot daemon

Long-running Node.js process that each fleet PC runs to participate in the Matrix-based fleet messaging fabric. Pairs with the `lan-bus` tooling and the Conduit homeserver on 13700K (see `docs/superpowers/specs/2026-05-27-fleet-messaging-design.md`).

## What it does

1. Logs in to the fleet Matrix server (`fleetcommand.slopclaude.com`) as `@<machine>:fleetcommand.slopclaude.com` using an access token saved at `~/.claude/.matrix_token` (mode 700)
2. Maintains a persistent sync connection — sub-second latency for incoming messages
3. On every `room.message` event in joined rooms or DMs: appends a JSON line to `~/.claude/.matrix_inbox.jsonl`
4. Listens on a Windows named pipe (`\\.\pipe\matrix-<machine>`) for outbound message requests from the local Claude Code session
5. Emits an `m.presence` "online" status every 5 minutes so peers see this machine alive
6. Auto-reconnects on disconnect (matrix-bot-sdk's built-in handling)

## Files

| File | Purpose |
|---|---|
| `config.js` | All env-driven paths + ports. Single source of truth for daemon configuration. |
| `login.js` | Token loading + MatrixClient construction. Pure wrapper around matrix-bot-sdk. |
| `inbox.js` | Append-only writer for `~/.claude/.matrix_inbox.jsonl`. |
| `outbox.js` | Named-pipe server that accepts outbound message requests from the local Claude session. |
| `daemon.js` | Entry point. Wires login + inbox + outbox + presence + sync loop. |
| `package.json` | npm manifest. Deps: `matrix-bot-sdk`, `winston`, `jest` (dev). |
| `media.js` | Pure helpers: MIME guess + header-based image dimensions for outbox uploads. |
| `tests/*.test.js` | Jest unit tests — 32 cover config, login, inbox, outbox, media. |

## State files (not in repo)

| Path | Contents |
|---|---|
| `~/.claude/.matrix_token` | Bot's access token (`syt_*` Matrix style). Mode 700. |
| `~/.claude/.matrix_inbox.jsonl` | Append-only log of incoming events. Drained by Claude session. |
| `~/.claude/.matrix_outbox.jsonl` | Audit log of outgoing messages (TBD: not yet written by daemon). |
| `~/.claude/.matrix-daemon.log` | Daemon's winston log (rotating, 5 MB × 3 files). |
| `~/.claude/.matrix_seen.json` | Sync token snapshot (matrix-bot-sdk manages internally via storage). |
| `~/.claude/matrix-daemon-storage.json` | matrix-bot-sdk's SimpleFsStorageProvider state. |

## Outbox API (named pipe)

POST a single JSON object to `\\.\pipe\matrix-<machine>`:

```json
{
  "room": "!FLEET-OPS-ROOM-ID:fleetcommand.slopclaude.com",
  "text": "merge feat/portal-hub when ready",
  "mention": ["@13700k:fleetcommand.slopclaude.com"],
  "thread_root": "$evtid:fleetcommand.slopclaude.com"
}
```

A message must carry **`text`, `image`, or both**. To post a screenshot, set
`image` to a local file path; the daemon uploads it to the homeserver media
repo and sends an `m.image` event. When `text` is also present it becomes the
image's caption. `mention` and `thread_root` work for images too.

```json
{
  "room": "!FLEET-OPS-ROOM-ID:fleetcommand.slopclaude.com",
  "image": "C:/gamedev/incoming/render.png",
  "text": "Jake's ship — full-res turntable"
}
```

Image dimensions are parsed from the file header (PNG/JPEG/GIF/BMP/WebP) with no
extra dependency; the upload rides the daemon's already-authenticated client, so
it isn't subject to the Cloudflare default-UA block that the standalone
`tools/fleet/fleet_image.py` helper has to work around.

**Response 200 (one JSON object on the same connection):**
```json
{ "ok": true, "eventId": "$newevt:fleetcommand.slopclaude.com" }
```

**Response on error:**
```json
{ "ok": false, "error": "<reason>" }
```

**PowerShell client example:**
```powershell
$pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'matrix-djbooth', [System.IO.Pipes.PipeDirection]::InOut)
$pipe.Connect(2000)
$msg = @{ room = '!fleet:fleetcommand.slopclaude.com'; text = 'hello' } | ConvertTo-Json -Compress
$writer = New-Object System.IO.StreamWriter($pipe)
$writer.Write($msg); $writer.Flush(); $writer.Close()
$reader = New-Object System.IO.StreamReader($pipe)
$resp = $reader.ReadToEnd()
$pipe.Dispose()
Write-Output $resp
```

## Setup (per fleet PC)

```bash
cd ~/.claude/
git clone <where-this-repo-or-its-tools-dir-lives> matrix-daemon-src
cp -r matrix-daemon-src/tools/matrix-daemon ./matrix-daemon
cd matrix-daemon
npm install
```

Then save the access token from Conduit (see spec §4.3 / plan Task 22):

```bash
echo 'syt_...your-bot-token...' > ~/.claude/.matrix_token
chmod 600 ~/.claude/.matrix_token
```

Set the machine name env var if it differs from the hostname:

```powershell
[System.Environment]::SetEnvironmentVariable('MATRIX_BOT_MACHINE','djbooth','User')
```

Run once manually to smoketest:

```bash
export PATH="/c/Program Files/nodejs:$PATH"
cd ~/.claude/matrix-daemon
node daemon.js
# Should log "identified" with the bot's user_id and "starting-sync"
# Ctrl+C to stop after confirming
```

Register as Scheduled Task (Windows):

```powershell
$daemonPath = "$env:USERPROFILE\.claude\matrix-daemon\daemon.js"
$action = New-ScheduledTaskAction -Execute 'C:\Program Files\nodejs\node.exe' -Argument $daemonPath -WorkingDirectory "$env:USERPROFILE\.claude\matrix-daemon"
$trigger = New-ScheduledTaskTrigger -AtLogOn
$settings = New-ScheduledTaskSettingsSet -StartWhenAvailable -RestartCount 5 -RestartInterval (New-TimeSpan -Minutes 1) -ExecutionTimeLimit (New-TimeSpan -Days 365) -MultipleInstances IgnoreNew
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive
Register-ScheduledTask -TaskName "$env:COMPUTERNAME-MatrixDaemon" -Action $action -Trigger $trigger -Settings $settings -Principal $principal -Description 'Matrix bot daemon (matrix-bot-sdk).'
Start-ScheduledTask -TaskName "$env:COMPUTERNAME-MatrixDaemon"
```

## Testing

```bash
cd ~/.claude/matrix-daemon
npx jest
# Expected: 18 tests pass across 4 suites
```

Tests cover pure logic (config defaults, token loading, inbox writes, outbox parsing). The network-touching parts (matrix-bot-sdk sync, presence emission) are exercised by end-to-end smoketest, not unit tests.

## Environment variables

| Var | Default | Purpose |
|---|---|---|
| `MATRIX_HOMESERVER_URL` | `https://fleetcommand.slopclaude.com` | Conduit endpoint |
| `MATRIX_BOT_MACHINE` | `os.hostname().toLowerCase()` | Identifies which fleet PC this is |
| `CLAUDE_DIR` | `~/.claude` | Root for state files |

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| "matrix access token file not found" | `~/.claude/.matrix_token` missing | Register the bot account, save the access_token |
| Sync immediately exits | Wrong homeserver URL or expired token | Check daemon log for HTTP error; re-register |
| Inbox not growing | Daemon not joined to room | Tim invites `@<machine>` to the room; AutojoinRoomsMixin handles the join |
| Named pipe connect fails | Daemon not running OR wrong machine name | `Get-Process node` to check; verify `MATRIX_BOT_MACHINE` matches pipe consumer |
| Presence not updating | matrix-bot-sdk version skew | Update `matrix-bot-sdk` dep, rerun smoketest |
