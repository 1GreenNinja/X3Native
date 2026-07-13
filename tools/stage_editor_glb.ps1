<#
.SYNOPSIS
  Stage a converted GLB pack into the editor's model-palette content root using
  HARDLINKS (no extra disk space; instant; same-volume only), then expose it to
  the X3Native editor via a machine-local directory JUNCTION.

.DESCRIPTION
  The converted GLBs live in deep Unity export trees, e.g.
      D:\Assets\_glb\tech\<pack>\Assets\<...>\Meshes\<sub>\<mesh>.glb
  The native editor's drag-and-drop palette (app/editor/editor.cpp) SCANS its
  mounted converted_glb dir recursively and surfaces every *.glb it finds. To get
  a pack into the palette WITHOUT bloating the git repo (each pack is multi-GB and
  the repo's assets/converted_glb is committed via LFS) this script:

    1. FLATTENS every *.glb in <SourceRoot> to  <EditorRoot>\<PackName>\<mesh>.glb
       using hardlinks (New-Item -ItemType HardLink == `mklink /H`). Same D:
       volume, so the link is instant and consumes no extra bytes.
    2. Creates a directory JUNCTION  <Worktree>\assets\converted_glb\<PackName>
       -> <EditorRoot>\<PackName>  (New-Item -ItemType Junction == `mklink /J`),
       so the editor's assetRoot() -> convertedGlbRoot() resolves the staged pack
       alongside the repo's existing committed GLBs. (We junction a SUBFOLDER, not
       converted_glb itself, because converted_glb already holds 57 tracked LFS
       GLBs; replacing it would delete them from the working tree.)
    3. Adds the junction path to .git/info/exclude (local, never committed) so the
       machine-local staged content never shows up in `git status` / `git add`.
    4. Writes a small manifest (<EditorRoot>\<PackName>\_editor_manifest.json).

  RESUMABLE: conversion of a pack may still be running. Re-run this anytime to
  pick up newly-converted modules — existing hardlinks are left as-is and only
  missing ones are added.

.EXAMPLE
  pwsh tools\stage_editor_glb.ps1 `
      -SourceRoot "D:\Assets\_glb\tech\Sci-Fi Space Stations Creator" `
      -PackName   "SpaceStationsCreator"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $SourceRoot,
    [Parameter(Mandatory = $true)] [string] $PackName,
    [string] $EditorRoot = "D:\Assets\_glb\_editor",
    [string] $Worktree   = (Split-Path -Parent $PSScriptRoot),
    [switch] $NoJunction
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $SourceRoot)) {
    throw "SourceRoot not found: $SourceRoot"
}
# Clean pack name: keep it filesystem- and label-friendly.
if ($PackName -notmatch '^[A-Za-z0-9_\- ]+$') {
    throw "PackName '$PackName' has characters that would confuse the flat staging layout."
}

$destPack = Join-Path $EditorRoot $PackName
New-Item -ItemType Directory -Force -Path $destPack | Out-Null

Write-Host "Staging pack '$PackName'" -ForegroundColor Cyan
Write-Host "  source : $SourceRoot"
Write-Host "  dest   : $destPack"

# ---- 1. Flatten *.glb via hardlinks (resumable) ---------------------------------
$glbs = Get-ChildItem -LiteralPath $SourceRoot -Recurse -File -Filter *.glb -ErrorAction SilentlyContinue
$staged = 0; $skipped = 0; $collisions = 0
$seen = @{}   # basename -> source full path, to resolve within-run collisions
$manifest = New-Object System.Collections.Generic.List[object]

foreach ($g in $glbs) {
    $base = $g.Name
    $targetName = $base

    # Within-run collision (two different source files share a basename): prefix
    # with the immediate parent folder so both survive as distinct flat names.
    if ($seen.ContainsKey($targetName) -and $seen[$targetName] -ne $g.FullName) {
        $parent = Split-Path -Leaf (Split-Path -Parent $g.FullName)
        $targetName = "${parent}_${base}"
        $collisions++
    }
    $seen[$base] = $g.FullName

    $target = Join-Path $destPack $targetName
    if (Test-Path -LiteralPath $target) {
        $skipped++            # already staged (resumable no-op)
    } else {
        # Hardlink == mklink /H. Same volume required (D: -> D:), so this is instant.
        New-Item -ItemType HardLink -Path $target -Target $g.FullName -Force | Out-Null
        $staged++
    }
    $manifest.Add([ordered]@{ module = $targetName; source = $g.FullName })
}

Write-Host ("  hardlinks: {0} new, {1} already present, {2} collision-renamed  ({3} modules total)" -f `
    $staged, $skipped, $collisions, $manifest.Count) -ForegroundColor Green

# ---- 4. Manifest ----------------------------------------------------------------
$manifestPath = Join-Path $destPack "_editor_manifest.json"
[ordered]@{
    pack       = $PackName
    sourceRoot = $SourceRoot
    stagedAt   = (Get-Date).ToString("s")
    count      = $manifest.Count
    modules    = $manifest
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

# ---- 2. Junction the staged pack into the editor's converted_glb ---------------
if (-not $NoJunction) {
    $cvtRoot  = Join-Path $Worktree "assets\converted_glb"
    if (-not (Test-Path -LiteralPath $cvtRoot)) {
        Write-Warning "converted_glb not found at $cvtRoot -- skipping junction (editor won't see the pack until it exists)."
    } else {
        $junction = Join-Path $cvtRoot $PackName
        if (Test-Path -LiteralPath $junction) {
            Write-Host "  junction already present: $junction" -ForegroundColor DarkGray
        } else {
            New-Item -ItemType Junction -Path $junction -Target $destPack | Out-Null
            Write-Host "  junction created: $junction -> $destPack" -ForegroundColor Green
        }

        # ---- 3. Keep the machine-local junction out of git --------------------
        # Git reads info/exclude from the COMMON git dir. In a linked worktree, .git
        # is a file "gitdir: <per-worktree>" whose sibling `commondir` points at the
        # shared .git. Resolve that so the exclude actually takes effect here.
        $gitDir = Join-Path $Worktree ".git"
        $excludePath = $null
        if (Test-Path -LiteralPath $gitDir -PathType Container) {
            $excludePath = Join-Path $gitDir "info\exclude"
        } elseif (Test-Path -LiteralPath $gitDir -PathType Leaf) {
            $line = (Get-Content -LiteralPath $gitDir | Select-Object -First 1)
            if ($line -match '^gitdir:\s*(.+)$') {
                $wtGitDir = $Matches[1].Trim()
                $common   = $wtGitDir
                $cdFile   = Join-Path $wtGitDir "commondir"
                if (Test-Path -LiteralPath $cdFile) {
                    $rel = (Get-Content -LiteralPath $cdFile | Select-Object -First 1).Trim()
                    $common = [System.IO.Path]::GetFullPath((Join-Path $wtGitDir $rel))
                }
                $excludePath = Join-Path $common "info\exclude"
            }
        }
        if ($excludePath) {
            $rel = "/assets/converted_glb/$PackName/"
            $infoDir = Split-Path -Parent $excludePath
            New-Item -ItemType Directory -Force -Path $infoDir | Out-Null
            $existing = (Test-Path -LiteralPath $excludePath) ? (Get-Content -LiteralPath $excludePath) : @()
            if ($existing -notcontains $rel) {
                Add-Content -LiteralPath $excludePath -Value $rel
                Write-Host "  git exclude += $rel" -ForegroundColor DarkGray
            }
        }
    }
}

Write-Host "Done. Re-run to pick up newly-converted modules (resumable)." -ForegroundColor Cyan
