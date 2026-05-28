$ErrorActionPreference = 'Continue'
Set-Location 'D:\GameDev\X3Native'
$exe = '.\build\bin\Release\X3Engine.exe'
if (-not (Test-Path $exe)) { Write-Output "ERR: exe not found at $exe"; exit 2 }

# Canonical list — derived from `else if (a == "--test-*") ...` in app/main.cpp
$flags = @(
  '--test-jobs','--test-asset','--test-console','--test-physics','--test-gltf',
  '--test-player','--test-interact','--test-pickup','--test-combat','--test-audio',
  '--test-level1','--test-phase2a','--test-phase2b','--test-anim','--test-terrain',
  '--test-terrainplace','--test-streaming','--test-ai','--test-bestiary','--test-bosses',
  '--test-act2bosses','--test-spiremid','--test-nexus','--test-spiretop','--test-dronehack',
  '--test-sublevels','--test-act2','--test-act2caves','--test-doorcode','--test-elevator',
  '--test-elevatorfsm','--test-net','--test-netsync','--test-netinterp','--test-netpredict',
  '--test-rescue','--test-destruction','--test-debris','--test-gpuskin','--test-collapse',
  '--test-physjoint','--test-ragdoll','--test-nav','--test-weapons','--test-vehicle',
  '--test-footik','--test-ui','--test-saveload','--test-valley','--test-cliffs',
  '--test-club','--test-locomotion'
)

$results = @()
$total = $flags.Count
$idx = 0
foreach ($f in $flags) {
  $idx++
  $t0 = Get-Date
  Write-Output "[$idx/$total] $f ..."
  & $exe $f *> "qa\last_$($f.TrimStart('-')).log"
  $rc = $LASTEXITCODE
  $dt = (Get-Date) - $t0
  $row = [PSCustomObject]@{ Flag = $f; Exit = $rc; Sec = [math]::Round($dt.TotalSeconds,1) }
  $results += $row
  if ($rc -eq 0) { Write-Output "  PASS in $($row.Sec)s" } else { Write-Output "  FAIL exit=$rc in $($row.Sec)s" }
  if ($f -eq '--test-gltf') {
    # spec: --test-gltf regenerates docs/GLB_IMPORT_REPORT.md; revert it
    git checkout -- docs/GLB_IMPORT_REPORT.md 2>$null
  }
}

Write-Output ''
Write-Output '=== RELEASE --smoketest (VUID check) ==='
$smokeLog = 'qa\last_smoketest_release.log'
& $exe --smoketest *> $smokeLog
$smokeRc = $LASTEXITCODE
$vuidLines = Select-String -Path $smokeLog -Pattern 'VUID|validation' -SimpleMatch -CaseSensitive:$false
$vuidCount = if ($vuidLines) { $vuidLines.Count } else { 0 }
Write-Output "  smoketest exit=$smokeRc, VUID-mentions=$vuidCount (lines in $smokeLog)"

Write-Output ''
Write-Output '=== SUMMARY ==='
$results | Format-Table -AutoSize
$failed = $results | Where-Object { $_.Exit -ne 0 }
Write-Output ''
Write-Output "Total: $total | Passed: $($total - $failed.Count) | Failed: $($failed.Count)"
if ($failed.Count -gt 0) {
  Write-Output 'FAILED FLAGS:'
  $failed | ForEach-Object { Write-Output "  $($_.Flag) (exit=$($_.Exit))" }
}
Write-Output "Release smoketest: exit=$smokeRc, VUID-mentions=$vuidCount"
if ($failed.Count -eq 0 -and $smokeRc -eq 0 -and $vuidCount -eq 0) {
  Write-Output 'GAUNTLET: PASS'
  exit 0
} else {
  Write-Output 'GAUNTLET: FAIL'
  exit 1
}
