# =============================================================================
# bootstrap.ps1 — One-time setup for the LAN bus on a fleet machine
# =============================================================================
#
# DESCRIPTION
#   Run once on each fleet PC to set up the LAN bus:
#     1. Generates ~/.claude/.fleet_secret (32-byte hex) if missing
#     2. Creates ~/.claude/.fleet_hosts.json (placeholder IPs) if missing
#     3. Registers the receiver as a Windows Scheduled Task (at logon)
#     4. Reserves a urlacl so HttpListener can bind on port 47474 without admin
#
# USAGE
#   pwsh -NoProfile -ExecutionPolicy Bypass -File bootstrap.ps1
#
# NOTES
#   - The shared secret MUST be the same string on every fleet PC. Copy this
#     PC's secret to the others (rsync, USB stick, password manager, secure DM).
#   - The hosts file maps machine-name (lowercased) -> LAN IPv4. Edit it on
#     each PC to reflect actual LAN IPs:
#       { "djbooth":"192.168.7.110", "13700k":"192.168.7.x", ... }
#   - Firewall rule for port 47474 requires admin elevation. Run
#     New-NetFirewallRule separately in an admin shell.
# =============================================================================

$ErrorActionPreference = 'Stop'

$ClaudeDir  = "$env:USERPROFILE\.claude"
$SecretFile = "$ClaudeDir\.fleet_secret"
$HostsFile  = "$ClaudeDir\.fleet_hosts.json"
$LanBusDir  = "$ClaudeDir\lan-bus"
$ReceiverPs = "$LanBusDir\receiver.ps1"
$Port       = 47474

New-Item -ItemType Directory -Force -Path $ClaudeDir | Out-Null
New-Item -ItemType Directory -Force -Path $LanBusDir | Out-Null

# 1. Generate secret if missing
if (-not (Test-Path $SecretFile)) {
  $bytes = New-Object byte[] 32
  ([System.Security.Cryptography.RandomNumberGenerator]::Create()).GetBytes($bytes)
  $hex = [System.BitConverter]::ToString($bytes).Replace('-','').ToLower()
  Set-Content -Path $SecretFile -Value $hex -Encoding utf8
  Write-Output "GENERATED secret -> $SecretFile"
  Write-Output ""
  Write-Output "  Secret: $hex"
  Write-Output ""
  Write-Output "  COPY THIS to every other fleet PC's $SecretFile so they auth equally."
} else {
  Write-Output "OK secret already exists at $SecretFile"
}

# 2. Create placeholder hosts file if missing
if (-not (Test-Path $HostsFile)) {
  $placeholder = @{
    djbooth   = '192.168.7.110'
    '13700k'  = '192.168.7.x'
    '14900k'  = '192.168.7.x'
    i5000     = '192.168.7.x'
    i4400     = '192.168.7.x'
    snake     = '192.168.7.x'
    mob_boss  = '192.168.7.x'
    laptop_og = '192.168.7.x'
  } | ConvertTo-Json -Compress
  Set-Content -Path $HostsFile -Value $placeholder -Encoding utf8
  Write-Output "WROTE placeholder hosts -> $HostsFile"
  Write-Output "  EDIT this file with each fleet PC's real LAN IPv4."
} else {
  Write-Output "OK hosts file already exists at $HostsFile"
}

# 3. Copy receiver.ps1 to the deployed location if running from the repo
$repoReceiver = "$PSScriptRoot\receiver.ps1"
if ((Test-Path $repoReceiver) -and ($repoReceiver -ne $ReceiverPs)) {
  Copy-Item -Force $repoReceiver $ReceiverPs
  Write-Output "COPIED receiver.ps1 -> $ReceiverPs"
}

# 4. Register Scheduled Task to run receiver at logon
$taskName = 'DJBOOTH-LanBusReceiver'  # rename per-machine if desired
$existing = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
if ($existing) {
  Write-Output "REPLACING existing scheduled task $taskName"
  Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
}

$action = New-ScheduledTaskAction -Execute 'powershell.exe' `
  -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$ReceiverPs`"" `
  -WorkingDirectory $LanBusDir
$trigger = New-ScheduledTaskTrigger -AtLogOn
$settings = New-ScheduledTaskSettingsSet -StartWhenAvailable `
  -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
  -ExecutionTimeLimit (New-TimeSpan -Days 365) `
  -RestartCount 5 -RestartInterval (New-TimeSpan -Minutes 1) `
  -MultipleInstances IgnoreNew
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive
Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger `
  -Settings $settings -Principal $principal `
  -Description 'Fleet LAN bus HTTP receiver on port 47474. Source: tools/lan-bus/'

Start-ScheduledTask -TaskName $taskName
Start-Sleep -Seconds 2
Write-Output ""
Write-Output "REGISTERED + STARTED scheduled task $taskName"
Get-ScheduledTask -TaskName $taskName | Get-ScheduledTaskInfo | Select-Object LastRunTime, NextRunTime, LastTaskResult

Write-Output ""
Write-Output "=== NEXT STEPS (manual) ==="
Write-Output "1. Add Windows Firewall rule (REQUIRES ADMIN):"
Write-Output "   New-NetFirewallRule -DisplayName 'Fleet LAN bus (47474)' -Direction Inbound -LocalPort $Port -Protocol TCP -Action Allow -Profile Private"
Write-Output ""
Write-Output "2. urlacl reservation if HttpListener fails (REQUIRES ADMIN):"
Write-Output "   netsh http add urlacl url=http://+:$Port/ user=$env:USERDOMAIN\$env:USERNAME"
Write-Output ""
Write-Output "3. Edit $HostsFile with real LAN IPs for each fleet PC."
Write-Output ""
Write-Output "4. Copy $SecretFile to every other fleet PC (the secret must match)."
Write-Output ""
Write-Output "5. From another fleet PC, test:"
Write-Output "   pwsh -File tools/lan-bus/send.ps1 djbooth `"hello from <machine>`""
Write-Output "   then on DJBOOTH:"
Write-Output "   Get-Content $ClaudeDir\.lan_inbox.jsonl -Tail 1"
