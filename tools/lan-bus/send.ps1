# =============================================================================
# send.ps1 — DJBOOTH-pattern fleet LAN bus sender
# =============================================================================
#
# DESCRIPTION
#   Posts a fleet-RPC message to a remote machine's LAN bus receiver, or to
#   ALL machines (broadcast). Looks up the target machine's LAN IP from
#   ~/.claude/.fleet_hosts.json. Authenticates with the shared secret at
#   ~/.claude/.fleet_secret.
#
# USAGE
#   pwsh send.ps1 <target-machine> "<message text>" [-Kind <fleet-rpc|note|broadcast>]
#   pwsh send.ps1 --all "<message text>"                # broadcast to all hosts
#
# EXAMPLES
#   pwsh send.ps1 13700k "merge feat/portal-hub when ready"
#   pwsh send.ps1 14900k "your visual-pass branch needs a rebase" -Kind note
#   pwsh send.ps1 --all "fleet status check — reply with current branch"
#
# EXIT CODES
#   0   Sent successfully (single target) or all targets succeeded (broadcast)
#   1   Auth/config error (missing secret or hosts file)
#   2   Target not in hosts file
#   3   HTTP request failed (network or remote receiver down)
#   4   Remote responded with non-2xx
# =============================================================================

[CmdletBinding()]
param(
  [Parameter(Mandatory=$true, Position=0)]
  [string]$Target,
  [Parameter(Mandatory=$true, Position=1)]
  [string]$Text,
  [string]$Kind = 'fleet-rpc',
  [int]$Port = 47474,
  [int]$TimeoutSec = 5
)

$ErrorActionPreference = 'Stop'
$ClaudeDir  = "$env:USERPROFILE\.claude"
$SecretFile = "$ClaudeDir\.fleet_secret"
$HostsFile  = "$ClaudeDir\.fleet_hosts.json"
$MachineName = ($env:COMPUTERNAME).ToLower()

if (-not (Test-Path $SecretFile)) {
  Write-Error "secret file missing at $SecretFile — run bootstrap.ps1"
  exit 1
}
if (-not (Test-Path $HostsFile)) {
  Write-Error "hosts file missing at $HostsFile — run bootstrap.ps1"
  exit 1
}

$secret = (Get-Content -Raw $SecretFile).Trim()
$hosts  = Get-Content -Raw $HostsFile | ConvertFrom-Json

function Send-One($targetName, $targetIp) {
  $body = @{
    from = $MachineName
    to   = $targetName
    text = $Text
    kind = $Kind
    ts   = [Math]::Round((Get-Date -UFormat %s) * 1000)
  } | ConvertTo-Json -Compress

  $url = "http://${targetIp}:${Port}/inbox"
  try {
    $resp = Invoke-WebRequest -Uri $url `
      -Method Post -Body $body `
      -ContentType 'application/json; charset=utf-8' `
      -Headers @{ Authorization = "Bearer $secret" } `
      -TimeoutSec $TimeoutSec -ErrorAction Stop
    $j = $resp.Content | ConvertFrom-Json
    if ($j.ok) {
      Write-Output "SENT to=$targetName ip=$targetIp text=`"$Text`" ts=$($j.ts)"
      return 0
    } else {
      Write-Warning "REJECTED by $targetName ($targetIp): $($j.error)"
      return 4
    }
  } catch {
    Write-Warning "FAILED to $targetName ($targetIp): $($_.Exception.Message)"
    return 3
  }
}

if ($Target -eq '--all' -or $Target -eq 'all') {
  $exitCode = 0
  foreach ($prop in $hosts.PSObject.Properties) {
    if ($prop.Name -eq $MachineName) { continue }  # don't send to self
    $rc = Send-One $prop.Name $prop.Value
    if ($rc -ne 0) { $exitCode = $rc }
  }
  exit $exitCode
}

$ip = $hosts.$Target
if (-not $ip) {
  Write-Error "target '$Target' not in hosts file. Known: $($hosts.PSObject.Properties.Name -join ', ')"
  exit 2
}
exit (Send-One $Target $ip)
