# tools/convert_hospital_pack.ps1
#
# Batch FBX -> GLB converter for the Hospital pack's manifest. Reads the unique
# prefab names from tools/manifests/f3_overview.x3lvl.json, looks up each name's
# sibling .fbx in the source pack, and invokes Blender headless (via
# tools/convert_fbx_glb.py) to write a GLB into
# assets/converted_glb/Modular_Abandoned_Hospital/<name>.glb.
#
# Pre-Blender-install: runs as DRY-RUN automatically, printing what it would do.
# Post-Blender-install: one command does the full pack.
#
# Usage:
#   .\tools\convert_hospital_pack.ps1                   # auto-detect Blender, convert
#   .\tools\convert_hospital_pack.ps1 -DryRun           # list-only
#   .\tools\convert_hospital_pack.ps1 -BlenderPath 'C:\Path\To\blender.exe'
#   .\tools\convert_hospital_pack.ps1 -Force            # re-convert even if GLB already exists
#
# Clean-room: relies on Blender's official import_scene.fbx / export_scene.gltf
# operators only (via convert_fbx_glb.py). No third-party FBX SDK or Unity
# importer code consulted.

param(
  [switch]$DryRun,
  [switch]$Force,
  [string]$BlenderPath
)

$ErrorActionPreference = 'Continue'

# Layout (worktree-relative).
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Worktree  = Split-Path -Parent $ScriptDir
$Manifest  = Join-Path $ScriptDir 'manifests\f3_overview.x3lvl.json'
$Pack      = 'G:\Assets\Modular Abandoned Hospital Horror Hospital Abandoned hospital Hospital'
$OutDir    = Join-Path $Worktree 'assets\converted_glb\Modular_Abandoned_Hospital'
$Conv      = Join-Path $ScriptDir 'convert_fbx_glb.py'

# --- 1. Find Blender ----------------------------------------------------------
if (-not $BlenderPath) {
  $candidates = @(
    'C:\Program Files\Blender Foundation\Blender 5.1\blender.exe',
    'C:\Program Files\Blender Foundation\Blender 5.0\blender.exe',
    'C:\Program Files\Blender Foundation\Blender 4.5\blender.exe',
    'C:\Program Files\Blender Foundation\Blender 4.2\blender.exe',
    'C:\Program Files\Blender Foundation\Blender 4.1\blender.exe',
    'C:\Program Files\Blender Foundation\Blender 4.0\blender.exe',
    'C:\Program Files\Blender Foundation\Blender 3.6\blender.exe'
  )
  $BlenderPath = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if ($BlenderPath -and (Test-Path $BlenderPath)) {
  Write-Host "Blender: $BlenderPath" -ForegroundColor Green
} else {
  Write-Host 'Blender NOT found -> dry-run mode. Install from G:\Resources\blender-5.1.0-windows-x64.msi and re-run.' -ForegroundColor Yellow
  $DryRun = $true
}

# --- 2. Pre-flight checks -----------------------------------------------------
if (-not (Test-Path $Manifest)) { throw "manifest missing: $Manifest" }
if (-not (Test-Path $Pack))     { throw "pack root missing: $Pack" }
if (-not (Test-Path $Conv))     { throw "convert_fbx_glb.py missing: $Conv" }

# --- 3. Read manifest, get unique prefab names --------------------------------
$ManifestJson = Get-Content $Manifest -Raw | ConvertFrom-Json
$Unique = $ManifestJson | ForEach-Object { $_.name } | Sort-Object -Unique
Write-Host "manifest entries: $($ManifestJson.Count)  unique prefabs: $($Unique.Count)"

# --- 4. Build name -> FBX path lookup for the pack ----------------------------
$FbxMap = @{}
Get-ChildItem $Pack -Recurse -Filter *.fbx -ErrorAction SilentlyContinue | ForEach-Object {
  $key = $_.BaseName.ToLower()
  if (-not $FbxMap.ContainsKey($key)) { $FbxMap[$key] = $_.FullName }
}
Write-Host "FBX files in pack: $($FbxMap.Count)"

# --- 5. Ensure output dir -----------------------------------------------------
if (-not $DryRun) {
  New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}

# --- 6. Convert each ----------------------------------------------------------
$converted = 0
$skipped   = 0
$failed    = @()
$missing   = @()
$idx       = 0
$total     = $Unique.Count

foreach ($name in $Unique) {
  $idx++
  $src = $FbxMap[$name.ToLower()]
  if (-not $src) {
    $missing += $name
    Write-Host "[$idx/$total] $name : NO FBX (skipped)" -ForegroundColor Yellow
    continue
  }
  $dst = Join-Path $OutDir "$name.glb"

  if ($DryRun) {
    Write-Host "[$idx/$total] DRY $name"
    Write-Host "          src: $src"
    Write-Host "          dst: $dst"
    continue
  }

  if ((Test-Path $dst) -and -not $Force) {
    $skipped++
    Write-Host "[$idx/$total] $name : already exists, skipping" -ForegroundColor DarkGray
    continue
  }

  Write-Host "[$idx/$total] $name ..." -NoNewline
  & $BlenderPath --background --python $Conv -- $src $dst 2>&1 | Out-Null
  if ($LASTEXITCODE -eq 0 -and (Test-Path $dst)) {
    $kb = [math]::Round((Get-Item $dst).Length / 1KB, 1)
    Write-Host " ok ($kb KB)" -ForegroundColor Green
    $converted++
  } else {
    Write-Host ' FAILED' -ForegroundColor Red
    $failed += $name
  }
}

# --- 7. Summary ---------------------------------------------------------------
Write-Host ''
Write-Host '--- done ---'
"converted        : $converted"
"skipped (exists) : $skipped"
"failed           : $($failed.Count)$(if ($failed.Count -gt 0) { " ($($failed -join ', '))" })"
"missing FBX      : $($missing.Count)$(if ($missing.Count -gt 0) { " ($($missing -join ', '))" })"
if (-not $DryRun) { "output dir       : $OutDir" }
