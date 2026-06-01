#!/usr/bin/env python3
"""fleet_send.py — send a Matrix message via this machine's bot access token.

Bypasses the daemon's named-pipe outbox and posts to the Matrix client-server
API directly with the token at ~/.claude/.matrix_token. Simpler than the pipe
path for one-shot sends from Claude or scripts.

Usage:
    python fleet_send.py <room_id_or_alias> "<message>"

Examples:
    python fleet_send.py "!0H8gfl2jP8rWT5mV_i54dPhxC0zg1v7zc7gHwQzJy5k" "hi fleet"
    python fleet_send.py "#fleet-ops:fleetcommand.slopclaude.com" "hi fleet"

The room name cache (~/.claude/.matrix_room_names.json) is updated each time a
new room is encountered, so fleet_inbox.py can show friendly names.
"""

from __future__ import annotations

import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

HOME = Path(os.path.expanduser('~'))
CLAUDE_DIR = Path(os.environ.get('CLAUDE_DIR', HOME / '.claude'))
TOKEN_PATH = CLAUDE_DIR / '.matrix_token'
ROOM_CACHE = CLAUDE_DIR / '.matrix_room_names.json'
HOMESERVER = os.environ.get('MATRIX_HOMESERVER_URL', 'https://fleetcommand.slopclaude.com')


def load_token() -> str:
    if not TOKEN_PATH.exists():
        die(f'No access token at {TOKEN_PATH}. Run the daemon registration first.')
    token = TOKEN_PATH.read_text().strip()
    if not token:
        die(f'Token file {TOKEN_PATH} is empty.')
    return token


def die(msg: str) -> None:
    print(f'fleet_send: {msg}', file=sys.stderr)
    sys.exit(1)


def http_request(method: str, url: str, token: str, body: dict | None = None) -> dict:
    data = json.dumps(body).encode('utf-8') if body is not None else None
    req = urllib.request.Request(
        url,
        data=data,
        method=method,
        headers={
            'Authorization': f'Bearer {token}',
            'Content-Type': 'application/json',
            # Cloudflare's WAF 403s the default Python-urllib UA — pose as a real client.
            'User-Agent': 'fleet-bot/1.0 (matrix-bot-sdk-companion)',
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            return json.loads(resp.read())
    except urllib.error.HTTPError as e:
        try:
            err = json.loads(e.read())
        except Exception:
            err = {'errcode': f'HTTP_{e.code}', 'error': str(e)}
        die(f'Matrix error: {err}')
        return {}  # unreachable


def resolve_alias(alias: str, token: str) -> str:
    """Resolve #room:server -> !room_id:server."""
    encoded = urllib.parse.quote(alias)
    url = f'{HOMESERVER}/_matrix/client/v3/directory/room/{encoded}'
    data = http_request('GET', url, token)
    return data['room_id']


def lookup_room_name(room_id: str, token: str) -> str | None:
    """Fetch the m.room.name state event for a room. None if not set."""
    encoded = urllib.parse.quote(room_id)
    url = f'{HOMESERVER}/_matrix/client/v3/rooms/{encoded}/state/m.room.name/'
    try:
        data = http_request('GET', url, token)
        return data.get('name')
    except SystemExit:
        return None


def cache_room_name(room_id: str, name: str | None) -> None:
    if not name:
        return
    cache: dict[str, str] = {}
    if ROOM_CACHE.exists():
        try:
            with open(ROOM_CACHE, 'r', encoding='utf-8') as f:
                cache = json.load(f)
        except (json.JSONDecodeError, OSError):
            cache = {}
    if cache.get(room_id) == name:
        return
    cache[room_id] = name
    ROOM_CACHE.parent.mkdir(parents=True, exist_ok=True)
    with open(ROOM_CACHE, 'w', encoding='utf-8') as f:
        json.dump(cache, f, indent=2, sort_keys=True)


def send(room_or_alias: str, text: str) -> dict:
    token = load_token()
    room_id = resolve_alias(room_or_alias, token) if room_or_alias.startswith('#') else room_or_alias

    # Best-effort cache the friendly name so fleet_inbox can show it later.
    try:
        name = lookup_room_name(room_id, token)
        cache_room_name(room_id, name)
    except Exception:
        pass

    txn = f'bot-{int(time.time() * 1000)}'
    url = f'{HOMESERVER}/_matrix/client/v3/rooms/{urllib.parse.quote(room_id)}/send/m.room.message/{txn}'
    body = {'msgtype': 'm.text', 'body': text}
    return http_request('PUT', url, token, body)


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__.strip().split('\n\n')[0], file=sys.stderr)
        print('\nUsage: fleet_send.py <room_id_or_#alias> "<message>"', file=sys.stderr)
        return 1
    room = sys.argv[1]
    text = ' '.join(sys.argv[2:])
    result = send(room, text)
    event_id = result.get('event_id', '?')
    print(f'sent: {event_id}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
