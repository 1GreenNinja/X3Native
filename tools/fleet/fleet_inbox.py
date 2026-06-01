#!/usr/bin/env python3
"""fleet_inbox.py — Claude Code hook: surface unread Matrix fleet messages.

Reads ~/.claude/.matrix_inbox.jsonl (written by the matrix-bot-sdk daemon),
finds messages newer than the "seen by Claude" marker at
~/.claude/.matrix_inbox_claude_seen.txt, prints them as concise markdown
for the hook output, and advances the marker.

This is intentionally separate from the daemon's own seen marker
(~/.claude/.matrix_seen.json) — the daemon tracks what it has delivered to
the inbox; this script tracks what Claude has surfaced to the user.

Usage (called by Claude Code SessionStart / UserPromptSubmit hooks):
    python fleet_inbox.py            # since last Claude-seen ts, mark as seen
    python fleet_inbox.py --peek      # show new but don't advance marker
    python fleet_inbox.py --recent 10 # last N regardless of seen marker

Silent (no output) when there's nothing new — keeps hook context clean.
"""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path

HOME = Path(os.path.expanduser('~'))
CLAUDE_DIR = Path(os.environ.get('CLAUDE_DIR', HOME / '.claude'))
INBOX = CLAUDE_DIR / '.matrix_inbox.jsonl'
SEEN = CLAUDE_DIR / '.matrix_inbox_claude_seen.txt'
ROOM_CACHE = CLAUDE_DIR / '.matrix_room_names.json'

# Truncation length for long messages in the hook output (so context stays tidy).
MAX_MSG_LEN = 280


def load_room_names() -> dict[str, str]:
    """Load the room-id -> friendly-name cache. Built up over time as
    we see new rooms; populated externally by `fleet_send.py --refresh-rooms`
    or directly written by the user."""
    if not ROOM_CACHE.exists():
        return {}
    try:
        with open(ROOM_CACHE, 'r', encoding='utf-8') as f:
            return json.load(f)
    except (json.JSONDecodeError, OSError):
        return {}


def read_inbox() -> list[dict]:
    """Load every event from the inbox JSONL. Skips blank/malformed lines."""
    if not INBOX.exists():
        return []
    events: list[dict] = []
    with open(INBOX, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                events.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    return events


def read_seen() -> int:
    """Last-seen timestamp (ms epoch). 0 if never seen — meaning surface everything."""
    if not SEEN.exists():
        return 0
    try:
        return int(SEEN.read_text().strip())
    except (ValueError, OSError):
        return 0


def write_seen(ts: int) -> None:
    SEEN.parent.mkdir(parents=True, exist_ok=True)
    SEEN.write_text(str(ts))


def fmt_event(e: dict, rooms: dict[str, str]) -> str:
    sender = e.get('sender', '@?:').split(':')[0].lstrip('@')
    room_id = e.get('room', '')
    room = rooms.get(room_id) or (f'…{room_id[-8:]}' if room_id else '?')
    text = (e.get('text') or '').strip()
    if len(text) > MAX_MSG_LEN:
        text = text[: MAX_MSG_LEN - 3] + '...'
    return f'- **@{sender}** in `{room}`: {text}'


def main() -> int:
    args = sys.argv[1:]
    peek = '--peek' in args
    recent_n: int | None = None
    if '--recent' in args:
        idx = args.index('--recent')
        if idx + 1 < len(args):
            try:
                recent_n = int(args[idx + 1])
            except ValueError:
                pass

    events = read_inbox()
    if not events:
        return 0

    if recent_n:
        to_show = events[-recent_n:]
    else:
        last_seen = read_seen()
        to_show = [e for e in events if int(e.get('ts', 0)) > last_seen]

    if not to_show:
        return 0  # silent — nothing new since last Claude check

    rooms = load_room_names()

    print(f'## Fleet messages ({len(to_show)} new)')
    print()
    for e in to_show:
        print(fmt_event(e, rooms))
    print()
    print(f'_(reply via the `fleet_send` helper: `python G:/X3Native/tools/fleet/fleet_send.py "<room-id-or-#alias>" "<text>"`)_')

    if not peek and recent_n is None:
        max_ts = max(int(e.get('ts', 0)) for e in to_show)
        write_seen(max_ts)

    return 0


if __name__ == '__main__':
    sys.exit(main())
