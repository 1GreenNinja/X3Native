<#
================================================================================
 echo_stream_ab.ps1 -- WP-5 verification kit for TIER 2 WorldStreamer adoption
 (docs/plans/TIER2_STREAMING_PLAN.md, section 4, WP-5).

 PowerShell 7. Drives the ALREADY-BUILT EchoHarbor.exe only -- this script
 never touches CMake and never edits any other file in the repo (WP-5's
 file-ownership rule: scripts/echo_stream_ab.ps1 + docs/plans/TIER2_VERIFY.md
 only).

 Subcommands
 -----------
   capture   -Label <name> [-Tod golden|dusk|night|noon|<0..1>] [-LegacyPost 0|1|2]
             [-Settle <n>] [-ExePath <path>] [-CaptureRoot <dir>]
       Fires the 6 canonical fixed-TOD --legacypost --shot-cam captures
       (postcard, crown street, mine, and the 3 district gates) into
       captures/tier2/<Label>/*.png + a manifest.json recording exactly how
       each shot was derived and framed.

   compare <labelA> <labelB> [-CaptureRoot <dir>]
       Byte-compares (SHA-256) every canonical capture between two label
       sets already produced by `capture`. Prints a PASS/FAIL table + a hash
       table, exits 0 iff every image is byte-identical.

   flyacross [-Tod golden] [-DurationSec 180] [-ExePath <path>] [-CaptureRoot <dir>] [-Force]
       Documents (prints) the manual flight the operator must fly by hand --
       spawn crown -> `go harbor` -> `go drag` -> each district gate at
       `speed 3` -- then launches the exe with ECHO_AUTOEXIT_SEC as a safety
       cutoff, tees console/log output to a timestamped file, and greps it
       afterward for '[worldstream]', 'PROXY collision floor engaged', and
       the per-second FPS line, printing a build/evict/proxy/FPS summary.

   boottime [-Runs 3] [-BootAutoExitSec 12] [-ExePath <path>] [-CaptureRoot <dir>]
       Launches the exe N times (default 3), measuring wall-clock time from
       process start to the FIRST "--world echotropolis: FPS" log line, and
       reports each run + the average.

 Every subcommand fails loudly (non-zero exit, clear message) if the exe is
 missing, if EchoHarbor.exe is already running (Tim may be playing), or if
 the exe file looks locked.

 Examples
 --------
   .\scripts\echo_stream_ab.ps1 capture -Label milestoneA-golden
   .\scripts\echo_stream_ab.ps1 capture -Label milestoneA-golden-after   # after WP-0 merges M-A
   .\scripts\echo_stream_ab.ps1 compare milestoneA-golden milestoneA-golden-after
   .\scripts\echo_stream_ab.ps1 flyacross -DurationSec 240
   .\scripts\echo_stream_ab.ps1 boottime -Runs 5
================================================================================
#>

[CmdletBinding()]
param(
    [Parameter(Position = 0, Mandatory = $true)]
    [ValidateSet('capture', 'compare', 'flyacross', 'boottime')]
    [string]$Command,

    # Positional convenience args: `capture <label>` / `compare <labelA> <labelB>`.
    [Parameter(Position = 1)] [string]$Arg1,
    [Parameter(Position = 2)] [string]$Arg2,

    [string]$Label,
    [string]$LabelA,
    [string]$LabelB,

    [string]$Tod = 'golden',
    [ValidateRange(0, 2)] [int]$LegacyPost = 1,
    [int]$Settle = 32,

    [string]$ExePath,
    [string]$CaptureRoot,

    [int]$DurationSec = 180,        # flyacross safety autoexit (ECHO_AUTOEXIT_SEC)
    [int]$Runs = 3,                 # boottime
    [int]$BootAutoExitSec = 12,     # boottime per-run ECHO_AUTOEXIT_SEC

    [switch]$Force                  # skip the "press Enter" gate / running-instance guard
)

Set-StrictMode -Version Latest
# NOTE: deliberately NOT 'Stop' -- every failure path below calls Write-Error
# (a non-terminating error under the default EAP) immediately followed by an
# explicit `exit <code>` so distinct exit codes survive. True .NET exceptions
# (e.g. from Start-Process) terminate regardless of this setting.
$ErrorActionPreference = 'Continue'

# --------------------------------------------------------------------------
# Paths
# --------------------------------------------------------------------------
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $ExePath)     { $ExePath     = Join-Path $RepoRoot 'build\bin\Release\EchoHarbor.exe' }
if (-not $CaptureRoot) { $CaptureRoot = Join-Path $RepoRoot 'captures\tier2' }

# --------------------------------------------------------------------------
# The 6 canonical shot-cam vantages (TIER2_STREAMING_PLAN.md sec 4, WP-5:
# "6 canonical cams: postcard, crown street, mine, each district gate").
# Every coordinate below is read directly from the code -- none invented:
#   - postcard / crown_street / mine reuse the host's own `go` bookmark
#     table (host_echotropolis.cpp ~line 3308-3318; "drag" is the crown's
#     main street per the comment at ~line 3111 "the crown city + drag +
#     districts").
#   - the 3 district gates are derived from assets/districts/districts.txt
#     (pad centers) + the woodlands keep-out rects baked into
#     host_echotropolis.cpp ~line 1166-1169 (a 25 m margin around each pad's
#     footprint). Each gate camera sits 40 m outside the keep-out edge on
#     the crown-facing (west) side, looking east (yaw 0) at the pad, at the
#     TIER2 plan's own approximate flats-bowl elevation (sec 1 table: "~35"
#     + ~5 m eye height). This is an approximation for a repeatable,
#     reproducible A/B camera -- NOT a final art-review framing; the plan's
#     own §1 note says use hf.heightAt(anchor) at runtime for the true Y.
# --------------------------------------------------------------------------
function Get-CanonicalCams {
    [PSCustomObject[]]@(
        [PSCustomObject]@{ Name = 'postcard';               X = -450; Y = 620; Z = 900;  Yaw = -1.02; Pitch = -0.28
            Source = "go-table 'postcard' (host_echotropolis.cpp ~3311). High/wide vista -- triggers the plan's vista rule (camY - hf.heightAt > 250m), so this shot should stay BYTE-IDENTICAL across all milestones (Decision 1)." }
        [PSCustomObject]@{ Name = 'crown_street';            X = -30;  Y = 205; Z = 740;  Yaw = 1.40;  Pitch = -0.15
            Source = "go-table 'drag' (host_echotropolis.cpp ~3313), the crown's main street." }
        [PSCustomObject]@{ Name = 'mine';                    X = -480; Y = 260; Z = 850;  Yaw = 0.00;  Pitch = -0.35
            Source = "go-table 'mine' (host_echotropolis.cpp ~3315); matches the west_shoulder region anchor (TIER2 plan sec 1)." }
        [PSCustomObject]@{ Name = 'district_urban_gate';     X = 475;  Y = 40;  Z = 350;  Yaw = 0.00;  Pitch = -0.12
            Source = "districts.txt URBAN DISTRICT pad (700,350); keep-out rect x[515,885] (host_echotropolis.cpp ~1168); cam 40m west of the keep-out edge, looking east." }
        [PSCustomObject]@{ Name = 'district_recife_gate';    X = 870;  Y = 40;  Z = 1250; Yaw = 0.00;  Pitch = -0.12
            Source = "districts.txt RECIFE 2050 pad (950,1250); keep-out rect x[910,1135] (host_echotropolis.cpp ~1167); cam 40m west of the keep-out edge, looking east." }
        [PSCustomObject]@{ Name = 'district_hivemind_gate';  X = 1145; Y = 40;  Z = 1000; Yaw = 0.00;  Pitch = -0.12
            Source = "districts.txt HIVEMIND CYBERCITY pad (1340,1000, yOff -23); keep-out rect x[1185,1495] (host_echotropolis.cpp ~1169); cam 40m west of the keep-out edge, looking east." }
    )
}

# --------------------------------------------------------------------------
# Readiness / safety checks -- "fails loudly if the exe is missing or locked"
# --------------------------------------------------------------------------
function Assert-ExeReady {
    param([string]$Exe, [switch]$AllowForce)

    if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
        Write-Error "FATAL: EchoHarbor.exe not found at '$Exe'. Build it first (WP-0 integrator owns cmake) or pass -ExePath <path>."
        exit 2
    }

    $running = Get-Process -Name 'EchoHarbor' -ErrorAction SilentlyContinue
    if ($running) {
        $pids = ($running | ForEach-Object { $_.Id }) -join ', '
        if ($AllowForce -and $Force) {
            Write-Warning "EchoHarbor.exe is already running (PID $pids). -Force given -- proceeding anyway. This may collide with an active session."
        } else {
            Write-Error "FATAL: EchoHarbor.exe is already running (PID $pids). The user may be playing -- refusing to launch a second instance. Close it first, or pass -Force to override (not recommended)."
            exit 3
        }
    }

    # Best-effort lock probe (e.g. a build mid-write holds an exclusive handle).
    try {
        $fs = [System.IO.File]::Open($Exe, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        $fs.Close()
    } catch {
        Write-Error "FATAL: '$Exe' appears to be locked (in use / mid-write): $($_.Exception.Message)"
        exit 4
    }
}

function Test-TodValue {
    param([string]$Value)
    if ($Value -in @('golden', 'dusk', 'night', 'noon')) { return }
    $f = 0.0
    if ([double]::TryParse($Value, [ref]$f) -and $f -ge 0.0 -and $f -lt 1.0) { return }
    Write-Error "FATAL: -Tod '$Value' is not one of golden|dusk|night|noon and not a raw [0,1) fraction (see canonTodFraction, host_echotropolis.cpp ~291)."
    exit 7
}

# --------------------------------------------------------------------------
# Process launch helpers. Every EchoHarbor.exe invocation runs with
# WorkingDirectory = repo root -- asset paths in host_echotropolis.cpp
# (e.g. "assets/districts/districts.txt") are relative to it.
# --------------------------------------------------------------------------
function Start-EchoExe {
    param([string]$Exe, [string[]]$ExeArgs, [string]$LogPath)

    $errPath = "$LogPath.err"
    Remove-Item -LiteralPath $LogPath -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $errPath -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $LogPath) | Out-Null

    try {
        $proc = Start-Process -FilePath $Exe -ArgumentList $ExeArgs -WorkingDirectory $RepoRoot `
                    -NoNewWindow -PassThru `
                    -RedirectStandardOutput $LogPath -RedirectStandardError $errPath
    } catch {
        Write-Error "FATAL: failed to launch '$Exe' ($ExeArgs -join ' '): $($_.Exception.Message)"
        exit 4
    }
    [PSCustomObject]@{ Process = $proc; Log = $LogPath; ErrLog = $errPath }
}

function Complete-EchoExe {
    param($Handle, [int]$TimeoutSec)

    $exited = $Handle.Process.WaitForExit($TimeoutSec * 1000)
    if (-not $exited) {
        Write-Warning "PID $($Handle.Process.Id) did not exit within ${TimeoutSec}s -- killing it."
        try { Stop-Process -Id $Handle.Process.Id -Force -ErrorAction Stop } catch {}
    }
    Start-Sleep -Milliseconds 100   # let the OS release the file handles
    if (Test-Path -LiteralPath $Handle.ErrLog) {
        Get-Content -LiteralPath $Handle.ErrLog -ErrorAction SilentlyContinue | Add-Content -LiteralPath $Handle.Log -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $Handle.ErrLog -ErrorAction SilentlyContinue
    }
    return $exited
}

# ============================================================================
# capture
# ============================================================================
function Invoke-CaptureCmd {
    param([string]$Lbl, [string]$TodVal, [int]$LegacyPostVal, [int]$SettleVal)

    if (-not $Lbl) { Write-Error "FATAL: capture needs -Label <name> (or a positional label)."; exit 1 }
    Test-TodValue -Value $TodVal
    Assert-ExeReady -Exe $ExePath -AllowForce

    $outDir = Join-Path $CaptureRoot $Lbl
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null

    $cams = Get-CanonicalCams
    $results = @()

    $env:ECHO_TOD = $TodVal
    try {
        foreach ($cam in $cams) {
            $outPath = Join-Path $outDir "$($cam.Name).png"
            $shotCam = '{0},{1},{2},{3},{4}' -f $cam.X, $cam.Y, $cam.Z, $cam.Yaw, $cam.Pitch
            $exeArgs = @('--world', 'echotropolis', '--screenshot', $outPath, "$SettleVal", '--shot-cam', $shotCam)
            if ($LegacyPostVal -eq 1)      { $exeArgs += '--legacypost' }
            elseif ($LegacyPostVal -eq 2)  { $exeArgs += '--legacypost2' }

            Write-Host "[$Lbl] capturing $($cam.Name) @ ($shotCam) ECHO_TOD=$TodVal ..."
            $camLog = Join-Path $outDir "$($cam.Name).log"
            $h = Start-EchoExe -Exe $ExePath -ExeArgs $exeArgs -LogPath $camLog
            Complete-EchoExe -Handle $h -TimeoutSec 60 | Out-Null

            $exitCode = $h.Process.ExitCode
            $ok = (Test-Path -LiteralPath $outPath) -and ((Get-Item -LiteralPath $outPath).Length -gt 0) -and ($exitCode -eq 0)
            if (-not $ok) {
                Write-Warning "capture FAILED for '$($cam.Name)' (exit $exitCode) -- see $camLog"
            }
            $results += [PSCustomObject]@{ Cam = $cam.Name; Path = $outPath; ExitCode = $exitCode; OK = $ok }
        }
    } finally {
        Remove-Item Env:ECHO_TOD -ErrorAction SilentlyContinue
    }

    $manifest = [PSCustomObject]@{
        label     = $Lbl
        tod       = $TodVal
        legacypost = $LegacyPostVal
        settle    = $SettleVal
        timestamp = (Get-Date).ToString('o')
        cams      = $cams
        results   = $results
    }
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $outDir 'manifest.json')

    $fails = @($results | Where-Object { -not $_.OK })
    Write-Host ""
    if ($fails.Count -eq 0) {
        Write-Host "capture '$Lbl': $($results.Count)/$($results.Count) shots OK -> $outDir" -ForegroundColor Green
        exit 0
    } else {
        Write-Host "capture '$Lbl': $($fails.Count)/$($results.Count) shots FAILED -> $outDir" -ForegroundColor Red
        exit 1
    }
}

# ============================================================================
# compare
# ============================================================================
function Invoke-CompareCmd {
    param([string]$LA, [string]$LB)

    if (-not $LA -or -not $LB) { Write-Error "FATAL: compare needs two labels: compare <labelA> <labelB>."; exit 1 }

    $dirA = Join-Path $CaptureRoot $LA
    $dirB = Join-Path $CaptureRoot $LB
    if (-not (Test-Path -LiteralPath $dirA)) { Write-Error "FATAL: capture set '$LA' not found at $dirA. Run 'capture -Label $LA' first."; exit 5 }
    if (-not (Test-Path -LiteralPath $dirB)) { Write-Error "FATAL: capture set '$LB' not found at $dirB. Run 'capture -Label $LB' first."; exit 5 }

    $cams = Get-CanonicalCams
    $rows = @()
    $failCount = 0

    foreach ($cam in $cams) {
        $fa = Join-Path $dirA "$($cam.Name).png"
        $fb = Join-Path $dirB "$($cam.Name).png"
        $existsA = Test-Path -LiteralPath $fa
        $existsB = Test-Path -LiteralPath $fb

        if (-not $existsA -or -not $existsB) {
            $rows += [PSCustomObject]@{ Cam = $cam.Name; Status = 'MISSING'
                HashA = $(if ($existsA) { (Get-FileHash -LiteralPath $fa -Algorithm SHA256).Hash } else { '<missing>' })
                HashB = $(if ($existsB) { (Get-FileHash -LiteralPath $fb -Algorithm SHA256).Hash } else { '<missing>' }) }
            $failCount++
            continue
        }

        $ha = (Get-FileHash -LiteralPath $fa -Algorithm SHA256).Hash
        $hb = (Get-FileHash -LiteralPath $fb -Algorithm SHA256).Hash
        $status = if ($ha -eq $hb) { 'PASS' } else { 'FAIL' }
        if ($status -eq 'FAIL') { $failCount++ }
        $rows += [PSCustomObject]@{ Cam = $cam.Name; Status = $status; HashA = $ha; HashB = $hb }
    }

    Write-Host ""
    Write-Host "byte-compare: '$LA' vs '$LB'"
    $rows | Format-Table -AutoSize | Out-String | Write-Host

    if ($failCount -eq 0) {
        Write-Host "RESULT: PASS -- all $($cams.Count) canonical captures byte-identical." -ForegroundColor Green
        exit 0
    } else {
        Write-Host "RESULT: FAIL -- $failCount / $($cams.Count) capture(s) differ or are missing." -ForegroundColor Red
        exit 1
    }
}

# ============================================================================
# flyacross
# ============================================================================
function Invoke-FlyacrossCmd {
    param([string]$TodVal, [int]$Duration)

    Test-TodValue -Value $TodVal
    Assert-ExeReady -Exe $ExePath -AllowForce

    $logDir = Join-Path $CaptureRoot 'logs'
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    $stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
    $log = Join-Path $logDir "flyacross_$stamp.log"

    Write-Host '===================================================================='
    Write-Host ' FLYACROSS -- manual region-streaming verification (milestones B/C/D)'
    Write-Host '===================================================================='
    Write-Host "Launching with ECHO_TOD=$TodVal, ECHO_AUTOEXIT_SEC=$Duration (safety cutoff)."
    Write-Host "Console/log output is teed to:`n  $log`n"
    Write-Host "Fly this BY HAND within the $Duration s window:"
    Write-Host '  1. Wait for spawn at the crown (default spawn point).'
    Write-Host '  2. Open the console (` backtick) and run:  go harbor'
    Write-Host '  3. Fly the harbor bay a few seconds, then console:  go drag'
    Write-Host '  4. Fly the crown street (the "drag"), then head out toward each'
    Write-Host '     district gate in turn: Urban (700,350), Recife (950,1250),'
    Write-Host '     Hivemind (1340,1000) -- at a moderate console `speed 3`'
    Write-Host '     (per TIER2 plan Decision 2: base 240 m/s x lookahead keeps'
    Write-Host '     you inside stream range; above ~600 m/s vista mode takes over).'
    Write-Host '  5. Watch for pop / hitching as you cross each region boundary.'
    Write-Host '  6. Let the run autoexit, or Esc -> pause menu -> Quit when done.'
    Write-Host ''

    if (-not $Force) {
        Write-Host 'Press Enter to launch now (Ctrl+C to abort) ...' -NoNewline
        [void](Read-Host)
    }

    $env:ECHO_TOD = $TodVal
    $env:ECHO_AUTOEXIT_SEC = "$Duration"
    try {
        $h = Start-EchoExe -Exe $ExePath -ExeArgs @('--world', 'echotropolis') -LogPath $log
        Write-Host "Launched PID $($h.Process.Id). Waiting up to $($Duration + 30)s for it to exit ..."
        Complete-EchoExe -Handle $h -TimeoutSec ($Duration + 30) | Out-Null
    } finally {
        Remove-Item Env:ECHO_TOD -ErrorAction SilentlyContinue
        Remove-Item Env:ECHO_AUTOEXIT_SEC -ErrorAction SilentlyContinue
    }

    Write-Host ''
    Write-Host "---- log analysis ($log) ----"
    if (-not (Test-Path -LiteralPath $log)) {
        Write-Error "FATAL: no log was written -- the process may have failed to launch."
        exit 6
    }

    $wsLines    = @(Select-String -LiteralPath $log -Pattern '\[worldstream\]')
    $buildLines = @($wsLines | Where-Object { $_.Line -match '\[worldstream\] \+' })
    $evictLines = @($wsLines | Where-Object { $_.Line -match '\[worldstream\] -' })
    $proxyLines = @(Select-String -LiteralPath $log -Pattern 'PROXY collision floor engaged')
    $fpsLines   = @(Select-String -LiteralPath $log -Pattern '--world echotropolis: FPS ')

    Write-Host ("[worldstream] total lines        : {0}" -f $wsLines.Count)
    Write-Host ("  region builds (+)               : {0}" -f $buildLines.Count)
    Write-Host ("  region evictions (-)             : {0}" -f $evictLines.Count)
    Write-Host ("proxy engages (risk #3, expect 0) : {0}" -f $proxyLines.Count)

    if ($fpsLines.Count -gt 0) {
        $fpsVals = @($fpsLines | ForEach-Object { if ($_.Line -match 'FPS ([\d.]+)') { [double]$Matches[1] } } | Where-Object { $null -ne $_ })
        if ($fpsVals.Count -gt 0) {
            $min = ($fpsVals | Measure-Object -Minimum).Minimum
            $avg = ($fpsVals | Measure-Object -Average).Average
            $max = ($fpsVals | Measure-Object -Maximum).Maximum
            Write-Host ("FPS samples: {0}  min={1:N1}  avg={2:N1}  max={3:N1}" -f $fpsVals.Count, $min, $avg, $max)
        }
    } else {
        Write-Warning 'No FPS lines found -- did the windowed loop actually run (vs. exiting early on an error)?'
    }

    if ($proxyLines.Count -gt 0) {
        Write-Host 'FINDING: proxyEngageCount > 0 -- per TIER2 plan Decision 3 this should be 0 at sane speeds. Investigate before signing off M-C/M-D.' -ForegroundColor Yellow
    }
    if ($wsLines.Count -eq 0) {
        Write-Host 'NOTE: 0 [worldstream] lines is EXPECTED before WP-0 integrates EchoRegionSet, or under ECHO_STREAM=0 (the milestone-A rollback path).' -ForegroundColor DarkYellow
    }
    Write-Host "Full log: $log"
}

# ============================================================================
# boottime
# ============================================================================
function Invoke-BoottimeCmd {
    param([string]$TodVal, [int]$RunCount, [int]$AutoExitSec)

    Test-TodValue -Value $TodVal
    Assert-ExeReady -Exe $ExePath -AllowForce

    $logDir = Join-Path $CaptureRoot 'logs'
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null

    $timings = @()
    for ($i = 1; $i -le $RunCount; $i++) {
        $stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
        $log = Join-Path $logDir "boottime_${stamp}_run$i.log"

        $env:ECHO_TOD = $TodVal
        $env:ECHO_AUTOEXIT_SEC = "$AutoExitSec"
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        try {
            $h = Start-EchoExe -Exe $ExePath -ExeArgs @('--world', 'echotropolis') -LogPath $log

            $found = $false
            $elapsedMs = $null
            $deadline = (Get-Date).AddSeconds($AutoExitSec + 20)
            while (-not $found -and (Get-Date) -lt $deadline) {
                if (Test-Path -LiteralPath $log) {
                    $hit = Select-String -LiteralPath $log -Pattern '--world echotropolis: FPS ' -ErrorAction SilentlyContinue | Select-Object -First 1
                    if ($hit) { $found = $true; $elapsedMs = $sw.Elapsed.TotalMilliseconds; break }
                }
                Start-Sleep -Milliseconds 25
            }

            if ($found) {
                Write-Host ("Run {0}: first FPS log line at {1:N0} ms" -f $i, $elapsedMs)
                $timings += $elapsedMs
            } else {
                Write-Warning "Run $i -- never saw the first FPS log line within $($AutoExitSec + 20)s -- treating as a boot failure. See $log"
            }

            Complete-EchoExe -Handle $h -TimeoutSec ($AutoExitSec + 30) | Out-Null
        } finally {
            Remove-Item Env:ECHO_TOD -ErrorAction SilentlyContinue
            Remove-Item Env:ECHO_AUTOEXIT_SEC -ErrorAction SilentlyContinue
        }
    }

    Write-Host ''
    if ($timings.Count -gt 0) {
        $avg = ($timings | Measure-Object -Average).Average
        $runsStr = ($timings | ForEach-Object { '{0:N0}' -f $_ }) -join ', '
        Write-Host ("boottime: {0}/{1} runs succeeded -- avg first-FPS-line = {2:N0} ms  (runs: {3} ms)" -f $timings.Count, $RunCount, $avg, $runsStr) -ForegroundColor Green
        exit 0
    } else {
        Write-Error "boottime: all $RunCount runs FAILED to reach the first FPS log line."
        exit 6
    }
}

# ============================================================================
# Dispatch
# ============================================================================
switch ($Command) {
    'capture' {
        $lbl = if ($Label) { $Label } else { $Arg1 }
        Invoke-CaptureCmd -Lbl $lbl -TodVal $Tod -LegacyPostVal $LegacyPost -SettleVal $Settle
    }
    'compare' {
        $la = if ($LabelA) { $LabelA } else { $Arg1 }
        $lb = if ($LabelB) { $LabelB } else { $Arg2 }
        Invoke-CompareCmd -LA $la -LB $lb
    }
    'flyacross' {
        Invoke-FlyacrossCmd -TodVal $Tod -Duration $DurationSec
    }
    'boottime' {
        Invoke-BoottimeCmd -TodVal $Tod -RunCount $Runs -AutoExitSec $BootAutoExitSec
    }
}
