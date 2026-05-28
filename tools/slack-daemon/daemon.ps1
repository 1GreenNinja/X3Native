# slack-daemon.ps1 — DJBOOTH's long-running Slack inbox poller.
#
# Runs as a Windows Scheduled Task (every 5 min). Each invocation:
#   1. Reads the bot token from ~/.claude/.slack_token
#   2. Reads last-seen timestamps from ~/.claude/.slack_seen.json (init if missing)
#   3. Polls Slack for NEW messages since those timestamps:
#        - DMs to djbooth on channel D0B6CNV1T34 (Tim's DM)
#        - @djbooth mentions in #fleet-ops (C0B76SQ0XQ8)
#   4. Appends each new message as one JSON line to ~/.claude/.slack_inbox.jsonl
#   5. Updates ~/.claude/.slack_seen.json with the newest ts seen
#   6. Logs to ~/.claude/.slack-daemon.log (rolling, keep last 200 lines)
#
# A Claude Code session at DJBOOTH reads .slack_inbox.jsonl on demand
# ("check Slack" or via /loop), responds, and truncates the file.

$ErrorActionPreference = 'Stop'

$ClaudeDir = "$env:USERPROFILE\.claude"
$TokenFile = "$ClaudeDir\.slack_token"
$SeenFile  = "$ClaudeDir\.slack_seen.json"
$InboxFile = "$ClaudeDir\.slack_inbox.jsonl"
$LogFile   = "$ClaudeDir\.slack-daemon.log"

# DJBOOTH's known Slack identifiers
$BotUserId   = 'U0B69BFULAZ'   # djbooth bot user
$TimUserId   = 'U0B6489G0CC'   # Tim
$DmChannel   = 'D0B6CNV1T34'   # Tim <-> djbooth DM
$FleetChannel = 'C0B76SQ0XQ8'  # #fleet-ops

function Write-Log($msg) {
  $line = "$(Get-Date -Format 'yyyy-MM-ddTHH:mm:sszzz') $msg"
  Add-Content -Path $LogFile -Value $line -Encoding utf8
  # Roll log if > 200 lines
  if ((Get-Item $LogFile -ErrorAction SilentlyContinue).Length -gt 50000) {
    $kept = Get-Content $LogFile -Tail 200
    Set-Content -Path $LogFile -Value $kept -Encoding utf8
  }
}

try {
  if (-not (Test-Path $TokenFile)) { Write-Log "ERR: no token file at $TokenFile"; exit 1 }
  $token = (Get-Content -Raw $TokenFile).Trim()
  if (-not $token) { Write-Log "ERR: token file empty"; exit 1 }

  # Load or init seen state
  if (Test-Path $SeenFile) {
    $seen = Get-Content -Raw $SeenFile | ConvertFrom-Json
  } else {
    # First run: start from "now" (skip historical messages)
    $now = [Math]::Round((Get-Date -UFormat %s)).ToString()
    $seen = @{ dm = $now; fleet = $now }
    $seen | ConvertTo-Json | Set-Content -Path $SeenFile -Encoding utf8
    Write-Log "init: seen state created, starting from now ($now)"
  }

  $headers = @{ Authorization = "Bearer $token" }
  $newMessages = @()

  # --- Poll DM channel ---
  $url = "https://slack.com/api/conversations.history?channel=$DmChannel&oldest=$($seen.dm)&inclusive=false&limit=20"
  $resp = Invoke-RestMethod -Uri $url -Headers $headers -Method Get
  if (-not $resp.ok) { Write-Log "ERR dm history: $($resp.error)" }
  else {
    foreach ($m in $resp.messages) {
      # Skip messages from the bot itself
      if ($m.user -eq $BotUserId -or $m.bot_id) { continue }
      $newMessages += [PSCustomObject]@{
        source  = 'dm'
        channel = $DmChannel
        user    = $m.user
        ts      = $m.ts
        text    = $m.text
      }
    }
    if ($resp.messages.Count -gt 0) {
      # Newest ts is the first element (Slack returns newest-first)
      $newest = ($resp.messages | Sort-Object { [double]$_.ts } -Descending | Select-Object -First 1).ts
      if ([double]$newest -gt [double]$seen.dm) { $seen.dm = $newest }
    }
  }

  # --- Poll #fleet-ops for @djbooth mentions ---
  $url = "https://slack.com/api/conversations.history?channel=$FleetChannel&oldest=$($seen.fleet)&inclusive=false&limit=20"
  $resp = Invoke-RestMethod -Uri $url -Headers $headers -Method Get
  if (-not $resp.ok) { Write-Log "ERR fleet history: $($resp.error)" }
  else {
    foreach ($m in $resp.messages) {
      if ($m.user -eq $BotUserId -or $m.bot_id) { continue }
      # Only capture if @djbooth is mentioned (look for <@U0B69BFULAZ> in text)
      if ($m.text -match "<@$BotUserId>") {
        $newMessages += [PSCustomObject]@{
          source  = 'channel'
          channel = $FleetChannel
          user    = $m.user
          ts      = $m.ts
          text    = $m.text
        }
      }
    }
    if ($resp.messages.Count -gt 0) {
      $newest = ($resp.messages | Sort-Object { [double]$_.ts } -Descending | Select-Object -First 1).ts
      if ([double]$newest -gt [double]$seen.fleet) { $seen.fleet = $newest }
    }
  }

  # --- Write new messages to inbox + update seen ---
  $MatrixInbox = "$ClaudeDir\.matrix_inbox.jsonl"
  if ($newMessages.Count -gt 0) {
    foreach ($m in $newMessages) {
      ($m | ConvertTo-Json -Compress) | Add-Content -Path $InboxFile -Encoding utf8

      # Phase 1 Slack -> Matrix mirror (Plan Task 33): also write a normalized
      # copy to .matrix_inbox.jsonl so the unified /loop drain handles both
      # surfaces. source=slack-mirror differentiates the origin. This is
      # ONE-WAY: Slack DM -> Matrix inbox. Matrix outbox -> Slack is Phase 3.
      $mirrored = [PSCustomObject]@{
        source         = 'slack-mirror'
        slack_channel  = $m.channel
        slack_user     = $m.user
        text           = $m.text
        ts             = $m.ts
        original_source = $m.source
      }
      ($mirrored | ConvertTo-Json -Compress) | Add-Content -Path $MatrixInbox -Encoding utf8
    }
    Write-Log "captured $($newMessages.Count) new message(s); inbox now has $((Get-Content $InboxFile -ErrorAction SilentlyContinue).Count) line(s); mirrored to matrix inbox"
  }
  $seen | ConvertTo-Json | Set-Content -Path $SeenFile -Encoding utf8

} catch {
  Write-Log "FATAL: $($_.Exception.Message)"
  exit 1
}
