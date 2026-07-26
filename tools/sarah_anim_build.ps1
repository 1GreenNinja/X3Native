<#
  sarah_anim_build.ps1 - bake Sarah's clip library (LANE B, Sarah companion combat).

  *** WIP - the bake this produces is NOT shippable yet. It runs clean and the engine
  loads all 6 clips, but the deformation is mangled (arms fuse into the torso, pelvis
  explodes). See the "_status" block in tools\sarah_clips.manifest.json and the
  evidence renders in docs\screenshots\sarah\. Do not commit its Sarah_anim.glb until
  a GROUNDED render (tools\pose_render_grounded.py) shows a clean pose. ***

  Usage:
    powershell -File tools\sarah_anim_build.ps1 [-TimeoutSec 900]

  Retargets Idle/Walking/Running/Aim_Fire/Death/Hitreaction from
  assets\rigged_glb\JakeClone_player.glb onto assets\rigged_glb\Sarah.glb and writes
  assets\rigged_glb\Sarah_anim.glb. app\sarah.cpp PREFERS Sarah_anim.glb when it
  exists and falls back to the stock Sarah.glb, so this is an optional artifact -
  a clean checkout still runs (she just idles on her single stock clip).

  Both rigs are Meshy auto-rigs on the SAME 24-joint standard humanoid skeleton
  (bones "Hips"/"LeftUpLeg"/"RightHand"/"Head"), so the manifest uses rig type
  "meshy" (identity bone map + the rest-relative orientation transfer).

  WHY this wrapper (same gotcha as tools\anim_build.ps1): Blender here is the
  Microsoft Store package. Direct blender.exe is ACL-denied; only the
  blender-launcher.exe alias runs, and it DETACHES (no stdout, returns instantly).
  So we launch via the alias and POLL for the <out>.done marker the Python script
  writes, then print its <out>.log.

  NOTE: keep this file ASCII-only. Windows PowerShell 5.1 reads .ps1 as ANSI, so
  non-ASCII chars (em dashes, etc.) corrupt the parser.
#>
param(
  [int]$TimeoutSec = 900
)
$ErrorActionPreference = "Stop"
$repo     = Split-Path -Parent $PSScriptRoot
$rigged   = Join-Path $repo "assets\rigged_glb"
$launcher = "C:\Users\Tim\AppData\Local\Microsoft\WindowsApps\blender-launcher.exe"
$script   = Join-Path $PSScriptRoot "retarget_library.py"
$manifest = Join-Path $PSScriptRoot "sarah_clips.manifest.json"

if (-not (Test-Path $launcher)) { throw "Blender launcher not found: $launcher" }
if (-not (Test-Path $manifest)) { throw "Manifest not found: $manifest" }

$target = Join-Path $rigged "Sarah.glb"
$out    = Join-Path $rigged "Sarah_anim.glb"
if (-not (Test-Path $target)) { throw "Target rig not found: $target (git lfs pull?)" }

$done = "$out.done"; $logf = "$out.log"
foreach ($p in @($done, $logf)) { if (Test-Path $p) { Remove-Item $p -Force } }

Write-Host "[sarah_anim] target $target" -ForegroundColor Cyan
Write-Host "[sarah_anim] -> $out"
Push-Location $repo    # manifest "src" paths are repo-relative
try {
  & $launcher --background --python $script -- $target $out $manifest | Out-Null
} finally {
  Pop-Location
}

$deadline = (Get-Date).AddSeconds($TimeoutSec)
while ((Get-Date) -lt $deadline) {
  if (Test-Path $done) { break }
  Start-Sleep -Seconds 5
}
if (-not (Test-Path $done)) {
  Write-Host "[sarah_anim] TIMEOUT - no .done marker (launcher may be sandboxed)" -ForegroundColor Red
  if (Test-Path $logf) { Get-Content $logf }
  exit 1
}
$status = (Get-Content $done -Raw).Trim()
if (Test-Path $logf) { Get-Content $logf }
if ($status -like "OK*") {
  $kb = [math]::Round(((Get-Item $out).Length / 1024), 1)
  Write-Host ("[sarah_anim] DONE {0} ({1} KB)" -f $status, $kb) -ForegroundColor Green
} else {
  Write-Host ("[sarah_anim] {0}" -f $status) -ForegroundColor Red
  exit 1
}
