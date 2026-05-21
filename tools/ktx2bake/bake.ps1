<#
.SYNOPSIS
    X3Native KTX2/Basis texture bake tool.

.DESCRIPTION
    Recursively finds PNG/JPG/JPEG/TGA source textures in an input directory,
    encodes each to KTX2 (UASTC for data/normal maps, ETC1S for color maps),
    generates mipmaps, and writes a manifest.json with per-texture metadata.

    Implements skip-if-unchanged: if the output .ktx2 already exists and the
    source SHA-256 matches the manifest record, the file is skipped.

.PARAMETER InputDir
    Root directory containing source textures. Searched recursively.

.PARAMETER OutputDir
    Destination directory for .ktx2 files (mirrored sub-path structure).

.PARAMETER Mode
    "auto"  (default) -- use filename heuristics to pick UASTC vs ETC1S.
    "uastc" -- force UASTC for all files.
    "etc1s" -- force ETC1S for all files.

.PARAMETER MaxFiles
    Maximum number of files to encode (default: unlimited). Useful for testing.

.PARAMETER Force
    Re-encode even if output exists and hash matches.

.EXAMPLE
    .\bake.ps1 -InputDir C:\textures\_originals -OutputDir C:\textures\ktx2
    .\bake.ps1 -InputDir . -OutputDir .\out -MaxFiles 10 -Mode auto
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InputDir,

    [Parameter(Mandatory = $true)]
    [string]$OutputDir,

    [ValidateSet("auto", "uastc", "etc1s")]
    [string]$Mode = "auto",

    [int]$MaxFiles = 0,

    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# ---------------------------------------------------------------------------
# 1. Locate encoder
# ---------------------------------------------------------------------------
function Find-Encoder {
    # (a) Already on PATH?
    $toktxOnPath = Get-Command toktx -ErrorAction SilentlyContinue
    if ($toktxOnPath) { return $toktxOnPath.Source }

    # (b) Sibling bin/ directory (downloaded previously by this script)
    $scriptDir = Split-Path -Parent $MyInvocation.ScriptName
    $localBin = Join-Path $scriptDir "bin\ktx_extract\bin\toktx.exe"
    if (Test-Path $localBin) { return $localBin }

    # (c) Vulkan SDK toktx
    $vulkanBin = $env:VULKAN_SDK
    if ($vulkanBin) {
        $vToktx = Join-Path $vulkanBin "Bin\toktx.exe"
        if (Test-Path $vToktx) { return $vToktx }
    }

    # (d) Download KTX-Software from GitHub releases
    Write-Host "[encoder] toktx not found -- downloading KTX-Software v4.4.2..." -ForegroundColor Yellow
    $scriptDir2 = Split-Path -Parent $MyInvocation.ScriptName
    $binDir = Join-Path $scriptDir2 "bin"
    New-Item -ItemType Directory -Path $binDir -Force | Out-Null

    $installer = Join-Path $binDir "KTX-Software-4.4.2-Windows-x64.exe"
    if (-not (Test-Path $installer)) {
        $url = "https://github.com/KhronosGroup/KTX-Software/releases/download/v4.4.2/KTX-Software-4.4.2-Windows-x64.exe"
        Write-Host "[encoder] Downloading $url" -ForegroundColor Cyan
        Invoke-WebRequest -Uri $url -OutFile $installer -UseBasicParsing
    }

    $extractDir = Join-Path $binDir "ktx_extract"
    New-Item -ItemType Directory -Path $extractDir -Force | Out-Null
    Write-Host "[encoder] Installing to $extractDir (silent NSIS)..." -ForegroundColor Cyan
    Start-Process -FilePath $installer -ArgumentList "/S /D=$extractDir" -Wait -NoNewWindow

    $toktxExtracted = Join-Path $extractDir "bin\toktx.exe"
    if (Test-Path $toktxExtracted) {
        Write-Host "[encoder] toktx ready: $toktxExtracted" -ForegroundColor Green
        return $toktxExtracted
    }

    throw "Failed to locate or download toktx. Install KTX-Software from https://github.com/KhronosGroup/KTX-Software/releases and put toktx.exe on PATH."
}

# ---------------------------------------------------------------------------
# 2. Format heuristic
# ---------------------------------------------------------------------------
function Get-EncodeFormat {
    param([string]$FileName, [string]$ForceMode)

    if ($ForceMode -ne "auto") { return $ForceMode }

    $name = [System.IO.Path]::GetFileNameWithoutExtension($FileName).ToLower()

    # Data/normal maps -> UASTC (lossless-ish, no color banding artefacts)
    $dataPatterns = @(
        "normal", "_norm", "nrm",
        "roughness", "rough",
        "metal", "metallic",
        "_mr", "metalrough",
        "_ao_", "^ao_", "_ao$",
        "occlusion", "ambient_occ",
        "height", "disp", "displacement",
        "emissive", "emission"
    )
    foreach ($pat in $dataPatterns) {
        if ($name -match $pat) { return "uastc" }
    }

    # Color / albedo maps -> ETC1S (smaller, perceptually good for sRGB)
    return "etc1s"
}

# ---------------------------------------------------------------------------
# 3. SHA-256 helper
# ---------------------------------------------------------------------------
function Get-FileSHA256 {
    param([string]$Path)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $bytes = $sha.ComputeHash($stream)
        return [BitConverter]::ToString($bytes) -replace "-", ""
    } finally {
        $stream.Dispose()
        $sha.Dispose()
    }
}

# ---------------------------------------------------------------------------
# 4. Image dimension helper (uses .NET System.Drawing - no extra deps)
# ---------------------------------------------------------------------------
function Get-ImageDimensions {
    param([string]$Path)
    try {
        Add-Type -AssemblyName System.Drawing -ErrorAction SilentlyContinue
        $img = [System.Drawing.Image]::FromFile((Resolve-Path $Path).Path)
        $w = $img.Width
        $h = $img.Height
        $img.Dispose()
        return @{ Width = $w; Height = $h }
    } catch {
        return @{ Width = 0; Height = 0 }
    }
}

# ---------------------------------------------------------------------------
# 5. Encode one texture with toktx
# ---------------------------------------------------------------------------
function Invoke-Toktx {
    param(
        [string]$ToktxExe,
        [string]$InputFile,
        [string]$OutputFile,
        [string]$Format
    )

    $outDir = Split-Path -Parent $OutputFile
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null

    # Build toktx argument list:
    #   --t2          -> KTX2 output format
    #   --encode      -> compression (uastc or etc1s)
    #   --genmipmap   -> generate full mip chain
    #   --assign_oetf -> color space hint
    $argList = @("--t2", "--encode", $Format, "--genmipmap")

    if ($Format -eq "uastc") {
        # Data/normal maps are linear (NOT sRGB)
        $argList += @("--assign_oetf", "linear")
        # UASTC quality level 2 (0=fastest, 4=slowest/best)
        $argList += @("--uastc_quality", "2")
        # Supercompress with Zstandard (level 18) for smaller on-disk files
        $argList += @("--zcmp", "18")
    } else {
        # Color/albedo maps are sRGB
        $argList += @("--assign_oetf", "srgb")
        # ETC1S quality 192/255 -- good balance; clevel 2 for better quality
        $argList += @("--qlevel", "192", "--clevel", "2")
    }

    $argList += @($OutputFile, $InputFile)

    $cmdResult = & $ToktxExe @argList 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "toktx failed for $InputFile (exit $LASTEXITCODE): $cmdResult"
        return $false
    }
    return $true
}

# ---------------------------------------------------------------------------
# 6. Load existing manifest (for cache / skip logic)
# ---------------------------------------------------------------------------
function Import-Manifest {
    param([string]$ManifestPath)
    if (-not (Test-Path $ManifestPath)) { return @{} }
    try {
        $json = Get-Content $ManifestPath -Raw | ConvertFrom-Json
        $map = @{}
        foreach ($entry in $json.textures) {
            $map[$entry.sourceRelPath] = $entry
        }
        return $map
    } catch {
        return @{}
    }
}

# ---------------------------------------------------------------------------
# MAIN
# ---------------------------------------------------------------------------

$InputDir  = (Resolve-Path $InputDir).Path
$OutputDir = [System.IO.Path]::GetFullPath($OutputDir)
$manifestPath = Join-Path $OutputDir "manifest.json"

Write-Host ""
Write-Host "=== X3Native KTX2 Bake ===" -ForegroundColor Cyan
Write-Host "Input:   $InputDir"
Write-Host "Output:  $OutputDir"
Write-Host "Mode:    $Mode"
Write-Host ""

# Find encoder
$toktxExe = Find-Encoder
Write-Host "[encoder] Using: $toktxExe" -ForegroundColor Green
Write-Host ""

# Create output dir
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null

# Collect source files
$extensions = @("*.png", "*.jpg", "*.jpeg", "*.tga")
$sourceFiles = @()
foreach ($ext in $extensions) {
    $found = Get-ChildItem -Path $InputDir -Filter $ext -Recurse -File -ErrorAction SilentlyContinue
    if ($found) { $sourceFiles += $found }
}
$sourceFiles = $sourceFiles | Sort-Object FullName

if ($MaxFiles -gt 0 -and $sourceFiles.Count -gt $MaxFiles) {
    Write-Host "[scan] Found $($sourceFiles.Count) textures; capping at $MaxFiles" -ForegroundColor Yellow
    $sourceFiles = $sourceFiles | Select-Object -First $MaxFiles
} else {
    Write-Host "[scan] Found $($sourceFiles.Count) textures" -ForegroundColor Cyan
}

# Load existing manifest for cache lookup
$existingManifest = Import-Manifest -ManifestPath $manifestPath

# Process each file
$results      = @()
$totalInBytes  = 0
$totalOutBytes = 0
$skippedCount  = 0
$encodedCount  = 0
$failedCount   = 0

$idx = 0
foreach ($src in $sourceFiles) {
    $idx++
    $relPath = $src.FullName.Substring($InputDir.Length).TrimStart('\', '/')
    $relKtx2 = [System.IO.Path]::ChangeExtension($relPath, ".ktx2")
    $outFile  = Join-Path $OutputDir $relKtx2

    $format  = Get-EncodeFormat -FileName $src.Name -ForceMode $Mode
    $srcSize = $src.Length

    Write-Host "[$idx/$($sourceFiles.Count)] $relPath -> $format" -NoNewline

    # Compute SHA-256 of source for cache check
    $srcHash = Get-FileSHA256 -Path $src.FullName

    # Skip-if-unchanged
    if ((-not $Force) -and (Test-Path $outFile)) {
        $cached = $existingManifest[$relPath]
        if ($cached -and ($cached.sourceSHA256 -eq $srcHash)) {
            $outSize = (Get-Item $outFile).Length
            Write-Host "  [skip - unchanged]" -ForegroundColor DarkGray
            $skippedCount++
            $totalInBytes  += $srcSize
            $totalOutBytes += $outSize
            $dims = Get-ImageDimensions -Path $src.FullName
            $results += [PSCustomObject]@{
                sourceRelPath    = $relPath
                sourceSHA256     = $srcHash
                outputRelPath    = $relKtx2
                format           = $format
                width            = $dims.Width
                height           = $dims.Height
                sourceSizeBytes  = $srcSize
                outputSizeBytes  = $outSize
                compressionRatio = if ($srcSize -gt 0) { [math]::Round($srcSize / $outSize, 2) } else { 0 }
                status           = "skipped"
            }
            continue
        }
    }

    # Encode
    $dims = Get-ImageDimensions -Path $src.FullName
    $ok   = Invoke-Toktx -ToktxExe $toktxExe -InputFile $src.FullName -OutputFile $outFile -Format $format

    if ($ok -and (Test-Path $outFile)) {
        $outSize = (Get-Item $outFile).Length
        $ratio   = if ($srcSize -gt 0) { [math]::Round($srcSize / $outSize, 2) } else { 0 }
        $inKB    = [math]::Round($srcSize / 1KB, 0)
        $outKB   = [math]::Round($outSize / 1KB, 0)
        Write-Host "  [${inKB} KB -> ${outKB} KB, ratio ${ratio}x]" -ForegroundColor Green
        $totalInBytes  += $srcSize
        $totalOutBytes += $outSize
        $encodedCount++
        $results += [PSCustomObject]@{
            sourceRelPath    = $relPath
            sourceSHA256     = $srcHash
            outputRelPath    = $relKtx2
            format           = $format
            width            = $dims.Width
            height           = $dims.Height
            sourceSizeBytes  = $srcSize
            outputSizeBytes  = $outSize
            compressionRatio = $ratio
            status           = "encoded"
        }
    } else {
        Write-Host "  [FAILED]" -ForegroundColor Red
        $failedCount++
        $totalInBytes += $srcSize
        $results += [PSCustomObject]@{
            sourceRelPath    = $relPath
            sourceSHA256     = $srcHash
            outputRelPath    = $relKtx2
            format           = $format
            width            = $dims.Width
            height           = $dims.Height
            sourceSizeBytes  = $srcSize
            outputSizeBytes  = 0
            compressionRatio = 0
            status           = "failed"
        }
    }
}

# Write manifest.json
$manifest = [ordered]@{
    generated   = (Get-Date -Format "yyyy-MM-ddTHH:mm:ssZ")
    encoder     = "toktx v4.4.2 (KhronosGroup/KTX-Software)"
    encoderPath = $toktxExe
    inputDir    = $InputDir
    outputDir   = $OutputDir
    mode        = $Mode
    textures    = $results
}

$manifestJson = $manifest | ConvertTo-Json -Depth 5
Set-Content -Path $manifestPath -Value $manifestJson -Encoding UTF8

# Print summary
$avgRatio = if ($totalOutBytes -gt 0) { [math]::Round($totalInBytes / $totalOutBytes, 2) } else { 0 }

Write-Host ""
Write-Host "=== Bake Summary ===" -ForegroundColor Cyan
Write-Host "  Total textures : $($sourceFiles.Count)"
Write-Host "  Encoded        : $encodedCount"
Write-Host "  Skipped(cached): $skippedCount"
Write-Host "  Failed         : $failedCount"
Write-Host "  Input  size    : $([math]::Round($totalInBytes  / 1MB, 2)) MB"
Write-Host "  Output size    : $([math]::Round($totalOutBytes / 1MB, 2)) MB"
Write-Host "  Avg ratio      : ${avgRatio}x"
Write-Host "  Manifest       : $manifestPath"
Write-Host ""

if ($failedCount -gt 0) {
    Write-Warning "$failedCount texture(s) failed to encode. Check output above."
    exit 1
}
exit 0