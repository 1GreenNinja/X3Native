#!/usr/bin/env python3
"""fleet_image.py - post an IMAGE to a Matrix fleet room via this box's bot token.

Uploads a local image to the homeserver media repo, then sends an ``m.image``
event (optionally followed by a text caption). Stdlib-only (urllib) so it runs
on any fleet box's plain Python, same as ``fleet_send.py`` - no Pillow needed
(image dimensions are parsed from file headers).

Usage:
    python fleet_image.py <room_id_or_#alias> <image_path> ["optional caption"]

Examples:
    python fleet_image.py "#fleet-ops:fleetcommand.slopclaude.com" render.png "Jake's ship"
    python fleet_image.py "!0H8gfl2jP8rWT5mV_i54dPhxC0zg1v7zc7gHwQzJy5k" shot.jpg

Token: ``~/.claude/.matrix_token`` by default. Override with env
``FLEET_TOKEN_FILE`` (absolute path, or a bare name resolved under
``~/.claude``) so multiple bots can share one host - e.g.
``FLEET_TOKEN_FILE=.matrix_token_snake`` to post as @snake instead of the
host's default bot. ``fleet_send.py`` can adopt the same override for text.
"""

from __future__ import annotations

import json
import os
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

HOME = Path(os.path.expanduser('~'))
CLAUDE_DIR = Path(os.environ.get('CLAUDE_DIR', HOME / '.claude'))
HOMESERVER = os.environ.get('MATRIX_HOMESERVER_URL', 'https://fleetcommand.slopclaude.com')
# Cloudflare's WAF 403s the default Python-urllib UA - pose as a real client.
UA = 'fleet-bot/1.0 (matrix-bot-sdk-companion)'

_MIME = {
    '.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg',
    '.gif': 'image/gif', '.webp': 'image/webp', '.bmp': 'image/bmp',
}


def die(msg: str) -> None:
    print(f'fleet_image: {msg}', file=sys.stderr)
    sys.exit(1)


def token_path() -> Path:
    """Resolve the access-token file, honouring FLEET_TOKEN_FILE for multi-bot hosts."""
    override = os.environ.get('FLEET_TOKEN_FILE')
    if override:
        p = Path(os.path.expanduser(override))
        return p if p.is_absolute() else (CLAUDE_DIR / override)
    return CLAUDE_DIR / '.matrix_token'


def load_token() -> str:
    p = token_path()
    if not p.exists():
        die(f'no access token at {p} (set FLEET_TOKEN_FILE or register the bot first)')
    tok = p.read_text().strip()
    if not tok:
        die(f'token file {p} is empty')
    return tok


def guess_mime(path: str) -> str:
    return _MIME.get(Path(path).suffix.lower(), 'application/octet-stream')


def image_size(path: str) -> tuple[int, int]:
    """Return (width, height) parsed from file headers. (0, 0) if unknown - no Pillow dependency."""
    try:
        with open(path, 'rb') as f:
            head = f.read(32)
            if head[:8] == b'\x89PNG\r\n\x1a\n':
                w, h = struct.unpack('>II', head[16:24])
                return w, h
            if head[:6] in (b'GIF87a', b'GIF89a'):
                w, h = struct.unpack('<HH', head[6:10])
                return w, h
            if head[:2] == b'BM':
                w, h = struct.unpack('<ii', head[18:26])
                return abs(w), abs(h)
            if head[:2] == b'\xff\xd8':  # JPEG: walk segments to the SOF marker
                f.seek(2)
                while True:
                    b = f.read(1)
                    if not b:
                        break
                    if b != b'\xff':
                        continue
                    marker = f.read(1)
                    while marker == b'\xff':
                        marker = f.read(1)
                    if marker in (b'\xc0', b'\xc1', b'\xc2', b'\xc3', b'\xc5', b'\xc6',
                                  b'\xc7', b'\xc9', b'\xca', b'\xcb', b'\xcd', b'\xce', b'\xcf'):
                        f.read(3)  # segment length (2) + sample precision (1)
                        h, w = struct.unpack('>HH', f.read(4))
                        return w, h
                    seg = f.read(2)
                    if len(seg) < 2:
                        break
                    f.seek(struct.unpack('>H', seg)[0] - 2, 1)
    except Exception:
        pass
    return 0, 0


def _http_json(method: str, url: str, token: str, body: dict | None = None) -> dict:
    data = json.dumps(body).encode('utf-8') if body is not None else None
    req = urllib.request.Request(
        url, data=data, method=method,
        headers={'Authorization': f'Bearer {token}', 'Content-Type': 'application/json', 'User-Agent': UA},
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return json.loads(resp.read())
    except urllib.error.HTTPError as e:
        try:
            err = json.loads(e.read())
        except Exception:
            err = {'errcode': f'HTTP_{e.code}', 'error': str(e)}
        die(f'matrix error: {err}')
        return {}  # unreachable


def upload_media(path: str, token: str) -> str:
    """Upload the file to the media repo, return its mxc:// URI."""
    data = Path(path).read_bytes()
    fn = urllib.parse.quote(Path(path).name)
    url = f'{HOMESERVER}/_matrix/media/v3/upload?filename={fn}'
    req = urllib.request.Request(
        url, data=data, method='POST',
        headers={'Authorization': f'Bearer {token}', 'Content-Type': guess_mime(path), 'User-Agent': UA},
    )
    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            return json.loads(resp.read())['content_uri']
    except urllib.error.HTTPError as e:
        try:
            err = json.loads(e.read())
        except Exception:
            err = {'errcode': f'HTTP_{e.code}', 'error': str(e)}
        die(f'media upload failed: {err}')
        return ''  # unreachable


def resolve_room(room: str, token: str) -> str:
    if room.startswith('#'):
        enc = urllib.parse.quote(room)
        return _http_json('GET', f'{HOMESERVER}/_matrix/client/v3/directory/room/{enc}', token)['room_id']
    return room


def send_event(room_id: str, token: str, content: dict, kind: str) -> str:
    txn = f'{kind}-{int(time.time() * 1000)}'
    enc = urllib.parse.quote(room_id)
    url = f'{HOMESERVER}/_matrix/client/v3/rooms/{enc}/send/m.room.message/{txn}'
    return _http_json('PUT', url, token, content).get('event_id', '?')


def main() -> int:
    if len(sys.argv) < 3:
        print('usage: fleet_image.py <room_id_or_#alias> <image_path> ["caption"]', file=sys.stderr)
        return 1
    room_arg, path = sys.argv[1], sys.argv[2]
    caption = ' '.join(sys.argv[3:]) if len(sys.argv) > 3 else None
    if not Path(path).exists():
        die(f'no such file: {path}')

    token = load_token()
    room_id = resolve_room(room_arg, token)
    mxc = upload_media(path, token)
    w, h = image_size(path)
    info: dict[str, object] = {'mimetype': guess_mime(path), 'size': Path(path).stat().st_size}
    if w and h:
        info['w'], info['h'] = w, h
    ev = send_event(room_id, token, {'msgtype': 'm.image', 'body': Path(path).name, 'url': mxc, 'info': info}, 'img')
    print(f'image sent: {ev}  ({mxc}, {w}x{h})')
    if caption:
        cap = send_event(room_id, token, {'msgtype': 'm.text', 'body': caption}, 'cap')
        print(f'caption sent: {cap}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
