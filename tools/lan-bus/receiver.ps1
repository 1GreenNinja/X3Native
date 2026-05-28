# =============================================================================
# receiver.ps1 — DJBOOTH-pattern fleet LAN bus receiver
# =============================================================================
#
# DESCRIPTION
#   HTTP listener on port 47474 (configurable). Each fleet PC runs a copy.
#   POST /inbox with JSON body + a shared-secret header. Receiver validates
#   the secret, appends the message to ~/.claude/.lan_inbox.jsonl, returns
#   200 + {ok:true,ts:...}. Any other path returns 404.
#
# USAGE
#   Run via Scheduled Task at logon. To test interactively:
#     pwsh -NoProfile -ExecutionPolicy Bypass -File receiver.ps1
#
# ENVIRONMENT
#   LAN_BUS_PORT    Override port (default 47474)
#   LAN_BUS_BIND    Override bind address (default http://+:47474/, all ifaces)
#
# FILES READ
#   ~/.claude/.fleet_secret   Shared secret across the fleet (32 hex bytes)
#
# FILES WRITTEN
#   ~/.claude/.lan_inbox.jsonl    Appended one JSON line per received message
#   ~/.claude/.lan-receiver.log   Rolling log (last 200 lines kept)
#
# WIRE FORMAT (POST /inbox)
#   Headers: Authorization: Bearer <hex-secret>
#            Content-Type: application/json
#   Body:    { "from": "<sender-machine>",
#              "to":   "<this-machine>",
#              "text": "<message body>",
#              "kind": "fleet-rpc" | "broadcast" | "note",
#              "ts":   <unix-ms> }
#
# EXIT CODES
#   0   Clean shutdown (Ctrl+C or stop-receiver.ps1)
#   1   Fatal init error (missing secret, port in use, etc.)
# =============================================================================

$ErrorActionPreference = 'Stop'

$ClaudeDir   = "$env:USERPROFILE\.claude"
$SecretFile  = "$ClaudeDir\.fleet_secret"
$InboxFile   = "$ClaudeDir\.lan_inbox.jsonl"
$LogFile     = "$ClaudeDir\.lan-receiver.log"
$Port        = if ($env:LAN_BUS_PORT) { [int]$env:LAN_BUS_PORT } else { 47474 }
$Bind        = if ($env:LAN_BUS_BIND) { $env:LAN_BUS_BIND } else { "http://+:$Port/" }
$MachineName = ($env:COMPUTERNAME).ToLower()

function Write-Log($msg) {
  $line = "$(Get-Date -Format 'yyyy-MM-ddTHH:mm:sszzz') $msg"
  Add-Content -Path $LogFile -Value $line -Encoding utf8
  if ((Get-Item $LogFile -ErrorAction SilentlyContinue).Length -gt 50000) {
    $kept = Get-Content $LogFile -Tail 200
    Set-Content -Path $LogFile -Value $kept -Encoding utf8
  }
}

# Load shared secret
if (-not (Test-Path $SecretFile)) {
  Write-Log "FATAL: secret file missing at $SecretFile. Generate one with bootstrap.ps1."
  exit 1
}
$Secret = (Get-Content -Raw $SecretFile).Trim()
if ($Secret.Length -lt 32) {
  Write-Log "FATAL: secret too short ($($Secret.Length) chars). Re-run bootstrap.ps1."
  exit 1
}

# Start HTTP listener
$listener = New-Object System.Net.HttpListener
try {
  $listener.Prefixes.Add($Bind)
  $listener.Start()
  Write-Log "listening on $Bind  (machine=$MachineName)"
} catch {
  Write-Log "FATAL: listener.Start() failed: $($_.Exception.Message)"
  Write-Log "       Likely: port already in use OR no urlacl reservation OR firewall."
  Write-Log "       Hint: netsh http add urlacl url=$Bind user=$env:USERDOMAIN\$env:USERNAME"
  exit 1
}

# Graceful shutdown on Ctrl+C
$null = [Console]::TreatControlCAsInput = $false
[Console]::CancelKeyPress.Add({ Write-Log "received SIGINT, stopping"; $listener.Stop() })

while ($listener.IsListening) {
  try {
    $ctx = $listener.GetContext()
    $req = $ctx.Request
    $res = $ctx.Response

    # CORS preflight tolerance (clients may probe)
    if ($req.HttpMethod -eq 'OPTIONS') {
      $res.Headers.Add('Access-Control-Allow-Origin', '*')
      $res.Headers.Add('Access-Control-Allow-Methods', 'POST, OPTIONS')
      $res.Headers.Add('Access-Control-Allow-Headers', 'Content-Type, Authorization')
      $res.StatusCode = 204
      $res.Close()
      continue
    }

    if ($req.Url.AbsolutePath -ne '/inbox' -or $req.HttpMethod -ne 'POST') {
      $res.StatusCode = 404
      $res.Close()
      continue
    }

    # Auth: Bearer <secret>
    $auth = $req.Headers['Authorization']
    if (-not $auth -or $auth -ne "Bearer $Secret") {
      Write-Log "AUTH-FAIL from $($req.RemoteEndPoint) -- header=[$auth]"
      $res.StatusCode = 401
      $body = [System.Text.Encoding]::UTF8.GetBytes('{"ok":false,"error":"unauthorized"}')
      $res.OutputStream.Write($body, 0, $body.Length)
      $res.Close()
      continue
    }

    # Parse body
    $reader = New-Object System.IO.StreamReader($req.InputStream, $req.ContentEncoding)
    $bodyText = $reader.ReadToEnd()
    $reader.Close()

    try {
      $msg = $bodyText | ConvertFrom-Json
    } catch {
      Write-Log "BAD-JSON from $($req.RemoteEndPoint): $($_.Exception.Message)"
      $res.StatusCode = 400
      $errBody = [System.Text.Encoding]::UTF8.GetBytes('{"ok":false,"error":"bad-json"}')
      $res.OutputStream.Write($errBody, 0, $errBody.Length)
      $res.Close()
      continue
    }

    # Validate required fields
    if (-not $msg.from -or -not $msg.text) {
      $res.StatusCode = 400
      $errBody = [System.Text.Encoding]::UTF8.GetBytes('{"ok":false,"error":"missing from or text"}')
      $res.OutputStream.Write($errBody, 0, $errBody.Length)
      $res.Close()
      continue
    }

    # Enrich and append to inbox
    $stored = [PSCustomObject]@{
      source = 'lan-bus'
      from   = $msg.from
      to     = if ($msg.to) { $msg.to } else { $MachineName }
      text   = $msg.text
      kind   = if ($msg.kind) { $msg.kind } else { 'fleet-rpc' }
      ts     = if ($msg.ts) { $msg.ts } else { [Math]::Round((Get-Date -UFormat %s) * 1000) }
      from_ip = $req.RemoteEndPoint.Address.ToString()
      recv_ts = [Math]::Round((Get-Date -UFormat %s) * 1000)
    }
    ($stored | ConvertTo-Json -Compress) | Add-Content -Path $InboxFile -Encoding utf8
    Write-Log "RECV from=$($msg.from) to=$($stored.to) kind=$($stored.kind) bytes=$($bodyText.Length)"

    # Respond
    $okBody = [System.Text.Encoding]::UTF8.GetBytes("{`"ok`":true,`"ts`":$($stored.recv_ts)}")
    $res.ContentType = 'application/json; charset=utf-8'
    $res.StatusCode = 200
    $res.OutputStream.Write($okBody, 0, $okBody.Length)
    $res.Close()

  } catch [System.Net.HttpListenerException] {
    # Listener stopped externally
    break
  } catch {
    Write-Log "ERR: $($_.Exception.Message)"
  }
}

$listener.Close()
Write-Log "shutdown complete"
exit 0
