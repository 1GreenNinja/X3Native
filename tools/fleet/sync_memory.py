#!/usr/bin/env python3
"""sync_memory.py — push Tim's ~/.claude/ memory + config to the fleet-memory
private GitHub repo. Runs every 3h via Scheduled Task FleetMemory-Sync; can
also be run manually.

Auto-commits any changes detected in ~/.claude/ (excluding everything in the
fleet-memory .gitignore) with a timestamp-only message. No-op if there are no
changes — git's empty-commit guard handles that naturally.

Why a Python script and not a one-liner: the Scheduled Task needs to ensure
the cwd is set right and that any push failures (network out, GitHub down,
auth expired) produce a readable log line in ~/.claude/.fleet-memory-sync.log
so the next interactive session can see what happened.

Exit codes:
    0 — synced (or no-op)
    1 — uncommitted changes already exist (manual conflict)
    2 — push failed (network / auth / etc.)
"""

from __future__ import annotations

import os
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

HOME = Path(os.path.expanduser('~'))
CLAUDE_DIR = HOME / '.claude'
LOG = CLAUDE_DIR / '.fleet-memory-sync.log'


def log(msg: str) -> None:
    line = f'[{datetime.now().isoformat(timespec="seconds")}] {msg}'
    print(line)
    try:
        with open(LOG, 'a', encoding='utf-8') as f:
            f.write(line + '\n')
    except OSError:
        pass


def run(cmd: list[str], cwd: Path | str | None = None) -> tuple[int, str, str]:
    """Run a subprocess; return (returncode, stdout, stderr)."""
    try:
        result = subprocess.run(
            cmd,
            cwd=cwd,
            capture_output=True,
            text=True,
            timeout=120,
        )
        return result.returncode, result.stdout.strip(), result.stderr.strip()
    except subprocess.TimeoutExpired:
        return 124, '', 'timeout'
    except FileNotFoundError as e:
        return 127, '', str(e)


def main() -> int:
    if not (CLAUDE_DIR / '.git').is_dir():
        log(f'NOT a git repo: {CLAUDE_DIR}/.git missing — bail.')
        return 1

    # Stage all changes (gitignore filters secrets etc.).
    rc, out, err = run(['git', 'add', '-A'], cwd=CLAUDE_DIR)
    if rc != 0:
        log(f'git add failed (rc={rc}): {err}')
        return 1

    # Anything actually staged?
    rc, out, _ = run(['git', 'diff', '--cached', '--name-only'], cwd=CLAUDE_DIR)
    if not out:
        log('no changes — skip commit')
        return 0
    n_changed = len(out.splitlines())

    # Defense-in-depth: refuse to commit if a secret pattern slipped through
    # somehow (new file the gitignore doesn't cover, etc.).
    secret_pat = ('token', 'cred', 'password', '.key', '.pem', 'secret', '.env')
    bad = [
        f for f in out.splitlines()
        if any(p in f.lower() for p in secret_pat)
    ]
    if bad:
        log(f'ABORT: {len(bad)} staged file(s) match secret patterns: {bad[:3]}')
        # Unstage them all so we don't leave a poisoned index.
        run(['git', 'reset'], cwd=CLAUDE_DIR)
        return 2

    msg = f'auto-sync {datetime.now().strftime("%Y-%m-%d %H:%M:%S")} — {n_changed} file(s)'
    rc, _, err = run(['git', 'commit', '-m', msg], cwd=CLAUDE_DIR)
    if rc != 0:
        log(f'git commit failed (rc={rc}): {err}')
        return 1

    rc, _, err = run(['git', 'push', 'origin', 'main'], cwd=CLAUDE_DIR)
    if rc != 0:
        log(f'git push failed (rc={rc}): {err}')
        return 2

    log(f'OK: committed + pushed {n_changed} file(s)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
