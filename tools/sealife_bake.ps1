<#
  sealife_bake.ps1 - one-command driver for tools/sealife_bake.py on this box.

  Usage:
    powershell -File tools\sealife_bake.ps1 [-Species all] [-TimeoutSec 1200]

  WHY this wrapper: Blender here is the Microsoft Store package. Direct blender.exe
  is ACL-denied; only the blender-launcher.exe alias runs and it DETACHES (no stdout,
  returns instantly). So we launch via the alias and POLL for the <out>.done marker
  the Python script writes, then print its <out>.log.

  It also PRE-EXTRACTS the great white's PBR textures, which do not ship loose next
  to its OBJ -- its .mtl points at a bogus "C:/texture_diffuse.png". The real 2048^2
  set is packed inside base_basic_pbr.usdz (a zip). Without this step the shark bakes
  out as a smooth untextured plastic toy.

  NOTE: keep this file ASCII-only. Windows PowerShell 5.1 reads .ps1 as ANSI, so
  non-ASCII chars (em dashes, etc.) corrupt the parser.
#>
param(
  [string]$Species = "all",
  [int]$TimeoutSec = 1800
)
$ErrorActionPreference = "Stop"
$repo     = Split-Path -Parent $PSScriptRoot
$launcher = "C:\Users\Tim\AppData\Local\Microsoft\WindowsApps\blender-launcher.exe"
$script   = Join-Path $PSScriptRoot "sealife_bake.py"
if (-not (Test-Path $launcher)) { throw "Blender launcher not found: $launcher" }

# ---- 1. recover the great white's textures out of its .usdz (zip) ----------
$usdz = "G:\GameModels\rodin_glb\GreatWhiteSharkGameReady\base_basic_pbr.usdz"
$texd = "D:\GameDev\_sealife_src\GreatWhiteSharkGameReady"
if (-not (Test-Path (Join-Path $texd "texture_diffuse.png"))) {
  Write-Host "[sealife] extracting great white PBR set from usdz..." -ForegroundColor Cyan
  New-Item -ItemType Directory -Force $texd | Out-Null
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  $zip = [System.IO.Compression.ZipFile]::OpenRead($usdz)
  foreach ($e in $zip.Entries) {
    if ($e.FullName -like "textures/*.png") {
      $dst = Join-Path $texd ([System.IO.Path]::GetFileName($e.FullName))
      [System.IO.Compression.ZipFileExtensions]::ExtractToFile($e, $dst, $true)
      Write-Host ("  " + [System.IO.Path]::GetFileName($e.FullName))
    }
  }
  $zip.Dispose()
}

# ---- 2. bake ---------------------------------------------------------------
$outDir = Join-Path $repo "assets\rigged_glb"
New-Item -ItemType Directory -Force $outDir | Out-Null

if ($Species -eq "all") {
  $out  = $outDir
  $logf = Join-Path $outDir "sealife_bake.log"
  $done = Join-Path $outDir "sealife_bake.done"
} else {
  $out  = Join-Path $outDir "$Species.glb"
  $logf = "$out.log"
  $done = "$out.done"
}
foreach ($p in @($done, $logf)) { if (Test-Path $p) { Remove-Item $p -Force } }

Write-Host "[sealife] baking '$Species' -> $out" -ForegroundColor Cyan
& $launcher --background --python $script -- $Species $out $done | Out-Null

$deadline = (Get-Date).AddSeconds($TimeoutSec)
while ((Get-Date) -lt $deadline) {
  if (Test-Path $done) { break }
  Start-Sleep -Seconds 5
}
if (-not (Test-Path $done)) {
  Write-Host "[sealife] TIMEOUT - no .done marker" -ForegroundColor Red
  if (Test-Path $logf) { Get-Content $logf }
  exit 1
}
$status = (Get-Content $done -Raw).Trim()
if (Test-Path $logf) { Get-Content $logf }
if ($status -like "OK*") {
  Write-Host "[sealife] DONE $status" -ForegroundColor Green
} else {
  Write-Host "[sealife] $status" -ForegroundColor Red
  exit 1
}
