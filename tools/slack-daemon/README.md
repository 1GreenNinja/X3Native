# slack-daemon — per-machine Slack inbox poller

Long-running PowerShell process that each fleet PC runs to receive incoming Slack messages addressed to its bot account, plus mirror them to the Matrix inbox during the Slack → Matrix migration soak.

## What it does

1. Reads the bot's `xoxb-...` token from `~/.claude/.slack_token` (mode 700, NOT in repo)
2. Polls Slack's `conversations.history` API every 5 minutes (via Scheduled Task) on:
   - The bot's DM channel with Tim (e.g. `D0B6CNV1T34` for DJBOOTH ↔ Tim)
   - `#fleet-ops` channel (`C0B76SQ0XQ8`), filtered for messages mentioning the bot's user_id
3. Appends new messages to `~/.claude/.slack_inbox.jsonl` (drained by the local Claude Code session via `/loop` or "check Slack" prompt)
4. **Plan Task 33 mirror:** ALSO appends to `~/.claude/.matrix_inbox.jsonl` with `source="slack-mirror"` so the unified Matrix-side drain handles both surfaces post-deployment
5. Maintains `~/.claude/.slack_seen.json` (per-channel last-seen-ts) so repeated runs don't re-process the same messages
6. Rolls `~/.claude/.slack-daemon.log` (last 200 lines kept after 50 KB)

## Files

| File | Purpose |
|---|---|
| `daemon.ps1` | Single-poll PowerShell script — runs once per Scheduled Task fire, exits |
| `README.md` | This file |

## State files (not in repo)

| Path | Contents |
|---|---|
| `~/.claude/.slack_token` | Bot's xoxb access token (mode 700) |
| `~/.claude/.slack_inbox.jsonl` | Append-only log of incoming Slack messages |
| `~/.claude/.slack_seen.json` | `{"dm":"<ts>","fleet":"<ts>"}` last-seen markers |
| `~/.claude/.slack-daemon.log` | Rolling daemon log |
| `~/.claude/.matrix_inbox.jsonl` | Cross-mirror target (Slack messages also land here) |

## Per-machine setup

The daemon embeds DJBOOTH's specific Slack user IDs + channel IDs at the top of the script. **Each new fleet PC needs to customize these constants** before running:

```powershell
# At the top of daemon.ps1:
$BotUserId    = 'U0B69BFULAZ'   # this machine's Slack bot user_id (xoxb-bot's @id)
$TimUserId    = 'U0B6489G0CC'   # Tim's user_id (same on every machine — this is workspace-level)
$DmChannel    = 'D0B6CNV1T34'   # IM channel between THIS bot and Tim
$FleetChannel = 'C0B76SQ0XQ8'   # #fleet-ops channel (same on every machine)
```

To find a new bot's IDs after creating its Slack app + installing it:

```powershell
$token = Get-Content -Raw "$env:USERPROFILE\.claude\.slack_token"
$token = $token.Trim()
# auth.test reveals the bot's user_id
Invoke-RestMethod -Uri 'https://slack.com/api/auth.test' -Headers @{ Authorization = "Bearer $token" }
# conversations.open with Tim's user_id reveals the DM channel id
Invoke-RestMethod -Method Post -Uri 'https://slack.com/api/conversations.open' `
  -Headers @{ Authorization = "Bearer $token"; 'Content-Type' = 'application/json; charset=utf-8' } `
  -Body '{"users":"U0B6489G0CC"}'
```

## Installing as a Scheduled Task

```powershell
$daemonPath = "$env:USERPROFILE\.claude\slack-daemon.ps1"  # or wherever you deploy
$action = New-ScheduledTaskAction -Execute 'powershell.exe' `
  -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$daemonPath`""
$trigger = New-ScheduledTaskTrigger -Once -At (Get-Date).AddMinutes(1) `
  -RepetitionInterval (New-TimeSpan -Minutes 5)
$settings = New-ScheduledTaskSettingsSet -StartWhenAvailable `
  -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
  -ExecutionTimeLimit (New-TimeSpan -Minutes 2) -MultipleInstances IgnoreNew
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive
Register-ScheduledTask -TaskName "$env:COMPUTERNAME-SlackPoller" `
  -Action $action -Trigger $trigger -Settings $settings -Principal $principal `
  -Description 'Polls Slack every 5 min for this machine bot. Source: tools/slack-daemon/'
```

## Testing manually

```powershell
# One-off run
pwsh -NoProfile -ExecutionPolicy Bypass -File "$env:USERPROFILE\.claude\slack-daemon.ps1"
# Check state
Get-Content "$env:USERPROFILE\.claude\.slack_seen.json"
Get-Content "$env:USERPROFILE\.claude\.slack-daemon.log" -Tail 5
Get-Content "$env:USERPROFILE\.claude\.slack_inbox.jsonl" -Tail 3
```

## Migration status

The Slack daemon is **kept** during the Phase 2 migration to Matrix. Plan §8:

| Day | What |
|---|---|
| **D-0** (today) | Slack-only — DJBOOTH daemon polling every 5 min |
| **D+1 of Matrix** | Conduit deployed; matrix-daemons online on DJBOOTH + 13700K. Slack daemon continues + Slack→Matrix mirror block writes to `.matrix_inbox.jsonl` |
| **D+7 to D+14** | Tim switches phone push notifications to Element. Slack stays as fallback |
| **D+30** | Matrix-only cutover. Slack daemon can be disabled OR kept dormant for archive access |

## Wire format (inbox JSON line)

```json
{
  "source": "dm",
  "channel": "D0B6CNV1T34",
  "user": "U0B6489G0CC",
  "ts": "1779934051.595419",
  "text": "Hey! if you're around..."
}
```

When `source = "channel"` (mention in #fleet-ops), the user is the mentioning author and the channel is `C0B76SQ0XQ8`.

The Matrix mirror writes a slightly different shape:

```json
{
  "source": "slack-mirror",
  "slack_channel": "D0B6CNV1T34",
  "slack_user": "U0B6489G0CC",
  "text": "...",
  "ts": "1779934051.595419",
  "original_source": "dm"
}
```
