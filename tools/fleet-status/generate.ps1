# =============================================================================
# generate.ps1 — Produce the fleet-status JSON the sidebar widget polls.
# =============================================================================
#
# RUNS ON: 13700K Command Center (it's the integrator + it hosts Conduit).
#
# DESCRIPTION
#   Walks the fleet, pings each machine's LAN IP, queries origin's branch
#   refs, and emits a single JSON document conforming to schema.json. The
#   Element sidebar widget polls this every 60s and re-renders.
#
# OUTPUT
#   C:\opt\element-web\fleet-status.json
#
# SCHEDULE
#   Windows Scheduled Task, every 60s. Lightweight enough to run that often.
#
# DEPENDS ON
#   git (in PATH on 13700K)
#   ~/.claude/.fleet_hosts.json (the canonical IP map maintained by Tim)
#   Network reachability from 13700K to the listed LAN IPs (ICMP)
# =============================================================================

$ErrorActionPreference = 'Continue'  # don't abort on ping fails

$ClaudeDir   = "$env:USERPROFILE\.claude"
$HostsFile   = "$ClaudeDir\.fleet_hosts.json"
$RepoDir     = 'D:\GameDev\X3Native'  # adjust on 13700K if different
$OutputFile  = 'C:\opt\element-web\fleet-status.json'
$GenMachine  = ($env:COMPUTERNAME).ToLower()

# Roles map — hand-maintained; the spec source of truth is the fleet design doc.
$RoleMap = @{
  djbooth   = 'worker'
  '13700k'  = 'integrator'
  '14900k'  = 'showcase'
  i5000     = 'worker'
  i4400     = 'worker'
  snake     = 'worker'
  mob_boss  = 'fallback'
  laptop_og = 'primary'
}

function Get-MachineBranches($machineName) {
  Push-Location $RepoDir
  try {
    # Branches matching this machine: heuristics —
    #   feat/*-<machine>     (e.g. feat/portal-hub-djbooth)
    #   feat/<machine>-*     (e.g. feat/djbooth-config)
    #   Or any branch that touches the <machine>.md file (looking up via blame)
    # Simpler heuristic for v1: pull branches whose latest commit touches the
    # machine's .md file in repo root.
    $branches = @()
    $allBranches = & git for-each-ref --format='%(refname:short)|%(objectname:short)|%(committerdate:iso-strict)|%(subject)' refs/remotes/origin 2>$null
    foreach ($line in $allBranches) {
      if (-not $line) { continue }
      $parts = $line -split '\|', 4
      if ($parts.Length -lt 4) { continue }
      $name = $parts[0] -replace '^origin/', ''
      if ($name -eq 'HEAD' -or $name -eq 'main' -or $name -eq 'master') { continue }
      # Does any commit on this branch (not on main) touch the machine's .md?
      $touchedFiles = & git diff --name-only "origin/main...origin/$name" 2>$null
      $machineFiles = @("$machineName.md", "$($machineName.ToUpper()).md", "snake13700k.md")
      $touched = $false
      foreach ($f in $touchedFiles) {
        if ($machineFiles -contains $f.ToLower()) { $touched = $true; break }
      }
      if (-not $touched) { continue }

      $ahead = (& git rev-list --count "origin/main..origin/$name" 2>$null) -as [int]
      $behind = (& git rev-list --count "origin/$name..origin/main" 2>$null) -as [int]

      # Parse STATUS block to find READY FOR INTEGRATION text
      $statusText = & git show "origin/${name}:${machineName}.md" 2>$null | Out-String
      $ready = $statusText -match 'READY FOR INTEGRATION'

      $branches += [PSCustomObject]@{
        name                 = $name
        head_sha             = $parts[1]
        head_subject         = $parts[3]
        head_committed_at    = $parts[2]
        ahead_of_main        = $ahead
        behind_main          = $behind
        ready_for_integration = [bool]$ready
      }
    }
    return $branches
  } finally {
    Pop-Location
  }
}

function Get-MachinePresence($lanIp) {
  if (-not $lanIp -or $lanIp -match 'x') { return @{ presence='unknown'; source='stale' } }
  $ping = Test-Connection -ComputerName $lanIp -Count 1 -Quiet -TimeoutSeconds 1 -ErrorAction SilentlyContinue
  if ($ping) {
    # Phase 1: ping alone. Phase 2 will cross-reference with matrix-daemon's
    # presence emission via the Matrix server's m.presence event store.
    return @{ presence='online'; source='ping-only' }
  } else {
    return @{ presence='dormant'; source='ping-only' }
  }
}

# Load fleet hosts
if (-not (Test-Path $HostsFile)) {
  Write-Error "Fleet hosts file missing at $HostsFile — run lan-bus bootstrap first"
  exit 1
}
$hosts = Get-Content -Raw $HostsFile | ConvertFrom-Json

# Walk machines
$machines = @()
foreach ($prop in $hosts.PSObject.Properties) {
  $name = $prop.Name
  $lanIp = $prop.Value
  $presence = Get-MachinePresence $lanIp
  $branches = Get-MachineBranches $name

  $machines += [PSCustomObject]@{
    name               = $name
    display_name       = $name.ToUpper()
    lan_ip             = $lanIp
    presence           = $presence.presence
    presence_source    = $presence.source
    last_seen_at       = $null
    last_seen_ago_sec  = $null
    branches           = $branches
    role               = if ($RoleMap.ContainsKey($name)) { $RoleMap[$name] } else { 'worker' }
    notes              = $null
  }
}

# Integration queue: union of all machines' READY-FOR-INTEGRATION branches
$queue = @()
foreach ($m in $machines) {
  foreach ($b in $m.branches) {
    if ($b.ready_for_integration) {
      $queue += [PSCustomObject]@{
        branch       = $b.name
        from_machine = $m.name
        head_sha     = $b.head_sha
        queued_at    = $b.head_committed_at
        status_blurb = $b.head_subject
      }
    }
  }
}

# Warnings
$warnings = @()
foreach ($m in $machines) {
  foreach ($b in $m.branches) {
    if ($b.behind_main -gt 20) {
      $warnings += [PSCustomObject]@{
        severity        = 'warn'
        text            = "$($m.display_name)'s $($b.name) is $($b.behind_main) commits behind main — needs rebase"
        affects_machine = $m.name
      }
    }
  }
  if ($m.presence -eq 'dormant') {
    $warnings += [PSCustomObject]@{
      severity        = 'info'
      text            = "$($m.display_name) is dormant (no LAN ping response)"
      affects_machine = $m.name
    }
  }
}

# Compose
$out = [PSCustomObject]@{
  generated_at      = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
  generator_machine = $GenMachine
  machines          = $machines
  integration_queue = $queue
  warnings          = $warnings
}

# Atomic write
$tmp = "$OutputFile.tmp"
$out | ConvertTo-Json -Depth 8 | Set-Content -Path $tmp -Encoding utf8
Move-Item -Force $tmp $OutputFile
