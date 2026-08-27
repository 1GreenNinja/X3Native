# fold_perf.ps1 - echotropolis FPS measurement for integration/fold-0812 (ASCII only).
# p10 across 2 s windows, per docs/PERF_FRAME_BREAKDOWN.md section 6.
# Usage: pwsh -File tools\fold_perf.ps1 -Label wave1-shipping [-Timers 0|1] [-Seconds 130]
param(
  [string]$Label = "perf",
  [int]$Timers = 0,        # X3_PASSTIMERS: 0 = shipping numbers
  [int]$Seconds = 130,
  # --bench turns VSYNC OFF (desc.vsync = !o.bench). Required when the Windows
  # session is LOCKED: DWM then throttles present for a windowed vsync app and the
  # frame reads ~264 ms / 3.8 FPS while the GPU is still only doing 17 ms of work.
  [switch]$Bench
)
$ErrorActionPreference = "Continue"
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo
$outDir = Join-Path $repo "gatelogs"
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Force $outDir | Out-Null }
$log = Join-Path $outDir "perf_$Label.txt"
"" | Out-File $log -Encoding ascii
function W([string]$s) { Write-Host $s; $s | Out-File $log -Append -Encoding ascii }

W "===== PERF $Label  $(Get-Date -Format o) ====="
W "config: X3_PASSTIMERS=$Timers  X3_PASSDUMP=2  ECHO_AUTOEXIT_SEC=$Seconds  --world echotropolis (windowed)$(if ($Bench) { ' --bench (VSYNC OFF)' })"
W "session locked? $((Get-Process LockApp -EA SilentlyContinue) -ne $null)  (a locked session throttles present on a vsync run)"
$foreign = Get-CimInstance Win32_Process -Filter "Name='X3Engine.exe'" |
           Where-Object { $_.CommandLine -notlike "*X3Native-fold*" }
if ($foreign) { foreach ($f in $foreign) { W "FOREIGN-ENGINE (inflates medians; NOT killed) pid=$($f.ProcessId) : $($f.CommandLine)" } }
else { W "FOREIGN-ENGINE none at start" }

$env:X3_PASSDUMP = "2"
$env:X3_PASSTIMERS = "$Timers"
$env:ECHO_AUTOEXIT_SEC = "$Seconds"
$exe = Join-Path $repo "build\bin\Release\X3Engine.exe"

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $exe
$psi.Arguments = "--world echotropolis" + $(if ($Bench) { " --bench" } else { "" })
$psi.WorkingDirectory = $repo
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$p = New-Object System.Diagnostics.Process
$p.StartInfo = $psi
$null = $p.Start()
$tOut = $p.StandardOutput.ReadToEndAsync()
$tErr = $p.StandardError.ReadToEndAsync()
# DO NOT touch the mouse/keyboard: echotropolis boots in fly mode with the cursor
# captured, so any cursor write becomes a mouselook delta and a stray ESC pauses
# the sim while the perf HUD keeps printing cheerful numbers.
if (-not $p.WaitForExit(($Seconds + 90) * 1000)) {
  & taskkill.exe /PID $p.Id /T /F 2>&1 | Out-Null
  try { $p.WaitForExit(10000) } catch {}
}
$txt = ""
try { if ($tOut.Wait(20000)) { $txt += $tOut.Result }; if ($tErr.Wait(20000)) { $txt += $tErr.Result } } catch {}
Remove-Item Env:\X3_PASSDUMP, Env:\X3_PASSTIMERS, Env:\ECHO_AUTOEXIT_SEC -EA SilentlyContinue

$raw = Join-Path $outDir "perf_${Label}_raw.txt"
$txt | Out-File $raw -Encoding ascii
$lines = $txt -split "`r?`n"
$windows = @()
$cur = $null
foreach ($ln in $lines) {
  $m = [regex]::Match($ln, "FRAME COST BREAKDOWN \((\w+)\).*?(\d+) frames.*?CPU ([\d.]+) ms/frame \(([\d.]+) FPS\) \| GPU ([\d.]+) ms/frame")
  if ($m.Success) {
    if ($null -ne $cur) { $windows += $cur }
    $cur = [ordered]@{ frames=[int]$m.Groups[2].Value; cpu=[double]$m.Groups[3].Value;
                       fps=[double]$m.Groups[4].Value; gpu=[double]$m.Groups[5].Value;
                       draws=0; drawn=0; simCalls=-1.0 }
    continue
  }
  if ($null -eq $cur) { continue }
  $s = [regex]::Match($ln, "drawMesh submitted (\d+).*?objects drawn (\d+)")
  if ($s.Success) { $cur.draws=[int]$s.Groups[1].Value; $cur.drawn=[int]$s.Groups[2].Value; continue }
  $h = [regex]::Match($ln, "cpu\.host_sim\s+[\d.]+ ms\s+[\d.]+%\s+calls/frame\s+([\d.]+)")
  if ($h.Success) { $cur.simCalls=[double]$h.Groups[1].Value; continue }
}
if ($null -ne $cur) { $windows += $cur }

W "windows parsed: $($windows.Count)"
# WARM-UP: drop the first 10 s (5 x 2 s windows). The sea-level lane's first perf run
# read 37.8 FPS purely because boot + region streaming was still in the sample.
$warm = 5
if ($windows.Count -gt $warm) { $windows = $windows[$warm..($windows.Count-1)] }
W "warm-up windows dropped (first 10 s): $warm  -> $($windows.Count) remain"
# VALIDITY: reject a paused sim (host_sim calls/frame < 1.0) or a camera that is not
# looking at the city (< 90k draws submitted).
# NOTE: with X3_PASSTIMERS=0 the CPU zone accounting is OFF, so cpu.host_sim reads
# 0.000 ms / 0.0 calls on every window. That is instrumentation being absent, NOT a
# paused sim - so the paused-sim guard is only applied on the instrumented arm, and
# the instrumented arm is what proves the sim was live for this camera/run shape.
$simKnown = ($Timers -ne 0)
$valid = @($windows | Where-Object { (-not $simKnown -or $_.simCalls -ge 1.0) -and $_.draws -ge 90000 })
$rejPause = if ($simKnown) { @($windows | Where-Object { $_.simCalls -lt 1.0 }).Count } else { "n/a (timers off - see instrumented arm)" }
$rejDraw  = @($windows | Where-Object { $_.draws -lt 90000 }).Count
W "windows REJECTED: paused-sim(simCalls<1.0)=$rejPause  low-draws(<90k)=$rejDraw"
W "windows VALID: $($valid.Count)"
if ($valid.Count -lt 5) {
  W "TOO FEW VALID WINDOWS - measurement not trustworthy"
  ($lines | Select-String -Pattern "FRAME COST|drawMesh submitted|host_sim" | Select-Object -First 10) | ForEach-Object { W "  $_" }
  exit 1
}
function P([double[]]$a, [double]$q) {
  $s = $a | Sort-Object
  $i = [int][math]::Floor($q * ($s.Count - 1))
  return $s[$i]
}
$cpus = @($valid | ForEach-Object { [double]$_.cpu })
$fpss = @($valid | ForEach-Object { [double]$_.fps })
$gpus = @($valid | ForEach-Object { [double]$_.gpu })
# p10 of FRAME TIME is the undisturbed (fastest-decile) estimate; external load only ADDS time.
$cpuP10 = P $cpus 0.10
$cpuMed = P $cpus 0.50
$fpsP90 = P $fpss 0.90     # the FPS that corresponds to the p10 frame time
$fpsMed = P $fpss 0.50
$gpuP10 = P $gpus 0.10
W ""
W ("FRAME CPU  p10 = {0:N2} ms   -> {1:N1} FPS   (median {2:N2} ms -> {3:N1} FPS)" -f $cpuP10, (1000.0/$cpuP10), $cpuMed, (1000.0/$cpuMed))
W ("FRAME GPU  p10 = {0:N2} ms" -f $gpuP10)
W ("REPORTED FPS p90 = {0:N1}   median = {1:N1}" -f $fpsP90, $fpsMed)
W ("draws submitted range: {0} .. {1}   objects drawn: {2} .. {3}" -f `
   (($valid | ForEach-Object {$_.draws}) | Measure-Object -Minimum).Minimum,
   (($valid | ForEach-Object {$_.draws}) | Measure-Object -Maximum).Maximum,
   (($valid | ForEach-Object {$_.drawn}) | Measure-Object -Minimum).Minimum,
   (($valid | ForEach-Object {$_.drawn}) | Measure-Object -Maximum).Maximum)
$foreign2 = Get-CimInstance Win32_Process -Filter "Name='X3Engine.exe'" |
            Where-Object { $_.CommandLine -notlike "*X3Native-fold*" }
if ($foreign2) { foreach ($f in $foreign2) { W "FOREIGN-ENGINE at end pid=$($f.ProcessId) : $($f.CommandLine)" } } else { W "FOREIGN-ENGINE none at end" }
W "===== PERF $Label END ====="
