#!/usr/bin/env bash
# Install the X3Native repo-safety hooks (tools/hooks/*) into this clone.
#
# Works from the main checkout OR any linked worktree: hooks live in the
# clone's COMMON git dir, so installing once protects every worktree/lane
# on this machine. Any pre-existing, different pre-commit hook is backed up
# alongside as pre-commit.bak.<timestamp> rather than silently destroyed.
#
# Usage (git-bash):  tools/install_hooks.sh
set -eu

repo_root=$(git rev-parse --show-toplevel)
hooks_dir=$(git rev-parse --git-path hooks)
src="$repo_root/tools/hooks/pre-commit"
dst="$hooks_dir/pre-commit"

[ -f "$src" ] || { echo "ERROR: $src not found (run from inside the repo)" >&2; exit 1; }
mkdir -p "$hooks_dir"

if [ -f "$dst" ] && ! cmp -s "$src" "$dst"; then
    bak="$dst.bak.$(date +%Y%m%d%H%M%S)"
    cp "$dst" "$bak"
    echo "Existing pre-commit hook backed up to: $bak"
fi

cp "$src" "$dst"
chmod +x "$dst"
echo "Installed: $dst"
echo "All worktrees of this clone are now guarded (blocks >50MB non-LFS, *.gguf/*.dmp/*.pdb)."
