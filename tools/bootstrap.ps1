# =============================================================================
# bootstrap.ps1 — X3Native dev-machine bootstrap
# =============================================================================
# Clones X3Native and verifies the working tree is clean (original engine only —
# no third-party engine source). X3Native is built clean-room from scratch from
# specs/ + public references; there is no GPL/RBDOOM code. See PROVENANCE.md.
#
# Usage:
#   pwsh -File tools/bootstrap.ps1 [-Dir C:\GameDev\X3Native]
# =============================================================================

param(
    [string]$Dir  = "C:\GameDev\X3Native",
    [string]$Repo = "https://github.com/1GreenNinja/X3Native.git",
    [string]$Branch = "main"
)

$ErrorActionPreference = "Stop"
Write-Host "=== X3Native bootstrap ===" -ForegroundColor Cyan

# 1. Clone (or reuse)
if (Test-Path $Dir) {
    Write-Warning "$Dir already exists. Skipping clone."
} else {
    Write-Host "Cloning $Repo ..." -ForegroundColor Yellow
    git clone $Repo $Dir
}
Set-Location $Dir
git checkout $Branch 2>$null

# 2. Provenance guard: no foreign-engine source should ever appear in the tree.
$forbidden = @("engine\_gpl_rbdoom", "engine\_rbdoom", "rbdoom", "doom3", "idtech")
$hit = $false
foreach ($f in $forbidden) {
    if (Test-Path (Join-Path $Dir $f)) {
        Write-Error "PROVENANCE VIOLATION: '$f' is present. X3Native must contain no third-party engine source. Remove it before any work."
        $hit = $true
    }
}
if (-not $hit) { Write-Host "PROVENANCE OK: no third-party engine source in the tree." -ForegroundColor Green }

# 3. Confirm the clean-room inputs ARE present
$need = @("specs", "PROVENANCE.md", "docs\CLEANROOM_PROCESS.md", "X3_NATIVE_ENGINE_PLAN.md")
foreach ($n in $need) {
    if (Test-Path (Join-Path $Dir $n)) { Write-Host "  present: $n" -ForegroundColor Green }
    else { Write-Warning "  MISSING: $n" }
}

# 4. Toolchain check (non-fatal — reports only)
Write-Host "`n=== Toolchain check ===" -ForegroundColor Cyan
function Check($name, $cmd) {
    try { $v = & $cmd 2>$null | Select-Object -First 1; Write-Host ("  {0,-12} {1}" -f $name, $v) -ForegroundColor Green }
    catch { Write-Warning "  $name not found" }
}
Check "cmake"   { cmake --version }
Check "git"     { git --version }
Check "vulkan"  { if ($env:VULKAN_SDK) { "SDK: $env:VULKAN_SDK" } else { "SDK NOT INSTALLED — winget install KhronosGroup.VulkanSDK" } }
$vs = Get-ChildItem "C:\Program Files\Microsoft Visual Studio" -Directory -ErrorAction SilentlyContinue | Select-Object -Last 1
if ($vs) { Write-Host "  vstudio      $($vs.Name)" -ForegroundColor Green } else { Write-Warning "  Visual Studio not found in default path" }

Write-Host "`n=== Ready ===" -ForegroundColor Cyan
Write-Host "Implement only from specs/*.spec.md + public references + permissive libs."
Write-Host "Never read or transcribe any third-party game-engine source. Record subsystems in PROVENANCE.md."
