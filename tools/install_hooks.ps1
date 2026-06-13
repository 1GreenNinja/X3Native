# Install the X3Native repo-safety hooks (tools/hooks/*) into this clone.
#
# Works from the main checkout OR any linked worktree: hooks live in the
# clone's COMMON git dir, so installing once protects every worktree/lane
# on this machine. Any pre-existing, different pre-commit hook is backed up
# alongside as pre-commit.bak.<timestamp> rather than silently destroyed.
#
# Usage (PowerShell):  tools\install_hooks.ps1
$ErrorActionPreference = 'Stop'

$repoRoot = (git rev-parse --show-toplevel).Trim()
$hooksDir = (git rev-parse --git-path hooks).Trim()
if ($LASTEXITCODE -ne 0) { throw "Not inside a git repository." }

$src = Join-Path $repoRoot 'tools/hooks/pre-commit'
$dst = Join-Path $hooksDir 'pre-commit'

if (-not (Test-Path $src)) { throw "ERROR: $src not found (run from inside the repo)" }
New-Item -ItemType Directory -Force $hooksDir | Out-Null

if ((Test-Path $dst) -and ((Get-FileHash $src).Hash -ne (Get-FileHash $dst).Hash)) {
    $bak = "$dst.bak.$(Get-Date -Format yyyyMMddHHmmss)"
    Copy-Item $dst $bak
    Write-Host "Existing pre-commit hook backed up to: $bak"
}

Copy-Item $src $dst -Force
# Git for Windows executes hooks via its bundled sh; no chmod needed on NTFS.
Write-Host "Installed: $dst"
Write-Host "All worktrees of this clone are now guarded (blocks >50MB non-LFS, *.gguf/*.dmp/*.pdb)."
