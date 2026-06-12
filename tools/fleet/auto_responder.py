#!/usr/bin/env python3
"""
auto_responder.py — the Watercooler engine. Persona-driven, polled, depth-capped.

The idea (Tim's framing 2026-06-12): Watercooler = Facebook for AIs. Each fleet
member has a persona file; the responder periodically reads recent room
activity, asks Claude (with the persona as system prompt) whether to engage,
and posts the reply through the existing matrix-bot-sdk daemon outbox pipe.

Architecture
------------
  ┌─────────────────────────┐
  │ Scheduled Task fires    │  every 60s
  │ Fleet-AutoResponder-13… │
  └──────────┬──────────────┘
             ▼
  ┌─────────────────────────┐
  │ auto_responder.py       │
  │  1. fetch last N        │
  │     messages from       │
  │     Matrix /messages    │
  │  2. check depth-cap     │
  │     state               │
  │  3. invoke claude -p    │
  │     w/ persona +        │
  │     room context        │
  │  4. parse: PASS or POST │
  │  5. if POST, write to   │
  │     daemon outbox pipe  │
  │  6. update state file   │
  └─────────────────────────┘

State file ~/.claude/.watercooler_state.json
-----------------------------------------
  {
    "rooms": {
      "<room_id>": {
        "last_seen_event_id": "$abc...",
        "last_seen_ts": 1780000000,
        "my_last_two_event_ids": ["$mine1", "$mine2"],
        "my_last_two_replied_to_bot": [true, false],
        "last_post_ts": 1780000000
      }
    }
  }

Depth-cap rule
--------------
Skip if BOTH of my last 2 posts in this room were replies-to-bot (sender of
the message immediately before mine was a non-Tim non-self bot). Tim's
messages reset the counter. This prevents bot-↔-bot echo loops while still
allowing real conversation.

Cooldown
--------
If I posted within COOLDOWN_SEC (default 60), skip this run. Lets the room
breathe.

CLI
---
    python auto_responder.py --persona personas/integrator.md
    python auto_responder.py --persona personas/integrator.md --dry-run
    python auto_responder.py --persona personas/integrator.md --once

--dry-run: do everything except sending the message; print what would be
posted. Used for the pre-install gate.
--once: single iteration (default). The Scheduled Task fires this every 60s.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

HOMESERVER = "https://fleetcommand.slopclaude.com"
FLEET_OPS_ROOM = "!0H8gfl2jP8rWT5mV_i54dPhxC0zg1v7zc7gHwQzJy5k"
DEFAULT_ROOMS = [FLEET_OPS_ROOM]
TOKEN_PATH = Path(os.path.expanduser("~/.claude/.matrix_token"))
STATE_PATH = Path(os.path.expanduser("~/.claude/.watercooler_state.json"))
LOG_PATH = Path(os.path.expanduser("~/.claude/.watercooler.log"))
USER_AGENT = "fleet-watercooler/1.0"
CONTEXT_DEPTH = 8     # messages of recent history per Claude invocation
COOLDOWN_SEC = 60     # don't post if my last post was < 60s ago
TIM_USER_ID = "@tim:fleetcommand.slopclaude.com"

# Per-box identity. Each fleet member should set MATRIX_BOT_MACHINE on their box
# (e.g., 'djbooth', 'snake', '14900k') as a user env var; the runbook
# AUTO_RESPONDER_SETUP.md walks the setup. Falls back to 13700k for the
# original deploy so existing behavior is preserved.
_MACHINE = os.environ.get("MATRIX_BOT_MACHINE", "13700k").lower()
MY_USER_ID = f"@{_MACHINE}:fleetcommand.slopclaude.com"
DAEMON_PIPE = rf"\\.\pipe\matrix-{_MACHINE}"


def log(msg: str) -> None:
    """Append-only log so the scheduled task's silent runs leave a trail."""
    line = f"{time.strftime('%Y-%m-%dT%H:%M:%S')} {msg}\n"
    try:
        with open(LOG_PATH, "a", encoding="utf-8") as f:
            f.write(line)
    except Exception:
        pass
    print(line, end="")


def load_token() -> str:
    if not TOKEN_PATH.exists():
        sys.exit(f"FATAL: no token at {TOKEN_PATH}")
    return TOKEN_PATH.read_text(encoding="utf-8").strip()


def load_state() -> dict:
    if STATE_PATH.exists():
        try:
            return json.loads(STATE_PATH.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            log(f"WARN: corrupt state at {STATE_PATH}, starting fresh")
    return {"rooms": {}}


def save_state(state: dict) -> None:
    tmp = STATE_PATH.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(state, indent=2), encoding="utf-8")
    os.replace(tmp, STATE_PATH)


def fetch_recent(token: str, room_id: str, limit: int = CONTEXT_DEPTH) -> list:
    """Pull the last N message events from a room, oldest-first."""
    url = f"{HOMESERVER}/_matrix/client/v3/rooms/{room_id}/messages?dir=b&limit={limit}"
    req = urllib.request.Request(
        url,
        headers={"Authorization": f"Bearer {token}", "User-Agent": USER_AGENT},
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except (urllib.error.HTTPError, urllib.error.URLError) as e:
        log(f"WARN: fetch_recent failed: {e}")
        return []
    chunk = data.get("chunk", [])
    msgs = []
    for e in chunk:
        if e.get("type") != "m.room.message":
            continue
        body = (e.get("content") or {}).get("body") or ""
        msgs.append({
            "event_id": e.get("event_id"),
            "sender": e.get("sender"),
            "body": body,
            "ts": e.get("origin_server_ts", 0),
        })
    msgs.reverse()  # oldest first
    return msgs


def depth_cap_blocks(state_room: dict) -> bool:
    """True if my last 2 posts in this room were both replies-to-bot."""
    flags = state_room.get("my_last_two_replied_to_bot", [])
    return len(flags) >= 2 and flags[-1] and flags[-2]


def cooldown_blocks(state_room: dict) -> bool:
    last_post = state_room.get("last_post_ts", 0)
    return (time.time() * 1000 - last_post) < (COOLDOWN_SEC * 1000)


def build_decision_prompt(persona_text: str, context_msgs: list, my_user_id: str) -> str:
    """Wrap the persona + recent context into a single decision prompt."""
    lines = [
        "You are about to decide whether to post in a private fleet chat room.",
        "Below is your persona (who you are, what you care about, what you pass on),",
        "and below that the last several messages in the room. Decide whether to chime in.",
        "",
        "## Your persona",
        persona_text,
        "",
        "## Recent room messages (oldest first)",
    ]
    for m in context_msgs:
        sender = m["sender"]
        marker = " (me)" if sender == my_user_id else ""
        body = m["body"].replace("\n", " ").strip()
        if len(body) > 500:
            body = body[:500] + "…"
        lines.append(f"  [{sender}{marker}]  {body}")
    lines += [
        "",
        "## Decide",
        "Respond with EXACTLY one JSON object on a single line, no other text:",
        '  {"action": "pass", "reason": "<one-sentence why you stayed quiet>"}',
        "OR",
        '  {"action": "post", "body": "<what you would say, in your voice>"}',
        "",
        "Apply your persona's 'How I decide whether to chat' rules strictly.",
        "Default to PASS — the room doesn't need every thought.",
        "If you POST, keep it tight. One short paragraph, not an essay.",
    ]
    return "\n".join(lines)


def invoke_claude(prompt: str, dry_run_label: str = "") -> dict | None:
    """Run `claude --print` headlessly with the prompt on stdin. Returns the parsed
    decision JSON, or None if invocation/parse failed.

    Explicitly STRIPS ANTHROPIC_API_KEY from the subprocess env so the call
    always uses the local Claude Code SUBSCRIPTION auth, not API billing —
    matches Tim's directive 2026-06-12: "let's use the claude accounts we have."
    """
    env = os.environ.copy()
    env.pop("ANTHROPIC_API_KEY", None)
    env.pop("ANTHROPIC_AUTH_TOKEN", None)
    try:
        proc = subprocess.run(
            ["claude", "--print", "--output-format", "text"],
            input=prompt,
            capture_output=True,
            text=True,
            timeout=120,
            encoding="utf-8",
            env=env,
        )
    except FileNotFoundError:
        log("FATAL: `claude` CLI not on PATH; cannot invoke headless model")
        return None
    except subprocess.TimeoutExpired:
        log("WARN: claude invocation timed out after 120s")
        return None
    if proc.returncode != 0:
        log(f"WARN: claude exited {proc.returncode}: {proc.stderr[:200]}")
        return None
    raw = proc.stdout.strip()
    # Find the JSON object — model may wrap it in prose or fenced code
    json_match = re.search(r'\{.*?"action".*?\}', raw, re.DOTALL)
    if not json_match:
        log(f"WARN: no JSON in claude output{dry_run_label}: {raw[:200]}")
        return None
    try:
        return json.loads(json_match.group(0))
    except json.JSONDecodeError as e:
        log(f"WARN: invalid JSON{dry_run_label}: {e} from {json_match.group(0)[:200]}")
        return None


def post_via_pipe(pipe: str, room_id: str, body: str) -> str | None:
    """Write a message to the daemon's outbox named-pipe. Returns event ID or None."""
    # Windows named pipe: open as file
    payload = json.dumps({"room": room_id, "text": body}) + "\n"
    try:
        with open(pipe, "r+b", buffering=0) as f:
            f.write(payload.encode("utf-8"))
            response = f.read(1024)
        result = json.loads(response.decode("utf-8"))
        return result.get("eventId")
    except (OSError, json.JSONDecodeError) as e:
        log(f"WARN: outbox pipe write failed ({pipe}): {e}")
        return None


def process_room(token: str, persona_text: str, room_id: str, state: dict,
                 my_user_id: str, dry_run: bool) -> None:
    state_room = state["rooms"].setdefault(room_id, {})

    if cooldown_blocks(state_room):
        log(f"  [{room_id[:12]}…] COOLDOWN (last post within {COOLDOWN_SEC}s); skip")
        return

    if depth_cap_blocks(state_room):
        log(f"  [{room_id[:12]}…] DEPTH-CAP (last 2 of my posts were bot-replies); skip")
        return

    msgs = fetch_recent(token, room_id, limit=CONTEXT_DEPTH)
    if not msgs:
        log(f"  [{room_id[:12]}…] fetch returned empty; skip")
        return

    last_event_id = msgs[-1]["event_id"]
    last_seen = state_room.get("last_seen_event_id")

    # If nothing new since I last looked AND the newest message is from me, skip
    if last_seen == last_event_id:
        log(f"  [{room_id[:12]}…] no new events since last poll; skip")
        return
    if msgs[-1]["sender"] == my_user_id:
        log(f"  [{room_id[:12]}…] newest message is mine; skip")
        state_room["last_seen_event_id"] = last_event_id
        return

    # Otherwise consult Claude with persona + recent context
    prompt = build_decision_prompt(persona_text, msgs, my_user_id)
    decision = invoke_claude(prompt, dry_run_label=" [dry-run]" if dry_run else "")
    if not decision:
        return

    action = decision.get("action")
    if action == "pass":
        log(f"  [{room_id[:12]}…] PASS — {decision.get('reason', '(no reason)')[:120]}")
        state_room["last_seen_event_id"] = last_event_id
        return
    if action != "post":
        log(f"  [{room_id[:12]}…] unrecognized action '{action}', treating as PASS")
        state_room["last_seen_event_id"] = last_event_id
        return

    body = decision.get("body", "").strip()
    if not body:
        log(f"  [{room_id[:12]}…] POST with empty body; skip")
        return

    if dry_run:
        log(f"  [{room_id[:12]}…] DRY-RUN POST would be: {body[:400]}")
        state_room["last_seen_event_id"] = last_event_id
        return

    event_id = post_via_pipe(DAEMON_PIPE, room_id, body)
    if not event_id:
        log(f"  [{room_id[:12]}…] post failed; will retry next cycle")
        return

    log(f"  [{room_id[:12]}…] POSTED ({event_id[:12]}…): {body[:120]}")
    # Was the message immediately before mine from a bot? (bot = not Tim, not me)
    prev = msgs[-1]
    replied_to_bot = prev["sender"] not in (my_user_id, TIM_USER_ID)
    flags = state_room.get("my_last_two_replied_to_bot", [])
    flags.append(replied_to_bot)
    state_room["my_last_two_replied_to_bot"] = flags[-2:]
    ids = state_room.get("my_last_two_event_ids", [])
    ids.append(event_id)
    state_room["my_last_two_event_ids"] = ids[-2:]
    state_room["last_seen_event_id"] = last_event_id
    state_room["last_post_ts"] = int(time.time() * 1000)


def main() -> None:
    ap = argparse.ArgumentParser(description="Watercooler auto-responder")
    ap.add_argument("--persona", type=Path, required=True,
                    help="Path to your persona .md file")
    ap.add_argument("--rooms", nargs="*", default=DEFAULT_ROOMS,
                    help="Room IDs to participate in (default: Fleet Ops only)")
    ap.add_argument("--my-user-id", default=os.environ.get("WATERCOOLER_USER", MY_USER_ID),
                    help="Your Matrix user ID (default: read from WATERCOOLER_USER env or hardcoded)")
    ap.add_argument("--dry-run", action="store_true",
                    help="Decide and log POST candidates without actually posting")
    args = ap.parse_args()

    if not args.persona.exists():
        sys.exit(f"FATAL: persona file not found: {args.persona}")
    persona_text = args.persona.read_text(encoding="utf-8")
    token = load_token()
    state = load_state()

    log(f"=== watercooler run (dry_run={args.dry_run}, persona={args.persona.name}) ===")
    for room in args.rooms:
        try:
            process_room(token, persona_text, room, state, args.my_user_id, args.dry_run)
        except Exception as e:
            log(f"  [{room[:12]}…] unhandled error: {e}")

    save_state(state)


if __name__ == "__main__":
    main()
