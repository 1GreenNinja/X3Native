<#
  anim_build.ps1 - one-command driver for tools/animate_creature.py on this box.

  Usage:
    powershell -File tools\anim_build.ps1 -Model alien_crawler.glb [-Gait core] [-TimeoutSec 300]

  -Model : a file in assets/rigged_glb (name or full path). Output is written
           next to it as <name>_anim.glb, which the runtime auto-prefers.
  -Gait  : biped | swim | core | auto (default: auto-detect from the rig).

  WHY this wrapper: Blender here is the Microsoft Store package. Direct
  blender.exe is ACL-denied; only the blender-launcher.exe alias runs and it
  DETACHES (no stdout, returns instantly). So we launch via the alias and POLL
  for the <out>.done marker the Python script writes, then print its <out>.log.

  NOTE: keep this file ASCII-only. Windows PowerShell 5.1 reads .ps1 as ANSI, so
  non-ASCII chars (em dashes, etc.) corrupt the parser.
#>
param(
  [Parameter(Mandatory=$true)][string]$Model,
  [string]$Gait = "auto",
  [int]$TimeoutSec = 300
)
$ErrorActionPreference = "Stop"
$repo     = Split-Path -Parent $PSScriptRoot
$rigged   = Join-Path $repo "assets\rigged_glb"
$launcher = "C:\Users\Tim\AppData\Local\Microsoft\WindowsApps\blender-launcher.exe"
$script   = Join-Path $PSScriptRoot "animate_creature.py"

if (-not (Test-Path $launcher)) { throw "Blender launcher not found: $launcher" }
$in = if (Test-Path $Model) { (Resolve-Path $Model).Path } else { Join-Path $rigged $Model }
if (-not (Test-Path $in)) { throw "Model not found: $in" }

$stem = [IO.Path]::GetFileNameWithoutExtension($in)
$out  = Join-Path (Split-Path -Parent $in) ($stem + "_anim.glb")
$done = "$out.done"; $logf = "$out.log"
foreach ($p in @($done, $logf)) { if (Test-Path $p) { Remove-Item $p -Force } }

Write-Host "[anim_build] $stem  gait=$Gait" -ForegroundColor Cyan
Write-Host "[anim_build] -> $out"
& $launcher --background --python $script -- $in $out $Gait | Out-Null

$deadline = (Get-Date).AddSeconds($TimeoutSec)
while ((Get-Date) -lt $deadline) {
  if (Test-Path $done) { break }
  Start-Sleep -Seconds 3
}
if (-not (Test-Path $done)) {
  Write-Host "[anim_build] TIMEOUT - no .done marker (launcher may be sandboxed)" -ForegroundColor Red
  if (Test-Path $logf) { Get-Content $logf }
  exit 1
}
$status = (Get-Content $done -Raw).Trim()
if (Test-Path $logf) { Get-Content $logf }
if ($status -like "OK*") {
  $kb = [math]::Round(((Get-Item $out).Length / 1024), 1)
  Write-Host ("[anim_build] DONE {0} ({1} KB)" -f $status, $kb) -ForegroundColor Green
} else {
  Write-Host ("[anim_build] {0}" -f $status) -ForegroundColor Red
  exit 1
}
