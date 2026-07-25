<#
  retarget_build.ps1 - driver for tools/retarget_library.py (multi-clip retarget).

  Usage:
    powershell -File tools\retarget_build.ps1 -Target <in.glb> -Out <out.glb> -Manifest <m.json> [-TimeoutSec 900]

  Blender here is the Microsoft Store package: blender-launcher.exe DETACHES, so we
  launch it and POLL for the <out>.done marker the Python writes, then print the log.
  ASCII-only (Windows PowerShell 5.1 reads .ps1 as ANSI).
#>
param(
  [Parameter(Mandatory=$true)][string]$Target,
  [Parameter(Mandatory=$true)][string]$Out,
  [Parameter(Mandatory=$true)][string]$Manifest,
  [int]$TimeoutSec = 900
)
$ErrorActionPreference = "Stop"
$launcher = "C:\Users\Tim\AppData\Local\Microsoft\WindowsApps\blender-launcher.exe"
$script   = Join-Path $PSScriptRoot "retarget_library.py"
if (-not (Test-Path $launcher)) { throw "Blender launcher not found: $launcher" }
if (-not (Test-Path $Target))   { throw "Target not found: $Target" }
if (-not (Test-Path $Manifest)) { throw "Manifest not found: $Manifest" }

$done = "$Out.done"; $logf = "$Out.log"
foreach ($p in @($done, $logf)) { if (Test-Path $p) { Remove-Item $p -Force } }

Write-Host "[retarget] $Target -> $Out  ($Manifest)" -ForegroundColor Cyan
& $launcher --background --python $script -- $Target $Out $Manifest | Out-Null

$deadline = (Get-Date).AddSeconds($TimeoutSec)
while ((Get-Date) -lt $deadline) {
  if (Test-Path $done) { break }
  Start-Sleep -Seconds 3
}
if (-not (Test-Path $done)) {
  Write-Host "[retarget] TIMEOUT - no .done marker" -ForegroundColor Red
  if (Test-Path $logf) { Get-Content $logf | Select-Object -Last 40 }
  exit 1
}
$status = (Get-Content $done -Raw).Trim()
if (Test-Path $logf) { Get-Content $logf | Select-Object -Last 40 }
if ($status -like "OK*") {
  $kb = [math]::Round(((Get-Item $Out).Length / 1024), 1)
  Write-Host ("[retarget] DONE {0} ({1} KB)" -f $status, $kb) -ForegroundColor Green
} else {
  Write-Host ("[retarget] {0}" -f $status) -ForegroundColor Red
  exit 1
}
