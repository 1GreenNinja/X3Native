# clean_worktrees.ps1 — remove Claude Code background sub-agent git worktrees.
#
# Each sub-agent runs in a FULL repo checkout under .claude/worktrees/agent-<hash>/
# (gigabytes of duplicated assets + build dirs). They are local-only and safe to
# delete once an agent finishes — its work is committed to its branch + pushed to
# origin, so nothing is lost.
#
# RUN THIS WHEN NO SUB-AGENTS ARE ACTIVELY WORKING (removing a live agent's worktree
# would break it). Usage:  ./tools/clean_worktrees.ps1   [-WhatIf to preview]
[CmdletBinding(SupportsShouldProcess = $true)]
param()

$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

Write-Host "Worktrees before:" -ForegroundColor Cyan
git worktree list

$before = 0
if (Test-Path ".claude/worktrees") {
    $before = (Get-ChildItem ".claude/worktrees" -Recurse -File -ErrorAction SilentlyContinue |
               Measure-Object -Property Length -Sum).Sum
}

# Enumerate agent worktrees from git's own registry (porcelain).
$paths = (git worktree list --porcelain) |
         Where-Object { $_ -match '^worktree\s+(.*agent-.*)$' } |
         ForEach-Object { ($_ -split '\s+', 2)[1] }

foreach ($p in $paths) {
    if ($PSCmdlet.ShouldProcess($p, "remove worktree")) {
        git worktree unlock $p 2>$null
        git worktree remove --force $p 2>$null
        Write-Host "removed $p"
    }
}
git worktree prune 2>$null

$after = 0
if (Test-Path ".claude/worktrees") {
    $after = (Get-ChildItem ".claude/worktrees" -Recurse -File -ErrorAction SilentlyContinue |
              Measure-Object -Property Length -Sum).Sum
}
$freedGB = [math]::Round(($before - $after) / 1GB, 2)
Write-Host "`nFreed ~$freedGB GB." -ForegroundColor Green
git worktree list
