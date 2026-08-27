# fold_gate.ps1 - integration/fold-0812 gate driver (ASCII only).
# Usage: powershell -File tools\fold_gate.ps1 -Label wave1 [-SkipBuild]
param(
  [string]$Label = "gate",
  [switch]$SkipBuild,
  [switch]$CleanConfigure
)
$ErrorActionPreference = "Continue"
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo
$outDir = Join-Path $repo "gatelogs"
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Force $outDir | Out-Null }
$log = Join-Path $outDir "$Label.txt"
"" | Out-File $log -Encoding ascii

function W([string]$s) { Write-Host $s; $s | Out-File $log -Append -Encoding ascii }

W "===== GATE $Label  $(Get-Date -Format o) ====="
$foreign = Get-CimInstance Win32_Process -Filter "Name='X3Engine.exe'" |
           Where-Object { $_.CommandLine -notlike "*X3Native-fold*" }
if ($foreign) { foreach ($f in $foreign) { W "FOREIGN-ENGINE pid=$($f.ProcessId) : $($f.CommandLine)" } }
else { W "FOREIGN-ENGINE none" }

if (-not $SkipBuild) {
  if ($CleanConfigure) {
    W "-- removing build/ for clean configure --"
    if (Test-Path build) { Remove-Item build -Recurse -Force -ErrorAction SilentlyContinue }
  }
  $t = Get-Date
  $cfg = cmake --preset windows-vs2026 2>&1
  $cfgExit = $LASTEXITCODE
  W "CONFIGURE exit=$cfgExit  ($([math]::Round(((Get-Date)-$t).TotalSeconds,1))s)"
  if ($cfgExit -ne 0) { $cfg | Select-Object -Last 30 | ForEach-Object { W $_ }; W "GATE ABORT: configure failed"; exit 1 }

  foreach ($conf in @("Release","Debug")) {
    $t = Get-Date
    $b = cmake --build build --config $conf 2>&1
    $bExit = $LASTEXITCODE
    $errs = $b | Select-String -Pattern ": error " | Select-Object -Unique
    W "BUILD $conf exit=$bExit errors=$($errs.Count)  ($([math]::Round(((Get-Date)-$t).TotalMinutes,2))min)"
    if ($bExit -ne 0) { $errs | Select-Object -First 40 | ForEach-Object { W "  $_" }; W "GATE ABORT: $conf build failed"; exit 1 }
  }
}

$rel = Join-Path $repo "build\bin\Release\X3Engine.exe"
$dbg = Join-Path $repo "build\bin\Debug\X3Engine.exe"

function RunEngine([string]$exe, [string[]]$argv, [int]$timeoutSec = 240) {
  $so = [System.IO.Path]::GetTempFileName(); $se = [System.IO.Path]::GetTempFileName()
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $exe
  $psi.Arguments = ($argv -join " ")
  $psi.WorkingDirectory = (Get-Location).Path
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $p = New-Object System.Diagnostics.Process
  $p.StartInfo = $psi
  $null = $p.Start()
  $tOut = $p.StandardOutput.ReadToEndAsync()
  $tErr = $p.StandardError.ReadToEndAsync()
  $code = -999
  if ($p.WaitForExit($timeoutSec * 1000)) { $p.WaitForExit(); $code = $p.ExitCode }
  else {
    # Windows PowerShell 5.1 has no Process.Kill(bool): kill the TREE via taskkill,
    # otherwise the inherited stdout pipe stays open and the async read deadlocks.
    & taskkill.exe /PID $p.Id /T /F 2>&1 | Out-Null
    try { $p.WaitForExit(10000) } catch {}
    if (-not $p.HasExited) { try { $p.Kill() } catch {} }
  }
  $txt = ""
  try {
    if ($tOut.Wait(15000)) { $txt += $tOut.Result }
    if ($tErr.Wait(15000)) { $txt += $tErr.Result }
  } catch {}
  Remove-Item $so,$se -Force -EA SilentlyContinue
  return @{ exit = $code; out = $txt }
}

# Count REAL validation findings, not the banner lines that merely name them.
function CountVuid([string]$txt) {
  return ([regex]::Matches($txt, "VUID-[A-Za-z0-9_\-]+")).Count
}
function CountHazard([string]$txt) {
  return ([regex]::Matches($txt, "SYNC-HAZARD-[A-Z\-]+")).Count
}

# ---- smoketests: Release + Debug, 3 worlds ----
foreach ($pair in @(@("Release",$rel), @("Debug",$dbg))) {
  $conf = $pair[0]; $exe = $pair[1]
  foreach ($w in @(@(), @("--world","echotropolis"), @("--world","canonlevel"))) {
    $name = if ($w.Count -eq 0) { "default" } else { $w[1] }
    $r = RunEngine $exe (@("--smoketest") + $w) 300
    $alloc = ([regex]::Matches($r.out, "allocationCount=(\d+)") | ForEach-Object { $_.Groups[1].Value }) -join ","
    $vuid  = CountVuid $r.out
    $lay   = if ($r.out -match "VALIDATION: layers=ON") { "layers=ON" } else { "layers=OFF" }
    W "SMOKE $conf/$name exit=$($r.exit) allocationCount=[$alloc] VUIDfindings=$vuid $lay"
    if ($r.exit -ne 0) {
      ($r.out -split "`n" | Select-String -Pattern "ERROR|error|FATAL|assert" | Select-Object -Last 8) | ForEach-Object { W "    $_" }
    }
  }
}

# ---- vksync hazard gate (Debug) ----
$r = RunEngine $dbg @("--smoketest","--vksync") 420
$haz  = CountHazard $r.out
$vuid = CountVuid $r.out
$sv = if ($r.out -match "sync-validation=ON") { "sync-validation=ON" } else { "sync-validation=OFF" }
W "VKSYNC Debug exit=$($r.exit) hazards=$haz VUIDfindings=$vuid $sv"
if ($haz -gt 0) { ($r.out -split "`n" | Select-String -Pattern "SYNC-HAZARD-" | Select-Object -First 6) | ForEach-Object { W "    $_" } }

# ---- test ladder (Release) ----
$ladder = @("--test-terrain","--test-terrainplace","--test-terraincorridor","--test-tunnelmouth",
            "--test-city","--test-cityblocks","--test-worldregions","--test-levellint","--test-propclip",
            "--test-csm","--test-clusterlights","--test-refldenoise","--test-geolod","--test-deathragdoll",
            "--test-canonlevel","--test-canonplay","--test-audio","--test-jukebox","--test-minefx",
            "--test-worldstream","--test-level1")
# Added by fix/echo-sea-level (wave 2, 27 checks). Unrecognized before that branch lands,
# where it would fall through to a real run -> the ladder marks it TIMEOUT/HANG.
if ($env:X3_GATE_SEALEVEL -ne "0") { $ladder += "--test-sealevel" }
foreach ($t in $ladder) {
  $r = RunEngine $rel @($t) 200
  # Prefer an explicit tally line; READ THE COUNTS (never -match "FAIL").
  $tally = ""
  $m = [regex]::Match($r.out, "(\d+)\s+passed,\s+(\d+)\s+failed")
  if ($m.Success) { $tally = "passed=$($m.Groups[1].Value) failed=$($m.Groups[2].Value)" }
  else {
    $m2 = [regex]::Match($r.out, "(\d+)/(\d+)\s+passed")
    if ($m2.Success) { $tally = "passed=$($m2.Groups[1].Value)/$($m2.Groups[2].Value)" }
    elseif ($r.out -match "PASS \(0 violations\)") { $tally = "PASS(0 violations)" }
    elseif ($r.out -match "ALL CHECKS PASSED") { $tally = "ALL CHECKS PASSED" }
  }
  $failLines = ([regex]::Matches($r.out, "FAIL [A-Za-z0-9]+")).Count
  if ($r.exit -eq -999) { $tally = "TIMEOUT/HANG (flag likely unrecognized -> fell through to a real run)" }
  W ("LADDER {0,-26} exit={1} {2} failLines={3}" -f $t, $r.exit, $tally, $failLines)
}
W "===== GATE $Label END $(Get-Date -Format o) ====="
