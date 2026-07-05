<#
  bake_fp_arms.ps1 - drive tools/bake_fp_arms.py through the MS-Store Blender
  launcher (which DETACHES with no stdout - see anim_build.ps1). Polls for the
  .done marker the Python script writes, then prints its .log.

  Usage: powershell -File tools\bake_fp_arms.ps1 [-In <rigged.glb>] [-Out <arms.glb>]
  ASCII-only on purpose (PS 5.1 reads .ps1 as ANSI).
#>
param(
  [string]$In  = "",
  [string]$Out = "",
  [int]$TimeoutSec = 240
)
$ErrorActionPreference = "Stop"
$repo     = Split-Path -Parent $PSScriptRoot
$rigged   = Join-Path $repo "assets\rigged_glb"
$launcher = "C:\Users\Tim\AppData\Local\Microsoft\WindowsApps\blender-launcher.exe"
$script   = Join-Path $PSScriptRoot "bake_fp_arms.py"

if (-not (Test-Path $launcher)) { throw "Blender launcher not found: $launcher" }
if ($In -eq "")  { $In  = Join-Path $rigged "Jake_22_actions.glb" }
if ($Out -eq "") { $Out = Join-Path $rigged "FPArms_Jake.glb" }
if (-not (Test-Path $In)) { throw "Input not found: $In" }

$done = "$Out.done"; $logf = "$Out.log"
foreach ($p in @($done, $logf)) { if (Test-Path $p) { Remove-Item $p -Force } }

Write-Host "[fp-arms] $In -> $Out"
& $launcher --background --python $script -- $In $Out | Out-Null

$deadline = (Get-Date).AddSeconds($TimeoutSec)
while ((Get-Date) -lt $deadline) {
  if (Test-Path $done) { break }
  Start-Sleep -Seconds 3
}
if (-not (Test-Path $done)) {
  Write-Host "[fp-arms] TIMEOUT - no .done marker" -ForegroundColor Red
  if (Test-Path $logf) { Get-Content $logf }
  exit 1
}
$status = (Get-Content $done -Raw).Trim()
if (Test-Path $logf) { Get-Content $logf }
if ($status -like "OK*") {
  Write-Host "[fp-arms] DONE $status" -ForegroundColor Green
} else {
  Write-Host "[fp-arms] $status" -ForegroundColor Red
  exit 1
}
