<#
  gate_build.ps1 - one-command driver for tools/build_rifthub_gate.py on this box.

  Usage:
    powershell -File tools\gate_build.ps1 [-Out assets\converted_glb\rifthub\gate_ring.glb] [-TimeoutSec 300]

  WHY this wrapper: Blender here is the Microsoft Store package. Direct
  blender.exe is ACL-denied; only the blender-launcher.exe alias runs and it
  DETACHES (no stdout, returns instantly). So we launch via the alias and POLL
  for the <out>.done marker the Python script writes, then print its <out>.log.

  NOTE: keep this file ASCII-only. Windows PowerShell 5.1 reads .ps1 as ANSI, so
  non-ASCII chars (em dashes, etc.) corrupt the parser.
#>
param(
  [string]$Out = "",
  [int]$TimeoutSec = 300
)
$ErrorActionPreference = "Stop"
$repo     = Split-Path -Parent $PSScriptRoot
$launcher = "C:\Users\Tim\AppData\Local\Microsoft\WindowsApps\blender-launcher.exe"
$script   = Join-Path $PSScriptRoot "build_rifthub_gate.py"

if (-not (Test-Path $launcher)) { throw "Blender launcher not found: $launcher" }
if ($Out -eq "") { $Out = Join-Path $repo "assets\converted_glb\rifthub\gate_ring.glb" }
$outDir = Split-Path -Parent $Out
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Force $outDir | Out-Null }

$done = "$Out.done"; $logf = "$Out.log"
foreach ($p in @($done, $logf)) { if (Test-Path $p) { Remove-Item $p -Force } }

Write-Host "[gate_build] -> $Out" -ForegroundColor Cyan
& $launcher --background --python $script -- $Out | Out-Null

$deadline = (Get-Date).AddSeconds($TimeoutSec)
while ((Get-Date) -lt $deadline) {
  if (Test-Path $done) { break }
  Start-Sleep -Seconds 3
}
if (-not (Test-Path $done)) {
  Write-Host "[gate_build] TIMEOUT - no .done marker (launcher may be sandboxed)" -ForegroundColor Red
  if (Test-Path $logf) { Get-Content $logf }
  exit 1
}
$status = (Get-Content $done -Raw).Trim()
if (Test-Path $logf) { Get-Content $logf }
if ($status -like "OK*") {
  $kb = [math]::Round(((Get-Item $Out).Length / 1024), 1)
  Write-Host ("[gate_build] DONE {0} ({1} KB)" -f $status, $kb) -ForegroundColor Green
} else {
  Write-Host ("[gate_build] {0}" -f $status) -ForegroundColor Red
  exit 1
}
