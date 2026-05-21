# =============================================================================
# cleanroom-setup.ps1 — 13700K clean-room bootstrap
# =============================================================================
# Sets up the clean-room team checkout on the 13700K. The information barrier
# is enforced by REPO TOPOLOGY: this checkout PHYSICALLY OMITS the GPL
# quarantine dir (engine/_gpl_rbdoom/) via git sparse-checkout, so clean-room
# agents cannot read the GPL source even by accident.
#
# Clean-room team implements ONLY from specs/*.spec.md + public references.
# See docs/CLEANROOM_PROCESS.md and specs/README.md.
#
# Usage (on the 13700K):
#   pwsh -File tools/cleanroom-setup.ps1 [-Dir C:\GameDev\X3Native-cleanroom]
# =============================================================================

param(
    [string]$Dir  = "C:\GameDev\X3Native-cleanroom",
    [string]$Repo = "https://github.com/1GreenNinja/X3Native.git",
    [string]$Branch = "main"
)

$ErrorActionPreference = "Stop"
Write-Host "=== X3Native clean-room bootstrap (13700K) ===" -ForegroundColor Cyan

# 1. Clone with NO checkout, sparse mode
if (Test-Path $Dir) {
    Write-Warning "$Dir already exists. Re-running sparse config only."
} else {
    Write-Host "Cloning $Repo (sparse, no GPL dir)..." -ForegroundColor Yellow
    git clone --filter=blob:none --no-checkout $Repo $Dir
}
Set-Location $Dir
git checkout $Branch 2>$null

# 2. Sparse-checkout: include everything EXCEPT the GPL quarantine
Write-Host "Configuring sparse-checkout to OMIT engine/_gpl_rbdoom/ ..." -ForegroundColor Yellow
git sparse-checkout init --cone
# Cone mode includes root files + listed dirs; we want all dirs except the GPL one.
# Use non-cone set with a negative pattern for precise exclusion:
git sparse-checkout set --no-cone `
    '/*' `
    '!/engine/_gpl_rbdoom/' `
    '!engine/_gpl_rbdoom'
git checkout $Branch

# 3. Hard verification: the GPL dir MUST NOT be on disk
if (Test-Path (Join-Path $Dir "engine\_gpl_rbdoom")) {
    Write-Error "BARRIER FAILED: engine/_gpl_rbdoom/ is present on the clean-room checkout. Do NOT proceed — the information barrier is broken. Fix sparse-checkout before any clean-room work."
    exit 1
} else {
    Write-Host "BARRIER OK: engine/_gpl_rbdoom/ is absent from this checkout." -ForegroundColor Green
}

# 4. Confirm the clean-room inputs ARE present
$need = @("specs", "GPL_DEBT.md", "docs\CLEANROOM_PROCESS.md", "X3_NATIVE_ENGINE_PLAN.md")
foreach ($n in $need) {
    if (Test-Path (Join-Path $Dir $n)) { Write-Host "  present: $n" -ForegroundColor Green }
    else { Write-Warning "  MISSING: $n" }
}

# 5. Toolchain check (non-fatal — reports only)
Write-Host "`n=== Toolchain check (13700K) ===" -ForegroundColor Cyan
function Check($name, $cmd) {
    try { $v = & $cmd 2>$null | Select-Object -First 1; Write-Host ("  {0,-12} {1}" -f $name, $v) -ForegroundColor Green }
    catch { Write-Warning "  $name not found" }
}
Check "cmake"   { cmake --version }
Check "git"     { git --version }
Check "vulkan"  { if ($env:VULKAN_SDK) { "SDK: $env:VULKAN_SDK" } else { "SDK NOT INSTALLED — winget install KhronosGroup.VulkanSDK" } }
$vs = Get-ChildItem "C:\Program Files\Microsoft Visual Studio" -Directory -ErrorAction SilentlyContinue | Select-Object -Last 1
if ($vs) { Write-Host "  vstudio      $($vs.Name)" -ForegroundColor Green } else { Write-Warning "  Visual Studio not found in default path" }

Write-Host "`n=== Clean-room ready ===" -ForegroundColor Cyan
Write-Host "Implement ONLY from specs/*.spec.md + public refs. Never request the GPL source."
Write-Host "Pick a TODO/SPEC row from GPL_DEBT.md, implement behind its interface, run acceptance tests, flip to DONE-CLEAN."
